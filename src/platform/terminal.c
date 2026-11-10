#define _GNU_SOURCE
#include "platform/terminal.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

/* ---------- 터미널 출력 (바뀐 칸만, 넓은 글자 처리) ---------- */

VCell g_back[MAX_H][MAX_W];
static VCell g_front[MAX_H][MAX_W];
int g_cols = 80, g_rows = 24;
static int g_full = 1;
static char g_out[1 << 22];
static char *g_op;
static struct termios g_orig;
volatile sig_atomic_t g_quit, g_resize;

static void write_all(const char *buf, size_t len)
{
    while (len > 0) {
        ssize_t n = write(STDOUT_FILENO, buf, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return;
        }
        buf += n;
        len -= (size_t)n;
    }
}

static inline void put_u8(unsigned v)
{
    if (v >= 100) { *g_op++ = (char)('0' + v / 100); v %= 100; *g_op++ = (char)('0' + v / 10); *g_op++ = (char)('0' + v % 10); }
    else if (v >= 10) { *g_op++ = (char)('0' + v / 10); *g_op++ = (char)('0' + v % 10); }
    else *g_op++ = (char)('0' + v);
}

static inline void sgr_rgb(int bg, uint32_t c)
{
    memcpy(g_op, bg ? "\x1b[48;2;" : "\x1b[38;2;", 7);
    g_op += 7;
    put_u8(c >> 16 & 255); *g_op++ = ';';
    put_u8(c >> 8 & 255); *g_op++ = ';';
    put_u8(c & 255); *g_op++ = 'm';
}

void term_present(void)
{
    uint32_t cf = 0xFFFFFFFF, cb = 0xFFFFFFFF;
    int cr = -1, cc = -1;
    g_op = g_out;
    if (g_full) { memcpy(g_op, "\x1b[0m\x1b[2J", 8); g_op += 8; }
    for (int r = 0; r < g_rows; r++)
        for (int c = 0; c < g_cols; c++) {
            const VCell *b = &g_back[r][c];
            if (b->wide == 2) continue;
            if (!g_full && memcmp(b, &g_front[r][c], sizeof *b) == 0 &&
                (b->wide != 1 || c + 1 >= g_cols || memcmp(&g_back[r][c + 1], &g_front[r][c + 1], sizeof *b) == 0))
                continue;
            if (cr != r || cc != c) g_op += sprintf(g_op, "\x1b[%d;%dH", r + 1, c + 1);
            const char *ch = b->ch;
            if (!strcmp(ch, "\xE2\x96\x80") && b->fg == b->bg) ch = " ";
            if (!(ch[0] == ' ' && ch[1] == 0) && b->fg != cf) { sgr_rgb(0, b->fg); cf = b->fg; }
            if (b->bg != cb) { sgr_rgb(1, b->bg); cb = b->bg; }
            size_t n = strlen(ch);
            memcpy(g_op, ch, n);
            g_op += n;
            cr = r;
            cc = c + (b->wide == 1 ? 2 : 1);
        }
    g_op += sprintf(g_op, "\x1b[0m");
    write_all(g_out, (size_t)(g_op - g_out));
    memcpy(g_front, g_back, sizeof g_front);
    g_full = 0;
}

static int u8dec(const char *s, uint32_t *cp)
{
    const unsigned char *u = (const unsigned char *)s;
    if (u[0] < 0x80) { *cp = u[0]; return 1; }
    if ((u[0] >> 5) == 6 && u[1]) { *cp = (uint32_t)(u[0] & 31) << 6 | (u[1] & 63); return 2; }
    if ((u[0] >> 4) == 14 && u[1] && u[2]) { *cp = (uint32_t)(u[0] & 15) << 12 | (uint32_t)(u[1] & 63) << 6 | (u[2] & 63); return 3; }
    *cp = '?';
    return 1;
}

static int cp_wide(uint32_t cp) { return (cp >= 0x1100 && cp <= 0x115F) || (cp >= 0x2E80 && cp <= 0xA4CF) || (cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0xFF00 && cp <= 0xFF60); }

int term_text(int x, int y, uint32_t fg, uint32_t bg, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    int x0 = x;
    for (const char *s = buf; *s;) {
        uint32_t cp;
        int l = u8dec(s, &cp), w = cp_wide(cp) ? 2 : 1;
        if (y >= 0 && y < g_rows && x >= 0 && x + w <= g_cols) {
            VCell *v = &g_back[y][x];
            memset(v->ch, 0, sizeof v->ch);
            memcpy(v->ch, s, (size_t)l);
            v->fg = fg;
            v->bg = bg;
            v->wide = w == 2;
            if (w == 2) g_back[y][x + 1] = (VCell){ fg, bg, "", 2 };
        }
        x += w;
        s += l;
    }
    return x - x0;
}

void term_bar(int y, uint32_t bg)
{
    for (int x = 0; x < g_cols; x++) g_back[y][x] = (VCell){ 0xFFFFFF, bg, " ", 0 };
}

int term_str_w(const char *s)
{
    int w = 0;
    while (*s) { uint32_t cp; s += u8dec(s, &cp); w += cp_wide(cp) ? 2 : 1; }
    return w;
}

/* ---------- 터미널 준비 ---------- */

void term_restore(void)
{
    const char *s = "\x1b[?9001l\x1b[?1004l\x1b[?1000l\x1b[?1006l\x1b[0m\x1b[?25h\x1b[?1049l";
    write_all(s, strlen(s));
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_orig);
}

void term_setup(void)
{
    struct termios raw;
    tcgetattr(STDIN_FILENO, &g_orig);
    raw = g_orig;
    raw.c_lflag &= ~(tcflag_t)(ICANON | ECHO | ISIG | IEXTEN);
    raw.c_iflag &= ~(tcflag_t)(IXON | ICRNL);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    const char *s = "\x1b[?1049h\x1b[?25l\x1b[2J\x1b[?9001h\x1b[?1004h\x1b[?1000h\x1b[?1006h";
    write_all(s, strlen(s));
}

static void on_signal(int sig)
{
    if (sig == SIGWINCH) g_resize = 1;
    else g_quit = 1;
}

void term_install_signals(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    sigaction(SIGWINCH, &sa, NULL);
}

void term_get_size(void)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
        g_cols = ws.ws_col;
        g_rows = ws.ws_row;
    }
    if (g_cols > MAX_W) g_cols = MAX_W;
    if (g_rows > MAX_H) g_rows = MAX_H;
    g_full = 1;
}
