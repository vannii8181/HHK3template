// Ashfall Mini: a small Doom-style shooter for the ClassPad fx-CP400 (Hollyhock-3)
//
// Controls:  UP / DOWN    = walk forward / back
//            LEFT / RIGHT = turn
//            Tap screen   = fire
//            Clear        = quit
//
// This file replaces src/main.cpp in the HHK3 template entirely.

#include <appdef.h>
#include <sdk/os/lcd.h>
#include <sdk/os/input.h>

APP_NAME("Ashfall Mini")
APP_AUTHOR("Gio")
APP_DESCRIPTION("Doom-style shooter: arrows move, tap to fire")
APP_VERSION("1.0.0")

// ---------- map: '.' = floor, anything else = wall ----------
#define MAP_W 16
#define MAP_H 16
static const char MAP[MAP_H][MAP_W + 1] = {
    "################",
    "#......#.......#",
    "#......#..%%%..#",
    "#..%%..#..%....#",
    "#..%...........#",
    "#......#####...#",
    "#..........#...#",
    "###..==....#...#",
    "#....=.........#",
    "#....=..%%%....#",
    "#.......%......#",
    "#..###..%..==..#",
    "#..#...........#",
    "#..#....####...#",
    "#..............#",
    "################",
};

struct Enemy { float x, y; int alive; };
static Enemy enemies[] = {
    {9.5f, 2.5f, 1}, {13.5f, 6.5f, 1}, {3.5f, 9.5f, 1},
    {10.5f, 10.5f, 1}, {13.5f, 13.5f, 1}, {5.5f, 14.5f, 1},
};
static const int ENEMY_COUNT = sizeof(enemies) / sizeof(enemies[0]);

// ---------- helpers (no math library needed) ----------
static inline uint16_t rgb(int r, int g, int b) {
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}
static inline uint16_t darker(uint16_t c) { return (c >> 1) & 0x7BEF; }
static inline float fab(float v) { return v < 0 ? -v : v; }
static inline int isWall(float x, float y) {
    int mx = (int)x, my = (int)y;
    if (mx < 0 || my < 0 || mx >= MAP_W || my >= MAP_H) return 1;
    return MAP[my][mx] != '.';
}
static void rotate(float &dx, float &dy, float &plx, float &ply, float c, float s) {
    float o = dx;  dx = dx * c - dy * s;   dy = o * s + dy * c;
    o = plx;       plx = plx * c - ply * s; ply = o * s + ply * c;
}
static void fillRect(uint16_t *vram, int W, int H, int x0, int y0, int w, int h, uint16_t c) {
    for (int y = y0; y < y0 + h; y++) {
        if (y < 0 || y >= H) continue;
        for (int x = x0; x < x0 + w; x++)
            if (x >= 0 && x < W) vram[y * W + x] = c;
    }
}

// ---------- game state ----------
static float px = 1.5f, py = 1.5f;      // player position
static float dx = 1.0f, dy = 0.0f;      // look direction
static float plx = 0.0f, ply = 0.66f;   // camera plane (sets field of view)
static int hp = 100, flashFrames = 0;
static float zbuf[512];

// Screen x of an enemy and its distance (ty). Returns 0 if behind the player.
static int project(const Enemy &e, int W, int &screenX, float &ty) {
    float sx = e.x - px, sy = e.y - py;
    float inv = 1.0f / (plx * dy - dx * ply);
    float tx = inv * (dy * sx - dx * sy);
    ty = inv * (-ply * sx + plx * sy);
    if (ty <= 0.15f) return 0;
    screenX = (int)((W / 2) * (1.0f + tx / ty));
    return 1;
}

static void render(uint16_t *vram, int W, int H) {
    int half = H / 2;
    uint16_t ceil = rgb(35, 32, 40), floor = rgb(75, 62, 52);
    for (int y = 0; y < H; y++) {
        uint16_t c = y < half ? ceil : floor;
        for (int x = 0; x < W; x++) vram[y * W + x] = c;
    }

    // walls: one ray per 2 screen columns (faster on the calculator)
    for (int x = 0; x < W; x += 2) {
        float cam = 2.0f * x / W - 1.0f;
        float rx = dx + plx * cam, ry = dy + ply * cam;
        int mx = (int)px, my = (int)py;
        float ddx = rx == 0 ? 1e30f : fab(1.0f / rx);
        float ddy = ry == 0 ? 1e30f : fab(1.0f / ry);
        int stepX, stepY, side = 0;
        float sdx, sdy;
        if (rx < 0) { stepX = -1; sdx = (px - mx) * ddx; } else { stepX = 1; sdx = (mx + 1.0f - px) * ddx; }
        if (ry < 0) { stepY = -1; sdy = (py - my) * ddy; } else { stepY = 1; sdy = (my + 1.0f - py) * ddy; }
        char hit = 0;
        for (int i = 0; i < 64 && !hit; i++) {
            if (sdx < sdy) { sdx += ddx; mx += stepX; side = 0; }
            else           { sdy += ddy; my += stepY; side = 1; }
            if (MAP[my][mx] != '.') hit = MAP[my][mx];
        }
        float perp = side ? sdy - ddy : sdx - ddx;
        if (perp < 0.01f) perp = 0.01f;
        zbuf[x] = zbuf[x + 1 < W ? x + 1 : x] = perp;

        int lh = (int)(W / perp);
        int y0 = half - lh / 2, y1 = half + lh / 2;
        if (y0 < 0) y0 = 0;
        if (y1 > H) y1 = H;
        uint16_t c = hit == '%' ? rgb(170, 90, 50) : hit == '=' ? rgb(60, 150, 140) : rgb(150, 150, 160);
        if (side) c = darker(c);
        if (perp > 6) c = darker(c);                 // simple distance fog
        for (int y = y0; y < y1; y++) {
            vram[y * W + x] = c;
            if (x + 1 < W) vram[y * W + x + 1] = c;
        }
    }

    // enemies: draw far ones first
    int order[16];
    for (int i = 0; i < ENEMY_COUNT; i++) order[i] = i;
    for (int i = 0; i < ENEMY_COUNT; i++)
        for (int j = i + 1; j < ENEMY_COUNT; j++) {
            Enemy &a = enemies[order[i]], &b = enemies[order[j]];
            float da = (a.x - px) * (a.x - px) + (a.y - py) * (a.y - py);
            float db = (b.x - px) * (b.x - px) + (b.y - py) * (b.y - py);
            if (db > da) { int t = order[i]; order[i] = order[j]; order[j] = t; }
        }
    for (int k = 0; k < ENEMY_COUNT; k++) {
        Enemy &e = enemies[order[k]];
        if (!e.alive) continue;
        int sx; float ty;
        if (!project(e, W, sx, ty)) continue;
        int size = (int)(W / ty * 0.7f);
        int bottom = half + (int)(W / ty / 2), top = bottom - size;
        for (int x = sx - size / 3; x < sx + size / 3; x++) {
            if (x < 0 || x >= W || ty >= zbuf[x]) continue;
            for (int y = top; y < bottom; y++) {
                if (y < 0 || y >= H) continue;
                int rel = (y - top) * 10 / size;     // 0..9 from head to feet
                uint16_t c = rgb(150, 40, 40);       // body
                if (rel == 1) c = rgb(255, 200, 60); // glowing eye band
                if (rel >= 7) c = rgb(60, 50, 50);   // legs
                if (ty > 6) c = darker(c);
                vram[y * W + x] = c;
            }
        }
    }

    // crosshair, muzzle flash, health bar
    fillRect(vram, W, H, W / 2 - 6, half, 13, 1, rgb(255, 120, 40));
    fillRect(vram, W, H, W / 2, half - 6, 1, 13, rgb(255, 120, 40));
    if (flashFrames > 0) { fillRect(vram, W, H, W / 2 - 14, H - 70, 28, 28, rgb(255, 220, 120)); flashFrames--; }
    fillRect(vram, W, H, W / 2 - 10, H - 45, 20, 45, rgb(70, 75, 82));   // gun barrel
    fillRect(vram, W, H, 10, 10, 102, 10, rgb(20, 20, 20));
    fillRect(vram, W, H, 11, 11, hp, 8, hp > 30 ? rgb(70, 210, 200) : rgb(220, 50, 60));

    LCD_Refresh();
}

static void shoot(int W) {
    flashFrames = 2;
    int best = -1; float bestT = zbuf[W / 2];
    for (int i = 0; i < ENEMY_COUNT; i++) {
        if (!enemies[i].alive) continue;
        int sx; float ty;
        if (!project(enemies[i], W, sx, ty)) continue;
        int halfW = (int)(W / ty * 0.7f) / 3;
        if (sx - halfW <= W / 2 && W / 2 <= sx + halfW && ty < bestT) { best = i; bestT = ty; }
    }
    if (best >= 0) enemies[best].alive = 0;
}

static void moveEnemies() {
    for (int i = 0; i < ENEMY_COUNT; i++) {
        Enemy &e = enemies[i];
        if (!e.alive) continue;
        float ex = px - e.x, ey = py - e.y, d2 = ex * ex + ey * ey;
        if (d2 > 49) continue;                       // only chase within 7 tiles
        float step = 0.06f, len = fab(ex) + fab(ey);
        float nx = e.x + ex / len * step, ny = e.y + ey / len * step;
        if (!isWall(nx, e.y)) e.x = nx;
        if (!isWall(e.x, ny)) e.y = ny;
        if (d2 < 0.5f) hp -= 4;                      // touching you hurts
    }
}

static void endScreen(uint16_t *vram, int W, int H, uint16_t c) {
    fillRect(vram, W, H, 0, 0, W, H, c);
    LCD_Refresh();
    struct Input_Event ev;
    for (;;) {                                       // wait for Clear
        ev = Input_Event{};
        GetInput(&ev, 0xFFFFFFFF, 0x10);
        if (ev.type == EVENT_KEY && ev.data.key.keyCode == KEYCODE_POWER_CLEAR) return;
    }
}

static void game() {
    uint16_t *vram = LCD_GetVRAMAddress();
    unsigned int uw, uh;
    LCD_GetSize(&uw, &uh);
    int W = (int)uw, H = (int)uh;
    if (W > 512) W = 512;

    const float C = 0.9950042f, S = 0.0998334f;      // cos/sin of 0.1 rad
    struct Input_Event ev;
    render(vram, W, H);

    for (;;) {
        ev = Input_Event{};
        GetInput(&ev, 0xFFFFFFFF, 0x10);
        if (ev.type == EVENT_KEY &&
            (ev.data.key.direction == KEY_PRESSED || ev.data.key.direction == KEY_HELD)) {
            float spd = 0.15f;
            switch (ev.data.key.keyCode) {
                case KEYCODE_UP:
                    if (!isWall(px + dx * spd * 2, py)) px += dx * spd;
                    if (!isWall(px, py + dy * spd * 2)) py += dy * spd;
                    break;
                case KEYCODE_DOWN:
                    if (!isWall(px - dx * spd * 2, py)) px -= dx * spd;
                    if (!isWall(px, py - dy * spd * 2)) py -= dy * spd;
                    break;
                case KEYCODE_LEFT:  rotate(dx, dy, plx, ply, C, -S); break;
                case KEYCODE_RIGHT: rotate(dx, dy, plx, ply, C,  S); break;
                case KEYCODE_POWER_CLEAR: return;    // quit
                default: break;
            }
        } else if (ev.type == EVENT_TOUCH && ev.data.touch_single.direction == TOUCH_DOWN) {
            shoot(W);
        }

        moveEnemies();
        if (hp <= 0) { endScreen(vram, W, H, rgb(150, 20, 30)); return; }
        int left = 0;
        for (int i = 0; i < ENEMY_COUNT; i++) left += enemies[i].alive;
        if (left == 0) { endScreen(vram, W, H, rgb(30, 140, 90)); return; }
        render(vram, W, H);
    }
}

int main(int, char **, char **) {
    game();
    return 0;
}
