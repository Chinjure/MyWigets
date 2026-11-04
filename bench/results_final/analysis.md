# pulse vs sandbox 挂起策略功耗对比

数据：`.\bench\results_final`，共 11 个 trial。功耗为 Intel RAPL 包功耗 1Hz 采样（`\Energy Meter(*)\Power`）的阶段中位数；残余 CPU 来自基准程序独立记账 Job。

| 场景 | 版本 | n | 首次挂起结果 | 匹配/挂起 | 挂起后残余 CPU（核） | 30s 残余 CPU（核·秒） | 挂起消除的包功耗（W） | 残余包功耗 vs 空闲（mW） | 策略 CPU（Mcycles/30s） |
|---|---|---:|---|---:|---:|---:|---:|---:|---:|
| 同目录 helper（对照） | pulse | 2 | Suspended×2 | 5/5 | 0.00 | 0.0 | +4.18 | -42 | 18.7 |
| 同目录 helper（对照） | sandbox | 2 | Suspended×2 | 5/5 | 0.00 | 0.0 | +5.05 | -25 | 22.6 |
| 含跨目录 helper | pulse | 2 | Suspended×2 | 3/3 | 0.94 | 28.3 | +2.78 | +2425 | 13.8 |
| 含跨目录 helper | sandbox | 2 | Suspended×2 | 5/5 | 0.00 | 0.0 | +4.04 | -4 | 22.2 |
| 主进程退出、helper 存活 | pulse | 2 | NoProcesses×2 | 0/0 | 1.88 | 56.2 | -0.04 | +5027 | 90.3 |
| 主进程退出、helper 存活 | sandbox | 1 | Suspended×1 | 4/4 | 0.00 | 0.0 | +4.83 | -474 | 22.1 |

## 每个场景两版差值（sandbox − pulse）

| 场景 | 残余 CPU 核差 | 挂起消除的包功耗差（W） | 策略 CPU 差（Mcycles/30s） |
|---|---:|---:|---:|
| 同目录 helper（对照） | +0.00 | +0.87 | +4.0 |
| 含跨目录 helper | -0.94 | +1.26 | +8.4 |
| 主进程退出、helper 存活 | -1.88 | +4.86 | -68.2 |

## 每次 trial 的原始值

| trial | scenario | version | first_code | matched | suspended | job_members | job_sandbox | victim_core_s | residual_cores | med_idle_pkg | med_active_pkg | med_suspended_pkg | med_post_pkg | d_pkg_removed | d_pkg_vs_idle | d_pp0_removed | driver_mcycles | review_mcycles |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 106 | control | pulse | Suspended | 5 | 5 | 0 | 0 | 0.0 | 0.0 | 310.8 | 3542.4 | 264.7 | 510.2 | 3277.7 | -46.1 | 2616.2 | 19.4 | 9.7 |
| 110 | control | pulse | Suspended | 5 | 5 | 0 | 0 | 0.0 | 0.0 | 309.7 | 5352.9 | 271.0 | 352.0 | 5081.9 | -38.7 | 4169.8 | 17.9 | 9.5 |
| 105 | control | sandbox | Suspended | 5 | 5 | 5 | 1 | 0.0 | 0.0 | 308.9 | 5482.7 | 279.9 | 310.1 | 5202.9 | -29.1 | 4273.0 | 22.7 | 9.6 |
| 109 | control | sandbox | Suspended | 5 | 5 | 5 | 1 | 0.0 | 0.0 | 329.5 | 5212.6 | 307.9 | 279.9 | 4904.7 | -21.6 | 3842.6 | 22.5 | 10.1 |
| 103 | cross | pulse | Suspended | 3 | 3 | 0 | 0 | 28.2 | 0.9 | 342.5 | 5345.9 | 2813.5 | 353.4 | 2532.4 | 2471.0 | 2265.1 | 13.7 | 7.0 |
| 107 | cross | pulse | Suspended | 3 | 3 | 0 | 0 | 28.4 | 1.0 | 416.2 | 5817.5 | 2795.5 | 490.8 | 3022.1 | 2379.3 | 2576.9 | 13.9 | 6.9 |
| 104 | cross | sandbox | Suspended | 5 | 5 | 5 | 1 | 0.0 | 0.0 | 263.6 | 3353.6 | 310.5 | 271.7 | 3043.0 | 46.9 | 2365.8 | 22.1 | 9.8 |
| 108 | cross | sandbox | Suspended | 5 | 5 | 5 | 1 | 0.0 | 0.0 | 357.9 | 5335.8 | 303.2 | 326.9 | 5032.6 | -54.7 | 4037.4 | 22.2 | 9.7 |
| 102 | orphan | pulse | NoProcesses | 0 | 0 | 0 | 0 | 56.5 | 1.8 | 303.5 | 5182.7 | 5162.4 | 362.1 | 20.3 | 4858.9 | 35.1 | 94.1 | 87.1 |
| 111 | orphan | pulse | NoProcesses | 0 | 0 | 0 | 0 | 55.8 | 1.9 | 283.7 | 5380.7 | 5478.8 | 1580.6 | -98.1 | 5195.1 | 173.8 | 86.6 | 80.0 |
| 101 | orphan | sandbox | Suspended | 4 | 4 | 4 | 1 | 0.0 | 0.0 | 1991.5 | 6342.8 | 1517.3 | 308.4 | 4825.6 | -474.2 | 4097.1 | 22.1 | 9.9 |
