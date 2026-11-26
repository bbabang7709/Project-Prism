/*
 * bridge.c - TCP server thread for the Windows mouse/keyboard helper.
 *
 * socket() / setsockopt() / bind() / listen() / accept() / recv() / send():
 * one client at a time. Motion is accumulated under a mutex and consumed by
 * the game loop (every 30 fps tick); the held-key mask replaces the terminal's guesswork.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "bridge.h"

static pthread_t       g_thr;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static int             g_listen = -1;
static volatile int    g_client = -1;
static volatile int    g_quit = 0;
static int             g_running = 0;
static long            g_dx = 0, g_dy = 0;
static uint32_t        g_keys = 0;
static double          g_last_packet = 0;

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void serve_client(int fd)
{
    BridgePacket p;
    size_t have = 0;
    char *buf = (char *)&p;

    while (!g_quit) {
        struct pollfd pf = { fd, POLLIN, 0 };
        int r = poll(&pf, 1, 200);
        if (r < 0 && errno != EINTR) break;
        if (r <= 0) continue;
        ssize_t n = recv(fd, buf + have, sizeof(p) - have, 0);
        if (n <= 0) break;                         /* helper closed */
        have += (size_t)n;
        if (have < sizeof(p)) continue;
        have = 0;
        pthread_mutex_lock(&g_mu);
        g_dx += p.dx;
        g_dy += p.dy;
        g_keys = p.keys;
        g_last_packet = now_s();
        pthread_mutex_unlock(&g_mu);
    }
}

static void *listen_main(void *arg)
{
    (void)arg;
    while (!g_quit) {
        struct pollfd pf = { g_listen, POLLIN, 0 };
        if (poll(&pf, 1, 250) <= 0) continue;
        int fd = accept(g_listen, NULL, NULL);
        if (fd < 0) continue;
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        pthread_mutex_lock(&g_mu);
        g_client = fd;
        pthread_mutex_unlock(&g_mu);
        bridge_send('L');
        serve_client(fd);
        pthread_mutex_lock(&g_mu);
        g_client = -1;
        g_keys = 0;
        g_dx = g_dy = 0;
        pthread_mutex_unlock(&g_mu);
        close(fd);
    }
    return NULL;
}

int bridge_start(void)
{
    g_listen = socket(AF_INET, SOCK_STREAM, 0);
    if (g_listen < 0) return -1;
    int one = 1;
    setsockopt(g_listen, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(BRIDGE_PORT);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);     /* never exposed to the network */
    if (bind(g_listen, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(g_listen, 1) < 0) {
        close(g_listen);
        g_listen = -1;
        return -1;
    }
    g_quit = 0;
    if (pthread_create(&g_thr, NULL, listen_main, NULL) != 0) {
        close(g_listen);
        g_listen = -1;
        return -1;
    }
    g_running = 1;
    return 0;
}

void bridge_stop(void)
{
    if (!g_running) return;
    bridge_send('Q');
    g_quit = 1;
    pthread_join(g_thr, NULL);
    close(g_listen);
    g_listen = -1;
    g_running = 0;
}

int bridge_connected(void)
{
    pthread_mutex_lock(&g_mu);
    int ok = g_client >= 0 && now_s() - g_last_packet < 0.5 && (g_keys & KB_ACTIVE);
    pthread_mutex_unlock(&g_mu);
    return ok;
}

void bridge_take_motion(int *dx, int *dy)
{
    pthread_mutex_lock(&g_mu);
    *dx = (int)g_dx;
    *dy = (int)g_dy;
    g_dx = g_dy = 0;
    pthread_mutex_unlock(&g_mu);
}

uint32_t bridge_keys(void)
{
    pthread_mutex_lock(&g_mu);
    uint32_t k = (g_client >= 0 && now_s() - g_last_packet < 0.5 && (g_keys & KB_ACTIVE)) ? g_keys : 0;
    pthread_mutex_unlock(&g_mu);
    return k;
}

/* Sent from the game thread while the listener thread may be closing the socket:
 * hold the mutex across the send so the fd cannot be closed (and reused) under us. */
void bridge_send(char c)
{
    pthread_mutex_lock(&g_mu);
    if (g_client >= 0) {
        ssize_t n = send(g_client, &c, 1, MSG_NOSIGNAL | MSG_DONTWAIT);   /* non-blocking: never stalls the game */
        (void)n;                                                          /* a lost control byte is re-sent on the next change */
    }
    pthread_mutex_unlock(&g_mu);
}
