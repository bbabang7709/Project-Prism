/*
 * PRISM - 터미널 3D 광학 퍼즐
 *
 * 진입점: 명령행 옵션, 시험·스크린샷 모드, 30fps 게임 루프 (혼자 / 호스트 / 참가).
 *
 *   world     방 파일, 블록과 물체
 *   optics    빛 시뮬레이션 (반사, 분할, 분광, 굴절, 필터, 수정)
 *   renderer  복셀 레이캐스팅과 SDF 물체, 빛줄기 발광, 윤곽선 (멀티스레드)
 *   game      플레이어, 카메라, 겨누기, 들기·놓기·돌리기, 퍼즐 판정 / hud: 화면 정보
 *   net       2인 협동 (TCP)
 *   platform  터미널 출력, 입력 해석, 시계
 */
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include "game/game.h"
#include "game/hud.h"
#include "net/net.h"
#include "platform/clock.h"
#include "platform/input.h"
#include "platform/terminal.h"
#include "renderer/renderer.h"
#include "world/world.h"

/* ---------- 입력 사건 연결 ---------- */

/* 이 터미널 플레이어의 조작: 참가자는 호스트로 보내고, 혼자나 호스트는 바로 처리한다 */
static void do_action(int act)
{
    if (net_mode() == NET_CLIENT) net_send_action(act);
    else game_action(g_me, act);
}

static void on_click(void) { do_action(ACT_TAKE); }
static void on_wheel(int dir) { do_action(dir < 0 ? ACT_ROT_LEFT : ACT_ROT_RIGHT); }

static void on_press(int vk)
{
    if (vk == VK_ESC) g_quit = 1;
    else if (vk == 'H') hud_toggle_help();
    else if (vk == 'F') do_action(ACT_TAKE);
    else if (vk == 'Q') do_action(ACT_ROT_LEFT);
    else if (vk == 'E') do_action(ACT_ROT_RIGHT);
    else if (vk == 'R') do_action(ACT_RESET);
    else game_view_key(vk);
}

static void on_quit(void) { g_quit = 1; }

/* ---------- 방 파일 ---------- */

/* 방 파일을 읽는다. dir을 주면 그 폴더만, 아니면 ./levels → 실행 파일 옆 levels → 기본 방 순서로 찾는다.
 * 잘못된 방 파일이 있으면 이유를 출력하고 0을 돌려준다 */
static int load_levels(const char *dir)
{
    char err[512];
    int n;
    if (dir) {
        n = world_load_levels(dir, err, sizeof err);
        if (n == 0) snprintf(err, sizeof err, "%s: 방 파일(.lvl)이 없습니다", dir);
    } else {
        n = world_load_levels("levels", err, sizeof err);
        if (n == 0) {                                     /* 다른 폴더에서 실행했을 때: 실행 파일 옆 */
            char exe[512];
            ssize_t len = readlink("/proc/self/exe", exe, sizeof exe - 16);
            if (len > 0) {
                exe[len] = 0;
                char *slash = strrchr(exe, '/');
                if (slash) {
                    strcpy(slash, "/levels");
                    n = world_load_levels(exe, err, sizeof err);
                }
            }
        }
        if (n == 0) { world_use_builtin(); n = 1; }
    }
    if (n <= 0) { fprintf(stderr, "prism: %s\n", err); return 0; }
    return 1;
}

/* ---------- 진입점 ---------- */

int main(int argc, char **argv)
{
    int check = 0, do_solve = 0, sel = -1, fp_shot = 0, level = 1, host_port = 0;
    float fx = 0, fz = 0, fyaw = 0, fpitch = 0;
    const char *shot = NULL, *level_dir = NULL, *join = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--host")) {
            host_port = NET_PORT;
            if (i + 1 < argc && argv[i + 1][0] != '-') host_port = atoi(argv[++i]);
            if (host_port <= 0 || host_port > 65535) { fprintf(stderr, "prism: 포트 번호가 잘못되었습니다\n"); return 1; }
        } else if (!strcmp(argv[i], "--join") && i + 1 < argc) join = argv[++i];
        else if (!strcmp(argv[i], "--check")) check = 1;
        else if (!strcmp(argv[i], "--solve")) do_solve = 1;
        else if (!strcmp(argv[i], "--select") && i + 1 < argc) sel = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--level") && i + 1 < argc) level = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--levels") && i + 1 < argc) level_dir = argv[++i];
        else if (!strcmp(argv[i], "--fp") && i + 4 < argc) {
            fp_shot = 1;
            fx = strtof(argv[++i], NULL); fz = strtof(argv[++i], NULL);
            fyaw = strtof(argv[++i], NULL); fpitch = strtof(argv[++i], NULL);
        } else if (!strcmp(argv[i], "--shot") && i + 1 < argc) {
            shot = argv[++i];
            if (i + 2 < argc && argv[i + 1][0] != '-') { cam_yaw = cam_yaw_target = strtof(argv[++i], NULL); cam_pitch = strtof(argv[++i], NULL); }
            if (i + 1 < argc && argv[i + 1][0] != '-') cam_dist = strtof(argv[++i], NULL);
        } else {
            fprintf(stderr, "usage: %s [--host [port] | --join addr[:port]] [--levels dir] [--level n]\n"
                            "       %*s [--check] [--solve] [--shot file.ppm [yaw pitch [dist]]] [--fp x z yaw pitch] [--select n]\n",
                    argv[0], (int)strlen(argv[0]), "");
            return 1;
        }
    }
    if ((join || host_port) && (check || shot)) { fprintf(stderr, "prism: --host/--join은 --check/--shot과 함께 쓸 수 없습니다\n"); return 1; }
    if (join && host_port) { fprintf(stderr, "prism: --host와 --join은 함께 쓸 수 없습니다\n"); return 1; }
    char err[256];
    if (join) {                                           /* 참가: 방과 상태는 호스트에게서 받는다 */
        if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) { fprintf(stderr, "prism: run this in a terminal\n"); return 1; }
        if (net_join(join, err, sizeof err) < 0) { fprintf(stderr, "prism: %s\n", err); return 1; }
    } else {
        if (!load_levels(level_dir)) return 1;
        if (!host_port && !check && !shot) world_drop_coop();   /* 혼자서는 2인용 방을 건너뛴다 */
        if (level < 1 || level > g_nlevels) {
            fprintf(stderr, "prism: 방 번호는 1~%d입니다 (%d)\n", g_nlevels, level);
            return 1;
        }
        game_init(level - 1);
    }
    int nthreads = renderer_init();

    if (check || shot) {
        Player *p0 = &g_pl[0];
        p0->fp = fp_shot;
        if (fp_shot) { p0->x = fx; p0->z = fz; p0->yaw = fyaw; p0->pitch = fpitch; }
        if (check) game_print_state("start");
        if (do_solve) {
            const Level *lv = &g_levels[g_cur_level];
            if (lv->coop) {                               /* 2인용 방: 두 번째 사람이 정해진 칸에서 빛을 막는다 */
                g_pl[1].on = 1;
                g_pl[1].x = (float)lv->stand_x + 0.5f;
                g_pl[1].z = (float)lv->stand_z + 0.5f;
            }
            int ok = game_solve();
            if (check) game_print_state(ok ? "solved" : "no solution found");
        }
        game_find_target(0);
        if (sel >= 0 && sel < g_nobj) p0->tobj = sel;
        if (shot) {
            int W = 240, H = 140;
            View view;
            game_view(&view);
            int64_t t0 = now_us();
            renderer_draw(&view, W, H, 0);
            double ms = (double)(now_us() - t0) / 1000.0;
            if (p0->fp)
                for (int k = -2; k <= 2; k++) { g_pix[H / 2][W / 2 + k] = 0xE8E8E8; g_pix[H / 2 + k][W / 2] = 0xE8E8E8; }
            renderer_write_ppm(shot, W, H, 3);
            printf("wrote %s (%dx%d px, %.1f ms with %d threads)\n", shot, W, H, ms, nthreads);
        }
        renderer_shutdown();
        return 0;
    }
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        fprintf(stderr, "prism: run this in a terminal\n");
        renderer_shutdown();
        net_close();
        return 1;
    }
    if (host_port && net_host(host_port, err, sizeof err) < 0) {
        fprintf(stderr, "prism: %s\n", err);
        renderer_shutdown();
        return 1;
    }

    input_init(&(InputHandlers){ on_press, on_click, on_wheel, on_quit });
    term_install_signals();
    int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
    struct itimerspec its = { { 0, 1000000000L / 30 }, { 0, 1000000000L / 30 } };
    timerfd_settime(tfd, 0, &its, NULL);
    term_setup();
    term_get_size();
    int64_t now = now_ms();
    input_set_time(now);
    if (net_mode() == NET_HOST)
        game_message(g_me, "다른 터미널에서 ./prism --join 주소 로 참가할 수 있어요");
    else if (net_mode() == NET_OFF) {
        game_find_target(g_me);
        game_message(g_me, "거울을 들고, 놓고, 돌려서 모든 수정에 맞는 색의 빛을 보내세요");
    }
    int64_t fps_t0 = now;
    int frame = 0, fps = 0, fps_n = 0;
    struct pollfd pf[4] = { { STDIN_FILENO, POLLIN, 0 }, { tfd, POLLIN, 0 } };
    while (!g_quit) {
        if (g_resize) { g_resize = 0; term_get_size(); }
        int nnet = net_pollfds(pf + 2, 2);
        if (poll(pf, (nfds_t)(2 + nnet), -1) < 0) {
            if (errno == EINTR) continue;
            break;
        }
        now = now_ms();
        input_set_time(now);
        if (pf[0].revents & POLLIN) input_read();
        if (input_esc_expired()) g_quit = 1;
        net_handle(pf + 2, nnet);                         /* 접속 받기, 받은 메시지 처리 */
        if (net_lost()) g_quit = 1;
        if (pf[1].revents & POLLIN) {
            uint64_t t;
            if (read(tfd, &t, sizeof t) != sizeof t) continue;
            if (net_mode() == NET_CLIENT) game_local_tick();
            else game_tick(1.0f / 30);
            net_after_tick();                             /* 호스트는 상태를, 참가자는 키를 보낸다 */
            hud_draw(frame++, fps, net_status());
            fps_n++;
            if (now - fps_t0 >= 1000) {
                fps = (int)(fps_n * 1000 / (now - fps_t0));
                fps_n = 0;
                fps_t0 = now;
            }
        }
    }
    term_restore();
    net_close();
    renderer_shutdown();
    close(tfd);
    if (net_lost()) { fprintf(stderr, "prism: %s\n", net_lost()); return 1; }
    return 0;
}
