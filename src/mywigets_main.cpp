// MyWigets - 桌面组件进程内宿主
//
// 功能：
//   1. 全部组件（时钟 / 日历 / 应用管理 / 顶栏 / Dock 栏）都以线程形式运行在
//      本进程内：每个组件在自己的线程里创建窗口并泵自己的消息循环，
//      不再存在任何独立的组件 exe 进程（同目录的其他组件 exe 已删除）。
//   2. 系统托盘菜单：
//        - 打开全部组件
//        - 时钟 / 日历 / 应用管理 / 顶栏 / Dock 栏 勾选式开关
//          （勾选 = 组件窗口已创建；点击 = 启动 / 优雅关闭该组件线程）
//        - Dock 栏热键子菜单：全局热键开关 Dock（默认 Alt+Space，
//          可选预设组合或自定义捕获，配置存 HKCU\...\MyWigets）
//        - 关闭全部组件（逐个 WM_CLOSE 优雅退出，超时则结束线程）
//        - 开机启动（勾选后写入 HKCU\...\Run）
//        - 退出 MyWigets（关闭全部组件后退出托盘）
//   3. 支持命令行：
//        MyWigets.exe -autostart on    开启开机启动
//        MyWigets.exe -autostart off   关闭开机启动
//        （不带参数运行即为托盘模式）

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef _WIN32_IE
#define _WIN32_IE 0x0600
#endif

#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <tlhelp32.h>

#include "resource.h"
#include "widgets.h"

#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")

namespace {

using SetProcessDpiAwarenessContextFn = BOOL(WINAPI*)(HANDLE);

// 进程级 DPI 感知（必须在本进程创建任何窗口之前设置；组件线程内也会调用，
// 但第二次调用会失败并被各组件忽略，这里先调一次保证组件窗口按真实 DPI 创建）
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

constexpr wchar_t kWindowClass[] = L"MyWigetsTrayWindow";
constexpr wchar_t kMutexName[] = L"Local\\MyWigets_SingleInstance";
constexpr wchar_t kRunKeyPath[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValueName[] = L"MyWigets";
constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kMsgOpenAll = WM_APP + 2;  // 第二实例 / 托盘左键：打开全部组件
constexpr UINT kTrayIconId = 1;

constexpr int kMenuOpenAll = 1001;
constexpr int kMenuToggleClock = 1002;
constexpr int kMenuToggleCalendar = 1003;
constexpr int kMenuToggleLauncher = 1006;
constexpr int kMenuToggleTopBar = 1007;
constexpr int kMenuToggleDock = 1008;
constexpr int kMenuCloseAll = 1009;
constexpr int kMenuAutoStart = 1004;
constexpr int kMenuExit = 1005;

// ---- Dock 开关热键 ----
constexpr wchar_t kConfigKeyPath[] = L"Software\\DesktopSuite\\MyWigets";
constexpr int kHotkeyId = 1;
constexpr int kMenuHotkeyEnable = 1020;
constexpr int kMenuHotkeyCustom = 1021;
constexpr int kMenuHotkeyPresetFirst = 1030;

// ---------- 组件注册表（全部进程内线程） ----------

void ShowTrayBalloon(HWND hwnd, const wchar_t* text);  // 定义在下方

// 清理旧版独立组件进程（迁移期：它们的 exe 已被删除，但内存中实例可能还在；
// 同一时刻只能有一份组件在运行，否则任务栏/Dock 状态会互相打架）
const wchar_t* const kLegacyWidgetExes[] = {
    L"DesktopClock-x64.exe", L"DesktopClock-x86.exe",
    L"DesktopCalendar-x64.exe", L"DesktopCalendar-x86.exe",
    L"DesktopLauncher-x64.exe", L"DesktopLauncher-x86.exe",
    L"DesktopTopBar-x64.exe", L"DesktopTopBar-x86.exe",
    L"DesktopDock-x64.exe", L"DesktopDock-x86.exe",
};

// WM_CLOSE 回调上下文：给指定 pid 的全部顶层窗口投递 WM_CLOSE
struct LegacyCloseCtx {
    DWORD pid = 0;
};

BOOL CALLBACK CloseLegacyWindowProc(HWND hwnd, LPARAM lp) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == reinterpret_cast<const LegacyCloseCtx*>(lp)->pid) {
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
    }
    return TRUE;
}

void CloseLegacyWidgetProcesses() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            bool legacy = false;
            for (const wchar_t* name : kLegacyWidgetExes) {
                if (_wcsicmp(pe.szExeFile, name) == 0) {
                    legacy = true;
                    break;
                }
            }
            if (!legacy) continue;
            // 优雅关闭，再兜底结束（与旧托盘“关闭组件”行为一致）
            LegacyCloseCtx ctx{pe.th32ProcessID};
            EnumWindows(CloseLegacyWindowProc,
                        reinterpret_cast<LPARAM>(&ctx));
            Sleep(200);
            HANDLE proc = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
            if (proc) {
                TerminateProcess(proc, 0);
                CloseHandle(proc);
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
}

struct WidgetEntry {
    const wchar_t* label;           // 托盘菜单显示名
    DWORD (WINAPI* proc)(LPVOID);   // 组件线程过程（原组件 WinMain）
    std::atomic<HWND>* hwnd;        // 组件主窗口句柄（组件线程发布）
    HANDLE thread = nullptr;        // 组件线程句柄（nullptr = 未运行）
    DWORD tid = 0;
};

WidgetEntry g_widgets[] = {
    {L"时钟", ClockThreadProc, &g_clockHwnd},
    {L"日历", CalendarThreadProc, &g_calendarHwnd},
    {L"应用管理", LauncherThreadProc, &g_launcherHwnd},
    {L"顶栏", TopbarThreadProc, &g_topbarHwnd},
    {L"Dock 栏", DockThreadProc, &g_dockHwnd},
};
constexpr int kWidgetCount =
    static_cast<int>(sizeof(g_widgets) / sizeof(g_widgets[0]));

// 回收已自行退出的组件线程（如组件右键菜单触发 WM_DESTROY）；
// 返回该组件当前是否运行中
bool IsWidgetRunning(int index) {
    WidgetEntry& w = g_widgets[index];
    if (!w.thread) return false;
    const DWORD code = WaitForSingleObject(w.thread, 0);
    if (code == WAIT_OBJECT_0) {
        // 线程已结束（窗口被组件自身关闭）：回收句柄，视为停止
        CloseHandle(w.thread);
        w.thread = nullptr;
        w.tid = 0;
        return false;
    }
    return true;
}

// 启动组件：创建线程并等待其主窗口创建完成（超时 5s）
bool StartWidget(int index) {
    WidgetEntry& w = g_widgets[index];
    if (IsWidgetRunning(index)) return true;  // 已在运行（内部会清理已退出线程）

    w.hwnd->store(nullptr);
    w.thread = CreateThread(nullptr, 0, w.proc,
                            GetModuleHandleW(nullptr), 0, &w.tid);
    if (!w.thread) return false;

    // 组件线程在窗口创建成功后立即发布句柄；启动失败会提前退出线程
    for (int i = 0; i < 500; ++i) {
        if (w.hwnd->load() != nullptr) return true;
        if (WaitForSingleObject(w.thread, 10) == WAIT_OBJECT_0) {
            CloseHandle(w.thread);
            w.thread = nullptr;
            w.tid = 0;
            return false;  // 组件启动失败（自身初始化错误）
        }
    }
    return true;  // 超时但线程健在：窗口稍后可见，按运行处理
}

// 关闭组件：WM_CLOSE 优雅退出（组件 WM_DESTROY 内自行清理并 PostQuitMessage），
// 轮询约 2s 仍存活则 TerminateThread 兜底（组件均无未保存状态）
void StopWidget(int index) {
    WidgetEntry& w = g_widgets[index];
    if (!w.thread) return;

    HWND hwnd = w.hwnd->load();
    if (hwnd && IsWindow(hwnd)) {
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
    } else {
        PostThreadMessageW(w.tid, WM_QUIT, 0, 0);
    }
    if (WaitForSingleObject(w.thread, 2000) != WAIT_OBJECT_0) {
        TerminateThread(w.thread, 0);
        WaitForSingleObject(w.thread, 300);
    }
    CloseHandle(w.thread);
    w.thread = nullptr;
    w.tid = 0;
    w.hwnd->store(nullptr);
}

void StartAllWidgets() {
    for (int i = 0; i < kWidgetCount; ++i) {
        StartWidget(i);
    }
}

void StopAllWidgets() {
    // 与旧的 exe 版 CloseAllComponents 相同的顺序：先停 Dock（恢复任务栏），
    // 再停顶栏 / 应用管理 / 日历 / 时钟
    for (int i = kWidgetCount - 1; i >= 0; --i) {
        StopWidget(i);
    }
}

// 托盘菜单 cmd → 组件索引（菜单项与 g_widgets 顺序一致）
int WidgetIndexForCommand(int cmd) {
    switch (cmd) {
    case kMenuToggleClock: return 0;
    case kMenuToggleCalendar: return 1;
    case kMenuToggleLauncher: return 2;
    case kMenuToggleTopBar: return 3;
    case kMenuToggleDock: return 4;
    default: return -1;
    }
}

// 单个组件的开关：运行中则优雅关闭，未运行则启动（托盘菜单勾选项共用）
void ToggleComponent(HWND hwnd, int index) {
    WidgetEntry& w = g_widgets[index];
    if (IsWidgetRunning(index)) {
        StopWidget(index);
        std::wstring tip = std::wstring(L"已关闭 ") + w.label;
        ShowTrayBalloon(hwnd, tip.c_str());
    } else {
        StartWidget(index);
    }
}

// ---------- Dock 开关热键 ----------

struct HotkeyChoice {
    UINT mods;
    UINT vk;
    const wchar_t* label;
};
constexpr HotkeyChoice kHotkeyPresets[] = {
    {MOD_ALT, VK_SPACE, L"Alt+Space"},
    {MOD_CONTROL, VK_SPACE, L"Ctrl+Space"},
    {MOD_ALT, 'Q', L"Alt+Q"},
    {MOD_CONTROL | MOD_ALT, 'D', L"Ctrl+Alt+D"},
    {MOD_WIN, 'D', L"Win+D"},
};
constexpr int kHotkeyPresetCount =
    sizeof(kHotkeyPresets) / sizeof(kHotkeyPresets[0]);

struct HotkeyConfig {
    bool enabled = true;
    UINT mods = MOD_ALT;   // 默认 Alt+Space
    UINT vk = VK_SPACE;
};
HotkeyConfig g_hotkey;
bool g_hotkeyRegistered = false;

std::wstring HotkeyToText(UINT mods, UINT vk) {
    std::wstring s;
    auto add = [&s](const wchar_t* part) {
        if (!s.empty()) s += L"+";
        s += part;
    };
    if (mods & MOD_CONTROL) add(L"Ctrl");
    if (mods & MOD_ALT) add(L"Alt");
    if (mods & MOD_SHIFT) add(L"Shift");
    if (mods & MOD_WIN) add(L"Win");
    wchar_t name[32] = {};
    if ((vk >= L'A' && vk <= L'Z') || (vk >= L'0' && vk <= L'9')) {
        name[0] = static_cast<wchar_t>(vk);
    } else if (vk >= VK_F1 && vk <= VK_F24) {
        swprintf_s(name, L"F%u", static_cast<unsigned>(vk - VK_F1 + 1));
    } else {
        switch (vk) {
        case VK_SPACE: wcscpy_s(name, L"Space"); break;
        case VK_OEM_3: wcscpy_s(name, L"`"); break;
        case VK_OEM_MINUS: wcscpy_s(name, L"-"); break;
        case VK_OEM_PLUS: wcscpy_s(name, L"="); break;
        case VK_TAB: wcscpy_s(name, L"Tab"); break;
        case VK_HOME: wcscpy_s(name, L"Home"); break;
        case VK_END: wcscpy_s(name, L"End"); break;
        case VK_PRIOR: wcscpy_s(name, L"PgUp"); break;
        case VK_NEXT: wcscpy_s(name, L"PgDn"); break;
        case VK_INSERT: wcscpy_s(name, L"Ins"); break;
        case VK_DELETE: wcscpy_s(name, L"Del"); break;
        default: {
            // GetKeyNameTextW 的 lParam：扫描码 << 16，扩展位 bit24
            UINT sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
            LONG lp = static_cast<LONG>(sc) << 16;
            if (vk >= VK_LEFT && vk <= VK_DOWN) lp |= 1 << 24;
            if (GetKeyNameTextW(lp, name, 31) == 0) {
                wcscpy_s(name, L"未知键");
            }
            break;
        }
        }
    }
    return s + name;
}

DWORD ReadCfgDword(HKEY key, const wchar_t* name, DWORD def) {
    DWORD type = 0;
    DWORD value = 0;
    DWORD bytes = sizeof(value);
    if (RegQueryValueExW(key, name, nullptr, &type,
                         reinterpret_cast<BYTE*>(&value),
                         &bytes) == ERROR_SUCCESS &&
        type == REG_DWORD && bytes == sizeof(value)) {
        return value;
    }
    return def;
}

void LoadHotkeyConfig() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kConfigKeyPath, 0, KEY_READ,
                      &key) != ERROR_SUCCESS) {
        return;  // 无配置 → 使用默认 Alt+Space
    }
    g_hotkey.enabled = ReadCfgDword(key, L"DockHotkeyEnabled", 1) != 0;
    g_hotkey.mods = ReadCfgDword(key, L"DockHotkeyMods", MOD_ALT);
    g_hotkey.vk = ReadCfgDword(key, L"DockHotkeyVk", VK_SPACE);
    RegCloseKey(key);
}

void SaveHotkeyConfig() {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kConfigKeyPath, 0, nullptr, 0,
                        KEY_WRITE, nullptr, &key,
                        nullptr) != ERROR_SUCCESS) {
        return;
    }
    const DWORD en = g_hotkey.enabled ? 1 : 0;
    const DWORD mods = g_hotkey.mods;
    const DWORD vk = g_hotkey.vk;
    RegSetValueExW(key, L"DockHotkeyEnabled", 0, REG_DWORD,
                   reinterpret_cast<const BYTE*>(&en), sizeof(en));
    RegSetValueExW(key, L"DockHotkeyMods", 0, REG_DWORD,
                   reinterpret_cast<const BYTE*>(&mods), sizeof(mods));
    RegSetValueExW(key, L"DockHotkeyVk", 0, REG_DWORD,
                   reinterpret_cast<const BYTE*>(&vk), sizeof(vk));
    RegCloseKey(key);
}

// 按当前配置重新注册热键；返回是否处于已注册状态
bool EnsureHotkeyRegistered(HWND hwnd) {
    if (g_hotkeyRegistered) {
        UnregisterHotKey(hwnd, kHotkeyId);
        g_hotkeyRegistered = false;
    }
    if (!g_hotkey.enabled) return false;
    g_hotkeyRegistered =
        RegisterHotKey(hwnd, kHotkeyId, g_hotkey.mods, g_hotkey.vk) != FALSE;
    return g_hotkeyRegistered;
}

void ApplyHotkey(HWND hwnd, bool enabled, UINT mods, UINT vk) {
    g_hotkey.enabled = enabled;
    g_hotkey.mods = mods;
    g_hotkey.vk = vk;
    const std::wstring combo = HotkeyToText(mods, vk);
    if (enabled && EnsureHotkeyRegistered(hwnd)) {
        SaveHotkeyConfig();
        ShowTrayBalloon(hwnd, (L"Dock 热键：" + combo).c_str());
    } else if (enabled) {
        // 注册失败（组合被占用）：回退为停用并持久化
        g_hotkey.enabled = false;
        SaveHotkeyConfig();
        ShowTrayBalloon(hwnd, (L"热键 " + combo + L" 注册失败，可能已被占用")
                                  .c_str());
    } else {
        EnsureHotkeyRegistered(hwnd);  // 内部只会注销
        SaveHotkeyConfig();
        ShowTrayBalloon(hwnd, L"Dock 热键已停用");
    }
}

// ---- 自定义热键捕获窗（按下组合即完成，Esc 取消）----

struct HotkeyCapture {
    bool running = false;
    bool confirmed = false;
    bool needModifierHint = false;
    UINT mods = 0;
    UINT vk = 0;
};
HotkeyCapture g_hkcap;

LRESULT CALLBACK HotkeyCaptureProc(HWND hwnd, UINT msg, WPARAM wParam,
                                   LPARAM lParam) {
    switch (msg) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        const UINT vk = static_cast<UINT>(wParam);
        const bool ctrl = GetAsyncKeyState(VK_CONTROL) < 0;
        const bool alt = GetAsyncKeyState(VK_MENU) < 0;
        const bool shift = GetAsyncKeyState(VK_SHIFT) < 0;
        const bool win = GetAsyncKeyState(VK_LWIN) < 0 ||
                         GetAsyncKeyState(VK_RWIN) < 0;
        const UINT mods = (ctrl ? MOD_CONTROL : 0u) | (alt ? MOD_ALT : 0u) |
                          (shift ? MOD_SHIFT : 0u) | (win ? MOD_WIN : 0u);
        if (vk == VK_ESCAPE && mods == 0) {
            g_hkcap.running = false;  // Esc 取消
        } else if (vk != VK_SHIFT && vk != VK_CONTROL && vk != VK_MENU &&
                   vk != VK_LWIN && vk != VK_RWIN && vk != VK_APPS &&
                   vk != VK_PAUSE && vk != VK_SCROLL &&
                   vk != VK_NUMLOCK) {
            // 必须带 Ctrl/Alt/Win 之一，避免注册裸字母/数字键无法触发或误触
            if ((mods & (MOD_CONTROL | MOD_ALT | MOD_WIN)) == 0) {
                g_hkcap.needModifierHint = true;
                InvalidateRect(hwnd, nullptr, TRUE);
                return 0;
            }
            g_hkcap.confirmed = true;
            g_hkcap.mods = mods;
            g_hkcap.vk = vk;
            g_hkcap.running = false;
        } else {
            g_hkcap.needModifierHint = false;
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        }
        PostThreadMessageW(GetCurrentThreadId(), WM_NULL, 0, 0);
        DestroyWindow(hwnd);
        return 0;
    }
    case WM_SYSCHAR:
        return 0;  // 吞掉 Alt 菜单响铃
    case WM_CLOSE:
        g_hkcap.running = false;
        PostThreadMessageW(GetCurrentThreadId(), WM_NULL, 0, 0);
        DestroyWindow(hwnd);
        return 0;
    case WM_ERASEBKGND: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        RECT rc{};
        GetClientRect(hwnd, &rc);
        HBRUSH bg = CreateSolidBrush(RGB(34, 35, 44));
        FillRect(dc, &rc, bg);
        DeleteObject(bg);
        return 1;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc{};
        GetClientRect(hwnd, &rc);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(235, 236, 242));
        HFONT font = CreateFontW(
            -19, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
        HFONT old = static_cast<HFONT>(SelectObject(dc, font));
        RECT top = rc;
        top.bottom = rc.bottom / 2;
        DrawTextW(dc, L"按下新的热键组合", -1, &top,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        RECT bottom = rc;
        bottom.top = rc.bottom / 2;
        SetTextColor(dc, g_hkcap.needModifierHint ? RGB(255, 176, 120)
                                                  : RGB(150, 152, 162));
        DrawTextW(dc, g_hkcap.needModifierHint
                          ? L"至少需要 Ctrl / Alt / Win 修饰键"
                          : L"Esc 取消",
                  -1, &bottom, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, old);
        DeleteObject(font);
        EndPaint(hwnd, &ps);
        return 0;
    }
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// 打开捕获窗；返回 true 表示用户按下了有效组合（写入 *modsOut/*vkOut）
bool RunHotkeyCaptureDialog(HWND owner, UINT* modsOut, UINT* vkOut) {
    constexpr wchar_t clsName[] = L"MyWigetsHotkeyCapture";
    static bool classReady = false;
    if (!classReady) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = HotkeyCaptureProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = clsName;
        if (!RegisterClassExW(&wc)) return false;
        classReady = true;
    }

    RECT wa{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    const int w = 400;
    const int h = 140;

    g_hkcap = HotkeyCapture{};
    g_hkcap.running = true;
    HWND cap = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW, clsName, L"设置 Dock 栏热键",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        wa.left + ((wa.right - wa.left) - w) / 2,
        wa.top + ((wa.bottom - wa.top) - h) / 2, w, h, owner, nullptr,
        GetModuleHandleW(nullptr), nullptr);
    if (!cap) return false;

    EnableWindow(owner, FALSE);
    ShowWindow(cap, SW_SHOW);
    SetForegroundWindow(cap);

    MSG m{};
    while (g_hkcap.running && IsWindow(cap) &&
           GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);

    if (g_hkcap.confirmed) {
        *modsOut = g_hkcap.mods;
        *vkOut = g_hkcap.vk;
        return true;
    }
    return false;
}

bool IsAutoStartEnabled() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKeyPath, 0, KEY_QUERY_VALUE,
                      &key) != ERROR_SUCCESS) {
        return false;
    }
    const LONG result = RegQueryValueExW(key, kRunValueName, nullptr, nullptr,
                                         nullptr, nullptr);
    RegCloseKey(key);
    return result == ERROR_SUCCESS;
}

bool SetAutoStart(bool enable) {
    wchar_t self[MAX_PATH] = {};
    const DWORD len = GetModuleFileNameW(nullptr, self, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        return false;
    }

    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKeyPath, 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return false;
    }

    LONG result = ERROR_SUCCESS;
    if (enable) {
        wchar_t command[MAX_PATH + 3] = {};
        swprintf_s(command, L"\"%s\"", self);
        const DWORD bytes = static_cast<DWORD>((wcslen(command) + 1) * sizeof(wchar_t));
        result = RegSetValueExW(key, kRunValueName, 0, REG_SZ,
                                reinterpret_cast<const BYTE*>(command), bytes);
    } else {
        result = RegDeleteValueW(key, kRunValueName);
        if (result == ERROR_FILE_NOT_FOUND) {
            result = ERROR_SUCCESS;
        }
    }

    RegCloseKey(key);
    return result == ERROR_SUCCESS;
}

void ShowTrayBalloon(HWND hwnd, const wchar_t* text) {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = kTrayIconId;
    nid.uFlags = NIF_INFO;
    wcscpy_s(nid.szInfo, text);
    wcscpy_s(nid.szInfoTitle, L"MyWigets");
    nid.dwInfoFlags = NIIF_INFO;
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

bool AddTrayIcon(HWND hwnd) {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = kTrayIconId;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = kTrayMessage;
    nid.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APP));
    wcscpy_s(nid.szTip, L"MyWigets - 时钟 + 日历 + 应用管理 + 顶栏 + Dock");
    return Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
}

bool RemoveTrayIcon(HWND hwnd) {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = kTrayIconId;
    return Shell_NotifyIconW(NIM_DELETE, &nid) != FALSE;
}

void ToggleAutoStart(HWND hwnd) {
    const bool enable = !IsAutoStartEnabled();
    if (SetAutoStart(enable)) {
        ShowTrayBalloon(hwnd, enable ? L"已开启开机启动" : L"已关闭开机启动");
    } else {
        ShowTrayBalloon(hwnd, L"设置开机启动失败");
    }
}

void ExecuteMenuCommand(HWND hwnd, int cmd) {
    switch (cmd) {
    case kMenuOpenAll:
        StartAllWidgets();
        break;
    case kMenuHotkeyEnable:
        ApplyHotkey(hwnd, !g_hotkey.enabled, g_hotkey.mods, g_hotkey.vk);
        break;
    case kMenuHotkeyCustom: {
        UINT mods = 0;
        UINT vk = 0;
        if (RunHotkeyCaptureDialog(hwnd, &mods, &vk)) {
            ApplyHotkey(hwnd, true, mods, vk);
        }
        break;
    }
    case kMenuCloseAll:
        StopAllWidgets();
        ShowTrayBalloon(hwnd, L"已关闭全部组件");
        break;
    case kMenuAutoStart:
        ToggleAutoStart(hwnd);
        break;
    case kMenuExit:
        DestroyWindow(hwnd);
        break;
    default: {
        const int idx = WidgetIndexForCommand(cmd);
        if (idx >= 0) {
            ToggleComponent(hwnd, idx);
        } else if (cmd >= kMenuHotkeyPresetFirst &&
                   cmd < kMenuHotkeyPresetFirst + kHotkeyPresetCount) {
            const HotkeyChoice& preset =
                kHotkeyPresets[cmd - kMenuHotkeyPresetFirst];
            ApplyHotkey(hwnd, true, preset.mods, preset.vk);
        }
        break;
    }
    }
}

void ShowTrayMenu(HWND hwnd) {
    POINT pt{};
    GetCursorPos(&pt);

    HMENU menu = CreatePopupMenu();
    if (!menu) {
        return;
    }

    AppendMenuW(menu, MF_STRING, kMenuOpenAll, L"打开全部组件");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    // 全部组件统一的开关项：勾选 = 运行中，点击切换打开/关闭
    // 菜单 id（1002/1003/1006/1007/1008）与 g_widgets 顺序的映射
    static constexpr int kMenuIds[kWidgetCount] = {
        kMenuToggleClock, kMenuToggleCalendar, kMenuToggleLauncher,
        kMenuToggleTopBar, kMenuToggleDock,
    };
    for (int i = 0; i < kWidgetCount; ++i) {
        WidgetEntry& w = g_widgets[i];
        AppendMenuW(menu,
                    MF_STRING | (IsWidgetRunning(i) ? MF_CHECKED
                                                    : MF_UNCHECKED),
                    kMenuIds[i], w.label);
    }

    // Dock 开关热键子菜单（默认 Alt+Space）
    HMENU hkMenu = CreatePopupMenu();
    if (hkMenu) {
        AppendMenuW(hkMenu,
                    MF_STRING | (g_hotkey.enabled ? MF_CHECKED
                                                  : MF_UNCHECKED),
                    kMenuHotkeyEnable,
                    g_hotkey.enabled ? L"启用热键"
                                     : L"启用热键（已停用）");
        AppendMenuW(hkMenu, MF_SEPARATOR, 0, nullptr);
        for (int i = 0; i < kHotkeyPresetCount; ++i) {
            const HotkeyChoice& p = kHotkeyPresets[i];
            const bool active = g_hotkey.enabled &&
                                g_hotkey.mods == p.mods &&
                                g_hotkey.vk == p.vk;
            AppendMenuW(hkMenu,
                        MF_STRING | (active ? MF_CHECKED : MF_UNCHECKED),
                        kMenuHotkeyPresetFirst + i, p.label);
        }
        AppendMenuW(hkMenu, MF_STRING, kMenuHotkeyCustom, L"自定义热键…");
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(hkMenu),
                    L"Dock 栏热键");
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    const bool autoStart = IsAutoStartEnabled();
    AppendMenuW(menu, MF_STRING | (autoStart ? MF_CHECKED : MF_UNCHECKED),
                kMenuAutoStart, L"开机启动");
    AppendMenuW(menu, MF_STRING, kMenuCloseAll, L"关闭全部组件");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuExit, L"退出 MyWigets");

    SetForegroundWindow(hwnd);
    const UINT flags = TPM_RIGHTBUTTON | TPM_LEFTALIGN | TPM_RETURNCMD | TPM_NONOTIFY;
    const int cmd = TrackPopupMenu(menu, flags, pt.x, pt.y, 0, hwnd, nullptr);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);

    if (cmd > 0) {
        ExecuteMenuCommand(hwnd, cmd);
    }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        AddTrayIcon(hwnd);
        CloseLegacyWidgetProcesses();  // 迁移期：清除内存中残留的旧版组件进程
        LoadHotkeyConfig();
        StartAllWidgets();
        if (g_hotkey.enabled && !EnsureHotkeyRegistered(hwnd)) {
            ShowTrayBalloon(hwnd, L"Dock 热键注册失败（可能被其他程序占用）");
        }
        return 0;

    case kMsgOpenAll:
        StartAllWidgets();
        return 0;

    case WM_HOTKEY:
        if (wParam == kHotkeyId) {
            ToggleComponent(hwnd, 4);  // Dock 栏
        }
        return 0;

    case kTrayMessage:
        if (LOWORD(lParam) == WM_RBUTTONUP || LOWORD(lParam) == WM_CONTEXTMENU) {
            ShowTrayMenu(hwnd);
        } else if (LOWORD(lParam) == WM_LBUTTONUP) {
            StartAllWidgets();
        }
        return 0;

    case WM_COMMAND:
        ExecuteMenuCommand(hwnd, LOWORD(wParam));
        return 0;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        // 无论从菜单退出还是 WM_CLOSE（如 taskkill 优雅关闭），
        // 都把进程内全部组件一并关闭，避免残留线程
        if (g_hotkeyRegistered) {
            UnregisterHotKey(hwnd, kHotkeyId);
            g_hotkeyRegistered = false;
        }
        StopAllWidgets();
        RemoveTrayIcon(hwnd);
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace

int APIENTRY WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR lpCmdLine, int) {
    // 命令行开关，便于脚本/用户直接设置开机启动
    if (lpCmdLine && std::strstr(lpCmdLine, "-autostart on")) {
        return SetAutoStart(true) ? 0 : 1;
    }
    if (lpCmdLine && std::strstr(lpCmdLine, "-autostart off")) {
        return SetAutoStart(false) ? 0 : 1;
    }

    EnableDpiAwareness();  // 先于本进程一切窗口创建

    HANDLE mutex = CreateMutexW(nullptr, TRUE, kMutexName);
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(mutex);
        // 已有托盘实例在运行：通知它打开全部组件，本次退出
        HWND existing = FindWindowW(kWindowClass, nullptr);
        if (existing) {
            PostMessageW(existing, kMsgOpenAll, 0, 0);
        }
        return 0;
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APP));
    wc.hIconSm = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APP));
    wc.lpszClassName = kWindowClass;

    if (!RegisterClassExW(&wc)) {
        CloseHandle(mutex);
        return 1;
    }

    HWND hwnd = CreateWindowExW(0, kWindowClass, L"MyWigets", WS_POPUP,
                                0, 0, 0, 0, nullptr, nullptr, hInstance, nullptr);
    if (!hwnd) {
        CloseHandle(mutex);
        return 1;
    }

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    CloseHandle(mutex);
    return static_cast<int>(msg.wParam);
}
