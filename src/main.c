/*
 * PRISM - 레이저 광선 퍼즐 프로토타입 (방 하나)
 *
 * 진입점: 명령행 옵션, 시험·스크린샷 모드, 30fps 게임 루프.
 *
 *   world     블록과 물체, 문자 지도
 *   optics    빛 시뮬레이션 (반사, 분할, 분광, 굴절, 필터, 수정)
 *   renderer  복셀 레이캐스팅과 SDF 물체, 빛줄기 발광, 윤곽선 (멀티스레드)
 *   game      플레이어, 카메라, 겨누기, 들기·놓기·돌리기, 퍼즐 판정 / hud: 화면 정보
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
#include "platform/clock.h"
#include "platform/input.h"
#include "platform/terminal.h"
#include "renderer/renderer.h"
#include "world/world.h"

/* ---------- 입력 사건 연결 ---------- */

static void on_press(int vk)
{
    if (vk == VK_ESC) g_quit = 1;
    else if (vk == 'H') hud_toggle_help();
    else game_press(vk);
}

static void on_quit(void) { g_quit = 1; }

/* ---------- 진입점 ---------- */

int main(int argc, char **argv)
{
    int check = 0, do_solve = 0, sel = -1, fp_shot = 0;
    float fx = 0, fz = 0, fyaw = 0, fpitch = 0;
    const char *shot = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--check")) check = 1;
        else if (!strcmp(argv[i], "--solve")) do_solve = 1;
        else if (!strcmp(argv[i], "--select") && i + 1 < argc) sel = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--fp") && i + 4 < argc) {
            fp_shot = 1;
            fx = strtof(argv[++i], NULL); fz = strtof(argv[++i], NULL);
            fyaw = strtof(argv[++i], NULL); fpitch = strtof(argv[++i], NULL);
        } else if (!strcmp(argv[i], "--shot") && i + 1 < argc) {
            shot = argv[++i];
            if (i + 2 < argc && argv[i + 1][0] != '-') { cam_yaw = cam_yaw_target = strtof(argv[++i], NULL); cam_pitch = strtof(argv[++i], NULL); }
            if (i + 1 < argc && argv[i + 1][0] != '-') cam_dist = strtof(argv[++i], NULL);
        } else {
            fprintf(stderr, "usage: %s [--check] [--solve] [--shot file.ppm [yaw pitch [dist]]] [--fp x z yaw pitch] [--select n]\n", argv[0]);
            return 1;
        }
    }
    game_init();
    int nthreads = renderer_init();

    if (check || shot) {
        g_fp = fp_shot;
        if (fp_shot) { g_px = fx; g_pz = fz; g_pyaw = fyaw; g_ppitch = fpitch; }
        if (check) game_print_state("start");
        if (do_solve) {
            int ok = game_solve();
            if (check) game_print_state(ok ? "solved" : "no solution found");
        }
        game_find_target();
        if (sel >= 0 && sel < g_nobj) g_tobj = sel;
        if (shot) {
            int W = 240, H = 140;
            View view;
            game_view(&view);
            int64_t t0 = now_us();
            renderer_draw(&view, W, H, 0);
            double ms = (double)(now_us() - t0) / 1000.0;
            if (g_fp)
                for (int k = -2; k <= 2; k++) { g_pix[H / 2][W / 2 + k] = 0xE8E8E8; g_pix[H / 2 + k][W / 2] = 0xE8E8E8; }
            renderer_write_ppm(shot, W, H, 3);
            printf("wrote %s (%dx%d px, %.1f ms with %d threads)\n", shot, W, H, ms, nthreads);
        }
        renderer_shutdown();
        return 0;
    }
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        fprintf(stderr, "prism: run this in a terminal\n");
        return 1;
    }

    input_init(&(InputHandlers){ on_press, game_act, game_rotate, on_quit });
    term_install_signals();
    int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
    struct itimerspec its = { { 0, 1000000000L / 30 }, { 0, 1000000000L / 30 } };
    timerfd_settime(tfd, 0, &its, NULL);
    term_setup();
    term_get_size();
    int64_t now = now_ms();
    input_set_time(now);
    game_find_target();
    game_message("거울을 들고, 놓고, 돌려서 모든 수정에 맞는 색의 빛을 보내세요");
    int64_t fps_t0 = now;
    int frame = 0, fps = 0, fps_n = 0;
    struct pollfd pf[2] = { { STDIN_FILENO, POLLIN, 0 }, { tfd, POLLIN, 0 } };
    while (!g_quit) {
        if (g_resize) { g_resize = 0; term_get_size(); }
        if (poll(pf, 2, -1) < 0) {
            if (errno == EINTR) continue;
            break;
        }
        now = now_ms();
        input_set_time(now);
        if (pf[0].revents & POLLIN) input_read();
        if (input_esc_expired()) g_quit = 1;
        if (pf[1].revents & POLLIN) {
            uint64_t t;
            if (read(tfd, &t, sizeof t) != sizeof t) continue;
            game_tick(1.0f / 30);
            hud_draw(frame++, fps);
            fps_n++;
            if (now - fps_t0 >= 1000) {
                fps = (int)(fps_n * 1000 / (now - fps_t0));
                fps_n = 0;
                fps_t0 = now;
            }
        }
    }
    term_restore();
    renderer_shutdown();
    close(tfd);
    return 0;
}
