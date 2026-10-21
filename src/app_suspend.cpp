// app_suspend.cpp - 见 app_suspend.h 的口径说明
//
// 这里只做三件事：
//   1. 从全量进程快照中按「同一 exe + 安装目录树内的后代」选出应用进程；
//   2. 通过 ntdll!NtSuspendProcess / NtResumeProcess 整进程挂起/恢复；
//   3. 用 PID + 创建时间双保险，并把挂起表持久化，供异常退出后恢复。
//
// 不按进程树整棵挂起、不按 exe 名字合并、不碰系统基础设施，原因见头文件。

#include "app_suspend.h"

#include <tlhelp32.h>

#include <algorithm>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace appsuspend {
namespace {

constexpr wchar_t kRegPath[] = L"Software\\DesktopSuite\\Dock";
constexpr DWORD kPersistMagic = 0x50555353;  // "SSUP"
constexpr DWORD kPersistVersion = 1;
constexpr DWORD kPersistHeaderBytes = sizeof(DWORD) * 3;

// 刚启动的进程先不挂起：Chrome/Electron/UWP 启动阶段会连续创建子进程，
// 太早冻住会把「正在打开的应用」永久卡在启动中。
constexpr ULONGLONG kMinProcessAgeDefaultMs = 8000;

// 硬件断点/调试器挂起等场景不属于这里；只列常见的系统基础设施。
// 基名均小写。应用自己的主 exe 若命中这里，Dock 侧也不会把它当可挂起项。
const wchar_t* const kNeverSuspendBasenames[] = {
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

std::wstring ToLower(std::wstring s) {
    for (auto& ch : s) ch = static_cast<wchar_t>(towlower(ch));
    return s;
}

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

bool IsNeverSuspendBase(const std::wstring& baseLower) {
    for (const wchar_t* name : kNeverSuspendBasenames) {
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

struct ProcKey {
    DWORD pid = 0;
    ULONGLONG creation = 0;

    bool operator==(const ProcKey& other) const {
        return pid == other.pid && creation == other.creation;
    }
};

struct ProcKeyHash {
    size_t operator()(const ProcKey& k) const noexcept {
        const size_t a = std::hash<DWORD>{}(k.pid);
        const size_t b = std::hash<ULONGLONG>{}(k.creation);
        return a ^ (b + 0x9e3779b97f4a7c15ull + (a << 6) + (a >> 2));
    }
};

struct TrackedProcess {
    DWORD pid = 0;
    ULONGLONG creation = 0;
};

struct PersistRecord {
    DWORD pid = 0;
    ULONGLONG creation = 0;
    std::wstring appKey;
};

using NtProcessControlFn = LONG(NTAPI*)(HANDLE);

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

bool IsNeverSuspendImage(const std::wstring& imagePath) {
    return IsNeverSuspendBase(BaseNameOf(NormalizePath(imagePath)));
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
            if (IsNeverSuspendImage(normalized)) continue;

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
        if (IsNeverSuspendImage(e.imagePath)) continue;
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
            if (IsNeverSuspendImage(child.imagePath)) continue;
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

struct Suspender::Impl {
    bool persist = true;
    ULONGLONG minProcessAgeMs = kMinProcessAgeDefaultMs;
    std::wstring stateValueName = L"SuspendedState";
    bool initialized = false;
    NtProcessControlFn suspendFn = nullptr;
    NtProcessControlFn resumeFn = nullptr;

    std::unordered_map<std::wstring, std::vector<TrackedProcess>> apps;
    std::unordered_map<ProcKey, std::wstring, ProcKeyHash> owner;

    Impl(bool persistState, ULONGLONG minAge, std::wstring valueName)
        : persist(persistState),
          minProcessAgeMs(minAge),
          stateValueName(std::move(valueName)) {}

    bool Initialize() {
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        if (!ntdll) return false;
        suspendFn = reinterpret_cast<NtProcessControlFn>(
            GetProcAddress(ntdll, "NtSuspendProcess"));
        resumeFn = reinterpret_cast<NtProcessControlFn>(
            GetProcAddress(ntdll, "NtResumeProcess"));
        initialized = suspendFn != nullptr && resumeFn != nullptr;
        return initialized;
    }

    std::vector<SuspendResult> SuspendMany(
        const std::vector<SuspendRequest>& requests,
        const std::vector<std::wstring>& allAppKeys) {
        std::vector<SuspendResult> results(requests.size());
        if (!initialized) {
            for (auto& r : results) {
                r.code = SuspendCode::Failed;
                r.detail = L"NtSuspendProcess 不可用";
            }
            return results;
        }

        bool stateChanged = false;
        std::vector<size_t> todo;
        todo.reserve(requests.size());
        for (size_t i = 0; i < requests.size(); ++i) {
            const std::wstring key = NormalizePath(requests[i].appKey);
            if (key.empty()) {
                results[i].code = SuspendCode::Failed;
                results[i].detail = L"空应用 key";
                continue;
            }
            auto existing = apps.find(key);
            if (existing != apps.end()) {
                // 应用可能在挂起期间被用户从任务管理器结束、或自行退出后
                // 又以同一 exe 重新启动。清掉已死的跟踪项；否则新实例
                // 会被误判为 AlreadySuspended，永远不会再被挂起。
                std::vector<TrackedProcess> alive;
                for (const TrackedProcess& t : existing->second) {
                    if (TrackedAlive(t)) {
                        alive.push_back(t);
                    } else {
                        owner.erase(ProcKey{t.pid, t.creation});
                    }
                }
                if (!alive.empty()) {
                    if (alive.size() != existing->second.size()) {
                        existing->second = std::move(alive);
                        stateChanged = true;
                    }
                    results[i].code = SuspendCode::AlreadySuspended;
                    continue;
                }
                apps.erase(existing);
                stateChanged = true;
            }
            todo.push_back(i);
        }
        if (todo.empty()) {
            if (stateChanged) Persist();
            return results;
        }

        std::unordered_set<std::wstring> foreignAppKeys;
        for (const std::wstring& k : allAppKeys) {
            const std::wstring normalized = NormalizePath(k);
            if (!normalized.empty()) foreignAppKeys.insert(normalized);
        }

        const std::vector<ProcessEntry> snapshot = SnapshotProcesses();
        if (snapshot.empty()) {
            for (size_t i : todo) {
                results[i].code = SuspendCode::NoProcesses;
                results[i].detail = L"进程快照为空";
            }
            if (stateChanged) Persist();
            return results;
        }

        FILETIME nowFt{};
        GetSystemTimeAsFileTime(&nowFt);
        const ULONGLONG now = FileTimeToU64(nowFt);
        const ULONGLONG minAge100ns = minProcessAgeMs * 10000ull;

        for (size_t requestIndex : todo) {
            SuspendResult& result = results[requestIndex];
            const std::wstring key = NormalizePath(requests[requestIndex].appKey);
            const Selection sel = SelectAppProcesses(key, snapshot,
                                                    &foreignAppKeys);
            result.matched = sel.processes.size();
            result.detail = sel.note;

            if (sel.processes.empty()) {
                result.code = SuspendCode::NoProcesses;
                continue;
            }

            // 最小年龄保护：只要有一个成员刚启动，就整组延后，避免
            // 主进程被冻住、辅助进程还没初始化完的半启动状态。
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
                result.code = SuspendCode::TooYoung;
                const ULONGLONG retryMs = (maxRetry100ns + 9999) / 10000;
                result.retryAfterMs =
                    static_cast<DWORD>(std::min<ULONGLONG>(retryMs + 100,
                                                          60000));
                result.detail += L"；进程刚启动，延后挂起";
                continue;
            }

            // 两阶段：先把整组进程都打开并核对，再统一挂起。
            // 任意一个成员无权限/受保护就整组放弃 —— 只冻住一半（例如主
            // 进程还在、渲染 helper 被冻）比不冻更容易把应用挂死。
            std::vector<TrackedProcess> candidates;
            std::vector<HANDLE> handles;
            size_t accessDenied = 0;
            size_t critical = 0;
            size_t reused = 0;
            size_t ownedByOther = 0;
            bool aborted = false;

            for (const ProcessRef& ref : sel.processes) {
                if (ref.creationTime == 0) {
                    ++accessDenied;
                    aborted = true;
                    break;
                }
                const ProcKey pk{ref.pid, ref.creationTime};
                auto own = owner.find(pk);
                if (own != owner.end()) {
                    if (own->second != key) ++ownedByOther;
                    continue;
                }

                DWORD err = ERROR_SUCCESS;
                HANDLE proc = OpenVerified(
                    ref.pid, ref.creationTime,
                    PROCESS_SUSPEND_RESUME | PROCESS_QUERY_LIMITED_INFORMATION,
                    &err);
                if (!proc) {
                    if (err == ERROR_NOT_FOUND ||
                        err == ERROR_INVALID_PARAMETER) {
                        ++reused;  // 刚好退出/PID 已被复用：跳过即可
                        continue;
                    }
                    ++accessDenied;
                    aborted = true;
                    break;
                }
                if (IsProcessCritical(proc)) {
                    ++critical;
                    CloseHandle(proc);
                    aborted = true;
                    break;
                }
                candidates.push_back(
                    TrackedProcess{ref.pid, ref.creationTime});
                handles.push_back(proc);
            }

            if (aborted) {
                for (HANDLE h : handles) CloseHandle(h);
                result.code = SuspendCode::AccessDenied;
                result.skipped = sel.processes.size();
                result.detail += L"；为保证整组一致，放弃本次挂起";
                if (accessDenied > 0) {
                    result.detail += L"（无权限/无法核对 " +
                                     std::to_wstring(accessDenied) + L"）";
                }
                if (critical > 0) {
                    result.detail += L"（受保护关键进程 " +
                                     std::to_wstring(critical) + L"）";
                }
                continue;
            }

            if (handles.empty()) {
                result.code = SuspendCode::NoProcesses;
                result.detail += L"；进程均已退出";
                if (reused > 0) {
                    result.detail += L"（" + std::to_wstring(reused) +
                                     L" 个已退出/PID 复用）";
                }
                if (ownedByOther > 0) {
                    result.detail += L"（" + std::to_wstring(ownedByOther) +
                                     L" 个已由其他应用挂起）";
                }
                continue;
            }

            size_t suspended = 0;
            bool suspendFailed = false;
            for (size_t i = 0; i < handles.size(); ++i) {
                if (!NtSuccess(suspendFn(handles[i]))) {
                    suspendFailed = true;
                    break;
                }
                ++suspended;
            }
            if (suspendFailed) {
                // 回滚已成功挂起的那部分，避免应用处于半冻结状态。
                // 若回滚本身也失败，必须把失败项留在跟踪表里，退出时
                // ResumeAll 还能再试，绝不能让进程无声地停在挂起态。
                std::vector<TrackedProcess> stuck;
                for (size_t i = 0; i < handles.size(); ++i) {
                    if (!NtSuccess(resumeFn(handles[i]))) {
                        stuck.push_back(candidates[i]);
                    }
                }
                for (HANDLE h : handles) CloseHandle(h);
                result.code = SuspendCode::Failed;
                result.skipped = sel.processes.size();
                result.detail += L"；挂起中途失败";
                if (!stuck.empty()) {
                    for (const TrackedProcess& t : stuck) {
                        owner[ProcKey{t.pid, t.creation}] = key;
                    }
                    apps[key] = std::move(stuck);
                    stateChanged = true;
                    result.detail += L"，回滚失败已保留跟踪";
                } else {
                    result.detail += L"，已回滚整组";
                }
                continue;
            }

            for (HANDLE h : handles) CloseHandle(h);
            for (const TrackedProcess& t : candidates) {
                owner[ProcKey{t.pid, t.creation}] = key;
            }
            apps[key] = std::move(candidates);
            stateChanged = true;
            result.code = SuspendCode::Suspended;
            result.suspended = suspended;
            result.skipped = sel.processes.size() - suspended;
            result.detail += L"；已挂起 " + std::to_wstring(suspended);
            if (reused > 0) {
                result.detail += L"，已退出 " + std::to_wstring(reused);
            }
            if (ownedByOther > 0) {
                result.detail += L"，共享进程已在挂起态 " +
                                 std::to_wstring(ownedByOther);
            }
        }

        if (stateChanged) Persist();
        return results;
    }

    bool Resume(const std::wstring& appKey, std::wstring* detail) {
        const std::wstring key = NormalizePath(appKey);
        auto it = apps.find(key);
        if (it == apps.end()) {
            if (detail) *detail = L"未挂起";
            return true;
        }

        std::vector<TrackedProcess> remain;
        size_t resumed = 0;
        size_t failed = 0;
        std::wstring failedPids;
        for (const TrackedProcess& t : it->second) {
            const ProcKey pk{t.pid, t.creation};
            DWORD err = ERROR_SUCCESS;
            HANDLE proc = OpenVerified(
                t.pid, t.creation,
                PROCESS_SUSPEND_RESUME | PROCESS_QUERY_LIMITED_INFORMATION,
                &err);
            if (!proc) {
                if (err == ERROR_NOT_FOUND || err == ERROR_INVALID_PARAMETER) {
                    // 进程已退出或 PID 已复用：没有可恢复的对象了。
                    owner.erase(pk);
                    continue;
                }
                remain.push_back(t);
                ++failed;
                if (failedPids.size() < 160) {
                    if (!failedPids.empty()) failedPids += L",";
                    failedPids += std::to_wstring(t.pid);
                }
                continue;
            }
            const LONG status = resumeFn(proc);
            CloseHandle(proc);
            if (NtSuccess(status)) {
                owner.erase(pk);
                ++resumed;
            } else {
                remain.push_back(t);
                ++failed;
                if (failedPids.size() < 160) {
                    if (!failedPids.empty()) failedPids += L",";
                    failedPids += std::to_wstring(t.pid);
                }
            }
        }

        if (remain.empty()) {
            apps.erase(it);
        } else {
            it->second = std::move(remain);
        }
        Persist();
        if (detail) {
            *detail = L"恢复 " + std::to_wstring(resumed);
            if (failed > 0) {
                *detail += L"，失败 " + std::to_wstring(failed) +
                           L"（pid " + failedPids + L"）";
            }
        }
        return failed == 0;
    }

    void ResumeAll() {
        std::vector<std::wstring> keys;
        keys.reserve(apps.size());
        for (const auto& kv : apps) keys.push_back(kv.first);
        for (const auto& key : keys) {
            std::wstring detail;
            Resume(key, &detail);
        }
    }

    bool IsSuspended(const std::wstring& appKey) const {
        auto it = apps.find(NormalizePath(appKey));
        return it != apps.end() && !it->second.empty();
    }

    size_t TrackedProcessCount() const {
        size_t n = 0;
        for (const auto& kv : apps) n += kv.second.size();
        return n;
    }

    std::vector<std::wstring> SuspendedKeys() const {
        std::vector<std::wstring> keys;
        keys.reserve(apps.size());
        for (const auto& kv : apps) {
            if (!kv.second.empty()) keys.push_back(kv.first);
        }
        std::sort(keys.begin(), keys.end());
        return keys;
    }

    void Persist() {
        if (!persist) return;
        HKEY key = nullptr;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegPath, 0, nullptr, 0,
                            KEY_WRITE, nullptr, &key, nullptr) !=
            ERROR_SUCCESS) {
            return;
        }

        std::vector<PersistRecord> records;
        for (const auto& kv : apps) {
            for (const TrackedProcess& t : kv.second) {
                records.push_back(PersistRecord{t.pid, t.creation, kv.first});
            }
        }
        if (records.empty()) {
            RegDeleteValueW(key, stateValueName.c_str());
            RegCloseKey(key);
            return;
        }

        std::vector<BYTE> buffer;
        buffer.reserve(kPersistHeaderBytes + records.size() * 64);
        const DWORD magic = kPersistMagic;
        const DWORD version = kPersistVersion;
        const DWORD count = static_cast<DWORD>(records.size());
        auto append = [&buffer](const void* data, size_t bytes) {
            const auto* p = static_cast<const BYTE*>(data);
            buffer.insert(buffer.end(), p, p + bytes);
        };
        append(&magic, sizeof(magic));
        append(&version, sizeof(version));
        append(&count, sizeof(count));
        for (const PersistRecord& r : records) {
            const DWORD chars = static_cast<DWORD>(r.appKey.size());
            append(&r.pid, sizeof(r.pid));
            append(&r.creation, sizeof(r.creation));
            append(&chars, sizeof(chars));
            if (chars > 0) {
                append(r.appKey.data(), chars * sizeof(wchar_t));
            }
        }
        RegSetValueExW(key, stateValueName.c_str(), 0, REG_BINARY,
                       buffer.data(),
                       static_cast<DWORD>(buffer.size()));
        RegCloseKey(key);
    }

    size_t RecoverPersisted(std::wstring* detail) {
        if (!persist || !initialized) return 0;
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegPath, 0, KEY_READ | KEY_WRITE,
                          &key) != ERROR_SUCCESS) {
            return 0;
        }
        DWORD type = 0;
        DWORD bytes = 0;
        if (RegQueryValueExW(key, stateValueName.c_str(), nullptr, &type,
                             nullptr, &bytes) != ERROR_SUCCESS ||
            type != REG_BINARY || bytes < kPersistHeaderBytes) {
            RegCloseKey(key);
            return 0;
        }
        std::vector<BYTE> buffer(bytes);
        if (RegQueryValueExW(key, stateValueName.c_str(), nullptr, &type,
                             buffer.data(), &bytes) != ERROR_SUCCESS) {
            RegCloseKey(key);
            return 0;
        }
        RegCloseKey(key);

        if (bytes < kPersistHeaderBytes) return 0;
        DWORD magic = 0;
        DWORD version = 0;
        DWORD count = 0;
        memcpy(&magic, buffer.data(), sizeof(magic));
        memcpy(&version, buffer.data() + 4, sizeof(version));
        memcpy(&count, buffer.data() + 8, sizeof(count));
        if (magic != kPersistMagic || version != kPersistVersion ||
            count > 100000) {
            RegDeleteKeyValueW(HKEY_CURRENT_USER, kRegPath,
                               stateValueName.c_str());
            return 0;
        }

        size_t offset = kPersistHeaderBytes;
        size_t recovered = 0;
        size_t failed = 0;
        std::vector<PersistRecord> remain;
        for (DWORD i = 0; i < count; ++i) {
            if (offset + sizeof(DWORD) + sizeof(ULONGLONG) + sizeof(DWORD) >
                buffer.size()) {
                break;
            }
            PersistRecord r;
            memcpy(&r.pid, buffer.data() + offset, sizeof(r.pid));
            offset += sizeof(r.pid);
            memcpy(&r.creation, buffer.data() + offset, sizeof(r.creation));
            offset += sizeof(r.creation);
            DWORD chars = 0;
            memcpy(&chars, buffer.data() + offset, sizeof(chars));
            offset += sizeof(chars);
            const size_t keyBytes = static_cast<size_t>(chars) * sizeof(wchar_t);
            if (chars > 32768 || offset + keyBytes > buffer.size()) break;
            if (chars > 0) {
                r.appKey.assign(
                    reinterpret_cast<const wchar_t*>(buffer.data() + offset),
                    chars);
            }
            offset += keyBytes;

            const std::wstring normalized = NormalizePath(r.appKey);
            DWORD err = ERROR_SUCCESS;
            HANDLE proc = OpenVerified(
                r.pid, r.creation,
                PROCESS_SUSPEND_RESUME | PROCESS_QUERY_LIMITED_INFORMATION,
                &err);
            if (!proc) {
                // 已退出/PID 复用：不需要恢复，直接丢弃。
                if (err != ERROR_NOT_FOUND &&
                    err != ERROR_INVALID_PARAMETER) {
                    r.appKey = normalized;
                    remain.push_back(r);
                    ++failed;
                }
                continue;
            }
            const LONG status = resumeFn(proc);
            CloseHandle(proc);
            if (NtSuccess(status)) {
                ++recovered;
            } else {
                r.appKey = normalized;
                remain.push_back(r);
                ++failed;
            }
        }

        if (remain.empty()) {
            RegDeleteKeyValueW(HKEY_CURRENT_USER, kRegPath,
                               stateValueName.c_str());
        } else {
            // 恢复失败的条目继续留在注册表里，下次启动再试。
            apps.clear();
            owner.clear();
            for (const PersistRecord& r : remain) {
                if (r.appKey.empty()) continue;
                apps[r.appKey].push_back(
                    TrackedProcess{r.pid, r.creation});
                owner[ProcKey{r.pid, r.creation}] = r.appKey;
            }
            Persist();
        }
        if (detail) {
            *detail = L"恢复 " + std::to_wstring(recovered);
            if (failed > 0) {
                *detail += L"，仍失败 " + std::to_wstring(failed);
            }
        }
        return recovered;
    }
};

Suspender::Suspender(bool persist, ULONGLONG minProcessAgeMs,
                     std::wstring stateValueName)
    : impl_(std::make_unique<Impl>(persist, minProcessAgeMs,
                                   std::move(stateValueName))) {}

Suspender::~Suspender() {
    if (impl_) impl_->ResumeAll();
}

bool Suspender::Initialize() { return impl_->Initialize(); }

std::vector<SuspendResult> Suspender::SuspendMany(
    const std::vector<SuspendRequest>& requests,
    const std::vector<std::wstring>& allAppKeys) {
    return impl_->SuspendMany(requests, allAppKeys);
}

bool Suspender::Resume(const std::wstring& appKey, std::wstring* detail) {
    return impl_->Resume(appKey, detail);
}

void Suspender::ResumeAll() { impl_->ResumeAll(); }

bool Suspender::IsSuspended(const std::wstring& appKey) const {
    return impl_->IsSuspended(appKey);
}

size_t Suspender::TrackedProcessCount() const {
    return impl_->TrackedProcessCount();
}

std::vector<std::wstring> Suspender::SuspendedKeys() const {
    return impl_->SuspendedKeys();
}

size_t Suspender::RecoverPersisted(std::wstring* detail) {
    return impl_->RecoverPersisted(detail);
}

}  // namespace appsuspend
