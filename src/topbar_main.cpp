// DesktopTopBar - 类 macOS 桌面顶栏（独立程序）
//
// 功能：
//   1. 固定在桌面顶部的一条半透明顶栏，外观与其他组件一致（GDI+ 逐像素透明）
//   2. 通过 Progman 属主 + HWND_BOTTOM 挂在桌面层，与其它三个组件一样
//      只展现在桌面上：不覆盖任何普通窗口，也不出现在任务栏/Alt-Tab 中
//   3. 顶栏居中显示"当前聚焦窗口"的名称（鼠标最后一次聚焦 / 前台窗口）
//   4. 顶栏右侧提供 Chrome 浏览器风格的 最小化 / 最大化 / 关闭 三个按钮，
//      用于控制当前聚焦窗口
//   5. 高度等于 Chrome 浏览器标签栏的高度（约 40px，随 DPI 缩放）

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

// Core Audio：按进程设置应用音量（类似 Windows 音量合成器）
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <tlhelp32.h>   // 进程快照：解析应用子进程的音频会话
#include <unordered_map>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "Mmdevapi.lib")

using namespace Gdiplus;
using Microsoft::WRL::ComPtr;

namespace {

constexpr wchar_t kWindowClass[] = L"DesktopTopBarWindow";
constexpr wchar_t kMutexName[] = L"Local\\DesktopTopBar_SingleInstance";

// Chrome 浏览器标签栏的高度（96 DPI 下约 40 像素），随 DPI 缩放
constexpr int kBaseTabHeight = 40;
// 每个窗口控制按钮的宽度（Chrome 风格，约等于标签栏高度）
constexpr int kButtonWidthBase = 46;
constexpr int kButtonGap = 4;

// 左上角音量按钮与展开面板
constexpr int kVolumeButtonW = 46;      // 音量按钮宽度（与窗口控制按钮一致，便于点击）
constexpr int kVolumePanelW = 260;      // 展开的音量面板宽度
constexpr int kVolumePanelH = 60;       // 展开的音量面板高度
constexpr int kVolumePanelMargin = 2;   // 按键/面板贴合左上角的小边距
constexpr int kMuteButtonW = 30;        // 面板内静音按钮宽度
constexpr UINT kVolumeApplyDelayMs = 40;

constexpr UINT kTrackTimerId = 1;        // 前台窗口轮询定时器
constexpr UINT kTrackIntervalMs = 250;
constexpr UINT kRedrawTimerId = 2;       // 定时重绘（用于按钮按下/最大化态刷新）
constexpr UINT kRedrawIntervalMs = 500;
constexpr int kMenuExit = 1001;

enum ButtonHit {
    kHitNone = -1,
    kHitVolume = 3,      // 左上角音量按钮（展开/收起）
    kHitMute = 4,        // 面板内静音开关
    kHitSlider = 5,      // 面板内音量滑条
    kHitMinimize = 0,
    kHitMaximize = 1,
    kHitClose = 2,
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
    wchar_t targetTitle[256] = {};
    bool targetMaximized = false;        // 目标当前是否处于最大化

    int hoverButton = kHitNone;          // 当前悬停的按钮
    bool trackingMouse = false;

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

// 轮询前台窗口，更新目标与标题。轮询而不是事件绑定，
// 是为了在 WS_EX_NOACTIVATE（点击不抢占焦点）的前提下，
// 稳定跟踪"鼠标最后一次聚焦"的窗口。
void UpdateTarget(AppState& s) {
    HWND fg = GetForegroundWindow();
    if (IsControlTarget(fg, s.hwnd)) {
        s.targetHwnd = fg;
        s.hasTarget = true;
        s.targetMaximized = IsZoomed(fg) != FALSE;

        const int got = GetWindowTextW(fg, s.targetTitle, 255);
        if (got <= 0) {
            // 无标题的窗口回退为类名
            wchar_t cls[64] = {};
            if (GetClassNameW(fg, cls, 63) > 0) {
                wcscpy_s(s.targetTitle, cls);
            } else {
                wcscpy_s(s.targetTitle, L"窗口");
            }
        }

        // 目标窗口变化时重新解析其进程的音频会话，供音量面板使用
        if (s.targetHwnd != s.volumeTargetHwnd) {
            s.volumeTargetHwnd = s.targetHwnd;
            ResolveVolumeSession(s);
        }
    } else {
        s.targetHwnd = nullptr;
        s.hasTarget = false;
        s.targetTitle[0] = L'\0';
        s.targetMaximized = false;
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
        if (IsZoomed(target)) {
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

// ---- 全局鼠标钩子：点击面板外任意处（含桌面/其他窗口）时收起音量面板 ----
constexpr UINT kCloseVolumeMsg = WM_APP + 2;
HHOOK g_volumeHook = nullptr;
HWND g_volumeHookHwnd = nullptr;

LRESULT CALLBACK VolumeLowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION && wParam == WM_LBUTTONDOWN) {
        const auto* info = reinterpret_cast<const MSLLHOOKSTRUCT*>(lParam);
        HWND hwnd = g_volumeHookHwnd;
        AppState* s = hwnd ? reinterpret_cast<AppState*>(
            GetWindowLongPtrW(hwnd, GWLP_USERDATA)) : nullptr;
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

void InstallVolumeHook(HWND hwnd) {
    if (g_volumeHook) {
        return;
    }
    g_volumeHookHwnd = hwnd;
    g_volumeHook = SetWindowsHookExW(WH_MOUSE_LL, VolumeLowLevelMouseProc,
                                     GetModuleHandleW(nullptr), 0);
}

void UninstallVolumeHook() {
    if (g_volumeHook) {
        UnhookWindowsHookEx(g_volumeHook);
        g_volumeHook = nullptr;
    }
    g_volumeHookHwnd = nullptr;
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
        InstallVolumeHook(s.hwnd);
        ShowVolumePanel(s);
    } else {
        UninstallVolumeHook();
        HideVolumePanel(s);
    }
}

// ---- 绘制 ----

// 绘制单个按钮的悬停/点击高亮背景。
void DrawButtonHover(Graphics& g, const RectF& r, ButtonHit hit, int hover) {
    if (hover != hit) {
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

    // ---- 居中显示聚焦窗口名称 ----
    // 左右各预留对称空间（以两侧控件区较宽者为准），保证标题真正居中
    if (s.targetTitle[0] != L'\0') {
        FontFamily titleFamily(L"Segoe UI");
        Gdiplus::Font titleFont(&titleFamily, 14.0f * k, FontStyleRegular, UnitPixel);

        const float leftBlock = volR.X + volR.Width + 6.0f * k;
        const float rightBlock = 3.0f * kButtonWidthBase * k + 6.0f * k;
        const float reserve = (std::max)(leftBlock, rightBlock);
        RectF textRect(reserve, 0,
                       (std::max)(static_cast<float>(s.width) - 2.0f * reserve, 1.0f),
                       barH);
        StringFormat sf;
        sf.SetAlignment(StringAlignmentCenter);
        sf.SetLineAlignment(StringAlignmentCenter);
        sf.SetTrimming(StringTrimmingEllipsisCharacter);

        SolidBrush shadowBrush(Color(70, 0, 0, 0));
        RectF shadowRect(textRect.X, textRect.Y + 1.0f, textRect.Width, textRect.Height);
        g.DrawString(s.targetTitle, -1, &titleFont, shadowRect, &sf, &shadowBrush);

        SolidBrush textBrush(s.hasTarget ? Color(235, 255, 255, 255)
                                         : Color(110, 255, 255, 255));
        g.DrawString(s.targetTitle, -1, &titleFont, textRect, &sf, &textBrush);
    }

    // ---- 右侧三个 Chrome 风格按钮 ----
    RectF minR, maxR, closeR;
    ComputeButtonRects(s, minR, maxR, closeR);

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
        if (!s->trackingMouse) {
            TRACKMOUSEEVENT tme{};
            tme.cbSize = sizeof(tme);
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = hwnd;
            TrackMouseEvent(&tme);
            s->trackingMouse = true;
        }
        POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        const ButtonHit hit = HitTestButton(*s, pt.x, pt.y);
        if (hit != s->hoverButton) {
            s->hoverButton = hit;
            DrawBarAndPresent(*s);
        }
        return 0;
    }

    case WM_MOUSELEAVE:
        if (s) {
            s->trackingMouse = false;
            if (!s->volumeDragging && s->hoverButton != kHitNone) {
                s->hoverButton = kHitNone;
                DrawBarAndPresent(*s);
            }
        }
        return 0;

    case WM_LBUTTONDOWN: {
        if (!s) {
            return 0;
        }
        POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        const ButtonHit hit = HitTestButton(*s, pt.x, pt.y);

        switch (hit) {
        case kHitVolume:
            SetVolumeOpen(*s, !s->volumeOpen);
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

    case WM_LBUTTONUP: {
        if (!s) {
            return 0;
        }
        if (s->volumeDragging) {
            s->volumeDragging = false;
            ReleaseCapture();
            DrawBarAndPresent(*s);
        }
        return 0;
    }

    case WM_RBUTTONUP:
        ShowExitMenu(hwnd);
        return 0;

    case kCloseVolumeMsg:
        // 全局鼠标钩子检测到点击面板外（桌面/其他窗口）时请求收起
        if (s && s->volumeOpen) {
            SetVolumeOpen(*s, false);
        }
        return 0;

    case WM_TIMER:
        if (!s) {
            return 0;
        }
        if (wParam == kTrackTimerId) {
            UpdateTarget(*s);
            DrawBarAndPresent(*s);
        } else if (wParam == kRedrawTimerId) {
            // 定期刷新，及时反映最大化/还原后的字形变化
            if (s->targetHwnd && s->hasTarget && IsWindow(s->targetHwnd)) {
                const bool zoomed = IsZoomed(s->targetHwnd) != FALSE;
                if (zoomed != s->targetMaximized) {
                    s->targetMaximized = zoomed;
                    DrawBarAndPresent(*s);
                }
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
        KillTimer(hwnd, kTrackTimerId);
        KillTimer(hwnd, kRedrawTimerId);
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

int APIENTRY WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int) {
    // 只允许运行一个实例
    HANDLE mutex = CreateMutexW(nullptr, TRUE, kMutexName);
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(mutex);
        return 0;
    }

    EnableDpiAwareness();

    // Core Audio（音量合成器）需要 COM；失败不致命，仅音量功能不可用
    HRESULT comInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    ULONG_PTR gdiplusToken = 0;
    GdiplusStartupInput gdiplusStartupInput;
    if (GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, nullptr) != Ok) {
        MessageBoxW(nullptr, L"GDI+ 初始化失败。", L"DesktopTopBar",
                    MB_OK | MB_ICONERROR);
        CloseHandle(mutex);
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
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kWindowClass;

    if (!RegisterClassExW(&wc)) {
        MessageBoxW(nullptr, L"注册窗口类失败。", L"DesktopTopBar",
                    MB_OK | MB_ICONERROR);
        GdiplusShutdown(gdiplusToken);
        CloseHandle(mutex);
        return 1;
    }

    AppState state;
    HWND hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        kWindowClass,
        L"DesktopTopBar",
        WS_POPUP,
        posX, posY, width, height,
        nullptr, nullptr, hInstance, &state);

    if (!hwnd) {
        MessageBoxW(nullptr, L"创建顶栏窗口失败。", L"DesktopTopBar",
                    MB_OK | MB_ICONERROR);
        GdiplusShutdown(gdiplusToken);
        CloseHandle(mutex);
        return 1;
    }

    RECT initial{};
    GetWindowRect(hwnd, &initial);
    if (!AttachToDesktop(hwnd, initial.left, initial.top, width, height)) {
        // 找不到桌面窗口时退化为普通底层窗口
        SetWindowPos(hwnd, HWND_BOTTOM, initial.left, initial.top, width, height,
                     SWP_NOACTIVATE);
    }

    if (!CreateBacking(state, width, height)) {
        MessageBoxW(hwnd, L"创建绘图缓冲失败。", L"DesktopTopBar",
                    MB_OK | MB_ICONERROR);
        CloseHandle(mutex);
        return 1;
    }

    SetTimer(hwnd, kTrackTimerId, kTrackIntervalMs, nullptr);
    SetTimer(hwnd, kRedrawTimerId, kRedrawIntervalMs, nullptr);
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
    CloseHandle(mutex);
    return static_cast<int>(msg.wParam);
}
