// DesktopDock - 类 macOS 26 Dock 栏（独立程序，与桌面套件其他组件风格一致）
//
// 功能：
//   1. 屏幕底部居中的一条毛玻璃质感底栏（GDI+ 逐像素透明 + 圆角），
//      悬停时图标以 macOS 风格放大（高斯衰减影响邻近图标，弹性动画）
//   2. 左侧为「固定」应用：支持拖入 .exe/.lnk 固定、右键取消固定，
//      顺序持久化到注册表 HKCU\Software\DesktopSuite\Dock
//   3. 右侧（分隔线之后）为「正在运行」的应用：
//      - 顶层窗口按进程 exe 分组（UWP 窗口穿透 ApplicationFrameHost 取真实宿主）
//   4. 特别要求：托盘图标中的第三方应用视同打开状态 ——
//      Win11 新任务栏的托盘已 XAML 化（Shell_TrayWnd 下已无 ToolbarWindow32，
//      也无 NotifyIconOverflowWindow），托盘归属无法再用 TB_GETBUTTON/TRAYDATA
//      读出。因此改为读取注册表 HKCU\Control Panel\NotifyIconSettings：
//      系统把每个登记过的托盘图标的 ExecutablePath 持久化在此。与当前存活进程
//      列表求交集：已登记托盘 ∩ 进程存活 − Windows 自带组件 − 本套件自身，
//      即「现在正以托盘形式驻留的第三方程序」。这类程序即使没有任何可见窗口，
//      也出现在 Dock 运行区并带运行圆点；纯后台静默项可右键隐藏。
//   5. 点击行为：应用未在前台时 打开该应用的全部主窗口（最小化的一次性恢复），
//      前台时 最小化全部；纯托盘驻留（隐藏 Qt 主窗等）仍走托盘图标触发或启动；
//      启动时有 macOS 风格弹跳动画直到应用出现；中键关闭该应用全部窗口
//
// 日志写入 ..\logs\dock.log

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
#include <shellapi.h>
#include <shlobj.h>
#include <commctrl.h>
#include <commoncontrols.h>  // IImageList 完整定义（Jumbo 图标列表）
#include <propsys.h>         // System.Link.TargetParsingPath（广告式 lnk 解析）
#include <dwmapi.h>
#include <UIAutomationClient.h>  // Win11 XAML 托盘图标触发（UIA Invoke）
#include <tlhelp32.h>
#include <objidl.h>  // GDI+ 需要 IStream 等 COM 类型，先于 gdiplus.h 包含
#include <gdiplus.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <mutex>    // std::once_flag：托盘注册表监听线程每进程仅建一次
#include <new>      // 组件重启时 placement-new 重建 g_state
#include <share.h>   // _SH_DENYNO：日志允许外部同时读取
#include <string>
#include <thread>    // 关闭应用的后台等待线程（UI 不阻塞）
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "widgets.h"

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "version.lib")   // 文件描述 → 显示名
#pragma comment(lib, "propsys.lib")   // lnk 属性存储解析
#pragma comment(lib, "dwmapi.lib")    // DWMWA_CLOAKED 幽灵窗口判断

using namespace Gdiplus;

namespace {

constexpr wchar_t kWindowClass[] = L"DesktopDockWindow";
constexpr wchar_t kRegPath[] = L"Software\\DesktopSuite\\Dock";
constexpr wchar_t kNotifyIconsKey[] = L"Control Panel\\NotifyIconSettings";

// ---- 尺寸常量均为 96 DPI 基准，使用处统一乘 scale ----
// 紧凑基准：图标几乎贴住玻璃边缘（macOS 式紧凑），总高约 57px
constexpr float kIconSize = 44.0f;      // 图标基准边长
constexpr float kIconGapHalf = 3.5f;    // 相邻图标半间隙（相邻推移随放大自然发生）
constexpr float kMaxScale = 1.66f;      // 悬停峰值缩放倍数
constexpr float kMagnifySigma = 48.0f;  // 高斯放大衰减 σ
constexpr float kBarPadX = 7.0f;        // 毛玻璃内左右留白（贴边）
constexpr float kPadTop = 5.0f;         // 图标上方留白（悬停放大的增量只加在顶部）
constexpr float kPadBottom = 8.0f;      // 图标下方留白（含运行圆点区）
constexpr float kDotDrop = 4.0f;        // 圆点中心距图标底边（须 < kPadBottom-半径）
constexpr float kShadowMargin = 14.0f;  // 窗口边缘阴影预留（顶/左/右）
constexpr float kShadowBottom = 7.0f;   // 底部阴影预留（贴底外观，为顶部的一半）
constexpr float kCornerRadius = 20.0f;
constexpr float kSepGap = 14.0f;        // 分隔线两侧额外间距
constexpr int kBottomGapBase = 0;       // 底栏距工作区底部（0=贴底；悬停放大向上生高）

// 悬停放大时窗口宽度随内容实时展宽（经 UpdateLayeredWindow 的
// pptDst/psize 每帧应用，静止时精确贴合内容）。缓冲位图额外预留的
// 放大宽度预算，超出时才重建 DIB（正常情况永不触发）：
constexpr float kMagWidthBudget = 170.0f;

// ---- 节奏 ----
constexpr UINT_PTR kFrameTimerId = 1;
constexpr UINT kFrameIntervalMs = 15;          // 动画帧周期（悬停/动画期间）
constexpr UINT_PTR kRefreshTimerId = 2;        // 事件合并一次性定时器
constexpr UINT_PTR kSafetyTimerId = 3;         // 事件失效保底（1 次/分钟）
constexpr ULONGLONG kRefreshMinGapMs = 1000;   // 刷新最小间隔（合并窗口事件风暴）
constexpr ULONGLONG kSafetyPollMs = 60000;     // 事件失效保底轮询（功耗≈0）
constexpr ULONGLONG kRunningPathsCacheMs = 3000;  // 运行路径/托盘注册表缓存（进程级枚举较贵）
constexpr ULONGLONG kTooltipDelayMs = 480;     // 悬停出提示延迟
constexpr ULONGLONG kLaunchBounceMs = 3600;    // 启动弹跳超时
constexpr ULONGLONG kTrayTriggerWindowMs = 2200;  // 托盘图标触发：任务栏临时可见窗口
constexpr int kMenuBaseId = 4000;
constexpr int kMenuExit = 1001;
constexpr int kMenuToggleAutoCollapse = 1002;  // 空白区右键：自动收起开关
constexpr int kDockStripHeightLogical = 2;     // 展开触发条高度（逻辑像素，与 Dock 同宽）
constexpr UINT_PTR kTooltipTimerId = 4;        // 悬停提示一次性定时器（停帧模式下到期踢帧）

// 事件驱动消息（WinEvent 回调/注册表监听线程 → 主窗口）
constexpr UINT kMsgRefresh = WM_APP + 1;   // 窗口集合/托盘状态可能变化，合并刷新
constexpr UINT kMsgHeal = WM_APP + 2;      // 套件组件被最小化，立即恢复
constexpr UINT kMsgTriggerDone = WM_APP + 3;  // 托盘触发完成（worker → 主窗口收尾）
constexpr UINT kMsgHookClick = WM_APP + 4; // 低层鼠标钩子采集的点击 → 主窗口线程执行
constexpr UINT kMsgHookMouse = WM_APP + 5; // 低层鼠标钩子：进入/离开 Dock 状态切换 → 主窗口线程

// 低层鼠标钩子采集的点击类型（wParam）
constexpr WPARAM kHookClickDockLeft = 1;      // Dock 图标左键（切换/打开）
constexpr WPARAM kHookClickDockMiddle = 2;    // Dock 图标中键（关闭应用）
constexpr WPARAM kHookClickStart = 3;         // 左下角开始按钮
constexpr WPARAM kHookClickShowDesktop = 4;   // 右下角显示桌面

// 启动台拖入固定（WM_COPYDATA 自定协议）：“DOCK” 魔数，lpData = UTF-16 路径
constexpr DWORD kDesktopDockPinMagic =
    static_cast<DWORD>('D') | (static_cast<DWORD>('O') << 8) |
    (static_cast<DWORD>('C') << 16) | (static_cast<DWORD>('K') << 24);

// 自家套件组件（顶栏/挂件等同样注册了托盘图标）永不进入 Dock。
// 注意 DesktopLauncher（启动台）不在此列：它是用户可固定的正常应用
const wchar_t* const kSelfExclusions[] = {
    L"desktopdock-x64.exe", L"desktoptopbar-x64.exe",
    L"mywigets-x64.exe", L"desktopclock-x64.exe",
    L"desktopcalendar-x64.exe",
};

// 运行库基础设施（可能代持托盘图标 / 并非用户意义上的“应用”）
const wchar_t* const kInfraBasenames[] = {
    L"explorer.exe", L"svchost.exe", L"conhost.exe", L"dllhost.exe",
    L"sihost.exe", L"ctfmon.exe", L"taskhostw.exe", L"runtimebroker.exe",
    L"searchhost.exe", L"searchapp.exe", L"shellexperiencehost.exe",
    L"startmenuexperiencehost.exe", L"textinputhost.exe", L"widgets.exe",
    L"widgetservice.exe", L"msiexec.exe", L"applicationframehost.exe",
};

// ---- “UI 宿主辅助进程”映射 ----
// 部分应用的可见主窗口由辅助进程承载（现代 Steam 客户端主窗口由
// steamwebhelper.exe(CEF) 承载、steam.exe 只做托盘/后台驻留）。Windows
// 任务栏按 AppUserModelID 把两者归并为一个按钮；Dock 若按进程 exe 路径
// 分组会把同一应用拆成“Steam 托盘”+“Steam Client WebHelper 窗口”两条，
// 且托盘那条无窗口可唤起。此处把辅助进程的窗口沿父进程链归并到宿主
// 应用 exe（helperBase → appBase，均小写）：窗口分组与关闭/唤起统一生效。
struct UiHostMapping {
    const wchar_t* helperBase;  // 承载 UI 的辅助进程基名（小写）
    const wchar_t* appBase;     // 宿主应用进程基名（小写）
};
constexpr UiHostMapping kUiHostMappings[] = {
    {L"steamwebhelper.exe", L"steam.exe"},
};

const UiHostMapping* FindUiHostMapping(const std::wstring& baseLower) {
    for (const auto& m : kUiHostMappings) {
        if (baseLower == m.helperBase) return &m;
    }
    return nullptr;
}

enum class ItemAction {
    None,
    ToggleFocus,
    SwitchToWindow,
    Launch,
    TogglePin,
    HideTrayItem,
    CloseAllWindows,
    ShowInExplorer,
};

struct MenuEntry {
    ItemAction action = ItemAction::None;
    size_t itemIndex = static_cast<size_t>(-1);
    HWND window = nullptr;
};

struct DockItem {
    std::wstring key;           // 归一化小写完整路径（分组主键）
    std::wstring launchPath;    // 启动用路径（固定的 .lnk 或原始 exe）
    std::wstring displayName;
    bool pinned = false;
    bool hasWindow = false;     // 有顶层窗口 → 打开
    bool trayMarked = false;    // 托盘第三方驻留 → 视同打开（特别需求）
    std::vector<HWND> windows;
    Bitmap* icon = nullptr;     // 引用 iconCache 所有权，不负责释放
    float scaleAnim = 1.0f;     // 当前渲染缩放（向目标缓动）
};

struct PendingLaunch {
    std::wstring key;
    ULONGLONG startTick = 0;
};

struct GeomSlot {
    RectF hit{};   // 命中区（整列高度）
    RectF icon{};  // 图标本帧实际绘制矩形（未含弹跳偏移）
    float cx = 0;  // 图标中心（下一帧高斯放大参考）
};

struct AppState {
    HWND hwnd = nullptr;
    int dpi = 96;
    float scale = 1.0f;
    int winW = 0;          // 当前呈现宽度（悬停放大会实时展宽）
    int winH = 0;          // 窗口高度（仅结构变化时变）
    int bufferW = 0;       // 后备位图宽度（≥ winW）
    int winX = 0;          // 当前窗口左上角（每帧由布局计算，ULW 应用）
    int winY = 0;
    int lastIdleW = 0;     // 上次静止宽度（检测结构变化用）

    HDC memDc = nullptr;
    HBITMAP dib = nullptr;
    HBITMAP dibOld = nullptr;
    void* bits = nullptr;
    Bitmap* surface = nullptr;

    std::vector<DockItem> items;
    std::vector<GeomSlot> geoms;
    std::vector<float> prevCenters;   // 上一帧图标中心（高斯参考）
    size_t pinCount = 0;              // 固定区数量（决定分隔线）
    float separatorX = -1.0f;         // 分隔线中心 x（客户坐标；<0 不画）
    std::unordered_map<std::wstring, Bitmap*> iconCache;
    std::unordered_map<std::wstring, size_t> orderMap;  // 出现顺序稳定化
    size_t nextOrdinal = 0;

    std::vector<std::wstring> pins;
    std::unordered_set<std::wstring> hiddenKeys;
    std::unordered_set<std::wstring> closingKeys;  // 中键关闭中：立即隐藏运行圆点
    int bottomGapBase = kBottomGapBase;
    RECT savedWorkArea{};  // 任务栏存在时的工作区（恢复任务栏时还原）

    float mouseX = -100000.0f;
    float mouseY = -100000.0f;
    bool mouseInside = false;
    bool trackingMouse = false;
    size_t hoverIndex = static_cast<size_t>(-1);
    ULONGLONG hoverSince = 0;

    std::vector<PendingLaunch> pendingLaunches;
    int frameIntervalMs = kFrameIntervalMs;            // 当前定时器周期（0=停止）
    ULONGLONG lastPollTick = 0;                        // 上次全量轮询时刻
    bool refreshArmed = false;                         // 事件合并定时器已挂起
    UINT taskbarCreatedMsg = 0;                        // TaskbarCreated 广播（explorer 重启）
    std::unordered_set<std::wstring> runningExeCache;  // 进程路径集合缓存（省全进程枚举）
    ULONGLONG runningExeCacheAt = 0;
    bool needsRedraw = true;
    bool autoCollapse = true;     // 自动收起开关（右键菜单，注册表持久化）
    bool hideRequested = false;   // 收起目标态（光标离开 Dock / 触碰下缘触发条）
    float collapseOffset = 0;     // 当前收起偏移（0=展开，winH=完全滑出屏幕底）
    std::atomic<bool> menuOpen{false};  // 右键菜单打开期间不收起（低层钩子线程也会读取）
    bool mouseOverDock = false;   // 悬停真值（LL 钩子按光标物理位置维护）
    bool wasOnDock = false;       // 钩子：上一拍光标是否在有效交互区（状态迁移）
    Font* uiFont = nullptr;

    MenuEntry menu[512];
    int menuCount = 0;

    uint64_t lastSignature = 0;  // 内容签名（FNV-1a），状态无变化时跳过重绘

    ULONGLONG taskbarShowUntil = 0;  // 托盘图标触发期间：任务栏临时可见的截止时刻
};

AppState g_state;

// ============================== 字符串 / 路径工具 ==============================

std::wstring ToLower(std::wstring s) {
    for (auto& ch : s) ch = static_cast<wchar_t>(towlower(ch));
    return s;
}

std::wstring NormalizePath(const std::wstring& in) {
    std::wstring out;
    out.reserve(in.size());
    bool started = false;
    for (wchar_t ch : in) {
        if (!started && ch == L'"') continue;  // 个别注册值带引号包裹，剥离
        started = true;
        ch = static_cast<wchar_t>(towlower(ch));
        out.push_back(ch == L'/' ? L'\\' : ch);
    }
    if (out.compare(0, 4, L"\\\\?\\") == 0) out.erase(0, 4);
    return out;
}

std::wstring PathDir(const std::wstring& path) {
    const size_t pos = path.find_last_of(L"\\/");
    return pos == std::wstring::npos ? std::wstring() : path.substr(0, pos);
}

std::wstring StripExtension(std::wstring name) {
    const size_t dot = name.rfind(L'.');
    if (dot != std::wstring::npos && dot > 0) name.resize(dot);
    return name;
}

std::wstring PathBasename(const std::wstring& path) {
    const size_t pos = path.find_last_of(L"\\/");
    return pos == std::wstring::npos ? path : path.substr(pos + 1);
}

std::wstring ExeDisplayName(const std::wstring& fullPathLower) {
    const DWORD size = GetFileVersionInfoSizeW(fullPathLower.c_str(), nullptr);
    if (size == 0) return L"";
    std::vector<BYTE> buf(size);
    if (!GetFileVersionInfoW(fullPathLower.c_str(), 0, size, buf.data())) {
        return L"";
    }
    struct LangCp {
        WORD lang;
        WORD cp;
    }* lcp = nullptr;
    UINT bytes = 0;
    if (!VerQueryValueW(buf.data(), L"\\VarFileInfo\\Translation",
                        reinterpret_cast<void**>(&lcp), &bytes) ||
        !lcp || bytes < sizeof(LangCp)) {
        return L"";
    }
    wchar_t sub[128] = {};
    swprintf_s(sub, L"\\StringFileInfo\\%04x%04x\\FileDescription",
               lcp[0].lang, lcp[0].cp);
    wchar_t* desc = nullptr;
    UINT descBytes = 0;
    if (VerQueryValueW(buf.data(), sub, reinterpret_cast<void**>(&desc),
                       &descBytes) &&
        desc && descBytes > 2 && desc[0]) {
        return desc;
    }
    return L"";
}

// ============================== 日志 ==============================
// main 分支禁止日志：kLogEnabled=false 时 LogInit 不建目录不开文件、
// Logf 直接返回（release 行为，零 I/O）。debug 分支保持 true（完整日志）。
constexpr bool kLogEnabled = false;

FILE* g_logFile = nullptr;
SRWLOCK g_logLock = SRWLOCK_INIT;  // 后台关闭线程与主线程可能同时写日志

void Logf(const wchar_t* fmt, ...) {
    if (!kLogEnabled || !g_logFile) return;
    AcquireSRWLockExclusive(&g_logLock);
    SYSTEMTIME st{};
    GetLocalTime(&st);
    fwprintf(g_logFile, L"[%02u:%02u:%02u.%03u] ",
             st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list args;
    va_start(args, fmt);
    vfwprintf(g_logFile, fmt, args);
    va_end(args);
    fputwc(L'\n', g_logFile);
    fflush(g_logFile);
    ReleaseSRWLockExclusive(&g_logLock);
}

void LogInit() {
    if (!kLogEnabled) return;  // main 分支：不创建 logs 目录、不写日志
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const std::wstring binDir = PathDir(exe);          // ...\bin
    const std::wstring repoDir = PathDir(binDir);      // ...\clock
    CreateDirectoryW((repoDir + L"\\logs").c_str(), nullptr);
    const std::wstring path = repoDir + L"\\logs\\dock.log";
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    bool truncate = false;
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa) &&
        (fa.nFileSizeHigh > 0 || fa.nFileSizeLow > 512 * 1024)) {
        truncate = true;
    }
    // 共享读写打开，外部 tail/编辑器可同时查看；
    // ccs=UTF-8：宽字符 Logf 直接落盘为 UTF-8，避免 MBCS 转换丢字
    g_logFile = _wfsopen(path.c_str(),
                         truncate ? L"w, ccs=UTF-8" : L"a, ccs=UTF-8",
                         _SH_DENYNO);
    if (g_logFile) {
        SYSTEMTIME st{};
        GetLocalTime(&st);
        Logf(L"---- DesktopDock 启动 (%04u-%02u-%02u) ----", st.wYear,
             st.wMonth, st.wDay);
    }
}

void LogClose() {
    if (!kLogEnabled || !g_logFile) return;
    Logf(L"---- DesktopDock 退出 ----");
    fclose(g_logFile);
    g_logFile = nullptr;
}

// ============================== 第三方判定 ==============================

bool StartsWithDir(const std::wstring& path, const std::wstring& dir) {
    if (dir.empty() || path.size() < dir.size()) return false;
    return path.compare(0, dir.size(), dir) == 0;
}

// NotifyIconSettings 的 ExecutablePath 可能是 "{GUID}\相对路径"
// （如 {F38BF404-...}\explorer.exe 即 %SystemRoot%\explorer.exe），先展开
std::wstring ExpandKnownFolderPrefix(const std::wstring& raw) {
    if (raw.empty() || raw.front() != L'{') return raw;
    const size_t braceEnd = raw.find(L'}');
    if (braceEnd == std::wstring::npos || braceEnd + 1 >= raw.size() ||
        raw[braceEnd + 1] != L'\\') {
        return raw;
    }
    CLSID clsid{};
    if (FAILED(CLSIDFromString(raw.substr(0, braceEnd + 1).c_str(),
                               &clsid))) {
        return raw;
    }
    PWSTR known = nullptr;
    if (FAILED(SHGetKnownFolderPath(clsid, KF_FLAG_DEFAULT, nullptr,
                                    &known))) {
        return raw;
    }
    std::wstring result = known;
    result += raw.substr(braceEnd + 1);
    CoTaskMemFree(known);
    return result;
}

// “Windows 自带组件”过滤。注意「第三方」以系统组件为界 ——
// 商店里第三方厂商的包仍算第三方。
bool IsSystemComponentPath(const std::wstring& p) {
    wchar_t windir[MAX_PATH] = {};
    GetWindowsDirectoryW(windir, MAX_PATH);
    const std::wstring winDir = NormalizePath(windir);
    if (StartsWithDir(p, winDir + L"\\")) return true;

    wchar_t progdata[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"ProgramData", progdata, MAX_PATH) > 0) {
        const std::wstring pd = NormalizePath(progdata);
        if (!pd.empty() && StartsWithDir(p, pd + L"\\") &&
            p.find(L"\\microsoft\\") != std::wstring::npos) {
            return true;
        }
    }
    if (p.find(L"\\windowsapps\\microsoft.") != std::wstring::npos) {
        return true;
    }
    if (p.find(L"\\microsoft\\") != std::wstring::npos) return true;
    return false;
}

bool IsInfraBaseline(const std::wstring& baseLower) {
    for (const wchar_t* name : kInfraBasenames) {
        if (baseLower == name) return true;
    }
    return false;
}

bool IsSelfSuiteBinary(const std::wstring& baseLower) {
    for (const wchar_t* name : kSelfExclusions) {
        if (baseLower == name) return true;
    }
    return false;
}

bool BasenameBlocked(const std::wstring& keyLower) {
    return IsInfraBaseline(PathBasename(keyLower)) ||
           IsSelfSuiteBinary(PathBasename(keyLower));
}

// ============================== 注册表配置 ==============================

std::wstring ReadRegString(HKEY key, const wchar_t* name) {
    DWORD type = 0;
    BYTE buf[4096];
    DWORD bytes = sizeof(buf) - sizeof(wchar_t);
    if (RegQueryValueExW(key, name, nullptr, &type, buf, &bytes) ==
            ERROR_SUCCESS &&
        type == REG_SZ && bytes >= sizeof(wchar_t)) {
        const size_t avail = bytes / sizeof(wchar_t);   // 可能不含终止符
        buf[avail * sizeof(wchar_t)] = 0;
        buf[avail * sizeof(wchar_t) + 1] = 0;
        auto* str = reinterpret_cast<const wchar_t*>(buf);
        size_t len = wcsnlen(str, avail + 1);
        return std::wstring(str, len);
    }
    return L"";
}

DWORD ReadRegDword(HKEY key, const wchar_t* name, DWORD fallback) {
    DWORD type = 0;
    DWORD value = 0;
    DWORD bytes = sizeof(value);
    if (RegQueryValueExW(key, name, nullptr, &type,
                         reinterpret_cast<BYTE*>(&value),
                         &bytes) == ERROR_SUCCESS &&
        type == REG_DWORD && bytes == sizeof(value)) {
        return value;
    }
    return fallback;
}

void LoadConfig(AppState& s, bool* existed) {
    HKEY key = nullptr;
    *existed = RegOpenKeyExW(HKEY_CURRENT_USER, kRegPath, 0, KEY_READ,
                             &key) == ERROR_SUCCESS;
    if (!*existed) return;
    const DWORD count = std::clamp<DWORD>(ReadRegDword(key, L"PinCount", 0),
                                          0, 120);
    for (DWORD i = 0; i < count; ++i) {
        wchar_t name[32] = {};
        swprintf_s(name, L"Pin%02u", i);
        std::wstring p = ReadRegString(key, name);
        if (!p.empty()) s.pins.push_back(p);
    }
    const DWORD hideCount =
        std::clamp<DWORD>(ReadRegDword(key, L"HideCount", 0), 0, 500);
    for (DWORD i = 0; i < hideCount; ++i) {
        wchar_t name[32] = {};
        swprintf_s(name, L"Hidden%02u", i);
        std::wstring p = ReadRegString(key, name);
        if (!p.empty()) s.hiddenKeys.insert(p);
    }
    s.bottomGapBase =
        static_cast<int>(ReadRegDword(key, L"BottomGap",
                                      static_cast<DWORD>(kBottomGapBase)));
    // 旧版本默认 6（悬空）→ 新版本默认 0（贴底），迁移历史配置
    if (s.bottomGapBase == 6) s.bottomGapBase = 0;
    s.autoCollapse = ReadRegDword(key, L"AutoCollapse", 1) != 0;
    RegCloseKey(key);
    Logf(L"配置载入：固定 %zu 项，隐藏 %zu 项", s.pins.size(),
         s.hiddenKeys.size());
}

void SaveConfig(AppState& s) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegPath, 0, nullptr, 0, KEY_WRITE,
                        nullptr, &key, nullptr) != ERROR_SUCCESS) {
        Logf(L"保存配置失败（注册表拒绝写入）");
        return;
    }
    const DWORD count = static_cast<DWORD>(s.pins.size());
    RegSetValueExW(key, L"PinCount", 0, REG_DWORD,
                   reinterpret_cast<const BYTE*>(&count), sizeof(count));
    for (DWORD i = 0; i < count; ++i) {
        wchar_t name[32] = {};
        swprintf_s(name, L"Pin%02u", i);
        RegSetValueExW(key, name, 0, REG_SZ,
                       reinterpret_cast<const BYTE*>(s.pins[i].c_str()),
                       static_cast<DWORD>((s.pins[i].size() + 1) *
                                          sizeof(wchar_t)));
    }
    const DWORD hideCount = static_cast<DWORD>(s.hiddenKeys.size());
    RegSetValueExW(key, L"HideCount", 0, REG_DWORD,
                   reinterpret_cast<const BYTE*>(&hideCount),
                   sizeof(hideCount));
    DWORD i = 0;
    for (const auto& h : s.hiddenKeys) {
        wchar_t name[32] = {};
        swprintf_s(name, L"Hidden%02u", i++);
        RegSetValueExW(key, name, 0, REG_SZ,
                       reinterpret_cast<const BYTE*>(h.c_str()),
                       static_cast<DWORD>((h.size() + 1) * sizeof(wchar_t)));
    }
    const DWORD gap = static_cast<DWORD>(s.bottomGapBase);
    RegSetValueExW(key, L"BottomGap", 0, REG_DWORD,
                   reinterpret_cast<const BYTE*>(&gap), sizeof(gap));
    const DWORD autoCollapse = s.autoCollapse ? 1 : 0;
    RegSetValueExW(key, L"AutoCollapse", 0, REG_DWORD,
                   reinterpret_cast<const BYTE*>(&autoCollapse),
                   sizeof(autoCollapse));
    RegCloseKey(key);
}

// 初次运行：从仓库目录常见快捷方式挑选固定项（仅注册表无任何记录时执行）
void SeedDefaultPinsIfEmpty(AppState& s) {
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const std::wstring binDir = PathDir(exe);
    const std::wstring repoDir = PathDir(binDir);  // ...\clock（桌面快捷方式都在此）

    auto exists = [](const std::wstring& p) {
        return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES;
    };
    const std::wstring candidates[] = {
        repoDir + L"\\Terminal.lnk",
        repoDir + L"\\记事本.lnk",
        repoDir + L"\\Excel.lnk",
        repoDir + L"\\Word.lnk",
        repoDir + L"\\PowerPoint.lnk",
        repoDir + L"\\此电脑.lnk",
    };
    for (const auto& c : candidates) {
        const bool ok = exists(c);
        Logf(L"种子候选 %ls ：%ls", c.c_str(), ok ? L"存在" : L"不存在");
        if (ok && s.pins.size() < 10) s.pins.push_back(c);
    }
    Logf(L"首次初始化：预置固定项 %zu 个", s.pins.size());
}

// ============================== 快捷方式解析 ==============================

struct ShortcutTarget {
    std::wstring target;
    std::wstring workDir;
};

ShortcutTarget ResolveShortcut(const std::wstring& lnkPath) {
    ShortcutTarget out;
    IShellLinkW* link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&link))) ||
        !link) {
        return out;
    }
    IPersistFile* persist = nullptr;
    if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&persist))) && persist) {
        if (SUCCEEDED(persist->Load(lnkPath.c_str(), STGM_READ))) {
            wchar_t buf[MAX_PATH] = {};
            link->GetPath(buf, MAX_PATH, nullptr, SLGP_RAWPATH);
            out.target = buf;
            if (out.target.empty()) {
                // Office 等“广告式快捷方式”RAWPATH 为空：
                // 优先读 System.Link.TargetParsingPath 属性
                IPropertyStore* ps = nullptr;
                if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&ps))) &&
                    ps) {
                    PROPERTYKEY pkey{};
                    PROPVARIANT v{};
                    PropVariantInit(&v);
                    if (SUCCEEDED(PSGetPropertyKeyFromName(
                            L"System.Link.TargetParsingPath", &pkey)) &&
                        SUCCEEDED(ps->GetValue(pkey, &v)) &&
                        v.vt == VT_LPWSTR && v.pwszVal) {
                        out.target = v.pwszVal;
                    }
                    PropVariantClear(&v);
                    ps->Release();
                }
            }
            if (out.target.empty()) {
                // 再退一步走 PIDL 让 shell 解析
                PIDLIST_ABSOLUTE pidl = nullptr;
                if (SUCCEEDED(link->GetIDList(&pidl)) && pidl) {
                    wchar_t pidlBuf[MAX_PATH] = {};
                    if (SHGetPathFromIDListW(pidl, pidlBuf)) {
                        out.target = pidlBuf;
                    }
                    ILFree(pidl);
                }
            }
            if (out.target.empty()) {
                // 最后一层：Office 广告式 lnk 的目标指向 Apps 命名空间，
                // 但图标路径通常就是真实 exe（EXCEL.EXE,0）
                wchar_t iconBuf[MAX_PATH * 2] = {};
                int iconIdx = 0;
                link->GetIconLocation(iconBuf, MAX_PATH * 2 - 1, &iconIdx);
                std::wstring iconPath = iconBuf;
                if (iconPath.size() > 4 &&
                    ToLower(iconPath).rfind(L".exe") ==
                        iconPath.size() - 4) {
                    wchar_t exp[MAX_PATH * 2] = {};
                    if (ExpandEnvironmentStringsW(iconPath.c_str(), exp,
                                                  MAX_PATH * 2 - 1) > 0) {
                        out.target = exp;
                    }
                }
            }
            wchar_t dirBuf[MAX_PATH] = {};
            link->GetWorkingDirectory(dirBuf, MAX_PATH);
            out.workDir = dirBuf;
        }
        persist->Release();
    }
    link->Release();
    return out;
}

struct PinDescriptor {
    std::wstring key;         // 归一化目标 exe（解析失败时就是 lnk 本身）
    std::wstring name;        // 显示名（lnk 文件名去掉扩展名）
    std::wstring launchPath;  // 原 pin 路径（点击启动用它）
};

// DescribePin 结果缓存：固定项解析（IShellLink COM 加载）只在首次执行。
// 固定列表变动时调用 ClearPinCache() 失效化。
static std::unordered_map<std::wstring, PinDescriptor> g_pinDescCache;

void ClearPinCache() { g_pinDescCache.clear(); }

PinDescriptor DescribePin(const std::wstring& pinPath) {
    auto it = g_pinDescCache.find(pinPath);
    if (it != g_pinDescCache.end()) return it->second;

    PinDescriptor d;
    d.launchPath = pinPath;
    d.name = StripExtension(PathBasename(pinPath));
    const std::wstring lower = ToLower(pinPath);
    const bool isLnk = lower.size() > 4 &&
                       lower.compare(lower.size() - 4, 4, L".lnk") == 0;
    if (isLnk) {
        ShortcutTarget t = ResolveShortcut(pinPath);
        if (!t.target.empty()) {
            if (t.target.size() >= 2 && t.target[1] != L':' &&
                t.target[0] != L'\\' && !t.workDir.empty()) {
                t.target = t.workDir + L"\\" + t.target;  // 补相对路径
            }
            d.key = NormalizePath(t.target);
        }
        Logf(L"解析固定项：%ls → %ls", pinPath.c_str(), d.key.c_str());
    }
    if (d.key.empty()) d.key = NormalizePath(pinPath);
    g_pinDescCache[pinPath] = d;
    return d;
}

// ============================== 图标提取（同 launcher 方案）===============

Bitmap* HiconToArgbBitmap(HICON hIcon) {
    // 与 launcher_main.cpp 同源：读 32bpp 颜色 + 1bpp 掩码，
    // 无 alpha 通道时退回掩码，最终统一预乘写入 PARGB 位图。
    ICONINFO ii{};
    if (!GetIconInfo(hIcon, &ii)) return nullptr;
    BITMAP bm{};
    if (!ii.hbmColor || !GetObjectW(ii.hbmColor, sizeof(bm), &bm) ||
        bm.bmWidth <= 0 || bm.bmHeight <= 0) {
        if (ii.hbmColor) DeleteObject(ii.hbmColor);
        if (ii.hbmMask) DeleteObject(ii.hbmMask);
        return nullptr;
    }
    const int w = bm.bmWidth;
    const int h = bm.bmHeight;
    HDC screenDc = GetDC(nullptr);
    Bitmap* bmp = nullptr;
    if (screenDc) {
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        std::vector<BYTE> color(static_cast<size_t>(w) * h * 4u, 0);
        const bool gotColor =
            GetDIBits(screenDc, ii.hbmColor, 0, h, color.data(), &bi,
                      DIB_RGB_COLORS) != 0;

        BITMAPINFO maskBi = bi;
        maskBi.bmiHeader.biBitCount = 1;
        const size_t maskStride =
            ((static_cast<size_t>(w) + 31u) / 32u) * 4u;
        std::vector<BYTE> mask(maskStride * h, 0);
        const bool gotMask =
            ii.hbmMask &&
            GetDIBits(screenDc, ii.hbmMask, 0, h, mask.data(), &maskBi,
                      DIB_RGB_COLORS) != 0;

        bool hasAlpha = false;
        if (gotColor) {
            for (size_t i = 3; i < color.size(); i += 4) {
                if (color[i] != 0) {
                    hasAlpha = true;
                    break;
                }
            }
        }
        // 老程序图标常见坑：32bpp 但 alpha 全 0，掩码也全 0。
        // 若掩码没有任何有效位，则把整图视为不透明，否则图标会整体消失。
        bool maskHasBits = false;
        if (gotMask) {
            for (const BYTE b : mask) {
                if (b != 0) {
                    maskHasBits = true;
                    break;
                }
            }
        }
        const bool forceOpaque = gotColor && !hasAlpha && !maskHasBits;

        bmp = new Bitmap(w, h, PixelFormat32bppPARGB);
        Gdiplus::Rect rect(0, 0, w, h);
        BitmapData bd{};
        if (bmp->LockBits(&rect, ImageLockModeWrite, PixelFormat32bppPARGB,
                          &bd) == Ok) {
            auto* dst = static_cast<BYTE*>(bd.Scan0);
            for (int y = 0; y < h; ++y) {
                const size_t maskRow = static_cast<size_t>(y) * maskStride;
                for (int x = 0; x < w; ++x) {
                    const size_t c =
                        (static_cast<size_t>(y) * w + x) * 4u;
                    BYTE alpha = 0;
                    if (hasAlpha) {
                        alpha = color[c + 3];
                    } else if (forceOpaque) {
                        alpha = 255;
                    } else if (gotMask) {
                        const BYTE bit =
                            mask[maskRow + static_cast<size_t>(x) / 8u] &
                            static_cast<BYTE>(0x80u >> (x & 7));
                        alpha = bit ? 255 : 0;
                    } else if (gotColor) {
                        alpha = (color[c] || color[c + 1] || color[c + 2])
                                    ? 255
                                    : 0;
                    }
                    const size_t d =
                        static_cast<size_t>(y) * bd.Stride +
                        static_cast<size_t>(x) * 4u;
                    if (alpha == 0) {
                        dst[d] = dst[d + 1] = dst[d + 2] = 0;
                    } else {
                        // DIB 是 BGRA；PARGB 需要预乘 alpha
                        dst[d] =
                            static_cast<BYTE>(color[c + 0] * alpha / 255);
                        dst[d + 1] =
                            static_cast<BYTE>(color[c + 1] * alpha / 255);
                        dst[d + 2] =
                            static_cast<BYTE>(color[c + 2] * alpha / 255);
                    }
                    dst[d + 3] = alpha;
                }
            }
            bmp->UnlockBits(&bd);
        }
        ReleaseDC(nullptr, screenDc);
    }
    DeleteObject(ii.hbmColor);
    DeleteObject(ii.hbmMask);
    return bmp;
}

Bitmap* GetIconBitmapForPath(const std::wstring& path) {
    SHFILEINFOW sfi{};
    if (!SHGetFileInfoW(path.c_str(), 0, &sfi, sizeof(sfi),
                        SHGFI_SYSICONINDEX)) {
        return nullptr;
    }
    IImageList* imageList = nullptr;
    if (SUCCEEDED(SHGetImageList(SHIL_JUMBO, IID_IImageList,
                                 reinterpret_cast<void**>(&imageList))) &&
        imageList) {
        HICON hIcon = nullptr;
        if (SUCCEEDED(imageList->GetIcon(sfi.iIcon, ILD_TRANSPARENT, &hIcon)) &&
            hIcon) {
            imageList->Release();
            Bitmap* bmp = HiconToArgbBitmap(hIcon);
            DestroyIcon(hIcon);
            if (bmp) return bmp;
        } else {
            imageList->Release();
        }
    }
    SHFILEINFOW fallback{};
    if (SHGetFileInfoW(path.c_str(), 0, &fallback, sizeof(fallback),
                       SHGFI_ICON | SHGFI_LARGEICON) &&
        fallback.hIcon) {
        Bitmap* bmp = HiconToArgbBitmap(fallback.hIcon);
        DestroyIcon(fallback.hIcon);
        return bmp;
    }
    return nullptr;
}

// 部分 .lnk/.exe 的图标源只有小尺寸帧，Jumbo 列表返回 256x256 画布、
// 内容只占左上角一小块（如 AltDrag）。按内容包围盒裁剪，让图标撑满显示区。
Bitmap* CropToIconContent(Bitmap* bmp) {
    if (!bmp) return nullptr;
    const int W = bmp->GetWidth();
    const int H = bmp->GetHeight();
    if (W <= 0 || H <= 0) return bmp;

    Gdiplus::Rect r(0, 0, W, H);
    BitmapData bd{};
    if (bmp->LockBits(&r, ImageLockModeRead, PixelFormat32bppPARGB, &bd) !=
        Ok) {
        return bmp;
    }
    int minX = W, minY = H, maxX = -1, maxY = -1;
    for (int y = 0; y < H; ++y) {
        const BYTE* row =
            static_cast<const BYTE*>(bd.Scan0) +
            static_cast<size_t>(y) * static_cast<size_t>(bd.Stride);
        for (int x = 0; x < W; ++x) {
            if (row[static_cast<size_t>(x) * 4u + 3u] > 8) {
                if (x < minX) minX = x;
                if (x > maxX) maxX = x;
                if (y < minY) minY = y;
                if (y > maxY) maxY = y;
            }
        }
    }
    bmp->UnlockBits(&bd);

    if (maxX < 0) return bmp;  // 全透明，保持原样

    const int cw = maxX - minX + 1;
    const int ch = maxY - minY + 1;
    // 内容已覆盖大部分画布：正常图标，无需裁剪
    if (cw >= W * 0.55f && ch >= H * 0.55f) return bmp;

    const int pad = std::max(1, static_cast<int>(
                                    std::lround(std::max(cw, ch) * 0.06f)));
    const int sx = std::max(0, minX - pad);
    const int sy = std::max(0, minY - pad);
    const int sw = std::min(W - sx, cw + pad * 2);
    const int sh = std::min(H - sy, ch + pad * 2);

    Bitmap* cropped = new Bitmap(sw, sh, PixelFormat32bppPARGB);
    if (!cropped) return bmp;
    Graphics g(cropped);
    g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    g.DrawImage(bmp, Gdiplus::Rect(0, 0, sw, sh), sx, sy, sw, sh, UnitPixel);
    return cropped;
}

// 图标源位图预缩上限：每帧 DrawImage 的成本 ≈ 源像素数 × 目标像素数。
// jumbo 源是 256×256，而渲染目标是 ≤ kMaxScale·kIconSize·dpi ≈ 73px——
// 每帧从 256px 缩放纯属浪费（实测 18 个图标占 ~17ms/帧，拖垮整帧节奏，
// 而 Dock 的 WH_MOUSE_LL 钩子跑在自身 UI 线程，线程被渲染占满会阻塞
// 全局鼠标事件 → 光标一到 Dock 就掉帧）。加载时一次性用最高质量
// Bicubic 预缩到渲染上限，之后每帧绘制成本下降 ~4 倍且视觉无损。
constexpr int kIconCacheMaxSourcePx = 160;

Bitmap* PreScaleIconSource(Bitmap* bmp) {
    if (!bmp) return nullptr;
    const int W = bmp->GetWidth();
    const int H = bmp->GetHeight();
    if (W <= kIconCacheMaxSourcePx && H <= kIconCacheMaxSourcePx) {
        return bmp;  // 无需预缩（多为已裁剪的小图标）
    }
    const float maxDim = static_cast<float>(std::max(W, H));
    const int sw = static_cast<int>(std::lround(W * (kIconCacheMaxSourcePx / maxDim)));
    const int sh = static_cast<int>(std::lround(H * (kIconCacheMaxSourcePx / maxDim)));
    if (sw <= 0 || sh <= 0) return bmp;
    Bitmap* scaled = new Bitmap(sw, sh, PixelFormat32bppPARGB);
    if (!scaled) return bmp;
    Graphics g(scaled);
    g.SetSmoothingMode(SmoothingModeHighQuality);
    g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    g.DrawImage(bmp, Gdiplus::Rect(0, 0, sw, sh), 0, 0, W, H, UnitPixel);
    return scaled;
}

Bitmap* GetOrLoadIcon(AppState& s, const std::wstring& lowerKey) {
    auto it = s.iconCache.find(lowerKey);
    if (it != s.iconCache.end()) return it->second;  // 失败也缓存避免反复重试
    Bitmap* bmp = GetIconBitmapForPath(lowerKey);
    if (bmp) {
        Bitmap* cropped = CropToIconContent(bmp);
        if (cropped != bmp) {
            delete bmp;
            bmp = cropped;
        }
        Bitmap* pre = PreScaleIconSource(bmp);
        if (pre != bmp) {
            delete bmp;
            bmp = pre;
        }
    } else {
        Logf(L"图标提取失败：%ls", lowerKey.c_str());
    }
    s.iconCache[lowerKey] = bmp;
    return bmp;
}

void ClearIconCache(AppState& s) {
    for (auto& kv : s.iconCache) delete kv.second;
    s.iconCache.clear();
}

// ============================== 运行状态采集 ==============================

std::unordered_set<std::wstring> CollectRunningExePaths() {
    std::unordered_set<std::wstring> set;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return set;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            HANDLE proc =
                OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                            pe.th32ProcessID);
            if (!proc) continue;
            wchar_t img[MAX_PATH] = {};
            DWORD size = MAX_PATH;
            if (QueryFullProcessImageNameW(proc, 0, img, &size) && size > 0) {
                set.insert(NormalizePath(img));
            }
            CloseHandle(proc);
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return set;
}

std::vector<std::wstring> ReadRegisteredTrayRawPaths() {
    std::vector<std::wstring> paths;
    HKEY root = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kNotifyIconsKey, 0, KEY_READ,
                      &root) != ERROR_SUCCESS) {
        static bool warnedOnce = false;
        if (!warnedOnce) {
            warnedOnce = true;
            Logf(L"读取 NotifyIconSettings 失败（旧系统？托盘特性降级）");
        }
        return paths;
    }
    DWORD index = 0;
    for (;;) {
        wchar_t name[128] = {};
        DWORD nameLen = 127;
        if (RegEnumKeyExW(root, index++, name, &nameLen, nullptr, nullptr,
                          nullptr, nullptr) != ERROR_SUCCESS) {
            break;
        }
        HKEY sub = nullptr;
        if (RegOpenKeyExW(root, name, 0, KEY_READ, &sub) == ERROR_SUCCESS) {
            std::wstring exe = ReadRegString(sub, L"ExecutablePath");
            if (!exe.empty()) paths.push_back(exe);
            RegCloseKey(sub);
        }
    }
    RegCloseKey(root);
    return paths;
}

DWORD PidOf(HWND hwnd) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    return pid;
}

std::wstring ImagePathOfPid(DWORD pid) {
    HANDLE proc =
        OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) return L"";
    wchar_t img[MAX_PATH] = {};
    DWORD size = MAX_PATH;
    std::wstring out;
    if (QueryFullProcessImageNameW(proc, 0, img, &size) && size > 0) {
        out = NormalizePath(img);
    }
    CloseHandle(proc);
    return out;
}

bool IsCloaked(HWND hwnd) {
    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked,
                                        sizeof(cloaked)))) {
        return cloaked != 0;
    }
    return false;
}

bool IsShellSystemClass(HWND hwnd) {
    wchar_t cls[64] = {};
    if (GetClassNameW(hwnd, cls, 63) <= 0) return false;
    return wcscmp(cls, L"Progman") == 0 ||
           wcscmp(cls, L"WorkerW") == 0 ||
           wcscmp(cls, L"SHELLDLL_DefView") == 0 ||
           wcscmp(cls, L"Shell_TrayWnd") == 0 ||
           wcscmp(cls, L"Shell_SecondaryTrayWnd") == 0 ||
           wcscmp(cls, L"XamlExplorerHostIslandWindow") == 0;
}

// 文件管理器窗口（explorer.exe 的 CabinetWClass / 旧版 ExploreWClass）：
// 尽管其宿主进程是基础设施，文件管理窗口本身是用户意义上的应用，
// Dock 的“来源 2”应以窗口为准放行（见 RefreshItems）
bool IsFileExplorerWindow(HWND hwnd) {
    wchar_t cls[64] = {};
    if (GetClassNameW(hwnd, cls, 63) <= 0) return false;
    return wcscmp(cls, L"CabinetWClass") == 0 ||
           wcscmp(cls, L"ExploreWClass") == 0;
}

// UWP 框架窗口的真实身份是其中 CoreWindow 所属的包进程
DWORD RealIdentityPidForFrame(HWND frame) {
    HWND core =
        FindWindowExW(frame, nullptr, L"Windows.UI.Core.CoreWindow", nullptr);
    if (!core) return 0;
    DWORD corePid = 0;
    GetWindowThreadProcessId(core, &corePid);
    return corePid;
}

struct WindowGroupContext {
    DWORD selfPid = 0;
    std::vector<std::pair<std::wstring, HWND>> ordered;  // Z 序
    // 单轮次内 pid→镜像路径缓存：多窗口应用（网易云等 5+ 顶层窗）
    // 同一 pid 反复查询会重复 OpenProcess，缓存后按进程只查一次
    std::unordered_map<DWORD, std::wstring> pathCache;
    // 单轮次内 pid→父pid 快照缓存：UI 宿主辅助进程归并（见 UiHostExeForPid）
    std::unordered_map<DWORD, DWORD> parentCache;
    // 单轮次内 pid→宿主应用路径：辅助进程多窗口时只解析一次父链
    std::unordered_map<DWORD, std::wstring> hostCache;
};

// 为“UI 宿主辅助进程”（如 steamwebhelper.exe）解析宿主应用 exe 路径
// （归一化小写）：沿父进程链（≤8 层）找 basename == appBase 的祖先进程。
// parentCache：pid→父pid 快照缓存（一次 EnumWindows 仅建一次，多窗口共享）。
// 解析失败返回空串 —— 调用方回退为原进程身份，行为无回归。
std::wstring UiHostExeForPid(WindowGroupContext* ctx, DWORD helperPid,
                             const UiHostMapping& m) {
    auto cached = ctx->hostCache.find(helperPid);
    if (cached != ctx->hostCache.end()) return cached->second;
    if (ctx->parentCache.empty()) {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE) return L"";
        PROCESSENTRY32W pe{};
        pe.dwSize = sizeof(pe);
        if (Process32FirstW(snap, &pe)) {
            do {
                ctx->parentCache[pe.th32ProcessID] = pe.th32ParentProcessID;
            } while (Process32NextW(snap, &pe));
        }
        CloseHandle(snap);
    }
    const std::wstring appBase(m.appBase);
    DWORD cur = helperPid;
    std::wstring host;
    for (int depth = 0; depth < 8; ++depth) {
        auto it = ctx->parentCache.find(cur);
        if (it == ctx->parentCache.end() || it->second == 0) break;
        cur = it->second;
        const std::wstring img = ImagePathOfPid(cur);
        if (img.empty()) continue;
        if (ToLower(PathBasename(img)) == appBase) {
            host = img;
            break;
        }
    }
    ctx->hostCache[helperPid] = host;  // 空值也缓存：同轮不再重复解析
    return host;
}

BOOL CALLBACK EnumAppWindowProc(HWND hwnd, LPARAM lParam) {
    auto* ctx = reinterpret_cast<WindowGroupContext*>(lParam);
    if (!ctx || !IsWindow(hwnd)) return TRUE;
    if (!IsWindowVisible(hwnd)) return TRUE;
    if (IsCloaked(hwnd)) return TRUE;
    if (IsShellSystemClass(hwnd)) return TRUE;
    if (GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE;
    const LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if ((ex & WS_EX_TOOLWINDOW) != 0 && (ex & WS_EX_APPWINDOW) == 0) {
        return TRUE;
    }
    DWORD pid = PidOf(hwnd);
    if (pid == 0 || pid == ctx->selfPid) return TRUE;

    wchar_t cls[64] = {};
    GetClassNameW(hwnd, cls, 63);

    // 托盘回调消息窗（微信 Qt51514WxTrayIconMessageWindowClass 等）不是
    // 主窗口：正常时隐藏，但被错误唤起后会带真窗口尺寸，绝不能算“有窗口”
    if (wcsstr(cls, L"TrayIconMessage") != nullptr) return TRUE;

    std::wstring identity;
    auto it = ctx->pathCache.find(pid);
    if (it != ctx->pathCache.end()) {
        identity = it->second;
    } else {
        identity = ImagePathOfPid(pid);
        ctx->pathCache[pid] = identity;
    }
    if (identity.empty()) return TRUE;
    if (wcscmp(cls, L"ApplicationFrameWindow") == 0) {
        const DWORD realPid = RealIdentityPidForFrame(hwnd);
        if (realPid != 0) {
            const std::wstring real = ImagePathOfPid(realPid);
            if (!real.empty()) identity = real;
        }
    }

    // “UI 宿主辅助进程”归并：Steam 等应用的可见主窗口由辅助进程
    // （steamwebhelper.exe / CEF）承载，宿主进程（steam.exe）只做托盘
    // 驻留。按 exe 分组会出现“Steam【托盘】+ Steam WebHelper【窗口】”
    // 两条且托盘条无窗口可唤起；沿父进程链归并到宿主应用 exe，
    // 让该窗口进入宿主应用条目（与任务栏按 AppUserModelID 归并一致）。
    const UiHostMapping* hostMap =
        FindUiHostMapping(ToLower(PathBasename(identity)));
    if (hostMap) {
        const std::wstring host = UiHostExeForPid(ctx, pid, *hostMap);
        if (!host.empty()) {
            // 只记录一次（避免每轮询刷屏；与“固定项并入运行组”同模式）
            static std::unordered_set<std::wstring> mergedLogged;
            if (mergedLogged.insert(identity).second) {
                Logf(L"窗口归并到宿主应用：%ls → %ls",
                     PathBasename(identity).c_str(), host.c_str());
            }
            identity = host;
        }
    }
    ctx->ordered.emplace_back(identity, hwnd);
    return TRUE;
}

// ============================== DPI / 前台辅助 ==============================

void EnableDpiAwareness() {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        using SetContextFn = BOOL(WINAPI*)(HANDLE);
        auto pSetContext = reinterpret_cast<SetContextFn>(
            GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
        if (pSetContext &&
            pSetContext(reinterpret_cast<HANDLE>(static_cast<INT_PTR>(-4)))) {
            return;  // PER_MONITOR_AWARE_V2
        }
    }
    SetProcessDPIAware();
}

int QueryPrimaryDpi() {
    HDC hdc = GetDC(nullptr);
    const int dpi = hdc ? GetDeviceCaps(hdc, LOGPIXELSX) : 96;
    if (hdc) ReleaseDC(nullptr, hdc);
    return dpi ? dpi : 96;
}

void ForceForegroundWindow(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return;
    HWND fg = GetForegroundWindow();
    if (fg == hwnd) return;

    // 关键：不再使用 AttachThreadInput + BringWindowToTop 抢前台。
    // 这两个调用可能因为目标应用（特别是 Steam/CEF、资源管理器等）的
    // 窗口线程忙/未响应而阻塞；Dock 的低层鼠标钩子就运行在同一个 UI 线程上，
    // 一旦 UI 线程卡住，全局鼠标输入都会被卡死（只能键盘操作/按 Ctrl+Alt+Del）。
    //
    // 改为经典的 ALT 键模拟：先注入一次 ALT 让本线程获得设置前台权限，
    // 再 SetForegroundWindow。整个过程不附加任何线程输入队列，不会卡死。
    INPUT alt[2]{};
    alt[0].type = INPUT_KEYBOARD;
    alt[0].ki.wVk = VK_MENU;
    alt[1].type = INPUT_KEYBOARD;
    alt[1].ki.wVk = VK_MENU;
    alt[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, alt, sizeof(INPUT));

    if (IsIconic(hwnd)) {
        DWORD_PTR dummy = 0;
        SendMessageTimeoutW(hwnd, WM_SYSCOMMAND, SC_RESTORE, 0,
                            SMTO_ABORTIFHUNG | SMTO_BLOCK, 500, &dummy);
    }
    SetForegroundWindow(hwnd);
}

RECT g_primaryWorkArea{};  // 主屏工作区缓存：鼠标移动热路径（LL 钩子/每帧）零系统调用

void RefreshPrimaryWorkArea() {
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &g_primaryWorkArea, 0);
}

RECT PrimaryWorkArea() {
    // 惰性兜底：首次调用或异常失效时重查；正常由启动、SPI_SETWORKAREA
    // 之后、WM_DPICHANGED/WM_DISPLAYCHANGE 与 DoPoll 保底刷新
    if (g_primaryWorkArea.right <= g_primaryWorkArea.left) {
        RefreshPrimaryWorkArea();
    }
    return g_primaryWorkArea;
}

// ============================== 任务栏控制 ==============================
// Dock 常驻期间隐藏 Windows 任务栏（含副屏任务栏），退出时恢复。
// 注意：ShowWindow(SW_HIDE) 只会让任务栏窗口消失，系统的工作区
// （SPI_GETWORKAREA）仍是任务栏占位时的旧值 —— 必须手动把工作区
// 扩为全屏，否则依赖工作区定位的程序（包括本 Dock）会停在任务栏
// 原有位置上方。恢复时同理，要把工作区还原为原值。

void RepositionDock(AppState& s, bool forceZOrder);  // 定义在下方（摆放）

RECT VirtualScreenRect() {
    RECT rc{
        GetSystemMetrics(SM_XVIRTUALSCREEN),
        GetSystemMetrics(SM_YVIRTUALSCREEN),
        GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN),
        GetSystemMetrics(SM_YVIRTUALSCREEN) + GetSystemMetrics(SM_CYVIRTUALSCREEN)};
    return rc;
}

void HideTaskbar() {
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &g_state.savedWorkArea, 0);
    HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (tray) ShowWindow(tray, SW_HIDE);
    for (HWND sec = FindWindowExW(nullptr, nullptr,
                                  L"Shell_SecondaryTrayWnd", nullptr);
         sec;
         sec = FindWindowExW(nullptr, sec, L"Shell_SecondaryTrayWnd",
                             nullptr)) {
        ShowWindow(sec, SW_HIDE);
    }
    const RECT full = VirtualScreenRect();
    // 注意不能带 SPIF_SENDCHANGE：广播 WM_SETTINGCHANGE 后 explorer
    // 会按任务栏占位重算工作区，把刚设置的全屏值覆盖回去
    SystemParametersInfoW(SPI_SETWORKAREA, 0, const_cast<RECT*>(&full), 0);
    RefreshPrimaryWorkArea();  // 工作区已改为全屏，同步缓存
}

void ShowTaskbar() {
    HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (tray) ShowWindow(tray, SW_SHOW);
    for (HWND sec = FindWindowExW(nullptr, nullptr,
                                  L"Shell_SecondaryTrayWnd", nullptr);
         sec;
         sec = FindWindowExW(nullptr, sec, L"Shell_SecondaryTrayWnd",
                             nullptr)) {
        ShowWindow(sec, SW_SHOW);
    }
    if (g_state.savedWorkArea.right > g_state.savedWorkArea.left &&
        g_state.savedWorkArea.bottom > g_state.savedWorkArea.top) {
        // 同上：不带 SPIF_SENDCHANGE，避免 explorer 覆盖
        SystemParametersInfoW(SPI_SETWORKAREA, 0, &g_state.savedWorkArea, 0);
    }
}

// 周期性保护：explorer 崩溃自动重启后任务栏会重建为可见，
// 轮询发现可见则再次隐藏（与 RefreshItems 同节奏，开销可忽略）
void EnsureTaskbarHidden(AppState& s) {
    // 托盘图标触发期间任务栏临时可见（见 TrayIconTrigger），不得抢收
    if (GetTickCount64() < s.taskbarShowUntil) return;
    bool touched = false;
    HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (tray && IsWindowVisible(tray)) {
        ShowWindow(tray, SW_HIDE);
        touched = true;
    }
    for (HWND sec = FindWindowExW(nullptr, nullptr,
                                  L"Shell_SecondaryTrayWnd", nullptr);
         sec;
         sec = FindWindowExW(nullptr, sec, L"Shell_SecondaryTrayWnd",
                             nullptr)) {
        if (IsWindowVisible(sec)) {
            ShowWindow(sec, SW_HIDE);
            touched = true;
        }
    }

    // 关键：任务栏即使已经处于隐藏状态，工作区也可能被 explorer/开始菜单/
    // 临时显示任务栏等操作重新改回“任务栏占位”值（例如底边从 1080 变成 1032）。
    // 原实现只在“本次发现任务栏可见并隐藏”时扩充工作区，导致这种隐藏但工作区
    // 未恢复的情况不会自愈，Dock 就会停在任务栏原位置上方。
    const RECT full = VirtualScreenRect();
    RECT wa{};
    const bool gotWorkArea =
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0) != FALSE;
    const bool workAreaStale =
        !gotWorkArea || wa.left != full.left || wa.top != full.top ||
        wa.right != full.right || wa.bottom != full.bottom;

    if (touched || workAreaStale) {
        SystemParametersInfoW(SPI_SETWORKAREA, 0,
                              const_cast<RECT*>(&full), 0);
        RefreshPrimaryWorkArea();  // 工作区已改为全屏，同步缓存
        RepositionDock(s, true);  // 任务栏隐藏，dock 重新贴底
    }
}

// ============================== 显示桌面（右下角）+ 开始菜单（左下角）==============================
// Dock 常驻时任务栏隐藏，屏幕两角让位给 Windows 系统按钮语义：
//   右下角：显示桌面隐形按钮（与 Windows 显示桌面按钮同尺寸 12×48 逻辑像素），
//            点击 = Windows 的显示桌面（再点恢复）；
//   左下角：开始按钮隐形按钮（同尺寸），点击 = 触发 Win 键（打开/切换开始菜单），
//            等价于 Windows 开始按钮。
// 两者均由 WH_MOUSE_LL 低层鼠标钩子识别并吞掉点击，零窗口零像素遮挡，完全隐形。
//
// 关于系统 Win+D 与“组件被最小化”：
// 早期版本任务栏可见时，系统 Show Desktop 只最小化任务栏窗口，套件组件
// （WS_EX_NOACTIVATE 工具窗）天然被排除；任务栏被 Dock 隐藏后，shell 的
// Show Desktop 不再能按任务栏窗口组计算排除集，会把组件一并最小化。
// 不拦截 Win+D（拦截会破坏 Win 修饰键状态并引发 Start 菜单/切换语义问题），
// 而是让其正常发生并由 Dock 每帧自愈：组件窗口一旦被最小化立即 SW_RESTORE，
// 对用户可见的闪烁 ≤1 帧；普通应用窗口维持系统 Show Desktop 的原生行为。
// 右下角隐形按钮走自家 ToggleShowDesktop（显式排除套件组件，零窗口零像素）。

constexpr int kShowDesktopWLogical = 12;  // 隐形按钮宽（逻辑像素，与 Windows 按钮一致）
constexpr int kShowDesktopHLogical = 48;  // 隐形按钮高（逻辑像素，与 Windows 按钮一致）

HHOOK g_showDesktopHook = nullptr;
bool g_hookLeftDownOnDock = false;         // 低层钩子吞掉过 Dock 左键按下：抬起也须吞掉
bool g_hookLastOnDock = false;             // 低层钩子线程本地：上次“在 Dock”状态
DWORD g_hookThreadId = 0;                  // WH_MOUSE_LL 钩子专用线程 ID
std::thread g_hookThread;                  // WH_MOUSE_LL 钩子专用线程
std::vector<HWND> g_showDesktopMinimized;  // 本次显示桌面最小化的窗口（再点恢复）
bool g_showDesktopActive = false;          // 显示桌面状态（true=再点恢复）

// 屏幕角部点击区（主屏物理坐标；尺寸随 DPI 缩放，与 Windows 按钮一致）
bool InCornerZone(POINT pt, bool left) {
    const int w = MulDiv(kShowDesktopWLogical, g_state.dpi, 96);
    const int h = MulDiv(kShowDesktopHLogical, g_state.dpi, 96);
    const int sw = GetSystemMetrics(SM_CXSCREEN);
    const int sh = GetSystemMetrics(SM_CYSCREEN);
    if (pt.y < sh - h || pt.y >= sh) return false;
    if (left) return pt.x >= 0 && pt.x < w;
    return pt.x >= sw - w && pt.x < sw;
}

// 触发 Win 键（干净的一下：按下+弹起，等同系统开始按钮），打开/切换开始菜单
void TriggerWinKey() {
    INPUT in[2]{};
    in[0].type = INPUT_KEYBOARD;
    in[0].ki.wVk = VK_LWIN;
    in[1].type = INPUT_KEYBOARD;
    in[1].ki.wVk = VK_LWIN;
    in[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, in, sizeof(INPUT));
}

// 套件组件顶层窗口（自愈缓存：Win+D 被最小化时立即恢复）
// 类名与各组件源码一致；MyWigetsTrayWindow 通常隐藏，不影响判断
std::vector<HWND> g_suiteWindows;

// 是否为套件组件窗口（类名判定：显式归属，防御同名窗口）
bool IsSuiteWindowClass(HWND hwnd) {
    if (!hwnd) return false;
    wchar_t cls[64] = {};
    if (GetClassNameW(hwnd, cls, 63) <= 0) return false;
    static const wchar_t* kSuiteCls[] = {
        L"DesktopTopBarWindow", L"DesktopAnalogClockWindow",
        L"DesktopCalendarWindow", L"DesktopLauncherWindow",
        L"MyWigetsTrayWindow",
    };
    for (const wchar_t* c : kSuiteCls) {
        if (wcscmp(cls, c) == 0) return true;
    }
    return false;
}

void RefreshSuiteWindowCache() {
    g_suiteWindows.clear();
    EnumWindows(
        [](HWND hwnd, LPARAM) -> BOOL {
            if (IsSuiteWindowClass(hwnd)) g_suiteWindows.push_back(hwnd);
            return TRUE;
        },
        0);
}

// 每帧调用：任何套件组件被最小化（系统 Win+D 的 Show Desktop 残留）
// 立即恢复 —— 保证“Win+D 不影响组件隐藏”
void HealMinimizedSuiteWindows() {
    for (HWND w : g_suiteWindows) {
        if (IsWindow(w) && IsIconic(w)) {
            ShowWindow(w, SW_RESTORE);
            Logf(L"显示桌面自愈：恢复被系统最小化的组件窗口 0x%X",
                 static_cast<unsigned>(reinterpret_cast<UINT_PTR>(w)));
        }
    }
}

// 自家套件进程（exe 基名）：显示桌面时绝不最小化
bool IsOwnSuitePid(DWORD pid) {
    if (pid == 0 || pid == GetCurrentProcessId()) return true;
    const std::wstring img = ImagePathOfPid(pid);
    if (img.empty()) return false;
    const std::wstring base = ToLower(PathBasename(img));
    for (const wchar_t* name : kSelfExclusions) {
        if (base == name) return true;  // 表内全部为小写
    }
    return false;
}

// 显示桌面：最小化除桌面/系统/套件组件外的全部可见顶层窗口；
// 处于显示桌面状态时再次调用则恢复上次最小化的窗口（与 Windows 一致）
void ToggleShowDesktop() {
    if (g_showDesktopActive) {
        const size_t n = g_showDesktopMinimized.size();
        for (HWND w : g_showDesktopMinimized) {
            if (IsWindow(w) && IsIconic(w)) ShowWindow(w, SW_RESTORE);
        }
        g_showDesktopMinimized.clear();
        g_showDesktopActive = false;
        Logf(L"显示桌面：恢复上次最小化的 %zu 个窗口", n);
        return;
    }

    g_showDesktopMinimized.clear();
    EnumWindows(
        [](HWND hwnd, LPARAM) -> BOOL {
            if (!IsWindowVisible(hwnd)) return TRUE;  // 隐藏窗不动
            if (IsIconic(hwnd)) return TRUE;          // 已最小化不动
            if (IsCloaked(hwnd)) return TRUE;         // UWP 幽灵窗不动
            if (GetWindow(hwnd, GW_OWNER)) return TRUE;  // 跟随属主
            if (IsShellSystemClass(hwnd)) return TRUE;   // 桌面/任务栏本体
            wchar_t cls[64] = {};
            GetClassNameW(hwnd, cls, 63);
            if (wcsstr(cls, L"Ghost") || wcsstr(cls, L"Island")) {
                return TRUE;
            }
            const LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
            // 工具窗（无任务栏按钮）与系统热键窗：不参与显示桌面
            if ((ex & WS_EX_TOOLWINDOW) != 0 &&
                (ex & WS_EX_APPWINDOW) == 0) {
                return TRUE;
            }
            if (IsOwnSuitePid(PidOf(hwnd))) return TRUE;  // 自家组件永不隐藏
            ShowWindow(hwnd, SW_MINIMIZE);
            g_showDesktopMinimized.push_back(hwnd);
            return TRUE;
        },
        0);

    // 没有任何窗口需要最小化时（如已在桌面态）不进入“恢复”状态，
    // 下次点击重新尝试最小化 —— 与 Windows 的显示桌面语义一致
    g_showDesktopActive = !g_showDesktopMinimized.empty();
    Logf(L"显示桌面：最小化 %zu 个窗口", g_showDesktopMinimized.size());
}

// 交互动作定义在下方；低层鼠标钩子只采集点击并投递给 WndProc 统一执行
// （透明底部空隙/贴底边缘可能收不到窗口消息，钩子负责命中采集）
int HitIndexAt(AppState& s, float x, float y);
void ToggleFocusOrLaunch(AppState& s, size_t idx);
void SetFrameCadence(AppState& s, bool fast);
void RequestCloseByIndex(AppState& s, size_t idx);  // 中键关闭 + 立即隐藏圆点

// 展开触发条：与 Dock 栏同宽（含当前呈现宽度）、高 2px（随 DPI）的
// 屏幕下边缘区域；光标触碰即从收起状态升起
bool InDockStrip(POINT pt) {
    const int h = MulDiv(kDockStripHeightLogical, g_state.dpi, 96);
    const int bottom = g_primaryWorkArea.bottom;
    // 含 bottom 这一行：物理屏幕最底像素/贴边时系统可能报 sh-1、sh 或 sh+1，
    // 一律算 Dock 有效区，避免最低一列丢失悬停/点击。上界再放宽 2px，
    // 吸收光标贴底时坐标在 sh-1/sh/sh+1 之间的抖动（否则 onDock 每秒
    // 在 0/1 间高频翻转，触发收起-展开往返与消息风暴）。
    if (pt.y < bottom - h || pt.y > bottom + 2) return false;
    return pt.x >= g_state.winX && pt.x < g_state.winX + g_state.winW;
}

// 点是否位于 Dock 有效交互区 = 窗口矩形 ∪ 底部触发条（与 Dock 同宽、
// 2px 高的屏幕下缘）。窗口矩形与屏幕底边之间有间隙/透明区，触发条位于
// 其下——光标在这些区域（含渲染区下方边缘）都属于“在 Dock 上”
bool PointInDockOrStrip(POINT pt) {
    // 低层鼠标钩子运行在专用线程上，这里只用 UI 线程维护的缓存几何，
    // 绝不调用 GetWindowRect / SystemParametersInfo 等可能等待 UI 线程的
    // 函数——否则 UI 线程一旦阻塞，钩子线程也会被拖住，全局鼠标再次卡死。
    const RECT wa = g_primaryWorkArea;
    const int gap = MulDiv(g_state.bottomGapBase, g_state.dpi, 96);
    if (pt.y < wa.bottom - gap - g_state.winH) return false;
    const int bottom = wa.bottom;
    const int left = g_state.winX;
    const int right = left + g_state.winW;
    const int top = g_state.winY;
    // 下界同 InDockStrip：吸收屏幕最底行坐标抖动（sh-1/sh/sh+1），
    // 避免贴底时光标在“在 Dock”与“不在 Dock”之间高频切换。
    if (pt.x >= left && pt.x < right && pt.y >= top && pt.y <= bottom + 2) {
        return true;
    }
    return InDockStrip(pt);
}

// 收起方向：上方/左侧/右侧离开，或越过有效区下缘（如移到主屏下方的副屏）
// 才收起；底部离开但仍在有效区（空隙/触发条/贴边容差）内则不收起。
// wasOnDock 只在收起触发时复位（环带中间区域不复位），缓慢离开一样触发。
LRESULT CALLBACK ShowDesktopHookProc(int code, WPARAM wParam, LPARAM lParam) {
    // 本回调运行在专用的 WH_MOUSE_LL 钩子线程上，绝不阻塞/等待 UI 线程。
    // 职责仅限：采集点击（投递 kMsgHookClick）、检测进入/离开 Dock（投递
    // kMsgHookMouse），以及成对吞掉 Dock 上的左键按下/抬起。
    if (code == HC_ACTION) {
        auto* ms = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);

        // 右键菜单（TrackPopupMenu）打开期间，低层钩子必须完全放行鼠标事件，
        // 否则菜单项一旦落在 Dock 命中区内就会被 Dock 吞掉，导致菜单点了没反应。
        if (g_state.menuOpen) {
            return CallNextHookEx(nullptr, code, wParam, lParam);
        }

        if (wParam == WM_MOUSEMOVE) {
            const bool onDock = PointInDockOrStrip(ms->pt);
            if (onDock != g_hookLastOnDock) {
                g_hookLastOnDock = onDock;
                // lParam 携带触发本次状态翻转的光标物理位置（ms->pt）：
                // UI 线程用它判断离开方向（上/左/右 = 收起），避免 UI 线程
                // 再次 GetCursorPos 读到抖动后的新位置，导致贴顶离开判定
                // 失败、收起永不触发。
                PostMessageW(g_state.hwnd, kMsgHookMouse, onDock ? 1 : 0,
                             MAKELPARAM(ms->pt.x, ms->pt.y));
            }
        } else if (wParam == WM_LBUTTONDOWN || wParam == WM_LBUTTONUP) {
            if (wParam == WM_LBUTTONDOWN && InCornerZone(ms->pt, true)) {
                // 左下角：仿 Windows 开始按钮 —— 触发 Win 键（打开开始菜单）
                PostMessageW(g_state.hwnd, kMsgHookClick, kHookClickStart, 0);
                g_hookLeftDownOnDock = true;
                return 1;  // 吞掉：点击不穿透到下方窗口
            }
            if (wParam == WM_LBUTTONDOWN && InCornerZone(ms->pt, false)) {
                // 右下角：显示桌面（再点恢复）
                PostMessageW(g_state.hwnd, kMsgHookClick,
                             kHookClickShowDesktop, 0);
                g_hookLeftDownOnDock = true;
                return 1;  // 吞掉：点击不穿透到下方窗口
            }

            // 只采集 Dock/透明底部空隙/贴底边缘的点击，动作由 UI 线程执行。
            if (wParam == WM_LBUTTONDOWN && PointInDockOrStrip(ms->pt)) {
                float mx = static_cast<float>(ms->pt.x - g_state.winX);
                float my = static_cast<float>(ms->pt.y - g_state.winY);
                // 空隙处于窗口矩形下方时，按最近有效边缘处理
                if (my >= static_cast<float>(g_state.winH)) {
                    my = static_cast<float>(g_state.winH) - 1.f;
                }
                PostMessageW(
                    g_state.hwnd, kMsgHookClick, kHookClickDockLeft,
                    MAKELPARAM(static_cast<short>(mx), static_cast<short>(my)));
                g_hookLeftDownOnDock = true;
                return 1;
            }

            // 未在上面 return 的按下 = 不是 Dock/角部消费的按下：
            // 清除可能残留的旧“钩子吞掉按下”状态，避免误吞后续普通抬起。
            if (wParam == WM_LBUTTONDOWN) {
                g_hookLeftDownOnDock = false;
            }

            // 如果按下是被上面吞掉的（Dock/角部），对应的抬起也要吞掉，
            // 否则系统会把一个没有按下的“孤儿抬键”派给资源管理器等窗口，
            // 造成鼠标按钮状态错乱、点击失效。
            if (wParam == WM_LBUTTONUP && g_hookLeftDownOnDock) {
                g_hookLeftDownOnDock = false;
                return 1;
            }
        } else if (wParam == WM_MBUTTONUP && PointInDockOrStrip(ms->pt)) {
            // 与窗口内中键一致：底部空隙/贴边也能中键关闭，并立即隐藏圆点。
            // 同样只采集，投递到 UI 线程后执行（避免钩子内做窗口管理）。
            float mx = static_cast<float>(ms->pt.x - g_state.winX);
            float my = static_cast<float>(ms->pt.y - g_state.winY);
            if (my >= static_cast<float>(g_state.winH)) {
                my = static_cast<float>(g_state.winH) - 1.f;
            }
            PostMessageW(
                g_state.hwnd, kMsgHookClick, kHookClickDockMiddle,
                MAKELPARAM(static_cast<short>(mx), static_cast<short>(my)));
            return 1;
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

// WH_MOUSE_LL 钩子线程：把低层鼠标钩子从 UI 线程隔离出来。
// 这样即使 UI 线程在窗口操作/刷新上偶发阻塞，也不会把全局鼠标输入拖死；
// 鼠标钩子线程总能及时返回，键盘/鼠标不会整体失去响应。
DWORD WINAPI ShowDesktopHookThreadProc(LPVOID) {
    g_hookThreadId = GetCurrentThreadId();
    g_showDesktopHook = SetWindowsHookExW(
        WH_MOUSE_LL, ShowDesktopHookProc, GetModuleHandleW(nullptr), 0);
    if (!g_showDesktopHook) {
        Logf(L"显示桌面钩子安装失败（err=%lu）", GetLastError());
    }
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (g_showDesktopHook) {
        UnhookWindowsHookEx(g_showDesktopHook);
        g_showDesktopHook = nullptr;
    }
    return 0;
}

// 安装/卸载右下角显示桌面钩子（失败不致命：仅失去该入口）
void InstallShowDesktopHook() {
    if (g_hookThread.joinable()) return;
    g_hookThread = std::thread(ShowDesktopHookThreadProc, nullptr);
}

void UninstallShowDesktopHook() {
    g_hookLeftDownOnDock = false;  // 卸载即清空配对状态，避免重装后残留
    if (g_hookThreadId) {
        PostThreadMessageW(g_hookThreadId, WM_QUIT, 0, 0);
    }
    if (g_hookThread.joinable()) {
        g_hookThread.join();
        g_hookThread = std::thread();
    }
    g_hookThreadId = 0;
    g_showDesktopHook = nullptr;
}

// ============================== 事件驱动（替代轮询）==============================
// 原实现的“全量刷新”由定时器每 1.2s 轮询触发（EnumWindows + 全进程枚举），
// 空闲时也持续唤醒。改为 Windows 原生事件机制，空闲零唤醒：
//   1. 窗口集合变化 → SetWinEventHook（CREATE/DESTROY/SHOW/HIDE）→ 合并刷新；
//   2. Win+D 把组件最小化 → EVENT_SYSTEM_MINIMIZESTART → 立即恢复（自愈）；
//   3. explorer 重启重建任务栏 → TaskbarCreated 广播 → 立即重新隐藏；
//   4. 托盘注册表变化 → RegNotifyChangeKeyValue 阻塞等待（专用线程），
//      变化即刷新，托盘条目不依赖轮询。
// 另保留 1 次/分钟的保底刷新（防极端情况下事件丢失），功耗可忽略。

std::vector<HWINEVENTHOOK> g_winEventHooks;

void CALLBACK DockWinEventProc(HWINEVENTHOOK /*hook*/, DWORD event, HWND hwnd,
                               LONG idObject, LONG /*idChild*/,
                               DWORD /*idThread*/, DWORD /*time*/) {
    if (!g_state.hwnd) return;
    if (event == EVENT_SYSTEM_MINIMIZESTART && idObject == OBJID_WINDOW) {
        // Win+D 把套件组件最小化：立即恢复（事件级延迟 <10ms，无闪烁）
        if (IsSuiteWindowClass(hwnd)) {
            PostMessageW(g_state.hwnd, kMsgHeal, 0, 0);
        }
        return;
    }
    if (idObject != OBJID_WINDOW) return;  // 只关心窗口级事件
    PostMessageW(g_state.hwnd, kMsgRefresh, 0, 0);
}

void InstallDockWinEventHook() {
    // 创建/销毁/显示/隐藏（0x8000-0x8003 连续区间）：窗口集合变化 → 刷新条目
    HWINEVENTHOOK h1 = SetWinEventHook(
        EVENT_OBJECT_CREATE, EVENT_OBJECT_HIDE, GetModuleHandleW(nullptr),
        DockWinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT);
    // 最小化开始/结束（0x0016-0x0017 连续区间）：组件自愈
    HWINEVENTHOOK h2 = SetWinEventHook(
        EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND,
        GetModuleHandleW(nullptr), DockWinEventProc, 0, 0,
        WINEVENT_OUTOFCONTEXT);
    if (h1) g_winEventHooks.push_back(h1);
    if (h2) g_winEventHooks.push_back(h2);
    if (g_winEventHooks.empty()) {
        Logf(L"WinEvent 钩子安装失败（回退 60s 保底轮询）");
    }
}

void UninstallDockWinEventHook() {
    for (HWINEVENTHOOK h : g_winEventHooks) UnhookWinEvent(h);
    g_winEventHooks.clear();
}

// 托盘注册表阻塞式监听：RegNotifyChangeKeyValue 阻塞等待变化，
// 变化时通知主窗口刷新（托盘条目零轮询）
void TrayRegistryWatchThread() {
    for (;;) {
        HKEY root = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, kNotifyIconsKey, 0,
                          KEY_NOTIFY | KEY_READ, &root) != ERROR_SUCCESS) {
            Sleep(30000);  // 键不存在（旧系统）时低速重试
            continue;
        }
        // REG_NOTIFY_CHANGE_LAST_WRITE(0x4) | REG_NOTIFY_CHANGE_SUBKEYS(0x1)
        // （常量在部分 SDK 头配置下未导出，按 ABI 值使用）
        const DWORD filter = 0x4 | 0x1;
        // fAsynchronous=FALSE：本线程阻塞等待下一次托盘注册表变化
        const LONG hr =
            RegNotifyChangeKeyValue(root, TRUE, filter, nullptr, FALSE);
        RegCloseKey(root);
        if (hr != ERROR_SUCCESS) {
            Sleep(30000);
            continue;
        }
        PostMessageW(g_dockHwnd.load(), kMsgRefresh, 0, 0);
    }
}

void StartTrayRegistryWatch() {
    // 进程内组件可反复重启（托盘关闭后再打开）：监听线程只随进程创建一次，
    // 通过原子句柄 g_dockHwnd 追踪当前 Dock 窗口（窗口销毁期间投递自然失效）
    static std::once_flag once;
    std::call_once(once, []() {
        std::thread([]() { TrayRegistryWatchThread(); }).detach();
    });
}

// ============================== 刷新合并 ==============================

DockItem MakeItemFromKey(const std::wstring& key) {
    DockItem item;
    item.key = key;
    item.launchPath = key;
    item.displayName = ExeDisplayName(key);
    if (item.displayName.empty()) {
        item.displayName = StripExtension(PathBasename(key));
    }
    return item;
}

// 新 key 分配稳定序号：延续上一帧的出现位置，新出现的追加尾部
size_t OrdinalFor(AppState& s,
                  const std::unordered_map<std::wstring, size_t>& prevOrder,
                  const std::wstring& key) {
    auto mine = s.orderMap.find(key);
    if (mine != s.orderMap.end()) return mine->second;
    auto old = prevOrder.find(key);
    const size_t value =
        (old != prevOrder.end()) ? old->second : s.nextOrdinal++;
    s.orderMap[key] = value;
    return value;
}

// 每次 poll 重建 items：
//   固定区 → pins 顺序（未运行也显示）
//   运行区 → 有顶层窗口的应用（orderMap 稳定排序）
//           → 托盘驻留第三方（特别需求核心逻辑）
// 同一 key 多来源合并成一条。返回内容是否发生变化。
bool RefreshItems(AppState& s) {
    std::vector<std::wstring> prevKeys;
    prevKeys.reserve(s.items.size());
    for (const auto& it : s.items) prevKeys.push_back(it.key);

    std::unordered_map<std::wstring, size_t> prevOrder(s.orderMap);
    std::unordered_map<std::wstring, float> prevAnim;
    for (const auto& it : s.items) prevAnim[it.key] = it.scaleAnim;

    struct Cand {
        DockItem item;
        size_t ordinal = SIZE_MAX;
    };
    std::vector<Cand> cands;
    std::unordered_map<std::wstring, size_t> indexOfKey;

    auto getCand = [&](const std::wstring& key) -> Cand& {
        auto it = indexOfKey.find(key);
        if (it != indexOfKey.end()) return cands[it->second];
        Cand c;
        c.item = MakeItemFromKey(key);
        c.ordinal = OrdinalFor(s, prevOrder, key);
        indexOfKey[key] = cands.size();
        cands.push_back(std::move(c));
        return cands.back();
    };

    // ---- 来源 1：固定项（保持注册表顺序）----
    for (const auto& pin : s.pins) {
        PinDescriptor d = DescribePin(pin);
        if (d.key.empty()) continue;
        if (BasenameBlocked(d.key)) continue;
        Cand& c = getCand(d.key);
        c.item.pinned = true;
        c.item.launchPath = d.launchPath;
        c.item.displayName = d.name;
    }

    // ---- 来源 2：顶层窗口分组 ----
    // 注意：这里不能套用 IsSystemComponentPath —— 商店应用
    // （Terminal/计算器/记事本等）路径在 WindowsApps\Microsoft.* 下，
    // 但有可见窗口就是正常的运行应用；“系统组件”过滤只用于托盘来源。
    WindowGroupContext wgctx;
    wgctx.selfPid = GetCurrentProcessId();
    EnumWindows(EnumAppWindowProc, reinterpret_cast<LPARAM>(&wgctx));
    for (const auto& kv : wgctx.ordered) {
        const std::wstring& key = kv.first;
        // 基础设施（explorer 等）整体排除；唯一例外：文件管理器窗口 ——
        // explorer.exe 同时承载桌面/任务栏与“文件管理器”，后者是用户
        // 意义上的应用，必须进 Dock（可聚焦/最小化/关闭）
        if (BasenameBlocked(key) && !IsFileExplorerWindow(kv.second)) continue;
        Cand& c = getCand(key);
        c.item.hasWindow = true;
        c.item.windows.push_back(kv.second);
    }

    // ---- 来源 3：托盘驻留第三方 —— 特别需求核心 ----
    // 进程级全量枚举（每个进程 OpenProcess + 查询镜像路径）较耗电，
    // “是否存活”容忍 3s 缓存延迟（应用启动/退出后托盘条目 ≤3s 校正）
    const ULONGLONG nowForCache = GetTickCount64();
    if (nowForCache - s.runningExeCacheAt > kRunningPathsCacheMs) {
        s.runningExeCache = CollectRunningExePaths();
        s.runningExeCacheAt = nowForCache;
    }
    const auto& runningNow = s.runningExeCache;
    for (const auto& raw : ReadRegisteredTrayRawPaths()) {
        const std::wstring expanded = ExpandKnownFolderPrefix(raw);
        const std::wstring key = NormalizePath(expanded);
        if (key.empty()) continue;
        if (runningNow.find(key) == runningNow.end()) continue;  // 必须存活
        if (BasenameBlocked(key)) continue;
        if (IsSystemComponentPath(key)) continue;  // 排除 Windows 自带组件
        auto found = indexOfKey.find(key);
        if (found != indexOfKey.end()) {
            cands[found->second].item.trayMarked = true;
            continue;
        }
        if (s.hiddenKeys.count(key)) continue;  // 用户手动隐藏的后台项
        Cand& c = getCand(key);
        c.item.trayMarked = true;
    }

    // ---- 来源 2.5：固定项与运行组/托盘组按名称模糊合并 ----
    // 有的应用“启动器 exe”和“实际主程序 exe”不是同一个文件
    // （例如钉钉：钉钉.lnk → dingtalklauncher.exe，而运行/托盘是
    // dingtalk.exe），会导致 Dock 出现同名重复图标。这里不再只处理
    // 解析失败的 lnk，只要固定项和某个运行/托盘组显示名一致，就把
    // 运行/托盘组升级为固定项（保留 lnk 启动路径），消除重复。
    auto PinNameMatchesWindow = [](const std::wstring& pinNameLower,
                                   const std::wstring& winKey) {
        const std::wstring base =
            StripExtension(ToLower(PathBasename(winKey)));
        if (!pinNameLower.empty() && base == pinNameLower) return true;
        const std::wstring desc = ToLower(ExeDisplayName(winKey));
        return !desc.empty() && !pinNameLower.empty() &&
               desc.find(pinNameLower) != std::wstring::npos;
    };
    for (size_t i = 0; i < cands.size(); ++i) {
        Cand& p = cands[i];
        if (!p.item.pinned || p.item.hasWindow || p.item.trayMarked) continue;
        const std::wstring nameLower = ToLower(p.item.displayName);
        if (nameLower.empty()) continue;
        for (size_t j = 0; j < cands.size(); ++j) {
            if (j == i) continue;
            Cand& w = cands[j];
            if (!w.item.hasWindow && !w.item.trayMarked) continue;
            if (w.item.key == p.item.key) continue;
            if (!PinNameMatchesWindow(nameLower, w.item.key)) continue;
            w.item.pinned = true;
            w.item.launchPath = p.item.launchPath;
            w.item.displayName = p.item.displayName;
            // 每对 (固定项, 运行组) 只记录一次，避免每轮询刷日志
            static std::unordered_set<std::wstring> mergedLogged;
            if (mergedLogged.insert(p.item.displayName + L"→" +
                                    w.item.key).second) {
                Logf(L"固定项按名称并入运行组：%ls → %ls",
                     p.item.displayName.c_str(), w.item.key.c_str());
            }
            // 从候选中剔除启动器固定项本体，避免同名重复图标
            cands.erase(cands.begin() + static_cast<long>(i));
            --i;
            break;
        }
    }

    // ---- 过滤 + 排序：固定区在前（pins 序），其余按稳定序号 ----
    std::vector<Cand*> shownPtrs;
    shownPtrs.reserve(cands.size());
    for (auto& c : cands) {
        if (c.item.trayMarked && !c.item.hasWindow && !c.item.pinned &&
            s.hiddenKeys.count(c.item.key)) {
            continue;
        }
        if (c.item.pinned || c.item.hasWindow || c.item.trayMarked) {
            shownPtrs.push_back(&c);
        }
    }
    std::stable_sort(shownPtrs.begin(), shownPtrs.end(),
                     [](const Cand* a, const Cand* b) {
                         const bool pa = a->item.pinned;
                         const bool pb = b->item.pinned;
                         if (pa != pb) return pa > pb;  // 固定区排前
                         return a->ordinal < b->ordinal;
                     });

    // ---- 应用到状态 ----
    s.items.clear();
    s.items.reserve(shownPtrs.size());
    s.pinCount = 0;
    s.separatorX = -1.0f;
    for (Cand* cp : shownPtrs) {
        DockItem item = std::move(cp->item);
        auto animIt = prevAnim.find(item.key);
        item.icon = GetOrLoadIcon(s, item.key);
        if (animIt != prevAnim.end()) item.scaleAnim = animIt->second;
        if (item.pinned) ++s.pinCount;
        s.items.push_back(std::move(item));
    }

    // ---- 中键关闭抑制状态清理 ----
    // 一旦应用确实不再有窗口/托盘驻留（或条目已消失），解除抑制；
    // 若仍存活（关闭中/优雅退出尚未完成），继续保持圆点隐藏。
    for (auto it = s.closingKeys.begin(); it != s.closingKeys.end();) {
        bool stillRunning = false;
        for (const auto& item : s.items) {
            if (item.key == *it && (item.hasWindow || item.trayMarked)) {
                stillRunning = true;
                break;
            }
        }
        if (!stillRunning) {
            it = s.closingKeys.erase(it);
        } else {
            ++it;
        }
    }

    // ---- 变更检测与日志（仅状态切换的那一次 poll 会触发）----
    bool changed = prevKeys.size() != s.items.size();
    for (size_t i = 0; i < s.items.size() && !changed; ++i) {
        if (prevKeys[i] != s.items[i].key) changed = true;
    }
    if (changed) {
        for (const auto& pk : prevKeys) {
            bool stillThere = false;
            for (const auto& it : s.items) {
                if (it.key == pk) {
                    stillThere = true;
                    break;
                }
            }
            if (!stillThere) Logf(L"离开 Dock：%ls", pk.c_str());
        }
        for (const auto& it : s.items) {
            bool isNew = true;
            for (const auto& pk : prevKeys) {
                if (pk == it.key) {
                    isNew = false;
                    break;
                }
            }
            if (isNew) {
                Logf(L"进入 Dock：%ls [%ls%ls%ls]", it.displayName.c_str(),
                     it.pinned ? L"固定/" : L"", it.hasWindow ? L"窗口/" : L"",
                     it.trayMarked ? L"托盘" : L"");
            }
        }
        // 条目清单快照：排查显示问题时一目了然（顺序即槽位顺序）
        std::wstring roster = L"清单(" +
                              std::to_wstring(s.items.size()) + L"):";
        for (const auto& it : s.items) {
            roster += L" " + it.displayName.substr(0, 12) +
                      (it.icon ? L"" : L"【无图标】") + L";";
        }
        Logf(L"%ls", roster.c_str());
    }

    // ---- 弹跳完成检测 ----
    const ULONGLONG now = GetTickCount64();
    for (auto pit = s.pendingLaunches.begin();
         pit != s.pendingLaunches.end();) {
        bool done = false;
        for (const auto& it : s.items) {
            if (it.key == pit->key && (it.hasWindow || it.trayMarked)) {
                done = true;
                Logf(L"启动完成：%ls（%.1fs）", it.displayName.c_str(),
                     static_cast<double>(now - pit->startTick) / 1000.0);
                break;
            }
        }
        if (done || now - pit->startTick > kLaunchBounceMs) {
            pit = s.pendingLaunches.erase(pit);
        } else {
            ++pit;
        }
    }

    s.needsRedraw = true;
    return changed;
}

// 内容签名（FNV-1a 64）：比较后不变则跳过重绘。零分配，可每帧调用；
// 条目 key、固定/窗口/托盘标记、数量与固定数全部混入，任何内容变化
// 都会改变签名。
uint64_t MakeSignature(const AppState& s) {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&h](const void* data, size_t bytes) {
        const auto* p = static_cast<const unsigned char*>(data);
        for (size_t i = 0; i < bytes; ++i) {
            h ^= p[i];
            h *= 1099511628211ull;
        }
    };
    for (const auto& it : s.items) {
        mix(it.key.data(), it.key.size() * sizeof(wchar_t));
        const unsigned char flags =
            static_cast<unsigned char>((it.pinned ? 1 : 0) |
                                       (it.hasWindow ? 2 : 0) |
                                       (it.trayMarked ? 4 : 0));
        mix(&flags, 1);
    }
    const size_t counts[2] = {s.items.size(), s.pinCount};
    mix(counts, sizeof(counts));
    return h;
}

// ============================== 绘图缓冲 ==============================

void DestroyBacking(AppState& s) {
    delete s.surface;
    s.surface = nullptr;
    if (s.memDc) {
        if (s.dib) {
            if (s.dibOld) SelectObject(s.memDc, s.dibOld);
            DeleteObject(s.dib);
            s.dib = nullptr;
        }
        DeleteDC(s.memDc);
        s.memDc = nullptr;
    }
    s.bits = nullptr;
}

bool CreateBacking(AppState& s, int width, int height) {
    DestroyBacking(s);
    HDC screenDc = GetDC(nullptr);
    if (!screenDc) return false;
    s.memDc = CreateCompatibleDC(screenDc);
    ReleaseDC(nullptr, screenDc);
    if (!s.memDc) return false;

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    s.dib =
        CreateDIBSection(s.memDc, &bmi, DIB_RGB_COLORS, &s.bits, nullptr, 0);
    if (!s.dib) {
        DestroyBacking(s);
        return false;
    }
    s.dibOld = static_cast<HBITMAP>(SelectObject(s.memDc, s.dib));
    s.surface = new Bitmap(width, height, PixelFormat32bppPARGB);
    if (!s.surface) {
        DestroyBacking(s);
        return false;
    }
    s.bufferW = width;
    s.winH = height;
    return true;
}

void PresentSurface(AppState& s) {
    if (!s.surface || !s.memDc || !s.bits) return;
    const int presentW = std::min(s.winW, s.bufferW);
    Gdiplus::Rect lockRect(0, 0, s.bufferW, s.winH);
    BitmapData bd{};
    if (s.surface->LockBits(&lockRect, ImageLockModeRead,
                            PixelFormat32bppPARGB, &bd) != Ok) {
        return;
    }
    auto* dst = static_cast<BYTE*>(s.bits);
    const auto* src = static_cast<const BYTE*>(bd.Scan0);
    // 目标 DIB 的行距按缓冲全宽计算；源位图行距用 LockBits 返回值。
    // 呈现宽度小于缓冲宽度时两者不同，混用会导致逐行错位（花屏）。
    const size_t copyBytes = static_cast<size_t>(presentW) * 4u;
    const size_t dstStride = static_cast<size_t>(s.bufferW) * 4u;
    for (int y = 0; y < s.winH; ++y) {
        std::memcpy(dst + static_cast<size_t>(y) * dstStride,
                    src + static_cast<size_t>(y) *
                              static_cast<size_t>(bd.Stride),
                    copyBytes);
    }
    s.surface->UnlockBits(&bd);

    // ULW 同时应用位置与尺寸：悬停放大的展宽/收窄和底部居中都在这一步完成，
    // 不需要 SetWindowPos（全透明像素自动点击穿透，多出的缓冲区不参与呈现）
    POINT ptDst{s.winX, s.winY};
    POINT ptSrc{0, 0};
    SIZE size{presentW, s.winH};
    BLENDFUNCTION blend{};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;
    HDC screenDc = GetDC(nullptr);
    if (screenDc) {
        const BOOL ulwOk = UpdateLayeredWindow(
            s.hwnd, screenDc, &ptDst, &size, s.memDc, &ptSrc, 0, &blend,
            ULW_ALPHA);
        if (!ulwOk) {
            static int ulwFails = 0;
            if (ulwFails++ < 5) {
                Logf(L"UpdateLayeredWindow 失败 err=%lu size=%dx%d",
                     GetLastError(), size.cx, size.cy);
            }
        }
        ReleaseDC(nullptr, screenDc);
    }
}

// ============================== 结构尺寸与摆放 ==============================

float IdleContentWidth(float k, size_t n, bool withSeparator) {
    const float unit = kIconSize * k + kIconGapHalf * 2.f * k;
    float w = unit * static_cast<float>(n);
    if (withSeparator) w += kSepGap * k;
    return w + kBarPadX * k * 2.f;
}

bool HasSeparatorCondition(AppState& s) {
    return s.pinCount > 0 && s.pinCount < s.items.size();
}

SIZE DesiredWindowSize(AppState& s) {
    // 静止尺寸：精确贴合当前条目内容（放大展宽由每帧布局另行处理）
    const float k = s.scale;
    const float w =
        IdleContentWidth(k, s.items.size(), HasSeparatorCondition(s)) +
        kShadowMargin * k * 2.f;
    const float hGrow = (kMaxScale - 1.0f) * kIconSize * k;  // 放大增高预算
    const float h = kShadowMargin * k + kShadowBottom * k + kPadTop * k +
                    kIconSize * k + hGrow + kPadBottom * k + 4.f * k;
    RECT wa = PrimaryWorkArea();
    const int capPx = static_cast<int>((wa.right - wa.left) * 0.92f);
    SIZE sz{std::min(static_cast<int>(w + 0.5f), capPx),
            static_cast<int>(h + 0.5f)};
    return sz;
}

// 依据当前呈现宽度计算窗口应处位置（底部对齐、水平居中）
// 收起偏移并入 winY：ULW 呈现、SetWindowPos、GetWindowRect、命中检测
// 全部使用同一位置源，避免"ULW 展示收起位、SetWindowPos 拉回展开位"的
// 打架（此前导致窗口矩形跳变、几何事件风暴与展开不稳定）
void UpdateDockPosition(AppState& s) {
    RECT wa = PrimaryWorkArea();
    const int gap = MulDiv(s.bottomGapBase, s.dpi, 96);
    s.winX = wa.left + ((wa.right - wa.left) - s.winW) / 2;
    s.winY = wa.bottom - gap - s.winH +
             static_cast<int>(s.collapseOffset + 0.5f);
}

void RepositionDock(AppState& s, bool forceZOrder) {
    UpdateDockPosition(s);
    SetWindowPos(s.hwnd, forceZOrder ? HWND_TOPMOST : nullptr, s.winX, s.winY,
                 s.winW, s.winH,
                 SWP_NOACTIVATE | (forceZOrder ? 0 : SWP_NOZORDER));
}

// 结构尺寸（条目数/固定列表/DPI）变化时调整窗口与后备缓冲
void EnsureWindowSize(AppState& s) {
    SIZE want = DesiredWindowSize(s);
    if (abs(want.cx - s.lastIdleW) <= 2 && abs(want.cy - s.winH) <= 2) return;
    const int bufferW = want.cx + static_cast<int>(kMagWidthBudget * s.scale);
    if (!CreateBacking(s, bufferW, want.cy)) return;
    s.winW = want.cx;
    s.lastIdleW = want.cx;
    RepositionDock(s, true);
    s.prevCenters.clear();
    s.needsRedraw = true;
}

// 当前峰值缩放（毛玻璃随之长高：macOS 风格，悬停时整条向上生高）
float CurrentPeakScale(AppState& s) {
    float peak = 1.0f;
    for (const auto& it : s.items) {
        if (it.scaleAnim > peak) peak = it.scaleAnim;
    }
    return peak;
}

RectF BodyRectForPeak(float k, int winW, int winH, float peak) {
    const float margin = kShadowMargin * k;
    const float bottomMargin = kShadowBottom * k;
    const float grow = (peak - 1.0f) * kIconSize * k;
    const float contentH =
        kPadTop * k + grow + kIconSize * k + kPadBottom * k;
    return RectF(margin, static_cast<float>(winH) - bottomMargin - contentH,
                 static_cast<float>(winW) - margin * 2.f, contentH);
}

// 弹跳偏移：启动后的余弦弹跳（幅度随时间衰减）
float BounceOffsetFor(AppState& s, const std::wstring& key) {
    const ULONGLONG now = GetTickCount64();
    for (const auto& p : s.pendingLaunches) {
        if (p.key != key) continue;
        const double t = static_cast<double>(now - p.startTick) / 1000.0;
        constexpr double kCycle = 0.62;         // 单次弹跳周期秒
        const double u = std::fmod(t, kCycle);
        const double fade =
            std::max(0.0, 1.0 - t / (static_cast<double>(kLaunchBounceMs) /
                                     1000.0));
        return static_cast<float>(-std::sin(u / kCycle * 3.14159265) *
                                  static_cast<double>(kMaxScale) * fade *
                                  g_state.scale * 17.0);
    }
    return 0.0f;
}

// 一帧布局：
//   1. 以上一帧各图标中心为参考计算高斯放大目标并缓动 scaleAnim
//   2. 以缓动后的宽度重摆槽位（底端对齐），记录命中框/图框/中心
// 返回是否有动画仍在进行。
bool UpdateLayoutOneFrame(AppState& s) {
    const size_t n = s.items.size();
    const float k = s.scale;
    if (s.geoms.size() != n) s.geoms.assign(n, GeomSlot{});
    if (n == 0) {
        s.prevCenters.clear();
        s.separatorX = -1.0f;
        s.hoverIndex = static_cast<size_t>(-1);
        return false;
    }

    // ---- 实时游标：窗口每帧随展宽位移，事件坐标会过期，直接换算 ----
    float hitBottomY = static_cast<float>(s.winH);  // 命中区下沿（窗口客户坐标）
    {
        POINT cp{};
        GetCursorPos(&cp);
        RECT wr{};
        if (GetWindowRect(s.hwnd, &wr)) {
            s.mouseX = static_cast<float>(cp.x - wr.left);
            s.mouseY = static_cast<float>(cp.y - wr.top);
            // 每帧用物理光标位置自愈 mouseOverDock：不依赖 LL 钩子是否
            // 正好送达最底一行，贴底/空隙悬停更稳定。
            s.mouseOverDock = PointInDockOrStrip(cp);
            if (s.mouseOverDock) {
                s.wasOnDock = true;
                if (s.hideRequested) s.hideRequested = false;
            }
            // 命中区覆盖到“屏幕底边”（含 Dock 与屏幕下边缘之间的整段空隙），
            // 不再把空隙坐标向上钳制到窗口内，最低一行也能直接命中。
            // +1 让“屏幕底边这一行”（y==bottom）也落入命中区间。
            const int bottom = PrimaryWorkArea().bottom;
            hitBottomY = static_cast<float>(bottom - wr.top + 1);
        }
    }
    const float sigma = kMagnifySigma * k;

    // 悬停以低层钩子/每帧自愈的物理真值 mouseOverDock 为准。mouseInside 只是
    // 窗口 WM_MOUSEMOVE 的旁证，在分层窗口快速移动/输入队列附加异常时会残留
    // true，仅当低层钩子安装失败时才回退使用它（否则人已离开 Dock 仍横向
    // 移动会误触发悬停动画）。
    const bool pointerActive = s.mouseOverDock ||
                               (s.mouseInside && g_showDesktopHook == nullptr);

    // ---- 目标缩放（悬停生效，悬停真值由 LL 钩子按光标物理位置维护，
    //     窗口事件在边缘/静止场景不可靠）----
    float activeAny = false;
    for (size_t i = 0; i < n; ++i) {
        float target = 1.0f;
        if (pointerActive && s.prevCenters.size() == n) {
            const float d = s.mouseX - s.prevCenters[i];
            const float f = std::exp(-(d * d) / (2.f * sigma * sigma));
            target = 1.0f + (kMaxScale - 1.0f) * f;
        }
        float cur = s.items[i].scaleAnim;
        cur += (target - cur) * (target > cur ? 0.42f : 0.20f);
        if (std::fabs(target - cur) < 0.004f) cur = target;
        if (cur > 1.001f) activeAny = true;
        s.items[i].scaleAnim = cur;
    }

    // ---- 窗口宽度实时贴合内容：静止时精确包住图标，悬停随放大展宽 ----
    const size_t pinN = s.pinCount;
    const bool withSep = pinN > 0 && pinN < n;
    float used = kBarPadX * k * 2.f;
    for (size_t i = 0; i < n; ++i) {
        used += kIconSize * k * s.items[i].scaleAnim +
                kIconGapHalf * 2.f * k;
    }
    if (withSep) used += kSepGap * k;

    {
        const float margin = kShadowMargin * k;
        const int wantW = static_cast<int>(used + margin * 2.f + 0.5f);
        if (wantW > s.bufferW) {
            // 极端情况兜底：直接扩容缓冲（正常放大预算内不会走到）
            CreateBacking(s, wantW + static_cast<int>(kMagWidthBudget * k),
                          s.winH);
            s.lastIdleW = 0;  // 触发下轮 EnsureWindowSize 校准
        }
        s.winW = std::min(wantW, s.bufferW);
        UpdateDockPosition(s);
    }

    const float peak = CurrentPeakScale(s);
    const RectF body = BodyRectForPeak(k, s.winW, s.winH, peak);
    float x = body.X + std::max((body.Width - used) * 0.5f, 0.f) +
              kBarPadX * k;
    const float iconBottom = body.Y + body.Height - kPadBottom * k;

    s.separatorX = -1.0f;
    bool animLeft = activeAny;

    if (s.prevCenters.size() != n) s.prevCenters.assign(n, 0.f);

    for (size_t i = 0; i < n; ++i) {
        const DockItem& it = s.items[i];
        const float half = kIconGapHalf * k;
        const float w = kIconSize * k * it.scaleAnim;
        x += half;  // 前导半隙：与 used 的每槽 2*half 口径一致，
                    // 缺了会把每槽 6px 全部累积成右端空隙
        const float left = x;
        GeomSlot& gm = s.geoms[i];
        // 命中区从玻璃主体顶部一直延伸到屏幕底边（含透明下边缘/贴底空隙），
        // 这样光标贴近屏幕下边缘（含空隙内最低一行）仍能稳定命中并悬停到对应图标。
        gm.hit = RectF(left, body.Y + 2.f, w + half * 2.f,
                       hitBottomY - (body.Y + 2.f));
        gm.icon = RectF(left, iconBottom - w, w, w);
        gm.cx = left + w * 0.5f;
        s.prevCenters[i] = gm.cx;
        x = left + w + half;

        if (withSep && i + 1 == pinN) {
            s.separatorX = x + kSepGap * k * 0.5f;
            x += kSepGap * k;
        }
    }

    // ---- 悬停命中（含悬停时间戳维护）----
    // 使用鼠标物理位置（LL 钩子维护的 mouseOverDock）而不是仅依赖窗口
    // WM_MOUSEMOVE：透明下边缘/空隙可能不派发窗口鼠标消息，否则贴近
    // 屏幕下边缘时悬停会闪烁。
    LONG newHover = -1;
    if (pointerActive) {
        for (size_t i = 0; i < n; ++i) {
            const RectF& r = s.geoms[i].hit;
            if (s.mouseX >= r.X && s.mouseX < r.X + r.Width &&
                s.mouseY >= r.Y && s.mouseY < r.Y + r.Height) {
                newHover = static_cast<LONG>(i);
                break;
            }
        }
    }
    const size_t hoverIdx = newHover < 0 ? static_cast<size_t>(-1)
                                         : static_cast<size_t>(newHover);
    if (hoverIdx != s.hoverIndex) {
        s.hoverIndex = hoverIdx;
        s.hoverSince = GetTickCount64();
        s.needsRedraw = true;
        // 悬停名称提示是时间驱动（480ms 后需要一帧绘制）。空闲已停帧，
        // 用一次性定时器到期踢帧，替代“悬停期间持续跑帧”。
        SetTimer(s.hwnd, kTooltipTimerId,
                 static_cast<UINT>(kTooltipDelayMs), nullptr);
    }

    // 一次性布局自检日志（排查显示问题时对账：窗口宽 ↔ 首末槽位）
    static bool geomLogged = false;
    if (!geomLogged) {
        geomLogged = true;
        Logf(L"布局自检：winW=%d n=%u pinN=%u 首槽left=%.0f 末槽cx=%.0f",
             s.winW, static_cast<unsigned>(n), static_cast<unsigned>(pinN),
             s.geoms.front().icon.X, s.geoms.back().cx);
    }

    // 只返回槽位缓动是否仍在进行。悬停状态不再让每帧持续跑（空闲零
    // 唤醒的关键）：悬停提示由 kTooltipTimerId 踢帧，悬停态变化本身
    // 由鼠标事件踢帧。
    return animLeft;
}

// ============================== 绘制 ==============================

void AddRoundRect(GraphicsPath& path, const RectF& r, float radius) {
    const float d = radius * 2.f;
    path.AddArc(r.X, r.Y, d, d, 180.f, 90.f);
    path.AddArc(r.X + r.Width - d, r.Y, d, d, 270.f, 90.f);
    path.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0.f, 90.f);
    path.AddArc(r.X, r.Y + r.Height - d, d, d, 90.f, 90.f);
    path.CloseFigure();
}

void DrawGlassBody(Graphics& g, const RectF& body, float radius, float k) {
    GraphicsPath p;
    AddRoundRect(p, body, radius);

    // 竖向渐变玻璃体：整体高透近乎无色，仅底部用中性灰轻压
    // （"底部颜色有点深"，顶部完全让壁纸透过来）
    LinearGradientBrush lg(
        body,
        Color(30, 255, 255, 255),
        Color(96, 96, 100, 112),
        LinearGradientModeVertical);
    {
        Color colors[3] = {
            Color(22, 255, 255, 255),   // 顶：几乎全透
            Color(58, 180, 184, 196),   // 中：轻微灰雾
            Color(96, 92, 96, 108),     // 底：偏深的冷灰托底
        };
        REAL positions[3] = {0.0f, 0.6f, 1.0f};
        lg.SetInterpolationColors(colors, positions, 3);
    }
    g.FillPath(&lg, &p);

    // 顶缘微光带（极低 alpha，只留一丝玻璃感，避免形成白色细线）
    {
        RectF gloss(body.X, body.Y, body.Width, 8.f * k);
        LinearGradientBrush glossGrad(
            gloss, Color(42, 255, 255, 255), Color(0, 255, 255, 255),
            LinearGradientModeVertical);
        Region clipRegion(&p);
        g.SetClip(&clipRegion);
        g.FillRectangle(&glossGrad, gloss);
        g.ResetClip();
    }

    // 单一边框：仅一条低对比亮边勾出轮廓（不再叠加内沿暗线）
    {
        Pen rim(Color(78, 255, 255, 255), 1.2f * k);
        g.DrawPath(&rim, &p);
    }
}

void DrawSeparator(Graphics& g, AppState& s, float iconBottom, float k) {
    if (s.separatorX < 0) return;
    const float h = kIconSize * k * 0.58f;
    const float y0 = iconBottom - h;
    // 深色玻璃上用单条低透明度亮线，避免双线视觉杂质
    Pen line(Color(70, 255, 255, 255), 1.0f * k);
    g.DrawLine(&line, s.separatorX, y0, s.separatorX, y0 + h);
}

void DrawFrame(Graphics& g, AppState& s) {
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    // 图标源已预缩至 ≤160px（见 PreScaleIconSource），目标 ≤73px：
    // Bilinear 已足够（Bicubic 慢 ~6 倍，视觉差异在此比例可忽略）。
    g.SetInterpolationMode(InterpolationModeHighQualityBilinear);
    g.Clear(Color(0, 0, 0, 0));

    const float k = s.scale;
    const float peak = CurrentPeakScale(s);
    const RectF body = BodyRectForPeak(k, s.winW, s.winH, peak);
    const float radius = kCornerRadius * k;
    const float iconBottom = body.Y + body.Height - kPadBottom * k;
    const float dotCy = iconBottom + kDotDrop * k;

    DrawGlassBody(g, body, radius, k);
    DrawSeparator(g, s, iconBottom, k);

    const size_t n = s.items.size();

    // 第 1 层：悬停光晕（画在图标之下）
    if (s.hoverIndex < n) {
        const RectF& ib = s.geoms[s.hoverIndex].icon;
        const float cx = ib.X + ib.Width * 0.5f;
        const float cy = ib.Y + ib.Height * 0.5f;
        const float gr = ib.Width * 0.78f;
        GraphicsPath gp;
        gp.AddEllipse(cx - gr, cy - gr, gr * 2.f, gr * 2.f);
        PathGradientBrush pg(&gp);
        pg.SetCenterColor(Color(64, 255, 255, 255));
        const Color surround = Color(0, 255, 255, 255);
        INT cnt = 1;
        pg.SetSurroundColors(&surround, &cnt);
        g.FillPath(&pg, &gp);
    }

    // 第 2 层：图标（底端对齐 + 启动弹跳偏移）
    for (size_t i = 0; i < n; ++i) {
        const DockItem& it = s.items[i];
        const GeomSlot& gm = s.geoms[i];
        const float dy = BounceOffsetFor(s, it.key);
        RectF box = gm.icon;
        box.Y += dy;
        if (it.icon) {
            g.DrawImage(it.icon, box, 0.f, 0.f,
                        static_cast<REAL>(it.icon->GetWidth()),
                        static_cast<REAL>(it.icon->GetHeight()), UnitPixel,
                        nullptr);
        } else {
            // 提取失败占位：圆角方块
            GraphicsPath pp;
            AddRoundRect(pp, RectF(box.X + box.Width * .15f,
                                   box.Y + box.Width * .15f,
                                   box.Width * .7f, box.Width * .7f),
                         8.f * k);
            SolidBrush br(Color(180, 178, 182, 194));
            g.FillPath(&br, &pp);
        }
        // 第 3 层：运行圆点（深色玻璃上用白色，macOS 深色 Dock 同款）
        // 中键关闭已发出后立即抑制圆点，不等待实际退出/下一次刷新。
        if ((it.hasWindow || it.trayMarked) &&
            s.closingKeys.find(it.key) == s.closingKeys.end()) {
            const float r = 2.6f * k;
            SolidBrush dot(Color(225, 244, 246, 250));
            g.FillEllipse(&dot, gm.cx - r, dotCy - r + dy * 0.2f, r * 2.f,
                          r * 2.f);
        }
    }

    // 第 4 层：悬停名称提示
    if (s.uiFont && s.hoverIndex < n &&
        GetTickCount64() - s.hoverSince > kTooltipDelayMs) {
        const DockItem& it = s.items[s.hoverIndex];
        // 度量缓存：MeasureString 是 GDI+ 高成本操作。以完整显示名为键，
        // 截断结果只在名称或最大宽度（窗口宽随悬停展宽变化）改变时重测。
        static std::wstring cachedKey;
        static std::wstring cachedText;
        static RectF cachedMeasured;
        static float cachedMaxTextW = -1.f;
        std::wstring text = it.displayName;
        const float maxTextW = body.Width * 0.4f;
        if (text != cachedKey ||
            std::fabs(maxTextW - cachedMaxTextW) > 1.0f) {
            // 文本截断（超出 maxTextW 时逐字缩短）
            std::wstring t = text;
            for (;;) {
                RectF m{};
                g.MeasureString(t.c_str(), static_cast<INT>(t.size()),
                                s.uiFont, PointF(0.f, 0.f), &m);
                if (m.Width <= maxTextW || t.size() <= 2) break;
                t.resize(t.size() - 1);
                if (t.size() > 1) t[t.size() - 1] = L'…';
            }
            RectF m{};
            g.MeasureString(t.c_str(), static_cast<INT>(t.size()), s.uiFont,
                            PointF(0.f, 0.f), &m);
            cachedKey = text;
            cachedText = std::move(t);
            cachedMeasured = m;
            cachedMaxTextW = maxTextW;
        }
        text = cachedText;
        const RectF& measured = cachedMeasured;
        const float tw = measured.Width + 24.f * k;
        const float th = 27.f * k;
        const float tipCenterX = s.geoms[s.hoverIndex].cx;
        const float tipX = std::max(std::min(tipCenterX - tw * 0.5f,
                                             body.X + body.Width - tw -
                                                 4.f * k),
                                    body.X + 4.f * k);
        const float tipY = std::max(body.Y - th * 0.42f, 3.f * k);
        RectF pill(tipX, tipY, tw, th);
        GraphicsPath pillPath;
        AddRoundRect(pillPath, pill, th * 0.5f);
        SolidBrush pillBg(Color(228, 26, 27, 33));
        g.FillPath(&pillBg, &pillPath);
        SolidBrush fgWhite(Color(255, 245, 246, 248));
        StringFormat sf;
        sf.SetAlignment(StringAlignmentCenter);
        sf.SetLineAlignment(StringAlignmentCenter);
        g.DrawString(text.c_str(), static_cast<INT>(text.size()), s.uiFont,
                     pill, &sf, &fgWhite);
    }
}

void DrawAndPresent(AppState& s) {
    if (!s.surface) return;
    Graphics g(s.surface);
    DrawFrame(g, s);
    PresentSurface(s);
    s.needsRedraw = false;
}

// ============================== 交互动作 ==============================

int HitIndexAt(AppState& s, float x, float y) {
    for (size_t i = 0; i < s.geoms.size(); ++i) {
        const RectF& r = s.geoms[i].hit;
        if (x >= r.X && x < r.X + r.Width && y >= r.Y && y < r.Y + r.Height) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// 主窗口形态门槛：≥ 400×300（多窗口应用的辅助窗——网易云歌词/迷你/弹窗
// 等——都不达标，不会被当成“应用本体”）
constexpr long kMainWindowMinArea = 400L * 300L;

bool AppOwnsForeground(const DockItem& item) {
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    if (IsCloaked(fg)) return false;
    // 前台必须是“主窗口形态”：歌词/迷你/弹窗等辅助窗抢前台时不算聚焦态，
    // 否则点 Dock 图标会误触发“最小化全部”而非“聚焦主窗口”
    RECT rc{};
    if (!GetWindowRect(fg, &rc)) return false;
    const long area = static_cast<long>(rc.right - rc.left) *
                      static_cast<long>(rc.bottom - rc.top);
    if (area < kMainWindowMinArea) return false;
    for (HWND w : item.windows) {
        if (!IsWindow(w)) continue;
        if (fg == w) return true;
    }
    // 同进程多顶级窗口：前台属于同 exe 即算聚焦态
    DWORD fgPid = PidOf(fg);
    for (HWND w : item.windows) {
        if (!IsWindow(w)) continue;
        if (PidOf(w) == fgPid) return true;
    }
    return false;
}

// 在多窗口集合中挑选“主窗口”：面积 + 有标题评分（主窗口最大且带标题），
// 辅助窗（歌词/迷你/弹窗，面积通常 < 400×300）自然落选
HWND PickMainWindow(const std::vector<HWND>& windows) {
    HWND best = nullptr;
    long bestScore = -1;
    for (HWND w : windows) {
        if (!IsWindow(w) || !IsWindowVisible(w) || IsIconic(w) ||
            IsCloaked(w)) {
            continue;
        }
        RECT rc{};
        if (!GetWindowRect(w, &rc)) continue;
        const long area = static_cast<long>(rc.right - rc.left) *
                          static_cast<long>(rc.bottom - rc.top);
        wchar_t title[128] = {};
        GetWindowTextW(w, title, 127);
        const long sc = (title[0] ? 100 : 0) +
                        std::min(area / 10000L, 100L);
        if (sc > bestScore) {
            bestScore = sc;
            best = w;
        }
    }
    return best;
}

// 判断是否是“应用本体”主窗口形态：非工具窗、非托盘消息窗、非幽灵窗，
// 且正常几何 ≥ 400×300。最小化窗口必须用 GetWindowPlacement 的
// rcNormalPosition，因为最小化后 GetWindowRect 只剩 160×26 的任务栏图标矩形，
// 直接按 GetWindowRect 会把多个最小化主窗全部误判成辅助小窗。
bool IsMainShapeWindow(HWND hwnd) {
    if (!IsWindow(hwnd) || IsCloaked(hwnd)) return false;
    if (GetWindow(hwnd, GW_OWNER) != nullptr) return false;  // 阴影/水印等属主窗
    wchar_t cls[64] = {};
    GetClassNameW(hwnd, cls, 63);
    if (wcsstr(cls, L"TrayIconMessage") != nullptr) return false;
    const LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if ((ex & WS_EX_TOOLWINDOW) != 0 && (ex & WS_EX_APPWINDOW) == 0) {
        return false;
    }
    long w = 0;
    long h = 0;
    if (IsIconic(hwnd)) {
        WINDOWPLACEMENT wp{};
        wp.length = sizeof(wp);
        if (!GetWindowPlacement(hwnd, &wp)) return false;
        w = wp.rcNormalPosition.right - wp.rcNormalPosition.left;
        h = wp.rcNormalPosition.bottom - wp.rcNormalPosition.top;
    } else {
        RECT rc{};
        if (!GetWindowRect(hwnd, &rc)) return false;
        w = rc.right - rc.left;
        h = rc.bottom - rc.top;
    }
    return w > 0 && h > 0 && w * h >= kMainWindowMinArea;
}

void LaunchItem(AppState& s, size_t idx) {
    if (idx >= s.items.size()) return;
    DockItem& item = s.items[idx];
    HINSTANCE h = ShellExecuteW(s.hwnd, L"open", item.launchPath.c_str(),
                                nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(h) <= 32) {
        Logf(L"启动失败(%d)：%ls",
             static_cast<int>(reinterpret_cast<INT_PTR>(h)),
             item.launchPath.c_str());
        return;
    }
    Logf(L"启动请求：%ls → %ls", item.displayName.c_str(),
         item.launchPath.c_str());
    s.pendingLaunches.push_back(PendingLaunch{item.key, GetTickCount64()});
    s.needsRedraw = true;
    SetFrameCadence(s, true);  // 空闲已停帧：启动弹跳动画需要帧驱动
}

// 按 exe 镜像路径（归一化小写 key）匹配该应用的全部存活 pid。
// key 若为“UI 宿主应用”（如 steam.exe），其辅助进程（steamwebhelper.exe）
// 的 pid 一并返回 —— 关闭/终止必须覆盖承载真实主窗口的辅助进程，
// 否则杀掉宿主后辅助进程残留、窗口仍挂在屏幕上。
std::vector<DWORD> FindPidsByKey(const std::wstring& key) {
    std::vector<DWORD> pids;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return pids;

    // 一次快照同时收集 pid→镜像路径 与 pid→父pid（宿主归并沿链用）
    struct ProcInfo {
        std::wstring img;
        DWORD parent = 0;
    };
    std::unordered_map<DWORD, ProcInfo> procs;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            ProcInfo info;
            info.parent = pe.th32ParentProcessID;
            HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,
                                      FALSE, pe.th32ProcessID);
            if (proc) {
                wchar_t img[MAX_PATH] = {};
                DWORD sz = MAX_PATH;
                if (QueryFullProcessImageNameW(proc, 0, img, &sz) && sz > 0) {
                    info.img = NormalizePath(img);
                }
                CloseHandle(proc);
            }
            if (!info.img.empty()) procs[pe.th32ProcessID] = std::move(info);
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);

    const std::wstring keyBase = ToLower(PathBasename(key));
    for (const auto& kv : procs) {
        const DWORD pid = kv.first;
        const ProcInfo& info = kv.second;
        if (info.img == key) {
            pids.push_back(pid);
            continue;
        }
        // 辅助进程：基名命中映射 helper 且父链可达宿主 key 才算同应用
        const UiHostMapping* hostMap =
            FindUiHostMapping(ToLower(PathBasename(info.img)));
        if (!hostMap || keyBase != hostMap->appBase) continue;
        DWORD cur = pid;
        for (int depth = 0; depth < 8; ++depth) {
            auto it = procs.find(cur);
            if (it == procs.end() || it->second.parent == 0) break;
            cur = it->second.parent;
            auto pit = procs.find(cur);
            if (pit != procs.end() && pit->second.img == key) {
                pids.push_back(pid);
                break;
            }
        }
    }
    return pids;
}

// 关闭应用：给其全部窗口发 WM_CLOSE 优雅退出，轮询约 1.5s
// 仍存活则 TerminateProcess（与 MyWigets 关闭组件策略一致）。
// 特殊：explorer.exe 是系统 shell（桌面/任务栏/托盘宿主）——“关闭应用”
// 只能关闭其用户意义上的窗口（文件管理器 CabinetWClass），绝不终止进程，
// 否则会把整个资源管理器杀掉（桌面/任务栏全部重建）。
void CloseAppByKey(const std::wstring& key, const std::wstring& displayName) {
    std::vector<DWORD> pids = FindPidsByKey(key);
    if (pids.empty()) {
        Logf(L"关闭 %ls：未在运行", displayName.c_str());
        return;
    }

    const std::wstring baseLower = ToLower(PathBasename(key));
    const bool isShellHost = baseLower == L"explorer.exe";  // 系统 shell 宿主
    const bool isInfra = IsInfraBaseline(baseLower);        // 其余基础设施进程

    // 1. 给进程的“主窗口形态”窗口发 WM_CLOSE（可见或隐藏但几何完整、
    //    面积 ≥ 400×300 —— 多窗口应用的歌词/迷你/弹窗/0×0 消息窗不再被
    //    轰炸；隐藏的大几何主窗仍优雅关闭）；shell 宿主只关文件管理器窗口
    for (DWORD pid : pids) {
        HANDLE tsnap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (tsnap == INVALID_HANDLE_VALUE) break;
        THREADENTRY32 te{};
        te.dwSize = sizeof(te);
        const LPARAM onlyFileExplorer = isShellHost ? 1 : 0;
        if (Thread32First(tsnap, &te)) {
            do {
                if (te.th32OwnerProcessID != pid) continue;
                EnumThreadWindows(
                    te.th32ThreadID,
                    [](HWND hwnd, LPARAM lp) -> BOOL {
                        if (lp && !IsFileExplorerWindow(hwnd)) {
                            return TRUE;  // 系统窗口不关（任务栏等）
                        }
                        if (IsCloaked(hwnd)) return TRUE;  // UWP 幽灵窗
                        RECT rc{};
                        if (!GetWindowRect(hwnd, &rc)) return TRUE;
                        const long area =
                            static_cast<long>(rc.right - rc.left) *
                            static_cast<long>(rc.bottom - rc.top);
                        if (area < kMainWindowMinArea) {
                            return TRUE;  // 辅助窗（歌词/迷你/弹窗/消息窗）
                        }
                        PostMessageW(hwnd, WM_CLOSE, 0, 0);
                        return TRUE;
                    },
                    onlyFileExplorer);
            } while (Thread32Next(tsnap, &te));
        }
        CloseHandle(tsnap);
    }
    Logf(L"关闭 %ls（已发 WM_CLOSE，等待退出…）", displayName.c_str());

    if (isShellHost || isInfra) {
        // 系统进程绝不等待退出、绝不强杀：窗口已发关闭（文件管理器会自行
        // 关闭），进程（explorer）必须继续驻留
        Logf(L"关闭 %ls：系统进程，不终止（仅关闭文件管理器窗口）",
             displayName.c_str());
        return;
    }

    // 2+3. 后台轮询等待退出，超时强杀 —— 同步等待会让中键后的 Dock
    //    冻结约 1.5s（窗口不重绘、不响应鼠标），改为后台线程，UI 立即恢复
    std::thread(
        [key, displayName]() {
            // 2. 轮询等待退出（最多 1.5s）
            for (int i = 0; i < 15; ++i) {
                Sleep(100);
                if (FindPidsByKey(key).empty()) {
                    Logf(L"关闭 %ls：已退出", displayName.c_str());
                    return;
                }
            }
            // 3. 超时强杀
            for (DWORD pid : FindPidsByKey(key)) {
                HANDLE proc = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
                if (proc) {
                    TerminateProcess(proc, 1);
                    CloseHandle(proc);
                }
            }
            Logf(L"关闭 %ls：超时已强制结束", displayName.c_str());
        })
        .detach();
}

// 中键关闭入口：先标记“关闭中”并立即重绘（运行圆点立刻消失），
// 再执行实际关闭；窗口内与低层钩子共用同一路径，行为一致。
void RequestCloseByIndex(AppState& s, size_t idx) {
    if (idx >= s.items.size()) return;
    DockItem& item = s.items[idx];
    s.closingKeys.insert(item.key);
    s.needsRedraw = true;
    DrawAndPresent(s);
    CloseAppByKey(item.key, item.displayName);
}

// ============ 托盘图标触发（Win11 XAML 托盘）============
// 参考桌面 TrayList 项目：Win11 任务栏托盘图标是 XAML 元素（无 HWND、
// 无 ToolbarWindow32），但 UIA 树暴露其元素且支持 Invoke /
// LegacyIAccessible.DoDefaultAction —— 等价于点击托盘图标，让应用自己
// 恢复主窗口。这是"仅托盘驻留"应用（微信主窗被销毁等场景）唯一可靠的
// 打开方式。任务栏必须可见（UIA 树才存在），由调用方负责临时显示。
//
// 为什么必须走这条路（而不是 ShowWindow 强显）：
// 微信 4.0（Weixin.exe，Qt 5.15 自绘无边框）主窗口的输入管线只有在应用
// 自身 show()/activateWindow() 时才会挂载。外部 ShowWindow 只翻转
// WS_VISIBLE 位：窗口渲染正常（消息还在刷新、未读角标还在跳），但点击 /
// 键盘全部无响应 —— “看得见点不动”的影子窗口，且微信随后还会检测到
// 外部篡改而销毁重建窗口。唯一可靠的恢复入口就是它自己的托盘图标处理器。

// 托盘图标元素匹配：主判据 = 元素 ProcessId ∈ 目标应用 pid；
// 兜底判据 = 元素 Name（工具提示）与显示名互相包含（含微信中文名别名）
bool TrayElementNameMatches(const std::wstring& hint,
                            const std::wstring& name) {
    if (hint.empty() || name.empty()) return false;
    if (name.find(hint) != std::wstring::npos) return true;
    if (hint.find(name) != std::wstring::npos) return true;
    const std::wstring lowerHint = ToLower(hint);
    if (lowerHint.find(L"weixin") != std::wstring::npos ||
        lowerHint.find(L"wechat") != std::wstring::npos) {
        // 微信的产品名是中文“微信”，显示名是英文 Weixin
        static const wchar_t* kWxNames[] = {L"微信", L"wechat", L"WeChat"};
        for (const wchar_t* a : kWxNames) {
            if (name.find(a) != std::wstring::npos) return true;
        }
    }
    return false;
}

// 单次 UIA 扫描：在任务栏 UIA 树中找目标应用的托盘图标并触发。
// 返回 true 表示已成功发出 Invoke / DoDefaultAction（应用将自行恢复）。
bool UiTrayIconInvokeOnce(IUIAutomation* uia, const std::wstring& nameHint,
                          const std::vector<DWORD>& pids) {
    if (!uia) return false;
    bool done = false;

    IUIAutomationElement* root = nullptr;
    if (FAILED(uia->GetRootElement(&root)) || !root) return false;

    VARIANT vCls{};
    vCls.vt = VT_BSTR;
    vCls.bstrVal = SysAllocString(L"Shell_TrayWnd");
    IUIAutomationCondition* cond = nullptr;
    if (SUCCEEDED(uia->CreatePropertyCondition(UIA_ClassNamePropertyId, vCls,
                                               &cond)) &&
        cond) {
        IUIAutomationElement* tray = nullptr;
        if (SUCCEEDED(root->FindFirst(TreeScope_Children, cond, &tray)) &&
            tray) {
            IUIAutomationCondition* trueCond = nullptr;
            if (SUCCEEDED(uia->CreateTrueCondition(&trueCond)) && trueCond) {
                IUIAutomationElementArray* els = nullptr;
                if (SUCCEEDED(tray->FindAll(TreeScope_Descendants, trueCond,
                                            &els)) &&
                    els) {
                    int len = 0;
                    els->get_Length(&len);
                    for (int i = 0; i < len && !done; ++i) {
                        IUIAutomationElement* el = nullptr;
                        if (FAILED(els->GetElement(i, &el)) || !el) continue;

                        // 只看托盘按钮元素（SystemTray.NormalButton /
                        // SystemTray.AccentButton 等）
                        BSTR cls = nullptr;
                        el->get_CurrentClassName(&cls);
                        const bool isTrayBtn =
                            cls && wcsstr(cls, L"SystemTray.") != nullptr;
                        if (cls) SysFreeString(cls);
                        if (!isTrayBtn) {
                            el->Release();
                            continue;
                        }

                        bool matched = false;
                        int pid = 0;
                        if (SUCCEEDED(el->get_CurrentProcessId(&pid))) {
                            const DWORD dpid = static_cast<DWORD>(pid);
                            for (DWORD p : pids) {
                                if (p == dpid) {
                                    matched = true;
                                    break;
                                }
                            }
                        }
                        if (!matched) {
                            BSTR nm = nullptr;
                            if (SUCCEEDED(el->get_CurrentName(&nm)) && nm) {
                                matched = TrayElementNameMatches(
                                    nameHint, nm);
                                SysFreeString(nm);
                            }
                        }
                        if (!matched) {
                            el->Release();
                            continue;
                        }

                        // InvokePattern 优先，失败退 LegacyIAccessible
                        IUIAutomationInvokePattern* inv = nullptr;
                        if (SUCCEEDED(el->GetCurrentPatternAs(
                                         UIA_InvokePatternId,
                                         IID_PPV_ARGS(&inv))) &&
                            inv) {
                            inv->Invoke();
                            inv->Release();
                            done = true;
                        } else {
                            IUIAutomationLegacyIAccessiblePattern* leg =
                                nullptr;
                            if (SUCCEEDED(el->GetCurrentPatternAs(
                                             UIA_LegacyIAccessiblePatternId,
                                             IID_PPV_ARGS(&leg))) &&
                                leg) {
                                leg->DoDefaultAction();
                                leg->Release();
                                done = true;
                            }
                        }
                        el->Release();
                    }
                    els->Release();
                }
                trueCond->Release();
            }
            tray->Release();
        }
        cond->Release();
    }
    VariantClear(&vCls);
    root->Release();
    return done;
}

// 托盘图标触发（异步）：临时显示任务栏 → 后台线程重试 UIA 触发 →
// 完成后通知主线程收回任务栏（kMsgTriggerDone）。
// 原同步实现把“显示任务栏 → UIA 查找/重试 → 收回”整个流程卡在点击线程上
// （最长 3.5s），期间 Dock 冻结、任务栏长时间闪现 —— “打开很慢”的主因。
bool g_skipTrayTriggerOnce = false;  // 触发失败后的二次回退：直接评分强显

void TrayIconTriggerAsync(AppState& s, const std::wstring& nameHint,
                          const std::vector<DWORD>& pids, size_t itemIdx) {
    if (pids.empty()) return;
    s.taskbarShowUntil = GetTickCount64() + kTrayTriggerWindowMs;
    ShowTaskbar();  // 任务栏可见 + 工作区还原为任务栏占位值

    std::thread([nameHint, pids, itemIdx]() {
        bool ok = false;
        IUIAutomation* uia = nullptr;
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE) {
            if (SUCCEEDED(CoCreateInstance(CLSID_CUIAutomation, nullptr,
                                           CLSCTX_INPROC_SERVER,
                                           IID_PPV_ARGS(&uia))) &&
                uia) {
                const ULONGLONG until =
                    GetTickCount64() + kTrayTriggerWindowMs;
                while (!ok && GetTickCount64() < until) {
                    ok = UiTrayIconInvokeOnce(uia, nameHint, pids);
                    if (!ok) {
                        // XAML 托盘树在任务栏显示后有一个装配延迟，重试
                        Sleep(250);
                    }
                }
                uia->Release();
            }
            if (SUCCEEDED(hr)) CoUninitialize();
        }
        Logf(ok ? L"托盘图标触发成功：%ls（等待应用自行恢复）"
                : L"托盘图标触发失败：%ls",
             nameHint.c_str());
        PostMessageW(g_dockHwnd.load(), kMsgTriggerDone, ok ? 1 : 0,
                     static_cast<LPARAM>(itemIdx));
    }).detach();
}

// Dock 已收集窗口里没有可打开的“主窗口形态”时（全隐藏/纯托盘/仅剩辅助小窗），
// 在这里通过进程线程枚举补找隐藏窗口并打开全部；仍区分托盘图标驻留：
// 纯托盘场景（微信等 Qt 自绘主窗完全隐藏）不能外部 ShowWindow —— 那会得到
// “看得见点不动”的影子窗口，必须继续走托盘图标触发，由应用自己恢复。
void WakeTrayOnlyApp(AppState& s, size_t idx) {
    DockItem& item = s.items[idx];

    // 找该应用的全部存活 pid（按 exe 镜像路径匹配 key）
    std::vector<DWORD> pids = FindPidsByKey(item.key);

    // 收集该应用全部“有界面”窗口。钉钉等进程动辄几十个隐藏窗口
    // （消息窗/阴影窗/工具窗），必须按“像不像主窗口”过滤，
    // 不能取枚举到的第一个 —— 那往往是 0×0 的消息窗，唤起了也没效果。
    struct Candidate {
        HWND hwnd = nullptr;
        bool visible = false;
        bool tool = false;    // WS_EX_TOOLWINDOW 且非 APPWINDOW
        bool hasTitle = false;
        long area = 0;        // 像素面积
    };
    std::vector<Candidate> cands;

    for (DWORD pid : pids) {
        HANDLE tsnap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (tsnap == INVALID_HANDLE_VALUE) break;
        THREADENTRY32 te{};
        te.dwSize = sizeof(te);
        if (Thread32First(tsnap, &te)) {
            do {
                if (te.th32OwnerProcessID != pid) continue;
                EnumThreadWindows(
                    te.th32ThreadID,
                    [](HWND hwnd, LPARAM lp) -> BOOL {
                        auto* out =
                            reinterpret_cast<std::vector<Candidate>*>(lp);
                        wchar_t cls[40] = {};
                        GetClassNameW(hwnd, cls, 39);
                        if (wcsstr(cls, L"Ghost") ||
                            wcsstr(cls, L"Island")) {
                            return TRUE;  // UWP 幽灵窗
                        }
                        // 托盘回调消息窗（微信 WxTrayIconMessageWindow 等）
                        // 尺寸往往比主窗口还大却只是消息接收器，必须排除
                        if (wcsstr(cls, L"TrayIconMessage")) return TRUE;
                        if (IsShellSystemClass(hwnd)) return TRUE;
                        RECT rc{};
                        if (!GetWindowRect(hwnd, &rc)) return TRUE;
                        const long w = rc.right - rc.left;
                        const long h = rc.bottom - rc.top;
                        if (w <= 0 || h <= 0) return TRUE;  // 无界面消息窗
                        const LONG_PTR ex =
                            GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
                        Candidate c;
                        c.hwnd = hwnd;
                        c.visible = IsWindowVisible(hwnd) != FALSE;
                        c.tool = (ex & WS_EX_TOOLWINDOW) != 0 &&
                                 (ex & WS_EX_APPWINDOW) == 0;
                        wchar_t title[128] = {};
                        GetWindowTextW(hwnd, title, 127);
                        c.hasTitle = title[0] != L'\0';
                        c.area = w * h;
                        out->push_back(c);
                        return TRUE;
                    },
                    reinterpret_cast<LPARAM>(&cands));
            } while (Thread32Next(tsnap, &te));
        }
        CloseHandle(tsnap);
    }

    auto IsQtClass = [](HWND hwnd) -> bool {
        wchar_t cls[64] = {};
        GetClassNameW(hwnd, cls, 63);
        return wcsstr(cls, L"Qt5") != nullptr ||
               wcsstr(cls, L"Qt6") != nullptr;
    };

    // 先筛出“主窗口形态”的窗口：这就是要打开的全部窗口集合。
    std::vector<HWND> mains;
    for (const Candidate& c : cands) {
        if (c.tool) continue;
        if (IsMainShapeWindow(c.hwnd)) mains.push_back(c.hwnd);
    }

    // 没有任何主窗口形态（纯后台/消息窗/极小辅助窗）：回退旧版评分唤起，
    // 避免遗漏个别特殊情况。
    if (mains.empty()) {
        auto ScoreOf = [](const Candidate& c) -> long {
            long sc = 0;
            if (!c.tool) sc += 1000;
            if (c.hasTitle) sc += 100;
            if (c.visible) sc += 10;
            sc += std::min(c.area / 10000L, 100L);
            return sc;
        };
        HWND best = nullptr;
        long bestScore = -1;
        for (const Candidate& c : cands) {
            const long sc = ScoreOf(c);
            if (sc > bestScore) {
                bestScore = sc;
                best = c.hwnd;
            }
        }
        if (best) {
            const bool qt = IsQtClass(best);
            if (IsIconic(best)) {
                DWORD_PTR dummy = 0;
                SendMessageTimeoutW(best, WM_SYSCOMMAND, SC_RESTORE, 0,
                                    SMTO_ABORTIFHUNG | SMTO_BLOCK, 500,
                                    &dummy);
                ForceForegroundWindow(best);
                Logf(L"恢复任务栏窗口：%ls", item.displayName.c_str());
                return;
            }
            if (!IsWindowVisible(best)) {
                if (qt && item.trayMarked && !pids.empty() &&
                    !g_skipTrayTriggerOnce) {
                    TrayIconTriggerAsync(s, item.displayName, pids, idx);
                    return;
                }
                ShowWindow(best, SW_SHOW);
                ForceForegroundWindow(best);
                Logf(L"唤起托盘应用主窗口：%ls", item.displayName.c_str());
                return;
            }
        }
        if (!pids.empty()) {
            Logf(L"托盘应用无可唤起窗口（纯后台驻留）：%ls",
                 item.displayName.c_str());
            return;
        }
        LaunchItem(s, idx);
        return;
    }

    // 托盘图标驻留 + Qt 隐藏主窗：必须走应用自己的托盘图标处理器。
    // 外部 ShowWindow 对这类窗口会得到影子窗口（无法交互），
    // 由 TrayIconTriggerAsync 临时显示任务栏并触发 UIA Invoke。
    bool hasHiddenQtMain = false;
    for (HWND w : mains) {
        if (!IsWindowVisible(w) && !IsIconic(w) && IsQtClass(w)) {
            hasHiddenQtMain = true;
            break;
        }
    }
    if (item.trayMarked && hasHiddenQtMain && !pids.empty() &&
        !g_skipTrayTriggerOnce) {
        TrayIconTriggerAsync(s, item.displayName, pids, idx);
        return;
    }

    // 打开全部主窗口：最小化的走 SC_RESTORE（任务栏语义，Qt 也安全），
    // 完全隐藏的非 Qt 窗口直接显示。
    int restored = 0;
    for (HWND w : mains) {
        if (!IsWindow(w) || IsCloaked(w)) continue;
        if (IsIconic(w)) {
            DWORD_PTR dummy = 0;
            SendMessageTimeoutW(w, WM_SYSCOMMAND, SC_RESTORE, 0,
                                SMTO_ABORTIFHUNG | SMTO_BLOCK, 500,
                                &dummy);
            ++restored;
        } else if (!IsWindowVisible(w)) {
            ShowWindow(w, SW_SHOW);
            ++restored;
        }
    }

    // 选一个主窗口抢前台（不抢的话用户可能以为没反应）；
    // 其余窗口已经在 Z 序顶部，用户可见。
    HWND target = PickMainWindow(mains);
    if (!target) {
        for (HWND w : mains) {
            if (IsWindow(w) && IsWindowVisible(w) && !IsIconic(w)) {
                target = w;
                break;
            }
        }
    }
    if (!target && !mains.empty()) target = mains.front();
    if (target) {
        ForceForegroundWindow(target);
        Logf(L"打开全部窗口：%ls（共 %zu 个，恢复 %d 个）",
             item.displayName.c_str(), mains.size(), restored);
    } else {
        Logf(L"打开全部窗口：%ls（共 %zu 个，无法聚焦）",
             item.displayName.c_str(), mains.size());
    }
}

void ToggleFocusOrLaunch(AppState& s, size_t idx) {
    SetFrameCadence(s, true);  // 空闲已停帧：点击反馈（圆点/悬停态）需要帧
    if (idx >= s.items.size()) return;
    DockItem& item = s.items[idx];

    // 应用已在前台：点击 = 最小化该应用全部可见窗口（保留原有切换语义）。
    if (AppOwnsForeground(item)) {
        for (HWND w : item.windows) {
            if (IsWindow(w) && IsWindowVisible(w) && !IsIconic(w)) {
                ShowWindow(w, SW_MINIMIZE);
            }
        }
        Logf(L"最小化：%ls", item.displayName.c_str());
        s.needsRedraw = true;
        return;
    }

    // 优先使用 Dock 已收集的顶层窗口（item.windows 已包含 UWP 的
    // ApplicationFrameWindow 等真实应用窗口，不能只靠按 exe 枚举线程窗口）。
    // 这里一次性恢复全部“主窗口形态”的窗口，而不是只唤起一个。
    std::vector<HWND> mains;
    for (HWND w : item.windows) {
        if (IsWindow(w) && IsMainShapeWindow(w)) mains.push_back(w);
    }

    // 如果收集到的是隐藏的 Qt 主窗（通常是刚隐藏、Dock 还没刷新的托盘应用），
    // 不能外部 ShowWindow——会得到“看得见点不动”的影子窗口。交给
    // WakeTrayOnlyApp 走托盘图标触发。
    if (item.trayMarked) {
        for (HWND w : mains) {
            if (!IsWindowVisible(w) && !IsIconic(w)) {
                wchar_t cls[64] = {};
                GetClassNameW(w, cls, 63);
                if (wcsstr(cls, L"Qt5") != nullptr ||
                    wcsstr(cls, L"Qt6") != nullptr) {
                    WakeTrayOnlyApp(s, idx);
                    s.needsRedraw = true;
                    return;
                }
            }
        }
    }

    if (!mains.empty()) {
        int restored = 0;
        for (HWND w : mains) {
            if (!IsWindow(w) || IsCloaked(w)) continue;
            if (IsIconic(w)) {
                DWORD_PTR dummy = 0;
                SendMessageTimeoutW(w, WM_SYSCOMMAND, SC_RESTORE, 0,
                                    SMTO_ABORTIFHUNG | SMTO_BLOCK, 500,
                                    &dummy);
                ++restored;
            } else if (!IsWindowVisible(w)) {
                ShowWindow(w, SW_SHOW);
                ++restored;
            }
        }

        HWND target = PickMainWindow(mains);
        if (!target) {
            for (HWND w : mains) {
                if (IsWindow(w) && IsWindowVisible(w) && !IsIconic(w)) {
                    target = w;
                    break;
                }
            }
        }
        if (!target && !mains.empty()) target = mains.front();
        if (target) {
            ForceForegroundWindow(target);
        }
        Logf(L"打开全部窗口：%ls（共 %zu 个，恢复 %d 个）",
             item.displayName.c_str(), mains.size(), restored);
        s.needsRedraw = true;
        return;
    }

    // item.windows 里没有可打开的主窗口形态（全隐藏/纯托盘/仅剩辅助小窗）：
    // 交给 WakeTrayOnlyApp 做隐藏窗口评分/托盘图标触发/启动。
    WakeTrayOnlyApp(s, idx);
    s.needsRedraw = true;
}

void CloseAllWindowsOf(DockItem& item) {
    int sent = 0;
    for (HWND w : item.windows) {
        if (IsWindow(w)) {
            PostMessageW(w, WM_CLOSE, 0, 0);
            ++sent;
        }
    }
    Logf(L"关闭 %ls 的 %d 个窗口", item.displayName.c_str(), sent);
}

void TogglePinByKey(AppState& s, const DockItem& item) {
    // 已固定：按解析 key 反查删除（兼容 lnk 固定来源）
    size_t removeAt = SIZE_MAX;
    for (size_t i = 0; i < s.pins.size(); ++i) {
        PinDescriptor d = DescribePin(s.pins[i]);
        if (d.key == item.key) {
            removeAt = i;
            break;
        }
    }
    if (removeAt != SIZE_MAX) {
        s.pins.erase(s.pins.begin() + static_cast<long>(removeAt));
        Logf(L"取消固定：%ls", item.displayName.c_str());
    } else {
        s.pins.push_back(item.launchPath);
        Logf(L"固定：%ls（%ls）", item.displayName.c_str(),
             item.launchPath.c_str());
    }
    ClearPinCache();
    SaveConfig(s);
    RefreshItems(s);
    EnsureWindowSize(s);
    s.needsRedraw = true;
}

void HideTrayItem(AppState& s, const DockItem& item) {
    s.hiddenKeys.insert(item.key);
    SaveConfig(s);
    Logf(L"隐藏托盘项：%ls", item.displayName.c_str());
    RefreshItems(s);
    EnsureWindowSize(s);
    s.needsRedraw = true;
}

// 固定一个应用（资源管理器拖入 WM_DROPFILES / 启动台拖入 WM_COPYDATA）：
// 校验 .exe/.lnk、解析描述、去重、入库 + 保存 + 刷新布局。
// 返回 true = 已新增固定。
bool PinPathIfNew(AppState& s, const std::wstring& path) {
    const std::wstring lower = ToLower(path);
    const bool acceptable =
        lower.size() > 4 &&
        (lower.rfind(L".exe") == lower.size() - 4 ||
         lower.rfind(L".lnk") == lower.size() - 4);
    if (!acceptable) return false;
    const PinDescriptor d = DescribePin(path);
    for (const auto& existing : s.pins) {
        if (DescribePin(existing).key == d.key) {
            Logf(L"固定请求重复：%ls", path.c_str());
            return false;  // 已固定
        }
    }
    if (s.pins.size() >= 120) {
        Logf(L"固定失败（上限 120）：%ls", path.c_str());
        return false;
    }
    s.pins.push_back(path);
    ClearPinCache();
    SaveConfig(s);
    RefreshItems(s);
    EnsureWindowSize(s);
    RepositionDock(s, true);
    Logf(L"固定：%ls", path.c_str());
    return true;
}

// ============================== 右键菜单 ==============================

void ResetMenuEntries(AppState& s) {
    s.menuCount = 0;
    for (int i = 0; i < 512; ++i) s.menu[i] = MenuEntry{};
}

int PushMenuEntry(AppState& s, ItemAction action, size_t itemIdx,
                  HWND window) {
    if (s.menuCount >= 512) return kMenuBaseId;
    s.menu[s.menuCount] = MenuEntry{action, itemIdx, window};
    return kMenuBaseId + s.menuCount++;
}

std::wstring WindowTitleForMenu(HWND hwnd) {
    wchar_t buf[128] = {};
    GetWindowTextW(hwnd, buf, 127);
    std::wstring t(buf);
    if (t.size() > 42) t = t.substr(0, 41) + L"…";
    return t.empty() ? L"(无标题窗口)" : t;
}

void ShowItemContextMenu(AppState& s, size_t idx) {
    if (s.menuOpen) return;  // 防止菜单未关闭时重复右键造成嵌套模态循环/卡死
    DockItem& item = s.items[idx];
    ResetMenuEntries(s);
    HMENU menu = CreatePopupMenu();

    // 窗口操作
    if (item.hasWindow) {
        const bool focused = AppOwnsForeground(item);
        const int actId =
            PushMenuEntry(s, ItemAction::ToggleFocus, idx, nullptr);
        AppendMenuW(menu, MF_STRING, actId,
                    focused ? L"最小化所有窗口" : L"打开全部窗口");
        if (item.windows.size() > 1) {
            HMENU wins = CreatePopupMenu();
            for (HWND w : item.windows) {
                if (!IsWindow(w)) continue;
                const int wid =
                    PushMenuEntry(s, ItemAction::SwitchToWindow, idx, w);
                AppendMenuW(wins, MF_STRING, wid,
                            WindowTitleForMenu(w).c_str());
            }
            AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(wins),
                        L"切换到窗口");
        }
        const int closeId =
            PushMenuEntry(s, ItemAction::CloseAllWindows, idx, nullptr);
        AppendMenuW(menu, MF_STRING, closeId, L"关闭应用");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    } else if (!item.pinned && !item.trayMarked) {
        // 无窗口、非托盘（理论少见）：直接给出启动入口
        const int runId = PushMenuEntry(s, ItemAction::Launch, idx, nullptr);
        AppendMenuW(menu, MF_STRING, runId, L"打开");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    } else if (!item.hasWindow && item.trayMarked) {
        // 纯托盘驻留：进程级关闭
        const int closeId =
            PushMenuEntry(s, ItemAction::CloseAllWindows, idx, nullptr);
        AppendMenuW(menu, MF_STRING, closeId, L"关闭应用");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    }

    // 固定 / 取消固定
    {
        const int pinId = PushMenuEntry(s, ItemAction::TogglePin, idx, nullptr);
        AppendMenuW(menu, MF_STRING, pinId,
                    item.pinned ? L"从 Dock 取消固定" : L"固定到 Dock");
    }

    // 仅托盘驻留项提供隐藏入口
    if (item.trayMarked && !item.pinned) {
        const int hidId =
            PushMenuEntry(s, ItemAction::HideTrayItem, idx, nullptr);
        AppendMenuW(menu, MF_STRING, hidId, L"从 Dock 隐藏此后台程序");
    }

    // 打开文件位置（固定项有意义：定位到 lnk/exe）
    if (GetFileAttributesW(item.launchPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
        const int showId =
            PushMenuEntry(s, ItemAction::ShowInExplorer, idx, nullptr);
        AppendMenuW(menu, MF_STRING, showId, L"打开文件位置");
    }

    // 统一追加空白处右键的全局菜单项：无论点在哪个图标上，都能看到
    // 自动收起开关和退出入口。
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu,
                MF_STRING | (s.autoCollapse ? MF_CHECKED : MF_UNCHECKED),
                kMenuToggleAutoCollapse,
                L"自动收起（光标离开收到底部，触碰下缘展开）");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuExit, L"退出桌面 Dock");

    POINT pt{};
    GetCursorPos(&pt);
    ForceForegroundWindow(s.hwnd);  // 不能只 SetForegroundWindow：NOACTIVATE 窗口易被前台锁拦截，菜单可能不来
    s.menuOpen = true;  // 菜单期间保持展开（避免光标移至菜单即被收起）
    const int cmd = TrackPopupMenu(menu,
                                   TPM_RETURNCMD | TPM_RIGHTBUTTON |
                                       TPM_NONOTIFY | TPM_LEFTALIGN |
                                       TPM_BOTTOMALIGN,
                                   pt.x, pt.y, 0, s.hwnd, nullptr);
    s.menuOpen = false;
    PostMessageW(s.hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);

    // 全局菜单项（与空白处右键一致）
    if (cmd == kMenuToggleAutoCollapse) {
        s.autoCollapse = !s.autoCollapse;
        if (!s.autoCollapse) s.hideRequested = false;  // 关闭即展开
        SaveConfig(s);
        Logf(s.autoCollapse ? L"自动收起：开启" : L"自动收起：关闭");
        return;
    }
    if (cmd == kMenuExit) {
        Logf(L"用户请求退出");
        DestroyWindow(s.hwnd);
        return;
    }

    if (cmd < kMenuBaseId || cmd - kMenuBaseId >= s.menuCount) return;
    const MenuEntry& e = s.menu[cmd - kMenuBaseId];
    switch (e.action) {
        case ItemAction::ToggleFocus:
            ToggleFocusOrLaunch(s, e.itemIndex);
            break;
        case ItemAction::SwitchToWindow:
            if (e.window && IsWindow(e.window)) {
                if (IsIconic(e.window)) {
                    // 任务栏语义恢复（等同点击任务栏按钮）
                    DWORD_PTR dummy = 0;
                    SendMessageTimeoutW(e.window, WM_SYSCOMMAND, SC_RESTORE,
                                        0, SMTO_ABORTIFHUNG | SMTO_BLOCK,
                                        500, &dummy);
                }
                ForceForegroundWindow(e.window);
            }
            break;
        case ItemAction::Launch:
            LaunchItem(s, e.itemIndex);
            break;
        case ItemAction::CloseAllWindows:
            RequestCloseByIndex(s, e.itemIndex);
            break;
        case ItemAction::TogglePin:
            TogglePinByKey(s, s.items[e.itemIndex]);
            break;
        case ItemAction::HideTrayItem:
            HideTrayItem(s, s.items[e.itemIndex]);
            break;
        case ItemAction::ShowInExplorer: {
            std::wstring params =
                L"/select,\"" + s.items[e.itemIndex].launchPath + L"\"";
            ShellExecuteW(s.hwnd, L"open", L"explorer.exe", params.c_str(),
                          nullptr, SW_SHOWNORMAL);
            break;
        }
        default:
            break;
    }
    // 空闲已停帧：菜单动作（固定/隐藏/关闭/自动收起切换）可能改变
    // 布局或收起态，踢一帧完成重绘与后续动画
    SetFrameCadence(s, true);
}

void ShowBlankContextMenu(AppState& s) {
    if (s.menuOpen) return;  // 防止菜单未关闭时重复右键造成嵌套模态循环/卡死
    ResetMenuEntries(s);
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu,
                MF_STRING | (s.autoCollapse ? MF_CHECKED : MF_UNCHECKED),
                kMenuToggleAutoCollapse,
                L"自动收起（光标离开收到底部，触碰下缘展开）");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuExit, L"退出桌面 Dock");

    POINT pt{};
    GetCursorPos(&pt);
    ForceForegroundWindow(s.hwnd);  // 同 ShowItemContextMenu：确保菜单能拿到前台
    s.menuOpen = true;  // 菜单期间保持展开
    const int cmd = TrackPopupMenu(menu,
                                   TPM_RETURNCMD | TPM_RIGHTBUTTON |
                                       TPM_NONOTIFY | TPM_LEFTALIGN,
                                   pt.x, pt.y, 0, s.hwnd, nullptr);
    s.menuOpen = false;
    PostMessageW(s.hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);
    if (cmd == kMenuToggleAutoCollapse) {
        s.autoCollapse = !s.autoCollapse;
        if (!s.autoCollapse) s.hideRequested = false;  // 关闭即展开
        SaveConfig(s);
        Logf(s.autoCollapse ? L"自动收起：开启" : L"自动收起：关闭");
    } else if (cmd == kMenuExit) {
        Logf(L"用户请求退出");
        DestroyWindow(s.hwnd);
    }
    // 空闲已停帧：菜单关闭/自动收起切换后的展开-收起动画需要帧驱动
    SetFrameCadence(s, true);
}

// ============================== 主循环一帧 ==============================

// 收起目标偏移：0=展开；winH=完全滑出屏幕底（菜单打开期间保持展开）
float CollapseTargetOf(const AppState& s) {
    return (s.autoCollapse && s.hideRequested && !s.menuOpen)
               ? static_cast<float>(s.winH)
               : 0.f;
}

// 自适应定时器节奏：动画/启动弹跳/悬停期间 15ms；空闲彻底停帧（零唤醒）。
// 帧驱动由事件路径负责重启：LL 钩子（进出场）、窗口鼠标消息、点击/启动、
// 菜单动作、悬停提示定时器，另有 DoPoll 安全网兜底。
void SetFrameCadence(AppState& s, bool fast) {
    const UINT want = fast ? kFrameIntervalMs : 0;  // 0 = 停止
    if (s.frameIntervalMs == static_cast<int>(want)) return;
    s.frameIntervalMs = static_cast<int>(want);
    if (want == 0) {
        KillTimer(s.hwnd, kFrameTimerId);
    } else {
        SetTimer(s.hwnd, kFrameTimerId, want, nullptr);
    }
}

// 全量刷新（事件驱动回调 + 60s 保底）：条目/尺寸/任务栏/组件自愈
void DoPoll(AppState& s) {
    s.lastPollTick = GetTickCount64();
    RefreshItems(s);
    EnsureWindowSize(s);
    EnsureTaskbarHidden(s);        // explorer 重启重建任务栏后再次隐藏
    RefreshSuiteWindowCache();     // 组件可能重启/重建窗口，刷新自愈缓存
    HealMinimizedSuiteWindows();   // 保底（正常由 WinEvent 即时触发）
    RefreshPrimaryWorkArea();      // 保底刷新工作区缓存（分辨率/任务栏变化）
    // 停帧安全网：收起偏移与目标不一致说明事件路径漏踢帧，重启帧驱动
    if (s.collapseOffset != CollapseTargetOf(s)) SetFrameCadence(s, true);
    if (s.needsRedraw) {
        const bool animating =
            UpdateLayoutOneFrame(s);  // 帧已停时几何缓存先对齐再画
        DrawAndPresent(s);
        if (animating) SetFrameCadence(s, true);  // 余下缓动交给帧驱动
    }
}

void FrameTick(AppState& s) {
    HealMinimizedSuiteWindows();  // 保底（事件路径之外的兜底检查）

    // 自动收起动画：收起 = 窗口向下滑出屏幕底边，展开 = 从屏幕底边升起
    // （缓动收敛；菜单打开期间保持展开以便操作）
    {
        const float target = CollapseTargetOf(s);
        const float cur = s.collapseOffset;
        const float next = cur + (target - cur) * 0.22f;
        s.collapseOffset =
            (std::fabs(target - next) < 0.5f) ? target : next;
    }
    const bool collapseAnimating =
        s.collapseOffset != 0.f && s.collapseOffset != static_cast<float>(s.winH);

    const bool animating = UpdateLayoutOneFrame(s);
    const uint64_t sig = MakeSignature(s);
    const bool structureChanged = sig != s.lastSignature;
    if (structureChanged) s.lastSignature = sig;

    if (structureChanged) s.needsRedraw = true;

    // 悬停/收起动画期间持续重绘；空闲时零重绘
    if (s.needsRedraw || animating || collapseAnimating) {
        DrawAndPresent(s);
    }

    // 动画/弹跳收敛后即停帧（零唤醒）；状态翻转由事件路径踢帧重启
    SetFrameCadence(s, animating || collapseAnimating ||
                           !s.pendingLaunches.empty());
}

// ============================== 窗口过程 ==============================

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    AppState* s = &g_state;

    // explorer 重启后重建任务栏的广播：立即重新隐藏（替代周期 FindWindow 轮询）
    if (s->taskbarCreatedMsg && msg == s->taskbarCreatedMsg) {
        EnsureTaskbarHidden(*s);
        return 0;
    }

    switch (msg) {
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

        case WM_MOUSEMOVE: {
            if (!s->trackingMouse) {
                TRACKMOUSEEVENT tme{};
                tme.cbSize = sizeof(tme);
                tme.dwFlags = TME_LEAVE;
                tme.hwndTrack = hwnd;
                TrackMouseEvent(&tme);
                s->trackingMouse = true;
            }
            s->mouseX = static_cast<float>(GET_X_LPARAM(lParam));
            s->mouseY = static_cast<float>(GET_Y_LPARAM(lParam));
            s->mouseInside = true;
            SetFrameCadence(*s, true);  // 进入 Dock：立即切回动画帧（放大不卡顿）
            s->needsRedraw = true;
            return 0;
        }

        case WM_MOUSELEAVE:
            // 悬停/收起状态由 LL 钩子按光标物理位置驱动（ShowDesktopHookProc），
            // 这里只复位窗口侧悬停数据；几何变化引发的假 leave 不影响任何状态
            s->trackingMouse = false;
            s->mouseInside = false;
            s->mouseX = -100000.f;
            s->mouseY = -100000.f;
            s->hoverIndex = static_cast<size_t>(-1);
            s->needsRedraw = true;
            SetFrameCadence(*s, true);  // 空闲已停帧：离开也需要一帧完成擦除
            return 0;

        case WM_LBUTTONDOWN: {
            POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            const int idx = HitIndexAt(*s, static_cast<float>(pt.x),
                                           static_cast<float>(pt.y));
            if (idx >= 0) {
                ToggleFocusOrLaunch(*s, static_cast<size_t>(idx));
            }
            return 0;
        }

        case WM_MBUTTONUP: {
            POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            const int idx = HitIndexAt(*s, static_cast<float>(pt.x),
                                           static_cast<float>(pt.y));
            if (idx >= 0) {
                // 中键关闭应用：标记关闭中并立即重绘，运行圆点立刻消失。
                RequestCloseByIndex(*s, static_cast<size_t>(idx));
            }
            return 0;
        }

        case WM_RBUTTONUP: {
            POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            const int idx = HitIndexAt(*s, static_cast<float>(pt.x),
                                           static_cast<float>(pt.y));
            if (idx >= 0) {
                // 图标右键：保留图标自身的操作，同时追加全局菜单项
                // （自动收起开关 / 退出），保证任意位置都能看到空白处内容。
                ShowItemContextMenu(*s, static_cast<size_t>(idx));
            } else {
                ShowBlankContextMenu(*s);
            }
            return 0;
        }

        // 低层鼠标钩子线程检测到进入/离开 Dock：只把状态切换投递到 UI 线程，
        // 避免钩子线程直接修改 UI 状态；即使 UI 线程偶然阻塞，鼠标钩子也已返回。
        case kMsgHookMouse: {
            const bool onDock = wParam != 0;
            s->mouseOverDock = onDock;
            if (onDock) {
                s->wasOnDock = true;
                if (s->hideRequested) s->hideRequested = false;
                s->needsRedraw = true;
                SetFrameCadence(*s, true);
            } else {
                s->mouseInside = false;
                if (s->wasOnDock) {
                    // 用钩子线程采集的“触发翻转时的光标位置”判定离开方向，
                    // 而不是在这里重新 GetCursorPos：UI 线程处理该消息时游标
                    // 可能已因 1-2px 抖动回到边界内（贴顶离开最常见的失败），
                    // 让收起判定永远落空。lParam 即钩子投递的 ms->pt。
                    const POINT cp{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                    RECT wr{};
                    if (GetWindowRect(s->hwnd, &wr)) {
                        const bool topExit = cp.y < wr.top;
                        const bool leftExit = cp.x < wr.left;
                        const bool rightExit = cp.x >= wr.right;
                        // 底部“离开”必须区分两种，只对第一种不收起：
                        //  1) 离开后仍落在 Dock 有效区（窗口/空隙/触发条/贴边
                        //     容差）内 —— 触碰下缘意为保持展开，且不复位
                        //     wasOnDock（光标仍在有效区，下次真正离开会重新判定）；
                        //  2) 越过有效区下缘（如移到主屏下方的副屏、或超出
                        //     贴边容差）—— 这是真正的离开，必须收起。
                        //     此前只认上/左/右三个方向，下方越界会误判为
                        //     “还在有效区”，导致光标远走后 Dock 停留在展开态。
                        const bool stillInZone = PointInDockOrStrip(cp);
                        if (topExit || leftExit || rightExit || !stillInZone) {
                            s->hideRequested = true;
                            s->wasOnDock = false;
                            s->needsRedraw = true;
                            SetFrameCadence(*s, true);
                        }
                    }
                }
            }
            return 0;
        }

        // WH_MOUSE_LL 钩子只采集点击，真正的窗口/输入管理动作在 UI 线程执行。
        // 这样避免 AttachThreadInput / SetForegroundWindow / ShowWindow 等调用
        // 发生在低层鼠标钩子回调里，杜绝资源管理器（explorer shell）失去
        // 鼠标响应、只能 Ctrl+Alt+Del 恢复的卡死。
        case kMsgHookClick: {
            if (wParam == kHookClickDockLeft ||
                wParam == kHookClickDockMiddle) {
                POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                const int idx = HitIndexAt(*s, static_cast<float>(pt.x),
                                           static_cast<float>(pt.y));
                if (idx >= 0) {
                    if (wParam == kHookClickDockLeft) {
                        ToggleFocusOrLaunch(*s, static_cast<size_t>(idx));
                    } else {
                        RequestCloseByIndex(*s, static_cast<size_t>(idx));
                    }
                }
            } else if (wParam == kHookClickStart) {
                TriggerWinKey();
            } else if (wParam == kHookClickShowDesktop) {
                ToggleShowDesktop();
            }
            // 与 Dock 交互后立即自愈工作区：开始菜单/显示桌面等操作可能让
            // explorer 把工作区改回“任务栏占位”，导致 Dock 跳到任务栏上方。
            EnsureTaskbarHidden(*s);
            return 0;
        }

        case WM_DROPFILES: {
            HDROP drop = reinterpret_cast<HDROP>(wParam);
            UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            int added = 0;
            for (UINT i = 0; i < count; ++i) {
                wchar_t path[MAX_PATH * 2] = {};
                if (DragQueryFileW(drop, i, path, MAX_PATH * 2 - 1) == 0) {
                    continue;
                }
                if (PinPathIfNew(*s, path)) ++added;
            }
            DragFinish(drop);
            if (added > 0) {
                s->needsRedraw = true;
                SetFrameCadence(*s, true);  // 空闲已停帧：新固定项需要重摆一帧
            }
            return 0;
        }

        // 启动台拖入固定：lpData = UTF-16 路径（见 launcher_main.cpp 协议）
        case WM_COPYDATA: {
            auto* cds = reinterpret_cast<COPYDATASTRUCT*>(lParam);
            if (cds && cds->dwData == kDesktopDockPinMagic && cds->lpData &&
                cds->cbData >= sizeof(wchar_t)) {
                std::wstring path(
                    reinterpret_cast<const wchar_t*>(cds->lpData),
                    cds->cbData / sizeof(wchar_t));
                while (!path.empty() && path.back() == L'\0') {
                    path.pop_back();
                }
                if (!path.empty() && PinPathIfNew(*s, path)) {
                    Logf(L"启动台拖入固定：%ls", path.c_str());
                    SetFrameCadence(*s, true);  // 空闲已停帧：新固定项需要重摆一帧
                    return TRUE;
                }
            }
            return FALSE;
        }

        case WM_TIMER:
            if (wParam == kFrameTimerId) {
                FrameTick(*s);
            } else if (wParam == kRefreshTimerId) {
                // 事件风暴合并定时器到期：执行合并后的刷新
                KillTimer(hwnd, kRefreshTimerId);
                s->refreshArmed = false;
                DoPoll(*s);
            } else if (wParam == kSafetyTimerId) {
                DoPoll(*s);  // 事件失效保底（1 次/分钟）
            } else if (wParam == kTooltipTimerId) {
                // 悬停名称提示到期：踢一帧完成绘制
                KillTimer(hwnd, kTooltipTimerId);
                SetFrameCadence(*s, true);
            }
            return 0;

        // WinEvent 事件：窗口集合/托盘状态变化 → 合并刷新（事件风暴下限 1s）
        case kMsgRefresh: {
            if (s->lastPollTick == 0) {
                DoPoll(*s);
                return 0;
            }
            const ULONGLONG now = GetTickCount64();
            if (now - s->lastPollTick >= kRefreshMinGapMs) {
                DoPoll(*s);
            } else if (!s->refreshArmed) {
                s->refreshArmed = true;
                const ULONGLONG wait =
                    kRefreshMinGapMs - (now - s->lastPollTick);
                SetTimer(hwnd, kRefreshTimerId, static_cast<UINT>(wait),
                         nullptr);
            }
            return 0;
        }

        // WinEvent 事件：套件组件被最小化（Win+D）→ 立即恢复
        case kMsgHeal:
            HealMinimizedSuiteWindows();
            return 0;

        // 托盘图标触发完成（worker 线程）：收回任务栏并收尾
        case kMsgTriggerDone: {
            const bool ok = wParam != 0;
            s->taskbarShowUntil = 0;
            EnsureTaskbarHidden(*s);  // 立即收回：隐藏任务栏 + 工作区扩回全屏
            if (!ok) {
                // 触发失败：回退评分强显（旧行为兜底，尽力而为）
                const size_t idx = static_cast<size_t>(lParam);
                if (idx < s->items.size()) {
                    g_skipTrayTriggerOnce = true;
                    WakeTrayOnlyApp(*s, idx);
                    g_skipTrayTriggerOnce = false;
                }
            }
            DoPoll(*s);  // 尽快把恢复的窗口纳入条目/更新运行状态
            return 0;
        }

        case WM_DPICHANGED: {
            s->dpi = static_cast<int>(HIWORD(wParam));
            s->scale = static_cast<float>(s->dpi) / 96.0f;
            delete s->uiFont;
            s->uiFont = nullptr;
            RefreshPrimaryWorkArea();  // DPI 变化常伴随工作区物理尺寸变化
            EnsureWindowSize(*s);
            RepositionDock(*s, true);
            s->needsRedraw = true;
            UpdateLayoutOneFrame(*s);  // 几何缓存与新尺寸对齐后再画
            DrawAndPresent(*s);
            return 0;
        }

        case WM_DISPLAYCHANGE:
            RefreshPrimaryWorkArea();  // 分辨率变化 → 工作区变化
            EnsureWindowSize(*s);
            RepositionDock(*s, true);
            s->needsRedraw = true;
            UpdateLayoutOneFrame(*s);
            DrawAndPresent(*s);  // 空闲可能已停帧，直接完成重绘
            return 0;

        case WM_DESTROY:
            KillTimer(hwnd, kFrameTimerId);
            KillTimer(hwnd, kRefreshTimerId);
            KillTimer(hwnd, kSafetyTimerId);
            UninstallDockWinEventHook();
            UninstallShowDesktopHook();
            ShowTaskbar();  // 恢复 Windows 任务栏
            PostQuitMessage(0);
            return 0;

        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ============================== 入口 ==============================

}  // namespace

// ============================== 组件接口 ==============================

std::atomic<HWND> g_dockHwnd{nullptr};

namespace {

// 进程内重启（托盘关闭后再次打开）时重建全部全局状态：
// g_state 里累积的 items/geoms/orderMap/缓存等都来自上一运行周期。
void ResetDockGlobalsForRestart() {
    g_state.~AppState();
    ::new (static_cast<void*>(&g_state)) AppState();
    g_showDesktopMinimized.clear();
    g_showDesktopActive = false;
    g_suiteWindows.clear();
    g_hookLastOnDock = false;
    g_hookLeftDownOnDock = false;
    g_skipTrayTriggerOnce = false;
}

}  // namespace

DWORD WINAPI DockThreadProc(LPVOID param) {
    const HINSTANCE hInstance = static_cast<HINSTANCE>(param);

    ResetDockGlobalsForRestart();

    EnableDpiAwareness();
    RefreshPrimaryWorkArea();  // DPI 感知就绪后建立工作区缓存（后续热路径零系统调用）

    // Shell API（SHGetFileInfo 解析 .lnk / jumbo 图标）需要 STA COM
    HRESULT comInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    ULONG_PTR token = 0;
    GdiplusStartupInput gsi{};
    if (GdiplusStartup(&token, &gsi, nullptr) != Ok) {
        if (SUCCEEDED(comInit)) CoUninitialize();
        return 1;
    }

    LogInit();

    AppState& s = g_state;
    s.hwnd = nullptr;
    s.dpi = QueryPrimaryDpi();
    s.scale = static_cast<float>(s.dpi) / 96.0f;

    bool configExists = false;
    LoadConfig(s, &configExists);
    if (!configExists) {
        SeedDefaultPinsIfEmpty(s);
        SaveConfig(s);
    }

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
            GdiplusShutdown(token);
            LogClose();
            if (SUCCEEDED(comInit)) CoUninitialize();
            return 1;
        }
    }

    const SIZE initial = DesiredWindowSize(s);
    s.hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE |
            WS_EX_ACCEPTFILES,
        kWindowClass, L"DesktopDock", WS_POPUP, CW_USEDEFAULT, CW_USEDEFAULT,
        initial.cx, initial.cy, nullptr, nullptr, hInstance, nullptr);
    if (!s.hwnd) {
        GdiplusShutdown(token);
        LogClose();
        if (SUCCEEDED(comInit)) CoUninitialize();
        return 1;
    }

    g_dockHwnd.store(s.hwnd);  // 先发布窗口句柄，宿主可立即隐藏/关闭

    DragAcceptFiles(s.hwnd, TRUE);

    s.uiFont = new Font(L"Microsoft YaHei UI",
                        static_cast<REAL>(MulDiv(12, s.dpi, 96)),
                        FontStyleBold, UnitPixel);
    CreateBacking(s, initial.cx, initial.cy);
    s.winW = initial.cx;
    s.lastIdleW = initial.cx;
    HideTaskbar();  // Dock 常驻期间隐藏 Windows 任务栏（此后定位使用全屏工作区）
    EnsureTaskbarHidden(s);  // 启动即自愈：防止 explorer 异步把工作区改回任务栏占位
    RepositionDock(s, true);
    InstallShowDesktopHook();  // 右下角显示桌面隐形按钮（与 Windows 按钮同尺寸）

    RefreshItems(s);
    s.lastSignature = MakeSignature(s);
    UpdateLayoutOneFrame(s);
    DrawAndPresent(s);
    // ULW 窗口同样必须 ShowWindow 一次，否则始终处于隐藏态
    ShowWindow(s.hwnd, SW_SHOWNOACTIVATE);
    // 事件驱动接管（替代定时轮询）：空暇零唤醒
    s.taskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");
    InstallDockWinEventHook();   // 窗口集合变化 / Win+D 最小化自愈
    StartTrayRegistryWatch();    // 托盘注册表阻塞式监听（RegNotifyChangeKeyValue）
    SetTimer(s.hwnd, kSafetyTimerId, static_cast<UINT>(kSafetyPollMs),
             nullptr);           // 事件失效保底（1 次/分钟）
    s.lastPollTick = GetTickCount64();  // 启动后立即首次轮询
    SetTimer(s.hwnd, kFrameTimerId, kFrameIntervalMs, nullptr);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    KillTimer(s.hwnd, kFrameTimerId);
    UninstallShowDesktopHook();
    delete s.uiFont;
    ClearIconCache(s);
    DestroyBacking(s);
    GdiplusShutdown(token);
    LogClose();
    if (SUCCEEDED(comInit)) CoUninitialize();
    g_dockHwnd.store(nullptr);  // 窗口已销毁，线程即将退出
    return static_cast<DWORD>(msg.wParam);
}
