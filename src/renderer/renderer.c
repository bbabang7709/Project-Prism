#define _GNU_SOURCE
#include "renderer/renderer.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "platform/clock.h"
#include "renderer/raycast.h"

/* ---------- threads ---------- */

static int g_nthreads = 1;
static pthread_barrier_t g_go, g_mid, g_done;
static volatile int g_wquit;
static pthread_t g_tid[MAX_THREADS];
static RenderStats g_stats;             /* timings of the last frame (each thread writes only its own slot) */

/* One frame: every thread draws its rows (all wait together at g_mid),
 * then, with the neighbouring rows drawn too, re-shoots edge pixels to smooth them */
static void render_rows(int id)
{
    int64_t t0 = now_us();
    raycast_rows(id, g_nthreads);
    int64_t t1 = now_us();
    pthread_barrier_wait(&g_mid);
    int64_t t2 = now_us();
    raycast_aa_rows(id, g_nthreads);
    int64_t t3 = now_us();
    g_stats.work_ms[id] = (float)((t1 - t0) + (t3 - t2)) / 1000.0f;   /* tracing rows + edge resampling */
    g_stats.wait_ms[id] = (float)(t2 - t1) / 1000.0f;                 /* idle at the barrier: load imbalance */
}

const RenderStats *renderer_stats(void) { return &g_stats; }

/* Start gate: workers are created first and wait here until the barriers exist.
 * The barriers need the exact thread count, which is only known once every
 * pthread_create has returned (one may fail). */
static pthread_mutex_t g_gate_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_gate_cv = PTHREAD_COND_INITIALIZER;
static int g_gate_open;

static void *worker(void *arg)
{
    int id = (int)(intptr_t)arg;
    pthread_mutex_lock(&g_gate_mu);
    while (!g_gate_open) pthread_cond_wait(&g_gate_cv, &g_gate_mu);
    pthread_mutex_unlock(&g_gate_mu);
    for (;;) {
        pthread_barrier_wait(&g_go);
        if (g_wquit) break;
        render_rows(id);
        pthread_barrier_wait(&g_done);
    }
    return NULL;
}

int renderer_init(void)
{
    int want = (int)sysconf(_SC_NPROCESSORS_ONLN), n = 1;
    want = want < 1 ? 1 : want > MAX_THREADS ? MAX_THREADS : want;
    for (int i = 1; i < want; i++, n++)                   /* thread 0 is the caller itself */
        if (pthread_create(&g_tid[i], NULL, worker, (void *)(intptr_t)i) != 0) {
            fprintf(stderr, "prism: 렌더 스레드를 %d개만 만들었어요 (계속 진행)\n", n);
            break;
        }
    g_nthreads = n;
    g_stats.nthreads = n;
    if (pthread_barrier_init(&g_go, NULL, (unsigned)n) != 0 || pthread_barrier_init(&g_mid, NULL, (unsigned)n) != 0 ||
        pthread_barrier_init(&g_done, NULL, (unsigned)n) != 0) {
        fprintf(stderr, "prism: pthread_barrier_init failed\n");
        exit(1);
    }
    pthread_mutex_lock(&g_gate_mu);                       /* barriers ready: let the workers in */
    g_gate_open = 1;
    pthread_cond_broadcast(&g_gate_cv);
    pthread_mutex_unlock(&g_gate_mu);
    return n;
}

void renderer_shutdown(void)
{
    g_wquit = 1;
    pthread_barrier_wait(&g_go);
    for (int i = 1; i < g_nthreads; i++) pthread_join(g_tid[i], NULL);
}

/* ---------- post-processing ---------- */

static inline uint32_t scale_c(uint32_t c, float k)
{
    return pack((float)(c >> 16 & 255) * k, (float)(c >> 8 & 255) * k, (float)(c & 255) * k);
}

/* outlines and the selection border */
static void post_pass(int W, int H, int sel_id, int frame)
{
    float pulse = 0.75f + 0.25f * sinf((float)frame * 0.25f);
    for (int j = 1; j < H - 1; j++)
        for (int i = 1; i < W - 1; i++) {
            uint16_t id = g_id[j][i];
            if (!id) continue;
            if (sel_id && id == sel_id &&
                (g_id[j][i + 1] != id || g_id[j][i - 1] != id || g_id[j + 1][i] != id || g_id[j - 1][i] != id)) {
                g_pix[j][i] = pack(255 * pulse, 210 * pulse, 80 * pulse);
                continue;
            }
            float z = g_depth[j][i], zl = g_depth[j][i - 1], zr = g_depth[j][i + 1], zu = g_depth[j - 1][i], zd = g_depth[j + 1][i];
            float iz = 1 / z, lap = fabsf(1 / zl + 1 / zr - 2 * iz) + fabsf(1 / zu + 1 / zd - 2 * iz);
            float far = fmaxf(fmaxf(zl, zr), fmaxf(zu, zd));
            /* objects, players and the guide (id 97 and up) get dark outlines to stand apart from the background;
             * wall and floor outlines stay light so objects catch the eye first */
            int thing = id >= 97;                                 /* 97..99: guide and players, 100+: objects */
            if (lap > iz * 0.15f && far > z * 1.04f) g_pix[j][i] = scale_c(g_pix[j][i], thing ? 0.35f : 0.62f);
            else if (g_id[j][i + 1] != id || g_id[j + 1][i] != id || g_id[j][i - 1] != id || g_id[j - 1][i] != id) {
                int nthing = g_id[j][i + 1] >= 97 || g_id[j + 1][i] >= 97 || g_id[j][i - 1] >= 97 || g_id[j - 1][i] >= 97;
                if (thing) g_pix[j][i] = scale_c(g_pix[j][i], 0.45f);     /* inside an object's edge */
                else if (!nthing && (g_id[j][i + 1] != id || g_id[j + 1][i] != id)) g_pix[j][i] = scale_c(g_pix[j][i], 0.86f);
            }
        }
    /* vignette: darken the corners a little to pull the eye to the centre */
    for (int j = 0; j < H; j++) {
        float ry = 2 * ((float)j + 0.5f) / (float)H - 1;
        for (int i = 0; i < W; i++) {
            float rx = 2 * ((float)i + 0.5f) / (float)W - 1;
            g_pix[j][i] = scale_c(g_pix[j][i], 1 - 0.18f * (rx * rx + ry * ry) * 0.5f);
        }
    }
}

/* ---------- camera and frame ---------- */

static void setup_frame(const View *v, int W, int H, int frame)
{
    Frame *F = &g_fr;
    float yaw = v->fp ? v->pyaw : v->cam_yaw, pitch = v->fp ? v->ppitch : v->cam_pitch;
    float cy = cosf(yaw), sy = sinf(yaw), cp = cosf(pitch), sp = sinf(pitch);
    F->fx = sy * cp; F->fy = sp; F->fz = cy * cp;
    F->rx = cy; F->ry = 0; F->rz = -sy;
    F->ux = -sy * sp; F->uy = cp; F->uz = -cy * sp;
    if (v->fp) {
        F->ox = v->px; F->oy = EYE_Y + v->py; F->oz = v->pz;
        F->tanh = 1.0f;                                   /* 90 degree horizontal field of view */
    } else {
        /* Look at the room centre, pulled a little toward the camera:
         * seen obliquely from above, the near edge stretches further down the screen */
        float hl = sqrtf(F->fx * F->fx + F->fz * F->fz) + 1e-6f, pull = 1.2f;
        float tx = RW / 2.0f - F->fx / hl * pull, tz = RD / 2.0f - F->fz / hl * pull;
        F->ox = tx - F->fx * v->cam_dist;
        F->oy = 1.0f - F->fy * v->cam_dist;
        F->oz = tz - F->fz * v->cam_dist;
        F->tanh = 0.58f;
    }
    float nl = sqrtf(F->fx * F->fx + F->fz * F->fz) + 1e-6f;
    F->nearx = -F->fx / nl; F->nearz = -F->fz / nl;
    F->W = W; F->H = H;
    F->pix = 2 * F->tanh / (float)W;
    F->aspect = (float)H / (float)W * (v->paspect > 0 ? v->paspect : 1);
    F->pixl = F->pix * (v->paspect > 1 ? v->paspect : 1);
    F->frame = frame;
    F->fp = v->fp;
    F->tx = v->tx; F->tz = v->tz;
    raycast_prepare(v->seg, v->nseg, v->bodies, v->nbody, F->oy);
}

void renderer_draw(const View *v, int W, int H, int frame)
{
    int64_t t0 = now_us();
    setup_frame(v, W, H, frame);
    pthread_barrier_wait(&g_go);
    render_rows(0);
    pthread_barrier_wait(&g_done);
    post_pass(W, H, v->sel_obj >= 0 ? 101 + v->sel_obj : 0, frame);
    g_stats.frame_ms = (float)(now_us() - t0) / 1000.0f;
    g_stats.W = W;
    g_stats.H = H;
}

void renderer_write_ppm(const char *path, int W, int H, int scale)
{
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return; }
    fprintf(f, "P6\n%d %d\n255\n", W * scale, H * scale);
    for (int y = 0; y < H * scale; y++)
        for (int x = 0; x < W * scale; x++) {
            uint32_t c = g_pix[y / scale][x / scale];
            unsigned char rgb[3] = { (unsigned char)(c >> 16), (unsigned char)(c >> 8), (unsigned char)c };
            fwrite(rgb, 1, 3, f);
        }
    fclose(f);
}

/* ---------- PNG output ---------- */

static uint32_t crc_table[256];

static uint32_t crc32_update(uint32_t c, const unsigned char *p, size_t n)
{
    if (!crc_table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t v = i;
            for (int k = 0; k < 8; k++) v = v & 1 ? 0xEDB88320u ^ (v >> 1) : v >> 1;
            crc_table[i] = v;
        }
    for (size_t i = 0; i < n; i++) c = crc_table[(c ^ p[i]) & 255] ^ (c >> 8);
    return c;
}

static void put_be32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)(v >> 24); p[1] = (unsigned char)(v >> 16); p[2] = (unsigned char)(v >> 8); p[3] = (unsigned char)v;
}

/* write one chunk: length, type, data, CRC */
static void png_chunk(FILE *f, const char *type, const unsigned char *data, uint32_t len)
{
    unsigned char hdr[8];
    put_be32(hdr, len);
    memcpy(hdr + 4, type, 4);
    fwrite(hdr, 1, 8, f);
    if (len) fwrite(data, 1, len, f);
    uint32_t c = crc32_update(0xFFFFFFFFu, (const unsigned char *)type, 4);
    c = crc32_update(c, data, len) ^ 0xFFFFFFFFu;
    put_be32(hdr, c);
    fwrite(hdr, 1, 4, f);
}

int renderer_write_png(const char *path, int W, int H, int scale)
{
    int w = W * scale, h = H * scale;
    size_t row = (size_t)w * 3 + 1, raw_len = row * (size_t)h;   /* each row: filter byte (0) + RGB */
    size_t nblk = (raw_len + 65534) / 65535;
    size_t zlen = 2 + raw_len + nblk * 5 + 4;                    /* zlib header + stored blocks + Adler-32 */
    unsigned char *raw = malloc(raw_len), *z = malloc(zlen);
    if (!raw || !z) { free(raw); free(z); return -1; }
    for (int y = 0; y < h; y++) {
        unsigned char *p = raw + row * (size_t)y;
        *p++ = 0;
        for (int x = 0; x < w; x++) {
            uint32_t c = g_pix[y / scale][x / scale];
            *p++ = (unsigned char)(c >> 16); *p++ = (unsigned char)(c >> 8); *p++ = (unsigned char)c;
        }
    }
    size_t o = 0;
    z[o++] = 0x78; z[o++] = 0x01;
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < raw_len; i += 65535) {
        size_t n = raw_len - i < 65535 ? raw_len - i : 65535;
        z[o++] = i + n == raw_len;                               /* last-block flag, stored (00) */
        z[o++] = (unsigned char)n; z[o++] = (unsigned char)(n >> 8);
        z[o++] = (unsigned char)~n; z[o++] = (unsigned char)(~n >> 8);
        memcpy(z + o, raw + i, n);
        for (size_t k = 0; k < n; k++) { a = (a + raw[i + k]) % 65521; b = (b + a) % 65521; }
        o += n;
    }
    put_be32(z + o, b << 16 | a);
    o += 4;
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); free(raw); free(z); return -1; }
    static const unsigned char SIG[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    fwrite(SIG, 1, 8, f);
    unsigned char ihdr[13];
    put_be32(ihdr, (uint32_t)w);
    put_be32(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = ihdr[11] = ihdr[12] = 0;   /* 8-bit RGB */
    png_chunk(f, "IHDR", ihdr, 13);
    png_chunk(f, "IDAT", z, (uint32_t)o);
    png_chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(raw);
    free(z);
    return 0;
}
