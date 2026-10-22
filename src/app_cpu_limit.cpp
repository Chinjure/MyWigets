// app_cpu_limit.cpp - 见 app_cpu_limit.h 的口径说明
//
// 这里做四件事：
//   1. 从全量进程快照中按「同一 exe + 安装目录树内的后代」选出应用进程；
//   2. 对每个应用进程调用 SetProcessInformation(ProcessPowerThrottling)，
//      把 QoS 切到效率模式（EcoQoS）；
//   3. 记录启用前的 PowerThrottling 状态，恢复时原样写回；
//   4. 用 PID + 创建时间双保险，并周期性把新出现的同应用辅助进程补进
//      效率模式（EcoQoS 不会随进程创建继承）。
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

// 刚启动的进程先不切效率模式：Chrome/Electron/UWP 启动阶段会连续创建
// 子进程，太早降速会把「正在打开的应用」卡在启动中。
constexpr ULONGLONG kMinProcessAgeDefaultMs = 8000;

// 硬件断点/调试器挂起等场景不属于这里；只列常见的系统基础设施。
// 基名均小写。应用自己的主 exe 若命中这里，Dock 侧也不会把它当可降速项。
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

// ---- Process Power Throttling (EcoQoS) ----
// _WIN32_WINNT 可能仍按 0x0601 编译，SDK 不一定导出 Windows 8 的新枚举名，
// 这里用 ABI 值：ProcessPowerThrottling = 4。
constexpr PROCESS_INFORMATION_CLASS kProcessPowerThrottling =
    static_cast<PROCESS_INFORMATION_CLASS>(4);
constexpr DWORD kPowerThrottlingVersion = 1;
constexpr DWORD kPowerThrottlingExecutionSpeed = 0x1;

// 与 PROCESS_POWER_THROTTLING_STATE 同布局，避免旧 _WIN32_WINNT 下类型缺失。
struct PowerThrottlingState {
    DWORD Version;
    DWORD ControlMask;
    DWORD StateMask;
};

using SetProcessInformationFn = BOOL(WINAPI*)(HANDLE,
                                              PROCESS_INFORMATION_CLASS, LPVOID,
                                              DWORD);
using GetProcessInformationFn = BOOL(WINAPI*)(HANDLE,
                                              PROCESS_INFORMATION_CLASS, LPVOID,
                                              DWORD);

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
    // 启用 EcoQoS 前的 PowerThrottling 状态，恢复时原样写回，避免把
    // 用户/系统本来就设好的效率模式误关掉。
    DWORD prevControlMask = 0;
    DWORD prevStateMask = 0;
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

// 动态取 kernel32!SetProcessInformation / GetProcessInformation：
// 项目仍按 _WIN32_WINNT=0x0601 编译，直接引用可能被 SDK 声明屏蔽；
// Windows 7 上没有这两个函数，功能降级为关闭。
SetProcessInformationFn SetProcessInformationPtr() {
    static SetProcessInformationFn fn = []() -> SetProcessInformationFn {
        HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
        if (!kernel) return nullptr;
        return reinterpret_cast<SetProcessInformationFn>(
            GetProcAddress(kernel, "SetProcessInformation"));
    }();
    return fn;
}

GetProcessInformationFn GetProcessInformationPtr() {
    static GetProcessInformationFn fn = []() -> GetProcessInformationFn {
        HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
        if (!kernel) return nullptr;
        return reinterpret_cast<GetProcessInformationFn>(
            GetProcAddress(kernel, "GetProcessInformation"));
    }();
    return fn;
}

bool QueryPowerState(HANDLE proc, DWORD* control, DWORD* state) {
    const GetProcessInformationFn fn = GetProcessInformationPtr();
    if (!fn) return false;
    PowerThrottlingState st{};
    st.Version = kPowerThrottlingVersion;
    if (!fn(proc, kProcessPowerThrottling, &st, sizeof(st))) return false;
    *control = st.ControlMask;
    *state = st.StateMask;
    return true;
}

bool SetPowerState(HANDLE proc, DWORD control, DWORD state) {
    const SetProcessInformationFn fn = SetProcessInformationPtr();
    if (!fn) return false;
    PowerThrottlingState st{};
    st.Version = kPowerThrottlingVersion;
    st.ControlMask = control;
    st.StateMask = state;
    return fn(proc, kProcessPowerThrottling, &st, sizeof(st)) != FALSE;
}

// 打开效率模式（EcoQoS）：保留调用前的 ControlMask/StateMask，恢复时
// 原样写回。返回 false 表示系统不支持或进程拒绝。
bool EnableEcoQoS(HANDLE proc, DWORD* prevControl, DWORD* prevState) {
    DWORD control = 0;
    DWORD state = 0;
    QueryPowerState(proc, &control, &state);  // 查询失败时按 0/0 处理
    *prevControl = control;
    *prevState = state;
    return SetPowerState(proc, control | kPowerThrottlingExecutionSpeed,
                         state | kPowerThrottlingExecutionSpeed);
}

bool RestorePowerState(HANDLE proc, DWORD control, DWORD state) {
    return SetPowerState(proc, control, state);
}

bool IsEcoQoSSupported() { return SetProcessInformationPtr() != nullptr; }

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
    // 没打开（进程已退出）没有可切换对象；只要还有一张可见且未最小化的
    // 窗口就保持正常；窗口全部最小化或纯托盘驻留时可以切效率模式。用户
    // 显式勾选「不对此应用启用效率模式」时永久豁免。
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
        bool ecoOn = false;
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
        initialized = IsEcoQoSSupported();
        return initialized;
    }

    static bool AnyTrackedAlive(const std::vector<TrackedProcess>& list) {
        for (const TrackedProcess& t : list) {
            if (TrackedAlive(t)) return true;
        }
        return false;
    }

    // 对一个进程启用 EcoQoS。成功时 out 记录恢复所需的原始状态。
    bool ApplyProcess(DWORD pid, ULONGLONG creation, TrackedProcess* out,
                      size_t* reusedPids, size_t* deniedPids,
                      bool* critical) {
        DWORD err = ERROR_SUCCESS;
        HANDLE proc = OpenVerified(
            pid, creation,
            PROCESS_SET_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION,
            &err);
        if (!proc) {
            if (err == ERROR_NOT_FOUND || err == ERROR_INVALID_PARAMETER) {
                if (reusedPids) ++*reusedPids;
            } else if (deniedPids) {
                ++*deniedPids;
            }
            return false;
        }
        if (IsProcessCritical(proc)) {
            if (critical) *critical = true;
            CloseHandle(proc);
            return false;
        }
        DWORD prevControl = 0;
        DWORD prevState = 0;
        const bool ok = EnableEcoQoS(proc, &prevControl, &prevState);
        CloseHandle(proc);
        if (!ok) {
            if (deniedPids) ++*deniedPids;
            return false;
        }
        out->pid = pid;
        out->creation = creation;
        out->prevControlMask = prevControl;
        out->prevStateMask = prevState;
        return true;
    }

    enum class RestoreOutcome { Restored, Gone, Failed };

    // 恢复一个进程的原始 PowerThrottling 状态。进程已退出视为 Gone；
    // 仍存活但恢复失败返回 Failed，调用方保留记录等待重试。
    static RestoreOutcome RestoreOne(const TrackedProcess& t) {
        DWORD err = ERROR_SUCCESS;
        HANDLE proc = OpenVerified(
            t.pid, t.creation,
            PROCESS_SET_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION,
            &err);
        if (!proc) {
            if (err == ERROR_NOT_FOUND || err == ERROR_INVALID_PARAMETER) {
                return RestoreOutcome::Gone;
            }
            return RestoreOutcome::Failed;
        }
        const bool ok =
            RestorePowerState(proc, t.prevControlMask, t.prevStateMask);
        CloseHandle(proc);
        return ok ? RestoreOutcome::Restored : RestoreOutcome::Failed;
    }

    static std::vector<TrackedProcess> Rollback(
        const std::vector<TrackedProcess>& tracked) {
        std::vector<TrackedProcess> stuck;
        for (const TrackedProcess& t : tracked) {
            if (RestoreOne(t) == RestoreOutcome::Failed) {
                stuck.push_back(t);
            }
        }
        return stuck;
    }

    std::vector<LimitResult> LimitMany(
        const std::vector<LimitRequest>& requests,
        const std::vector<std::wstring>& allAppKeys) {
        std::vector<LimitResult> results(requests.size());
        if (!initialized) {
            for (auto& r : results) {
                r.code = LimitCode::Failed;
                r.detail = L"EcoQoS/SetProcessInformation 不可用";
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
            if (it != apps.end() && it->second.ecoOn) {
                if (AnyTrackedAlive(it->second.processes)) {
                    results[i].code = LimitCode::AlreadyLimited;
                    continue;
                }
                // 跟踪的进程都退出了：没有需要恢复的进程，直接丢掉条目。
                apps.erase(it);
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
            const std::wstring key =
                NormalizePath(requests[requestIndex].appKey);
            const Selection sel = SelectAppProcesses(key, snapshot,
                                                    &foreignAppKeys);
            result.matched = sel.processes.size();
            result.detail = sel.note;

            if (sel.processes.empty()) {
                result.code = LimitCode::NoProcesses;
                continue;
            }

            // 最小年龄保护：只要有一个成员刚启动，就整组延后，避免把
            // 正在初始化的应用切到效率模式。
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
                result.detail += L"；进程刚启动，延后进入效率模式";
                continue;
            }

            AppEntry& entry = apps[key];  // 不存在则默认构造
            std::vector<TrackedProcess> tracked;
            size_t added = 0;
            size_t reused = 0;
            size_t denied = 0;
            size_t exactApplied = 0;
            bool critical = false;
            for (const ProcessRef& ref : sel.processes) {
                TrackedProcess t;
                if (!ApplyProcess(ref.pid, ref.creationTime, &t, &reused,
                                  &denied, &critical)) {
                    if (critical) break;
                    continue;
                }
                tracked.push_back(t);
                ++added;
                if (ref.exactMatch) ++exactApplied;
            }

            // 主进程一个都没进来：只把 helper 切到效率模式会形成“主进程
            // 全速、子进程低功耗”的半吊子状态，整组回滚，等下一轮重试。
            if (critical || exactApplied == 0) {
                result.code = critical
                                  ? LimitCode::AccessDenied
                                  : (denied > 0 ? LimitCode::AccessDenied
                                                : LimitCode::NoProcesses);
                result.limited = 0;
                result.skipped = sel.processes.size();
                result.detail += critical
                                     ? L"；含受保护关键进程，放弃本组"
                                     : (denied > 0
                                            ? L"；主进程无权限启用效率模式"
                                            : L"；主进程已退出");
                std::vector<TrackedProcess> stuck = Rollback(tracked);
                if (!stuck.empty()) {
                    entry.ecoOn = true;
                    entry.processes = std::move(stuck);
                } else {
                    apps.erase(key);
                }
                continue;
            }

            entry.ecoOn = true;
            entry.processes = std::move(tracked);
            result.code = LimitCode::Limited;
            result.limited = entry.processes.size();
            result.skipped = sel.processes.size() - result.limited;
            result.detail += L"；已对 " +
                             std::to_wstring(result.limited) +
                             L" 个进程启用效率模式（EcoQoS）";
            if (added > 0) {
                result.detail += L"，新启用 " + std::to_wstring(added);
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

    bool Restore(const std::wstring& appKey, std::wstring* detail) {
        const std::wstring key = NormalizePath(appKey);
        auto it = apps.find(key);
        if (it == apps.end() || !it->second.ecoOn) {
            if (detail) *detail = L"未处于效率模式";
            return true;
        }

        std::vector<TrackedProcess> remain;
        size_t restored = 0;
        size_t failed = 0;
        for (const TrackedProcess& t : it->second.processes) {
            switch (RestoreOne(t)) {
                case RestoreOutcome::Restored:
                    ++restored;
                    break;
                case RestoreOutcome::Gone:
                    break;
                case RestoreOutcome::Failed:
                    remain.push_back(t);
                    ++failed;
                    break;
            }
        }
        if (remain.empty()) {
            apps.erase(it);
        } else {
            it->second.processes = std::move(remain);
        }
        if (detail) {
            *detail = L"已退出效率模式 " + std::to_wstring(restored);
            if (failed > 0) {
                *detail += L"，仍失败 " + std::to_wstring(failed);
            }
        }
        return failed == 0;
    }

    void RestoreAll() {
        std::vector<std::wstring> keys;
        keys.reserve(apps.size());
        for (const auto& kv : apps) {
            if (kv.second.ecoOn) keys.push_back(kv.first);
        }
        for (const std::wstring& key : keys) {
            Restore(key, nullptr);
        }
    }

    bool IsLimited(const std::wstring& appKey) const {
        auto it = apps.find(NormalizePath(appKey));
        return it != apps.end() && it->second.ecoOn;
    }

    size_t TrackedProcessCount() const {
        size_t n = 0;
        for (const auto& kv : apps) {
            if (kv.second.ecoOn) n += kv.second.processes.size();
        }
        return n;
    }

    std::vector<std::wstring> LimitedKeys() const {
        std::vector<std::wstring> keys;
        keys.reserve(apps.size());
        for (const auto& kv : apps) {
            if (kv.second.ecoOn) keys.push_back(kv.first);
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
