// DesktopCalendar - 类 macOS 桌面日历组件（独立程序）
//
// 与 DesktopClock 分开：
//   - 独立窗口类 / 独立单实例互斥体 / 独立注册表位置
//   - 同样只停留在桌面层，不覆盖普通窗口，可拖动，右键退出

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#include <windows.h>
#include <windowsx.h>
#include <objidl.h>  // GDI+ 需要 IStream 等 COM 类型，先于 gdiplus.h 包含
#include <gdiplus.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "widgets.h"

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "advapi32.lib")

using namespace Gdiplus;

namespace {

constexpr wchar_t kWindowClass[] = L"DesktopCalendarWindow";
constexpr wchar_t kRegPath[] = L"Software\\DesktopCalendar";

constexpr int kBaseWidth = 260;
constexpr int kBaseHeight = 300;
constexpr int kDefaultMargin = 32;
constexpr UINT kDrawTimerId = 1;
constexpr UINT kDrawIntervalMs = 1000;      // 每 1 秒检查一次是否跨天
constexpr int kMenuExit = 1001;

struct AppState {
    HWND hwnd = nullptr;
    int width = 0;
    int height = 0;
    int dpi = 96;
    float scale = 1.0f;

    HDC hdcMem = nullptr;
    HBITMAP hbmDib = nullptr;
    HBITMAP hbmOld = nullptr;
    void* pvBits = nullptr;
    Gdiplus::Bitmap* bitmap = nullptr;

    bool dragging = false;
    POINT dragCursor = {};
    POINT dragOrigin = {};

    // 只在跨天时重绘
    WORD lastYear = 0;
    WORD lastMonth = 0;
    WORD lastDay = 0;
};

using SetProcessDpiAwarenessContextFn = BOOL(WINAPI*)(HANDLE);
using GetDpiForWindowFn = UINT(WINAPI*)(HWND);

void EnableDpiAwareness() {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        auto pSetContext = reinterpret_cast<SetProcessDpiAwarenessContextFn>(
            GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
        if (pSetContext) {
            // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 == -4
            if (pSetContext(reinterpret_cast<HANDLE>(static_cast<INT_PTR>(-4)))) {
                return;
            }
        }
    }
    SetProcessDPIAware();
}

int GetWindowDpi(HWND hwnd) {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        auto pGetDpi = reinterpret_cast<GetDpiForWindowFn>(
            GetProcAddress(user32, "GetDpiForWindow"));
        if (pGetDpi) {
            return static_cast<int>(pGetDpi(hwnd));
        }
    }
    HDC hdc = GetDC(hwnd);
    int dpi = GetDeviceCaps(hdc, LOGPIXELSX);
    ReleaseDC(hwnd, hdc);
    return dpi;
}

void ClampToWorkArea(int width, int height, POINT& pt) {
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    HMONITOR monitor = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    if (!GetMonitorInfoW(monitor, &mi)) {
        return;
    }

    const RECT& work = mi.rcWork;
    const LONG availableW = work.right - work.left;
    const LONG availableH = work.bottom - work.top;

    if (width >= availableW) {
        pt.x = work.left;
    } else {
        pt.x = std::clamp<LONG>(pt.x, work.left, work.right - width);
    }

    if (height >= availableH) {
        pt.y = work.top;
    } else {
        pt.y = std::clamp<LONG>(pt.y, work.top, work.bottom - height);
    }
}

bool LoadSavedPosition(POINT& pt) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegPath, 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return false;
    }

    bool ok = false;
    DWORD x = 0;
    DWORD y = 0;
    DWORD size = sizeof(x);
    DWORD type = REG_NONE;

    if (RegQueryValueExW(key, L"X", nullptr, &type,
                         reinterpret_cast<LPBYTE>(&x), &size) == ERROR_SUCCESS &&
        type == REG_DWORD) {
        size = sizeof(y);
        type = REG_NONE;
        if (RegQueryValueExW(key, L"Y", nullptr, &type,
                             reinterpret_cast<LPBYTE>(&y), &size) == ERROR_SUCCESS &&
            type == REG_DWORD) {
            pt.x = static_cast<LONG>(x);
            pt.y = static_cast<LONG>(y);
            ok = true;
        }
    }

    RegCloseKey(key);
    return ok;
}

void SavePosition(HWND hwnd) {
    RECT rc{};
    if (!GetWindowRect(hwnd, &rc)) {
        return;
    }

    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegPath, 0, nullptr, 0, KEY_WRITE,
                        nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return;
    }

    DWORD x = static_cast<DWORD>(rc.left);
    DWORD y = static_cast<DWORD>(rc.top);
    RegSetValueExW(key, L"X", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&x), sizeof(x));
    RegSetValueExW(key, L"Y", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&y), sizeof(y));
    RegCloseKey(key);
}

void DestroyBacking(AppState& s) {
    delete s.bitmap;
    s.bitmap = nullptr;

    if (s.hdcMem) {
        if (s.hbmDib) {
            if (s.hbmOld) {
                SelectObject(s.hdcMem, s.hbmOld);
            }
            DeleteObject(s.hbmDib);
            s.hbmDib = nullptr;
        }
        DeleteDC(s.hdcMem);
        s.hdcMem = nullptr;
    }
    s.hbmOld = nullptr;
    s.pvBits = nullptr;
}

bool CreateBacking(AppState& s, int width, int height) {
    DestroyBacking(s);

    HDC screenDc = GetDC(s.hwnd);
    if (!screenDc) {
        return false;
    }

    s.hdcMem = CreateCompatibleDC(screenDc);
    if (!s.hdcMem) {
        ReleaseDC(s.hwnd, screenDc);
        return false;
    }

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height; // 自上而下
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    s.hbmDib = CreateDIBSection(screenDc, &bmi, DIB_RGB_COLORS, &s.pvBits,
                                nullptr, 0);
    ReleaseDC(s.hwnd, screenDc);

    if (!s.hbmDib) {
        DeleteDC(s.hdcMem);
        s.hdcMem = nullptr;
        return false;
    }

    s.hbmOld = static_cast<HBITMAP>(SelectObject(s.hdcMem, s.hbmDib));
    // 注意：GdiplusBase 提供了自己的 operator new，不能使用 nothrow 形式
    s.bitmap = new Gdiplus::Bitmap(width, height, PixelFormat32bppPARGB);
    if (!s.bitmap) {
        DestroyBacking(s);
        return false;
    }

    s.width = width;
    s.height = height;
    s.scale = static_cast<float>(s.dpi) / 96.0f;
    return true;
}

void AddDays(SYSTEMTIME& st, int days) {
    FILETIME ft{};
    if (!SystemTimeToFileTime(&st, &ft)) {
        return;
    }
    ULARGE_INTEGER ul{};
    ul.LowPart = ft.dwLowDateTime;
    ul.HighPart = ft.dwHighDateTime;
    ul.QuadPart += static_cast<ULONGLONG>(days) * 864000000000ULL;
    ft.dwLowDateTime = ul.LowPart;
    ft.dwHighDateTime = ul.HighPart;
    FileTimeToSystemTime(&ft, &st);
}

void GetWeekdayNames(const SYSTEMTIME& today, wchar_t names[7][16]) {
    SYSTEMTIME base = today;
    AddDays(base, -static_cast<int>(base.wDayOfWeek)); // 本周日

    for (int i = 0; i < 7; ++i) {
        SYSTEMTIME d = base;
        AddDays(d, i);
        if (!GetDateFormatW(GetUserDefaultLCID(), 0, &d, L"ddd",
                            names[i], 16)) {
            static const wchar_t* fallback[] = {
                L"日", L"一", L"二", L"三", L"四", L"五", L"六"};
            wcsncpy_s(names[i], 16, fallback[i], _TRUNCATE);
        }
    }
}

void DrawCalendarFace(AppState& s) {
    Gdiplus::Bitmap* bmp = s.bitmap;
    if (!bmp) {
        return;
    }

    Graphics g(bmp);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    g.Clear(Color(0, 0, 0, 0)); // 全透明背景

    const float k = s.scale;
    const float w = static_cast<float>(s.width);
    const float h = static_cast<float>(s.height);

    // 圆角卡片区域
    const float margin = 8.0f * k;
    const float radius = 20.0f * k;
    const float left = margin;
    const float top = margin;
    const float right = w - margin;
    const float bottom = h - margin;

    GraphicsPath cardPath;
    const float d = radius * 2.0f;
    cardPath.AddArc(left, top, d, d, 180.0f, 90.0f);
    cardPath.AddArc(right - d, top, d, d, 270.0f, 90.0f);
    cardPath.AddArc(right - d, bottom - d, d, d, 0.0f, 90.0f);
    cardPath.AddArc(left, bottom - d, d, d, 90.0f, 90.0f);
    cardPath.CloseFigure();

    RectF card(left, top, right - left, bottom - top);
    LinearGradientBrush bgBrush(
        card,
        Color(112, 70, 74, 84),
        Color(92, 16, 18, 24),
        LinearGradientModeForwardDiagonal);
    g.FillPath(&bgBrush, &cardPath);

    Pen border(Color(110, 255, 255, 255), 1.2f * k);
    g.DrawPath(&border, &cardPath);

    SYSTEMTIME now{};
    GetLocalTime(&now);

    // 顶部月份标题
    wchar_t title[64] = {};
    if (!GetDateFormatW(GetUserDefaultLCID(), 0, &now,
                        L"yyyy'年'M'月'", title, 64)) {
        swprintf_s(title, L"%u年%u月", static_cast<unsigned>(now.wYear),
                   static_cast<unsigned>(now.wMonth));
    }

    FontFamily family(L"Microsoft YaHei UI");
    StringFormat centerFormat;
    centerFormat.SetAlignment(StringAlignmentCenter);
    centerFormat.SetLineAlignment(StringAlignmentCenter);

    SolidBrush titleBrush(Color(255, 250, 250, 252));
    Gdiplus::Font titleFont(&family, 17.0f * k, FontStyleBold, UnitPixel);
    PointF titlePos((left + right) * 0.5f, top + 25.0f * k);
    g.DrawString(title, -1, &titleFont, titlePos, &centerFormat, &titleBrush);

    // 星期标题
    wchar_t weekNames[7][16] = {};
    GetWeekdayNames(now, weekNames);

    const float gridLeft = left + 10.0f * k;
    const float gridTop = top + 62.0f * k;
    const float gridRight = right - 10.0f * k;
    const float gridBottom = bottom - 10.0f * k;
    const float cellW = (gridRight - gridLeft) / 7.0f;
    const float cellH = (gridBottom - gridTop) / 6.0f;

    Gdiplus::Font weekdayFont(&family, 11.5f * k, FontStyleRegular, UnitPixel);
    for (int i = 0; i < 7; ++i) {
        const float cx = gridLeft + (static_cast<float>(i) + 0.5f) * cellW;
        const float cy = gridTop - 10.0f * k;
        const Color textColor = (i == 0 || i == 6)
                                    ? Color(255, 255, 110, 100)
                                    : Color(172, 255, 255, 255);
        SolidBrush weekdayBrush(textColor);
        g.DrawString(weekNames[i], -1, &weekdayFont, PointF(cx, cy),
                     &centerFormat, &weekdayBrush);
    }

    // 42 个日期格：从当月 1 号所在星期的周日开始
    SYSTEMTIME gridStart = now;
    gridStart.wDay = 1;
    gridStart.wHour = 0;
    gridStart.wMinute = 0;
    gridStart.wSecond = 0;
    gridStart.wMilliseconds = 0;
    AddDays(gridStart, -static_cast<int>(gridStart.wDayOfWeek));

    Gdiplus::Font dayFont(&family, 14.0f * k, FontStyleRegular, UnitPixel);
    Gdiplus::Font todayFont(&family, 14.0f * k, FontStyleBold, UnitPixel);

    for (int cell = 0; cell < 42; ++cell) {
        SYSTEMTIME day = gridStart;
        AddDays(day, cell);

        const int col = cell % 7;
        const int row = cell / 7;
        const float cx = gridLeft + (static_cast<float>(col) + 0.5f) * cellW;
        const float cy = gridTop + (static_cast<float>(row) + 0.5f) * cellH;

        wchar_t dayText[8] = {};
        swprintf_s(dayText, L"%u", static_cast<unsigned>(day.wDay));

        const bool isToday = (day.wYear == now.wYear &&
                              day.wMonth == now.wMonth &&
                              day.wDay == now.wDay);
        const bool otherMonth = (day.wMonth != now.wMonth);

        if (isToday) {
            // macOS 风格：今天用红色实心圆高亮
            const float circleR = (std::min)(cellW, cellH) * 0.40f;
            SolidBrush todayBrush(Color(255, 255, 69, 58));
            g.FillEllipse(&todayBrush, cx - circleR, cy - circleR,
                          circleR * 2.0f, circleR * 2.0f);

            SolidBrush todayTextBrush(Color(255, 255, 255, 255));
            g.DrawString(dayText, -1, &todayFont, PointF(cx, cy),
                         &centerFormat, &todayTextBrush);
        } else {
            SolidBrush dayBrush(otherMonth
                                    ? Color(92, 190, 190, 194)
                                    : Color(240, 246, 246, 248));
            g.DrawString(dayText, -1, &dayFont, PointF(cx, cy),
                         &centerFormat, &dayBrush);
        }
    }
}

void PresentCalendar(AppState& s) {
    if (!s.bitmap || !s.hdcMem || !s.pvBits) {
        return;
    }

    Gdiplus::Rect lockRect(0, 0, s.width, s.height);
    BitmapData bitmapData{};
    const Status lockStatus = s.bitmap->LockBits(
        &lockRect, ImageLockModeRead, PixelFormat32bppPARGB, &bitmapData);
    if (lockStatus != Ok) {
        return;
    }

    auto* dst = static_cast<BYTE*>(s.pvBits);
    const auto* src = static_cast<const BYTE*>(bitmapData.Scan0);
    const size_t rowBytes = static_cast<size_t>(s.width) * 4u;
    for (int y = 0; y < s.height; ++y) {
        std::memcpy(dst + static_cast<size_t>(y) * rowBytes,
                    src + static_cast<size_t>(y) * static_cast<size_t>(bitmapData.Stride),
                    rowBytes);
    }
    s.bitmap->UnlockBits(&bitmapData);

    RECT rc{};
    GetWindowRect(s.hwnd, &rc);
    POINT ptDst{rc.left, rc.top};
    POINT ptSrc{0, 0};
    SIZE size{s.width, s.height};

    BLENDFUNCTION blend{};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;

    HDC screenDc = GetDC(nullptr);
    if (screenDc) {
        UpdateLayeredWindow(s.hwnd, screenDc, &ptDst, &size, s.hdcMem,
                            &ptSrc, 0, &blend, ULW_ALPHA);
        ReleaseDC(nullptr, screenDc);
    }
}

void DrawCalendar(AppState& s) {
    DrawCalendarFace(s);
    PresentCalendar(s);
}

// 将屏幕坐标转换为父窗口客户区坐标后移动窗口。
// 组件挂到 Progman 后仍是屏幕坐标拖动，这里统一转换。
void SetWidgetScreenPos(HWND hwnd, int screenX, int screenY,
                       int width, int height, UINT flags) {
    POINT pt{screenX, screenY};
    HWND parent = GetParent(hwnd);
    if (parent) {
        ScreenToClient(parent, &pt);
    }
    SetWindowPos(hwnd, nullptr, pt.x, pt.y, width, height, flags);
}

// 把 Progman 设为组件的 owner（属主）。
// Win32 规则保证：owned window 始终位于 owner 上方。
// 因此 Win+D 抬升 Progman 时，组件会被系统自动保持在桌面层上方，
// 不需要任何定时轮询；组件仍是顶层窗口，layered 渲染/拖动不受影响。
bool AttachToDesktop(HWND hwnd, int screenX, int screenY,
                     int width, int height) {
    HWND hProgman = FindWindowW(L"Progman", nullptr);
    if (!hProgman) {
        return false;
    }

    SetWindowLongPtrW(hwnd, GWLP_HWNDPARENT,
                      reinterpret_cast<LONG_PTR>(hProgman));

    // 只需在启动时排一次：放到普通窗口之下
    SetWindowPos(hwnd, HWND_BOTTOM, screenX, screenY, width, height,
                 SWP_NOACTIVATE);
    return true;
}

bool HitTestCard(HWND hwnd, const AppState& s, LPARAM lParam) {
    POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
    ScreenToClient(hwnd, &pt);

    const float k = s.scale;
    const float left = 8.0f * k;
    const float top = 8.0f * k;
    const float right = static_cast<float>(s.width) - left;
    const float bottom = static_cast<float>(s.height) - top;
    const float r = 20.0f * k;

    auto insideRect = [](float x, float y, float l, float t, float rr, float bb) {
        return x >= l && x <= rr && y >= t && y <= bb;
    };

    // 近似圆角区域：四角圆形以外的透明区域不接收鼠标
    if (!insideRect(static_cast<float>(pt.x), static_cast<float>(pt.y),
                    left + r, top, right - r, bottom)) {
        return false;
    }
    if (!insideRect(static_cast<float>(pt.x), static_cast<float>(pt.y),
                    left, top + r, right, bottom - r)) {
        return false;
    }
    return true;
}

void ShowExitMenu(HWND hwnd) {
    POINT pt{};
    GetCursorPos(&pt);

    HMENU menu = CreatePopupMenu();
    if (!menu) {
        return;
    }

    AppendMenuW(menu, MF_STRING, kMenuExit, L"退出 DesktopCalendar");
    SetForegroundWindow(hwnd);

    const UINT flags = TPM_RIGHTBUTTON | TPM_LEFTALIGN | TPM_RETURNCMD | TPM_NONOTIFY;
    const int cmd = TrackPopupMenu(menu, flags, pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(menu);

    if (cmd == kMenuExit) {
        DestroyWindow(hwnd);
    }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    AppState* s = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        s = static_cast<AppState*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(s));
    } else {
        s = reinterpret_cast<AppState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    switch (msg) {
    case WM_NCCREATE:
        s->hwnd = hwnd;
        return DefWindowProcW(hwnd, msg, wParam, lParam);

    case WM_CREATE: {
        if (!s) {
            return -1;
        }
        s->hwnd = hwnd;
        s->dpi = GetWindowDpi(hwnd);
        const int w = MulDiv(kBaseWidth, s->dpi, 96);
        const int h = MulDiv(kBaseHeight, s->dpi, 96);

        // 设为 Progman 的属主窗口，Win+D 时自动保持在桌面层上方
        RECT initial{};
        GetWindowRect(hwnd, &initial);
        if (!AttachToDesktop(hwnd, initial.left, initial.top, w, h)) {
            // 极端情况下找不到桌面窗口：退化为普通底层窗口
            SetWindowPos(hwnd, HWND_BOTTOM, initial.left, initial.top, w, h,
                         SWP_NOACTIVATE);
        }

        if (!CreateBacking(*s, w, h)) {
            MessageBoxW(hwnd, L"创建绘图缓冲失败。", L"DesktopCalendar",
                        MB_OK | MB_ICONERROR);
            return -1;
        }

        SetTimer(hwnd, kDrawTimerId, kDrawIntervalMs, nullptr);
        DrawCalendar(*s);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps{};
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_WINDOWPOSCHANGING: {
        // 防御：任何 Z 序变化都强制回到桌面层底部，防止覆盖普通窗口
        auto* wp = reinterpret_cast<WINDOWPOS*>(lParam);
        if ((wp->flags & SWP_NOZORDER) == 0 && wp->hwndInsertAfter != HWND_BOTTOM) {
            wp->hwndInsertAfter = HWND_BOTTOM;
            wp->flags |= SWP_NOACTIVATE;
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    case WM_NCHITTEST:
        if (!s || s->width <= 0 || s->height <= 0) {
            return HTTRANSPARENT;
        }
        return HitTestCard(hwnd, *s, lParam) ? HTCLIENT : HTTRANSPARENT;

    case WM_LBUTTONDOWN: {
        if (!s || s->dragging) {
            return 0;
        }
        s->dragging = true;
        GetCursorPos(&s->dragCursor);
        RECT rc{};
        GetWindowRect(hwnd, &rc);
        s->dragOrigin.x = rc.left;
        s->dragOrigin.y = rc.top;
        SetCapture(hwnd);
        return 0;
    }

    case WM_MOUSEMOVE: {
        if (!s || !s->dragging) {
            return 0;
        }
        POINT pt{};
        GetCursorPos(&pt);
        const int x = s->dragOrigin.x + (pt.x - s->dragCursor.x);
        const int y = s->dragOrigin.y + (pt.y - s->dragCursor.y);
        SetWidgetScreenPos(hwnd, x, y, 0, 0,
                           SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }

    case WM_LBUTTONUP: {
        if (!s) {
            return 0;
        }
        if (s->dragging) {
            s->dragging = false;
            ReleaseCapture();
            SavePosition(hwnd);
        }
        return 0;
    }

    case WM_CAPTURECHANGED:
        if (s) {
            s->dragging = false;
        }
        return 0;

    case WM_RBUTTONUP:
        ShowExitMenu(hwnd);
        return 0;

    case WM_TIMER:
        if (!s) {
            return 0;
        }
        if (wParam == kDrawTimerId) {
            SYSTEMTIME now{};
            GetLocalTime(&now);
            if (now.wYear != s->lastYear || now.wMonth != s->lastMonth ||
                now.wDay != s->lastDay) {
                s->lastYear = now.wYear;
                s->lastMonth = now.wMonth;
                s->lastDay = now.wDay;
                DrawCalendar(*s);
            }
        }
        return 0;

    case WM_DPICHANGED: {
        if (!s) {
            return 0;
        }
        const UINT newDpi = HIWORD(wParam);
        const auto* suggested = reinterpret_cast<RECT*>(lParam);
        s->dpi = static_cast<int>(newDpi);
        const int w = MulDiv(kBaseWidth, newDpi, 96);
        const int h = MulDiv(kBaseHeight, newDpi, 96);

        s->dragging = false;
        if (suggested) {
            SetWidgetScreenPos(hwnd, suggested->left, suggested->top, w, h,
                               SWP_NOZORDER | SWP_NOACTIVATE);
        } else {
            RECT rc{};
            GetWindowRect(hwnd, &rc);
            SetWidgetScreenPos(hwnd, rc.left, rc.top, w, h,
                               SWP_NOZORDER | SWP_NOACTIVATE);
        }

        if (CreateBacking(*s, w, h)) {
            DrawCalendar(*s);
        }
        return 0;
    }

    case WM_DESTROY:
        KillTimer(hwnd, kDrawTimerId);
        SavePosition(hwnd);
        if (s) {
            DestroyBacking(*s);
        }
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace

// ============================== 组件接口 ==============================

std::atomic<HWND> g_calendarHwnd{nullptr};

DWORD WINAPI CalendarThreadProc(LPVOID param) {
    const HINSTANCE hInstance = static_cast<HINSTANCE>(param);

    EnableDpiAwareness();

    ULONG_PTR gdiplusToken = 0;
    GdiplusStartupInput gdiplusStartupInput;
    if (GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, nullptr) != Ok) {
        return 1;
    }

    HDC screenDc = GetDC(nullptr);
    const int systemDpi = screenDc ? GetDeviceCaps(screenDc, LOGPIXELSX) : 96;
    if (screenDc) {
        ReleaseDC(nullptr, screenDc);
    }
    const int width = MulDiv(kBaseWidth, systemDpi, 96);
    const int height = MulDiv(kBaseHeight, systemDpi, 96);

    RECT workArea{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
    // 默认放工作区左上角，避免和默认在右上角的时钟重叠
    POINT pos{workArea.left + kDefaultMargin,
              workArea.top + kDefaultMargin};
    LoadSavedPosition(pos);
    ClampToWorkArea(width, height, pos);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursorW(nullptr, IDC_HAND);
    wc.lpszClassName = kWindowClass;

    if (!RegisterClassExW(&wc)) {
        if (GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            // 组件在进程内被重启（关闭后再次打开）时类已注册，视为成功
            GdiplusShutdown(gdiplusToken);
            return 1;
        }
    }

    AppState state;
    HWND hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        kWindowClass,
        L"DesktopCalendar",
        WS_POPUP,
        pos.x, pos.y, width, height,
        nullptr, nullptr, hInstance, &state);

    if (!hwnd) {
        GdiplusShutdown(gdiplusToken);
        return 1;
    }

    g_calendarHwnd.store(hwnd);  // 先发布窗口句柄，宿主可立即隐藏/关闭

    ShowWindow(hwnd, SW_SHOWNOACTIVATE);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    GdiplusShutdown(gdiplusToken);
    g_calendarHwnd.store(nullptr);  // 窗口已销毁，线程即将退出
    return static_cast<DWORD>(msg.wParam);
}
