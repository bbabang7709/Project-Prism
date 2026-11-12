#define _GNU_SOURCE
#include "renderer/renderer.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "renderer/raycast.h"

/* ---------- 스레드 ---------- */

static int g_nthreads = 1;
static pthread_barrier_t g_go, g_done;
static volatile int g_wquit;
static pthread_t g_tid[MAX_THREADS];

static void render_rows(int id)
{
    raycast_rows(id, g_nthreads);
}

static void *worker(void *arg)
{
    int id = (int)(intptr_t)arg;
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
    int nthreads = (int)sysconf(_SC_NPROCESSORS_ONLN);
    nthreads = nthreads < 1 ? 1 : nthreads > MAX_THREADS ? MAX_THREADS : nthreads;
    g_nthreads = nthreads;
    pthread_barrier_init(&g_go, NULL, (unsigned)nthreads);
    pthread_barrier_init(&g_done, NULL, (unsigned)nthreads);
    for (int i = 1; i < nthreads; i++) pthread_create(&g_tid[i], NULL, worker, (void *)(intptr_t)i);
    return nthreads;
}

void renderer_shutdown(void)
{
    g_wquit = 1;
    pthread_barrier_wait(&g_go);
    for (int i = 1; i < g_nthreads; i++) pthread_join(g_tid[i], NULL);
}

/* ---------- 후처리 ---------- */

static inline uint32_t scale_c(uint32_t c, float k)
{
    return pack((float)(c >> 16 & 255) * k, (float)(c >> 8 & 255) * k, (float)(c & 255) * k);
}

/* 윤곽선과 선택 테두리 */
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
            if (lap > iz * 0.15f && far > z * 1.04f) g_pix[j][i] = scale_c(g_pix[j][i], 0.45f);
            else if (g_id[j][i + 1] != id || g_id[j + 1][i] != id) g_pix[j][i] = scale_c(g_pix[j][i], 0.72f);
        }
}

/* ---------- 카메라와 프레임 ---------- */

static void setup_frame(const View *v, int W, int H, int frame)
{
    Frame *F = &g_fr;
    float yaw = v->fp ? v->pyaw : v->cam_yaw, pitch = v->fp ? v->ppitch : v->cam_pitch;
    float cy = cosf(yaw), sy = sinf(yaw), cp = cosf(pitch), sp = sinf(pitch);
    F->fx = sy * cp; F->fy = sp; F->fz = cy * cp;
    F->rx = cy; F->ry = 0; F->rz = -sy;
    F->ux = -sy * sp; F->uy = cp; F->uz = -cy * sp;
    if (v->fp) {
        F->ox = v->px; F->oy = EYE_Y; F->oz = v->pz;
        F->tanh = 0.72f;
    } else {
        F->ox = RW / 2.0f - F->fx * v->cam_dist;
        F->oy = 1.0f - F->fy * v->cam_dist;
        F->oz = RD / 2.0f - F->fz * v->cam_dist;
        F->tanh = 0.58f;
    }
    float nl = sqrtf(F->fx * F->fx + F->fz * F->fz) + 1e-6f;
    F->nearx = -F->fx / nl; F->nearz = -F->fz / nl;
    F->W = W; F->H = H;
    F->frame = frame;
    F->fp = v->fp;
    F->tx = v->tx; F->tz = v->tz;
    raycast_prepare(v->seg, v->nseg, v->bodies, v->nbody);
}

void renderer_draw(const View *v, int W, int H, int frame)
{
    setup_frame(v, W, H, frame);
    pthread_barrier_wait(&g_go);
    render_rows(0);
    pthread_barrier_wait(&g_done);
    post_pass(W, H, v->sel_obj >= 0 ? 101 + v->sel_obj : 0, frame);
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

/* ---------- PNG 저장 ---------- */

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

/* 길이, 종류, 내용, CRC 순서로 청크 하나를 쓴다 */
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
    size_t row = (size_t)w * 3 + 1, raw_len = row * (size_t)h;   /* 줄마다 필터 바이트(0) + RGB */
    size_t nblk = (raw_len + 65534) / 65535;
    size_t zlen = 2 + raw_len + nblk * 5 + 4;                    /* zlib 머리 + 무압축 블록들 + Adler-32 */
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
        z[o++] = i + n == raw_len;                               /* 마지막 블록 표시, 무압축(00) */
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
    ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = ihdr[11] = ihdr[12] = 0;   /* 8비트 RGB */
    png_chunk(f, "IHDR", ihdr, 13);
    png_chunk(f, "IDAT", z, (uint32_t)o);
    png_chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(raw);
    free(z);
    return 0;
}
