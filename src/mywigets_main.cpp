// MyWigets - 桌面组件启动器
//
// 功能：
//   1. 启动 MyWigets 时同时启动全部桌面组件：DesktopClock、DesktopCalendar、
//      DesktopLauncher、DesktopTopBar、DesktopDock
//      （各组件仍是独立进程、独立窗口，可分别自由拖动）
//   2. 系统托盘菜单：
//        - 打开全部组件
//        - 时钟 / 日历 / 应用管理 / 顶栏 / Dock 栏 勾选式开关
//        - Dock 栏热键子菜单：全局热键开关 Dock（默认 Alt+Space，
//          可选预设组合或自定义捕获，配置存 HKCU\...\MyWigets）
//        - 关闭全部组件（先发 WM_CLOSE 优雅退出，超时则结束进程）
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

#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")

namespace {

constexpr wchar_t kWindowClass[] = L"MyWigetsTrayWindow";
constexpr wchar_t kMutexName[] = L"Local\\MyWigets_SingleInstance";
constexpr wchar_t kRunKeyPath[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValueName[] = L"MyWigets";
constexpr UINT kTrayMessage = WM_APP + 1;
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

#if defined(_M_X64) || defined(_M_AMD64) || defined(__x86_64__)
constexpr wchar_t kClockExe[] = L"DesktopClock-x64.exe";
constexpr wchar_t kCalendarExe[] = L"DesktopCalendar-x64.exe";
constexpr wchar_t kLauncherExe[] = L"DesktopLauncher-x64.exe";
constexpr wchar_t kTopBarExe[] = L"DesktopTopBar-x64.exe";
constexpr wchar_t kDockExe[] = L"DesktopDock-x64.exe";
#else
constexpr wchar_t kClockExe[] = L"DesktopClock-x86.exe";
constexpr wchar_t kCalendarExe[] = L"DesktopCalendar-x86.exe";
constexpr wchar_t kLauncherExe[] = L"DesktopLauncher-x86.exe";
constexpr wchar_t kTopBarExe[] = L"DesktopTopBar-x86.exe";
constexpr wchar_t kDockExe[] = L"DesktopDock-x86.exe";
#endif

bool GetMyWigetsDir(wchar_t* dir, size_t count) {
    wchar_t self[MAX_PATH] = {};
    const DWORD len = GetModuleFileNameW(nullptr, self, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        return false;
    }
    wchar_t* slash = wcsrchr(self, L'\\');
    if (slash) {
        *slash = L'\0'; // 去掉文件名，保留目录
    }
    return wcscpy_s(dir, count, self) == 0;
}

bool BuildWidgetPath(const wchar_t* exeName, wchar_t* path, size_t count) {
    wchar_t dir[MAX_PATH] = {};
    if (!GetMyWigetsDir(dir, MAX_PATH)) {
        return false;
    }
    return swprintf_s(path, count, L"%s\\%s", dir, exeName) > 0;
}

bool IsWidgetRunning(const wchar_t* mutexName) {
    HANDLE h = OpenMutexW(SYNCHRONIZE, FALSE, mutexName);
    if (h) {
        CloseHandle(h);
        return true;
    }
    return false;
}

bool LaunchWidget(const wchar_t* exeName, const wchar_t* mutexName) {
    if (IsWidgetRunning(mutexName)) {
        return true; // 已经在运行，不重复启动
    }

    wchar_t path[MAX_PATH] = {};
    if (!BuildWidgetPath(exeName, path, MAX_PATH)) {
        return false;
    }

    wchar_t cmdLine[MAX_PATH] = {};
    wcscpy_s(cmdLine, path);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmdLine, nullptr, nullptr, FALSE,
                        0, nullptr, nullptr, &si, &pi)) {
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

void LaunchAllWidgets() {
    LaunchWidget(kClockExe, L"Local\\DesktopAnalogClock_SingleInstance");
    LaunchWidget(kCalendarExe, L"Local\\DesktopCalendar_SingleInstance");
    LaunchWidget(kLauncherExe, L"Local\\DesktopLauncher_SingleInstance");
    LaunchWidget(kTopBarExe, L"Local\\DesktopTopBar_SingleInstance");
    LaunchWidget(kDockExe, L"Local\\DesktopDock_SingleInstance");
}

// ---------- 组件进程的查找与关闭 ----------

// 按可执行文件名（不带路径）找出全部存活 pid
std::vector<DWORD> FindPidsByExeName(const wchar_t* exeName) {
    std::vector<DWORD> pids;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return pids;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, exeName) == 0) {
                pids.push_back(pe.th32ProcessID);
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pids;
}

// EnumWindows 回调上下文：给目标 pid 的全部顶层窗口投递 WM_CLOSE
struct CloseWindowsContext {
    DWORD pid = 0;
};

BOOL CALLBACK CloseWindowsOfPidProc(HWND hwnd, LPARAM lParam) {
    auto* ctx = reinterpret_cast<CloseWindowsContext*>(lParam);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == ctx->pid) {
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
    }
    return TRUE;
}

// 关闭一个组件：先向其全部顶层窗口发 WM_CLOSE 优雅退出，
// 轮询约 1.5s 仍存活则逐个 TerminateProcess（组件均无未保存状态）
void CloseComponent(const wchar_t* exeName) {
    std::vector<DWORD> pids = FindPidsByExeName(exeName);
    if (pids.empty()) return;

    for (DWORD pid : pids) {
        CloseWindowsContext ctx{pid};
        EnumWindows(CloseWindowsOfPidProc, reinterpret_cast<LPARAM>(&ctx));
    }
    for (int i = 0; i < 15; ++i) {
        Sleep(100);
        if (FindPidsByExeName(exeName).empty()) return;
    }
    pids = FindPidsByExeName(exeName);
    for (DWORD pid : pids) {
        HANDLE proc = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
        if (proc) {
            TerminateProcess(proc, 0);
            CloseHandle(proc);
        }
    }
}

void CloseAllComponents() {
    CloseComponent(kDockExe);
    CloseComponent(kTopBarExe);
    CloseComponent(kLauncherExe);
    CloseComponent(kCalendarExe);
    CloseComponent(kClockExe);
}

bool IsComponentRunning(const wchar_t* exeName) {
    return !FindPidsByExeName(exeName).empty();
}

// ---------- Dock 开关热键 ----------

void ShowTrayBalloon(HWND hwnd, const wchar_t* text);  // 定义在下方

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
    wcscpy_s(nid.szTip, L"MyWigets - 时钟 + 日历 + 顶栏 + Dock");
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

// 单个组件的开关：运行中则关闭，未运行则启动（托盘菜单勾选项共用）
void ToggleComponent(HWND hwnd, const wchar_t* exeName, const wchar_t* mutexName,
                     const wchar_t* label) {
    if (IsComponentRunning(exeName)) {
        CloseComponent(exeName);
        std::wstring tip = std::wstring(L"已关闭 ") + label;
        ShowTrayBalloon(hwnd, tip.c_str());
    } else {
        LaunchWidget(exeName, mutexName);
    }
}

void ExecuteMenuCommand(HWND hwnd, int cmd) {
    switch (cmd) {
    case kMenuOpenAll:
        LaunchAllWidgets();
        break;
    case kMenuToggleClock:
        ToggleComponent(hwnd, kClockExe,
                        L"Local\\DesktopAnalogClock_SingleInstance", L"时钟");
        break;
    case kMenuToggleCalendar:
        ToggleComponent(hwnd, kCalendarExe,
                        L"Local\\DesktopCalendar_SingleInstance", L"日历");
        break;
    case kMenuToggleLauncher:
        ToggleComponent(hwnd, kLauncherExe,
                        L"Local\\DesktopLauncher_SingleInstance", L"应用管理");
        break;
    case kMenuToggleTopBar:
        ToggleComponent(hwnd, kTopBarExe,
                        L"Local\\DesktopTopBar_SingleInstance", L"顶栏");
        break;
    case kMenuToggleDock:
        ToggleComponent(hwnd, kDockExe,
                        L"Local\\DesktopDock_SingleInstance", L"Dock 栏");
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
        CloseAllComponents();
        ShowTrayBalloon(hwnd, L"已关闭全部组件");
        break;
    case kMenuAutoStart:
        ToggleAutoStart(hwnd);
        break;
    case kMenuExit:
        DestroyWindow(hwnd);
        break;
    default:
        if (cmd >= kMenuHotkeyPresetFirst &&
            cmd < kMenuHotkeyPresetFirst + kHotkeyPresetCount) {
            const HotkeyChoice& preset =
                kHotkeyPresets[cmd - kMenuHotkeyPresetFirst];
            ApplyHotkey(hwnd, true, preset.mods, preset.vk);
        }
        break;
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
    struct ComponentToggle {
        int id;
        const wchar_t* exe;
        const wchar_t* mutex;
        const wchar_t* label;
    };
    const ComponentToggle toggles[] = {
        {kMenuToggleClock, kClockExe,
         L"Local\\DesktopAnalogClock_SingleInstance", L"时钟"},
        {kMenuToggleCalendar, kCalendarExe,
         L"Local\\DesktopCalendar_SingleInstance", L"日历"},
        {kMenuToggleLauncher, kLauncherExe,
         L"Local\\DesktopLauncher_SingleInstance", L"应用管理"},
        {kMenuToggleTopBar, kTopBarExe,
         L"Local\\DesktopTopBar_SingleInstance", L"顶栏"},
        {kMenuToggleDock, kDockExe,
         L"Local\\DesktopDock_SingleInstance", L"Dock 栏"},
    };
    for (const auto& t : toggles) {
        AppendMenuW(menu,
                    MF_STRING | (IsComponentRunning(t.exe) ? MF_CHECKED
                                                           : MF_UNCHECKED),
                    t.id, t.label);
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
        LaunchAllWidgets();
        LoadHotkeyConfig();
        if (g_hotkey.enabled && !EnsureHotkeyRegistered(hwnd)) {
            ShowTrayBalloon(hwnd, L"Dock 热键注册失败（可能被其他程序占用）");
        }
        return 0;

    case WM_HOTKEY:
        if (wParam == kHotkeyId) {
            ToggleComponent(hwnd, kDockExe,
                            L"Local\\DesktopDock_SingleInstance",
                            L"Dock 栏");
        }
        return 0;

    case kTrayMessage:
        if (LOWORD(lParam) == WM_RBUTTONUP || LOWORD(lParam) == WM_CONTEXTMENU) {
            ShowTrayMenu(hwnd);
        } else if (LOWORD(lParam) == WM_LBUTTONUP) {
            LaunchAllWidgets();
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
        // 都把托管的全部组件一并关闭，避免残留孤儿进程
        if (g_hotkeyRegistered) {
            UnregisterHotKey(hwnd, kHotkeyId);
            g_hotkeyRegistered = false;
        }
        CloseAllComponents();
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

    HANDLE mutex = CreateMutexW(nullptr, TRUE, kMutexName);
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(mutex);
        // 已有托盘实例在运行，本次只负责确保组件都打开，然后退出
        LaunchAllWidgets();
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
