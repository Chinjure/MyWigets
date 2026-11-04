// victim.cpp - 基准测试用的无窗口“多进程应用”
//
// 主进程（victim.exe）拉起若干 helper.exe：
//   - same  : 与主进程同目录（pulse 版按安装目录树能选中）
//   - cross : 另一个目录（例如 %TEMP% 下的安装副本；pulse 版选不中，
//             sandbox 版靠 Job 继承/跨目录后代能选中）
// helper 以固定占空比消耗 CPU，模拟后台应用里仍在干活的渲染/同步 helper。
// 没有任何顶层窗口，因此不会被挂起器的“可见窗口”保护挡住。
#include <windows.h>
#include <shellapi.h>

#include <string>
#include <vector>

namespace {

struct Config {
    bool helper = false;
    std::wstring extDir;
    int same = 0;
    int cross = 0;
    int burnMs = 30;
    int sleepMs = 20;
    int exitAfterMs = 0;
};

std::wstring SelfPath() {
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, &buf[0],
                                           static_cast<DWORD>(buf.size()));
        if (n == 0) return L"";
        if (n < buf.size() - 1) {
            buf.resize(n);
            return buf;
        }
        buf.resize(buf.size() * 2);
    }
}

std::wstring DirOf(const std::wstring& path) {
    const size_t p = path.find_last_of(L"\\/");
    return p == std::wstring::npos ? L"." : path.substr(0, p);
}

Config ParseArgs() {
    Config cfg;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return cfg;
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        auto next = [&](int* out) {
            if (i + 1 < argc) *out = _wtoi(argv[++i]);
        };
        if (a == L"--role" && i + 1 < argc) {
            cfg.helper = std::wstring(argv[++i]) == L"helper";
        } else if (a == L"--ext-dir" && i + 1 < argc) {
            cfg.extDir = argv[++i];
        } else if (a == L"--same") {
            next(&cfg.same);
        } else if (a == L"--cross") {
            next(&cfg.cross);
        } else if (a == L"--burn-ms") {
            next(&cfg.burnMs);
        } else if (a == L"--sleep-ms") {
            next(&cfg.sleepMs);
        } else if (a == L"--exit-after-ms") {
            next(&cfg.exitAfterMs);
        }
    }
    LocalFree(argv);
    return cfg;
}

// 占空比燃烧：burnMs 忙等 + sleepMs 休眠。被 NtSuspendProcess 冻结时整个
// 循环停止；恢复后继续，不依赖任何外部信号。
void BurnForever(int burnMs, int sleepMs) {
    if (burnMs < 1) burnMs = 1;
    if (sleepMs < 0) sleepMs = 0;
    LARGE_INTEGER freq{};
    QueryPerformanceFrequency(&freq);
    volatile ULONGLONG sink = 0;
    for (;;) {
        LARGE_INTEGER start{}, now{};
        QueryPerformanceCounter(&start);
        do {
            for (int i = 0; i < 4000; ++i) sink += static_cast<ULONGLONG>(i);
            QueryPerformanceCounter(&now);
        } while ((now.QuadPart - start.QuadPart) * 1000 / freq.QuadPart <
                 burnMs);
        if (sleepMs > 0) Sleep(static_cast<DWORD>(sleepMs));
    }
}

bool SpawnHelper(const std::wstring& exe, const std::wstring& cwd,
                 int burnMs, int sleepMs) {
    std::wstring cmd = L"\"" + exe + L"\" --role helper --burn-ms " +
                       std::to_wstring(burnMs) + L" --sleep-ms " +
                       std::to_wstring(sleepMs);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(
        exe.c_str(), &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
        nullptr, cwd.c_str(), &si, &pi);
    if (!ok) return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

int RunMain(const Config& cfg) {
    const std::wstring self = SelfPath();
    const std::wstring appDir = DirOf(self);
    const std::wstring sameExe = appDir + L"\\helper.exe";
    const std::wstring crossExe =
        cfg.extDir.empty() ? L"" : cfg.extDir + L"\\helper.exe";

    for (int i = 0; i < cfg.same; ++i) {
        SpawnHelper(sameExe, appDir, cfg.burnMs, cfg.sleepMs);
    }
    for (int i = 0; i < cfg.cross; ++i) {
        if (!crossExe.empty()) {
            SpawnHelper(crossExe, cfg.extDir, cfg.burnMs, cfg.sleepMs);
        }
    }

    if (cfg.exitAfterMs > 0) {
        Sleep(static_cast<DWORD>(cfg.exitAfterMs));
        return 0;
    }
    for (;;) Sleep(1000);
}

}  // namespace

int APIENTRY wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    const Config cfg = ParseArgs();
    if (cfg.helper) {
        BurnForever(cfg.burnMs, cfg.sleepMs);
        return 0;
    }
    return RunMain(cfg);
}
