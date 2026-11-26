#include "game/editor.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "game/game.h"
#include "game/hud.h"
#include "optics/optics.h"
#include "platform/input.h"
#include "platform/terminal.h"
#include "renderer/renderer.h"

/* 놓을 수 있는 것: 방 파일의 지도 글자와 같다. '@'와 '2'만 지도 글자가 아니라 시작 위치, 2번 플레이어 시작 위치 */
static const struct { char c; const char *name; } TOOLS[] = {
    { '#', "벽" }, { '|', "기둥" }, { 'g', "유리 블록" }, { 'D', "문" }, { '_', "발판" }, { '=', "창살" },
    { 'E', "광원" }, { 'P', "프리즘" }, { 'M', "거울" }, { 'S', "반투명 거울" }, { 'b', "파랑 필터" }, { 'r', "빨강 필터" },
    { 'R', "빨강 수정" }, { 'G', "초록 수정" }, { 'B', "파랑 수정" }, { 'Y', "노랑 수정" }, { 'C', "청록 수정" }, { 'V', "자홍 수정" },
    { 'O', "감지기" }, { 'T', "순간이동 고리" }, { 'K', "상자" }, { 'h', "전달 창구" },
    { '@', "시작 위치" }, { '2', "2번 시작 위치 (나뉜 2인용 방)" },
};
#define NTOOL ((int)(sizeof TOOLS / sizeof TOOLS[0]))
static const char OBJ_CHARS[] = "EPMSbrRGBYCVOTK";   /* must match world.c */

static Level s_lv;                      /* 편집 중인 방 */
static char s_file[64];                 /* 저장할 파일 이름 */
static int s_cx = 1, s_cz = 1;          /* 커서 칸 */
static int s_tool = 8;                  /* 고른 도구 (처음은 거울) */
static int s_dirty;                     /* 저장하지 않은 변경이 있다 */
static int s_quit_armed;                /* ESC를 한 번 눌렀다 (저장 안 하고 나가기 확인) */
static int s_naming;                    /* 이름 입력 중 */
static char s_old_name[64];
static char s_msg[200];

static void msg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void msg(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_msg, sizeof s_msg, fmt, ap);
    va_end(ap);
}

static const char *cell_name(char c)
{
    if (c == '.') return "바닥";
    for (int i = 0; i < NTOOL; i++)
        if (TOOLS[i].c == c) return TOOLS[i].name;
    return "?";
}

/* 편집 중인 방으로 월드를 다시 만들고 빛을 계산한다 (사람은 없다고 본다) */
static void apply(void)
{
    g_levels[0] = s_lv;
    g_nlevels = 1;
    g_cur_level = 0;
    load_level();
    for (int i = 0; i < MAXPLAYER; i++) g_pl[i].on = 0;
    game_simulate();
}

static void changed(void)
{
    s_dirty = 1;
    s_quit_armed = 0;
    apply();
}

void editor_open(const Level *src)
{
    if (src) {
        s_lv = *src;
        snprintf(s_file, sizeof s_file, "%s", src->file);
    } else {                                              /* 새 방: 벽으로 둘러싸인 빈 방과 오른쪽 문 */
        memset(&s_lv, 0, sizeof s_lv);
        snprintf(s_lv.name, sizeof s_lv.name, "새 방");
        for (int z = 0; z < RD; z++)
            for (int x = 0; x < RW; x++)
                s_lv.map[z][x] = (x == 0 || z == 0 || x == RW - 1 || z == RD - 1) ? '#' : '.';
        s_lv.map[5][RW - 1] = s_lv.map[6][RW - 1] = 'D';
        s_lv.sx = 2; s_lv.sz = 6; s_lv.face = 'E';
        s_file[0] = 0;
    }
    if (!s_file[0]) world_new_file_name(s_file, sizeof s_file);
    s_cx = s_lv.sx; s_cz = s_lv.sz;
    s_dirty = s_quit_armed = s_naming = 0;
    apply();
    msg("방향키로 칸을 고르고 Enter로 놓으세요. V: 풀이 확인, T: 시험 플레이, S: 저장");
}

void editor_resume(void)
{
    apply();
    msg("시험 플레이에서 돌아왔어요");
}

const Level *editor_level(void) { return &s_lv; }

/* ---------- 편집 ---------- */

/* 화면 위아래·좌우를 지금 카메라 방향에 맞는 칸 이동으로 바꾼다 */
static void move_cursor(int up, int right)
{
    float fx = sinf(cam_yaw), fz = cosf(cam_yaw), rx = cosf(cam_yaw), rz = -sinf(cam_yaw);
    float vx = fx * (float)up + rx * (float)right, vz = fz * (float)up + rz * (float)right;
    if (fabsf(vx) >= fabsf(vz)) s_cx += vx > 0 ? 1 : -1;
    else s_cz += vz > 0 ? 1 : -1;
    s_cx = s_cx < 0 ? 0 : s_cx >= RW ? RW - 1 : s_cx;
    s_cz = s_cz < 0 ? 0 : s_cz >= RD ? RD - 1 : s_cz;
}

static int count_objects(void)
{
    int n = 0;
    for (int z = 0; z < RD; z++)
        for (int x = 0; x < RW; x++) n += strchr(OBJ_CHARS, s_lv.map[z][x]) != NULL;
    return n;
}

static void place(void)
{
    char c = TOOLS[s_tool].c, *cell = &s_lv.map[s_cz][s_cx];
    int border = s_cx == 0 || s_cz == 0 || s_cx == RW - 1 || s_cz == RD - 1;
    if (c == '@') {
        if (*cell != '.') { msg("시작 위치는 빈 바닥에만 둘 수 있어요"); return; }
        s_lv.sx = s_cx; s_lv.sz = s_cz;
        msg("시작 위치를 (%d, %d)로 옮겼어요", s_cx, s_cz);
        changed();
        return;
    }
    if (c == '2') {                                       /* 같은 칸에 다시 놓으면 없앤다 */
        if (s_lv.has_start2 && s_lv.sx2 == s_cx && s_lv.sz2 == s_cz) {
            s_lv.has_start2 = 0;
            msg("2번 시작 위치를 없앴어요 (2번 플레이어는 1번 옆에서 시작)");
        } else if (*cell != '.' || (s_cx == s_lv.sx && s_cz == s_lv.sz)) {
            msg("2번 시작 위치는 1번 시작 위치가 아닌 빈 바닥에만 둘 수 있어요");
            return;
        } else {
            s_lv.has_start2 = 1; s_lv.sx2 = s_cx; s_lv.sz2 = s_cz;
            if (!s_lv.face2) s_lv.face2 = 'W';
            s_lv.coop = 1;                                /* 2번 시작 위치가 있으면 2인용 방 */
            msg("2번 시작 위치를 (%d, %d)로 옮겼어요. 2인용 방이 됩니다", s_cx, s_cz);
        }
        changed();
        return;
    }
    if (border && c != '#' && c != 'D') { msg("테두리에는 벽과 문만 놓을 수 있어요"); return; }
    if (s_cx == s_lv.sx && s_cz == s_lv.sz) { msg("시작 위치예요. 시작 위치를 먼저 옮기세요"); return; }
    if (s_lv.has_start2 && s_cx == s_lv.sx2 && s_cz == s_lv.sz2) { msg("2번 시작 위치예요. 먼저 옮기세요"); return; }
    if (*cell == c) return;
    if (c == 'T') {                                       /* 순간이동 고리는 한 쌍까지 */
        int n = 0;
        for (int z = 0; z < RD; z++)
            for (int x = 0; x < RW; x++) n += s_lv.map[z][x] == 'T';
        if (n >= 2) { msg("순간이동 고리는 한 쌍(2개)까지 놓을 수 있어요"); return; }
    }
    if (strchr(OBJ_CHARS, c) && !strchr(OBJ_CHARS, *cell) && count_objects() >= MAXOBJ) {
        msg("물체는 %d개까지 놓을 수 있어요", MAXOBJ);
        return;
    }
    *cell = c;
    s_lv.rot[s_cz][s_cx] = 0;
    s_lv.chan[s_cz][s_cx] = 0;
    msg("(%d, %d)에 %s", s_cx, s_cz, TOOLS[s_tool].name);
    changed();
}

static void erase(void)
{
    int border = s_cx == 0 || s_cz == 0 || s_cx == RW - 1 || s_cz == RD - 1;
    char want = border ? '#' : '.';
    if (s_lv.map[s_cz][s_cx] == want) return;
    s_lv.map[s_cz][s_cx] = want;
    s_lv.rot[s_cz][s_cx] = 0;
    s_lv.chan[s_cz][s_cx] = 0;
    msg("(%d, %d)를 %s", s_cx, s_cz, border ? "벽으로 되돌렸어요" : "지웠어요");
    changed();
}

static void rotate(int d)
{
    char c = s_lv.map[s_cz][s_cx];
    if (c != 'M' && c != 'S' && c != 'E') { msg("돌릴 수 있는 것은 거울, 반투명 거울, 광원이에요"); return; }
    s_lv.rot[s_cz][s_cx] = (signed char)((s_lv.rot[s_cz][s_cx] + d + 8) % 8);
    msg("%s 각도 %d (%s)", cell_name(c), s_lv.rot[s_cz][s_cx], c == 'E' ? "45도 단위" : "22.5도 단위");
    changed();
}

/* 발판·창살·감지기의 연결 번호를 0→1→2→3→0으로 바꾼다. 같은 번호끼리만 이어진다 */
static void cycle_link(void)
{
    char c = s_lv.map[s_cz][s_cx];
    if (!strchr("_=O", c) || !c) { msg("연결 번호는 발판, 창살, 감지기에만 붙일 수 있어요"); return; }
    s_lv.chan[s_cz][s_cx] = (signed char)((s_lv.chan[s_cz][s_cx] + 1) % 4);
    static const char *COL[4] = { "기본", "주황", "분홍", "하늘" };
    msg("%s 연결 번호 %d (%s). 창살은 같은 번호의 발판·감지기가 모두 켜져야 열려요", cell_name(c), s_lv.chan[s_cz][s_cx], COL[(int)s_lv.chan[s_cz][s_cx]]);
    changed();
}

/* 풀이 확인: 거울 각도를 모두 시험한다. 처음부터 풀려 있으면 알려 준다 */
static void verify(void)
{
    char err[160];
    if (world_check_level(&s_lv, err, sizeof err) < 0) { msg("%s", err); return; }
    int k = 0;
    for (int z = 0; z < RD; z++)
        for (int x = 0; x < RW; x++) k += s_lv.map[z][x] == 'M' || s_lv.map[z][x] == 'S';
    if (k > SOLVE_MAX_MIRRORS) { msg("거울이 %d개라 모든 각도를 시험할 수 없어요 (%d개까지)", k, SOLVE_MAX_MIRRORS); return; }
    apply();
    game_solve_setup();                                   /* 누군가 서 있거나 물체를 옮겨야 풀리는 방 */
    game_simulate();
    /* the search tries 8^k mirror settings; estimate its time from one measured light calculation
     * so the editor never freezes for long (the game itself is single-threaded here) */
    double est = (double)(g_sim_us > 0 ? g_sim_us : 1) / 1e6;
    for (int i = 0; i < k; i++) est *= 8;
    if (est > 5) { msg("거울이 %d개라 풀이 확인에 약 %.0f초 걸려요. 거울을 줄이거나 방 파일의 rot로 정답에 가깝게 두세요", k, est); return; }
    if (g_solved) {
        msg("처음부터 풀려 있어요. 거울 각도를 바꿔 풀리지 않은 상태로 시작하게 하세요");
    } else if (game_solve()) {
        char sol[160] = "";
        size_t n = 0;
        for (int i = 0; i < g_nobj && n < sizeof sol - 24; i++)
            if (g_obj[i].kind == O_MIRROR || g_obj[i].kind == O_SPLIT)
                n += (size_t)snprintf(sol + n, sizeof sol - n, " (%d,%d)→%d", g_obj[i].x, g_obj[i].z, g_obj[i].rot);
        msg("풀 수 있어요! 정답 각도:%s", sol);
    } else {
        msg("거울 각도만으로는 풀 수 없어요 (물체를 옮겨야 풀리거나, 풀 수 없는 방)");
    }
    apply();
}

static void save(void)
{
    char err[200];
    if (world_save_level(&s_lv, s_file, err, sizeof err) < 0) { msg("저장하지 못했어요: %s", err); return; }
    snprintf(s_lv.file, sizeof s_lv.file, "%s", s_file);
    s_dirty = s_quit_armed = 0;
    msg("저장했어요: %s/%s", g_level_dir, s_file);
}

/* 이름 끝의 UTF-8 글자 하나를 지운다 */
static void name_backspace(void)
{
    size_t n = strlen(s_lv.name);
    while (n > 0 && (s_lv.name[n - 1] & 0xC0) == 0x80) n--;
    if (n > 0) n--;
    s_lv.name[n] = 0;
}

int editor_key(int vk)
{
    if (s_naming) {
        if (vk == VK_ENTER) { s_naming = 0; s_dirty = 1; msg("이름: %s", s_lv.name); }
        else if (vk == VK_BACK) name_backspace();
        else if (vk == VK_ESC) { s_naming = 0; snprintf(s_lv.name, sizeof s_lv.name, "%s", s_old_name); msg("이름 바꾸기를 취소했어요"); }
        return EDIT_NONE;
    }
    if (vk != VK_ESC) s_quit_armed = 0;
    switch (vk) {
    case VK_UP: move_cursor(1, 0); break;
    case VK_DOWN: move_cursor(-1, 0); break;
    case VK_LEFT: move_cursor(0, -1); break;
    case VK_RIGHT: move_cursor(0, 1); break;
    case VK_TAB: s_tool = (s_tool + 1) % NTOOL; break;
    case VK_ENTER: case 'F': place(); break;
    case VK_BACK: case 'X': erase(); break;
    case 'Q': rotate(-1); break;
    case 'E': rotate(1); break;
    case VK_PLUS: cam_dist = fmaxf(10, cam_dist - 1.5f); break;
    case VK_MINUS: cam_dist = fminf(30, cam_dist + 1.5f); break;
    case 'N':
        s_naming = 1;
        snprintf(s_old_name, sizeof s_old_name, "%s", s_lv.name);
        s_lv.name[0] = 0;                                 /* 새로 입력 (ESC면 원래 이름으로) */
        msg("새 이름을 입력하고 Enter (한글 가능, ESC 취소)");
        break;
    case 'V': verify(); break;
    case 'L': cycle_link(); break;
    case 'S': save(); break;
    case 'T': {
        char err[160];
        if (world_check_level(&s_lv, err, sizeof err) < 0) { msg("시험 플레이 전에: %s", err); break; }
        return EDIT_TEST;
    }
    case VK_ESC:
        if (s_dirty && !s_quit_armed) {
            s_quit_armed = 1;
            msg("저장하지 않은 변경이 있어요. 그냥 나가려면 ESC를 한 번 더, 저장하려면 S");
            break;
        }
        return EDIT_QUIT;
    }
    return EDIT_NONE;
}

void editor_text(int ch)
{
    if (s_naming) {                                       /* 유니코드 글자를 UTF-8로 붙인다 */
        char u[5] = { 0 };
        if (ch < 0x80) u[0] = (char)ch;
        else if (ch < 0x800) { u[0] = (char)(0xC0 | ch >> 6); u[1] = (char)(0x80 | (ch & 0x3F)); }
        else if (ch < 0x10000) { u[0] = (char)(0xE0 | ch >> 12); u[1] = (char)(0x80 | (ch >> 6 & 0x3F)); u[2] = (char)(0x80 | (ch & 0x3F)); }
        else return;
        if (strlen(s_lv.name) + strlen(u) < 48) strcat(s_lv.name, u);
        return;
    }
    if (ch == ']') s_tool = (s_tool + 1) % NTOOL;
    else if (ch == '[') s_tool = (s_tool + NTOOL - 1) % NTOOL;
    else if (ch == ',') cam_yaw_target -= PI / 4;
    else if (ch == '.') cam_yaw_target += PI / 4;
}

/* ---------- 그리기 ---------- */

void editor_draw(int frame)
{
    if (g_cols < 80 || g_rows < 24) {
        for (int r = 0; r < g_rows; r++) term_bar(r, 0x101010);
        term_text(1, 1, 0xFFD060, 0x101010, "창이 작아요: 80x24 이상 (지금 %dx%d)", g_cols, g_rows);
        term_present();
        return;
    }
    cam_yaw += (cam_yaw_target - cam_yaw) * 0.25f;
    int W, H;
    View v;
    memset(&v, 0, sizeof v);
    hud_frame_size(&W, &H, &v.paspect);
    v.cam_yaw = cam_yaw; v.cam_pitch = cam_pitch; v.cam_dist = cam_dist;
    v.tx = s_cx; v.tz = s_cz;                             /* 커서 칸 표시 */
    v.sel_obj = g_objat[s_cz][s_cx] - 1;                  /* 커서 칸의 물체에는 노란 테두리 */
    v.seg = g_seg; v.nseg = g_nseg;
    static Body start[2];                                 /* 시작 위치에 사람을 세워 보여 준다 (2번 시작 위치는 2번 색) */
    start[0] = (Body){ (float)s_lv.sx + 0.5f, (float)s_lv.sz + 0.5f, 0, 0 };
    start[1] = (Body){ (float)s_lv.sx2 + 0.5f, (float)s_lv.sz2 + 0.5f, 1, 0 };
    v.bodies = start; v.nbody = s_lv.has_start2 ? 2 : 1;
    renderer_draw(&v, W, H, frame);
    hud_blit(256);

    int total, lit = optics_count_lit(&total);
    term_bar(0, 0x2A1E12);
    int x = term_text(1, 0, 0xFFD27A, 0x2A1E12, " 방 만들기 ") + 2;
    x += term_text(x, 0, 0xF0E6D8, 0x2A1E12, "%s%s", s_lv.name, s_dirty ? " *" : "");
    x += term_text(x, 0, 0xB8A890, 0x2A1E12, "  |  %s/%s  |  수정 %d/%d %s", g_level_dir, s_file, lit, total, g_solved ? "(풀린 상태)" : "");

    /* 도구 줄: 이전 ◀ [지금] ▶ 다음,  커서 칸 내용 */
    term_bar(1, 0x161822);
    int p = (s_tool + NTOOL - 1) % NTOOL, n = (s_tool + 1) % NTOOL;
    x = term_text(1, 1, 0x9AA0B0, 0x161822, " 도구  %s ◀ ", TOOLS[p].name);
    x += term_text(1 + x, 1, 0x101010, 0xFFD27A, " %s ", TOOLS[s_tool].name);
    term_text(1 + x, 1, 0x9AA0B0, 0x161822, " ▶ %s", TOOLS[n].name);
    char c = s_lv.map[s_cz][s_cx], info[96];
    char link[24] = "";
    if (strchr("_=O", c) && c && s_lv.chan[s_cz][s_cx]) snprintf(link, sizeof link, " · 연결 %d", s_lv.chan[s_cz][s_cx]);
    snprintf(info, sizeof info, "커서 (%d, %d): %s%s%s%s", s_cx, s_cz, cell_name(c), link,
             s_cx == s_lv.sx && s_cz == s_lv.sz ? " · 시작 위치" : "",
             s_lv.has_start2 && s_cx == s_lv.sx2 && s_cz == s_lv.sz2 ? " · 2번 시작 위치" : "");
    if (c == 'M' || c == 'S' || c == 'E') snprintf(info + strlen(info), sizeof info - strlen(info), " · 각도 %d", s_lv.rot[s_cz][s_cx]);
    term_text(g_cols - 1 - term_str_w(info), 1, 0xE8ECF4, 0x161822, "%s", info);

    if (s_naming) {                                       /* 이름 입력 상자 */
        int bw = 50, bx = g_cols / 2 - bw / 2, by = g_rows / 2 - 2;
        for (int y = by; y < by + 4; y++)
            for (int xx = bx; xx < bx + bw; xx++) g_back[y][xx] = (VCell){ 0xFFFFFF, 0x1A1E2A, " ", 0 };
        term_text(bx + 2, by + 1, 0xFFD27A, 0x1A1E2A, "방 이름");
        term_text(bx + 2, by + 2, 0xFFFFFF, 0x2E3550, " %s%s ", s_lv.name, (frame / 8) % 2 ? "_" : " ");
    }
    if (s_msg[0]) term_text(g_cols / 2 - term_str_w(s_msg) / 2 - 1, g_rows - 3, 0xFFE6A0, 0x22252F, " %s ", s_msg);
    term_bar(g_rows - 1, 0x161822);
    term_text(1, g_rows - 1, 0x9AA0B0, 0x161822,
              "방향키 커서  Tab/[ ] 도구  Enter/F 놓기  X 지우기  Q/E 돌리기  L 연결  ,/. 카메라  V 풀이확인  T 시험  S 저장  N 이름  ESC 나가기");
    term_present();
}
