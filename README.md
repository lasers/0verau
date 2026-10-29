![](1snap.png)

### Lyrics

![](2snap.png)

### Online radio

![](3snap.png)

### Albums and album cover

![](4snap.png)

# 0verau

ncurses music player alternative to cmus

# Compile

```bash
make -j8  # 8 cores/threads to use in parallel compile
sudo/doas make install
```

### Listening to online radio

To start the program you must supply 2 arguments, `0verau mp3/folder 'm3u/folder'` , so you can choose between playing online stream/or local \*.mp3 files, for radio streams the most popular is https://www.internet-radio.com/stations/house/ , download the \*.m3u file and supply it as argument so you should use it like this `0verau mp3/folder m3u/folder` , to start playing the online stream you should press `SHIFT + ^` and it will list the `m3u` radio stations.

# Requirements

In Debian it's `sudo apt install libncurses5-dev libncursesw5-dev libsfml-dev libtag1-dev curl libcurl4-openssl-dev libvlc-dev libvlc-bin vlc`, in your other OS's search for `lib ncurses lib sfml lib tag  lib curl lib vlc vlc`.

---

# keybinds

You can specify your own keybings (before starting **0verau**) in `$HOME/0verau.conf` file:

```bash
# Specifying custom keybindings
UP=i
DOWN=j
PLAY=o
PAUSE=p
QUIT=q
REPEAT=@
SHUFFLE=!
SEARCH=/
VOLUMEUP=+
VOLUMEDOWN=-
SEEKLEFT=,
SEEKRIGHT=.
SHOW_HIDE_ALBUM=$
SHOW_HIDE_ARTIST=#
SHOW_HIDE_LYRICS=%
SHOW_HIDE_ONLINE_RADIO=^
NEXT_SONG=&
PREVIOUS_SONG=*
FAVOURITE=f       # (remove me) pressing f again removes the song entry
SHOW_HIDE_FAVOURITES=F
GOTO=:
QUEUE=a
SORT=s
EXPORT_FAVOURITES=E
QUIT=q
SHOW_HIDE_SIDEBAR=g  # to swtich between sidebar/main menu press TAB
```

---

When searchin  `/` (key pressed) for a song with the search key and type the desired name/album, to get back to the menu with songs, press `/` again followed by `enter` key (don't type anything).