// app_suspend.cpp - 见 app_suspend.h 的口径说明
//
// 这里做四件事：
//   1. 每应用一个 Job Object 做进程围栏：启动时/首次挂起时把主进程及其
//      后代追加进去，Windows 自动把未来子进程（无论落在哪个目录）也
//      放进 Job；父进程退出后 Job 成员仍可枚举；
//   2. 挂起集合 = Job 成员 ∪ 快照选择（兜底允许跨目录后代），过滤系统
//      进程、其它 Dock 应用、可见窗口、已由别的应用挂起的进程；
//   3. 两阶段整组挂起/恢复（ntdll!NtSuspendProcess / NtResumeProcess），
//      冻结后循环补冻 Job 新成员，并对 Job 追加失败的兜底成员做一次
//      快照收尾补冻；已挂起应用不再周期补挂/轮询；
//   4. 用 PID + 创建时间双保险，并把挂起表持久化，供异常退出后恢复。
//
// 不按进程树整棵挂起、不按 exe 名字合并、不碰系统基础设施，原因见头文件。

#include "app_suspend.h"

#include <dwmapi.h>
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

// 挂起表的持久化格式。沿用旧版「挂起未聚焦应用」的值名与布局
// （magic "SSUP" + version 1），这样历史上残留的挂起表也能在启动时恢复。
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

bool QueryProcessTimes(HANDLE proc, ULONGLONG* created, ULONGLONG* exited) {
    FILETIME c{}, e{}, kernel{}, user{};
    if (!GetProcessTimes(proc, &c, &e, &kernel, &user)) {
        return false;
    }
    if (created) *created = FileTimeToU64(c);
    if (exited) *exited = FileTimeToU64(e);
    return true;
}

bool QueryCreationTime(HANDLE proc, ULONGLONG* out) {
    // 已终止的进程句柄仍可能打得开（例如被挂起后又被更新器替换掉的
    // 残留进程），但它不再是可挂起/可恢复的对象。
    ULONGLONG exited = 0;
    return QueryProcessTimes(proc, out, &exited) && exited == 0;
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
    ULONGLONG exited = 0;
    if (!QueryProcessTimes(proc, &actual, &exited)) {
        if (lastError) *lastError = GetLastError();
        CloseHandle(proc);
        return nullptr;
    }
    if (actual != creation || exited != 0) {
        // 两种情况都必须让调用方丢弃记录，否则会永久卡死交互：
        //   1) 原进程已退出、PID 被另一个进程复用 —— 绝不能碰新进程；
        //   2) 原进程已终止但句柄还开着（典型：挂起期间被更新器替换，
        //      或中断在退出中途）—— NtResumeProcess 必然失败，若保留
        //      记录，每次交互都会"恢复失败"并取消切换。
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

// 该进程是否拥有一张“用户此刻看得到”的顶层窗口。用于两件事：
//   1) 追加 Job 时，不把应用顺手拉起的浏览器/播放器/终端等外部程序
//      及其 helper 圈进来；
//   2) 冻结前复查 Job 成员，发现成员里冒出了可见窗口（例如更新器弹窗、
//      被拉起的另一个应用），整组放弃，绝不让用户面前正在显示的窗口卡住。
// 只看 IsWindowVisible + !IsIconic；被 DWM cloak 的 UWP 窗口也按“可见”
// 处理，宁可少挂一个应用，也不误冻桌面。
bool IsCloakedWindow(HWND hwnd) {
    DWORD cloaked = 0;
    return SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked,
                                           sizeof(cloaked))) &&
           cloaked != 0;
}

struct VisibleWindowSearch {
    DWORD pid = 0;
    bool found = false;
};

BOOL CALLBACK FindVisibleWindowProc(HWND hwnd, LPARAM param) {
    auto* search = reinterpret_cast<VisibleWindowSearch*>(param);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != search->pid) return TRUE;
    if (!IsWindowVisible(hwnd) || IsIconic(hwnd) || IsCloakedWindow(hwnd)) {
        return TRUE;
    }
    RECT rect{};
    if (!GetWindowRect(hwnd, &rect)) return TRUE;
    if (rect.right <= rect.left || rect.bottom <= rect.top) return TRUE;
    search->found = true;
    return FALSE;
}

bool ProcessOwnsVisibleWindow(DWORD pid) {
    if (pid == 0) return false;
    VisibleWindowSearch search;
    search.pid = pid;
    EnumWindows(FindVisibleWindowProc, reinterpret_cast<LPARAM>(&search));
    return search.found;
}

// 一次枚举所有“可见且未最小化”的顶层窗口，返回 PID 集合。避免为每个
// 候选进程各 EnumWindows 一遍（多应用 × 多 helper 时会很贵）。
std::unordered_set<DWORD> VisibleWindowPids() {
    std::unordered_set<DWORD> pids;
    EnumWindows(
        [](HWND hwnd, LPARAM param) -> BOOL {
            auto* out = reinterpret_cast<std::unordered_set<DWORD>*>(param);
            if (!IsWindowVisible(hwnd) || IsIconic(hwnd) ||
                IsCloakedWindow(hwnd)) {
                return TRUE;
            }
            RECT rect{};
            if (!GetWindowRect(hwnd, &rect)) return TRUE;
            if (rect.right <= rect.left || rect.bottom <= rect.top) {
                return TRUE;
            }
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            if (pid != 0) out->insert(pid);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&pids));
    return pids;
}

std::vector<DWORD> QueryJobProcessIds(HANDLE job) {
    std::vector<DWORD> pids;
    if (!job) return pids;
    DWORD capacity = 256;
    for (int attempt = 0; attempt < 4; ++attempt) {
        std::vector<BYTE> buffer(
            sizeof(JOBOBJECT_BASIC_PROCESS_ID_LIST) +
            static_cast<size_t>(capacity) * sizeof(ULONG_PTR));
        auto* list =
            reinterpret_cast<JOBOBJECT_BASIC_PROCESS_ID_LIST*>(buffer.data());
        list->NumberOfAssignedProcesses = capacity;
        list->NumberOfProcessIdsInList = 0;
        DWORD needed = 0;
        if (!QueryInformationJobObject(job, JobObjectBasicProcessIdList, list,
                                       static_cast<DWORD>(buffer.size()),
                                       &needed)) {
            return pids;
        }
        const DWORD count = list->NumberOfProcessIdsInList;
        if (count < capacity) {
            pids.reserve(count);
            for (DWORD i = 0; i < count; ++i) {
                pids.push_back(
                    static_cast<DWORD>(list->ProcessIdList[i]));
            }
            return pids;
        }
        capacity *= 4;
    }
    return pids;
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

bool ShouldPauseApp(bool opened, bool hasVisibleWindow, bool userExcluded) {
    // 没打开（进程已退出）没有可暂停对象；只要还有一张可见且未最小化的
    // 窗口就保持运行；窗口全部最小化或纯托盘驻留时可以暂停。用户显式
    // 勾选「不暂停此应用」时永久豁免，即使之后收起窗口变成托盘应用。
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
            ULONGLONG exited = 0;
            const bool imageOk =
                QueryFullProcessImageNameW(proc, 0, image, &imageLen) != FALSE &&
                imageLen > 0;
            const bool timesOk = QueryProcessTimes(proc, &created, &exited);
            CloseHandle(proc);
            if (!imageOk) continue;
            // 已终止但句柄仍存在的残留进程（挂起期间被更新器替换等）没有
            // 可挂起对象，直接跳过 —— 留在快照里会因为创建时间为 0 让整组
            // 挂起被判定为"无法核对"而放弃。
            if (!timesOk || exited != 0) continue;

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
    const std::unordered_set<std::wstring>* foreignAppKeys,
    bool includeExternalDescendants) {
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
            // 默认只收安装目录树内的 helper；Job 沙箱路径或兜底补挂时
            // 允许跨目录后代（应用把 helper.exe 落到 %TEMP% / AppData 再
            // 拉起的场景）。外部程序仍由调用方用可见窗口过滤。
            if (!sameDir && !inside && !includeExternalDescendants) continue;
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
               sel.installRoot + (sel.broadRoot ? L"，宽泛目录" : L"") +
               (includeExternalDescendants ? L"，含跨目录后代" : L"") + L"）";
    return sel;
}

struct Suspender::Impl {
    bool persist = true;
    bool useJobSandbox = true;
    ULONGLONG minProcessAgeMs = kMinProcessAgeDefaultMs;
    std::wstring stateValueName = L"SuspendedState";
    bool initialized = false;
    NtProcessControlFn suspendFn = nullptr;
    NtProcessControlFn resumeFn = nullptr;

    std::unordered_map<std::wstring, std::vector<TrackedProcess>> apps;
    std::unordered_map<ProcKey, std::wstring, ProcKeyHash> owner;
    // 每个应用一个 Job。Job 只做进程归属围栏：不设 UI 限制、不设
    // breakaway、不设 kill-on-close，因此不会改变应用的行为，也不会在
    // Dock 退出时杀进程；Job 句柄关闭后进程继续运行。
    std::unordered_map<std::wstring, HANDLE> jobs;

    Impl(bool persistState, ULONGLONG minAge, std::wstring valueName,
         bool jobSandbox)
        : persist(persistState),
          useJobSandbox(jobSandbox),
          minProcessAgeMs(minAge),
          stateValueName(std::move(valueName)) {}

    ~Impl() {
        for (auto& kv : jobs) {
            if (kv.second) CloseHandle(kv.second);
        }
        jobs.clear();
    }

    HANDLE EnsureJob(const std::wstring& key) {
        if (!useJobSandbox) return nullptr;
        auto it = jobs.find(key);
        if (it != jobs.end()) return it->second;
        HANDLE job = CreateJobObjectW(nullptr, nullptr);
        if (!job) return nullptr;
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
        // LimitFlags = 0：不允许主动 breakaway、不 kill-on-close、无 UI
        // 限制。UI 限制会阻止 Chromium/Electron 自己再嵌套 sandbox Job。
        info.BasicLimitInformation.LimitFlags = 0;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info,
                                sizeof(info));
        jobs.emplace(key, job);
        return job;
    }

    // 把快照里属于该应用的进程（含跨目录后代）追加进 Job。已在该 Job
    // 里的进程跳过；受保护/无权限的进程只记录失败，不阻断其它成员 ——
    // Job 是增强路径，失败时 SuspendMany 仍有快照兜底。
    size_t AdoptIntoJob(const std::wstring& key, HANDLE job,
                        const std::vector<ProcessEntry>& snapshot,
                        const std::unordered_set<std::wstring>& foreignKeys,
                        const std::unordered_set<DWORD>& visiblePids,
                        size_t* failed) {
        if (!job) return 0;
        const Selection adopt = SelectAppProcesses(
            key, snapshot, &foreignKeys, /*includeExternalDescendants=*/true);
        if (adopt.processes.empty()) return 0;

        std::unordered_map<DWORD, const ProcessEntry*> byPid;
        byPid.reserve(snapshot.size());
        for (const ProcessEntry& e : snapshot) byPid[e.pid] = &e;

        // 父进程创建时间一定早于子进程（SelectAppProcesses 已校验），
        // 按创建时间排序就是“先父后子”，保证追加过程中新创建的子进程
        // 也能继承 Job。
        std::vector<ProcessRef> ordered = adopt.processes;
        std::sort(ordered.begin(), ordered.end(),
                  [&byPid](const ProcessRef& a, const ProcessRef& b) {
                      const auto ia = byPid.find(a.pid);
                      const auto ib = byPid.find(b.pid);
                      const ULONGLONG ta =
                          ia == byPid.end() ? 0 : ia->second->creationTime;
                      const ULONGLONG tb =
                          ib == byPid.end() ? 0 : ib->second->creationTime;
                      if (ta != tb) return ta < tb;
                      return a.pid < b.pid;
                  });

        // 可见窗口的外部程序及其后代不追加：那是用户正在看的另一个
        // 应用，不属于本应用的后台 helper。
        std::unordered_set<DWORD> excluded;
        size_t adopted = 0;
        for (const ProcessRef& ref : ordered) {
            auto entryIt = byPid.find(ref.pid);
            if (entryIt == byPid.end()) continue;
            const ProcessEntry& entry = *entryIt->second;
            const bool isRoot = entry.imagePath == key;
            if (!isRoot) {
                if (excluded.count(entry.parentPid) != 0) {
                    excluded.insert(ref.pid);
                    continue;
                }
                if (visiblePids.count(ref.pid) != 0) {
                    excluded.insert(ref.pid);
                    continue;
                }
            }
            auto owned = owner.find(ProcKey{ref.pid, ref.creationTime});
            if (owned != owner.end() && owned->second != key) {
                excluded.insert(ref.pid);
                continue;
            }

            // 先用最小权限查一次是否已在 Job：已经在 Job 里的成员占多数，
            // 避免对每个 helper 都请求 PROCESS_SET_QUOTA。
            HANDLE probe = OpenVerified(
                ref.pid, ref.creationTime,
                PROCESS_QUERY_LIMITED_INFORMATION, nullptr);
            if (!probe) {
                if (failed) ++*failed;
                continue;
            }
            BOOL already = FALSE;
            const BOOL inJob = IsProcessInJob(probe, job, &already);
            CloseHandle(probe);
            if (inJob && already) continue;

            DWORD err = ERROR_SUCCESS;
            HANDLE proc = OpenVerified(
                ref.pid, ref.creationTime,
                PROCESS_SET_QUOTA | PROCESS_TERMINATE |
                    PROCESS_QUERY_LIMITED_INFORMATION,
                &err);
            if (!proc) {
                if (failed) ++*failed;
                continue;
            }
            if (!AssignProcessToJobObject(job, proc)) {
                if (failed) ++*failed;
            } else {
                ++adopted;
            }
            CloseHandle(proc);
        }
        return adopted;
    }

    // Job 当前成员（含子 Job），转换成 PID + 创建时间。快照里没有的
    // 新进程现场补查创建时间；查不到就丢弃，避免 PID 复用。
    std::vector<ProcessRef> JobMembers(const std::wstring& key,
                                       size_t* unreadable = nullptr) {
        std::vector<ProcessRef> members;
        if (!useJobSandbox) return members;
        auto it = jobs.find(key);
        if (it == jobs.end() || !it->second) return members;
        const std::vector<DWORD> pids = QueryJobProcessIds(it->second);
        members.reserve(pids.size());
        for (DWORD pid : pids) {
            if (pid == 0 || pid == GetCurrentProcessId()) continue;
            HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                      pid);
            if (!proc) {
                // Job 列表里可能还残留刚退出的 PID；这不是“无权限”，
                // 不能因此让整组挂起失败。只有真正的拒绝访问才计入
                // unreadable。
                const DWORD err = GetLastError();
                if (err != ERROR_INVALID_PARAMETER &&
                    err != ERROR_NOT_FOUND) {
                    if (unreadable) ++*unreadable;
                }
                continue;
            }
            // Job 列表理论上不会把已退出/复用的 PID 留给我们，但这里
            // 再做一次 IsProcessInJob 核对，挡住极端 PID 复用。
            BOOL belongs = FALSE;
            if (!IsProcessInJob(proc, it->second, &belongs) || !belongs) {
                CloseHandle(proc);
                continue;
            }
            ULONGLONG created = 0;
            const bool ok = QueryCreationTime(proc, &created);
            const DWORD timeError = GetLastError();
            CloseHandle(proc);
            // 句柄已拿到但查询失败：多半是进程正在退出（残留句柄），
            // 跳过；只有明确的拒绝访问才让整组放弃。
            if (!ok || created == 0) {
                if (!ok && timeError == ERROR_ACCESS_DENIED) {
                    if (unreadable) ++*unreadable;
                }
                continue;
            }
            members.push_back(ProcessRef{pid, created});
        }
        return members;
    }

    bool AdoptProcess(const std::wstring& appKey, HANDLE process,
                      std::wstring* detail) {
        if (!process) {
            if (detail) *detail = L"空进程句柄";
            return false;
        }
        const std::wstring key = NormalizePath(appKey);
        if (key.empty()) {
            if (detail) *detail = L"空应用 key";
            return false;
        }
        // 绝不能把系统基础设施/本套件自己塞进 Job；调用方传错 key 时
        // 这里再兜一层。
        if (IsNeverSuspendImage(key)) {
            if (detail) *detail = L"该镜像永不挂起，拒绝加入 Job";
            return false;
        }
        HANDLE job = EnsureJob(key);
        if (!job) {
            if (detail) *detail = L"Job 不可用";
            return false;
        }
        BOOL already = FALSE;
        if (IsProcessInJob(process, job, &already) && already) {
            if (detail) *detail = L"已在 Job 中";
            return true;
        }
        if (!AssignProcessToJobObject(job, process)) {
            if (detail) {
                *detail = L"AssignProcessToJobObject 失败：" +
                           std::to_wstring(GetLastError());
            }
            return false;
        }
        if (detail) *detail = L"已加入 Job";
        return true;
    }

    struct CandidateSet {
        std::vector<ProcessRef> refs;
        std::unordered_set<DWORD> blockedPids;
        size_t skipped = 0;
        size_t ownedByOther = 0;
        size_t unreadable = 0;
        bool visibleWindow = false;
        std::wstring note;
    };

    // 汇总“本次要冻结的进程”：Job 成员（若有）∪ 快照选择（含跨目录
    // 后代），再过滤永不挂起、其它 Dock 应用的整棵子树、已由别的应用
    // 挂起的进程，以及拥有可见窗口的外部程序。
    CandidateSet BuildCandidates(
        const std::wstring& key,
        const std::vector<ProcessEntry>& snapshot,
        const std::unordered_set<std::wstring>& foreignKeys,
        const std::unordered_set<DWORD>& visiblePids) {
        CandidateSet out;
        std::unordered_map<DWORD, const ProcessEntry*> byPid;
        byPid.reserve(snapshot.size());
        for (const ProcessEntry& e : snapshot) byPid[e.pid] = &e;

        // 其它 Dock 应用及其后代整棵排除：即使 Job 因继承把它们带进来，
        // 也不应该由当前应用冻结（Office 的 WINWORD 拉起 EXCEL 等）。
        std::unordered_set<DWORD> blocked;
        std::unordered_map<DWORD, std::vector<DWORD>> children;
        for (const ProcessEntry& e : snapshot) {
            children[e.parentPid].push_back(e.pid);
            if (e.imagePath != key && foreignKeys.count(e.imagePath) != 0) {
                blocked.insert(e.pid);
            }
        }
        if (!blocked.empty()) {
            std::queue<DWORD> pending;
            for (DWORD pid : blocked) pending.push(pid);
            while (!pending.empty()) {
                const DWORD pid = pending.front();
                pending.pop();
                auto it = children.find(pid);
                if (it == children.end()) continue;
                for (DWORD child : it->second) {
                    if (blocked.insert(child).second) pending.push(child);
                }
            }
        }

        const Selection sel = SelectAppProcesses(
            key, snapshot, &foreignKeys, /*includeExternalDescendants=*/true);

        const std::vector<ProcessRef> jobMembers =
            JobMembers(key, &out.unreadable);
        std::vector<ProcessRef> merged;
        merged.reserve(sel.processes.size() + jobMembers.size());
        std::unordered_set<ProcKey, ProcKeyHash> seen;
        auto addRef = [&merged, &seen](const ProcessRef& ref) {
            if (ref.pid == 0 || ref.creationTime == 0) return;
            if (seen.insert(ProcKey{ref.pid, ref.creationTime}).second) {
                merged.push_back(ref);
            }
        };
        for (const ProcessRef& ref : jobMembers) addRef(ref);
        for (const ProcessRef& ref : sel.processes) addRef(ref);

        for (const ProcessRef& ref : merged) {
            const ProcessEntry* entry = nullptr;
            auto byPidIt = byPid.find(ref.pid);
            if (byPidIt != byPid.end()) entry = byPidIt->second;

            // Job 里可能有快照之后才创建的新进程：现场查镜像路径。
            ProcessEntry fetched;
            if (!entry) {
                HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,
                                          FALSE, ref.pid);
                if (!proc) {
                    ++out.skipped;
                    continue;
                }
                wchar_t image[32768] = {};
                DWORD len = 32760;
                if (QueryFullProcessImageNameW(proc, 0, image, &len) &&
                    len > 0) {
                    fetched.pid = ref.pid;
                    fetched.creationTime = ref.creationTime;
                    fetched.imagePath = NormalizePath(image);
                }
                CloseHandle(proc);
                if (fetched.imagePath.empty()) {
                    ++out.skipped;
                    continue;
                }
                entry = &fetched;
            }

            if (IsNeverSuspendImage(entry->imagePath)) {
                ++out.skipped;
                continue;
            }
            // 快照之后才创建、因 Job 继承而带进来的另一个 Dock 应用：
            // 同样不能由当前应用冻结。
            if (entry->imagePath != key &&
                (blocked.count(ref.pid) != 0 ||
                 foreignKeys.count(entry->imagePath) != 0)) {
                ++out.skipped;
                continue;
            }
            auto owned = owner.find(ProcKey{ref.pid, ref.creationTime});
            if (owned != owner.end()) {
                if (owned->second != key) ++out.ownedByOther;
                ++out.skipped;
                continue;
            }
            if (visiblePids.count(ref.pid) != 0) {
                out.visibleWindow = true;
                out.note = L"成员 pid " + std::to_wstring(ref.pid) +
                           L" 仍有可见且未最小化的窗口";
                return out;
            }
            out.refs.push_back(ref);
        }
        out.blockedPids = std::move(blocked);
        out.note = sel.note + L"；Job 成员 " +
                   std::to_wstring(jobMembers.size()) + L" -> 待挂起 " +
                   std::to_wstring(out.refs.size());
        if (out.unreadable > 0) {
            out.note += L"；Job 中 " + std::to_wstring(out.unreadable) +
                        L" 个进程无法核对（保持整组一致，放弃挂起）";
        }
        return out;
    }

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
                r.detail = L"NtSuspendProcess/NtResumeProcess 不可用";
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
                    // 已经在挂起态：不再做任何快照/补挂。同一 Job 内
                    // 后续创建的子进程不可能发生 —— 能创建进程的成员都
                    // 已经冻结；竞态窗口由 SuspendMany 收尾阶段的补冻覆盖。
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

        std::unordered_set<DWORD> visiblePids = VisibleWindowPids();

        FILETIME nowFt{};
        GetSystemTimeAsFileTime(&nowFt);
        const ULONGLONG now = FileTimeToU64(nowFt);
        const ULONGLONG minAge100ns = minProcessAgeMs * 10000ull;

        for (size_t requestIndex : todo) {
            SuspendResult& result = results[requestIndex];
            const std::wstring key =
                NormalizePath(requests[requestIndex].appKey);
            HANDLE job = EnsureJob(key);
            size_t adoptFailed = 0;
            const size_t adopted = AdoptIntoJob(
                key, job, snapshot, foreignAppKeys, visiblePids,
                &adoptFailed);
            // 追加 Job 期间可能有 helper 刚显示窗口；冻结前重新取一次，
            // 避免把用户眼前的新窗口一起冻住。
            visiblePids = VisibleWindowPids();
            CandidateSet candidates = BuildCandidates(
                key, snapshot, foreignAppKeys, visiblePids);
            result.jobSandbox = job != nullptr;
            result.jobMembers =
                job ? static_cast<size_t>(QueryJobProcessIds(job).size()) : 0;
            result.matched = candidates.refs.size();
            result.detail = candidates.note;
            if (job) {
                result.detail += L"；Job 追加 " + std::to_wstring(adopted);
                if (adoptFailed > 0) {
                    result.detail += L"（失败 " +
                                     std::to_wstring(adoptFailed) + L"）";
                }
            }
            if (candidates.visibleWindow) {
                result.code = SuspendCode::VisibleWindow;
                result.detail += L"；可见窗口成员，放弃本次挂起";
                continue;
            }
            if (candidates.unreadable > 0) {
                result.code = SuspendCode::AccessDenied;
                result.detail += L"；Job 成员无法核对，放弃本次挂起";
                continue;
            }
            if (candidates.refs.empty()) {
                result.code = SuspendCode::NoProcesses;
                continue;
            }

            // 最小年龄保护：只要有一个成员刚启动，就整组延后，避免
            // 主进程被冻住、辅助进程还没初始化完的半启动状态。
            ULONGLONG maxRetry100ns = 0;
            bool tooYoung = false;
            for (const ProcessRef& ref : candidates.refs) {
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
            std::vector<TrackedProcess> tracked;
            std::vector<HANDLE> handles;
            size_t accessDenied = 0;
            size_t critical = 0;
            size_t reused = 0;
            size_t ownedByOther = 0;
            bool aborted = false;

            for (const ProcessRef& ref : candidates.refs) {
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
                tracked.push_back(
                    TrackedProcess{ref.pid, ref.creationTime});
                handles.push_back(proc);
            }

            if (aborted) {
                for (HANDLE h : handles) CloseHandle(h);
                result.code = SuspendCode::AccessDenied;
                result.skipped = candidates.refs.size();
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

            // 关掉“枚举之后、冻结生效之前”的窗口：这段时间父进程可能
            // 刚创建了子进程，它已经继承 Job，但不在上面的 handles 里。
            // 全部成员冻结后不会再有新进程；循环补冻直到没有新成员。
            std::unordered_set<ProcKey, ProcKeyHash> seen;
            for (const TrackedProcess& t : tracked) {
                seen.insert(ProcKey{t.pid, t.creation});
            }
            bool quiesceFailed = false;
            bool quiesceVisible = false;
            for (int pass = 0; pass < 8 && !suspendFailed &&
                                !quiesceFailed;
                 ++pass) {
                bool added = false;
                size_t unreadableNow = 0;
                const std::vector<ProcessRef> members =
                    JobMembers(key, &unreadableNow);
                if (unreadableNow > 0) {
                    quiesceFailed = true;
                    break;
                }
                for (const ProcessRef& ref : members) {
                    const ProcKey pk{ref.pid, ref.creationTime};
                    if (!seen.insert(pk).second) continue;
                    if (owner.count(pk) != 0) continue;
                    if (candidates.blockedPids.count(ref.pid) != 0) continue;

                    std::wstring image;
                    for (const ProcessEntry& e : snapshot) {
                        if (e.pid == ref.pid) {
                            image = e.imagePath;
                            break;
                        }
                    }
                    if (image.empty()) {
                        HANDLE probe = OpenProcess(
                            PROCESS_QUERY_LIMITED_INFORMATION, FALSE, ref.pid);
                        if (probe) {
                            wchar_t path[32768] = {};
                            DWORD len = 32760;
                            if (QueryFullProcessImageNameW(probe, 0, path,
                                                           &len) &&
                                len > 0) {
                                image = NormalizePath(path);
                            }
                            CloseHandle(probe);
                        }
                    }
                    if (image.empty()) {
                        // 查不到镜像就无法判断是不是 helper/系统进程；
                        // 为保证整组一致，回滚本次挂起而不是跳过。
                        quiesceFailed = true;
                        break;
                    }
                    if (IsNeverSuspendImage(image)) continue;
                    if (image != key &&
                        foreignAppKeys.count(image) != 0) {
                        continue;
                    }
                    if (ProcessOwnsVisibleWindow(ref.pid)) {
                        quiesceVisible = true;
                        quiesceFailed = true;
                        break;
                    }
                    DWORD err = ERROR_SUCCESS;
                    HANDLE proc = OpenVerified(
                        ref.pid, ref.creationTime,
                        PROCESS_SUSPEND_RESUME |
                            PROCESS_QUERY_LIMITED_INFORMATION,
                        &err);
                    if (!proc) {
                        quiesceFailed = true;
                        break;
                    }
                    if (!NtSuccess(suspendFn(proc))) {
                        CloseHandle(proc);
                        quiesceFailed = true;
                        break;
                    }
                    handles.push_back(proc);
                    tracked.push_back(
                        TrackedProcess{ref.pid, ref.creationTime});
                    ++suspended;
                    added = true;
                }
                if (!added) break;
            }

            // Job 追加失败的兜底成员不在 Job 里，Job 成员枚举看不到它们
            // 在冻结边界刚创建的子进程。这里在冻结后再做一次（只做一次，
            // 不是轮询）完整快照选择：所有已冻结的父进程都不可能再创建
            // 新进程，因此补完即收敛。Job 正常时这一轮通常没有新增。
            for (int pass = 0; pass < 4 && !suspendFailed &&
                                !quiesceFailed;
                 ++pass) {
                bool added = false;
                const std::vector<ProcessEntry> verifySnapshot =
                    SnapshotProcesses();
                if (verifySnapshot.empty()) break;
                std::unordered_map<DWORD, const ProcessEntry*> byPid;
                byPid.reserve(verifySnapshot.size());
                for (const ProcessEntry& e : verifySnapshot) {
                    byPid[e.pid] = &e;
                }
                const Selection verify = SelectAppProcesses(
                    key, verifySnapshot, &foreignAppKeys,
                    /*includeExternalDescendants=*/true);
                for (const ProcessRef& ref : verify.processes) {
                    const ProcKey pk{ref.pid, ref.creationTime};
                    if (!seen.insert(pk).second) continue;
                    if (owner.count(pk) != 0) continue;
                    auto byPidIt = byPid.find(ref.pid);
                    if (byPidIt == byPid.end()) continue;
                    const ProcessEntry& entry = *byPidIt->second;
                    if (IsNeverSuspendImage(entry.imagePath)) continue;
                    if (entry.imagePath != key &&
                        foreignAppKeys.count(entry.imagePath) != 0) {
                        continue;
                    }
                    if (ProcessOwnsVisibleWindow(ref.pid)) {
                        quiesceVisible = true;
                        quiesceFailed = true;
                        break;
                    }
                    DWORD err = ERROR_SUCCESS;
                    HANDLE proc = OpenVerified(
                        ref.pid, ref.creationTime,
                        PROCESS_SUSPEND_RESUME |
                            PROCESS_QUERY_LIMITED_INFORMATION,
                        &err);
                    if (!proc) {
                        if (err == ERROR_NOT_FOUND ||
                            err == ERROR_INVALID_PARAMETER) {
                            continue;  // 快照后刚退出
                        }
                        quiesceFailed = true;
                        break;
                    }
                    if (!NtSuccess(suspendFn(proc))) {
                        CloseHandle(proc);
                        quiesceFailed = true;
                        break;
                    }
                    handles.push_back(proc);
                    tracked.push_back(
                        TrackedProcess{ref.pid, ref.creationTime});
                    ++suspended;
                    added = true;
                }
                if (!added) break;
            }

            if (suspendFailed || quiesceFailed) {
                // 回滚已成功挂起的那部分，避免应用处于半冻结状态。
                // 只恢复本次确实挂起过的前 suspended 个句柄，绝不误碰
                // 调用前就被别的组件挂起的进程。
                // 若回滚本身也失败，必须把失败项留在跟踪表里，退出时
                // ResumeAll 还能再试，绝不能让进程无声地停在挂起态。
                std::vector<TrackedProcess> stuck;
                for (size_t i = 0; i < suspended; ++i) {
                    if (!NtSuccess(resumeFn(handles[i]))) {
                        stuck.push_back(tracked[i]);
                    }
                }
                for (HANDLE h : handles) CloseHandle(h);
                result.code = quiesceVisible ? SuspendCode::VisibleWindow
                                             : SuspendCode::Failed;
                result.skipped = candidates.refs.size();
                result.detail += quiesceVisible
                                     ? L"；补冻阶段发现新成员有可见窗口，"
                                       L"已回滚"
                                     : L"；挂起中途失败";
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
            for (const TrackedProcess& t : tracked) {
                owner[ProcKey{t.pid, t.creation}] = key;
            }
            apps[key] = std::move(tracked);
            stateChanged = true;
            result.code = SuspendCode::Suspended;
            result.suspended = suspended;
            result.skipped = candidates.refs.size() - suspended;
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

    // 挂起表持久化：Dock 被强杀/崩溃时，进程仍停在挂起态，下次启动
    // RecoverPersisted 按 PID+创建时间核对后恢复。空表直接删除值。
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
        // 只读打开：恢复只需要读挂起表；删除值另走 RegDeleteKeyValueW。
        // 这样即使当前令牌没有写权限（受限/只读配置），也还能恢复进程。
        if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegPath, 0, KEY_READ, &key) !=
            ERROR_SUCCESS) {
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
                     std::wstring stateValueName, bool useJobSandbox)
    : impl_(std::make_unique<Impl>(persist, minProcessAgeMs,
                                   std::move(stateValueName),
                                   useJobSandbox)) {}

Suspender::~Suspender() {
    if (impl_) impl_->ResumeAll();
}

bool Suspender::Initialize() { return impl_->Initialize(); }

std::vector<SuspendResult> Suspender::SuspendMany(
    const std::vector<SuspendRequest>& requests,
    const std::vector<std::wstring>& allAppKeys) {
    return impl_->SuspendMany(requests, allAppKeys);
}

bool Suspender::AdoptProcess(const std::wstring& appKey, HANDLE process,
                             std::wstring* detail) {
    return impl_->AdoptProcess(appKey, process, detail);
}

size_t Suspender::JobMemberCount(const std::wstring& appKey) const {
    if (!impl_) return 0;
    auto it = impl_->jobs.find(NormalizePath(appKey));
    if (it == impl_->jobs.end() || !it->second) return 0;
    return QueryJobProcessIds(it->second).size();
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
