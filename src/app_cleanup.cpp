// app_cleanup.cpp - 见 app_cleanup.h
//
// 关键点：
//   - 归因只用「精确 key / Job 成员 / 后代 / 窄安装根 / 服务宿主与
//     服务后代 / 产品家族服务」这些可核对证据；
//   - 执行时先把候选放进瞬时 Job，再 TerminateJobObject 一次；不循环杀；
//   - 服务拒绝停止就报 NeedsElevation，不静默、不用轮询绕过；
//   - 本模块不依赖 Dock 的 UIA：托盘由调用方注入 TrayProbe。

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#include "app_cleanup.h"

#include <dwmapi.h>
#include <tlhelp32.h>
#include <winsvc.h>

#include <algorithm>
#include <cwctype>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace appcleanup {
namespace {

constexpr DWORD kServiceStopTimeoutMs = 5000;

std::wstring ToLower(std::wstring s) {
    for (wchar_t& ch : s) ch = static_cast<wchar_t>(towlower(ch));
    return s;
}

std::wstring TrimTrailingSlash(std::wstring path) {
    while (path.size() > 3 && path.back() == L'\\') path.pop_back();
    return path;
}

bool IsInsideDir(const std::wstring& path, const std::wstring& dir) {
    if (path.size() <= dir.size() + 1) return false;
    if (_wcsnicmp(path.c_str(), dir.c_str(), dir.size()) != 0) return false;
    return path[dir.size()] == L'\\';
}

bool SameOrInside(const std::wstring& path, const std::wstring& dir) {
    return path == dir || IsInsideDir(path, dir);
}

DWORD CurrentSessionId() {
    DWORD session = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &session);
    return session;
}

bool IsCloaked(HWND hwnd) {
    DWORD cloaked = 0;
    return SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked,
                                           sizeof(cloaked))) &&
           cloaked != 0;
}

BOOL CALLBACK EnumUserWindowProc(HWND hwnd, LPARAM param) {
    auto* out = reinterpret_cast<std::vector<DWORD>*>(param);
    if (!out || !IsWindow(hwnd)) return TRUE;
    const bool visible = IsWindowVisible(hwnd) != FALSE;
    const bool cloaked = IsCloaked(hwnd);
    if (!visible && !cloaked) return TRUE;
    if (GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE;
    const LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if ((ex & WS_EX_TOOLWINDOW) != 0 && (ex & WS_EX_APPWINDOW) == 0) {
        return TRUE;
    }
    RECT rc{};
    if (!GetWindowRect(hwnd, &rc)) return TRUE;
    if (rc.right <= rc.left || rc.bottom <= rc.top) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != 0 && pid != GetCurrentProcessId()) out->push_back(pid);
    return TRUE;
}

bool QueryCreationTime(HANDLE proc, ULONGLONG* out) {
    FILETIME c{}, e{}, k{}, u{};
    if (!GetProcessTimes(proc, &c, &e, &k, &u)) return false;
    if (out) {
        *out = (static_cast<ULONGLONG>(c.dwHighDateTime) << 32) |
               c.dwLowDateTime;
    }
    FILETIME zero{};
    return (e.dwHighDateTime == zero.dwHighDateTime &&
            e.dwLowDateTime == zero.dwLowDateTime);
}

bool IsProcessCritical(HANDLE proc) {
    using Fn = BOOL(WINAPI*)(HANDLE, PBOOL);
    static Fn fn = [] {
        HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
        if (!kernel) return static_cast<Fn>(nullptr);
        return reinterpret_cast<Fn>(GetProcAddress(kernel, "IsProcessCritical"));
    }();
    if (!fn) return false;
    BOOL critical = FALSE;
    if (!fn(proc, &critical)) return false;
    return critical != FALSE;
}

HANDLE OpenVerified(DWORD pid, ULONGLONG creation, DWORD access,
                    DWORD* err) {
    if (pid == 0 || creation == 0) {
        if (err) *err = ERROR_INVALID_PARAMETER;
        return nullptr;
    }
    HANDLE proc = OpenProcess(access, FALSE, pid);
    if (!proc) {
        if (err) *err = GetLastError();
        return nullptr;
    }
    ULONGLONG actual = 0;
    if (!QueryCreationTime(proc, &actual) || actual != creation) {
        if (err) *err = ERROR_NOT_FOUND;
        CloseHandle(proc);
        return nullptr;
    }
    if (err) *err = ERROR_SUCCESS;
    return proc;
}

std::wstring ImagePathOf(HANDLE proc) {
    wchar_t image[32768] = {};
    DWORD len = 32760;
    if (!QueryFullProcessImageNameW(proc, 0, image, &len) || len == 0) {
        return L"";
    }
    return appsuspend::NormalizePath(image);
}

VOID CALLBACK ServiceNotifyCallback(PVOID context) {
    SetEvent(static_cast<HANDLE>(context));
}

bool WaitForServiceStopped(SC_HANDLE service, DWORD timeoutMs) {
    HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ev) return false;
    SERVICE_NOTIFY notify{};
    notify.dwVersion = SERVICE_NOTIFY_STATUS_CHANGE;
    notify.pfnNotifyCallback = ServiceNotifyCallback;
    notify.pContext = ev;
    const DWORD notifyErr = NotifyServiceStatusChangeW(
        service, SERVICE_NOTIFY_STOPPED, &notify);
    bool stopped = false;
    if (notifyErr == ERROR_SUCCESS) {
        stopped = WaitForSingleObject(ev, timeoutMs) == WAIT_OBJECT_0;
    } else {
        // 通知注册失败时只做一次有界等待，不进入轮询循环。
        WaitForSingleObject(ev, 250);
    }
    if (!stopped) {
        SERVICE_STATUS_PROCESS status{};
        DWORD needed = 0;
        if (QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO,
                                 reinterpret_cast<LPBYTE>(&status),
                                 sizeof(status), &needed) &&
            status.dwCurrentState == SERVICE_STOPPED) {
            stopped = true;
        }
    }
    CloseHandle(ev);
    return stopped;
}

std::vector<ServiceInfo> CollectServices() {
    std::vector<ServiceInfo> out;
    SC_HANDLE scm = OpenSCManagerW(
        nullptr, nullptr, SC_MANAGER_CONNECT | SC_MANAGER_ENUMERATE_SERVICE);
    if (!scm) return out;

    DWORD resume = 0;
    for (int guard = 0; guard < 16; ++guard) {
        DWORD needed = 0;
        DWORD returned = 0;
        const DWORD bufBytes = 256 * 1024;
        std::vector<BYTE> buffer(bufBytes);
        const BOOL ok = EnumServicesStatusExW(
            scm, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL,
            buffer.data(), bufBytes, &needed, &returned, &resume, nullptr);
        if (!ok) {
            if (GetLastError() == ERROR_MORE_DATA && needed > bufBytes) {
                buffer.resize(needed);
                needed = 0;
                returned = 0;
                if (!EnumServicesStatusExW(
                        scm, SC_ENUM_PROCESS_INFO, SERVICE_WIN32,
                        SERVICE_STATE_ALL, buffer.data(),
                        static_cast<DWORD>(buffer.size()), &needed, &returned,
                        &resume, nullptr)) {
                    break;
                }
            } else {
                break;
            }
        }
        auto* entries = reinterpret_cast<ENUM_SERVICE_STATUS_PROCESSW*>(
            buffer.data());
        for (DWORD i = 0; i < returned; ++i) {
            const auto& e = entries[i];
            ServiceInfo info;
            info.name = e.lpServiceName ? e.lpServiceName : L"";
            info.displayName = e.lpDisplayName ? e.lpDisplayName : L"";
            info.pid = e.ServiceStatusProcess.dwProcessId;
            info.type = e.ServiceStatusProcess.dwServiceType;
            info.state = e.ServiceStatusProcess.dwCurrentState;
            info.acceptStop =
                (e.ServiceStatusProcess.dwControlsAccepted &
                 SERVICE_ACCEPT_STOP) != 0;

            SC_HANDLE svc =
                OpenServiceW(scm, info.name.c_str(),
                             SERVICE_QUERY_CONFIG | SERVICE_QUERY_STATUS);
            if (svc) {
                DWORD cfgBytes = 0;
                QueryServiceConfigW(svc, nullptr, 0, &cfgBytes);
                if (cfgBytes > 0) {
                    std::vector<BYTE> cfg(cfgBytes);
                    auto* qc =
                        reinterpret_cast<QUERY_SERVICE_CONFIGW*>(cfg.data());
                    if (QueryServiceConfigW(
                            svc, qc, cfgBytes, &cfgBytes)) {
                        info.startType = qc->dwStartType;
                        if (qc->lpBinaryPathName) {
                            wchar_t expanded[32768] = {};
                            if (ExpandEnvironmentStringsW(
                                    qc->lpBinaryPathName, expanded,
                                    32760) > 0) {
                                info.binaryPath = appsuspend::NormalizePath(
                                    expanded);
                            } else {
                                info.binaryPath = appsuspend::NormalizePath(
                                    qc->lpBinaryPathName);
                            }
                        }
                    }
                }
                DWORD depNeeded = 0;
                DWORD depCount = 0;
                if (!EnumDependentServicesW(svc, SERVICE_ACTIVE, nullptr, 0,
                                            &depNeeded, &depCount)) {
                    if (GetLastError() == ERROR_MORE_DATA) {
                        info.activeDependents = depCount;
                    }
                }
                CloseServiceHandle(svc);
            }
            out.push_back(std::move(info));
        }
        if (returned == 0 || resume == 0) break;
    }
    CloseServiceHandle(scm);
    return out;
}

std::wstring ServiceTokenFromRoot(const std::wstring& root) {
    const std::wstring base = ToLower(appsuspend::BaseNameOf(root));
    std::wstring token;
    token.reserve(base.size());
    for (wchar_t ch : base) {
        if (iswalnum(ch)) token.push_back(ch);
    }
    return token;
}

bool ServiceMatchesApp(const ServiceInfo& service,
                       const std::vector<std::wstring>& keys,
                       const std::vector<std::wstring>& roots,
                       bool sharedRoot, const std::wstring& rootParent,
                       const std::wstring& productToken) {
    if (service.binaryPath.empty()) return false;
    if ((service.type & SERVICE_WIN32_OWN_PROCESS) == 0) return false;
    for (const std::wstring& key : keys) {
        if (!key.empty() && service.binaryPath == key) return true;
    }
    if (!sharedRoot) {
        for (const std::wstring& root : roots) {
            if (!root.empty() && SameOrInside(service.binaryPath, root)) {
                return true;
            }
        }
    }
    // 产品家族匹配：QuarkCloudDrive 与 QuarkCloudDriveUpdater 这类兄弟目录。
    if (productToken.size() >= 5 && !rootParent.empty() &&
        IsInsideDir(service.binaryPath, rootParent) &&
        service.binaryPath.find(productToken) != std::wstring::npos) {
        const std::wstring display = ToLower(service.displayName);
        const std::wstring name = ToLower(service.name);
        if (display.find(productToken) != std::wstring::npos ||
            name.find(productToken) != std::wstring::npos) {
            return true;
        }
    }
    return false;
}

bool ServiceIsRunning(DWORD state) {
    return state == SERVICE_RUNNING || state == SERVICE_START_PENDING ||
           state == SERVICE_CONTINUE_PENDING || state == SERVICE_PAUSE_PENDING ||
           state == SERVICE_PAUSED;
}

}  // namespace

bool ShouldCleanupResidual(bool pinned, bool thirdParty, bool enabled,
                           bool hasWindow, bool hasTray, bool launching,
                           bool safeDesktop, bool foreground) {
    return pinned && thirdParty && enabled && !hasWindow && !hasTray &&
           !launching && safeDesktop && !foreground;
}

std::vector<DWORD> CollectUserWindowPids() {
    std::vector<DWORD> pids;
    EnumWindows(EnumUserWindowProc, reinterpret_cast<LPARAM>(&pids));
    std::sort(pids.begin(), pids.end());
    pids.erase(std::unique(pids.begin(), pids.end()), pids.end());
    return pids;
}

Evidence CollectEvidence() {
    Evidence evidence;
    const std::vector<appsuspend::ProcessEntry> entries =
        appsuspend::SnapshotProcessesEx(
            appsuspend::ProcessSnapshotOptions{true, true});
    const std::vector<DWORD> windowPids = CollectUserWindowPids();
    std::unordered_set<DWORD> windowSet(windowPids.begin(), windowPids.end());
    evidence.userWindowPids = windowPids;
    evidence.processes.reserve(entries.size());
    for (const auto& e : entries) {
        ProcessInfo info;
        info.pid = e.pid;
        info.parentPid = e.parentPid;
        info.creationTime = e.creationTime;
        info.sessionId = e.sessionId;
        info.imagePath = e.imagePath;
        info.userWindow = windowSet.count(e.pid) != 0;
        evidence.processes.push_back(std::move(info));
    }
    evidence.services = CollectServices();
    return evidence;
}

std::vector<ServiceInfo> ServicesForApp(const std::wstring& appKey,
                                        const std::wstring& pinKey) {
    std::vector<std::wstring> keys;
    const std::wstring key = appsuspend::NormalizePath(appKey);
    const std::wstring pin = appsuspend::NormalizePath(pinKey);
    if (!key.empty()) keys.push_back(key);
    if (!pin.empty() && pin != key) keys.push_back(pin);
    std::vector<std::wstring> roots;
    for (const std::wstring& k : keys) {
        if (appsuspend::BaseNameOf(k).size() <= 4) continue;
        bool broad = false;
        const std::wstring root =
            TrimTrailingSlash(appsuspend::InstallRootForApp(k, &broad));
        if (root.empty() || broad) continue;
        if (std::find(roots.begin(), roots.end(), root) == roots.end()) {
            roots.push_back(root);
        }
    }
    std::vector<ServiceInfo> out;
    if (keys.empty() || roots.empty()) return out;
    const std::wstring rootParent =
        appsuspend::ParentDirOf(roots.front());
    const std::wstring token = ServiceTokenFromRoot(roots.front());
    for (ServiceInfo& svc : CollectServices()) {
        if (ServiceMatchesApp(svc, keys, roots, /*sharedRoot=*/false,
                              rootParent, token)) {
            out.push_back(std::move(svc));
        }
    }
    return out;
}

Plan BuildPlan(const CleanupRequest& request, const Evidence& evidence,
               const TrayProbe& trayProbe, const CleanupOptions& options) {
    Plan plan;
    const std::wstring key = appsuspend::NormalizePath(request.appKey);
    const std::wstring pin = appsuspend::NormalizePath(request.pinKey);
    if (key.empty() && pin.empty()) {
        plan.abort = true;
        plan.abortReason = L"空应用 key";
        return plan;
    }

    std::vector<std::wstring> keys;
    if (!key.empty()) keys.push_back(key);
    if (!pin.empty() && pin != key) keys.push_back(pin);

    std::unordered_set<std::wstring> foreignKeys;
    for (const std::wstring& k : request.allAppKeys) {
        const std::wstring n = appsuspend::NormalizePath(k);
        if (!n.empty() && std::find(keys.begin(), keys.end(), n) == keys.end()) {
            foreignKeys.insert(n);
        }
    }

    std::vector<std::wstring> roots;
    std::vector<bool> rootBroad;
    for (const std::wstring& k : keys) {
        bool broad = false;
        const std::wstring root =
            TrimTrailingSlash(appsuspend::InstallRootForApp(k, &broad));
        if (root.empty()) continue;
        if (std::find(roots.begin(), roots.end(), root) == roots.end()) {
            roots.push_back(root);
            rootBroad.push_back(broad);
        }
    }

    bool sharedRoot = false;
    for (const std::wstring& foreign : foreignKeys) {
        bool fb = false;
        const std::wstring fr =
            TrimTrailingSlash(appsuspend::InstallRootForApp(foreign, &fb));
        if (fr.empty()) continue;
        for (const std::wstring& root : roots) {
            if (SameOrInside(fr, root) || SameOrInside(root, fr)) {
                sharedRoot = true;
                break;
            }
        }
        if (sharedRoot) break;
    }

    std::unordered_map<DWORD, const ProcessInfo*> byPid;
    std::unordered_map<DWORD, std::vector<const ProcessInfo*>> children;
    byPid.reserve(evidence.processes.size());
    for (const ProcessInfo& p : evidence.processes) {
        byPid[p.pid] = &p;
        children[p.parentPid].push_back(&p);
    }
    std::unordered_set<DWORD> windowPids(evidence.userWindowPids.begin(),
                                         evidence.userWindowPids.end());

    struct Selected {
        Target target;
        int priority = 0;
    };
    std::unordered_map<DWORD, Selected> selected;
    auto addTarget = [&](const ProcessInfo& p, TargetReason reason,
                         const std::wstring& serviceName, int priority) {
        if (p.pid == 0 || p.creationTime == 0 || p.imagePath.empty()) return;
        if (appsuspend::IsNeverSuspendImage(p.imagePath)) return;
        if (foreignKeys.count(p.imagePath) != 0) return;
        auto found = selected.find(p.pid);
        if (found != selected.end()) {
            if (priority > found->second.priority) {
                found->second.target = Target{p.pid, p.creationTime,
                                              p.imagePath, reason,
                                              serviceName};
                found->second.priority = priority;
            }
            return;
        }
        Selected s;
        s.priority = priority;
        s.target = Target{p.pid, p.creationTime, p.imagePath, reason,
                          serviceName};
        selected.emplace(p.pid, std::move(s));
    };

    auto addDescendants = [&](const ProcessInfo& root,
                              const std::wstring& serviceName, int priority) {
        std::queue<const ProcessInfo*> queue;
        queue.push(&root);
        std::unordered_set<DWORD> visited;
        visited.insert(root.pid);
        while (!queue.empty()) {
            const ProcessInfo* parent = queue.front();
            queue.pop();
            auto it = children.find(parent->pid);
            if (it == children.end()) continue;
            for (const ProcessInfo* child : it->second) {
                if (!child || !visited.insert(child->pid).second) continue;
                if (appsuspend::IsNeverSuspendImage(child->imagePath)) continue;
                if (foreignKeys.count(child->imagePath) != 0) continue;
                if (parent->creationTime != 0 && child->creationTime != 0 &&
                    child->creationTime < parent->creationTime) {
                    continue;
                }
                addTarget(*child, TargetReason::Descendant, serviceName,
                          priority);
                queue.push(child);
            }
        }
    };

    // A. 精确 key。
    std::vector<const ProcessInfo*> seeds;
    for (const ProcessInfo& p : evidence.processes) {
        if (std::find(keys.begin(), keys.end(), p.imagePath) == keys.end()) {
            continue;
        }
        if (p.sessionId != CurrentSessionId()) continue;
        addTarget(p, TargetReason::ExactKey, L"", 50);
        seeds.push_back(&p);
    }

    // B. Job 成员。
    for (const auto& ref : request.jobMembers) {
        auto it = byPid.find(ref.pid);
        if (it == byPid.end() || !it->second) continue;
        if (it->second->creationTime != ref.creationTime) continue;
        addTarget(*it->second, TargetReason::JobMember, L"", 40);
        seeds.push_back(it->second);
    }

    // C. A/B 后代。
    for (const ProcessInfo* seed : seeds) {
        if (seed) addDescendants(*seed, L"", 30);
    }

    // D/E. 服务宿主与服务后代。
    const std::wstring rootParent =
        roots.empty() ? std::wstring()
                      : appsuspend::ParentDirOf(roots.front());
    const std::wstring productToken =
        roots.empty() ? std::wstring() : ServiceTokenFromRoot(roots.front());
    std::unordered_set<DWORD> serviceHostPids;
    for (const ServiceInfo& svc : evidence.services) {
        if (!ServiceMatchesApp(svc, keys, roots, sharedRoot, rootParent,
                               productToken)) {
            continue;
        }
        if (!ServiceIsRunning(svc.state) || svc.pid == 0) continue;
        serviceHostPids.insert(svc.pid);
        ServiceTarget target;
        target.name = svc.name;
        target.displayName = svc.displayName;
        target.binaryPath = svc.binaryPath;
        target.pid = svc.pid;
        plan.services.push_back(std::move(target));
        auto host = byPid.find(svc.pid);
        if (host != byPid.end() && host->second) {
            addDescendants(*host->second, svc.name, 20);
        }
    }

    // F. 窄安装根镜像（服务宿主已单独处理；共享根/宽泛根禁用）。
    if (!sharedRoot) {
        for (size_t i = 0; i < roots.size(); ++i) {
            if (rootBroad[i]) continue;
            for (const ProcessInfo& p : evidence.processes) {
                if (p.sessionId != CurrentSessionId()) continue;
                if (!SameOrInside(p.imagePath, roots[i])) continue;
                addTarget(p, TargetReason::InstallRoot, L"", 10);
            }
        }
    } else {
        plan.notes.push_back(L"安装根与其它 Dock 应用共享，禁用安装根兜底");
    }

    // 服务宿主不直接作为进程目标：先停服务，避免边停边杀。
    for (DWORD hostPid : serviceHostPids) selected.erase(hostPid);

    plan.targets.reserve(selected.size());
    for (auto& kv : selected) plan.targets.push_back(std::move(kv.second.target));
    std::sort(plan.targets.begin(), plan.targets.end(),
              [](const Target& a, const Target& b) { return a.pid < b.pid; });

    // 窗口是硬边界：任何候选拥有可见/最小化/cloaked 窗口 -> 整组放弃。
    for (const Target& t : plan.targets) {
        if (windowPids.count(t.pid) != 0) {
            plan.abort = true;
            plan.abortReason = L"候选 pid " + std::to_wstring(t.pid) +
                               L" 仍有可见/最小化窗口";
            return plan;
        }
        auto it = byPid.find(t.pid);
        if (it != byPid.end() && it->second && it->second->userWindow) {
            plan.abort = true;
            plan.abortReason = L"候选 pid " + std::to_wstring(t.pid) +
                               L" 仍有用户窗口";
            return plan;
        }
        if (it != byPid.end() && it->second &&
            it->second->sessionId != CurrentSessionId() &&
            t.serviceName.empty()) {
            // 其它用户会话不碰。
            plan.notes.push_back(L"跳过其它会话 pid " +
                                 std::to_wstring(t.pid));
        }
    }

    // 只保留当前会话与明确服务后代。
    plan.targets.erase(
        std::remove_if(plan.targets.begin(), plan.targets.end(),
                       [&](const Target& t) {
                           auto it = byPid.find(t.pid);
                           if (it == byPid.end() || !it->second) return false;
                           return it->second->sessionId != CurrentSessionId() &&
                                  t.serviceName.empty();
                       }),
        plan.targets.end());

    // 托盘是硬边界。
    std::vector<DWORD> pids;
    pids.reserve(plan.targets.size());
    for (const Target& t : plan.targets) pids.push_back(t.pid);
    if (trayProbe && trayProbe(request, pids)) {
        plan.abort = true;
        plan.abortReason = L"仍有存活托盘图标";
        return plan;
    }

    // 刚启动的进程先不清理（应用可能正在初始化/拉起托盘）。
    if (!plan.targets.empty()) {
        FILETIME nowFt{};
        GetSystemTimeAsFileTime(&nowFt);
        const ULONGLONG now =
            (static_cast<ULONGLONG>(nowFt.dwHighDateTime) << 32) |
            nowFt.dwLowDateTime;
        const ULONGLONG minAge = options.minProcessAgeMs * 10000ull;
        for (const Target& t : plan.targets) {
            if (t.creationTime > now) continue;
            if (now - t.creationTime < minAge) {
                plan.abort = true;
                plan.abortReason = L"进程刚启动（pid " +
                                   std::to_wstring(t.pid) + L"），延后";
                return plan;
            }
        }
    }

    if (plan.targets.empty() && plan.services.empty()) {
        plan.notes.push_back(L"没有可归因进程");
    }
    if (!options.stopServices && !plan.services.empty()) {
        plan.notes.push_back(L"服务停止已关闭，保留 " +
                             std::to_wstring(plan.services.size()) +
                             L" 个服务");
    }
    return plan;
}

namespace {

bool StopServiceOnce(SC_HANDLE scm, const ServiceTarget& target,
                     std::wstring* detail) {
    SC_HANDLE svc = OpenServiceW(
        scm, target.name.c_str(), SERVICE_STOP | SERVICE_QUERY_STATUS);
    if (!svc) {
        if (detail) {
            *detail = L"打开服务失败（" + std::to_wstring(GetLastError()) +
                      L"）";
        }
        return false;
    }
    SERVICE_STATUS status{};
    if (!ControlService(svc, SERVICE_CONTROL_STOP, &status)) {
        const DWORD err = GetLastError();
        if (err == ERROR_SERVICE_NOT_ACTIVE) {
            CloseServiceHandle(svc);
            return true;
        }
        if (detail) {
            *detail = L"ControlService 失败（" + std::to_wstring(err) + L"）";
        }
        CloseServiceHandle(svc);
        return false;
    }
    const bool stopped = WaitForServiceStopped(svc, kServiceStopTimeoutMs);
    if (!stopped && detail) *detail = L"等待服务停止超时";
    CloseServiceHandle(svc);
    return stopped;
}

CleanupResult ExecutePlan(const Plan& plan, const CleanupOptions& options) {
    CleanupResult result;
    result.matched = plan.targets.size();
    result.servicesMatched = plan.services.size();
    if (options.dryRun) {
        result.verdict = CleanupVerdict::DryRun;
        return result;
    }
    if (options.cancel && options.cancel->load()) {
        result.verdict = CleanupVerdict::AbortedCancelled;
        result.detail = L"清理已被取消（应用重新打开/交互）";
        return result;
    }

    SC_HANDLE scm = nullptr;
    if (options.stopServices && !plan.services.empty()) {
        scm = OpenSCManagerW(nullptr, nullptr,
                             SC_MANAGER_CONNECT);
    }
    if (scm) {
        for (const ServiceTarget& service : plan.services) {
            std::wstring why;
            if (StopServiceOnce(scm, service, &why)) {
                ++result.servicesStopped;
                result.detail += L"停止服务 " + service.name + L"；";
            } else {
                ++result.servicesDenied;
                result.needsElevation = true;
                result.detail += L"服务 " + service.name + L" 未停止（" +
                                 why + L"）；";
            }
        }
        CloseServiceHandle(scm);
    } else if (!plan.services.empty()) {
        result.servicesDenied = plan.services.size();
        result.needsElevation = true;
        result.detail += L"无法打开 SCM，服务未停止；";
    }

    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    std::vector<HANDLE> handles;
    std::vector<HANDLE> individual;
    handles.reserve(plan.targets.size());
    for (const Target& target : plan.targets) {
        DWORD err = ERROR_SUCCESS;
        HANDLE proc = OpenVerified(
            target.pid, target.creationTime,
            PROCESS_SET_QUOTA | PROCESS_TERMINATE |
                PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE,
            &err);
        if (!proc) {
            proc = OpenVerified(
                target.pid, target.creationTime,
                PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION |
                    SYNCHRONIZE,
                &err);
        }
        if (!proc) {
            if (err == ERROR_NOT_FOUND || err == ERROR_INVALID_PARAMETER) {
                // 计划与执行之间进程已退出（或 PID 已失效）：视为已解决，
                // 不能报 NeedsElevation，否则测试/用户会看到假失败。
                ++result.terminated;
                result.detail += L"已退出 pid " +
                                 std::to_wstring(target.pid) + L"；";
                continue;
            }
            ++result.denied;
            result.needsElevation = true;
            result.detail += L"拒绝/无法打开 pid " +
                             std::to_wstring(target.pid) + L"（" +
                             std::to_wstring(err) + L"）；";
            continue;
        }
        const std::wstring image = ImagePathOf(proc);
        if (!image.empty() && image != target.imagePath) {
            CloseHandle(proc);
            ++result.denied;
            result.detail += L"镜像变化 pid " + std::to_wstring(target.pid) +
                             L"；";
            continue;
        }
        if (IsProcessCritical(proc)) {
            CloseHandle(proc);
            ++result.denied;
            result.detail += L"关键进程 pid " + std::to_wstring(target.pid) +
                             L"；";
            continue;
        }
        bool assigned = false;
        if (job) {
            assigned = AssignProcessToJobObject(job, proc) != FALSE;
        }
        if (assigned) {
            handles.push_back(proc);
        } else {
            individual.push_back(proc);
        }
    }

    if (job && !handles.empty()) {
        if (options.cancel && options.cancel->load()) {
            for (HANDLE h : handles) CloseHandle(h);
            for (HANDLE h : individual) CloseHandle(h);
            if (job) CloseHandle(job);
            result.verdict = CleanupVerdict::AbortedCancelled;
            result.detail = L"终止前被取消";
            return result;
        }
        TerminateJobObject(job, 1);
    }
    for (HANDLE proc : individual) {
        TerminateProcess(proc, 1);
    }

    // 一次性有界等待，不做轮询。
    const size_t kChunk = 60;
    for (size_t start = 0; start < handles.size(); start += kChunk) {
        const size_t count = std::min(kChunk, handles.size() - start);
        std::vector<HANDLE> chunk(handles.begin() + static_cast<long>(start),
                                  handles.begin() + static_cast<long>(start + count));
        const DWORD wait = WaitForMultipleObjects(
            static_cast<DWORD>(chunk.size()), chunk.data(), TRUE, 3000);
        if (wait == WAIT_FAILED || wait == WAIT_TIMEOUT) {
            for (HANDLE h : chunk) {
                DWORD code = 0;
                if (GetExitCodeProcess(h, &code) && code == STILL_ACTIVE) {
                    result.detail += L"清理后仍存活 pid；";
                }
            }
        }
    }

    for (HANDLE h : handles) {
        DWORD code = 0;
        if (GetExitCodeProcess(h, &code) && code != STILL_ACTIVE) {
            ++result.terminated;
        }
    }
    for (HANDLE h : individual) {
        DWORD code = 0;
        if (GetExitCodeProcess(h, &code) && code != STILL_ACTIVE) {
            ++result.terminated;
        }
        CloseHandle(h);
    }
    for (HANDLE h : handles) CloseHandle(h);
    if (job) CloseHandle(job);

    if (result.terminated == 0 && result.matched == 0 &&
        result.servicesMatched == 0) {
        result.verdict = CleanupVerdict::NoProcesses;
    } else if (result.denied > 0 || result.servicesDenied > 0) {
        result.verdict = result.terminated > 0 ? CleanupVerdict::Cleaned
                                               : CleanupVerdict::NeedsElevation;
    } else if (result.terminated < result.matched) {
        result.verdict = CleanupVerdict::Failed;
    } else {
        result.verdict = CleanupVerdict::Cleaned;
    }
    return result;
}

}  // namespace

CleanupResult CleanupOnce(const CleanupRequest& request,
                          const CleanupOptions& options,
                          const TrayProbe& trayProbe) {
    CleanupResult result;
    HDESK desktop =
        OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    if (!desktop) {
        result.verdict = CleanupVerdict::AbortedDesktop;
        result.detail = L"无法打开输入桌面";
        return result;
    }
    wchar_t name[128] = {};
    DWORD needed = 0;
    const BOOL nameOk = GetUserObjectInformationW(
        desktop, UOI_NAME, name, sizeof(name), &needed);
    CloseDesktop(desktop);
    if (!nameOk || _wcsicmp(name, L"Default") != 0) {
        result.verdict = CleanupVerdict::AbortedDesktop;
        result.detail = L"当前不是 Default 桌面";
        return result;
    }

    const Evidence evidence = CollectEvidence();
    Plan plan;
    plan = BuildPlan(request, evidence, trayProbe, options);
    if (plan.abort) {
        result.verdict = plan.abortReason.find(L"窗口") != std::wstring::npos
                             ? CleanupVerdict::AbortedOpenWindow
                             : (plan.abortReason.find(L"托盘") != std::wstring::npos
                                    ? CleanupVerdict::AbortedTray
                                    : CleanupVerdict::AbortedLaunching);
        result.detail = plan.abortReason;
        return result;
    }
    if (plan.targets.empty() && plan.services.empty()) {
        result.verdict = CleanupVerdict::NoProcesses;
        result.detail = L"没有可归因进程/服务";
        return result;
    }
    result = ExecutePlan(plan, options);
    for (const std::wstring& note : plan.notes) {
        if (!result.detail.empty()) result.detail += L"；";
        result.detail += note;
    }
    return result;
}

}  // namespace appcleanup
