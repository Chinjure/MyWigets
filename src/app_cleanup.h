// app_cleanup.h - Dock「可见关闭即零进程」清理
//
// 与 app_suspend（挂起）分开：
//   - 挂起只处理仍然打开、但窗口最小化/托盘驻留的应用；
//   - 清理处理「没有可见/最小化窗口、也没有存活托盘图标」的关闭状态。
//
// 清理不看 SuspendException：菜单里的「不暂停此应用（后台保持运行）」
// 只影响挂起，关闭后仍必须清理。
//
// 禁止用轮询关闭反复重启的进程：
//   - 关闭由 WinEvent/托盘变化/Job 退出/服务状态事件触发；
//   - 每次关闭 episode 只做一次清理；
//   - 候选进程加入一个瞬时 Job，TerminateJobObject 一次性原子终止；
//   - 反复重启只允许停止一次源头（应用私有服务/计划任务），失败即
//     Blocked，不进入 kill-retry 循环。

#pragma once

#include <windows.h>
#include <winsvc.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "app_suspend.h"

namespace appcleanup {

struct ProcessInfo {
    DWORD pid = 0;
    DWORD parentPid = 0;
    ULONGLONG creationTime = 0;
    DWORD sessionId = 0;
    std::wstring imagePath;
    bool userWindow = false;
};

struct ServiceInfo {
    std::wstring name;
    std::wstring displayName;
    std::wstring binaryPath;
    DWORD pid = 0;
    DWORD type = 0;
    DWORD state = 0;
    DWORD startType = SERVICE_DEMAND_START;
    bool acceptStop = false;
    size_t activeDependents = 0;
};

struct Evidence {
    std::vector<ProcessInfo> processes;
    std::vector<ServiceInfo> services;
    // 可见（含最小化）或 DWM cloak 的顶层窗口 PID。
    std::vector<DWORD> userWindowPids;
};

struct CleanupRequest {
    std::wstring appKey;    // DockItem.key（归一化 exe）
    std::wstring pinKey;    // 名称合并条目的固定项 key，可为空
    std::wstring displayName;
    std::vector<appsuspend::ProcessRef> jobMembers;
    // Dock 当前的托盘标记（快速路径；worker 仍会用 UIA/注册表复核）。
    bool trayMarked = false;
    // Dock 当前全部应用 key（含未运行固定项），用于排除其它应用。
    std::vector<std::wstring> allAppKeys;
};

struct CleanupOptions {
    bool dryRun = false;
    bool stopServices = true;
    ULONGLONG minProcessAgeMs = 15000;
    // UI 线程在应用重新打开/交互时置 true；worker 在终止前检查。
    std::shared_ptr<std::atomic<bool>> cancel;
};

enum class TargetReason {
    ExactKey,
    JobMember,
    Descendant,
    InstallRoot,
    ServiceChild,
};

struct Target {
    DWORD pid = 0;
    ULONGLONG creationTime = 0;
    std::wstring imagePath;
    TargetReason reason = TargetReason::ExactKey;
    std::wstring serviceName;
};

struct ServiceTarget {
    std::wstring name;
    std::wstring displayName;
    std::wstring binaryPath;
    DWORD pid = 0;
};

struct Plan {
    bool abort = false;
    std::wstring abortReason;
    std::vector<Target> targets;
    std::vector<ServiceTarget> services;
    std::vector<std::wstring> notes;
};

enum class CleanupVerdict {
    Cleaned,
    NoProcesses,
    DryRun,
    AbortedOpenWindow,
    AbortedTray,
    AbortedLaunching,
    AbortedDesktop,
    AbortedCancelled,
    NeedsElevation,
    BlockedCritical,
    Failed,
};

struct CleanupResult {
    CleanupVerdict verdict = CleanupVerdict::Failed;
    size_t matched = 0;
    size_t terminated = 0;
    size_t denied = 0;
    size_t servicesMatched = 0;
    size_t servicesStopped = 0;
    size_t servicesDenied = 0;
    bool needsElevation = false;
    std::wstring detail;
};

// 纯策略函数，便于单元测试。注意：没有 userExcluded 参数。
bool ShouldCleanupResidual(bool pinned, bool thirdParty, bool enabled,
                           bool hasWindow, bool hasTray, bool launching,
                           bool safeDesktop, bool foreground);

// 托盘探针：Dock 注入 UIA/注册表实现；返回 true 表示有存活托盘图标。
using TrayProbe = std::function<bool(const CleanupRequest&,
                                     const std::vector<DWORD>& pids)>;

// 纯归因（不碰系统）：测试注入 Evidence。
Plan BuildPlan(const CleanupRequest& request, const Evidence& evidence,
               const TrayProbe& trayProbe, const CleanupOptions& options);

// 一次完成采集、归因、执行；不做任何周期/重试。
CleanupResult CleanupOnce(const CleanupRequest& request,
                          const CleanupOptions& options,
                          const TrayProbe& trayProbe);

// 采集系统证据（供 CleanupOnce 和 dry-run 测试程序使用）。
Evidence CollectEvidence();
std::vector<DWORD> CollectUserWindowPids();

// 列出归属该应用的全部应用私有服务（含已停止服务）。用于服务守护：
// 关闭后禁用、应用启动时恢复。只匹配 own-process、窄安装根/产品家族。
std::vector<ServiceInfo> ServicesForApp(const std::wstring& appKey,
                                        const std::wstring& pinKey);

}  // namespace appcleanup
