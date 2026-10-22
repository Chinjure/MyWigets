// app_cpu_limit.cpp - 见 app_cpu_limit.h 的口径说明
//
// 这里做四件事：
//   1. 从全量进程快照中按「同一 exe + 安装目录树内的后代」选出应用进程；
//   2. 为每个应用维护一个 Job Object，用 CPU rate control 的 hard cap
//      把它的 CPU 占用限制到 0.01%；
//   3. 用 SILENT_BREAKAWAY_OK 防止应用拉起的外部程序继承限速 Job，
//      并周期性把新出现的同应用辅助进程补进 Job；
//   4. 用 PID + 创建时间双保险，恢复时只关闭 rate control、不带着 hard cap
//      关闭 Job 句柄。
//
// 不按进程树整棵挂起、不按 exe 名字合并、不碰系统基础设施，原因见头文件。

#include "app_cpu_limit.h"

#include <tlhelp32.h>

#include <algorithm>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace appcpu {
namespace {

constexpr wchar_t kRegPath[] = L"Software\\DesktopSuite\\Dock";

// 旧版「挂起未聚焦应用」持久化值的格式（只用于升级时恢复残留进程）。
constexpr wchar_t kLegacyStateValue[] = L"SuspendedState";
constexpr DWORD kLegacyMagic = 0x50555353;  // "SSUP"
constexpr DWORD kLegacyVersion = 1;
constexpr DWORD kLegacyHeaderBytes = sizeof(DWORD) * 3;

// 刚启动的进程先不限速：Chrome/Electron/UWP 启动阶段会连续创建子进程，
// 太早压到 0.01% 会把「正在打开的应用」卡在启动中。
constexpr ULONGLONG kMinProcessAgeDefaultMs = 8000;

// 硬件断点/调试器挂起等场景不属于这里；只列常见的系统基础设施。
// 基名均小写。应用自己的主 exe 若命中这里，Dock 侧也不会把它当可限速项。
const wchar_t* const kNeverLimitBasenames[] = {
    L"system", L"smss.exe", L"csrss.exe", L"wininit.exe", L"winlogon.exe",
    L"services.exe", L"lsass.exe", L"svchost.exe", L"dwm.exe",
    L"fontdrvhost.exe", L"explorer.exe", L"sihost.exe", L"ctfmon.exe",
    L"taskhostw.exe", L"runtimebroker.exe", L"searchhost.exe",
    L"searchapp.exe", L"searchprotocolhost.exe", L"searchfilterhost.exe",
    L"shellexperiencehost.exe", L"startmenuexperiencehost.exe",
    L"textinputhost.exe", L"applicationframehost.exe", L"widgets.exe",
    L"widgetservice.exe", L"conhost.exe", L"dllhost.exe", L"msiexec.exe",
    L"werfault.exe", L"werfaultsecure.exe", L"backgroundtaskhost.exe",
    L"lockapp.exe", L"logonui.exe", L"userinit.exe", L"rundll32.exe",
    L"regsvr32.exe", L"splwow64.exe", L"wudfhost.exe", L"audiodg.exe",
    // 本套件自身（与 dock_main.cpp 的 kSelfExclusions 保持同步）。
    L"mywigets.exe", L"mywigets-x64.exe", L"mywigets-x86.exe",
    L"desktopdock-x64.exe", L"desktopdock-x86.exe",
    L"desktoptopbar-x64.exe", L"desktoptopbar-x86.exe",
    L"desktopclock-x64.exe", L"desktopclock-x86.exe",
    L"desktopcalendar-x64.exe", L"desktopcalendar-x86.exe",
    L"desktoplauncher-x64.exe", L"desktoplauncher-x86.exe",
};

// ---- Job Object CPU rate control ----
// _WIN32_WINNT 可能仍按 0x0601 编译，SDK 不一定导出 Windows 8 的新枚举名，
// 这里用 ABI 值：JobObjectCpuRateControlInformation = 15。
constexpr JOBOBJECTINFOCLASS kJobCpuRateControlInformation =
    static_cast<JOBOBJECTINFOCLASS>(15);
constexpr DWORD kCpuRateControlEnable = 0x1;
constexpr DWORD kCpuRateControlHardCap = 0x4;
// CpuRate 单位是 1/10000（万分之一）。1 = 0.01%。
constexpr DWORD kCpuRateOneHundredthPercent = 1;

// 与 JOBOBJECT_CPU_RATE_CONTROL_INFORMATION 同布局；不直接使用 SDK 类型，
// 避免旧 _WIN32_WINNT 下结构体/枚举缺失。
struct JobCpuRateInfo {
    DWORD ControlFlags;
    union {
        DWORD CpuRate;
        DWORD Weight;
    };
};

std::wstring TrimTrailingSlash(std::wstring path) {
    while (path.size() > 3 && path.back() == L'\\') path.pop_back();
    return path;
}

bool IsDriveRoot(const std::wstring& path) {
    return path.size() == 3 && path[1] == L':' && path[2] == L'\\';
}

// path 是否在 dir 的子树内（不含 dir 自身）。
bool IsInsideDir(const std::wstring& path, const std::wstring& dir) {
    if (path.size() <= dir.size() + 1) return false;
    if (_wcsnicmp(path.c_str(), dir.c_str(), dir.size()) != 0) return false;
    return path[dir.size()] == L'\\';
}

ULONGLONG FileTimeToU64(const FILETIME& ft) {
    return (static_cast<ULONGLONG>(ft.dwHighDateTime) << 32) |
           static_cast<ULONGLONG>(ft.dwLowDateTime);
}

bool IsNeverLimitBase(const std::wstring& baseLower) {
    for (const wchar_t* name : kNeverLimitBasenames) {
        if (baseLower == name) return true;
    }
    return false;
}

void AddEnvPath(std::vector<std::wstring>* out, const wchar_t* name) {
    wchar_t buf[32768] = {};
    const DWORD n = GetEnvironmentVariableW(name, buf, 32760);
    if (n > 0 && n < 32760) {
        const std::wstring p = NormalizePath(buf);
        if (!p.empty()) out->push_back(TrimTrailingSlash(p));
    }
}

std::vector<std::wstring> BroadRoots() {
    std::vector<std::wstring> roots;
    wchar_t winDir[32768] = {};
    if (GetWindowsDirectoryW(winDir, 32760) > 0) {
        const std::wstring w = TrimTrailingSlash(NormalizePath(winDir));
        if (!w.empty()) {
            roots.push_back(w);
            roots.push_back(w + L"\\system32");
            roots.push_back(w + L"\\syswow64");
            roots.push_back(w + L"\\winsxs");
            roots.push_back(w + L"\\temp");
            roots.push_back(w + L"\\installer");
            roots.push_back(w + L"\\servicing");
            roots.push_back(w + L"\\assembly");
        }
    }
    AddEnvPath(&roots, L"ProgramFiles");
    AddEnvPath(&roots, L"ProgramFiles(x86)");
    AddEnvPath(&roots, L"ProgramData");
    AddEnvPath(&roots, L"SystemDrive");
    AddEnvPath(&roots, L"USERPROFILE");
    AddEnvPath(&roots, L"PUBLIC");
    AddEnvPath(&roots, L"TEMP");
    AddEnvPath(&roots, L"TMP");
    AddEnvPath(&roots, L"APPDATA");
    AddEnvPath(&roots, L"LOCALAPPDATA");

    const std::wstring profile = [] {
        wchar_t buf[32768] = {};
        const DWORD n = GetEnvironmentVariableW(L"USERPROFILE", buf, 32760);
        return (n > 0 && n < 32760) ? TrimTrailingSlash(NormalizePath(buf))
                                    : std::wstring();
    }();
    if (!profile.empty()) {
        roots.push_back(profile + L"\\desktop");
        roots.push_back(profile + L"\\downloads");
        roots.push_back(profile + L"\\documents");
        roots.push_back(profile + L"\\appdata");
        roots.push_back(profile + L"\\appdata\\local");
        roots.push_back(profile + L"\\appdata\\locallow");
        roots.push_back(profile + L"\\appdata\\roaming");
        roots.push_back(profile + L"\\appdata\\local\\programs");
        roots.push_back(profile + L"\\appdata\\local\\temp");
        roots.push_back(profile + L"\\appdata\\local\\microsoft");
        roots.push_back(profile + L"\\appdata\\roaming\\microsoft");
        const size_t slash = profile.find_last_of(L'\\');
        if (slash != std::wstring::npos && slash > 2) {
            roots.push_back(profile.substr(0, slash));  // C:\Users
        }
    }

    wchar_t pf[32768] = {};
    if (GetEnvironmentVariableW(L"ProgramFiles", pf, 32760) > 0) {
        const std::wstring p = TrimTrailingSlash(NormalizePath(pf));
        if (!p.empty()) {
            roots.push_back(p + L"\\windowsapps");
            roots.push_back(p + L"\\common files");
        }
    }
    if (GetEnvironmentVariableW(L"ProgramFiles(x86)", pf, 32760) > 0) {
        const std::wstring p = TrimTrailingSlash(NormalizePath(pf));
        if (!p.empty()) roots.push_back(p + L"\\common files");
    }

    std::sort(roots.begin(), roots.end());
    roots.erase(std::unique(roots.begin(), roots.end()), roots.end());
    return roots;
}

struct TrackedProcess {
    DWORD pid = 0;
    ULONGLONG creation = 0;
};

using NtResumeProcessFn = LONG(NTAPI*)(HANDLE);

bool NtSuccess(LONG status) { return status >= 0; }

bool QueryCreationTime(HANDLE proc, ULONGLONG* out) {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(proc, &created, &exited, &kernel, &user)) {
        return false;
    }
    *out = FileTimeToU64(created);
    return true;
}

bool TrackedAlive(const TrackedProcess& ref) {
    if (ref.pid == 0 || ref.creation == 0) return false;
    HANDLE proc =
        OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, ref.pid);
    if (!proc) return false;
    ULONGLONG created = 0;
    const bool ok =
        QueryCreationTime(proc, &created) && created == ref.creation;
    CloseHandle(proc);
    return ok;
}

// 打开并核对 PID 没有被复用。creation 为 0 时拒绝操作（无法核对）。
HANDLE OpenVerified(DWORD pid, ULONGLONG creation, DWORD access,
                    DWORD* lastError) {
    if (pid == 0 || creation == 0) {
        if (lastError) *lastError = ERROR_INVALID_PARAMETER;
        return nullptr;
    }
    HANDLE proc = OpenProcess(access, FALSE, pid);
    if (!proc) {
        if (lastError) *lastError = GetLastError();
        return nullptr;
    }
    ULONGLONG actual = 0;
    if (!QueryCreationTime(proc, &actual)) {
        if (lastError) *lastError = GetLastError();
        CloseHandle(proc);
        return nullptr;
    }
    if (actual != creation) {
        // 原进程已退出，PID 被另一个进程复用：绝不能碰新进程。
        if (lastError) *lastError = ERROR_NOT_FOUND;
        CloseHandle(proc);
        return nullptr;
    }
    if (lastError) *lastError = ERROR_SUCCESS;
    return proc;
}

bool IsProcessCritical(HANDLE proc) {
    using IsProcessCriticalFn = BOOL(WINAPI*)(HANDLE, PBOOL);
    static IsProcessCriticalFn fn = []() -> IsProcessCriticalFn {
        HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
        if (!kernel) return nullptr;
        return reinterpret_cast<IsProcessCriticalFn>(
            GetProcAddress(kernel, "IsProcessCritical"));
    }();
    if (!fn) return false;
    BOOL critical = FALSE;
    if (!fn(proc, &critical)) return false;
    return critical != FALSE;
}

// 打开/关闭 Job 的 CPU rate control。enable=false 时 ControlFlags=0，
// 解除硬上限；这是唯一安全的“还进程自由”的方式。
bool ApplyCpuRate(HANDLE job, bool enable) {
    JobCpuRateInfo info{};
    if (enable) {
        info.ControlFlags = kCpuRateControlEnable | kCpuRateControlHardCap;
        info.CpuRate = kCpuRateOneHundredthPercent;
    }
    return SetInformationJobObject(
               job, kJobCpuRateControlInformation, &info, sizeof(info)) != FALSE;
}

// 让 Job 里的进程创建子进程时不自动继承 Job：否则应用拉起的浏览器/
// 终端/安装器会被一起压到 0.01%。代价是新出现的同应用 helper 需要由
// Dock 周期性重新选中并 Assign（见 EnforceLimited）。
bool EnableSilentBreakaway(HANDLE job) {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION ext{};
    ext.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK;
    return SetInformationJobObject(
               job, JobObjectExtendedLimitInformation, &ext, sizeof(ext)) !=
           FALSE;
}

bool IsCpuRateControlSupported() {
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (!job) return false;
    // 先在空 Job 上真正打开一次 hard cap，再关闭：部分系统对“从未启用过
    // CPU rate control 的 Job 直接 ControlFlags=0”会返回 INVALID_ARGS，
    // 用它探测支持性会误判为不可用。
    const bool supported = ApplyCpuRate(job, true);
    if (supported) ApplyCpuRate(job, false);
    CloseHandle(job);
    return supported;
}

}  // namespace

std::wstring NormalizePath(const std::wstring& path) {
    std::wstring out;
    out.reserve(path.size());
    bool started = false;
    for (wchar_t ch : path) {
        if (!started && ch == L'"') continue;
        started = true;
        ch = static_cast<wchar_t>(towlower(ch));
        out.push_back(ch == L'/' ? L'\\' : ch);
    }
    if (out.compare(0, 4, L"\\\\?\\") == 0) out.erase(0, 4);
    return out;
}

std::wstring BaseNameOf(const std::wstring& path) {
    const size_t pos = path.find_last_of(L"\\/");
    return pos == std::wstring::npos ? path : path.substr(pos + 1);
}

std::wstring ParentDirOf(const std::wstring& path) {
    const size_t pos = path.find_last_of(L"\\/");
    return pos == std::wstring::npos ? std::wstring() : path.substr(0, pos);
}

bool IsBroadInstallRoot(const std::wstring& dir) {
    const std::wstring d = TrimTrailingSlash(NormalizePath(dir));
    if (d.empty() || IsDriveRoot(d)) return true;
    const std::vector<std::wstring> roots = BroadRoots();
    return std::find(roots.begin(), roots.end(), d) != roots.end();
}

std::wstring InstallRootForApp(const std::wstring& appKey, bool* broadRoot) {
    std::wstring dir = TrimTrailingSlash(ParentDirOf(NormalizePath(appKey)));
    bool broad = IsBroadInstallRoot(dir);

    // 常见布局：...\Product\bin\app.exe、...\Product\bin\x64\app.exe。
    // 上提一层覆盖 ...\Product\bin\..\helper.exe；但父目录过宽时不上提，
    // 否则 Program Files 本身会变成安装根。
    //
    // 只上提明确的“二进制子目录”，不把 application/app/current 之类
    // 泛化目录名当子目录 —— 那些很可能本身就是产品目录，上提会把
    // 隔壁应用也纳入安装根。Chrome/Edge 的辅助进程都在 Application 之下，
    // 不提也能覆盖。
    static const wchar_t* const kBinaryDirs[] = {
        L"bin", L"x64", L"x86", L"win64", L"win32", L"amd64",
    };
    for (int depth = 0; depth < 3 && !dir.empty() && !broad; ++depth) {
        const std::wstring base = BaseNameOf(dir);
        bool binaryDir = false;
        for (const wchar_t* name : kBinaryDirs) {
            if (base == name) {
                binaryDir = true;
                break;
            }
        }
        if (!binaryDir) break;
        const std::wstring parent =
            TrimTrailingSlash(ParentDirOf(dir));
        if (parent.empty() || IsBroadInstallRoot(parent)) break;
        dir = parent;
    }
    broad = IsBroadInstallRoot(dir);
    if (broadRoot) *broadRoot = broad;
    return dir;
}

bool IsNeverLimitImage(const std::wstring& imagePath) {
    return IsNeverLimitBase(BaseNameOf(NormalizePath(imagePath)));
}

bool ShouldLimitApp(bool opened, bool hasVisibleWindow, bool userExcluded) {
    // 没打开（进程已退出）没有可限速对象；只要还有一张可见且未最小化的
    // 窗口就保持运行；窗口全部最小化或纯托盘驻留时可以限速。用户显式
    // 勾选「不限制此应用」时永久豁免，即使之后收起窗口变成托盘应用。
    return opened && !hasVisibleWindow && !userExcluded;
}

std::vector<ProcessEntry> SnapshotProcesses() {
    std::vector<ProcessEntry> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;

    DWORD selfSession = 0;
    const bool sessionKnown =
        ProcessIdToSessionId(GetCurrentProcessId(), &selfSession) != FALSE;
    const DWORD selfPid = GetCurrentProcessId();

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            const DWORD pid = pe.th32ProcessID;
            if (pid == 0 || pid == selfPid) continue;
            if (sessionKnown) {
                DWORD session = 0;
                if (ProcessIdToSessionId(pid, &session) &&
                    session != selfSession) {
                    continue;
                }
            }

            HANDLE proc =
                OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            if (!proc) continue;

            wchar_t image[32768] = {};
            DWORD imageLen = 32760;
            ULONGLONG created = 0;
            const bool imageOk =
                QueryFullProcessImageNameW(proc, 0, image, &imageLen) != FALSE &&
                imageLen > 0;
            QueryCreationTime(proc, &created);
            CloseHandle(proc);
            if (!imageOk) continue;

            const std::wstring normalized = NormalizePath(image);
            if (normalized.empty()) continue;
            if (IsNeverLimitImage(normalized)) continue;

            ProcessEntry entry;
            entry.pid = pid;
            entry.parentPid = pe.th32ParentProcessID;
            entry.creationTime = created;
            entry.imagePath = normalized;
            out.push_back(std::move(entry));
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return out;
}

Selection SelectAppProcesses(
    const std::wstring& appKey, const std::vector<ProcessEntry>& snapshot,
    const std::unordered_set<std::wstring>* foreignAppKeys) {
    Selection sel;
    const std::wstring key = NormalizePath(appKey);
    if (key.empty()) {
        sel.note = L"空应用 key";
        return sel;
    }
    sel.installRoot = InstallRootForApp(key, &sel.broadRoot);

    std::unordered_map<DWORD, std::vector<size_t>> children;
    for (size_t i = 0; i < snapshot.size(); ++i) {
        children[snapshot[i].parentPid].push_back(i);
    }

    std::vector<bool> selected(snapshot.size(), false);
    std::queue<size_t> pending;

    // A. 同一 exe 的全部实例（Chrome/Edge/Electron 的多进程主体）。
    for (size_t i = 0; i < snapshot.size(); ++i) {
        const ProcessEntry& e = snapshot[i];
        if (e.imagePath != key) continue;
        if (IsNeverLimitImage(e.imagePath)) continue;
        selected[i] = true;
        pending.push(i);
        ++sel.exactMatches;
    }
    if (sel.exactMatches == 0) {
        sel.note = L"主进程未运行";
        return sel;
    }

    // B. 只沿被选中的进程向下找辅助进程。
    while (!pending.empty()) {
        const size_t parentIndex = pending.front();
        pending.pop();
        const ProcessEntry& parent = snapshot[parentIndex];
        auto it = children.find(parent.pid);
        if (it == children.end()) continue;
        for (size_t childIndex : it->second) {
            if (selected[childIndex]) continue;
            const ProcessEntry& child = snapshot[childIndex];
            if (IsNeverLimitImage(child.imagePath)) continue;
            // 子进程本身就是 Dock 的另一个应用（Office 同目录的 Excel 等）：
            // 它是被拉起的独立应用，不是本应用的辅助进程。
            if (foreignAppKeys && child.imagePath != key &&
                foreignAppKeys->count(child.imagePath) != 0) {
                continue;
            }
            // PID 可能被复用：后代的创建时间不能早于父进程。
            if (parent.creationTime != 0 && child.creationTime != 0 &&
                child.creationTime < parent.creationTime) {
                continue;
            }
            const std::wstring childDir = ParentDirOf(child.imagePath);
            const bool sameDir =
                !sel.installRoot.empty() && childDir == sel.installRoot;
            const bool inside =
                !sel.broadRoot && !sel.installRoot.empty() &&
                IsInsideDir(child.imagePath, sel.installRoot);
            if (!sameDir && !inside) continue;  // 外部程序：不碰
            selected[childIndex] = true;
            pending.push(childIndex);
            ++sel.helperMatches;
        }
    }

    for (size_t i = 0; i < snapshot.size(); ++i) {
        if (!selected[i]) continue;
        ProcessRef ref;
        ref.pid = snapshot[i].pid;
        ref.creationTime = snapshot[i].creationTime;
        ref.exactMatch = snapshot[i].imagePath == key;
        sel.processes.push_back(ref);
    }
    std::sort(sel.processes.begin(), sel.processes.end(),
              [](const ProcessRef& a, const ProcessRef& b) {
                  return a.pid < b.pid;
              });

    sel.note = L"精确 " + std::to_wstring(sel.exactMatches) + L" + 辅助 " +
               std::to_wstring(sel.helperMatches) + L"（根 " +
               sel.installRoot + (sel.broadRoot ? L"，宽泛目录" : L"") + L"）";
    return sel;
}

struct CpuLimiter::Impl {
    ULONGLONG minProcessAgeMs = kMinProcessAgeDefaultMs;
    bool initialized = false;
    NtResumeProcessFn resumeFn = nullptr;

    struct AppEntry {
        HANDLE job = nullptr;
        bool capOn = false;
        std::vector<TrackedProcess> processes;
    };
    std::unordered_map<std::wstring, AppEntry> apps;

    explicit Impl(ULONGLONG minAge) : minProcessAgeMs(minAge) {}

    ~Impl() { Shutdown(); }

    bool Initialize() {
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        if (ntdll) {
            resumeFn = reinterpret_cast<NtResumeProcessFn>(
                GetProcAddress(ntdll, "NtResumeProcess"));
        }
        initialized = IsCpuRateControlSupported();
        return initialized;
    }

    static void AddTracked(std::vector<TrackedProcess>* list,
                           const ProcessRef& ref) {
        for (const TrackedProcess& t : *list) {
            if (t.pid == ref.pid && t.creation == ref.creationTime) return;
        }
        list->push_back(TrackedProcess{ref.pid, ref.creationTime});
    }

    static bool AnyTrackedAlive(const std::vector<TrackedProcess>& list) {
        for (const TrackedProcess& t : list) {
            if (TrackedAlive(t)) return true;
        }
        return false;
    }

    // 创建一个新 Job：CPU hard cap + silent breakaway。先不打开 cap，
    // 等进程都 Assign 完再打开，避免把 Assign 过程本身压到极慢。
    HANDLE CreateAppJob() {
        HANDLE job = CreateJobObjectW(nullptr, nullptr);
        if (!job) return nullptr;
        if (!EnableSilentBreakaway(job)) {
            CloseHandle(job);
            return nullptr;
        }
        return job;
    }

    // 把一个进程放进 Job。调用方负责 CloseHandle(proc)。
    // 返回 0=失败，1=新加入，2=本来就在 Job 里。
    int AssignVerifiedProcess(HANDLE job, const ProcessRef& ref,
                              size_t* reusedPids, size_t* deniedPids,
                              bool* critical) {
        DWORD err = ERROR_SUCCESS;
        HANDLE proc = OpenVerified(
            ref.pid, ref.creationTime,
            PROCESS_SET_QUOTA | PROCESS_TERMINATE |
                PROCESS_QUERY_LIMITED_INFORMATION,
            &err);
        if (!proc) {
            if (err == ERROR_NOT_FOUND || err == ERROR_INVALID_PARAMETER) {
                if (reusedPids) ++*reusedPids;
            } else if (deniedPids) {
                ++*deniedPids;
            }
            return 0;
        }
        if (IsProcessCritical(proc)) {
            if (critical) *critical = true;
            CloseHandle(proc);
            return 0;
        }
        BOOL inJob = FALSE;
        if (IsProcessInJob(proc, job, &inJob) && inJob) {
            CloseHandle(proc);
            return 2;
        }
        const BOOL ok = AssignProcessToJobObject(job, proc);
        CloseHandle(proc);
        if (!ok) {
            if (deniedPids) ++*deniedPids;
            return 0;
        }
        return 1;
    }

    std::vector<LimitResult> LimitMany(
        const std::vector<LimitRequest>& requests,
        const std::vector<std::wstring>& allAppKeys) {
        std::vector<LimitResult> results(requests.size());
        if (!initialized) {
            for (auto& r : results) {
                r.code = LimitCode::Failed;
                r.detail = L"Job Object CPU 限速不可用";
            }
            return results;
        }

        std::vector<size_t> todo;
        todo.reserve(requests.size());
        for (size_t i = 0; i < requests.size(); ++i) {
            const std::wstring key = NormalizePath(requests[i].appKey);
            if (key.empty()) {
                results[i].code = LimitCode::Failed;
                results[i].detail = L"空应用 key";
                continue;
            }
            auto it = apps.find(key);
            if (it != apps.end() && it->second.capOn) {
                if (AnyTrackedAlive(it->second.processes)) {
                    results[i].code = LimitCode::AlreadyLimited;
                    continue;
                }
                // 跟踪的进程都退出了：先解除旧 cap（不能带着 cap 关句柄），
                // 保留 Job 供本轮/下次复用。
                if (it->second.job &&
                    !ApplyCpuRate(it->second.job, false)) {
                    results[i].code = LimitCode::Failed;
                    results[i].detail = L"解除旧 CPU 限制失败";
                    continue;
                }
                it->second.capOn = false;
                it->second.processes.clear();
            }
            todo.push_back(i);
        }
        if (todo.empty()) return results;

        std::unordered_set<std::wstring> foreignAppKeys;
        for (const std::wstring& k : allAppKeys) {
            const std::wstring normalized = NormalizePath(k);
            if (!normalized.empty()) foreignAppKeys.insert(normalized);
        }

        const std::vector<ProcessEntry> snapshot = SnapshotProcesses();
        if (snapshot.empty()) {
            for (size_t i : todo) {
                results[i].code = LimitCode::NoProcesses;
                results[i].detail = L"进程快照为空";
            }
            return results;
        }

        FILETIME nowFt{};
        GetSystemTimeAsFileTime(&nowFt);
        const ULONGLONG now = FileTimeToU64(nowFt);
        const ULONGLONG minAge100ns = minProcessAgeMs * 10000ull;

        for (size_t requestIndex : todo) {
            LimitResult& result = results[requestIndex];
            const std::wstring key = NormalizePath(requests[requestIndex].appKey);
            const Selection sel = SelectAppProcesses(key, snapshot,
                                                    &foreignAppKeys);
            result.matched = sel.processes.size();
            result.detail = sel.note;

            if (sel.processes.empty()) {
                result.code = LimitCode::NoProcesses;
                continue;
            }

            // 最小年龄保护：只要有一个成员刚启动，就整组延后，避免把
            // 正在初始化的应用压到 0.01%。
            ULONGLONG maxRetry100ns = 0;
            bool tooYoung = false;
            for (const ProcessRef& ref : sel.processes) {
                if (ref.creationTime == 0 || ref.creationTime > now) {
                    continue;
                }
                const ULONGLONG age = now - ref.creationTime;
                if (age < minAge100ns) {
                    tooYoung = true;
                    maxRetry100ns =
                        std::max(maxRetry100ns, minAge100ns - age);
                }
            }
            if (tooYoung) {
                result.code = LimitCode::TooYoung;
                const ULONGLONG retryMs = (maxRetry100ns + 9999) / 10000;
                result.retryAfterMs =
                    static_cast<DWORD>(std::min<ULONGLONG>(retryMs + 100,
                                                          60000));
                result.detail += L"；进程刚启动，延后限速";
                continue;
            }

            auto entryIt = apps.find(key);
            if (entryIt == apps.end()) {
                AppEntry fresh;
                fresh.job = CreateAppJob();
                if (!fresh.job) {
                    result.code = LimitCode::Failed;
                    result.detail += L"；创建限速 Job 失败";
                    continue;
                }
                entryIt = apps.emplace(key, std::move(fresh)).first;
            } else if (!entryIt->second.job) {
                entryIt->second.job = CreateAppJob();
                if (!entryIt->second.job) {
                    result.code = LimitCode::Failed;
                    result.detail += L"；创建限速 Job 失败";
                    continue;
                }
            }
            AppEntry& entry = entryIt->second;

            std::vector<TrackedProcess> tracked;
            size_t added = 0;
            size_t already = 0;
            size_t reused = 0;
            size_t denied = 0;
            size_t exactLimited = 0;
            bool critical = false;
            for (const ProcessRef& ref : sel.processes) {
                const int status = AssignVerifiedProcess(
                    entry.job, ref, &reused, &denied, &critical);
                if (critical) break;
                if (status == 1) ++added;
                if (status == 2) ++already;
                if (status != 0) {
                    tracked.push_back(TrackedProcess{ref.pid, ref.creationTime});
                    if (ref.exactMatch) ++exactLimited;
                }
            }

            if (critical) {
                result.code = LimitCode::AccessDenied;
                result.limited = 0;
                result.skipped = sel.processes.size();
                result.detail += L"；含受保护关键进程，放弃本组限速";
                if (entry.job) {
                    ApplyCpuRate(entry.job, false);
                    CloseHandle(entry.job);
                }
                apps.erase(key);
                continue;
            }

            // 主进程一个都没进来：只限制 helper 会形成“主进程全速、子进程
            // 饿死”的半吊子状态，整组放弃，等下一轮重试。
            if (exactLimited == 0) {
                result.code = denied > 0 ? LimitCode::AccessDenied
                                         : LimitCode::NoProcesses;
                result.skipped = sel.processes.size();
                result.detail += denied > 0
                                     ? L"；主进程无权限加入 CPU 限制"
                                     : L"；主进程已退出";
                if (entry.job) {
                    ApplyCpuRate(entry.job, false);
                    CloseHandle(entry.job);
                }
                apps.erase(key);
                continue;
            }

            if (!ApplyCpuRate(entry.job, true)) {
                result.code = LimitCode::Failed;
                result.skipped = sel.processes.size();
                result.detail += L"；设置 CPU 硬限制失败";
                ApplyCpuRate(entry.job, false);
                CloseHandle(entry.job);
                apps.erase(key);
                continue;
            }

            entry.capOn = true;
            entry.processes = std::move(tracked);
            result.code = LimitCode::Limited;
            result.limited = entry.processes.size();
            result.skipped = sel.processes.size() - result.limited;
            result.detail += L"；已限制 " + std::to_wstring(result.limited) +
                             L" 个进程到 0.01% CPU";
            if (added > 0) {
                result.detail += L"，新加入 " + std::to_wstring(added);
            }
            if (already > 0) {
                result.detail += L"，已在 Job " + std::to_wstring(already);
            }
            if (reused > 0) {
                result.detail += L"，已退出 " + std::to_wstring(reused);
            }
            if (denied > 0) {
                result.detail += L"，无权限 " + std::to_wstring(denied);
            }
        }
        return results;
    }

    size_t EnforceLimited(const std::vector<std::wstring>& appKeys,
                          const std::vector<std::wstring>& allAppKeys,
                          std::wstring* detail) {
        if (!initialized) return 0;
        std::vector<std::wstring> keys;
        keys.reserve(appKeys.size());
        for (const std::wstring& k : appKeys) {
            const std::wstring normalized = NormalizePath(k);
            auto it = apps.find(normalized);
            if (it != apps.end() && it->second.capOn &&
                it->second.job != nullptr) {
                keys.push_back(normalized);
            }
        }
        if (keys.empty()) return 0;

        std::unordered_set<std::wstring> foreignAppKeys;
        for (const std::wstring& k : allAppKeys) {
            const std::wstring normalized = NormalizePath(k);
            if (!normalized.empty()) foreignAppKeys.insert(normalized);
        }

        const std::vector<ProcessEntry> snapshot = SnapshotProcesses();
        if (snapshot.empty()) return 0;

        size_t totalAdded = 0;
        for (const std::wstring& key : keys) {
            auto entryIt = apps.find(key);
            if (entryIt == apps.end() || !entryIt->second.capOn) continue;
            const Selection sel = SelectAppProcesses(key, snapshot,
                                                    &foreignAppKeys);
            for (const ProcessRef& ref : sel.processes) {
                size_t denied = 0;
                size_t reused = 0;
                bool critical = false;
                const int status = AssignVerifiedProcess(
                    entryIt->second.job, ref, &reused, &denied, &critical);
                if (critical) break;
                if (status == 1) {
                    ++totalAdded;
                    AddTracked(&entryIt->second.processes, ref);
                } else if (status == 2) {
                    AddTracked(&entryIt->second.processes, ref);
                }
            }
        }
        if (totalAdded > 0 && detail) {
            *detail = L"已补限 " + std::to_wstring(totalAdded) + L" 个新进程";
        }
        return totalAdded;
    }

    bool Restore(const std::wstring& appKey, std::wstring* detail) {
        const std::wstring key = NormalizePath(appKey);
        auto it = apps.find(key);
        if (it == apps.end() || !it->second.capOn) {
            if (detail) *detail = L"未处于限速状态";
            return true;
        }
        if (!ApplyCpuRate(it->second.job, false)) {
            if (detail) *detail = L"解除 CPU 限制失败";
            return false;
        }
        it->second.capOn = false;
        // 解除后 Job 句柄仍保留，方便同一应用下次直接复用；若跟踪的
        // 进程都已退出，关掉句柄释放资源（此时 cap 已关闭，安全）。
        if (!AnyTrackedAlive(it->second.processes)) {
            if (it->second.job) CloseHandle(it->second.job);
            apps.erase(it);
        } else {
            it->second.processes.clear();
        }
        if (detail) *detail = L"已解除 CPU 限制";
        return true;
    }

    void RestoreAll() {
        std::vector<std::wstring> keys;
        keys.reserve(apps.size());
        for (const auto& kv : apps) {
            if (kv.second.capOn) keys.push_back(kv.first);
        }
        for (const std::wstring& key : keys) {
            Restore(key, nullptr);
        }
    }

    bool IsLimited(const std::wstring& appKey) const {
        auto it = apps.find(NormalizePath(appKey));
        return it != apps.end() && it->second.capOn;
    }

    size_t TrackedProcessCount() const {
        size_t n = 0;
        for (const auto& kv : apps) {
            if (kv.second.capOn) n += kv.second.processes.size();
        }
        return n;
    }

    std::vector<std::wstring> LimitedKeys() const {
        std::vector<std::wstring> keys;
        keys.reserve(apps.size());
        for (const auto& kv : apps) {
            if (kv.second.capOn) keys.push_back(kv.first);
        }
        std::sort(keys.begin(), keys.end());
        return keys;
    }

    size_t RecoverLegacySuspended(std::wstring* detail) {
        size_t recovered = 0;
        if (!resumeFn) {
            if (detail) *detail = L"NtResumeProcess 不可用，无法恢复旧挂起表";
            return 0;
        }
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegPath, 0, KEY_READ | KEY_WRITE,
                          &key) != ERROR_SUCCESS) {
            return 0;
        }
        DWORD type = 0;
        DWORD bytes = 0;
        if (RegQueryValueExW(key, kLegacyStateValue, nullptr, &type, nullptr,
                             &bytes) != ERROR_SUCCESS ||
            type != REG_BINARY || bytes < kLegacyHeaderBytes) {
            RegCloseKey(key);
            return 0;
        }
        std::vector<BYTE> buffer(bytes);
        if (RegQueryValueExW(key, kLegacyStateValue, nullptr, &type,
                             buffer.data(), &bytes) != ERROR_SUCCESS) {
            RegCloseKey(key);
            return 0;
        }

        if (bytes < kLegacyHeaderBytes) {
            RegCloseKey(key);
            return 0;
        }
        DWORD magic = 0;
        DWORD version = 0;
        DWORD count = 0;
        memcpy(&magic, buffer.data(), sizeof(magic));
        memcpy(&version, buffer.data() + 4, sizeof(version));
        memcpy(&count, buffer.data() + 8, sizeof(count));
        if (magic != kLegacyMagic || version != kLegacyVersion ||
            count > 100000) {
            RegDeleteValueW(key, kLegacyStateValue);
            RegCloseKey(key);
            return 0;
        }

        size_t offset = kLegacyHeaderBytes;
        size_t failed = 0;
        for (DWORD i = 0; i < count; ++i) {
            if (offset + sizeof(DWORD) + sizeof(ULONGLONG) + sizeof(DWORD) >
                buffer.size()) {
                break;
            }
            DWORD pid = 0;
            ULONGLONG creation = 0;
            DWORD chars = 0;
            memcpy(&pid, buffer.data() + offset, sizeof(pid));
            offset += sizeof(pid);
            memcpy(&creation, buffer.data() + offset, sizeof(creation));
            offset += sizeof(creation);
            memcpy(&chars, buffer.data() + offset, sizeof(chars));
            offset += sizeof(chars);
            const size_t keyBytes = static_cast<size_t>(chars) * sizeof(wchar_t);
            if (chars > 32768 || offset + keyBytes > buffer.size()) break;
            offset += keyBytes;

            DWORD err = ERROR_SUCCESS;
            HANDLE proc = OpenVerified(
                pid, creation,
                PROCESS_SUSPEND_RESUME | PROCESS_QUERY_LIMITED_INFORMATION,
                &err);
            if (!proc) {
                if (err != ERROR_NOT_FOUND &&
                    err != ERROR_INVALID_PARAMETER) {
                    ++failed;
                }
                continue;
            }
            const LONG status = resumeFn(proc);
            CloseHandle(proc);
            if (NtSuccess(status)) {
                ++recovered;
            } else {
                ++failed;
            }
        }

        if (failed == 0) {
            RegDeleteValueW(key, kLegacyStateValue);
        }
        RegCloseKey(key);
        if (detail) {
            *detail = L"恢复旧挂起进程 " + std::to_wstring(recovered);
            if (failed > 0) {
                *detail += L"，仍失败 " + std::to_wstring(failed);
            }
        }
        return recovered;
    }

    void Shutdown() {
        RestoreAll();
        for (auto& kv : apps) {
            if (!kv.second.job) continue;
            // 仍处于 capOn 的条目说明解除失败；不要关闭它的句柄（虽然
            // 进程退出时 OS 仍会关，但至少不在代码里主动制造硬上限残留）。
            if (!kv.second.capOn) CloseHandle(kv.second.job);
            kv.second.job = nullptr;
        }
        apps.clear();
    }
};

CpuLimiter::CpuLimiter(ULONGLONG minProcessAgeMs)
    : impl_(std::make_unique<Impl>(minProcessAgeMs)) {}

CpuLimiter::~CpuLimiter() = default;

bool CpuLimiter::Initialize() { return impl_->Initialize(); }

std::vector<LimitResult> CpuLimiter::LimitMany(
    const std::vector<LimitRequest>& requests,
    const std::vector<std::wstring>& allAppKeys) {
    return impl_->LimitMany(requests, allAppKeys);
}

size_t CpuLimiter::EnforceLimited(
    const std::vector<std::wstring>& appKeys,
    const std::vector<std::wstring>& allAppKeys, std::wstring* detail) {
    return impl_->EnforceLimited(appKeys, allAppKeys, detail);
}

bool CpuLimiter::Restore(const std::wstring& appKey, std::wstring* detail) {
    return impl_->Restore(appKey, detail);
}

void CpuLimiter::RestoreAll() { impl_->RestoreAll(); }

bool CpuLimiter::IsLimited(const std::wstring& appKey) const {
    return impl_->IsLimited(appKey);
}

size_t CpuLimiter::TrackedProcessCount() const {
    return impl_->TrackedProcessCount();
}

std::vector<std::wstring> CpuLimiter::LimitedKeys() const {
    return impl_->LimitedKeys();
}

size_t CpuLimiter::RecoverLegacySuspended(std::wstring* detail) {
    return impl_->RecoverLegacySuspended(detail);
}

}  // namespace appcpu
