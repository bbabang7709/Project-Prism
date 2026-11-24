/*
 * PRISM - 터미널 3D 광학 퍼즐
 *
 * 진입점: 명령행 옵션, 시험·스크린샷 모드, 시작 메뉴, 30fps 게임 루프 (혼자 / 호스트 / 참가).
 *
 *   world     방 파일, 블록과 물체
 *   optics    빛 시뮬레이션 (반사, 분할, 분광, 굴절, 필터, 수정)
 *   renderer  복셀 레이캐스팅과 SDF 물체, 빛줄기 발광, 윤곽선 (멀티스레드)
 *   game      플레이어, 카메라, 겨누기, 들기·놓기·돌리기, 퍼즐 판정 / hud: 화면 정보 / menu: 시작 메뉴
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
#include "game/editor.h"
#include "game/menu.h"
#include "game/progress.h"
#include "net/net.h"
#include "platform/bridge.h"
#include "platform/clock.h"
#include "platform/input.h"
#include "platform/terminal.h"
#include "renderer/renderer.h"
#include "world/world.h"

enum { UI_MENU, UI_PLAY, UI_EDIT };
static int s_testing;                   /* 에디터에서 시험 플레이 중 (ESC로 에디터에 돌아간다) */
static int s_ui = UI_PLAY;
static int s_look = 1;                  /* V: 마우스로 시선 돌리기 (마우스 잠금 도우미가 연결되어 있을 때) */
#define MOUSE_SENS 0.0022f              /* 마우스 한 칸(count)당 돌아가는 각도 (rad) */
static const char *s_level_dir;         /* --levels */
static int s_port = NET_PORT;           /* 메뉴에서 호스트로 시작할 때의 포트 */
static int s_frame;

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

/* 혼자 하기는 2인용 방을 뺀다 */
static void prepare_levels(int solo)
{
    load_levels(s_level_dir);                             /* 시작할 때 한 번 확인했으므로 실패하지 않는다 */
    if (solo) world_drop_coop();
}

/* ---------- 세션 시작과 끝 ---------- */

static void go_menu(const char *msg)
{
    s_testing = 0;
    net_close();
    game_reset();
    prepare_levels(0);
    game_init(0);
    game_solve();                                         /* 배경은 풀린 방을 보여 준다 */
    progress_load();                                      /* 메뉴에 푼 방을 표시한다 */
    menu_open(msg);
    s_ui = UI_MENU;
}

static void start_solo(int level)
{
    game_reset();
    prepare_levels(1);
    g_record = 1;                                         /* 푼 방을 진행 기록에 남긴다 */
    game_init(level < g_nlevels ? level : 0);
    game_find_target(g_me);
    game_message(g_me, "거울을 들고, 놓고, 돌려서 모든 수정에 맞는 색의 빛을 보내세요");
    s_ui = UI_PLAY;
}

static void start_host(int level)
{
    char err[256];
    game_reset();
    prepare_levels(0);
    g_record = 1;
    game_init(level < g_nlevels ? level : 0);
    if (net_host(s_port, err, sizeof err) < 0) { go_menu(err); return; }
    game_find_target(g_me);
    game_message(g_me, "다른 터미널에서 ./prism --join 주소 로 참가할 수 있어요");
    s_ui = UI_PLAY;
}

static void start_join(const char *addr)
{
    char err[256], what[300];
    snprintf(what, sizeof what, "%s에 접속하는 중...", addr);
    menu_message(what);
    menu_draw(s_frame);                                   /* 기다리는 동안 보일 화면 (최대 5초) */
    game_reset();
    if (net_join(addr, err, sizeof err) < 0) { go_menu(err); return; }
    s_ui = UI_PLAY;
}

/* ---------- 입력 사건 연결 ---------- */

/* 이 터미널 플레이어의 조작: 참가자는 호스트로 보내고, 혼자나 호스트는 바로 처리한다 */
static void do_action(int act)
{
    if (net_mode() == NET_CLIENT) net_send_action(act);
    else game_action(g_me, act);
}

static void on_click(void) { if (s_ui == UI_PLAY) do_action(ACT_TAKE); }
static void on_wheel(int dir) { if (s_ui == UI_PLAY) do_action(dir < 0 ? ACT_ROT_LEFT : ACT_ROT_RIGHT); }
static void on_text(int ch)
{
    if (s_ui == UI_MENU) menu_text(ch);
    else if (s_ui == UI_EDIT) editor_text(ch);
}

/* 레벨 에디터 열기: level번 방을 고친다 (-1이면 새 방) */
static void start_edit(int level)
{
    game_reset();
    prepare_levels(0);
    Level src;
    if (level >= 0 && level < g_nlevels) src = g_levels[level];
    editor_open(level >= 0 && level < g_nlevels ? &src : NULL);
    s_ui = UI_EDIT;
}

/* 에디터의 방을 그대로 혼자 해 본다 */
static void start_test(void)
{
    Level lv = *editor_level();
    game_reset();
    g_levels[0] = lv;
    g_nlevels = 1;
    game_init(0);
    game_find_target(g_me);
    game_message(g_me, "시험 플레이: ESC를 누르면 에디터로 돌아가요");
    s_testing = 1;
    s_ui = UI_PLAY;
}

static void back_to_editor(void)
{
    s_testing = 0;
    for (int i = 0; i < MAXPLAYER; i++) g_pl[i].on = 0;
    editor_resume();
    s_ui = UI_EDIT;
}

static void on_press(int vk)
{
    if (s_ui == UI_MENU) {
        MenuChoice c = menu_key(vk);
        if (c.what == MENU_SOLO) start_solo(c.level);
        else if (c.what == MENU_HOST) start_host(c.level);
        else if (c.what == MENU_JOIN) start_join(c.addr);
        else if (c.what == MENU_EDIT) start_edit(c.level);
        else if (c.what == MENU_QUIT) g_quit = 1;
        return;
    }
    if (s_ui == UI_EDIT) {
        int r = editor_key(vk);
        if (r == EDIT_QUIT) go_menu(NULL);
        else if (r == EDIT_TEST) start_test();
        return;
    }
    if (s_testing && (vk == VK_ESC || (g_cleared && vk == VK_ENTER))) back_to_editor();
    else if (vk == VK_ESC || (g_cleared && vk == VK_ENTER)) go_menu(NULL);
    else if (vk == 'H') hud_toggle_help();
    else if (vk == 'V') {                                 /* 마우스 시선 켜기/끄기 (포인터 잠금도 같이) */
        s_look = !s_look;
        game_message(g_me, s_look ? "마우스 시선 켜짐: 마우스로 둘러봐요 (V로 끄기)" : "마우스 시선 꺼짐: 포인터가 풀렸어요 (V로 켜기)");
    }
    else if (vk == 'G') {                                 /* 화면 방식 바꾸기 */
        hud_set_quad(!hud_quad());
        game_message(g_me, hud_quad() ? "선명한 화면: 한 칸에 2×2 픽셀" : "반블록 화면: 한 칸에 1×2 픽셀 (블록 글자 이음매가 보이면)");
    }
    else if (vk == 'F') do_action(ACT_TAKE);
    else if (vk == 'Q') do_action(ACT_ROT_LEFT);
    else if (vk == 'E') do_action(ACT_ROT_RIGHT);
    else if (vk == 'R') do_action(ACT_RESET);
    else if (vk == 'Z') do_action(ACT_UNDO);
    else if (vk == 'I') do_action(ACT_HINT);
    else game_view_key(vk);
}

static void on_quit(void) { g_quit = 1; }

/*
 * 마우스 잠금 도우미(tools/mouselock.c)가 연결되어 있으면 1인칭에서 마우스로 시선을 돌린다.
 * 도우미는 게임이 보내는 L(잠금)·U(풀기)에 따라 포인터를 숨기고 창 가운데에 고정하거나 푼다.
 * 1인칭 플레이 중이 아니면(메뉴, 에디터, 내려다보기) 포인터를 풀어 둔다.
 */
static void mouse_look_update(void)
{
    static int was_want = -1, was_conn = 0;
    int want = s_ui == UI_PLAY && g_pl[g_me].fp && s_look;
    int conn = bridge_connected();
    if (want != was_want || conn != was_conn) {        /* 도우미가 새로 연결되거나 창이 맨 앞이 되면 상태를 다시 알려 준다 */
        bridge_send(want ? 'L' : 'U');
        if (conn && !was_conn && s_ui == UI_PLAY) game_message(g_me, "마우스 시선 연결됨: 마우스로 둘러봐요 (V로 끄기)");
        was_want = want;
        was_conn = conn;
    }
    int dx, dy;
    bridge_take_motion(&dx, &dy);
    g_look_local = want && conn;
    if (g_look_local && (dx || dy)) game_mouse_look(dx * MOUSE_SENS, dy * MOUSE_SENS);
}

/* ---------- 진입점 ---------- */

int main(int argc, char **argv)
{
    int check = 0, do_solve = 0, sel = -1, fp_shot = 0, level = 1, level_set = 0, host_port = 0, shot_w = PIX_W, shot_h = PIX_H;
    int term_cols = 0, term_rows = 0;
    float fx = 0, fz = 0, fyaw = 0, fpitch = 0;
    const char *shot = NULL, *join = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--host")) {
            host_port = NET_PORT;
            if (i + 1 < argc && argv[i + 1][0] != '-') host_port = atoi(argv[++i]);
            if (host_port <= 0 || host_port > 65535) { fprintf(stderr, "prism: 포트 번호가 잘못되었습니다\n"); return 1; }
        } else if (!strcmp(argv[i], "--join") && i + 1 < argc) join = argv[++i];
        else if (!strcmp(argv[i], "--check")) check = 1;
        else if (!strcmp(argv[i], "--solve")) do_solve = 1;
        else if (!strcmp(argv[i], "--select") && i + 1 < argc) sel = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--level") && i + 1 < argc) { level = atoi(argv[++i]); level_set = 1; }
        else if (!strcmp(argv[i], "--levels") && i + 1 < argc) s_level_dir = argv[++i];
        else if (!strcmp(argv[i], "--halfblock")) hud_set_quad(0);
        else if (!strcmp(argv[i], "--term-shot") && i + 3 < argc) {   /* 개발용: 터미널 화면 미리보기 PNG */
            shot = argv[++i];
            term_cols = atoi(argv[++i]); term_rows = atoi(argv[++i]);
            if (term_cols < 20 || term_rows < 10 || term_cols > MAX_W || term_rows > MAX_H) {
                fprintf(stderr, "prism: --term-shot 칸 수는 20x10 ~ %dx%d입니다\n", MAX_W, MAX_H);
                return 1;
            }
        }
        else if (!strcmp(argv[i], "--size") && i + 2 < argc) {
            shot_w = atoi(argv[++i]); shot_h = atoi(argv[++i]);
            if (shot_w < 16 || shot_h < 16 || shot_w > PIX_W || shot_h > PIX_H) {
                fprintf(stderr, "prism: --size는 16x16 ~ %dx%d입니다\n", PIX_W, PIX_H);
                return 1;
            }
        }
        else if (!strcmp(argv[i], "--fp") && i + 4 < argc) {
            fp_shot = 1;
            fx = strtof(argv[++i], NULL); fz = strtof(argv[++i], NULL);
            fyaw = strtof(argv[++i], NULL); fpitch = strtof(argv[++i], NULL);
        } else if (!strcmp(argv[i], "--shot") && i + 1 < argc) {
            shot = argv[++i];
            if (i + 2 < argc && argv[i + 1][0] != '-') { cam_yaw = cam_yaw_target = strtof(argv[++i], NULL); cam_pitch = strtof(argv[++i], NULL); }
            if (i + 1 < argc && argv[i + 1][0] != '-') cam_dist = strtof(argv[++i], NULL);
        } else {
            fprintf(stderr, "usage: %s [--host [port] | --join addr[:port]] [--levels dir] [--level n] [--halfblock]\n"
                            "       %*s [--check] [--solve] [--shot file.ppm|file.png [yaw pitch [dist]]] [--size w h] [--fp x z yaw pitch] [--select n]\n"
                            "       옵션 없이 실행하면 시작 메뉴가 열립니다\n",
                    argv[0], (int)strlen(argv[0]), "");
            return 1;
        }
    }
    if ((join || host_port) && (check || shot)) { fprintf(stderr, "prism: --host/--join은 --check/--shot과 함께 쓸 수 없습니다\n"); return 1; }
    if (join && host_port) { fprintf(stderr, "prism: --host와 --join은 함께 쓸 수 없습니다\n"); return 1; }
    if (host_port) s_port = host_port;
    char err[256];
    if (join) {                                           /* 참가: 방과 상태는 호스트에게서 받는다 */
        if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) { fprintf(stderr, "prism: run this in a terminal\n"); return 1; }
        if (!load_levels(s_level_dir)) return 1;          /* 메뉴로 돌아왔을 때 쓸 방 */
        if (net_join(join, err, sizeof err) < 0) { fprintf(stderr, "prism: %s\n", err); return 1; }
    } else {
        if (!load_levels(s_level_dir)) return 1;
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
            game_solve_setup();                           /* 방 파일의 stand·move 적용 */
            int ok = game_solve();
            if (check) game_print_state(ok ? "solved" : "no solution found");
        }
        game_find_target(0);
        if (sel >= 0 && sel < g_nobj) p0->tobj = sel;
        if (shot && term_cols) {                          /* 터미널 화면 미리보기: 칸으로 옮긴 결과를 블록 모양 그대로 그린다 */
            g_cols = term_cols;
            g_rows = term_rows;
            int W, H;
            View view;
            game_view(&view);
            hud_frame_size(&W, &H, &view.paspect);
            renderer_draw(&view, W, H, 0);
            hud_blit(256);
            int cw = PIX_W / g_cols;
            if (cw > 8) cw = 8;
            cw &= ~1;
            while (cw > 2 && cw * 2 * g_rows > PIX_H) cw -= 2;
            hud_rasterize(cw, cw * 2);
            renderer_write_png(shot, g_cols * cw, g_rows * cw * 2, 1);
            printf("wrote %s (%dx%d cells, %s)\n", shot, g_cols, g_rows, hud_quad() ? "2x2 quadrant" : "half block");
        } else if (shot) {
            int W = shot_w, H = shot_h, cr = W / 120 > 2 ? W / 120 : 2;   /* 기본 720×420을 그대로 그린다 (늘리지 않음) */
            View view;
            game_view(&view);
            int64_t t0 = now_us();
            renderer_draw(&view, W, H, 0);
            double ms = (double)(now_us() - t0) / 1000.0;
            if (p0->fp)
                for (int k = -cr; k <= cr; k++) { g_pix[H / 2][W / 2 + k] = 0xE8E8E8; g_pix[H / 2 + k][W / 2] = 0xE8E8E8; }
            size_t sl = strlen(shot);
            if (sl > 4 && !strcmp(shot + sl - 4, ".png")) renderer_write_png(shot, W, H, 1);
            else renderer_write_ppm(shot, W, H, 1);
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

    input_init(&(InputHandlers){ on_press, on_click, on_wheel, on_quit, on_text });
    term_install_signals();
    int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
    struct itimerspec its = { { 0, 1000000000L / 30 }, { 0, 1000000000L / 30 } };
    timerfd_settime(tfd, 0, &its, NULL);
    term_setup();
    term_get_size();
    if (bridge_start() != 0) fprintf(stderr, "prism: 마우스 도우미 포트 %d를 쓸 수 없어요 (키보드 시선만 사용)\n", BRIDGE_PORT);
    int64_t now = now_ms();
    input_set_time(now);
    if (net_mode() == NET_HOST)
        game_message(g_me, "다른 터미널에서 ./prism --join 주소 로 참가할 수 있어요");
    else if (net_mode() == NET_OFF && level_set) {
        game_find_target(g_me);
        game_message(g_me, "거울을 들고, 놓고, 돌려서 모든 수정에 맞는 색의 빛을 보내세요");
    } else if (net_mode() == NET_OFF)
        go_menu(NULL);                                    /* 옵션 없이 실행: 시작 메뉴 */
    int64_t fps_t0 = now;
    int fps = 0, fps_n = 0;
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
        if (input_esc_expired()) on_press(VK_ESC);        /* ESC 키 단독 */
        mouse_look_update();                              /* 마우스 잠금 도우미: 시선과 포인터 잠금 */
        net_handle(pf + 2, nnet);                         /* 접속 받기, 받은 메시지 처리 */
        if (net_lost()) {                                 /* 호스트와 끊어졌다: 이유를 보여 주며 메뉴로 */
            char why[160];
            snprintf(why, sizeof why, "%s", net_lost());
            go_menu(why);
        }
        if (pf[1].revents & POLLIN) {
            uint64_t t;
            if (read(tfd, &t, sizeof t) != sizeof t) continue;
            if (s_ui == UI_MENU) menu_draw(s_frame++);
            else if (s_ui == UI_EDIT) editor_draw(s_frame++);
            else {
                if (net_mode() == NET_CLIENT) game_local_tick();
                else game_tick(1.0f / 30);
                net_after_tick();                         /* 호스트는 상태를, 참가자는 키를 보낸다 */
                hud_draw(s_frame++, fps, net_status());
            }
            fps_n++;
            if (now - fps_t0 >= 1000) {
                fps = (int)(fps_n * 1000 / (now - fps_t0));
                fps_n = 0;
                fps_t0 = now;
            }
        }
    }
    bridge_stop();                                        /* 도우미에게 포인터를 풀고 끝내라고 알린다 */
    term_restore();
    net_close();
    renderer_shutdown();
    close(tfd);
    return 0;
}
