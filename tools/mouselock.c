/*
 * mouselock.c - optional Windows companion for the game running in WSL.
 *
 * A terminal cannot hide or re-centre the mouse pointer and never reports
 * key releases. This small Win32 program does what a normal game would do
 * and streams the result to the game over TCP (127.0.0.1:47017, forwarded
 * into WSL):
 *   - while the "<game>" terminal window is in the foreground it hides
 *     the pointer, clips it to the window centre and reads raw mouse motion
 *     (WM_INPUT), so the camera follows the mouse like a normal FPS
 *   - it reports the real held/released state of the movement keys
 *   - Alt-Tab / another window / the game's "unlock" message releases it
 *
 * Build (in WSL):  sudo apt install gcc-mingw-w64-x86-64 && make winhelper
 * If the pointer ever stays invisible:  mouselock.exe --restore
 */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <mmsystem.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define PORT       47017
#define TIMER_ID   1
#define TIMER_MS   8

/* key bits sent to the game (must match include/bridge.h) */
enum {
    KB_W = 1 << 0, KB_A = 1 << 1, KB_S = 1 << 2, KB_D = 1 << 3,
    KB_SPACE = 1 << 4, KB_SHIFT = 1 << 5, KB_LEFT = 1 << 6, KB_RIGHT = 1 << 7,
    KB_UP = 1 << 8, KB_DOWN = 1 << 9, KB_F = 1 << 10, KB_LMB = 1 << 11
};

#define KB_ACTIVE 0x80000000u   /* helper has the game window in the foreground */

#pragma pack(push, 1)
typedef struct { int32_t dx, dy; uint32_t keys, seq; } Packet;
#pragma pack(pop)

static char   g_title[128] = "Prism";   /* exact window title of the game; --title <name> */
static HWND   g_target = NULL;
static SOCKET g_sock = INVALID_SOCKET;
static int    g_want_lock = 1;     /* game asked for mouse lock */
static int    g_locked = 0;
static int    g_hidden = 0;
static long   g_ax = 0, g_ay = 0;  /* accumulated raw motion */
static uint32_t g_seq = 0, g_last_keys = 0;
static DWORD  g_last_try = 0;

/* ---------- diagnostics: tools\mouselock.log (state changes only) ---------- */

static char g_logpath[MAX_PATH];

static void dlog(const char *fmt, ...)
{
    if (!g_logpath[0]) {
        GetModuleFileNameA(NULL, g_logpath, MAX_PATH);
        char *dot = strrchr(g_logpath, '.');
        if (dot) strcpy(dot, ".log"); else strcat(g_logpath, ".log");
    }
    FILE *f = fopen(g_logpath, "a");
    if (!f) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(f, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

static BOOL CALLBACK dump_proc(HWND h, LPARAM lp)
{
    char title[128], cls[128];
    (void)lp;
    if (!IsWindowVisible(h)) return TRUE;
    GetWindowTextA(h, title, sizeof(title));
    GetClassNameA(h, cls, sizeof(cls));
    if (title[0] || strstr(cls, "CASCADIA") || strstr(cls, "Console"))
        dlog("  window hwnd=%p class=\"%s\" title=\"%s\"", (void *)h, cls, title);
    return TRUE;
}

/* ---------- pointer hiding (system cursor swap, always restored) ---------- */

static void show_pointer(int show)
{
    if (!show && !g_hidden) {
        static const DWORD ids[] = { 32512, 32513, 32649, 32515, 32514, 32650 };
        BYTE andm[128], xorm[128];
        memset(andm, 0xFF, sizeof(andm));
        memset(xorm, 0x00, sizeof(xorm));
        for (unsigned i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
            HCURSOR c = CreateCursor(GetModuleHandle(NULL), 0, 0, 32, 32, andm, xorm);
            if (c) SetSystemCursor(c, ids[i]);     /* takes ownership of c */
        }
        g_hidden = 1;
    } else if (show && g_hidden) {
        SystemParametersInfoW(SPI_SETCURSORS, 0, NULL, 0);
        g_hidden = 0;
    }
}

static void release_all(void)
{
    ClipCursor(NULL);
    show_pointer(1);
    g_locked = 0;
}

/* ---------- target window ---------- */

static BOOL CALLBACK find_proc(HWND h, LPARAM lp)
{
    char title[256], cls[128];
    if (!IsWindowVisible(h)) return TRUE;
    GetWindowTextA(h, title, sizeof(title));
    GetClassNameA(h, cls, sizeof(cls));
    /* exact title only: a shell tab whose path contains "<game>" must not match */
    if (!strcmp(title, g_title) &&
        (!strcmp(cls, "CASCADIA_HOSTING_WINDOW_CLASS") || !strcmp(cls, "ConsoleWindowClass"))) {
        *(HWND *)lp = h;
        return FALSE;
    }
    return TRUE;
}

static int target_is_game(HWND h)
{
    char title[256];
    if (!h || !IsWindow(h)) return 0;
    GetWindowTextA(h, title, sizeof(title));
    return strcmp(title, g_title) == 0;
}

static HWND find_target(void)
{
    HWND h = NULL;
    EnumWindows(find_proc, (LPARAM)&h);
    return h;
}

/* ---------- socket ---------- */

static void try_connect(void)
{
    if (g_sock != INVALID_SOCKET || GetTickCount() - g_last_try < 500) return;
    g_last_try = GetTickCount();
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return;
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(PORT);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(s, (struct sockaddr *)&a, sizeof(a)) != 0) {
        static int fails = 0;
        if (fails++ % 40 == 0) dlog("connect to game failed (WSA error %d)", WSAGetLastError());
        closesocket(s);
        return;
    }
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));
    g_sock = s;
    dlog("connected to game 127.0.0.1:%d", PORT);
}

static void drop_socket(void)
{
    dlog("socket dropped");
    if (g_sock != INVALID_SOCKET) closesocket(g_sock);
    g_sock = INVALID_SOCKET;
    release_all();
}

/* control bytes from the game: 'L' lock, 'U' unlock, 'Q' quit */
static int poll_control(void)
{
    char buf[64];
    for (;;) {
        int n = recv(g_sock, buf, sizeof(buf), 0);
        if (n == 0) { drop_socket(); return 0; }
        if (n < 0) {
            if (WSAGetLastError() != WSAEWOULDBLOCK) drop_socket();
            return 0;
        }
        for (int i = 0; i < n; i++) {
            dlog("control byte '%c'", buf[i]);
            if (buf[i] == 'L') g_want_lock = 1;
            else if (buf[i] == 'U') g_want_lock = 0;
            else if (buf[i] == 'Q') return 1;
        }
    }
}

static uint32_t read_keys(void)
{
    static const struct { int vk; uint32_t bit; } map[] = {
        { 'W', KB_W }, { 'A', KB_A }, { 'S', KB_S }, { 'D', KB_D }, { VK_SPACE, KB_SPACE },
        { VK_SHIFT, KB_SHIFT }, { VK_LEFT, KB_LEFT }, { VK_RIGHT, KB_RIGHT }, { VK_UP, KB_UP },
        { VK_DOWN, KB_DOWN }, { 'F', KB_F }, { VK_LBUTTON, KB_LMB },
    };
    uint32_t k = 0;
    for (unsigned i = 0; i < sizeof(map) / sizeof(map[0]); i++)
        if (GetAsyncKeyState(map[i].vk) & 0x8000) k |= map[i].bit;
    return k;
}

/* ---------- main loop pieces ---------- */

static void update_lock(void)
{
    int fg = g_target && GetForegroundWindow() == g_target && !IsIconic(g_target);
    int lock = fg && g_want_lock && g_sock != INVALID_SOCKET;
    static int last_state = -1;
    int state = (fg ? 1 : 0) | (g_want_lock ? 2 : 0) | (g_sock != INVALID_SOCKET ? 4 : 0);
    if (state != last_state) {
        last_state = state;
        dlog("state: foreground=%d want_lock=%d connected=%d -> lock=%d (fg window=%p target=%p)",
             fg ? 1 : 0, g_want_lock, g_sock != INVALID_SOCKET, lock, (void *)GetForegroundWindow(), (void *)g_target);
    }
    if (lock) {
        RECT r;
        GetWindowRect(g_target, &r);
        int cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
        RECT c = { cx, cy, cx + 1, cy + 1 };
        ClipCursor(&c);                 /* re-applied: Windows resets it on focus changes */
        if (!g_locked) SetCursorPos(cx, cy);
        show_pointer(0);
        g_locked = 1;
    } else if (g_locked || g_hidden) {
        release_all();
        g_ax = g_ay = 0;
    }
}

static void send_state(void)
{
    if (g_sock == INVALID_SOCKET) return;
    int fg = g_target && GetForegroundWindow() == g_target;
    uint32_t keys = fg ? (read_keys() | KB_ACTIVE) : 0;
    if (!g_locked) { g_ax = g_ay = 0; }
    if (g_ax == 0 && g_ay == 0 && keys == g_last_keys && (g_seq & 7)) { g_seq++; return; }
    Packet p = { (int32_t)g_ax, (int32_t)g_ay, keys, g_seq++ };
    int n = send(g_sock, (const char *)&p, sizeof(p), 0);
    if (n == (int)sizeof(p)) { g_ax = g_ay = 0; g_last_keys = keys; }
    else if (n < 0 && WSAGetLastError() != WSAEWOULDBLOCK) drop_socket();
}

static LRESULT CALLBACK wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_INPUT: {
        RAWINPUT ri;
        UINT sz = sizeof(ri);
        if (GetRawInputData((HRAWINPUT)lp, RID_INPUT, &ri, &sz, sizeof(RAWINPUTHEADER)) != (UINT)-1 &&
            ri.header.dwType == RIM_TYPEMOUSE && !(ri.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) && g_locked) {
            g_ax += ri.data.mouse.lLastX;
            g_ay += ri.data.mouse.lLastY;
        }
        break;
    }
    case WM_TIMER:
        static DWORD last_check = 0;
        if (g_target && GetTickCount() - last_check > 500) {
            last_check = GetTickCount();
            if (!target_is_game(g_target)) {
                dlog("target window no longer matches, searching again");
                g_target = NULL;
            }
        }
        if (!g_target || !IsWindow(g_target)) {
            g_target = find_target();
            if (g_target) {
                char t[128], c[128];
                GetWindowTextA(g_target, t, sizeof(t));
                GetClassNameA(g_target, c, sizeof(c));
                dlog("target window found hwnd=%p class=\"%s\" title=\"%s\"", (void *)g_target, c, t);
            } else {
                static DWORD last_dump = 0;
                if (GetTickCount() - last_dump > 5000) {
                    last_dump = GetTickCount();
                    dlog("no window titled \"%s\" (class CASCADIA_HOSTING_WINDOW_CLASS / ConsoleWindowClass); visible windows:", g_title);
                    EnumWindows(dump_proc, 0);
                }
            }
            static DWORD start = 0;
            if (!start) start = GetTickCount();
            if (!g_target && GetTickCount() - start > 30000) { dlog("no game window after 30s, exiting"); PostQuitMessage(0); }   /* game never appeared */
        }
        try_connect();
        {
            static int was_connected = 0;
            static DWORD lost_at = 0;
            if (g_sock != INVALID_SOCKET) { was_connected = 1; lost_at = 0; }
            else if (was_connected) {
                if (!lost_at) lost_at = GetTickCount();
                if (GetTickCount() - lost_at > 5000) { dlog("game gone for 5s, exiting"); PostQuitMessage(0); break; }
            }
        }
        if (g_sock != INVALID_SOCKET && poll_control()) { PostQuitMessage(0); break; }
        update_lock();
        send_state();
        break;
    case WM_ENDSESSION:
    case WM_DESTROY:
        release_all();
        PostQuitMessage(0);
        break;
    }
    return DefWindowProcA(h, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    (void)prev; (void)show;
    if (cmd && strstr(cmd, "--restore")) {          /* emergency: bring the pointer back */
        ClipCursor(NULL);
        SystemParametersInfoW(SPI_SETCURSORS, 0, NULL, 0);
        return 0;
    }
    /* one instance only */
    dlog("mouselock started");
    if (cmd) {                                       /* --title <name> or --title "<name>" */
        const char *t = strstr(cmd, "--title");
        if (t) {
            t += 7;
            while (*t == ' ') t++;
            int q = (*t == '"');
            if (q) t++;
            size_t n = 0;
            while (t[n] && t[n] != (q ? '"' : ' ') && n < sizeof(g_title) - 1) { g_title[n] = t[n]; n++; }
            g_title[n] = 0;
        }
    }
    char objname[160];
    snprintf(objname, sizeof(objname), "MouseLockHelper_%s", g_title);
    dlog("title=\"%s\"", g_title);
    HANDLE mtx = CreateMutexA(NULL, TRUE, objname);
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 1;

    WNDCLASSA wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.lpszClassName = objname;
    RegisterClassA(&wc);
    HWND h = CreateWindowA(wc.lpszClassName, "", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, inst, NULL);

    RAWINPUTDEVICE rid = { 0x01, 0x02, RIDEV_INPUTSINK, h };   /* generic desktop / mouse */
    RegisterRawInputDevices(&rid, 1, sizeof(rid));
    SetTimer(h, TIMER_ID, TIMER_MS, NULL);
    timeBeginPeriod(1);

    MSG m;
    while (GetMessageA(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageA(&m);
    }

    release_all();
    if (g_sock != INVALID_SOCKET) closesocket(g_sock);
    WSACleanup();
    timeEndPeriod(1);
    if (mtx) CloseHandle(mtx);
    return 0;
}
