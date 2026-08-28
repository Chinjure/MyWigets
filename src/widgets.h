// widgets.h - 组件进程内线程接口
//
// 全部桌面组件（时钟 / 日历 / 应用管理 / 顶栏 / Dock 栏）都以独立线程形式
// 运行在 MyWigets 进程内：每个组件在线程里创建自己的窗口并泵自己的消息
// 循环（与旧版独立 exe 的 WinMain 行为完全一致），不再存在独立的组件 exe
// 进程。宿主（mywigets_main.cpp）通过本接口启动/停止组件线程，并通过
// g_*Hwnd 原子句柄感知组件主窗口的创建与销毁。

#pragma once

#include <windows.h>
#include <atomic>

// 时钟（原 src/main.cpp）
DWORD WINAPI ClockThreadProc(LPVOID param);
extern std::atomic<HWND> g_clockHwnd;

// 日历（原 src/calendar_main.cpp）
DWORD WINAPI CalendarThreadProc(LPVOID param);
extern std::atomic<HWND> g_calendarHwnd;

// 应用管理（原 src/launcher_main.cpp）
DWORD WINAPI LauncherThreadProc(LPVOID param);
extern std::atomic<HWND> g_launcherHwnd;

// 顶栏（原 src/topbar_main.cpp）
DWORD WINAPI TopbarThreadProc(LPVOID param);
extern std::atomic<HWND> g_topbarHwnd;

// Dock 栏（原 src/dock_main.cpp）
DWORD WINAPI DockThreadProc(LPVOID param);
extern std::atomic<HWND> g_dockHwnd;
