// DesktopClock - 类 macOS 桌面指针时钟
//
// 特性：
//   1. 圆形指针式时钟，GDI+ 抗锯齿绘制，秒针每秒跳秒更新（1Hz，低功耗）
//   2. 分层窗口(WS_EX_LAYERED)实现逐像素透明
//   3. WS_EX_NOACTIVATE + HWND_BOTTOM：始终停留在桌面层，不覆盖任何普通窗口，
//      点击/拖动不会抢焦点，也不会出现在任务栏和 Alt-Tab 中
//   4. 按住左键可拖动，右键菜单可退出
//   5. 位置保存在 HKCU\Software\DesktopClock，支持高 DPI

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <windowsx.h>
#include <objidl.h>  // GDI+ 需要 IStream 等 COM 类型，先于 gdiplus.h 包含
#include <gdiplus.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <new>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "advapi32.lib")

using namespace Gdiplus;

namespace {

constexpr wchar_t kWindowClass[] = L"DesktopAnalogClockWindow";
constexpr wchar_t kMutexName[] = L"Local\\DesktopAnalogClock_SingleInstance";
constexpr wchar_t kRegPath[] = L"Software\\DesktopClock";

constexpr int kBaseSize = 220;          // 96 DPI 下的窗口边长
constexpr int kDefaultMargin = 32;      // 默认距离工作区右上角的边距
constexpr UINT kDrawTimerId = 1;
constexpr UINT kDrawIntervalMs = 1000;  // 1 Hz，秒针跳秒（低功耗：每秒仅重绘一次）

constexpr int kMenuExit = 1001;
constexpr float kPi = 3.14159265358979323846f;

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
};

inline float DegToRad(float deg) {
    return deg * kPi / 180.0f;
}

Gdiplus::PointF PolarPoint(float cx, float cy, float angleRad, float radius) {
    return Gdiplus::PointF(cx + std::cos(angleRad) * radius,
                           cy + std::sin(angleRad) * radius);
}

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
    // 旧系统回退
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
    bmi.bmiHeader.biHeight = -height; // 自上而下，便于 memcpy
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

void DrawShadowedLine(Graphics& g,
                      const PointF& a,
                      const PointF& b,
                      const Color& shadowColor,
                      const Color& lineColor,
                      float width,
                      float shadowOffsetX,
                      float shadowOffsetY) {
    Pen shadow(shadowColor, width + 1.0f);
    shadow.SetStartCap(LineCapRound);
    shadow.SetEndCap(LineCapRound);
    g.DrawLine(&shadow,
               PointF(a.X + shadowOffsetX, a.Y + shadowOffsetY),
               PointF(b.X + shadowOffsetX, b.Y + shadowOffsetY));

    Pen line(lineColor, width);
    line.SetStartCap(LineCapRound);
    line.SetEndCap(LineCapRound);
    g.DrawLine(&line, a, b);
}

void DrawClockFace(AppState& s) {
    Gdiplus::Bitmap* bmp = s.bitmap;
    if (!bmp) {
        return;
    }

    Graphics g(bmp);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(PixelOffsetModeHighQuality);
    g.Clear(Color(0, 0, 0, 0)); // 全透明背景

    const float k = s.scale;
    const float cx = static_cast<float>(s.width) * 0.5f;
    const float cy = static_cast<float>(s.height) * 0.5f;
    const float faceR = (std::min)(cx, cy) - 6.0f * k;

    // macOS 风格深色半透明渐变表盘
    RectF faceRect(cx - faceR, cy - faceR, faceR * 2.0f, faceR * 2.0f);
    LinearGradientBrush faceBrush(
        faceRect,
        Color(112, 82, 86, 96),   // 左上：较亮的半透明灰
        Color(88, 14, 16, 22),    // 右下：更暗
        LinearGradientModeForwardDiagonal);
    g.FillEllipse(&faceBrush, faceRect);

    Pen outerBorder(Color(118, 255, 255, 255), 1.4f * k);
    g.DrawEllipse(&outerBorder, faceRect);

    Pen innerBorder(Color(40, 255, 255, 255), 1.0f * k);
    RectF innerRect(cx - faceR + 3.5f * k,
                    cy - faceR + 3.5f * k,
                    (faceR - 3.5f * k) * 2.0f,
                    (faceR - 3.5f * k) * 2.0f);
    g.DrawEllipse(&innerBorder, innerRect);

    // 60 个刻度，每 5 个为长刻度
    const float tickOuter = faceR - 6.0f * k;
    for (int i = 0; i < 60; ++i) {
        const bool major = (i % 5) == 0;
        // 钟面 0 秒/0 分/0 点必须指向正上方，因此减去 90°
        const float angle = DegToRad(static_cast<float>(i) * 6.0f) - kPi * 0.5f;
        const float tickInner = tickOuter - (major ? 13.0f * k : 7.0f * k);
        const PointF p0 = PolarPoint(cx, cy, angle, tickInner);
        const PointF p1 = PolarPoint(cx, cy, angle, tickOuter);

        Pen tick(major ? Color(235, 255, 255, 255)
                       : Color(96, 255, 255, 255),
                 major ? 2.4f * k : 1.1f * k);
        if (major) {
            tick.SetStartCap(LineCapRound);
            tick.SetEndCap(LineCapRound);
        }
        g.DrawLine(&tick, p0, p1);
    }

    // ---- 表盘小时数字 1-12 ----
    FontFamily clockFontFamily(L"Segoe UI");
    StringFormat hourNumberFormat;
    hourNumberFormat.SetAlignment(StringAlignmentCenter);
    hourNumberFormat.SetLineAlignment(StringAlignmentCenter);
    Gdiplus::Font hourNumberFont(&clockFontFamily, 15.0f * k,
                                 FontStyleBold, UnitPixel);
    SolidBrush hourNumberBrush(Color(235, 255, 255, 255));
    const float numberRadius = faceR - 25.0f * k;
    for (int i = 1; i <= 12; ++i) {
        const float angle = DegToRad(static_cast<float>(i) * 30.0f) - kPi * 0.5f;
        const PointF numberPos = PolarPoint(cx, cy, angle, numberRadius);
        wchar_t numberText[4] = {};
        swprintf_s(numberText, L"%d", i);
        g.DrawString(numberText, -1, &hourNumberFont, numberPos,
                     &hourNumberFormat, &hourNumberBrush);
    }

    // ---- 指针角度：秒针跳秒（吸附整秒刻度），分/时针含秒分量平滑推进 ----
    SYSTEMTIME st{};
    GetLocalTime(&st);
    const float secondOfMinute = static_cast<float>(st.wSecond);
    const float minuteOfHour = static_cast<float>(st.wMinute) + secondOfMinute / 60.0f;
    const float hourOfDay = static_cast<float>(st.wHour % 12) + minuteOfHour / 60.0f;

    const float secondAngle = DegToRad(secondOfMinute * 6.0f) - kPi * 0.5f;
    const float minuteAngle = DegToRad(minuteOfHour * 6.0f) - kPi * 0.5f;
    const float hourAngle = DegToRad(hourOfDay * 30.0f) - kPi * 0.5f;

    const Color handShadow(88, 0, 0, 0);
    const Color hourColor(255, 250, 250, 252);
    const Color minuteColor(255, 246, 246, 248);
    const Color secondColor(255, 255, 69, 58); // 苹果风红色秒针

    // 时针
    const float hourLen = faceR * 0.44f;
    const float hourTail = faceR * 0.10f;
    DrawShadowedLine(g,
                     PolarPoint(cx, cy, hourAngle + kPi, hourTail),
                     PolarPoint(cx, cy, hourAngle, hourLen),
                     handShadow, hourColor, 5.2f * k, 1.2f * k, 1.4f * k);

    // 分针
    const float minuteLen = faceR * 0.62f;
    const float minuteTail = faceR * 0.12f;
    DrawShadowedLine(g,
                     PolarPoint(cx, cy, minuteAngle + kPi, minuteTail),
                     PolarPoint(cx, cy, minuteAngle, minuteLen),
                     handShadow, minuteColor, 3.8f * k, 1.2f * k, 1.4f * k);

    // 秒针
    const float secondLen = faceR * 0.70f;
    const float secondTail = faceR * 0.16f;
    DrawShadowedLine(g,
                     PolarPoint(cx, cy, secondAngle + kPi, secondTail),
                     PolarPoint(cx, cy, secondAngle, secondLen),
                     handShadow, secondColor, 1.8f * k, 1.0f * k, 1.2f * k);

    // 中心轴
    const float centerR = 5.6f * k;
    SolidBrush centerDark(Color(255, 18, 20, 24));
    g.FillEllipse(&centerDark, cx - centerR, cy - centerR, centerR * 2.0f, centerR * 2.0f);

    const float centerDotR = 2.7f * k;
    SolidBrush centerRed(Color(255, 255, 69, 58));
    g.FillEllipse(&centerRed, cx - centerDotR, cy - centerDotR,
                  centerDotR * 2.0f, centerDotR * 2.0f);
}

void PresentClock(AppState& s) {
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

void DrawClock(AppState& s) {
    DrawClockFace(s);
    PresentClock(s);
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

void ShowExitMenu(HWND hwnd) {
    POINT pt{};
    GetCursorPos(&pt);

    HMENU menu = CreatePopupMenu();
    if (!menu) {
        return;
    }

    AppendMenuW(menu, MF_STRING, kMenuExit, L"退出 DesktopClock");

    // 让菜单能在不激活主窗口的情况下正常使用
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
        const int w = MulDiv(kBaseSize, s->dpi, 96);
        const int h = MulDiv(kBaseSize, s->dpi, 96);

        // 设为 Progman 的属主窗口，Win+D 时自动保持在桌面层上方
        RECT initial{};
        GetWindowRect(hwnd, &initial);
        if (!AttachToDesktop(hwnd, initial.left, initial.top, w, h)) {
            // 极端情况下找不到桌面窗口：退化为普通底层窗口
            SetWindowPos(hwnd, HWND_BOTTOM, initial.left, initial.top, w, h,
                         SWP_NOACTIVATE);
        }

        if (!CreateBacking(*s, w, h)) {
            MessageBoxW(hwnd, L"创建绘图缓冲失败。", L"DesktopClock",
                        MB_OK | MB_ICONERROR);
            return -1;
        }

        SetTimer(hwnd, kDrawTimerId, kDrawIntervalMs, nullptr);
        DrawClock(*s);
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

    case WM_NCHITTEST: {
        // 只有圆形表盘区域接收鼠标，四角完全“穿透”到桌面
        if (!s || s->width <= 0 || s->height <= 0) {
            return HTTRANSPARENT;
        }
        POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ScreenToClient(hwnd, &pt);
        const float cx = static_cast<float>(s->width) * 0.5f;
        const float cy = static_cast<float>(s->height) * 0.5f;
        const float radius = (std::min)(cx, cy) - 4.0f * s->scale;
        const float dx = static_cast<float>(pt.x) - cx;
        const float dy = static_cast<float>(pt.y) - cy;
        return (dx * dx + dy * dy <= radius * radius) ? HTCLIENT : HTTRANSPARENT;
    }

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
            DrawClock(*s);
        }
        return 0;

    case WM_DPICHANGED: {
        if (!s) {
            return 0;
        }
        const UINT newDpi = HIWORD(wParam);
        const auto* suggested = reinterpret_cast<RECT*>(lParam);
        s->dpi = static_cast<int>(newDpi);
        const int w = MulDiv(kBaseSize, newDpi, 96);
        const int h = MulDiv(kBaseSize, newDpi, 96);

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
            DrawClock(*s);
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

int APIENTRY WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int) {
    // 只允许运行一个实例
    HANDLE mutex = CreateMutexW(nullptr, TRUE, kMutexName);
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(mutex);
        return 0;
    }

    EnableDpiAwareness();

    ULONG_PTR gdiplusToken = 0;
    GdiplusStartupInput gdiplusStartupInput;
    if (GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, nullptr) != Ok) {
        MessageBoxW(nullptr, L"GDI+ 初始化失败。", L"DesktopClock",
                    MB_OK | MB_ICONERROR);
        CloseHandle(mutex);
        return 1;
    }

    // 默认位置：主工作区右上角
    HDC screenDc = GetDC(nullptr);
    const int systemDpi = screenDc ? GetDeviceCaps(screenDc, LOGPIXELSX) : 96;
    if (screenDc) {
        ReleaseDC(nullptr, screenDc);
    }
    const int width = MulDiv(kBaseSize, systemDpi, 96);
    const int height = MulDiv(kBaseSize, systemDpi, 96);

    RECT workArea{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
    POINT pos{workArea.right - width - kDefaultMargin,
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
        MessageBoxW(nullptr, L"注册窗口类失败。", L"DesktopClock",
                    MB_OK | MB_ICONERROR);
        GdiplusShutdown(gdiplusToken);
        CloseHandle(mutex);
        return 1;
    }

    AppState state;
    HWND hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        kWindowClass,
        L"DesktopClock",
        WS_POPUP,
        pos.x, pos.y, width, height,
        nullptr, nullptr, hInstance, &state);

    if (!hwnd) {
        MessageBoxW(nullptr, L"创建时钟窗口失败。", L"DesktopClock",
                    MB_OK | MB_ICONERROR);
        GdiplusShutdown(gdiplusToken);
        CloseHandle(mutex);
        return 1;
    }

    ShowWindow(hwnd, SW_SHOWNOACTIVATE);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    GdiplusShutdown(gdiplusToken);
    CloseHandle(mutex);
    return static_cast<int>(msg.wParam);
}
