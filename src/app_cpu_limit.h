// app_cpu_limit.h - 「后台应用 CPU 限速」的进程归属与限速/恢复
//
// 需求：Dock 里已经没有可见窗口的应用（窗口全部最小化，或纯托盘驻留），
// 一旦不再处于顶栏当前焦点，就把它的 CPU 占用硬限制到 0.01%（万分之一），
// 重新成为焦点或被交互时恢复正常。单个应用可以由用户在图标右键菜单里
// 豁免（不限制）。
//
// 为什么不再直接挂起进程：整进程 NtSuspendProcess 会冻结持有 WH_KEYBOARD_LL
// / WH_MOUSE_LL 低级钩子的线程。Windows 每次输入都要等该线程执行钩子回调，
// 线程被冻住时只能等 LowLevelHooksTimeout 才跳过，表现为键盘/鼠标输入明显
// 卡顿甚至无法输入。Job Object 的 CPU hard cap 只限制调度配额，线程仍会
// 运行，低级钩子可以按时执行。
//
// 真正容易出错的地方不是限速 API，而是「哪些进程算这个应用」：
//
//   1. 不能用窗口数量/进程数量判断：Chrome、Edge、Electron、微信、钉钉
//      都是多进程应用，主窗只是其中一个进程。
//   2. 不能只看进程树：应用启动的外部程序（浏览器、终端、安装器）也会
//      是它的子进程；把整棵子树限速会误伤无关应用。
//   3. 不能只看 exe 名字：同名 exe 可能来自不同目录、根本不是同一个应用
//      （例如用户自己编译的 chrome.exe）。
//   4. 不能只用 AppUserModelID：Dock 当前的分组主键是归一化 exe 路径，
//      AUMID 在启动器/绿色版/部分 Qt 应用上并不可靠。
//
// 本模块采用的口径与 Dock 条目严格对齐：
//
//   应用身份 = DockItem.key（归一化小写 exe 完整路径）
//   进程集合 =
//     A. 所有镜像路径恰好等于 key 的进程（多进程应用的主体，如 Chrome
//        的全部 chrome.exe）；
//     B. 从 A 出发、仍在同一安装目录树内的后代进程（不同 exe 的辅助进程，
//        如微信的 WeChatAppEx.exe、Electron 的渲染/GPU helper、Steam 的
//        steamwebhelper.exe）。
//
//   B 的「同一安装目录树」有严格边界：
//     - 常态下要求子进程镜像位于应用 exe 所在安装根目录之下；
//     - 安装根若是 Windows/Program Files/用户目录等宽泛目录，只允许与
//       主 exe **同目录** 的后代，避免把系统进程或隔壁应用吞进来；
//     - 只沿已选中的进程向下扩展。因此应用 A 启动的外部程序如果不在 A
//       的安装目录内，它和它的后代都不会被选中；A 的启动器父进程也不会
//       被选中（只收后代，不收祖先）。
//
// 另外用进程创建时间做 PID 复用防护：限速/恢复前重新打开进程并核对
// 创建时间，绝不把「同 PID 的新进程」当成原来那个进程。
//
// 限速实现：
//   - 每个应用一个 Job Object，JOBOBJECT_CPU_RATE_CONTROL_INFORMATION
//     使用 JOB_OBJECT_CPU_RATE_CONTROL_ENABLE | HARD_CAP，CpuRate=1
//     （1/10000 = 0.01%）；Windows 8+ 支持，Windows 7 上初始化失败并降级。
//   - 已在该 Job 里的进程/其子进程不重复 Assign；解除限制只需把
//     ControlFlags 设为 0，进程立刻恢复全额 CPU。
//   - Job 句柄在 Dock 生命周期内保留以便反复开关；Dock 正常退出/组件重启
//     会先解除限制再关闭句柄。
//
// 注意：Job 的 CPU 硬上限是附着在进程上的。若 Dock 被强杀/崩溃，最后一个
// Job 句柄关闭后上限仍会留在进程上（Windows 没有公开的反向 API），此时
// 需要重启对应应用才能恢复。正常退出路径会清理干净。
//
// 安全边界（由 Dock 在调用前再叠加一层）：
//   - 只把「没有可见窗口」（窗口全部最小化或纯托盘）且未被用户豁免的
//     应用交给本模块；
//   - 不碰 MyWigets 自身、explorer、conhost 等系统基础设施；
//   - 不碰当前系统前台应用（即使顶栏目标暂时粘在别的应用上）；
//   - 不碰安全桌面（UAC/锁屏）期间的新限速请求；
//   - 刚启动的进程有最小年龄保护，避免把正在初始化的应用限速；
//   - 正常退出/组件重启时恢复所有自己限制的进程。

#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace appcpu {

// 进程快照中的一项。imagePath 为归一化小写完整路径；creationTime 为
// FILETIME 的 64 位值（100ns），0 表示查询失败。
struct ProcessEntry {
    DWORD pid = 0;
    DWORD parentPid = 0;
    ULONGLONG creationTime = 0;
    std::wstring imagePath;
};

struct ProcessRef {
    DWORD pid = 0;
    ULONGLONG creationTime = 0;
    // true = 与应用 key 完全同镜像的主进程；false = 安装目录树内的辅助进程。
    // 限速时要求至少有一个主进程能加入 Job，避免只把 helper 限死而主进程
    // 全速运行，造成“限了但没限到点子上”的半吊子状态。
    bool exactMatch = false;
};

// 选择结果：processes 是最终要限速的进程；installRoot/broadRoot 仅用于
// 日志与测试，说明本次按哪个目录树收拢辅助进程。
struct Selection {
    std::vector<ProcessRef> processes;
    std::wstring installRoot;
    bool broadRoot = false;
    size_t exactMatches = 0;
    size_t helperMatches = 0;
    std::wstring note;
};

std::wstring NormalizePath(const std::wstring& path);
std::wstring BaseNameOf(const std::wstring& path);
std::wstring ParentDirOf(const std::wstring& path);

// 应用安装根：通常就是 exe 所在目录；若 exe 位于 bin/x64/application 等
// 常见二级目录，则上提到产品目录，以覆盖 bin\..\helper.exe 这类布局。
// broadRoot=true 表示根目录过宽（Windows、Program Files、用户目录等），
// 此时辅助进程只允许来自与主 exe 完全相同的目录。
std::wstring InstallRootForApp(const std::wstring& appKey, bool* broadRoot);

bool IsBroadInstallRoot(const std::wstring& dir);

// 永不属于任何可限速应用集合的系统/套件进程（按 exe 基名）。
bool IsNeverLimitImage(const std::wstring& imagePath);

// Dock 侧的限速策略（纯函数，便于单元测试）：只有「仍然打开、当前没有
// 任何“可见且未最小化”的窗口、且没有被用户单独豁免」的应用才允许限速。
// hasVisibleWindow 为 true 表示至少有一张可见且未最小化的顶层窗口；
// 窗口全部最小化、或纯托盘驻留（根本没有窗口）时都为 false，可以限速。
bool ShouldLimitApp(bool opened, bool hasVisibleWindow, bool userExcluded);

// 全量进程快照（忽略其他登录会话、系统基础设施与自身）。
std::vector<ProcessEntry> SnapshotProcesses();

// 按上面的口径，从快照中选出属于 appKey 的进程。纯函数，便于单元测试。
//
// foreignAppKeys：Dock 中其它应用的 key 集合。用于避免把「应用 A 启动的
// 应用 B」误当 A 的辅助进程 —— 典型是 Office 同一安装目录下 WINWORD 拉起
// EXCEL，两者在同一个 Office16 目录树里。子进程镜像恰好是另一个 Dock
// 应用的 key 时，不并入当前应用，也不继续向下遍历它的后代。
Selection SelectAppProcesses(
    const std::wstring& appKey, const std::vector<ProcessEntry>& snapshot,
    const std::unordered_set<std::wstring>* foreignAppKeys = nullptr);

enum class LimitCode {
    Limited,          // 至少把一个进程纳入 CPU 限制（或已在限制中）
    AlreadyLimited,   // 该应用已经在限速表中
    NoProcesses,      // 找不到属于该应用的进程
    TooYoung,         // 进程刚启动，等过了最小年龄再试
    AccessDenied,     // 有匹配进程，但一个也打不开/加不进 Job（权限/受保护）
    Failed,           // 其他失败（Job 创建/CPU 限制设置失败）
};

struct LimitRequest {
    std::wstring appKey;
    std::wstring displayName;
};

struct LimitResult {
    LimitCode code = LimitCode::Failed;
    size_t matched = 0;
    size_t limited = 0;   // 已纳入 CPU 限制的进程数
    size_t skipped = 0;
    DWORD retryAfterMs = 0;
    std::wstring detail;
};

// CPU 限速控制器。只应由 Dock 的 UI 线程调用；内部不创建线程。
//
// 每个应用对应一个长期保留的 Job Object：限制时把进程加入 Job 并打开
// hard cap；恢复时只关闭 CPU rate control（不关 Job 句柄），这样同一个
// 应用反复开关不会不断创建新 Job。析构/正常退出时解除所有限制并关闭句柄。
class CpuLimiter {
public:
    explicit CpuLimiter(ULONGLONG minProcessAgeMs = 8000);
    ~CpuLimiter();

    CpuLimiter(const CpuLimiter&) = delete;
    CpuLimiter& operator=(const CpuLimiter&) = delete;

    bool Initialize();

    // 一次快照处理多个应用，避免桌面/无焦点时逐个应用重复枚举全进程。
    // 返回与 requests 一一对应的结果。
    // allAppKeys = Dock 当前全部应用 key（含固定但未运行的），用于让
    // SelectAppProcesses 区分「辅助进程」与「被本应用拉起的另一个应用」。
    // 同一应用重复调用时：已在限制中返回 AlreadyLimited；已解除限制但
    // Job 仍在的应用会重新加入新出现的进程并再次打开 hard cap。
    std::vector<LimitResult> LimitMany(
        const std::vector<LimitRequest>& requests,
        const std::vector<std::wstring>& allAppKeys = {});

    // 给已经在限制中的应用补挂新出现的进程（主进程新开的 helper、重启后
    // 的新实例等）。Job 设置了 SILENT_BREAKAWAY_OK，子进程不会自动继承，
    // 必须由 Dock 周期性重新按“同 exe + 安装目录树”口径选中并 Assign。
    // 返回本次新加入 Job 的进程数；detail 仅在确有新增时填写。
    size_t EnforceLimited(const std::vector<std::wstring>& appKeys,
                          const std::vector<std::wstring>& allAppKeys,
                          std::wstring* detail = nullptr);

    // 解除该应用的 CPU 限制。个别进程已退出视为成功；解除失败会保留
    // Job 句柄和跟踪表，等待下次重试（绝不带着 hard cap 关掉句柄）。
    bool Restore(const std::wstring& appKey, std::wstring* detail = nullptr);

    void RestoreAll();

    bool IsLimited(const std::wstring& appKey) const;
    size_t TrackedProcessCount() const;
    std::vector<std::wstring> LimitedKeys() const;

    // 一次性迁移：旧版「挂起未聚焦应用」可能在注册表留下挂起表。
    // 读取并 NtResumeProcess 恢复这些进程，成功清理后返回恢复数量。
    size_t RecoverLegacySuspended(std::wstring* detail = nullptr);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace appcpu
