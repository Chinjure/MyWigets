// app_suspend.h - 「后台应用挂起」的进程归属与挂起/恢复
//
// 需求：Dock 里已经没有「可见且未最小化」窗口的应用（窗口全部最小化，
// 或纯托盘驻留、根本没有顶层窗口），一旦不再处于顶栏当前焦点，就暂停
// 它的全部进程（ntdll!NtSuspendProcess）；重新成为焦点或被交互时恢复。
// 单个应用可以由用户在图标右键菜单里豁免（不暂停此应用）。
//
// 为什么不是效率模式 / CPU 限速：
//   - 需求明确要求“暂停它的所有进程”，而不是把调度优先级降到效率模式
//     （EcoQoS）或给 Job Object 设 CPU 上限。挂起后进程完全不再获得
//     CPU 时间，后台占用真正归零。
//
// 已知代价（早先版本因此改用 EcoQoS，这是本模块唯一需要留意的风险）：
//   - 挂起会冻结持有 WH_KEYBOARD_LL / WH_MOUSE_LL 低级钩子的线程。
//     Windows 每次输入都要等该线程执行钩子回调，线程被冻住时只能等
//     LowLevelHooksTimeout 才跳过，表现为键盘/鼠标输入卡顿。Dock 侧通过
//     「交互前先恢复」和 1.2s 宽限期缓解，但无法完全消除。
//   - 因此只挂起「窗口全部最小化或纯托盘」的应用；只要还有一张可见且
//     未最小化的窗口就不碰。用户也可以按应用单独豁免。
//   - 最小化窗口仍会出现在 Alt+Tab 列表里：进程被冻结后，切换器无法
//     还原/激活它。Dock 因此在 Alt+Tab 提前触发、EVENT_SYSTEM_SWITCHSTART、
//     SWITCHEND、顶栏键盘钩子的 Alt 松开通知，以及 Win11 “任务切换”窗口
//     的显示/隐藏（触摸板左右轻扫也走这个窗口）时，先恢复“可能是目标”的
//     挂起应用（所有仍有可见顶层窗口的挂起应用），切换期间禁止重新挂起；
//     纯托盘应用没有窗口，不会出现在 Alt+Tab，保持挂起。恢复/挂起仍只由
//     Dock UI 线程调用本模块，本模块不感知 Alt+Tab。
//
// 真正容易出错的地方不是 NtSuspendProcess，而是「哪些进程算这个应用」：
//
//   1. 不能用窗口数量/进程数量判断：Chrome、Edge、Electron、微信、钉钉
//      都是多进程应用，主窗只是其中一个进程。
//   2. 不能只看进程树：应用启动的外部程序（浏览器、终端、安装器）也会
//      是它的子进程；把整棵子树挂起会误伤无关应用。
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
//     B. 从 A 出发的后代进程（不同 exe 的辅助进程，如微信的
//        WeChatAppEx.exe、Electron 的渲染/GPU helper、Steam 的
//        steamwebhelper.exe；也包括应用把 exe 落到 %TEMP%、更新目录
//        后再拉起的子进程）。
//
//   B 有两条路径：
//     1) 首选「Job 沙箱」：应用主进程被放进每个应用独立的 Job Object，
//        Windows 在创建子进程时自动把后代放进同一个 Job，后续无论
//        子进程从哪个目录启动、父进程是否已经退出，都不会漏。
//     2) Job 不可用时的兜底：按进程快照的父子关系向下遍历，并允许
//        跨安装目录；但只用于已经存在的后代，无法覆盖「枚举之后才
//        创建」的进程，所以仍需 2s 复核补挂。
//
// 兜底遍历仍排除：
//   - 永不挂起的系统/套件进程；
//   - Dock 中另一个应用的 key（应用 A 拉起的应用 B 不应算 A 的 helper）；
//   - 拥有可见且未最小化顶层窗口的外部程序及其后代（那是用户正在看的
//     另一个应用，不能因为 A 收起窗口就被一起冻住）。
//
// 另外用进程创建时间做 PID 复用防护：挂起/恢复前重新打开进程并核对
// 创建时间，绝不把「同 PID 的新进程」当成原来那个进程。
//
// 挂起实现：
//   - 两阶段：先把整组进程全部打开并核对（PID+创建时间、受保护进程、
//     权限），任意一个成员不可挂起就整组放弃 —— 只冻住一半（例如主进程
//     还在、渲染 helper 被冻）比不冻更容易把应用挂死；
//   - 再统一调用 NtSuspendProcess；中途失败立即回滚已挂起的成员，仍回滚
//     失败的保留在跟踪表里，退出时 ResumeAll 还能再试；
//   - 挂起后重新枚举 Job 成员：父进程在「枚举之后、冻结生效之前」刚创建
//     的子进程已经自动进入 Job，但不在第一批句柄里。循环补冻直到没有新
//     成员，冻结后的进程不可能再创建新进程，因此能收敛；
//   - 恢复用 NtResumeProcess，只递减一次挂起计数，不会把别的组件（调试器、
//     系统）挂起的线程误恢复；
//   - 已挂起应用每 2s 复核时做一次「补挂」：Job 里出现新的成员（例如被
//     另一个未冻结的兄弟进程拉起）就单独挂起，不重复挂起已在跟踪表中的
//     进程（重复 NtSuspendProcess 会让恢复只减一次计数，永久挂死）；
//   - 挂起表持久化到 HKCU\Software\DesktopSuite\Dock\SuspendedState，
//     Dock 被强杀/崩溃后下次启动按 PID+创建时间核对并恢复，避免应用
//     永久停在挂起态。
//
// 安全边界（由 Dock 在调用前再叠加一层）：
//   - 只把「没有可见窗口」（窗口全部最小化或纯托盘）且未被用户豁免的
//     应用交给本模块；
//   - 不碰 MyWigets 自身、explorer、conhost 等系统基础设施；
//   - 不碰当前系统前台应用（即使顶栏目标暂时粘在别的应用上）；
//   - 不碰安全桌面（UAC/锁屏）期间的新挂起请求；
//   - 刚启动的进程有最小年龄保护，避免把正在初始化的应用冻住；
//   - 正常退出/组件重启时恢复所有自己挂起的进程。

#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace appsuspend {

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
};

// 选择结果：processes 是最终要挂起的进程；installRoot/broadRoot 仅用于
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

// 永不属于任何可挂起应用集合的系统/套件进程（按 exe 基名）。
bool IsNeverSuspendImage(const std::wstring& imagePath);

// Dock 侧的暂停策略（纯函数，便于单元测试）：只有「仍然打开、当前没有
// 任何“可见且未最小化”的窗口、且没有被用户单独豁免」的应用才允许暂停。
// hasVisibleWindow 为 true 表示至少有一张可见且未最小化的顶层窗口；
// 窗口全部最小化、或纯托盘驻留（根本没有窗口）时都为 false，可以暂停。
bool ShouldPauseApp(bool opened, bool hasVisibleWindow, bool userExcluded);

// 全量进程快照（忽略其他登录会话、系统基础设施与自身）。
std::vector<ProcessEntry> SnapshotProcesses();

// 按上面的口径，从快照中选出属于 appKey 的进程。纯函数，便于单元测试。
//
// foreignAppKeys：Dock 中其它应用的 key 集合。用于避免把「应用 A 启动的
// 应用 B」误当 A 的辅助进程 —— 典型是 Office 同一安装目录下 WINWORD 拉起
// EXCEL，两者在同一个 Office16 目录树里。子进程镜像恰好是另一个 Dock
// 应用的 key 时，不并入当前应用，也不继续向下遍历它的后代。
//
// includeExternalDescendants=true 时，后代不再受安装目录树限制：应用把
// helper.exe 写到 %TEMP%、AppData 或另一个产品目录后再拉起，仍会被收进来。
// 这是 Job 沙箱不可用时的兜底；Dock 侧还会用「可见窗口」过滤掉用户正在
// 看的外部程序。纯选择函数不接触窗口，所以调用方要自己叠加这层过滤。
Selection SelectAppProcesses(
    const std::wstring& appKey, const std::vector<ProcessEntry>& snapshot,
    const std::unordered_set<std::wstring>* foreignAppKeys = nullptr,
    bool includeExternalDescendants = false);

enum class SuspendCode {
    Suspended,        // 至少挂起了一个进程（整组已挂起）
    AlreadySuspended, // 该应用已经在管理表中
    NoProcesses,      // 找不到属于该应用的进程
    TooYoung,         // 进程刚启动，等过了最小年龄再试
    AccessDenied,     // 有匹配进程，但一个也打不开/受保护，整组放弃
    VisibleWindow,    // 成员里出现了可见且未最小化的窗口，整组放弃
    Failed,           // 其他失败（NtSuspendProcess 不可用等）
};

struct SuspendRequest {
    std::wstring appKey;
    std::wstring displayName;
};

struct SuspendResult {
    SuspendCode code = SuspendCode::Failed;
    size_t matched = 0;
    size_t suspended = 0;   // 本次已挂起的进程数
    size_t skipped = 0;
    size_t jobMembers = 0;  // Job 沙箱当前成员数（0 表示未用 Job）
    bool jobSandbox = false;
    DWORD retryAfterMs = 0;
    std::wstring detail;
};

// 挂起控制器。只应由 Dock 的 UI 线程调用；内部不创建线程、不做轮询。
//
// persist=true 时把挂起表写入
// HKCU\Software\DesktopSuite\Dock\<stateValueName>（默认 SuspendedState），
// 用于 Dock 被强杀后下次启动恢复进程，避免应用永久停在挂起态。
class Suspender {
public:
    // useJobSandbox=true 时优先用 Job Object 做进程围栏：应用主进程进入
    // 每个应用独立的、无 UI 限制/无 breakaway/不随句柄关闭杀进程的 Job，
    // 之后它创建的所有后代（无论 exe 在哪个目录、父进程是否已退出）都
    // 自动留在 Job 里，挂起时按 Job 成员整组冻结。Job 创建/追加失败时
    // 自动退回旧的「快照 + 跨目录后代」选择。
    explicit Suspender(bool persist = true,
                       ULONGLONG minProcessAgeMs = 8000,
                       std::wstring stateValueName = L"SuspendedState",
                       bool useJobSandbox = true);
    ~Suspender();

    Suspender(const Suspender&) = delete;
    Suspender& operator=(const Suspender&) = delete;

    bool Initialize();

    // 一次快照处理多个应用，避免桌面/无焦点时逐个应用重复枚举全进程。
    // 返回与 requests 一一对应的结果。
    // allAppKeys = Dock 当前全部应用 key（含固定但未运行的），用于让
    // SelectAppProcesses 区分「辅助进程」与「被本应用拉起的另一个应用」。
    std::vector<SuspendResult> SuspendMany(
        const std::vector<SuspendRequest>& requests,
        const std::vector<std::wstring>& allAppKeys = {});

    // 启动应用时调用（CreateProcess 得到句柄之后、ResumeThread 之前）：
    // 把主进程直接放进该应用的 Job，保证从第一个子进程起就不会漏。
    // 没有这个调用也能工作，但只能靠「事后 OpenProcess + 追加」，
    // 对拒绝 PROCESS_SET_QUOTA 的进程会退回快照选择。
    bool AdoptProcess(const std::wstring& appKey, HANDLE process,
                      std::wstring* detail = nullptr);

    // 当前 Job 成员数（仅测试/日志用；未启用或创建失败返回 0）。
    size_t JobMemberCount(const std::wstring& appKey) const;

    // 恢复该应用被本控制器挂起的全部进程。个别进程已退出视为成功；
    // 仍存活但恢复失败的条目会保留，等待下次重试。
    bool Resume(const std::wstring& appKey, std::wstring* detail = nullptr);

    void ResumeAll();

    bool IsSuspended(const std::wstring& appKey) const;
    size_t TrackedProcessCount() const;
    std::vector<std::wstring> SuspendedKeys() const;

    // 启动时调用：读取上次异常退出留下的挂起表，按 PID+创建时间核对后
    // 恢复，并清除残留。返回成功恢复的进程数。
    size_t RecoverPersisted(std::wstring* detail = nullptr);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace appsuspend
