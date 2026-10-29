# Dock 后台应用挂起：Job Object 沙箱方案

日期：2026-10-29

## 1. 问题

Dock 现有的后台应用挂起按“快照 + 进程树/安装目录”认定进程：

- 同一 exe 的全部实例；
- 从这些实例出发、仍在安装目录树内的后代。

这无法覆盖两类真实情况：

1. **异步派生**：快照枚举之后、冻结生效之前，父进程刚创建了子进程；
   旧实现只冻结快照里的句柄，新子进程继续运行。
2. **跨目录派生**：应用把 helper.exe 写到 `%TEMP%`、`%LOCALAPPDATA%`、
   更新目录或另一个产品目录，再从那里启动；按安装目录树判断会漏掉。
3. **父进程退出**：启动器/主进程退出后 helper 还在跑；按 exe 路径
   已经找不到“主进程”，旧实现直接返回 `NoProcesses`。

关键结论：**“哪些进程属于这个应用”不能靠枚举快照来回答**。枚举天然有
竞态，而且目录/文件名不是可靠的归属边界。需要一个由内核维护、随进程
创建自动扩展的归属容器 —— Windows 的 **Job Object**。

## 2. 方案对比

| 方案 | 能否覆盖异步/跨目录子进程 | 父进程退出后 | 代价/限制 |
|---|---|---|---|
| 快照 + 进程树（现状） | 否，有枚举竞态 | 否 | 无额外权限；无法保证“全部” |
| 每 2s 轮询补挂 | 2s 窗口内漏 | 否 | 仍有窗口；进程可短命 |
| WMI `Win32_ProcessStartTrace` | 事件延迟、可能丢 | 部分 | 通常需管理员；WMI 创建的子进程不继承 Job |
| ETW `Microsoft-Windows-Kernel-Process` | 可实时 | 可 | 需提权/复杂会话管理；仍有归因问题 |
| 内核 `PsSetCreateProcessNotifyRoutineEx` 驱动 | 可 | 可 | 需签名驱动；安全软件敏感；维护成本高 |
| 注入 `CreateProcess` 钩子 | 可 | 可 | 注入第三方进程，易被安全软件拦截 |
| **Job Object 沙箱（本方案）** | **是，内核在创建时自动继承** | **是，Job 成员与父进程无关** | 只能覆盖后代；关联不可撤销；部分受限进程无法追加 |

Job Object 是 Windows 原生的“进程组/沙箱”原语。把应用主进程放进一个
Job 后，默认所有子进程在创建时自动进入同一个 Job；父进程退出不会让
子进程脱离；子进程再把 exe 落到任何目录、或再开孙进程，仍自动在 Job 里。
这正是“总是挂起应用的全部进程”需要的语义。

## 3. 实现

### 3.1 每个应用一个 Job

`appsuspend::Suspender` 内部维护 `appKey -> HANDLE job`：

- `CreateJobObjectW(nullptr, nullptr)`；
- `JOBOBJECT_EXTENDED_LIMIT_INFORMATION.BasicLimitInformation.LimitFlags = 0`：
  - **不设** `KILL_ON_JOB_CLOSE`：Dock 退出/崩溃不会杀应用；
  - **不设** `BREAKAWAY_OK`：子进程不能主动脱离（见 5.2）；
  - **不设** `JobObjectBasicUIRestrictions`：否则 Chromium/Electron 自己
    再嵌套 sandbox Job 时会失败；
- Job 句柄只由 Dock 持有，不继承给子进程；Dock 退出时关闭句柄，
  进程继续运行。

### 3.2 启动时入 Job（最优路径）

`dock_main.cpp::LaunchItem` 从 `ShellExecuteW` 改为
`ShellExecuteExW(SEE_MASK_NOCLOSEPROCESS)`，拿到目标进程句柄后调用：

```cpp
g_suspender.AdoptProcess(item.key, sei.hProcess, &detail);
```

`AdoptProcess` 把主进程 `AssignProcessToJobObject`。这样从目标进程的
第一个子进程开始，后代就自动在 Job 里，不依赖之后枚举。

对 URL、DDE、UWP 等拿不到进程句柄的启动方式，退回旧行为；首次挂起时
仍会走 3.3 的事后追加。

### 3.3 首次挂起时事后追加

应用不是 Dock 启动的（Explorer、开机自启、更新器拉起）时，主进程不在
Job 里。`SuspendMany` 在第一次处理该应用时：

1. `EnsureJob` 创建/复用 Job；
2. `AdoptIntoJob` 用快照选择“主 exe 全部实例 + 跨目录后代”，按创建时间
   从父到子依次 `OpenProcess(PROCESS_SET_QUOTA|PROCESS_TERMINATE)` +
   `AssignProcessToJobObject`；
3. 只追加**不拥有可见且未最小化顶层窗口**的非根后代；这类进程是用户
   正在看的另一个应用，不应当作本应用 helper；
4. 单个进程追加失败（受保护/无权限/已在互斥 Job 中）只记录失败，不阻断
   其余成员；Job 是增强路径，挂起仍有快照兜底。

### 3.4 挂起：Job 成员 ∪ 快照，冻结后补冻

`BuildCandidates`：

- `JobObjectBasicProcessIdList` 取 Job 当前成员（含子 Job）；
- `SelectAppProcesses(..., includeExternalDescendants=true)` 取快照选择；
- 合并、按 PID+创建时间去重；
- 过滤：
  - 永不挂起的系统/套件进程；
  - 另一个 Dock 应用及其整棵后代；
  - 已由别的应用挂起的进程；
  - 拥有可见且未最小化窗口的成员（整组放弃，`VisibleWindow`）；
  - Job 中无法核对创建时间的进程（整组放弃，`AccessDenied`，避免只冻一半）。

两阶段冻结：

1. 先把整组进程打开并核对（PID+创建时间、受保护进程、权限），任一失败
   整组放弃；
2. 统一 `NtSuspendProcess`；
3. **Job 补冻循环**：重新枚举 Job 成员。父进程在“枚举之后、冻结生效之前”
   创建的子进程已经进入 Job，但不在第一批句柄中；对新增成员继续
   `NtSuspendProcess`，直到没有新成员（最多 8 轮）；
4. **兜底快照补冻**：Job 追加失败的成员不在 Job 里，Job 枚举看不到它们
   在冻结边界刚创建的子进程。全部成员冻结后再做一次（最多几轮）完整快照
   选择，把新出现的后代补冻。所有父进程已冻结，补完即收敛。Job 正常时
   这一轮通常没有新增；
5. 任一步失败立即回滚本次已挂起的成员；回滚失败的保留跟踪，退出时
   `ResumeAll` 再试。

### 3.5 不做周期补挂

已挂起应用在 Dock 的 2s 复核里直接返回 `AlreadySuspended`，不再枚举
Job/进程、不再做快照。理由：

- 能创建进程的成员都已经冻结；冻结后的进程不可能再 `CreateProcess`；
- Job 新成员只可能出现在“枚举之后、冻结生效之前”的极窄边界，已由 3.4
  的 Job 补冻循环覆盖；
- Job 追加失败、只能靠快照兜底的成员，也在**挂起收尾阶段**用一次完整
  快照补冻（最多几轮）覆盖，不依赖时间轮询。

因此没有 `TopUpSuspended`、没有每 2s 的全量进程快照，也没有新增线程/定时器。

### 3.6 恢复

`Resume` 不变：按跟踪表 PID+创建时间 `NtResumeProcess`，逐个递减一次
挂起计数；进程已退出视为成功。Job 成员关系保留，恢复后新创建的子进程
仍会自动进入 Job，下一次挂起继续覆盖。

### 3.7 持久化

挂起表仍写 `HKCU\Software\DesktopSuite\Dock\SuspendedState`。Dock 被强杀
时 Job 句柄随之关闭，但进程仍停在挂起态；下次启动 `RecoverPersisted`
按 PID+创建时间恢复。Job 没有 `KILL_ON_JOB_CLOSE`，不会因 Dock 崩溃杀进程。

## 4. 安全边界

在原有边界上新增：

- **可见窗口**：Job 成员里只要有一张可见且未最小化、非 cloak、非零尺寸的
  顶层窗口，整组放弃。用于兜住“应用顺手拉起浏览器/播放器/安装器”以及
  更新器弹窗；
- **另一个 Dock 应用**：按 `allAppKeys` 排除其整棵子树，即使 Job 继承把它
  带进来也不冻结；
- **不可核对成员**：Job 中有进程打不开/无法核对创建时间时整组放弃，避免
  “主进程冻了、helper 还在跑”的半冻结；
- **最小进程年龄**、**安全桌面**、**前台/顶栏焦点保护**、**用户按应用豁免**
  保持不变。

## 5. 已知限制与取舍

### 5.1 Job 关联不可撤销

进程一旦进入 Job，除非退出，否则不能移出。因此：

- 应用拉起的外部程序如果在追加时**没有可见窗口**（例如最小化的浏览器），
  可能被圈进 Job；之后它最小化时可能被一起挂起；
- 同一个 Job 里的另一个 Dock 应用会被 4 的过滤排除，不会冻结；但它仍在
  Job 里，父应用退出后 Job 仍持有它（不会杀，只是归属）。

缓解：可见窗口过滤 + 其它 Dock 应用整棵排除 + 单应用“不暂停此应用”豁免。
如果发现某个应用把外部程序拉进 Job 导致误冻，先给该应用加豁免。

### 5.2 不允许 breakaway

Job 不设 `BREAKAWAY_OK`，所以应用调用
`CreateProcess(CREATE_BREAKAWAY_FROM_JOB)` 会失败（或子进程留在 Job）。
绝大多数应用不主动 breakaway；Chromium/Electron 的 renderer 是正常继承，
它们自己再建子 Job 也不受 UI 限制影响。

如果实测某个应用因此启动/更新失败：
- 优先给该应用加“不暂停此应用”豁免；
- 或后续增加注册表开关，对该应用改用 `JOB_OBJECT_LIMIT_BREAKAWAY_OK`
  （代价是它 breakaway 出来的子进程不再被跟踪）。

### 5.3 非后代、非同名 exe 的进程无法归因

Job 只能覆盖“从主进程派生”的进程。以下仍无法自动认定：

- 由独立服务/计划任务启动、与主进程没有父子关系的 helper；
- 另一个 exe 但确实属于该应用、又不是后代（例如某些驱动/服务）。

这类进程需要应用自己暴露身份（AUMID/命名管道/包身份）或用户手动豁免；
单靠路径/进程树无法可靠解决。

### 5.4 权限边界

- 受保护进程、跨完整性级别/elevated 进程可能拒绝
  `PROCESS_SET_QUOTA`/`PROCESS_SUSPEND_RESUME`；
- 此时 `AdoptIntoJob` 追加失败，`BuildCandidates` 若 Job 中有不可核对成员
  会整组放弃；不会出现半冻结；
- UWP/AppContainer 的 Job 追加可能失败，退回快照选择。

### 5.5 系统版本

嵌套 Job 需要 Windows 8+；本套件目标 Windows 10/11，满足。Windows 7
不支持嵌套 Job，本方案不适用。

## 6. 测试与验证

新增/扩展 `tests/app_suspend_test.cpp`：

1. `SelectAppProcesses(..., includeExternalDescendants=true)` 选中跨目录后代；
2. **Job 沙箱集成**：
   - 主进程 `CREATE_SUSPENDED`，`AdoptProcess` 入 Job，再 `ResumeThread`；
   - 主进程从 `dir\outside\helper.exe` 拉起不同目录 helper；
   - `JobMemberCount >= 2`；
   - `SuspendMany` 返回 `Suspended`、`jobSandbox=true`、`jobMembers>=2`；
   - 主进程与跨目录 helper 心跳同时停止，恢复后同时继续；
3. **重复挂起幂等**：第二次 `SuspendMany` 返回 `AlreadySuspended`，
   不会重复 suspend（否则恢复后无法解冻）；
4. **父进程退出**：主进程 spawn helper 后退出，`JobMemberCount>=1`；
   `SuspendMany` 在没有主进程快照条目的情况下仍按 Job 成员挂起 helper，
   心跳停止/恢复；
5. 原有选择、暂停策略、同目录 helper、持久化恢复用例全部保留。

本机（Windows 11 25H2）实测结果：

```text
[PASS] 跨目录 helper 自动继承 Job
[PASS] Job 成员（含跨目录 helper）整组挂起
[PASS] 重复 SuspendMany 幂等，不重复冻结
[PASS] 无主进程时按 Job 成员挂起 helper
[PASS] 父退出场景 helper 心跳停止
ALL PASS
```

完整 `MyWigets-x64.exe` Release 构建通过。

## 7. 部署与观察

1. `powershell -NoProfile -ExecutionPolicy Bypass -File build_mywigets.ps1`
2. 用现有 `deploy_mywigets.ps1` 覆盖部署，重启 MyWigets；
3. 打开“后台应用挂起”，最小化 Chrome/Electron/微信/钉钉等，观察
   `logs` 中的：
   - `[挂起] 启动后加入 Job：...`
   - `[挂起] ... Job 追加 N；... Job 成员 M -> 待挂起 K；已挂起 K`
4. 重点回归：
   - Chrome/Edge 多标签、渲染进程；
   - 微信/钉钉的 AppEx/临时目录 helper；
   - 带更新器的应用（更新器在 `%TEMP%` 启动）；
   - Alt+Tab、触摸板切换、托盘唤起仍能及时恢复；
   - 关闭/重启 Dock 后没有应用永久停在挂起态。

## 8. 后续可选项

- 注册表/右键菜单增加“Job 沙箱”总开关或按应用 breakaway 兼容开关；
- 用 `JOBOBJECT_ASSOCIATE_COMPLETION_PORT` 监听 `JOB_OBJECT_MSG_NEW_PROCESS`
  作为可观测性/调试手段；当前正确性不依赖它：挂起收尾补冻后所有成员已冻结，
  不会再有新进程，运行期的子进程也已由 Job 自动继承；
- 对 UWP 用包身份（Package SID/AUMID）补充归因，减少对进程树的依赖。
