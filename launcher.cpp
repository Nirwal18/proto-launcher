#include <iostream> // for log output
#include <unistd.h> // starting applications
#include <fstream> // for input file stream
#include <string> // string type
#include <vector> // flexible arrays
#include <map> // hashmaps
#include <algorithm> // for sorting
#include <chrono> // sleep and duration
#include <thread> // main loop management
#include <X11/Xlib.h> // X11 api
#include <X11/Xatom.h> // X11 Atoms (for preferences)
#include <X11/Xutil.h> // used to handle keyboard events
#include <X11/Xft/Xft.h> // fonts (requires libxft)
#include <filesystem> // used for scanning application dirs
#include <pwd.h> // used to get user home dir

namespace fs = std::filesystem;
using std::string, std::map, std::vector, std::ifstream, std::ofstream, std::stringstream;

enum StyleAttribute {
	C_TITLE, C_COMMENT, C_BG, C_HIGHLIGHT, C_MATCH, // colors
	F_REGULAR, F_BOLD, F_SMALLREGULAR, F_SMALLBOLD, F_LARGE // fonts
};

struct Keyword {
	string word;
	int weight;
};

struct Application {
	string id, name, genericName, comment, cmd;
	vector<Keyword> keywords;
	int count = 0;
};

struct Result {
	Application *app;
	int score;
};

const    int ROW_HEIGHT    = 72;
const    int LINE_WIDTH    = 6;
const string HOME_DIR      = getenv("HOME")            != NULL ? getenv("HOME")            : getpwuid(getuid())->pw_dir;
const string CONFIG_DIR    = getenv("XDG_CONFIG_HOME") != NULL ? getenv("XDG_CONFIG_HOME") : HOME_DIR + "/.config";
const string DATA_DIR      = getenv("XDG_DATA_HOME")   != NULL ? getenv("XDG_DATA_HOME")   : HOME_DIR + "/.local/share";
const string CONFIG        = CONFIG_DIR + "/launcher.conf";
const string APP_DIRS[]    = { "/usr/share/applications", "/usr/local/share/applications", DATA_DIR + "/applications" };
const StyleAttribute COLORS[] = { C_TITLE, C_COMMENT, C_BG, C_HIGHLIGHT, C_MATCH };
const StyleAttribute FONTS[] = { F_REGULAR, F_BOLD, F_SMALLREGULAR, F_SMALLBOLD, F_LARGE };

map<StyleAttribute, const string> STYLE_ATTRIBUTES = {
	{ C_TITLE, "title" },
	{ C_COMMENT, "comment" },
	{ C_BG, "background" },
	{ C_HIGHLIGHT, "highlight" },
	{ C_MATCH, "match" },
	{ F_REGULAR, "regular" },
	{ F_BOLD, "bold" },
	{ F_SMALLREGULAR, "smallregular" },
	{ F_SMALLBOLD, "smallbold" },
	{ F_LARGE, "large" }
};

map<StyleAttribute, string> DEFAULT_STYLE = {
	{ C_TITLE, "#111111" },
	{ C_COMMENT, "#999999" },
	{ C_BG, "#ffffff" },
	{ C_HIGHLIGHT, "#f8c291" },
	{ C_MATCH, "#111111" },
	{ F_REGULAR, "Ubuntu,sans-11" },
	{ F_BOLD, "Ubuntu,sans-11:bold" },
	{ F_SMALLREGULAR, "Ubuntu,sans-10" },
	{ F_SMALLBOLD, "Ubuntu,sans-10:bold" },
	{ F_LARGE, "Ubuntu,sans-20:light" }
};

Display *display;
int screen;
Window window;
GC gc;
XIC xic;
XftDraw *xftdraw;
string query = "";
string queryi = ""; // lower case
int selected = 0;
int cursor = 0;
bool cursorVisible = false;
vector<Application> applications;
vector<Result> results;
map<StyleAttribute, string> style = DEFAULT_STYLE;
map<StyleAttribute, XftFont*> fonts;
map<StyleAttribute, XftColor> colors;

string lowercase (const string &str) {
	string out = str;
	transform(out.begin(), out.end(), out.begin(), ::tolower);
	return out;
};

int renderText(const int x, const int y, string text, XftFont &font, const XftColor &color) {
	XftDrawString8(xftdraw, &color, &font, x, y, (XftChar8 *) text.c_str(), text.length());
	if (text.back() == ' ') { // XftTextExtents appears to not count whitespace at the end of a string, so move it to the beginning
		text = " " + text;
	}
	XGlyphInfo extents;
	XftTextExtents8(display, &font, (FcChar8 *) text.c_str(), text.length(), &extents);
	return x + extents.width;
}

void search () {
	results = {};
	if (queryi.length() > 0) {
		for (Application &app : applications) {
			int score = 0;
			int i = 0;
			for (const Keyword &keyword : app.keywords) {
				int matchIndex = keyword.word.find(queryi);
				if (matchIndex != string::npos) {
					// score determined by:
					// - apps whose names begin with the query string appear first
					// - apps whose names or descriptions contain the query string then appear
					// - apps which hav e been opened most frequently should be prioritised
					score = (100 - i) * keyword.weight * (matchIndex == 0 ? 10000 : 100) + app.count;
					break;
				}
				i++;
			}
			if (score > 0) {
				results.push_back({ &app, score });
			}
		}
		sort(results.begin(), results.end(), [](const Result &a, const Result &b) {
			return b.score < a.score;
		});
		if (results.size() > 10) {
			results.resize(10); // limit to 10 results
		}
	}
}

auto lastBlink = std::chrono::system_clock::now();
void renderTextInput (const bool showCursor) {
	lastBlink = std::chrono::system_clock::now();
	int ty = 0.66 * ROW_HEIGHT * 1.25;
	int cursorX = renderText(14, ty, query.substr(0, cursor), *fonts[F_LARGE], colors[C_BG]); // invisible text just to figure out cursor position
	XSetForeground(display, gc, showCursor ? colors[C_TITLE].pixel : colors[C_BG].pixel);
	XFillRectangle(display, window, gc, cursorX, ROW_HEIGHT * 1.25 / 4, 3, ROW_HEIGHT * 1.25 / 2);
	renderText(14, ty, query, *fonts[F_LARGE], colors[C_TITLE]);
	cursorVisible = showCursor;
}

void cursorBlink () {
	auto now = std::chrono::system_clock::now();
	std::chrono::duration<double> delta = now - lastBlink;
	if (delta > (std::chrono::milliseconds) 700) { // seconds
		renderTextInput(!cursorVisible);
	}
}

void render () {
	int resultCount = results.size();
	int screenWidth = DisplayWidth(display, screen);
	int width = screenWidth / 3.4;
	int x = screenWidth / 2 - width / 2;
	int height = (resultCount + 1.25) * ROW_HEIGHT;

	XMoveResizeWindow(display, window, x, 200, width, height);
	XClearWindow(display, window);
	renderTextInput(true);
	XSetForeground(display, gc, colors[C_HIGHLIGHT].pixel);
	XSetLineAttributes(display, gc, LINE_WIDTH, LineSolid, CapButt, JoinRound);
	XDrawRectangle(display, window, gc, 0, 0, width - 1, ROW_HEIGHT * 1.25);
	XDrawRectangle(display, window, gc, 0, ROW_HEIGHT * 1.25 - 1, width - 1, resultCount * ROW_HEIGHT - 1);
	
	for (int i = 0; i < resultCount; i++) {
		const Result result = results[i];
		const string d = result.app->name + " - " + result.app->genericName + result.app->comment;
		if (i == selected) {
			XSetForeground(display, gc, colors[C_HIGHLIGHT].pixel);
			XFillRectangle(display, window, gc, 0, (i + 1.25) * ROW_HEIGHT, width, ROW_HEIGHT);
		}
		
		int y = (i + 1.25) * ROW_HEIGHT + 0.63 * ROW_HEIGHT;
		int x = 14;
		int namei = lowercase(result.app->name).find(queryi);
		if (namei == string::npos) {
			x = renderText(x, y, result.app->name.c_str(), *fonts[F_REGULAR], colors[C_TITLE]);
		} else {
			string str = result.app->name.substr(0, namei);
			x = renderText(x, y, str.c_str(), *fonts[F_REGULAR], colors[C_TITLE]);
			str = result.app->name.substr(namei, query.length());
			x = renderText(x, y, str.c_str(), *fonts[F_BOLD], colors[C_MATCH]);
			str = result.app->name.substr(namei + query.length());
			x = renderText(x, y, str.c_str(), *fonts[F_REGULAR], colors[C_TITLE]);
		}

		x += 8;
		int commenti = lowercase(result.app->comment).find(queryi);
		if (commenti == string::npos) {
			renderText(x, y, result.app->comment.c_str(), *fonts[F_SMALLREGULAR], colors[C_COMMENT]);
		} else {
			string str = result.app->comment.substr(0, commenti);
			x = renderText(x, y, str.c_str(), *fonts[F_SMALLREGULAR], colors[C_COMMENT]);
			str = result.app->comment.substr(commenti, query.length());
			x = renderText(x, y, str.c_str(), *fonts[F_SMALLBOLD], colors[C_COMMENT]);
			str = result.app->comment.substr(commenti + query.length());
			renderText(x, y, str.c_str(), *fonts[F_SMALLREGULAR], colors[C_COMMENT]);
		}
	}
}

void readConfig () {
	struct stat info;
	if (stat(CONFIG.c_str(), &info) != 0) { return; }
	ifstream infile(CONFIG);
	string line;
	while (getline(infile, line)) {
		const int i = line.find_last_of("=");
		if (i == string::npos) { continue; }
		const string key = line.substr(0, i);
		const string val = line.substr(i + 1);
		if (key.find("/") == string::npos) {
			for (const auto &[type, attr] : STYLE_ATTRIBUTES) {
				if (key == attr) {
					style[type] = val;
					break;
				}
			}
		} else {
			for (Application &app : applications) {
				if (app.id == key) {
					app.count = stoi(val);
					break;
				}
			}
		}
	}
}

void writeConfig () {
	ofstream outfile;
	outfile.open(CONFIG);
	outfile << "[Style]\n";
	for (const auto &[type, attr] : STYLE_ATTRIBUTES) {
		if (style[type] != DEFAULT_STYLE[type]) {
			outfile << STYLE_ATTRIBUTES[type] << "=" << style[type] << "\n";
		}
	}
	outfile << "\n[Application Launch Counts]\n";
	for (const Application &app : applications) {
		if (app.count > 0) {
			outfile << app.id << "=" << app.count << "\n";
		}
	}
	outfile.close();
}

void getApplications () {
	for (const string &dir : APP_DIRS) {
		struct stat info;
		if (stat(dir.c_str(), &info) != 0) { continue; }
		for (const auto &entry : fs::directory_iterator(dir)) {
			Application app = {};
			app.id = entry.path();
			ifstream infile(app.id);
			string line;
			while (getline(infile, line)) {
				if (app.name == "" && line.find("Name=") == 0) {
					app.name = line.substr(5);
				}
				if (app.genericName == "" && line.find("GenericName=") == 0) {
					app.genericName = line.substr(12);
				}
				if (app.comment == "" && line.find("Comment=") == 0) {
					app.comment = line.substr(8);
				}
				if (app.cmd == "" && line.find("Exec=") == 0 && line.substr(5) != "") {
					app.cmd = line.substr(5);
				}
				if (app.cmd == "" && line.find("Keywords=") == 0) {
					stringstream ss(lowercase(line.substr(9)));
					string word;
					while (getline(ss, word, ' ')) {
						app.keywords.push_back({ word, 1 });
					}
				}
			}
			
			stringstream ss(lowercase(app.name));
			string word;
			while (getline(ss, word, ' ')) {
				app.keywords.push_back({ word, 1000 });
			}

			stringstream ss2(lowercase(app.genericName + ' ' + app.comment));
			while (getline(ss2, word, ' ')) {
				app.keywords.push_back({ word, 1 });
			}

			applications.push_back(app);
		}
	}
}

void setProperty (const char *property, const char *value) {
	const Atom propertyAtom = XInternAtom(display, property, False);
	const long valueAtom = XInternAtom(display, value, False);
	XChangeProperty(display, window, propertyAtom, XA_ATOM, 32, PropModeReplace, (unsigned char *) &valueAtom, 1);
}

void launch (Application &app) {
	const int pid = fork(); // this duplicates the launcher process
	if (pid == 0) { // if this is the child process, replace it with the application
		chdir(HOME_DIR.c_str());
		stringstream ss(app.cmd);
		vector<char*> args;
		string arg;
		while (getline(ss, arg, ' ')) {
			if (arg.find('%') != 0) {
				string *a = new string(arg);
				args.push_back(const_cast<char *>(a->c_str()));
			}
		}
		args.push_back(NULL);
		char **command = &args[0];
		execvp(command[0], command);
	} else {
		app.count++;
		writeConfig();
	}
	exit(0);
}

void onKeyPress (XEvent &event) {
	char text[128] = {0};
	KeySym keysym;
	int textlength = Xutf8LookupString(xic, &event.xkey, text, sizeof text, &keysym, NULL);
	switch (keysym) {
		case XK_Escape:
			exit(0);
			break;
		case XK_Return:
			launch(*results[selected].app);
			break;
		case XK_Up:
			selected = selected > 0 ? selected - 1 : results.size() - 1;
			break;
		case XK_Down:
			selected = selected < results.size() - 1 ? selected + 1 : 0;
			break;
		case XK_Left:
			cursor = cursor > 0 ? cursor - 1 : 0;
			break;
		case XK_Right:
			cursor = cursor < query.length() ? cursor + 1 : query.length();
			break;
		case XK_Home:
			cursor = 0;
			break;
		case XK_End:
			cursor = query.length();
			break;
		case XK_BackSpace:
			if (cursor > 0) {
				query.erase(cursor - 1, 1);
				cursor--;
			}
			break;
		case XK_Delete:
			if (cursor < query.length()) {
				query.erase(cursor, 1);
			}
			break;
		default:
			if (textlength == 1) { // check it's a character
				query = query.substr(0, cursor) + text + query.substr(cursor, query.length());
				cursor++;
			}
	}
	queryi = lowercase(query);
}

int main () {
	getApplications();
	readConfig();

	display = XOpenDisplay(NULL);
	screen = DefaultScreen(display);
	Visual *visual = DefaultVisual(display, screen);
	Colormap colormap = DefaultColormap(display, screen);
	int depth = DefaultDepth(display, screen);

	for (const StyleAttribute c : COLORS) {
		unsigned short r = stol(style[c].substr(1, 2), nullptr, 16) * 256;
		unsigned short g = stol(style[c].substr(3, 2), nullptr, 16) * 256;
		unsigned short b = stol(style[c].substr(5, 2), nullptr, 16) * 256;
		XRenderColor xrcolor = { .red = r, .green = g, .blue = b, .alpha = 255 * 256 };
		XftColorAllocValue(display, visual, colormap, &xrcolor, &colors[c]);
	}

	for (const StyleAttribute c : FONTS) {
		fonts[c] = XftFontOpenName(display, screen, style[c].c_str());
	}

	XSetWindowAttributes attributes;
	attributes.background_pixel = colors[C_BG].pixel;

	window = XCreateWindow(display, XRootWindow(display, screen),
		-100, -100, 100, 100,
		5, depth, InputOutput, visual, CWBackPixel, &attributes);
	XSelectInput(display, window, ExposureMask | KeyPressMask | FocusChangeMask);
	XIM xim = XOpenIM(display, 0, 0, 0);
	xic = XCreateIC(xim, // input context
		XNInputStyle,   XIMPreeditNothing | XIMStatusNothing,
		XNClientWindow, window,
		XNFocusWindow,  window,
		NULL);

	XGCValues gr_values;
	gc = XCreateGC(display, window, 0, &gr_values);

	setProperty("_NET_WM_WINDOW_TYPE", "_NET_WM_WINDOW_TYPE_DOCK");
	setProperty("_NET_WM_STATE", "_NET_WM_STATE_ABOVE");
	setProperty("_NET_WM_STATE", "_NET_WM_STATE_MODAL");

	XMapWindow(display, window);

	xftdraw = XftDrawCreate(display, window, visual, colormap);

	XEvent event;
	while (1) {
		while (XCheckMaskEvent(display, ExposureMask | KeyPressMask | FocusChangeMask, &event)) {
			if (event.type == KeyPress) { onKeyPress(event); }
			if (event.type == FocusOut) { exit(0); }
			search();
			if (selected >= results.size()) {
				selected = 0;
			}
			render();
		}
		cursorBlink();
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
}
