// probe_findwindow2.cpp — native check: can FindWindowW find the suite's own window classes?
// build: cl /nologo /std:c++17 /EHsc /utf-8 /DUNICODE /D_UNICODE probe_findwindow2.cpp /link user32.lib
// NOTE: format strings kept ASCII-only (console codepage drops lines containing CJK).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <stdio.h>

static void PrintClass(const wchar_t* cls) {
    HWND byFind = FindWindowW(cls, nullptr);
    HWND byFindEx = FindWindowExW(nullptr, nullptr, cls, nullptr);

    struct Ctx { const wchar_t* cls; HWND found; } ctx{cls, nullptr};
    EnumWindows([](HWND h, LPARAM lp) -> BOOL {
        auto* c = reinterpret_cast<Ctx*>(lp);
        wchar_t buf[256] = {};
        GetClassNameW(h, buf, 255);
        if (wcscmp(buf, c->cls) == 0) { c->found = h; return FALSE; }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));

    wprintf(L"[%ls]\n", cls);
    wprintf(L"   FindWindowW       = %p\n", (void*)byFind);
    wprintf(L"   FindWindowExW     = %p\n", (void*)byFindEx);
    wprintf(L"   EnumWindows       = %p\n", (void*)ctx.found);
    wprintf(L"   find==enum        = %ls\n", (byFind && byFind == ctx.found) ? L"YES" : L"NO");
}

int wmain() {
    wprintf(L"self pid=%lu  GetDesktopWindow=%p\n", GetCurrentProcessId(), (void*)GetDesktopWindow());
    const wchar_t* classes[] = {
        L"DesktopTopBarWindow", L"DesktopDockWindow", L"MyWigetsTrayWindow",
        L"Shell_TrayWnd", L"CASCADIA_HOSTING_WINDOW_CLASS", L"Progman",
        L"Chrome_WidgetWin_1",
    };
    for (const wchar_t* c : classes) PrintClass(c);

    HWND bar = FindWindowW(L"DesktopTopBarWindow", nullptr);
    if (!bar) {
        struct C { HWND h; } c{nullptr};
        EnumWindows([](HWND h, LPARAM lp) -> BOOL {
            wchar_t buf[256] = {};
            GetClassNameW(h, buf, 255);
            if (wcscmp(buf, L"DesktopTopBarWindow") == 0) { reinterpret_cast<C*>(lp)->h = h; return FALSE; }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&c));
        bar = c.h;
        wprintf(L"\ntopbar: FindWindow FAILED, EnumWindows = %p\n", (void*)bar);
    } else {
        wprintf(L"\ntopbar: FindWindow = %p\n", (void*)bar);
    }
    if (bar) {
        DWORD pid = 0, tid = GetWindowThreadProcessId(bar, &pid);
        HDESK inputDesk = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
        HDESK targetDesk = GetThreadDesktop(tid);
        HDESK myDesk = GetThreadDesktop(GetCurrentThreadId());
        auto nameOf = [](HDESK h) -> const wchar_t* {
            static wchar_t buf[128];
            DWORD need = 0;
            if (!h || !GetUserObjectInformationW(h, UOI_NAME, buf, sizeof(buf), &need)) return L"<err>";
            return buf;
        };
        wprintf(L"   thread pid=%lu tid=%lu\n", pid, tid);
        wprintf(L"   desktop input=%ls myThread=%ls targetThread=%ls\n",
                nameOf(inputDesk), nameOf(myDesk), nameOf(targetDesk));
        wprintf(L"   PostMessage(WM_NULL) = %ls\n", PostMessageW(bar, WM_NULL, 0, 0) ? L"OK" : L"FAIL");
        if (inputDesk) CloseDesktop(inputDesk);
    }
    return 0;
}
