// DesktopTopBar - 类 macOS 桌面顶栏（独立程序）
//
// 功能：
//   1. 固定在桌面顶部的一条半透明顶栏，外观与其他组件一致（GDI+ 逐像素透明）
//   2. 通过 Progman 属主 + HWND_BOTTOM 挂在桌面层，与其它三个组件一样
//      只展现在桌面上：不覆盖任何普通窗口，也不出现在任务栏/Alt-Tab 中
//   3. 从音量键右侧到右侧三键之间显示 Chrome 风格标签：
//      - 普通应用：标签为聚焦窗口所属应用打开的全部窗口，名字为对应窗口名
//      - Chrome/Edge（安装了 chrome-tab-sync 扩展并连接后）：标签为浏览器内
//        当前聚焦窗口的真实标签页（标题同步、点击切换、中键关闭、
//        右键新建标签页）。未连接扩展时回退为窗口枚举。
//   4. 顶栏右侧提供 Chrome 浏览器风格的 最小化 / 最大化 / 关闭 三个按钮，
//      用于控制当前聚焦窗口；双击顶栏空白处最大化/还原当前聚焦窗口；
//      按住顶栏空白处拖动可移动当前聚焦窗口（等同标题栏拖动）
//   5. 最小化键左侧提供 Win11 风格任务栏时钟：时间/日期两行显示
//      （跟随系统区域格式与"显示秒"设置），悬停有药丸高亮，
//      点击打开/收起系统"日期和时间"日历浮出窗口
//   6. 高度等于 Chrome 浏览器标签栏的高度（约 40px，随 DPI 缩放）

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

// WinSock2 必须先于 windows.h 引入；本文件用 WIN32_LEAN_AND_MEAN 排除
// windows.h 内的旧 winsock.h，因此这里（windows.h 之后）引入是安全的
#include <winsock2.h>
#include <ws2tcpip.h>

#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <objidl.h>  // GDI+ 需要 IStream 等 COM 类型，先于 gdiplus.h 包含
#include <gdiplus.h>

// Core Audio：按进程设置应用音量（类似 Windows 音量合成器）
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <tlhelp32.h>   // 进程快照：解析应用子进程的音频会话
#include <unordered_map>
#include <vector>

// Chrome 标签同步：纯逻辑层（JSON / SHA-1 / Base64 / WS 帧 / 同步模型）
#include "topbar_ws_proto.h"
#include "widgets.h"

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "Mmdevapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ws2_32.lib")

using namespace Gdiplus;
using Microsoft::WRL::ComPtr;

// SDK 头文件未定义的窗口状态事件（值来自 WinUser.h 文档，稳定）
#ifndef EVENT_SYSTEM_RESTORE
#define EVENT_SYSTEM_RESTORE 0x000A
#endif
#ifndef EVENT_SYSTEM_MAXIMIZESTART
#define EVENT_SYSTEM_MAXIMIZESTART 0x0018
#endif
#ifndef EVENT_SYSTEM_MAXIMIZEEND
#define EVENT_SYSTEM_MAXIMIZEEND 0x0019
#endif

namespace {

constexpr wchar_t kWindowClass[] = L"DesktopTopBarWindow";

// Chrome 浏览器标签栏的高度（96 DPI 下约 40 像素），随 DPI 缩放
constexpr int kBaseTabHeight = 40;
// 每个窗口控制按钮的宽度（Chrome 风格，约等于标签栏高度）
constexpr int kButtonWidthBase = 46;
constexpr int kButtonGap = 4;

// Win11 风格任务栏时钟区域（时间/日期两行），位于最小化按钮左侧
constexpr int kClockWidthBase = 112;   // 时钟区域宽度（96 DPI 基准，随 DPI 缩放）
constexpr UINT_PTR kClockTimerId = 5;  // 时钟刷新定时器：每秒检查文本变化
constexpr UINT kClockTickMs = 1000;    // 与任务栏时钟一致；文本未变化不重绘

// Chrome 风格标签的视觉留白（相对 96 DPI，绘制时乘 scale）
constexpr float kTabTopInset = 5.0f;        // 活动标签顶部留白，底部贴住标签栏
constexpr float kTabDividerInset = 8.0f;    // 未激活标签之间竖线的上下留白

// 统一标签宽度（96 DPI 基准，随 DPI 缩放）：空间充裕时所有标签等宽，
// 不随标题长度变化；标签多到放不下时沿用自动缩小逻辑
constexpr float kUniformTabWidth = 240.0f;   // 1.5 × 160 基准宽度

// 左上角音量按钮与展开面板
constexpr int kVolumeButtonW = 46;      // 音量按钮宽度（与窗口控制按钮一致，便于点击）
constexpr int kVolumePanelW = 260;      // 展开的音量面板宽度
constexpr int kVolumePanelH = 60;       // 展开的音量面板高度
constexpr int kVolumePanelMargin = 2;   // 按键/面板贴合左上角的小边距
constexpr int kMuteButtonW = 30;        // 面板内静音按钮宽度
constexpr UINT kVolumeApplyDelayMs = 40;
// 挂起态短轮询：右键新建窗口待插入 / 抢前台重试期间每 200ms 刷新一次，
// 状态结束即停（不常驻）
constexpr UINT_PTR kTabRefreshTimerId = 1;
constexpr UINT kPendingPollMs = 200;
// 前台窗口创建瞬间尚未可见时的延迟复查（一次性定时器）
constexpr UINT_PTR kTargetRetryTimerId = 2;
// 前台窗口创建中不可见时最多复查次数（每次间隔 150ms，
// 资源管理器等窗口从激活到可见可能需要数百毫秒）
constexpr int kMaxTargetRetry = 5;
// 事件驱动刷新的抖动合并定时器（一次性）：高频窗口事件合并为一次刷新
constexpr UINT_PTR kTabRefreshDebounceTimerId = 3;
constexpr UINT kTabRefreshDebounceMs = 100;
// 低频兜底自检（一次性，每次刷新后重置）：事件驱动正常时基本不触发，
// 防止窗口事件丢失导致标签过期；无目标时停用（零轮询）。
// 10s 是功耗与兜底时延的折中（EnumWindows 全量枚举，频率越低越省电）
constexpr UINT_PTR kSlowRefreshTimerId = 4;
constexpr UINT kSlowRefreshMs = 10000;
// 右键新建窗口后抢前台的失败重试次数（随挂起态短轮询触发，约每 200ms 一次）
constexpr int kMaxPendingFocusAttempts = 5;

constexpr int kMenuExit = 1001;

// ---- Chrome 标签同步（WebSocket 服务端）----
// 本地回环端口：chrome-tab-sync 扩展连接此端口推送/接收标签数据
constexpr int kChromeSyncPort = 9786;
// 收到扩展消息后通知 UI 线程（lParam = new wchar_t[] JSON，UI 线程负责释放）
constexpr UINT kChromeSyncMsg = WM_APP + 10;
// 扩展连接/断开状态通知（wParam = 1 连接 / 0 断开）
constexpr UINT kChromeSyncStateMsg = WM_APP + 11;
// 客户端 socket 接收超时：空闲时发 ping 探活，连续超时则断开。
// 阈值必须大于扩展心跳间隔（60s）：4 × 25s = 100s 才判失联，
// 扩展每 60s ping 一次时不会误断，同时把失联检测从 50s 放宽到 100s
constexpr int kChromeSyncRecvTimeoutMs = 25000;
constexpr int kChromeSyncMaxIdleTimeouts = 4;
// 握手阶段读取超时（5s）与请求头上限（8KB）
constexpr int kChromeSyncHandshakeTimeoutMs = 5000;
constexpr size_t kChromeSyncMaxHeaderBytes = 8192;

enum ButtonHit {
    kHitNone = -1,
    kHitVolume = 3,      // 左上角音量按钮（展开/收起）
    kHitMute = 4,        // 面板内静音开关
    kHitSlider = 5,      // 面板内音量滑条
    kHitMinimize = 0,
    kHitMaximize = 1,
    kHitClose = 2,
    kHitTab = 6,        // Chrome 风格标签区域
    kHitClock = 7,      // 最小化键左侧的 Win11 风格时钟（时间/日期）
};

struct TabInfo {
    HWND hwnd = nullptr;
    DWORD pid = 0;
    std::wstring title;
    RectF rect;  // 顶栏客户区坐标

    // Chrome 同步模式（isChrome 为 true 时 hwnd/pid 无效，使用以下字段）
    bool isChrome = false;
    int chromeTabId = 0;
    bool chromeActive = false;
    bool chromePinned = false;
};

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

    // 当前要控制的"前台/最后一次聚焦"窗口
    HWND targetHwnd = nullptr;
    bool hasTarget = false;              // 是否有可操作的有效目标窗口
    bool targetMaximized = false;        // 目标当前是否处于最大化
    bool targetSticky = false;           // 目标最小化后保持跟踪，不跟随系统自动转移的焦点

    int hoverButton = kHitNone;          // 当前悬停的按钮
    int hoverTab = -1;                   // 当前悬停的 Chrome 标签索引
    bool trackingMouse = false;

    // Win11 风格任务栏时钟（显示在最小化键左侧：时间在上、日期在下两行）
    std::wstring clockTimeText;          // 当前时间文本（如 "21:45"）
    std::wstring clockDateText;          // 当前日期文本（如 "2026/8/28"）
    bool clockShowSeconds = false;       // 是否显示秒（跟随系统"显示秒"设置）

    // 按住空白处拖动（等同标题栏拖动，移动当前目标窗口）
    bool dragWindow = false;             // 正在拖动（按下空白处）
    bool dragMoving = false;             // 已超过阈值，真正移动窗口
    HWND dragHwnd = nullptr;             // 被拖动的窗口
    POINT dragStartPt{};                 // 按下点（屏幕坐标）
    POINT dragOffset{};                  // 光标相对窗口左上角的偏移

    // Chrome 风格标签：聚焦窗口所属应用打开的全部顶层窗口
    std::vector<TabInfo> tabs;

    // Chrome 标签同步状态（UI 线程维护，数据来自 chrome-tab-sync 扩展）
    wsproto::ChromeSyncModel chromeSync;

    // 右键“新建窗口”后的待插入状态：新窗口出现时插入到该标签右侧
    HWND insertAfterTab = nullptr;
    bool insertPending = false;
    ULONGLONG insertPendingSince = 0;  // 挂起起始时间：超时未出现则放弃

    // 右键新建窗口的聚焦重试：新窗口出现后反复尝试抢前台，
    // 直到窗口真正成为前台、用户已切到其他应用或达到重试上限
    HWND pendingFocusHwnd = nullptr;
    HWND pendingFocusOriginHwnd = nullptr;  // 挂起时的前台窗口：用户未切换则继续抢
    int pendingFocusAttempts = 0;
    int targetRetryCount = 0;   // 前台窗口创建中不可见的复查次数

    // ---- 音量调节（类似 Windows 音量合成器，控制前台窗口所在进程）----
    bool volumeOpen = false;             // 音量面板是否展开
    bool volumeReady = false;            // 是否有可控制的有效前台会话
    bool volumeDragging = false;         // 正在拖动滑条
    bool volumeHoverMute = false;        // 悬停在静音按钮上
    bool volumeHoverSlider = false;      // 悬停在滑条上
    float volumeValue = 0.0f;            // 0..1 目标进程当前音量
    bool volumeMuted = false;            // 目标进程是否静音
    DWORD volumePid = 0;                 // 当前目标进程 PID
    ComPtr<ISimpleAudioVolume> volumeSession;  // 解析到的目标音频会话
    DWORD volumeSessionPid = 0;          // volumeSession 对应的 PID
    HWND volumeTargetHwnd = nullptr;     // 已为其解析会话的目标窗口

    // 独立的音量面板窗口（顶栏保持 40px 高度，避免窗口矩形变化干扰其他组件/窗口）
    HWND volumePanelHwnd = nullptr;
    HDC panelHdcMem = nullptr;
    HBITMAP panelHbmDib = nullptr;
    HBITMAP panelHbmOld = nullptr;
    void* panelPvBits = nullptr;
    Gdiplus::Bitmap* panelBitmap = nullptr;
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

// 计算右侧三个按钮的水平布局（从右往左：关闭 | 最大化 | 最小化）
void ComputeButtonRects(AppState& s,
                        RectF& minRect, RectF& maxRect, RectF& closeRect) {
    float bw = kButtonWidthBase * s.scale;
    float gap = kButtonGap * s.scale;
    const float top = 0.0f;
    // 窗口控制按钮只占顶栏高度（下面还有音量面板时会变高）
    const float h = static_cast<float>(MulDiv(kBaseTabHeight, s.dpi, 96));

    closeRect.X = s.width - bw;
    closeRect.Y = top;
    closeRect.Width = bw;
    closeRect.Height = h;

    maxRect.X = closeRect.X - gap - bw;
    maxRect.Y = top;
    maxRect.Width = bw;
    maxRect.Height = h;

    minRect.X = maxRect.X - gap - bw;
    minRect.Y = top;
    minRect.Width = bw;
    minRect.Height = h;
}

// 计算 Win11 风格时钟区域：紧贴最小化按钮左侧
// （布局从右往左：关闭 | 最大化 | 最小化 | 时钟 | 标签）
void ComputeClockRect(AppState& s, RectF& clockR) {
    const float k = s.scale;
    RectF minR, maxR, closeR;
    ComputeButtonRects(s, minR, maxR, closeR);
    const float gap = kButtonGap * k;
    clockR.X = minR.X - gap - kClockWidthBase * k;
    clockR.Y = 0.0f;
    clockR.Width = kClockWidthBase * k;
    // BarAreaHeight 定义在下方（此处仅有前置声明），直接按同式展开
    clockR.Height = static_cast<float>(MulDiv(kBaseTabHeight, s.dpi, 96));
}

// 读取系统"在系统托盘时钟中显示秒"设置（Windows 设置 > 个性化 > 任务栏），
// 与任务栏时钟保持一致；读取失败时默认不显示秒（Win11 默认行为）。
bool ReadShowSecondsSetting() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(
            HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced",
            0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
        return false;
    }

    bool enabled = false;
    DWORD type = 0;
    DWORD size = 0;
    if (RegQueryValueExW(key, L"ShowSecondsInSystemClock", nullptr, &type,
                         nullptr, &size) == ERROR_SUCCESS) {
        if (type == REG_SZ && size >= 2 && size <= 8) {
            // 新旧 Windows 均以字符串 "1"/"0" 存储
            wchar_t buf[4] = {};
            DWORD bufSize = static_cast<DWORD>(sizeof(buf));
            if (RegQueryValueExW(key, L"ShowSecondsInSystemClock", nullptr,
                                 nullptr, reinterpret_cast<BYTE*>(buf),
                                 &bufSize) == ERROR_SUCCESS) {
                enabled = buf[0] == L'1';
            }
        } else if (type == REG_DWORD && size >= sizeof(DWORD)) {
            // 兼容个别版本以 DWORD 存储的情况
            DWORD val = 0;
            DWORD valSize = static_cast<DWORD>(sizeof(val));
            if (RegQueryValueExW(key, L"ShowSecondsInSystemClock", nullptr,
                                 nullptr, reinterpret_cast<BYTE*>(&val),
                                 &valSize) == ERROR_SUCCESS) {
                enabled = val != 0;
            }
        }
    }
    RegCloseKey(key);
    return enabled;
}

// 刷新时钟文本：时间/日期按系统区域格式生成；仅当文本变化时返回 true，
// 供每秒定时器决定是否需要重绘（分钟跳变 / 跨午夜 / 秒显示时无谓重绘）。
bool UpdateClock(AppState& s) {
    SYSTEMTIME st{};
    GetLocalTime(&st);

    wchar_t timeBuf[64] = {};
    wchar_t dateBuf[64] = {};
    if (s.clockShowSeconds) {
        // 跟随系统设置显示秒：按用户 12/24 小时制偏好补上 ":ss"
        DWORD itime = 0;
        GetLocaleInfoW(LOCALE_USER_DEFAULT, LOCALE_ITIME | LOCALE_RETURN_NUMBER,
                       reinterpret_cast<LPWSTR>(&itime),
                       static_cast<int>(sizeof(itime) / sizeof(wchar_t)));
        const wchar_t* fmt =
            (itime != 0) ? L"HH:mm:ss" : L"h:mm:ss tt";
        GetTimeFormatW(LOCALE_USER_DEFAULT, 0, &st, fmt, timeBuf, 64);
    } else {
        GetTimeFormatW(LOCALE_USER_DEFAULT, TIME_NOSECONDS, &st, nullptr,
                       timeBuf, 64);
    }
    GetDateFormatW(LOCALE_USER_DEFAULT, DATE_SHORTDATE, &st, nullptr,
                   dateBuf, 64);

    if (s.clockTimeText == timeBuf && s.clockDateText == dateBuf) {
        return false;
    }
    s.clockTimeText = timeBuf;
    s.clockDateText = dateBuf;
    return true;
}

// 模拟 Win+Alt+D（系统级注册快捷键）：与点击 Win11 任务栏时钟一致，
// 打开/收起"日期和时间"日历浮出窗口。
void ToggleCalendarFlyout() {
    INPUT inputs[6] = {};
    auto keyEvent = [&inputs](int idx, WORD vk, bool up) {
        inputs[idx].type = INPUT_KEYBOARD;
        inputs[idx].ki.wVk = vk;
        inputs[idx].ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
    };
    keyEvent(0, VK_LWIN, false);
    keyEvent(1, VK_MENU, false);
    keyEvent(2, 'D', false);
    keyEvent(3, 'D', true);
    keyEvent(4, VK_MENU, true);
    keyEvent(5, VK_LWIN, true);
    SendInput(6, inputs, sizeof(INPUT));
}

// 全局钩子：用户"显式选择窗口"的信号（鼠标点击 / Alt+Tab），
// 供目标跟踪区分系统自动转移焦点与用户主动切换窗口
HWND g_lastClickWindow = nullptr;
bool g_altTabPressed = false;
HHOOK g_keyHook = nullptr;

// 判断一个窗口是否应视为"可控制的有效目标窗口"
bool IsControlTarget(HWND hwnd, HWND self) {    if (!hwnd || hwnd == self) {
        return false;
    }
    if (!IsWindow(hwnd) || !IsWindowVisible(hwnd)) {
        return false;
    }
    // 忽略桌面 / 任务栏等系统窗口
    wchar_t cls[64] = {};
    const int len = GetClassNameW(hwnd, cls, 63);
    if (len <= 0) {
        return false;
    }
    if (wcscmp(cls, L"Progman") == 0 ||
        wcscmp(cls, L"WorkerW") == 0 ||
        wcscmp(cls, L"SHELLDLL_DefView") == 0 ||
        wcscmp(cls, L"Shell_TrayWnd") == 0 ||
        wcscmp(cls, L"Shell_SecondaryTrayWnd") == 0) {
        return false;
    }
    // 忽略我们自己进程创建的组件窗口
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId()) {
        return false;
    }
    return true;
}

void ResolveVolumeSession(AppState& s);  // 前置声明（定义在前台窗口轮询之后）
bool ProcessTreeContains(DWORD rootPid, DWORD sessionPid);  // 前置声明（定义在下方）
bool RefreshTabs(AppState& s);             // 前置声明（定义在下方）
int HitTestTab(AppState& s, int x, int y); // 前置声明（定义在下方）

// Chrome 标签同步相关前置声明（定义在下方）
bool IsChromeTarget(HWND hwnd);
bool ChromeSyncMode(AppState& s);
bool ChromeSyncSendJson(const std::string& utf8Json);
void ChromeSyncSendActivateTab(int tabId);
void ChromeSyncSendCloseTab(int tabId);
void ChromeSyncSendNewTab();
void ChromeSyncStart(HWND hwnd);
void ChromeSyncStop();

// 音量面板几何 / 操作的前置声明（定义在下方，供命中测试与消息处理使用）
int BarHeight(AppState& s);
int BarAreaHeight(AppState& s);
int WindowHeight(AppState& s);
void VolumeButtonRect(AppState& s, RectF& r);
void VolumePanelInner(AppState& s, RectF& panel, RectF& mute, RectF& track);
void ApplyVolume(AppState& s, float value);
void ToggleMute(AppState& s);
void DrawBarAndPresent(AppState& s);        // 前置声明（定义在下方）
void ReassertDesktopLayer(HWND hwnd);       // 前置声明（定义在下方）
void SetVolumeOpen(AppState& s, bool open);

// 独立音量面板窗口相关前置声明
void DrawButtonHover(Graphics& g, const RectF& r, ButtonHit hit, int hover);
void DrawVolumeGlyph(Graphics& g, const RectF& r, float scale, const Color& col,
                     bool muted);
void DrawVolumePercent(Graphics& g, AppState& s, const RectF& r, float value,
                       bool ready);
void AddRoundedRectPath(GraphicsPath& path, const RectF& r, float radius);
void DrawVolumePanelContent(Graphics& g, AppState& s);
void PresentVolumePanel(AppState& s);
void ShowVolumePanel(AppState& s);
void HideVolumePanel(AppState& s);

// 把某个窗口设为当前目标并刷新最大化状态、音量会话与 Chrome 标签
void ApplyTargetInfo(AppState& s, HWND hwnd) {
    s.targetHwnd = hwnd;
    s.hasTarget = true;
    s.targetMaximized = IsZoomed(hwnd) != FALSE;

    // 目标窗口变化时重新解析其进程的音频会话，供音量面板使用
    if (s.targetHwnd != s.volumeTargetHwnd) {
        s.volumeTargetHwnd = s.targetHwnd;
        ResolveVolumeSession(s);
    }

    // 刷新 Chrome 风格标签：显示该应用打开的全部窗口
    RefreshTabs(s);
}

// 轮询前台窗口，更新目标与 Chrome 标签。轮询而不是事件绑定，
// 是为了在 WS_EX_NOACTIVATE（点击不抢占焦点）的前提下，
// 稳定跟踪"鼠标最后一次聚焦"的窗口。
//
// 特殊处理：当当前目标被最小化时，Windows 会自动把前台让给下一个窗口
// （如 A 最小化后 B 成为前台）。此时顶栏不跟随系统转移——目标保持 A，
// 直到用户显式点击其他窗口或 Alt+Tab 切换，避免"最小化 A 却变成操作 B"。
void UpdateTarget(AppState& s) {
    HWND fg = GetForegroundWindow();

    // 目标粘住：当前目标因最小化而失去前台时，保持跟踪它
    if (s.hasTarget && s.targetHwnd && IsWindow(s.targetHwnd)) {
        if (IsIconic(s.targetHwnd)) {
            s.targetSticky = true;
        }
        if (s.targetSticky) {
            // 用户显式选择：鼠标点击其他窗口 / Alt+Tab
            HWND explicitHwnd = nullptr;
            if (g_lastClickWindow && g_lastClickWindow != s.targetHwnd &&
                IsControlTarget(g_lastClickWindow, s.hwnd)) {
                explicitHwnd = g_lastClickWindow;
            } else if (g_altTabPressed && IsControlTarget(fg, s.hwnd)) {
                explicitHwnd = fg;
            }
            g_lastClickWindow = nullptr;
            g_altTabPressed = false;

            if (explicitHwnd) {
                // 用户主动切到其他窗口：解除粘住并切换
                ApplyTargetInfo(s, explicitHwnd);
                s.targetSticky = false;
            } else if (fg == s.targetHwnd) {
                // 目标被恢复（win+↑ / 点击任务栏）
                s.targetSticky = false;
                ApplyTargetInfo(s, s.targetHwnd);
            } else {
                // 系统自动转移（最小化目标导致前台跳到别的窗口）：保持目标
                s.targetMaximized = IsZoomed(s.targetHwnd) != FALSE;
            }
            return;
        }
    }

    // 非粘住：正常跟随前台窗口；顺带清掉历史显式选择信号
    g_lastClickWindow = nullptr;
    g_altTabPressed = false;

    if (IsControlTarget(fg, s.hwnd)) {
        ApplyTargetInfo(s, fg);
        s.targetSticky = false;
    } else {
        // 前台是顶栏自身：忽略（顶栏 WS_EX_NOACTIVATE 本不应成为前台，
        // 若发生不应因此清掉目标）
        if (fg == s.hwnd) {
            return;
        }
        // 前台窗口激活瞬间可能尚未可见（新建窗口创建中）：
        // 保持当前目标，由 fg 事件处理里的延迟定时器在可见后复查，
        // 避免"右键新建资源管理器窗口"等场景误清目标导致标签全丢
        if (fg && IsWindow(fg) && !IsWindowVisible(fg)) {
            return;
        }
        s.targetHwnd = nullptr;
        s.hasTarget = false;
        s.targetSticky = false;
        s.targetMaximized = false;
        s.tabs.clear();
        s.hoverButton = kHitNone;
        s.hoverTab = -1;
        if (s.volumeTargetHwnd != nullptr) {
            s.volumeTargetHwnd = nullptr;
            ResolveVolumeSession(s);
        }
    }
}

// 按钮点击动作
void HitButton(HWND self, HWND target, ButtonHit hit) {
    if (target == self || !IsWindow(target)) {
        return;
    }
    switch (hit) {
    case kHitMinimize:
        ShowWindow(target, SW_MINIMIZE);
        break;
    case kHitMaximize:
        if (IsIconic(target)) {
            // 最小化时先还原显示（窗口化），不直接最大化；
            // 再次点击才执行最大化/还原切换
            ShowWindow(target, SW_RESTORE);
        } else if (IsZoomed(target)) {
            ShowWindow(target, SW_RESTORE);
        } else {
            ShowWindow(target, SW_MAXIMIZE);
        }
        break;
    case kHitClose:
        // 标准优雅关闭消息，与 Alt+F4 一致
        PostMessageW(target, WM_CLOSE, 0, 0);
        break;
    default:
        break;
    }
}

int HitTestTab(AppState& s, int x, int y) {
    if (y < 0 || y >= BarAreaHeight(s) || x < 0 || x >= s.width) {
        return -1;
    }
    for (size_t i = 0; i < s.tabs.size(); ++i) {
        const RectF& r = s.tabs[i].rect;
        if (x >= static_cast<int>(r.X) && x < static_cast<int>(r.X + r.Width)) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// 强制把窗口带到前台：附加到前台线程输入队列后依次执行
// BringWindowToTop / SetForegroundWindow / SetActiveWindow / SetFocus。
// SetActiveWindow / SetFocus 跨进程调用必须依赖输入队列附加，否则直接失败；
// 顶栏自身是 WS_EX_NOACTIVATE 背景窗口，单独调 SetForegroundWindow
// 很容易被系统前台锁拦截。全程不做按键模拟。
// 返回窗口是否已成为前台窗口。
bool ForceForegroundWindow(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) {
        return false;
    }
    if (IsIconic(hwnd)) {
        ShowWindow(hwnd, SW_RESTORE);
    }
    const DWORD curThread = GetCurrentThreadId();
    const HWND fg = GetForegroundWindow();
    const DWORD fgThread = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
    const bool attached = fgThread && fgThread != curThread &&
                          AttachThreadInput(curThread, fgThread, TRUE) != FALSE;
    BringWindowToTop(hwnd);
    SetForegroundWindow(hwnd);
    if (attached) {
        // 附加后目标窗口与当前线程共享输入队列，跨进程激活/聚焦才被允许
        SetActiveWindow(hwnd);
        if (IsWindow(hwnd)) {
            SetFocus(hwnd);
        }
        AttachThreadInput(curThread, fgThread, FALSE);
    }
    return GetForegroundWindow() == hwnd;
}

// 右键标签：启动该应用的新进程/新窗口，并记录待插入位置
std::wstring GetProcessPath(DWORD pid);  // 定义在下方
void OpenNewAppWindow(AppState& s, HWND tabHwnd) {
    DWORD pid = 0;
    GetWindowThreadProcessId(tabHwnd, &pid);
    if (pid == 0) {
        return;
    }
    const std::wstring path = GetProcessPath(pid);
    if (path.empty()) {
        return;
    }

    // 顶栏是 WS_EX_NOACTIVATE 背景窗口，平时没有设置前台的权限，
    // 直接 AllowSetForegroundWindow 不会生效（调用者必须自己能设置前台）。
    // 先附加到前台线程输入队列，使本线程被视为前台进程，
    // 再授权后，资源管理器等由 Shell 代建窗口的系统应用才能把新窗口带到前台。
    const DWORD curThread = GetCurrentThreadId();
    HWND fg = GetForegroundWindow();
    const DWORD fgThread = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
    const bool attached = fgThread && fgThread != curThread &&
                          AttachThreadInput(curThread, fgThread, TRUE) != FALSE;
    AllowSetForegroundWindow(ASFW_ANY);
    // 用 ShellExecute 启动，交给 Shell 处理前台/焦点
    HINSTANCE result = ShellExecuteW(nullptr, L"open", path.c_str(),
                                     nullptr, nullptr, SW_SHOWNORMAL);
    if (attached) {
        AttachThreadInput(curThread, fgThread, FALSE);
    }
    if (reinterpret_cast<INT_PTR>(result) > 32) {
        s.insertAfterTab = tabHwnd;
        s.insertPending = true;
        s.insertPendingSince = GetTickCount64();
        // 立即启动挂起态短轮询：新窗口可能出现得较慢，不依赖事件
        SetTimer(s.hwnd, kTabRefreshTimerId, kPendingPollMs, nullptr);
    }
}

ButtonHit HitTestButton(AppState& s, int x, int y) {
    if (y < 0 || y >= s.height || x < 0 || x >= s.width) {
        return kHitNone;
    }

    // 左上角音量按钮（始终可用，展开/收起面板）
    RectF volR;
    VolumeButtonRect(s, volR);
    if (x >= static_cast<int>(volR.X) && x < static_cast<int>(volR.X + volR.Width) &&
        y < BarAreaHeight(s)) {
        return kHitVolume;
    }

    // Chrome 风格标签区域
    if (HitTestTab(s, x, y) >= 0) {
        return kHitTab;
    }

    // 最小化键左侧的 Win11 风格时钟区域（点击 = 打开系统日历浮出窗口）
    RectF clockR;
    ComputeClockRect(s, clockR);
    if (x >= static_cast<int>(clockR.X) &&
        x < static_cast<int>(clockR.X + clockR.Width)) {
        return kHitClock;
    }

    RectF minR, maxR, closeR;
    ComputeButtonRects(s, minR, maxR, closeR);
    if (x >= static_cast<int>(minR.X) && x < static_cast<int>(minR.X + minR.Width)) {
        return kHitMinimize;
    }
    if (x >= static_cast<int>(maxR.X) && x < static_cast<int>(maxR.X + maxR.Width)) {
        return kHitMaximize;
    }
    if (x >= static_cast<int>(closeR.X) && x < static_cast<int>(closeR.X + closeR.Width)) {
        return kHitClose;
    }
    return kHitNone;
}

// ---- 音量面板几何 ----

// 顶栏基础高度
int BarHeight(AppState& s) {
    return MulDiv(kBaseTabHeight, s.dpi, 96);
}

// 音量面板上方的顶栏高度；音量按钮位于此区域左上角
int BarAreaHeight(AppState& s) {
    return BarHeight(s);
}

// 当前窗口总高：音量面板是独立窗口，顶栏高度始终等于 Chrome 标签栏高度
int WindowHeight(AppState& s) {
    return BarHeight(s);
}

void VolumeButtonRect(AppState& s, RectF& r) {
    const float k = s.scale;
    // 从窗口最左缘(x=0)开始，贴合左上角，避免屏幕最左边触发不到
    r.X = 0;
    r.Y = 0;
    r.Width = kVolumeButtonW * k;
    r.Height = static_cast<float>(BarAreaHeight(s));
}

// 面板内布局：静音按钮（左）+ 滑条（主体）
void VolumePanelInner(AppState& s, RectF& panel, RectF& mute, RectF& track) {
    const float k = s.scale;
    const float pad = 6.0f * k;
    mute.X = panel.X + pad;
    mute.Y = panel.Y + (panel.Height - kMuteButtonW * k) * 0.5f;
    mute.Width = kMuteButtonW * k;
    mute.Height = kMuteButtonW * k;

    const float trackX0 = mute.X + mute.Width + pad;
    // 右侧预留百分比数字显示空间（约 60px），避免轨道与百分比重合/贴边
    const float percentReserve = 60.0f * k;
    const float trackX1 = panel.X + panel.Width - pad - percentReserve;
    track.X = trackX0;
    track.Y = panel.Y + panel.Height * 0.5f - 2.0f * k;
    track.Width = trackX1 - trackX0;
    track.Height = 4.0f * k;
}

// ---- Core Audio：按进程设置应用音量（类似 Windows 音量合成器）----

// 判断 sessionPid 是否属于以 rootPid 为根的进程树（rootPid 本身或其后代）
bool ProcessTreeContains(DWORD rootPid, DWORD sessionPid) {
    if (rootPid == sessionPid) {
        return true;
    }

    std::unordered_map<DWORD, DWORD> parentMap;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        return false;
    }

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            parentMap[pe.th32ProcessID] = pe.th32ParentProcessID;
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);

    // 沿父链向上走，直到找到 rootPid
    DWORD cur = sessionPid;
    for (int i = 0; i < 128; ++i) {
        if (cur == rootPid) {
            return true;
        }
        auto it = parentMap.find(cur);
        if (it == parentMap.end()) {
            return false;
        }
        const DWORD parent = it->second;
        if (parent == cur || parent == 0) {
            return false;
        }
        cur = parent;
    }
    return false;
}

// ---- Chrome 标签同步：WebSocket 服务端 ----
//
// 顶栏内置一个 127.0.0.1:kChromeSyncPort 的 WebSocket 服务端。
// chrome-tab-sync 扩展（MV3）连接后推送当前聚焦 Chrome 窗口的标签列表与
// 增量事件；顶栏向扩展发送 激活/关闭/新建标签 命令。
// 网络工作在线程（g_chromeServer.thread），消息经 PostMessage 回到 UI 线程；
// 命令发送由 sendMutex 串行化。

std::wstring GetProcessName(DWORD pid);  // 定义在下方（同应用窗口枚举节）

struct ChromeSyncServer {
    HWND notifyHwnd = nullptr;
    SOCKET listenSock = INVALID_SOCKET;
    SOCKET clientSock = INVALID_SOCKET;  // 由 sendMutex 保护
    std::mutex sendMutex;
    std::thread thread;
    std::atomic<bool> stop{false};
    std::atomic<bool> connected{false};
    std::string recvBuf;        // 客户端数据缓冲（握手残留 + 帧流）
    std::string pendingText;    // 分片文本消息累积
    int idleTimeouts = 0;       // 连续接收超时计数（心跳探活）
};

ChromeSyncServer g_chromeServer;

// 判断目标窗口是否属于 Chrome/Edge（Chromium）进程
bool IsChromeTarget(HWND hwnd) {
    if (!hwnd) {
        return false;
    }
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0) {
        return false;
    }
    std::wstring name = GetProcessName(pid);
    for (auto& ch : name) {
        ch = static_cast<wchar_t>(towlower(ch));
    }
    return name == L"chrome.exe" || name == L"msedge.exe";
}

// 当前是否处于 Chrome 同步模式：扩展已连接 且 目标是 Chromium 窗口
bool ChromeSyncMode(AppState& s) {
    return s.chromeSync.connected && IsChromeTarget(s.targetHwnd);
}

// 向扩展发送一条 JSON 命令（UTF-8 文本帧）；未连接时返回 false
bool ChromeSyncSendJson(const std::string& utf8Json) {
    if (!g_chromeServer.connected.load()) {
        return false;
    }
    const std::string frame =
        wsproto::WsEncodeFrame(0x1, utf8Json, true);
    std::lock_guard<std::mutex> lock(g_chromeServer.sendMutex);
    if (g_chromeServer.clientSock == INVALID_SOCKET) {
        return false;
    }
    const int sent = send(g_chromeServer.clientSock, frame.data(),
                          static_cast<int>(frame.size()), 0);
    return sent == static_cast<int>(frame.size());
}

void ChromeSyncSendActivateTab(int tabId) {
    ChromeSyncSendJson(wsproto::ChromeSyncBuildCommand(L"activateTab", tabId));
}

void ChromeSyncSendCloseTab(int tabId) {
    ChromeSyncSendJson(wsproto::ChromeSyncBuildCommand(L"closeTab", tabId));
}

void ChromeSyncSendNewTab() {
    ChromeSyncSendJson(wsproto::ChromeSyncBuildCommand(L"newTab"));
}

// 关闭当前客户端连接（可跨线程调用；sendMutex 串行化与 send 的竞争）
void ChromeSyncCloseClient() {
    std::lock_guard<std::mutex> lock(g_chromeServer.sendMutex);
    if (g_chromeServer.clientSock != INVALID_SOCKET) {
        shutdown(g_chromeServer.clientSock, SD_BOTH);
        closesocket(g_chromeServer.clientSock);
        g_chromeServer.clientSock = INVALID_SOCKET;
    }
    g_chromeServer.recvBuf.clear();
    g_chromeServer.pendingText.clear();
    if (g_chromeServer.connected.exchange(false)) {
        PostMessageW(g_chromeServer.notifyHwnd, kChromeSyncStateMsg, 0, 0);
    }
}

// 发送一个原始 WS 帧（服务端线程内部使用，不经过 sendMutex）
bool ChromeSyncSendFrameRaw(int opcode, const std::string& payload) {
    if (g_chromeServer.clientSock == INVALID_SOCKET) {
        return false;
    }
    const std::string frame = wsproto::WsEncodeFrame(opcode, payload, true);
    const int sent = send(g_chromeServer.clientSock, frame.data(),
                          static_cast<int>(frame.size()), 0);
    return sent == static_cast<int>(frame.size());
}

// 大小写不敏感地在一段文本中查找子串
std::string::size_type ChromeSyncFindCI(const std::string& hay,
                                        const char* needle) {
    std::string low = hay;
    std::transform(low.begin(), low.end(), low.begin(),
                   [](unsigned char c) { return static_cast<char>(tolower(c)); });
    const std::string nl = needle;
    std::string lowN = nl;
    std::transform(lowN.begin(), lowN.end(), lowN.begin(),
                   [](unsigned char c) { return static_cast<char>(tolower(c)); });
    return low.find(lowN);
}

// WebSocket 握手：读取请求头，校验并回复 101
int ChromeSyncHandshake() {
    SOCKET c = g_chromeServer.clientSock;
    if (c == INVALID_SOCKET) {
        return -1;
    }
    // 握手阶段较短超时
    DWORD timeout = kChromeSyncHandshakeTimeoutMs;
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&timeout), sizeof(timeout));

    std::string req;
    char tmp[2048];
    while (req.find("\r\n\r\n") == std::string::npos &&
           req.size() < kChromeSyncMaxHeaderBytes) {
        const int r = recv(c, tmp, sizeof(tmp), 0);
        if (r <= 0) {
            return -1;
        }
        req.append(tmp, static_cast<size_t>(r));
    }
    const size_t headerEnd = req.find("\r\n\r\n");
    if (headerEnd == std::string::npos) {
        return -1;
    }

    // 提取 Sec-WebSocket-Key（Chrome 发送的头部大小写固定，仍做容错查找）
    std::string key;
    size_t pos = ChromeSyncFindCI(req, "Sec-WebSocket-Key:");
    if (pos == std::string::npos) {
        return -1;
    }
    pos += 19;
    const size_t lineEnd = req.find("\r\n", pos);
    if (lineEnd == std::string::npos) {
        return -1;
    }
    key = req.substr(pos, lineEnd - pos);
    // 去首尾空白
    size_t b = key.find_first_not_of(" \t");
    size_t e = key.find_last_not_of(" \t");
    if (b == std::string::npos || e == std::string::npos || e < b) {
        return -1;
    }
    key = key.substr(b, e - b + 1);

    const std::string accept = wsproto::ComputeWsAccept(key);
    const std::string resp =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: " +
        accept + "\r\n\r\n";

    size_t sent = 0;
    while (sent < resp.size()) {
        const int r = send(c, resp.data() + sent,
                           static_cast<int>(resp.size() - sent), 0);
        if (r <= 0) {
            return -1;
        }
        sent += static_cast<size_t>(r);
    }

    // 握手请求里可能已夹带首帧（极少见），保留到帧缓冲
    if (headerEnd + 4 < req.size()) {
        g_chromeServer.recvBuf = req.substr(headerEnd + 4);
    }

    // 恢复常规接收超时（探活用）
    timeout = kChromeSyncRecvTimeoutMs;
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    return 0;
}

// 把一条完整文本消息交给 UI 线程（堆上复制，UI 线程释放）
void ChromeSyncDeliverText(const std::string& utf8) {
    const std::wstring wide = wsproto::Utf8ToWide(utf8);
    if (wide.empty()) {
        return;
    }
    auto* copy = new wchar_t[wide.size() + 1];
    wcscpy_s(copy, wide.size() + 1, wide.c_str());
    PostMessageW(g_chromeServer.notifyHwnd, kChromeSyncMsg, 0,
                 reinterpret_cast<LPARAM>(copy));
}

// 解析帧缓冲中的全部完整帧；返回 false 表示协议错误需断开
bool ChromeSyncProcessFrames() {
    while (!g_chromeServer.stop.load()) {
        size_t consumed = 0;
        wsproto::WsFrame frame;
        std::string err;
        const int rc = wsproto::WsDecodeFrame(g_chromeServer.recvBuf, consumed,
                                              frame, err);
        if (rc == 0) {
            return true;  // 数据不足，等更多数据
        }
        if (rc < 0) {
            return false;
        }
        g_chromeServer.recvBuf.erase(0, consumed);

        switch (frame.opcode) {
        case 0x1:  // 文本
            if (frame.fin) {
                ChromeSyncDeliverText(frame.payload);
            } else {
                g_chromeServer.pendingText += frame.payload;
            }
            break;
        case 0x0:  // 延续帧
            g_chromeServer.pendingText += frame.payload;
            if (frame.fin) {
                ChromeSyncDeliverText(g_chromeServer.pendingText);
                g_chromeServer.pendingText.clear();
            }
            break;
        case 0x8:  // 关闭
            ChromeSyncSendFrameRaw(0x8, frame.payload);
            ChromeSyncCloseClient();
            return true;
        case 0x9:  // ping -> pong
            ChromeSyncSendFrameRaw(0xA, frame.payload);
            break;
        case 0xA:  // pong：忽略
            break;
        default:   // 二进制等：忽略
            break;
        }
    }
    return true;
}

void ChromeSyncServerThread() {
    while (!g_chromeServer.stop.load()) {
        if (g_chromeServer.clientSock == INVALID_SOCKET) {
            // 等待新连接（可被 stop / closesocket 打断）
            fd_set rf;
            FD_ZERO(&rf);
            FD_SET(g_chromeServer.listenSock, &rf);
            timeval tv{0, 500000};
            if (select(0, &rf, nullptr, nullptr, &tv) <= 0) {
                continue;
            }
            SOCKET c = accept(g_chromeServer.listenSock, nullptr, nullptr);
            if (c == INVALID_SOCKET) {
                continue;
            }
            {
                std::lock_guard<std::mutex> lock(g_chromeServer.sendMutex);
                if (g_chromeServer.stop.load()) {
                    closesocket(c);
                    break;
                }
                g_chromeServer.clientSock = c;
            }
            if (ChromeSyncHandshake() != 0) {
                ChromeSyncCloseClient();
                continue;
            }
            g_chromeServer.idleTimeouts = 0;
            g_chromeServer.connected.store(true);
            PostMessageW(g_chromeServer.notifyHwnd, kChromeSyncStateMsg, 1, 0);
            continue;
        }

        // 客户端循环
        char tmp[8192];
        const int r = recv(g_chromeServer.clientSock, tmp, sizeof(tmp), 0);
        if (r > 0) {
            g_chromeServer.recvBuf.append(tmp, static_cast<size_t>(r));
            g_chromeServer.idleTimeouts = 0;
            if (!ChromeSyncProcessFrames()) {
                ChromeSyncCloseClient();
            }
            continue;
        }
        if (r == 0) {
            ChromeSyncCloseClient();  // 对端关闭
            continue;
        }
        const int err = WSAGetLastError();
        if (err == WSAETIMEDOUT) {
            // 空闲探活：发 ping；连续多次无响应则断开
            ChromeSyncSendFrameRaw(0x9, "");
            if (++g_chromeServer.idleTimeouts > kChromeSyncMaxIdleTimeouts) {
                ChromeSyncCloseClient();
            }
            continue;
        }
        if (err == WSAEWOULDBLOCK) {
            continue;
        }
        ChromeSyncCloseClient();
    }
}

// 启动服务端（幂等）：绑定 127.0.0.1:kChromeSyncPort 并启动监听线程。
// 端口被占用等失败情形只禁用该功能，不影响顶栏其余部分。
void ChromeSyncStart(HWND hwnd) {
    if (g_chromeServer.thread.joinable()) {
        return;
    }
    g_chromeServer.notifyHwnd = hwnd;

    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        return;
    }
    BOOL reuse = TRUE;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // 仅本机
    addr.sin_port = htons(static_cast<u_short>(kChromeSyncPort));
    if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        closesocket(s);
        return;
    }
    if (listen(s, 4) != 0) {
        closesocket(s);
        return;
    }
    g_chromeServer.listenSock = s;
    g_chromeServer.stop.store(false);
    g_chromeServer.thread = std::thread(ChromeSyncServerThread);
}

// 停止服务端并回收线程（WM_DESTROY 时调用；幂等）
void ChromeSyncStop() {
    g_chromeServer.stop.store(true);
    if (g_chromeServer.listenSock != INVALID_SOCKET) {
        closesocket(g_chromeServer.listenSock);
        g_chromeServer.listenSock = INVALID_SOCKET;
    }
    ChromeSyncCloseClient();
    if (g_chromeServer.thread.joinable()) {
        g_chromeServer.thread.join();
    }
}

// ---- Chrome 风格标签：同应用窗口枚举 / 布局 ----

std::wstring GetWindowTitleText(HWND hwnd) {
    wchar_t buf[256] = {};
    const int n = GetWindowTextW(hwnd, buf, 255);
    if (n > 0) {
        return std::wstring(buf, n);
    }
    wchar_t cls[64] = {};
    if (GetClassNameW(hwnd, cls, 63) > 0) {
        return std::wstring(cls);
    }
    return L"窗口";
}

std::wstring GetProcessName(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) {
        return L"";
    }
    wchar_t path[MAX_PATH] = {};
    DWORD size = MAX_PATH;
    std::wstring name;
    if (QueryFullProcessImageNameW(h, 0, path, &size)) {
        wchar_t* slash = wcsrchr(path, L'\\');
        name = slash ? (slash + 1) : path;
    }
    CloseHandle(h);
    return name;
}

std::wstring GetProcessPath(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) {
        return L"";
    }
    wchar_t path[MAX_PATH] = {};
    DWORD size = MAX_PATH;
    if (QueryFullProcessImageNameW(h, 0, path, &size)) {
        CloseHandle(h);
        return std::wstring(path);
    }
    CloseHandle(h);
    return L"";
}

// 判断两个顶层窗口是否属于同一个"应用"：
// 同进程，或同可执行文件名（覆盖多进程/多窗口应用）。
// 注意不能使用进程树判断：Explorer 启动的子进程会把资源管理器和其他应用误判为同一应用。
bool IsSameApplication(HWND candidate, HWND target) {
    DWORD candPid = 0;
    DWORD targetPid = 0;
    GetWindowThreadProcessId(candidate, &candPid);
    GetWindowThreadProcessId(target, &targetPid);
    if (candPid == 0 || targetPid == 0) {
        return false;
    }
    if (candPid == targetPid) {
        return true;
    }
    const std::wstring candName = GetProcessName(candPid);
    const std::wstring targetName = GetProcessName(targetPid);
    return !candName.empty() && candName == targetName;
}

bool IsShellSystemWindow(HWND hwnd) {
    wchar_t cls[64] = {};
    if (GetClassNameW(hwnd, cls, 63) <= 0) {
        return false;
    }
    return wcscmp(cls, L"Progman") == 0 ||
           wcscmp(cls, L"WorkerW") == 0 ||
           wcscmp(cls, L"SHELLDLL_DefView") == 0 ||
           wcscmp(cls, L"Shell_TrayWnd") == 0 ||
           wcscmp(cls, L"Shell_SecondaryTrayWnd") == 0;
}

struct EnumTabsContext {
    HWND self = nullptr;
    HWND target = nullptr;
    DWORD selfPid = 0;
    std::vector<TabInfo>* tabs = nullptr;
};

BOOL CALLBACK EnumAppWindowsProc(HWND hwnd, LPARAM lParam) {
    auto* ctx = reinterpret_cast<EnumTabsContext*>(lParam);
    if (!ctx || !ctx->tabs || !ctx->target || hwnd == ctx->self) {
        return TRUE;
    }
    if (!IsWindowVisible(hwnd)) {
        return TRUE;
    }
    // 忽略桌面 / 任务栏等系统 Shell 窗口，避免资源管理器标签混入系统窗口
    if (IsShellSystemWindow(hwnd)) {
        return TRUE;
    }
    // 只收集普通应用主窗口：忽略属主窗口（对话框/浮层）和工具窗口
    if (GetWindow(hwnd, GW_OWNER) != nullptr) {
        return TRUE;
    }
    if ((GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) != 0) {
        return TRUE;
    }
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0 || pid == ctx->selfPid) {
        return TRUE;
    }
    if (!IsSameApplication(hwnd, ctx->target)) {
        return TRUE;
    }

    TabInfo info;
    info.hwnd = hwnd;
    info.pid = pid;
    info.title = GetWindowTitleText(hwnd);
    ctx->tabs->push_back(info);
    return TRUE;
}

// 根据当前宽度计算每个标签的矩形（从音量键右侧到右侧三键左侧）。
// 所有标签始终等宽，不随标题长度变化：
//   - 空间充裕（统一宽度放得下所有标签）时，全部使用统一宽度；
//   - 标签多到放不下时，每个标签同时缩小相同程度，
//     总长度恰好铺满标签区（每格 = 可用宽度 / 标签数），
//     即整体缩小时正好是容纳所有标签的最大宽度。
// measureG 保留仅为兼容调用方（已不再需要测量标题）。
void LayoutTabs(AppState& s, std::vector<TabInfo>& tabs,
                Graphics* measureG = nullptr) {
    (void)measureG;
    if (tabs.empty()) {
        return;
    }
    const float k = s.scale;

    RectF volR;
    VolumeButtonRect(s, volR);
    RectF minR, maxR, closeR;
    ComputeButtonRects(s, minR, maxR, closeR);
    RectF clockR;
    ComputeClockRect(s, clockR);

    const float left = volR.X + volR.Width + 6.0f * k;
    // 标签区右边界让位给最小化键左侧的时钟区域
    const float right = clockR.X - 6.0f * k;
    const float available = right - left;
    if (available <= 0.0f) {
        return;
    }

    // 空间充裕取统一宽度；不足则等分铺满（每个标签缩小相同程度）
    const float w = (std::min)(kUniformTabWidth * k,
                               available / static_cast<float>(tabs.size()));
    float x = left;
    for (auto& tab : tabs) {
        tab.rect.X = x;
        tab.rect.Y = 0.0f;
        tab.rect.Width = w;
        tab.rect.Height = static_cast<float>(BarAreaHeight(s));
        x += w;
    }
}

// 重新枚举当前目标应用的全部顶层窗口并刷新标签列表。
// 返回 true 表示列表内容有变化（供定时器决定是否需要重绘）。
bool RefreshTabs(AppState& s) {
    const bool chromeMode = ChromeSyncMode(s);
    std::vector<TabInfo> next;
    if (s.hasTarget && s.targetHwnd && IsWindow(s.targetHwnd)) {
        if (chromeMode) {
            // Chrome 同步模式：标签直接来自扩展推送的浏览器标签页，
            // 顺序即浏览器内顺序（扩展按 index 排序维护）
            next.reserve(s.chromeSync.tabs.size());
            for (const auto& ct : s.chromeSync.tabs) {
                TabInfo ti;
                ti.isChrome = true;
                ti.chromeTabId = ct.id;
                ti.chromeActive = ct.active;
                ti.chromePinned = ct.pinned;
                ti.title = ct.title;
                if (ti.title.empty()) {
                    ti.title = ct.url;
                }
                if (ti.title.empty()) {
                    ti.title = L"新标签页";
                }
                next.push_back(std::move(ti));
            }
        } else {
            EnumTabsContext ctx;
            ctx.self = s.hwnd;
            ctx.target = s.targetHwnd;
            ctx.selfPid = GetCurrentProcessId();
            ctx.tabs = &next;
            EnumWindows(EnumAppWindowsProc, reinterpret_cast<LPARAM>(&ctx));
        }
    }

    // 保持已有标签顺序：切换聚焦/置顶不会让窗口跳到第一位，新窗口追加到末尾。
    // EnumWindows 返回的是 Z 序，直接用会让聚焦窗口总排在第一。
    // （Chrome 同步模式的顺序由扩展维护，跳过重排）
    bool hasCommonWindow = false;
    if (!chromeMode && !s.tabs.empty() && !next.empty()) {
        for (const auto& old : s.tabs) {
            for (const auto& n : next) {
                if (old.hwnd == n.hwnd) {
                    hasCommonWindow = true;
                    break;
                }
            }
            if (hasCommonWindow) {
                break;
            }
        }
    }
    if (!hasCommonWindow && !next.empty()) {
        // 首次遇到该应用（或刚从别的应用切换过来）时按 Z 序倒序排列，
        // 近似“打开顺序”，避免聚焦窗口固定第一。
        // （Chrome 同步模式同样跳过：扩展已按浏览器顺序给出）
        if (!chromeMode) {
            std::reverse(next.begin(), next.end());
        }
    }
    if (!chromeMode && !s.tabs.empty() && !next.empty()) {
        std::vector<TabInfo> ordered;
        ordered.reserve(next.size());
        std::vector<bool> used(next.size(), false);
        for (const auto& old : s.tabs) {
            for (size_t i = 0; i < next.size(); ++i) {
                if (!used[i] && next[i].hwnd == old.hwnd) {
                    ordered.push_back(next[i]);
                    used[i] = true;
                    break;
                }
            }
        }

        // 右键新建窗口的待插入：新出现的标签插入到指定标签右侧
        if (s.insertAfterTab && s.insertPending) {
            bool foundAnchor = false;
            bool inserted = false;
            HWND newHwnd = nullptr;
            for (size_t pos = 0; pos < ordered.size(); ++pos) {
                if (ordered[pos].hwnd == s.insertAfterTab) {
                    foundAnchor = true;
                    for (size_t i = 0; i < next.size(); ++i) {
                        if (!used[i]) {
                            newHwnd = next[i].hwnd;
                            ordered.insert(ordered.begin() + pos + 1, next[i]);
                            used[i] = true;
                            inserted = true;
                            break;
                        }
                    }
                    break;
                }
            }
            if (inserted && newHwnd) {
                // 新窗口出现后主动抢前台，解决 ShellExecute 打开后无法聚焦的问题；
                // 单次尝试可能撞上前台锁 / 窗口初始化等瞬时状态，
                // 因此挂起重试，直到窗口真正成为前台、用户切走或达到上限
                s.pendingFocusHwnd = newHwnd;
                s.pendingFocusOriginHwnd = GetForegroundWindow();
                s.pendingFocusAttempts = 0;
                ForceForegroundWindow(newHwnd);
            }
            if (!foundAnchor || inserted) {
                s.insertAfterTab = nullptr;
                s.insertPending = false;
            }
        }

        for (size_t i = 0; i < next.size(); ++i) {
            if (!used[i]) {
                ordered.push_back(next[i]);
            }
        }
        next.swap(ordered);
    }

    // 右键新建窗口的聚焦重试：新窗口出现后可能因前台锁 / 初始化时序
    // 第一次抢前台失败，随标签刷新定时器（约 1s）重试。
    // 窗口消失、已成为前台、用户已切到其他应用或达到次数上限即停止。
    if (s.pendingFocusHwnd) {
        const HWND fg = GetForegroundWindow();
        const bool done = !IsWindow(s.pendingFocusHwnd) || fg == s.pendingFocusHwnd;
        if (done || ++s.pendingFocusAttempts > kMaxPendingFocusAttempts) {
            s.pendingFocusHwnd = nullptr;
            s.pendingFocusOriginHwnd = nullptr;
            s.pendingFocusAttempts = 0;
        } else {
            // 用户已主动切到其他窗口（不是挂起时的前台窗口，
            // 也不是桌面/任务栏等系统窗口）：不再抢前台，避免打扰用户
            if (fg && fg != s.pendingFocusOriginHwnd &&
                !IsShellSystemWindow(fg)) {
                s.pendingFocusHwnd = nullptr;
                s.pendingFocusOriginHwnd = nullptr;
                s.pendingFocusAttempts = 0;
            } else {
                ForceForegroundWindow(s.pendingFocusHwnd);
            }
        }
    }

    if (s.bitmap) {
        Graphics g(s.bitmap);
        LayoutTabs(s, next, &g);
    } else {
        LayoutTabs(s, next, nullptr);
    }

    bool changed = s.tabs.size() != next.size();
    if (!changed) {
        for (size_t i = 0; i < next.size(); ++i) {
            const TabInfo& a = s.tabs[i];
            const TabInfo& b = next[i];
            if (a.isChrome != b.isChrome || a.hwnd != b.hwnd ||
                a.title != b.title ||
                (a.isChrome &&
                 (a.chromeTabId != b.chromeTabId ||
                  a.chromeActive != b.chromeActive ||
                  a.chromePinned != b.chromePinned))) {
                changed = true;
                break;
            }
        }
    }

    if (changed) {
        s.tabs.swap(next);
        if (s.hoverTab >= static_cast<int>(s.tabs.size())) {
            s.hoverTab = -1;
        }
    }

    // 定时器状态维护：
    // - 挂起态（右键新建窗口待插入 / 抢前台重试）保持 200ms 短轮询，结束即停
    // - 有目标时保持 10s 低频兜底自检（一次性，每次刷新重置，
    //   事件驱动正常时基本不触发，仅防窗口事件丢失）；无目标时零轮询
    if (s.insertPending) {
        // 新窗口长时间未出现（启动失败等）：超时放弃，避免短轮询常驻
        if (s.insertPendingSince &&
            GetTickCount64() - s.insertPendingSince > 10000) {
            s.insertAfterTab = nullptr;
            s.insertPending = false;
        }
    }
    if (s.insertPending || s.pendingFocusHwnd) {
        SetTimer(s.hwnd, kTabRefreshTimerId, kPendingPollMs, nullptr);
    } else {
        KillTimer(s.hwnd, kTabRefreshTimerId);
    }
    if (s.hasTarget && s.targetHwnd && IsWindow(s.targetHwnd)) {
        SetTimer(s.hwnd, kSlowRefreshTimerId, kSlowRefreshMs, nullptr);
    } else {
        KillTimer(s.hwnd, kSlowRefreshTimerId);
    }
    return changed;
}

// 枚举默认渲染端点的音频会话，返回与目标进程匹配的会话。
// Chrome/Edge 等多进程应用的窗口属于主进程，但音频会话在渲染子进程，
// 因此匹配目标进程的整个进程树（自身 + 所有后代）。
ComPtr<ISimpleAudioVolume> FindProcessVolumeSession(DWORD pid) {
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                CLSCTX_ALL, IID_PPV_ARGS(&enumerator)))) {
        return nullptr;
    }

    ComPtr<IMMDevice> device;
    if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device))) {
        return nullptr;
    }

    ComPtr<IAudioSessionManager2> manager;
    void* managerRaw = nullptr;
    if (FAILED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL,
                                nullptr, &managerRaw))) {
        return nullptr;
    }
    manager.Attach(static_cast<IAudioSessionManager2*>(managerRaw));

    ComPtr<IAudioSessionEnumerator> sessionEnum;
    if (FAILED(manager->GetSessionEnumerator(&sessionEnum))) {
        return nullptr;
    }

    int count = 0;
    if (FAILED(sessionEnum->GetCount(&count))) {
        return nullptr;
    }

    for (int i = 0; i < count; ++i) {
        ComPtr<IAudioSessionControl> control;
        if (FAILED(sessionEnum->GetSession(i, &control))) {
            continue;
        }
        ComPtr<IAudioSessionControl2> ctrl2;
        if (FAILED(control->QueryInterface(IID_PPV_ARGS(&ctrl2)))) {
            continue;
        }
        DWORD sessionPid = 0;
        if (FAILED(ctrl2->GetProcessId(&sessionPid))) {
            continue;
        }
        if (ProcessTreeContains(pid, sessionPid)) {
            ComPtr<ISimpleAudioVolume> vol;
            if (SUCCEEDED(control->QueryInterface(IID_PPV_ARGS(&vol)))) {
                return vol;
            }
        }
    }
    return nullptr;
}

// 刷新音量会话：目标窗口变化时重新解析其进程的音频会话
void ResolveVolumeSession(AppState& s) {
    s.volumeSession.Reset();
    s.volumeSessionPid = 0;
    s.volumeReady = false;

    if (!s.hasTarget || !s.targetHwnd) {
        return;
    }
    DWORD pid = 0;
    GetWindowThreadProcessId(s.targetHwnd, &pid);
    if (pid == 0) {
        return;
    }

    ComPtr<ISimpleAudioVolume> vol = FindProcessVolumeSession(pid);
    if (!vol) {
        return;
    }

    s.volumeSession = vol;
    s.volumeSessionPid = pid;
    s.volumePid = pid;

    float v = 0.0f;
    BOOL muted = FALSE;
    vol->GetMasterVolume(&v);
    vol->GetMute(&muted);

    s.volumeValue = v;
    s.volumeMuted = muted != FALSE;
    s.volumeReady = true;
}

void ApplyVolume(AppState& s, float value) {
    if (!s.volumeSession) {
        return;
    }
    value = std::clamp(value, 0.0f, 1.0f);
    s.volumeValue = value;
    s.volumeSession->SetMasterVolume(value, nullptr);
}

void ToggleMute(AppState& s) {
    if (!s.volumeSession) {
        return;
    }
    s.volumeMuted = !s.volumeMuted;
    s.volumeSession->SetMute(BOOL(s.volumeMuted), nullptr);
}

// ---- 全局鼠标/键盘钩子 ----
// 用途：1) 点击面板外任意处（含桌面/其他窗口）时收起音量面板；
//      2) 记录用户"显式"点击 / Alt+Tab 切换窗口的信号，
//         供目标跟踪区分"系统自动转移焦点"与"用户主动切换窗口"。
constexpr UINT kCloseVolumeMsg = WM_APP + 2;
constexpr UINT kUserSwitchMsg = WM_APP + 3;  // 用户显式点击/Alt+Tab 其他窗口时通知刷新目标
HHOOK g_volumeHook = nullptr;
HWND g_volumeHookHwnd = nullptr;

LRESULT CALLBACK VolumeLowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION && wParam == WM_LBUTTONDOWN) {
        const auto* info = reinterpret_cast<const MSLLHOOKSTRUCT*>(lParam);
        HWND hwnd = g_volumeHookHwnd;
        AppState* s = hwnd ? reinterpret_cast<AppState*>(
            GetWindowLongPtrW(hwnd, GWLP_USERDATA)) : nullptr;

        // 记录用户显式点击的顶层窗口（排除顶栏自身与音量面板）
        HWND clicked = WindowFromPoint(info->pt);
        if (clicked) {
            clicked = GetAncestor(clicked, GA_ROOT);
            if (clicked == hwnd ||
                (s && s->volumePanelHwnd && clicked == s->volumePanelHwnd)) {
                clicked = nullptr;
            }
        }
        g_lastClickWindow = clicked;

        // 目标粘住时，用户显式点击其他窗口应立即解除粘住并切换，
        // 不依赖 WinEvent 事件（事件可能延迟或不触发）
        if (s && s->targetSticky && clicked &&
            clicked != s->targetHwnd && IsControlTarget(clicked, hwnd)) {
            PostMessageW(hwnd, kUserSwitchMsg, 0, 0);
        }

        if (s && s->volumeOpen) {
            // 音量按钮屏幕区域（点它不收起，由顶栏自己 toggle）
            RECT rc{};
            GetWindowRect(hwnd, &rc);
            RectF volR;
            VolumeButtonRect(*s, volR);
            RECT volScreen = {
                rc.left + static_cast<LONG>(volR.X),
                rc.top + static_cast<LONG>(volR.Y),
                rc.left + static_cast<LONG>(volR.X + volR.Width),
                rc.top + static_cast<LONG>(volR.Y + volR.Height)
            };

            // 独立音量面板窗口屏幕区域
            bool inPanel = false;
            if (s->volumePanelHwnd) {
                RECT panelScreen{};
                GetWindowRect(s->volumePanelHwnd, &panelScreen);
                inPanel = PtInRect(&panelScreen, info->pt) != FALSE;
            }

            const bool inVolBtn = PtInRect(&volScreen, info->pt) != FALSE;
            if (!inPanel && !inVolBtn) {
                PostMessageW(hwnd, kCloseVolumeMsg, 0, 0);
            }
        }
    }
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        const auto* info = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
        // Alt+Tab：系统键按下 Tab 且 Alt 处于按下状态
        if ((wParam == WM_SYSKEYDOWN || wParam == WM_KEYDOWN) &&
            info->vkCode == VK_TAB &&
            (GetAsyncKeyState(VK_MENU) & 0x8000) != 0) {
            g_altTabPressed = true;
            HWND bar = g_volumeHookHwnd;
            AppState* s = bar ? reinterpret_cast<AppState*>(
                GetWindowLongPtrW(bar, GWLP_USERDATA)) : nullptr;
            // 目标粘住时，Alt+Tab 也立即通知刷新（不依赖 WinEvent）
            if (s && s->targetSticky) {
                PostMessageW(bar, kUserSwitchMsg, 0, 0);
            }
        }
    }
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

// ---- WinEvent 钩子：替代前台窗口 / 最大化状态 / 标签列表的轮询 ----
// 前台切换、最小化/最大化/还原、目标应用窗口增删/显隐/标题变化
// 均由系统事件驱动，不再用常驻定时器轮询。
HWINEVENTHOOK g_winEventHook[5] = {nullptr, nullptr, nullptr,
                                   nullptr, nullptr};

void CALLBACK WinEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd,
                           LONG idObject, LONG idChild, DWORD, DWORD) {
    if (idObject != OBJID_WINDOW || idChild != CHILDID_SELF) {
        return;
    }
    HWND bar = g_volumeHookHwnd;
    AppState* s = bar ? reinterpret_cast<AppState*>(
        GetWindowLongPtrW(bar, GWLP_USERDATA)) : nullptr;
    if (!s) {
        return;
    }

    if (event == EVENT_SYSTEM_FOREGROUND) {
        // 新窗口激活瞬间可能尚未可见（正在创建）：立即按前台处理会
        // 把目标误清（见 UpdateTarget）。延迟 150ms 等窗口可见后再复查，
        // 期间保持当前目标不变；复查时仍不可见则继续（见 kMaxTargetRetry）。
        if (hwnd && IsWindow(hwnd) && !IsWindowVisible(hwnd)) {
            s->targetRetryCount = 0;
            SetTimer(bar, kTargetRetryTimerId, 150, nullptr);
            return;
        }
        UpdateTarget(*s);
        DrawBarAndPresent(*s);
        return;
    }

    // 窗口状态事件：只关心当前目标或前台窗口
    HWND top = GetAncestor(hwnd, GA_ROOT);
    switch (event) {
    case EVENT_SYSTEM_MINIMIZESTART:
    case EVENT_SYSTEM_MINIMIZEEND:
    case EVENT_SYSTEM_MAXIMIZESTART:
    case EVENT_SYSTEM_MAXIMIZEEND:
    case EVENT_SYSTEM_RESTORE:
        if (top == s->targetHwnd || top == GetForegroundWindow()) {
            UpdateTarget(*s);
            DrawBarAndPresent(*s);
        }
        break;
    case EVENT_OBJECT_CREATE:
    case EVENT_OBJECT_DESTROY:
    case EVENT_OBJECT_SHOW:
    case EVENT_OBJECT_HIDE:
    case EVENT_OBJECT_NAMECHANGE:
        // 目标应用顶层窗口增删/显隐/标题变化：事件驱动刷新标签。
        // 只处理目标应用的顶层窗口，降低高频对象事件的噪音；
        // 经 100ms 抖动合并，避免连续事件反复刷新。
        if (!s->hasTarget || !s->targetHwnd || !IsWindow(s->targetHwnd)) {
            break;
        }
        // 只看顶层窗口（排除子控件）；销毁事件中句柄可能已部分失效，放宽
        if (event != EVENT_OBJECT_DESTROY && hwnd &&
            GetAncestor(hwnd, GA_ROOT) != hwnd) {
            break;
        }
        if (hwnd) {
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            // 查不到进程（窗口正在销毁）时保守刷新
            if (pid && !IsSameApplication(hwnd, s->targetHwnd)) {
                break;
            }
        }
        SetTimer(bar, kTabRefreshDebounceTimerId, kTabRefreshDebounceMs,
                 nullptr);
        break;
    default:
        break;
    }
}

void InstallVolumeHook(HWND hwnd) {
    if (!g_volumeHook) {
        g_volumeHookHwnd = hwnd;
        g_volumeHook = SetWindowsHookExW(WH_MOUSE_LL, VolumeLowLevelMouseProc,
                                         GetModuleHandleW(nullptr), 0);
    }
    if (!g_keyHook) {
        g_keyHook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc,
                                      GetModuleHandleW(nullptr), 0);
    }
    if (!g_winEventHook[0]) {
        // 前台窗口切换
        g_winEventHook[0] = SetWinEventHook(
            EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
            nullptr, WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT);
        // 最小化 / 最大化
        g_winEventHook[1] = SetWinEventHook(
            EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MAXIMIZEEND,
            nullptr, WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT);
        // 还原
        g_winEventHook[2] = SetWinEventHook(
            EVENT_SYSTEM_RESTORE, EVENT_SYSTEM_RESTORE,
            nullptr, WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT);
        // 目标应用窗口创建 / 销毁 / 显示 / 隐藏：事件驱动刷新标签
        g_winEventHook[3] = SetWinEventHook(
            EVENT_OBJECT_CREATE, EVENT_OBJECT_HIDE,
            nullptr, WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT);
        // 目标应用窗口标题变化
        g_winEventHook[4] = SetWinEventHook(
            EVENT_OBJECT_NAMECHANGE, EVENT_OBJECT_NAMECHANGE,
            nullptr, WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT);
    }
}

void UninstallVolumeHook() {
    if (g_volumeHook) {
        UnhookWindowsHookEx(g_volumeHook);
        g_volumeHook = nullptr;
    }
    if (g_keyHook) {
        UnhookWindowsHookEx(g_keyHook);
        g_keyHook = nullptr;
    }
    for (auto& h : g_winEventHook) {
        if (h) {
            UnhookWinEvent(h);
            h = nullptr;
        }
    }
    g_volumeHookHwnd = nullptr;
    g_lastClickWindow = nullptr;
    g_altTabPressed = false;
}

// ---- 独立音量面板窗口 ----
void DestroyPanelBacking(AppState& s) {
    delete s.panelBitmap;
    s.panelBitmap = nullptr;

    if (s.panelHdcMem) {
        if (s.panelHbmDib) {
            if (s.panelHbmOld) {
                SelectObject(s.panelHdcMem, s.panelHbmOld);
            }
            DeleteObject(s.panelHbmDib);
            s.panelHbmDib = nullptr;
        }
        DeleteDC(s.panelHdcMem);
        s.panelHdcMem = nullptr;
    }
    s.panelHbmOld = nullptr;
    s.panelPvBits = nullptr;
}

bool CreatePanelBacking(AppState& s, int w, int h) {
    DestroyPanelBacking(s);

    HDC screenDc = GetDC(s.volumePanelHwnd ? s.volumePanelHwnd : s.hwnd);
    if (!screenDc) {
        return false;
    }
    s.panelHdcMem = CreateCompatibleDC(screenDc);
    if (!s.panelHdcMem) {
        ReleaseDC(s.volumePanelHwnd, screenDc);
        return false;
    }

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    s.panelHbmDib = CreateDIBSection(screenDc, &bmi, DIB_RGB_COLORS, &s.panelPvBits,
                                     nullptr, 0);
    ReleaseDC(s.volumePanelHwnd ? s.volumePanelHwnd : s.hwnd, screenDc);
    if (!s.panelHbmDib) {
        DeleteDC(s.panelHdcMem);
        s.panelHdcMem = nullptr;
        return false;
    }

    s.panelHbmOld = static_cast<HBITMAP>(SelectObject(s.panelHdcMem, s.panelHbmDib));
    s.panelBitmap = new Gdiplus::Bitmap(w, h, PixelFormat32bppPARGB);
    if (!s.panelBitmap) {
        DestroyPanelBacking(s);
        return false;
    }
    return true;
}

// 绘制音量面板内容（面板窗口客户区，panel 从 (0,0) 开始）
void DrawVolumePanelContent(Graphics& g, AppState& s) {
    const float k = s.scale;
    const int pw = MulDiv(kVolumePanelW, s.dpi, 96);
    const int ph = MulDiv(kVolumePanelH, s.dpi, 96);
    RectF panel(0, 0, static_cast<float>(pw), static_cast<float>(ph));
    RectF mute, track;
    VolumePanelInner(s, panel, mute, track);

    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(PixelOffsetModeHighQuality);
    g.Clear(Color(0, 0, 0, 0));

    // 面板背景：圆角矩形 + 细边框
    GraphicsPath panelPath;
    AddRoundedRectPath(panelPath, panel, 10.0f * k);
    SolidBrush panelBg(Color(210, 44, 48, 56));
    g.FillPath(&panelBg, &panelPath);
    Pen panelBorder(Color(100, 255, 255, 255), 1.0f);
    g.DrawPath(&panelBorder, &panelPath);

    // 静音按钮
    DrawButtonHover(g, mute, kHitMute, s.hoverButton);
    DrawVolumeGlyph(g, mute, k,
                    s.volumeMuted ? Color(255, 96, 92, 84)
                                  : Color(220, 255, 255, 255),
                    s.volumeMuted);

    // 滑条轨道（两端圆角的小胶囊）
    const float trackRadius = track.Height * 0.5f;
    if (!s.volumeReady) {
        GraphicsPath disPath;
        AddRoundedRectPath(disPath, track, trackRadius);
        SolidBrush dis(Color(50, 200, 200, 200));
        g.FillPath(&dis, &disPath);
    } else {
        GraphicsPath basePath;
        AddRoundedRectPath(basePath, track, trackRadius);
        SolidBrush base(Color(90, 90, 90, 100));
        g.FillPath(&base, &basePath);

        RectF filled(track.X, track.Y, track.Width * s.volumeValue, track.Height);
        if (filled.Width > trackRadius * 2.0f) {
            GraphicsPath fillPath;
            AddRoundedRectPath(fillPath, filled, trackRadius);
            SolidBrush fillBrush(Color(150, 90, 200, 250));
            g.FillPath(&fillBrush, &fillPath);
        } else {
            SolidBrush fillBrush(Color(150, 90, 200, 250));
            g.FillRectangle(&fillBrush, filled);
        }

        const float thumbR = 6.0f * k;
        const float thumbX = track.X + track.Width * s.volumeValue;
        const float thumbY = track.Y + track.Height * 0.5f;
        SolidBrush thumbCol(Color(255, 255, 255, 255));
        g.FillEllipse(&thumbCol, thumbX - thumbR, thumbY - thumbR,
                      thumbR * 2.0f, thumbR * 2.0f);
    }

    RectF pctR(panel.X + panel.Width - 56.0f * k, panel.Y, 50.0f * k, panel.Height);
    DrawVolumePercent(g, s, pctR, s.volumeValue, s.volumeReady);
}

void PresentVolumePanel(AppState& s) {
    if (!s.panelBitmap || !s.panelHdcMem || !s.panelPvBits || !s.volumePanelHwnd) {
        return;
    }
    const int w = MulDiv(kVolumePanelW, s.dpi, 96);
    const int h = MulDiv(kVolumePanelH, s.dpi, 96);

    Gdiplus::Rect lockRect(0, 0, w, h);
    BitmapData bd{};
    if (s.panelBitmap->LockBits(&lockRect, ImageLockModeRead,
                                PixelFormat32bppPARGB, &bd) != Ok) {
        return;
    }
    auto* dst = static_cast<BYTE*>(s.panelPvBits);
    const auto* src = static_cast<const BYTE*>(bd.Scan0);
    const size_t rowBytes = static_cast<size_t>(w) * 4u;
    for (int y = 0; y < h; ++y) {
        std::memcpy(dst + static_cast<size_t>(y) * rowBytes,
                    src + static_cast<size_t>(y) * static_cast<size_t>(bd.Stride),
                    rowBytes);
    }
    s.panelBitmap->UnlockBits(&bd);

    RECT rc{};
    GetWindowRect(s.volumePanelHwnd, &rc);
    POINT ptDst{rc.left, rc.top};
    POINT ptSrc{0, 0};
    SIZE size{w, h};

    BLENDFUNCTION blend{};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;

    HDC screenDc = GetDC(nullptr);
    if (screenDc) {
        UpdateLayeredWindow(s.volumePanelHwnd, screenDc, &ptDst, &size,
                            s.panelHdcMem, &ptSrc, 0, &blend, ULW_ALPHA);
        ReleaseDC(nullptr, screenDc);
    }
}

// 面板窗口命中：客户区坐标 -> 静音/滑条
ButtonHit VolumePanelHitTest(AppState& s, int x, int y) {
    const int pw = MulDiv(kVolumePanelW, s.dpi, 96);
    const int ph = MulDiv(kVolumePanelH, s.dpi, 96);
    if (x < 0 || y < 0 || x >= pw || y >= ph) {
        return kHitNone;
    }
    RectF panel(0, 0, static_cast<float>(pw), static_cast<float>(ph));
    RectF mute, track;
    VolumePanelInner(s, panel, mute, track);
    if (x >= static_cast<int>(mute.X) && x < static_cast<int>(mute.X + mute.Width) &&
        y >= static_cast<int>(mute.Y) && y < static_cast<int>(mute.Y + mute.Height)) {
        return kHitMute;
    }
    return kHitSlider;
}

LRESULT CALLBACK VolumePanelWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
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
        return DefWindowProcW(hwnd, msg, wParam, lParam);

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps{};
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_NCHITTEST:
        return HTCLIENT;

    case WM_WINDOWPOSCHANGING: {
        // 音量面板是置顶浮层：任何 Z 序变化都强制保持置顶，防止被普通窗口覆盖
        auto* wp = reinterpret_cast<WINDOWPOS*>(lParam);
        if ((wp->flags & SWP_NOZORDER) == 0 && wp->hwndInsertAfter != HWND_TOPMOST) {
            wp->hwndInsertAfter = HWND_TOPMOST;
            wp->flags |= SWP_NOACTIVATE;
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    case WM_MOUSEMOVE: {
        if (!s) {
            return 0;
        }
        if (!s->trackingMouse) {
            TRACKMOUSEEVENT tme{};
            tme.cbSize = sizeof(tme);
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = hwnd;
            TrackMouseEvent(&tme);
            s->trackingMouse = true;
        }
        POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (s->volumeDragging) {
            RectF panel(0, 0,
                        static_cast<float>(MulDiv(kVolumePanelW, s->dpi, 96)),
                        static_cast<float>(MulDiv(kVolumePanelH, s->dpi, 96)));
            RectF mute, track;
            VolumePanelInner(*s, panel, mute, track);
            const float span = track.Width > 1.0f ? track.Width : 1.0f;
            const float ratio = (static_cast<float>(pt.x) - track.X) / span;
            ApplyVolume(*s, std::clamp(ratio, 0.0f, 1.0f));
            Graphics g(s->panelBitmap);
            DrawVolumePanelContent(g, *s);
            PresentVolumePanel(*s);
        }
        const ButtonHit hit = VolumePanelHitTest(*s, pt.x, pt.y);
        if (hit != s->hoverButton) {
            s->hoverButton = hit;
            Graphics g(s->panelBitmap);
            DrawVolumePanelContent(g, *s);
            PresentVolumePanel(*s);
        }
        return 0;
    }

    case WM_MOUSELEAVE:
        if (s) {
            s->trackingMouse = false;
            if (!s->volumeDragging && s->hoverButton != kHitNone) {
                s->hoverButton = kHitNone;
                Graphics g(s->panelBitmap);
                DrawVolumePanelContent(g, *s);
                PresentVolumePanel(*s);
            }
        }
        return 0;

    case WM_LBUTTONDOWN: {
        if (!s) {
            return 0;
        }
        POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        const ButtonHit hit = VolumePanelHitTest(*s, pt.x, pt.y);
        if (hit == kHitMute) {
            ToggleMute(*s);
            Graphics g(s->panelBitmap);
            DrawVolumePanelContent(g, *s);
            PresentVolumePanel(*s);
        } else if (hit == kHitSlider && s->volumeReady && s->volumeSession) {
            s->volumeDragging = true;
            SetCapture(hwnd);
            RectF panel(0, 0,
                        static_cast<float>(MulDiv(kVolumePanelW, s->dpi, 96)),
                        static_cast<float>(MulDiv(kVolumePanelH, s->dpi, 96)));
            RectF mute, track;
            VolumePanelInner(*s, panel, mute, track);
            const float span = track.Width > 1.0f ? track.Width : 1.0f;
            const float ratio = (static_cast<float>(pt.x) - track.X) / span;
            ApplyVolume(*s, std::clamp(ratio, 0.0f, 1.0f));
            Graphics g(s->panelBitmap);
            DrawVolumePanelContent(g, *s);
            PresentVolumePanel(*s);
        }
        return 0;
    }

    case WM_LBUTTONUP:
        if (s && s->volumeDragging) {
            s->volumeDragging = false;
            ReleaseCapture();
        }
        return 0;

    case WM_CAPTURECHANGED:
        // 捕获被系统或其他窗口夺走（如面板被隐藏/销毁）时结束滑块拖动，
        // 避免残留拖拽状态；捕获此刻已不属于本窗口，无需再 ReleaseCapture。
        if (s) {
            s->volumeDragging = false;
        }
        return 0;

    case WM_DESTROY:
        if (s) {
            s->volumePanelHwnd = nullptr;
        }
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool RegisterVolumePanelClass() {
    static bool registered = false;
    if (registered) {
        return true;
    }
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = VolumePanelWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"DesktopTopBarVolumePanelWindow";
    registered = RegisterClassExW(&wc) != 0;
    return registered;
}

void ShowVolumePanel(AppState& s) {
    if (!RegisterVolumePanelClass()) {
        return;
    }
    const int pw = MulDiv(kVolumePanelW, s.dpi, 96);
    const int ph = MulDiv(kVolumePanelH, s.dpi, 96);

    RECT barRc{};
    GetWindowRect(s.hwnd, &barRc);
    const int x = barRc.left + static_cast<int>(kVolumePanelMargin * s.scale);
    const int y = barRc.top + BarAreaHeight(s);

    if (!s.volumePanelHwnd) {
        s.volumePanelHwnd = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST,
            L"DesktopTopBarVolumePanelWindow", L"",
            WS_POPUP,
            x, y, pw, ph,
            nullptr, nullptr, GetModuleHandleW(nullptr), &s);

        if (!s.volumePanelHwnd) {
            return;
        }
        // 音量面板是独立置顶窗口：不挂到桌面层，始终显示在普通窗口之上
    }

    if (!CreatePanelBacking(s, pw, ph)) {
        return;
    }
    Graphics g(s.panelBitmap);
    DrawVolumePanelContent(g, s);
    PresentVolumePanel(s);

    // 显示并保持置顶（音量面板与顶栏不同，允许覆盖普通窗口）
    ShowWindow(s.volumePanelHwnd, SW_SHOWNOACTIVATE);
    SetWindowPos(s.volumePanelHwnd, HWND_TOPMOST, x, y, pw, ph,
                 SWP_NOACTIVATE);
}

void HideVolumePanel(AppState& s) {
    if (s.volumePanelHwnd) {
        // 正在拖动滑块时收起：先释放捕获再隐藏，避免捕获留在隐藏窗口上，
        // 把后续全部鼠标输入都吞进不可见面板（全局点不动、键盘正常）。
        if (s.volumeDragging) {
            s.volumeDragging = false;
            ReleaseCapture();
        }
        ShowWindow(s.volumePanelHwnd, SW_HIDE);
    }
}

// 展开 / 收起音量面板；顶栏自身高度保持不变，面板为独立小窗口
void SetVolumeOpen(AppState& s, bool open) {
    if (s.volumeOpen == open) {
        return;
    }
    s.volumeOpen = open;
    if (open) {
        ShowVolumePanel(s);
    } else {
        HideVolumePanel(s);
    }
}

// ---- 绘制 ----

// 绘制单个按钮的悬停/点击高亮背景。
void DrawButtonHover(Graphics& g, const RectF& r, ButtonHit hit, int hover) {
    if (hover != hit) {
        return;
    }
    if (hit == kHitClock) {
        // Win11 任务栏时钟：悬停时显示内缩的圆角药丸高亮
        const float inset = r.Height * 0.08f;
        RectF pill(r.X + inset, r.Y + inset,
                   r.Width - inset * 2.0f, r.Height - inset * 2.0f);
        GraphicsPath path;
        AddRoundedRectPath(path, pill, pill.Height * 0.28f);
        SolidBrush bg(Color(50, 255, 255, 255));
        g.FillPath(&bg, &path);
        return;
    }
    Color hoverColor(70, 255, 255, 255);
    if (hit == kHitClose) {
        // Chrome 关闭按钮：红色背景
        hoverColor = Color(200, 224, 60, 49);
    }
    SolidBrush bg(hoverColor);
    g.FillRectangle(&bg, r);
}

// 最小化字形：横线
void DrawMinimizeGlyph(Graphics& g, const RectF& r, float scale, const Color& col) {
    const float cx = r.X + r.Width * 0.5f;
    const float cy = r.Y + r.Height * 0.5f;
    Pen pen(col, 1.6f * scale);
    pen.SetStartCap(LineCapRound);
    pen.SetEndCap(LineCapRound);
    g.DrawLine(&pen, cx - 5.0f * scale, cy, cx + 5.0f * scale, cy);
}

// 最大化 / 还原字形（需知窗口是否最大化）
void DrawMaximizeGlyph(Graphics& g, const RectF& r, float scale, bool maximized,
                       const Color& col) {
    const float cx = r.X + r.Width * 0.5f;
    const float cy = r.Y + r.Height * 0.5f;
    Pen pen(col, 1.4f * scale);
    const float box = 4.6f * scale;
    if (maximized) {
        // 还原：两个交叠方块
        RectF r1(cx - box, cy - box + 1.5f * scale, box * 2.0f, box * 2.0f);
        g.DrawRectangle(&pen, r1);
        RectF r2(cx - box + 2.2f * scale, cy - box - 1.5f * scale,
                 box * 2.0f, box * 2.0f);
        g.DrawRectangle(&pen, r2);
    } else {
        RectF r1(cx - box, cy - box, box * 2.0f, box * 2.0f);
        g.DrawRectangle(&pen, r1);
    }
}

// 关闭字形：X
void DrawCloseGlyph(Graphics& g, const RectF& r, float scale, const Color& col) {
    const float cx = r.X + r.Width * 0.5f;
    const float cy = r.Y + r.Height * 0.5f;
    Pen pen(col, 1.6f * scale);
    pen.SetStartCap(LineCapRound);
    pen.SetEndCap(LineCapRound);
    const float d = 5.0f * scale;
    g.DrawLine(&pen, cx - d, cy - d, cx + d, cy + d);
    g.DrawLine(&pen, cx + d, cy - d, cx - d, cy + d);
}

// 使用 Windows 自带的 Segoe MDL2 Assets 图标字体绘制喇叭/音量图标，
// 与系统音量按钮完全一致（E767=喇叭，E74F=静音喇叭）。
void DrawVolumeGlyph(Graphics& g, const RectF& r, float scale, const Color& col,
                     bool muted) {
    (void)scale;  // 字体字形已按 r.Height 缩放，无需手动 scale
    // 优先 Segoe Fluent Icons（Win11），回退 Segoe MDL2 Assets（Win10）
    FontFamily famFluent(L"Segoe Fluent Icons");
    FontFamily famMdl2(L"Segoe MDL2 Assets");
    FontFamily* fam = &famMdl2;
    if (famFluent.GetLastStatus() == Ok) {
        fam = &famFluent;
    }

    // 字形尺寸要明显小于按钮高度，并给四周留内边距，
    // 避免贴左上角时图标超出左/上边缘。
    const float fontSize = r.Height * 0.62f;
    Gdiplus::Font font(fam, fontSize, FontStyleRegular, UnitPixel);

    wchar_t ch = muted ? 0xE74F : 0xE767;
    StringFormat sf;
    sf.SetAlignment(StringAlignmentCenter);
    sf.SetLineAlignment(StringAlignmentCenter);

    const float inset = r.Height * 0.12f;
    RectF drawR(r.X + inset, r.Y + inset,
                r.Width - inset * 2.0f, r.Height - inset * 2.0f);

    SolidBrush brush(col);
    g.DrawString(&ch, 1, &font, drawR, &sf, &brush);
}

// Win11 风格任务栏时钟：时间在上、日期在下两行，居中显示于时钟区域。
// 字体用圆润的 MiSans（回退 Segoe UI Variable / Segoe UI），
// 并强制灰度抗锯齿：分层窗口上 GDI+ 默认的 ClearType 会产生彩色描边。
void DrawClock(Graphics& g, AppState& s, const RectF& r) {
    const float k = s.scale;

    // 灰度抗锯齿，去掉文本彩色描边（默认 TextRenderingHint 是 ClearType）
    g.SetTextRenderingHint(TextRenderingHintAntiAlias);

    // 圆润字体选择：MiSans → Segoe UI Variable Text Semibold → Segoe UI Semibold。
    // GDI+ FontFamily 拷贝赋值是 private，用多个实例 + 指针选择。
    FontFamily famMiSans(L"MiSans");
    FontFamily famSegVar(L"Segoe UI Variable Text Semibold");
    FontFamily famSeg(L"Segoe UI Semibold");
    const FontFamily* fam = &famMiSans;
    if (famMiSans.GetLastStatus() != Ok) {
        fam = &famSegVar;
        if (famSegVar.GetLastStatus() != Ok) {
            fam = &famSeg;
        }
    }
    Gdiplus::Font font(fam, 12.0f * k, FontStyleRegular, UnitPixel);

    const float half = r.Height * 0.5f;
    RectF timeR(r.X, r.Y, r.Width, half);
    RectF dateR(r.X, r.Y + half, r.Width, half);

    StringFormat sf;
    sf.SetAlignment(StringAlignmentCenter);
    sf.SetLineAlignment(StringAlignmentCenter);
    sf.SetFormatFlags(StringFormatFlagsNoWrap);

    SolidBrush textBrush(Color(255, 255, 255, 255));
    if (!s.clockTimeText.empty()) {
        g.DrawString(s.clockTimeText.c_str(), -1, &font, timeR, &sf,
                     &textBrush);
    }
    if (!s.clockDateText.empty()) {
        g.DrawString(s.clockDateText.c_str(), -1, &font, dateR, &sf,
                     &textBrush);
    }
}

// 滑块上的音量百分比文本
void DrawVolumePercent(Graphics& g, AppState& s, const RectF& r, float value,
                       bool ready) {
    if (!ready) {
        return;
    }
    const float k = s.scale;
    FontFamily family(L"Segoe UI");
    Gdiplus::Font font(&family, 12.0f * k, FontStyleRegular, UnitPixel);

    wchar_t text[16] = {};
    swprintf_s(text, L"%d%%", static_cast<int>(value * 100.0f + 0.5f));

    StringFormat sf;
    sf.SetAlignment(StringAlignmentFar);
    sf.SetLineAlignment(StringAlignmentCenter);

    SolidBrush brush(Color(220, 255, 255, 255));
    g.DrawString(text, -1, &font, r, &sf, &brush);
}

// 向 GraphicsPath 添加一个圆角矩形路径
void AddRoundedRectPath(GraphicsPath& path, const RectF& r, float radius) {
    const float d = radius * 2.0f;
    path.AddArc(r.X, r.Y, d, d, 180.0f, 90.0f);
    path.AddArc(r.X + r.Width - d, r.Y, d, d, 270.0f, 90.0f);
    path.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0.0f, 90.0f);
    path.AddArc(r.X, r.Y + r.Height - d, d, d, 90.0f, 90.0f);
    path.CloseFigure();
}

void DrawBar(AppState& s) {
    Gdiplus::Bitmap* bmp = s.bitmap;
    if (!bmp) {
        return;
    }

    const float k = s.scale;
    Graphics g(bmp);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(PixelOffsetModeHighQuality);
    g.Clear(Color(0, 0, 0, 0)); // 全透明背景

    const float barH = static_cast<float>(BarAreaHeight(s));

    // 半透明深色顶栏（macOS 风格），跨全宽
    RectF barRect(0, 0, static_cast<float>(s.width), barH);
    SolidBrush barBrush(Color(150, 44, 48, 56));
    g.FillRectangle(&barBrush, barRect);

    // 底部细边框
    Pen bottomLine(Color(70, 255, 255, 255), 1.0f);
    g.DrawLine(&bottomLine,
               PointF(0.0f, barH - 0.5f),
               PointF(static_cast<float>(s.width), barH - 0.5f));

    // ---- 左上角音量按钮 ----
    RectF volR;
    VolumeButtonRect(s, volR);
    DrawButtonHover(g, volR, kHitVolume, s.hoverButton);
    DrawVolumeGlyph(g, volR, k,
                    s.hoverButton == kHitVolume ? Color(255, 255, 255, 255)
                                                : Color(200, 255, 255, 255),
                    s.volumeMuted);

    // ---- Chrome 风格标签：从音量键右侧到右侧三键之间 ----
    LayoutTabs(s, s.tabs, &g);
    RectF minR, maxR, closeR;
    ComputeButtonRects(s, minR, maxR, closeR);

    FontFamily tabFamily(L"Segoe UI");
    Gdiplus::Font tabFont(&tabFamily, 12.0f * k, FontStyleRegular, UnitPixel);

    // 标签是否处于激活状态（Chrome 同步模式看浏览器内激活态）
    const auto tabActive = [&s](const TabInfo& t) {
        return t.isChrome ? t.chromeActive : (t.hwnd == s.targetHwnd);
    };
    const bool chromeMode = ChromeSyncMode(s);

    // 未激活标签之间的竖线：高度略小于标签栏高度，上下留白相等
    for (size_t i = 0; i + 1 < s.tabs.size(); ++i) {
        const bool leftActive = tabActive(s.tabs[i]);
        const bool rightActive = tabActive(s.tabs[i + 1]);
        if (leftActive || rightActive) {
            continue;
        }
        const float sepX = s.tabs[i].rect.X + s.tabs[i].rect.Width;
        const float sepInset = kTabDividerInset * k;
        const float sepY0 = sepInset;
        const float sepY1 = barH - sepInset;
        if (sepY1 > sepY0) {
            Pen sepPen(Color(120, 255, 255, 255), 1.0f * k);
            g.DrawLine(&sepPen, sepX, sepY0, sepX, sepY1);
        }
    }

    for (size_t i = 0; i < s.tabs.size(); ++i) {
        const TabInfo& tab = s.tabs[i];
        const RectF& r = tab.rect;
        if (r.Width <= 0.0f || r.Height <= 0.0f) {
            continue;
        }

        const bool active = tabActive(tab);
        const bool hover = (s.hoverButton == kHitTab &&
                            s.hoverTab == static_cast<int>(i));

        if (active) {
            // 当前标签：两侧始终使用相同的下圆角样式，即使没有相邻未打开标签也不出现平直底部。
            const float inset = kTabTopInset * k;
            RectF visual(r.X, inset, r.Width, barH - inset);
            if (visual.Height > 0.0f) {
                GraphicsPath activePath;
                const float rTop = (std::min)(8.0f * k, visual.Width * 0.5f);
                const float rBot = (std::min)(8.0f * k, visual.Width * 0.25f);
                const float x0 = visual.X;
                const float y0 = visual.Y;
                const float x1 = visual.X + visual.Width;
                const float y1 = visual.Y + visual.Height;

                // 左上外凸圆角
                activePath.AddArc(x0, y0, rTop * 2.0f, rTop * 2.0f,
                                  180.0f, 90.0f);
                activePath.AddLine(x0 + rTop, y0, x1 - rTop, y0);
                // 右上外凸圆角
                activePath.AddArc(x1 - rTop * 2.0f, y0,
                                  rTop * 2.0f, rTop * 2.0f, 270.0f, 90.0f);
                // 右侧边框向下到右下圆角起点
                activePath.AddLine(x1, y0 + rTop, x1, y1 - rBot);
                // 右下圆角（与未打开标签共用的样式）
                {
                    RectF brBox(x1,
                                y1 - rBot * 2.0f,
                                rBot * 2.0f, rBot * 2.0f);
                    activePath.AddArc(brBox, 180.0f, -90.0f);
                }
                // 底边
                activePath.AddLine(x1 + rBot, y1, x0 - rBot, y1);
                // 左下圆角（与未打开标签共用的样式）
                {
                    RectF blBox(x0 - rBot * 2.0f,
                                y1 - rBot * 2.0f,
                                rBot * 2.0f, rBot * 2.0f);
                    activePath.AddArc(blBox, 90.0f, -90.0f);
                }
                activePath.CloseFigure();

                SolidBrush fillBrush(hover ? Color(80, 255, 255, 255)
                                           : Color(45, 255, 255, 255));
                g.FillPath(&fillBrush, &activePath);
                // 不绘制边框，只保留浅色背景填充
            }
        }

    }

    // 所有标签文字统一在所有背景绘制完之后再绘制，避免已打开标签延伸的背景盖住左侧未打开标签
    for (size_t i = 0; i < s.tabs.size(); ++i) {
        const TabInfo& tab = s.tabs[i];
        const RectF& r = tab.rect;
        if (r.Width <= 0.0f || r.Height <= 0.0f) {
            continue;
        }

        float textLeft = r.X + 8.0f * k;
        if (chromeMode && tab.chromePinned) {
            // 固定标签：左侧画小圆点，标题相应右移
            const float dotR = 2.6f * k;
            SolidBrush dotBrush(Color(220, 255, 255, 255));
            g.FillEllipse(&dotBrush, textLeft + 2.0f * k,
                          r.Y + r.Height * 0.5f - dotR, dotR * 2.0f,
                          dotR * 2.0f);
            textLeft += 10.0f * k;
        }
        // 所有标签标题使用相同的高度和垂直居中，避免已打开/未打开标题高度不一致
        const float textRight = r.X + r.Width - 8.0f * k;
        RectF textR = RectF(textLeft, r.Y,
                            (std::max)(textRight - textLeft, 1.0f),
                            r.Height);
        StringFormat tabSf;
        tabSf.SetFormatFlags(StringFormatFlagsNoWrap);  // 只显示一行，超出宽度直接截断
        tabSf.SetAlignment(StringAlignmentNear);  // 从左开始，超出部分在右侧截断
        tabSf.SetLineAlignment(StringAlignmentCenter);
        tabSf.SetTrimming(StringTrimmingNone);  // 不用省略号，按渲染宽度截断

        // 未打开标签与已打开标签使用相同亮度的文字颜色；不绘制末尾渐变
        const Color textCol(255, 255, 255, 255);
        SolidBrush textBrush(textCol);
        GraphicsState state = g.Save();
        g.SetClip(textR);
        g.DrawString(tab.title.c_str(), -1, &tabFont, textR, &tabSf, &textBrush);
        g.Restore(state);
    }

    // ---- 最小化键左侧：Win11 风格任务栏时钟（时间/日期两行）----
    RectF clockR;
    ComputeClockRect(s, clockR);
    DrawButtonHover(g, clockR, kHitClock, s.hoverButton);
    DrawClock(g, s, clockR);

    // ---- 右侧三个 Chrome 风格按钮 ----

    const Color glyph(200, 255, 255, 255);
    const Color glyphHover(255, 255, 255, 255);

    DrawButtonHover(g, minR, kHitMinimize, s.hoverButton);
    DrawMinimizeGlyph(g, minR, k,
                      s.hoverButton == kHitMinimize ? glyphHover : glyph);

    DrawButtonHover(g, maxR, kHitMaximize, s.hoverButton);
    DrawMaximizeGlyph(g, maxR, k, s.targetMaximized,
                      s.hoverButton == kHitMaximize ? glyphHover : glyph);

    DrawButtonHover(g, closeR, kHitClose, s.hoverButton);
    DrawCloseGlyph(g, closeR, k,
                   s.hoverButton == kHitClose ? glyphHover : glyph);
}

void PresentBar(AppState& s) {
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

void DrawBarAndPresent(AppState& s) {
    DrawBar(s);
    PresentBar(s);
}

// 顶栏固定：挂到 Progman 并置于桌面层底部，
// 与其它组件一样不覆盖任何普通窗口。固定位置由调用方给出.
bool AttachToDesktop(HWND hwnd, int screenX, int screenY,
                     int width, int height) {
    HWND hProgman = FindWindowW(L"Progman", nullptr);
    if (!hProgman) {
        return false;
    }

    SetWindowLongPtrW(hwnd, GWLP_HWNDPARENT,
                      reinterpret_cast<LONG_PTR>(hProgman));
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
    AppendMenuW(menu, MF_STRING, kMenuExit, L"退出 DesktopTopBar");

    SetForegroundWindow(hwnd);
    const UINT flags = TPM_RIGHTBUTTON | TPM_LEFTALIGN | TPM_RETURNCMD | TPM_NONOTIFY;
    const int cmd = TrackPopupMenu(menu, flags, pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(menu);

    if (cmd == kMenuExit) {
        DestroyWindow(hwnd);
    }
}

// 在 Win+D / 显示桌面之后，把顶栏重新贴回桌面层底部（防御性，怕被抬升）
void ReassertDesktopLayer(HWND hwnd) {
    HWND hProgman = FindWindowW(L"Progman", nullptr);
    if (hProgman) {
        SetWindowPos(hwnd, HWND_BOTTOM, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
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
        if (s) {
            s->hwnd = hwnd;
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);

    case WM_CREATE: {
        if (!s) {
            return -1;
        }
        s->hwnd = hwnd;
        s->dpi = GetWindowDpi(hwnd);
        s->scale = static_cast<float>(s->dpi) / 96.0f;
        InstallVolumeHook(hwnd); // 常驻全局钩子：显式点击/Alt+Tab 检测 + 面板外点击收起
        // 标签刷新不设常驻定时器：WinEvent 事件驱动（见 WinEventProc），
        // 仅挂起态（新建窗口/抢前台）启用 200ms 短轮询、静止时 10s 低频兜底
        ChromeSyncStart(hwnd);   // Chrome 标签同步服务端（扩展连接用）
        // Win11 风格时钟：跟随系统"显示秒"设置，每秒检查文本变化
        s->clockShowSeconds = ReadShowSecondsSetting();
        UpdateClock(*s);
        SetTimer(hwnd, kClockTimerId, kClockTickMs, nullptr);
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

    case WM_NCHITTEST:
        // 整条顶栏接收鼠标（顶栏在桌面层，点击不会覆盖/抢占任何窗口）
        return HTCLIENT;

    case WM_WINDOWPOSCHANGING: {
        // 防御：任何 Z 序变化都强制回到桌面层底部，防止覆盖普通窗口
        auto* wp = reinterpret_cast<WINDOWPOS*>(lParam);
        if ((wp->flags & SWP_NOZORDER) == 0 && wp->hwndInsertAfter != HWND_BOTTOM) {
            wp->hwndInsertAfter = HWND_BOTTOM;
            wp->flags |= SWP_NOACTIVATE;
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    case WM_MOUSEMOVE: {
        if (!s) {
            return 0;
        }

        // 拖动中：移动目标窗口（等同标题栏拖动）
        if (s->dragWindow && s->dragHwnd && IsWindow(s->dragHwnd)) {
            POINT cur{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ClientToScreen(hwnd, &cur);
            const int dx = cur.x - s->dragStartPt.x;
            const int dy = cur.y - s->dragStartPt.y;
            if (!s->dragMoving) {
                // 移动阈值：超过才真正开始拖动，避免误触
                const int threshold = (std::max)(4, MulDiv(4, s->dpi, 96));
                const int adx = dx < 0 ? -dx : dx;
                const int ady = dy < 0 ? -dy : dy;
                if (adx < threshold && ady < threshold) {
                    return 0;
                }
                s->dragMoving = true;
                RECT rc{};
                GetWindowRect(s->dragHwnd, &rc);
                if (IsZoomed(s->dragHwnd)) {
                    // 最大化窗口拖动：先还原，抓点按比例换算（同 Windows 标题栏）
                    ShowWindow(s->dragHwnd, SW_RESTORE);
                    RECT rc2{};
                    GetWindowRect(s->dragHwnd, &rc2);
                    const float rx =
                        rc.right > rc.left
                            ? static_cast<float>(s->dragStartPt.x - rc.left) /
                                  static_cast<float>(rc.right - rc.left)
                            : 0.5f;
                    const float ry =
                        rc.bottom > rc.top
                            ? static_cast<float>(s->dragStartPt.y - rc.top) /
                                  static_cast<float>(rc.bottom - rc.top)
                            : 0.5f;
                    s->dragOffset.x = static_cast<LONG>(
                        rx * static_cast<float>(rc2.right - rc2.left));
                    s->dragOffset.y = static_cast<LONG>(
                        ry * static_cast<float>(rc2.bottom - rc2.top));
                    s->targetMaximized = false;
                    DrawBarAndPresent(*s);  // 更新右侧最大化按钮字形
                } else {
                    s->dragOffset.x = s->dragStartPt.x - rc.left;
                    s->dragOffset.y = s->dragStartPt.y - rc.top;
                }
            }
            SetWindowPos(s->dragHwnd, nullptr, cur.x - s->dragOffset.x,
                         cur.y - s->dragOffset.y, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }

        if (!s->trackingMouse) {
            TRACKMOUSEEVENT tme{};
            tme.cbSize = sizeof(tme);
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = hwnd;
            TrackMouseEvent(&tme);
            s->trackingMouse = true;
        }
        POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        const int tabIndex = HitTestTab(*s, pt.x, pt.y);
        const ButtonHit hit = HitTestButton(*s, pt.x, pt.y);
        const int newHoverTab = (hit == kHitTab) ? tabIndex : -1;

        if (hit != s->hoverButton || newHoverTab != s->hoverTab) {
            s->hoverButton = hit;
            s->hoverTab = newHoverTab;
            DrawBarAndPresent(*s);
        }
        return 0;
    }

    case WM_CAPTURECHANGED:
        // 捕获被系统或其他窗口夺走时结束拖动，避免状态残留
        if (s && s->dragWindow) {
            s->dragWindow = false;
            s->dragMoving = false;
            s->dragHwnd = nullptr;
        }
        return 0;

    case WM_MOUSELEAVE:
        if (s) {
            s->trackingMouse = false;
            if (!s->volumeDragging &&
                (s->hoverButton != kHitNone || s->hoverTab != -1)) {
                s->hoverButton = kHitNone;
                s->hoverTab = -1;
                DrawBarAndPresent(*s);
            }
        }
        return 0;

    case WM_LBUTTONDOWN: {
        if (!s) {
            return 0;
        }
        POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        const int tabIndex = HitTestTab(*s, pt.x, pt.y);
        const ButtonHit hit = HitTestButton(*s, pt.x, pt.y);
        const bool chromeMode = ChromeSyncMode(*s);

        // 按住空白处：进入标题栏式拖动（移动阈值内只等待，不移动）
        if (hit == kHitNone && s->hasTarget && s->targetHwnd &&
            IsWindow(s->targetHwnd) && !IsIconic(s->targetHwnd)) {
            s->dragWindow = true;
            s->dragMoving = false;
            s->dragHwnd = s->targetHwnd;
            s->dragStartPt = pt;
            ClientToScreen(hwnd, &s->dragStartPt);
            SetCapture(hwnd);
        }

        switch (hit) {
        case kHitVolume:
            SetVolumeOpen(*s, !s->volumeOpen);
            break;
        case kHitClock:
            // 与 Win11 任务栏时钟一致：点击打开/收起系统日历浮出窗口
            ToggleCalendarFlyout();
            break;
        case kHitTab:
            if (tabIndex >= 0 && tabIndex < static_cast<int>(s->tabs.size())) {
                const TabInfo& tab = s->tabs[tabIndex];
                if (chromeMode && tab.isChrome) {
                    // Chrome 同步模式：点击标签 = 切换（关闭用中键）
                    ChromeSyncSendActivateTab(tab.chromeTabId);
                    break;
                }
                HWND tabHwnd = tab.hwnd;
                if (IsIconic(tabHwnd)) {
                    ShowWindow(tabHwnd, SW_RESTORE);
                }
                ForceForegroundWindow(tabHwnd);
                ApplyTargetInfo(*s, tabHwnd);
                s->targetSticky = false;
                DrawBarAndPresent(*s);
            }
            break;
        case kHitMinimize:
        case kHitMaximize:
        case kHitClose:
            if (s->hasTarget && s->targetHwnd) {
                HitButton(s->hwnd, s->targetHwnd, hit);
            }
            break;
        default:
            break;
        }
        return 0;
    }

    case WM_LBUTTONDBLCLK: {
        // 双击顶栏空白处：最大化/还原当前目标窗口（与标题栏双击行为一致）。
        // 双击落在按钮/标签上时不触发（第二次按下已由各自逻辑处理）。
        if (!s) {
            return 0;
        }
        POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        const ButtonHit hit = HitTestButton(*s, pt.x, pt.y);
        if (hit == kHitNone && s->hasTarget && s->targetHwnd &&
            IsWindow(s->targetHwnd)) {
            HitButton(s->hwnd, s->targetHwnd, kHitMaximize);
            // 刷新最大化状态（右侧按钮字形与后续布局）
            s->targetMaximized = IsZoomed(s->targetHwnd) != FALSE;
            DrawBarAndPresent(*s);
        }
        return 0;
    }

    case WM_LBUTTONUP: {
        if (!s) {
            return 0;
        }
        if (s->volumeDragging) {
            s->volumeDragging = false;
            ReleaseCapture();
            DrawBarAndPresent(*s);
        }
        if (s->dragWindow) {
            s->dragWindow = false;
            s->dragMoving = false;
            s->dragHwnd = nullptr;
            ReleaseCapture();
        }
        return 0;
    }

    case WM_MBUTTONDOWN: {
        // 中键点击标签：普通应用关闭对应窗口；Chrome 同步模式关闭对应标签
        if (!s) {
            return 0;
        }
        POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        const int tabIndex = HitTestTab(*s, pt.x, pt.y);
        if (tabIndex >= 0 && tabIndex < static_cast<int>(s->tabs.size())) {
            const TabInfo& tab = s->tabs[tabIndex];
            if (ChromeSyncMode(*s) && tab.isChrome) {
                ChromeSyncSendCloseTab(tab.chromeTabId);
            } else {
                PostMessageW(tab.hwnd, WM_CLOSE, 0, 0);
            }
        }
        return 0;
    }

    case WM_RBUTTONUP: {
        // 右键标签：普通应用在该标签右侧打开应用新窗口；
        // Chrome 同步模式 = 新建标签页；右键空白处仍显示退出菜单
        if (!s) {
            return 0;
        }
        POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        const int tabIndex = HitTestTab(*s, pt.x, pt.y);
        if (tabIndex >= 0 && tabIndex < static_cast<int>(s->tabs.size())) {
            const TabInfo& tab = s->tabs[tabIndex];
            if (ChromeSyncMode(*s) && tab.isChrome) {
                ChromeSyncSendNewTab();
            } else {
                OpenNewAppWindow(*s, tab.hwnd);
            }
        } else {
            ShowExitMenu(hwnd);
        }
        return 0;
    }

    case kChromeSyncMsg: {
        // 扩展推送的标签数据（JSON，lParam = new wchar_t[]，这里负责释放）
        if (!s) {
            if (lParam) {
                delete[] reinterpret_cast<wchar_t*>(lParam);
            }
            return 0;
        }
        const wchar_t* json = reinterpret_cast<const wchar_t*>(lParam);
        if (json) {
            const wsproto::JsonValue jsonValue = wsproto::JsonParse(json);
            if (wsproto::ChromeSyncApplyMessage(s->chromeSync, jsonValue)) {
                if (RefreshTabs(*s)) {
                    DrawBarAndPresent(*s);
                }
            }
        }
        delete[] reinterpret_cast<wchar_t*>(lParam);
        return 0;
    }

    case kChromeSyncStateMsg: {
        // 扩展连接 / 断开：连接后立即以现有数据刷新（首帧 hello 稍后到达），
        // 断开则清空同步数据并回退为窗口枚举
        if (!s) {
            return 0;
        }
        const bool connected = wParam != 0;
        if (s->chromeSync.connected != connected) {
            s->chromeSync.connected = connected;
            if (!connected) {
                s->chromeSync.tabs.clear();
                s->chromeSync.windowId = 0;
            }
            if (RefreshTabs(*s)) {
                DrawBarAndPresent(*s);
            }
        }
        return 0;
    }

    case kCloseVolumeMsg:
        // 全局鼠标钩子检测到点击面板外（桌面/其他窗口）时请求收起
        if (s && s->volumeOpen) {
            SetVolumeOpen(*s, false);
        }
        return 0;

    case kUserSwitchMsg:
        // 用户显式点击/Alt+Tab 其他窗口：立即解除粘住并切换目标
        if (s) {
            UpdateTarget(*s);
            DrawBarAndPresent(*s);
        }
        return 0;

    case WM_TIMER:
        if (!s) {
            return 0;
        }
        if (wParam == kTabRefreshTimerId) {
            // 挂起态短轮询：新建窗口待插入 / 抢前台重试
            if (RefreshTabs(*s)) {
                DrawBarAndPresent(*s);
            }
        } else if (wParam == kTargetRetryTimerId) {
            // 前台窗口创建中的延迟复查：窗口应已可见
            KillTimer(hwnd, kTargetRetryTimerId);
            UpdateTarget(*s);
            DrawBarAndPresent(*s);
            // 前台窗口仍在创建中（不可见）：继续复查，最多 kMaxTargetRetry 次
            const HWND fg = GetForegroundWindow();
            if (fg && IsWindow(fg) && !IsWindowVisible(fg) &&
                ++s->targetRetryCount < kMaxTargetRetry) {
                SetTimer(hwnd, kTargetRetryTimerId, 150, nullptr);
            }
        } else if (wParam == kTabRefreshDebounceTimerId) {
            // 事件驱动的标签刷新（抖动合并后）
            KillTimer(hwnd, kTabRefreshDebounceTimerId);
            if (RefreshTabs(*s)) {
                DrawBarAndPresent(*s);
            }
        } else if (wParam == kSlowRefreshTimerId) {
            // 低频兜底自检：事件驱动正常时被刷新重置，很少触发
            KillTimer(hwnd, kSlowRefreshTimerId);
            if (RefreshTabs(*s)) {
                DrawBarAndPresent(*s);
            }
        } else if (wParam == kClockTimerId) {
            // 时钟：每秒检查一次，仅时间/日期文本变化时重绘
            if (UpdateClock(*s)) {
                DrawBarAndPresent(*s);
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
        s->scale = static_cast<float>(s->dpi) / 96.0f;

        const int h = WindowHeight(*s);
        const int w = suggested ? (suggested->right - suggested->left) : s->width;
        const int x = suggested ? suggested->left : 0;
        const int y = suggested ? suggested->top : 0;

        ReassertDesktopLayer(hwnd);
        SetWindowPos(hwnd, HWND_BOTTOM, x, y, w, h,
                     SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        if (CreateBacking(*s, w, h)) {
            DrawBarAndPresent(*s);
        }
        return 0;
    }

    case WM_DESTROY:
        KillTimer(hwnd, kTabRefreshTimerId);
        KillTimer(hwnd, kTargetRetryTimerId);
        KillTimer(hwnd, kTabRefreshDebounceTimerId);
        KillTimer(hwnd, kSlowRefreshTimerId);
        KillTimer(hwnd, kClockTimerId);
        ChromeSyncStop();  // 先停网络线程（会向本窗口发状态消息，需在窗口销毁前）
        UninstallVolumeHook();
        if (s) {
            if (s->volumePanelHwnd) {
                DestroyWindow(s->volumePanelHwnd);
                s->volumePanelHwnd = nullptr;
            }
            DestroyPanelBacking(*s);
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

std::atomic<HWND> g_topbarHwnd{nullptr};

DWORD WINAPI TopbarThreadProc(LPVOID param) {
    const HINSTANCE hInstance = static_cast<HINSTANCE>(param);

    EnableDpiAwareness();

    // Core Audio（音量合成器）需要 COM；失败不致命，仅音量功能不可用
    HRESULT comInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    ULONG_PTR gdiplusToken = 0;
    GdiplusStartupInput gdiplusStartupInput;
    if (GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, nullptr) != Ok) {
        if (SUCCEEDED(comInit)) CoUninitialize();
        return 1;
    }

    HDC screenDc = GetDC(nullptr);
    const int systemDpi = screenDc ? GetDeviceCaps(screenDc, LOGPIXELSX) : 96;
    if (screenDc) {
        ReleaseDC(nullptr, screenDc);
    }

    // 顶栏固定在主工作区顶部，横跨全宽
    RECT workArea{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
    const int width = workArea.right - workArea.left;
    const int height = MulDiv(kBaseTabHeight, systemDpi, 96);
    const int posX = workArea.left + (workArea.right - workArea.left - width) / 2;
    const int posY = workArea.top;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;  // CS_DBLCLKS：双击空白处最大化
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kWindowClass;

    if (!RegisterClassExW(&wc)) {
        if (GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            // 组件在进程内被重启（关闭后再次打开）时类已注册，视为成功
            GdiplusShutdown(gdiplusToken);
            if (SUCCEEDED(comInit)) CoUninitialize();
            return 1;
        }
    }

    // Chrome 标签同步服务端需要 WinSock；失败只禁用该功能
    WSADATA wsaData{};
    const bool wsaOk = WSAStartup(MAKEWORD(2, 2), &wsaData) == 0;

    AppState state;
    HWND hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        kWindowClass,
        L"DesktopTopBar",
        WS_POPUP,
        posX, posY, width, height,
        nullptr, nullptr, hInstance, &state);

    if (!hwnd) {
        GdiplusShutdown(gdiplusToken);
        if (SUCCEEDED(comInit)) CoUninitialize();
        if (wsaOk) WSACleanup();
        return 1;
    }

    g_topbarHwnd.store(hwnd);  // 先发布窗口句柄，宿主可立即隐藏/关闭

    RECT initial{};
    GetWindowRect(hwnd, &initial);
    if (!AttachToDesktop(hwnd, initial.left, initial.top, width, height)) {
        // 找不到桌面窗口时退化为普通底层窗口
        SetWindowPos(hwnd, HWND_BOTTOM, initial.left, initial.top, width, height,
                     SWP_NOACTIVATE);
    }

    if (!CreateBacking(state, width, height)) {
        GdiplusShutdown(gdiplusToken);
        if (SUCCEEDED(comInit)) CoUninitialize();
        if (wsaOk) WSACleanup();
        g_topbarHwnd.store(nullptr);
        return 1;
    }

    UpdateTarget(state);
    DrawBarAndPresent(state);

    ShowWindow(hwnd, SW_SHOWNOACTIVATE);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    GdiplusShutdown(gdiplusToken);
    if (SUCCEEDED(comInit)) {
        CoUninitialize();
    }
    if (wsaOk) {
        WSACleanup();
    }
    g_topbarHwnd.store(nullptr);  // 窗口已销毁，线程即将退出
    return static_cast<DWORD>(msg.wParam);
}
