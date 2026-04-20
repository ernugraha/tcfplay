/*
 * tcfplay - The Chapter Free Player
 * Version 2026.4.2
 *
 * Build:
 *   g++ -O2 -std=c++11 -o tcfplay tcfplay.cpp -lncursesw -lpthread
 */

#define _XOPEN_SOURCE_EXTENDED 1
#include <ncursesw/ncurses.h>
#include <locale.h>
#include <wchar.h>
#include <math.h>
#include <string>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <fcntl.h>
#include <pthread.h>
#include <algorithm>
#include <sstream>
#include <time.h>

#define TCFPLAY_VERSION "2026.4.2"

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

// ─── Constants ────────────────────────────────────────────────────────────────

#define NUM_BANDS  10
#define VIZ_SR     8000
#define VIZ_CHUNK  512

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
    bool   playing;
    bool   quit;
    bool   video_mode;   // true = show video window
    pid_t  ffplay_pid;
    int    volume;
    // playlist
    std::vector<std::string> playlist;
    int    playlist_idx;
    pthread_mutex_t lock;
};

struct VizState {
    float           bands[NUM_BANDS];
    float           peaks[NUM_BANDS];
    pthread_mutex_t mu;
    pid_t           ffmpeg_pid;
    int             pipe_fd;
    volatile bool   running;
};

static PlayerState G;
static VizState    VIZ;
static volatile bool g_resize = false;

struct PlayClock {
    double          start_pos;
    struct timespec start_wall;
    bool            paused;
    double          pause_pos;
};
static PlayClock PC;

static struct timespec g_vol_dirty   = {0, 0};
static bool            g_vol_pending = false;

// ─── Helpers ──────────────────────────────────────────────────────────────────

static std::string basename_of(const std::string& path) {
    size_t p = path.rfind('/');
    return (p == std::string::npos) ? path : path.substr(p + 1);
}

static std::string fmt_time(double secs) {
    if (secs < 0) secs = 0;
    int h = (int)secs/3600, m = ((int)secs%3600)/60, s = (int)secs%60;
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
    for (int i = (int)chs.size()-1; i >= 0; i--)
        if (pos >= chs[i].start_time) return i;
    return 0;
}

// ─── Spectrum visualizer ─────────────────────────────────────────────────────

struct Biquad {
    float b0, b1, b2, a1, a2;
    float x1, x2, y1, y2;

    void init_bandpass(float freq, float sr, float Q) {
        float omega = 2.0f * (float)M_PI * freq / sr;
        float alpha = sinf(omega) / (2.0f * Q);
        float a0    = 1.0f + alpha;
        b0 =  alpha / a0;  b1 = 0.0f;  b2 = -alpha / a0;
        a1 = (-2.0f * cosf(omega)) / a0;
        a2 = (1.0f - alpha) / a0;
        x1 = x2 = y1 = y2 = 0.0f;
    }

    inline float process(float x) {
        float y = b0*x + b1*x1 + b2*x2 - a1*y1 - a2*y2;
        x2=x1; x1=x; y2=y1; y1=y;
        return y;
    }
};

static const float BAND_FREQS[NUM_BANDS] = {
    100, 200, 350, 550, 800, 1100, 1500, 2000, 2700, 3400
};

static void* viz_thread(void* arg) {
    (void)arg;
    Biquad filters[NUM_BANDS];
    for (int b = 0; b < NUM_BANDS; b++)
        filters[b].init_bandpass(BAND_FREQS[b], (float)VIZ_SR, 1.2f);

    float smooth[NUM_BANDS] = {0};
    float peaks[NUM_BANDS]  = {0};
    const float SMOOTH_UP  = 0.7f;
    const float SMOOTH_DN  = 0.2f;
    const float PEAK_DECAY = 0.05f;

    int16_t buf[VIZ_CHUNK];
    int fd = VIZ.pipe_fd;

    { int fl = fcntl(fd, F_GETFL, 0); fcntl(fd, F_SETFL, fl | O_NONBLOCK); }

    while (VIZ.running) {
        int bytes_needed = VIZ_CHUNK * 2, total = 0;
        while (total < bytes_needed) {
            if (!VIZ.running) goto done;
            fd_set rfds; FD_ZERO(&rfds); FD_SET(fd, &rfds);
            struct timeval tv = {0, 100000};
            int ret = select(fd+1, &rfds, NULL, NULL, &tv);
            if (ret < 0) goto done;
            if (ret == 0) continue;
            int n = read(fd, (char*)buf + total, bytes_needed - total);
            if (n <= 0) goto done;
            total += n;
        }
        if (!VIZ.running) break;

        int N = total / 2;
        float sum_sq[NUM_BANDS] = {0};
        for (int i = 0; i < N; i++) {
            float x = (float)buf[i] / 32768.0f;
            for (int b = 0; b < NUM_BANDS; b++) {
                float y = filters[b].process(x);
                sum_sq[b] += y * y;
            }
        }

        float rms[NUM_BANDS];
        for (int b = 0; b < NUM_BANDS; b++)
            rms[b] = sqrtf(sum_sq[b] / (float)N);

        static float rmax = 1e-4f;
        for (int b = 0; b < NUM_BANDS; b++)
            if (rms[b] > rmax) rmax = rms[b];
        rmax *= 0.998f;
        if (rmax < 1e-4f) rmax = 1e-4f;

        pthread_mutex_lock(&VIZ.mu);
        for (int b = 0; b < NUM_BANDS; b++) {
            float norm  = rms[b] / rmax;
            if (norm > 1.0f) norm = 1.0f;
            float alpha = (norm > smooth[b]) ? SMOOTH_UP : SMOOTH_DN;
            smooth[b]   = alpha * norm + (1.0f - alpha) * smooth[b];
            VIZ.bands[b] = smooth[b];
            if (smooth[b] >= peaks[b]) peaks[b] = smooth[b];
            else peaks[b] *= (1.0f - PEAK_DECAY);
            if (peaks[b] < 0) peaks[b] = 0;
            VIZ.peaks[b] = peaks[b];
        }
        pthread_mutex_unlock(&VIZ.mu);
    }
done:
    pthread_mutex_lock(&VIZ.mu);
    memset(VIZ.bands, 0, sizeof(VIZ.bands));
    memset(VIZ.peaks, 0, sizeof(VIZ.peaks));
    pthread_mutex_unlock(&VIZ.mu);
    return NULL;
}

static pthread_t g_viz_thread;
static bool      g_viz_thread_running = false;

static void stop_viz() {
    if (!g_viz_thread_running && VIZ.ffmpeg_pid <= 0) return;
    VIZ.running = false;
    if (VIZ.pipe_fd >= 0) { close(VIZ.pipe_fd); VIZ.pipe_fd = -1; }
    if (g_viz_thread_running) {
        pthread_join(g_viz_thread, NULL);
        g_viz_thread_running = false;
    }
    if (VIZ.ffmpeg_pid > 0) {
        kill(VIZ.ffmpeg_pid, SIGTERM);
        int st; waitpid(VIZ.ffmpeg_pid, &st, 0);
        VIZ.ffmpeg_pid = -1;
    }
}

static void start_viz(double seek_pos) {
    if (!G.chapters.empty()) return;
    stop_viz();
    int pipefd[2];
    if (pipe(pipefd) < 0) return;
    pid_t pid = fork();
    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO); close(pipefd[1]);
        int devnull = open("/dev/null", O_WRONLY);
        dup2(devnull, STDERR_FILENO); close(devnull);
        char ss_buf[32];
        snprintf(ss_buf, sizeof(ss_buf), "%.2f", seek_pos);
        execlp("ffmpeg", "ffmpeg",
               "-re", "-ss", ss_buf,
               "-i", G.filepath.c_str(),
               "-vn", "-f", "s16le", "-ac", "1", "-ar", "8000",
               "-", (char*)NULL);
        _exit(1);
    }
    close(pipefd[1]);
    VIZ.ffmpeg_pid = pid;
    VIZ.pipe_fd    = pipefd[0];
    VIZ.running    = true;
    pthread_create(&g_viz_thread, NULL, viz_thread, NULL);
    g_viz_thread_running = true;
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
        // In video mode: let ffplay open its own window normally.
        // In audio mode: redirect stdout/stderr and pass -nodisp.
        if (!G.video_mode) {
            int devnull = open("/dev/null", O_WRONLY);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        char ss_buf[32], vol_buf[16];
        snprintf(ss_buf,  sizeof(ss_buf),  "%.2f", seek_pos);
        snprintf(vol_buf, sizeof(vol_buf), "%d",   G.volume);
        if (G.video_mode) {
            execlp("ffplay", "ffplay",
                   "-autoexit",
                   "-ss", ss_buf,
                   "-volume", vol_buf,
                   G.filepath.c_str(),
                   (char*)NULL);
        } else {
            execlp("ffplay", "ffplay",
                   "-nodisp", "-autoexit",
                   "-ss", ss_buf,
                   "-volume", vol_buf,
                   G.filepath.c_str(),
                   (char*)NULL);
        }
        _exit(1);
    }
    G.ffplay_pid  = pid;
    PC.start_pos  = seek_pos;
    PC.paused     = false;
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

static void do_pause() {
    if (G.ffplay_pid <= 0) return;
    PC.pause_pos = estimated_position();
    PC.paused    = true;
    kill(G.ffplay_pid, SIGSTOP);
    if (VIZ.ffmpeg_pid > 0) kill(VIZ.ffmpeg_pid, SIGSTOP);
    G.playing = false;
}

static void do_resume() {
    if (G.ffplay_pid <= 0) return;
    PC.start_pos = PC.pause_pos;
    clock_gettime(CLOCK_MONOTONIC, &PC.start_wall);
    PC.paused    = false;
    kill(G.ffplay_pid, SIGCONT);
    if (VIZ.ffmpeg_pid > 0) kill(VIZ.ffmpeg_pid, SIGCONT);
    G.playing = true;
}

static void do_toggle_pause() {
    if (G.playing) do_pause();
    else           do_resume();
}

// ─── Playlist helpers ─────────────────────────────────────────────────────────

// Load a new file (used for playlist advance and initial load)
static bool load_file(const std::string& path) {
    G.filepath = path;
    G.filename = basename_of(path);
    G.duration = 0;
    G.position = 0;
    G.current_chapter = 0;
    G.chapters.clear();

    if (!probe_media(path, G.duration, G.chapters))
        return false;

    stop_viz();
    spawn_ffplay(0.0);
    start_viz(0.0);
    G.playing = true;
    return true;
}

static void playlist_next() {
    if (G.playlist.empty()) return;
    int next = G.playlist_idx + 1;
    if (next >= (int)G.playlist.size()) { G.quit = true; return; }
    G.playlist_idx = next;
    load_file(G.playlist[next]);
}

// ─── Seek ─────────────────────────────────────────────────────────────────────

static void do_seek(double new_pos) {
    if (new_pos < 0)          new_pos = 0;
    if (new_pos > G.duration) new_pos = G.duration;
    G.position        = new_pos;
    G.current_chapter = find_chapter(G.chapters, new_pos);
    spawn_ffplay(new_pos);
    start_viz(new_pos);
    if (!G.playing) { usleep(80000); do_pause(); }
}

// ─── Volume ───────────────────────────────────────────────────────────────────

static void do_volume_change(int delta) {
    G.volume += delta;
    if (G.volume < 0)   G.volume = 0;
    if (G.volume > 100) G.volume = 100;
    clock_gettime(CLOCK_MONOTONIC, &g_vol_dirty);
    g_vol_pending = true;
}

static void maybe_apply_volume() {
    if (!g_vol_pending) return;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double age = (now.tv_sec  - g_vol_dirty.tv_sec)
               + (now.tv_nsec - g_vol_dirty.tv_nsec) / 1e9;
    if (age < 0.5) return;
    g_vol_pending = false;
    spawn_ffplay(estimated_position());
    if (!G.playing) { usleep(80000); do_pause(); }
}

// ─── UI ───────────────────────────────────────────────────────────────────────

static void draw_viz(int start_row, int end_row, int cols) {
    int viz_rows = end_row - start_row;
    // No label row anymore — use all rows for bars
    if (viz_rows < 1 || cols < NUM_BANDS * 2) return;

    float bands[NUM_BANDS], peaks[NUM_BANDS];
    pthread_mutex_lock(&VIZ.mu);
    memcpy(bands, VIZ.bands, sizeof(bands));
    memcpy(peaks, VIZ.peaks, sizeof(peaks));
    pthread_mutex_unlock(&VIZ.mu);

    int band_w = (cols - 2) / NUM_BANDS;
    if (band_w < 1) band_w = 1;

    for (int b = 0; b < NUM_BANDS; b++) {
        int bx     = 1 + b * band_w;
        int bar_h  = (int)(bands[b] * viz_rows + 0.5f);
        int peak_r = (int)(peaks[b] * viz_rows + 0.5f);
        if (bar_h  > viz_rows) bar_h  = viz_rows;
        if (peak_r > viz_rows) peak_r = viz_rows;

        for (int r = 0; r < viz_rows; r++) {
            int screen_row = start_row + viz_rows - 1 - r;
            bool filled  = (r < bar_h);
            bool is_peak = (r == peak_r && peaks[b] > 0.03f);

            for (int c = 0; c < band_w - 1; c++) {
                if (is_peak) {
                    attron(COLOR_PAIR(1) | A_BOLD);
                    mvaddch(screen_row, bx+c, '-');
                    attroff(COLOR_PAIR(1) | A_BOLD);
                } else if (filled) {
                    int pair = (bands[b] < 0.4f) ? 2
                             : (bands[b] < 0.75f) ? 4 : 1;
                    attron(COLOR_PAIR(pair));
                    mvaddch(screen_row, bx+c, ACS_BLOCK);
                    attroff(COLOR_PAIR(pair));
                } else {
                    mvaddch(screen_row, bx+c, ' ');
                }
            }
        }
    }
}

static void draw_ui() {
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    if (cols < 20 || rows < 5) return;
    erase();

    double pos = G.position;

    // Row 0: title + playlist index if applicable
    attron(COLOR_PAIR(1) | A_BOLD);
    mvhline(0, 0, ' ', cols);
    std::string status = G.playing ? " [>] " : " [||] ";
    std::string title  = status + G.filename;
    if (G.playlist.size() > 1) {
        char idx[16];
        snprintf(idx, sizeof(idx), " (%d/%d)", G.playlist_idx+1, (int)G.playlist.size());
        title += idx;
    }
    mvaddutf8(0, 0, trunc_display(title, cols - 1));
    attroff(COLOR_PAIR(1) | A_BOLD);

    // Row 1: time + chapter name
    std::string time_str = " " + fmt_time(pos) + " / " + fmt_time(G.duration);
    attron(COLOR_PAIR(5));
    mvaddutf8(1, 0, time_str);
    if (!G.chapters.empty() && G.current_chapter >= 0) {
        const Chapter& c = G.chapters[G.current_chapter];
        std::string ch_str = "[" + std::to_string(G.current_chapter+1) + "/"
            + std::to_string((int)G.chapters.size()) + "] " + c.title;
        int avail = cols - display_width(time_str) - 3;
        if (avail > 4) {
            ch_str = trunc_display(ch_str, avail);
            int x  = cols - display_width(ch_str) - 1;
            if (x > display_width(time_str) + 2) mvaddutf8(1, x, ch_str);
        }
    }
    attroff(COLOR_PAIR(5));

    // Row 2: progress bar
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

    // Row 3: volume bar
    {
        int vbw = cols / 3;
        if (vbw < 10) vbw = 10;
        if (vbw > 30) vbw = 30;
        int fv = (int)((double)G.volume / 100.0 * vbw);
        char pct[16]; snprintf(pct, sizeof(pct), " %3d%%", G.volume);
        std::string lbl = " VOL [";
        attron(COLOR_PAIR(5)); mvaddstr(3, 0, lbl.c_str()); attroff(COLOR_PAIR(5));
        int vx = (int)lbl.size();
        attron(COLOR_PAIR(4));
        for (int i = 0; i < fv; i++) mvaddch(3, vx+i, ACS_BLOCK);
        attroff(COLOR_PAIR(4));
        attron(COLOR_PAIR(3));
        for (int i = fv; i < vbw; i++) mvaddch(3, vx+i, '.');
        attroff(COLOR_PAIR(3));
        attron(COLOR_PAIR(5));
        mvaddch(3, vx+vbw, ']');
        mvaddstr(3, vx+vbw+1, pct);
        attroff(COLOR_PAIR(5));
    }

    // Rows 4..rows-2: chapters OR spectrum
    int content_start = 4;
    int content_end   = rows - 2;

    if (!G.chapters.empty()) {
        if (rows > 9) {
            attron(COLOR_PAIR(5));
            mvaddstr(content_start, 0, " Chapters:");
            attroff(COLOR_PAIR(5));
            int list_row = content_start + 1;
            int max_vis  = content_end - list_row;
            int start_ch = 0;
            if (G.current_chapter >= 0 && G.current_chapter >= max_vis)
                start_ch = G.current_chapter - max_vis + 1;
            for (int i = start_ch;
                 i < (int)G.chapters.size() && list_row < content_end;
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
    } else {
        if (content_end > content_start)
            draw_viz(content_start, content_end, cols);
    }

    // Bottom hint row
    std::string hints;
    if (!G.playlist.empty() && G.playlist.size() > 1) {
        hints = G.chapters.empty()
            ? " [<-/->] seek  [^/v] vol  [space] pause  [n] next  [q] quit"
            : " [<-/->] seek  [^/v] vol  [space] pause  [s/w] ch  [n] next  [q] quit";
    } else {
        hints = G.chapters.empty()
            ? " [<-/->] seek  [^/v] vol  [space] pause  [q] quit"
            : " [<-/->] seek  [^/v] vol  [space] pause  [s/w] chapter  [q] quit";
    }
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
        // Advance playlist on natural end, or quit if last/single file
        if (G.position >= G.duration - 2.0) {
            if (!G.playlist.empty() && G.playlist_idx + 1 < (int)G.playlist.size())
                playlist_next();
            else
                G.quit = true;
        }
    }
    if (p > 0 && p == VIZ.ffmpeg_pid) VIZ.ffmpeg_pid = -1;
}

// ─── Main ─────────────────────────────────────────────────────────────────────

static void print_help(const char* prog) {
    printf("tcfplay %s — terminal media player\n\n", TCFPLAY_VERSION);
    printf("Usage:\n");
    printf("  %s [options] <file> [file2 ...]\n\n", prog);
    printf("Options:\n");
    printf("  -help         Show this help\n");
    printf("  -version      Show version\n");
    printf("  -video        Show video window (default: audio only)\n");
    printf("  -playlist     Treat all file arguments as a playlist\n\n");
    printf("Controls:\n");
    printf("  Space         Pause / resume\n");
    printf("  Left / Right  Seek -/+ 10 seconds\n");
    printf("  Up / Down     Volume +/- 5%%\n");
    printf("  s             Next chapter\n");
    printf("  w             Previous chapter\n");
    printf("  n             Next file in playlist\n");
    printf("  q             Quit\n\n");
    printf("Examples:\n");
    printf("  %s song.mp3\n", prog);
    printf("  %s -video movie.mp4\n", prog);
    printf("  %s -playlist a.mp3 b.mp3 c.mp3\n", prog);
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: tcfplay [options] <file> [file2 ...]\n");
        fprintf(stderr, "Try 'tcfplay -help' for more information.\n");
        return 1;
    }

    setlocale(LC_ALL, "");

    // ── Argument parsing ──────────────────────────────────────────────────────
    bool opt_video    = false;
    bool opt_playlist = false;
    std::vector<std::string> files;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "-help" || arg == "--help" || arg == "-h") {
            print_help(argv[0]);
            return 0;
        } else if (arg == "-version" || arg == "--version") {
            printf("tcfplay %s\n", TCFPLAY_VERSION);
            return 0;
        } else if (arg == "-video") {
            opt_video = true;
        } else if (arg == "-playlist") {
            opt_playlist = true;
        } else if (arg[0] == '-') {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            fprintf(stderr, "Try 'tcfplay -help'\n");
            return 1;
        } else {
            files.push_back(arg);
        }
    }

    if (files.empty()) {
        fprintf(stderr, "Error: no input file specified.\n");
        return 1;
    }

    if (system("which ffplay >/dev/null 2>&1") != 0) {
        fprintf(stderr, "Error: ffplay not found. Please install ffmpeg.\n");
        return 1;
    }

    // ── Init state ────────────────────────────────────────────────────────────
    memset(&G,   0, sizeof(G));
    memset(&VIZ, 0, sizeof(VIZ));
    G.ffplay_pid   = -1;
    G.volume       = 100;
    G.playing      = true;
    G.video_mode   = opt_video;
    VIZ.ffmpeg_pid = -1;
    VIZ.pipe_fd    = -1;
    pthread_mutex_init(&G.lock, NULL);
    pthread_mutex_init(&VIZ.mu, NULL);

    // Build playlist
    // With -playlist: all files are queued.
    // Without -playlist: only first file plays (extra files ignored).
    if (opt_playlist) {
        G.playlist = files;
    } else {
        G.playlist.push_back(files[0]);
    }
    G.playlist_idx = 0;

    // Probe first file
    G.filepath = G.playlist[0];
    G.filename = basename_of(G.filepath);
    {
        struct stat st;
        if (stat(G.filepath.c_str(), &st) != 0) {
            fprintf(stderr, "File not found: %s\n", G.filepath.c_str());
            return 1;
        }
    }
    fprintf(stderr, "Probing media...\n");
    if (!probe_media(G.filepath, G.duration, G.chapters)) {
        fprintf(stderr, "Failed to probe media.\n");
        return 1;
    }

    // ── ncurses init ──────────────────────────────────────────────────────────
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
        init_pair(1, COLOR_CYAN,   -1);
        init_pair(2, COLOR_GREEN,  -1);
        init_pair(3, COLOR_WHITE,  -1);
        init_pair(4, COLOR_YELLOW, -1);
        init_pair(5, COLOR_WHITE,  -1);
    }

    signal(SIGWINCH, handle_sigwinch);
    signal(SIGCHLD,  handle_sigchld);

    spawn_ffplay(0.0);
    start_viz(0.0);

    // ── Main loop ─────────────────────────────────────────────────────────────
    while (!G.quit) {
        if (G.ffplay_pid > 0) {
            G.position        = estimated_position();
            G.current_chapter = find_chapter(G.chapters, G.position);
        }
        maybe_apply_volume();
        if (g_resize) { g_resize = false; endwin(); refresh(); clear(); }
        draw_ui();

        int ch = getch();
        if (ch == ERR) continue;
        switch (ch) {
            case 'q': case 'Q': G.quit = true; break;
            case ' ':           do_toggle_pause(); break;
            case KEY_RIGHT:     do_seek(G.position + 10.0); break;
            case KEY_LEFT:      do_seek(G.position - 10.0); break;
            case KEY_UP:        do_volume_change(+5); break;
            case KEY_DOWN:      do_volume_change(-5); break;
            case 'n': case 'N': playlist_next(); break;
            case 's': case 'S':
                if (!G.chapters.empty()) {
                    int nx = G.current_chapter + 1;
                    if (nx < (int)G.chapters.size())
                        do_seek(G.chapters[nx].start_time);
                }
                break;
            case 'w': case 'W':
                if (!G.chapters.empty()) {
                    int pv = G.current_chapter - 1;
                    do_seek(pv >= 0 ? G.chapters[pv].start_time : 0.0);
                }
                break;
        }
    }

    stop_viz();
    kill_ffplay();
    endwin();
    pthread_mutex_destroy(&G.lock);
    pthread_mutex_destroy(&VIZ.mu);
    printf("\ntcfplay: playback ended.\n");
    return 0;
}
