#define _GNU_SOURCE
#include "net/net.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "game/game.h"
#include "platform/clock.h"
#include "world/world.h"

enum { M_HELLO = 1, M_WELCOME, M_LEVEL, M_STATE, M_INPUT, M_ACTION, M_BYE };
#define MAGIC 0x5052534Du                 /* 'PRSM' */
#define VERSION 1
#define MAXMSG 2048

typedef struct {
    int fd;
    unsigned char in[8192];               /* 아직 처리하지 않은 받은 바이트 */
    int len;
    int ready;                            /* HELLO/WELCOME을 주고받았다 */
} Conn;

static int s_mode = NET_OFF;
static int s_listen = -1, s_port;
static Conn s_peer = { .fd = -1 };
static int s_sent_level = -1;             /* 호스트: 마지막으로 보낸 방 */
static int s_last_seq = -1;               /* 참가자: 마지막으로 받은 알림 번호 */
static int s_got_level, s_got_state;      /* 참가자: 처음 상태를 다 받았는가 */
static char s_addr[96];                   /* 보여 줄 주소 */
static char s_status[160], s_lost[160];

int net_mode(void) { return s_mode; }
const char *net_lost(void) { return s_lost[0] ? s_lost : NULL; }

/* ---------- 메시지 만들기와 읽기 ---------- */

typedef struct { unsigned char b[MAXMSG]; int n; } Buf;
typedef struct { const unsigned char *p; int n, i, bad; } Rd;

static void put8(Buf *w, unsigned v) { if (w->n < MAXMSG) w->b[w->n++] = (unsigned char)v; }
static void put16(Buf *w, unsigned v) { put8(w, v >> 8); put8(w, v); }
static void put32(Buf *w, uint32_t v) { put16(w, v >> 16); put16(w, v & 0xFFFF); }
static void putf(Buf *w, float f) { uint32_t u; memcpy(&u, &f, 4); put32(w, u); }
static void putbytes(Buf *w, const void *p, int n) { for (int i = 0; i < n; i++) put8(w, ((const unsigned char *)p)[i]); }

static unsigned get8(Rd *r) { if (r->i >= r->n) { r->bad = 1; return 0; } return r->p[r->i++]; }
static unsigned get16(Rd *r) { unsigned h = get8(r); return h << 8 | get8(r); }
static uint32_t get32(Rd *r) { uint32_t h = get16(r); return h << 16 | get16(r); }
static float getf(Rd *r) { uint32_t u = get32(r); float f; memcpy(&f, &u, 4); return f; }
static void getbytes(Rd *r, void *p, int n) { for (int i = 0; i < n; i++) ((unsigned char *)p)[i] = (unsigned char)get8(r); }

/* 한 메시지를 다 보낸다. 보내기 버퍼가 차 있으면 잠깐(최대 200ms) 기다린다 */
static int send_msg(int fd, int type, const Buf *w)
{
    unsigned char out[3 + MAXMSG];
    out[0] = (unsigned char)type;
    out[1] = (unsigned char)(w->n >> 8);
    out[2] = (unsigned char)w->n;
    memcpy(out + 3, w->b, (size_t)w->n);
    size_t len = 3 + (size_t)w->n, off = 0;
    while (off < len) {
        ssize_t k = send(fd, out + off, len - off, MSG_NOSIGNAL);
        if (k > 0) { off += (size_t)k; continue; }
        if (k < 0 && errno == EINTR) continue;
        if (k < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd p = { fd, POLLOUT, 0 };
            if (poll(&p, 1, 200) > 0) continue;
        }
        return -1;
    }
    return 0;
}

static void send_bye(int fd, const char *why)
{
    Buf w = { .n = 0 };
    putbytes(&w, why, (int)strlen(why));
    send_msg(fd, M_BYE, &w);
}

static void set_nonblock(int fd)
{
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);   /* 작은 메시지를 바로 보낸다 */
}

/* ---------- 호스트 → 참가자 ---------- */

static int send_level(int fd)
{
    const Level *lv = &g_levels[g_cur_level];
    Buf w = { .n = 0 };
    put8(&w, (unsigned)g_cur_level);
    put8(&w, (unsigned)g_nlevels);
    putbytes(&w, lv->name, sizeof lv->name);
    for (int z = 0; z < RD; z++) putbytes(&w, lv->map[z], RW);
    for (int z = 0; z < RD; z++) putbytes(&w, lv->rot[z], RW);
    put8(&w, (unsigned)lv->sx);
    put8(&w, (unsigned)lv->sz);
    put8(&w, (unsigned char)lv->face);
    put8(&w, (unsigned)lv->coop);
    return send_msg(fd, M_LEVEL, &w);
}

static int send_state(int fd, int to)
{
    Buf w = { .n = 0 };
    put8(&w, (unsigned)g_cur_level);
    put8(&w, (unsigned)g_solved);
    put8(&w, (unsigned)g_cleared);
    put16(&w, (unsigned)g_moves);
    for (int i = 0; i < MAXPLAYER; i++) {
        const Player *p = &g_pl[i];
        put8(&w, (unsigned)p->on);
        putf(&w, p->x); putf(&w, p->z); putf(&w, p->yaw); putf(&w, p->pitch);
        put8(&w, (unsigned char)(signed char)p->carry);
        put8(&w, (unsigned char)(signed char)p->tobj);
        put8(&w, (unsigned char)(signed char)p->tx);
        put8(&w, (unsigned char)(signed char)p->tz);
    }
    const Player *p = &g_pl[to];
    int ml = (int)strlen(p->msg);
    put16(&w, (unsigned)p->msg_seq & 0xFFFF);
    put8(&w, (unsigned)ml);
    putbytes(&w, p->msg, ml);
    put8(&w, (unsigned)g_nobj);
    for (int i = 0; i < g_nobj; i++) {
        put8(&w, (unsigned)g_obj[i].x); put8(&w, (unsigned)g_obj[i].z);
        put8(&w, (unsigned)g_obj[i].rot); put8(&w, (unsigned)g_obj[i].held);
    }
    return send_msg(fd, M_STATE, &w);
}

/* ---------- 참가자가 받은 것 적용 ---------- */

static void apply_level(Rd *r)
{
    int idx = (int)get8(r), n = (int)get8(r);
    if (idx >= MAXLEVEL || n > MAXLEVEL || idx >= n) { r->bad = 1; return; }
    Level *lv = &g_levels[idx];
    memset(lv, 0, sizeof *lv);
    getbytes(r, lv->name, sizeof lv->name);
    lv->name[sizeof lv->name - 1] = 0;
    for (int z = 0; z < RD; z++) { getbytes(r, lv->map[z], RW); lv->map[z][RW] = 0; }
    for (int z = 0; z < RD; z++) getbytes(r, lv->rot[z], RW);
    lv->sx = (int)get8(r);
    lv->sz = (int)get8(r);
    lv->face = (char)get8(r);
    lv->coop = (int)get8(r);
    if (r->bad) return;
    g_nlevels = n;
    g_cur_level = idx;
    load_level();
    s_got_level = 1;
}

static void apply_state(Rd *r)
{
    int level = (int)get8(r);
    int solved = (int)get8(r);
    g_cleared = (int)get8(r);
    g_moves = (int)get16(r);
    (void)solved;                                       /* 빛은 아래에서 직접 다시 계산한다 */
    for (int i = 0; i < MAXPLAYER; i++) {
        Player *p = &g_pl[i];
        p->on = (int)get8(r);
        p->x = getf(r); p->z = getf(r); p->yaw = getf(r); p->pitch = getf(r);
        p->carry = (signed char)get8(r);
        p->tobj = (signed char)get8(r);
        p->tx = (signed char)get8(r);
        p->tz = (signed char)get8(r);
    }
    int seq = (int)get16(r), ml = (int)get8(r);
    char msg[256];
    getbytes(r, msg, ml);
    msg[ml] = 0;
    int nobj = (int)get8(r);
    if (r->bad || level != g_cur_level || nobj != g_nobj) { r->bad = 1; return; }
    memset(g_objat, 0, sizeof g_objat);
    for (int i = 0; i < nobj; i++) {
        Obj *o = &g_obj[i];
        o->x = (int)get8(r); o->z = (int)get8(r); o->rot = (int)get8(r); o->held = (int)get8(r);
        if (o->x >= RW || o->z >= RD) { r->bad = 1; return; }
        if (!o->held) g_objat[o->z][o->x] = i + 1;
    }
    Player *me = &g_pl[g_me];
    if (seq != s_last_seq) {                            /* 새 알림 */
        snprintf(me->msg, sizeof me->msg, "%.159s", msg);
        me->msg_t = 90;
        s_last_seq = seq;
    }
    game_simulate();                                    /* 받은 물체 상태로 빛줄기와 수정을 다시 계산 */
    s_got_state = 1;
}

/* ---------- 받은 메시지 처리 ---------- */

static void drop_peer(const char *why)
{
    if (s_peer.fd >= 0) close(s_peer.fd);
    int was_ready = s_peer.ready;
    s_peer = (Conn){ .fd = -1 };
    if (s_mode == NET_HOST) {
        if (was_ready) game_leave(1);
    } else if (!s_lost[0]) {
        snprintf(s_lost, sizeof s_lost, "%s", why);
    }
}

static void dispatch(int type, Rd *r)
{
    if (s_mode == NET_HOST) {
        if (type == M_HELLO) {
            uint32_t magic = get32(r);
            unsigned ver = get16(r);
            if (r->bad || magic != MAGIC || ver != VERSION) {
                send_bye(s_peer.fd, "PRISM 버전이 다릅니다");
                drop_peer("버전 다름");
                return;
            }
            s_peer.ready = 1;
            game_join(1);
            Buf w = { .n = 0 };
            put8(&w, 1);
            send_msg(s_peer.fd, M_WELCOME, &w);
            if (send_level(s_peer.fd) < 0) { drop_peer(""); return; }
            s_sent_level = g_cur_level;
            send_state(s_peer.fd, 1);
        } else if (!s_peer.ready) {
            r->bad = 1;
        } else if (type == M_INPUT) {
            unsigned keys = get16(r), fp = get8(r);
            float walk = getf(r);
            if (r->bad) return;
            g_pl[1].keys = keys;
            g_pl[1].fp = fp != 0;
            g_pl[1].walk_yaw = walk;
        } else if (type == M_ACTION) {
            int act = (int)get8(r);
            if (!r->bad) game_action(1, act);
        }
    } else {
        if (type == M_WELCOME) {
            int idx = (int)get8(r);
            if (r->bad || idx < 0 || idx >= MAXPLAYER) { r->bad = 1; return; }
            g_me = idx;
            g_pl[idx].fp = 1;
            s_peer.ready = 1;
        } else if (type == M_LEVEL) apply_level(r);
        else if (type == M_STATE) { if (s_got_level) apply_state(r); }
        else if (type == M_BYE) {
            char why[128];
            int n = r->n < (int)sizeof why - 1 ? r->n : (int)sizeof why - 1;
            getbytes(r, why, n);
            why[n] = 0;
            snprintf(s_lost, sizeof s_lost, "%s", why);
        }
    }
}

/* 읽을 수 있는 만큼 읽고, 다 온 메시지를 처리한다.
 * 상대가 닫았어도 먼저 받아 둔 메시지(예: BYE의 이유)를 처리한 뒤에 끊는다 */
static void read_peer(void)
{
    int closed = 0;
    for (;;) {
        ssize_t k = recv(s_peer.fd, s_peer.in + s_peer.len, sizeof s_peer.in - (size_t)s_peer.len, 0);
        if (k > 0) { s_peer.len += (int)k; if (s_peer.len < (int)sizeof s_peer.in) continue; }
        else if (k == 0) closed = 1;
        else if (errno == EINTR) continue;
        else if (errno != EAGAIN && errno != EWOULDBLOCK) closed = 1;
        break;
    }
    int i = 0;
    while (s_peer.len - i >= 3) {
        int type = s_peer.in[i], plen = s_peer.in[i + 1] << 8 | s_peer.in[i + 2];
        if (plen > MAXMSG) { drop_peer("잘못된 메시지를 받았어요"); return; }
        if (s_peer.len - i < 3 + plen) break;
        Rd r = { s_peer.in + i + 3, plen, 0, 0 };
        dispatch(type, &r);
        if (s_peer.fd < 0) return;                      /* 처리 중에 끊었다 */
        if (r.bad) { drop_peer("잘못된 메시지를 받았어요"); return; }
        i += 3 + plen;
    }
    memmove(s_peer.in, s_peer.in + i, (size_t)(s_peer.len - i));
    s_peer.len -= i;
    if (closed) drop_peer(s_mode == NET_HOST ? "" : "호스트와 연결이 끊어졌어요");
}

/* ---------- 호스트 ---------- */

/* 다른 컴퓨터가 접속할 수 있는 IPv4 주소 하나 (없으면 127.0.0.1) */
static void local_ip(char *out, size_t n)
{
    snprintf(out, n, "127.0.0.1");
    struct ifaddrs *list;
    if (getifaddrs(&list) < 0) return;
    for (struct ifaddrs *a = list; a; a = a->ifa_next) {
        if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET) continue;
        const struct sockaddr_in *sin = (const struct sockaddr_in *)a->ifa_addr;
        if ((ntohl(sin->sin_addr.s_addr) >> 24) == 127) continue;
        inet_ntop(AF_INET, &sin->sin_addr, out, (socklen_t)n);
        break;
    }
    freeifaddrs(list);
}

int net_host(int port, char *err, size_t errlen)
{
    s_listen = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (s_listen < 0) { snprintf(err, errlen, "socket: %s", strerror(errno)); return -1; }
    int one = 1;
    setsockopt(s_listen, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port), .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (bind(s_listen, (struct sockaddr *)&a, sizeof a) < 0 || listen(s_listen, 2) < 0) {
        snprintf(err, errlen, "포트 %d를 열 수 없습니다: %s", port, strerror(errno));
        close(s_listen);
        s_listen = -1;
        return -1;
    }
    fcntl(s_listen, F_SETFL, fcntl(s_listen, F_GETFL) | O_NONBLOCK);
    s_port = port;
    local_ip(s_addr, sizeof s_addr);
    s_mode = NET_HOST;
    return 0;
}

static void accept_peer(void)
{
    int fd = accept4(s_listen, NULL, NULL, SOCK_CLOEXEC);
    if (fd < 0) return;
    set_nonblock(fd);
    if (s_peer.fd >= 0) {                               /* 이미 둘이다 */
        send_bye(fd, "이미 2명이 플레이 중이에요");
        close(fd);
        return;
    }
    s_peer = (Conn){ .fd = fd };
}

/* ---------- 참가자 ---------- */

int net_join(const char *addr, char *err, size_t errlen)
{
    char host[128];
    int port = NET_PORT;
    snprintf(host, sizeof host, "%s", addr);
    char *colon = strrchr(host, ':');
    if (colon && strchr(host, ':') == colon) { *colon = 0; port = atoi(colon + 1); }
    char ps[16];
    snprintf(ps, sizeof ps, "%d", port);
    struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM }, *res;
    int gai = getaddrinfo(host, ps, &hints, &res);
    if (gai) { snprintf(err, errlen, "%s: 주소를 찾을 수 없습니다 (%s)", host, gai_strerror(gai)); return -1; }
    int fd = -1;
    for (struct addrinfo *a = res; a && fd < 0; a = a->ai_next) {
        fd = socket(a->ai_family, a->ai_socktype | SOCK_CLOEXEC, a->ai_protocol);
        if (fd < 0) continue;
        set_nonblock(fd);
        if (connect(fd, a->ai_addr, a->ai_addrlen) < 0 && errno != EINPROGRESS) { close(fd); fd = -1; continue; }
        struct pollfd p = { fd, POLLOUT, 0 };             /* 접속을 최대 5초 기다린다 */
        int soerr = 0;
        socklen_t sl = sizeof soerr;
        if (poll(&p, 1, 5000) <= 0 || getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl) < 0 || soerr) {
            errno = soerr ? soerr : ETIMEDOUT;
            close(fd);
            fd = -1;
        }
    }
    freeaddrinfo(res);
    if (fd < 0) { snprintf(err, errlen, "%s:%d에 접속할 수 없습니다 (%s)", host, port, strerror(errno)); return -1; }
    s_mode = NET_CLIENT;
    s_peer = (Conn){ .fd = fd };
    snprintf(s_addr, sizeof s_addr, "%.80s:%d", host, port);
    Buf w = { .n = 0 };
    put32(&w, MAGIC);
    put16(&w, VERSION);
    if (send_msg(fd, M_HELLO, &w) < 0) { snprintf(err, errlen, "접속이 끊어졌습니다"); return -1; }
    int64_t until = now_ms() + 5000;                    /* WELCOME, LEVEL, 첫 STATE를 기다린다 */
    while (!(s_peer.ready && s_got_level && s_got_state)) {
        int64_t left = until - now_ms();
        struct pollfd p = { s_peer.fd, POLLIN, 0 };
        if (left <= 0 || poll(&p, 1, (int)left) <= 0) { snprintf(err, errlen, "호스트가 응답하지 않습니다"); return -1; }
        read_peer();
        if (s_lost[0] || s_peer.fd < 0) { snprintf(err, errlen, "%s", s_lost[0] ? s_lost : "접속이 끊어졌습니다"); return -1; }
    }
    return 0;
}

/* ---------- 루프에서 ---------- */

int net_pollfds(struct pollfd *pf, int max)
{
    int n = 0;
    if (s_listen >= 0 && n < max) pf[n++] = (struct pollfd){ s_listen, POLLIN, 0 };
    if (s_peer.fd >= 0 && n < max) pf[n++] = (struct pollfd){ s_peer.fd, POLLIN, 0 };
    return n;
}

void net_handle(const struct pollfd *pf, int n)
{
    for (int i = 0; i < n; i++) {
        if (!pf[i].revents) continue;
        if (pf[i].fd == s_listen) accept_peer();
        else if (pf[i].fd == s_peer.fd) read_peer();
    }
}

void net_after_tick(void)
{
    if (s_peer.fd < 0 || !s_peer.ready) return;
    if (s_mode == NET_HOST) {
        if (g_cur_level != s_sent_level) {
            if (send_level(s_peer.fd) < 0) { drop_peer(""); return; }
            s_sent_level = g_cur_level;
        }
        if (send_state(s_peer.fd, 1) < 0) drop_peer("");
    } else {
        Buf w = { .n = 0 };
        put16(&w, game_local_keys());
        put8(&w, (unsigned)g_pl[g_me].fp);
        putf(&w, cam_yaw);
        if (send_msg(s_peer.fd, M_INPUT, &w) < 0) drop_peer("호스트와 연결이 끊어졌어요");
    }
}

void net_send_action(int act)
{
    if (s_mode != NET_CLIENT || s_peer.fd < 0) return;
    Buf w = { .n = 0 };
    put8(&w, (unsigned)act);
    if (send_msg(s_peer.fd, M_ACTION, &w) < 0) drop_peer("호스트와 연결이 끊어졌어요");
}

const char *net_status(void)
{
    if (s_mode == NET_HOST)
        snprintf(s_status, sizeof s_status, "호스트 %s:%d  ·  %s", s_addr, s_port,
                 s_peer.ready ? "2명이 함께 플레이 중" : "2번 플레이어를 기다리는 중");
    else if (s_mode == NET_CLIENT)
        snprintf(s_status, sizeof s_status, "참가 %s  ·  %d번 플레이어", s_addr, g_me + 1);
    else
        return NULL;
    return s_status;
}

void net_close(void)
{
    if (s_peer.fd >= 0) {
        if (s_mode == NET_HOST) send_bye(s_peer.fd, "호스트가 게임을 끝냈어요");
        close(s_peer.fd);
        s_peer.fd = -1;
    }
    if (s_listen >= 0) { close(s_listen); s_listen = -1; }
}
