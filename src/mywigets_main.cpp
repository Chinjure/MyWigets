// MyWigets - 桌面组件启动器
//
// 功能：
//   1. 启动 MyWigets 时同时启动 DesktopClock 和 DesktopCalendar
//      （两个组件仍是独立进程、独立窗口、可分别自由拖动）
//   2. 系统托盘菜单：
//        - 打开时钟和日历
//        - 打开时钟
//        - 打开日历
//        - 开机启动（勾选后写入 HKCU\...\Run）
//        - 退出 MyWigets
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

#include "resource.h"

#include <cstring>
#include <cwchar>

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
constexpr int kMenuOpenClock = 1002;
constexpr int kMenuOpenCalendar = 1003;
constexpr int kMenuOpenLauncher = 1006;
constexpr int kMenuOpenTopBar = 1007;
constexpr int kMenuAutoStart = 1004;
constexpr int kMenuExit = 1005;

#if defined(_M_X64) || defined(_M_AMD64) || defined(__x86_64__)
constexpr wchar_t kClockExe[] = L"DesktopClock-x64.exe";
constexpr wchar_t kCalendarExe[] = L"DesktopCalendar-x64.exe";
constexpr wchar_t kLauncherExe[] = L"DesktopLauncher-x64.exe";
constexpr wchar_t kTopBarExe[] = L"DesktopTopBar-x64.exe";
#else
constexpr wchar_t kClockExe[] = L"DesktopClock-x86.exe";
constexpr wchar_t kCalendarExe[] = L"DesktopCalendar-x86.exe";
constexpr wchar_t kLauncherExe[] = L"DesktopLauncher-x86.exe";
constexpr wchar_t kTopBarExe[] = L"DesktopTopBar-x86.exe";
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
    LaunchWidget(kClockExe, L"Local\\DesktopClock_SingleInstance");
    LaunchWidget(kCalendarExe, L"Local\\DesktopCalendar_SingleInstance");
    LaunchWidget(kLauncherExe, L"Local\\DesktopLauncher_SingleInstance");
    LaunchWidget(kTopBarExe, L"Local\\DesktopTopBar_SingleInstance");
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
    wcscpy_s(nid.szTip, L"MyWigets - 桌面时钟 + 日历 + 顶栏");
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
        LaunchAllWidgets();
        break;
    case kMenuOpenClock:
        LaunchWidget(kClockExe, L"Local\\DesktopClock_SingleInstance");
        break;
    case kMenuOpenCalendar:
        LaunchWidget(kCalendarExe, L"Local\\DesktopCalendar_SingleInstance");
        break;
    case kMenuOpenLauncher:
        LaunchWidget(kLauncherExe, L"Local\\DesktopLauncher_SingleInstance");
        break;
    case kMenuOpenTopBar:
        LaunchWidget(kTopBarExe, L"Local\\DesktopTopBar_SingleInstance");
        break;
    case kMenuAutoStart:
        ToggleAutoStart(hwnd);
        break;
    case kMenuExit:
        DestroyWindow(hwnd);
        break;
    default:
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
    AppendMenuW(menu, MF_STRING, kMenuOpenClock, L"打开时钟");
    AppendMenuW(menu, MF_STRING, kMenuOpenCalendar, L"打开日历");
    AppendMenuW(menu, MF_STRING, kMenuOpenLauncher, L"打开应用管理");
    AppendMenuW(menu, MF_STRING, kMenuOpenTopBar, L"打开顶栏");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    const bool autoStart = IsAutoStartEnabled();
    AppendMenuW(menu, MF_STRING | (autoStart ? MF_CHECKED : MF_UNCHECKED),
                kMenuAutoStart, L"开机启动");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuExit, L"退出 MyWigets");

    SetForegroundWindow(hwnd);
    const UINT flags = TPM_RIGHTBUTTON | TPM_LEFTALIGN | TPM_RETURNCMD | TPM_NONOTIFY;
    const int cmd = TrackPopupMenu(menu, flags, pt.x, pt.y, 0, hwnd, nullptr);
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
