// app_suspend_test.cpp - app_suspend 的纯函数单元测试 + 真实挂起/恢复集成测试
//
// 集成测试不碰系统里已有的应用：把本测试 exe 复制成临时目录里的 app.exe /
// helper.exe，由 app.exe 再派生 helper.exe（模拟“主进程 + 不同 exe 的辅助
// 进程”），然后只对临时目录里的这一组进程调用 Suspender。通过两个心跳文件
// 是否停止增长，验证“辅助进程真的被一起挂起”，再恢复并验证心跳继续。

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <tlhelp32.h>

#include <fcntl.h>
#include <io.h>

#include <cstdio>
#include <string>
#include <unordered_set>
#include <vector>

#include "app_suspend.h"

namespace {

int g_failures = 0;

void Check(bool condition, const wchar_t* name) {
    if (condition) {
        wprintf(L"[PASS] %ls\n", name);
    } else {
        wprintf(L"[FAIL] %ls\n", name);
        ++g_failures;
    }
}

std::wstring Join(const std::wstring& a, const std::wstring& b) {
    if (a.empty()) return b;
    if (a.back() == L'\\') return a + b;
    return a + L"\\" + b;
}

ULONGLONG Tick() { return GetTickCount64(); }

void HeartbeatLoop(const std::wstring& path) {
    HANDLE file = CreateFileW(
        path.c_str(), FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    const char byte = 'x';
    for (;;) {
        DWORD written = 0;
        WriteFile(file, &byte, 1, &written, nullptr);
        FlushFileBuffers(file);
        Sleep(80);
    }
}

bool SpawnProcess(const std::wstring& exe, const std::wstring& commandLine,
                  const std::wstring& workDir, PROCESS_INFORMATION* out) {
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::wstring mutableCmd = commandLine;
    const BOOL ok = CreateProcessW(
        exe.c_str(), mutableCmd.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW, nullptr, workDir.c_str(), &si, &pi);
    if (!ok) return false;
    if (out) {
        *out = pi;
    } else {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    return true;
}

ULONGLONG FileSize(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) {
        return 0;
    }
    return (static_cast<ULONGLONG>(data.nFileSizeHigh) << 32) |
           data.nFileSizeLow;
}

bool WaitForFile(const std::wstring& path, ULONGLONG minSize,
                 DWORD timeoutMs) {
    const ULONGLONG deadline = Tick() + timeoutMs;
    while (Tick() < deadline) {
        if (FileSize(path) >= minSize) return true;
        Sleep(50);
    }
    return false;
}

bool WaitForGrowth(const std::wstring& path, ULONGLONG before,
                   DWORD timeoutMs) {
    const ULONGLONG deadline = Tick() + timeoutMs;
    while (Tick() < deadline) {
        if (FileSize(path) > before) return true;
        Sleep(50);
    }
    return false;
}

void TerminateImage(const std::wstring& imagePath) {
    const std::wstring want = appsuspend::NormalizePath(imagePath);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            HANDLE proc =
                OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE,
                            FALSE, pe.th32ProcessID);
            if (!proc) continue;
            wchar_t image[32768] = {};
            DWORD len = 32760;
            if (QueryFullProcessImageNameW(proc, 0, image, &len) && len > 0 &&
                appsuspend::NormalizePath(image) == want) {
                TerminateProcess(proc, 0);
            }
            CloseHandle(proc);
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
}

appsuspend::ProcessEntry MakeEntry(DWORD pid, DWORD parent,
                                   const wchar_t* path,
                                   ULONGLONG creation) {
    appsuspend::ProcessEntry entry;
    entry.pid = pid;
    entry.parentPid = parent;
    entry.creationTime = creation;
    entry.imagePath = appsuspend::NormalizePath(path);
    return entry;
}

void TestSelection() {
    {
        std::vector<appsuspend::ProcessEntry> procs;
        procs.push_back(MakeEntry(10, 1, L"C:\\Apps\\Chrome\\chrome.exe", 100));
        procs.push_back(MakeEntry(11, 10, L"C:\\Apps\\Chrome\\chrome.exe", 200));
        procs.push_back(MakeEntry(12, 10, L"C:\\Apps\\Chrome\\chrome.exe", 300));
        procs.push_back(MakeEntry(13, 1, L"C:\\Apps\\Other\\chrome.exe", 400));
        const auto sel = appsuspend::SelectAppProcesses(
            L"C:\\Apps\\Chrome\\chrome.exe", procs);
        Check(sel.processes.size() == 3, L"同一 exe 的全部实例都选中");
        Check(sel.exactMatches == 3, L"精确匹配计数正确");
    }

    {
        std::vector<appsuspend::ProcessEntry> procs;
        procs.push_back(MakeEntry(20, 1, L"C:\\Apps\\WeChat\\WeChat.exe", 1000));
        procs.push_back(
            MakeEntry(21, 20, L"C:\\Apps\\WeChat\\WeChatAppEx.exe", 1100));
        procs.push_back(
            MakeEntry(22, 20, L"C:\\Windows\\System32\\conhost.exe", 1150));
        procs.push_back(
            MakeEntry(23, 20, L"C:\\Other\\browser.exe", 1200));
        procs.push_back(
            MakeEntry(24, 21, L"C:\\Apps\\WeChat\\sub\\helper.dll.exe", 1300));
        procs.push_back(
            MakeEntry(25, 20, L"C:\\Apps\\WeChat\\stale.exe", 900));
        const auto sel = appsuspend::SelectAppProcesses(
            L"C:\\Apps\\WeChat\\WeChat.exe", procs);
        bool has21 = false, has24 = false, bad = false;
        for (const auto& p : sel.processes) {
            if (p.pid == 21) has21 = true;
            if (p.pid == 24) has24 = true;
            if (p.pid == 22 || p.pid == 23 || p.pid == 25) bad = true;
        }
        Check(has21, L"同安装目录内不同 exe 的辅助进程被选中");
        Check(has24, L"辅助进程的后代也在安装目录树内时被选中");
        Check(!bad, L"系统进程/外部程序/创建时间早于父进程的 PID 被排除");

        // Job 不可用时的兜底：跨目录后代也要收进来（应用把 helper.exe
        // 落到 %TEMP%/更新目录再拉起）。默认选择仍保持保守口径。
        const auto extended = appsuspend::SelectAppProcesses(
            L"C:\\Apps\\WeChat\\WeChat.exe", procs, nullptr,
            /*includeExternalDescendants=*/true);
        bool extendedHas23 = false;
        for (const auto& p : extended.processes) {
            if (p.pid == 23) extendedHas23 = true;
        }
        Check(extendedHas23, L"兜底模式选中跨目录后代");
    }

    {
        std::vector<appsuspend::ProcessEntry> procs;
        procs.push_back(
            MakeEntry(30, 1, L"C:\\Windows\\notepad.exe", 1000));
        procs.push_back(MakeEntry(31, 30, L"C:\\Windows\\same_dir.exe", 1100));
        procs.push_back(
            MakeEntry(32, 30, L"C:\\Windows\\subdir\\outside.exe", 1200));
        const auto sel = appsuspend::SelectAppProcesses(
            L"C:\\Windows\\notepad.exe", procs);
        bool has31 = false, has32 = false;
        for (const auto& p : sel.processes) {
            if (p.pid == 31) has31 = true;
            if (p.pid == 32) has32 = true;
        }
        Check(sel.broadRoot, L"Windows 目录被识别为宽泛安装根");
        Check(has31, L"宽泛根下仍允许同目录辅助进程");
        Check(!has32, L"宽泛根下禁止递归子目录，避免吞系统进程");
    }

    {
        std::vector<appsuspend::ProcessEntry> procs;
        procs.push_back(MakeEntry(40, 1, L"C:\\Apps\\A\\app.exe", 1000));
        procs.push_back(MakeEntry(41, 40, L"C:\\Apps\\B\\app.exe", 1100));
        const auto sel = appsuspend::SelectAppProcesses(
            L"C:\\Apps\\A\\app.exe", procs);
        Check(sel.processes.size() == 1,
              L"同名但不同目录的 exe 不会被合并");
    }

    {
        std::vector<appsuspend::ProcessEntry> procs;
        procs.push_back(MakeEntry(50, 1, L"C:\\Apps\\A\\launcher.exe", 900));
        procs.push_back(MakeEntry(51, 50, L"C:\\Apps\\A\\app.exe", 1000));
        const auto sel = appsuspend::SelectAppProcesses(
            L"C:\\Apps\\A\\app.exe", procs);
        Check(sel.processes.size() == 1 && sel.processes[0].pid == 51,
              L"启动器父进程不会被当成应用进程");
    }

    {
        std::vector<appsuspend::ProcessEntry> procs;
        procs.push_back(
            MakeEntry(60, 1, L"C:\\Office\\WINWORD.EXE", 1000));
        procs.push_back(
            MakeEntry(61, 60, L"C:\\Office\\EXCEL.EXE", 1100));
        std::unordered_set<std::wstring> foreign;
        foreign.insert(
            appsuspend::NormalizePath(L"C:\\Office\\EXCEL.EXE"));
        const auto sel = appsuspend::SelectAppProcesses(
            L"C:\\Office\\WINWORD.EXE", procs, &foreign);
        Check(sel.processes.size() == 1 && sel.processes[0].pid == 60,
              L"同目录但属于另一个 Dock 应用的后代不被吞并");
    }

    {
        bool broad = false;
        const std::wstring root = appsuspend::InstallRootForApp(
            L"C:\\Apps\\Product\\bin\\app.exe", &broad);
        Check(root == L"c:\\apps\\product" && !broad,
              L"bin 目录上提到产品安装根");
        const std::wstring sysRoot = appsuspend::InstallRootForApp(
            L"C:\\Windows\\System32\\notepad.exe", &broad);
        Check(sysRoot == L"c:\\windows\\system32" && broad,
              L"System32 被识别为宽泛根");
    }

    {
        Check(appsuspend::IsNeverSuspendImage(L"C:\\Windows\\explorer.exe"),
              L"explorer 永不可挂起");
        Check(appsuspend::IsNeverSuspendImage(L"C:\\X\\conhost.exe"),
              L"conhost 永不可挂起");
        Check(!appsuspend::IsNeverSuspendImage(L"C:\\Apps\\App\\app.exe"),
              L"普通应用可挂起");
    }
}

void TestPausePolicy() {
    Check(!appsuspend::ShouldPauseApp(false, false, false),
          L"未打开的应用没有暂停对象");
    Check(!appsuspend::ShouldPauseApp(true, true, false),
          L"有可见且未最小化窗口的应用不暂停");
    Check(appsuspend::ShouldPauseApp(true, false, false),
          L"窗口全部最小化或纯托盘（无可见窗口）可暂停");
    Check(!appsuspend::ShouldPauseApp(true, false, true),
          L"用户勾选「不暂停此应用」后永久豁免");
}

// Job 沙箱集成测试：
//   1. 主进程在 ResumeThread 之前被 AdoptProcess 放进应用 Job；
//   2. 主进程从“另一个目录”拉起 helper.exe（模拟应用把 exe 落到
//      %TEMP%/更新目录后再开子进程）；
//   3. 不靠安装目录树，Job 自动把 helper 收进来，SuspendMany 必须
//      把两个进程一起冻结。
bool TestJobSandbox() {
    wchar_t self[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, self, MAX_PATH) == 0) {
        ++g_failures;
        wprintf(L"[FAIL] Job 测试取自身路径失败：%lu\n", GetLastError());
        return false;
    }
    const std::wstring dir =
        Join(appsuspend::ParentDirOf(self),
             L"mywigets_job_test_" + std::to_wstring(GetCurrentProcessId()) +
                 L"_" + std::to_wstring(GetTickCount64()));
    const std::wstring outside = Join(dir, L"outside");
    if (!CreateDirectoryW(dir.c_str(), nullptr) ||
        !CreateDirectoryW(outside.c_str(), nullptr)) {
        ++g_failures;
        wprintf(L"[FAIL] Job 测试目录创建失败：%lu（%ls）\n", GetLastError(),
                dir.c_str());
        return false;
    }

    const std::wstring appExe = Join(dir, L"app.exe");
    const std::wstring helperExe = Join(outside, L"helper.exe");
    const std::wstring appBeat = Join(dir, L"app.heartbeat");
    const std::wstring helperBeat = Join(outside, L"helper.heartbeat");
    bool copied = CopyFileW(self, appExe.c_str(), FALSE) != FALSE;
    copied = copied && CopyFileW(self, helperExe.c_str(), FALSE) != FALSE;
    if (!copied) {
        ++g_failures;
        wprintf(L"[FAIL] Job 测试复制 exe 失败：%lu\n", GetLastError());
        RemoveDirectoryW(outside.c_str());
        RemoveDirectoryW(dir.c_str());
        return false;
    }

    const std::wstring appCmd =
        L"\"" + appExe + L"\" --spawn-tree \"" + dir + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION appPi{};
    std::wstring mutableCmd = appCmd;
    const BOOL started = CreateProcessW(
        appExe.c_str(), mutableCmd.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, dir.c_str(), &si,
        &appPi);
    if (!started) {
        ++g_failures;
        wprintf(L"[FAIL] Job 测试启动失败：%lu\n", GetLastError());
        DeleteFileW(appExe.c_str());
        DeleteFileW(helperExe.c_str());
        RemoveDirectoryW(outside.c_str());
        RemoveDirectoryW(dir.c_str());
        return false;
    }

    appsuspend::Suspender suspender(false, 0, L"SuspendedStateTest", true);
    const bool init = suspender.Initialize();
    Check(init, L"Job 测试控制器初始化");
    std::wstring detail;
    const bool adopted =
        init && suspender.AdoptProcess(appsuspend::NormalizePath(appExe),
                                       appPi.hProcess, &detail);
    Check(adopted, L"启动时把主进程加入应用 Job");
    if (adopted) {
        Check(suspender.JobMemberCount(appsuspend::NormalizePath(appExe)) >= 1,
              L"Job 成员查询包含主进程");
    }
    ResumeThread(appPi.hThread);
    CloseHandle(appPi.hThread);

    bool ok = adopted && WaitForFile(appBeat, 1, 6000) &&
              WaitForFile(helperBeat, 1, 6000);
    Check(ok, L"跨目录 helper 心跳已启动");
    const std::wstring key = appsuspend::NormalizePath(appExe);
    if (ok) {
        // 等 helper 真正进入 Job；Job 继承是创建时完成的，这里只是查表。
        const size_t members = suspender.JobMemberCount(key);
        if (members < 2) {
            wprintf(L"  job members=%zu（期望 >=2）\n", members);
        }
        Check(members >= 2, L"跨目录 helper 自动继承 Job");

        Sleep(250);
        std::vector<appsuspend::SuspendRequest> requests;
        requests.push_back({key, L"job-test"});
        auto results = suspender.SuspendMany(requests);
        const bool suspended =
            results.size() == 1 &&
            results[0].code == appsuspend::SuspendCode::Suspended &&
            results[0].jobSandbox && results[0].jobMembers >= 2 &&
            results[0].suspended >= 2;
        Check(suspended, L"Job 成员（含跨目录 helper）整组挂起");
        if (!results.empty()) {
            wprintf(L"  job result: code=%d matched=%zu suspended=%zu "
                    L"job=%d members=%zu detail=%ls\n",
                    static_cast<int>(results[0].code), results[0].matched,
                    results[0].suspended, results[0].jobSandbox ? 1 : 0,
                    results[0].jobMembers, results[0].detail.c_str());
        }
        if (suspended) {
            // 重复调用必须是幂等的：不能对同一进程再次 NtSuspendProcess，
            // 否则 Resume 只减一次计数，应用会永久停在挂起态。
            auto again = suspender.SuspendMany(requests);
            Check(again.size() == 1 &&
                      again[0].code ==
                          appsuspend::SuspendCode::AlreadySuspended,
                  L"重复 SuspendMany 幂等，不重复冻结");
            Sleep(300);
            const ULONGLONG appFrozen = FileSize(appBeat);
            const ULONGLONG helperFrozen = FileSize(helperBeat);
            Sleep(700);
            Check(FileSize(appBeat) == appFrozen,
                  L"Job 挂起后主进程心跳停止");
            Check(FileSize(helperBeat) == helperFrozen,
                  L"Job 挂起后跨目录 helper 心跳停止");
            std::wstring resumeDetail;
            const bool resumed = suspender.Resume(key, &resumeDetail);
            Check(resumed, L"Job 组恢复成功");
            Check(WaitForGrowth(appBeat, appFrozen, 2500),
                  L"Job 恢复后主进程心跳继续");
            Check(WaitForGrowth(helperBeat, helperFrozen, 2500),
                  L"Job 恢复后跨目录 helper 心跳继续");
        }
    }

    if (appPi.hProcess) {
        TerminateProcess(appPi.hProcess, 0);
        WaitForSingleObject(appPi.hProcess, 1000);
        CloseHandle(appPi.hProcess);
    }
    TerminateImage(helperExe);
    Sleep(150);
    DeleteFileW(appBeat.c_str());
    DeleteFileW(helperBeat.c_str());
    DeleteFileW(appExe.c_str());
    DeleteFileW(helperExe.c_str());
    RemoveDirectoryW(outside.c_str());
    RemoveDirectoryW(dir.c_str());
    return ok && g_failures == 0;
}

// 父进程退出后，Job 仍持有 helper：此时按 exe 路径已经找不到“主进程”，
// 旧选择会返回 NoProcesses；Job 成员必须仍能整组挂起/恢复。
bool TestJobSurvivesRootExit() {
    wchar_t self[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, self, MAX_PATH) == 0) {
        ++g_failures;
        wprintf(L"[FAIL] 父退出测试取自身路径失败：%lu\n", GetLastError());
        return false;
    }
    const std::wstring dir =
        Join(appsuspend::ParentDirOf(self),
             L"mywigets_job_exit_test_" +
                 std::to_wstring(GetCurrentProcessId()) + L"_" +
                 std::to_wstring(GetTickCount64()));
    const std::wstring outside = Join(dir, L"outside");
    if (!CreateDirectoryW(dir.c_str(), nullptr) ||
        !CreateDirectoryW(outside.c_str(), nullptr)) {
        ++g_failures;
        wprintf(L"[FAIL] 父退出测试目录创建失败：%lu\n", GetLastError());
        return false;
    }
    const std::wstring appExe = Join(dir, L"app.exe");
    const std::wstring helperExe = Join(outside, L"helper.exe");
    const std::wstring helperBeat = Join(outside, L"helper.heartbeat");
    bool copied = CopyFileW(self, appExe.c_str(), FALSE) != FALSE;
    copied = copied && CopyFileW(self, helperExe.c_str(), FALSE) != FALSE;
    if (!copied) {
        ++g_failures;
        wprintf(L"[FAIL] 父退出测试复制 exe 失败：%lu\n", GetLastError());
        RemoveDirectoryW(outside.c_str());
        RemoveDirectoryW(dir.c_str());
        return false;
    }

    const std::wstring appCmd =
        L"\"" + appExe + L"\" --spawn-tree-exit \"" + dir + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION appPi{};
    std::wstring mutableCmd = appCmd;
    const BOOL started = CreateProcessW(
        appExe.c_str(), mutableCmd.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, dir.c_str(), &si,
        &appPi);
    if (!started) {
        ++g_failures;
        wprintf(L"[FAIL] 父退出测试启动失败：%lu\n", GetLastError());
        DeleteFileW(appExe.c_str());
        DeleteFileW(helperExe.c_str());
        RemoveDirectoryW(outside.c_str());
        RemoveDirectoryW(dir.c_str());
        return false;
    }

    appsuspend::Suspender suspender(false, 0, L"SuspendedStateTest", true);
    const bool init = suspender.Initialize();
    std::wstring detail;
    const bool adopted =
        init && suspender.AdoptProcess(appsuspend::NormalizePath(appExe),
                                       appPi.hProcess, &detail);
    Check(adopted, L"父退出测试：主进程加入 Job");
    ResumeThread(appPi.hThread);
    CloseHandle(appPi.hThread);

    bool ok = adopted && WaitForFile(helperBeat, 1, 6000);
    if (ok) {
        // 等主进程真正退出；Job 里的 helper 不应随之退出。
        const DWORD wait = WaitForSingleObject(appPi.hProcess, 5000);
        Check(wait == WAIT_OBJECT_0, L"父退出测试：主进程已退出");
        Check(WaitForFile(helperBeat, 1, 1000) != FALSE,
              L"父退出后 helper 仍存活");
        const std::wstring key = appsuspend::NormalizePath(appExe);
        Check(suspender.JobMemberCount(key) >= 1,
              L"父退出后 Job 仍持有 helper");
        std::vector<appsuspend::SuspendRequest> requests;
        requests.push_back({key, L"job-exit-test"});
        auto results = suspender.SuspendMany(requests);
        const bool suspended =
            results.size() == 1 &&
            results[0].code == appsuspend::SuspendCode::Suspended &&
            results[0].jobSandbox && results[0].suspended >= 1;
        Check(suspended, L"无主进程时按 Job 成员挂起 helper");
        if (!results.empty()) {
            wprintf(L"  exit result: code=%d suspended=%zu job=%d "
                    L"members=%zu detail=%ls\n",
                    static_cast<int>(results[0].code),
                    results[0].suspended, results[0].jobSandbox ? 1 : 0,
                    results[0].jobMembers, results[0].detail.c_str());
        }
        if (suspended) {
            Sleep(300);
            const ULONGLONG frozen = FileSize(helperBeat);
            Sleep(700);
            Check(FileSize(helperBeat) == frozen,
                  L"父退出场景 helper 心跳停止");
            std::wstring resumeDetail;
            Check(suspender.Resume(key, &resumeDetail),
                  L"父退出场景 Job 组恢复成功");
            Check(WaitForGrowth(helperBeat, frozen, 2500),
                  L"父退出场景 helper 心跳继续");
        }
    }

    if (appPi.hProcess) {
        WaitForSingleObject(appPi.hProcess, 1000);
        CloseHandle(appPi.hProcess);
    }
    TerminateImage(helperExe);
    Sleep(150);
    DeleteFileW(helperBeat.c_str());
    DeleteFileW(appExe.c_str());
    DeleteFileW(helperExe.c_str());
    RemoveDirectoryW(outside.c_str());
    RemoveDirectoryW(dir.c_str());
    return ok && g_failures == 0;
}

bool TestSuspendResume() {
    // 测试目录放在测试 exe 旁边（bin 下），不用 %TEMP%：受限运行环境可能
    // 只允许工作区写入，%TEMP% 会以 ERROR_ACCESS_DENIED 创建失败。
    wchar_t self[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, self, MAX_PATH) == 0) {
        ++g_failures;
        wprintf(L"[FAIL] GetModuleFileNameW 失败：%lu\n", GetLastError());
        return false;
    }
    const std::wstring dir =
        Join(appsuspend::ParentDirOf(self),
             L"mywigets_suspend_test_" +
                 std::to_wstring(GetCurrentProcessId()) + L"_" +
                 std::to_wstring(GetTickCount64()));
    if (!CreateDirectoryW(dir.c_str(), nullptr)) {
        ++g_failures;
        wprintf(L"[FAIL] 创建测试目录失败：%lu（%ls）\n", GetLastError(),
                dir.c_str());
        return false;
    }

    const std::wstring appExe = Join(dir, L"app.exe");
    const std::wstring helperExe = Join(dir, L"helper.exe");
    const std::wstring appBeat = Join(dir, L"app.heartbeat");
    const std::wstring helperBeat = Join(dir, L"helper.heartbeat");
    bool copied = CopyFileW(self, appExe.c_str(), FALSE) != FALSE;
    copied = copied && CopyFileW(self, helperExe.c_str(), FALSE) != FALSE;
    if (!copied) {
        ++g_failures;
        wprintf(L"[FAIL] 复制测试 exe 失败：%lu\n", GetLastError());
        RemoveDirectoryW(dir.c_str());
        return false;
    }

    PROCESS_INFORMATION appPi{};
    const std::wstring appCmd =
        L"\"" + appExe + L"\" --spawn-helper \"" + dir + L"\"";
    const bool started = SpawnProcess(appExe, appCmd, dir, &appPi);

    bool ok = started;
    if (!ok) {
        wprintf(L"[FAIL] 启动测试应用失败：%lu\n", GetLastError());
    } else {
        ok = WaitForFile(appBeat, 1, 6000) &&
             WaitForFile(helperBeat, 1, 6000);
        Check(ok, L"测试应用与辅助进程心跳已启动");
    }

    appsuspend::Suspender suspender(false, 0);
    if (!suspender.Initialize()) {
        wprintf(L"[FAIL] NtSuspendProcess/NtResumeProcess 不可用\n");
        ok = false;
    }

    if (ok) {
        const std::wstring key = appsuspend::NormalizePath(appExe);
        std::vector<appsuspend::SuspendRequest> requests;
        requests.push_back({key, L"suspend-test"});
        // 先确认挂起前心跳仍在增长，再挂起；否则后面无法区分“本来就停了”
        // 和“被我们挂起了”。窗口要覆盖至少两个心跳周期（80ms）。
        Sleep(250);
        const ULONGLONG appBefore = FileSize(appBeat);
        const ULONGLONG helperBefore = FileSize(helperBeat);
        // 用超时等待而不是固定窗口：受限环境里 FlushFileBuffers 可能很慢，
        // 固定 250ms 偶尔覆盖不到一次心跳。
        const bool grewBefore = WaitForGrowth(appBeat, appBefore, 2000) &&
                                WaitForGrowth(helperBeat, helperBefore, 2000);
        if (!grewBefore) {
            wprintf(L"  heartbeat sizes: app %llu→%llu, helper %llu→%llu\n",
                    static_cast<unsigned long long>(appBefore),
                    static_cast<unsigned long long>(FileSize(appBeat)),
                    static_cast<unsigned long long>(helperBefore),
                    static_cast<unsigned long long>(FileSize(helperBeat)));
        }
        auto results = suspender.SuspendMany(requests);
        ok = results.size() == 1 &&
             results[0].code == appsuspend::SuspendCode::Suspended &&
             results[0].matched >= 2 && results[0].suspended >= 2;
        Check(ok, L"主进程与不同 exe 的辅助进程一起挂起");
        if (!ok && !results.empty()) {
            wprintf(L"  result: matched=%zu suspended=%zu detail=%ls\n",
                    results[0].matched, results[0].suspended,
                    results[0].detail.c_str());
        }

        if (ok) {
            Sleep(300);
            const ULONGLONG appFrozen = FileSize(appBeat);
            const ULONGLONG helperFrozen = FileSize(helperBeat);
            Sleep(700);
            Check(FileSize(appBeat) == appFrozen,
                  L"挂起后主进程心跳停止");
            Check(FileSize(helperBeat) == helperFrozen,
                  L"挂起后辅助进程心跳停止");
            Check(grewBefore, L"挂起前心跳确实在增长");

            std::wstring detail;
            const bool resumed = suspender.Resume(key, &detail);
            Check(resumed, L"恢复调用成功");
            Check(!suspender.IsSuspended(key), L"恢复后管理表已清空");
            Check(WaitForGrowth(appBeat, appFrozen, 2500),
                  L"恢复后主进程心跳继续");
            Check(WaitForGrowth(helperBeat, helperFrozen, 2500),
                  L"恢复后辅助进程心跳继续");

            // 持久化 + 异常退出恢复：用独立的注册表值名，不碰真实配置。
            // owner 故意保持在作用域内（模拟“崩溃前持有挂起表”），由另一个
            // Suspender 实例读注册表把进程恢复出来。
            {
                appsuspend::Suspender owner(true, 0,
                                           L"SuspendedStateTest");
                const bool ownerInit = owner.Initialize();
                Check(ownerInit, L"持久化挂起控制器初始化");
                if (ownerInit) {
                    auto again = owner.SuspendMany(requests);
                    const bool suspendedAgain =
                        again.size() == 1 &&
                        again[0].code ==
                            appsuspend::SuspendCode::Suspended;
                    Check(suspendedAgain, L"再次挂起用于崩溃恢复测试");
                    if (suspendedAgain) {
                        // 受限运行环境（受限令牌 / 专门的 ACL）下 HKCU 写入可能
                        // 被拒绝；此时跳过持久化用例并明确标注，避免误报失败。
                        HKEY probeWrite = nullptr;
                        const LONG probeCreate = RegCreateKeyExW(
                            HKEY_CURRENT_USER, L"Software\\DesktopSuite\\Dock",
                            0, nullptr, 0, KEY_WRITE, nullptr, &probeWrite,
                            nullptr);
                        if (probeWrite) RegCloseKey(probeWrite);
                        Sleep(250);
                        const ULONGLONG appFrozen2 = FileSize(appBeat);
                        const ULONGLONG helperFrozen2 = FileSize(helperBeat);
                        Sleep(500);
                        Check(FileSize(appBeat) == appFrozen2 &&
                                  FileSize(helperBeat) == helperFrozen2,
                              L"崩溃恢复测试前进程确实处于冻结");
                        if (probeCreate != ERROR_SUCCESS) {
                            wprintf(L"[SKIP] 持久化恢复用例：HKCU 挂起表键不可写"
                                    L"（RegCreateKeyEx=%ld）\n",
                                    probeCreate);
                        } else {
                            appsuspend::Suspender recoverer(
                                true, 0, L"SuspendedStateTest");
                            if (recoverer.Initialize()) {
                                std::wstring recoverDetail;
                                const size_t recovered =
                                    recoverer.RecoverPersisted(&recoverDetail);
                                Check(recovered >= 2,
                                      L"启动时从持久化表恢复挂起进程");
                                Check(WaitForGrowth(appBeat, appFrozen2, 2500),
                                      L"恢复后主进程心跳继续（持久化路径）");
                                Check(WaitForGrowth(helperBeat, helperFrozen2,
                                                    2500),
                                      L"恢复后辅助进程心跳继续（持久化路径）");
                            } else {
                                Check(false, L"恢复控制器初始化");
                            }
                        }
                        // owner 析构时会再次 Resume（幂等）并清空测试键。
                    }
                }
            }
            RegDeleteKeyValueW(HKEY_CURRENT_USER,
                               L"Software\\DesktopSuite\\Dock",
                               L"SuspendedStateTest");
        }
    }

    if (appPi.hProcess) {
        TerminateProcess(appPi.hProcess, 0);
        WaitForSingleObject(appPi.hProcess, 1000);
        CloseHandle(appPi.hProcess);
        CloseHandle(appPi.hThread);
    }
    TerminateImage(helperExe);
    Sleep(150);
    DeleteFileW(appBeat.c_str());
    DeleteFileW(helperBeat.c_str());
    DeleteFileW(appExe.c_str());
    DeleteFileW(helperExe.c_str());
    RemoveDirectoryW(dir.c_str());
    return ok && g_failures == 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc >= 3 && _wcsicmp(argv[1], L"--heartbeat") == 0) {
        HeartbeatLoop(argv[2]);
        return 0;
    }
    if (argc >= 3 && _wcsicmp(argv[1], L"--spawn-helper") == 0) {
        const std::wstring dir = argv[2];
        const std::wstring helper = Join(dir, L"helper.exe");
        const std::wstring helperCmd =
            L"\"" + helper + L"\" --heartbeat \"" + Join(dir, L"helper.heartbeat") +
            L"\"";
        SpawnProcess(helper, helperCmd, dir, nullptr);
        HeartbeatLoop(Join(dir, L"app.heartbeat"));
        return 0;
    }
    // Job 沙箱测试：从 dir\outside\helper.exe 拉起不同目录的辅助进程。
    if (argc >= 3 && _wcsicmp(argv[1], L"--spawn-tree") == 0) {
        const std::wstring dir = argv[2];
        const std::wstring outside = Join(dir, L"outside");
        const std::wstring helper = Join(outside, L"helper.exe");
        const std::wstring helperCmd =
            L"\"" + helper + L"\" --heartbeat \"" +
            Join(outside, L"helper.heartbeat") + L"\"";
        SpawnProcess(helper, helperCmd, outside, nullptr);
        HeartbeatLoop(Join(dir, L"app.heartbeat"));
        return 0;
    }
    // 父进程退出后 helper 仍留在 Job 里：验证“没有主进程快照条目”
    // 时仍能按 Job 成员挂起。
    if (argc >= 3 && _wcsicmp(argv[1], L"--spawn-tree-exit") == 0) {
        const std::wstring dir = argv[2];
        const std::wstring outside = Join(dir, L"outside");
        const std::wstring helper = Join(outside, L"helper.exe");
        const std::wstring helperCmd =
            L"\"" + helper + L"\" --heartbeat \"" +
            Join(outside, L"helper.heartbeat") + L"\"";
        SpawnProcess(helper, helperCmd, outside, nullptr);
        Sleep(1200);
        return 0;
    }
    if (argc >= 2 && _wcsicmp(argv[1], L"--dump") == 0) {
        SetConsoleOutputCP(CP_UTF8);
        _setmode(_fileno(stdout), _O_U8TEXT);
        DWORD selfSession = 0;
        ProcessIdToSessionId(GetCurrentProcessId(), &selfSession);
        const auto snapshot = appsuspend::SnapshotProcesses();
        wprintf(L"self_pid=%lu self_session=%lu snapshot=%zu\n",
                GetCurrentProcessId(), selfSession, snapshot.size());
        for (const auto& e : snapshot) {
            wprintf(L"pid=%lu ppid=%lu created=%llu %ls\n", e.pid,
                    e.parentPid,
                    static_cast<unsigned long long>(e.creationTime),
                    e.imagePath.c_str());
        }
        return 0;
    }
    // 诊断：只打印某 exe 会被归入哪些进程，不执行任何挂起。
    if (argc >= 3 && _wcsicmp(argv[1], L"--probe") == 0) {
        SetConsoleOutputCP(CP_UTF8);
        _setmode(_fileno(stdout), _O_U8TEXT);
        const auto snapshot = appsuspend::SnapshotProcesses();
        const auto sel =
            appsuspend::SelectAppProcesses(argv[2], snapshot);
        wprintf(L"snapshot=%zu key=%ls\nroot=%ls broad=%d exact=%zu "
                L"helper=%zu total=%zu\n",
                snapshot.size(), appsuspend::NormalizePath(argv[2]).c_str(),
                sel.installRoot.c_str(), sel.broadRoot ? 1 : 0,
                sel.exactMatches, sel.helperMatches, sel.processes.size());
        for (const auto& ref : sel.processes) {
            std::wstring path;
            DWORD parent = 0;
            for (const auto& e : snapshot) {
                if (e.pid == ref.pid) {
                    path = e.imagePath;
                    parent = e.parentPid;
                    break;
                }
            }
            wprintf(L"  pid=%lu ppid=%lu %ls\n", ref.pid, parent,
                    path.c_str());
        }
        return 0;
    }

    SetConsoleOutputCP(CP_UTF8);
    _setmode(_fileno(stdout), _O_U8TEXT);
    setvbuf(stdout, nullptr, _IONBF, 0);
    wprintf(L"== app_suspend selection tests ==\n");
    TestSelection();
    wprintf(L"== app_suspend pause policy tests ==\n");
    TestPausePolicy();
    wprintf(L"== app_suspend job sandbox integration ==\n");
    TestJobSandbox();
    wprintf(L"== app_suspend job root-exit integration ==\n");
    TestJobSurvivesRootExit();
    wprintf(L"== app_suspend suspend/resume integration ==\n");
    TestSuspendResume();

    if (g_failures == 0) {
        wprintf(L"\nALL PASS\n");
        return 0;
    }
    wprintf(L"\n%d FAILURE(S)\n", g_failures);
    return 1;
}
