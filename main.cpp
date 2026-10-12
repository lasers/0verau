/*
 * Copyright 10/12/2026 12/07/2025 https://github.com/su8/0verau
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA 02110-1301, USA.
 */
#include <filesystem>
#include <vector>
#include <string>
#include <algorithm>
#include <random>
#include <iostream>
#include <fstream>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <sstream>
#include <cmath>
#include <thread>
#include <chrono>
#include <iomanip>
#include <regex>
#include <atomic>
#include <csignal>
#include <SFML/Audio.hpp>
#include <ncurses.h>
#include <taglib/fileref.h>
#include <taglib/tag.h>
#include <mpg123.h>
#include <curl/curl.h>
#include <vlc/vlc.h>
#include "json.hpp"

struct Track {
  std::string path;
  std::string name;
  std::string title;
  std::string artist;
  std::string album;
  std::string duration;
};

struct LyricLine {
  float time; // seconds
  std::string text;
};

// Draw the lyrics for given song
void drawLyrics(int rows, int cols, std::vector<Track> playlist);
// Draw function tracks and status lines
void drawStatus(int rows, int cols, std::vector<Track> playlist, int highlight, int colorPair, std::string status, int offset, bool shuffle, bool repeat, float volume, std::string &searchQuery, std::unordered_map<std::string, int> keys, int showHideAlbum, int showHideArtist, std::vector<Track> customPlaylist);
// Filter playlist by search term
std::vector<Track> filterTracks(const std::vector<Track> &tracks, const std::string &term);
// List audio files in directory
std::vector<Track> listAudioFiles(const std::string &path);
// List m3u online radio files in directory
std::vector<Track> listM3uFiles(const std::string &path);
// Function to read metadata using TagLib
Track readMetadata(const std::filesystem::path &filePath);
// Function to read the m3u metadata
Track readM3uMetadata(const std::filesystem::path &filePath);
// Parse .lrc file
std::vector<LyricLine> loadLyrics(const std::string &filename);
// Format seconds into mm:ss
std::string formatTime(float duration);
// Draw progress bar with time
void drawProgressBarWithTime(float elapsed, float total, int width, int y, int x);
// Callback function to write received data into a string
static size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp);
// fetch the and save the lyrics
bool fetchLyricsToFile(const std::string &url, const std::string &outputFile);
// Convert string to key code
int keyFromString(const std::string &val);
// Trim whitespace
static inline std::string trim(const std::string &s);
// Parse .m3u playlist
std::vector<std::string> parseM3U(const std::vector<std::string> &filename);
std::vector<std::string> listM3u(const std::string &path);
// Event callback for metadata changes (libvlc)
static void handle_event(const libvlc_event_t* event, void* user_data);
// Signal handler for Ctrl+C
void signal_handler(int);
// Load key bindings from config file
std::unordered_map<std::string, int> loadKeyBindings(const std::string &configPath);

sf::Music music;
int currentLine = 0;
int currentTrack = -1;
std::vector<Track> playlist2;
std::vector<Track> mp3Playlist;
std::vector<Track> m3uPlaylist;
std::string vlcSongMeta = "";
bool vlcPlaying = false;
bool playingMp3 = false;
std::atomic<bool> running(true);
libvlc_media_t *media = nullptr;
std::string trackName = "No track selected";
std::string mp3Name = "";

using json = nlohmann::json;

int main(int argc, char *argv[]) {
  if (argc < 2) { std::cerr << "You must provide some folder with music in it and if you have radio.m3u folder (as second argument) for listening to online radio stations." << std::endl; return EXIT_FAILURE; }
  std::signal(SIGINT, signal_handler);
  std::string musicDir = argv[1]; // Change to your music folder
  auto playlist = listAudioFiles(musicDir);
  if (playlist.empty()) { std::cerr << "No audio files found in " << musicDir << "\n"; return EXIT_FAILURE; }

  mp3Playlist = playlist;
  // Load key bindings from config file
  auto keys = loadKeyBindings((getenv("HOME") ? static_cast<std::string>(getenv("HOME")) : static_cast<std::string>(".")) + static_cast<std::string>("/0verau.conf"));

  // ncurses setup
  initscr();
  start_color();
  use_default_colors();
  init_pair(1, COLOR_GREEN, -1); // Playing
  init_pair(2, COLOR_YELLOW, -1); // Paused
  init_pair(3, COLOR_RED, -1); // Stopped
  noecho();
  cbreak();
  keypad(stdscr, TRUE);
  curs_set(0);
  timeout(200); // Non-blocking getch

  int highlight = 0;
  int offset = 0;
  int choice;
  bool shuffle = false;
  bool repeat = false;
  int showHideAlbum = 0;
  int showHideArtist = 0;
  int showHideLyrics = 0;
  int showOnlineRadio = 0;
  float volume = 100.f;
  std::string searchQuery;
  const char *vlc_args[] = {
    "--no-xlib", // Avoid X11 dependency for headless
    "--quiet"
  };
  libvlc_instance_t *vlc = libvlc_new(sizeof(vlc_args) / sizeof(vlc_args[0]), vlc_args);
  libvlc_media_player_t *player = nullptr;
  std::vector<std::string> parsedM3u;
  if (argc > 2) {
    playlist2 = listM3uFiles(argv[2]);
    parsedM3u = parseM3U(listM3u(argv[2]));
    m3uPlaylist = playlist2;
  }

  while (running) {
    //clear();
    werase(stdscr);
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    // Now Playing section
    std::string status;
    int colorPair = 3; // Default: stopped
    if (music.getStatus() == sf::Music::Playing || vlcPlaying) {
      status = "Playing";
      colorPair = 1;
    } else if (music.getStatus() == sf::Music::Paused || !vlcPlaying) {
      status = "Paused";
      colorPair = 2;
    } else {
      status = "Stopped";
      colorPair = 3;
    }
    if (showHideLyrics == 0 && showOnlineRadio == 0) {
      drawStatus(rows, cols, playlist, highlight, colorPair, status, offset, shuffle, repeat, volume, searchQuery, keys, showHideAlbum, showHideArtist, mp3Playlist);
    }
    else if (showOnlineRadio == 1) {
      drawStatus(rows, cols, playlist2, highlight, colorPair, status, offset, shuffle, repeat, volume, searchQuery, keys, showHideAlbum, showHideArtist, m3uPlaylist);
      std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    else {
      drawLyrics(rows, cols, playlist);
      std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }

    // Show progress bar if playing
    if (music.getStatus() != sf::Music::Stopped) {
      float elapsed = music.getPlayingOffset().asSeconds();
      float total = music.getDuration().asSeconds();
      drawProgressBarWithTime(elapsed, total, cols - 20, rows - 2, 0);
    }

    wrefresh(stdscr);
    choice = getch();
    if (choice == keys["UP"]) {
      if (showOnlineRadio == 0) {
        highlight = (highlight - 1 + playlist.size()) % playlist.size();
      }
      else {
        if (!playlist2.empty()) {
          highlight = (highlight - 1 + playlist2.size()) % playlist2.size();
        }
      }
    }
    else if (choice == keys["DOWN"]) {
      if (showOnlineRadio == 0) {
        highlight = (highlight + 1) % playlist.size();
      }
      else {
        if (!playlist2.empty()) {
          highlight = (highlight + 1) % playlist2.size();
        }
      }
    }
    else if (choice == keys["PLAY"] || choice == keys["NEXT_SONG"] || choice == keys["PREVIOUS_SONG"]) {
      if (player) {
        libvlc_media_player_stop(player);
        libvlc_media_player_release(player);
      }
      if (showOnlineRadio == 0) {
        if (choice == keys["PREVIOUS_SONG"]) {
          highlight = (highlight - 1 + playlist.size()) % playlist.size();
        }
        else if (choice == keys["NEXT_SONG"]) {
          highlight = (highlight + 1 + playlist.size()) % playlist.size();
        }
        if (!music.openFromFile(playlist[highlight].path)) {
          mvprintw(rows - 1, 0, "Error: Cannot play file.");
        } else {
          music.setVolume(volume);
          music.play();
        }
        currentTrack = highlight;
        vlcPlaying = false;
        playingMp3 = true;
        mp3Name = playlist[currentTrack].title;
      }
      else {
        if (!playlist2.empty()) {
          if (choice == keys["PREVIOUS_SONG"]) {
            highlight = (highlight - 1 + playlist2.size()) % playlist2.size();
          }
          else if (choice == keys["NEXT_SONG"]) {
            highlight = (highlight + 1 + playlist2.size()) % playlist2.size();
          }
          media = libvlc_media_new_location(vlc, parsedM3u[highlight].c_str());
          // Attach event listener for metadata changes
          libvlc_event_manager_t *eventManager = libvlc_media_event_manager(media);
          libvlc_event_attach(eventManager, libvlc_MediaMetaChanged, handle_event, media);
          player = libvlc_media_player_new_from_media(media);
          libvlc_media_release(media);
          libvlc_media_player_play(player);
          vlcPlaying = true;
          playingMp3 = false;
          currentTrack = highlight;
          if (music.getStatus() == sf::Music::Playing) {
            music.pause();
          }
        }
      }
    }
    else if (choice == keys["PAUSE"]) {
      if (vlcPlaying) {
        libvlc_media_player_stop(player);
        libvlc_media_player_release(player);
        vlcPlaying = false;
      }
      if (music.getStatus() == sf::Music::Playing) {
        music.pause();
      }
      else if (music.getStatus() == sf::Music::Paused && !vlcPlaying) {
        music.play();
      }
    }
    else if (choice == keys["VOLUMEUP"] || choice == keys["VOLUMEDOWN"]) {
      volume = (choice == keys["VOLUMEUP"]) ? std::min(100.f, volume + 5.f) : std::max(0.f, volume - 5.f);
      if (showOnlineRadio == 0) {
        music.setVolume(volume);
      }
      else {
        if (player) {
          libvlc_audio_set_volume(player, volume);
        }
      }
    }
    else if (choice == keys["SHUFFLE"]) {
      shuffle = !shuffle;
    }
    else if (choice == keys["REPEAT"]) {
      repeat = !repeat;
    }
    else if (choice == keys["SHOW_HIDE_ALBUM"]) {
      showHideAlbum = !showHideAlbum;
    }
    else if (choice == keys["SHOW_HIDE_ARTIST"]) {
      showHideArtist = !showHideArtist;
    }
    else if (choice == keys["SHOW_HIDE_LYRICS"]) {
      showHideLyrics = !showHideLyrics;
    }
    else if (choice == keys["SHOW_HIDE_ONLINE_RADIO"]) {
      showOnlineRadio = !showOnlineRadio;
      if (vlcPlaying) {
        music.pause();
      }
      if (!playlist2.empty()) {
        highlight = (highlight + playlist2.size()) % playlist2.size();
      }
    }
    else if (choice == keys["SEARCH"]) {
      echo();
      curs_set(1);
      char buf[256] = {'\0'};
      mvprintw(rows - 3, 0, "Search: ");
      getnstr(buf, 255);
      searchQuery = buf;
      // Filter playlist
      if (showOnlineRadio == 0) {
        playlist.clear();
        auto allFiles = listAudioFiles(musicDir);
        playlist = filterTracks(allFiles, searchQuery);
      }
      else {
        if (argc > 2) {
          playlist2.clear();
          auto allFiles = listM3uFiles(argv[2]);
          playlist2 = filterTracks(allFiles, searchQuery);
        }
      }
      highlight = 0;
      offset = 0;
      noecho();
      curs_set(0);
    }
    else if (choice == keys["SEEKLEFT"]) {
      if (music.getStatus() != sf::Music::Stopped) {
        float newPos = music.getPlayingOffset().asSeconds() - 5.0f;
        if (newPos < 0) newPos = 0;
        music.setPlayingOffset(sf::seconds(newPos));
      }
    }
    else if (choice == keys["SEEKRIGHT"]) {
      if (music.getStatus() != sf::Music::Stopped) {
        float newPos = music.getPlayingOffset().asSeconds() + 5.0f;
        if (newPos > music.getDuration().asSeconds())
          newPos = music.getDuration().asSeconds();
        music.setPlayingOffset(sf::seconds(newPos));
      }
    }
    else if (choice == keys["QUIT"]) {
      running = false;
    }
    // Auto-play next track
    if (music.getStatus() == sf::Music::Stopped && currentTrack != -1) {
      if (showOnlineRadio == 0 && !vlcPlaying) {
        if (repeat) {
          music.play();
        } else {
          if (shuffle) {
            static std::mt19937 rng(std::random_device{}());
            std::uniform_int_distribution<int> dist(0, playlist.size() - 1);
            highlight = dist(rng);
          } else {
            highlight = (highlight + 1 + playlist.size()) % playlist.size();
          }
          if (!music.openFromFile(playlist[highlight].path)) {
            mvprintw(rows - 1, 0, "Error: Cannot play file.");
          } else {
            music.setVolume(volume);
            music.play();
            currentTrack = highlight;
          }
        }
        mp3Name = playlist[currentTrack].title;
        playingMp3 = true;
        vlcPlaying = false;
      }
      else {
        if (!playingMp3 && player && vlcPlaying) {
          libvlc_media_parse_with_options(media, libvlc_media_parse_network, 0);
          vlcPlaying = true;
        }
      }
    }
  }
  if (player) {
    libvlc_media_player_stop(player);
    libvlc_media_player_release(player);
  }
  libvlc_release(vlc);
  // Cleanup ncurses
  endwin();
  return EXIT_SUCCESS;
}

// Event callback for metadata changes
static void handle_event(const libvlc_event_t *event, void *user_data) {
  if (event->type == libvlc_MediaMetaChanged) {
    libvlc_media_t *media2 = static_cast<libvlc_media_t *>(user_data);
    //const char *title = libvlc_media_get_meta(media2, libvlc_meta_Title);
    //const char *title2 = libvlc_media_get_meta(media2, libvlc_meta_Artist);
    //const char *title3 = libvlc_media_get_meta(media2, libvlc_meta_Album);
    char *title4 = libvlc_media_get_meta(media2, libvlc_meta_NowPlaying);
    if (title4) {
      vlcSongMeta = title4;
      playlist2[currentTrack].title = title4;
      libvlc_free(title4);
    }
  }
}

// Signal handler for Ctrl+C
void signal_handler(int) {
  running = false;
}

// Trim whitespace
static inline std::string trim(const std::string &s) {
  auto start = s.find_first_not_of(" \t\r\n");
  auto end = s.find_last_not_of(" \t\r\n");
  return (start == std::string::npos) ? "" : s.substr(start, end - start + 1);
}

// Parse .m3u playlist
std::vector<std::string> parseM3U(const std::vector<std::string> &filename) {
  std::vector<std::string> urls;
  for (auto &x : filename) {
    std::ifstream file(x);
    if (!file) {
      return urls;
    }
    std::string line;
    while (std::getline(file, line)) {
      line = trim(line);
      if (line.empty() || line[0] == '#') continue;
      urls.push_back(line);
    }
  }
  return urls;
}

// Function to draw the lyrics
void drawLyrics(int rows, int cols, std::vector<Track> playlist) {
  if (music.getStatus() != sf::Music::Playing) {
    return;
  }
  std::string apiUrl = "https://lrclib.net/api/get?artist_name=" + playlist[currentTrack].artist + "&track_name=" + playlist[currentTrack].title;
  std::string api2 = std::regex_replace(apiUrl, std::regex(" "), "%20");
  std::string curLyrFile = std::regex_replace(playlist[currentTrack].path, std::regex(" "), "_") + static_cast<std::string>(".lrc");
  if (!std::filesystem::exists(curLyrFile)) {
    if (!fetchLyricsToFile(api2, curLyrFile)) {
      attron(COLOR_PAIR(3) | A_BOLD);
      printw("Can't find lyrics for this song. Switch back to the menu with the songs.");
      attroff(COLOR_PAIR(3) | A_BOLD);
      return;
    }
  }
  std::ifstream f(curLyrFile);
  json data = json::parse(f);
  std::string songLrc = data["syncedLyrics"];
  f.close();
  std::ofstream outFile;
  outFile.open("/tmp/.song2.lrc", std::ios::out);
  if (!outFile) {
    return;
  }
  outFile << songLrc;
  outFile.close();
  auto lyrics = loadLyrics("/tmp/.song2.lrc");
  float currentTime = music.getPlayingOffset().asSeconds();
  float duration = music.getDuration().asSeconds();
  //float progress = currentTime / duration;

  // Find current line index
  while (currentLine + 1 < lyrics.size() && currentTime >= lyrics[currentLine + 1].time) {
    currentLine++;
  }
  // Calculate smooth scroll offset
  float lineStartTime = lyrics[currentLine].time;
  float lineEndTime = (currentLine + 1 < lyrics.size()) ? lyrics[currentLine + 1].time : duration;
  float lineDuration = lineEndTime - lineStartTime;
  float elapsedInLine = currentTime - lineStartTime;
  float scrollOffset = (elapsedInLine / lineDuration) * 1.0f; // 1.0 = one line height
  // Draw lyrics with fractional offset
  int centerY = rows / 2;
  for (int i = -3; i <= 3; i++) {
    int idx = static_cast<int>(currentLine) + i;
    if (idx >= 0 && idx < static_cast<int>(lyrics.size())) {
      int yPos = centerY + static_cast<int>(((i - scrollOffset) * 2)); // 2 = line spacing
      if (yPos >= 0 && yPos < rows - 3) {
        if (i == 0) attron(A_BOLD | A_STANDOUT);
        mvprintw(yPos, (cols - lyrics[idx].text.size()) / 2, "%s", lyrics[idx].text.c_str());
        if (i == 0) attroff(A_BOLD | A_STANDOUT);
      }
    }
  }
  currentLine = 0;
}

// Draw function tracks and status lines
void drawStatus(int rows, int cols, std::vector<Track> playlist, int highlight, int colorPair, std::string status, int offset, bool shuffle, bool repeat, float volume, std::string &searchQuery, std::unordered_map<std::string, int> keys, int showHideAlbum, int showHideArtist, std::vector<Track> customPlaylist) {
  if (playlist.empty()) {
    return;
  }
  if (currentTrack >= 0 && (currentTrack <= customPlaylist.size()) && !vlcPlaying) {
    unsigned int x = 0U;
    for (auto &z : customPlaylist) {
      if (z.title == playlist[currentTrack].title) {
        trackName = z.title;
        break;
      }
      x++;
    }
    currentTrack = x;
  }
  if (!mp3Name.empty() && !vlcPlaying) {
    trackName = mp3Name;
  }
  if (vlcPlaying) {
    trackName = vlcSongMeta;
  }
  if (static_cast<int>(trackName.size()) > cols - 20) {
    trackName = trackName.substr(0, cols - 23) + "...";
  }

  attron(COLOR_PAIR(colorPair) | A_BOLD);
  mvprintw(0, 0, "%s", status.c_str());
  mvprintw(0, 15, "%s", trackName.c_str());
  attroff(COLOR_PAIR(colorPair) | A_BOLD);

  mvprintw(1, 0, "%c %c Navigate | %c Play | %c Pause | SEEK %c left %c right | %c %c Volume UP/DOWN | %c Search | %c Shuffle | %c Repeat | %c Quit", keys["UP"], keys["DOWN"], keys["PLAY"], keys["PAUSE"], keys["SEEKLEFT"], keys["SEEKRIGHT"], keys["VOLUMEUP"], keys["VOLUMEDOWN"], keys["SEARCH"], keys["SHUFFLE"], keys["REPEAT"], keys["QUIT"]);

  // Show playlist (scrollable)
  int visibleRows = rows - 6;
  //if (highlight < offset) offset = highlight;
  //if (highlight >= offset + visibleRows) offset = highlight - visibleRows + 1;

  for (int i = 0; i < visibleRows && i + offset < static_cast<int>(playlist.size()); ++i) {
    int idx = i + offset;
    if (idx == highlight) attron(A_REVERSE);
    if (highlight < offset) { offset = highlight; };
    if (highlight >= offset + visibleRows) { offset = highlight - visibleRows + 1; }
    //mvprintw(i + 2, 0, "%s", playlist[idx].name.c_str());
    mvprintw(i + 2, 0, "%s %s %s %s", ((showHideAlbum == 1) ? playlist[idx].album.c_str() : ""), ((showHideArtist == 1) ? playlist[idx].artist.c_str() : ""), playlist[idx].title.c_str(), playlist[idx].duration.c_str());
    if (idx == highlight) attroff(A_REVERSE);
  }

  // Show status
  mvprintw(rows - 4, 0, "Tracks: %u | Shuffle: %s | Repeat: %s | Show Album %s | Show Artist %s | Volume: %u%%", static_cast<unsigned int>(playlist.size()), shuffle ? "ON" : "OFF", repeat ? "ON" : "OFF", showHideAlbum ? "ON" : "OFF", showHideArtist ? "ON" : "OFF" , static_cast<unsigned int>(volume));

  // Show search query
  if (!searchQuery.empty()) {
    mvprintw(rows - 3, 0, "Search: %s", searchQuery.c_str());
  }
  #pragma GCC diagnostic push
  #pragma GCC diagnostic ignored "-Wstrict-overflow"
}
#pragma GCC diagnostic pop

// Filter playlist by search term
std::vector<Track> filterTracks(const std::vector<Track> &tracks, const std::string &term) {
  if (term.empty() || (term.length() >= 2 && term[1] == '/')) return tracks;
  std::vector<Track> filtered;
  for (auto &t : tracks) {
    std::string lowerName = t.title;
    std::string lowerTerm = term;
    std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);
    std::transform(lowerTerm.begin(), lowerTerm.end(), lowerTerm.begin(), ::tolower);
    if (lowerName.rfind(lowerTerm) != std::string::npos) {
      filtered.push_back(t);
    }
  }
  return filtered;
}

// List audio files in directory
std::vector<Track> listAudioFiles(const std::string &path) {
  std::vector<Track> files;
  try {
    for (const auto &entry : std::filesystem::directory_iterator(path)) {
      if (entry.is_regular_file()) {
        std::string ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext == ".wav" || ext == ".ogg" || ext == ".flac" || ext == ".mp3") {
          files.push_back(readMetadata(entry.path()));
          //files.push_back({entry.path().string(), entry.path().filename().string()});
        }
      }
    }
  } catch (const std::exception &e) {
    std::cerr << "Error reading directory: " << e.what() << "\n";
  }
  return files;
}

// List m3u online radio files in directory and reads the metadata
std::vector<Track> listM3uFiles(const std::string &path) {
  std::vector<Track> files;
  try {
    for (const auto &entry : std::filesystem::directory_iterator(path)) {
      if (entry.is_regular_file()) {
        std::string ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext == ".m3u") {
          files.push_back(readM3uMetadata(entry.path()));
          //files.push_back({entry.path().string(), entry.path().filename().string()});
        }
      }
    }
  } catch (const std::exception &e) {
    std::cerr << "Error reading directory: " << e.what() << "\n";
  }
  return files;
}

// List only the .m3u files and store their paths
std::vector<std::string> listM3u(const std::string &path) {
  std::vector<std::string> files;
  try {
    for (const auto &entry : std::filesystem::directory_iterator(path)) {
      if (entry.is_regular_file()) {
        std::string ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext == ".m3u") {
          files.push_back(entry.path());
          //files.push_back({entry.path().string(), entry.path().filename().string()});
        }
      }
    }
  } catch (const std::exception &e) {
    std::cerr << "Error reading directory: " << e.what() << "\n";
  }
  return files;
}

// Function to read metadata using TagLib
Track readMetadata(const std::filesystem::path &filePath) {
  Track info;
  mpg123_handle *mh = NULL;
  int err;
  off_t samples;
  long int rate;
  int channeles;
  int encoding;
  unsigned int allOkay = 1U;
  info.path = filePath.string();
  info.title = filePath.filename().string();
  info.artist = "Unknown Artist";
  info.album = "Unknown Album";
  info.duration = "";
  if (mpg123_init() != MPG123_OK) {
    allOkay = 0U;
  }
  if (allOkay == 1U && (mh = mpg123_new(NULL, &err)) == NULL) {
    mpg123_exit();
    allOkay = 0U;
  }
  if (allOkay == 1U && mpg123_open(mh, info.path.c_str()) != MPG123_OK) {
    mpg123_delete(mh);
    mpg123_exit();
    allOkay = 0U;
  }
  if (allOkay == 1U && mpg123_getformat(mh, &rate, &channeles, &encoding) != MPG123_OK) {
    mpg123_close(mh);
    mpg123_delete(mh);
    mpg123_exit();
    allOkay = 0U;
  }
  if (allOkay == 1U && (samples = mpg123_length(mh)) == MPG123_ERR) {
    mpg123_close(mh);
    mpg123_delete(mh);
    mpg123_exit();
    allOkay = 0U;
  }
  TagLib::FileRef f(filePath.c_str());
  if (!f.isNull() && f.tag()) {
    TagLib::Tag *tag = f.tag();
    info.title  = tag->title().isEmpty()  ? filePath.filename().string() : tag->title().to8Bit(true);
    info.artist = tag->artist().isEmpty() ? "Unknown Artist" : tag->artist().to8Bit(true);
    info.album  = tag->album().isEmpty()  ? "Unknown Album" : tag->album().to8Bit(true);
  }
  if (allOkay == 1U) {
    float duration = static_cast<float>(samples) / static_cast<float>(rate);
    info.duration = formatTime(duration);
    mpg123_close(mh);
    mpg123_delete(mh);
    mpg123_exit();
  }
  return info;
}

// Function to read the m3u metadata
Track readM3uMetadata(const std::filesystem::path &filePath) {
  Track info;
  info.path = filePath.string();
  info.title = filePath.filename().string();
  info.artist = "Unknown Artist";
  info.album = "Unknown Album";
  info.duration = "";
  return info;
}

// Format seconds into mm:ss
std::string formatTime(float duration) {
  int mins = static_cast<int>(duration) / 60;
  int secs = static_cast<int>(duration) % 60;
  char buf[256] = {'\0'};
  snprintf(buf, sizeof(buf) - 1, "%02d:%02d", mins, secs);
  return std::string(buf);
}

// Draw progress bar with time
void drawProgressBarWithTime(float elapsed, float total, int width, int y, int x) {
  float progress = (total > 0) ? elapsed / total : 0.f;
  int filled = static_cast<int>(progress * width);

  // Elapsed time
  mvprintw(y, x, "%s ", formatTime(elapsed).c_str());

  // Progress bar
  addch('[');
  for (int i = 0; i < width; ++i) {
    if (i < filled) addch('=');
    else addch(' ');
  }
  addch(']');

  // Total time
  printw(" %s", formatTime(total).c_str());
}

// Parse .lrc file
std::vector<LyricLine> loadLyrics(const std::string &filename) {
  std::vector<LyricLine> lyrics;
  std::ifstream file(filename);
  if (!file) throw std::runtime_error("Cannot open lyrics file");
  std::string line;
  std::regex timeRegex(R"(\[(\d+):(\d+\.\d+)\](.*))");
  std::smatch match;
  while (std::getline(file, line)) {
    if (std::regex_match(line, match, timeRegex)) {
      float minutes = std::stof(match[1]);
      float seconds = std::stof(match[2]);
      std::string text = match[3];
      lyrics.push_back({minutes * 60 + seconds, text});
    }
  }
  return lyrics;
}

// Callback function to write received data into a string
static size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t totalSize = size * nmemb;
    std::string* str = static_cast<std::string*>(userp);
    str->append(static_cast<char*>(contents), totalSize);
    return totalSize;
}

// fetch the and save the lyrics
bool fetchLyricsToFile(const std::string &url, const std::string &outputFile) {
    CURL* curl;
    CURLcode res;
    std::string response;
    long int http_code = 0;

    curl_global_init(CURL_GLOBAL_DEFAULT);
    curl = curl_easy_init();

    if (!curl) {
      std::cerr << "Failed to initialize cURL.\n";
      curl_global_cleanup();
      return false;
    }

    // Set cURL options
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L); // Follow redirects
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mozilla 5.0");

    // Perform the request
    res = curl_easy_perform(curl);

    if (res != CURLE_OK) {
      std::cerr << "cURL error: " << curl_easy_strerror(res) << "\n";
      curl_easy_cleanup(curl);
      curl_global_cleanup();
      return false;
    }

    if (res == CURLE_OK) {
      curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
      if (http_code == 404) {
        return false;
      }
    }

    // Save to .lrc file
    std::ofstream outFile(outputFile, std::ios::out | std::ios::trunc);
    if (!outFile) {
      std::cerr << "Error opening file for writing: " << outputFile << "\n";
      curl_easy_cleanup(curl);
      curl_global_cleanup();
      return false;
    }
    outFile << response;
    outFile.close();

    //std::cout << "Lyrics saved to: " << outputFile << "\n";

    // Cleanup
    curl_easy_cleanup(curl);
    curl_global_cleanup();
    return true;
}

// Convert string to key code
int keyFromString(const std::string &val) {
  if (val == "ENTER") return '\n';
  if (val.size() == 1) return val[0];
  return -1; // Invalid
}

// Load key bindings from config file
std::unordered_map<std::string, int> loadKeyBindings(const std::string &configPath) {
  std::unordered_map<std::string, int> keys = {
    {"UP", 'i'}, {"DOWN", 'j'}, {"PLAY", 'o'}, {"SEEKLEFT", ','}, {"SEEKRIGHT", '.'}, {"NEXT_SONG", '&'}, {"PREVIOUS_SONG", '*'},
    {"PAUSE", 'p'}, {"QUIT", 'q'}, {"REPEAT", '@'}, {"SHOW_HIDE_ALBUM", '$'}, {"SHOW_HIDE_ONLINE_RADIO", '^'},
    {"SHUFFLE", '!'}, {"SEARCH", '/'}, {"VOLUMEUP", '+'}, {"VOLUMEDOWN", '-'}, {"SHOW_HIDE_ARTIST", '#'}, {"SHOW_HIDE_LYRICS", '%'},
  };
  std::ifstream file(configPath);
  if (!file.is_open()) { return keys; }
  std::string line;
  while (std::getline(file, line)) {
    if (line.empty() || line[0] == '#') continue; // Skip comments
    std::istringstream iss(line);
    std::string key, val;
    if (std::getline(iss, key, '=') && std::getline(iss, val)) {
      key.erase(remove_if(key.begin(), key.end(), ::isspace), key.end());
      val.erase(remove_if(val.begin(), val.end(), ::isspace), val.end());
      int code = keyFromString(val);
      if (code != -1) {
        keys[key] = code;
      } else {
        std::cerr << "Invalid key binding: " << key << "=" << val << "\n";
      }
    }
  }
  return keys;
}