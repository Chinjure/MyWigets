# Dock 关闭后清理：可见关闭即零进程

日期：2026-10-30  
代码：`src/app_cleanup.h`、`src/app_cleanup.cpp`、`Dock` 事件调度

## 1. 目标

对 Dock 中固定的第三方应用，在「可见关闭」状态下不保留任何可归因进程：

- 没有可见顶层窗口（包括最小化窗口）；
- 没有存活托盘图标；
- 不在启动中/切换/前台/安全桌面；
- 用户没有正在通过 Dock 与该应用交互。

「不暂停此应用（后台保持运行）」**只影响挂起**，不影响关闭后清理；清理
路径没有 `userExcluded` 参数。

## 2. 与挂起的关系

- 挂起：应用仍有最小化窗口或托盘图标，只是不在焦点 → 冻结；
- 清理：应用已无窗口/托盘 → 一次性结束全部可归因进程；
- 清理终止的进程如果仍在 `Suspender` 跟踪表里，下一次 `SuspendMany`
  会按「已退出」清掉；清理器不调用 `Suspender` 的挂起/恢复 API。

## 3. 无轮询策略（硬约束）

不允许用周期扫描/重试来关闭反复重启的进程。

- 触发只来自事件：WinEvent 的窗口 `DESTROY/HIDE/CLOAKED`、最小化结束、
  前台切换；托盘注册表监听；Dock 启动 30s 后一次性 sweep；用户显式关闭；
- `RefreshItems` 只在「打开 → 可见关闭」迁移时登记一次 5s 宽限；
- 宽限到期由一次性 `SetTimer(kResidualTimerId)` 触发；
- 宽限/复审之后不再有周期调用，不在 2s/60s 保底里调用清理；
- 一次关闭 episode 只允许一次清理；失败/权限不足直接标记 `Blocked`；
- 关停反复重启：把候选加入瞬时 Job，`TerminateJobObject` 一次原子终止；
  应用私有服务作为源头用 `ControlService(STOP)` 停一次。服务拒绝访问时
  报 `NeedsElevation`，不进入 kill-retry 循环。

代码审查规则：清理路径不得新增周期定时器、不得有
`while (process exists) kill`、不得维护 `retryCount/nextAttempt`。

## 4. 进程归因

| 证据 | 处理 |
|---|---|
| 精确 key（含 pinKey/运行 key） | 必收 |
| Job 成员（Dock 启动/接管的后代） | 必收、去重 |
| A/B 的后代（允许跨目录） | 必收 |
| 窄安装根内镜像（父进程已退出） | 必收；根 broad/共享时禁用 |
| 应用私有服务宿主（own-process） | 先停服务，宿主不作为普通 kill 目标 |
| 服务后代 | 必收 |
| 产品家族服务（如 QuarkCloudDrive 与 QuarkCloudDriveUpdater） | 按产品 token/显示名匹配后停服务 |

排除：系统/套件/基础设施、受保护关键进程、其它 Dock 应用及后代、其它
用户会话、宽泛安装根、`WindowsApps` UWP、任何拥有可见/最小化/cloaked
窗口的候选。窗口和托盘是硬边界：出现任何一个就整组放弃。

## 5. 托盘判定

1. Dock 的 `trayMarked` 为真 → 有托盘；
2. UIA 枚举可见托盘区/已打开溢出区，复用 `TrayIconMatchesApp`；
3. UIA 不可用时按注册表同 basename 匹配，但要求仍有用户会话进程；
4. 不确定 → 不清理，等下一次事件；不轮询。

这解决钉钉 `current_new` vs `current` 这类注册表旧路径导致的误判。

## 6. 服务与权限

- 只处理 `SERVICE_WIN32_OWN_PROCESS`、非驱动、`SERVICE_ACCEPT_STOP`、
  无 active dependents 的应用私有服务；共享宿主（svchost）、安全软件、
  Windows 服务不碰。
- 停止等待使用 `NotifyServiceStatusChange`/进程句柄，不做状态轮询。
- 标准用户无法停止 LocalSystem 服务（例如 QuarkCloudDriveUpdater）时：
  结果 `NeedsElevation`，日志列出服务名/PID/路径；Quark 门禁失败，
  不使用轮询反复杀子进程来“绕过”源头。

完整服务启动类型管理见 [service-guard.md](service-guard.md)：安装一次性
计划任务 broker 后，关闭时停止并临时禁用应用私有服务，应用打开时恢复
原启动类型；安装/卸载需要一次 UAC，日常关闭/启动不需要。

### 6.1 一次性提权助手

`MyWigetsCleanup.exe`（`src/cleanup_admin.cpp`，`requireAdministrator`）
是用户显式运行的一次性助手，不是常驻服务：

```powershell
Start-Process .\bin\MyWigetsCleanup.exe `
  -ArgumentList @('--app','C:\Program Files\QuarkCloudDrive\quark_cloud_drive.exe') `
  -Verb RunAs -Wait
```

它复用 `appcleanup::CleanupOnce`（`stopServices=true`、`minProcessAgeMs=0`），
停在源服务并终止残留；执行完退出。Dock 的自动路径在权限不足时只报
`NeedsElevation`，不会静默失败，也不会自动弹 UAC 或反复重试。

## 7. 夸克网盘验收（阻断项）

脚本：`tests/quark_residual_test.ps1`

1. 备份 Dock 配置，临时 `ResidualCleanup=1`，通过 WM_COPYDATA 固定夸克；
2. 启动夸克，等待主窗口；
3. 完全退出（窗口和托盘图标都消失）；
4. 等 `[清理] ... Quark/夸克` 日志，采样 10s：
   - `C:\Program Files\QuarkCloudDrive`（含 `7.4.2.822`、`components`、
     `Resources`）下无进程；
   - 无 `quark_cloud_drive*`、`notification_helper.exe`、`apollocmd.exe`、
     `quark_cloud_drive_pwa_launcher.exe`、
     `clouddrive_desktop_widget_independent.exe`、`media_container.exe`、
     `quark_host_client.exe`；
   - `QuarkCloudDriveUpdaterService1.0.0.11` 与
     `...InternalService...` 均非 Running；
5. 清理后重新启动夸克正常。

测试采样是测试观察，不是产品清理逻辑；产品路径没有轮询。脚本在发现
Quark updater 服务仍 Running 时会尝试 `MyWigetsCleanup.exe -Verb RunAs`
（会弹一次 UAC）；用户拒绝则测试如实失败，不会把 SYSTEM 服务残留
伪装成通过。

## 8. 配置与日志

注册表 `HKCU\Software\DesktopSuite\Dock`：

| 值 | 类型 | 默认 | 含义 |
|---|---|---|---|
| `ResidualCleanup` | DWORD | 0 | 关闭后清理总开关 |

`SuspendException*` 保持原样，仅用于挂起。日志前缀 `[清理]`，包含匹配/
终止/拒绝/服务停止计数和 `NeedsElevation` 提示。

## 9. 已知边界

- Dock 退出后不继续清理；启动 30s sweep 只补一次。
- UIA 托盘判定失败时保守漏杀；不通过轮询补偿。
- 其它虚拟桌面（cloaked）窗口算打开，不清理。
- UWP/Store 第三方应用初版不纳入。
- SYSTEM 服务自动清理需要额外提权组件；本轮提供一次性提权助手
  `MyWigetsCleanup.exe`，Dock 自动路径只做 `NeedsElevation` 报告。
