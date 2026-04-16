# tplay — Terminal Media Player

A lightweight terminal audio player powered by ffplay.
Designed to run comfortably on old hardware (2011 PCs and up).

Video files (MP4, MKV, etc.) play **audio only** — no video window is opened,
keeping CPU and RAM usage minimal.

Japanese, Chinese, Korean, and other multi-byte filenames and chapter titles
are fully supported.

## UI layout

```
 * 夜に駆ける.m4a
 3:42 / 12:30                          [2/5] 第二章 - Bridge
 XXXXXXXXXXXXXXXX|──────────────────────────O─────────────────
 Chapters:
   1. Intro             [0:00]
 > 2. 第二章 - Bridge   [3:30]
   3. Chorus            [7:00]
   4. Outro             [11:00]
 [<-/->] seek 10s   [i] next ch   [k] prev ch   [q] quit
```

- Top bar: filename (safely truncated for wide CJK characters)
- Second row: elapsed / total time, current chapter on the right
- Progress bar: filled blocks, chapter markers `|`, playhead `O`
- Chapter list: scrolls to keep the active entry visible
- Bottom bar: key hints

## Requirements

| Package | Provides |
|---|---|
| `ffmpeg` | `ffplay` and `ffprobe` |
| `libncursesw5-dev` (or `-devel`) | wide-character terminal UI |
| `g++` | C++11 compiler |

## Installation

### Debian / Ubuntu
```bash
sudo apt install ffmpeg libncursesw5-dev g++
make
sudo make install   # optional: copies tplay to /usr/local/bin
```

### Arch Linux
```bash
sudo pacman -S ffmpeg ncurses gcc
make
```

### Fedora / RHEL
```bash
sudo dnf install ffmpeg ncurses-devel gcc-c++
make
```

### Build manually
```bash
g++ -O2 -std=c++11 -o tplay tplay.cpp -lncursesw -lpthread
```

## Usage

```bash
./tplay song.mp3
./tplay podcast.m4a
./tplay movie.mp4        # audio only — no video window
./tplay 夜に駆ける.flac   # Japanese filenames work fine
```

## Controls

| Key | Action |
|---|---|
| `→` Right arrow | Seek forward 10 seconds |
| `←` Left arrow  | Seek backward 10 seconds |
| `i` | Jump to next chapter |
| `k` | Jump to previous chapter |
| `q` | Quit |

## Why it is light on old hardware

- **No video decoding at all.** The `-nodisp` flag passed to ffplay disables
  the entire video pipeline, even for MP4/MKV. ffplay decodes audio only.
- **Wall-clock position tracking.** Position is estimated from elapsed time
  since the last seek — no polling threads, no repeated ffprobe calls.
- **5 redraws per second.** The ncurses loop wakes every 200 ms.
- **Single process.** The player binary does nothing heavy; all decoding
  happens inside the ffplay child which the OS schedules independently.

## Troubleshooting

**`ffplay not found`**
Install ffmpeg: `sudo apt install ffmpeg`

**Japanese / CJK shows as `?` or boxes**
Your terminal locale must be UTF-8:
```bash
echo $LANG          # should show something like en_US.UTF-8 or ja_JP.UTF-8
locale -a | grep -i utf
```
Also make sure your terminal font includes CJK glyphs (Noto Sans CJK,
WenQuanYi, IPAGothic, etc.).

**Terminal is garbled after a crash**
```bash
reset
```

**Chapters not showing**
The file has no embedded chapter metadata. You can add chapters with
`mkvpropedit` (MKV) or `mp4chaps` (MP4/M4A).
