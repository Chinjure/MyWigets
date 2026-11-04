// bench_driver.cpp - 对比 pulse / sandbox 两版“后台应用挂起策略”的功耗基准
//
// 每个 trial 的流程（与 Dock 的实际调用节奏对齐）：
//   1. 空闲基线 8s；
//   2. 启动无窗口的多进程 victim（主进程 + 同目录 helper + 跨目录 helper），
//      以 CREATE_SUSPENDED 启动，先放进基准程序的记账 Job；sandbox 版还会
//      调用 AdoptProcess（模拟 Dock 启动应用时的路径）；
//   3. 活跃 6s（helper 按占空比烧 CPU）；
//   4. 调用一次 SuspendMany，随后 30s 内每 2s 再调用一次（模拟 Dock 的 2s
//      保底复核），同时 1Hz 采样 Intel RAPL 包功耗；
//   5. 恢复、结束 Job、清理。
//
// 记账 Job 用来精确统计“victim 全部进程（含已退出）在挂起窗口内消耗的
// CPU 时间”，不受 PID 复用/进程退出的影响。sandbox 版把 victim 再放进
// 自己的 Job 时形成嵌套 Job（Win8+ 支持），不影响记账。
#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <shellapi.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "app_suspend.h"

namespace {

constexpr int kIdleSec = 8;
constexpr int kActiveSec = 6;
constexpr int kSuspendSec = 30;

struct Options {
    std::wstring version;
    std::wstring scenario;
    std::wstring appDir;
    std::wstring extDir;
    std::wstring outDir;
    int trial = 0;
    int same = 0;
    int cross = 0;
    int exitAfterMs = 0;
    bool adopt = true;
};

std::wstring SelfDir() {
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, &buf[0],
                                           static_cast<DWORD>(buf.size()));
        if (n == 0) return L".";
        if (n < buf.size() - 1) {
            buf.resize(n);
            const size_t p = buf.find_last_of(L"\\/");
            return p == std::wstring::npos ? L"." : buf.substr(0, p);
        }
        buf.resize(buf.size() * 2);
    }
}

std::wstring Join(const std::wstring& a, const std::wstring& b) {
    if (a.empty()) return b;
    if (a.back() == L'\\' || a.back() == L'/') return a + b;
    return a + L"\\" + b;
}

std::string Utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(),
                                      static_cast<int>(w.size()), nullptr, 0,
                                      nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                        &out[0], n, nullptr, nullptr);
    return out;
}

std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(),
                                      static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                        &out[0], n);
    return out;
}

ULONGLONG NowMs() {
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u{};
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return (u.QuadPart - 116444736000000000ull) / 10000ull;
}

double ProcCpuMs() {
    FILETIME c{}, e{}, k{}, u{};
    if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) return 0.0;
    ULARGE_INTEGER kk{}, uu{};
    kk.LowPart = k.dwLowDateTime;
    kk.HighPart = k.dwHighDateTime;
    uu.LowPart = u.dwLowDateTime;
    uu.HighPart = u.dwHighDateTime;
    return static_cast<double>(kk.QuadPart + uu.QuadPart) / 10000.0;
}

// GetProcessTimes 的粒度约一个时钟 tick（15.6ms），做策略开销对比太粗；
// QueryProcessCycleTime 给到 TSC 级别，用来比较两版每次 SuspendMany 的代价。
ULONGLONG ProcCycles() {
    ULONGLONG cycles = 0;
    if (!QueryProcessCycleTime(GetCurrentProcess(), &cycles)) return 0;
    return cycles;
}

double JobCpuMs(HANDLE job) {
    JOBOBJECT_BASIC_AND_IO_ACCOUNTING_INFORMATION info{};
    if (!QueryInformationJobObject(job, JobObjectBasicAndIoAccountingInformation,
                                   &info, sizeof(info), nullptr)) {
        return 0.0;
    }
    return static_cast<double>(info.BasicInfo.TotalUserTime.QuadPart +
                               info.BasicInfo.TotalKernelTime.QuadPart) /
           10000.0;
}

// ---- RAPL：\Energy Meter(*)\Power（Intel 包功耗，mW） ----
class RaplSampler {
public:
    ~RaplSampler() {
        if (query_) PdhCloseQuery(query_);
    }

    bool Open() {
        if (PdhOpenQueryW(nullptr, 0, &query_) != ERROR_SUCCESS) return false;
        PDH_STATUS st = PdhAddEnglishCounterW(
            query_, L"\\Energy Meter(*)\\Power", 0, &counter_);
        if (st != ERROR_SUCCESS) {
            st = PdhAddCounterW(query_, L"\\Energy Meter(*)\\Power", 0,
                                &counter_);
        }
        if (st != ERROR_SUCCESS) return false;
        PdhCollectQueryData(query_);
        return true;
    }

    bool Sample(double* pkg, double* pp0) const {
        *pkg = NAN;
        *pp0 = NAN;
        if (!query_ || !counter_) return false;
        if (PdhCollectQueryData(query_) != ERROR_SUCCESS) return false;

        DWORD size = 0;
        DWORD count = 0;
        PDH_STATUS st = PdhGetFormattedCounterArrayW(
            counter_, PDH_FMT_DOUBLE, &size, &count, nullptr);
        if (st != PDH_MORE_DATA || size == 0) return false;
        std::vector<BYTE> buf(size);
        st = PdhGetFormattedCounterArrayW(
            counter_, PDH_FMT_DOUBLE, &size, &count,
            reinterpret_cast<PPDH_FMT_COUNTERVALUE_ITEM_W>(buf.data()));
        if (st != ERROR_SUCCESS) return false;

        auto* items =
            reinterpret_cast<PPDH_FMT_COUNTERVALUE_ITEM_W>(buf.data());
        for (DWORD i = 0; i < count; ++i) {
            const wchar_t* name = items[i].szName ? items[i].szName : L"";
            const PDH_FMT_COUNTERVALUE& v = items[i].FmtValue;
            if (v.CStatus != PDH_CSTATUS_VALID_DATA &&
                v.CStatus != PDH_CSTATUS_NEW_DATA) {
                continue;
            }
            const bool pkgName =
                _wcsicmp(name, L"rapl_package0_pkg") == 0;
            const bool pp0Name =
                _wcsicmp(name, L"rapl_package0_pp0") == 0;
            if (pkgName || (!pkgName && wcsstr(name, L"pkg") &&
                            !wcsstr(name, L"dram"))) {
                if (std::isnan(*pkg)) *pkg = v.doubleValue;
            } else if (pp0Name || wcsstr(name, L"pp0")) {
                if (std::isnan(*pp0)) *pp0 = v.doubleValue;
            }
        }
        return !std::isnan(*pkg) || !std::isnan(*pp0);
    }

private:
    PDH_HQUERY query_ = nullptr;
    PDH_HCOUNTER counter_ = nullptr;
};

struct PhaseStats {
    std::vector<double> pkg;
    std::vector<double> pp0;
    void Add(double p, double q) {
        if (!std::isnan(p)) pkg.push_back(p);
        if (!std::isnan(q)) pp0.push_back(q);
    }
    static double Mean(const std::vector<double>& v) {
        if (v.empty()) return NAN;
        double s = 0;
        for (double x : v) s += x;
        return s / static_cast<double>(v.size());
    }
};

std::wstring CodeName(appsuspend::SuspendCode c) {
    switch (c) {
        case appsuspend::SuspendCode::Suspended: return L"Suspended";
        case appsuspend::SuspendCode::AlreadySuspended: return L"AlreadySuspended";
        case appsuspend::SuspendCode::NoProcesses: return L"NoProcesses";
        case appsuspend::SuspendCode::TooYoung: return L"TooYoung";
        case appsuspend::SuspendCode::AccessDenied: return L"AccessDenied";
#ifdef BENCH_JOB
        case appsuspend::SuspendCode::VisibleWindow: return L"VisibleWindow";
#endif
        case appsuspend::SuspendCode::Failed: return L"Failed";
    }
    return L"?";
}

struct TrialResult {
    int trial = 0;
    std::wstring version;
    std::wstring scenario;
    bool adoptOk = false;
    appsuspend::SuspendCode firstCode = appsuspend::SuspendCode::Failed;
    size_t matched = 0;
    size_t suspended = 0;
    size_t jobMembers = 0;
    bool jobSandbox = false;
    int reviewCalls = 0;
    size_t reviewAdded = 0;
    double driverCpuUs = 0.0;
    double firstCpuUs = 0.0;
    double reviewCpuUs = 0.0;
    unsigned long long driverCycles = 0;
    unsigned long long firstCycles = 0;
    unsigned long long reviewCycles = 0;
    double victimCpuWindowMs = 0.0;
    double victimCpuTailMs = 0.0;
    PhaseStats idle, active, susp, post;
    std::wstring detail;
};

struct SampleRow {
    std::string phase;
    ULONGLONG epochMs = 0;
    double pkg = NAN;
    double pp0 = NAN;
};

void WriteSamples(const std::wstring& path,
                  const std::vector<SampleRow>& rows) {
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return;
    std::fprintf(f, "phase,epoch_ms,pkg_mw,pp0_mw\n");
    for (const auto& r : rows) {
        std::fprintf(f, "%s,%llu,%.3f,%.3f\n", r.phase.c_str(),
                     static_cast<unsigned long long>(r.epochMs),
                     std::isnan(r.pkg) ? -1.0 : r.pkg,
                     std::isnan(r.pp0) ? -1.0 : r.pp0);
    }
    std::fclose(f);
}

// 采样 1Hz，rows 记录 phase 与采样时刻。
void SampleAndRecord(RaplSampler& sampler, const char* phase,
                     PhaseStats* stats,
                     std::vector<SampleRow>* rows) {
    double pkg = NAN, pp0 = NAN;
    sampler.Sample(&pkg, &pp0);
    if (stats) stats->Add(pkg, pp0);
    if (rows) {
        SampleRow row;
        row.phase = phase;
        row.epochMs = NowMs();
        row.pkg = pkg;
        row.pp0 = pp0;
        rows->push_back(std::move(row));
    }
}

HANDLE LaunchVictim(const Options& o, PROCESS_INFORMATION* pi) {
    const std::wstring exe = Join(o.appDir, L"victim.exe");
    std::wstring cmd = L"\"" + exe + L"\" --same " + std::to_wstring(o.same) +
                       L" --cross " + std::to_wstring(o.cross) +
                       L" --burn-ms 30 --sleep-ms 20";
    if (!o.extDir.empty()) cmd += L" --ext-dir \"" + o.extDir + L"\"";
    if (o.exitAfterMs > 0) {
        cmd += L" --exit-after-ms " + std::to_wstring(o.exitAfterMs);
    }
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    if (!CreateProcessW(exe.c_str(), &cmd[0], nullptr, nullptr, FALSE,
                        CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr,
                        o.appDir.c_str(), &si, pi)) {
        return nullptr;
    }
    return pi->hProcess;
}

void KillLeftovers(const Options& o) {
    for (;;) {
        const auto snap = appsuspend::SnapshotProcesses();
        int killed = 0;
        for (const auto& e : snap) {
            const bool mine =
                e.imagePath == appsuspend::NormalizePath(Join(o.appDir, L"victim.exe")) ||
                e.imagePath == appsuspend::NormalizePath(Join(o.appDir, L"helper.exe")) ||
                e.imagePath == appsuspend::NormalizePath(Join(o.extDir, L"helper.exe"));
            if (!mine) continue;
            HANDLE p = OpenProcess(PROCESS_TERMINATE, FALSE, e.pid);
            if (!p) continue;
            TerminateProcess(p, 7);
            CloseHandle(p);
            ++killed;
        }
        if (killed == 0) break;
        Sleep(100);
    }
}

std::wstring Sanitize(const std::wstring& s) {
    std::wstring out = s;
    for (wchar_t& c : out) {
        if (c == L',' || c == L'\n' || c == L'\r') c = L';';
    }
    return out;
}

}  // namespace

int wmain() {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return 2;
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        auto value = [&]() -> std::wstring {
            return (i + 1 < argc) ? argv[++i] : L"";
        };
        if (a == L"--version") o.version = value();
        else if (a == L"--scenario") o.scenario = value();
        else if (a == L"--app-dir") o.appDir = value();
        else if (a == L"--ext-dir") o.extDir = value();
        else if (a == L"--out-dir") o.outDir = value();
        else if (a == L"--trial") o.trial = _wtoi(value().c_str());
        else if (a == L"--same") o.same = _wtoi(value().c_str());
        else if (a == L"--cross") o.cross = _wtoi(value().c_str());
        else if (a == L"--exit-after-ms") o.exitAfterMs = _wtoi(value().c_str());
        else if (a == L"--adopt") o.adopt = _wtoi(value().c_str()) != 0;
    }
    LocalFree(argv);

    if (o.appDir.empty() || o.outDir.empty()) return 2;

    TrialResult r;
    r.trial = o.trial;
    r.version = o.version;
    r.scenario = o.scenario;

    const std::wstring appKey = appsuspend::NormalizePath(Join(o.appDir, L"victim.exe"));
    appsuspend::Suspender suspender(/*persist=*/false, /*minAge=*/0,
                                    L"BenchSuspendedState");
    if (!suspender.Initialize()) {
        std::fwprintf(stderr, L"suspender init failed\n");
        return 3;
    }

    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (!job) return 4;

    RaplSampler sampler;
    const bool rapl = sampler.Open();

    std::vector<SampleRow> rows;

    // 1) 空闲基线
    for (int s = 0; s < kIdleSec; ++s) {
        SampleAndRecord(sampler, "idle", &r.idle, &rows);
        Sleep(1000);
    }

    // 2) 启动 victim
    PROCESS_INFORMATION pi{};
    HANDLE proc = LaunchVictim(o, &pi);
    if (!proc) {
        std::fwprintf(stderr, L"launch failed %lu\n", GetLastError());
        CloseHandle(job);
        return 5;
    }
    if (!AssignProcessToJobObject(job, proc)) {
        std::fwprintf(stderr, L"assign accounting job failed %lu\n",
                      GetLastError());
    }
#ifdef BENCH_JOB
    if (o.adopt) {
        std::wstring detail;
        r.adoptOk = suspender.AdoptProcess(appKey, proc, &detail);
    }
#endif
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    Sleep(4000);  // 等 helper 全部起来

    // 3) 活跃基线
    for (int s = 0; s < kActiveSec; ++s) {
        SampleAndRecord(sampler, "active", &r.active, &rows);
        Sleep(1000);
    }

    // 4) 挂起 + 30s 复核
    const double cpu0 = ProcCpuMs();
    const ULONGLONG cyc0 = ProcCycles();
    const double v0 = JobCpuMs(job);
    std::vector<appsuspend::SuspendRequest> reqs;
    reqs.push_back({appKey, L"BenchVictim"});
    const std::vector<std::wstring> allKeys{appKey};

    auto first = suspender.SuspendMany(reqs, allKeys);
    const double cpu1 = ProcCpuMs();
    const ULONGLONG cyc1 = ProcCycles();
    r.driverCpuUs += (cpu1 - cpu0) * 1000.0;
    r.driverCycles += cyc1 - cyc0;
    r.firstCpuUs = (cpu1 - cpu0) * 1000.0;
    r.firstCycles = cyc1 - cyc0;
    if (!first.empty()) {
        r.firstCode = first[0].code;
        r.matched = first[0].matched;
        r.suspended = first[0].suspended;
        r.detail = first[0].detail;
#ifdef BENCH_JOB
        r.jobMembers = first[0].jobMembers;
        r.jobSandbox = first[0].jobSandbox;
#endif
    }

    double vTail = 0.0;
    for (int s = 0; s < kSuspendSec; ++s) {
        SampleAndRecord(sampler, "suspended", &r.susp, &rows);
        if (s == kSuspendSec - 2) vTail = JobCpuMs(job);
        if (s > 0 && (s % 2) == 0) {
            const double c0 = ProcCpuMs();
            const ULONGLONG y0 = ProcCycles();
            auto res = suspender.SuspendMany(reqs, allKeys);
            const double c1 = ProcCpuMs();
            const ULONGLONG y1 = ProcCycles();
            r.driverCpuUs += (c1 - c0) * 1000.0;
            r.reviewCpuUs += (c1 - c0) * 1000.0;
            r.driverCycles += y1 - y0;
            r.reviewCycles += y1 - y0;
            ++r.reviewCalls;
            if (!res.empty()) r.reviewAdded += res[0].suspended;
        }
        Sleep(1000);
    }
    const double v1 = JobCpuMs(job);
    r.victimCpuWindowMs = v1 - v0;
    r.victimCpuTailMs = v1 - vTail;

    // 5) 恢复并清理
    std::wstring resumeDetail;
    suspender.Resume(appKey, &resumeDetail);
    Sleep(300);
    TerminateJobObject(job, 0);
    if (proc) {
        WaitForSingleObject(proc, 3000);
    }
    suspender.ResumeAll();
    KillLeftovers(o);
    if (pi.hProcess) CloseHandle(pi.hProcess);
    CloseHandle(job);

    // 收尾后的空闲基线：与开头 idle 一起界定本次 trial 的背景漂移。
    for (int s = 0; s < 5; ++s) {
        SampleAndRecord(sampler, "post", &r.post, &rows);
        Sleep(1000);
    }

    // 写样本
    if (!o.outDir.empty()) {
        const std::wstring samples = Join(
            o.outDir, L"samples_" + std::to_wstring(o.trial) + L"_" +
                          o.version + L"_" + o.scenario + L".csv");
        WriteSamples(samples, rows);
    }

    // 写汇总
    const std::wstring summaryPath = Join(o.outDir, L"summary.csv");
    const bool needHeader = GetFileAttributesW(summaryPath.c_str()) ==
                            INVALID_FILE_ATTRIBUTES;
    FILE* f = _wfopen(summaryPath.c_str(), L"ab");
    if (!f) return 6;
    if (needHeader) {
        std::fprintf(f,
                     "trial,version,scenario,adopt_ok,rapl_ok,first_code,"
                     "first_matched,first_suspended,first_job_members,"
                     "first_job_sandbox,review_calls,review_added,"
                     "driver_cpu_us,first_cpu_us,review_cpu_us,"
                     "driver_mcycles,first_mcycles,review_mcycles,"
                     "victim_cpu_ms_window,victim_cpu_ms_tail,"
                     "idle_pkg_mw,active_pkg_mw,susp_pkg_mw,post_pkg_mw,"
                     "idle_pp0_mw,active_pp0_mw,susp_pp0_mw,post_pp0_mw,"
                     "first_detail\n");
    }
    std::fprintf(
        f, "%d,%s,%s,%d,%d,%s,%zu,%zu,%zu,%d,%d,%zu,"
           "%.1f,%.1f,%.1f,%.3f,%.3f,%.3f,"
           "%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,"
           "%.1f,%.1f,%.1f,%.1f,%s\n",
        r.trial, Utf8(r.version).c_str(), Utf8(r.scenario).c_str(),
        r.adoptOk ? 1 : 0, rapl ? 1 : 0,
        Utf8(CodeName(r.firstCode)).c_str(), r.matched, r.suspended,
        r.jobMembers, r.jobSandbox ? 1 : 0, r.reviewCalls, r.reviewAdded,
        r.driverCpuUs, r.firstCpuUs, r.reviewCpuUs,
        static_cast<double>(r.driverCycles) / 1e6,
        static_cast<double>(r.firstCycles) / 1e6,
        static_cast<double>(r.reviewCycles) / 1e6,
        r.victimCpuWindowMs, r.victimCpuTailMs,
        PhaseStats::Mean(r.idle.pkg), PhaseStats::Mean(r.active.pkg),
        PhaseStats::Mean(r.susp.pkg), PhaseStats::Mean(r.post.pkg),
        PhaseStats::Mean(r.idle.pp0), PhaseStats::Mean(r.active.pp0),
        PhaseStats::Mean(r.susp.pp0), PhaseStats::Mean(r.post.pp0),
        Utf8(Sanitize(r.detail)).c_str());
    std::fclose(f);
    return 0;
}
