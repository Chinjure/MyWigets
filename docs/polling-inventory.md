# 套件空闲轮询/定时唤醒清单（功耗排查）

日期：2026-10-30  
代码：`HEAD 5c2ad41`（`bin\MyWigets-x64.exe` 20:53 构建，桌面副本自 20:56 起运行）  
动机：空闲包功耗（`\Energy Meter(*)\Power` 的 `rapl_package0_pkg`）隔一段时间突破 0.4 W，
怀疑 Dock 栏轮询导致。本文先把**所有周期性唤醒**统计清楚，再给实测开销与排序。

## 0. 结论

- 空闲时套件**合计约 0.44% 单核**、约 **6~7 次/秒** 定时器唤醒；
  其中 **Dock 线程占 0.37%**，是绝对大头（其余四个线程加起来 0.06%）。
- Dock 线程的周期开销分两类：
  1. **`kCollapseWatchTimerId` @2 s，常驻**：实测 **≈0.8 ms/次**（30 次/分钟 ≈ 24 ms），
     是"隔一段时间"最稳定的那条节拍；
  2. **事件合并后的 `DoPoll`/`RefreshItems`**：实测 **20~40 ms/次**（一次跑
     `EnumWindows`×2 + 每窗口 DWM cloak 查询 + 托盘注册表枚举 + 每 3 s 一次全进程枚举），
     这是**单次尖峰最高**的项，60 s 里出现 5~6 次。
- 真正"零成本"的只有事件钩子/阻塞等待（WinEvent、两个低级钩子、托盘注册表
  `RegNotifyChangeKeyValue`、清理 worker 的条件变量）；挂起/清理/服务守护模块
  **没有任何常驻轮询**（见 §6）。
- 若把 0.4 W 的瞬时突破当作判据，**最可疑的是上面第 2 项**：40 ms × ~2 W ≈ 80 mJ，
  在 1 s 分辨率的功耗曲线上就是一次 +60~80 mW 的鼓包；第 1 项（0.8 ms）单独看不
  足以把 1 s 平均值推过 0.4 W，但它是最稳定的唤醒源，会把 CPU 反复拽出深睡态。

## 1. 测量方法

- 采样工具：自写 `WakeSample.exe`（`QueryThreadCycleTime` 每 20 ms 采一次，按线程记账）
  与 `PowerSample.exe`（PDH `\Energy Meter(*)\Power` + `GetProcessTimes`），
  源码见 `bench\polling\`（`PowerSample.cs` / `WakeSample.cs`，原始样本 `idle1.csv` / `wake1.csv`）。
- TSC 频率实测标定 **3.068 GHz**（忙循环对照 `GetTickCount64`），cycles 换算成 ms。
- 采样窗口 60 s，机器空闲、仅 DSH GUI 自身在跑（约 13% 单核，与本进程无关，
  按线程记账不受影响）。
- 运行配置（`HKCU\Software\DesktopSuite`）：`KeepTop=1`、`AutoCollapse=0`、
  `SuspendInactive=1`、`ResidualCleanup=1`、`ServiceGuardEnabled=1`；
  `ComponentsOff=3` → **时钟、日历已关闭**，在跑的是 应用管理 / 顶栏 / Dock。
- 线程↔组件对应关系用 `GetWindowThreadProcessId` 按窗口类名核对。

### 实测（60 s）

| 线程 | 组件 | 有活动的采样窗口 | CPU（60 s） | 占单核 | 主要节拍 |
|---|---|---:|---:|---:|---|
| 12192 | Dock UI | 57 | **225 ms** | **0.37%** | 2.0 s 常驻 + 事件刷新尖峰 |
| 12092 | 顶栏 UI | 93 | 13.1 ms | 0.02% | 1.0 s |
| 5576 | 启动台 UI | 117 | 11.6 ms | 0.02% | 0.5 s |
| 12140 | 顶栏·Chrome 同步线程 | 118 | 8.8 ms | 0.014% | 0.5 s（`select`） |
| 11840 | 宿主（托盘窗口） | 60 | 3.7 ms | 0.006% | 1.0 s |
| 合计 | | | **262 ms** | **0.44%** | |

Dock 线程逐次唤醒（ms，截取 0~12 s）：`0.77 / 23.3+8.4（刷新尖峰） /
14.2+10.0（刷新尖峰） / 0.77 / 0.77 / 0.76 / 0.70 / 0.79 …`——
**2 s 定点 ≈0.8 ms，刷新尖峰 20~40 ms**，两者量级差 30~50 倍。

## 2. Dock 线程（`dock_main.cpp`）全部定时器

常驻 = 启动后一直挂着的；一次性 = 到期即 `KillTimer`（不属于轮询，列出仅为完整性）。

| 定时器 ID | 常量/周期 | 常驻? | 到期做的事 | 实测/估算 |
|---|---|---|---|---|
| 1 `kFrameTimerId` | 15 ms（动画帧） | 否，仅动画/弹跳/拖拽/悬停 | `FrameTick`：收起缓动、`UpdateLayoutOneFrame`、`MakeSignature`、按需 `DrawAndPresent`（整窗 ULW 提交） | 空闲已停帧（实测无 15 ms 节拍）；动画期间单帧 ≈0.1~0.5 ms + 一次分层图提交 |
| 2 `kRefreshTimerId` | 一次性，≤1000 ms | 否（事件合并） | `DoPoll`（见 §3） | 200~400 ms 内的一次合并，成本并入 §3 |
| 3 `kSafetyTimerId` | **60 000 ms** | **是** | `DoPoll`：整轮刷新 + 任务栏隐藏自愈 + 组件窗口自愈 + 全屏复核 + 挂起复核 | **20~40 ms/次**（= `RefreshItems` 主导） |
| 4 `kTooltipTimerId` | 一次性 480 ms | 否 | 踢一帧画名字提示 | 一帧的重绘 |
| 5 `kCollapseWatchTimerId` | **2000 ms** | **是** | ①`fsguard::Detect()` ②`ReloadDockConfigFromRegistry`（`RegOpenKeyEx` + 5×`RegQueryValueEx`） ③`keeptop::IsEnabledFromRegistry`（再 1 次开键+读） ④`UpdateKeepTopWorkArea`→`ApplyWorkArea`（`SPI_GETWORKAREA` 比较；目标不一致时**写工作区 + `Sleep(60)` + `EnumWindows` 重排所有最大化窗口**） ⑤`ReconcileAppSuspend` ⑥`CollapseWatchdog`（`GetCursorPos`） | **≈0.8 ms/次 → 24 ms/分钟**；命中④的"不一致"分支时单次 60 ms+ 且会移动窗口（当前工作区 = 0,40-1707,1007 与目标一致，未命中） |
| 6 `kSuspendTimerId` | 一次性（宽限/重试，≤1 s） | 否 | `ReconcileAppSuspend` | 仅在有候选项时挂 |
| 7 `kResidualTimerId` | 一次性 50~5000 ms | 否 | `ReconcileResidualDue` | 仅关闭应用后 5 s 内挂一次 |
| 8 `kResidualStartupTimerId` | 一次性 30 000 ms | 否（启动一次） | 启动 sweep | 启动后 30 s 一次 |

Dock 线程上另有**常驻阻塞/事件**源，不算轮询：

- `TrayRegistryWatchThread`：`RegNotifyChangeKeyValue(fAsynchronous=FALSE)` 阻塞等待，
  只有注册表键打不开/调用失败才 `Sleep(30000)` 重试（正常路径 0 唤醒）。
- 清理 worker（`g_residualThread`）：`condition_variable::wait` 阻塞，任务来了才醒。
- `ShowDesktopHookThreadProc`：`WH_MOUSE_LL` 低级鼠标钩子的消息循环，随输入触发。
- 5 个 `SetWinEventHook`：窗口显示/隐藏/前台切换/切换器 cloak 回调，事件驱动。

## 3. `DoPoll` / `RefreshItems` 为什么一次要 20~40 ms

`DoPoll`（dock_main.cpp:7712）依次执行：

1. `RefreshItems`（:2940）——`EnumWindows` 遍历全部顶层窗口，**每个窗口**
   `DwmGetWindowAttribute(DWMWA_CLOAKED)` + `GetWindowLongPtr` + `GetClassName`，
   可见普通窗口再 `OpenProcess`+`QueryFullProcessImageNameW`（UWP 还要
   `EnumChildWindows` 找真实进程；UI 宿主辅助进程要建一次全进程快照走父链）；
2. 托盘来源：`ReadRegisteredTrayRawPaths` 枚举 `NotifyIconSettings` 全部子键并逐个开键读值；
3. `CollectRunningExePaths`：**每 3 s 缓存过期就重扫全部进程**
   （`CreateToolhelp32Snapshot` + 每进程 `OpenProcess`/`QueryFullProcessImageNameW`，
   命中托盘候选的还要读命令行）——单次约 10~20 ms；
4. `EnsureWindowSize`、`EnsureTaskbarHidden`（`FindWindow`×2 + `SPI_GETWORKAREA`）、
   `RefreshSuiteWindowCache`（再一次 `EnumWindows`）、`HealMinimizedSuiteWindows`、
   `RefreshPrimaryWorkArea`、`UpdateFullscreenState`（`SHQueryUserNotificationState`）、
   `ReconcileAppSuspend`。

入口有两个：**60 s 保底**（上面第 3 号定时器）与 **`kMsgRefresh` 事件**
（WinEvent → 合并窗口最小 1 s）。后者才是尖峰的主要来源：桌面上任何窗口的
显示/隐藏/改名都会触发一次 20~40 ms 的刷新。

## 4. 顶栏（`topbar_main.cpp`）定时器

| ID | 常量/周期 | 常驻? | 每次做的事 | 实测/估算 |
|---|---|---|---|---|
| 5 `kClockTimerId` | **1000 ms** | **是**（窗口隐藏也走） | `TargetOverlapsBar`（`DwmGetWindowAttribute` + 2×`GetWindowRect` + 若干）、`UpdateClock`（`GetLocalTime` + 3 个 NLS 格式化 + 字符串比较，不变不重绘）；`KeepTop=1` 时每 2 s 还跑一次 `fsguard::Detect()` | 顶栏线程 60 s 共 13 ms（≈0.2 ms/s，含下面 10 s 项） |
| 4 `kSlowRefreshTimerId` | 10 000 ms | 有目标时 | 全量 `RefreshTabs`：`EnumWindows` + 每候选 `GetProcessName`×2（`OpenProcess`+`QueryFullProcessImageNameW`）+ DWM/类名/标题；**枚举不做早退**，只有重绘被签名挡住 | 0.3~3 ms/次 |
| 1 `kTabRefreshTimerId` | 200 ms | 否（新建窗口/抢焦点待定态，≤10 s） | 同上 + `ForceForegroundWindow` 重试 | 瞬间 ~1.5% 单核 |
| 2 `kTargetRetryTimerId` | 150 ms×5 | 否 | `UpdateTarget` + 整窗重绘 | 瞬态 |
| 3 `kTabRefreshDebounceTimerId` | 100 ms 一次性 | 否 | 事件抖动合并后一次刷新 | 瞬态 |
| 6 `kVolumeRetryTimerId` | 1500 ms | 否（音量面板开着且无音频会话） | COM 音频会话枚举 + 全进程快照 | 0.5~5 ms，瞬态 |
| 7 `kChromeSyncGraceTimerId` | 4000 ms 一次性 | 否 | 断线后回落窗口枚举 | 瞬态 |
| 8 `kQuickSettingsAlignTimerId` | 20 ms→250 ms | 否（快速设置面板打开） | `GetWindowRect`×2 + `CreateRectRgn`/`GetWindowRgn`/`GetRgnBox`，必要时 `SetWindowPos` | 10~25 µs/次，瞬态 |

顶栏还有一个**常驻线程**：Chrome 同步 WebSocket 服务（`ChromeSyncServerThread`）。
没有扩展连进来时用 `select(...,{0,500000})` **每 500 ms 醒一次**（实测 118 次/60 s，
8.8 ms/60 s）；连上后改成 15 s 收超时 + 心跳 ping。

## 5. 启动台 / 日历 / 时钟 / 宿主

| 位置 | 周期 | 常驻? | 每次做的事 | 实测/估算 |
|---|---|---|---|---|
| `launcher_main.cpp:72-74` | **500 ms**（`kIdlePollMs`，初始 16 ms） | **是** | 空闲态只做 5 个状态位判断 + `SetTimer(500)` 重挂，**不重绘**（`dirty` 在本树里没有写入者，重绘分支是死代码） | 117 次/60 s、11.6 ms/60 s ≈ **0.1 ms/次×2 次/秒**（PowerShell 侧看到的是纯定时器唤醒） |
| 同上 | 16 ms | 否（翻页/按压/拖拽/改名动效） | `DrawLauncher` + `UpdateLayeredWindow` 整窗（1.2~2.7 MB/帧） | 60 Hz，瞬态 |
| `calendar_main.cpp:44-45` | 1000 ms | 是（**当前组件已关闭**） | `GetLocalTime` + 3 个 WORD 比较，跨天才重绘 | ≈µs/次 |
| `main.cpp:45-46`（模拟时钟） | 1000 ms | 是（**当前组件已关闭**） | **无条件整窗重绘**：60 条刻度线 + 12 个数字 + 3 根指针 + 194 KB 位图拷贝 | 0.2~1 ms/次 |
| `mywigets_main.cpp:127-128` | 1000 ms | **是** | `ReconcileWidgetStates`：5 个组件各一次原子读 + 必要时 `WaitForSingleObject(thread,0)`；**无枚举、无绘制**，只在组件真的退出时才写注册表+气泡 | 60 次/60 s、3.7 ms/60 s ≈ 0.06 ms/次 |
| `mywigets_main.cpp:241-264` | 10 ms | 否（组件启动握手，≤5 s） | 原子读 hwnd + 等待线程 | 启动期 100 Hz |

## 6. 挂起 / 清理 / 服务守护：没有轮询

- `app_suspend.cpp`、`app_cleanup.cpp`、`cleanup_admin.cpp`、`service_guard.cpp`
  **0 个 `SetTimer` / 0 个自建线程 / 0 次 `Sleep` 轮询**；
  仅 5 处有界一次性等待（`WaitForSingleObject` 等停机事件、等子进程退出）。
- 历史提交 `87a5c31` 的"效率模式监控线程"（Job + IOCP）在 `7f57501` 已被删除，
  当前树里不存在。
- 空闲时 `ReconcileAppSuspend` 不取进程快照、不做 `EnumWindows`（`due` 为空就不进
  `SuspendMany`）；每拍是几十个 user32/kernel 调用 + 一次到顶栏的
  `SendMessageTimeoutW`（150 ms 上限，正常 <1 ms）。**这笔开销记在 §2 第 5 号
  定时器的 0.8 ms 里。**

## 7. 与"0.4 W 突破"的对应（按嫌疑排序）

1. **`DoPoll`/`RefreshItems` 尖峰（20~40 ms）** —— 60 s 一次保底 + 窗口事件触发
   （合并 1 s）。单次能量最大，曲线上表现为不规则鼓包。若"隔一段时间"是几十秒
   量级，先查这里。
2. **2 s 常驻 watchdog（0.8 ms）** —— 最稳定的节拍，30 次/分钟；本身不足以推到
   0.4 W，但会把封装反复拽出低功耗态，并放大上面的鼓包。`KeepTop=1` 时它每拍
   还要读 6 个注册表值、跑一次 `SHQueryUserNotificationState`、问一次顶栏目标。
3. **6~7 次/秒的零散唤醒** —— 启动台 2 Hz、Chrome 同步 `select` 2 Hz、顶栏 1 Hz、
   宿主 1 Hz。单次 <0.2 ms，但让封装几乎没有长时间静止的机会。
4. **未命中的重路径**：`ApplyWorkArea` 一旦发现工作区被 explorer 改回
   （隐藏任务栏后偶发），2 s 那一拍会变成"写工作区 + `Sleep(60)` + 重排所有最大化
   窗口"，那才是真正的功耗尖峰。当前实测工作区与目标一致，未命中。

## 8. 可削减项（按收益/风险排）

1. `DoPoll` 的 `CollectRunningExePaths`：托盘注册表没变就**不要每 3 s 重扫全进程**，
   改成"托盘键变化 → 置脏 → 下一次刷新才扫"，或把缓存拉到 30 s。
2. `RefreshItems`：先比 `MakeSignature` 轻量签名（窗口集合 Z 序 + 托盘键版本）再决定
   是否需要每窗口 DWM 查询；`IsCloaked` 只对候选窗口做。
3. 2 s watchdog 拆档：`KeepTop=0` 且 `AutoCollapse=0` 时不需要每 2 s 重读注册表与
   全屏复核 → 5~10 s；注册表开关改用 `RegNotifyChangeKeyValue`（托盘已在用）。
4. `ReconcileAppSuspend` 的顶栏查询：顶栏已有 `kMsgTopBarTargetChanged` 主动推送，
   2 s 那拍的 `SendMessageTimeout` 可只在"有候选应用"时才发。
5. Chrome 同步线程：无客户端时把 `select` 超时从 500 ms 放到 5 s（或懒启动）。
6. 启动台 500 ms 心跳：空闲无动效时改 2 s，或改为完全事件驱动（当前它什么都不做）。
7. 顶栏 1 s 时钟拍：`TargetOverlapsBar` 只在"有目标且目标窗口可见"时算。
8. 顶栏 10 s 全量 `RefreshTabs`：同样先比签名，没变就不枚举。

## 9. 本轮已实施（2026-10-31）与实测效果

上面 §7/§8 的最贵几项已按"先便宜、后昂贵"的顺序改掉，并在同一台机器、
同一会话状态下做了改前/改后对照（`QueryThreadCycleTime` 20 ms 采样 60 s，
TSC 3.068 GHz；两种构建各跑一次，间隔仅数分钟）。

### 9.1 改了什么

| 位置 | 改动 | 目的 |
|---|---|---|
| `dock_main.cpp` `ExeDisplayName` | 加进程内缓存（原来每次刷新对 17 个条目各读一次版本资源） | 版本资源是**文件 I/O**，是 `RefreshItems` 15~30 ms 的主体 |
| `dock_main.cpp` `CollectRunningExePaths` | ① 进程列表改用 `NtQuerySystemInformation(SystemProcessInformation)`（Toolhelp 快照单独就要 5~7.5 ms）；② 基名预筛；③ 同路径确认存活后不再重复读命令行（Chromium 同 exe 十几个进程，单次命令行 ~0.9 ms）；④ 只返回候选路径 | 托盘存活判定从 12 ms 降到 ~7 ms，且只在必要时跑 |
| `dock_main.cpp` `RefreshItems` 尾部 | `needsRedraw` 由"无条件置真"改为**内容签名变化才置真**（签名补入 `displayName`） | 原来每次事件刷新都整窗重画一次（约 10 ms） |
| `dock_main.cpp` 托盘注册表监听 | 监听线程置 `g_trayRegistryDirty`，刷新时**按需**重扫进程存活；TTL 3 s → 10 s 仅作兜底 | 无托盘变化时完全跳过整轮进程枚举 |
| `dock_main.cpp` 2 s 保底 | 拆成两档：全屏让位/挂起复核仍 2 s；注册表读取 + 工作区对齐改 5 s（`kWorkAreaWatchMs`） | 6 次注册表读取 + `SPI_GETWORKAREA` 是那一拍的主要开销，而它服务的都是低频人工操作 |
| `dock_main.cpp` `ReconcileAppSuspend` | ① 无候选且无挂起进程时直接返回（省掉跨进程查询与输入桌面探针）；② 顶栏目标改为**缓存**（顶栏目标变化本来就主动推消息），仅当顶栏窗口变化/缓存未建立时才 `SendMessageTimeout` | 去掉 2 s 一次的跨进程同步调用（它还会顺带唤醒顶栏线程） |
| `dock_main.cpp` `EnumAppWindowProc` | DWM `cloaked` 查询移到所有廉价过滤之后 | 减少每轮窗口枚举里的 DWM 调用 |
| `dock_main.cpp` `RequestServiceGuard` | `schtasks /run` 的等待（实测 200~300 ms，最坏 3 s）挪到后台线程 | 应用关闭那一刻不再卡 UI、也不再有那一段功耗尖峰 |
| `topbar_main.cpp` `UpdateClock` | 不显示秒时按"分钟戳"跳过 NLS 格式化 | 每秒两次 `GetTimeFormatW`/`GetDateFormatW` → 每分钟一次 |
| `topbar_main.cpp` Chrome 同步线程 | 无客户端时 `select` 超时 0.5 s → 1 s | 常驻唤醒 2 Hz → 1 Hz |
| `launcher_main.cpp` | 空闲心跳 `kIdlePollMs` 0.5 s → 2 s | 空转心跳 2 Hz → 0.5 Hz（空闲拍本来什么都不做） |

诊断用的一行日志（`[刷新耗时]`/`[条目耗时]`，总耗时 >4~5 ms 才落）保留：
日志关闭时只多几次 `QueryPerformanceCounter`，需要排查时把 `LogEnabled`
打开即可看到分段耗时。

### 9.2 实测（同一会话、各 60 s）

| 指标 | 改前 | 改后 | 变化 |
|---|---:|---:|---:|
| 套件进程合计 | 211.5 ms / 0.354% 单核 | **38.6 ms / 0.065%** | **5.5×** |
| Dock 线程 | 173.5 ms / 0.290% | **17.6 ms / 0.029%** | **9.9×** |
| 2 s 拍中位耗时 | 0.42 ms | **0.17 ms** | 2.5× |
| >2 ms 的刷新尖峰 | 7 次 / 合计 153 ms / 最大 32 ms | **1 次 / 9.9 ms** | 尖峰基本消失 |
| 顶栏线程 | 10.1 ms、94 次唤醒 | 5.8 ms、60 次 | 唤醒减半 |
| 启动台线程 | 9.3 ms、117 次唤醒 | 4.6 ms、60 次 | 唤醒减半 |
| Chrome 同步线程 | 11.6 ms、118 次唤醒 | 6.1 ms、60 次 | 唤醒减半 |

分项实测（日志版构建）：`RefreshItems` 26~40 ms → **8~10 ms**（含一次进程
枚举），不含枚举的刷新 <4 ms；`DrawAndPresent` 整窗重绘只在实际变化时发生
（约 10 ms 的事件成本，改前每次刷新都有）。

### 9.3 仍然存在的成本（以及为什么保留）

- `CollectRunningExePaths` 单次 ~7 ms：`NtQuerySystemInformation` 本身 3~4 ms，
  其余是每候选 ~0.22 ms 的 `OpenProcess`+镜像名（这台机器上系统级钩子让单次
  开进程偏贵）。现在最多 10 s 一次，且托盘没变化就不跑。
- 60 s 保底 `DoPoll` 仍会做一次完整刷新（含上面那次枚举）。
- 顶栏 10 s 全量 `RefreshTabs`（`EnumWindows`）未改：需要"事件丢失"兜底，
  单次 0.3~3 ms，暂不值得动。
- `escalation`（服务守护）第一次触发仍会拉起 `schtasks.exe`，但不再阻塞 UI。

测量说明：两次采样时桌面处于**锁屏**状态（`IsInteractiveDesktopAvailable`
为 false，挂起复核会提前返回），因此绝对值低于解锁空闲态；改前/改后同条件，
比值可比。解锁后的真实空闲值会略高，但 2 s 拍与刷新这两条主要路径的下降
与锁屏无关（注册表读取、DWM 查询、整窗重绘都按同样的次数被移除）。

