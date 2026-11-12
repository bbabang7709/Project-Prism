#define _GNU_SOURCE
#include "platform/input.h"

#include <string.h>
#include <unistd.h>
#include "platform/clock.h"

static InputHandlers g_h;
static uint8_t g_kdown[256];
static int64_t g_ktap[256], g_now;
static int g_win32;
static unsigned char g_in[4096];
static int g_in_len;
static int64_t g_esc_since;

void input_init(const InputHandlers *h) { g_h = *h; }
void input_set_time(int64_t now) { g_now = now; }
int input_held(int vk) { return g_kdown[vk] || g_ktap[vk] > g_now; }
int input_win32(void) { return g_win32; }

int input_esc_expired(void)
{
    if (!(g_in_len == 1 && g_in[0] == 27 && g_esc_since && g_now - g_esc_since > 60)) return 0;
    g_in_len = 0;                                        /* ESC 한 바이트를 썼다 */
    g_esc_since = 0;
    return 1;
}

static void key_event(int vk, int down)
{
    vk &= 255;
    if (down) {
        if (!g_kdown[vk]) g_h.press(vk);
        g_kdown[vk] = 1;
    } else {
        g_kdown[vk] = 0;
    }
}

/* 키를 뗀 순간을 모르는 터미널용: 키 반복으로 누르고 있음을 흉내 */
static void key_tap(int vk)
{
    int was = g_ktap[vk] > g_now, walk = vk == 'W' || vk == 'A' || vk == 'S' || vk == 'D';
    if (!was || !walk) g_h.press(vk);                   /* 걷기만 '누르고 있음', 나머지는 누를 때마다 실행 */
    g_ktap[vk] = g_now + (was ? 90 : 200);
}

static void text(int c) { if (g_h.text && c >= 32 && c < 127) g_h.text(c); }

static void plain_char(int c)
{
    text(c);                                            /* 글자 입력란이 쓰는 글자 */
    if (c == 3) g_h.quit();
    else if (c == '\r' || c == '\n') key_tap(VK_ENTER);
    else if (c == 127 || c == 8) key_tap(VK_BACK);
    else if (c >= 'a' && c <= 'z') key_tap(c - 32);
    else if (c >= 'A' && c <= 'Z') key_tap(c);
    else if (c == '\t') key_tap(VK_TAB);
    else if (c == '+' || c == '=') key_tap(VK_PLUS);
    else if (c == '-' || c == '_') key_tap(VK_MINUS);
}

static void csi(const unsigned char *p, int n, char fin)
{
    int prm[8] = { 0, 0, 0, 0, 0, 1, 0, 0 }, np = 0, priv = 0, any = 0;
    if (n > 0 && p[0] == '<') { priv = 1; p++; n--; }
    for (int i = 0; i < n && np < 8; i++) {
        if (p[i] == ';') { np++; any = 0; continue; }
        if (p[i] >= '0' && p[i] <= '9') {
            if (!any) prm[np] = 0;
            prm[np] = prm[np] * 10 + (p[i] - '0');
            any = 1;
        }
    }
    if (fin == '_') {                                    /* win32-input-mode: Vk;Sc;Uc;Kd;Cs;Rc */
        g_win32 = 1;
        if (prm[0] == 'C' && prm[3] && (prm[4] & 0x0C)) g_h.quit();
        if (prm[3] && prm[2] >= 32 && prm[2] < 127) text(prm[2]);   /* 키 반복도 글자로 */
        if (prm[0] > 0) key_event(prm[0], prm[3]);
    } else if (priv && fin == 'M') {                     /* 마우스: 왼쪽 클릭 = 들기/놓기, 휠 = 돌리기 */
        if (prm[0] == 0) g_h.click();
        else if (prm[0] == 64) g_h.wheel(-1);
        else if (prm[0] == 65) g_h.wheel(1);
    } else if (fin >= 'A' && fin <= 'D' && !priv) {
        static const int vk[4] = { VK_UP, VK_DOWN, VK_RIGHT, VK_LEFT };
        key_tap(vk[fin - 'A']);
    } else if (fin == 'O' && n == 0) {                   /* 창이 초점을 잃음: 누른 키를 모두 뗀 것으로 */
        memset(g_kdown, 0, sizeof g_kdown);
    }
}

void input_read(void)
{
    ssize_t n = read(STDIN_FILENO, g_in + g_in_len, sizeof g_in - (size_t)g_in_len);
    if (n > 0) g_in_len += (int)n;
    int i = 0;
    while (i < g_in_len) {
        unsigned char c = g_in[i];
        if (c != 27) { plain_char(c); i++; continue; }
        if (i + 1 >= g_in_len) { if (!g_esc_since) g_esc_since = now_ms(); break; }
        g_esc_since = 0;
        if (g_in[i + 1] != '[') { i++; continue; }
        int j = i + 2;
        while (j < g_in_len && !(g_in[j] >= 0x40 && g_in[j] <= 0x7E)) j++;
        if (j >= g_in_len) { if (g_in_len - i > 64) i = g_in_len; break; }
        csi(g_in + i + 2, j - (i + 2), (char)g_in[j]);
        i = j + 1;
    }
    memmove(g_in, g_in + i, (size_t)(g_in_len - i));
    g_in_len -= i;
}
