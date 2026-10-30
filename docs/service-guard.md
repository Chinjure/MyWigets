# 应用服务守护（禁止应用服务拉起后台进程）

日期：2026-10-30  
代码：`src/service_guard.cpp`、`src/app_cleanup.cpp`、`src/dock_main.cpp`

## 1. 目标

应用可见关闭后，除了结束残留进程，还要阻止它的 **Windows 服务**在后台
继续拉起进程。做法是：

1. 停止匹配到的应用私有服务；
2. 把服务启动类型临时改为 `Disabled`，防止 SCM 自动启动/失败恢复再拉起；
3. 应用下次打开时恢复原启动类型；如果关闭前服务在运行，则 `StartService`
   恢复；
4. 状态持久化，Dock 重启后仍能在应用打开时恢复。

这不是常驻服务，也不是轮询：Dock 只写一次请求并触发一次计划任务，broker
执行完退出。

## 2. 安装（一次性 UAC）

**分发要求**：`MyWigetsServiceGuard.exe` 必须与 `MyWigets.exe` 放在同一目录
（`deploy_mywigets.ps1` 会自动复制）。Dock 菜单会优先在当前 exe 目录查找
它；找不到时才回退到 `clock\bin`。普通用户不需要运行任何测试脚本。

Dock 右键菜单：

- **安装应用服务守护（需要管理员）**：复制
  `MyWigetsServiceGuard.exe` 到 `%ProgramFiles%\DesktopSuite`，创建计划任务
  `DesktopSuite\ServiceGuardBroker`，并开启 `ServiceGuardEnabled`。
- **卸载应用服务守护**：恢复所有被禁用的服务，删除任务和文件。
- **修复此应用的服务启动类型（需要管理员）**：对旧版本遗留的错误 state
  做一次 `--install --repair-auto`，无需测试脚本。

安装只在此时需要 UAC。之后关闭/启动应用不需要再确认。

计划任务：

- 以当前用户、**最高权限**运行；
- **无定时/登录触发器**，只在 Dock 通过 `schtasks /run` 按需触发；
- 任务 XML 显式关闭 `DisallowStartIfOnBatteries` /
  `StopIfGoingOnBatteries` / `StopOnIdleEnd`，避免笔记本电池模式下
  任务长期 Queued、broker 根本不执行；
- 动作固定为 `MyWigetsServiceGuard.exe --broker`，不能传入任意命令。

如果旧版本安装的任务在电池模式下一直处于 `Queued`，用 Dock 菜单
**「修复应用服务守护（重新创建任务）」** 重装一次即可。

## 3. 请求与状态

- 请求：`%LOCALAPPDATA%\DesktopSuite\ServiceGuard\request.txt`
  ```text
  action=disable | restore | restore-all
  app=<归一化 exe 路径>
  pin=<固定项 exe 路径，可为空>
  ```
- 结果：同目录 `result.txt`；broker 还会向 `DesktopDockWindow` 投递
  `DesktopSuite.ServiceGuardDone`。
- 状态：`%ProgramData%\DesktopSuite\ServiceGuard\state.txt`
  ```text
  app|serviceName|originalStartType|wasRunning
  ```
  用于恢复原 `Auto/Manual/Disabled` 和关闭前是否在运行。

## 4. 服务范围与安全校验

只处理：

- `SERVICE_WIN32_OWN_PROCESS`；
- 非驱动、非共享 svchost、非 Windows 系统目录；
- 有 `SERVICE_ACCEPT_STOP`、无 active dependents；
- 二进制位于应用窄安装根内，或产品家族匹配
  （例如 `QuarkCloudDrive` 与 `QuarkCloudDriveUpdater`）。

broker 会拒绝：

- 应用路径在 Windows 目录；
- 安装根 broad（例如直接位于 `%ProgramFiles%`、用户目录根）；
- 非 `.exe`、不存在的应用。

即使 request 文件对同用户可写，broker 也只会在上述范围内改服务；
不会接受任意服务名或任意命令。

## 5. 与清理的配合

- 应用进入「无窗口、无托盘」的可见关闭状态后，清理 worker 结束进程；
  清理结果落定后，Dock 对固定第三方应用发出 `disable` 请求。
- 应用重新打开（Dock 启动、图标启动、或窗口出现）时发 `restore`；
  `LaunchItem` 在启动前也会先发一次，尽量减少“服务还被禁用”的窗口。
- 服务守护只管理“应用私有服务”；共享服务、Microsoft/安全服务不碰。
- 清理结果中的 `NeedsElevation` 不影响服务守护：broker 本身是最高权限，
  可以继续停服务。

## 6. 已知限制

- 必须先在菜单安装守护；未安装时 Dock 只做普通清理，不会改服务启动类型。
- 任务以当前用户最高权限运行，不是 LocalSystem；如果某服务 ACL 连管理员
  都拒绝修改，broker 会报告失败，不会无限重试。
- 不管理启动类型以外的服务配置（依赖、失败恢复、ACL、驱动）。
- 不监听服务状态变化；服务被人为/其它程序重新拉起后，不通过轮询补禁，
  由下一次关闭或手动处理。
- 卸载时会尽力恢复 state 中的服务；若服务已被删除，state 中对应项会保留
  直到下次成功恢复/手动清理。

## 7. 验收

```powershell
# 安装（Dock 菜单也可）：会弹一次 UAC
Start-Process .\bin\MyWigetsServiceGuard.exe -ArgumentList '--install' -Verb RunAs -Wait

# 查看 Quark 归属服务
.\bin\app_cleanup_test.exe --services "C:\Program Files\QuarkCloudDrive\quark_cloud_drive.exe"

# 关闭 Quark 后确认：
# - state.txt 出现 QuarkCloudDriveUpdaterService... 行，StartType=Disabled
# - 服务 State=Stopped，StartMode=Disabled
# - 重新打开 Quark 后恢复 Auto 并 StartService（若关闭前在运行）
```
