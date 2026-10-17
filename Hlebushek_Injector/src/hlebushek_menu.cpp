// Hlebushek Mod Menu — DLL, которая загружается в процесс игры (Dizzy.exe v0.5, 32 бит).
// Всё делается автоматически: при загрузке DLL сама находит нужные места в коде игры (по результатам
// разбора exe), ставит хуки и показывает меню с переключателями. Ничего искать вручную не нужно.
//
// Что известно об игре (RVA относительно базы образа, ImageBase 0x400000):
//   CGame (this = ecx в методе отрисовки RVA 0xD6F0): +0x58 жизни (макс. 3), +0x5C энергия (0..100), +0x6C очки
//   CActionEnergyUpdate::Execute  — проверка урона по RVA 0x5D7D (патч: игнорировать урон, кроме падений/воды = -100)
//   CActionAddScore::Execute      — начисление очков по RVA 0x1804 (патч: множитель)
// Горячие клавиши в игре: Insert — меню, F1 — бессмертие, F2 — бесконечные жизни, F3 — вечная энергия.
#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>

#ifdef _WIN64
#error "Игра 32-битная: собирай DLL как 32-bit (x86)"
#endif

// ------------------------------------------------------------------ константы игры (v0.5)
static const DWORD EXPECT_SIZE = 0x78000, EXPECT_STAMP = 0x6AD1EC6C;
static const uintptr_t RVA_DRAW = 0xD6F0, RVA_DRAW_RET = 0xD6F6;
static const uintptr_t RVA_GOD = 0x5D7D, RVA_GOD_CONT = 0x5D87, RVA_GOD_SKIP = 0x5E29;
static const uintptr_t RVA_SCORE = 0x1804, RVA_SCORE_NEG = 0x1809, RVA_SCORE_DONE = 0x1810;
static const int OFF_LIVES = 0x58, OFF_ENERGY = 0x5C, OFF_SCORE = 0x6C;

struct Shared { volatile DWORD game, godFlag, mult, frames; };
struct Patch  { uintptr_t site; size_t len; BYTE orig[16], neu[16]; };

static HINSTANCE g_self;
static HWND g_wnd;
static uintptr_t g_base;
static BYTE* g_code = NULL;
static Shared* g_sh = NULL;
static std::vector<Patch> g_patches;
static bool g_supported = false, g_hooked = false;
static volatile bool g_quit = false, g_eject = false;
static HANDLE g_worker;

static bool g_god = false, g_infLives = false, g_infEnergy = false;
static int g_mult = 1;
static volatile bool g_fresh = false;
static volatile int g_lives = 0, g_energy = 0, g_score = 0;
static std::wstring g_toast;
static DWORD g_toastUntil = 0;

// ------------------------------------------------------------------ память
static bool Rd(uintptr_t a, void* buf, size_t n) {
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), (LPCVOID)a, buf, n, &got) && got == n;
}
static bool Wr(uintptr_t a, const void* buf, size_t n) {
    SIZE_T got = 0;
    return WriteProcessMemory(GetCurrentProcess(), (LPVOID)a, buf, n, &got) && got == n;
}

// ------------------------------------------------------------------ установка хуков
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

// Применить/откатить все патчи при остановленных остальных потоках процесса.
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

        bool busy = false;  // чей-то EIP внутри изменяемых байт?
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

static bool InstallHooks() {
    HMODULE exe = GetModuleHandleW(NULL);
    g_base = (uintptr_t)exe;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)exe;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)((BYTE*)exe + dos->e_lfanew);
    if (nt->OptionalHeader.SizeOfImage != EXPECT_SIZE || nt->FileHeader.TimeDateStamp != EXPECT_STAMP) return false;

    static const BYTE sDraw[] = {0x53, 0x8B, 0xDC, 0x83, 0xEC, 0x08, 0x83, 0xE4, 0xF8};
    static const BYTE sGod[] = {0x83, 0x7E, 0x60, 0x00, 0x0F, 0x8F, 0xA2, 0x00, 0x00, 0x00, 0x8B, 0x4E, 0x5C};
    static const BYTE sScore[] = {0x01, 0x4E, 0x6C, 0x79, 0x07, 0xC7, 0x46, 0x6C};
    if (!Verify(RVA_DRAW, sDraw, sizeof sDraw) || !Verify(RVA_GOD, sGod, sizeof sGod) ||
        !Verify(RVA_SCORE, sScore, sizeof sScore))
        return false;

    g_code = (BYTE*)VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!g_code) return false;
    memset(g_code, 0xCC, 4096);
    g_sh = (Shared*)g_code;
    g_sh->game = 0; g_sh->godFlag = 0; g_sh->mult = 1; g_sh->frames = 0;

    // --- заглушка 1: запоминаем this (CGame) на каждом кадре отрисовки
    BYTE* draw = g_code + 0x20;
    BYTE* p = draw;
    *p++ = 0x89; *p++ = 0x0D; EmitU32(p, (uint32_t)(uintptr_t)&g_sh->game);        // mov [game], ecx
    *p++ = 0xFF; *p++ = 0x05; EmitU32(p, (uint32_t)(uintptr_t)&g_sh->frames);      // inc dword [frames]
    *p++ = 0x53; *p++ = 0x8B; *p++ = 0xDC; *p++ = 0x83; *p++ = 0xEC; *p++ = 0x08;  // исходные инструкции
    EmitJmp(p, g_base + RVA_DRAW_RET);

    // --- заглушка 2: бессмертие. Урон -1..-99 игнорируется, -100 (вода/пропасти) работает как раньше
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

    // --- заглушка 3: множитель очков
    BYTE* sc = g_code + 0xA0;
    p = sc;
    *p++ = 0x0F; *p++ = 0xAF; *p++ = 0x0D; EmitU32(p, (uint32_t)(uintptr_t)&g_sh->mult);      // imul ecx,[mult]
    *p++ = 0x01; *p++ = 0x4E; *p++ = 0x6C;                                                    // add [esi+0x6c],ecx
    *p++ = 0x79; *p++ = 0x05;                                                                 // jns L_done
    EmitJmp(p, g_base + RVA_SCORE_NEG);
    EmitJmp(p, g_base + RVA_SCORE_DONE);                                                      // L_done

    g_patches.push_back(MakeJmpPatch(RVA_DRAW, 6, (uintptr_t)draw));
    g_patches.push_back(MakeJmpPatch(RVA_GOD, 10, (uintptr_t)god));
    g_patches.push_back(MakeJmpPatch(RVA_SCORE, 5, (uintptr_t)sc));
    FlushInstructionCache(GetCurrentProcess(), g_code, 4096);
    g_hooked = ApplyPatches(true);
    return g_hooked;
}

// ------------------------------------------------------------------ рабочий поток: статус и удержание значений
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

// ------------------------------------------------------------------ интерфейс
enum { TOG_GOD = 1, TOG_LIVES, TOG_ENERGY, MULT_1, MULT_2, MULT_5, MULT_10,
       ACT_HEAL, ACT_LIFE, ACT_S1, ACT_S2, BTN_CLOSE, BTN_EJECT };

static const int W = 400, H = 590;
static const COLORREF C_BG = RGB(20, 22, 28), C_CARD = RGB(30, 33, 43), C_HOT = RGB(40, 44, 58), C_ACC = RGB(255, 159, 67),
                      C_TEXT = RGB(236, 239, 246), C_SUB = RGB(139, 146, 166), C_OFF = RGB(66, 71, 88),
                      C_OK = RGB(84, 204, 124), C_BAD = RGB(236, 92, 92), C_WARN = RGB(240, 190, 80);
struct Item { int id; RECT r; };
static std::vector<Item> g_items;
static int g_hot = 0;
static bool g_track = false;
static float g_anim[3] = {0, 0, 0};
static HFONT fTitle, fH, fBody, fSmall, fBig;
static bool g_keyDown[4] = {false, false, false, false};

static void Toast(const wchar_t* s) { g_toast = s; g_toastUntil = GetTickCount() + 2800; }

static void FillRound(HDC dc, RECT r, int rad, COLORREF fill) {
    HBRUSH b = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_NULL, 0, 0);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, pen);
    RoundRect(dc, r.left, r.top, r.right + 1, r.bottom + 1, rad, rad);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(b);
    DeleteObject(pen);
}
static void Txt(HDC dc, const std::wstring& s, RECT r, HFONT f, COLORREF c, UINT fmt) {
    SelectObject(dc, f);
    SetTextColor(dc, c);
    DrawTextW(dc, s.c_str(), -1, &r, fmt | DT_NOPREFIX | DT_SINGLELINE);
}
static void Dot(HDC dc, int cx, int cy, int rad, COLORREF c) {
    HBRUSH b = CreateSolidBrush(c);
    HPEN pen = CreatePen(PS_NULL, 0, 0);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, pen);
    Ellipse(dc, cx - rad, cy - rad, cx + rad + 1, cy + rad + 1);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(b);
    DeleteObject(pen);
}
static COLORREF Mix(COLORREF a, COLORREF b, float t) {
    return RGB((int)(GetRValue(a) + (GetRValue(b) - GetRValue(a)) * t), (int)(GetGValue(a) + (GetGValue(b) - GetGValue(a)) * t),
               (int)(GetBValue(a) + (GetBValue(b) - GetBValue(a)) * t));
}
static void AddItem(int id, RECT r) { Item it; it.id = id; it.r = r; g_items.push_back(it); }

static void ToggleRow(HDC dc, int id, int y, const wchar_t* title, const wchar_t* desc, float anim) {
    RECT r = {16, y, W - 16, y + 56};
    FillRound(dc, r, 14, g_hot == id ? C_HOT : C_CARD);
    RECT t1 = {32, y + 8, W - 100, y + 28}, t2 = {32, y + 29, W - 100, y + 48};
    Txt(dc, title, t1, fH, C_TEXT, DT_LEFT);
    Txt(dc, desc, t2, fSmall, C_SUB, DT_LEFT | DT_END_ELLIPSIS);
    RECT sw = {W - 74, y + 16, W - 28, y + 40};
    FillRound(dc, sw, 24, Mix(C_OFF, C_ACC, anim));
    int kx = (int)(sw.left + 12 + anim * 22);
    Dot(dc, kx, y + 28, 9, RGB(255, 255, 255));
    AddItem(id, r);
}
static void Button(HDC dc, int id, RECT r, const wchar_t* text, bool accent, bool selected) {
    bool hot = g_hot == id;
    COLORREF bg = selected ? C_ACC : (accent ? (hot ? RGB(255, 178, 100) : C_ACC) : (hot ? C_HOT : C_CARD));
    FillRound(dc, r, 12, bg);
    Txt(dc, text, r, fH, (selected || accent) ? RGB(30, 22, 12) : C_TEXT, DT_CENTER | DT_VCENTER);
    AddItem(id, r);
}

static void Paint(HDC dc) {
    g_items.clear();
    RECT all = {0, 0, W, H};
    HBRUSH bg = CreateSolidBrush(C_BG);
    FillRect(dc, &all, bg);
    DeleteObject(bg);
    SetBkMode(dc, TRANSPARENT);

    // шапка с градиентом
    for (int x = 0; x < W; x++) {
        HBRUSH b = CreateSolidBrush(Mix(RGB(255, 170, 80), RGB(235, 105, 60), (float)x / W));
        RECT c = {x, 0, x + 1, 78};
        FillRect(dc, &c, b);
        DeleteObject(b);
    }
    RECT t = {22, 12, W - 60, 46}, st = {22, 46, W - 60, 68};
    Txt(dc, L"ХЛЕБУШЕК", t, fTitle, RGB(40, 24, 10), DT_LEFT | DT_VCENTER);
    Txt(dc, L"Mod Menu  ·  Insert — показать / скрыть", st, fSmall, RGB(88, 50, 20), DT_LEFT | DT_VCENTER);
    RECT cl = {W - 46, 18, W - 18, 46};
    FillRound(dc, cl, 28, g_hot == BTN_CLOSE ? RGB(255, 255, 255) : RGB(255, 205, 150));
    Txt(dc, L"×", cl, fH, RGB(60, 30, 10), DT_CENTER | DT_VCENTER);
    AddItem(BTN_CLOSE, cl);

    // статус
    RECT sc = {16, 90, W - 16, 148};
    FillRound(dc, sc, 14, C_CARD);
    const wchar_t* msg;
    COLORREF col;
    if (!g_supported) { msg = L"Версия игры не поддерживается"; col = C_BAD; }
    else if (!g_hooked) { msg = L"Не удалось подключиться к игре"; col = C_BAD; }
    else if (g_fresh) { msg = L"Игра подключена — читы активны"; col = C_OK; }
    else { msg = L"Жду начала игры (загрузи уровень)"; col = C_WARN; }
    Dot(dc, 34, 108, 5, col);
    RECT s1 = {46, 98, W - 28, 118};
    Txt(dc, msg, s1, fH, C_TEXT, DT_LEFT | DT_VCENTER);
    wchar_t line[128];
    if (GetTickCount() < g_toastUntil) {
        _snwprintf(line, 128, L"%s", g_toast.c_str());
    } else if (g_fresh) {
        _snwprintf(line, 128, L"Жизни: %d     Энергия: %d     Очки: %d", g_lives, g_energy, g_score);
    } else {
        _snwprintf(line, 128, L"Бессмертие и множитель очков работают сразу");
    }
    RECT s2 = {46, 120, W - 28, 140};
    Txt(dc, line, s2, fSmall, GetTickCount() < g_toastUntil ? C_ACC : C_SUB, DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);

    // защита
    RECT l1 = {20, 158, W - 20, 176};
    Txt(dc, L"ЗАЩИТА", l1, fSmall, C_SUB, DT_LEFT);
    ToggleRow(dc, TOG_GOD, 180, L"Бессмертие", L"Враги и ловушки не наносят урон (F1)", g_anim[0]);
    ToggleRow(dc, TOG_LIVES, 244, L"Бесконечные жизни", L"Жизни не заканчиваются (F2)", g_anim[1]);
    ToggleRow(dc, TOG_ENERGY, 308, L"Вечная энергия", L"Энергия всегда полная (F3)", g_anim[2]);

    // очки
    RECT l2 = {20, 378, W - 20, 396};
    Txt(dc, L"МНОЖИТЕЛЬ ОЧКОВ", l2, fSmall, C_SUB, DT_LEFT);
    const wchar_t* mt[4] = {L"×1", L"×2", L"×5", L"×10"};
    const int mv[4] = {1, 2, 5, 10};
    int bw = (W - 32 - 3 * 8) / 4;
    for (int i = 0; i < 4; i++) {
        RECT r = {16 + i * (bw + 8), 400, 16 + i * (bw + 8) + bw, 438};
        Button(dc, MULT_1 + i, r, mt[i], false, g_mult == mv[i]);
    }

    // быстрые действия
    RECT l3 = {20, 452, W - 20, 470};
    Txt(dc, L"БЫСТРЫЕ ДЕЙСТВИЯ", l3, fSmall, C_SUB, DT_LEFT);
    int hw = (W - 32 - 8) / 2;
    RECT a1 = {16, 474, 16 + hw, 512}, a2 = {24 + hw, 474, W - 16, 512};
    RECT a3 = {16, 520, 16 + hw, 558}, a4 = {24 + hw, 520, W - 16, 558};
    Button(dc, ACT_HEAL, a1, L"Полная энергия", true, false);
    Button(dc, ACT_LIFE, a2, L"Жизни = 3", true, false);
    Button(dc, ACT_S1, a3, L"+1 000 очков", false, false);
    Button(dc, ACT_S2, a4, L"+10 000 очков", false, false);

    RECT ej = {W - 150, 566, W - 16, 584};
    Txt(dc, L"Выгрузить меню", ej, fSmall, g_hot == BTN_EJECT ? C_ACC : C_SUB, DT_RIGHT);
    AddItem(BTN_EJECT, ej);
}

static void Action(int id) {
    if (!g_supported || !g_hooked) { Toast(L"Эта версия игры не поддерживается."); return; }
    switch (id) {
    case TOG_GOD: g_god = !g_god; g_sh->godFlag = g_god ? 1 : 0; Toast(g_god ? L"Бессмертие включено" : L"Бессмертие выключено"); break;
    case TOG_LIVES: g_infLives = !g_infLives; Toast(g_infLives ? L"Бесконечные жизни включены" : L"Бесконечные жизни выключены"); break;
    case TOG_ENERGY: g_infEnergy = !g_infEnergy; Toast(g_infEnergy ? L"Вечная энергия включена" : L"Вечная энергия выключена"); break;
    case MULT_1: case MULT_2: case MULT_5: case MULT_10: {
        static const int mv[4] = {1, 2, 5, 10};
        g_mult = mv[id - MULT_1];
        g_sh->mult = g_mult;
        wchar_t b[64];
        _snwprintf(b, 64, L"Очки за подбор умножаются на %d", g_mult);
        Toast(b);
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
    case BTN_CLOSE: ShowWindow(g_wnd, SW_HIDE); break;
    case BTN_EJECT: g_eject = true; DestroyWindow(g_wnd); break;
    }
}

static int HitTest(int x, int y) {
    for (size_t i = g_items.size(); i-- > 0;)
        if (x >= g_items[i].r.left && x <= g_items[i].r.right && y >= g_items[i].r.top && y <= g_items[i].r.bottom) return g_items[i].id;
    return 0;
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE: {
        g_wnd = h;
        HRGN rg = CreateRoundRectRgn(0, 0, W + 1, H + 1, 26, 26);
        SetWindowRgn(h, rg, TRUE);
        SetTimer(h, 1, 30, NULL);
        SetTimer(h, 2, 40, NULL);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, W, H);
        HGDIOBJ old = SelectObject(mem, bmp);
        Paint(mem);
        BitBlt(dc, 0, 0, W, H, mem, 0, 0, SRCCOPY);
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
            if (pt.y < 78 && HitTest(pt.x, pt.y) == 0) return HTCAPTION;
        }
        return r;
    }
    case WM_MOUSEMOVE: {
        int id = HitTest((short)LOWORD(l), (short)HIWORD(l));
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
        int id = HitTest((short)LOWORD(l), (short)HIWORD(l));
        if (id) Action(id);
        InvalidateRect(h, NULL, FALSE);
        return 0;
    }
    case WM_TIMER:
        if (w == 1) {
            const bool on[3] = {g_god, g_infLives, g_infEnergy};
            for (int i = 0; i < 3; i++) {
                float tgt = on[i] ? 1.0f : 0.0f;
                g_anim[i] += (tgt - g_anim[i]) * 0.35f;
                if (g_anim[i] - tgt < 0.01f && tgt - g_anim[i] < 0.01f) g_anim[i] = tgt;
            }
            if (IsWindowVisible(h)) InvalidateRect(h, NULL, FALSE);
        } else {
            DWORD pid = 0;
            GetWindowThreadProcessId(GetForegroundWindow(), &pid);
            bool ours = pid == GetCurrentProcessId();
            const int keys[4] = {VK_INSERT, VK_F1, VK_F2, VK_F3};
            for (int i = 0; i < 4; i++) {
                bool d = (GetAsyncKeyState(keys[i]) & 0x8000) != 0;
                if (ours && d && !g_keyDown[i]) {
                    if (i == 0) {
                        if (IsWindowVisible(h)) ShowWindow(h, SW_HIDE);
                        else { ShowWindow(h, SW_SHOWNOACTIVATE); }
                    } else Action(TOG_GOD + i - 1);
                }
                g_keyDown[i] = d;
            }
        }
        return 0;
    case WM_CLOSE: ShowWindow(h, SW_HIDE); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static DWORD WINAPI MenuThread(LPVOID) {
    g_supported = InstallHooks();
    g_worker = CreateThread(NULL, 0, Worker, NULL, 0, NULL);

    fTitle = CreateFontW(-26, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    fH = CreateFontW(-16, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    fBody = CreateFontW(-14, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    fSmall = CreateFontW(-13, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    fBig = fBody;

    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = g_self;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = L"HlebushekModMenu";
    RegisterClassExW(&wc);

    int x = 60, y = 60;
    HWND game = NULL;
    {
        struct F { static BOOL CALLBACK cb(HWND w, LPARAM lp) {
            DWORD pid = 0;
            GetWindowThreadProcessId(w, &pid);
            if (pid == GetCurrentProcessId() && IsWindowVisible(w) && !GetWindow(w, GW_OWNER)) { *(HWND*)lp = w; return FALSE; }
            return TRUE; } };
        EnumWindows(F::cb, (LPARAM)&game);
    }
    if (game) { RECT r; GetWindowRect(game, &r); x = r.left + 24; y = r.top + 48; }
    HWND hw = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, wc.lpszClassName, L"Hlebushek Mod Menu", WS_POPUP, x, y, W, H, NULL, NULL, g_self, NULL);
    if (hw) {
        ShowWindow(hw, SW_SHOWNOACTIVATE);
        UpdateWindow(hw);
        MSG msg;
        while (GetMessageW(&msg, NULL, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    g_quit = true;
    WaitForSingleObject(g_worker, 2000);
    CloseHandle(g_worker);
    if (g_hooked) {
        if (g_sh) { g_sh->godFlag = 0; g_sh->mult = 1; }
        ApplyPatches(false);   // вернуть оригинальный код игры
        Sleep(150);            // дать потокам выйти из заглушек; сам блок кода не освобождаем
    }
    UnregisterClassW(wc.lpszClassName, g_self);
    DeleteObject(fTitle); DeleteObject(fH); DeleteObject(fBody); DeleteObject(fSmall);
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
