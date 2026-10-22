// app_cpu_limit.h - 「后台应用效率模式（EcoQoS）」的进程归属与启用/恢复
//
// 需求：Dock 里已经没有可见窗口的应用（窗口全部最小化，或纯托盘驻留），
// 一旦不再处于顶栏当前焦点，就把进程的 QoS 切到效率模式（EcoQoS，即
// 任务管理器里的“效率模式”）；重新成为焦点或被交互时恢复正常。单个应用
// 可以由用户在图标右键菜单里豁免（不启用效率模式）。
//
// 为什么不再挂起/硬限速：
//   1. 整进程 NtSuspendProcess 会冻结持有 WH_KEYBOARD_LL / WH_MOUSE_LL
//      低级钩子的线程。Windows 每次输入都要等该线程执行钩子回调，线程
//      被冻住时只能等 LowLevelHooksTimeout 才跳过，表现为键盘/鼠标输入
//      明显卡顿甚至无法输入。
//   2. Job Object 的 0.01% CPU hard cap 虽然不冻结线程，但把钩子线程也
//      饿到几乎得不到调度；实测微信仍能感到输入延迟。
//   3. EcoQoS 使用 SetProcessInformation(ProcessPowerThrottling) 只降低
//      进程的调度/能效等级，不硬性截断线程；UI/低级钩子线程仍能按时运行。
//      它等价于任务管理器手动开启“效率模式”，也是系统推荐的背景任务做法。
//
// 真正容易出错的地方不是 QoS API，而是「哪些进程算这个应用」：
//
//   1. 不能用窗口数量/进程数量判断：Chrome、Edge、Electron、微信、钉钉
//      都是多进程应用，主窗只是其中一个进程。
//   2. 不能只看进程树：应用启动的外部程序（浏览器、终端、安装器）也会
//      是它的子进程；把整棵子树降速会误伤无关应用。
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
// 另外用进程创建时间做 PID 复用防护：启用/恢复效率模式前重新打开进程并
// 核对创建时间，绝不把「同 PID 的新进程」当成原来那个进程。
//
// 效率模式实现：
//   - 对应用中每个选中进程调用 SetProcessInformation(ProcessPowerThrottling)，
//     ControlMask |= EXECUTION_SPEED，StateMask |= EXECUTION_SPEED。
//   - 记录启用前的 ControlMask/StateMask；恢复时原样写回，不把用户或系统
//     本来设置好的效率模式误关掉。
//   - 子进程不会继承 EcoQoS。本实现不做周期轮询：只对应用进入效率模式
//     当时选中的进程生效；之后新开的 helper 需要等应用重新进入效率模式
//     （重新聚焦后再最小化/收托盘）时由 LimitMany 重新选中。
//   - 不使用 Job Object，不存在“崩溃后硬上限留在进程上”的问题。正常退出/
//     组件重启会恢复全部；Dock 被强杀时应用最多停在低功耗效率模式，仍然
//     可响应输入，下次启动/重新聚焦时会恢复。
//
// 安全边界（由 Dock 在调用前再叠加一层）：
//   - 只把「没有可见窗口」（窗口全部最小化或纯托盘）且未被用户豁免的
//     应用交给本模块；
//   - 不碰 MyWigets 自身、explorer、conhost 等系统基础设施；
//   - 不碰当前系统前台应用（即使顶栏目标暂时粘在别的应用上）；
//   - 不碰安全桌面（UAC/锁屏）期间的新效率模式请求；
//   - 刚启动的进程有最小年龄保护，避免把正在初始化的应用切到效率模式；
//   - 正常退出/组件重启时恢复所有自己启用过效率模式的进程。

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
    // 启用效率模式时要求至少有一个主进程成功切换，避免只把 helper 限死而
    // 主进程全速运行，造成“限了但没限到点子上”的半吊子状态。
    bool exactMatch = false;
};

// 选择结果：processes 是最终要启用效率模式的进程；installRoot/broadRoot
// 仅用于日志与测试，说明本次按哪个目录树收拢辅助进程。
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

// 永不属于任何可启用效率模式应用集合的系统/套件进程（按 exe 基名）。
bool IsNeverLimitImage(const std::wstring& imagePath);

// Dock 侧的效率模式策略（纯函数，便于单元测试）：只有「仍然打开、当前
// 没有“可见且未最小化”的窗口、且没有被用户单独豁免」的应用才启用。
// hasVisibleWindow 为 true 表示至少有一张可见且未最小化的顶层窗口；
// 窗口全部最小化、或纯托盘驻留（根本没有窗口）时都为 false，可以启用。
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
    Limited,          // 至少一个进程已切到效率模式（或本来就在）
    AlreadyLimited,   // 该应用已经在效率模式表中
    NoProcesses,      // 找不到属于该应用的进程
    TooYoung,         // 进程刚启动，等过了最小年龄再试
    AccessDenied,     // 有匹配进程，但一个也打不开/无法设置（权限/受保护）
    Failed,           // 其他失败（SetProcessInformation 不可用等）
};

struct LimitRequest {
    std::wstring appKey;
    std::wstring displayName;
};

struct LimitResult {
    LimitCode code = LimitCode::Failed;
    size_t matched = 0;
    size_t limited = 0;   // 已启用效率模式的进程数
    size_t skipped = 0;
    DWORD retryAfterMs = 0;
    std::wstring detail;
};

// 后台应用效率模式控制器。只应由 Dock 的 UI 线程调用；内部不创建线程。
//
// 每个应用维护一组已启用 EcoQoS 的进程（PID+创建时间+启用前状态）。启用
// 时逐个 SetProcessInformation；恢复时把启用前的状态原样写回。析构/正常
// 退出时恢复所有进程。
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
    // 同一应用重复调用时：已在效率模式返回 AlreadyLimited；已恢复的应用
    // 会重新选中新出现的进程并再次启用。
    std::vector<LimitResult> LimitMany(
        const std::vector<LimitRequest>& requests,
        const std::vector<std::wstring>& allAppKeys = {});

    // 注意：EcoQoS 不随进程创建继承。本实现不做轮询，只对进入效率模式
    // 当时选中的进程生效；之后新开的 helper 需要等应用重新进入效率模式
    //（重新聚焦后再最小化/收托盘）时由 LimitMany 重新选中。
    // 如需“新 helper 出现即生效”且不轮询，可改用 Job Object 完成端口的
    // JOB_OBJECT_MSG_NEW_PROCESS 事件（本项目暂不引入该线程）。

    // 退出该应用的效率模式。个别进程已退出视为成功；恢复失败会保留记录，
    // 等待下次重试。
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
