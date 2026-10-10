/* Claude Pet - a tiny pixel-art desktop pet for Windows.
 * Authors: Jeffrey Landgrave, Claude (Anthropic)
 * License: MIT (see LICENSE)
 *
 *   ClaudePet.exe                 run the pet (single instance)
 *   ClaudePet.exe --hook          Claude Code hook: reads hook JSON on stdin, tells the pet
 *   ClaudePet.exe --event EV [TOOL]   send an event by hand (testing)
 *   ClaudePet.exe --always        always visible, even without the Claude window
 *   ClaudePet.exe --quit          close a running pet
 *
 * No frameworks: one layered Win32 window, ~30 fps timer only while visible. */
#define WIN32_LEAN_AND_MEAN
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <dwmapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <limits.h>
#include "pet_draw.h"

#define WND_CLASS   L"ClaudePetWnd"
#define MUTEX_NAME  L"Local\\ClaudePet.Singleton"
#define REG_KEY     L"Software\\ClaudePet"
#define RUN_KEY     L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define COPY_MAGIC  0x50455421u
#define WM_TRAY     (WM_APP + 1)
#define TIMER_ANIM  1
#define TIMER_POLL  2

enum { ID_ALWAYS = 100, ID_GUESS, ID_STARTUP, ID_NAP, ID_WAKE, ID_HIDE, ID_QUIT,
       ID_SIZE0 = 200 /* +scale */ };

/* ------------------------------------------------------------------ state */
static HINSTANCE g_inst;
static HWND g_hwnd;
static UINT g_taskbarMsg;
static NOTIFYICONDATAW g_nid;
static HICON g_icon;

static int S = 4, W, H;                 /* pixel scale, window size */
static double px, py, vx, vy;           /* window top-left (screen px), velocity px/frame */
static int onGround = 1, landT = 0;
static void react(int st, int dur);
static int facing = 1;

static int dragging = 0, dragMoved = 0;
static POINT grab, downPt;
static DWORD downTick;
static double histX[6], histY[6]; static int histN = 0;

static int autoSt = ST_IDLE, autoT = 0, autoDur = 90;
static int walkTarget, walkPhase, walkAcc;
static int reactSt = -1, reactT = 0, reactDur = 0;     /* short reactions to clicks */
static int mode = -1, modeT = 0, gframe = 0;
static int blinkLeft = 0, nextBlink = 90;
static int lookX, lookY, lookHold = 0;
static double energy = 1.0, happy = 0.6;
static int clickCount = 0; static DWORD clickStart = 0;

static int activity = 0, pendingAct = 0, actFromGuess = 0;
static DWORD actStart, actLast, lastHook = 0;

static int cfgAlways = 0, cfgGuess = 1, cfgScale = 0;  /* 0 = auto from DPI */
static int forceAlways = 0, userHidden = 0;
static int shown = 0, claudeOpen = 0;

static uint32_t canvas[CW * CH], prevCanvas[CW * CH];
static HDC memDC; static HBITMAP dib, oldBmp; static uint32_t *dibBits;
static int lastX = INT_MIN, lastY = INT_MIN, needFull = 1;

static unsigned rng = 0x12345678u;
static unsigned rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static int rr(int a, int b) { return a + (int)(rnd() % (unsigned)(b - a + 1)); }
static double rf(void) { return (rnd() & 0xFFFFFF) / (double)0x1000000; }

/* ------------------------------------------------------------- settings */
static DWORD reg_get(const wchar_t *name, DWORD def) {
    DWORD v = def, sz = sizeof v;
    if (RegGetValueW(HKEY_CURRENT_USER, REG_KEY, name, RRF_RT_REG_DWORD, NULL, &v, &sz) != ERROR_SUCCESS) v = def;
    return v;
}
static void reg_set(const wchar_t *name, DWORD v) {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL) == ERROR_SUCCESS) {
        RegSetValueExW(k, name, 0, REG_DWORD, (BYTE *)&v, sizeof v); RegCloseKey(k);
    }
}
static int startup_enabled(void) {
    wchar_t buf[MAX_PATH * 2]; DWORD sz = sizeof buf;
    return RegGetValueW(HKEY_CURRENT_USER, RUN_KEY, L"ClaudePet", RRF_RT_REG_SZ, NULL, buf, &sz) == ERROR_SUCCESS;
}
static void startup_set(int on) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    if (on) {
        wchar_t exe[MAX_PATH], cmd[MAX_PATH + 4];
        GetModuleFileNameW(NULL, exe, MAX_PATH);
        swprintf(cmd, MAX_PATH + 4, L"\"%ls\"", exe);
        RegSetValueExW(k, L"ClaudePet", 0, REG_SZ, (BYTE *)cmd, (DWORD)((wcslen(cmd) + 1) * sizeof(wchar_t)));
    } else RegDeleteValueW(k, L"ClaudePet");
    RegCloseKey(k);
}

/* -------------------------------------------------------------- helpers */
static RECT work_area(void) {
    POINT c = { (LONG)(px + W / 2), (LONG)(py + H - 1) };
    MONITORINFO mi = { sizeof mi };
    GetMonitorInfoW(MonitorFromPoint(c, MONITOR_DEFAULTTONEAREST), &mi);
    return mi.rcWork;
}
static double padX(void) { return (CW / 2 - 8) * (double)S; }      /* transparent margin beside arms */
static double padTop(void) { return (CH - 10) * (double)S; }

static int dpi_scale(void) {
    HDC dc = GetDC(NULL); int dpi = GetDeviceCaps(dc, LOGPIXELSX); ReleaseDC(NULL, dc);
    int s = (int)lround(4.0 * dpi / 96.0); return s < 2 ? 2 : s;
}

/* ---------------------------------------------------------- the "mind" */
static void set_auto(int st, int dur) { autoSt = st; autoT = 0; autoDur = dur; }

/* ------------------------------------------------- open windows as platforms */
typedef struct { HWND h; RECT r; } Plat;
static Plat plats[160]; static int platN = 0, platRefresh = 0;
static HWND standOn = NULL, ignorePlat = NULL; static RECT standR;
static int jumping = 0, jumpPrep = 0; static double jumpVx, jumpVy;
static DWORD selfPid;

static int win_rect(HWND h, RECT *r) {
    if (!IsWindow(h) || !IsWindowVisible(h) || IsIconic(h)) return 0;
    if (FAILED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, r, sizeof *r)) && !GetWindowRect(h, r)) return 0;
    return r->right > r->left && r->bottom > r->top;
}
static int plat_candidate(HWND h, RECT *r) {
    if (!IsWindowVisible(h) || IsIconic(h)) return 0;
    LONG ex = GetWindowLongW(h, GWL_EXSTYLE);
    if (ex & (WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE)) return 0;   /* overlays, tooltips */
    DWORD pid; GetWindowThreadProcessId(h, &pid); if (pid == selfPid) return 0;
    BOOL cloaked = FALSE;   /* windows on other virtual desktops / suspended UWP */
    if (SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof cloaked)) && cloaked) return 0;
    wchar_t cls[64]; if (!GetClassNameW(h, cls, 64)) return 0;
    if (!wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW") || !wcscmp(cls, L"Shell_TrayWnd") ||
        !wcscmp(cls, L"Shell_SecondaryTrayWnd")) return 0;
    if (!win_rect(h, r)) return 0;
    return r->right - r->left >= 120 && r->bottom - r->top >= 60;
}
static BOOL CALLBACK plat_cb(HWND h, LPARAM lp) {
    (void)lp;
    if (platN >= 160) return FALSE;
    RECT r; if (plat_candidate(h, &r)) { plats[platN].h = h; plats[platN].r = r; platN++; }
    return TRUE;
}
static void refresh_plats(void) { platN = 0; EnumWindows(plat_cb, 0); }   /* top of z-order first */
static int plat_index(HWND h) { for (int i = 0; i < platN; i++) if (plats[i].h == h) return i; return -1; }

/* is window idx's top edge visible at screen x, i.e. not under a window above it? */
static int edge_open(int idx, double x, LONG t) {
    for (int j = 0; j < idx; j++) {
        const RECT *o = &plats[j].r;
        if (x >= o->left && x < o->right && t >= o->top - 2 && t < o->bottom) return 0;
    }
    return 1;
}
/* standing there must leave the pet on screen and above the taskbar */
static int body_fits(double x, LONG t) {
    POINT p = { (LONG)x, t };
    MONITORINFO mi = { sizeof mi };
    GetMonitorInfoW(MonitorFromPoint(p, MONITOR_DEFAULTTONEAREST), &mi);
    return t - 12 * S >= mi.rcMonitor.top && t < mi.rcWork.bottom - 4 * S;
}
static int on_plat(int i, double fx) {
    const RECT *r = &plats[i].r;
    return fx >= r->left - 3.0 * S && fx <= r->right + 3.0 * S && edge_open(i, fx, r->top) && body_fits(fx, r->top);
}
static void fall_off(void) {
    int walking = autoSt == ST_WALK && !activity && reactSt < 0;
    onGround = 0; standOn = NULL; vy = 0; jumping = 0;
    vx = walking ? facing * 0.33 * S : 0;
    if (autoSt == ST_WALK) set_auto(ST_IDLE, 40);
}
static void start_jump(double jvx, double jvy) { jumpVx = jvx; jumpVy = jvy; jumpPrep = 7; }

/* leap from the taskbar (or a window) up onto a window top in reach */
static int try_jump_up(void) {
    refresh_plats();
    double fx = px + W / 2.0, fy = py + H, g = 0.22 * S;
    int cand[24], cx[24], n = 0;
    for (int i = 0; i < platN && n < 24; i++) {
        const RECT *r = &plats[i].r;
        double h = fy - r->top;
        if (plats[i].h == standOn || h < 12 * S || h > 80 * S) continue;
        int a = (int)fmax(r->left + 10.0 * S, fx - 140.0 * S), b = (int)fmin(r->right - 10.0 * S, fx + 140.0 * S);
        if (a > b) continue;
        int x = rr(a, b);
        if (!on_plat(i, x)) continue;
        cand[n] = i; cx[n] = x; n++;
    }
    if (!n) return 0;
    int k = rr(0, n - 1); const RECT *r = &plats[cand[k]].r;
    double h = fy - r->top, apex = 8.0 * S;
    double vy0 = -sqrt(2 * g * (h + apex)), T = -vy0 / g + sqrt(2 * apex / g);
    facing = cx[k] >= fx ? 1 : -1;
    start_jump((cx[k] - fx) / T, vy0);
    return 1;
}
static void jump_down(void) {
    double fx = px + W / 2.0, g = 0.22 * S, vy0 = -3.0 * S, T = 2 * -vy0 / g;
    int right = (standR.right - fx) < (fx - standR.left);
    if (rr(0, 3) == 0) right = !right;
    double d = right ? (standR.right - fx) + 14.0 * S : -((fx - standR.left) + 14.0 * S);
    facing = right ? 1 : -1;
    ignorePlat = standOn;
    start_jump(d / T, vy0);
}

static void choose_next(void) {
    RECT wa = work_area();
    if (energy < 0.18) { set_auto(rr(0, 2) ? ST_YAWN : ST_SLEEP, 60); if (autoSt == ST_SLEEP) autoDur = rr(900, 1800); return; }
    if (onGround && energy > 0.3) {
        if (!standOn && rf() < 0.14 && try_jump_up()) { set_auto(ST_IDLE, 60); return; }
        if (standOn && rf() < 0.08) { jump_down(); set_auto(ST_IDLE, 60); return; }
        if (standOn && rf() < 0.05 && try_jump_up()) { set_auto(ST_IDLE, 60); return; }   /* window to higher window */
    }
    double w[ST_COUNT] = {0};
    w[ST_IDLE] = 3; w[ST_WALK] = 4; w[ST_SIT] = 2;
    w[ST_HOP] = 1.2 * happy; w[ST_DANCE] = happy > 0.65 ? 0.8 : 0.1;
    w[ST_YAWN] = energy < 0.5 ? 1.0 : 0.1; w[ST_TRIP] = 0.12; w[ST_WAVE] = 0.35;
    w[ST_SLEEP] = energy < 0.35 ? 1.5 : 0;
    if (autoSt != ST_IDLE) w[autoSt] *= 0.4;          /* variety */
    double tot = 0; for (int i = 0; i < ST_COUNT; i++) tot += w[i];
    double r = rf() * tot; int st = ST_IDLE;
    for (int i = 0; i < ST_COUNT; i++) { if ((r -= w[i]) <= 0) { st = i; break; } }
    switch (st) {
    case ST_WALK: {
        double lo = wa.left - padX(), hi = wa.right - W + padX(), tgt;
        if (standOn && rf() < 0.2) {            /* wander right off the edge */
            tgt = (rr(0, 1) ? standR.left - 14.0 * S : standR.right + 14.0 * S) - W / 2.0;
        } else {
            if (standOn) {
                lo = fmax(lo, standR.left + 6.0 * S - W / 2.0);
                hi = fmin(hi, standR.right - 6.0 * S - W / 2.0);
                if (hi <= lo) { set_auto(ST_SIT, rr(150, 300)); break; }
            }
            int dist = rr(80, 520) * (rr(0, 1) ? 1 : -1);
            tgt = px + dist; if (tgt < lo) tgt = px + abs(dist); if (tgt > hi) tgt = px - abs(dist);
        }
        if (tgt < wa.left - padX()) tgt = wa.left - padX(); if (tgt > wa.right - W + padX()) tgt = wa.right - W + padX();
        if (!standOn) { if (tgt < lo) tgt = lo; if (tgt > hi) tgt = hi; }
        walkTarget = (int)tgt; facing = tgt >= px ? 1 : -1;
        set_auto(ST_WALK, 99999);
    } break;
    case ST_IDLE:  set_auto(st, rr(60, 200)); break;
    case ST_SIT:   set_auto(st, rr(150, 420)); break;
    case ST_HOP:   set_auto(st, 30 * rr(2, 3)); break;
    case ST_DANCE: set_auto(st, rr(120, 240)); break;
    case ST_YAWN:  set_auto(st, 70); break;
    case ST_TRIP:  set_auto(st, 100); break;
    case ST_WAVE:  set_auto(st, 60); break;
    case ST_SLEEP: set_auto(st, rr(900, 1800)); break;
    default:       set_auto(ST_IDLE, 90);
    }
}

static void react(int st, int dur) { reactSt = st; reactT = 0; reactDur = dur; }

static void on_click(void) {
    DWORD now = GetTickCount();
    if (now - clickStart > 2500) { clickStart = now; clickCount = 0; }
    clickCount++;
    if (autoSt == ST_SLEEP && !activity) { energy += 0.3; react(ST_GRUMPY, 60); set_auto(ST_IDLE, 90); return; }
    if (clickCount >= 5) { react(ST_DIZZY, 75); clickCount = 0; happy -= 0.05; return; }
    happy += 0.08; if (happy > 1) happy = 1;
    react(ST_LOVE, 50);
}

/* ------------------------------------------------- Claude activity input */
static int tool_state(const char *tool) {
    static const struct { const char *n; int st; } map[] = {
        {"Read", ST_READ}, {"NotebookRead", ST_READ}, {"LS", ST_READ}, {"View", ST_READ},
        {"Grep", ST_SEARCH}, {"Glob", ST_SEARCH}, {"WebSearch", ST_SEARCH}, {"WebFetch", ST_SEARCH}, {"ToolSearch", ST_SEARCH},
        {"Write", ST_WRITE}, {"Edit", ST_WRITE}, {"MultiEdit", ST_WRITE}, {"NotebookEdit", ST_WRITE},
        {"TodoWrite", ST_WRITE}, {"TaskCreate", ST_WRITE}, {"TaskUpdate", ST_WRITE},
        {"Bash", ST_TERM}, {"PowerShell", ST_TERM}, {"BashOutput", ST_TERM}, {"KillShell", ST_TERM}, {"KillBash", ST_TERM},
        {"Task", ST_AGENT}, {"Agent", ST_AGENT},
    };
    for (size_t i = 0; i < sizeof map / sizeof *map; i++) if (!strcmp(tool, map[i].n)) return map[i].st;
    return ST_TOOL;
}
static int is_transient(int st) { return st == ST_DONE || st == ST_HELLO || st == ST_OOPS; }

static void set_activity(int st) {
    DWORD now = GetTickCount();
    actLast = now;
    if (st == activity) return;
    if (activity && !is_transient(activity) && !is_transient(st) && st != ST_ATTN && now - actStart < 1400) {
        pendingAct = st; return;                       /* let the current animation play a moment */
    }
    activity = st; pendingAct = 0; actStart = now;
    if (st && autoSt == ST_SLEEP) { set_auto(ST_IDLE, 60); energy += 0.25; }
}

static void handle_event(const char *ev, const char *tool) {
    int st = -1;
    if      (!strcmp(ev, "SessionStart"))       st = ST_HELLO;
    else if (!strcmp(ev, "UserPromptSubmit"))   st = ST_THINK;
    else if (!strcmp(ev, "PreToolUse"))         st = tool_state(tool);
    else if (!strcmp(ev, "PostToolUse"))        st = ST_THINK;
    else if (!strcmp(ev, "PostToolUseFailure")) st = ST_OOPS;
    else if (!strcmp(ev, "Notification") || !strcmp(ev, "PermissionRequest")) st = ST_ATTN;
    else if (!strcmp(ev, "PreCompact"))         st = ST_TOOL;
    else if (!strcmp(ev, "Stop"))               st = ST_DONE;
    else if (!strcmp(ev, "SessionEnd"))         st = ST_HELLO;
    else {  /* direct state names, for --event testing */
        for (int i = 0; i < ST_COUNT; i++) if (!strcmp(ev, ST_NAMES[i])) {
            if (i >= ST_THINK) st = i; else { set_auto(i, i == ST_SLEEP ? 900 : 150); return; }
        }
    }
    if (st < 0) return;
    lastHook = GetTickCount(); actFromGuess = 0;
    set_activity(st);
}

static void activity_tick(void) {
    if (!activity) return;
    DWORD now = GetTickCount(), age = now - actStart, idle = now - actLast;
    if (pendingAct && age >= 1400) { activity = pendingAct; pendingAct = 0; actStart = now; return; }
    switch (activity) {
    case ST_DONE:  if (age > 2600) { activity = 0; happy += 0.05; set_auto(ST_IDLE, 60); } break;
    case ST_HELLO: if (age > 2200) activity = 0; break;
    case ST_OOPS:  if (age > 2000) { activity = ST_THINK; actStart = now; } break;
    case ST_ATTN:  if (idle > 30000) activity = 0; break;
    default:       if (idle > 120000 && !actFromGuess) activity = 0; break;
    }
}

/* ---------------------------------------------------- Claude window watch */
typedef struct { DWORD pid; int isClaude; } PidInfo;
static PidInfo pidCache[64]; static int pidN = 0;

static int pid_is_claude(DWORD pid) {
    for (int i = 0; i < pidN; i++) if (pidCache[i].pid == pid) return pidCache[i].isClaude;
    int is = 0;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (h) {
        wchar_t path[MAX_PATH]; DWORD n = MAX_PATH;
        if (QueryFullProcessImageNameW(h, 0, path, &n)) {
            const wchar_t *b = wcsrchr(path, L'\\'); b = b ? b + 1 : path;
            is = !_wcsicmp(b, L"claude.exe");
        }
        CloseHandle(h);
    }
    if (pidN == 64) pidN = 0;          /* crude refresh */
    pidCache[pidN].pid = pid; pidCache[pidN].isClaude = is; pidN++;
    return is;
}

static BOOL CALLBACK enum_cb(HWND h, LPARAM lp) {
    if (!IsWindowVisible(h) || GetWindow(h, GW_OWNER)) return TRUE;
    if (GetWindowLongW(h, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return TRUE;
    wchar_t cls[64];
    if (!GetClassNameW(h, cls, 64) || wcscmp(cls, L"Chrome_WidgetWin_1")) return TRUE;
    DWORD pid; GetWindowThreadProcessId(h, &pid);
    if (!pid_is_claude(pid)) return TRUE;
    if (!IsIconic(h)) { *(int *)lp = 1; return FALSE; }
    return TRUE;
}
static int claude_window_open(void) { int found = 0; EnumWindows(enum_cb, (LPARAM)&found); return found; }

/* CPU-based guess of whether the Claude app is busy (for chats whose hooks don't run on this PC) */
static HANDLE cpuH[32]; static int cpuN = 0, cpuRefresh = 0;
static ULONGLONG cpuPrev = 0, wallPrev = 0;
static double cpuBase = 100, cpuEma = 0; static int busyRun = 0, calmRun = 0, guessBusy = 0;

static void cpu_refresh_handles(void) {
    for (int i = 0; i < cpuN; i++) CloseHandle(cpuH[i]);
    cpuN = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W pe = { sizeof pe };
    for (BOOL ok = Process32FirstW(snap, &pe); ok && cpuN < 32; ok = Process32NextW(snap, &pe))
        if (!_wcsicmp(pe.szExeFile, L"claude.exe")) {
            HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
            if (h) cpuH[cpuN++] = h;
        }
    CloseHandle(snap);
}
static void guess_tick(void) {
    if (!cfgGuess || !claudeOpen) { busyRun = calmRun = 0; return; }
    if (cpuRefresh-- <= 0) { cpu_refresh_handles(); cpuRefresh = 12; cpuPrev = 0; }
    ULONGLONG tot = 0;
    for (int i = 0; i < cpuN; i++) {
        FILETIME c, e, k, u;
        if (GetProcessTimes(cpuH[i], &c, &e, &k, &u))
            tot += ((ULONGLONG)k.dwHighDateTime << 32 | k.dwLowDateTime) + ((ULONGLONG)u.dwHighDateTime << 32 | u.dwLowDateTime);
    }
    ULONGLONG wall = GetTickCount64() * 10000ULL;
    if (cpuPrev && wall > wallPrev && tot >= cpuPrev) {
        double pct = 100.0 * (double)(tot - cpuPrev) / (double)(wall - wallPrev);
        cpuEma = cpuEma * 0.5 + pct * 0.5;
        if (pct < cpuBase) cpuBase = pct; else cpuBase += 0.01 * (pct - cpuBase);
        int busy = cpuEma > cpuBase + 7.0;
        if (busy) { busyRun++; calmRun = 0; } else { calmRun++; busyRun = 0; }
        int hooksQuiet = !lastHook || GetTickCount() - lastHook > 90000;
        if (hooksQuiet) {
            if (!guessBusy && busyRun >= 3) { guessBusy = 1; if (!activity) { set_activity(ST_THINK); actFromGuess = 1; } }
            else if (guessBusy && calmRun >= 6) {
                guessBusy = 0;
                if (actFromGuess && activity == ST_THINK) { activity = 0; set_activity(ST_DONE); actFromGuess = 0; }
            }
        }
    }
    cpuPrev = tot; wallPrev = wall;
}

/* --------------------------------------------------------------- render */
static void make_dib(void) {
    if (memDC) { SelectObject(memDC, oldBmp); DeleteObject(dib); DeleteDC(memDC); }
    W = CW * S; H = CH * S;
    BITMAPINFO bi = {0};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = W; bi.bmiHeader.biHeight = -H;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    HDC sdc = GetDC(NULL);
    memDC = CreateCompatibleDC(sdc);
    dib = CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, (void **)&dibBits, NULL, 0);
    ReleaseDC(NULL, sdc);
    oldBmp = SelectObject(memDC, dib);
    needFull = 1;
}

static void draw_frame(void) {
    DrawIn in = { mode, modeT, gframe, facing, blinkLeft > 0, lookX, lookY, walkPhase, vy > 0 ? 1 : -1, energy < 0.35 };
    pet_draw(canvas, &in);
    int x = (int)lround(px), y = (int)lround(py);
    int changed = needFull || memcmp(canvas, prevCanvas, sizeof canvas);
    if (changed) {
        memcpy(prevCanvas, canvas, sizeof canvas);
        for (int cy = 0; cy < CH; cy++) for (int cx = 0; cx < CW; cx++) {
            uint32_t c = canvas[cy * CW + cx], a = c >> 24;
            uint32_t pm = (a << 24) | ((((c >> 16) & 255) * a / 255) << 16) | ((((c >> 8) & 255) * a / 255) << 8) | ((c & 255) * a / 255);
            for (int j = 0; j < S; j++) {
                uint32_t *row = dibBits + (cy * S + j) * W + cx * S;
                for (int i = 0; i < S; i++) row[i] = pm;
            }
        }
        POINT dst = { x, y }, src = { 0, 0 }; SIZE sz = { W, H };
        BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
        UpdateLayeredWindow(g_hwnd, NULL, &dst, &sz, memDC, &src, 0, &bf, ULW_ALPHA);
        needFull = 0;
    } else if (x != lastX || y != lastY) {
        SetWindowPos(g_hwnd, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    lastX = x; lastY = y;
}

static HICON make_icon(void) {
    uint32_t cv[CW * CH]; DrawIn in = { ST_IDLE, 0, 0, 1, 0, 0, 0, 0, 0, 0 };
    pet_draw(cv, &in);
    BITMAPV5HEADER bh = {0};
    bh.bV5Size = sizeof bh; bh.bV5Width = 32; bh.bV5Height = -32; bh.bV5Planes = 1; bh.bV5BitCount = 32;
    bh.bV5Compression = BI_BITFIELDS; bh.bV5RedMask = 0xFF0000; bh.bV5GreenMask = 0xFF00; bh.bV5BlueMask = 0xFF; bh.bV5AlphaMask = 0xFF000000;
    uint32_t *bits; HDC dc = GetDC(NULL);
    HBITMAP col = CreateDIBSection(dc, (BITMAPINFO *)&bh, DIB_RGB_COLORS, (void **)&bits, NULL, 0);
    ReleaseDC(NULL, dc);
    memset(bits, 0, 32 * 32 * 4);
    /* pet occupies x 12..27, y 20..29 -> scale 2 into 32x20, centred vertically */
    for (int y = 0; y < 32; y++) for (int x = 0; x < 32; x++) {
        int sx = 12 + x / 2, sy = 20 + (y - 6) / 2;
        if (y < 6 || sy >= CH) continue;
        uint32_t c = cv[sy * CW + sx]; uint32_t a = c >> 24;
        bits[y * 32 + x] = a ? c : 0;
    }
    HBITMAP mask = CreateBitmap(32, 32, 1, 1, NULL);
    ICONINFO ii = { TRUE, 0, 0, mask, col };
    HICON ic = CreateIconIndirect(&ii);
    DeleteObject(mask); DeleteObject(col);
    return ic;
}

/* ----------------------------------------------------------- visibility */
static void update_visibility(void) {
    claudeOpen = claude_window_open();
    int want = !userHidden && (cfgAlways || forceAlways || claudeOpen);
    if (want == shown) return;
    shown = want;
    if (shown) {
        RECT wa = work_area();
        if (onGround && !standOn) py = wa.bottom - H;
        needFull = 1; draw_frame();
        ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
        SetWindowPos(g_hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        SetTimer(g_hwnd, TIMER_ANIM, 33, NULL);
        if (!activity && autoSt != ST_SLEEP) react(ST_HELLO, 45);
    } else {
        KillTimer(g_hwnd, TIMER_ANIM);
        if (dragging) { dragging = 0; ReleaseCapture(); }
        ShowWindow(g_hwnd, SW_HIDE);
    }
}

/* ---------------------------------------------------------------- tick */
static void tick(void) {
    gframe++;
    RECT wa = work_area();
    double floorY = wa.bottom - H, lo = wa.left - padX(), hi = wa.right - W + padX(), top = wa.top - padTop();
    activity_tick();

    if (dragging) {
        POINT c; GetCursorPos(&c);
        if (abs(c.x - downPt.x) + abs(c.y - downPt.y) > 4) dragMoved = 1;
        if (dragMoved) { px = c.x - grab.x; py = c.y - grab.y; standOn = NULL; jumpPrep = 0; jumping = 0; }
        memmove(histX + 1, histX, sizeof(double) * 5); memmove(histY + 1, histY, sizeof(double) * 5);
        histX[0] = px; histY[0] = py; if (histN < 6) histN++;
    } else if (!onGround) {
        double g = 0.22 * S, prevFeet = py + H;
        if (platRefresh-- <= 0) { refresh_plats(); platRefresh = 5; }
        vy += g; if (vy > 18 * S) vy = 18 * S;
        px += vx; py += vy; if (!jumping) vx *= 0.993;
        if (px < lo) { px = lo; vx = fabs(vx) * 0.6; }
        if (px > hi) { px = hi; vx = -fabs(vx) * 0.6; }
        if (py < top) { py = top; vy = fabs(vy) * 0.4; }
        if (vx > 0.5) facing = 1; else if (vx < -0.5) facing = -1;
        /* land on the first window top crossed on the way down */
        int best = -1; LONG bt = LONG_MAX; double fx = px + W / 2.0, nf = py + H;
        if (vy > 0) for (int i = 0; i < platN; i++) {
            LONG t = plats[i].r.top;
            if (plats[i].h != ignorePlat && t >= prevFeet - 0.5 && t <= nf && t < bt && t < floorY + H && on_plat(i, fx)) { best = i; bt = t; }
        }
        if (best >= 0) {
            py = bt - H; jumping = 0; ignorePlat = NULL;
            if (vy > 3.5 * S) {
                if (vy > 11 * S) react(ST_DIZZY, 75);
                vy = -vy * 0.35; vx *= 0.6; landT = 4;
            } else {
                onGround = 1; standOn = plats[best].h; standR = plats[best].r; vy = 0; vx = 0; landT = 8;
            }
        } else if (py >= floorY) {
            py = floorY; jumping = 0; ignorePlat = NULL; standOn = NULL;
            if (vy > 3.5 * S) {
                if (vy > 11 * S) react(ST_DIZZY, 75);
                vy = -vy * 0.35; vx *= 0.6; landT = 4;
            } else {
                onGround = 1; vy = 0; vx = 0; landT = 8;
            }
        }
    } else if (standOn) {
        /* ride along with the window; fall if it goes away or slides out from under us */
        if (platRefresh-- <= 0) { refresh_plats(); platRefresh = 15; }
        RECT r; int idx = plat_index(standOn);
        if (idx < 0 || !win_rect(standOn, &r)) fall_off();
        else {
            if (r.left - standR.left == r.right - standR.right) px += r.left - standR.left;   /* moved, not resized */
            standR = r; plats[idx].r = r;
            py = r.top - H;
            if (!on_plat(idx, px + W / 2.0)) fall_off();
        }
        if (px < lo) px = lo; if (px > hi) px = hi;
    } else {
        py = floorY;
        if (px < lo) px = lo; if (px > hi) px = hi;
    }
    if (jumpPrep > 0 && onGround && !dragging && --jumpPrep == 0) {
        onGround = 0; standOn = NULL; jumping = 1; vx = jumpVx; vy = jumpVy;
    }

    /* pick what to show */
    int ns;
    if (dragging && dragMoved) ns = ST_DRAG;
    else if (!onGround) ns = ST_FALL;
    else if (jumpPrep > 0) ns = ST_LAND;
    else if (landT > 0) { ns = ST_LAND; landT--; }
    else if (reactSt >= 0) { ns = reactSt; if (++reactT >= reactDur) reactSt = -1; }
    else if (activity) ns = activity;
    else {
        ns = autoSt; autoT++;
        if (autoSt == ST_WALK) {
            double sp = 0.33 * S * (happy > 0.8 ? 1.3 : 1.0);
            px += facing * sp;
            if (++walkAcc % 7 == 0) walkPhase++;
            if ((facing > 0 && px >= walkTarget) || (facing < 0 && px <= walkTarget) || px <= lo || px >= hi) {
                if (px < lo) px = lo; if (px > hi) px = hi;
                set_auto(ST_IDLE, rr(40, 120));
            } else if (rr(0, 2500) == 0) set_auto(ST_TRIP, 100);
        } else if (autoSt == ST_SLEEP) {
            energy += 1.0 / 1200.0;
            if (autoT >= autoDur || energy >= 1.0) { if (energy > 1) energy = 1; set_auto(ST_YAWN, 70); }
        } else if (autoT >= autoDur) choose_next();
    }
    if (ns != mode) { mode = ns; modeT = 0; } else modeT++;

    /* needs drift */
    if (autoSt != ST_SLEEP) energy -= 1.0 / (30.0 * 60 * 12);   /* ~12 min awake to get sleepy */
    if (energy < 0) energy = 0;
    happy += (0.6 - happy) * 0.0005;

    /* eyes: blink + look at the cursor when it's near */
    if (blinkLeft > 0) blinkLeft--;
    else if (--nextBlink <= 0) { blinkLeft = 4; nextBlink = rr(60, 200); if (rr(0, 5) == 0) nextBlink = 8; }
    POINT c; GetCursorPos(&c);
    double ex = c.x - (px + W / 2.0), ey = c.y - (py + (CH - 7) * S);
    if (fabs(ex) < 600 && fabs(ey) < 450) {
        lookX = ex > 30 ? 1 : ex < -30 ? -1 : 0;
        lookY = ey < -50 ? -1 : ey > 50 ? 1 : 0;
        lookHold = 60;
        if (mode == ST_IDLE && fabs(ex) < 120 && fabs(ey) < 120 && rr(0, 400) == 0 && reactSt < 0) {
            facing = ex > 0 ? 1 : -1; react(ST_WAVE, 50);
        }
    } else if (--lookHold <= 0) {
        lookX = rr(-1, 1); lookY = rr(0, 3) ? 0 : -1; lookHold = rr(40, 160);
    }
    draw_frame();
}

/* ----------------------------------------------------------------- menu */
static void show_menu(void) {
    HMENU m = CreatePopupMenu(), sz = CreatePopupMenu();
    int cur = cfgScale ? cfgScale : dpi_scale();
    static const int sizes[] = { 2, 3, 4, 5, 6, 8 };
    for (int i = 0; i < 6; i++) {
        wchar_t lbl[32]; swprintf(lbl, 32, L"%dx%ls", sizes[i], sizes[i] == dpi_scale() ? L"  (default)" : L"");
        AppendMenuW(sz, MF_STRING | (cur == sizes[i] ? MF_CHECKED : 0), ID_SIZE0 + sizes[i], lbl);
    }
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"Claude Pet");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING | (cfgAlways ? MF_CHECKED : 0), ID_ALWAYS, L"Show even when Claude is minimized");
    AppendMenuW(m, MF_STRING | (cfgGuess ? MF_CHECKED : 0), ID_GUESS, L"React to Claude app activity");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)sz, L"Size");
    AppendMenuW(m, MF_STRING | (startup_enabled() ? MF_CHECKED : 0), ID_STARTUP, L"Start with Windows");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, autoSt == ST_SLEEP ? ID_WAKE : ID_NAP, autoSt == ST_SLEEP ? L"Wake up" : L"Nap time");
    AppendMenuW(m, MF_STRING, ID_HIDE, userHidden ? L"Show pet" : L"Hide pet");
    AppendMenuW(m, MF_STRING, ID_QUIT, L"Quit");
    POINT p; GetCursorPos(&p);
    SetForegroundWindow(g_hwnd);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, p.x, p.y, 0, g_hwnd, NULL);
    PostMessageW(g_hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);
    switch (cmd) {
    case ID_ALWAYS: cfgAlways = !cfgAlways; reg_set(L"AlwaysShow", cfgAlways); break;
    case ID_GUESS: cfgGuess = !cfgGuess; reg_set(L"GuessActivity", cfgGuess); if (!cfgGuess && actFromGuess) activity = 0; break;
    case ID_STARTUP: startup_set(!startup_enabled()); break;
    case ID_NAP: energy = 0.1; set_auto(ST_SLEEP, 1200); activity = 0; break;
    case ID_WAKE: energy = 1; set_auto(ST_YAWN, 70); break;
    case ID_HIDE: userHidden = !userHidden; break;
    case ID_QUIT: DestroyWindow(g_hwnd); return;
    default:
        if (cmd > ID_SIZE0 && cmd <= ID_SIZE0 + 16) {
            double feetX = px + W / 2.0, feetY = py + H;
            cfgScale = cmd - ID_SIZE0; reg_set(L"Scale", cfgScale == dpi_scale() ? 0 : cfgScale);
            S = cfgScale; make_dib();
            px = feetX - W / 2.0; py = feetY - H;
        }
    }
    update_visibility();
}

/* -------------------------------------------------------------- wndproc */
static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_TIMER:
        if (wp == TIMER_ANIM) tick();
        else if (wp == TIMER_POLL) {
            update_visibility();
            guess_tick();
            if (shown) SetWindowPos(h, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            if (!shown) activity_tick();
        }
        return 0;
    case WM_LBUTTONDOWN: {
        POINT c; GetCursorPos(&c);
        dragging = 1; dragMoved = 0; downPt = c; downTick = GetTickCount();
        grab.x = c.x - (LONG)lround(px); grab.y = c.y - (LONG)lround(py);
        histN = 0; SetCapture(h);
        return 0;
    }
    case WM_LBUTTONUP:
        if (dragging) {
            int moved = dragMoved;
            dragging = 0; ReleaseCapture();
            if (!moved) on_click();
            else {
                onGround = 0; standOn = NULL; ignorePlat = NULL; platRefresh = 0;
                if (histN >= 4) { vx = (histX[0] - histX[3]) / 3.0; vy = (histY[0] - histY[3]) / 3.0; }
                else { vx = 0; vy = 0; }
                double lim = 12.0 * S;
                if (vx > lim) vx = lim; if (vx < -lim) vx = -lim; if (vy > lim) vy = lim; if (vy < -lim) vy = -lim;
                if (fabs(vx) + fabs(vy) > 9 * S) happy += 0.05;   /* wheee */
            }
        }
        return 0;
    case WM_CAPTURECHANGED:
        if (dragging && (HWND)lp != h) { dragging = 0; if (dragMoved) { onGround = 0; standOn = NULL; vx = vy = 0; } }
        return 0;
    case WM_RBUTTONUP: show_menu(); return 0;
    case WM_SETCURSOR: SetCursor(LoadCursorW(NULL, dragging && dragMoved ? IDC_SIZEALL : IDC_HAND)); return TRUE;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_COPYDATA: {
        COPYDATASTRUCT *cd = (COPYDATASTRUCT *)lp;
        if (cd->dwData != COPY_MAGIC || cd->cbData == 0 || cd->cbData > 512) return FALSE;
        char buf[513]; memcpy(buf, cd->lpData, cd->cbData); buf[cd->cbData] = 0;
        char *bar = strchr(buf, '|'); const char *tool = "";
        if (bar) { *bar = 0; tool = bar + 1; }
        if (!strcmp(buf, "__quit")) { DestroyWindow(h); return TRUE; }
        handle_event(buf, tool);
        return TRUE;
    }
    case WM_TRAY:
        if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU) show_menu();
        else if (LOWORD(lp) == WM_LBUTTONUP) {
            if (userHidden) { userHidden = 0; update_visibility(); }
            else if (shown) { onGround = 0; standOn = NULL; jumpPrep = 0; vy = -6.0 * S; vx = 0; }   /* boing */
        }
        return 0;
    case WM_DISPLAYCHANGE: case WM_SETTINGCHANGE:
        if (!cfgScale) { int s = dpi_scale(); if (s != S) { double fx = px + W / 2.0, fy = py + H; S = s; make_dib(); px = fx - W / 2.0; py = fy - H; } }
        return 0;
    case WM_DESTROY:
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        PostQuitMessage(0);
        return 0;
    }
    if (msg == g_taskbarMsg && g_taskbarMsg) { Shell_NotifyIconW(NIM_ADD, &g_nid); return 0; }
    return DefWindowProcW(h, msg, wp, lp);
}

/* ------------------------------------------------------------ hook side */
static void json_str(const char *js, const char *key, char *out, size_t n) {
    out[0] = 0;
    char pat[64]; snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(js, pat); if (!p) return;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p != ':') return; p++;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p != '"') return; p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < n) { if (*p == '\\' && p[1]) p++; out[i++] = *p++; }
    out[i] = 0;
}

static void launch_self(const char *initial) {
    wchar_t exe[MAX_PATH], cmd[MAX_PATH + 600];
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    if (initial && *initial) swprintf(cmd, MAX_PATH + 600, L"\"%ls\" --initial \"%hs\"", exe, initial);
    else swprintf(cmd, MAX_PATH + 600, L"\"%ls\"", exe);
    STARTUPINFOW si = { sizeof si }; PROCESS_INFORMATION pi;
    /* break away from the hook runner's job so the pet outlives the hook */
    if (!CreateProcessW(exe, cmd, NULL, NULL, FALSE, CREATE_BREAKAWAY_FROM_JOB | CREATE_NEW_PROCESS_GROUP, NULL, NULL, &si, &pi) &&
        !CreateProcessW(exe, cmd, NULL, NULL, FALSE, CREATE_NEW_PROCESS_GROUP, NULL, NULL, &si, &pi)) return;
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
}

static int send_to_pet(const char *msg, int launchIfMissing) {
    HWND h = FindWindowW(WND_CLASS, NULL);
    if (!h) { if (launchIfMissing) launch_self(msg); return 1; }
    COPYDATASTRUCT cd = { COPY_MAGIC, (DWORD)strlen(msg), (PVOID)msg };
    DWORD_PTR r;
    SendMessageTimeoutW(h, WM_COPYDATA, 0, (LPARAM)&cd, SMTO_ABORTIFHUNG | SMTO_BLOCK, 800, &r);
    return 0;
}

static int run_hook(void) {
    static char buf[65536];
    DWORD total = 0, n;
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    if (in && in != INVALID_HANDLE_VALUE) {
        DWORD ft = GetFileType(in);
        if (ft == FILE_TYPE_PIPE || ft == FILE_TYPE_DISK) {
            char sink[8192];
            for (;;) {
                if (total < sizeof buf - 1) {
                    if (!ReadFile(in, buf + total, (DWORD)(sizeof buf - 1 - total), &n, NULL) || !n) break;
                    total += n;
                } else if (!ReadFile(in, sink, sizeof sink, &n, NULL) || !n) break;   /* drain */
            }
        }
    }
    buf[total] = 0;
    char ev[64], tool[160], msg[240];
    json_str(buf, "hook_event_name", ev, sizeof ev);
    json_str(buf, "tool_name", tool, sizeof tool);
    if (!ev[0]) return 0;
    snprintf(msg, sizeof msg, "%s|%s", ev, tool);
    send_to_pet(msg, !strcmp(ev, "SessionStart"));
    return 0;
}

/* ----------------------------------------------------------------- main */
int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cl, int show) {
    (void)prev; (void)cl; (void)show;
    int argc; wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    char initial[512] = "";
    for (int i = 1; i < argc; i++) {
        if (!wcscmp(argv[i], L"--hook")) return run_hook();
        if (!wcscmp(argv[i], L"--quit")) return send_to_pet("__quit|", 0);
        if (!wcscmp(argv[i], L"--event") && i + 1 < argc) {
            char m[300]; snprintf(m, sizeof m, "%ls|%ls", argv[i + 1], i + 2 < argc ? argv[i + 2] : L"");
            return send_to_pet(m, 0);
        }
        if (!wcscmp(argv[i], L"--always")) forceAlways = 1;
        if (!wcscmp(argv[i], L"--initial") && i + 1 < argc) snprintf(initial, sizeof initial, "%ls", argv[++i]);
    }
    HANDLE mtx = CreateMutexW(NULL, TRUE, MUTEX_NAME);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (initial[0]) { send_to_pet(initial, 0); return 0; }      /* a hook woke us: just pass the event on */
        /* a plain launch replaces the running pet, so a new build takes over without a reboot */
        send_to_pet("__quit|", 0);
        DWORD w = WaitForSingleObject(mtx, 5000);
        if (w != WAIT_OBJECT_0 && w != WAIT_ABANDONED) return 0;
    }

    /* crisp pixels on high-DPI screens */
    typedef BOOL (WINAPI *SetCtx)(HANDLE);
    SetCtx setCtx = (SetCtx)(void *)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext");
    if (!setCtx || !setCtx((HANDLE)-4)) SetProcessDPIAware();

    g_inst = inst;
    selfPid = GetCurrentProcessId();
    rng ^= GetTickCount() * 2654435761u; if (!rng) rng = 1;
    cfgAlways = reg_get(L"AlwaysShow", 0);
    cfgGuess = reg_get(L"GuessActivity", 1);
    cfgScale = reg_get(L"Scale", 0);
    S = cfgScale ? cfgScale : dpi_scale();
    make_dib();

    WNDCLASSEXW wc = { sizeof wc };
    wc.lpfnWndProc = wndproc; wc.hInstance = inst; wc.lpszClassName = WND_CLASS;
    wc.hCursor = LoadCursorW(NULL, IDC_HAND);
    g_icon = make_icon(); wc.hIcon = g_icon; wc.hIconSm = g_icon;
    RegisterClassExW(&wc);
    g_hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                             WND_CLASS, L"Claude Pet", WS_POPUP, 0, 0, W, H, NULL, NULL, inst, NULL);
    if (!g_hwnd) return 1;
    typedef BOOL (WINAPI *FilterFn)(HWND, UINT, DWORD, void *);
    FilterFn filt = (FilterFn)(void *)GetProcAddress(GetModuleHandleW(L"user32.dll"), "ChangeWindowMessageFilterEx");
    if (filt) filt(g_hwnd, WM_COPYDATA, 1 /*MSGFLT_ALLOW*/, NULL);

    RECT wa; SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    px = wa.right - W - 80 + padX(); py = wa.bottom - H;
    if (reg_get(L"X", 0x7FFFFFFF) != 0x7FFFFFFF) px = (int)reg_get(L"X", 0);

    g_taskbarMsg = RegisterWindowMessageW(L"TaskbarCreated");
    g_nid.cbSize = sizeof g_nid; g_nid.hWnd = g_hwnd; g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP; g_nid.uCallbackMessage = WM_TRAY; g_nid.hIcon = g_icon;
    wcscpy(g_nid.szTip, L"Claude Pet");
    Shell_NotifyIconW(NIM_ADD, &g_nid);

    if (initial[0]) {
        char *bar = strchr(initial, '|'); const char *tool = "";
        if (bar) { *bar = 0; tool = bar + 1; }
        handle_event(initial, tool);
    }
    SetTimer(g_hwnd, TIMER_POLL, 700, NULL);
    update_visibility();

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageW(&m); }
    reg_set(L"X", (DWORD)(int)lround(px));
    if (mtx) { ReleaseMutex(mtx); CloseHandle(mtx); }
    return 0;
}
