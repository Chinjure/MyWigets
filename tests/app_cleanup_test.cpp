// app_cleanup_test.cpp - 清理归因/策略/一次性 Job 终止测试
//
// 纯归因用合成 Evidence；集成部分只操作测试自己在临时目录复制出的
// app.exe，不碰系统里的真实应用。

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

#include "app_cleanup.h"
#include "app_suspend.h"

namespace {

int g_failures = 0;

void Check(bool ok, const wchar_t* name) {
    wprintf(ok ? L"[PASS] %ls\n" : L"[FAIL] %ls\n", name);
    if (!ok) ++g_failures;
}

std::wstring ExeDir() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring s(path);
    const size_t pos = s.find_last_of(L"\\/");
    return pos == std::wstring::npos ? std::wstring() : s.substr(0, pos);
}

std::wstring Join(const std::wstring& a, const std::wstring& b) {
    if (a.empty()) return b;
    if (a.back() == L'\\') return a + b;
    return a + L"\\" + b;
}

DWORD CurrentSession() {
    DWORD session = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &session);
    return session;
}

appcleanup::ProcessInfo Proc(DWORD pid, DWORD parent, const std::wstring& image,
                             ULONGLONG created, DWORD session = 1,
                             bool window = false) {
    appcleanup::ProcessInfo p;
    p.pid = pid;
    p.parentPid = parent;
    p.imagePath = image;
    p.creationTime = created;
    p.sessionId = session;
    p.userWindow = window;
    return p;
}

appcleanup::CleanupRequest Request(const std::wstring& key) {
    appcleanup::CleanupRequest r;
    r.appKey = key;
    r.displayName = L"TestApp";
    return r;
}

appcleanup::CleanupOptions Options() {
    appcleanup::CleanupOptions o;
    o.minProcessAgeMs = 0;
    o.stopServices = false;
    return o;
}

appcleanup::TrayProbe NoTray() {
    return [](const appcleanup::CleanupRequest&,
              const std::vector<DWORD>&) { return false; };
}

void TestPolicy() {
    Check(appcleanup::ShouldCleanupResidual(true, true, true, false, false,
                                           false, true, false),
          L"策略：固定第三方、无窗口无托盘 -> 清理");
    Check(!appcleanup::ShouldCleanupResidual(true, true, true, true, false,
                                            false, true, false),
          L"策略：有窗口 -> 不清理");
    Check(!appcleanup::ShouldCleanupResidual(true, true, true, false, true,
                                            false, true, false),
          L"策略：有托盘 -> 不清理");
    Check(!appcleanup::ShouldCleanupResidual(true, true, true, false, false,
                                            true, true, false),
          L"策略：启动中 -> 不清理");
    Check(!appcleanup::ShouldCleanupResidual(false, true, true, false, false,
                                            false, true, false),
          L"策略：未固定 -> 不清理");
    Check(!appcleanup::ShouldCleanupResidual(true, false, true, false, false,
                                            false, true, false),
          L"策略：非第三方 -> 不清理");
    // 该函数没有 userExcluded 参数：SuspendException 不影响清理。
}

void TestAttribution() {
    const std::wstring root = L"c:\\program files\\testapp";
    const std::wstring app = root + L"\\app.exe";
    const std::wstring helper = root + L"\\helper.exe";
    const std::wstring jobHelper = L"c:\\users\\u\\appdata\\local\\temp\\jobhelper.exe";

    appcleanup::Evidence ev;
    ev.processes.push_back(Proc(100, 1, app, 1000, CurrentSession()));
    ev.processes.push_back(Proc(101, 100, helper, 1100, CurrentSession()));
    ev.processes.push_back(Proc(102, 1, jobHelper, 1200, CurrentSession()));
    ev.processes.push_back(Proc(103, 1, L"c:\\other\\foreign.exe", 1300,
                                CurrentSession()));

    appcleanup::CleanupRequest req = Request(app);
    req.jobMembers.push_back(appsuspend::ProcessRef{102, 1200});
    req.allAppKeys.push_back(L"c:\\other\\foreign.exe");
    appcleanup::Plan plan =
        appcleanup::BuildPlan(req, ev, NoTray(), Options());
    Check(!plan.abort, L"归因：不应 abort");
    bool hasApp = false, hasHelper = false, hasJob = false, hasForeign = false;
    for (const auto& t : plan.targets) {
        if (t.pid == 100) hasApp = true;
        if (t.pid == 101) hasHelper = true;
        if (t.pid == 102) hasJob = true;
        if (t.pid == 103) hasForeign = true;
    }
    Check(hasApp && hasHelper && hasJob && !hasForeign,
          L"归因：精确+后代+Job，排除其它 Dock 应用");

    // 安装根里父进程已退出的不同 exe helper。
    appcleanup::Evidence ev2;
    ev2.processes.push_back(Proc(200, 1, root + L"\\orphan.exe", 2000,
                                 CurrentSession()));
    appcleanup::Plan p2 = appcleanup::BuildPlan(
        Request(root + L"\\app.exe"), ev2, NoTray(), Options());
    Check(p2.targets.size() == 1 && p2.targets[0].pid == 200,
          L"归因：窄安装根无主进程时收 orphan helper");

    // 可见窗口是硬边界。
    appcleanup::Evidence ev3 = ev;
    ev3.userWindowPids.push_back(101);
    appcleanup::Plan p3 =
        appcleanup::BuildPlan(Request(app), ev3, NoTray(), Options());
    Check(p3.abort, L"归因：候选有窗口 -> abort");

    // 显式关闭超时强制退出：关掉窗口边界后同样的证据不再 abort。
    appcleanup::CleanupOptions forceWindowOptions = Options();
    forceWindowOptions.ignoreUserWindows = true;
    appcleanup::Plan p3f =
        appcleanup::BuildPlan(Request(app), ev3, NoTray(), forceWindowOptions);
    Check(!p3f.abort && p3f.targets.size() >= 2,
          L"归因：ignoreUserWindows 强杀正在使用的窗口");

    // 托盘是硬边界。
    appcleanup::Plan p4 = appcleanup::BuildPlan(
        Request(app), ev,
        [](const appcleanup::CleanupRequest&,
           const std::vector<DWORD>&) { return true; },
        Options());
    Check(p4.abort, L"归因：有托盘 -> abort");

    // 夸克式：主程序在窄安装根，更新服务在兄弟产品目录。
    appcleanup::Evidence ev5;
    ev5.processes.push_back(Proc(300, 1,
                                 L"c:\\program files\\quarkclouddrive\\7.4.2.822\\quark_cloud_drive.exe",
                                 3000, CurrentSession()));
    ev5.processes.push_back(Proc(301, 1,
                                 L"c:\\program files\\quarkclouddriveupdater\\updater.exe",
                                 3100, 0));
    ev5.processes.push_back(Proc(302, 301,
                                 L"c:\\program files\\quarkclouddriveupdater\\helper.exe",
                                 3200, 0));
    appcleanup::ServiceInfo svc;
    svc.name = L"QuarkCloudDriveUpdaterService1.0.0.11";
    svc.displayName = L"QuarkCloudDriveUpdater Service 1.0.0.11";
    svc.binaryPath =
        L"c:\\program files\\quarkclouddriveupdater\\updater.exe";
    svc.pid = 301;
    svc.type = SERVICE_WIN32_OWN_PROCESS;
    svc.state = SERVICE_RUNNING;
    svc.acceptStop = true;
    ev5.services.push_back(svc);
    appcleanup::Plan p5 = appcleanup::BuildPlan(
        Request(L"c:\\program files\\quarkclouddrive\\quark_cloud_drive.exe"),
        ev5, NoTray(), Options());
    Check(p5.services.size() == 1 && p5.services[0].pid == 301,
          L"归因：夸克产品家族服务匹配");
    bool child = false;
    for (const auto& t : p5.targets) {
        if (t.pid == 302) child = true;
        if (t.pid == 301) child = false;  // 服务宿主不作为进程目标
    }
    Check(child, L"归因：服务后代纳入目标，服务宿主不直接 kill");
}

bool SpawnSelf(const std::wstring& exe, const std::wstring& args,
               PROCESS_INFORMATION* out) {
    std::wstring cmd = L"\"" + exe + L"\" " + args;
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        return false;
    }
    *out = pi;
    return true;
}

void TestIntegration() {
    const std::wstring dir =
        Join(ExeDir(), L"cleanup_test_tmp_" +
                           std::to_wstring(GetCurrentProcessId()));
    if (!CreateDirectoryW(dir.c_str(), nullptr)) {
        const DWORD err = GetLastError();
        if (err != ERROR_ALREADY_EXISTS) {
            wprintf(L"mkdir failed err=%lu dir=%ls\n", err, dir.c_str());
        }
    }
    const std::wstring app = Join(dir, L"app.exe");
    wchar_t self[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    if (!CopyFileW(self, app.c_str(), FALSE)) {
        wprintf(L"copy failed err=%lu self=%ls app=%ls\n", GetLastError(), self,
                app.c_str());
        Check(false, L"集成：复制测试 exe");
        return;
    }
    struct Cleanup {
        std::wstring dir;
        ~Cleanup() {
            DeleteFileW(Join(dir, L"app.exe").c_str());
            RemoveDirectoryW(dir.c_str());
        }
    } cleanup{dir};

    PROCESS_INFORMATION pi{};
    if (!SpawnSelf(app, L"--sleep 60000", &pi)) {
        Check(false, L"集成：启动测试 app.exe");
        return;
    }
    CloseHandle(pi.hThread);
    Sleep(300);

    appcleanup::CleanupRequest req = Request(appsuspend::NormalizePath(app));
    appcleanup::CleanupOptions opt = Options();
    opt.dryRun = true;
    appcleanup::CleanupResult dry =
        appcleanup::CleanupOnce(req, opt, NoTray());
    Check(dry.verdict == appcleanup::CleanupVerdict::DryRun &&
              WaitForSingleObject(pi.hProcess, 0) == WAIT_TIMEOUT,
          L"集成：dry-run 不杀进程");
    opt.dryRun = false;
    const DWORD waitBefore = WaitForSingleObject(pi.hProcess, 0);
    appcleanup::CleanupResult result =
        appcleanup::CleanupOnce(req, opt, NoTray());
    const DWORD waitAfter = WaitForSingleObject(pi.hProcess, 4000);
    Check(waitBefore == WAIT_TIMEOUT && waitAfter == WAIT_OBJECT_0,
          L"集成：无窗口进程被一次性清理");
    Check(result.terminated >= 1, L"集成：终止计数 >= 1");
    CloseHandle(pi.hProcess);

    // 有可见窗口的进程：默认窗口边界必须挡住；显式关闭超时（ignoreUserWindows）
    // 必须照样杀掉。
    PROCESS_INFORMATION pw{};
    if (!SpawnSelf(app, L"--window 60000", &pw)) {
        Check(false, L"集成：启动有窗口的测试 app.exe");
        return;
    }
    CloseHandle(pw.hThread);
    Sleep(600);
    const DWORD winWaitBefore = WaitForSingleObject(pw.hProcess, 0);
    appcleanup::CleanupOptions winOpt = Options();
    appcleanup::CleanupResult winBlocked =
        appcleanup::CleanupOnce(req, winOpt, NoTray());
    const DWORD winWaitAfterBlocked = WaitForSingleObject(pw.hProcess, 0);
    Check(winBlocked.verdict ==
                  appcleanup::CleanupVerdict::AbortedOpenWindow &&
              winWaitBefore == WAIT_TIMEOUT &&
              winWaitAfterBlocked == WAIT_TIMEOUT,
          L"集成：有窗口 -> 窗口边界挡住，不杀");
    winOpt.ignoreUserWindows = true;
    appcleanup::CleanupResult winForced =
        appcleanup::CleanupOnce(req, winOpt, NoTray());
    const DWORD winWaitAfterForce = WaitForSingleObject(pw.hProcess, 4000);
    Check(winWaitAfterForce == WAIT_OBJECT_0 && winForced.terminated >= 1,
          L"集成：ignoreUserWindows 强杀正在使用的窗口");
    CloseHandle(pw.hProcess);
}

int RunSelfMode(int argc, wchar_t** argv) {
    if (argc >= 3 && _wcsicmp(argv[1], L"--sleep") == 0) {
        Sleep(static_cast<DWORD>(_wtoi(argv[2])));
        return 0;
    }
    if (argc >= 3 && _wcsicmp(argv[1], L"--window") == 0) {
        // 造一个"可见"顶层窗口（放在屏幕外，避免测试时弹窗干扰），持续存活
        // 到超时；用于验证窗口边界与 ignoreUserWindows 的强杀行为。
        const DWORD ms = static_cast<DWORD>(_wtoi(argv[2]));
        WNDCLASSW wc{};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"AppCleanupTestWindow";
        RegisterClassW(&wc);
        HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"cleanup test",
                                    WS_OVERLAPPEDWINDOW | WS_VISIBLE, -2000,
                                    -2000, 320, 200, nullptr, nullptr,
                                    wc.hInstance, nullptr);
        if (!hwnd) return 2;
        const ULONGLONG deadline = GetTickCount64() + ms;
        MSG msg;
        while (GetTickCount64() < deadline) {
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            Sleep(10);
        }
        DestroyWindow(hwnd);
        return 0;
    }
    if (argc >= 3 && _wcsicmp(argv[1], L"--services") == 0) {
        const auto services =
            appcleanup::ServicesForApp(argv[2], L"");
        wprintf(L"services=%zu\n", services.size());
        for (const auto& s : services) {
            wprintf(L"%ls state=%lu start=%ls\n", s.name.c_str(), s.state,
                    s.binaryPath.c_str());
        }
        return 0;
    }
    return -1;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    const int self = RunSelfMode(argc, argv);
    if (self >= 0) return self;

    wprintf(L"=== app_cleanup tests ===\n");
    TestPolicy();
    TestAttribution();
    TestIntegration();
    wprintf(g_failures == 0 ? L"ALL PASS\n" : L"FAILURES: %d\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
