// Hlebushek Mod Menu v2 — DLL, загружается в процесс игры (Dizzy.exe v0.5, 32 бит).
// ...
#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>
#include <tlhelp32.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>

// Безопасные обёртки: не зависят от макросов min/max из windows.h
template<class T> static inline T Max(T a, T b) { return a > b ? a : b; }
template<class T> static inline T Min(T a, T b) { return a < b ? a : b; }

#ifdef _WIN64
#error "Игра 32-битная: собирай DLL как 32-bit (x86)"
#endif

// ================================================================== данные игры (Dizzy.exe v0.5)
static const DWORD EXPECT_SIZE = 0x78000, EXPECT_STAMP = 0x6AD1EC6C;
static const uintptr_t RVA_DRAW = 0xD6F0, RVA_DRAW_RET = 0xD6F6;
static const uintptr_t RVA_GOD = 0x5D7D, RVA_GOD_CONT = 0x5D87, RVA_GOD_SKIP = 0x5E29;
static const uintptr_t RVA_SCORE = 0x1804, RVA_SCORE_NEG = 0x1809, RVA_SCORE_DONE = 0x1810;
static const uintptr_t RVA_TIMEPROC = 0x30420;                       // TimeProc мультимедийного таймера игры (тик 16 мс)
static const uintptr_t RVA_IAT_SETDIB = 0x5A004, RVA_IAT_STRETCH = 0x5A00C;
static const int OFF_LIVES = 0x58, OFF_ENERGY = 0x5C, OFF_SCORE = 0x6C;

struct Shared { volatile DWORD game, godFlag, mult, frames; };
struct Patch  { uintptr_t site; size_t len; BYTE orig[16], neu[16]; };

static HINSTANCE g_self;
static HWND g_wnd;
static uintptr_t g_base;
static BYTE* g_code = NULL;
static Shared* g_sh = NULL;
static std::vector<Patch> g_patches;
static bool g_supported = false, g_hooked = false, g_visOk = false, g_speedOk = false;
static volatile bool g_quit = false, g_eject = false;
static HANDLE g_worker;
static volatile LONG g_inHook = 0;

// ================================================================== настройки (сохраняются в Hlebushek_menu.ini)
static bool g_god = false, g_infLives = false, g_infEnergy = false, g_turbo = false, g_xray = false, g_fx = true;
static int g_mult = 1, g_speed10 = 10;
static int g_weather = 0, g_wInt = 50, g_wWind = 0;
static int g_filter = 0, g_fInt = 70, g_vig = 0, g_scan = 0;
static int g_accent = 0, g_theme = 0, g_opac = 100, g_scale = 1, g_dock = 0;
static int g_heroMode = 0, g_heroSpd = 6, g_heroSize = 100;
static bool g_rainbow = false;
static volatile bool g_fresh = false;
static volatile int g_lives = 0, g_energy = 0, g_score = 0;
static std::wstring g_toast;
static DWORD g_toastUntil = 0;

static std::wstring IniPath() {
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(NULL, p, MAX_PATH);
    std::wstring s = p;
    size_t k = s.find_last_of(L"\\/");
    return (k == std::wstring::npos ? L"" : s.substr(0, k + 1)) + L"Hlebushek_menu.ini";
}
struct IntSetting { const wchar_t* key; int* v; int mn, mx; };
static IntSetting g_ints[] = {
    {L"mult", &g_mult, 1, 10}, {L"speed", &g_speed10, 10, 40}, {L"weather", &g_weather, 0, 6}, {L"wint", &g_wInt, 1, 100},
    {L"wind", &g_wWind, -10, 10}, {L"filter", &g_filter, 0, 7}, {L"fint", &g_fInt, 0, 100}, {L"vig", &g_vig, 0, 100},
    {L"scan", &g_scan, 0, 100}, {L"accent", &g_accent, 0, 7}, {L"theme", &g_theme, 0, 2}, {L"opac", &g_opac, 40, 100},
    {L"scale", &g_scale, 0, 2}, {L"dock", &g_dock, 0, 2},
    {L"hmode", &g_heroMode, 0, 3}, {L"hspd", &g_heroSpd, -18, 18}, {L"hsize", &g_heroSize, 50, 120}};
static void LoadSettings() {
    std::wstring p = IniPath();
    for (size_t i = 0; i < sizeof g_ints / sizeof g_ints[0]; i++) {
        wchar_t tb[32] = L"";
        GetPrivateProfileStringW(L"menu", g_ints[i].key, L"", tb, 32, p.c_str());   // GetPrivateProfileInt не читает отрицательные
        int v = tb[0] ? _wtoi(tb) : *g_ints[i].v;
        if (v < g_ints[i].mn) v = g_ints[i].mn;
        if (v > g_ints[i].mx) v = g_ints[i].mx;
        *g_ints[i].v = v;
    }
    g_xray = GetPrivateProfileIntW(L"menu", L"xray", 0, p.c_str()) != 0;
    g_fx = GetPrivateProfileIntW(L"menu", L"fx", 1, p.c_str()) != 0;
    g_turbo = GetPrivateProfileIntW(L"menu", L"turbo", 0, p.c_str()) != 0;
    g_rainbow = GetPrivateProfileIntW(L"menu", L"rainbow", 0, p.c_str()) != 0;
}
static void SaveSettings() {
    std::wstring p = IniPath();
    wchar_t b[32];
    for (size_t i = 0; i < sizeof g_ints / sizeof g_ints[0]; i++) {
        _snwprintf(b, 32, L"%d", *g_ints[i].v);
        WritePrivateProfileStringW(L"menu", g_ints[i].key, b, p.c_str());
    }
    WritePrivateProfileStringW(L"menu", L"xray", g_xray ? L"1" : L"0", p.c_str());
    WritePrivateProfileStringW(L"menu", L"fx", g_fx ? L"1" : L"0", p.c_str());
    WritePrivateProfileStringW(L"menu", L"turbo", g_turbo ? L"1" : L"0", p.c_str());
    WritePrivateProfileStringW(L"menu", L"rainbow", g_rainbow ? L"1" : L"0", p.c_str());
}

// ================================================================== память и патчи кода
static bool Rd(uintptr_t a, void* buf, size_t n) {
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), (LPCVOID)a, buf, n, &got) && got == n;
}
static bool Wr(uintptr_t a, const void* buf, size_t n) {
    SIZE_T got = 0;
    return WriteProcessMemory(GetCurrentProcess(), (LPVOID)a, buf, n, &got) && got == n;
}
static bool Verify(uintptr_t rva, const BYTE* sig, size_t n) {
    BYTE cur[32];
    return n <= sizeof cur && Rd(g_base + rva, cur, n) && memcmp(cur, sig, n) == 0;
}
static void EmitU32(BYTE*& p, uint32_t v) { memcpy(p, &v, 4); p += 4; }
static void EmitJmp(BYTE*& p, uintptr_t target) {
    uint32_t rel = (uint32_t)(target - ((uintptr_t)p + 5));
    *p++ = 0xE9;
    EmitU32(p, rel);
}
static Patch MakeJmpPatch(uintptr_t rva, size_t len, uintptr_t stub) {
    Patch p;
    memset(&p, 0, sizeof p);
    p.site = g_base + rva;
    p.len = len;
    Rd(p.site, p.orig, len);
    memset(p.neu, 0x90, sizeof p.neu);
    p.neu[0] = 0xE9;
    uint32_t rel = (uint32_t)(stub - (p.site + 5));
    memcpy(p.neu + 1, &rel, 4);
    return p;
}
static bool ApplyPatches(bool apply) {
    for (int attempt = 0; attempt < 30; attempt++) {
        std::vector<HANDLE> th;
        th.reserve(256);
        DWORD me = GetCurrentThreadId(), pid = GetCurrentProcessId();
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        THREADENTRY32 te;
        te.dwSize = sizeof te;
        if (snap != INVALID_HANDLE_VALUE && Thread32First(snap, &te)) {
            do {
                if (te.th32OwnerProcessID == pid && te.th32ThreadID != me) {
                    HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, te.th32ThreadID);
                    if (h) { SuspendThread(h); th.push_back(h); }
                }
            } while (Thread32Next(snap, &te));
        }
        if (snap != INVALID_HANDLE_VALUE) CloseHandle(snap);
        bool busy = false;
        for (size_t i = 0; i < th.size() && !busy; i++) {
            CONTEXT c;
            memset(&c, 0, sizeof c);
            c.ContextFlags = CONTEXT_CONTROL;
            if (GetThreadContext(th[i], &c))
                for (size_t k = 0; k < g_patches.size(); k++)
                    if (c.Eip >= g_patches[k].site && c.Eip < g_patches[k].site + g_patches[k].len) busy = true;
        }
        bool ok = false;
        if (!busy) {
            ok = true;
            for (size_t k = 0; k < g_patches.size(); k++) {
                const Patch& p = g_patches[k];
                DWORD old = 0;
                if (!VirtualProtect((LPVOID)p.site, p.len, PAGE_EXECUTE_READWRITE, &old)) { ok = false; continue; }
                memcpy((void*)p.site, apply ? p.neu : p.orig, p.len);
                VirtualProtect((LPVOID)p.site, p.len, old, &old);
                FlushInstructionCache(GetCurrentProcess(), (LPCVOID)p.site, p.len);
            }
        }
        for (size_t i = 0; i < th.size(); i++) { ResumeThread(th[i]); CloseHandle(th[i]); }
        if (ok) return true;
        Sleep(10);
    }
    return false;
}

// ================================================================== эффекты: рисование в кадр игры
struct Frame { BYTE* p; int w, h, stride, bpp; bool topDown; };
static uint32_t g_rng = 2463534242u;
static inline uint32_t Rnd() { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return g_rng; }
static inline float Rf() { return (Rnd() & 0xFFFFFF) / 16777216.0f; }
static inline int Clamp(int v, int a, int b) { return v < a ? a : (v > b ? b : v); }

static inline BYTE* Px(Frame& f, int x, int y) {
    if ((unsigned)x >= (unsigned)f.w || (unsigned)y >= (unsigned)f.h) return NULL;
    int ry = f.topDown ? y : f.h - 1 - y;
    return f.p + ry * f.stride + x * f.bpp;
}
static inline void Blend(Frame& f, int x, int y, int r, int g, int b, int a) {
    BYTE* p = Px(f, x, y);
    if (!p) return;
    p[0] = (BYTE)(p[0] + (b - p[0]) * a / 256);
    p[1] = (BYTE)(p[1] + (g - p[1]) * a / 256);
    p[2] = (BYTE)(p[2] + (r - p[2]) * a / 256);
}
static void Rect(Frame& f, int x, int y, int s, int r, int g, int b, int a) {
    for (int j = 0; j < s; j++) for (int i = 0; i < s; i++) Blend(f, x + i, y + j, r, g, b, a);
}

static void ApplyFilter(Frame& f, int mode, int amt) {
    if (mode <= 0 || amt <= 0) return;
    for (int y = 0; y < f.h; y++) {
        BYTE* row = f.p + y * f.stride;
        for (int x = 0; x < f.w; x++) {
            BYTE* p = row + x * f.bpp;
            int b = p[0], g = p[1], r = p[2], fr, fg, fb;
            int l = (r * 77 + g * 150 + b * 29) >> 8;
            switch (mode) {
            case 1: fr = r * 30 / 100; fg = g * 40 / 100; fb = Clamp(b * 75 / 100 + 22, 0, 255); break;                    // ночь
            case 2: fr = Clamp(r * 115 / 100 + 25, 0, 255); fg = g * 85 / 100 + 8; fb = b * 60 / 100; break;               // закат
            case 3: fr = Clamp((r * 393 + g * 769 + b * 189) / 1000, 0, 255); fg = Clamp((r * 349 + g * 686 + b * 168) / 1000, 0, 255);
                    fb = Clamp((r * 272 + g * 534 + b * 131) / 1000, 0, 255); break;                                       // сепия
            case 4: fr = fg = fb = l; break;                                                                               // ч/б
            case 5: fr = r * 80 / 100; fg = g * 95 / 100 + 5; fb = Clamp(b * 115 / 100 + 15, 0, 255); break;              // холод
            case 6: { int v = Clamp(l * 14 / 10 + 12, 0, 255); fr = v * 20 / 100; fg = v; fb = v * 30 / 100; break; }     // ночное зрение
            default: fr = 255 - r; fg = 255 - g; fb = 255 - b; break;                                                      // инверсия
            }
            p[0] = (BYTE)(b + (fb - b) * amt / 100);
            p[1] = (BYTE)(g + (fg - g) * amt / 100);
            p[2] = (BYTE)(r + (fr - r) * amt / 100);
        }
    }
}
static std::vector<BYTE> g_lum;
static void ApplyXRay(Frame& f) {
    size_t n = (size_t)f.w * f.h;
    if (g_lum.size() != n) g_lum.resize(n);
    for (int y = 0; y < f.h; y++) {
        BYTE* row = f.p + y * f.stride;
        for (int x = 0; x < f.w; x++) {
            BYTE* p = row + x * f.bpp;
            g_lum[(size_t)y * f.w + x] = (BYTE)((p[2] * 77 + p[1] * 150 + p[0] * 29) >> 8);
        }
    }
    for (int y = 0; y < f.h; y++) {
        BYTE* row = f.p + y * f.stride;
        for (int x = 0; x < f.w; x++) {
            BYTE* p = row + x * f.bpp;
            int l = g_lum[(size_t)y * f.w + x];
            int dx = x + 1 < f.w ? abs(l - g_lum[(size_t)y * f.w + x + 1]) : 0;
            int dy = y + 1 < f.h ? abs(l - g_lum[(size_t)(y + 1) * f.w + x]) : 0;
            if (dx + dy > 36) {   // контур
                p[0] = (BYTE)(p[0] + (230 - p[0]) * 190 / 256);
                p[1] = (BYTE)(p[1] + (255 - p[1]) * 190 / 256);
                p[2] = (BYTE)(p[2] + (60 - p[2]) * 190 / 256);
            } else if (l < 70) {   // подсветка тёмных мест
                int k = 70 - l;
                p[0] = (BYTE)Clamp(p[0] + k * 2, 0, 255);
                p[1] = (BYTE)Clamp(p[1] + k * 2, 0, 255);
                p[2] = (BYTE)Clamp(p[2] + k * 2, 0, 255);
            }
        }
    }
}
static void ApplyVignette(Frame& f, int strength) {
    float s = strength / 100.0f, cx = f.w * 0.5f, cy = f.h * 0.5f;
    for (int y = 0; y < f.h; y++) {
        float dy = (y - cy) / cy;
        BYTE* row = f.p + y * f.stride;
        for (int x = 0; x < f.w; x++) {
            float dx = (x - cx) / cx, d = dx * dx + dy * dy - 0.3f;
            if (d <= 0) continue;
            int k = (int)(256 * (1.0f - s * d / 1.7f));
            if (k < 0) k = 0;
            BYTE* p = row + x * f.bpp;
            p[0] = (BYTE)(p[0] * k >> 8); p[1] = (BYTE)(p[1] * k >> 8); p[2] = (BYTE)(p[2] * k >> 8);
        }
    }
}
static void ApplyScan(Frame& f, int strength) {
    int k = 256 - strength * 128 / 100;
    for (int y = 1; y < f.h; y += 2) {
        BYTE* row = f.p + y * f.stride;
        for (int x = 0; x < f.w; x++) {
            BYTE* p = row + x * f.bpp;
            p[0] = (BYTE)(p[0] * k >> 8); p[1] = (BYTE)(p[1] * k >> 8); p[2] = (BYTE)(p[2] * k >> 8);
        }
    }
}

struct Part { float x, y, vx, vy, ph; BYTE col; BYTE size; };
static std::vector<Part> g_parts;
static int g_partKind = -1, g_partW = 0, g_partH = 0;
static DWORD g_lastT = 0, g_nextFlash = 0, g_flashStart = 0;
static const int LEAF[4][3] = {{214, 120, 40}, {186, 80, 30}, {230, 170, 60}, {130, 150, 45}};

static void Spawn(Part& p, int kind, int w, int h, bool anywhere) {
    float sx = w / 320.0f, sy = h / 240.0f;
    float wind = (float)g_wWind;
    p.x = Rf() * (w + 40) - 20;
    p.y = anywhere ? Rf() * h : -4.0f - Rf() * 12;
    p.ph = Rf() * 6.28f;
    p.col = (BYTE)(Rnd() & 3);
    p.size = (BYTE)(Rnd() % 3 == 0 ? 2 : 1);
    switch (kind) {
    case 1: p.vy = (18 + Rf() * 30) * sy; p.vx = wind * 6 * sx; break;                              // снег
    case 2: p.vy = (260 + Rf() * 120) * sy; p.vx = wind * 22 * sx; break;                           // дождь
    case 3: p.vy = (340 + Rf() * 140) * sy; p.vx = (wind * 22 + 40) * sx; break;                    // гроза
    case 5: p.y = Rf() * h; p.vx = (Rf() - 0.5f) * 14 * sx; p.vy = (Rf() - 0.5f) * 14 * sy; break; // светлячки
    default: p.vy = (14 + Rf() * 16) * sy; p.vx = wind * 5 * sx; break;                             // листопад
    }
}
static void StepWeather(Frame& f, float dt, DWORD now) {
    int kind = g_weather;
    if (kind == 4) {   // туман
        float amt = g_wInt * 0.9f / 100.0f;
        float t = now / 1500.0f;
        for (int y = 0; y < f.h; y++) {
            float base = amt * (0.35f + 0.65f * y / f.h);
            for (int x = 0; x < f.w; x++) {
                float a = base * (0.8f + 0.2f * sinf(x * 0.045f + t + y * 0.02f));
                Blend(f, x, y, 200, 205, 215, (int)(a * 200));
            }
        }
        return;
    }
    if (kind != g_partKind || f.w != g_partW || f.h != g_partH) { g_parts.clear(); g_partKind = kind; g_partW = f.w; g_partH = f.h; }
    float dens = g_wInt / 50.0f;
    int target = (int)((f.w * f.h / 2000.0f) * dens * (kind == 3 ? 1.5f : (kind == 2 ? 1.2f : (kind == 5 ? 0.12f : (kind == 6 ? 0.12f : 1.0f)))));
    target = Clamp(target, 1, 1500);
    while ((int)g_parts.size() < target) { Part p; Spawn(p, kind, f.w, f.h, true); g_parts.push_back(p); }
    if ((int)g_parts.size() > target) g_parts.resize(target);
    float sx = f.w / 320.0f, sy = f.h / 240.0f;
    int sz = std::max(1, (int)(sx + 0.5f));
    for (size_t i = 0; i < g_parts.size(); i++) {
        Part& p = g_parts[i];
        p.ph += dt * (kind == 5 ? 3.0f : 2.0f);
        float sway = (kind == 1 || kind == 6) ? sinf(p.ph) * 10 * sx : 0;
        p.x += (p.vx + sway) * dt;
        p.y += p.vy * dt;
        if (kind == 5) { p.vx += (Rf() - 0.5f) * 20 * dt; p.vy += (Rf() - 0.5f) * 20 * dt; if (p.x < 0) p.x = (float)f.w; if (p.x > f.w) p.x = 0; if (p.y < 0) p.y = (float)f.h; if (p.y > f.h) p.y = 0; }
        else if (p.y > f.h + 4 || p.x < -30 || p.x > f.w + 30) Spawn(p, kind, f.w, f.h, false);
        int x = (int)p.x, y = (int)p.y;
        switch (kind) {
        case 1: Rect(f, x, y, sz * p.size, 255, 255, 255, p.size == 2 ? 230 : 160); break;
        case 2: case 3: {
            int len = std::max(3, (int)(9 * sy));
            float k = p.vx / p.vy;
            for (int j = 0; j < len; j++) Blend(f, (int)(p.x - k * j), y - j, 175, 195, 255, 150 - j * 90 / len);
            break; }
        case 5: { int a = (int)(120 + 120 * sinf(p.ph)); Rect(f, x, y, sz, 255, 240, 120, Clamp(a, 20, 240));
                  Blend(f, x - 1, y, 255, 240, 120, Clamp(a / 3, 0, 255)); Blend(f, x + sz, y, 255, 240, 120, Clamp(a / 3, 0, 255));
                  Blend(f, x, y - 1, 255, 240, 120, Clamp(a / 3, 0, 255)); Blend(f, x, y + sz, 255, 240, 120, Clamp(a / 3, 0, 255)); break; }
        default: Rect(f, x, y, sz * 2, LEAF[p.col][0], LEAF[p.col][1], LEAF[p.col][2], 230); break;
        }
    }
    if (kind == 3) {   // молнии
        if (now > g_nextFlash) { g_flashStart = now; g_nextFlash = now + 3500 + (DWORD)(Rf() * 7000); }
        DWORD age = now - g_flashStart;
        if (g_flashStart && age < 320) {
            int a = (int)(170 * (1.0f - age / 320.0f));
            if (age > 90 && age < 140) a /= 3;   // двойная вспышка
            for (int y = 0; y < f.h; y++) for (int x = 0; x < f.w; x++) Blend(f, x, y, 235, 240, 255, a);
        }
    }
}

static bool NeedFx() { return g_fx && (g_weather || g_filter || g_xray || g_vig > 0 || g_scan > 0); }
static void RunEffects(Frame& f) {
    DWORD now = GetTickCount();
    float dt = g_lastT ? (now - g_lastT) / 1000.0f : 0.016f;
    if (dt > 0.1f) dt = 0.1f;
    g_lastT = now;
    if (g_filter) ApplyFilter(f, g_filter, g_fInt);
    if (g_xray) ApplyXRay(f);
    if (g_weather) StepWeather(f, dt, now);
    if (g_vig > 0) ApplyVignette(f, g_vig);
    if (g_scan > 0) ApplyScan(f, g_scan);
}

typedef int (WINAPI *PFN_SETDIB)(HDC, int, int, DWORD, DWORD, int, int, UINT, UINT, const void*, const BITMAPINFO*, UINT);
typedef int (WINAPI *PFN_STRETCH)(HDC, int, int, int, int, int, int, int, int, const void*, const BITMAPINFO*, UINT, DWORD);
static PFN_SETDIB g_origSetDib = NULL;
static PFN_STRETCH g_origStretch = NULL;
static BYTE* g_tmp = NULL;
static size_t g_tmpSize = 0;

// копия кадра -> эффекты -> отдаём игровой функции копию (исходный буфер игры не портим)
static const void* ProcessBits(const void* bits, const BITMAPINFO* bi) {
    if (!bits || !bi || !NeedFx()) return bits;
    const BITMAPINFOHEADER& h = bi->bmiHeader;
    if ((h.biBitCount != 32 && h.biBitCount != 24) || h.biCompression != 0 || h.biWidth <= 0 || h.biHeight == 0) return bits;
    Frame f;
    f.bpp = h.biBitCount / 8;
    f.w = h.biWidth;
    f.h = h.biHeight < 0 ? -h.biHeight : h.biHeight;
    f.topDown = h.biHeight < 0;
    f.stride = ((f.w * h.biBitCount + 31) / 32) * 4;
    if (f.w > 4096 || f.h > 4096) return bits;
    size_t need = (size_t)f.stride * f.h;
    if (need > g_tmpSize) {
        free(g_tmp);
        g_tmp = (BYTE*)malloc(need);
        g_tmpSize = g_tmp ? need : 0;
        if (!g_tmp) return bits;
    }
    memcpy(g_tmp, bits, need);
    f.p = g_tmp;
    RunEffects(f);
    return g_tmp;
}
static int WINAPI HkSetDib(HDC dc, int xd, int yd, DWORD w, DWORD h, int xs, int ys, UINT start, UINT lines,
                           const void* bits, const BITMAPINFO* bi, UINT use) {
    InterlockedIncrement(&g_inHook);
    const void* src = bits;
    if (start == 0 && bi && lines == (UINT)abs(bi->bmiHeader.biHeight)) src = ProcessBits(bits, bi);
    int r = g_origSetDib(dc, xd, yd, w, h, xs, ys, start, lines, src, bi, use);
    InterlockedDecrement(&g_inHook);
    return r;
}
static int WINAPI HkStretch(HDC dc, int xd, int yd, int wd, int hd, int xs, int ys, int ws, int hs, const void* bits,
                            const BITMAPINFO* bi, UINT use, DWORD rop) {
    InterlockedIncrement(&g_inHook);
    const void* src = ProcessBits(bits, bi);
    int r = g_origStretch(dc, xd, yd, wd, hd, xs, ys, ws, hs, src, bi, use, rop);
    InterlockedDecrement(&g_inHook);
    return r;
}
static bool HookIat(uintptr_t rva, void* hook, void** orig) {
    uintptr_t slot = g_base + rva, cur = 0;
    if (!Rd(slot, &cur, 4) || cur == 0 || (cur >= g_base && cur < g_base + EXPECT_SIZE)) return false;
    DWORD old = 0;
    if (!VirtualProtect((LPVOID)slot, 4, PAGE_READWRITE, &old)) return false;
    *orig = (void*)cur;
    *(volatile uintptr_t*)slot = (uintptr_t)hook;
    VirtualProtect((LPVOID)slot, 4, old, &old);
    return true;
}
static void UnhookIat(uintptr_t rva, void* orig) {
    if (!orig) return;
    uintptr_t slot = g_base + rva;
    DWORD old = 0;
    if (VirtualProtect((LPVOID)slot, 4, PAGE_READWRITE, &old)) {
        *(volatile uintptr_t*)slot = (uintptr_t)orig;
        VirtualProtect((LPVOID)slot, 4, old, &old);
    }
}

// ================================================================== скорость игры: дополнительный таймер вызывает TimeProc игры
static MMRESULT g_speedTimer = 0;
static UINT g_speedPeriod = 0;
static void SetExtraSpeed(double m) {
    if (!g_speedOk) return;
    UINT period = 0;
    if (m > 1.04) {
        double p = 16.0 / (m - 1.0);
        period = (UINT)(p + 0.5);
        if (period < 2) period = 2;
        if (period > 400) period = 400;
    }
    if (period == g_speedPeriod) return;
    if (g_speedTimer) { timeKillEvent(g_speedTimer); g_speedTimer = 0; }
    g_speedPeriod = period;
    if (period) g_speedTimer = timeSetEvent(period, 0, (LPTIMECALLBACK)(g_base + RVA_TIMEPROC), 0, TIME_PERIODIC);
}
static void UpdateSpeed(bool shiftHeld) {
    double m = g_speed10 / 10.0;
    if (g_turbo && shiftHeld && m < 2.0) m = 2.0;
    SetExtraSpeed(m);
}

// ================================================================== установка хуков
static bool InstallHooks() {
    HMODULE exe = GetModuleHandleW(NULL);
    g_base = (uintptr_t)exe;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)exe;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)((BYTE*)exe + dos->e_lfanew);
    if (nt->OptionalHeader.SizeOfImage != EXPECT_SIZE || nt->FileHeader.TimeDateStamp != EXPECT_STAMP) return false;

    static const BYTE sDraw[] = {0x53, 0x8B, 0xDC, 0x83, 0xEC, 0x08, 0x83, 0xE4, 0xF8};
    static const BYTE sGod[] = {0x83, 0x7E, 0x60, 0x00, 0x0F, 0x8F, 0xA2, 0x00, 0x00, 0x00, 0x8B, 0x4E, 0x5C};
    static const BYTE sScore[] = {0x01, 0x4E, 0x6C, 0x79, 0x07, 0xC7, 0x46, 0x6C};
    static const BYTE sTime[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8, 0x83, 0xEC, 0x08, 0x8D, 0x04, 0x24, 0x50, 0xFF, 0x15};
    bool codeOk = Verify(RVA_DRAW, sDraw, sizeof sDraw) && Verify(RVA_GOD, sGod, sizeof sGod) && Verify(RVA_SCORE, sScore, sizeof sScore);
    g_speedOk = Verify(RVA_TIMEPROC, sTime, sizeof sTime);
    g_visOk = HookIat(RVA_IAT_SETDIB, (void*)HkSetDib, (void**)&g_origSetDib) & HookIat(RVA_IAT_STRETCH, (void*)HkStretch, (void**)&g_origStretch);
    if (!codeOk) return true;   // версия верная, но патчи кода не поставить: остальное всё равно работает

    g_code = (BYTE*)VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!g_code) return true;
    memset(g_code, 0xCC, 4096);
    g_sh = (Shared*)g_code;
    g_sh->game = 0; g_sh->godFlag = 0; g_sh->mult = 1; g_sh->frames = 0;

    BYTE* draw = g_code + 0x20;
    BYTE* p = draw;
    *p++ = 0x89; *p++ = 0x0D; EmitU32(p, (uint32_t)(uintptr_t)&g_sh->game);        // mov [game], ecx
    *p++ = 0xFF; *p++ = 0x05; EmitU32(p, (uint32_t)(uintptr_t)&g_sh->frames);      // inc dword [frames]
    *p++ = 0x53; *p++ = 0x8B; *p++ = 0xDC; *p++ = 0x83; *p++ = 0xEC; *p++ = 0x08;  // исходные инструкции
    EmitJmp(p, g_base + RVA_DRAW_RET);

    BYTE* god = g_code + 0x60;
    p = god;
    *p++ = 0x80; *p++ = 0x3D; EmitU32(p, (uint32_t)(uintptr_t)&g_sh->godFlag); *p++ = 0x00;  // cmp byte [flag],0
    *p++ = 0x74; *p++ = 0x05;                                                                 // je L_orig
    *p++ = 0x83; *p++ = 0xF8; *p++ = 0x9D;                                                    // cmp eax,-99
    *p++ = 0x7D; *p++ = 0x0B;                                                                 // jge L_skip
    *p++ = 0x83; *p++ = 0x7E; *p++ = 0x60; *p++ = 0x00;                                       // L_orig: cmp [esi+0x60],0
    *p++ = 0x7F; *p++ = 0x05;                                                                 // jg L_skip
    EmitJmp(p, g_base + RVA_GOD_CONT);
    EmitJmp(p, g_base + RVA_GOD_SKIP);                                                        // L_skip

    BYTE* sc = g_code + 0xA0;
    p = sc;
    *p++ = 0x0F; *p++ = 0xAF; *p++ = 0x0D; EmitU32(p, (uint32_t)(uintptr_t)&g_sh->mult);      // imul ecx,[mult]
    *p++ = 0x01; *p++ = 0x4E; *p++ = 0x6C;                                                    // add [esi+0x6c],ecx
    *p++ = 0x79; *p++ = 0x05;                                                                 // jns L_done
    EmitJmp(p, g_base + RVA_SCORE_NEG);
    EmitJmp(p, g_base + RVA_SCORE_DONE);

    g_patches.push_back(MakeJmpPatch(RVA_DRAW, 6, (uintptr_t)draw));
    g_patches.push_back(MakeJmpPatch(RVA_GOD, 10, (uintptr_t)god));
    g_patches.push_back(MakeJmpPatch(RVA_SCORE, 5, (uintptr_t)sc));
    FlushInstructionCache(GetCurrentProcess(), g_code, 4096);
    g_hooked = ApplyPatches(true);
    if (g_hooked) g_sh->mult = g_mult;
    return true;
}

static DWORD WINAPI Worker(LPVOID) {
    DWORD lastFrames = 0, lastChange = GetTickCount();
    while (!g_quit) {
        if (g_sh) {
            DWORD f = g_sh->frames, now = GetTickCount();
            if (f != lastFrames) { lastFrames = f; lastChange = now; }
            g_fresh = g_sh->game != 0 && f != 0 && (now - lastChange) < 400;
            if (g_fresh) {
                uintptr_t g = g_sh->game;
                int lives = -1, en = -1, sc = 0;
                if (Rd(g + OFF_LIVES, &lives, 4) && Rd(g + OFF_ENERGY, &en, 4) && lives >= 0 && lives <= 99 && en >= 0 && en <= 100) {
                    Rd(g + OFF_SCORE, &sc, 4);
                    g_lives = lives; g_energy = en; g_score = sc;
                    if (g_infLives && lives > 0 && lives < 3) { int v = 3; Wr(g + OFF_LIVES, &v, 4); }
                    if (g_infEnergy && en > 0 && en < 100) { int v = 100; Wr(g + OFF_ENERGY, &v, 4); }
                }
            }
        }
        Sleep(5);
    }
    return 0;
}


// ================================================================== модель героя: вращение / качание / зеркало / размер / радуга
// Спрайт-лист героя (Sprites/dizzy.tga: 44 кадра по 100x88, цвет-ключ в пикселе (0,0)) игра держит в памяти.
// DLL читает такой же файл с диска, находит копию листа в памяти игры, запоминает оригинал и каждый тик
// записывает в лист кадры, повёрнутые/масштабированные/перекрашенные. Игра сама рисует уже изменённого героя.
// ===== hero begin
struct HeroFmt { int bpp, pr, pg, pb; };
static const HeroFmt HF[6] = {{3, 2, 1, 0}, {3, 0, 1, 2}, {4, 2, 1, 0}, {4, 0, 1, 2}, {4, 1, 2, 3}, {4, 3, 2, 1}};
struct HeroCopy { uintptr_t base; HeroFmt f; bool flipped; std::vector<uint8_t> orig, keyPx; };
static const int HERO_FW = 100;
static int g_hw = 0, g_hh = 0, g_hframes = 0;
static double g_hA = 44, g_hB = 42;                 // половины размеров содержимого кадра (для подгонки при повороте)
static std::vector<uint8_t> g_hfile, g_hmask;       // RGB (3 байта) и маска непрозрачности, строки сверху вниз
static std::vector<HeroCopy> g_hcopies;
static volatile int g_hstate = 0;                   // 0 не искали, 1 ищу, 2 найдено, 3 не найдено, 4 устарело, 5 нет файла
static std::vector<uint8_t> g_hraw;                 // сырой файл (держим во время поиска, чтобы не принять его за копию игры)

static bool HeroParse(const std::vector<uint8_t>& raw) {
    if (raw.size() < 18 || raw[2] != 2) return false;
    int idlen = raw[0], w = raw[12] | (raw[13] << 8), h = raw[14] | (raw[15] << 8), bpp = raw[16], desc = raw[17];
    if (bpp != 24 || w != 4400 || h != 88) return false;
    size_t off = 18 + idlen;
    if (raw.size() < off + (size_t)w * h * 3) return false;
    g_hw = w; g_hh = h; g_hframes = w / HERO_FW;
    g_hfile.assign((size_t)w * h * 3, 0);
    g_hmask.assign((size_t)w * h, 0);
    bool topDown = (desc & 0x20) != 0;
    for (int y = 0; y < h; y++) {
        const uint8_t* src = &raw[off + (size_t)(topDown ? y : h - 1 - y) * w * 3];
        for (int x = 0; x < w; x++) {
            uint8_t* d = &g_hfile[((size_t)y * w + x) * 3];
            d[0] = src[x * 3 + 2]; d[1] = src[x * 3 + 1]; d[2] = src[x * 3];
        }
    }
    const uint8_t kr = g_hfile[0], kg = g_hfile[1], kb = g_hfile[2];
    int minx = HERO_FW, maxx = 0, miny = h, maxy = 0;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const uint8_t* p = &g_hfile[((size_t)y * w + x) * 3];
            bool op = !(p[0] == kr && p[1] == kg && p[2] == kb);
            g_hmask[(size_t)y * w + x] = op;
            if (op) { int cx = x % HERO_FW; minx = std::min(minx, cx); maxx = std::max(maxx, cx); miny = std::min(miny, y); maxy = std::max(maxy, y); }
        }
    g_hA = std::max(HERO_FW / 2.0 - minx, maxx + 1 - HERO_FW / 2.0);
    g_hB = std::max(h / 2.0 - miny, maxy + 1 - h / 2.0);
    return true;
}
static bool HeroPickSig(int& sx, int& sy) {
    sy = g_hh / 2;
    for (int x = HERO_FW; x + 6 < g_hw; x++) {
        bool ok = true;
        for (int k = 0; k < 6 && ok; k++) {
            if (!g_hmask[(size_t)sy * g_hw + x + k]) { ok = false; break; }
            for (int j = k + 1; j < 6; j++)
                if (memcmp(&g_hfile[((size_t)sy * g_hw + x + k) * 3], &g_hfile[((size_t)sy * g_hw + x + j) * 3], 3) == 0) { ok = false; break; }
        }
        if (ok) { sx = x; return true; }
    }
    return false;
}
static inline uintptr_t HeroAddr(const HeroCopy& c, int x, int y) {
    int ry = c.flipped ? g_hh - 1 - y : y;
    return c.base + ((size_t)ry * g_hw + x) * c.f.bpp;
}
// проверка кандидата: одинаков ли весь лист в памяти с файлом (по 800 выборочным непрозрачным пикселям)
static bool HeroVerify(uintptr_t sigAddr, int sx, int sy, const HeroFmt& f, HeroCopy& out) {
    const size_t total = (size_t)g_hw * g_hh;
    for (int flip = 0; flip < 2; flip++) {
        size_t rowIdx = (size_t)(flip ? g_hh - 1 - sy : sy);
        size_t back = (rowIdx * g_hw + sx) * f.bpp;
        if (sigAddr < back) continue;
        HeroCopy c;
        c.base = sigAddr - back;
        c.f = f;
        c.flipped = flip != 0;
        c.orig.resize(total * f.bpp);
        if (!Rd(c.base, &c.orig[0], c.orig.size())) continue;
        int tested = 0, good = 0;
        uint64_t seed = 88172645463325252ull;
        for (int i = 0; i < 4000 && tested < 800; i++) {
            seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
            size_t pos = (size_t)(seed % total);
            if (!g_hmask[pos]) continue;
            int x = (int)(pos % g_hw), y = (int)(pos / g_hw);
            const uint8_t* m = &c.orig[((size_t)(flip ? g_hh - 1 - y : y) * g_hw + x) * f.bpp];
            const uint8_t* fl = &g_hfile[pos * 3];
            tested++;
            if (m[f.pr] == fl[0] && m[f.pg] == fl[1] && m[f.pb] == fl[2]) good++;
        }
        if (tested >= 200 && good * 100 >= tested * 97) {
            const uint8_t* k = &c.orig[(size_t)(flip ? g_hh - 1 : 0) * g_hw * f.bpp];
            c.keyPx.assign(k, k + f.bpp);
            out = c;
            return true;
        }
    }
    return false;
}
static void HeroRegions(std::vector<std::pair<uintptr_t, size_t> >& out);   // перечень областей памяти для поиска (ниже / в тесте)
struct HeroHit { uintptr_t addr; int var; };
static void HeroScanBuf(const uint8_t* b, size_t n, uintptr_t addr, const uint8_t* sig, std::vector<HeroHit>& hits) {
    for (int vi = 0; vi < 6; vi++) {
        const HeroFmt& f = HF[vi];
        const size_t span = (size_t)5 * f.bpp + f.bpp, step = f.bpp == 3 ? 1 : 4;
        if (n < span) continue;
        const uint8_t s0 = sig[0], s1 = sig[1], s2 = sig[2];
        for (size_t i = 0; i + span <= n; i += step) {
            if (b[i + f.pr] != s0 || b[i + f.pg] != s1 || b[i + f.pb] != s2) continue;
            bool ok = true;
            for (int k = 1; k < 6 && ok; k++) {
                const uint8_t* q = b + i + (size_t)k * f.bpp;
                ok = q[f.pr] == sig[k * 3] && q[f.pg] == sig[k * 3 + 1] && q[f.pb] == sig[k * 3 + 2];
            }
            if (ok) { HeroHit h; h.addr = addr + i; h.var = vi; hits.push_back(h); }
        }
    }
}
static bool HeroSearch(const std::wstring& tgaPath) {
    g_hcopies.clear();
    g_hraw.clear();
    HANDLE fh = CreateFileW(tgaPath.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (fh == INVALID_HANDLE_VALUE) { g_hstate = 5; return false; }
    DWORD sz = GetFileSize(fh, NULL), rd = 0;
    g_hraw.resize(sz);
    if (sz) ReadFile(fh, &g_hraw[0], sz, &rd, NULL);
    CloseHandle(fh);
    if (!HeroParse(g_hraw)) { g_hraw.clear(); g_hstate = 5; return false; }
    int sx = 0, sy = 0;
    if (!HeroPickSig(sx, sy)) { g_hstate = 3; return false; }
    uint8_t sig[18];
    memcpy(sig, &g_hfile[((size_t)sy * g_hw + sx) * 3], 18);
    g_hstate = 1;

    std::vector<std::pair<uintptr_t, size_t> > regs;
    HeroRegions(regs);
    const size_t CH = 4u << 20;
    std::vector<uint8_t> buf(CH);
    std::vector<HeroHit> hits;
    const uintptr_t bufLo = (uintptr_t)buf.data(), fileLo = (uintptr_t)g_hfile.data(), rawLo = (uintptr_t)g_hraw.data();
    for (size_t r = 0; r < regs.size(); r++) {
        uintptr_t lo = regs[r].first, hi = lo + regs[r].second;
        if ((bufLo >= lo && bufLo < hi) || (fileLo >= lo && fileLo < hi) || (rawLo >= lo && rawLo < hi)) {
            // область с нашими буферами: ищем только вне их, поэтому просто пропускаем (текстуры игры там нет)
            continue;
        }
        for (uintptr_t p = lo; p < hi; p += CH) {
            size_t n = (size_t)std::min<uintptr_t>(CH, hi - p);
            if (!Rd(p, buf.data(), n)) continue;
            HeroScanBuf(buf.data(), n, p, sig, hits);
        }
    }
    for (size_t i = 0; i < hits.size() && g_hcopies.size() < 4; i++) {
        HeroCopy c;
        if (HeroVerify(hits[i].addr, sx, sy, HF[hits[i].var], c)) {
            bool dup = false;
            for (size_t k = 0; k < g_hcopies.size(); k++) if (g_hcopies[k].base == c.base) dup = true;
            if (!dup) g_hcopies.push_back(c);
        }
    }
    g_hraw.clear();
    g_hraw.shrink_to_fit();
    g_hstate = g_hcopies.empty() ? 3 : 2;
    return !g_hcopies.empty();
}
// карта обратного преобразования для одной ячейки кадра: куда в исходном кадре смотрит каждый пиксель
static void HeroBuildMap(std::vector<int>& map, double theta, double scale, bool mirror) {
    const double c = cos(theta), s = sin(theta), cx = HERO_FW / 2.0, cy = g_hh / 2.0;
    map.assign((size_t)HERO_FW * g_hh, -1);
    for (int dy = 0; dy < g_hh; dy++)
        for (int dx = 0; dx < HERO_FW; dx++) {
            double u = dx + 0.5 - cx, v = dy + 0.5 - cy;
            double x = (c * u + s * v) / scale, y = (-s * u + c * v) / scale;
            int sx = (int)floor(x + cx), sy = (int)floor(y + cy);
            if (mirror) sx = HERO_FW - 1 - sx;
            if (sx >= 0 && sx < HERO_FW && sy >= 0 && sy < g_hh) map[(size_t)dy * HERO_FW + dx] = sy * HERO_FW + sx;
        }
}
static void HeroWrite(const HeroCopy& c, const std::vector<int>& map, bool rainbow, double hueRad) {
    double ca = cos(hueRad), sa = sin(hueRad);
    const double m[9] = {.213 + ca * .787 - sa * .213, .715 - ca * .715 - sa * .715, .072 - ca * .072 + sa * .928,
                         .213 - ca * .213 + sa * .143, .715 + ca * .285 + sa * .140, .072 - ca * .072 - sa * .283,
                         .213 - ca * .213 - sa * .787, .715 - ca * .715 + sa * .715, .072 + ca * .928 + sa * .072};
    const int bpp = c.f.bpp;
    for (int fr = 0; fr < g_hframes; fr++)
        for (int dy = 0; dy < g_hh; dy++) {
            uint8_t* drow = (uint8_t*)HeroAddr(c, fr * HERO_FW, dy);
            for (int dx = 0; dx < HERO_FW; dx++) {
                uint8_t* d = drow + (size_t)dx * bpp;
                int mi = map[(size_t)dy * HERO_FW + dx];
                if (mi < 0) { memcpy(d, &c.keyPx[0], bpp); continue; }
                int sx = fr * HERO_FW + mi % HERO_FW, sy = mi / HERO_FW;
                const uint8_t* src = &c.orig[((size_t)(c.flipped ? g_hh - 1 - sy : sy) * g_hw + sx) * bpp];
                memcpy(d, src, bpp);
                if (rainbow && g_hmask[(size_t)sy * g_hw + sx]) {
                    const uint8_t* rgb = &g_hfile[((size_t)sy * g_hw + sx) * 3];
                    double r = rgb[0], g = rgb[1], b = rgb[2];
                    int nr = (int)(m[0] * r + m[1] * g + m[2] * b), ng = (int)(m[3] * r + m[4] * g + m[5] * b), nb = (int)(m[6] * r + m[7] * g + m[8] * b);
                    d[c.f.pr] = (uint8_t)(nr < 0 ? 0 : nr > 255 ? 255 : nr);
                    d[c.f.pg] = (uint8_t)(ng < 0 ? 0 : ng > 255 ? 255 : ng);
                    d[c.f.pb] = (uint8_t)(nb < 0 ? 0 : nb > 255 ? 255 : nb);
                }
            }
        }
}
static void HeroRestore(const HeroCopy& c) { memcpy((void*)c.base, c.orig.data(), c.orig.size()); }
// подгонка масштаба: повёрнутый кадр не должен вылезать за ячейку 100x88
static double HeroFit(double theta) {
    double cs = fabs(cos(theta)), sn = fabs(sin(theta));
    double fx = (HERO_FW / 2.0) / (g_hA * cs + g_hB * sn), fy = (g_hh / 2.0) / (g_hA * sn + g_hB * cs);
    double f = fx < fy ? fx : fy;
    return f > 1.0 ? 1.0 : f;
}
// ===== hero end

static void HeroRegions(std::vector<std::pair<uintptr_t, size_t> >& out) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    uintptr_t a = (uintptr_t)si.lpMinimumApplicationAddress;
    const uintptr_t maxa = (uintptr_t)si.lpMaximumApplicationAddress;
    MEMORY_BASIC_INFORMATION mbi;
    while (a < maxa && VirtualQuery((LPCVOID)a, &mbi, sizeof mbi)) {
        uintptr_t base = (uintptr_t)mbi.BaseAddress, end = base + mbi.RegionSize;
        if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE && mbi.Protect == PAGE_READWRITE) out.push_back(std::make_pair(base, (size_t)mbi.RegionSize));
        if (end <= base) break;
        a = end;
    }
}
static bool HeroValid() {
    for (size_t i = 0; i < g_hcopies.size(); i++) {
        MEMORY_BASIC_INFORMATION mbi;
        const HeroCopy& c = g_hcopies[i];
        if (!VirtualQuery((LPCVOID)c.base, &mbi, sizeof mbi)) return false;
        if (mbi.State != MEM_COMMIT || mbi.Protect != PAGE_READWRITE) return false;
        if (c.base + c.orig.size() > (uintptr_t)mbi.BaseAddress + mbi.RegionSize) return false;
    }
    return !g_hcopies.empty();
}
static std::wstring GameDirFile(const wchar_t* rel) {
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(NULL, p, MAX_PATH);
    std::wstring s = p;
    size_t k = s.find_last_of(L"\\/");
    return (k == std::wstring::npos ? L"" : s.substr(0, k + 1)) + rel;
}
static void HeroLoop(bool& dirty, DWORD& t0, int& sigPrev, double& lastTheta, double& lastScale, int& lastMir, int& lastHue) {
    bool want = g_heroMode != 0 || g_heroSize != 100 || g_rainbow;
    int sig = g_heroMode * 1000003 + g_heroSize * 31 + (g_rainbow ? 7 : 0);
    if (want && g_hstate != 2 && g_hstate != 1 && (g_hstate == 0 || g_hstate == 4 || sig != sigPrev)) {
        HeroSearch(GameDirFile(L"Sprites\\dizzy.tga"));
        t0 = GetTickCount();
        lastTheta = 1e9;
    }
    sigPrev = sig;
    if (g_hstate != 2) return;
    if (!HeroValid()) { g_hcopies.clear(); g_hstate = 4; dirty = false; return; }
    if (!want) {
        if (dirty) { for (size_t i = 0; i < g_hcopies.size(); i++) HeroRestore(g_hcopies[i]); dirty = false; lastTheta = 1e9; }
        return;
    }
    double t = (GetTickCount() - t0) / 1000.0, theta = 0, spd = g_heroSpd;
    int mirror = 0;
    if (g_heroMode == 1) theta = fmod(spd * 60.0 * t, 360.0) * 3.14159265358979 / 180.0;
    else if (g_heroMode == 2) theta = sin(t * (fabs(spd) * 0.5 + 1.0)) * 0.6;
    else if (g_heroMode == 3) { double iv = 600.0 / (fabs(spd) + 1.0); mirror = ((int)(t * 1000.0 / (iv < 40 ? 40 : iv))) & 1; }
    double scale = (g_heroSize / 100.0) * (theta != 0 ? HeroFit(theta) : 1.0);
    int hue = g_rainbow ? (int)(t * 120.0) % 360 : 0;
    // не перезаписываем, если ничего не изменилось (углы квантуем до 1°)
    double qtheta = floor(theta * 57.29578) / 57.29578;
    if (qtheta == lastTheta && scale == lastScale && mirror == lastMir && hue / 4 == lastHue) return;
    lastTheta = qtheta; lastScale = scale; lastMir = mirror; lastHue = hue / 4;
    std::vector<int> map;
    HeroBuildMap(map, qtheta, scale, mirror != 0);
    for (size_t i = 0; i < g_hcopies.size(); i++) HeroWrite(g_hcopies[i], map, g_rainbow, hue * 3.14159265358979 / 180.0);
    dirty = true;
}
static DWORD WINAPI HeroThread(LPVOID) {
    bool dirty = false;
    DWORD t0 = GetTickCount();
    int sigPrev = 0, lastMir = -1, lastHue = -1;
    double lastTheta = 1e9, lastScale = -1;
    while (!g_quit) {
        HeroLoop(dirty, t0, sigPrev, lastTheta, lastScale, lastMir, lastHue);
        Sleep(20);
    }
    if (dirty && g_hstate == 2 && HeroValid()) for (size_t i = 0; i < g_hcopies.size(); i++) HeroRestore(g_hcopies[i]);
    return 0;
}

// ================================================================== интерфейс
enum {
    TAB_0 = 1, TAB_1, TAB_2, TAB_3, TAB_4,
    TOG_GOD = 10, TOG_LIVES, TOG_ENERGY, TOG_TURBO, TOG_XRAY, TOG_FX, TOG_RAIN,
    MULT_1 = 20, MULT_2, MULT_5, MULT_10,
    ACT_HEAL = 30, ACT_LIFE, ACT_S1, ACT_S2,
    SL_SPEED = 40, SL_WINT, SL_WIND, SL_FINT, SL_VIG, SL_SCAN, SL_HSPD, SL_HSIZE, SL_OPAC,
    WE_0 = 60,   // 60..66 погода
    FI_0 = 70,   // 70..77 фильтры
    ACC_0 = 80,  // 80..87 акцент
    TH_0 = 90,   // 90..92 тема
    SC_0 = 95,   // 95..97 размер
    DK_0 = 100,  // 100..102 положение
    HM_0 = 120,  // 120..123 режим модели героя
    BTN_CLOSE = 110, BTN_EJECT
};

static const int W = 420, H = 650;
static COLORREF C_BG, C_CARD, C_HOT, C_TEXT, C_SUB, C_OFF, C_ACC, C_HDR_TXT, C_HDR_SUB;
static const COLORREF C_OK = RGB(84, 204, 124), C_BAD = RGB(236, 92, 92), C_WARN = RGB(240, 190, 80);
struct Item { int id; RECT r; };
struct Slider { int id; RECT track; int* val; int mn, mx; };
static std::vector<Item> g_items;
static std::vector<Slider> g_sliders;
static int g_hot = 0, g_tab = 0, g_drag = 0;
static bool g_track = false;
static float g_anim[7] = {0, 0, 0, 0, 0, 0, 0};
static HFONT fTitle, fH, fSmall, fTab;
static bool g_keyDown[6] = {false, false, false, false, false, false};

static void Toast(const wchar_t* s) { g_toast = s; g_toastUntil = GetTickCount() + 2800; }
static COLORREF Mix(COLORREF a, COLORREF b, float t) {
    return RGB((int)(GetRValue(a) + (GetRValue(b) - GetRValue(a)) * t), (int)(GetGValue(a) + (GetGValue(b) - GetGValue(a)) * t),
               (int)(GetBValue(a) + (GetBValue(b) - GetBValue(a)) * t));
}
static COLORREF Hsv(float h, float s, float v) {
    float c = v * s, x = c * (1 - fabsf(fmodf(h / 60.0f, 2) - 1)), m = v - c, r = 0, g = 0, b = 0;
    int i = (int)(h / 60.0f) % 6;
    switch (i) { case 0: r = c; g = x; break; case 1: r = x; g = c; break; case 2: g = c; b = x; break;
                 case 3: g = x; b = c; break; case 4: r = x; b = c; break; default: r = c; b = x; break; }
    return RGB((int)((r + m) * 255), (int)((g + m) * 255), (int)((b + m) * 255));
}
static const COLORREF ACCENTS[7] = {RGB(255, 159, 67), RGB(80, 150, 255), RGB(84, 204, 124), RGB(150, 110, 255),
                                    RGB(240, 90, 90), RGB(255, 120, 190), RGB(60, 210, 210)};
static COLORREF AccentColor(int i) { return i >= 7 ? Hsv((float)((GetTickCount() / 25) % 360), 0.65f, 1.0f) : ACCENTS[i]; }
static void UpdateTheme() {
    if (g_theme == 1) { C_BG = RGB(242, 244, 248); C_CARD = RGB(255, 255, 255); C_HOT = RGB(230, 234, 244); C_TEXT = RGB(28, 30, 38); C_SUB = RGB(110, 116, 134); C_OFF = RGB(190, 195, 208); }
    else if (g_theme == 2) { C_BG = RGB(0, 0, 0); C_CARD = RGB(18, 18, 20); C_HOT = RGB(32, 32, 38); C_TEXT = RGB(240, 242, 248); C_SUB = RGB(130, 136, 154); C_OFF = RGB(60, 64, 78); }
    else { C_BG = RGB(20, 22, 28); C_CARD = RGB(30, 33, 43); C_HOT = RGB(40, 44, 58); C_TEXT = RGB(236, 239, 246); C_SUB = RGB(139, 146, 166); C_OFF = RGB(66, 71, 88); }
    C_ACC = AccentColor(g_accent);
    int lum = (GetRValue(C_ACC) * 3 + GetGValue(C_ACC) * 6 + GetBValue(C_ACC)) / 10;
    C_HDR_TXT = lum > 150 ? RGB(40, 24, 10) : RGB(255, 255, 255);
    C_HDR_SUB = lum > 150 ? RGB(88, 50, 20) : RGB(235, 235, 245);
}

static void FillRound(HDC dc, RECT r, int rad, COLORREF fill) {
    HBRUSH b = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_NULL, 0, 0);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, pen);
    RoundRect(dc, r.left, r.top, r.right + 1, r.bottom + 1, rad, rad);
    SelectObject(dc, ob); SelectObject(dc, op);
    DeleteObject(b); DeleteObject(pen);
}
static void Dot(HDC dc, int cx, int cy, int rad, COLORREF c) {
    HBRUSH b = CreateSolidBrush(c);
    HPEN pen = CreatePen(PS_NULL, 0, 0);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, pen);
    Ellipse(dc, cx - rad, cy - rad, cx + rad + 1, cy + rad + 1);
    SelectObject(dc, ob); SelectObject(dc, op);
    DeleteObject(b); DeleteObject(pen);
}
static void Txt(HDC dc, const std::wstring& s, RECT r, HFONT f, COLORREF c, UINT fmt) {
    SelectObject(dc, f);
    SetTextColor(dc, c);
    DrawTextW(dc, s.c_str(), -1, &r, fmt | DT_NOPREFIX | DT_SINGLELINE);
}
static void AddItem(int id, RECT r) { Item it; it.id = id; it.r = r; g_items.push_back(it); }
static void Label(HDC dc, int y, const wchar_t* s) { RECT r = {20, y, W - 20, y + 18}; Txt(dc, s, r, fSmall, C_SUB, DT_LEFT); }

static void ToggleRow(HDC dc, int id, int ai, int y, const wchar_t* title, const wchar_t* desc) {
    RECT r = {16, y, W - 16, y + 56};
    FillRound(dc, r, 14, g_hot == id ? C_HOT : C_CARD);
    RECT t1 = {32, y + 8, W - 100, y + 28}, t2 = {32, y + 29, W - 100, y + 48};
    Txt(dc, title, t1, fH, C_TEXT, DT_LEFT);
    Txt(dc, desc, t2, fSmall, C_SUB, DT_LEFT | DT_END_ELLIPSIS);
    RECT sw = {W - 74, y + 16, W - 28, y + 40};
    FillRound(dc, sw, 24, Mix(C_OFF, C_ACC, g_anim[ai]));
    Dot(dc, (int)(sw.left + 12 + g_anim[ai] * 22), y + 28, 9, RGB(255, 255, 255));
    AddItem(id, r);
}
static void Btn(HDC dc, int id, RECT r, const wchar_t* text, bool accent, bool selected) {
    bool hot = g_hot == id;
    COLORREF bg = selected ? C_ACC : (accent ? Mix(C_ACC, RGB(255, 255, 255), hot ? 0.25f : 0.0f) : (hot ? C_HOT : C_CARD));
    FillRound(dc, r, 12, bg);
    bool dark = (selected || accent);
    int lum = (GetRValue(C_ACC) * 3 + GetGValue(C_ACC) * 6 + GetBValue(C_ACC)) / 10;
    Txt(dc, text, r, (r.right - r.left) < 80 ? fTab : fH, dark ? (lum > 150 ? RGB(30, 22, 12) : RGB(255, 255, 255)) : C_TEXT, DT_CENTER | DT_VCENTER);
    AddItem(id, r);
}
static void Grid(HDC dc, int baseId, const wchar_t* const* labels, int n, int cols, int y, int sel) {
    int gap = 8, bw = (W - 32 - (cols - 1) * gap) / cols;
    for (int i = 0; i < n; i++) {
        int cx = i % cols, cy = i / cols;
        RECT r = {16 + cx * (bw + gap), y + cy * 44, 16 + cx * (bw + gap) + bw, y + cy * 44 + 36};
        Btn(dc, baseId + i, r, labels[i], false, sel == i);
    }
}
static void SliderRow(HDC dc, int id, int y, const wchar_t* title, const std::wstring& valText, int* val, int mn, int mx) {
    RECT r = {16, y, W - 16, y + 56};
    FillRound(dc, r, 14, C_CARD);
    RECT t1 = {32, y + 8, W - 130, y + 28}, t2 = {W - 130, y + 8, W - 32, y + 28};
    Txt(dc, title, t1, fH, C_TEXT, DT_LEFT);
    Txt(dc, valText, t2, fH, C_ACC, DT_RIGHT);
    RECT tr = {32, y + 38, W - 32, y + 44};
    FillRound(dc, tr, 6, C_OFF);
    float t = (float)(*val - mn) / (mx - mn);
    RECT fl = tr;
    fl.right = tr.left + (int)((tr.right - tr.left) * t);
    if (fl.right > fl.left + 4) FillRound(dc, fl, 6, C_ACC);
    Dot(dc, tr.left + (int)((tr.right - tr.left) * t), y + 41, g_drag == id ? 9 : 8, C_ACC);
    Slider s; s.id = id; s.track = tr; s.val = val; s.mn = mn; s.mx = mx;
    g_sliders.push_back(s);
    RECT hit = {16, y, W - 16, y + 56};
    AddItem(id, hit);
}
static std::wstring Fmt(const wchar_t* f, int v) { wchar_t b[48]; _snwprintf(b, 48, f, v); return b; }

static void Paint(HDC dc) {
    g_items.clear();
    g_sliders.clear();
    UpdateTheme();
    RECT all = {0, 0, W, H};
    HBRUSH bg = CreateSolidBrush(C_BG);
    FillRect(dc, &all, bg);
    DeleteObject(bg);
    SetBkMode(dc, TRANSPARENT);

    COLORREF c1 = Mix(C_ACC, RGB(255, 255, 255), 0.18f), c2 = Mix(C_ACC, RGB(0, 0, 0), 0.25f);
    for (int x = 0; x < W; x++) {
        HBRUSH b = CreateSolidBrush(Mix(c1, c2, (float)x / W));
        RECT c = {x, 0, x + 1, 78};
        FillRect(dc, &c, b);
        DeleteObject(b);
    }
    RECT t = {22, 12, W - 60, 46}, st = {22, 46, W - 60, 68};
    Txt(dc, L"ХЛЕБУШЕК", t, fTitle, C_HDR_TXT, DT_LEFT | DT_VCENTER);
    Txt(dc, L"Mod Menu  ·  Insert — показать / скрыть", st, fSmall, C_HDR_SUB, DT_LEFT | DT_VCENTER);
    RECT cl = {W - 46, 18, W - 18, 46};
    FillRound(dc, cl, 28, g_hot == BTN_CLOSE ? RGB(255, 255, 255) : Mix(C_ACC, RGB(255, 255, 255), 0.5f));
    Txt(dc, L"×", cl, fH, RGB(50, 30, 15), DT_CENTER | DT_VCENTER);
    AddItem(BTN_CLOSE, cl);

    // статус
    RECT sc = {16, 88, W - 16, 144};
    FillRound(dc, sc, 14, C_CARD);
    const wchar_t* msg;
    COLORREF col;
    if (!g_supported) { msg = L"Версия игры не поддерживается"; col = C_BAD; }
    else if (!g_hooked) { msg = L"Часть функций недоступна"; col = C_WARN; }
    else if (g_fresh) { msg = L"Игра подключена — читы активны"; col = C_OK; }
    else { msg = L"Жду начала игры (загрузи уровень)"; col = C_WARN; }
    Dot(dc, 34, 106, 5, col);
    RECT s1 = {46, 96, W - 28, 116};
    Txt(dc, msg, s1, fH, C_TEXT, DT_LEFT | DT_VCENTER);
    wchar_t line[128];
    bool toast = GetTickCount() < g_toastUntil;
    if (toast) _snwprintf(line, 128, L"%s", g_toast.c_str());
    else if (g_fresh) _snwprintf(line, 128, L"Жизни: %d     Энергия: %d     Очки: %d", g_lives, g_energy, g_score);
    else _snwprintf(line, 128, L"Погода, эффекты, скорость и бессмертие работают сразу");
    RECT s2 = {46, 118, W - 28, 138};
    Txt(dc, line, s2, fSmall, toast ? C_ACC : C_SUB, DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);

    // вкладки
    const wchar_t* tabs[5] = {L"Игрок", L"Мир", L"Эффекты", L"Герой", L"Меню"};
    int tw = (W - 32 - 4 * 6) / 5;
    for (int i = 0; i < 5; i++) {
        RECT r = {16 + i * (tw + 6), 154, 16 + i * (tw + 6) + tw, 188};
        Btn(dc, TAB_0 + i, r, tabs[i], false, g_tab == i);
    }
    const int Y = 198;

    if (g_tab == 0) {
        Label(dc, Y, L"ЗАЩИТА");
        ToggleRow(dc, TOG_GOD, 0, Y + 22, L"Бессмертие", L"Враги и ловушки не наносят урон (F1)");
        ToggleRow(dc, TOG_LIVES, 1, Y + 86, L"Бесконечные жизни", L"Жизни не заканчиваются (F2)");
        ToggleRow(dc, TOG_ENERGY, 2, Y + 150, L"Вечная энергия", L"Энергия всегда полная (F3)");
        Label(dc, Y + 220, L"МНОЖИТЕЛЬ ОЧКОВ");
        const wchar_t* mt[4] = {L"×1", L"×2", L"×5", L"×10"};
        int sel = g_mult == 1 ? 0 : (g_mult == 2 ? 1 : (g_mult == 5 ? 2 : 3));
        Grid(dc, MULT_1, mt, 4, 4, Y + 242, sel);
        Label(dc, Y + 296, L"БЫСТРЫЕ ДЕЙСТВИЯ");
        RECT a1 = {16, Y + 318, 16 + (W - 40) / 2, Y + 356}, a2 = {24 + (W - 40) / 2, Y + 318, W - 16, Y + 356};
        RECT a3 = {16, Y + 364, 16 + (W - 40) / 2, Y + 402}, a4 = {24 + (W - 40) / 2, Y + 364, W - 16, Y + 402};
        Btn(dc, ACT_HEAL, a1, L"Полная энергия", true, false);
        Btn(dc, ACT_LIFE, a2, L"Жизни = 3", true, false);
        Btn(dc, ACT_S1, a3, L"+1 000 очков", false, false);
        Btn(dc, ACT_S2, a4, L"+10 000 очков", false, false);
    } else if (g_tab == 1) {
        Label(dc, Y, L"СКОРОСТЬ ИГРЫ");
        wchar_t sp[24];
        _snwprintf(sp, 24, L"×%d.%d", g_speed10 / 10, g_speed10 % 10);
        SliderRow(dc, SL_SPEED, Y + 22, L"Скорость", sp, &g_speed10, 10, 40);
        ToggleRow(dc, TOG_TURBO, 3, Y + 86, L"Быстрый бег (Shift)", L"Пока зажат Shift — игра идёт быстрее");
        Label(dc, Y + 156, L"ПОГОДА (F5 — сменить)");
        const wchar_t* we[7] = {L"Нет", L"Снег", L"Дождь", L"Гроза", L"Туман", L"Светляки", L"Листья"};
        Grid(dc, WE_0, we, 7, 4, Y + 178, g_weather);
        SliderRow(dc, SL_WINT, Y + 272, L"Интенсивность", Fmt(L"%d%%", g_wInt), &g_wInt, 1, 100);
        SliderRow(dc, SL_WIND, Y + 336, L"Ветер", g_wWind == 0 ? L"штиль" : (g_wWind < 0 ? Fmt(L"← %d", -g_wWind) : Fmt(L"%d →", g_wWind)), &g_wWind, -10, 10);
    } else if (g_tab == 2) {
        ToggleRow(dc, TOG_FX, 5, Y, L"Визуальные эффекты", L"Общий выключатель погоды и эффектов (F4)");
        Label(dc, Y + 68, L"ФИЛЬТР КАРТИНКИ");
        const wchar_t* fi[8] = {L"Нет", L"Ночь", L"Закат", L"Сепия", L"Ч/Б", L"Холод", L"Ноч. зрение", L"Инверсия"};
        Grid(dc, FI_0, fi, 8, 4, Y + 90, g_filter);
        SliderRow(dc, SL_FINT, Y + 184, L"Сила фильтра", Fmt(L"%d%%", g_fInt), &g_fInt, 0, 100);
        ToggleRow(dc, TOG_XRAY, 4, Y + 248, L"ВХ / X-Ray", L"Контуры объектов + подсветка тёмных мест");
        SliderRow(dc, SL_VIG, Y + 312, L"Виньетка", Fmt(L"%d%%", g_vig), &g_vig, 0, 100);
        SliderRow(dc, SL_SCAN, Y + 376, L"Сканлайны (CRT)", Fmt(L"%d%%", g_scan), &g_scan, 0, 100);
        if (!g_visOk) { RECT w = {20, Y + 440, W - 20, Y + 460}; Txt(dc, L"Хуки вывода кадра не установились — эффекты недоступны", w, fSmall, C_BAD, DT_LEFT); }
    } else if (g_tab == 3) {
        Label(dc, Y, L"МОДЕЛЬ ГЕРОЯ");
        const wchar_t* hm[4] = {L"Выкл", L"Вращение", L"Качание", L"Зеркало"};
        Grid(dc, HM_0, hm, 4, 4, Y + 22, g_heroMode);
        std::wstring spt = g_heroSpd == 0 ? std::wstring(L"стоп") : Fmt(g_heroSpd > 0 ? L"↻ %d°/с" : L"↺ %d°/с", (g_heroSpd < 0 ? -g_heroSpd : g_heroSpd) * 60);
        SliderRow(dc, SL_HSPD, Y + 74, L"Скорость вращения", spt, &g_heroSpd, -18, 18);
        SliderRow(dc, SL_HSIZE, Y + 138, L"Размер героя", Fmt(L"%d%%", g_heroSize), &g_heroSize, 50, 120);
        ToggleRow(dc, TOG_RAIN, 6, Y + 202, L"Радужный герой", L"Цвет героя плавно меняется по кругу");
        RECT hc = {16, Y + 272, W - 16, Y + 328};
        FillRound(dc, hc, 14, C_CARD);
        const wchar_t* hs;
        COLORREF hcol = C_WARN;
        switch (g_hstate) {
        case 1: hs = L"Ищу текстуру героя в памяти игры…"; break;
        case 2: hs = L"Текстура героя найдена — модель управляется"; hcol = C_OK; break;
        case 3: hs = L"Текстура героя в памяти игры не найдена"; hcol = C_BAD; break;
        case 4: hs = L"Текстура обновилась — ищу заново"; break;
        case 5: hs = L"Нет файла Sprites\\dizzy.tga рядом с игрой"; hcol = C_BAD; break;
        default: hs = L"Выбери режим — модель героя найдётся сама"; break;
        }
        Dot(dc, 34, Y + 292, 5, hcol);
        RECT h1 = {46, Y + 280, W - 28, Y + 304}, h2 = {46, Y + 302, W - 28, Y + 322};
        Txt(dc, hs, h1, fH, C_TEXT, DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
        wchar_t hb[96];
        _snwprintf(hb, 96, L"Копий листа спрайтов в памяти: %u", (unsigned)g_hcopies.size());
        Txt(dc, hb, h2, fSmall, C_SUB, DT_LEFT | DT_VCENTER);
    } else {
        Label(dc, Y, L"ЦВЕТ АКЦЕНТА");
        int bw = (W - 32 - 7 * 6) / 8;
        for (int i = 0; i < 8; i++) {
            RECT r = {16 + i * (bw + 6), Y + 22, 16 + i * (bw + 6) + bw, Y + 22 + bw};
            COLORREF c = AccentColor(i);
            if (g_accent == i) { RECT o = {r.left - 3, r.top - 3, r.right + 3, r.bottom + 3}; FillRound(dc, o, 14, C_TEXT); }
            FillRound(dc, r, 12, c);
            AddItem(ACC_0 + i, r);
        }
        Label(dc, Y + 80, L"ТЕМА");
        const wchar_t* th[3] = {L"Тёмная", L"Светлая", L"AMOLED"};
        Grid(dc, TH_0, th, 3, 3, Y + 102, g_theme);
        SliderRow(dc, SL_OPAC, Y + 152, L"Прозрачность меню", Fmt(L"%d%%", g_opac), &g_opac, 40, 100);
        Label(dc, Y + 222, L"РАЗМЕР МЕНЮ");
        const wchar_t* sz[3] = {L"90%", L"100%", L"115%"};
        Grid(dc, SC_0, sz, 3, 3, Y + 244, g_scale);
        Label(dc, Y + 294, L"ПОЛОЖЕНИЕ НА ЭКРАНЕ");
        const wchar_t* dk[3] = {L"Слева", L"По центру", L"Справа"};
        Grid(dc, DK_0, dk, 3, 3, Y + 316, -1);
    }
    RECT ej = {W - 150, 628, W - 16, 646};
    Txt(dc, L"Выгрузить меню", ej, fSmall, g_hot == BTN_EJECT ? C_ACC : C_SUB, DT_RIGHT);
    AddItem(BTN_EJECT, ej);
}

static float ScaleF() { return g_scale == 0 ? 0.9f : (g_scale == 2 ? 1.15f : 1.0f); }
static HWND FindGameWindow() {
    struct F { static BOOL CALLBACK cb(HWND w, LPARAM lp) {
        DWORD pid = 0;
        GetWindowThreadProcessId(w, &pid);
        if (pid == GetCurrentProcessId() && w != g_wnd && IsWindowVisible(w) && !GetWindow(w, GW_OWNER)) { *(HWND*)lp = w; return FALSE; }
        return TRUE; } };
    HWND game = NULL;
    EnumWindows(F::cb, (LPARAM)&game);
    return game;
}
static void ApplyWindowLook(bool reposition) {
    if (!g_wnd) return;
    float s = ScaleF();
    int w = (int)(W * s), h = (int)(H * s);
    RECT cur;
    GetWindowRect(g_wnd, &cur);
    int x = cur.left, y = cur.top;
    if (reposition) {
        HWND game = FindGameWindow();
        RECT gr;
        if (game && GetWindowRect(game, &gr)) {
            if (g_dock == 0) x = gr.left + 16;
            else if (g_dock == 1) x = gr.left + (gr.right - gr.left - w) / 2;
            else x = gr.right - w - 16;
            y = gr.top + 48;
        }
    }
    SetWindowPos(g_wnd, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE);
    SetWindowRgn(g_wnd, CreateRoundRectRgn(0, 0, w + 1, h + 1, 26, 26), TRUE);
    SetLayeredWindowAttributes(g_wnd, 0, (BYTE)(255 * g_opac / 100), LWA_ALPHA);
}

static void Action(int id);
static void OnSlider(int id) {
    if (id == SL_SPEED) UpdateSpeed((GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0);
    if (id == SL_OPAC) ApplyWindowLook(false);
}

static void Action(int id) {
    if (id >= TAB_0 && id <= TAB_4) { g_tab = id - TAB_0; return; }
    if (id >= HM_0 && id <= HM_0 + 3) { g_heroMode = id - HM_0; SaveSettings(); return; }
    if (id >= ACC_0 && id <= ACC_0 + 7) { g_accent = id - ACC_0; SaveSettings(); return; }
    if (id >= TH_0 && id <= TH_0 + 2) { g_theme = id - TH_0; SaveSettings(); return; }
    if (id >= SC_0 && id <= SC_0 + 2) { g_scale = id - SC_0; ApplyWindowLook(false); SaveSettings(); return; }
    if (id >= DK_0 && id <= DK_0 + 2) { g_dock = id - DK_0; ApplyWindowLook(true); SaveSettings(); return; }
    if (id >= WE_0 && id <= WE_0 + 6) { g_weather = id - WE_0; g_fx = true; SaveSettings(); return; }
    if (id >= FI_0 && id <= FI_0 + 7) { g_filter = id - FI_0; g_fx = true; SaveSettings(); return; }
    switch (id) {
    case TOG_FX: g_fx = !g_fx; SaveSettings(); return;
    case TOG_RAIN: g_rainbow = !g_rainbow; SaveSettings(); return;
    case TOG_XRAY: g_xray = !g_xray; g_fx = true; SaveSettings(); return;
    case TOG_TURBO: g_turbo = !g_turbo; SaveSettings(); Toast(g_turbo ? L"Зажми Shift в игре — ускорение" : L"Быстрый бег выключен"); return;
    case BTN_CLOSE: ShowWindow(g_wnd, SW_HIDE); return;
    case BTN_EJECT: g_eject = true; DestroyWindow(g_wnd); return;
    }
    if (!g_hooked) { Toast(L"Эта функция недоступна для данной версии игры."); return; }
    switch (id) {
    case TOG_GOD: g_god = !g_god; g_sh->godFlag = g_god ? 1 : 0; Toast(g_god ? L"Бессмертие включено" : L"Бессмертие выключено"); break;
    case TOG_LIVES: g_infLives = !g_infLives; Toast(g_infLives ? L"Бесконечные жизни включены" : L"Бесконечные жизни выключены"); break;
    case TOG_ENERGY: g_infEnergy = !g_infEnergy; Toast(g_infEnergy ? L"Вечная энергия включена" : L"Вечная энергия выключена"); break;
    case MULT_1: case MULT_2: case MULT_5: case MULT_10: {
        static const int mv[4] = {1, 2, 5, 10};
        g_mult = mv[id - MULT_1];
        g_sh->mult = g_mult;
        SaveSettings();
        Toast(Fmt(L"Очки за подбор умножаются на %d", g_mult).c_str());
        break;
    }
    case ACT_HEAL: case ACT_LIFE: case ACT_S1: case ACT_S2: {
        if (!g_fresh) { Toast(L"Сначала начни игру — загрузи уровень."); break; }
        uintptr_t g = g_sh->game;
        int v = 0;
        if (id == ACT_HEAL) { v = 100; Wr(g + OFF_ENERGY, &v, 4); Toast(L"Энергия восстановлена"); }
        else if (id == ACT_LIFE) { v = 3; Wr(g + OFF_LIVES, &v, 4); Toast(L"Жизни: 3"); }
        else {
            int add = id == ACT_S1 ? 1000 : 10000, sc = 0;
            if (Rd(g + OFF_SCORE, &sc, 4)) { sc += add; Wr(g + OFF_SCORE, &sc, 4); }
            Toast(id == ACT_S1 ? L"+1 000 очков" : L"+10 000 очков");
        }
        break;
    }
    }
}

static int HitTest(int x, int y) {
    for (size_t i = g_items.size(); i-- > 0;)
        if (x >= g_items[i].r.left && x <= g_items[i].r.right && y >= g_items[i].r.top && y <= g_items[i].r.bottom) return g_items[i].id;
    return 0;
}
static void DragSlider(int x) {
    for (size_t i = 0; i < g_sliders.size(); i++) {
        if (g_sliders[i].id != g_drag) continue;
        const Slider& s = g_sliders[i];
        float t = (float)(x - s.track.left) / (s.track.right - s.track.left);
        if (t < 0) t = 0;
        if (t > 1) t = 1;
        *s.val = s.mn + (int)(t * (s.mx - s.mn) + 0.5f);
        OnSlider(s.id);
    }
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE: g_wnd = h; SetTimer(h, 1, 30, NULL); SetTimer(h, 2, 40, NULL); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, W, H);
        HGDIOBJ old = SelectObject(mem, bmp);
        Paint(mem);
        float s = ScaleF();
        if (s == 1.0f) BitBlt(dc, 0, 0, W, H, mem, 0, 0, SRCCOPY);
        else { SetStretchBltMode(dc, HALFTONE); SetBrushOrgEx(dc, 0, 0, NULL); StretchBlt(dc, 0, 0, (int)(W * s), (int)(H * s), mem, 0, 0, W, H, SRCCOPY); }
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_NCHITTEST: {
        LRESULT r = DefWindowProcW(h, m, w, l);
        if (r == HTCLIENT) {
            POINT pt = {(short)LOWORD(l), (short)HIWORD(l)};
            ScreenToClient(h, &pt);
            float s = ScaleF();
            int x = (int)(pt.x / s), y = (int)(pt.y / s);
            if (y < 78 && HitTest(x, y) == 0) return HTCAPTION;
        }
        return r;
    }
    case WM_MOUSEMOVE: {
        float s = ScaleF();
        int x = (int)((short)LOWORD(l) / s), y = (int)((short)HIWORD(l) / s);
        if (g_drag) DragSlider(x);
        int id = HitTest(x, y);
        if (id != g_hot) { g_hot = id; InvalidateRect(h, NULL, FALSE); }
        if (!g_track) {
            TRACKMOUSEEVENT tm;
            tm.cbSize = sizeof tm; tm.dwFlags = TME_LEAVE; tm.hwndTrack = h; tm.dwHoverTime = 0;
            TrackMouseEvent(&tm);
            g_track = true;
        }
        return 0;
    }
    case WM_MOUSELEAVE: g_track = false; g_hot = 0; InvalidateRect(h, NULL, FALSE); return 0;
    case WM_LBUTTONDOWN: {
        float s = ScaleF();
        int x = (int)((short)LOWORD(l) / s), y = (int)((short)HIWORD(l) / s);
        int id = HitTest(x, y);
        if (id >= SL_SPEED && id <= SL_OPAC) { g_drag = id; SetCapture(h); DragSlider(x); }
        else if (id) Action(id);
        InvalidateRect(h, NULL, FALSE);
        return 0;
    }
    case WM_LBUTTONUP:
        if (g_drag) { g_drag = 0; ReleaseCapture(); SaveSettings(); }
        return 0;
    case WM_TIMER:
        if (w == 1) {
            const bool on[7] = {g_god, g_infLives, g_infEnergy, g_turbo, g_xray, g_fx, g_rainbow};
            for (int i = 0; i < 7; i++) {
                float tgt = on[i] ? 1.0f : 0.0f;
                g_anim[i] += (tgt - g_anim[i]) * 0.35f;
                if (g_anim[i] - tgt < 0.01f && tgt - g_anim[i] < 0.01f) g_anim[i] = tgt;
            }
            if (IsWindowVisible(h)) InvalidateRect(h, NULL, FALSE);
        } else {
            DWORD pid = 0;
            GetWindowThreadProcessId(GetForegroundWindow(), &pid);
            bool ours = pid == GetCurrentProcessId();
            const int keys[6] = {VK_INSERT, VK_F1, VK_F2, VK_F3, VK_F4, VK_F5};
            for (int i = 0; i < 6; i++) {
                bool d = (GetAsyncKeyState(keys[i]) & 0x8000) != 0;
                if (ours && d && !g_keyDown[i]) {
                    if (i == 0) { if (IsWindowVisible(h)) ShowWindow(h, SW_HIDE); else ShowWindow(h, SW_SHOWNOACTIVATE); }
                    else if (i <= 3) Action(TOG_GOD + i - 1);
                    else if (i == 4) { g_fx = !g_fx; SaveSettings(); Toast(g_fx ? L"Эффекты включены" : L"Эффекты выключены"); }
                    else { g_weather = (g_weather + 1) % 7; g_fx = true; SaveSettings(); Toast(L"Погода сменена"); }
                }
                g_keyDown[i] = d;
            }
            UpdateSpeed(ours && (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0);
        }
        return 0;
    case WM_CLOSE: ShowWindow(h, SW_HIDE); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static DWORD WINAPI MenuThread(LPVOID) {
    LoadSettings();
    g_supported = InstallHooks();
    g_worker = CreateThread(NULL, 0, Worker, NULL, 0, NULL);

    fTitle = CreateFontW(-26, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    fH = CreateFontW(-16, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    fSmall = CreateFontW(-13, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    fTab = CreateFontW(-14, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    HANDLE heroTh = CreateThread(NULL, 0, HeroThread, NULL, 0, NULL);

    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = g_self;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = L"HlebushekModMenu";
    RegisterClassExW(&wc);

    HWND hw = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED, wc.lpszClassName, L"Hlebushek Mod Menu", WS_POPUP,
                              60, 60, W, H, NULL, NULL, g_self, NULL);
    if (hw) {
        g_wnd = hw;
        ApplyWindowLook(true);
        ShowWindow(hw, SW_SHOWNOACTIVATE);
        UpdateWindow(hw);
        MSG msg;
        while (GetMessageW(&msg, NULL, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    g_quit = true;
    SetExtraSpeed(1.0);
    WaitForSingleObject(g_worker, 2000);
    CloseHandle(g_worker);
    if (heroTh) { WaitForSingleObject(heroTh, 3000); CloseHandle(heroTh); }   // поток героя сам вернёт оригинал текстуры
    UnhookIat(RVA_IAT_SETDIB, (void*)g_origSetDib);
    UnhookIat(RVA_IAT_STRETCH, (void*)g_origStretch);
    if (g_hooked) {
        if (g_sh) { g_sh->godFlag = 0; g_sh->mult = 1; }
        ApplyPatches(false);
    }
    for (int i = 0; i < 100 && g_inHook; i++) Sleep(10);   // дождаться, пока потоки выйдут из наших хуков
    Sleep(200);
    UnregisterClassW(wc.lpszClassName, g_self);
    DeleteObject(fTitle); DeleteObject(fH); DeleteObject(fSmall); DeleteObject(fTab);
    if (g_eject) FreeLibraryAndExitThread(g_self, 0);
    return 0;
}

BOOL APIENTRY DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = inst;
        DisableThreadLibraryCalls(inst);
        HANDLE t = CreateThread(NULL, 0, MenuThread, NULL, 0, NULL);
        if (t) CloseHandle(t);
    }
    return TRUE;
}
