// alt_tab_victim.cpp - Alt+Tab 挂起恢复测试的“受害应用”
//
// 只为 tests/alt_tab_suspend_resume_test.ps1 服务：创建一个普通顶层窗口，
// 由测试脚本把它最小化，等 Dock 的「后台应用挂起」把它整组冻结，再用
// Alt+Tab 验证 Dock 是否在激活前恢复它。
//
// 与测试脚本的契约：
//   alt_tab_victim.exe <readyFile> <heartbeatFile>
//     readyFile     ：窗口显示后写入 "<pid> <hwnd>"（十进制 pid / 十六进制 hwnd）
//     heartbeatFile ：每 100ms 追加一个字节；进程被挂起时停止增长，
//                     恢复后继续增长 —— 比单纯 SendMessageTimeout 更能
//                     证明“消息循环真的重新跑起来了”。
//
// 故意用 /SUBSYSTEM:WINDOWS 编译：控制台窗口会成为第二个顶层窗口，干扰
// Dock 的“这一组窗口”判定和 Alt+Tab 目标，测试就不再是单窗口场景。

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>

#include <cstdio>
#include <string>

namespace {

constexpr wchar_t kWindowClass[] = L"AltTabSuspendVictimWindow";
constexpr wchar_t kWindowTitle[] = L"AltTabSuspendVictim";
constexpr UINT_PTR kHeartbeatTimerId = 1;
constexpr UINT kHeartbeatIntervalMs = 100;

HANDLE g_heartbeat = INVALID_HANDLE_VALUE;

void AppendHeartbeat() {
    if (g_heartbeat == INVALID_HANDLE_VALUE) return;
    const char byte = 'x';
    DWORD written = 0;
    WriteFile(g_heartbeat, &byte, 1, &written, nullptr);
    FlushFileBuffers(g_heartbeat);
}

void WriteReadyFile(const wchar_t* path, DWORD pid, HWND hwnd) {
    HANDLE file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    char text[64] = {};
    const int len = sprintf_s(text, "%lu 0x%llX", pid,
                              static_cast<unsigned long long>(
                                  reinterpret_cast<ULONG_PTR>(hwnd)));
    DWORD written = 0;
    if (len > 0) {
        WriteFile(file, text, static_cast<DWORD>(len), &written, nullptr);
    }
    CloseHandle(file);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_TIMER:
            if (wParam == kHeartbeatTimerId) AppendHeartbeat();
            return 0;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv || argc < 3) {
        if (argv) LocalFree(argv);
        return 2;
    }
    const std::wstring readyPath = argv[1];
    const std::wstring heartbeatPath = argv[2];
    LocalFree(argv);

    g_heartbeat = CreateFileW(
        heartbeatPath.c_str(), FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = kWindowClass;
    if (!RegisterClassExW(&wc)) return 3;

    // 普通无属主、非工具窗的顶层窗口：Dock 的 EnumAppWindowProc 与
    // Windows Alt+Tab 都会把它当成正常的应用窗口。
    HWND hwnd = CreateWindowExW(
        0, kWindowClass, kWindowTitle, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
        CW_USEDEFAULT, 640, 420, nullptr, nullptr, instance, nullptr);
    if (!hwnd) return 4;

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    WriteReadyFile(readyPath.c_str(), GetCurrentProcessId(), hwnd);
    SetTimer(hwnd, kHeartbeatTimerId, kHeartbeatIntervalMs, nullptr);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_heartbeat != INVALID_HANDLE_VALUE) CloseHandle(g_heartbeat);
    return static_cast<int>(msg.wParam);
}
