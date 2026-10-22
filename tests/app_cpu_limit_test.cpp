// app_cpu_limit_test.cpp - app_cpu_limit 的纯函数单元测试 + 真实 CPU 限速集成测试
//
// 集成测试不碰系统里已有的应用：把本测试 exe 复制成临时目录里的 app.exe /
// helper.exe，由 app.exe 再派生 helper.exe（模拟“主进程 + 不同 exe 的辅助
// 进程”），然后只对临时目录里的这一组进程调用 CpuLimiter。通过
// GetProcessTimes 的 CPU 时间增量验证：限速后主进程/辅助进程都几乎不再
// 消耗 CPU，Restore 后又恢复满速。

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

#include "app_cpu_limit.h"

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

// 全速忙循环，同时每 100ms 往心跳文件写一个字节。
// 0.01% CPU hard cap 下心跳几乎停止，正好用来观察限速是否生效。
void BusyLoop(const std::wstring& path) {
    HANDLE file = CreateFileW(
        path.c_str(), FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    const char byte = 'x';
    ULONGLONG lastWrite = 0;
    volatile ULONGLONG counter = 0;
    for (;;) {
        ++counter;
        if ((counter & 0xFFFFF) != 0) continue;
        const ULONGLONG now = GetTickCount64();
        if (now - lastWrite < 100) continue;
        lastWrite = now;
        if (file != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteFile(file, &byte, 1, &written, nullptr);
            FlushFileBuffers(file);
        }
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

void TerminateImage(const std::wstring& imagePath) {
    const std::wstring want = appcpu::NormalizePath(imagePath);
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
                appcpu::NormalizePath(image) == want) {
                TerminateProcess(proc, 0);
            }
            CloseHandle(proc);
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
}

DWORD FindPidByImage(const std::wstring& imagePath) {
    const std::wstring want = appcpu::NormalizePath(imagePath);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    DWORD found = 0;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            HANDLE proc =
                OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                            pe.th32ProcessID);
            if (!proc) continue;
            wchar_t image[32768] = {};
            DWORD len = 32760;
            if (QueryFullProcessImageNameW(proc, 0, image, &len) && len > 0 &&
                appcpu::NormalizePath(image) == want) {
                found = pe.th32ProcessID;
                CloseHandle(proc);
                break;
            }
            CloseHandle(proc);
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

ULONGLONG CpuTime(HANDLE proc) {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(proc, &created, &exited, &kernel, &user)) return 0;
    return ((static_cast<ULONGLONG>(kernel.dwHighDateTime) << 32) |
            kernel.dwLowDateTime) +
           ((static_cast<ULONGLONG>(user.dwHighDateTime) << 32) |
            user.dwLowDateTime);
}

appcpu::ProcessEntry MakeEntry(DWORD pid, DWORD parent,
                               const wchar_t* path,
                               ULONGLONG creation) {
    appcpu::ProcessEntry entry;
    entry.pid = pid;
    entry.parentPid = parent;
    entry.creationTime = creation;
    entry.imagePath = appcpu::NormalizePath(path);
    return entry;
}

void TestSelection() {
    {
        std::vector<appcpu::ProcessEntry> procs;
        procs.push_back(MakeEntry(10, 1, L"C:\\Apps\\Chrome\\chrome.exe", 100));
        procs.push_back(MakeEntry(11, 10, L"C:\\Apps\\Chrome\\chrome.exe", 200));
        procs.push_back(MakeEntry(12, 10, L"C:\\Apps\\Chrome\\chrome.exe", 300));
        procs.push_back(MakeEntry(13, 1, L"C:\\Apps\\Other\\chrome.exe", 400));
        const auto sel = appcpu::SelectAppProcesses(
            L"C:\\Apps\\Chrome\\chrome.exe", procs);
        Check(sel.processes.size() == 3, L"同一 exe 的全部实例都选中");
        Check(sel.exactMatches == 3, L"精确匹配计数正确");
    }

    {
        std::vector<appcpu::ProcessEntry> procs;
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
        const auto sel = appcpu::SelectAppProcesses(
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
    }

    {
        std::vector<appcpu::ProcessEntry> procs;
        procs.push_back(
            MakeEntry(30, 1, L"C:\\Windows\\notepad.exe", 1000));
        procs.push_back(MakeEntry(31, 30, L"C:\\Windows\\same_dir.exe", 1100));
        procs.push_back(
            MakeEntry(32, 30, L"C:\\Windows\\subdir\\outside.exe", 1200));
        const auto sel = appcpu::SelectAppProcesses(
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
        std::vector<appcpu::ProcessEntry> procs;
        procs.push_back(MakeEntry(40, 1, L"C:\\Apps\\A\\app.exe", 1000));
        procs.push_back(MakeEntry(41, 40, L"C:\\Apps\\B\\app.exe", 1100));
        const auto sel = appcpu::SelectAppProcesses(
            L"C:\\Apps\\A\\app.exe", procs);
        Check(sel.processes.size() == 1,
              L"同名但不同目录的 exe 不会被合并");
    }

    {
        std::vector<appcpu::ProcessEntry> procs;
        procs.push_back(MakeEntry(50, 1, L"C:\\Apps\\A\\launcher.exe", 900));
        procs.push_back(MakeEntry(51, 50, L"C:\\Apps\\A\\app.exe", 1000));
        const auto sel = appcpu::SelectAppProcesses(
            L"C:\\Apps\\A\\app.exe", procs);
        Check(sel.processes.size() == 1 && sel.processes[0].pid == 51,
              L"启动器父进程不会被当成应用进程");
    }

    {
        std::vector<appcpu::ProcessEntry> procs;
        procs.push_back(
            MakeEntry(60, 1, L"C:\\Office\\WINWORD.EXE", 1000));
        procs.push_back(
            MakeEntry(61, 60, L"C:\\Office\\EXCEL.EXE", 1100));
        std::unordered_set<std::wstring> foreign;
        foreign.insert(appcpu::NormalizePath(L"C:\\Office\\EXCEL.EXE"));
        const auto sel = appcpu::SelectAppProcesses(
            L"C:\\Office\\WINWORD.EXE", procs, &foreign);
        Check(sel.processes.size() == 1 && sel.processes[0].pid == 60,
              L"同目录但属于另一个 Dock 应用的后代不被吞并");
    }

    {
        bool broad = false;
        const std::wstring root = appcpu::InstallRootForApp(
            L"C:\\Apps\\Product\\bin\\app.exe", &broad);
        Check(root == L"c:\\apps\\product" && !broad,
              L"bin 目录上提到产品安装根");
        const std::wstring sysRoot = appcpu::InstallRootForApp(
            L"C:\\Windows\\System32\\notepad.exe", &broad);
        Check(sysRoot == L"c:\\windows\\system32" && broad,
              L"System32 被识别为宽泛根");
    }

    {
        Check(appcpu::IsNeverLimitImage(L"C:\\Windows\\explorer.exe"),
              L"explorer 永不可限速");
        Check(appcpu::IsNeverLimitImage(L"C:\\X\\conhost.exe"),
              L"conhost 永不可限速");
        Check(!appcpu::IsNeverLimitImage(L"C:\\Apps\\App\\app.exe"),
              L"普通应用可限速");
    }
}

void TestLimitPolicy() {
    Check(!appcpu::ShouldLimitApp(false, false, false),
          L"未打开的应用没有限速对象");
    Check(!appcpu::ShouldLimitApp(true, true, false),
          L"有可见且未最小化窗口的应用不限速");
    Check(appcpu::ShouldLimitApp(true, false, false),
          L"窗口全部最小化或纯托盘（无可见窗口）可限速");
    Check(!appcpu::ShouldLimitApp(true, false, true),
          L"用户勾选「不限制此应用」后永久豁免");
}

bool TestLimitRestore() {
    wchar_t temp[MAX_PATH] = {};
    if (GetTempPathW(MAX_PATH, temp) == 0) return false;
    const std::wstring dir =
        Join(temp, L"mywigets_cpu_limit_test_" +
                       std::to_wstring(GetCurrentProcessId()) + L"_" +
                       std::to_wstring(GetTickCount64()));
    if (!CreateDirectoryW(dir.c_str(), nullptr)) return false;

    wchar_t self[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    const std::wstring appExe = Join(dir, L"app.exe");
    const std::wstring helperExe = Join(dir, L"helper.exe");
    const std::wstring appBeat = Join(dir, L"app.heartbeat");
    const std::wstring helperBeat = Join(dir, L"helper.heartbeat");
    bool copied = CopyFileW(self, appExe.c_str(), FALSE) != FALSE;
    copied = copied && CopyFileW(self, helperExe.c_str(), FALSE) != FALSE;
    if (!copied) {
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
        Check(ok, L"测试应用与辅助进程忙循环已启动");
    }

    appcpu::CpuLimiter limiter(0);
    if (!limiter.Initialize()) {
        wprintf(L"[FAIL] Job Object CPU rate control 不可用\n");
        ok = false;
    }

    if (ok) {
        const std::wstring key = appcpu::NormalizePath(appExe);
        std::vector<appcpu::LimitRequest> requests;
        requests.push_back({key, L"cpu-limit-test"});
        auto results = limiter.LimitMany(requests);
        ok = results.size() == 1 &&
             results[0].code == appcpu::LimitCode::Limited &&
             results[0].matched >= 2 && results[0].limited >= 2;
        Check(ok, L"主进程与不同 exe 的辅助进程一起加入 CPU 限速");
        if (!ok && !results.empty()) {
            wprintf(L"  result: matched=%zu limited=%zu detail=%ls\n",
                    results[0].matched, results[0].limited,
                    results[0].detail.c_str());
        }

        if (ok) {
            Check(limiter.IsLimited(key), L"限速状态已记录");
            const DWORD helperPid = FindPidByImage(helperExe);
            HANDLE helperProc =
                helperPid ? OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,
                                        FALSE, helperPid)
                          : nullptr;
            Sleep(2500);
            const ULONGLONG appBefore = CpuTime(appPi.hProcess);
            const ULONGLONG helperBefore =
                helperProc ? CpuTime(helperProc) : 0;
            Sleep(5000);
            const ULONGLONG appCapped = CpuTime(appPi.hProcess) - appBefore;
            const ULONGLONG helperCapped =
                helperProc ? CpuTime(helperProc) - helperBefore : 0;
            wprintf(L"  capped cpu delta: app=%llu helper=%llu\n",
                    appCapped, helperCapped);
            // 0.01% of 5s ≈ 5000（100ns 单位）；Windows 每个调度区间可能
            // 给一个最小时间片，留出 ~0.5% 的余量。
            Check(appCapped < 250000,
                  L"限速后主进程 CPU 占用接近停摆");
            Check(helperProc != nullptr && helperCapped < 250000,
                  L"限速后辅助进程 CPU 占用接近停摆");

            std::wstring detail;
            const bool restored = limiter.Restore(key, &detail);
            Check(restored, L"解除 CPU 限制调用成功");
            Check(!limiter.IsLimited(key), L"解除后限速状态已清空");

            Sleep(300);
            const ULONGLONG appBefore2 = CpuTime(appPi.hProcess);
            const ULONGLONG helperBefore2 =
                helperProc ? CpuTime(helperProc) : 0;
            Sleep(1000);
            const ULONGLONG appFull = CpuTime(appPi.hProcess) - appBefore2;
            const ULONGLONG helperFull =
                helperProc ? CpuTime(helperProc) - helperBefore2 : 0;
            wprintf(L"  restored cpu delta: app=%llu helper=%llu\n",
                    appFull, helperFull);
            Check(appFull > 500000, L"解除后主进程恢复满速");
            Check(helperProc != nullptr && helperFull > 500000,
                  L"解除后辅助进程恢复满速");

            if (helperProc) CloseHandle(helperProc);
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
    if (argc >= 2 && _wcsicmp(argv[1], L"--busy") == 0) {
        BusyLoop(argc >= 3 ? argv[2] : L"");
        return 0;
    }
    if (argc >= 3 && _wcsicmp(argv[1], L"--spawn-helper") == 0) {
        const std::wstring dir = argv[2];
        const std::wstring helper = Join(dir, L"helper.exe");
        const std::wstring helperCmd =
            L"\"" + helper + L"\" --busy \"" +
            Join(dir, L"helper.heartbeat") + L"\"";
        SpawnProcess(helper, helperCmd, dir, nullptr);
        BusyLoop(Join(dir, L"app.heartbeat"));
        return 0;
    }
    if (argc >= 2 && _wcsicmp(argv[1], L"--dump") == 0) {
        SetConsoleOutputCP(CP_UTF8);
        _setmode(_fileno(stdout), _O_U8TEXT);
        DWORD selfSession = 0;
        ProcessIdToSessionId(GetCurrentProcessId(), &selfSession);
        const auto snapshot = appcpu::SnapshotProcesses();
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
    // 诊断：只打印某 exe 会被归入哪些进程，不执行任何限速。
    if (argc >= 3 && _wcsicmp(argv[1], L"--probe") == 0) {
        SetConsoleOutputCP(CP_UTF8);
        _setmode(_fileno(stdout), _O_U8TEXT);
        const auto snapshot = appcpu::SnapshotProcesses();
        const auto sel = appcpu::SelectAppProcesses(argv[2], snapshot);
        wprintf(L"snapshot=%zu key=%ls\nroot=%ls broad=%d exact=%zu "
                L"helper=%zu total=%zu\n",
                snapshot.size(), appcpu::NormalizePath(argv[2]).c_str(),
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
    wprintf(L"== app_cpu_limit selection tests ==\n");
    TestSelection();
    wprintf(L"== app_cpu_limit policy tests ==\n");
    TestLimitPolicy();
    wprintf(L"== app_cpu_limit limit/restore integration ==\n");
    TestLimitRestore();

    if (g_failures == 0) {
        wprintf(L"\nALL PASS\n");
        return 0;
    }
    wprintf(L"\n%d FAILURE(S)\n", g_failures);
    return 1;
}
