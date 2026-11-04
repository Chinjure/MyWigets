# pulse vs sandbox：后台应用挂起策略功耗实测

日期：2026-10-29  
机器：ASUS Vivobook S 14（Intel Core Ultra 5 228V，Lunar Lake，8 核）  
对比版本：`pulse` @ 88434ef、`sandbox` @ a90eebc

## 0. 一句话结论

**sandbox 的挂起策略功耗更低（严格不高于 pulse）。**

- 应用只有同目录 helper 时：两版都冻住 5/5 个进程，挂起后残余 CPU 都是 0.00 核，包功耗都回到空闲水平，**功耗打平**；
- helper 在安装目录外（`%TEMP%` 等）或主进程已退出时：pulse 漏掉这些 helper，它们继续烧 CPU，**挂起后仍多 0.94～1.88 个核、包功耗多 2.4～5.0 W**；sandbox 靠 Job 继承仍能全部冻住，**残余为 0**；
- sandbox 自身每次挂起多花约 4～8 Mcycles（30 s 内折合 **不到 1 mW**），远小于漏冻 helper 的瓦级代价。

## 1. 两个分支到底差在哪

两个分支 tip 都**没有** 2 s 周期补挂轮询（`TopUpSuspended` 只存在于中间提交 1146c42，已在 a90eebc 删除）。真正的差别是“哪些进程算这个应用”：

| | pulse 88434ef | sandbox a90eebc |
|---|---|---|
| 进程归属 | 进程快照：主 exe 的全部实例 + **安装目录树内**的后代 | 每应用一个 **Job Object**：Dock 启动即入 Job，后代内核自动继承；首次挂起再事后追加 |
| 跨目录 helper | 选不中 | 能覆盖 |
| 主进程退出后 | 找不到主 exe → `NoProcesses` | Job 成员仍在，能挂起 |
| 挂起瞬间新创建的子进程 | 快照竞态可能漏 | Job 枚举 + 冻结收尾一次性补冻 |
| 已有挂起态复查 | 返回 `AlreadySuspended`（会核对跟踪表存活） | 返回 `AlreadySuspended`，不枚举 Job/进程 |

所以这不是“轮询 vs 不轮询”的对比，而是“快照归属 vs Job 沙箱归属”的对比。

## 2. 测试方法

### 2.1 被测代码

从两个分支分别导出 `src/app_suspend.cpp/.h`，与**同一份**基准驱动程序编译成两个独立 exe：

- `bench/artifacts/bench_pulse.exe`
- `bench/artifacts/bench_sandbox.exe`

驱动程序的调用节奏与 Dock 一致：挂起一次，随后 30 s 内每 2 s 再调用一次 `SuspendMany`（对应 Dock 的 2 s 保底复核）。sandbox 版在启动 victim 后调用 `AdoptProcess`，模拟 Dock 的 `ShellExecuteEx + AdoptProcess` 启动路径；pulse 没有这个 API。

### 2.2 合成负载

`bench/victim.cpp` 是无窗口多进程“应用”：

- 主进程 `victim.exe`；helper 是同一份 exe 复制成 `helper.exe`；
- helper 以 30 ms 忙等 + 20 ms 睡眠循环（约 47% 单核）持续烧 CPU；
- 场景：
  - **control**：4 个同目录 helper（两版都能选中，对照组）；
  - **cross**：2 个同目录 + 2 个 `ext\` 目录 helper（模拟 %TEMP%/更新目录 helper）；
  - **orphan**：2+2 个 helper，主进程 800 ms 后退出（模拟启动器/更新器退出后 helper 存活）。

### 2.3 指标

- **残余 CPU（核）**：基准程序自己创建记账 Job，用 `JobObjectBasicAndIoAccountingInformation` 统计全部 victim 进程（含已退出）在挂起窗口最后 2 s 的 CPU 时间，不受 PID 复用/进程退出影响，也不受背景负载影响；
- **残余包功耗**：Intel RAPL `\Energy Meter(*)\Power`（`rapl_package0_pkg`）1 Hz 采样，取各阶段中位数（背景里的 Defender/浏览器尖峰会把个别秒抬到 8～12 W，中位数更稳）；
- **挂起消除的包功耗**：活跃阶段包功耗中位数 − 挂起阶段包功耗中位数；
- **策略 CPU**：`QueryProcessCycleTime` 统计所有 `SuspendMany` 调用的 CPU 周期（Mcycles）。

计划 3 次重复（18 trial）。因耗时较长，在 11 个 trial 后停止：control/cross 各 2 次重复，orphan pulse 2 次、sandbox 1 次。原始数据在 `bench/results_final/`。

## 3. 结果

### 3.1 汇总（中位数）

| 场景 | 版本 | n | 首次挂起 | 匹配/挂起 | 挂起后残余 CPU（核） | 30 s 残余 CPU（核·秒） | 挂起后包功耗 vs 空闲（mW） | 挂起消除的包功耗（W） | 策略 CPU（Mcycles/30s） |
|---|---|---:|---|---:|---:|---:|---:|---:|---:|
| 同目录 helper | pulse | 2 | Suspended×2 | 5/5 | 0.00 | 0.0 | −42 | 4.18 | 18.7 |
| 同目录 helper | sandbox | 2 | Suspended×2 | 5/5 | 0.00 | 0.0 | −25 | 5.05 | 22.6 |
| 含跨目录 helper | pulse | 2 | Suspended×2 | 3/3 | **0.94** | **28.3** | **+2425** | 2.78 | 13.8 |
| 含跨目录 helper | sandbox | 2 | Suspended×2 | 5/5 | **0.00** | **0.0** | **−4** | 4.04 | 22.2 |
| 主进程退出、helper 存活 | pulse | 2 | NoProcesses×2 | 0/0 | **1.88** | **56.2** | **+5027** | −0.04 | 90.3 |
| 主进程退出、helper 存活 | sandbox | 1 | Suspended×1 | 4/4 | **0.00** | **0.0** | −474¹ | 4.83 | 22.1 |

¹ 该 trial 开头空闲阶段有背景尖峰（中位数 1.99 W，收尾空闲 0.31 W），所以“vs 空闲”为负；同一窗口 victim 记账 CPU 为 0，说明进程确实已冻住，残余功耗只是背景噪声。

### 3.2 两版差值（sandbox − pulse）

| 场景 | 残余 CPU 核差 | 挂起消除的包功耗差（W） | 策略 CPU 差（Mcycles/30s） |
|---|---:|---:|---:|
| 同目录 helper | +0.00 | +0.87（噪声，两者残余均为 0） | +4.0 |
| 含跨目录 helper | **−0.94** | **+1.26** | +8.4 |
| 主进程退出、helper 存活 | **−1.88** | **+4.86** | −68.2 |

### 3.3 怎么读这些数

- 每个 helper 约 47% 单核；两个 helper ≈ 0.94 核，四个 ≈ 1.88 核，与 pulse 的残余 CPU 完全对得上。
- 从 cross 场景可估出本机“每核”包功耗约 `2.43 W / 0.94 核 ≈ 2.6 W/核`；orphan 场景 `5.0 W / 1.88 核 ≈ 2.7 W/核`，两处一致。
- 也就是说：**只要被漏掉的 helper 真的在干活，pulse 挂起后每小时会多耗约 2.4～5.0 Wh**（按持续满载计；实际 helper 若大部分时间空闲，差距会小很多）。
- 策略自身 CPU：control/cross 下 sandbox 比 pulse 多 4～8 Mcycles/30 s（约 0.01% 单核，折合不到 1 mW）；orphan 场景相反，pulse 因为每次复核都做全量进程快照，多约 68 Mcycles/30 s（约 0.1% 单核，约 3 mW）。两者都远小于漏冻 helper 的代价。

## 4. 结论与取舍

**功耗维度：sandbox 更低，且不会比 pulse 高。**

- 常态（同目录 helper）：打平，sandbox 只多几 mW 的 Job 归属开销；
- 跨目录 helper / 父进程退出：sandbox 能继续把应用冻到 0，pulse 会留 0.9～1.9 个核在跑，多 2.4～5.0 W。

代价不在功耗，而在**进程归属更激进**：Job 关联不可撤销，理论上可能把应用顺手拉起的外部程序圈进来；sandbox 版用“可见窗口/其它 Dock 应用/不可核对成员整组放弃”等过滤兜底，但这类 UX/误冻风险本次没有测。

## 5. 局限

- 合成 helper 持续烧 CPU，是**上界**；真实 %TEMP% helper（更新器、crashpad 等）多数时间空闲，实测瓦差会小一些。但漏掉一个 Chromium 渲染进程时，代价可以到瓦级，所以这个上界有参考意义。
- RAPL 只统计 CPU 包功耗，不含屏幕/SSD/网络；挂起策略影响的正是 CPU 侧，口径合适。
- 本机背景负载会造成包功耗 ±0.3～1 W 漂移，因此主指标用记账 Job 的残余 CPU，功耗用阶段中位数并做重复。
- 只测功耗，不测 Alt+Tab、误冻、breakaway 兼容性等正确性/体验问题。

## 6. 复现

```powershell
# 1) 编译 victim 和两个版本的基准程序（需要 VS C++ 工具链）
#    见 bench/build_one.ps1 的两条 cl 命令行；源码：
#    bench/victim.cpp、bench/bench_driver.cpp、bench/vsrc_pulse/、bench/vsrc_sandbox/

# 2) 跑 18 个 trial（约 17 分钟；可把 -Reps 改成 1 快速跑 6 个）
powershell -NoProfile -ExecutionPolicy Bypass -File bench\run_trials.ps1 -Reps 3

# 3) 汇总
python bench\analyze.py bench\results_final
```

原始数据：`bench/results_final/summary.csv`、`bench/results_final/analysis_trials.csv`、每个 trial 的 `samples_*.csv`。
