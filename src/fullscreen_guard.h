// fullscreen_guard.h —— 全屏应用（游戏全屏 / 全屏视频 / 演示模式）检测
//
// 背景：本套件的 Dock 栏常驻屏幕底部、隐藏系统任务栏，并把工作区扩为整屏。
// 由此产生两个必须处理的耦合：
//   1. 工作区=整屏后，「最大化窗口」的几何与「全屏窗口」完全重合 —— 实测
//      最大化 Chrome 的 DWM 扩展边框就是 0,0,1920,1080（整个显示器）。
//      所以几何判定必须再用 IsZoomed 把二者区分开，否则任何最大化窗口
//      都会被误判成全屏；
//   2. 全屏应用运行期间，Dock 升起、下缘触发条、屏幕角部隐形按钮
//      （开始/显示桌面/音量角）以及 Dock 全局热键都必须彻底让位，
//      否则鼠标一触屏幕下缘就会在全屏画面上弹出一条 Dock。
//
// 判定口径（任一命中即算全屏占用）：
//   A. 几何：前台窗口可见、非桌面/任务栏等 shell 窗口、非 UWP 幽灵窗
//      （cloaked）、非本套件自身窗口，且 DWM 扩展边框（或窗口矩形）铺满其
//      所在显示器的整幅画面；同时窗口矩形不得比显示器外扩（最大化窗口恒定
//      外扩一个不可见缩放边框，真全屏不外扩 —— 见 WindowCoversItsMonitor
//      里网页全屏视频的实测表，它同样带 IsZoomed 标记）；
//   B. shell：SHQueryUserNotificationState 报 QUNS_RUNNING_D3D_FULL_SCREEN(3)
//      独占 D3D 全屏 / QUNS_PRESENTATION_MODE(4) 演示模式。
//
// 为什么不采信 QUNS_BUSY(2)：实测（本机 1920×1080、工作区已扩为整屏）
//   - 铺满显示器的无边框窗口：quns=2（正确）；
//   - 最大化 Chrome：quns=2（误报！）而最大化 WinForms 窗口 quns=5。
//   shell 的「全屏应用」判定同样依赖窗口矩形是否铺满显示器，被本套件
//   「工作区=整屏」的环境带偏，与几何判定踩的是同一个坑；所以这里对
//   shell 信号只保留语义明确、与窗口几何无关的两档（独占 D3D 全屏 /
//   演示模式），全屏窗口一律以几何口径 A 为准。
//
// Detect() 同时给出判定所依据窗口所在的显示器：调用方据此只认「自己所在
// 显示器」上的全屏应用（副屏上的全屏视频不该收走主屏 Dock）。
//
// 开销：实测整次 Detect() 约 75µs（其中 SHQueryUserNotificationState 约 74µs，
// GetWindowRect+DwmGetWindowAttribute 约 2.4µs）。调用方一律按事件驱动调用
// （前台切换 / 2s 保底 / 光标触到下缘触发条时复核），不进任何逐帧热路径。

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601  // Vista+：SHQueryUserNotificationState
#endif

#include <windows.h>
#include <shellapi.h>  // SHQueryUserNotificationState
#include <dwmapi.h>    // DwmGetWindowAttribute（扩展边框 / cloaked）

#include <cstddef>

#if defined(_MSC_VER)
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")
#endif

// 极旧 SDK 配置（NTDDI 早于 Vista）下 shellapi.h 不导出该枚举与函数，
// 按 ABI 值手动声明，保证本文件在任何 _WIN32_WINNT 下都能编译。
#if !defined(NTDDI_VERSION) || (NTDDI_VERSION < NTDDI_VISTA)
typedef enum {
    QUNS_NOT_PRESENT = 1,
    QUNS_BUSY = 2,
    QUNS_RUNNING_D3D_FULL_SCREEN = 3,
    QUNS_PRESENTATION_MODE = 4,
    QUNS_ACCEPTS_NOTIFICATIONS = 5,
    QUNS_QUIET_TIME = 6,
    QUNS_APP = 7
} QUERY_USER_NOTIFICATION_STATE;
extern "C" HRESULT WINAPI
SHQueryUserNotificationState(QUERY_USER_NOTIFICATION_STATE* pquns);
#endif

namespace fsguard {

struct Result {
    bool fullscreen = false;     // 当前是否有全屏应用在前台
    bool byGeometry = false;     // 几何判定命中
    bool byShell = false;        // shell(QUNS) 判定命中
    HWND hwnd = nullptr;         // 判定所依据的前台窗口
    HMONITOR monitor = nullptr;  // 该窗口所在显示器（可能为 nullptr）
    const wchar_t* note = L"";   // 命中原因（日志用）
};

inline bool ClassInList(HWND hwnd, const wchar_t* const* list, size_t count) {
    wchar_t cls[64] = {};
    if (GetClassNameW(hwnd, cls, 63) <= 0) return false;
    for (size_t i = 0; i < count; ++i) {
        if (wcscmp(cls, list[i]) == 0) return true;
    }
    return false;
}

// 桌面 / 任务栏等 shell 本体窗口：即便铺满屏幕也不是「应用全屏」
inline bool IsShellWindow(HWND hwnd) {
    static const wchar_t* const kShell[] = {
        L"Progman", L"WorkerW", L"SHELLDLL_DefView",
        L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd",
        L"XamlExplorerHostIslandWindow",
    };
    return ClassInList(hwnd, kShell, sizeof(kShell) / sizeof(kShell[0]));
}

// 本套件自身窗口（Dock / 顶栏 / 时钟 / 日历 / 应用管理 / 托盘宿主）
inline bool IsSuiteWindow(HWND hwnd) {
    static const wchar_t* const kSuite[] = {
        L"DesktopDockWindow", L"DesktopTopBarWindow",
        L"DesktopAnalogClockWindow", L"DesktopCalendarWindow",
        L"DesktopLauncherWindow", L"MyWigetsTrayWindow",
    };
    return ClassInList(hwnd, kSuite, sizeof(kSuite) / sizeof(kSuite[0]));
}

// UWP 幽灵窗（cloaked）：切走的 UWP 应用窗口仍「可见」但不是真前台
inline bool IsCloakedWindow(HWND hwnd) {
    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked,
                                        sizeof(cloaked)))) {
        return cloaked != 0;
    }
    return false;
}

inline bool RectCoversMonitor(const RECT& r, const RECT& monitor, int tol) {
    return r.left <= monitor.left + tol && r.top <= monitor.top + tol &&
           r.right >= monitor.right - tol && r.bottom >= monitor.bottom - tol;
}

// 前台窗口是否铺满其所在显示器（几何口径 A）。
//
// 关于"最大化 ≠ 全屏"：本套件把任务栏藏起来并把工作区扩为整屏后，最大化窗口
// 的可见边框也会铺满整屏，所以必须另找一个判据把它排除掉。原先一律用
// IsZoomed(hwnd) == false 当门槛，实测（Win11 / Chrome）会漏掉一大类真全屏：
//
//   窗口形态              window rect              DWM 扩展边框        IsZoomed
//   ------------------------------------------------------------------------
//   普通最大化窗口        (-8,32,1928,1024) 外扩   (0,40,1920,1016)    True
//   网页全屏视频          (0,0,1920,1080) 不外扩   (0,0,1920,1080)     True
//   无边框全屏（kiosk等） (0,0,1920,1080) 不外扩   (0,0,1920,1080)     False
//
// 网页里播放全屏视频（HTML5 Fullscreen API）时，Chrome 的窗口矩形正好铺满
// 显示器且**不带 DWM 不可见缩放边框**（不是"被系统最大化的窗口"，虽然它同样
// 会被标成 Zoomed）—— 用 IsZoomed 一票否决就会把它误判成最大化窗口，
// 于是顶栏与 Dock 都不让位、压在视频上。
//
// 因此判据改成"以 DWM 扩展边框为准 + 用窗口矩形是否外扩区分最大化"：
//   1. 扩展边框（或退化为窗口矩形）通过 RectCoversMonitor 检查：必须铺满整屏，
//      这一步排除普通最大化窗口（其扩展边框停在任务栏上方）；
//   2. 若窗口矩形本身也已铺满整屏，而 DWM 给出过扩展边框，则要求"窗口矩形
//      不比显示器外扩" —— 最大化窗口恒定外扩一个不可见缩放边框（实测各方向
//      约 8px），真全屏窗口不外扩。两条同时成立才算全屏。
inline bool WindowCoversItsMonitor(HWND hwnd, int tolerancePx = 1) {
    if (!hwnd || !IsWindow(hwnd)) return false;
    if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) return false;
    if (IsCloakedWindow(hwnd)) return false;
    if (IsShellWindow(hwnd) || IsSuiteWindow(hwnd)) return false;

    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    const HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    if (!mon || !GetMonitorInfoW(mon, &mi)) return false;
    const RECT& screen = mi.rcMonitor;

    RECT wr{};
    const bool gotWindow = GetWindowRect(hwnd, &wr) != FALSE;
    RECT fr{};
    const bool gotFrame =
        SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &fr,
                                        sizeof(fr)));
    const bool windowCovers =
        gotWindow && RectCoversMonitor(wr, screen, tolerancePx);
    const bool frameCovers =
        gotFrame && RectCoversMonitor(fr, screen, tolerancePx);
    if (!windowCovers && !frameCovers) return false;

    // 最大化窗口的窗口矩形会外扩一个不可见缩放边框；真全屏不外扩
    if (gotFrame) {
        const bool overhang =
            wr.left < screen.left - tolerancePx ||
            wr.top < screen.top - tolerancePx ||
            wr.right > screen.right + tolerancePx ||
            wr.bottom > screen.bottom + tolerancePx;
        if (overhang) return false;
    }
    return true;
}

// shell 是否报「独占 D3D 全屏 / 演示模式」（口径 B；QUNS_BUSY 不可信，见上）
inline bool ShellReportsFullscreen() {
    QUERY_USER_NOTIFICATION_STATE quns = QUNS_ACCEPTS_NOTIFICATIONS;
    if (FAILED(SHQueryUserNotificationState(&quns))) return false;
    return quns == QUNS_RUNNING_D3D_FULL_SCREEN ||
           quns == QUNS_PRESENTATION_MODE;
}

// 检测当前是否有全屏应用占用屏幕（口径 A ∪ 口径 B）
inline Result Detect() {
    Result r;
    const HWND fg = GetForegroundWindow();
    if (!fg) return r;
    r.hwnd = fg;
    r.monitor = MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST);

    if (ShellReportsFullscreen()) {
        r.byShell = true;
        r.note = L"shell 报独占全屏/演示模式";
    }
    if (WindowCoversItsMonitor(fg)) {
        r.byGeometry = true;
        r.note = r.byShell ? L"几何+shell 双命中" : L"前台窗口铺满显示器";
    }
    r.fullscreen = r.byGeometry || r.byShell;
    return r;
}

// 主显示器句柄（宿主托盘热键用：副屏全屏不该禁掉主屏 Dock 热键）
inline HMONITOR PrimaryMonitor() {
    POINT origin{0, 0};
    return MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
}

}  // namespace fsguard
