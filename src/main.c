/*
 * PRISM - a terminal 3D optics puzzle
 *
 * Entry point: command-line options, test and screenshot modes, the start menu, the 30 fps game loop (solo / host / join).
 *
 *   world     level files, blocks and objects
 *   optics    light simulation (reflection, splitting, dispersion, refraction, filters, crystals)
 *   renderer  voxel ray casting and SDF objects, glowing beams, outlines (multithreaded)
 *   game      players, cameras, aiming, take/place/turn, puzzle checks / hud: screen info / menu: start menu
 *   net       two-player co-op (TCP)
 *   platform  terminal output, input parsing, clock, mouse-helper bridge
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
#include "world/watch.h"
#include "world/world.h"

enum { UI_MENU, UI_PLAY, UI_EDIT };
static int s_testing;                   /* test play from the editor (ESC goes back to the editor) */
static int s_ui = UI_PLAY;
static int s_look = 1;                  /* V: mouse look (while the mouse-lock helper is connected) */
#define MOUSE_SENS 0.0022f              /* view angle per mouse count (radians) */
static const char *s_level_dir;         /* --levels */
static int s_port = NET_PORT;           /* port used when hosting from the menu */
static int s_frame;

/* ---------- level files ---------- */

/* Read the level files: only dir if given, otherwise ./levels -> levels next to the executable -> the built-in room.
 * On a broken level file, print why and return 0 */
static int load_levels(const char *dir)
{
    char err[512];
    int n;
    if (dir) {
        n = world_load_levels(dir, err, sizeof err);
        if (n == 0) snprintf(err, sizeof err, "%s: 방 파일(.lvl)이 없습니다", dir);
    } else {
        n = world_load_levels("levels", err, sizeof err);
        if (n == 0) {                                     /* run from another folder: next to the executable */
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

/* solo play leaves out two-player rooms */
static void prepare_levels(int solo)
{
    load_levels(s_level_dir);                             /* checked once at startup, so this does not fail */
    watch_loaded();                                       /* hot reload compares against this */
    if (solo) world_drop_coop();
}

/* ---------- starting and ending sessions ---------- */

static void go_menu(const char *msg)
{
    s_testing = 0;
    net_close();
    game_reset();
    prepare_levels(0);
    game_init(0);
    game_solve();                                         /* the background shows the solved room */
    progress_load();                                      /* show solved rooms in the menu */
    menu_open(msg);
    s_ui = UI_MENU;
}

static void start_solo(int level)
{
    game_reset();
    prepare_levels(1);
    g_record = 1;                                         /* record solved rooms in the progress file */
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
    menu_draw(s_frame);                                   /* screen shown while waiting (up to 5 s) */
    game_reset();
    if (net_join(addr, err, sizeof err) < 0) { go_menu(err); return; }
    s_ui = UI_PLAY;
}

/* ---------- level hot reload ---------- */

/* The level files changed on disk (world/watch.c): reload them. On a parse error keep the old rooms and show the error.
 * Only while playing solo or hosting; the editor and joiners pick changes up on their next start. */
static void reload_levels(void)
{
    static Level old[MAXLEVEL];
    char err[512], file[64];
    int old_n = g_nlevels, old_cur = g_cur_level;
    watch_loaded();
    if (s_ui != UI_PLAY || s_testing || net_mode() == NET_CLIENT) return;
    memcpy(old, g_levels, sizeof old);
    snprintf(file, sizeof file, "%s", g_levels[g_cur_level].file);
    int n = world_load_levels(g_level_dir, err, sizeof err);
    if (n <= 0) {
        memcpy(g_levels, old, sizeof old);
        g_nlevels = old_n;
        if (n == 0) snprintf(err, sizeof err, "%s: 방 파일(.lvl)이 없습니다", g_level_dir);
        game_message(g_me, "방 파일 오류, 이전 방을 그대로 써요: %.200s", err);
        return;
    }
    if (net_mode() == NET_OFF) world_drop_coop();
    int idx = -1;
    for (int i = 0; i < g_nlevels; i++)
        if (file[0] && !strcmp(g_levels[i].file, file)) idx = i;
    if (idx < 0) idx = old_cur < g_nlevels ? old_cur : g_nlevels - 1;
    if (memcmp(&old[old_cur], &g_levels[idx], sizeof(Level)) == 0) {   /* another file changed: keep playing */
        g_cur_level = idx;
        game_message(g_me, "방 목록을 다시 읽었어요 (%d개)", g_nlevels);
        return;
    }
    for (int i = 0; i < MAXPLAYER; i++) { g_pl[i].carry = -1; g_pl[i].hint_t = 0; }
    game_init(idx);                                       /* rebuild the room from the new file */
    game_find_target(g_me);
    if (net_mode() == NET_HOST) net_level_changed();
    game_message(-1, "방 파일이 바뀌어 다시 만들었어요: %s", g_levels[idx].name);
}

/* ---------- wiring input events ---------- */

/* this terminal player's action: a joiner sends it to the host, solo and host handle it here */
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

/* open the level editor on room level (-1 = new room) */
static void start_edit(int level)
{
    game_reset();
    prepare_levels(0);
    Level src;
    if (level >= 0 && level < g_nlevels) src = g_levels[level];
    editor_open(level >= 0 && level < g_nlevels ? &src : NULL);
    s_ui = UI_EDIT;
}

/* play the editor's room solo as it is */
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
    else if (vk == 'P') hud_toggle_perf();
    else if (vk == 'V' && !bridge_connected())            /* nothing to toggle without the Windows mouse helper */
        game_message(g_me, "마우스 시선은 Windows에서 mouselock 도우미를 실행했을 때만 쓸 수 있어요");
    else if (vk == 'V') {                                 /* mouse look on/off (and the pointer lock with it) */
        s_look = !s_look;
        game_message(g_me, s_look ? "마우스 시선 켜짐: 마우스로 둘러봐요 (V로 끄기)" : "마우스 시선 꺼짐: 포인터가 풀렸어요 (V로 켜기)");
    }
    else if (vk == 'G') {                                 /* switch display mode */
        hud_set_quad(!hud_quad());
        game_message(g_me, hud_quad() ? "선명한 화면: 한 칸에 2×2 픽셀" : "반블록 화면: 한 칸에 1×2 픽셀 (블록 글자 이음매가 보이면)");
    }
    else if (vk == 'F') do_action(ACT_TAKE);
    else if (vk == 'Q') do_action(ACT_ROT_LEFT);
    else if (vk == 'E') do_action(ACT_ROT_RIGHT);
    else if (vk == 'R') do_action(ACT_RESET);
    else if (vk == 'Z') do_action(ACT_UNDO);
    else if (vk == 'I') do_action(ACT_HINT);
    else if (vk == VK_SPACE) do_action(ACT_JUMP);
    else game_view_key(vk);
}

static void on_quit(void) { g_quit = 1; }

/*
 * With the mouse-lock helper (tools/mouselock.c) connected, the mouse turns the view in first person.
 * The helper hides the pointer and pins it to the window centre on the game's L (lock), or frees it on U (unlock).
 * Outside first-person play (menu, editor, top-down) the pointer is left free.
 */
static void mouse_look_update(void)
{
    static int was_want = -1, was_conn = 0;
    int want = s_ui == UI_PLAY && g_pl[g_me].fp && s_look;
    int conn = bridge_connected();
    if (want != was_want || conn != was_conn) {        /* tell the helper again when it (re)connects or the window comes to the front */
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

/* ---------- entry point ---------- */

int main(int argc, char **argv)
{
    int check = 0, do_solve = 0, sel = -1, fp_shot = 0, level = 1, level_set = 0, host_port = 0, shot_w = PIX_W, shot_h = PIX_H;
    int term_cols = 0, term_rows = 0, guide_ticks = -1;
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
        else if (!strcmp(argv[i], "--full-res")) hud_set_autores(0);   /* no dynamic resolution (benchmarks) */
        else if (!strcmp(argv[i], "--guide") && i + 1 < argc) guide_ticks = atoi(argv[++i]);   /* dev: hint, then simulate n ticks */
        else if (!strcmp(argv[i], "--term-shot") && i + 3 < argc) {   /* development: PNG preview of the terminal screen */
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
            fprintf(stderr, "usage: %s [--host [port] | --join addr[:port]] [--levels dir] [--level n] [--halfblock] [--full-res]\n"
                            "       %*s [--check] [--solve] [--shot file.ppm|file.png [yaw pitch [dist]]] [--size w h] [--fp x z yaw pitch] [--select n]\n"
                            "       %*s [--term-shot file.png cols rows]\n"
                            "       옵션 없이 실행하면 시작 메뉴가 열립니다\n",
                    argv[0], (int)strlen(argv[0]), "", (int)strlen(argv[0]), "");
            return 1;
        }
    }
    if ((join || host_port) && (check || shot)) { fprintf(stderr, "prism: --host/--join은 --check/--shot과 함께 쓸 수 없습니다\n"); return 1; }
    if (join && host_port) { fprintf(stderr, "prism: --host와 --join은 함께 쓸 수 없습니다\n"); return 1; }
    if (host_port) s_port = host_port;
    char err[256];
    if (join) {                                           /* join: the room and state come from the host */
        if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) { fprintf(stderr, "prism: run this in a terminal\n"); return 1; }
        if (!load_levels(s_level_dir)) return 1;          /* rooms to use after returning to the menu */
        if (net_join(join, err, sizeof err) < 0) { fprintf(stderr, "prism: %s\n", err); return 1; }
    } else {
        if (!load_levels(s_level_dir)) return 1;
        if (!host_port && !check && !shot) world_drop_coop();   /* solo skips two-player rooms */
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
            game_solve_setup();                           /* apply the level's stand and move lines */
            int ok = game_solve();
            if (check) game_print_state(ok ? "solved" : "no solution found");
        }
        game_find_target(0);
        if (guide_ticks >= 0) {                           /* show the hint guide walking toward its target */
            game_action(0, ACT_HINT);
            for (int t = 0; t < guide_ticks; t++) game_tick(1.0f / 30);
            if (check) printf("hint: %s\n", p0->msg);
        }
        if (sel >= 0 && sel < g_nobj) p0->tobj = sel;
        if (shot && term_cols) {                          /* terminal preview: draw the cells' block characters as they would appear */
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
            int W = shot_w, H = shot_h, cr = W / 120 > 2 ? W / 120 : 2;   /* draws the default 720x420 as is (no scaling) */
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
    /* the 30 fps tick; without it the loop would wait forever, so failing here is fatal */
    int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
    struct itimerspec its = { { 0, 1000000000L / 30 }, { 0, 1000000000L / 30 } };
    if (tfd < 0 || timerfd_settime(tfd, 0, &its, NULL) < 0) {
        perror("prism: timerfd");
        renderer_shutdown();
        net_close();
        return 1;
    }
    if (term_setup() < 0) {
        perror("prism: terminal");
        close(tfd);
        renderer_shutdown();
        net_close();
        return 1;
    }
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
        go_menu(NULL);                                    /* no options: the start menu */
    int64_t fps_t0 = now;
    int fps = 0, fps_n = 0;
    /* poll set: stdin, game timer, inotify (fd -1 is ignored by poll), then up to two sockets */
    struct pollfd pf[5] = { { STDIN_FILENO, POLLIN, 0 }, { tfd, POLLIN, 0 }, { watch_start(), POLLIN, 0 } };
    while (!g_quit) {
        if (g_resize) { g_resize = 0; term_get_size(); }
        int nnet = net_pollfds(pf + 3, 2);
        if (poll(pf, (nfds_t)(3 + nnet), -1) < 0) {
            if (errno == EINTR) continue;
            break;
        }
        now = now_ms();
        input_set_time(now);
        if (pf[0].revents & POLLIN) input_read();
        if (input_esc_expired()) on_press(VK_ESC);        /* the ESC key alone */
        if (pf[2].revents & POLLIN) watch_events(now);    /* level files changed (inotify) */
        if (watch_due(now)) reload_levels();
        mouse_look_update();                              /* mouse-lock helper: view and pointer lock */
        net_handle(pf + 3, nnet);                         /* accept connections, handle received messages */
        if (net_lost()) {                                 /* lost the host: back to the menu showing why */
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
                net_after_tick();                         /* host sends the state, joiner sends its keys */
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
    bridge_stop();                                        /* tell the helper to free the pointer and quit */
    term_restore();
    net_close();
    renderer_shutdown();
    close(tfd);
    watch_stop();
    return 0;
}
