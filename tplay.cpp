/*
 * tplay - Terminal Media Player
 * Uses ffplay (audio only) with ncursesw UI
 * Lightweight, designed for old hardware (2011+)
 *
 * Build:
 *   g++ -O2 -std=c++11 -o tplay tplay.cpp -lncursesw -lpthread
 */

#define _XOPEN_SOURCE_EXTENDED 1
#include <ncursesw/ncurses.h>
#include <locale.h>
#include <wchar.h>
#include <string>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <pthread.h>
#include <algorithm>
#include <sstream>
#include <time.h>

// ─── Unicode helpers ──────────────────────────────────────────────────────────

static std::wstring utf8_to_wcs(const std::string& s) {
    std::wstring ws;
    const char* src = s.c_str();
    size_t len = s.size(), i = 0;
    while (i < len) {
        wchar_t wc = 0;
        int b = mbtowc(&wc, src + i, len - i);
        if (b <= 0) { ws += L'?'; i++; }
        else        { ws += wc;   i += b; }
    }
    return ws;
}

static int display_width(const std::string& s) {
    std::wstring ws = utf8_to_wcs(s);
    int w = wcswidth(ws.c_str(), ws.size());
    return (w < 0) ? (int)ws.size() : w;
}

static std::string trunc_display(const std::string& s, int max_w,
                                  const std::string& suf = "...") {
    if (display_width(s) <= max_w) return s;
    int target = max_w - display_width(suf);
    if (target <= 0) return suf.substr(0, max_w);
    std::wstring ws = utf8_to_wcs(s), res;
    int acc = 0;
    for (wchar_t wc : ws) {
        int cw = wcwidth(wc); if (cw < 0) cw = 1;
        if (acc + cw > target) break;
        res += wc; acc += cw;
    }
    std::string out; char mb[MB_CUR_MAX + 1];
    for (wchar_t wc : res) { int n = wctomb(mb, wc); if (n > 0) out.append(mb, n); }
    return out + suf;
}

static void mvaddutf8(int row, int col, const std::string& s) {
    mvaddstr(row, col, s.c_str());
}

// ─── Structs ──────────────────────────────────────────────────────────────────

struct Chapter {
    std::string title;
    double start_time, end_time;
};

struct PlayerState {
    std::string filepath, filename;
    double duration, position;
    int    current_chapter;
    std::vector<Chapter> chapters;
    bool   playing;          // false = paused
    bool   quit;
    pid_t  ffplay_pid;
    int    volume;           // 0–100, passed to ffplay -volume
    pthread_mutex_t lock;
};

static PlayerState G;
static volatile bool g_resize = false;

// Wall-clock for position estimation
struct PlayClock {
    double          start_pos;
    struct timespec start_wall;
    // When paused: we record the position at pause time and stop advancing
    bool            paused;
    double          pause_pos;
};
static PlayClock PC;

// Volume debounce: after last volume keypress, wait 500ms before restarting
static struct timespec g_vol_dirty = {0, 0};   // time of last vol change
static bool            g_vol_pending = false;  // restart needed?

// ─── Time helpers ─────────────────────────────────────────────────────────────

static double mono_now() {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

// ─── Helpers ──────────────────────────────────────────────────────────────────

static std::string basename_of(const std::string& path) {
    size_t p = path.rfind('/');
    return (p == std::string::npos) ? path : path.substr(p + 1);
}

static std::string fmt_time(double secs) {
    if (secs < 0) secs = 0;
    int h = (int)secs / 3600, m = ((int)secs % 3600) / 60, s = (int)secs % 60;
    char buf[32];
    if (h > 0) snprintf(buf, sizeof(buf), "%d:%02d:%02d", h, m, s);
    else        snprintf(buf, sizeof(buf), "%d:%02d", m, s);
    return buf;
}

// ─── Media probing ────────────────────────────────────────────────────────────

static bool probe_media(const std::string& path,
                        double& duration, std::vector<Chapter>& chapters)
{
    {
        std::string cmd = "ffprobe -v quiet -show_entries format=duration "
                          "-of default=noprint_wrappers=1:nokey=1 \""
                          + path + "\" 2>/dev/null";
        FILE* f = popen(cmd.c_str(), "r");
        if (!f) return false;
        char buf[64] = {0};
        if (fgets(buf, sizeof(buf), f)) duration = atof(buf);
        pclose(f);
    }
    if (duration <= 0) return false;

    {
        std::string cmd = "ffprobe -v quiet -print_format csv -show_chapters \""
                          + path + "\" 2>/dev/null";
        FILE* f = popen(cmd.c_str(), "r");
        if (!f) return true;
        char line[2048];
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, "chapter,", 8) != 0) continue;
            std::string s(line);
            while (!s.empty() && (s.back()=='\n'||s.back()=='\r')) s.pop_back();
            std::vector<std::string> parts;
            std::stringstream ss(s); std::string tok;
            while (std::getline(ss, tok, ',')) parts.push_back(tok);
            if (parts.size() < 7) continue;
            Chapter ch;
            ch.start_time = atof(parts[4].c_str());
            ch.end_time   = atof(parts[6].c_str());
            if (parts.size() >= 8) {
                ch.title = parts[7];
                for (size_t i = 8; i < parts.size(); i++) ch.title += "," + parts[i];
            } else {
                ch.title = "Chapter " + parts[1];
            }
            chapters.push_back(ch);
        }
        pclose(f);
    }
    return true;
}

static int find_chapter(const std::vector<Chapter>& chs, double pos) {
    if (chs.empty()) return -1;
    for (int i = (int)chs.size() - 1; i >= 0; i--)
        if (pos >= chs[i].start_time) return i;
    return 0;
}

// ─── ffplay management ────────────────────────────────────────────────────────

static void kill_ffplay() {
    if (G.ffplay_pid > 0) {
        kill(G.ffplay_pid, SIGTERM);
        int st; waitpid(G.ffplay_pid, &st, 0);
        G.ffplay_pid = -1;
    }
}

static void spawn_ffplay(double seek_pos) {
    kill_ffplay();
    pid_t pid = fork();
    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        dup2(devnull, STDOUT_FILENO);
        dup2(devnull, STDERR_FILENO);
        close(devnull);
        char ss_buf[32], vol_buf[16];
        snprintf(ss_buf,  sizeof(ss_buf),  "%.2f", seek_pos);
        snprintf(vol_buf, sizeof(vol_buf), "%d",   G.volume);
        // -nodisp  : audio only, no video window (even for .mp4/.mkv)
        // -autoexit: exit when stream ends
        // -volume  : 0–100, internal ffplay volume (does NOT touch system mixer)
        execlp("ffplay", "ffplay",
               "-nodisp", "-autoexit",
               "-ss", ss_buf,
               "-volume", vol_buf,
               G.filepath.c_str(),
               (char*)NULL);
        _exit(1);
    }
    G.ffplay_pid     = pid;
    PC.start_pos     = seek_pos;
    PC.paused        = false;
    clock_gettime(CLOCK_MONOTONIC, &PC.start_wall);
}

static double estimated_position() {
    if (PC.paused) return PC.pause_pos;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double elapsed = (now.tv_sec  - PC.start_wall.tv_sec)
                   + (now.tv_nsec - PC.start_wall.tv_nsec) / 1e9;
    double pos = PC.start_pos + elapsed;
    if (pos > G.duration) pos = G.duration;
    return pos;
}

// ─── Pause / resume ───────────────────────────────────────────────────────────
//
// SIGSTOP freezes the ffplay process at the OS level — zero CPU, audio stops
// instantly. SIGCONT resumes it. No restart needed, no audio glitch.
// We also freeze the wall-clock estimator so the progress bar does not drift.

static void do_pause() {
    if (G.ffplay_pid <= 0) return;
    PC.pause_pos = estimated_position();
    PC.paused    = true;
    kill(G.ffplay_pid, SIGSTOP);
    G.playing = false;
}

static void do_resume() {
    if (G.ffplay_pid <= 0) return;
    // Re-anchor the wall clock to now so estimation stays accurate
    PC.start_pos = PC.pause_pos;
    clock_gettime(CLOCK_MONOTONIC, &PC.start_wall);
    PC.paused    = false;
    kill(G.ffplay_pid, SIGCONT);
    G.playing = true;
}

static void do_toggle_pause() {
    if (G.playing) do_pause();
    else           do_resume();
}

// ─── Seek ─────────────────────────────────────────────────────────────────────

static void do_seek(double new_pos) {
    if (new_pos < 0)          new_pos = 0;
    if (new_pos > G.duration) new_pos = G.duration;
    G.position        = new_pos;
    G.current_chapter = find_chapter(G.chapters, new_pos);
    spawn_ffplay(new_pos);
    // Respect paused state: if we were paused, immediately re-pause new child
    if (!G.playing) {
        // Give ffplay a moment to start before stopping it
        usleep(80000);
        do_pause();
    }
}

// ─── Volume ───────────────────────────────────────────────────────────────────
//
// We change G.volume and mark a pending restart. The main loop restarts ffplay
// once 500ms of silence (no more vol keypresses) has passed. This way rapid
// key-holds only cause one restart at the end, not one per frame.

static void do_volume_change(int delta) {
    G.volume += delta;
    if (G.volume < 0)   G.volume = 0;
    if (G.volume > 100) G.volume = 100;
    // Record time of this change for debounce
    clock_gettime(CLOCK_MONOTONIC, &g_vol_dirty);
    g_vol_pending = true;
}

// Called from main loop: apply volume restart if debounce period has elapsed
static void maybe_apply_volume() {
    if (!g_vol_pending) return;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double age = (now.tv_sec  - g_vol_dirty.tv_sec)
               + (now.tv_nsec - g_vol_dirty.tv_nsec) / 1e9;
    if (age < 0.5) return;  // still within debounce window

    g_vol_pending = false;
    double pos = estimated_position();
    spawn_ffplay(pos);
    if (!G.playing) {
        usleep(80000);
        do_pause();
    }
}

// ─── UI ───────────────────────────────────────────────────────────────────────
//
// Color pairs:
//   1 = cyan   — title bar, playhead
//   2 = green  — filled progress bar
//   3 = white  — empty bar, inactive items
//   4 = yellow — active chapter, chapter markers, volume bar
//   5 = white  — time info, hints
//   6 = red    — PAUSED indicator

static void draw_ui() {
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    if (cols < 20 || rows < 5) return;
    erase();

    double pos = G.position;

    // ── Row 0: title + pause/play indicator ──────────────────────────────────
    attron(COLOR_PAIR(1) | A_BOLD);
    mvhline(0, 0, ' ', cols);
    // Status tag on the left: "[||]" paused or "[>]" playing
    std::string status = G.playing ? " [>] " : " [||] ";
    std::string title  = status + G.filename;
    mvaddutf8(0, 0, trunc_display(title, cols - 1));
    attroff(COLOR_PAIR(1) | A_BOLD);

    // ── Row 1: time + chapter name ────────────────────────────────────────────
    std::string time_str = " " + fmt_time(pos) + " / " + fmt_time(G.duration);
    attron(COLOR_PAIR(5));
    mvaddutf8(1, 0, time_str);
    if (!G.chapters.empty() && G.current_chapter >= 0) {
        const Chapter& c = G.chapters[G.current_chapter];
        std::string ch_str = "[" + std::to_string(G.current_chapter + 1) + "/"
            + std::to_string((int)G.chapters.size()) + "] " + c.title;
        int avail = cols - display_width(time_str) - 3;
        if (avail > 4) {
            ch_str = trunc_display(ch_str, avail);
            int x  = cols - display_width(ch_str) - 1;
            if (x > display_width(time_str) + 2)
                mvaddutf8(1, x, ch_str);
        }
    }
    attroff(COLOR_PAIR(5));

    // ── Row 2: progress bar ───────────────────────────────────────────────────
    int bar_x = 1, bar_w = cols - 2;
    if (bar_w > 0 && G.duration > 0) {
        double frac  = pos / G.duration;
        if (frac > 1) frac = 1;
        int filled = (int)(frac * bar_w);

        attron(COLOR_PAIR(2));
        for (int i = 0; i < filled && i < bar_w; i++) mvaddch(2, bar_x+i, ACS_BLOCK);
        attroff(COLOR_PAIR(2));
        attron(COLOR_PAIR(3));
        for (int i = filled; i < bar_w; i++) mvaddch(2, bar_x+i, ACS_HLINE);
        attroff(COLOR_PAIR(3));

        for (const auto& ch : G.chapters) {
            if (ch.start_time <= 0) continue;
            int mx = bar_x + (int)(ch.start_time / G.duration * bar_w);
            if (mx > bar_x && mx < bar_x + bar_w) {
                attron(COLOR_PAIR(4) | A_BOLD);
                mvaddch(2, mx, '|');
                attroff(COLOR_PAIR(4) | A_BOLD);
            }
        }

        int thumb = bar_x + filled;
        if (thumb >= bar_x + bar_w) thumb = bar_x + bar_w - 1;
        attron(COLOR_PAIR(1) | A_BOLD);
        mvaddch(2, thumb, 'O');
        attroff(COLOR_PAIR(1) | A_BOLD);
    }

    // ── Row 3: volume bar ─────────────────────────────────────────────────────
    {
        // "VOL [########··········] 75%"
        // Fits in one line, always visible
        int vol_bar_w = cols / 3;
        if (vol_bar_w < 10) vol_bar_w = 10;
        if (vol_bar_w > 30) vol_bar_w = 30;
        int filled_v = (int)((double)G.volume / 100.0 * vol_bar_w);

        char vol_pct[16];
        snprintf(vol_pct, sizeof(vol_pct), " %3d%%", G.volume);

        std::string vol_label = " VOL [";
        attron(COLOR_PAIR(5));
        mvaddstr(3, 0, vol_label.c_str());
        attroff(COLOR_PAIR(5));

        int vx = (int)vol_label.size();
        attron(COLOR_PAIR(4));
        for (int i = 0; i < filled_v; i++)   mvaddch(3, vx+i, ACS_BLOCK);
        attroff(COLOR_PAIR(4));
        attron(COLOR_PAIR(3));
        for (int i = filled_v; i < vol_bar_w; i++) mvaddch(3, vx+i, '.');
        attroff(COLOR_PAIR(3));

        attron(COLOR_PAIR(5));
        mvaddch(3, vx + vol_bar_w, ']');
        mvaddstr(3, vx + vol_bar_w + 1, vol_pct);
        attroff(COLOR_PAIR(5));
    }

    // ── Rows 4+: chapter list ─────────────────────────────────────────────────
    int list_row = 4;
    if (!G.chapters.empty() && rows > 9) {
        attron(COLOR_PAIR(5));
        mvaddstr(list_row++, 0, " Chapters:");
        attroff(COLOR_PAIR(5));

        int max_vis = rows - list_row - 2;
        int start_ch = 0;
        if (G.current_chapter >= 0 && G.current_chapter >= max_vis)
            start_ch = G.current_chapter - max_vis + 1;

        for (int i = start_ch;
             i < (int)G.chapters.size() && list_row < rows - 2;
             i++, list_row++)
        {
            bool active = (i == G.current_chapter);
            std::string entry = (active ? " > " : "   ")
                + std::to_string(i+1) + ". "
                + G.chapters[i].title
                + "  [" + fmt_time(G.chapters[i].start_time) + "]";
            entry = trunc_display(entry, cols - 1);
            if (active) attron(COLOR_PAIR(4) | A_BOLD);
            else        attron(COLOR_PAIR(3));
            mvhline(list_row, 0, ' ', cols);
            mvaddutf8(list_row, 0, entry);
            if (active) attroff(COLOR_PAIR(4) | A_BOLD);
            else        attroff(COLOR_PAIR(3));
        }
    }

    // ── Bottom row: key hints ─────────────────────────────────────────────────
    std::string hints = G.chapters.empty()
        ? " [<-/->] seek  [^/v] volume  [space] pause  [q] quit"
        : " [<-/->] seek  [^/v] volume  [space] pause  [i/k] chapter  [q] quit";
    attron(COLOR_PAIR(5));
    mvhline(rows-1, 0, ' ', cols);
    mvaddstr(rows-1, 0, trunc_display(hints, cols-1).c_str());
    attroff(COLOR_PAIR(5));

    refresh();
}

// ─── Signal handlers ─────────────────────────────────────────────────────────

static void handle_sigwinch(int) { g_resize = true; }

static void handle_sigchld(int) {
    int st;
    pid_t p = waitpid(-1, &st, WNOHANG);
    if (p > 0 && p == G.ffplay_pid) {
        G.ffplay_pid = -1;
        if (G.position >= G.duration - 2.0)
            G.quit = true;
    }
}

// ─── Main ─────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: tplay <media_file>\n");
        return 1;
    }

    setlocale(LC_ALL, "");

    if (system("which ffplay >/dev/null 2>&1") != 0) {
        fprintf(stderr, "Error: ffplay not found. Please install ffmpeg.\n");
        return 1;
    }

    memset(&G, 0, sizeof(G));
    G.ffplay_pid = -1;
    G.volume     = 100;
    G.playing    = true;
    pthread_mutex_init(&G.lock, NULL);
    G.filepath = argv[1];
    G.filename = basename_of(G.filepath);

    {
        struct stat st;
        if (stat(G.filepath.c_str(), &st) != 0) {
            fprintf(stderr, "File not found: %s\n", argv[1]);
            return 1;
        }
    }

    fprintf(stderr, "Probing media...\n");
    if (!probe_media(G.filepath, G.duration, G.chapters)) {
        fprintf(stderr, "Failed to probe media. Is the file valid?\n");
        return 1;
    }

    G.current_chapter = 0;
    G.position        = 0.0;

    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    nodelay(stdscr, TRUE);
    timeout(200);

    if (has_colors()) {
        start_color();
        use_default_colors();
        init_pair(1, COLOR_CYAN,   -1);  // title / thumb
        init_pair(2, COLOR_GREEN,  -1);  // filled progress
        init_pair(3, COLOR_WHITE,  -1);  // empty / inactive
        init_pair(4, COLOR_YELLOW, -1);  // volume / active chapter / markers
        init_pair(5, COLOR_WHITE,  -1);  // text / hints
        init_pair(6, COLOR_RED,    -1);  // (reserved for future use)
    }

    signal(SIGWINCH, handle_sigwinch);
    signal(SIGCHLD,  handle_sigchld);

    spawn_ffplay(0.0);

    while (!G.quit) {
        // Update position estimate (only when playing)
        if (G.ffplay_pid > 0) {
            G.position        = estimated_position();
            G.current_chapter = find_chapter(G.chapters, G.position);
        }

        // Apply pending volume change after debounce
        maybe_apply_volume();

        if (g_resize) {
            g_resize = false;
            endwin(); refresh(); clear();
        }

        draw_ui();

        int ch = getch();
        if (ch == ERR) continue;

        switch (ch) {
            case 'q': case 'Q':
                G.quit = true;
                break;
            case ' ':
                do_toggle_pause();
                break;
            case KEY_RIGHT:
                do_seek(G.position + 10.0);
                break;
            case KEY_LEFT:
                do_seek(G.position - 10.0);
                break;
            case KEY_UP:
                do_volume_change(+5);
                break;
            case KEY_DOWN:
                do_volume_change(-5);
                break;
            case 'i': case 'I':
                if (!G.chapters.empty()) {
                    int nx = G.current_chapter + 1;
                    if (nx < (int)G.chapters.size())
                        do_seek(G.chapters[nx].start_time);
                }
                break;
            case 'k': case 'K':
                if (!G.chapters.empty()) {
                    int pv = G.current_chapter - 1;
                    do_seek(pv >= 0 ? G.chapters[pv].start_time : 0.0);
                }
                break;
        }
    }

    kill_ffplay();
    endwin();
    pthread_mutex_destroy(&G.lock);
    printf("\ntplay: playback ended.\n");
    return 0;
}
