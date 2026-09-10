// mosquito — a nudge-you-to-move / nudge-you-to-sleep overlay for Windows 11.
//
// MVP scaffold: the "sedentary" (久坐) mode only.
//  - each mosquito is its own tiny click-through, always-on-top layered window
//    that moves across the whole virtual desktop (all monitors). This keeps
//    memory in the single-digit MB range instead of holding a full-screen buffer.
//  - random-walking colored mosquitoes drawn with GDI+ (per-pixel alpha)
//  - activity detection via GetLastInputInfo with a Schmitt-trigger (hysteresis)
//    so a single accidental nudge never counts as "resumed work"
//  - fullscreen suppression (hide while a fullscreen app / screen-share is up)
//
// Sleep (睡觉) mode, sound, autostart and the 滕王阁序 dismissal come later.
//
// Build:  build.bat
// Dev:    mosquito.exe --fast   compress the 40-min timeline into seconds + logs
//         mosquito.exe --demo   show the mosquitoes immediately (skip the gate)
//         Ctrl+Alt+Q quits (windows are click-through, so there is no X to click).

#define UNICODE
#define _UNICODE
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objidl.h>   // MinGW: gdiplus.h needs PROPID from here first
#include <gdiplus.h>
#include <shellapi.h>
#include <mmsystem.h>
#include <shlwapi.h>
#include <winhttp.h>
#include <vector>
#include <deque>
#include <string>
#include <cwchar>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>

using namespace Gdiplus;

// Each mosquito lives in a BOX x BOX window. Base sprite is scale 2.0 (~56px);
// sleep mode later grows it up to 3x of that, so the box is sized for the
// largest case plus margin and never has to be recreated on resize.
static const int BOX = 220;

#define APP_VER_STR "1.1.2"          // single source of truth (narrow, for update compare)
#define APP_VER_WIDE2(x) L##x
#define APP_VER_WIDE(x) APP_VER_WIDE2(x)
#define APP_VER APP_VER_WIDE(APP_VER_STR)   // wide L"1.0.0" for UI text

// ------------------------------- language -----------------------------------
static int g_lang = 0;   // 0 = 中文 (default), 1 = English
static const wchar_t* T(const wchar_t* zh, const wchar_t* en) { return g_lang ? en : zh; }
static const wchar_t* BRAND() { return g_lang ? L"AngryMoz" : L"愤怒的蚊子"; }

// ------------------------------- tuning -------------------------------------

struct Config {
    int  work_threshold_s;   // continuous work before the 1st mosquito
    int  escalate_every_s;   // add one more mosquito per this many extra seconds
    int  max_sedentary;      // hard cap on sedentary-mode mosquitoes
    int  resume_window_s;    // sliding window used to detect "back to work"
    int  resume_need_s;      // need this many active seconds within that window
    int  rest_window_s;      // sliding window used to detect "on a break"
    int  rest_max_active_s;  // at most this many active seconds within it = resting
    int  sleep_growth_s;     // sleep mode: add one more mosquito per this many seconds
    int  sleep_max;          // sleep mode: cap per monitor
    int  sleep_return_s;     // after a dismissal, how long until they come back
    int  work_break_s;       // WORK phase: continuous stillness that counts as a real break
};

static Config make_config(bool fast) {
    if (fast) return Config{ 15, 3, 5, 6, 3, 30, 2, 3, 10, 10, 8 };        // dev timeline
    return Config{ 40 * 60, 60, 5, 60, 30, 5 * 60, 15, 3 * 60, 10, 5 * 60, 15 * 60 }; // real timeline
}

// ------------------------------- activity -----------------------------------
//
// Sample once per second whether the machine saw input. A Schmitt trigger with
// two thresholds and a hold-in-between region means isolated events cannot flip
// the state:
//   RESTING -> WORKING : sustained activity  (>= resume_need_s in last window)
//   WORKING -> RESTING : sustained idleness  (<= rest_max_active_s in last window)

class ActivityDetector {
public:
    enum State { RESTING, WORKING };
    explicit ActivityDetector(const Config& c) : cfg_(c) {}

    void tick() {
        LASTINPUTINFO lii{ sizeof(lii), 0 };
        GetLastInputInfo(&lii);
        bool active = (GetTickCount() - lii.dwTime) < 1500;   // input within ~1.5s

        history_.push_front(active ? 1 : 0);
        int cap = cfg_.rest_window_s > cfg_.resume_window_s ? cfg_.rest_window_s
                                                            : cfg_.resume_window_s;
        while ((int)history_.size() > cap) history_.pop_back();

        if (state_ == RESTING) {
            if (active_in(cfg_.resume_window_s) >= cfg_.resume_need_s) {
                state_ = WORKING; just_started_working_ = true;
            }
        } else {
            if (active_in(cfg_.rest_window_s) <= cfg_.rest_max_active_s) {
                state_ = RESTING; just_started_resting_ = true;
            }
        }
    }

    State state() const { return state_; }
    bool take_started_working() { bool v = just_started_working_; just_started_working_ = false; return v; }
    bool take_started_resting() { bool v = just_started_resting_; just_started_resting_ = false; return v; }

private:
    int active_in(int window_s) const {
        int n = window_s < (int)history_.size() ? window_s : (int)history_.size();
        int sum = 0;
        for (int i = 0; i < n; ++i) sum += history_[i];
        return sum;
    }
    Config cfg_;
    State state_ = RESTING;
    std::deque<int> history_;
    bool just_started_working_ = false;
    bool just_started_resting_ = false;
};

// ------------------------------- mosquito -----------------------------------

static float frand(float a, float b) { return a + (b - a) * ((float)rand() / (float)RAND_MAX); }

// Each mosquito flies one of several trajectory algorithms, assigned at random,
// so the swarm doesn't all drift in the same lazy circle.
enum FlightPattern { P_WANDER = 0, P_SPIRAL, P_SWEAVE, P_ZIG, P_COUNT };

struct Mosquito {
    float x, y;      // absolute virtual-desktop pixel position (sprite center)
    float vx, vy;    // velocity px/frame
    float size;      // scale (3.0 == ~84px base sprite)
    float phase;     // wing-flutter phase
    int   mode;      // 0 = flying, 1 = landed/stopped
    int   mode_t;    // frames left while landed
    int   mon;       // index of the monitor this mosquito is bound to
    int   pattern;   // FlightPattern
    float heading;   // current flight direction (radians)
    float base_dir;  // slowly-drifting reference direction (spiral/weave advance)
    float turn;      // angular rate for the spiral
    int   pat_t;     // pattern timer (zag interval / weave phase counter)
    Color color;
    Color wing;
};

// Saturated colors that stay visible on both dark and light backgrounds
// (paired with the thin dark outline in draw_sprite).
static Color pick_color() {
    static const BYTE pal[][3] = {
        {255, 90, 95}, {255, 176, 32}, {25, 195, 214},
        {231, 83, 184}, {142, 209, 30}, {120, 130, 255},
    };
    int i = rand() % (int)(sizeof(pal) / sizeof(pal[0]));
    return Color(255, pal[i][0], pal[i][1], pal[i][2]);
}

static Mosquito spawn_mosquito(int x0, int y0, int w, int h, float base_size) {
    Mosquito m;
    m.x = frand(x0 + w * 0.2f, x0 + w * 0.8f);
    m.y = frand(y0 + h * 0.2f, y0 + h * 0.8f);
    float a = frand(0.0f, 6.2831853f), s = frand(3.0f, 6.0f);
    m.vx = std::cos(a) * s; m.vy = std::sin(a) * s;
    m.size = base_size;
    m.phase = frand(0.0f, 6.28f);
    m.mode = 0;
    m.mode_t = (int)frand(60.0f, 240.0f);
    m.pattern = rand() % P_COUNT;
    m.heading = a;
    m.base_dir = a;
    m.turn = (0.06f + frand(0.0f, 0.10f)) * ((rand() % 2) ? 1.0f : -1.0f);
    m.pat_t = (int)frand(12.0f, 60.0f);
    Color c = pick_color();
    m.color = c;
    m.wing = Color(90, c.GetR(), c.GetG(), c.GetB());
    return m;
}

static void clamp_bounds(Mosquito& m, int x0, int y0, int w, int h) {
    const float pad = 20.0f;
    if (m.x < x0 + pad)     { m.x = x0 + pad;     m.vx = std::fabs(m.vx); }
    if (m.x > x0 + w - pad) { m.x = x0 + w - pad; m.vx = -std::fabs(m.vx); }
    if (m.y < y0 + pad)     { m.y = y0 + pad;     m.vy = std::fabs(m.vy); }
    if (m.y > y0 + h - pad) { m.y = y0 + h - pad; m.vy = -std::fabs(m.vy); }
}

// Real-mosquito flight. Each mosquito follows one of several trajectory
// algorithms (assigned at spawn); all of them scale with maxs (--speed) and any
// of them can pause (land) anywhere before bursting off again.
static void update_mosquito(Mosquito& m, int x0, int y0, int w, int h, float maxs) {
    if (m.mode == 1) {                       // landed: sit still, wings barely move
        m.vx *= 0.55f; m.vy *= 0.55f;
        m.x += m.vx;   m.y += m.vy;
        m.phase += 0.15f;
        if (--m.mode_t <= 0) {               // take off in a fresh direction
            m.heading = frand(0.0f, 6.2831853f);
            m.base_dir = m.heading;
            m.mode = 0;
        }
        clamp_bounds(m, x0, y0, w, h);
        return;
    }

    float s;
    switch (m.pattern) {
    case P_SPIRAL:                           // looping / spiralling advance
        m.heading += m.turn;
        m.turn += frand(-0.003f, 0.003f);
        { float sg = m.turn < 0 ? -1.0f : 1.0f, at = std::fabs(m.turn);
          at = at < 0.05f ? 0.05f : (at > 0.20f ? 0.20f : at); m.turn = sg * at; }
        m.base_dir += frand(-0.03f, 0.03f);
        s = maxs * 0.8f;
        m.vx = std::cos(m.heading) * s + std::cos(m.base_dir) * maxs * 0.4f;
        m.vy = std::sin(m.heading) * s + std::sin(m.base_dir) * maxs * 0.4f;
        break;
    case P_SWEAVE:                           // S-shaped weave along a drifting line
        m.pat_t++;
        m.heading = m.base_dir + 0.9f * std::sin(m.pat_t * 0.15f);
        if (frand(0.0f, 1.0f) < 0.01f) m.base_dir += frand(-1.5f, 1.5f);
        s = maxs * 0.9f;
        m.vx = std::cos(m.heading) * s; m.vy = std::sin(m.heading) * s;
        break;
    case P_ZIG:                              // straight dashes with sharp U-turns
        if (--m.pat_t <= 0) {
            m.heading += frand(1.4f, 2.4f) * ((rand() % 2) ? 1.0f : -1.0f);
            m.pat_t = (int)frand(12.0f, 40.0f);
        }
        s = maxs;
        m.vx = std::cos(m.heading) * s; m.vy = std::sin(m.heading) * s;
        break;
    default:                                 // P_WANDER: erratic roaming, quick turns
        m.heading += frand(-0.25f, 0.25f);
        if (frand(0.0f, 1.0f) < 0.05f) m.heading += frand(-2.0f, 2.0f);
        s = maxs * frand(0.6f, 1.0f);
        m.vx = std::cos(m.heading) * s; m.vy = std::sin(m.heading) * s;
        break;
    }

    float sp = std::sqrt(m.vx * m.vx + m.vy * m.vy), cap = maxs * 1.3f;
    if (sp > cap) { m.vx *= cap / sp; m.vy *= cap / sp; }
    m.x += m.vx; m.y += m.vy;

    // Bounce off the monitor edges and turn the flight direction with it.
    const float pad = 20.0f;
    bool bx = false, by = false;
    if (m.x < x0 + pad)     { m.x = x0 + pad;     bx = true; }
    if (m.x > x0 + w - pad) { m.x = x0 + w - pad; bx = true; }
    if (m.y < y0 + pad)     { m.y = y0 + pad;     by = true; }
    if (m.y > y0 + h - pad) { m.y = y0 + h - pad; by = true; }
    if (bx) m.vx = -m.vx;
    if (by) m.vy = -m.vy;
    if (bx || by) { m.heading = std::atan2(m.vy, m.vx); m.base_dir = m.heading; }
    m.phase += 0.8f;

    if (frand(0.0f, 1.0f) < 0.005f) {        // land somewhere and pause
        m.mode = 1; m.mode_t = (int)frand(25.0f, 110.0f);
    }
}

static void fill_rot_ellipse(Graphics& g, Brush* b, float cx, float cy,
                             float rx, float ry, float deg) {
    GraphicsState st = g.Save();
    g.TranslateTransform(cx, cy);
    g.RotateTransform(deg);
    g.FillEllipse(b, -rx, -ry, rx * 2, ry * 2);
    g.Restore(st);
}

// The "A · realistic silhouette" sprite (ported from the agreed SVG), drawn
// around the current transform origin. Caller sets translate; we apply scale.
static void draw_sprite(Graphics& g, const Mosquito& m) {
    GraphicsState st = g.Save();
    g.ScaleTransform(m.size, m.size);

    Pen legPen(m.color, 0.9f);
    legPen.SetStartCap(LineCapRound); legPen.SetEndCap(LineCapRound);
    const float legs[6][4] = {
        {-2,0,-8,6}, {-1,1,-6,8}, {0,1,2,9}, {-2,-1,-9,3}, {0,0,7,7}, {1,0,9,4},
    };
    for (auto& l : legs) g.DrawLine(&legPen, l[0], l[1], l[2], l[3]);

    float flut = 0.85f + 0.15f * std::sin(m.phase);
    SolidBrush wingBrush(m.wing);
    fill_rot_ellipse(g, &wingBrush, 3, -5, 8, 3.2f * flut, -18);
    fill_rot_ellipse(g, &wingBrush, 5, -3, 8, 3.0f * flut, -8);

    SolidBrush body(m.color);
    Pen outline(Color(90, 0, 0, 0), 0.4f);
    fill_rot_ellipse(g, &body, 5, 4, 8, 2.3f, 28);
    { GraphicsState s2 = g.Save(); g.TranslateTransform(5, 4); g.RotateTransform(28);
      g.DrawEllipse(&outline, -8.0f, -2.3f, 16.0f, 4.6f); g.Restore(s2); }
    g.FillEllipse(&body, -2 - 2.8f, -1 - 2.8f, 5.6f, 5.6f);
    g.DrawEllipse(&outline, -2 - 2.8f, -1 - 2.8f, 5.6f, 5.6f);
    g.FillEllipse(&body, -6 - 1.9f, -3 - 1.9f, 3.8f, 3.8f);
    g.DrawEllipse(&outline, -6 - 1.9f, -3 - 1.9f, 3.8f, 3.8f);
    Pen pro(m.color, 0.8f);
    g.DrawLine(&pro, -7.0f, -4.0f, -12.0f, -7.0f);

    g.Restore(st);
}

// --------------------------- per-mosquito window ----------------------------
//
// One small layered window per mosquito. Its backing store is a single
// premultiplied-ARGB DIB, wrapped by a GDI+ Bitmap in PARGB format so GDI+
// draws straight into it with correct premultiplied alpha — one buffer, no copy.

struct MosView {
    HWND hwnd = nullptr;
    HDC memDC = nullptr;
    HBITMAP dib = nullptr, old = nullptr;
    void* bits = nullptr;
    Bitmap* bmp = nullptr;
    bool shown = false;
};

static const wchar_t* MOS_CLASS = L"MosquitoSprite";

static MosView create_mosview(HINSTANCE hInst) {
    MosView v;
    v.hwnd = CreateWindowEx(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        MOS_CLASS, L"", WS_POPUP, 0, 0, BOX, BOX, nullptr, nullptr, hInst, nullptr);

    HDC screen = GetDC(nullptr);
    v.memDC = CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = BOX;
    bi.bmiHeader.biHeight = -BOX;   // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    v.dib = CreateDIBSection(v.memDC, &bi, DIB_RGB_COLORS, &v.bits, nullptr, 0);
    v.old = (HBITMAP)SelectObject(v.memDC, v.dib);
    ReleaseDC(nullptr, screen);

    v.bmp = new Bitmap(BOX, BOX, BOX * 4, PixelFormat32bppPARGB, (BYTE*)v.bits);
    return v;
}

static void destroy_mosview(MosView& v) {
    delete v.bmp;
    if (v.memDC) { SelectObject(v.memDC, v.old); DeleteDC(v.memDC); }
    if (v.dib)  DeleteObject(v.dib);
    if (v.hwnd) DestroyWindow(v.hwnd);
    v = MosView{};
}

static void render_mosview(MosView& v, const Mosquito& m) {
    Graphics g(v.bmp);
    g.SetCompositingMode(CompositingModeSourceOver);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.Clear(Color(0, 0, 0, 0));
    g.TranslateTransform(BOX / 2.0f, BOX / 2.0f);
    draw_sprite(g, m);

    POINT src{ 0, 0 };
    POINT dst{ (LONG)(m.x - BOX / 2), (LONG)(m.y - BOX / 2) };
    SIZE  sz{ BOX, BOX };
    BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    HDC screen = GetDC(nullptr);
    UpdateLayeredWindow(v.hwnd, screen, &dst, &sz, v.memDC, &src, 0, &bf, ULW_ALPHA);
    ReleaseDC(nullptr, screen);
}

// ---- monitors: the configured count is PER monitor, not a global total ----

static std::vector<RECT> g_monitors;

static BOOL CALLBACK mon_enum(HMONITOR h, HDC, LPRECT, LPARAM) {
    MONITORINFO mi{ sizeof(mi) };
    if (GetMonitorInfo(h, &mi)) g_monitors.push_back(mi.rcMonitor);
    return TRUE;
}

static void enumerate_monitors() {
    g_monitors.clear();
    EnumDisplayMonitors(nullptr, nullptr, mon_enum, 0);
    if (g_monitors.empty()) {
        RECT r{ 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
        g_monitors.push_back(r);
    }
}

// Which monitor (index) has a real fullscreen app in front (game / video /
// screen-share)? -1 if none. We suppress only that monitor's mosquitoes.
static int fullscreen_monitor_index() {
    HWND fg = GetForegroundWindow();
    if (!fg || fg == GetDesktopWindow() || fg == GetShellWindow()) return -1;
    RECT wr;
    if (!GetWindowRect(fg, &wr)) return -1;
    MONITORINFO mi{ sizeof(mi) };
    if (!GetMonitorInfo(MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST), &mi)) return -1;
    bool full = wr.left <= mi.rcMonitor.left && wr.top <= mi.rcMonitor.top &&
                wr.right >= mi.rcMonitor.right && wr.bottom >= mi.rcMonitor.bottom;
    if (!full) return -1;
    for (size_t i = 0; i < g_monitors.size(); ++i) {
        const RECT& r = g_monitors[i];
        if (r.left == mi.rcMonitor.left && r.top == mi.rcMonitor.top &&
            r.right == mi.rcMonitor.right && r.bottom == mi.rcMonitor.bottom)
            return (int)i;
    }
    return -1;
}

// ------------------------------- app ----------------------------------------

static const UINT ID_LOGIC  = 1;   // 1 Hz: activity + escalation
static const UINT ID_RENDER = 2;   // ~30 Hz: animation, only while visible
static const UINT ID_BLINK  = 3;   // tray-icon blink (period depends on urgency)
static const int  HOTKEY_QUIT  = 100;
static const int  HOTKEY_PAUSE = 101;
static const UINT WM_TRAY    = WM_APP + 1;
static const UINT IDM_PAUSE_30  = 210;
static const UINT IDM_PAUSE_60  = 211;
static const UINT IDM_PAUSE_120 = 212;
static const UINT IDM_PAUSE_OFF = 213;
static const UINT IDM_DISMISS   = 201;
static const UINT IDM_QUIT      = 202;
static const UINT IDM_AUTOSTART = 203;
static const UINT IDM_SOUND     = 204;
static const UINT IDM_SETTINGS  = 205;
static const UINT IDM_HELP      = 206;
static const UINT IDM_SKIP_REST = 207;
static const UINT IDM_UPDATE_OPEN = 208;

// Update check (set by a background thread; read on the UI thread).
static volatile bool g_update_ready = false;   // a newer GitHub release exists
static std::wstring   g_update_tag;             // its tag, e.g. "v1.1.0"

static void update_tray();          // defined after the tray helpers below
static void sound_set_volume(float v);
static DWORD g_boot = 0;            // for the 5-minute "dismissal method" edit window

struct App {
    Config cfg;
    bool fast = false, demo = false;
    float base_size = 3.0f;    // sprite scale (~84px). Override with --size=N
    float max_speed = 32.0f;   // px/frame cap (~5x the earlier feel). --speed=N

    // Sleep (睡觉) mode.
    bool  sleep_enabled = true;
    bool  force_sleep = false;         // --sleep: ignore the clock, always sleep mode
    int   sleep_start_min = 23 * 60 + 30;  // 23:30
    int   sleep_end_min   = 6 * 60;        // 06:00 (wraps past midnight)
    float sleep_size_mult = 2.0f;      // grow up to 2x base
    bool  in_sleep = false;
    int   sleep_seconds = 0;

    HINSTANCE hInst = nullptr;
    HWND ctrl = nullptr;             // hidden message-only-ish window for timers/hotkey
    ActivityDetector* act = nullptr;
    std::vector<Mosquito> mos;
    std::vector<MosView> views;   // index-aligned with mos
    int work_seconds = 0;
    int suppress_mon = -1;
    bool rendering = false;
    float cur_prog = 0.0f;     // sleep-mode urgency 0..1 (drives the tray icon)
    int   idle_s = 0;          // seconds since last input (for the sedentary ring)
    float battery = 1.0f;      // sedentary "battery": work drains, rest charges (one ring)
    int   overwork_s = 0;      // active seconds since depletion (drives mosquito escalation)
    int   rest_s = 0;          // accumulated stillness while depleted (paused, not reset, by a touch)
    int   active_run_s = 0;    // consecutive active seconds while depleted (voids rest if sustained)
    bool  depleted = false;    // once empty, mosquitoes stay until a full rest clears them
    bool  working = false;     // a work session is running → drain continuously (wall-clock)
    int   start_run_s = 0;     // consecutive active seconds at full, toward starting a work session
    int dismiss_N = 100;       // chars to transcribe; grows with each dismissal
    int dismiss_N_base = 100;  // configured starting value (reset to this each night)
    int dismiss_count = 0;     // dismissals so far tonight (escalating penalty)
    bool escalate_penalty = true;
    bool sound_enabled = true; // the buzzing
    DWORD sed_pause_until = 0; // GetTickCount deadline; sedentary reminders snoozed until then

    bool is_sed_paused() { return sed_pause_until && GetTickCount() < sed_pause_until; }
    void pause_sed(int minutes) { sed_pause_until = GetTickCount() + (DWORD)minutes * 60000; work_seconds = 0; }

    // Reset the sedentary energy model to a clean full-battery state and clear any
    // swarm. Used when resuming from a pause and by "skip this rest" — so you never
    // come back to a mid-depletion state with mosquitoes waiting to pop out again.
    void refresh_full() {
        battery = 1.0f; depleted = false; working = false;
        start_run_s = 0; overwork_s = 0; rest_s = 0; active_run_s = 0;
        clear_swarm();
    }
    void resume_sed() { sed_pause_until = 0; work_seconds = 0; refresh_full(); }

    void update_sound() {
        if (!sound_enabled || !rendering || mos.empty()) { sound_set_volume(0.0f); return; }
        // Sedentary: a quiet hint. Sleep: grows with urgency.
        sound_set_volume(in_sleep ? (0.20f + 0.80f * cur_prog) : 0.15f);
    }

    void log(const char* msg) { if (fast) printf("[%5ds] %s\n", work_seconds, msg); }

    void clear_swarm() {
        for (auto& v : views) destroy_mosview(v);
        views.clear(); mos.clear();
        set_render(false);
    }
    void dismiss_success() {                 // called when the transcription completes
        clear_swarm();
        sleep_seconds = -cfg.sleep_return_s; // they come back after the return delay
        dismiss_count++;
        if (escalate_penalty) { dismiss_N += 100; if (dismiss_N > 1000) dismiss_N = 1000; }
        log("dismissed; swarm cleared, will return");
    }

    // Per-monitor sedentary target: the configured count applies to each display.
    int sedentary_count() {
        if (demo) return cfg.max_sedentary;
        if (act->state() != ActivityDetector::WORKING) return 0;
        if (work_seconds < cfg.work_threshold_s) return 0;
        int n = 1 + (work_seconds - cfg.work_threshold_s) / cfg.escalate_every_s;
        return n > cfg.max_sedentary ? cfg.max_sedentary : n;
    }

    // Is it bedtime? (--sleep forces it; --demo is a sedentary demo.)
    bool sleep_now() {
        if (force_sleep) return true;
        if (demo || !sleep_enabled) return false;
        SYSTEMTIME st; GetLocalTime(&st);
        int cur = st.wHour * 60 + st.wMinute;
        int a = sleep_start_min, b = sleep_end_min;
        return (a < b) ? (cur >= a && cur < b) : (cur >= a || cur < b);
    }

    void set_render(bool on) {
        if (on == rendering) return;
        rendering = on;
        if (on) SetTimer(ctrl, ID_RENDER, 42, nullptr);   // ~24 fps
        else    KillTimer(ctrl, ID_RENDER);
    }

    void apply_visibility() {
        for (size_t i = 0; i < views.size(); ++i) {
            bool want = (mos[i].mon != suppress_mon);
            if (want != views[i].shown) {
                ShowWindow(views[i].hwnd, want ? SW_SHOWNOACTIVATE : SW_HIDE);
                views[i].shown = want;
            }
        }
    }

    void spawn_on(int mon) {
        const RECT& r = g_monitors[mon];
        mos.push_back(spawn_mosquito(r.left, r.top, r.right - r.left, r.bottom - r.top, base_size));
        mos.back().mon = mon;
        views.push_back(create_mosview(hInst));
        render_mosview(views.back(), mos.back());   // position before first show
        log("+1 mosquito");
    }
    void remove_at(int i) {                          // swap-remove, keeping alignment
        destroy_mosview(views[i]);
        views[i] = views.back(); views.pop_back();
        mos[i] = mos.back();     mos.pop_back();
    }

    void logic_tick() {
        act->tick();
        if (act->take_started_working()) { work_seconds = 0; log("-> WORKING (new round)"); }
        if (act->take_started_resting()) { log("-> RESTING (break, clearing)"); }
        // Advance the work clock only while actually WORKING and not idle-away, so
        // leaving mid-session (e.g. lunch at minute 20) never marches it to 40.
        LASTINPUTINFO lii{ sizeof(lii), 0 };
        GetLastInputInfo(&lii);
        idle_s = (int)((GetTickCount() - lii.dwTime) / 1000);
        int away_grace = fast ? 2 : 60;
        if (act->state() == ActivityDetector::WORKING && idle_s < away_grace) work_seconds++;

        // Choose mode; compute the per-monitor target and the current sprite size.
        bool sl = sleep_now();
        if (sl != in_sleep) {
            in_sleep = sl; sleep_seconds = 0;
            if (sl) { dismiss_count = 0; dismiss_N = dismiss_N_base; }   // fresh night, reset penalty
            log(sl ? "== SLEEP mode ==" : "== SEDENTARY mode ==");
        }

        int want; float size;
        if (sl) {
            sleep_seconds++;
            if (sleep_seconds <= 0) {           // still in the post-dismissal grace period
                want = 0; size = base_size; cur_prog = 0.0f;
            } else {
                int gc = 1 + sleep_seconds / cfg.sleep_growth_s;
                want = gc > cfg.sleep_max ? cfg.sleep_max : gc;
                float prog = (float)sleep_seconds / (float)(cfg.sleep_growth_s * cfg.sleep_max);
                if (prog > 1.0f) prog = 1.0f;
                cur_prog = prog;
                size = base_size * (1.0f + (sleep_size_mult - 1.0f) * prog);
            }
        } else {
            // Simple energy model:
            //  • Only *operating* (input within `gap`) drains energy; idle just holds it —
            //    so after any refill it stays full until you start operating again.
            //  • WORK phase: 5 min of continuous stillness → energy instantly full.
            //  • Once empty, mosquitoes appear; operating spawns MORE, and stillness
            //    accumulates (paused, not reset, by a stray touch) — 5 min total → clear + full.
            size = base_size; cur_prog = 0.0f;
            int gap = fast ? 2 : 3;                 // "operating" if input within this many secs
            bool active = idle_s < gap;
            if (is_sed_paused()) {
                want = 0;                            // snoozed
            } else if (!depleted) {
                int start_need = fast ? 3 : 10;      // "start work" = this many seconds of continuous input
                if (!working) {
                    // At full energy: only a sustained run of activity starts the work session.
                    if (active) { if (++start_run_s >= start_need) { working = true; start_run_s = 0; } }
                    else start_run_s = 0;
                    // battery holds full until the session actually starts
                } else if (idle_s >= cfg.work_break_s) {
                    battery = 1.0f; working = false; start_run_s = 0;         // 15-min break → full, off the clock
                } else {
                    battery -= 1.0f / (float)cfg.work_threshold_s;            // fixed continuous drain (wall-clock)
                    if (battery <= 0.0f) { battery = 0.0f; depleted = true; overwork_s = 0; rest_s = 0; }
                }
                want = 0;
            } else {
                if (active) {
                    overwork_s++;                                             // operating → more mosquitoes
                    int void_s = fast ? 3 : 10;
                    if (++active_run_s >= void_s) rest_s = 0;                 // sustained 10s = not resting → void
                } else {
                    active_run_s = 0;
                    if (++rest_s >= cfg.rest_window_s) {                      // enough total stillness
                        battery = 1.0f; depleted = false; working = false; overwork_s = 0; rest_s = 0;  // clear + full
                    }
                }
                want = depleted ? (1 + overwork_s / cfg.escalate_every_s) : 0;
                if (want > cfg.max_sedentary) want = cfg.max_sedentary;
            }
        }

        enumerate_monitors();
        int M = (int)g_monitors.size();
        for (auto& m : mos) if (m.mon >= M) m.mon = M - 1;   // display unplugged

        for (int mi = 0; mi < M; ++mi) {
            int cnt = 0;
            for (auto& m : mos) if (m.mon == mi) ++cnt;
            while (cnt < want) { spawn_on(mi); ++cnt; }
            while (cnt > want) {
                for (int i = (int)mos.size() - 1; i >= 0; --i)
                    if (mos[i].mon == mi) { remove_at(i); break; }
                --cnt;
            }
        }

        for (auto& m : mos) m.size = size;   // sleep mode grows them over time

        suppress_mon = fullscreen_monitor_index();
        apply_visibility();
        bool any_visible = false;
        for (auto& m : mos) if (m.mon != suppress_mon) { any_visible = true; break; }
        set_render(any_visible);
        update_tray();
        update_sound();
    }

    void render_tick() {
        int M = (int)g_monitors.size();
        // Sleep mode: mosquitoes speed up with bedtime urgency, 1x → 2x by the end.
        float spd = in_sleep ? max_speed * (1.0f + cur_prog) : max_speed;
        for (size_t i = 0; i < mos.size(); ++i) {
            if (mos[i].mon == suppress_mon) continue;         // hidden: don't animate
            int mon = mos[i].mon < M ? mos[i].mon : M - 1;
            const RECT& r = g_monitors[mon];
            update_mosquito(mos[i], r.left, r.top, r.right - r.left, r.bottom - r.top, spd);
            render_mosview(views[i], mos[i]);
        }
    }
};

static App g_app;
static HICON g_win_icon = nullptr;   // the exe icon, for dialog title bars

// ----------------------- transcription text (language-aware) ----------------
//
// Chinese mode transcribes 《滕王阁序》 (Han characters); English mode transcribes
// a different English article (letters). The text is loaded from an external file
// next to the exe (tengwang.txt / english.txt) if present, else from the copy
// embedded in the exe (RCDATA 101 / 102), else a short built-in fallback. The
// external file always overrides, so the exact text stays editable.

static std::wstring g_text;

// Normalize per language: 中文 keeps Han only; English keeps letters, lowercased,
// with runs of any non-letters collapsed to a single space (natural word gaps).
static std::wstring filter_text(const std::wstring& s) {
    std::wstring o;
    if (g_lang == 0) {
        for (wchar_t c : s) if (c >= 0x4E00 && c <= 0x9FFF) o += c;
    } else {
        bool gap = false;
        for (wchar_t c : s) {
            bool up = (c >= 'A' && c <= 'Z'), lo = (c >= 'a' && c <= 'z');
            if (up || lo) {
                if (gap && !o.empty()) o += L' ';
                gap = false;
                o += (wchar_t)(up ? c - 'A' + 'a' : c);
            } else {
                gap = true;
            }
        }
    }
    return o;
}

static bool read_utf8(const char* data, int sz, std::wstring& text) {
    if (!data || sz <= 0) return false;
    int off = (sz >= 3 && (unsigned char)data[0] == 0xEF) ? 3 : 0;   // strip BOM
    int wl = MultiByteToWideChar(CP_UTF8, 0, data + off, sz - off, nullptr, 0);
    if (wl <= 0) return false;
    text.resize(wl);
    MultiByteToWideChar(CP_UTF8, 0, data + off, sz - off, &text[0], wl);
    return true;
}

static void load_text() {
    const wchar_t* fname = g_lang ? L"english.txt" : L"tengwang.txt";
    int resid = g_lang ? 102 : 101;

    wchar_t path[MAX_PATH];
    GetModuleFileName(nullptr, path, MAX_PATH);
    std::wstring p = path;
    size_t pos = p.find_last_of(L"\\/");
    if (pos != std::wstring::npos) p = p.substr(0, pos + 1);
    p += fname;

    std::wstring text;
    HANDLE hf = CreateFile(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (hf != INVALID_HANDLE_VALUE) {
        DWORD sz = GetFileSize(hf, nullptr), rd = 0;
        std::string buf(sz, 0);
        if (sz) ReadFile(hf, &buf[0], sz, &rd, nullptr);
        CloseHandle(hf);
        read_utf8(buf.data(), (int)buf.size(), text);
    }
    // No external file? Fall back to the copy embedded in the exe, so a bare
    // AngryMoz.exe still has a proper transcription. The external file overrides.
    if (text.empty()) {
        HRSRC hr = FindResource(nullptr, MAKEINTRESOURCE(resid), RT_RCDATA);
        if (hr) {
            HGLOBAL hg = LoadResource(nullptr, hr);
            read_utf8((const char*)LockResource(hg), (int)SizeofResource(nullptr, hr), text);
        }
    }
    if (text.empty())
        text = g_lang
               ? L"Four score and seven years ago our fathers brought forth on this "
                 L"continent a new nation conceived in liberty and dedicated to the "
                 L"proposition that all men are created equal"
               : L"豫章故郡洪都新府星分翼轸地接衡庐襟三江而带五湖控蛮荆而引瓯越"
                 L"物华天宝龙光射牛斗之墟人杰地灵徐孺下陈蕃之榻雄州雾列俊采星驰";
    g_text = filter_text(text);
}

// ----------------------- dismissal dialog (default) -------------------------

static const int IDC_EDIT = 301;

struct DismissDlg {
    HWND hwnd = nullptr, edit = nullptr, prompt = nullptr, prog = nullptr;
    HFONT font = nullptr, bigfont = nullptr;
    int start = 0, N = 0;
    bool open = false;
} g_dlg;

static int common_prefix(const std::wstring& a, const std::wstring& b) {
    int n = 0;
    while (n < (int)a.size() && n < (int)b.size() && a[n] == b[n]) ++n;
    return n;
}

// The edit box must be typed by hand — no pasting past the transcription.
static WNDPROC g_edit_orig = nullptr;
static LRESULT CALLBACK EditNoPaste(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_PASTE || m == WM_CONTEXTMENU) return 0;   // blocks Ctrl+V / Shift+Ins / right-click paste
    return CallWindowProc(g_edit_orig, h, m, w, l);
}

static void update_progress() {
    int len = GetWindowTextLength(g_dlg.edit);
    std::wstring buf(len + 1, 0);
    if (len) GetWindowText(g_dlg.edit, &buf[0], len + 1);
    buf.resize(len);
    std::wstring han = filter_text(buf);
    std::wstring target = g_text.substr(g_dlg.start, g_dlg.N);
    int matched = common_prefix(han, target);
    wchar_t s[64];
    wsprintf(s, T(L"已正确 %d / %d 字", L"Correct %d / %d"), matched, g_dlg.N);
    SetWindowText(g_dlg.prog, s);
    if (matched >= g_dlg.N) DestroyWindow(g_dlg.hwnd);   // WM_NCDESTROY commits it
}

static LRESULT CALLBACK DlgProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_COMMAND:
        if (LOWORD(w) == IDC_EDIT && HIWORD(w) == EN_CHANGE) update_progress();
        return 0;
    case WM_CLOSE:                         // cancelled: the swarm stays
        DestroyWindow(h);
        return 0;
    case WM_DESTROY: {
        std::wstring target = g_text.substr(g_dlg.start, g_dlg.N);
        int len = GetWindowTextLength(g_dlg.edit);
        std::wstring buf(len + 1, 0);
        if (len) GetWindowText(g_dlg.edit, &buf[0], len + 1);
        buf.resize(len);
        bool done = common_prefix(filter_text(buf), target) >= g_dlg.N;
        if (done) g_app.dismiss_success();
        return 0;
    }
    case WM_NCDESTROY:
        g_dlg.open = false; g_dlg.hwnd = nullptr;
        if (g_dlg.font)    { DeleteObject(g_dlg.font);    g_dlg.font = nullptr; }
        if (g_dlg.bigfont) { DeleteObject(g_dlg.bigfont); g_dlg.bigfont = nullptr; }
        return 0;
    }
    return DefWindowProc(h, m, w, l);
}

static void open_dismiss() {
    if (g_dlg.open) { SetForegroundWindow(g_dlg.hwnd); return; }
    load_text();                          // reload so the text matches the current language
    if ((int)g_text.size() < 20) return;

    int N = g_app.dismiss_N;
    if (N > (int)g_text.size() - 1) N = (int)g_text.size() - 1;
    int maxstart = (int)g_text.size() - N;
    if (maxstart < 1) maxstart = 1;
    g_dlg.start = rand() % maxstart;      // N <= chars remaining to the end, by construction
    g_dlg.N = N;

    static bool reg = false;
    if (!reg) {
        WNDCLASSEX wc{ sizeof(wc) };
        wc.lpfnWndProc = DlgProc;
        wc.hInstance = g_app.hInst;
        wc.lpszClassName = L"MosDismiss";
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hIcon = g_win_icon; wc.hIconSm = g_win_icon;
        RegisterClassEx(&wc);
        reg = true;
    }

    int W = 780, H = 600;
    int sx = (GetSystemMetrics(SM_CXSCREEN) - W) / 2, sy = (GetSystemMetrics(SM_CYSCREEN) - H) / 2;
    g_dlg.hwnd = CreateWindowEx(WS_EX_TOPMOST, L"MosDismiss", T(L"驱散蚊子 — 默写《滕王阁序》", L"Dismiss — transcription"),
                                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, sx, sy, W, H,
                                nullptr, nullptr, g_app.hInst, nullptr);
    g_dlg.font    = CreateFont(20, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Microsoft YaHei");
    g_dlg.bigfont = CreateFont(28, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Microsoft YaHei");

    int ctxlen = g_dlg.start < 12 ? g_dlg.start : 12;
    std::wstring ctx = g_text.substr(g_dlg.start - ctxlen, ctxlen);
    std::wstring pm = std::wstring(T(L"提示：…", L"Hint: …")) + ctx + L"\r\n"
                      + T(L"请接着往下默写 ", L"Keep typing the next ") + std::to_wstring(N)
                      + T(L" 个字（睡吧，别熬了）", L" characters (just go to sleep~)");
    g_dlg.prompt = CreateWindow(L"STATIC", pm.c_str(), WS_CHILD | WS_VISIBLE, 20, 16, W - 56, 64, g_dlg.hwnd, nullptr, g_app.hInst, nullptr);
    g_dlg.edit   = CreateWindowEx(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL,
                                  20, 88, W - 56, 430, g_dlg.hwnd, (HMENU)(INT_PTR)IDC_EDIT, g_app.hInst, nullptr);
    g_dlg.prog   = CreateWindow(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 20, 528, W - 56, 30, g_dlg.hwnd, nullptr, g_app.hInst, nullptr);
    g_edit_orig = (WNDPROC)SetWindowLongPtr(g_dlg.edit, GWLP_WNDPROC, (LONG_PTR)EditNoPaste);
    SendMessage(g_dlg.prompt, WM_SETFONT, (WPARAM)g_dlg.font, TRUE);
    SendMessage(g_dlg.edit,   WM_SETFONT, (WPARAM)g_dlg.bigfont, TRUE);
    SendMessage(g_dlg.prog,   WM_SETFONT, (WPARAM)g_dlg.font, TRUE);

    g_dlg.open = true;
    update_progress();
    ShowWindow(g_dlg.hwnd, SW_SHOW);
    SetForegroundWindow(g_dlg.hwnd);
    SetFocus(g_dlg.edit);
}

// ----------------------- tray icon (B: ring + mosquito) ---------------------

static NOTIFYICONDATA g_nid{};
static HICON g_tray_full = nullptr, g_tray_dim = nullptr;
static bool  g_tray_lit = true;
static int   g_tray_stage = -99;

enum { TRAY_STEADY = 0, TRAY_BREATHE = 1, TRAY_SLEEPBLINK = 2 };
static int    g_tray_mode = TRAY_STEADY;
static float  g_tray_prog = 0.0f;
static Color  g_tray_col(255, 150, 150, 150);
static float  g_breathe_phase = 0.0f;
static HICON  g_breathe_icon = nullptr;

static Color stage_color(int stg) {
    switch (stg) {
    case 0: return Color(255, 25, 195, 214);   // teal
    case 1: return Color(255, 142, 209, 30);   // green
    case 2: return Color(255, 255, 176, 32);   // amber
    case 3: return Color(255, 255, 122, 26);   // orange
    case 4: return Color(255, 229, 72, 77);    // red
    default: return Color(255, 150, 150, 150); // idle gray
    }
}
// Blink half-period in ms: low urgency = slow/calm, high urgency = fast/urgent.
// Must never return 0 — SetTimer clamps 0 to ~10ms, which strobes (the early-sleep
// "fast flash" bug). Stage 0 is the calmest slow blink.
static int blink_half_ms(int stg) {
    switch (stg) { case 0: return 1500; case 1: return 1000; case 2: return 500; case 3: return 250; case 4: return 125; default: return 1500; }
}

static Bitmap* g_face = nullptr;   // the user's mosquito artwork (embedded PNG)

static void load_face(HINSTANCE hInst) {
    HRSRC r = FindResource(hInst, MAKEINTRESOURCE(100), RT_RCDATA);
    if (!r) return;
    HGLOBAL h = LoadResource(hInst, r);
    void* data = LockResource(h);
    DWORD sz = SizeofResource(hInst, r);
    if (!data || !sz) return;
    IStream* s = SHCreateMemStream((const BYTE*)data, sz);
    if (s) { g_face = Bitmap::FromStream(s); s->Release(); }
}

static HICON draw_tray_icon(float prog, Color col, float alpha) {
    Bitmap bmp(32, 32, PixelFormat32bppARGB);
    Graphics g(&bmp);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    g.Clear(Color(0, 0, 0, 0));
    BYTE A = (BYTE)(alpha * 255);

    // The user's mosquito face on a transparent background (alpha for blink/breathing).
    if (g_face) {
        ColorMatrix cm = { 1,0,0,0,0, 0,1,0,0,0, 0,0,1,0,0, 0,0,0,alpha,0, 0,0,0,0,1 };
        ImageAttributes ia; ia.SetColorMatrix(&cm);
        Rect dst(2, 2, 28, 28);
        g.DrawImage(g_face, dst, 0, 0, (INT)g_face->GetWidth(), (INT)g_face->GetHeight(), UnitPixel, &ia);
    }
    // State ring on the outer edge (battery / urgency).
    Pen track(Color((BYTE)(alpha * 110), 90, 95, 102), 3.0f);
    g.DrawEllipse(&track, 3.0f, 3.0f, 26.0f, 26.0f);
    if (prog > 0.001f) {
        Pen arc(Color(A, col.GetR(), col.GetG(), col.GetB()), 3.0f);
        arc.SetStartCap(LineCapRound); arc.SetEndCap(LineCapRound);
        g.DrawArc(&arc, 3.0f, 3.0f, 26.0f, 26.0f, -90.0f, prog * 360.0f);
    }
    // "Update available" badge — a small red dot, top-right, always at full opacity
    // (stays visible through blink/breathe) so it reads as a persistent notification.
    if (g_update_ready) {
        SolidBrush dot(Color(255, 229, 60, 60));
        Pen ring(Color(255, 255, 255, 255), 1.5f);
        g.FillEllipse(&dot, 20.0f, 1.0f, 10.0f, 10.0f);
        g.DrawEllipse(&ring, 20.0f, 1.0f, 10.0f, 10.0f);
    }
    HICON ic = nullptr;
    bmp.GetHICON(&ic);
    return ic;
}

static int sleep_stage(float p) {
    if (p < 0.05f) return 0;
    if (p < 0.30f) return 1;
    if (p < 0.55f) return 2;
    if (p < 0.80f) return 3;
    return 4;
}

static Color lerp_color(Color a, Color b, float t) {
    if (t < 0) t = 0; if (t > 1) t = 1;
    return Color(255,
        (BYTE)(a.GetR() + (b.GetR() - a.GetR()) * t),
        (BYTE)(a.GetG() + (b.GetG() - a.GetG()) * t),
        (BYTE)(a.GetB() + (b.GetB() - a.GetB()) * t));
}

static void set_tray_icon(HICON ic, const wchar_t* tip) {
    g_nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    g_nid.hIcon = ic;
    lstrcpyn(g_nid.szTip, tip, (int)(sizeof(g_nid.szTip) / sizeof(wchar_t)));
    Shell_NotifyIcon(NIM_MODIFY, &g_nid);
}

static void update_tray() {
    const Color GREEN(255, 142, 209, 30), RED(255, 229, 72, 77), GRAY(255, 150, 150, 150);
    float arc = 0.0f;              // ring length 0..1
    Color col = GRAY;
    int stg = -1, mode = TRAY_STEADY;
    wchar_t tip[128];

    if (g_app.in_sleep && !g_app.mos.empty()) {
        // Sleep: urgency ring — fills up, color + blink escalate.
        stg = sleep_stage(g_app.cur_prog);
        arc = g_app.cur_prog; col = stage_color(stg); mode = TRAY_SLEEPBLINK;
        wsprintf(tip, T(L"愤怒的蚊子 · 睡觉 紧迫度 %d%%", L"AngryMoz · Bedtime urgency %d%%"), (int)(arc * 100));
    } else if (g_app.in_sleep) {
        lstrcpyn(tip, T(L"愤怒的蚊子 · 睡觉待命", L"AngryMoz · Bedtime standby"), 128);
    } else if (g_app.is_sed_paused()) {
        int left = (int)((g_app.sed_pause_until - GetTickCount()) / 60000) + 1;
        wsprintf(tip, T(L"愤怒的蚊子 · 久坐已暂停（约 %d 分钟）", L"AngryMoz · Sedentary paused (~%d min)"), left);
    } else {
        // One battery ring: arc = charge, color red(empty)→green(full). Mosquitoes (if
        // any) stay while it recharges; the ring keeps showing the true charge level.
        float b = g_app.battery;
        arc = b; col = lerp_color(RED, GREEN, b);
        int gap = g_app.fast ? 2 : 3;
        bool active = g_app.idle_s < gap;
        // No pulse in sedentary mode: the ring stays steady (empty when depleted —
        // the on-screen mosquitoes already signal it's rest time).
        if (g_app.depleted) {
            if (active) {
                lstrcpyn(tip, T(L"愤怒的蚊子 · 该起来！再动=更多蚊子", L"AngryMoz · Get up! more input = more mosquitoes"), 128);
            } else {
                int need = (int)std::ceil((g_app.cfg.rest_window_s - g_app.rest_s) / 60.0f);
                if (need < 1) need = 1;
                wsprintf(tip, T(L"愤怒的蚊子 · 休息中 · 再静止 %d 分钟清空蚊子", L"AngryMoz · Resting · %d more still min to clear"), need);
            }
        } else if (!g_app.working) {
            int start_need = g_app.fast ? 3 : 10;
            if (g_app.start_run_s > 0) {
                int left = start_need - g_app.start_run_s; if (left < 0) left = 0;
                wsprintf(tip, T(L"愤怒的蚊子 · 识别开工中… 再连续操作 %d 秒", L"AngryMoz · Detecting work… %d more sec"), left);
            } else {
                lstrcpyn(tip, T(L"愤怒的蚊子 · 精力已满（连续操作开始工作）", L"AngryMoz · Full (keep using to start)"), 128);
            }
        } else if (g_app.idle_s >= gap) {                          // paused during a work session
            int need = (int)std::ceil((g_app.cfg.work_break_s - g_app.idle_s) / 60.0f);
            if (need < 1) need = 1;
            wsprintf(tip, T(L"愤怒的蚊子 · 已停手 · 再 %d 分钟不动回满（否则继续消耗）", L"AngryMoz · Idle · %d min still to refill (else draining)"), need);
        } else {
            int rem = (int)std::ceil(b * g_app.cfg.work_threshold_s / 60.0f);
            if (rem < 1) rem = 1;
            wsprintf(tip, T(L"愤怒的蚊子 · 距出现蚊子约 %d 分钟", L"AngryMoz · ~%d min to mosquitoes"), rem);
        }
    }

    g_tray_prog = arc; g_tray_col = col;
    if (g_tray_full) DestroyIcon(g_tray_full);
    if (g_tray_dim)  DestroyIcon(g_tray_dim);
    g_tray_full = draw_tray_icon(arc, col, 1.0f);
    g_tray_dim  = draw_tray_icon(arc, col, 0.28f);

    // (Re)arm the animation timer only when the mode (or sleep stage) changes.
    if (mode != g_tray_mode || (mode == TRAY_SLEEPBLINK && stg != g_tray_stage)) {
        KillTimer(g_app.ctrl, ID_BLINK);
        if (mode == TRAY_SLEEPBLINK) SetTimer(g_app.ctrl, ID_BLINK, blink_half_ms(stg), nullptr);
        else if (mode == TRAY_BREATHE) SetTimer(g_app.ctrl, ID_BLINK, 800, nullptr);  // slow rest pulse
        g_tray_lit = true;
    }
    g_tray_mode = mode; g_tray_stage = stg;

    // While breathing, the ID_BLINK timer owns the icon — don't overwrite it each
    // second (that caused the stutter). Just refresh the tooltip text.
    if (mode == TRAY_BREATHE) lstrcpyn(g_nid.szTip, tip, 128);
    else set_tray_icon(g_tray_full, tip);
}

// ----------------------- buzzing (streamed, non-repeating) ------------------
//
// A real mosquito whine: a harmonic-rich (reedy) tone around 500-700 Hz whose
// pitch constantly slides as it manoeuvres, never a steady beep. We stream it
// with double-buffering and keep evolving the pitch, so it never loops audibly.

static const int  SR = 22050;
static const int  CHUNK = 1103;          // ~50 ms per buffer
static const int  NBUF = 3;
static HWAVEOUT   g_wo = nullptr;
static WAVEHDR    g_hdr[NBUF];
static std::vector<short> g_buf[NBUF];
static volatile bool g_snd_run = false;

static double g_phase = 0.0, g_freq = 480.0, g_ftarget = 500.0, g_flut = 0.0;

static void gen_chunk(short* out, int n) {
    const double PI = 3.14159265358979;
    // Steep harmonic rolloff = rounder / more muffled (less piercing).
    static const double W[8] = { 1.0, 0.45, 0.22, 0.11, 0.05, 0.0, 0.0, 0.0 };
    // Occasionally aim the pitch somewhere new — that restless wandering whine.
    if ((rand() % 3) == 0) g_ftarget = 420.0 + (rand() % 200);
    for (int i = 0; i < n; ++i) {
        g_freq += (g_ftarget - g_freq) * 0.0007;                 // glide toward target
        double jit = 1.0 + 0.0010 * (((rand() % 2000) / 1000.0) - 1.0);
        g_phase = std::fmod(g_phase + 2 * PI * g_freq * jit / SR, 2 * PI);
        double s = 0.0;
        for (int k = 0; k < 8; ++k) s += W[k] * std::sin((k + 1) * g_phase);
        g_flut = std::fmod(g_flut + 2 * PI * 11.0 / SR, 2 * PI);
        double env = 0.82 + 0.18 * std::sin(g_flut);             // subtle wing-beat swell
        double v = s * env * 0.19;
        if (v > 1) v = 1; if (v < -1) v = -1;
        out[i] = (short)(v * 32767);
    }
}

static void CALLBACK sndCB(HWAVEOUT, UINT m, DWORD_PTR, DWORD_PTR p1, DWORD_PTR) {
    if (m == WOM_DONE && g_snd_run) {
        WAVEHDR* h = (WAVEHDR*)p1;
        gen_chunk((short*)h->lpData, h->dwBufferLength / 2);
        waveOutWrite(g_wo, h, sizeof(WAVEHDR));
    }
}

static void sound_init() {
    WAVEFORMATEX f{};
    f.wFormatTag = WAVE_FORMAT_PCM; f.nChannels = 1; f.nSamplesPerSec = SR;
    f.wBitsPerSample = 16; f.nBlockAlign = 2; f.nAvgBytesPerSec = SR * 2;
    if (waveOutOpen(&g_wo, WAVE_MAPPER, &f, (DWORD_PTR)sndCB, 0, CALLBACK_FUNCTION) != MMSYSERR_NOERROR) { g_wo = nullptr; return; }
    g_snd_run = true;
    for (int b = 0; b < NBUF; ++b) {
        g_buf[b].assign(CHUNK, 0);
        g_hdr[b] = {};
        g_hdr[b].lpData = (LPSTR)g_buf[b].data();
        g_hdr[b].dwBufferLength = CHUNK * 2;
        waveOutPrepareHeader(g_wo, &g_hdr[b], sizeof(WAVEHDR));
        gen_chunk((short*)g_hdr[b].lpData, CHUNK);
        waveOutWrite(g_wo, &g_hdr[b], sizeof(WAVEHDR));
    }
    waveOutSetVolume(g_wo, 0);   // silent until update_sound raises it
}

static void sound_set_volume(float v) {
    if (!g_wo) return;
    if (v < 0) v = 0; if (v > 1) v = 1;
    DWORD w = (DWORD)(v * 0xFFFF);
    waveOutSetVolume(g_wo, w | (w << 16));
}

static void sound_close() {
    if (!g_wo) return;
    g_snd_run = false;
    waveOutReset(g_wo);
    for (int b = 0; b < NBUF; ++b) waveOutUnprepareHeader(g_wo, &g_hdr[b], sizeof(WAVEHDR));
    waveOutClose(g_wo);
    g_wo = nullptr;
}

// ----------------------- autostart (HKCU Run) -------------------------------

static const wchar_t* RUN_KEY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

static bool is_autostart() {
    HKEY k;
    if (RegOpenKeyEx(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS) return false;
    bool has = RegQueryValueEx(k, L"Mosquito", nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
    RegCloseKey(k);
    return has;
}

static void set_autostart(bool on) {
    HKEY k;
    if (RegCreateKeyEx(HKEY_CURRENT_USER, RUN_KEY, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS) return;
    if (on) {
        wchar_t path[MAX_PATH]; GetModuleFileName(nullptr, path, MAX_PATH);
        std::wstring q = L"\""; q += path; q += L"\"";
        RegSetValueEx(k, L"Mosquito", 0, REG_SZ, (const BYTE*)q.c_str(), (DWORD)((q.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValue(k, L"Mosquito");
    }
    RegCloseKey(k);
}

// ----------------------- update check (GitHub Releases) ---------------------
//
// Lightweight: one silent HTTPS GET on startup (a background thread that exits
// when done — no polling, no resident timer). If a newer release exists we flip
// g_update_ready, which paints a red dot on the tray icon and adds a "download"
// item to the menu. We never download or self-update; the user grabs the exe.

static const wchar_t* RELEASES_URL = L"https://github.com/YangJun233/AngryMoz/releases/latest";

static bool https_get_release(std::string& out) {
    bool ok = false;
    HINTERNET s = WinHttpOpen(L"AngryMoz/" APP_VER, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) return false;
    WinHttpSetTimeouts(s, 5000, 5000, 5000, 5000);
    HINTERNET c = WinHttpConnect(s, L"api.github.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (c) {
        HINTERNET r = WinHttpOpenRequest(c, L"GET", L"/repos/YangJun233/AngryMoz/releases/latest",
                                         nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        if (r) {
            WinHttpAddRequestHeaders(r, L"Accept: application/vnd.github+json\r\n", (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);
            if (WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
                    && WinHttpReceiveResponse(r, nullptr)) {
                DWORD avail = 0;
                do {
                    avail = 0;
                    if (!WinHttpQueryDataAvailable(r, &avail)) break;
                    if (avail) {
                        std::string buf(avail, 0); DWORD rd = 0;
                        if (WinHttpReadData(r, &buf[0], avail, &rd)) out.append(buf.data(), rd);
                    }
                } while (avail > 0);
                ok = !out.empty();
            }
            WinHttpCloseHandle(r);
        }
        WinHttpCloseHandle(c);
    }
    WinHttpCloseHandle(s);
    return ok;
}

static std::string parse_tag_name(const std::string& body) {
    size_t p = body.find("\"tag_name\"");
    if (p == std::string::npos) return "";
    p = body.find(':', p); if (p == std::string::npos) return "";
    p = body.find('"', p); if (p == std::string::npos) return "";
    size_t q = body.find('"', p + 1); if (q == std::string::npos) return "";
    return body.substr(p + 1, q - p - 1);
}

static bool ver_newer(const std::string& latest, const std::string& cur) {
    auto nextnum = [](const std::string& s, size_t& i) {
        int v = 0; while (i < s.size() && s[i] >= '0' && s[i] <= '9') v = v * 10 + (s[i++] - '0');
        if (i < s.size() && s[i] == '.') ++i; return v;
    };
    size_t ai = 0, bi = 0;
    for (int k = 0; k < 3; ++k) { int a = nextnum(latest, ai), b = nextnum(cur, bi); if (a != b) return a > b; }
    return false;
}

// 1 = update available, 0 = up to date, -1 = check failed. Sets g_update_* when newer.
static int do_update_check() {
    std::string body;
    if (!https_get_release(body)) return -1;
    std::string tag = parse_tag_name(body);
    if (tag.empty()) return -1;
    std::string latest = tag;
    if (!latest.empty() && (latest[0] == 'v' || latest[0] == 'V')) latest.erase(0, 1);
    if (!ver_newer(latest, APP_VER_STR)) return 0;
    int wl = MultiByteToWideChar(CP_UTF8, 0, tag.data(), (int)tag.size(), nullptr, 0);
    std::wstring w(wl, 0);
    if (wl > 0) MultiByteToWideChar(CP_UTF8, 0, tag.data(), (int)tag.size(), &w[0], wl);
    g_update_tag = w;
    g_update_ready = true;   // set last: the UI reads the tag only once this is true
    return 1;
}

static DWORD WINAPI update_thread(LPVOID) { do_update_check(); return 0; }

static void check_update_interactive(HWND owner) {
    HCURSOR old = SetCursor(LoadCursor(nullptr, IDC_WAIT));
    int r = do_update_check();
    SetCursor(old);
    if (r < 0) {
        MessageBox(owner, T(L"检查更新失败，请检查网络连接。", L"Update check failed — check your network."),
                   BRAND(), MB_OK | MB_ICONWARNING);
    } else if (r == 0) {
        MessageBox(owner, T(L"已是最新版本。", L"You're on the latest version."),
                   BRAND(), MB_OK | MB_ICONINFORMATION);
    } else {
        std::wstring msg = std::wstring(T(L"发现新版本 ", L"New version ")) + g_update_tag
                           + T(L"（当前 v" APP_VER L"）。\n是否打开下载页面？",
                               L" available (current v" APP_VER L").\nOpen the download page?");
        if (MessageBox(owner, msg.c_str(), BRAND(), MB_YESNO | MB_ICONINFORMATION) == IDYES)
            ShellExecute(nullptr, L"open", RELEASES_URL, nullptr, nullptr, SW_SHOWNORMAL);
    }
}

// ----------------------- settings (AngryMoz.ini + dialog) -------------------

struct Settings {
    int work_min = 40, rest_min = 5, sed_step_min = 1, sed_max = 5, work_break_min = 15;
    int sleep_enabled = 1, sleep_start = 23 * 60 + 30, sleep_end = 6 * 60, sleep_step_min = 3, sleep_max = 10;
    double sleep_mult = 2.0, base_size = 3.0, max_speed = 32.0;
    int dismiss_N = 100, return_min = 5, escalate_penalty = 1;
    int sound = 1, autostart = 0;
    int lang = 0;   // 0 = 中文, 1 = English
} g_settings;

static std::wstring ini_path() {
    wchar_t p[MAX_PATH]; GetModuleFileName(nullptr, p, MAX_PATH);
    std::wstring s = p; size_t q = s.find_last_of(L"\\/");
    if (q != std::wstring::npos) s = s.substr(0, q + 1);
    return s + L"AngryMoz.ini";
}

static void load_settings() {
    std::wstring p = ini_path(); const wchar_t* S = L"settings"; const wchar_t* f = p.c_str();
    Settings& g = g_settings;
    g.work_min       = GetPrivateProfileInt(S, L"work_min", g.work_min, f);
    g.rest_min       = GetPrivateProfileInt(S, L"rest_min", g.rest_min, f);
    g.sed_step_min   = GetPrivateProfileInt(S, L"sed_step_min", g.sed_step_min, f);
    g.sed_max        = GetPrivateProfileInt(S, L"sed_max", g.sed_max, f);
    g.work_break_min = GetPrivateProfileInt(S, L"work_break_min", g.work_break_min, f);
    g.sleep_enabled  = GetPrivateProfileInt(S, L"sleep_enabled", g.sleep_enabled, f);
    g.sleep_start    = GetPrivateProfileInt(S, L"sleep_start", g.sleep_start, f);
    g.sleep_end      = GetPrivateProfileInt(S, L"sleep_end", g.sleep_end, f);
    g.sleep_step_min = GetPrivateProfileInt(S, L"sleep_step_min", g.sleep_step_min, f);
    g.sleep_max      = GetPrivateProfileInt(S, L"sleep_max", g.sleep_max, f);
    g.dismiss_N      = GetPrivateProfileInt(S, L"dismiss_N", g.dismiss_N, f);
    g.return_min     = GetPrivateProfileInt(S, L"return_min", g.return_min, f);
    g.escalate_penalty = GetPrivateProfileInt(S, L"escalate_penalty", g.escalate_penalty, f);
    g.sound          = GetPrivateProfileInt(S, L"sound", g.sound, f);
    g.lang           = GetPrivateProfileInt(S, L"lang", g.lang, f);
    wchar_t b[64];
    GetPrivateProfileString(S, L"sleep_mult", L"2.0", b, 64, f); g.sleep_mult = wcstod(b, nullptr);
    GetPrivateProfileString(S, L"base_size",  L"3.0", b, 64, f); g.base_size  = wcstod(b, nullptr);
    GetPrivateProfileString(S, L"max_speed", L"32.0", b, 64, f); g.max_speed  = wcstod(b, nullptr);
    g.autostart = is_autostart() ? 1 : 0;
}

static void save_settings() {
    std::wstring p = ini_path(); const wchar_t* S = L"settings"; const wchar_t* f = p.c_str();
    Settings& g = g_settings; wchar_t b[64];
    auto wi = [&](const wchar_t* k, int v) { wsprintf(b, L"%d", v); WritePrivateProfileString(S, k, b, f); };
    wi(L"work_min", g.work_min); wi(L"rest_min", g.rest_min); wi(L"sed_step_min", g.sed_step_min); wi(L"sed_max", g.sed_max);
    wi(L"work_break_min", g.work_break_min);
    wi(L"sleep_enabled", g.sleep_enabled); wi(L"sleep_start", g.sleep_start); wi(L"sleep_end", g.sleep_end);
    wi(L"sleep_step_min", g.sleep_step_min); wi(L"sleep_max", g.sleep_max);
    wi(L"dismiss_N", g.dismiss_N); wi(L"return_min", g.return_min); wi(L"escalate_penalty", g.escalate_penalty);
    wi(L"sound", g.sound); wi(L"lang", g.lang);
    swprintf(b, 64, L"%.2f", g.sleep_mult); WritePrivateProfileString(S, L"sleep_mult", b, f);
    swprintf(b, 64, L"%.2f", g.base_size);  WritePrivateProfileString(S, L"base_size", b, f);
    swprintf(b, 64, L"%.2f", g.max_speed);  WritePrivateProfileString(S, L"max_speed", b, f);
}

static void apply_settings() {
    Settings& g = g_settings;
    Config c = make_config(g_app.fast);
    if (!g_app.fast) {              // dev --fast keeps its compressed timeline
        c.work_threshold_s = g.work_min * 60;
        c.escalate_every_s = g.sed_step_min * 60;
        c.max_sedentary    = g.sed_max;
        c.work_break_s     = g.work_break_min * 60;
        c.rest_window_s    = g.rest_min * 60;
        c.rest_max_active_s = g.rest_min * 60 * 5 / 100;
        c.sleep_growth_s   = g.sleep_step_min * 60;
        c.sleep_max        = g.sleep_max;
        c.sleep_return_s   = g.return_min * 60;
    }
    g_app.cfg = c;
    delete g_app.act; g_app.act = new ActivityDetector(g_app.cfg);
    g_app.sleep_enabled   = g.sleep_enabled != 0;
    g_app.sleep_start_min = g.sleep_start;
    g_app.sleep_end_min   = g.sleep_end;
    g_app.sleep_size_mult = (float)g.sleep_mult;
    g_app.base_size       = (float)g.base_size;
    g_app.max_speed       = (float)g.max_speed;
    g_app.sound_enabled   = g.sound != 0;
    g_lang                = g.lang;
    g_app.dismiss_N_base  = g.dismiss_N;
    g_app.dismiss_N       = g.dismiss_N;
    g_app.escalate_penalty = g.escalate_penalty != 0;
}

static const UINT IDSAVE = 400, IDCANC = 401, IDBTN_UPDATE = 402;

struct SettingsDlg {
    HWND hwnd = nullptr, work, wbrk, rest, sstep, smax;
    HWND sen, sstart, send, slstep, slmax, smult;
    HWND dN, ret, esc, base, spd, snd, autos;
    HFONT font = nullptr;
    bool open = false;
} g_set;

// Owner-drawn checkboxes (the classic glyph is tiny next to a big font). Each
// has a fixed control id; we track its checked state here.
static const int IDCK_SLEEP = 500, IDCK_ESC = 501, IDCK_SND = 502, IDCK_AUTO = 503, IDCK_LANG = 504;
static bool g_ck[5];
static std::vector<HWND> g_sleep_ctrls;   // grayed out when sleep mode is off

static int g_sc = 2;   // settings-dialog scale (everything laid out at 1x, drawn at g_sc)

static HWND s_label(HWND p, const wchar_t* t, int x, int y) {
    return CreateWindow(L"STATIC", t, WS_CHILD | WS_VISIBLE, x * g_sc, (y + 3) * g_sc, 140 * g_sc, 20 * g_sc, p, nullptr, g_app.hInst, nullptr);
}
static HWND s_edit(HWND p, const std::wstring& v, int x, int y) {
    return CreateWindowEx(WS_EX_CLIENTEDGE, L"EDIT", v.c_str(), WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, x * g_sc, y * g_sc, 92 * g_sc, 24 * g_sc, p, nullptr, g_app.hInst, nullptr);
}
static HWND s_check(HWND p, const wchar_t* t, int x, int y, bool on, int id) {
    if (id >= IDCK_SLEEP && id <= IDCK_AUTO) g_ck[id - IDCK_SLEEP] = on;
    return CreateWindow(L"BUTTON", t, WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                        x * g_sc, y * g_sc, 180 * g_sc, 26 * g_sc, p, (HMENU)(INT_PTR)id, g_app.hInst, nullptr);
}
static HWND s_group(HWND p, const wchar_t* t, int x, int y, int w, int h) {
    return CreateWindow(L"BUTTON", t, WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                        x * g_sc, y * g_sc, w * g_sc, h * g_sc, p, nullptr, g_app.hInst, nullptr);
}
static void set_sleep_enabled(bool on) {
    for (HWND w : g_sleep_ctrls) if (w) EnableWindow(w, on);
}
static int   g_int(HWND h) { wchar_t b[32]; GetWindowText(h, b, 32); return _wtoi(b); }
static double g_dbl(HWND h) { wchar_t b[32]; GetWindowText(h, b, 32); return wcstod(b, nullptr); }
static bool  g_chk(HWND h) { return SendMessage(h, BM_GETCHECK, 0, 0) == BST_CHECKED; }
static std::wstring hhmm(int m) { wchar_t b[8]; wsprintf(b, L"%02d:%02d", m / 60, m % 60); return b; }
static std::wstring fmt1(double v) { wchar_t b[16]; swprintf(b, 16, L"%.1f", v); return b; }
static int parse_hhmm(HWND h) { wchar_t b[16]; GetWindowText(h, b, 16); int hh = 0, mm = 0; swscanf(b, L"%d:%d", &hh, &mm); return (hh % 24) * 60 + (mm % 60); }

static LRESULT CALLBACK SetProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_DRAWITEM) {
        DRAWITEMSTRUCT* dis = (DRAWITEMSTRUCT*)l;
        int idx = (int)dis->CtlID - IDCK_SLEEP;
        if (idx >= 0 && idx < 5) {
            HDC dc = dis->hDC; RECT rc = dis->rcItem;
            bool en = !(dis->itemState & ODS_DISABLED);
            FillRect(dc, &rc, (HBRUSH)(COLOR_BTNFACE + 1));
            int ch = rc.bottom - rc.top, box = ch * 66 / 100;
            RECT br = { rc.left + 2, rc.top + (ch - box) / 2, rc.left + 2 + box, rc.top + (ch - box) / 2 + box };
            HPEN pen = CreatePen(PS_SOLID, 2, en ? RGB(110, 110, 110) : RGB(190, 190, 190));
            HPEN oldp = (HPEN)SelectObject(dc, pen);
            HBRUSH oldb = (HBRUSH)SelectObject(dc, GetStockObject(WHITE_BRUSH));
            Rectangle(dc, br.left, br.top, br.right, br.bottom);
            SelectObject(dc, oldb); SelectObject(dc, oldp); DeleteObject(pen);
            if (g_ck[idx]) {
                HPEN cp = CreatePen(PS_SOLID, 3, en ? RGB(40, 140, 50) : RGB(185, 185, 185));
                HPEN o2 = (HPEN)SelectObject(dc, cp);
                MoveToEx(dc, br.left + box * 22 / 100, br.top + box * 52 / 100, nullptr);
                LineTo(dc, br.left + box * 42 / 100, br.top + box * 72 / 100);
                LineTo(dc, br.left + box * 80 / 100, br.top + box * 26 / 100);
                SelectObject(dc, o2); DeleteObject(cp);
            }
            wchar_t txt[80]; GetWindowText(dis->hwndItem, txt, 80);
            SetBkMode(dc, TRANSPARENT); SetTextColor(dc, en ? RGB(20, 20, 20) : RGB(160, 160, 160));
            HFONT of = (HFONT)SelectObject(dc, g_set.font);
            RECT tr = { br.right + 12, rc.top, rc.right, rc.bottom };
            DrawText(dc, txt, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            SelectObject(dc, of);
            return TRUE;
        }
    }
    if (m == WM_COMMAND) {
        int cid = LOWORD(w);
        if (cid >= IDCK_SLEEP && cid <= IDCK_LANG && HIWORD(w) == BN_CLICKED) {
            g_ck[cid - IDCK_SLEEP] = !g_ck[cid - IDCK_SLEEP];
            InvalidateRect(GetDlgItem(h, cid), nullptr, TRUE);
            if (cid == IDCK_SLEEP) set_sleep_enabled(g_ck[0]);   // gray/ungray the sleep group
            return 0;
        }
        if (LOWORD(w) == IDSAVE) {
            Settings& g = g_settings;
            g.work_min = g_int(g_set.work); g.work_break_min = g_int(g_set.wbrk); g.rest_min = g_int(g_set.rest);
            g.sed_step_min = g_int(g_set.sstep); g.sed_max = g_int(g_set.smax);
            g.sleep_enabled = g_ck[0]; g.sleep_start = parse_hhmm(g_set.sstart);
            g.sleep_end = parse_hhmm(g_set.send); g.sleep_step_min = g_int(g_set.slstep);
            g.sleep_max = g_int(g_set.slmax); g.sleep_mult = g_dbl(g_set.smult);
            g.dismiss_N = g_int(g_set.dN); g.return_min = g_int(g_set.ret);
            g.escalate_penalty = g_ck[1];
            g.base_size = g_dbl(g_set.base); g.max_speed = g_dbl(g_set.spd);
            g.sound = g_ck[2]; g.autostart = g_ck[3]; g.lang = g_ck[4];
            save_settings(); apply_settings(); set_autostart(g.autostart != 0);
            DestroyWindow(h);
        } else if (LOWORD(w) == IDCANC) {
            DestroyWindow(h);
        } else if (LOWORD(w) == IDBTN_UPDATE) {
            check_update_interactive(h);
        }
        return 0;
    }
    if (m == WM_NCDESTROY) {
        g_set.open = false; g_set.hwnd = nullptr;
        if (g_set.font) { DeleteObject(g_set.font); g_set.font = nullptr; }
        return 0;
    }
    return DefWindowProc(h, m, w, l);
}

static void open_settings() {
    if (g_set.open) { SetForegroundWindow(g_set.hwnd); return; }
    load_settings();
    static bool reg = false;
    if (!reg) {
        WNDCLASSEX wc{ sizeof(wc) };
        wc.lpfnWndProc = SetProc; wc.hInstance = g_app.hInst; wc.lpszClassName = L"MosSettings";
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1); wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hIcon = g_win_icon; wc.hIconSm = g_win_icon;
        RegisterClassEx(&wc); reg = true;
    }
    int W = 620 * g_sc, H = 515 * g_sc;
    int sx = (GetSystemMetrics(SM_CXSCREEN) - W) / 2, sy = (GetSystemMetrics(SM_CYSCREEN) - H) / 2;
    HWND d = CreateWindowEx(WS_EX_TOPMOST, L"MosSettings", T(L"愤怒的蚊子 · 设置", L"AngryMoz · Settings"),
                            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, sx, sy, W, H, nullptr, nullptr, g_app.hInst, nullptr);
    g_set.hwnd = d;
    Settings& g = g_settings;
    g_sleep_ctrls.clear();
    auto SLP = [](HWND w) { g_sleep_ctrls.push_back(w); return w; };   // register a sleep-group control

    // ---- Global (top) ----
    s_group(d, T(L"通用（全局）", L"General"), 14, 8, 592, 82);
    s_label(d, T(L"蚊子大小", L"Size"), 28, 32);   g_set.base = s_edit(d, fmt1(g.base_size), 140, 32);
    s_label(d, T(L"速度", L"Speed"), 300, 32);      g_set.spd  = s_edit(d, fmt1(g.max_speed), 400, 32);
    g_set.snd   = s_check(d, T(L"嗡嗡声", L"Buzz"), 28, 60, g.sound != 0, IDCK_SND);
    g_set.autos = s_check(d, T(L"开机自启", L"Auto-start"), 210, 60, g.autostart != 0, IDCK_AUTO);
    s_check(d, L"English", 392, 60, g.lang != 0, IDCK_LANG);

    // ---- 久坐 (left) ----
    s_group(d, T(L"久坐提醒", L"Sedentary"), 14, 100, 289, 196);
    int LX = 26, LC = 156;
    s_label(d, T(L"工作时长(分)", L"Work (min)"), LX, 126);        g_set.work  = s_edit(d, std::to_wstring(g.work_min), LC, 126);
    s_label(d, T(L"离开回满(分)", L"Away→full (min)"), LX, 156); g_set.wbrk  = s_edit(d, std::to_wstring(g.work_break_min), LC, 156);
    s_label(d, T(L"出蚊子后休息(分)", L"Rest→clear (min)"), LX, 186); g_set.rest = s_edit(d, std::to_wstring(g.rest_min), LC, 186);
    s_label(d, T(L"每隔(分)+1只", L"+1 every (min)"), LX, 216);     g_set.sstep = s_edit(d, std::to_wstring(g.sed_step_min), LC, 216);
    s_label(d, T(L"上限(只/屏)", L"Max (/screen)"), LX, 246);       g_set.smax  = s_edit(d, std::to_wstring(g.sed_max), LC, 246);

    // ---- 睡觉 (right) — grayed out when disabled ----
    s_group(d, T(L"睡觉提醒", L"Bedtime"), 317, 100, 289, 344);
    int RX = 334, RC = 476;
    g_set.sen = s_check(d, T(L"启用睡觉模式", L"Enable bedtime"), RX, 126, g.sleep_enabled != 0, IDCK_SLEEP);
    SLP(CreateWindow(L"STATIC", T(L"（建议家庭 / 个人电脑启用）", L"(for home/personal PC)"), WS_CHILD | WS_VISIBLE,
                     RX * g_sc, 154 * g_sc, 270 * g_sc, 22 * g_sc, d, nullptr, g_app.hInst, nullptr));
    SLP(s_label(d, T(L"开始(HH:MM)", L"Start (HH:MM)"), RX, 182)); g_set.sstart = SLP(s_edit(d, hhmm(g.sleep_start), RC, 182));
    SLP(s_label(d, T(L"结束(HH:MM)", L"End (HH:MM)"), RX, 210));   g_set.send   = SLP(s_edit(d, hhmm(g.sleep_end), RC, 210));
    SLP(s_label(d, T(L"每隔(分)+1只", L"+1 every (min)"), RX, 238)); g_set.slstep = SLP(s_edit(d, std::to_wstring(g.sleep_step_min), RC, 238));
    SLP(s_label(d, T(L"上限(只/屏)", L"Max (/screen)"), RX, 266)); g_set.slmax  = SLP(s_edit(d, std::to_wstring(g.sleep_max), RC, 266));
    SLP(s_label(d, T(L"放大倍数", L"Grow x"), RX, 294));          g_set.smult  = SLP(s_edit(d, fmt1(g.sleep_mult), RC, 294));
    bool method_editable = (GetTickCount() - g_boot) < 5 * 60 * 1000;
    SLP(s_label(d, method_editable ? T(L"驱散：默写(可改)", L"Dismiss: type (editable)") : T(L"驱散：默写(锁定)", L"Dismiss: type (locked)"), RX, 322));
    SLP(s_label(d, T(L"字数 N", L"Chars N"), RX, 350));           g_set.dN  = SLP(s_edit(d, std::to_wstring(g.dismiss_N), RC, 350));
    SLP(s_label(d, T(L"撤销后重来(分)", L"Return after (min)"), RX, 378)); g_set.ret = SLP(s_edit(d, std::to_wstring(g.return_min), RC, 378));
    g_set.esc = SLP(s_check(d, T(L"驱散后加倍", L"Escalate"), RX, 406, g.escalate_penalty != 0, IDCK_ESC));

    CreateWindow(L"BUTTON", T(L"保存", L"Save"), WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 330 * g_sc, 456 * g_sc, 110 * g_sc, 32 * g_sc, d, (HMENU)(INT_PTR)IDSAVE, g_app.hInst, nullptr);
    CreateWindow(L"BUTTON", T(L"取消", L"Cancel"), WS_CHILD | WS_VISIBLE, 455 * g_sc, 456 * g_sc, 110 * g_sc, 32 * g_sc, d, (HMENU)(INT_PTR)IDCANC, g_app.hInst, nullptr);
    CreateWindow(L"STATIC", L"AngryMoz  v" APP_VER, WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
                 20 * g_sc, 456 * g_sc, 140 * g_sc, 32 * g_sc, d, nullptr, g_app.hInst, nullptr);
    CreateWindow(L"BUTTON", T(L"检查更新", L"Check update"), WS_CHILD | WS_VISIBLE,
                 168 * g_sc, 456 * g_sc, 140 * g_sc, 32 * g_sc, d, (HMENU)(INT_PTR)IDBTN_UPDATE, g_app.hInst, nullptr);

    g_set.font = CreateFont(15 * g_sc, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Microsoft YaHei");
    EnumChildWindows(d, [](HWND c, LPARAM lp) -> BOOL { SendMessage(c, WM_SETFONT, (WPARAM)lp, TRUE); return TRUE; }, (LPARAM)g_set.font);
    set_sleep_enabled(g.sleep_enabled != 0);   // gray the sleep group if disabled

    g_set.open = true;
    ShowWindow(d, SW_SHOW); SetForegroundWindow(d);
}

// ----------------------- help / 使用说明 window -----------------------------

static const wchar_t* HELP_ZH =
    L"愤怒的蚊子 · AngryMoz   v" APP_VER L"\r\n"
    L"────────────────────────────\r\n"
    L"嗨！我是一只专门盯着你的蚊子 🦟\r\n"
    L"我的工作只有两件：白天不让你坐太久，晚上催你早点睡。\r\n"
    L"\r\n"
    L"【我藏在哪】\r\n"
    L"我平时缩在右下角任务栏，是一个蚊子小图标，外面套着一圈“电量环”。\r\n"
    L"所有操作都在这个小图标上【点右键】。\r\n"
    L"\r\n"
    L"【白天：别坐太久】\r\n"
    L"· 你一直用电脑，电量环会慢慢从绿变红——那是你的“精力”在被我吸。\r\n"
    L"· 大约干满 40 分钟，精力见底，我就带一群蚊子飞到你屏幕上，嗡嗡嗡。\r\n"
    L"· 想赶走我们？很简单：站起来，离开键盘鼠标，歇一会儿。\r\n"
    L"· 你越是坐着继续敲，蚊子越多（别嘴硬 😏）。\r\n"
    L"· 安静歇够 5 分钟，蚊子全飞走，精力满血复活。\r\n"
    L"\r\n"
    L"【怎么才算“开始工作”】\r\n"
    L"精力满的时候，你连续动键盘鼠标 10 秒，我才认定“开工了”，开始倒计时。\r\n"
    L"（随手晃一下、碰一下不算，免得冤枉你。）\r\n"
    L"\r\n"
    L"【怎么才算“真的在休息”】\r\n"
    L"· 还没出蚊子时：如果你中途离开（吃饭、上厕所），连续 15 分钟不碰键鼠，\r\n"
    L"  精力就直接回满。（这 15 分钟可以在设置里改。）\r\n"
    L"· 出蚊子之后：安静歇着，累计够 5 分钟蚊子就全飞走、精力回满。\r\n"
    L"  偶尔碰一下只是暂停计时（之前歇的不白歇）；但你要一直乱动，\r\n"
    L"  我就当你根本没在休息，重新从头数。（这 5 分钟也能在设置里改。）\r\n"
    L"\r\n"
    L"【晚上：早点睡】\r\n"
    L"· 到了睡觉点（默认 23:30），蚊子会陆续冒出来，越来越多、越来越大、越来越吵，\r\n"
    L"  就是想烦到你乖乖去睡。\r\n"
    L"· 实在还要用电脑？右键我 →“驱散蚊子”，会弹出一段《滕王阁序》让你默写。\r\n"
    L"  （其实我不是真要你抄，是想让你困到放弃，早点睡~）\r\n"
    L"· 赶走一会儿后，我还会回来的哦。\r\n"
    L"\r\n"
    L"【开会 / 不想被打扰】\r\n"
    L"右键 →“暂停久坐提醒”，选 30 分钟 / 1 小时 / 2 小时。\r\n"
    L"（只暂停白天久坐，不影响晚上催睡。）\r\n"
    L"\r\n"
    L"【右键菜单都有啥】\r\n"
    L"· 驱散蚊子 —— 晚上有蚊子时，默写驱散\r\n"
    L"· 跳过本次休息 —— 白天有蚊子但你要连续赶工时，回满精力继续（少数情况用）\r\n"
    L"· 暂停久坐提醒 —— 开会神器（暂停后恢复会重置到满精力，不会立刻又冒蚊子）\r\n"
    L"· 设置 —— 各种时间、数量自己调（见下）\r\n"
    L"· 嗡嗡声 —— 嫌吵可以关掉\r\n"
    L"· 开机自启 —— 开机就自动帮你盯着\r\n"
    L"· 使用说明 —— 就是你正在看的这个啦\r\n"
    L"· 退出 —— 放我下班\r\n"
    L"\r\n"
    L"【设置里能调什么】\r\n"
    L"· 工作时长：坐多久算久坐（默认 40 分钟）\r\n"
    L"· 离开回满：白天离开/停手多久精力回满（默认 15 分钟）\r\n"
    L"· 出蚊子后休息：被蚊子催了之后，歇多久蚊子消失（默认 5 分钟）\r\n"
    L"· 每隔几分钟多一只 / 最多几只：蚊子变多的速度和上限\r\n"
    L"· 睡觉开始、结束时间：晚上几点开始催、到几点收工\r\n"
    L"· 蚊子大小、飞行速度：想要更大更快，随你\r\n"
    L"· 声音开关、开机自启\r\n"
    L"（设置也会存进程序旁边的 AngryMoz.ini，会玩的也能直接改。）\r\n"
    L"\r\n"
    L"就这些啦！我不是真想烦你，只是想让你身体好一点、睡得早一点。\r\n"
    L"——你的私人蚊子 🦟\r\n";

static const wchar_t* HELP_EN =
    L"AngryMoz · 愤怒的蚊子   v" APP_VER L"\r\n"
    L"────────────────────────────\r\n"
    L"Hi! I'm a mosquito that keeps an eye on you 🦟\r\n"
    L"I have two jobs: keep you from sitting too long by day, and nudge you to bed at night.\r\n"
    L"\r\n"
    L"[Where I hide]\r\n"
    L"I sit in the tray at the bottom-right — a little mosquito icon with a status ring.\r\n"
    L"There is no main window; do everything by RIGHT-CLICKING that icon.\r\n"
    L"\r\n"
    L"[Daytime: don't sit too long]\r\n"
    L"- While you use the PC, the ring drains from green to red — that's your 'energy'.\r\n"
    L"- After about 40 min of work, energy runs out and angry mosquitoes fly onto your screen.\r\n"
    L"- Keep using the PC and more show up. To shoo them off: get up and take a break.\r\n"
    L"\r\n"
    L"[What counts as 'starting work']\r\n"
    L"When energy is full, 10 seconds of continuous input starts the work timer.\r\n"
    L"(One stray click or wiggle won't — so you're not wrongly blamed.)\r\n"
    L"\r\n"
    L"[What counts as 'really resting']\r\n"
    L"- Before mosquitoes: if you leave (meal, restroom) for 15 continuous minutes, energy refills.\r\n"
    L"  (You can change this 15 min in Settings.)\r\n"
    L"- After mosquitoes: stay still; once you accumulate 5 minutes of stillness they all clear.\r\n"
    L"  A stray touch only pauses the count; but constant fidgeting resets it. (Also editable.)\r\n"
    L"\r\n"
    L"[Night: go to bed]\r\n"
    L"- At bedtime (default 23:30) mosquitoes appear, growing more/bigger/louder to push you to sleep.\r\n"
    L"- Still must use the PC? Right-click -> 'Dismiss', and transcribe a passage.\r\n"
    L"  (I don't really want you to copy text — I want you too sleepy to bother, so you sleep~)\r\n"
    L"- They'll come back after a while.\r\n"
    L"\r\n"
    L"[In a meeting / don't disturb]\r\n"
    L"Right-click -> 'Pause sedentary' (30 min / 1 h / 2 h). (Only pauses daytime; night still nags.)\r\n"
    L"\r\n"
    L"[Right-click menu]\r\n"
    L"- Dismiss — transcribe to clear the night mosquitoes\r\n"
    L"- Skip this rest — daytime swarm is out but you must keep working: refill & go on\r\n"
    L"- Pause sedentary — meeting saver (resuming refills to full, no instant swarm)\r\n"
    L"- Settings — times, counts, size, sound, etc.\r\n"
    L"- Buzz — sound on/off\r\n"
    L"- Auto-start — run at startup\r\n"
    L"- Help — this window\r\n"
    L"- Exit\r\n"
    L"\r\n"
    L"[Settings]\r\n"
    L"- Work (min): how long counts as sitting too long (default 40)\r\n"
    L"- Break->full (min): still time before mosquitoes that refills energy (default 15)\r\n"
    L"- Rest->clear (min): still time after mosquitoes to clear them (default 5)\r\n"
    L"- +1 every / Max: how fast mosquitoes grow and the cap\r\n"
    L"- Bedtime start / end, mosquito size, speed, sound, auto-start\r\n"
    L"(Settings are saved to AngryMoz.ini next to the exe; you can edit it directly too.)\r\n"
    L"\r\n"
    L"That's it! I don't really want to bug you — I just want you healthier and better rested.\r\n"
    L"— your personal mosquito 🦟\r\n";

struct HelpDlg { HWND hwnd = nullptr, edit = nullptr; HFONT font = nullptr; bool open = false; } g_help;

static LRESULT CALLBACK HelpProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
        SetBkColor((HDC)w, RGB(255, 255, 255));
        SetTextColor((HDC)w, RGB(30, 30, 30));
        return (LRESULT)GetStockObject(WHITE_BRUSH);
    case WM_SIZE:
        if (g_help.edit) { RECT rc; GetClientRect(h, &rc); MoveWindow(g_help.edit, 14, 14, rc.right - 28, rc.bottom - 28, TRUE); }
        return 0;
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    case WM_NCDESTROY:
        g_help.open = false; g_help.hwnd = nullptr;
        if (g_help.font) { DeleteObject(g_help.font); g_help.font = nullptr; }
        return 0;
    }
    return DefWindowProc(h, m, w, l);
}

static void open_help() {
    if (g_help.open) { SetForegroundWindow(g_help.hwnd); return; }
    static bool reg = false;
    if (!reg) {
        WNDCLASSEX wc{ sizeof(wc) };
        wc.lpfnWndProc = HelpProc;
        wc.hInstance = g_app.hInst;
        wc.lpszClassName = L"MosHelp";
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hIcon = LoadIcon(g_app.hInst, MAKEINTRESOURCE(1));
        wc.hIconSm = wc.hIcon;
        RegisterClassEx(&wc);
        reg = true;
    }
    int W = 900, H = 800;
    int sx = (GetSystemMetrics(SM_CXSCREEN) - W) / 2, sy = (GetSystemMetrics(SM_CYSCREEN) - H) / 2;
    g_help.hwnd = CreateWindowEx(WS_EX_TOPMOST, L"MosHelp", T(L"愤怒的蚊子 · 使用说明", L"AngryMoz · Help"),
                                 WS_OVERLAPPEDWINDOW, sx, sy, W, H, nullptr, nullptr, g_app.hInst, nullptr);
    RECT rc; GetClientRect(g_help.hwnd, &rc);
    g_help.edit = CreateWindowEx(0, L"EDIT", g_lang ? HELP_EN : HELP_ZH,
                                 WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | WS_VSCROLL | ES_AUTOVSCROLL,
                                 14, 14, rc.right - 28, rc.bottom - 28, g_help.hwnd, nullptr, g_app.hInst, nullptr);
    g_help.font = CreateFont(27, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Microsoft YaHei");
    SendMessage(g_help.edit, WM_SETFONT, (WPARAM)g_help.font, TRUE);
    SendMessage(g_help.edit, EM_SETSEL, 0, 0);            // caret at top, not selected
    g_help.open = true;
    ShowWindow(g_help.hwnd, SW_SHOW);
    SetForegroundWindow(g_help.hwnd);
}

static LRESULT CALLBACK CtrlProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_TIMER:
        if (wp == ID_LOGIC) g_app.logic_tick();
        else if (wp == ID_RENDER) g_app.render_tick();
        else if (wp == ID_BLINK) {
            // Toggle between two cached icons — cheap, no per-frame redraw.
            g_tray_lit = !g_tray_lit;
            set_tray_icon(g_tray_lit ? g_tray_full : g_tray_dim, g_nid.szTip);
        }
        return 0;
    case WM_TRAY:
        if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU) {
            POINT pt; GetCursorPos(&pt);
            HMENU menu = CreatePopupMenu();
            if (g_update_ready) {                    // a newer release exists → offer the download page
                std::wstring um = std::wstring(T(L"🔴 发现新版本 ", L"🔴 New version ")) + g_update_tag
                                  + T(L" — 点击下载", L" — click to download");
                AppendMenu(menu, MF_STRING, IDM_UPDATE_OPEN, um.c_str());
                AppendMenu(menu, MF_SEPARATOR, 0, nullptr);
            }
            bool can = g_app.in_sleep && !g_app.mos.empty();
            AppendMenu(menu, MF_STRING | (can ? 0 : MF_GRAYED), IDM_DISMISS, T(L"驱散蚊子（默写滕王阁序）", L"Dismiss mosquitoes (transcribe)"));
            bool can_skip = !g_app.in_sleep && !g_app.mos.empty();   // sedentary swarm out
            AppendMenu(menu, MF_STRING | (can_skip ? 0 : MF_GRAYED), IDM_SKIP_REST, T(L"跳过本次休息（回满精力继续工作）", L"Skip this rest (refill & keep working)"));
            AppendMenu(menu, MF_SEPARATOR, 0, nullptr);
            HMENU pausem = CreatePopupMenu();
            AppendMenu(pausem, MF_STRING, IDM_PAUSE_30,  T(L"30 分钟", L"30 min"));
            AppendMenu(pausem, MF_STRING, IDM_PAUSE_60,  T(L"1 小时", L"1 hour"));
            AppendMenu(pausem, MF_STRING, IDM_PAUSE_120, T(L"2 小时", L"2 hours"));
            AppendMenu(pausem, MF_SEPARATOR, 0, nullptr);
            AppendMenu(pausem, MF_STRING | (g_app.is_sed_paused() ? 0 : MF_GRAYED), IDM_PAUSE_OFF, T(L"恢复久坐提醒", L"Resume"));
            AppendMenu(menu, MF_POPUP | (g_app.is_sed_paused() ? MF_CHECKED : 0), (UINT_PTR)pausem, T(L"暂停久坐提醒", L"Pause sedentary"));
            AppendMenu(menu, MF_STRING, IDM_SETTINGS, T(L"设置…", L"Settings…"));
            AppendMenu(menu, MF_STRING | (g_app.sound_enabled ? MF_CHECKED : 0), IDM_SOUND, T(L"嗡嗡声", L"Buzz"));
            AppendMenu(menu, MF_STRING | (is_autostart() ? MF_CHECKED : 0), IDM_AUTOSTART, T(L"开机自启", L"Auto-start"));
            AppendMenu(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenu(menu, MF_STRING, IDM_HELP, T(L"使用说明", L"Help"));
            AppendMenu(menu, MF_STRING, IDM_QUIT, T(L"退出", L"Exit"));
            SetForegroundWindow(hwnd);
            TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
            DestroyMenu(menu);
            PostMessage(hwnd, WM_NULL, 0, 0);
        }
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == IDM_DISMISS) open_dismiss();
        else if (LOWORD(wp) == IDM_SKIP_REST) { g_app.refresh_full(); update_tray(); }
        else if (LOWORD(wp) == IDM_UPDATE_OPEN) ShellExecute(nullptr, L"open", RELEASES_URL, nullptr, nullptr, SW_SHOWNORMAL);
        else if (LOWORD(wp) == IDM_PAUSE_30) g_app.pause_sed(30);
        else if (LOWORD(wp) == IDM_PAUSE_60) g_app.pause_sed(60);
        else if (LOWORD(wp) == IDM_PAUSE_120) g_app.pause_sed(120);
        else if (LOWORD(wp) == IDM_PAUSE_OFF) g_app.resume_sed();
        else if (LOWORD(wp) == IDM_SETTINGS) open_settings();
        else if (LOWORD(wp) == IDM_SOUND) g_app.sound_enabled = !g_app.sound_enabled;
        else if (LOWORD(wp) == IDM_AUTOSTART) set_autostart(!is_autostart());
        else if (LOWORD(wp) == IDM_HELP) open_help();
        else if (LOWORD(wp) == IDM_QUIT) PostQuitMessage(0);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR lpCmdLine, int) {
    const char* cl = lpCmdLine ? lpCmdLine : "";
    bool fast = strstr(cl, "--fast") != nullptr;
    bool demo = strstr(cl, "--demo") != nullptr;
    bool sleep = strstr(cl, "--sleep") != nullptr;
    if (fast || demo || sleep) {
        AllocConsole();
        FILE* f; freopen_s(&f, "CONOUT$", "w", stdout);
        printf("mosquito %s%s%s: Ctrl+Alt+Q to quit.\n",
               fast ? "--fast " : "", demo ? "--demo " : "", sleep ? "--sleep " : "");
    }

    // Single instance: a global hotkey can only belong to one process, and
    // stacked copies would fight over it. Second launch just exits.
    HANDLE inst_mtx = CreateMutex(nullptr, TRUE, L"MosquitoSingletonMutex");
    if (inst_mtx && GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    SetProcessDPIAware();
    srand((unsigned)time(nullptr));
    g_app.hInst = hInst;
    g_app.fast = fast; g_app.demo = demo; g_app.force_sleep = sleep;
    g_boot = GetTickCount();
    bool first_run = GetFileAttributes(ini_path().c_str()) == INVALID_FILE_ATTRIBUTES;
    load_settings();
    apply_settings();          // cfg + activity detector + app fields from AngryMoz.ini

    // First launch: ask language (default 中文).
    if (first_run && !fast && !demo && !sleep) {
        int r = MessageBox(nullptr, L"选择语言 / Choose language\n\n[ 是 / Yes ] 中文\n[ 否 / No ] English",
                           L"AngryMoz · 愤怒的蚊子", MB_YESNO | MB_ICONQUESTION | MB_TOPMOST | MB_DEFBUTTON1);
        g_settings.lang = (r == IDNO) ? 1 : 0;
        g_lang = g_settings.lang;
        save_settings();
    }

    // CLI live tuning overrides the saved size/speed for this run.
    const char* sp;
    if ((sp = strstr(cl, "--size="))  != nullptr) g_app.base_size = (float)atof(sp + 7);
    if ((sp = strstr(cl, "--speed=")) != nullptr) g_app.max_speed = (float)atof(sp + 8);
    if (fast || demo || sleep) printf("size=%.1f  speed=%.1f px/frame\n", g_app.base_size, g_app.max_speed);
    enumerate_monitors();

    ULONG_PTR token; GdiplusStartupInput gsi;
    GdiplusStartup(&token, &gsi, nullptr);
    load_face(hInst);
    g_win_icon = (HICON)LoadImage(hInst, MAKEINTRESOURCE(1), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE | LR_SHARED);
    load_text();

    // Shared window class for all mosquito sprites.
    WNDCLASSEX wc{ sizeof(wc) };
    wc.lpfnWndProc = DefWindowProc;
    wc.hInstance = hInst;
    wc.lpszClassName = MOS_CLASS;
    RegisterClassEx(&wc);

    // Hidden control window owns the timers and the quit hotkey.
    WNDCLASSEX cc{ sizeof(cc) };
    cc.lpfnWndProc = CtrlProc;
    cc.hInstance = hInst;
    cc.lpszClassName = L"MosquitoCtrl";
    RegisterClassEx(&cc);
    g_app.ctrl = CreateWindowEx(0, L"MosquitoCtrl", L"", 0, 0, 0, 0, 0,
                                HWND_MESSAGE, nullptr, hInst, nullptr);

    SetTimer(g_app.ctrl, ID_LOGIC, 1000, nullptr);

    // Tray icon: right-click for the dismiss/exit menu; icon shows urgency.
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_app.ctrl;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon = draw_tray_icon(0.0f, stage_color(-1), 1.0f);
    lstrcpyn(g_nid.szTip, T(L"愤怒的蚊子 · 守护中", L"AngryMoz · on watch"), 128);
    Shell_NotifyIcon(NIM_ADD, &g_nid);

    // One silent update check on startup (background thread, exits when done).
    if (HANDLE ht = CreateThread(nullptr, 0, update_thread, nullptr, 0, nullptr)) CloseHandle(ht);

    sound_init();   // autostart is opt-in via the tray "开机自启" checkbox

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    sound_close();
    Shell_NotifyIcon(NIM_DELETE, &g_nid);
    for (auto& v : g_app.views) destroy_mosview(v);
    GdiplusShutdown(token);
    return 0;
}
