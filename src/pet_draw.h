/* pet_draw.h - procedural pixel-art renderer for the Claude pet.
 * Pure C, no OS dependencies: draws one frame into a CW x CH ARGB canvas.
 * Shared by the Windows app and the Linux preview tool.
 * Authors: ASDFboy, Claude (Anthropic)
 * License: MIT (see LICENSE) */
#ifndef PET_DRAW_H
#define PET_DRAW_H
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define CW 40
#define CH 30

enum {
    /* autonomous */
    ST_IDLE, ST_WALK, ST_SIT, ST_SLEEP, ST_HOP, ST_DANCE, ST_YAWN, ST_TRIP, ST_WAVE,
    ST_LOVE, ST_DIZZY, ST_GRUMPY,
    /* physical */
    ST_DRAG, ST_FALL, ST_LAND,
    /* what Claude is doing */
    ST_THINK, ST_READ, ST_SEARCH, ST_WRITE, ST_TERM, ST_AGENT, ST_TOOL, ST_ATTN,
    ST_DONE, ST_OOPS, ST_HELLO,
    ST_COUNT
};

static const char *const ST_NAMES[ST_COUNT] = {
    "idle","walk","sit","sleep","hop","dance","yawn","trip","wave","love","dizzy","grumpy",
    "drag","fall","land",
    "think","read","search","write","terminal","agent","tool","attention","done","oops","hello"
};

typedef struct {
    int st;         /* display state */
    int t;          /* frames (30 fps) since this state started */
    int g;          /* global frame counter */
    int facing;     /* -1 left, +1 right */
    int blink;      /* 1 = eyes closed this frame */
    int lookX, lookY; /* -1..1 gaze */
    int walkPhase;
    int vy;         /* sign of vertical speed while airborne */
    int sleepy;     /* low energy */
} DrawIn;

/* palette (ARGB, straight alpha) */
#define C_OR    0xFFD97757u
#define C_ORD   0xFFB45A3Cu
#define C_BK    0xFF161616u
#define C_WH    0xFFFFFFFFu
#define C_OL    0xFF2B2B2Bu
#define C_PAP   0xFFF6F1E7u
#define C_LN    0xFFB9B2A6u
#define C_GR    0xFFA3A3A8u
#define C_GRD   0xFF4D4D52u
#define C_YEL   0xFFF5C842u
#define C_RED   0xFFE5484Du
#define C_BLU   0xFF7FC8F5u
#define C_BRN   0xFF7A4A2Eu
#define C_PNK   0xFFF29BB0u
#define C_GLASS 0x70BFE6FFu
#define C_PUFF  0xC0C8C8C8u

enum { A_REST, A_UP, A_WAVE2, A_DOWN, A_OUT, A_FRONT, A_HOLD, A_HIDE };
enum { E_OPEN, E_BLINK, E_HAPPY, E_WIDE, E_DIZZY, E_GRUMPY, E_SAD, E_HEART, E_SQUEEZE };

static uint32_t *pd_cv;

static void pd_px(int x, int y, uint32_t c) {
    if ((unsigned)x >= CW || (unsigned)y >= CH) return;
    uint32_t a = c >> 24;
    if (a == 255) { pd_cv[y * CW + x] = c; return; }
    if (!a) return;
    uint32_t d = pd_cv[y * CW + x];
    float fa = a / 255.f, fd = (d >> 24) / 255.f * (1.f - fa), oa = fa + fd;
    uint32_t out = (uint32_t)(oa * 255.f + .5f) << 24;
    for (int s = 0; s <= 16; s += 8) {
        float cs = (c >> s) & 255, ds = (d >> s) & 255;
        out |= (uint32_t)((cs * fa + ds * fd) / oa + .5f) << s;
    }
    pd_cv[y * CW + x] = out;
}
static void pd_rect(int x, int y, int w, int h, uint32_t c) {
    for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) pd_px(x + i, y + j, c);
}
/* tiny bitmap glyphs: rows of '#' */
static void pd_glyph(int x, int y, const char *rows, int w, uint32_t c) {
    for (int i = 0; rows[i]; i++) if (rows[i] == '#') pd_px(x + i % w, y + i / w, c);
}
#define G_Q    "###" "..#" ".##" "..." ".#."
#define G_EX   ".#." ".#." ".#." "..." ".#."
#define G_Z    "###" ".#." "###"
#define G_NOTE ".##" ".#." ".#." "##." "##."
#define G_HEART ".#.#." "#####" ".###." "..#.."
#define G_PLUS ".#." "###" ".#."

static void pd_bubble(int x, int y, int w, int h, int tail) {
    pd_rect(x + 1, y + 1, w - 2, h - 2, C_WH);
    pd_rect(x + 1, y, w - 2, 1, C_OL); pd_rect(x + 1, y + h - 1, w - 2, 1, C_OL);
    pd_rect(x, y + 1, 1, h - 2, C_OL); pd_rect(x + w - 1, y + 1, 1, h - 2, C_OL);
    if (tail >= 0) {
        pd_px(tail, y + h - 1, C_WH); pd_px(tail + 1, y + h - 1, C_WH);
        pd_px(tail, y + h, C_WH);
        pd_px(tail - 1, y + h, C_OL); pd_px(tail + 1, y + h, C_OL); pd_px(tail, y + h + 1, C_OL);
    }
}

typedef struct {
    int bw, bh, dx, sink, lift;
    int leg[4];
    int arm[2], armdy[2];
    int eyes, ex, ey;
} Pose;
typedef struct { int x0, top, bottom, bw, bh, cx; } Geo;

static void pd_eye(int x, int y, int type, int side) {
    switch (type) {
    case E_OPEN:   pd_rect(x, y, 2, 2, C_BK); break;
    case E_BLINK:  pd_rect(x, y + 1, 2, 1, C_BK); break;
    case E_HAPPY:  pd_px(x, y + 1, C_BK); pd_px(x + 1, y, C_BK); pd_px(x + 2, y + 1, C_BK);
                   if (side) { /* keep symmetric: shift right eye left by one */ } break;
    case E_WIDE:   pd_rect(x, y - 1, 2, 3, C_BK); break;
    case E_DIZZY:  pd_px(x - 1, y - 1, C_BK); pd_px(x + 1, y - 1, C_BK); pd_px(x, y, C_BK);
                   pd_px(x - 1, y + 1, C_BK); pd_px(x + 1, y + 1, C_BK); break;
    case E_GRUMPY: pd_rect(x, y + 1, 2, 1, C_BK); pd_px(side ? x + 1 : x, y, C_BK); break;
    case E_SAD:    pd_rect(x, y + 1, 2, 1, C_BK); pd_px(side ? x : x + 1, y, C_BK); break;
    case E_HEART:  pd_glyph(x - 1, y - 1, "#.#" "###" ".#.", 3, C_RED); break;
    case E_SQUEEZE:
        if (!side) { pd_px(x, y - 1, C_BK); pd_px(x + 1, y, C_BK); pd_px(x, y + 1, C_BK); }
        else       { pd_px(x + 1, y - 1, C_BK); pd_px(x, y, C_BK); pd_px(x + 1, y + 1, C_BK); }
        break;
    }
}

static void pd_arm(const Geo *G, int side, int pose, int dy, int front) {
    int isFront = (pose == A_FRONT || pose == A_HOLD);
    if (isFront != front || pose == A_HIDE) return;
    int s = side ? 1 : -1;
    int sx = side ? G->x0 + G->bw : G->x0 - 2;
    int t = G->top + dy;
    switch (pose) {
    case A_REST:  pd_rect(sx, t + 3, 2, 2, C_OR); break;
    case A_UP:    pd_rect(sx, t - 1, 2, 3, C_OR); break;
    case A_WAVE2: pd_rect(sx, t, 2, 2, C_OR); pd_rect(sx + s, t - 2, 2, 2, C_OR); break;
    case A_DOWN:  pd_rect(sx, t + 5, 2, 2, C_OR); break;
    case A_OUT:   pd_rect(side ? sx : sx - 1, t + 2, 3, 2, C_OR); break;
    case A_FRONT: pd_rect(side ? G->x0 + G->bw - 3 : G->x0 + 1, t + 5, 2, 2, C_ORD); break;
    case A_HOLD:  pd_rect(side ? G->x0 + G->bw - 4 : G->x0 + 2, t + 4, 2, 2, C_ORD); break;
    }
}

static void pd_mini(int x, int bottom, int hop, int blink) {
    int b = bottom - hop, top = b - 3;
    pd_px(x + 1, b + 1, C_OR); pd_px(x + 4, b + 1, C_OR);
    if (!hop) { pd_px(x + 1, b + 2, C_OR); pd_px(x + 4, b + 2, C_OR); }
    else      { pd_px(x + 1, b + 2, C_OR); pd_px(x + 4, b + 2, C_OR); }
    pd_rect(x, top, 6, 4, C_OR);
    pd_rect(x, b, 6, 1, C_ORD);
    pd_px(x - 1, top + 2 - (hop ? 1 : 0), C_OR); pd_px(x + 6, top + 2 - (hop ? 1 : 0), C_OR);
    if (!blink) { pd_px(x + 1, top + 1, C_BK); pd_px(x + 4, top + 1, C_BK); }
    else        { pd_px(x + 1, top + 2, C_BK); pd_px(x + 4, top + 2, C_BK); }
}

static void pd_sparkle(int x, int y, int big) {
    if (big) pd_glyph(x - 1, y - 1, G_PLUS, 3, C_YEL); else pd_px(x, y, C_YEL);
}

static void pet_draw(uint32_t *cv, const DrawIn *in) {
    pd_cv = cv;
    memset(cv, 0, sizeof(uint32_t) * CW * CH);
    Pose p = { 12, 8, 0, 0, 0, {2, 2, 2, 2}, {A_REST, A_REST}, {0, 0}, E_OPEN, 0, 0 };
    int t = in->t, f = in->facing;
    p.ex = in->lookX; p.ey = in->lookY;
    int allowBlink = 1;

    switch (in->st) {
    case ST_IDLE:
        if (t % 70 >= 45) p.bh = 7;
        if (in->sleepy && (in->g / 80) % 3 == 0) { p.eyes = E_BLINK; allowBlink = 0; }
        break;
    case ST_WALK: {
        int lp = in->walkPhase & 1;
        int a[4] = {2, 1, 2, 1}, b[4] = {1, 2, 1, 2};
        memcpy(p.leg, lp ? a : b, sizeof a);
        p.ex = f; p.ey = 0;
        p.armdy[0] = lp; p.armdy[1] = !lp;
    } break;
    case ST_SIT:
        p.sink = 1;
        if (t % 90 >= 60) p.bh = 7;
        break;
    case ST_SLEEP:
        p.sink = 2; p.bw = 13; p.bh = (t % 100 < 50) ? 7 : 6;
        p.eyes = E_BLINK; p.ex = 0; p.ey = 0; allowBlink = 0;
        break;
    case ST_HOP: case ST_DONE: {
        int u = t % 30;
        if (u < 4) { p.bw = 14; p.bh = 6; }
        else if (u < 22) { double s = (u - 4) / 18.0; p.lift = (int)lround(7 * sin(M_PI * s));
                           p.arm[0] = p.arm[1] = A_UP; if (u > 8 && u < 18) { p.bw = 11; p.bh = 9; } }
        else if (u < 26) { p.bw = 13; p.bh = 7; }
        p.eyes = E_HAPPY; p.ex = 0; p.ey = 0; allowBlink = 0;
        if (in->st == ST_DONE) { p.arm[0] = p.arm[1] = A_UP; }
    } break;
    case ST_DANCE: {
        int beat = (t / 8) & 1;
        p.dx = beat ? 1 : -1;
        p.arm[0] = beat ? A_UP : A_REST; p.arm[1] = beat ? A_REST : A_UP;
        int a[4] = {2, 1, 2, 1}, b[4] = {1, 2, 1, 2};
        memcpy(p.leg, beat ? a : b, sizeof a);
        if ((t / 4) % 4 == 1) p.bh = 7;
        p.eyes = E_HAPPY; p.ex = 0; p.ey = 0; allowBlink = 0;
    } break;
    case ST_YAWN:
        if (t < 45) { p.bw = 11; p.bh = 9; p.arm[0] = p.arm[1] = (t < 10) ? A_UP : A_WAVE2; p.eyes = E_BLINK; }
        else { p.eyes = E_BLINK; }
        p.ex = 0; p.ey = 0; allowBlink = 0;
        break;
    case ST_TRIP:
        if (t < 8)       { p.dx = f; p.eyes = E_WIDE; p.arm[0] = p.arm[1] = A_OUT; }
        else if (t < 12) { p.dx = 2 * f; p.sink = 1; p.eyes = E_WIDE; p.arm[0] = p.arm[1] = A_UP; }
        else if (t < 70) { p.dx = 2 * f; p.sink = 2; p.bw = 13; p.bh = 6; p.eyes = E_DIZZY; p.arm[0] = p.arm[1] = A_OUT; }
        else if (t < 85) { p.dx = f; p.sink = 1; p.eyes = E_BLINK; }
        else             { p.dx = ((t / 3) & 1) ? 1 : -1; p.eyes = E_SQUEEZE; }
        p.ex = 0; p.ey = 0; allowBlink = 0;
        break;
    case ST_WAVE: case ST_HELLO: {
        int side = f > 0 ? 1 : 0;
        p.arm[side] = ((t / 6) & 1) ? A_UP : A_WAVE2;
        p.eyes = E_HAPPY; p.ex = 0; p.ey = 0; allowBlink = 0;
    } break;
    case ST_LOVE:
        p.lift = (t % 20 < 10) ? 1 : 0;
        p.arm[0] = p.arm[1] = A_UP;
        p.eyes = (t / 15) & 1 ? E_HEART : E_HAPPY; p.ex = 0; p.ey = 0; allowBlink = 0;
        break;
    case ST_DIZZY:
        p.dx = ((t / 10) & 1) ? 1 : -1;
        p.eyes = E_DIZZY; p.ex = 0; p.ey = 0; allowBlink = 0;
        p.arm[0] = p.arm[1] = A_OUT;
        break;
    case ST_GRUMPY:
        p.eyes = E_GRUMPY; p.ex = 0; p.ey = 0; allowBlink = 0;
        p.arm[0] = p.arm[1] = A_HIDE;
        break;
    case ST_DRAG: {
        int k = (t / 4) & 1;
        int a[4] = {2, 1, 2, 1}, b[4] = {1, 2, 1, 2};
        memcpy(p.leg, k ? a : b, sizeof a);
        p.bw = 11; p.bh = 9;
        p.arm[0] = p.arm[1] = ((t / 5) & 1) ? A_UP : A_WAVE2;
        p.eyes = E_WIDE; p.ex = 0; p.ey = 0; allowBlink = 0;
    } break;
    case ST_FALL:
        p.arm[0] = p.arm[1] = A_UP; p.eyes = E_WIDE; p.ex = 0; p.ey = in->vy > 0 ? 1 : -1; allowBlink = 0;
        if (in->vy > 0) { p.bw = 11; p.bh = 9; }
        break;
    case ST_LAND:
        if (t < 3) { p.bw = 14; p.bh = 6; p.eyes = E_BLINK; } else { p.bw = 13; p.bh = 7; }
        p.arm[0] = p.arm[1] = A_OUT; p.ex = 0; p.ey = 0; allowBlink = 0;
        break;
    case ST_THINK:
        p.ex = 1; p.ey = -1;
        if (t % 80 >= 60) p.ex = 0;
        break;
    case ST_READ:
        p.sink = 1; p.arm[0] = p.arm[1] = A_HOLD; p.ey = 1;
        p.ex = ((t / 24) % 3) - 1;
        break;
    case ST_SEARCH:
        p.arm[1] = A_UP; p.ey = 0;
        break;
    case ST_WRITE:
        p.arm[1] = A_HOLD; p.arm[0] = A_HOLD; p.ey = 1;
        p.ex = (((t / 2) % 44) % 9 > 4) ? 1 : 0;
        break;
    case ST_TERM:
        p.arm[0] = p.arm[1] = A_FRONT;
        p.armdy[0] = ((t / 3) & 1); p.armdy[1] = !((t / 3) & 1);
        p.ey = 1; p.ex = ((t / 40) & 1) ? 1 : 0;
        break;
    case ST_AGENT: {
        int k = (t / 15) & 1;
        p.arm[0] = k ? A_UP : A_REST; p.arm[1] = k ? A_REST : A_UP;
        p.ex = k ? -1 : 1; p.ey = 0;
    } break;
    case ST_TOOL:
        p.arm[0] = p.arm[1] = A_FRONT; p.armdy[0] = ((t / 8) & 1); p.ey = -1; p.ex = 0;
        break;
    case ST_ATTN:
        p.lift = (t % 16 < 8) ? 2 : 0;
        p.arm[0] = p.arm[1] = ((t / 5) & 1) ? A_UP : A_WAVE2;
        p.eyes = E_WIDE; p.ex = 0; p.ey = 0; allowBlink = 0;
        break;
    case ST_OOPS:
        p.dx = (t < 18) ? (((t / 2) & 1) ? 1 : 0) : 0;
        p.eyes = E_SQUEEZE; p.ex = 0; p.ey = 0; allowBlink = 0;
        p.arm[0] = p.arm[1] = A_OUT;
        break;
    }
    if (allowBlink && in->blink && p.eyes == E_OPEN) p.eyes = E_BLINK;

    /* ---- geometry ---- */
    Geo G;
    G.bw = p.bw; G.bh = p.bh;
    G.bottom = CH - 3 + p.sink - p.lift;
    G.top = G.bottom - p.bh + 1;
    G.x0 = CW / 2 - p.bw / 2 + p.dx;
    G.cx = G.x0 + p.bw / 2;

    /* legs */
    int lx[4] = { G.x0 + 1, G.x0 + 3, G.x0 + p.bw - 4, G.x0 + p.bw - 2 };
    for (int i = 0; i < 4; i++) {
        int len = p.leg[i];
        if (!p.lift) { int room = 2 - p.sink; if (len > room) len = room; }
        if (len > 0) pd_rect(lx[i], G.bottom + 1, 1, len, C_OR);
    }
    /* back-side arms, body */
    pd_arm(&G, 0, p.arm[0], p.armdy[0], 0);
    pd_arm(&G, 1, p.arm[1], p.armdy[1], 0);
    pd_rect(G.x0, G.top, p.bw, p.bh, C_OR);
    pd_rect(G.x0, G.bottom, p.bw, 1, C_ORD);
    /* eyes */
    int eyY = G.top + (p.bh >= 8 ? 2 : 1) + p.ey;
    if (eyY < G.top) eyY = G.top;
    if (eyY > G.bottom - 2) eyY = G.bottom - 2;
    int lex = G.x0 + p.bw / 2 - 3 + p.ex, rex = G.x0 + p.bw / 2 + 1 + p.ex;
    if (p.eyes == E_HAPPY) { pd_eye(lex - 1, eyY, p.eyes, 0); pd_eye(rex, eyY, p.eyes, 1); }
    else if (p.eyes == E_DIZZY || p.eyes == E_HEART) { pd_eye(lex, eyY, p.eyes, 0); pd_eye(rex + 1, eyY, p.eyes, 1); }
    else { pd_eye(lex, eyY, p.eyes, 0); pd_eye(rex, eyY, p.eyes, 1); }

    /* ---- props ---- */
    int cx = G.cx;
    switch (in->st) {
    case ST_SLEEP:
        for (int k = 0; k < 3; k++) {
            int ph = (t + k * 40) % 120;
            if (ph < 96) pd_glyph(G.x0 + p.bw + 1 + ph / 24, G.top - 2 - ph / 10, G_Z, 3, ph < 72 ? C_GRD : C_GR);
        }
        break;
    case ST_DANCE:
        for (int k = 0; k < 2; k++) {
            int ph = (t + k * 30) % 60;
            int nx = k ? G.x0 + p.bw + 2 : G.x0 - 6;
            pd_glyph(nx + (ph / 15 & 1), G.top - 3 - ph / 8, G_NOTE, 3, C_GRD);
        }
        break;
    case ST_TRIP:
        if (t >= 12 && t < 24) {
            int d = (t - 12) / 3;
            pd_px(G.x0 - 2 - d, G.bottom + 1 - d / 2, C_PUFF); pd_px(G.x0 + p.bw + 1 + d, G.bottom + 1 - d / 2, C_PUFF);
        }
        if (t >= 14 && t < 70) {
            for (int k = 0; k < 2; k++) {
                double a = t * 0.18 + k * M_PI;
                pd_px(cx + (int)lround(5 * cos(a)), G.top - 2 + (int)lround(1.2 * sin(a)), C_YEL);
            }
        }
        break;
    case ST_LOVE:
        for (int k = 0; k < 3; k++) {
            int ph = (t + k * 14) % 42;
            if (ph < 38) pd_glyph(cx - 3 + (k - 1) * 7 + (int)lround(sin(ph * 0.3)), G.top - 3 - ph / 3, G_HEART, 5, C_RED);
        }
        break;
    case ST_DIZZY:
        for (int k = 0; k < 3; k++) {
            double a = t * 0.15 + k * 2.094;
            int sx = cx + (int)lround(6 * cos(a)), sy = G.top - 3 + (int)lround(1.5 * sin(a));
            pd_sparkle(sx, sy, sin(a) > 0);
        }
        break;
    case ST_GRUMPY: {
        int ph = t % 30;
        pd_rect(G.x0 - 1, G.top + 4, p.bw + 2, 2, C_OR);
        pd_rect(G.x0 + 2, G.top + 4, p.bw - 4, 2, C_ORD);
        pd_rect(G.x0 + p.bw + ph / 10, G.top - 2 - ph / 6, 2, 2, C_PUFF);
    } break;
    case ST_THINK: {
        int bx = cx - 2, by = G.top - 11;
        pd_bubble(bx, by, 15, 7, -1);
        pd_px(cx + 1, G.top - 2, C_OL);
        pd_rect(cx + 2, G.top - 4, 2, 1, C_OL);
        int n = (t / 12) % 4;
        for (int k = 0; k < n; k++) pd_rect(bx + 3 + k * 4, by + 3, 2, 2, C_BK);
    } break;
    case ST_READ: {
        int y = G.top + 4;
        pd_rect(cx - 5, y, 11, 5, C_BRN);
        pd_rect(cx - 4, y, 4, 4, C_PAP); pd_rect(cx + 1, y, 4, 4, C_PAP);
        pd_rect(cx - 4, y + 1, 3, 1, C_LN); pd_rect(cx + 1, y + 1, 3, 1, C_LN);
        pd_rect(cx - 4, y + 3, 2, 1, C_LN); pd_rect(cx + 1, y + 3, 3, 1, C_LN);
        int ph = t % 90;
        if (ph >= 78) { int k = ph - 78; pd_rect(cx + 1 - k / 3, y - 1 - (k < 6 ? k / 2 : (12 - k) / 2), 3, 2, C_PAP); }
    } break;
    case ST_SEARCH: {
        int lcx = cx + (int)lround(5 * sin(t * 0.07));
        int lcy = eyY + 1 + (int)lround(sin(t * 0.14));
        int x = lcx - 2, y = lcy - 2;
        pd_rect(x + 1, y + 1, 3, 3, C_GLASS);
        /* magnified eye under the glass */
        if (abs(lcx - (lex + 1)) <= 1 || abs(lcx - (rex + 1)) <= 1) pd_rect(lcx - 1, lcy - 1, 3, 3, C_BK);
        pd_px(x + 1, y + 1, C_WH);
        pd_rect(x + 1, y, 3, 1, C_GRD); pd_rect(x + 1, y + 4, 3, 1, C_GRD);
        pd_rect(x, y + 1, 1, 3, C_GRD); pd_rect(x + 4, y + 1, 1, 3, C_GRD);
        pd_px(x + 5, y + 5, C_BRN); pd_px(x + 6, y + 6, C_BRN); pd_px(x + 5, y + 6, C_BRN);
        if ((t / 45) % 4 == 3) pd_glyph(G.x0 + p.bw + 2, G.top - 6, G_Q, 3, C_GRD);
    } break;
    case ST_WRITE: {
        int x = cx - 5, y = G.top + 5;
        pd_rect(x, y, 10, 6, C_PAP);
        pd_rect(x, y + 5, 10, 1, C_LN);
        int pr = (t / 2) % 44, hx = x + 1, hy = y + 1;
        for (int k = 0; k < 3; k++) {
            int len = pr - k * 9; if (len > 8) len = 8;
            if (len > 0) { pd_rect(x + 1, y + 1 + k * 2, len, 1, C_GRD); hx = x + 1 + len; hy = y + 1 + k * 2; }
        }
        hy -= ((t / 2) & 1);
        pd_px(hx, hy, C_BK); pd_px(hx + 1, hy - 1, C_YEL); pd_px(hx + 2, hy - 2, C_YEL);
        pd_px(hx + 3, hy - 3, C_YEL); pd_px(hx + 4, hy - 4, C_PNK);
    } break;
    case ST_TERM: {
        int kx = cx - 8, ky = CH - 4;
        pd_rect(kx, ky, 16, 3, C_GRD);
        for (int i = 0; i < 7; i++) { pd_px(kx + 1 + i * 2, ky + 1, C_GR); pd_px(kx + 2 + i * 2, ky + 2, C_GR); }
        int key = ((t / 3) * 7) % 7;
        pd_px(kx + 1 + key * 2, ky + 1, C_WH);
        int bx = G.x0 + p.bw + 1, by = G.top - 9;
        pd_bubble(bx, by, 8, 7, bx + 1);
        pd_glyph(bx + 2, by + 2, "#." ".#" "#.", 2, C_GRD);
        if ((t / 10) & 1) pd_rect(bx + 4, by + 4, 2, 1, C_GRD);
    } break;
    case ST_AGENT: {
        int hA = (int)lround(3 * fabs(sin(t * 0.12)));
        int hB = (int)lround(3 * fabs(sin(t * 0.12 + 1.6)));
        pd_mini(3, CH - 3, hA, (in->g / 7) % 23 == 0);
        pd_mini(31, CH - 3, hB, (in->g / 7) % 29 == 0);
        if ((t / 30) % 3 == 0) pd_glyph(cx - 1, G.top - 7, G_EX, 3, C_ORD);
    } break;
    case ST_TOOL: {
        int gx = cx, gy = G.top - 6, r = (t / 6) & 1;
        pd_rect(gx - 2, gy - 2, 5, 5, C_GRD);
        pd_cv[(gy - 2) * CW + gx - 2] = 0; pd_cv[(gy - 2) * CW + gx + 2] = 0;
        pd_cv[(gy + 2) * CW + gx - 2] = 0; pd_cv[(gy + 2) * CW + gx + 2] = 0;
        pd_px(gx, gy, C_GR);
        if (r) { pd_px(gx, gy - 3, C_GRD); pd_px(gx, gy + 3, C_GRD); pd_px(gx - 3, gy, C_GRD); pd_px(gx + 3, gy, C_GRD); }
        else   { pd_px(gx - 3, gy - 3, C_GRD); pd_px(gx + 3, gy - 3, C_GRD); pd_px(gx - 3, gy + 3, C_GRD); pd_px(gx + 3, gy + 3, C_GRD); }
        if ((t / 4) % 6 == 0) pd_px(gx + 4, gy - 4, C_YEL);
    } break;
    case ST_ATTN: {
        int bx = cx - 3, by = G.top - 11;
        pd_bubble(bx, by, 7, 9, cx - 1);
        pd_glyph(bx + 2, by + 2, G_EX, 3, ((t / 8) & 1) ? C_RED : C_ORD);
    } break;
    case ST_DONE: {
        static const int sp[5][2] = { {-9, -3}, {9, -5}, {-6, -9}, {7, 0}, {1, -12} };
        for (int k = 0; k < 5; k++) {
            int ph = (t + k * 7) % 20;
            if (ph < 14) pd_sparkle(cx + sp[k][0], G.top + sp[k][1], ph < 7);
        }
    } break;
    case ST_OOPS: {
        int dy = (t / 5) % 5;
        int x = G.x0 + p.bw, y = G.top - 1 + dy;
        pd_px(x + 1, y, C_BLU); pd_rect(x, y + 1, 2, 2, C_BLU); pd_px(x, y + 1, C_WH);
    } break;
    case ST_HELLO: {
        int side = f > 0;
        int hx = side ? G.x0 + p.bw + 3 : G.x0 - 4;
        if ((t / 6) & 1) pd_sparkle(hx, G.top - 3, (t / 12) & 1);
    } break;
    default: break;
    }
    /* front arms last so hands sit on top of held props */
    pd_arm(&G, 0, p.arm[0], p.armdy[0], 1);
    pd_arm(&G, 1, p.arm[1], p.armdy[1], 1);
}

#endif
