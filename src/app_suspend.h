// app_suspend.h - 「未聚焦应用挂起」的进程归属与挂起/恢复
//
// 需求：Dock 里已经打开的应用，一旦不再处于顶栏当前焦点，就暂停它的
// 进程；重新成为焦点时恢复。真正容易出错的地方不是 NtSuspendProcess，
// 而是「哪些进程算这个应用」——
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
// 另外用进程创建时间做 PID 复用防护：挂起/恢复前重新打开进程并核对
// 创建时间，绝不把「同 PID 的新进程」当成原来那个进程。
//
// 安全边界（由 Dock 在调用前再叠加一层）：
//   - 不碰 MyWigets 自身、explorer、conhost 等系统基础设施；
//   - 不碰当前系统前台应用（即使顶栏目标暂时粘在别的应用上）；
//   - 不碰安全桌面（UAC/锁屏）期间的新挂起请求；
//   - 刚启动的进程有最小年龄保护，避免把正在初始化的应用冻住；
//   - 退出/崩溃恢复时恢复所有自己挂起的进程。

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

enum class SuspendCode {
    Suspended,        // 至少挂起了一个进程
    AlreadySuspended, // 该应用已经在管理表中
    NoProcesses,      // 找不到属于该应用的进程
    TooYoung,         // 进程刚启动，等过了最小年龄再试
    AccessDenied,     // 有匹配进程，但一个也打不开（权限/受保护进程）
    Failed,           // 其他失败
};

struct SuspendRequest {
    std::wstring appKey;
    std::wstring displayName;
};

struct SuspendResult {
    SuspendCode code = SuspendCode::Failed;
    size_t matched = 0;
    size_t suspended = 0;
    size_t skipped = 0;
    DWORD retryAfterMs = 0;
    std::wstring detail;
};

// 挂起控制器。只应由 Dock 的 UI 线程调用；内部不创建线程。
//
// persist=true 时把挂起表写入
// HKCU\Software\DesktopSuite\Dock\<stateValueName>，用于 Dock 被强杀后
// 下次启动恢复进程，避免应用永久停在挂起态。
class Suspender {
public:
    explicit Suspender(bool persist = true,
                       ULONGLONG minProcessAgeMs = 8000,
                       std::wstring stateValueName = L"SuspendedState");
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
