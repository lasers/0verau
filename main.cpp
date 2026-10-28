#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <clocale>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <deque>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <SFML/Audio.hpp>
#include <ncurses.h>
#include <taglib/fileref.h>
#include <taglib/tag.h>
#include <taglib/audioproperties.h>
#include <curl/curl.h>
#include <vlc/vlc.h>
#include "json.hpp"

using json = nlohmann::json;

struct Track {
  std::string path;      // the file on disk, or the .m3u file of a radio station
  std::string url;       // stream address of a radio station, empty for songs
  std::string title;
  std::string artist;
  std::string album;
  std::string duration;
  long long size = 0;    // file size and modification time: a song is read again only when they change
  long long mtime = 0;
};

struct LyricLine {
  float time;            // seconds, negative when the lyrics are not synchronised
  std::string text;
};

struct LyricsResult {
  std::vector<LyricLine> lines;
  bool synced = true;
  std::string error;     // why there are no lyrics
};

// A list on the screen with its cursor, scroll position and search
struct List {
  std::vector<Track> items;
  int highlight = 0;
  int offset = 0;
  std::string query;
};

// One line of the album list on the left: "All" (empty key) or an album
struct AlbumEntry {
  std::string key;       // the album name in lower case, albums are grouped without regard to case
  std::string name;      // as it is shown
  int count = 0;         // songs in it
};

struct Sidebar {
  std::vector<AlbumEntry> items;
  int highlight = 0;     // the selected album, the song list shows its songs
  int offset = 0;
};

struct Action {
  const char *name;
  int key;
  const char *help;
};

// Config names, default keys and the help text
const Action ACTIONS[] = {
  {"UP", 'i', "Move up"},
  {"DOWN", 'j', "Move down"},
  {"PLAY", 'o', "Play the selected song or station"},
  {"PAUSE", 'p', "Pause or resume (a radio stream is stopped)"},
  {"NEXT_SONG", '&', "Next song or station"},
  {"PREVIOUS_SONG", '*', "Previous song or station"},
  {"SEEKLEFT", ',', "Seek back 5 seconds"},
  {"SEEKRIGHT", '.', "Seek forward 5 seconds"},
  {"VOLUMEUP", '+', "Volume up"},
  {"VOLUMEDOWN", '-', "Volume down"},
  {"SHUFFLE", '!', "Shuffle on/off"},
  {"REPEAT", '@', "Repeat on/off"},
  {"SEARCH", '/', "Search, an empty text clears the search"},
  {"SHOW_HIDE_ALBUM", '$', "Show/hide the album"},
  {"SHOW_HIDE_ARTIST", '#', "Show/hide the artist"},
  {"SHOW_HIDE_LYRICS", '%', "Show/hide the lyrics"},
  {"SHOW_HIDE_ONLINE_RADIO", '^', "Switch between songs and radio stations"},
  {"SHOW_HIDE_SIDEBAR", 'g', "Show/hide the album list on the left"},
  {"SWITCH_FOCUS", '\t', "Switch between the album list and the songs"},
  {"FAVOURITE", 'f', "Add or remove the selected item from the favourites"},
  {"SHOW_HIDE_FAVOURITES", 'F', "Show only the favourites"},
  {"GOTO", ':', "Go to a song by its number"},
  {"QUEUE", 'a', "Add the selected song to the queue"},
  {"SORT", 's', "Sort by path, title, artist or album"},
  {"EXPORT_FAVOURITES", 'E', "Save the favourites as .m3u files in your home folder"},
  {"QUIT", 'q', "Quit"},
};

const char *const SORT_NAMES[] = {"path", "title", "artist", "album"};
const int SORT_MODES = 4;

enum class Source { None, Local, Radio };

struct App {
  std::unordered_map<std::string, int> keys;
  List songs;
  List stations;
  std::vector<Track> allStations;
  bool shuffle = false;
  bool repeat = false;
  bool showAlbum = false;
  bool showArtist = false;
  bool showLyrics = false;
  bool showRadio = false;
  bool showFavs = false;         // only the favourites are listed
  // The album list on the left
  Sidebar sidebar;
  bool showSidebar = true;
  bool sidebarFocus = false;     // the keys move in the album list instead of the songs
  std::string albumFilter;       // key of the selected album, empty for all songs
  std::vector<Track> snapshot;   // the library as the screen last saw it
  unsigned int snapshotVersion = 0xFFFFFFFFU;
  int sortMode = 0;              // index into SORT_NAMES
  float volume = 100.f;
  // What is playing now
  Source source = Source::None;
  Track now;
  bool localActive = false;      // a song was started and has not ended yet
  libvlc_instance_t *vlc = nullptr;
  libvlc_media_player_t *player = nullptr;
  // Songs that play before the normal order continues
  std::deque<Track> queue;
  // Favourites: songs by file, stations by address
  std::set<std::string> favSongs;
  std::set<std::string> favStations;
  // Saved settings
  std::string lastSong;          // the cursor goes back to this song at the start
  bool restoreCursor = false;
  bool stateDirty = false;
  std::chrono::steady_clock::time_point lastStateSave = std::chrono::steady_clock::now();
  // One line message at the bottom
  std::string message;
  std::chrono::steady_clock::time_point messageUntil;
  // Lyrics of the playing song
  std::string lyricsFor;
  LyricsResult lyrics;
  std::future<LyricsResult> lyricsFuture;
  bool lyricsLoading = false;
  unsigned int seenLibraryVersion = 0xFFFFFFFFU;
};

sf::Music music;
std::atomic<bool> running(true);
// The song library is filled by the scanning thread
std::mutex libraryMutex;
std::vector<Track> library;
std::atomic<bool> scanDone(false);
std::atomic<bool> libraryPreloaded(false); // the list came from the saved library and is being checked
std::atomic<unsigned int> libraryVersion(0);
// Title of the radio stream, it is set from a libvlc thread
std::mutex metaMutex;
std::string streamTitle;

// Text helpers
std::string lower(std::string s);
std::string trim(const std::string &s);
std::string keyLabel(int key);
int measure(const std::string &s, int maxCols, size_t &bytes);
std::string fitWidth(const std::string &s, int cols);
std::string ellipsize(const std::string &s, int cols);
std::string formatTime(float duration);
std::string cleanField(std::string s);
std::vector<std::string> splitTabs(const std::string &line);
// Files: settings, favourites and the saved library
std::string homeDir();
std::string configDir();
std::string cacheDir();
std::string hashHex(const std::string &s);
bool writeFileAtomically(const std::string &path, const std::string &content);
std::unordered_map<std::string, Track> loadLibraryCache(const std::string &file, const std::string &root);
bool saveLibraryCache(const std::string &file, const std::string &root, const std::vector<Track> &tracks);
void loadState(App &app);
bool saveState(App &app);
void loadFavourites(App &app);
bool saveFavourites(const App &app);
bool isFavourite(const App &app, const Track &track, bool radio);
void toggleFavourite(App &app, const Track &track, bool radio);
void exportFavourites(App &app);
// Library and stations
Track readMetadata(const std::filesystem::path &filePath);
void scanLibrary(const std::string &root, const std::string &cacheFile, std::unordered_map<std::string, Track> cached, bool live);
std::vector<Track> filterTracks(const std::vector<Track> &tracks, const std::string &term);
void sortTracks(std::vector<Track> &tracks, int mode);
std::vector<Track> loadStations(const std::string &dir);
std::string albumKey(const Track &track);
void rebuildSidebar(App &app, const std::vector<Track> &base);
void selectAlbum(App &app);
void rebuildSongView(App &app);
void rebuildStationView(App &app);
// Lyrics
static size_t WriteCallback(void *contents, size_t size, size_t nmemb, void *userp);
bool fetchLyrics(const std::string &artist, const std::string &title, std::string &body);
std::vector<LyricLine> parseLrc(const std::string &lrc);
LyricsResult loadLyrics(Track track);
// Keys
int keyFromString(const std::string &val);
std::unordered_map<std::string, int> loadKeyBindings(const std::string &configPath, std::vector<std::string> &warnings);
std::string actionFor(const App &app, int ch);
bool isBound(const App &app, int ch);
// Playback
static void onMediaMeta(const libvlc_event_t *event, void *);
void setMessage(App &app, const std::string &text, int seconds = 4);
void stopRadio(App &app);
bool playLocal(App &app, const Track &track);
void playRadio(App &app, const Track &station);
void playNext(App &app, int direction, bool automatic);
void togglePause(App &app);
void changeVolume(App &app, float delta);
void seekBy(float seconds);
void autoAdvance(App &app);
// Screen
std::string browserLine(const App &app, const Track &track, bool radio, int index, int numberWidth);
void drawHeader(App &app, int rows, int cols);
int sidebarWidth(const App &app, int cols);
void drawSidebar(App &app, int rows, int width);
void drawBrowser(App &app, List &list, bool radio, int rows, int x, int width);
void drawLyrics(App &app, int rows, int cols);
void drawFooter(App &app, int rows, int cols, bool listView);
void drawProgressBarWithTime(float elapsed, float total, int cols, int y);
void showHelp(App &app);
std::string promptLine(int rows, const std::string &label);
void doSearch(App &app, int rows);
void gotoNumber(App &app, int rows);
void handleKey(App &app, int ch, int rows, int cols);
void runUi(App &app);
void onSignal(int);

int main(int argc, char *argv[]) {
  if (argc < 2) {
    std::cerr << "You must provide some folder with music in it and if you have radio.m3u folder (as second argument) for listening to online radio stations." << std::endl;
    return EXIT_FAILURE;
  }
  const std::string musicDir = argv[1];
  std::error_code ec;
  if (!std::filesystem::is_directory(musicDir, ec)) {
    std::cerr << musicDir << " is not a folder." << std::endl;
    return EXIT_FAILURE;
  }
  std::setlocale(LC_ALL, ""); // UTF-8 titles, needs ncursesw to draw them
  std::signal(SIGINT, onSignal);
  std::signal(SIGTERM, onSignal);
  curl_global_init(CURL_GLOBAL_DEFAULT); // once, before any other thread starts

  App app;
  std::vector<std::string> warnings;
  app.keys = loadKeyBindings(homeDir() + "/0verau.conf", warnings);
  loadState(app);
  loadFavourites(app);
  if (!app.showSidebar) { app.albumFilter.clear(); }
  if (!warnings.empty()) {
    std::string all;
    for (const auto &w : warnings) { all += (all.empty() ? "" : "; ") + w; }
    setMessage(app, all, 10);
  }

  // The saved library shows the songs at once, the scan then only reads what is new or changed
  const std::string root = std::filesystem::weakly_canonical(musicDir, ec).string();
  const std::string cacheFile = cacheDir() + "/library-" + hashHex(root) + ".tsv";
  std::unordered_map<std::string, Track> cached = loadLibraryCache(cacheFile, root);
  const bool live = cached.empty(); // no saved library: the songs appear one by one while they are found
  if (!live) {
    std::vector<Track> preloaded;
    for (const auto &entry : cached) { preloaded.push_back(entry.second); }
    sortTracks(preloaded, 0);
    library = preloaded;
    libraryVersion++;
    libraryPreloaded = true;
  }
  app.restoreCursor = !app.lastSong.empty();

  std::thread scanner(scanLibrary, musicDir, cacheFile, cached, live);
  std::string failure;
  initscr();
  try {
    if (argc > 2) {
      app.allStations = loadStations(argv[2]);
      rebuildStationView(app);
    }
    const char *vlcArgs[] = {
      "--no-xlib", // Avoid X11 dependency for headless
      "--quiet"
    };
    app.vlc = libvlc_new(sizeof(vlcArgs) / sizeof(vlcArgs[0]), vlcArgs);
    if (!app.vlc) { setMessage(app, "Warning: libVLC could not be started, radio is not available.", 10); }
    start_color();
    use_default_colors();
    init_pair(1, COLOR_GREEN, -1);  // Playing
    init_pair(2, COLOR_YELLOW, -1); // Paused
    init_pair(3, COLOR_RED, -1);    // Stopped
    noecho();
    cbreak();
    keypad(stdscr, TRUE);
    curs_set(0);
    runUi(app);
  } catch (const std::exception &e) {
    failure = e.what();
  }
  // Cleanup, also after an error: the terminal is always restored and the settings are saved
  running = false;
  endwin();
  saveState(app);
  music.stop();
  stopRadio(app);
  if (app.vlc) { libvlc_release(app.vlc); }
  scanner.join();
  curl_global_cleanup();
  if (!failure.empty()) {
    std::cerr << "Error: " << failure << std::endl;
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}

void onSignal(int) {
  running = false;
}

void runUi(App &app) {
  while (running) {
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    // The scanner found more songs, or finished and sorted them
    unsigned int version = libraryVersion.load();
    if (version != app.seenLibraryVersion) {
      app.seenLibraryVersion = version;
      rebuildSongView(app);
      // Put the cursor on the song that played last, unless a key was pressed already
      if (app.restoreCursor) {
        for (size_t i = 0; i < app.songs.items.size(); i++) {
          if (app.songs.items[i].path == app.lastSong) {
            app.songs.highlight = static_cast<int>(i);
            app.restoreCursor = false;
            break;
          }
        }
        if (scanDone) { app.restoreCursor = false; }
      }
    }
    erase();
    drawHeader(app, rows, cols);
    bool listView = true;
    if (app.showRadio) {
      drawBrowser(app, app.stations, true, rows, 0, cols);
    } else if (app.showLyrics) {
      drawLyrics(app, rows, cols);
      listView = false;
    } else {
      // The albums on the left, the songs of the selected album next to them
      const int sidebar = sidebarWidth(app, cols);
      if (sidebar > 0) {
        drawSidebar(app, rows, sidebar);
        drawBrowser(app, app.songs, false, rows, sidebar + 1, cols - sidebar - 1);
      } else {
        drawBrowser(app, app.songs, false, rows, 0, cols);
      }
    }
    drawFooter(app, rows, cols, listView);
    refresh();
    timeout((app.showLyrics && !app.showRadio) ? 50 : 100);
    int ch = getch();
    if (ch != ERR) { handleKey(app, ch, rows, cols); }
    autoAdvance(app);
    // The settings are saved a moment after they change, not at every key press
    if (app.stateDirty && std::chrono::steady_clock::now() - app.lastStateSave > std::chrono::seconds(2)) { saveState(app); }
  }
}

// ---- Text helpers ----

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

std::string trim(const std::string &s) {
  size_t start = s.find_first_not_of(" \t\r\n");
  if (start == std::string::npos) { return ""; }
  size_t end = s.find_last_not_of(" \t\r\n");
  return s.substr(start, end - start + 1);
}

std::string keyLabel(int key) {
  if (key == '\n') { return "Enter"; }
  if (key == ' ') { return "Space"; }
  if (key == '\t') { return "Tab"; }
  if (key > 32 && key < 127) { return std::string(1, static_cast<char>(key)); }
  return "?";
}

// Terminal columns used by the start of s, at most maxCols; bytes is the length of that start.
// Titles are UTF-8, so a byte count would cut characters in half and misplace the text
int measure(const std::string &s, int maxCols, size_t &bytes) {
  std::mbstate_t state{};
  int width = 0;
  size_t pos = 0;
  while (pos < s.size()) {
    wchar_t wc;
    size_t n = std::mbrtowc(&wc, s.c_str() + pos, s.size() - pos, &state);
    int w = 1;
    if (n == static_cast<size_t>(-1) || n == static_cast<size_t>(-2)) {
      n = 1; // not valid in this locale: one byte, one column
      state = std::mbstate_t();
    } else if (n == 0) {
      break;
    } else {
      w = wcwidth(wc);
      if (w < 0) { w = 0; }
    }
    if (width + w > maxCols) { break; }
    width += w;
    pos += n;
  }
  bytes = pos;
  return width;
}

// The part of s that fits into cols columns
std::string fitWidth(const std::string &s, int cols) {
  if (cols <= 0) { return ""; }
  size_t bytes = 0;
  measure(s, cols, bytes);
  return s.substr(0, bytes);
}

std::string ellipsize(const std::string &s, int cols) {
  size_t bytes = 0;
  if (measure(s, INT_MAX, bytes) <= cols) { return s; }
  return fitWidth(s, cols - 3) + "...";
}

// Format seconds into mm:ss
std::string formatTime(float duration) {
  if (!(duration > 0.f)) { duration = 0.f; } // also NaN
  int total = static_cast<int>(duration);
  char buf[32] = {'\0'};
  std::snprintf(buf, sizeof(buf) - 1, "%02d:%02d", total / 60, total % 60);
  return std::string(buf);
}

// A tab or a line break would break the line of a saved file
std::string cleanField(std::string s) {
  std::replace(s.begin(), s.end(), '\t', ' ');
  std::replace(s.begin(), s.end(), '\n', ' ');
  std::replace(s.begin(), s.end(), '\r', ' ');
  return s;
}

std::vector<std::string> splitTabs(const std::string &line) {
  std::vector<std::string> parts;
  size_t start = 0;
  for (;;) {
    size_t tab = line.find('\t', start);
    if (tab == std::string::npos) {
      parts.push_back(line.substr(start));
      break;
    }
    parts.push_back(line.substr(start, tab - start));
    start = tab + 1;
  }
  return parts;
}

// ---- Files: settings, favourites and the saved library ----

std::string homeDir() {
  const char *home = std::getenv("HOME");
  return (home && *home) ? std::string(home) : std::string(".");
}

// ~/.config/0verau, or $XDG_CONFIG_HOME/0verau
std::string configDir() {
  const char *xdg = std::getenv("XDG_CONFIG_HOME");
  return ((xdg && *xdg) ? std::string(xdg) : homeDir() + "/.config") + "/0verau";
}

// ~/.cache/0verau, or $XDG_CACHE_HOME/0verau
std::string cacheDir() {
  const char *xdg = std::getenv("XDG_CACHE_HOME");
  return ((xdg && *xdg) ? std::string(xdg) : homeDir() + "/.cache") + "/0verau";
}

// FNV-1a: the same text always gives the same name for the saved library of a folder
std::string hashHex(const std::string &s) {
  unsigned long long hash = 1469598103934665603ULL;
  for (unsigned char c : s) {
    hash ^= c;
    hash *= 1099511628211ULL;
  }
  char buf[32] = {'\0'};
  std::snprintf(buf, sizeof(buf) - 1, "%016llx", hash);
  return std::string(buf);
}

// Write a temporary file and rename it, so that a crash never leaves half a file
bool writeFileAtomically(const std::string &path, const std::string &content) {
  namespace fs = std::filesystem;
  std::error_code ec;
  fs::create_directories(fs::path(path).parent_path(), ec);
  const std::string tmp = path + ".tmp";
  {
    std::ofstream out(tmp, std::ios::out | std::ios::trunc | std::ios::binary);
    if (!out) { return false; }
    out << content;
    if (!out) { return false; }
  }
  fs::rename(tmp, path, ec);
  return !ec;
}

// The saved library: one song per line, fields separated by tabs. It belongs to one music folder
std::unordered_map<std::string, Track> loadLibraryCache(const std::string &file, const std::string &root) {
  std::unordered_map<std::string, Track> cached;
  std::ifstream in(file);
  if (!in) { return cached; }
  std::string line;
  std::getline(in, line);
  if (line != "0verau-library 1\t" + root) { return cached; }
  while (std::getline(in, line)) {
    std::vector<std::string> f = splitTabs(line);
    if (f.size() != 7 || f[0].empty()) { continue; }
    Track t;
    t.path = f[0];
    t.size = std::strtoll(f[1].c_str(), nullptr, 10);
    t.mtime = std::strtoll(f[2].c_str(), nullptr, 10);
    t.duration = f[3];
    t.title = f[4];
    t.artist = f[5];
    t.album = f[6];
    cached[t.path] = t;
  }
  return cached;
}

bool saveLibraryCache(const std::string &file, const std::string &root, const std::vector<Track> &tracks) {
  std::ostringstream out;
  out << "0verau-library 1\t" << root << "\n";
  for (const auto &t : tracks) {
    if (t.path.find_first_of("\t\r\n") != std::string::npos) { continue; } // can't be written on one line
    out << t.path << '\t' << t.size << '\t' << t.mtime << '\t' << t.duration << '\t'
        << cleanField(t.title) << '\t' << cleanField(t.artist) << '\t' << cleanField(t.album) << '\n';
  }
  return writeFileAtomically(file, out.str());
}

// ~/.config/0verau/state.conf: volume, shuffle, repeat and the other settings of the last run
void loadState(App &app) {
  std::ifstream in(configDir() + "/state.conf");
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') { line.pop_back(); }
    if (line.empty() || line[0] == '#') { continue; }
    size_t eq = line.find('=');
    if (eq == std::string::npos) { continue; }
    const std::string key = trim(line.substr(0, eq));
    const std::string raw = line.substr(eq + 1);
    const std::string val = trim(raw);
    try {
      if (key == "volume") { app.volume = std::clamp(std::stof(val), 0.f, 100.f); }
      else if (key == "shuffle") { app.shuffle = (val == "1"); }
      else if (key == "repeat") { app.repeat = (val == "1"); }
      else if (key == "show_album") { app.showAlbum = (val == "1"); }
      else if (key == "show_artist") { app.showArtist = (val == "1"); }
      else if (key == "favourites_only") { app.showFavs = (val == "1"); }
      else if (key == "sort") { app.sortMode = std::clamp(std::stoi(val), 0, SORT_MODES - 1); }
      else if (key == "show_sidebar") { app.showSidebar = (val == "1"); }
      else if (key == "album") { app.albumFilter = lower(val); }
      else if (key == "last_song") { app.lastSong = raw; }
    } catch (const std::exception &) {
      // a damaged value is ignored, the default stays
    }
  }
}

bool saveState(App &app) {
  std::ostringstream out;
  out << "# Saved by 0verau when you change a setting and when you quit.\n"
      << "# You can edit it while the player is not running.\n"
      << "volume=" << static_cast<int>(app.volume) << "\n"
      << "shuffle=" << (app.shuffle ? 1 : 0) << "\n"
      << "repeat=" << (app.repeat ? 1 : 0) << "\n"
      << "show_album=" << (app.showAlbum ? 1 : 0) << "\n"
      << "show_artist=" << (app.showArtist ? 1 : 0) << "\n"
      << "favourites_only=" << (app.showFavs ? 1 : 0) << "\n"
      << "sort=" << app.sortMode << "\n"
      << "show_sidebar=" << (app.showSidebar ? 1 : 0) << "\n";
  if (!app.lastSong.empty() && app.lastSong.find_first_of("\r\n") == std::string::npos) {
    out << "last_song=" << app.lastSong << "\n";
  }
  if (app.showSidebar && !app.albumFilter.empty() && app.albumFilter.find_first_of("\r\n") == std::string::npos) {
    out << "album=" << app.albumFilter << "\n"; // the selected album is selected again at the next start
  }
  app.stateDirty = false;
  app.lastStateSave = std::chrono::steady_clock::now();
  return writeFileAtomically(configDir() + "/state.conf", out.str());
}

// ~/.config/0verau/favourites.txt: "song<TAB>file" and "station<TAB>address", one per line
void loadFavourites(App &app) {
  std::ifstream in(configDir() + "/favourites.txt");
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') { line.pop_back(); }
    size_t tab = line.find('\t');
    if (tab == std::string::npos || tab + 1 >= line.size()) { continue; }
    const std::string kind = line.substr(0, tab);
    const std::string value = line.substr(tab + 1);
    if (kind == "song") { app.favSongs.insert(value); }
    else if (kind == "station") { app.favStations.insert(value); }
  }
}

bool saveFavourites(const App &app) {
  std::ostringstream out;
  for (const auto &song : app.favSongs) {
    if (song.find_first_of("\r\n") == std::string::npos) { out << "song\t" << song << "\n"; }
  }
  for (const auto &station : app.favStations) {
    if (station.find_first_of("\r\n") == std::string::npos) { out << "station\t" << station << "\n"; }
  }
  return writeFileAtomically(configDir() + "/favourites.txt", out.str());
}

bool isFavourite(const App &app, const Track &track, bool radio) {
  return radio ? app.favStations.count(track.url) > 0 : app.favSongs.count(track.path) > 0;
}

// Add or remove a song or station; the file is saved at once, so a crash loses nothing
void toggleFavourite(App &app, const Track &track, bool radio) {
  std::set<std::string> &favourites = radio ? app.favStations : app.favSongs;
  const std::string &id = radio ? track.url : track.path;
  if (favourites.erase(id) > 0) {
    setMessage(app, "Removed from the favourites: " + track.title);
  } else {
    favourites.insert(id);
    setMessage(app, "Added to the favourites: " + track.title);
  }
  if (!saveFavourites(app)) { setMessage(app, "Error: the favourites could not be saved in " + configDir()); }
  if (app.showFavs) { // the list shows only favourites, so it changes now
    if (radio) { rebuildStationView(app); } else { rebuildSongView(app); }
  }
}

// Save the favourites as playlists that other players can open too
void exportFavourites(App &app) {
  std::vector<Track> all;
  {
    std::lock_guard<std::mutex> lock(libraryMutex);
    all = library;
  }
  std::ostringstream songs;
  songs << "#EXTM3U\n";
  int songCount = 0;
  for (const auto &t : all) {
    if (app.favSongs.count(t.path) && t.path.find_first_of("\r\n") == std::string::npos) {
      songs << "#EXTINF:-1," << cleanField(t.artist) << " - " << cleanField(t.title) << "\n" << t.path << "\n";
      songCount++;
    }
  }
  std::ostringstream stations;
  stations << "#EXTM3U\n";
  int stationCount = 0;
  for (const auto &s : app.allStations) {
    if (app.favStations.count(s.url)) {
      stations << "#EXTINF:-1," << cleanField(s.title) << "\n" << s.url << "\n";
      stationCount++;
    }
  }
  if (songCount == 0 && stationCount == 0) {
    setMessage(app, "No favourites to save yet, press the favourite key on a song first.");
    return;
  }
  bool ok = true;
  if (songCount > 0) { ok = writeFileAtomically(homeDir() + "/0verau-favourites.m3u", songs.str()) && ok; }
  if (stationCount > 0) { ok = writeFileAtomically(homeDir() + "/0verau-favourite-stations.m3u", stations.str()) && ok; }
  if (ok) {
    setMessage(app, "Saved " + std::to_string(songCount) + " songs and " + std::to_string(stationCount) + " stations in " + homeDir());
  } else {
    setMessage(app, "Error: the playlists could not be written in " + homeDir());
  }
}

// ---- Library and stations ----

// Read the tags and the length with TagLib, for every format it knows
Track readMetadata(const std::filesystem::path &filePath) {
  Track info;
  info.path = filePath.string();
  info.title = filePath.filename().string();
  info.artist = "Unknown Artist";
  info.album = "Unknown Album";
  TagLib::FileRef f(filePath.c_str());
  if (!f.isNull()) {
    if (f.tag()) {
      TagLib::Tag *tag = f.tag();
      if (!tag->title().isEmpty()) { info.title = tag->title().to8Bit(true); }
      if (!tag->artist().isEmpty()) { info.artist = tag->artist().to8Bit(true); }
      if (!tag->album().isEmpty()) { info.album = tag->album().to8Bit(true); }
    }
    if (f.audioProperties()) {
      info.duration = formatTime(static_cast<float>(f.audioProperties()->lengthInSeconds()));
    }
  }
  return info;
}

// Runs in its own thread: find the songs in the folder and in its subfolders. A song that is in the
// saved library with the same size and time is not read again. When the scan is done the library
// is saved, so the next start shows the songs at once
void scanLibrary(const std::string &root, const std::string &cacheFile, std::unordered_map<std::string, Track> cached, bool live) {
  namespace fs = std::filesystem;
  std::error_code ec;
  std::vector<Track> fresh;
  fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
  const fs::recursive_directory_iterator end;
  while (!ec && it != end && running) {
    std::error_code fileEc;
    if (it->is_regular_file(fileEc)) {
      std::string ext = lower(it->path().extension().string());
      if (ext == ".wav" || ext == ".ogg" || ext == ".flac" || ext == ".mp3") {
        const std::string path = it->path().string();
        std::error_code sizeEc, timeEc;
        const long long size = static_cast<long long>(it->file_size(sizeEc));
        const long long mtime = static_cast<long long>(it->last_write_time(timeEc).time_since_epoch().count());
        Track track;
        auto known = cached.find(path);
        if (known != cached.end() && !sizeEc && !timeEc && known->second.size == size && known->second.mtime == mtime) {
          track = known->second;
        } else {
          // The tags are read without holding the lock, the screen keeps working
          track = readMetadata(it->path());
          track.size = size;
          track.mtime = mtime;
        }
        fresh.push_back(track);
        if (live) {
          std::lock_guard<std::mutex> lock(libraryMutex);
          library.push_back(track);
          libraryVersion++;
        }
      }
    }
    it.increment(ec);
  }
  if (!running) { return; } // the player is closing: a half finished scan is not saved
  std::sort(fresh.begin(), fresh.end(), [](const Track &a, const Track &b) { return a.path < b.path; });
  {
    std::lock_guard<std::mutex> lock(libraryMutex);
    library = fresh;
    libraryVersion++;
  }
  libraryPreloaded = false;
  scanDone = true;
  saveLibraryCache(cacheFile, root, fresh);
}

// Songs whose title, artist or album contain the text; no text means all of them
std::vector<Track> filterTracks(const std::vector<Track> &tracks, const std::string &term) {
  if (term.empty()) { return tracks; }
  const std::string needle = lower(term);
  std::vector<Track> filtered;
  for (const auto &t : tracks) {
    if (lower(t.title).find(needle) != std::string::npos
        || lower(t.artist).find(needle) != std::string::npos
        || lower(t.album).find(needle) != std::string::npos) {
      filtered.push_back(t);
    }
  }
  return filtered;
}

// mode: 0 path, 1 title, 2 artist, 3 album. Songs that are equal stay in the order of their files
void sortTracks(std::vector<Track> &tracks, int mode) {
  auto field = [mode](const Track &t) -> const std::string & {
    switch (mode) {
      case 1: return t.title;
      case 2: return t.artist;
      case 3: return t.album;
      default: return t.path;
    }
  };
  std::stable_sort(tracks.begin(), tracks.end(), [&field](const Track &a, const Track &b) { return lower(field(a)) < lower(field(b)); });
}

// Albums are grouped by name without regard to case; songs without an album are "Unknown Album"
std::string albumKey(const Track &track) {
  return lower(track.album.empty() ? std::string("Unknown Album") : track.album);
}

// The album list on the left: "All" and every album with its number of songs. base is the library
// after the favourites filter, so the numbers are what the song list can show
void rebuildSidebar(App &app, const std::vector<Track> &base) {
  std::unordered_map<std::string, AlbumEntry> groups;
  for (const auto &t : base) {
    const std::string key = albumKey(t);
    auto found = groups.find(key);
    if (found == groups.end()) {
      AlbumEntry entry;
      entry.key = key;
      entry.name = t.album.empty() ? std::string("Unknown Album") : t.album;
      entry.count = 1;
      groups[key] = entry;
    } else {
      found->second.count++;
    }
  }
  std::vector<AlbumEntry> albums;
  for (const auto &group : groups) { albums.push_back(group.second); }
  std::sort(albums.begin(), albums.end(), [](const AlbumEntry &a, const AlbumEntry &b) { return a.key < b.key; });

  Sidebar &sidebar = app.sidebar;
  sidebar.items.clear();
  AlbumEntry all;
  all.name = "All";
  all.count = static_cast<int>(base.size());
  sidebar.items.push_back(all);
  sidebar.items.insert(sidebar.items.end(), albums.begin(), albums.end());

  // The selected album stays selected when the list changes
  int selected = 0;
  if (!app.albumFilter.empty()) {
    selected = -1;
    for (size_t i = 0; i < sidebar.items.size(); i++) {
      if (sidebar.items[i].key == app.albumFilter) { selected = static_cast<int>(i); break; }
    }
    if (selected < 0) {
      if (!scanDone) {
        // The scan has not found it yet (the saved album of the last run): keep it
        AlbumEntry waiting;
        waiting.key = waiting.name = app.albumFilter;
        sidebar.items.push_back(waiting);
        selected = static_cast<int>(sidebar.items.size()) - 1;
      } else {
        app.albumFilter.clear(); // the album is gone
        selected = 0;
      }
    }
  }
  sidebar.highlight = selected;
}

// The album that is highlighted in the album list becomes the selected one
void selectAlbum(App &app) {
  Sidebar &sidebar = app.sidebar;
  if (sidebar.items.empty()) { return; }
  sidebar.highlight = std::clamp(sidebar.highlight, 0, static_cast<int>(sidebar.items.size()) - 1);
  app.albumFilter = sidebar.items[sidebar.highlight].key;
  app.stateDirty = true;
  rebuildSongView(app);
  app.songs.highlight = 0;
  app.songs.offset = 0;
}

// The list on the screen: the favourites if that is on, the selected album, the search, the sort order
void rebuildSongView(App &app) {
  // The library is copied only when the scanner changed it, moving in the album list stays fast
  const unsigned int version = libraryVersion.load();
  if (version != app.snapshotVersion) {
    std::lock_guard<std::mutex> lock(libraryMutex);
    app.snapshot = library;
    app.snapshotVersion = version;
  }
  std::vector<Track> base;
  if (app.showFavs) {
    for (const auto &t : app.snapshot) {
      if (app.favSongs.count(t.path)) { base.push_back(t); }
    }
  } else {
    base = app.snapshot;
  }
  rebuildSidebar(app, base);
  if (!app.albumFilter.empty()) {
    base.erase(std::remove_if(base.begin(), base.end(), [&app](const Track &t) { return albumKey(t) != app.albumFilter; }), base.end());
  }
  std::vector<Track> view = filterTracks(base, app.songs.query);
  sortTracks(view, app.sortMode);
  app.songs.items = view;
}

void rebuildStationView(App &app) {
  std::vector<Track> view = filterTracks(app.allStations, app.stations.query);
  if (app.showFavs) {
    view.erase(std::remove_if(view.begin(), view.end(), [&app](const Track &t) { return app.favStations.count(t.url) == 0; }), view.end());
  }
  app.stations.items = view;
}

// One station per URL of every .m3u file in the folder. The address is kept in the station itself,
// so the list can be searched and sorted without the stations getting mixed up
std::vector<Track> loadStations(const std::string &dir) {
  namespace fs = std::filesystem;
  std::vector<fs::path> files;
  std::error_code ec;
  for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
    std::error_code fileEc;
    if (it->is_regular_file(fileEc) && lower(it->path().extension().string()) == ".m3u") {
      files.push_back(it->path());
    }
  }
  std::sort(files.begin(), files.end(), [](const fs::path &a, const fs::path &b) { return lower(a.string()) < lower(b.string()); });
  std::vector<Track> stations;
  for (const auto &file : files) {
    std::ifstream in(file);
    if (!in) { continue; } // an unreadable file doesn't hide the other stations
    std::vector<Track> found;
    std::string extTitle;
    bool named = false;
    std::string line;
    while (std::getline(in, line)) {
      line = trim(line);
      if (line.empty()) { continue; }
      if (line.rfind("#EXTINF", 0) == 0) {
        size_t comma = line.find(',');
        extTitle = (comma == std::string::npos) ? "" : trim(line.substr(comma + 1));
        continue;
      }
      if (line[0] == '#') { continue; }
      Track station;
      station.path = file.string();
      station.url = line;
      station.title = extTitle.empty() ? file.stem().string() : extTitle;
      station.artist = "Unknown Artist";
      station.album = "Unknown Album";
      named = named || !extTitle.empty();
      extTitle.clear();
      found.push_back(station);
    }
    if (!named && found.size() > 1) {
      for (size_t i = 0; i < found.size(); i++) { found[i].title += " #" + std::to_string(i + 1); }
    }
    stations.insert(stations.end(), found.begin(), found.end());
  }
  return stations;
}

// ---- Lyrics ----

// Callback function to write received data into a string
static size_t WriteCallback(void *contents, size_t size, size_t nmemb, void *userp) {
  size_t totalSize = size * nmemb;
  static_cast<std::string *>(userp)->append(static_cast<char *>(contents), totalSize);
  return totalSize;
}

// Download the lyrics, false when there are none or the server doesn't answer in time
bool fetchLyrics(const std::string &artist, const std::string &title, std::string &body) {
  std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);
  if (!curl) { return false; }
  auto escape = [&curl](const std::string &s) {
    char *escaped = curl_easy_escape(curl.get(), s.c_str(), static_cast<int>(s.size()));
    std::string result = escaped ? escaped : "";
    curl_free(escaped);
    return result;
  };
  const std::string url = "https://lrclib.net/api/get?artist_name=" + escape(artist) + "&track_name=" + escape(title);
  curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, WriteCallback);
  curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &body);
  curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl.get(), CURLOPT_FAILONERROR, 1L); // a 404 is a failure, not a lyrics file
  curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT, 5L);
  curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, 10L);
  curl_easy_setopt(curl.get(), CURLOPT_USERAGENT, "0verau (https://github.com/su8/0verau)");
  return curl_easy_perform(curl.get()) == CURLE_OK && !body.empty();
}

// Parse .lrc text: "[01:23.45]text", also several times in front of one text
std::vector<LyricLine> parseLrc(const std::string &lrc) {
  std::vector<LyricLine> lines;
  std::istringstream in(lrc);
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') { line.pop_back(); }
    std::vector<float> times;
    size_t pos = 0;
    while (pos < line.size() && line[pos] == '[') {
      size_t close = line.find(']', pos);
      int minutes = 0;
      float seconds = 0.f;
      if (close == std::string::npos || std::sscanf(line.c_str() + pos + 1, "%d:%f", &minutes, &seconds) != 2) { break; }
      times.push_back(static_cast<float>(minutes) * 60.f + seconds);
      pos = close + 1;
    }
    if (times.empty()) { continue; } // tags like [ar:Artist] and empty lines
    const std::string text = trim(line.substr(pos));
    for (float time : times) { lines.push_back({time, text}); }
  }
  std::stable_sort(lines.begin(), lines.end(), [](const LyricLine &a, const LyricLine &b) { return a.time < b.time; });
  return lines;
}

// Runs in its own thread: the cached file next to the song, or the download
LyricsResult loadLyrics(Track track) {
  LyricsResult result;
  try {
    const std::string cacheFile = track.path + ".lrc";
    std::string body;
    {
      std::ifstream in(cacheFile);
      if (in) {
        std::ostringstream all;
        all << in.rdbuf();
        body = all.str();
      }
    }
    bool fromCache = !body.empty();
    if (!fromCache) {
      if (track.artist == "Unknown Artist") {
        result.error = "The song has no artist tag, lyrics can't be looked up.";
        return result;
      }
      if (!fetchLyrics(track.artist, track.title, body)) {
        result.error = "Can't find lyrics for this song.";
        return result;
      }
    }
    // A broken or unexpected answer must not stop the player
    json data = json::parse(body, nullptr, false);
    if (data.is_discarded() || !data.is_object()) {
      result.error = "The lyrics answer can't be read.";
      return result;
    }
    std::string synced, plain;
    if (data.contains("syncedLyrics") && data["syncedLyrics"].is_string()) { synced = data["syncedLyrics"].get<std::string>(); }
    if (data.contains("plainLyrics") && data["plainLyrics"].is_string()) { plain = data["plainLyrics"].get<std::string>(); }
    if (!synced.empty()) {
      result.lines = parseLrc(synced);
    }
    if (result.lines.empty() && !plain.empty()) {
      result.synced = false;
      std::istringstream in(plain);
      std::string line;
      while (std::getline(in, line)) { result.lines.push_back({-1.f, trim(line)}); }
    }
    if (result.lines.empty()) { result.error = "This song has no lyrics (instrumental?)."; }
    if (!fromCache) {
      std::ofstream out(cacheFile, std::ios::out | std::ios::trunc); // the music folder can be read-only: no cache then
      if (out) { out << body; }
    }
  } catch (const std::exception &e) {
    result.lines.clear();
    result.error = std::string("Lyrics error: ") + e.what();
  }
  return result;
}

// ---- Keys ----

int keyFromString(const std::string &val) {
  if (val == "ENTER") return '\n';
  if (val == "SPACE") return ' ';
  if (val == "TAB") return '\t';
  if (val.size() == 1) return static_cast<unsigned char>(val[0]);
  return -1; // Invalid
}

// Load key bindings from the config file; problems go to warnings, the screen is not up yet
std::unordered_map<std::string, int> loadKeyBindings(const std::string &configPath, std::vector<std::string> &warnings) {
  std::unordered_map<std::string, int> keys;
  for (const auto &action : ACTIONS) { keys[action.name] = action.key; }
  std::ifstream file(configPath);
  if (file.is_open()) {
    std::string line;
    while (std::getline(file, line)) {
      line = trim(line);
      if (line.empty() || line[0] == '#') continue; // Skip comments
      size_t eq = line.find('=');
      if (eq == std::string::npos) continue;
      std::string name = trim(line.substr(0, eq));
      std::string val = trim(line.substr(eq + 1));
      if (!keys.count(name)) {
        warnings.push_back("Unknown key binding name: " + name);
        continue;
      }
      int code = keyFromString(val);
      if (code != -1) {
        keys[name] = code;
      } else {
        warnings.push_back("Invalid key binding: " + name + "=" + val);
      }
    }
  }
  const size_t count = sizeof(ACTIONS) / sizeof(ACTIONS[0]);
  for (size_t i = 0; i < count; i++) {
    for (size_t j = i + 1; j < count; j++) {
      if (keys[ACTIONS[i].name] == keys[ACTIONS[j].name]) {
        warnings.push_back(std::string(ACTIONS[i].name) + " and " + ACTIONS[j].name + " use the same key " + keyLabel(keys[ACTIONS[i].name]));
      }
    }
  }
  return keys;
}

// Which action a key stands for. The arrow keys, Page Up/Down, Home/End, Enter and ? work
// without any config, but a key you bound yourself always wins
std::string actionFor(const App &app, int ch) {
  for (const auto &action : ACTIONS) {
    if (app.keys.at(action.name) == ch) { return action.name; }
  }
  switch (ch) {
    case KEY_UP: return "UP";
    case KEY_DOWN: return "DOWN";
    case '\n': case KEY_ENTER: return "PLAY";
    case KEY_LEFT: return "SEEKLEFT";
    case KEY_RIGHT: return "SEEKRIGHT";
    case KEY_PPAGE: return "PAGE_UP";
    case KEY_NPAGE: return "PAGE_DOWN";
    case KEY_HOME: return "HOME";
    case KEY_END: return "END";
    case '?': return "HELP";
  }
  return "";
}

bool isBound(const App &app, int ch) {
  for (const auto &action : ACTIONS) {
    if (app.keys.at(action.name) == ch) { return true; }
  }
  return false;
}

// ---- Playback ----

// Event callback for metadata changes (libvlc), it runs in a libvlc thread
static void onMediaMeta(const libvlc_event_t *event, void *) {
  if (event->type != libvlc_MediaMetaChanged) { return; }
  libvlc_media_t *media = static_cast<libvlc_media_t *>(event->p_obj); // the media that sent the event, never a dangling pointer
  char *nowPlaying = libvlc_media_get_meta(media, libvlc_meta_NowPlaying);
  if (nowPlaying) {
    std::lock_guard<std::mutex> lock(metaMutex);
    streamTitle = nowPlaying;
    libvlc_free(nowPlaying);
  }
}

void setMessage(App &app, const std::string &text, int seconds) {
  app.message = text;
  app.messageUntil = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
}

void stopRadio(App &app) {
  if (app.player) {
    libvlc_media_player_stop(app.player);
    libvlc_media_player_release(app.player);
    app.player = nullptr; // a second release would crash
  }
  {
    std::lock_guard<std::mutex> lock(metaMutex);
    streamTitle.clear();
  }
  if (app.source == Source::Radio) { app.source = Source::None; }
}

bool playLocal(App &app, const Track &track) {
  stopRadio(app);
  if (!music.openFromFile(track.path)) {
    app.localActive = false;
    setMessage(app, "Error: Cannot play file.");
    return false;
  }
  music.setVolume(app.volume);
  music.play();
  app.source = Source::Local;
  app.now = track;
  app.localActive = true;
  app.lastSong = track.path; // saved, the cursor is here at the next start
  app.stateDirty = true;
  return true;
}

void playRadio(App &app, const Track &station) {
  if (!app.vlc) {
    setMessage(app, "Error: libVLC is not available.");
    return;
  }
  if (music.getStatus() != sf::Music::Stopped) { music.stop(); }
  app.localActive = false;
  stopRadio(app);
  libvlc_media_t *media = libvlc_media_new_location(app.vlc, station.url.c_str());
  if (!media) {
    setMessage(app, "Error: Cannot open the stream.");
    return;
  }
  // Attach event listener for metadata changes
  libvlc_event_attach(libvlc_media_event_manager(media), libvlc_MediaMetaChanged, onMediaMeta, nullptr);
  app.player = libvlc_media_player_new_from_media(media);
  libvlc_media_release(media);
  if (!app.player) {
    setMessage(app, "Error: Cannot create the player.");
    return;
  }
  libvlc_media_player_play(app.player);
  libvlc_audio_set_volume(app.player, static_cast<int>(app.volume));
  app.source = Source::Radio;
  app.now = station;
}

// Play the next or previous song; automatic is true when the song ended by itself.
// The songs of the queue come first
void playNext(App &app, int direction, bool automatic) {
  const std::vector<Track> &songs = app.songs.items;
  if (direction > 0 && !app.queue.empty() && !(automatic && app.repeat)) {
    Track queued = app.queue.front();
    app.queue.pop_front();
    for (size_t i = 0; i < songs.size(); i++) {
      if (songs[i].path == queued.path) { app.songs.highlight = static_cast<int>(i); break; }
    }
    playLocal(app, queued);
    return;
  }
  if (songs.empty()) { return; }
  const int count = static_cast<int>(songs.size());
  // Find the playing song again by its file: the list may have been searched or sorted since
  int current = -1;
  for (int i = 0; i < count; i++) {
    if (songs[i].path == app.now.path) { current = i; break; }
  }
  if (current < 0) { current = std::clamp(app.songs.highlight, 0, count - 1); }
  int next;
  if (automatic && app.repeat) {
    next = current;
  } else if (app.shuffle && direction > 0) {
    static std::mt19937 rng(std::random_device{}());
    next = current;
    if (count > 1) {
      std::uniform_int_distribution<int> dist(0, count - 2);
      next = dist(rng);
      if (next >= current) { next++; } // never the same song twice in a row
    }
  } else {
    next = ((current + direction) % count + count) % count;
  }
  app.songs.highlight = next;
  playLocal(app, songs[next]);
}

void togglePause(App &app) {
  if (app.player) { // a live stream can't be resumed, it is stopped
    stopRadio(app);
    return;
  }
  if (music.getStatus() == sf::Music::Playing) {
    music.pause();
  } else if (music.getStatus() == sf::Music::Paused) {
    music.play();
  }
}

void changeVolume(App &app, float delta) {
  app.volume = std::clamp(app.volume + delta, 0.f, 100.f);
  music.setVolume(app.volume);
  if (app.player) { libvlc_audio_set_volume(app.player, static_cast<int>(app.volume)); }
  app.stateDirty = true;
}

void seekBy(float seconds) {
  if (music.getStatus() == sf::Music::Stopped) { return; }
  float position = music.getPlayingOffset().asSeconds() + seconds;
  position = std::clamp(position, 0.f, music.getDuration().asSeconds());
  music.setPlayingOffset(sf::seconds(position));
}

// Auto-play next track when a song ended
void autoAdvance(App &app) {
  if (app.source == Source::Local && app.localActive && music.getStatus() == sf::Music::Stopped) {
    app.localActive = false; // if the next song can't be opened, don't try again every frame
    playNext(app, 1, true);
  }
}

// ---- Screen ----

// A line of the list: "  12. >+ Artist  Title  03:45". > is the playing item, + a favourite.
// The numbers go from 1 to the number of items shown
std::string browserLine(const App &app, const Track &track, bool radio, int index, int numberWidth) {
  const std::string number = std::to_string(index + 1);
  std::string line = std::string(static_cast<size_t>(std::max(0, numberWidth - static_cast<int>(number.size()))), ' ') + number + ". ";
  const bool playing = (app.source == Source::Local && !radio && track.path == app.now.path)
    || (app.source == Source::Radio && radio && track.url == app.now.url);
  line += playing ? '>' : ' ';
  line += isFavourite(app, track, radio) ? '+' : ' ';
  line += ' ';
  if (!radio && app.showAlbum) { line += track.album + "  "; }
  if (!radio && app.showArtist) { line += track.artist + "  "; }
  line += track.title;
  if (!track.duration.empty()) { line += "  " + track.duration; }
  return line;
}

void drawHeader(App &app, int rows, int cols) {
  (void)rows;
  int colorPair = 3;
  std::string status = "Stopped";
  if (app.player || music.getStatus() == sf::Music::Playing) {
    status = "Playing";
    colorPair = 1;
  } else if (music.getStatus() == sf::Music::Paused) {
    status = "Paused";
    colorPair = 2;
  }
  std::string title = "No track selected";
  if (app.source == Source::Local) {
    title = app.now.title;
  } else if (app.source == Source::Radio) {
    std::lock_guard<std::mutex> lock(metaMutex);
    title = streamTitle.empty() ? app.now.title : streamTitle;
  }
  attron(COLOR_PAIR(colorPair) | A_BOLD);
  mvaddstr(0, 0, status.c_str());
  mvaddstr(0, 15, ellipsize(title, cols - 20).c_str());
  attroff(COLOR_PAIR(colorPair) | A_BOLD);

  auto k = [&app](const char *name) { return keyLabel(app.keys.at(name)); };
  std::string help = k("UP") + " " + k("DOWN") + " Navigate | " + k("PLAY") + " Play | " + k("PAUSE") + " Pause | SEEK "
    + k("SEEKLEFT") + " left " + k("SEEKRIGHT") + " right | " + k("VOLUMEUP") + " " + k("VOLUMEDOWN") + " Volume UP/DOWN | "
    + k("SEARCH") + " Search | " + k("FAVOURITE") + " Favourite | " + k("GOTO") + " Go to | " + k("SHOW_HIDE_SIDEBAR") + " Albums | " + k("QUIT") + " Quit";
  if (!isBound(app, '?')) { help += " | ? Help"; }
  mvaddstr(1, 0, fitWidth(help, cols).c_str()); // a longer line would wrap into the list
}

// The songs or the radio stations; the cursor line is highlighted
void drawBrowser(App &app, List &list, bool radio, int rows, int x, int width) {
  const int visible = std::max(1, rows - 7);
  const int count = static_cast<int>(list.items.size());
  if (count == 0) {
    list.highlight = 0;
    list.offset = 0;
    std::string hint;
    if (app.showFavs) {
      hint = "No favourites here yet. Press " + keyLabel(app.keys.at("FAVOURITE")) + " on an item to add it, "
        + keyLabel(app.keys.at("SHOW_HIDE_FAVOURITES")) + " shows everything again.";
    } else if (radio) {
      hint = "No radio stations. Start 0verau with a folder of .m3u files as the second argument.";
    } else if (scanDone) {
      hint = (list.query.empty() && app.albumFilter.empty()) ? "No songs found." : "No songs match the album or the search.";
    }
    mvaddstr(2, x, fitWidth(hint, width).c_str());
    return;
  }
  list.highlight = std::clamp(list.highlight, 0, count - 1);
  if (list.highlight < list.offset) { list.offset = list.highlight; }
  if (list.highlight >= list.offset + visible) { list.offset = list.highlight - visible + 1; }
  list.offset = std::clamp(list.offset, 0, std::max(0, count - visible));
  const int numberWidth = static_cast<int>(std::to_string(count).size());
  for (int i = 0; i < visible && list.offset + i < count; ++i) {
    const int idx = list.offset + i;
    std::string line = fitWidth(browserLine(app, list.items[idx], radio, idx, numberWidth), width - 1);
    if (idx == list.highlight) {
      size_t bytes = 0;
      line.append(static_cast<size_t>(std::max(0, width - 1 - measure(line, INT_MAX, bytes))), ' '); // the bar spans the whole line
      attron(A_REVERSE);
    }
    mvaddstr(i + 2, x, line.c_str());
    if (idx == list.highlight) { attroff(A_REVERSE); }
  }
}

// Width of the album list, 0 when it is not shown: it belongs to the song list and needs a wide terminal
int sidebarWidth(const App &app, int cols) {
  if (!app.showSidebar || app.showRadio || app.showLyrics || cols < 60) { return 0; }
  return std::clamp(cols / 4, 18, 32);
}

// "All" and the albums on the left. The selected album is marked with >, in focus it is a bar
void drawSidebar(App &app, int rows, int width) {
  Sidebar &sidebar = app.sidebar;
  const int visible = std::max(1, rows - 7);
  const int count = static_cast<int>(sidebar.items.size());
  for (int y = 0; y < visible; y++) { mvaddstr(y + 2, width, "|"); }
  if (count == 0) { return; }
  sidebar.highlight = std::clamp(sidebar.highlight, 0, count - 1);
  if (sidebar.highlight < sidebar.offset) { sidebar.offset = sidebar.highlight; }
  if (sidebar.highlight >= sidebar.offset + visible) { sidebar.offset = sidebar.highlight - visible + 1; }
  sidebar.offset = std::clamp(sidebar.offset, 0, std::max(0, count - visible));
  for (int i = 0; i < visible && sidebar.offset + i < count; i++) {
    const int idx = sidebar.offset + i;
    const AlbumEntry &entry = sidebar.items[idx];
    const bool selected = (idx == sidebar.highlight);
    const std::string countText = std::to_string(entry.count);
    const int room = std::max(1, width - 2 - static_cast<int>(countText.size()) - 1);
    std::string line = std::string(selected ? "> " : "  ") + ellipsize(entry.name, room);
    size_t bytes = 0;
    const int used = measure(line, INT_MAX, bytes);
    // The number of songs is on the right
    line += std::string(static_cast<size_t>(std::max(1, width - used - static_cast<int>(countText.size()))), ' ') + countText;
    line = fitWidth(line, width);
    if (selected && app.sidebarFocus) {
      line.append(static_cast<size_t>(std::max(0, width - measure(line, INT_MAX, bytes))), ' ');
      attron(A_REVERSE);
    } else if (selected) {
      attron(A_BOLD);
    }
    mvaddstr(i + 2, 0, line.c_str());
    attroff(A_REVERSE | A_BOLD);
  }
}

// The lyrics of the playing song, scrolled with the music
void drawLyrics(App &app, int rows, int cols) {
  if (app.source != Source::Local) {
    mvaddstr(2, 0, fitWidth("Lyrics are shown for the songs you play. Start a song, or switch back to the list.", cols).c_str());
    return;
  }
  // Collect a finished download first, then start the one for the new song
  if (app.lyricsLoading && app.lyricsFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
    app.lyrics = app.lyricsFuture.get();
    app.lyricsLoading = false;
  }
  if (!app.lyricsLoading && app.lyricsFor != app.now.path) {
    app.lyricsFor = app.now.path;
    app.lyrics = LyricsResult();
    app.lyricsLoading = true;
    app.lyricsFuture = std::async(std::launch::async, loadLyrics, app.now); // the screen doesn't wait for the network
  }
  if (app.lyricsLoading || app.lyricsFor != app.now.path) {
    mvaddstr(2, 0, "Loading lyrics...");
    return;
  }
  if (!app.lyrics.error.empty()) {
    attron(COLOR_PAIR(3) | A_BOLD);
    mvaddstr(2, 0, fitWidth(app.lyrics.error + " Switch back to the menu with the songs.", cols).c_str());
    attroff(COLOR_PAIR(3) | A_BOLD);
    return;
  }
  const std::vector<LyricLine> &lines = app.lyrics.lines;
  const float currentTime = music.getPlayingOffset().asSeconds();
  const float duration = music.getDuration().asSeconds();
  if (!app.lyrics.synced) {
    // Plain text: show it from the top, moving down as the song goes on
    const int total = static_cast<int>(lines.size());
    const float progress = duration > 0.f ? std::min(1.f, currentTime / duration) : 0.f;
    int first = static_cast<int>(progress * static_cast<float>(total)) - rows / 4;
    first = std::clamp(first, 0, std::max(0, total - 1));
    for (int row = 0; row < rows - 5 && first + row < total; row++) {
      mvaddstr(row + 2, 0, fitWidth(lines[first + row].text, cols).c_str());
    }
    return;
  }
  // The line that is sung now: the last one whose time has come, -1 before the first
  auto after = std::upper_bound(lines.begin(), lines.end(), currentTime, [](float t, const LyricLine &l) { return t < l.time; });
  const int current = static_cast<int>(after - lines.begin()) - 1;
  float scrollOffset = 0.f; // how far we are in the current line, 0..1
  if (current >= 0) {
    const float lineStart = lines[current].time;
    const float lineEnd = (current + 1 < static_cast<int>(lines.size())) ? lines[current + 1].time : duration;
    const float lineDuration = lineEnd - lineStart;
    if (lineDuration > 0.f) { scrollOffset = std::clamp((currentTime - lineStart) / lineDuration, 0.f, 1.f); }
  }
  const int centerY = rows / 2;
  for (int i = -3; i <= 3; i++) {
    const int idx = current + i;
    if (idx < 0 || idx >= static_cast<int>(lines.size())) { continue; }
    const int y = centerY + static_cast<int>((static_cast<float>(i) - scrollOffset) * 2.f); // 2 = line spacing
    if (y < 0 || y >= rows - 3) { continue; }
    const std::string text = fitWidth(lines[idx].text, cols);
    size_t bytes = 0;
    const int x = std::max(0, (cols - measure(text, INT_MAX, bytes)) / 2);
    if (i == 0) { attron(A_BOLD | A_STANDOUT); }
    mvaddstr(y, x, text.c_str());
    if (i == 0) { attroff(A_BOLD | A_STANDOUT); }
  }
}

void drawProgressBarWithTime(float elapsed, float total, int cols, int y) {
  const int width = cols - 20;
  if (width < 1) { return; }
  const float progress = (total > 0.f) ? std::min(1.f, elapsed / total) : 0.f;
  const int filled = static_cast<int>(progress * static_cast<float>(width));
  std::string bar(static_cast<size_t>(filled), '=');
  bar.append(static_cast<size_t>(width - filled), ' ');
  mvaddstr(y, 0, (formatTime(elapsed) + " [" + bar + "] " + formatTime(total)).c_str());
}

void drawFooter(App &app, int rows, int cols, bool listView) {
  auto onOff = [](bool value) { return value ? "ON" : "OFF"; };
  if (listView) {
    const List &list = app.showRadio ? app.stations : app.songs;
    const std::string volume = "Volume: " + std::to_string(static_cast<int>(app.volume)) + "%";
    std::string toggles, info;
    if (app.showRadio) {
      toggles = std::string("Favourites only: ") + onOff(app.showFavs);
      info = "Stations: " + std::to_string(list.items.size()) + " | " + volume;
    } else {
      toggles = std::string("Shuffle: ") + onOff(app.shuffle) + " | Repeat: " + onOff(app.repeat) + " | Show Album " + onOff(app.showAlbum)
        + " | Show Artist " + onOff(app.showArtist) + " | Favourites only: " + onOff(app.showFavs);
      if (!scanDone) {
        info = std::string(libraryPreloaded ? "Checking the library for changes... " : "Scanning for songs, please wait.... ")
          + "(" + std::to_string(list.items.size()) + " found)";
      } else {
        info = "Tracks: " + std::to_string(list.items.size());
        if (!app.albumFilter.empty() && app.sidebar.highlight < static_cast<int>(app.sidebar.items.size())) {
          info += " | Album: " + app.sidebar.items[app.sidebar.highlight].name;
        }
        info += std::string(" | Sort: ") + SORT_NAMES[app.sortMode];
        if (!app.queue.empty()) { info += " | Queue: " + std::to_string(app.queue.size()); }
        info += " | " + volume;
      }
    }
    mvaddstr(rows - 5, 0, fitWidth(toggles, cols).c_str());
    mvaddstr(rows - 4, 0, fitWidth(info, cols).c_str());
    if (!list.query.empty()) { mvaddstr(rows - 3, 0, fitWidth("Search: " + list.query, cols).c_str()); }
  }
  if (app.source == Source::Local && music.getStatus() != sf::Music::Stopped) {
    drawProgressBarWithTime(music.getPlayingOffset().asSeconds(), music.getDuration().asSeconds(), cols, rows - 2);
  }
  if (!app.message.empty() && std::chrono::steady_clock::now() < app.messageUntil) {
    mvaddstr(rows - 1, 0, fitWidth(app.message, cols).c_str());
  }
}

// All the keys on one screen, in columns when the terminal is small
void showHelp(App &app) {
  int rows, cols;
  getmaxyx(stdscr, rows, cols);
  erase();
  mvaddstr(0, 0, fitWidth("Keys (change them in $HOME/0verau.conf)   Markers: > playing, + favourite", cols).c_str());
  const int perColumn = std::max(1, rows - 5);
  const int columnWidth = 46;
  int index = 0;
  for (const auto &action : ACTIONS) {
    const int x = (index / perColumn) * columnWidth;
    const int y = 2 + index % perColumn;
    const std::string label = keyLabel(app.keys.at(action.name));
    std::string entry = label + std::string(static_cast<size_t>(std::max(1, 7 - static_cast<int>(label.size()))), ' ') + action.help;
    if (x < cols) { mvaddstr(y, x, fitWidth(entry, std::min(columnWidth - 1, cols - x)).c_str()); }
    index++;
  }
  mvaddstr(rows - 2, 0, fitWidth("Arrow keys, Page Up/Down, Home/End and Enter work as well. Press any key to go back.", cols).c_str());
  refresh();
  timeout(-1);
  getch();
}

// Ask for a line of text in the bottom area. The input must wait for the keys: the normal
// 100 ms timeout would end it after a fraction of a second
std::string promptLine(int rows, const std::string &label) {
  timeout(-1);
  echo();
  curs_set(1);
  move(rows - 3, 0);
  clrtoeol();
  mvaddstr(rows - 3, 0, label.c_str());
  char buf[256] = {'\0'};
  getnstr(buf, 255);
  noecho();
  curs_set(0);
  return trim(buf);
}

void doSearch(App &app, int rows) {
  List &list = app.showRadio ? app.stations : app.songs;
  list.query = promptLine(rows, "Search: ");
  if (app.showRadio) { rebuildStationView(app); } else { rebuildSongView(app); }
  list.highlight = 0;
  list.offset = 0;
}

// Jump to a number of the list: the songs are numbered from 1
void gotoNumber(App &app, int rows) {
  List &list = app.showRadio ? app.stations : app.songs;
  const int count = static_cast<int>(list.items.size());
  if (count == 0) { return; }
  const std::string text = promptLine(rows, "Go to number (1-" + std::to_string(count) + "): ");
  if (text.empty()) { return; }
  try {
    size_t used = 0;
    const long number = std::stol(text, &used);
    if (used != text.size()) { throw std::invalid_argument("number"); }
    list.highlight = static_cast<int>(std::clamp(number - 1, 0L, static_cast<long>(count - 1)));
  } catch (const std::exception &) {
    setMessage(app, "That is not a number: " + text);
  }
}

void handleKey(App &app, int ch, int rows, int cols) {
  const std::string action = actionFor(app, ch);
  if (!action.empty()) { app.restoreCursor = false; } // the cursor stays where the user put it
  // The album list gets the movement keys while it is in focus, and the songs follow the selected album
  if (sidebarWidth(app, cols) > 0 && app.sidebarFocus) {
    Sidebar &sidebar = app.sidebar;
    const int albums = static_cast<int>(sidebar.items.size());
    const int albumPage = std::max(1, rows - 7);
    bool moved = true;
    if (action == "UP") {
      if (albums > 0) { sidebar.highlight = (sidebar.highlight - 1 + albums) % albums; }
    } else if (action == "DOWN") {
      if (albums > 0) { sidebar.highlight = (sidebar.highlight + 1) % albums; }
    } else if (action == "PAGE_UP") {
      sidebar.highlight = std::max(0, sidebar.highlight - albumPage);
    } else if (action == "PAGE_DOWN") {
      sidebar.highlight = std::max(0, std::min(albums - 1, sidebar.highlight + albumPage));
    } else if (action == "HOME") {
      sidebar.highlight = 0;
    } else if (action == "END") {
      sidebar.highlight = std::max(0, albums - 1);
    } else {
      moved = false;
    }
    if (moved) {
      selectAlbum(app);
      return;
    }
    if (action == "PLAY" || ch == KEY_RIGHT) { // go to the songs of the album
      app.sidebarFocus = false;
      return;
    }
  }
  List &list = app.showRadio ? app.stations : app.songs;
  const int count = static_cast<int>(list.items.size());
  const int page = std::max(1, rows - 7);
  if (action == "UP") {
    if (count > 0) { list.highlight = (list.highlight - 1 + count) % count; }
  } else if (action == "DOWN") {
    if (count > 0) { list.highlight = (list.highlight + 1) % count; }
  } else if (action == "PAGE_UP") {
    list.highlight = std::max(0, list.highlight - page);
  } else if (action == "PAGE_DOWN") {
    list.highlight = std::max(0, std::min(count - 1, list.highlight + page));
  } else if (action == "HOME") {
    list.highlight = 0;
  } else if (action == "END") {
    list.highlight = std::max(0, count - 1);
  } else if (action == "PLAY") {
    if (count > 0) {
      if (app.showRadio) { playRadio(app, list.items[list.highlight]); }
      else { playLocal(app, list.items[list.highlight]); }
    }
  } else if (action == "NEXT_SONG" || action == "PREVIOUS_SONG") {
    const int direction = (action == "NEXT_SONG") ? 1 : -1;
    if (app.showRadio) {
      if (count > 0) {
        list.highlight = ((list.highlight + direction) % count + count) % count;
        playRadio(app, list.items[list.highlight]);
      }
    } else {
      playNext(app, direction, false);
    }
  } else if (action == "PAUSE") {
    togglePause(app);
  } else if (action == "VOLUMEUP") {
    changeVolume(app, 5.f);
  } else if (action == "VOLUMEDOWN") {
    changeVolume(app, -5.f);
  } else if (action == "SHUFFLE") {
    app.shuffle = !app.shuffle;
    app.stateDirty = true;
  } else if (action == "REPEAT") {
    app.repeat = !app.repeat;
    app.stateDirty = true;
  } else if (action == "SHOW_HIDE_ALBUM") {
    app.showAlbum = !app.showAlbum;
    app.stateDirty = true;
  } else if (action == "SHOW_HIDE_ARTIST") {
    app.showArtist = !app.showArtist;
    app.stateDirty = true;
  } else if (action == "SHOW_HIDE_LYRICS") {
    app.showLyrics = !app.showLyrics;
  } else if (action == "SHOW_HIDE_ONLINE_RADIO") {
    app.showRadio = !app.showRadio;
    if (app.showRadio && app.allStations.empty()) { setMessage(app, "No radio stations: give a folder with .m3u files as the second argument."); }
  } else if (action == "FAVOURITE") {
    if (count > 0) { toggleFavourite(app, list.items[list.highlight], app.showRadio); }
  } else if (action == "SHOW_HIDE_FAVOURITES") {
    app.showFavs = !app.showFavs;
    app.stateDirty = true;
    rebuildSongView(app);
    rebuildStationView(app);
    app.songs.highlight = app.songs.offset = 0;
    app.stations.highlight = app.stations.offset = 0;
  } else if (action == "SHOW_HIDE_SIDEBAR") {
    if (app.showRadio || app.showLyrics) {
      // The album list is not part of these views: its setting is not changed where it can't be seen
      setMessage(app, "The album list is hidden in this view. In the song list, " + keyLabel(app.keys.at("SHOW_HIDE_SIDEBAR")) + " shows or hides it.");
    } else {
      app.showSidebar = !app.showSidebar;
      app.sidebarFocus = app.showSidebar;
      app.stateDirty = true;
      if (!app.showSidebar) { // without the album list there is no album to choose: all songs again
        app.albumFilter.clear();
        rebuildSongView(app);
        app.songs.highlight = app.songs.offset = 0;
      }
    }
  } else if (action == "SWITCH_FOCUS") {
    if (app.showRadio || app.showLyrics) {
      setMessage(app, "The album list is hidden in this view.");
    } else if (sidebarWidth(app, cols) > 0) {
      app.sidebarFocus = !app.sidebarFocus;
    } else if (!app.showSidebar) {
      app.showSidebar = true;
      app.sidebarFocus = true;
      app.stateDirty = true;
    } else {
      setMessage(app, "The album list needs the song list and a wider terminal.");
    }
  } else if (action == "GOTO") {
    gotoNumber(app, rows);
  } else if (action == "QUEUE") {
    if (!app.showRadio && count > 0) {
      app.queue.push_back(list.items[list.highlight]);
      setMessage(app, "Queued (" + std::to_string(app.queue.size()) + "): " + list.items[list.highlight].title);
    }
  } else if (action == "SORT") {
    app.sortMode = (app.sortMode + 1) % SORT_MODES;
    app.stateDirty = true;
    rebuildSongView(app);
    setMessage(app, std::string("Songs sorted by ") + SORT_NAMES[app.sortMode]);
  } else if (action == "EXPORT_FAVOURITES") {
    exportFavourites(app);
  } else if (action == "SEARCH") {
    doSearch(app, rows);
  } else if (action == "SEEKLEFT") {
    seekBy(-5.f);
  } else if (action == "SEEKRIGHT") {
    seekBy(5.f);
  } else if (action == "HELP") {
    showHelp(app);
  } else if (action == "QUIT") {
    running = false;
  }
}