# analyze.py - 汇总 pulse / sandbox 挂起策略功耗基准
#
# 输入：bench/results_final/summary.csv + samples_*.csv
# 输出：analysis_trials.csv、analysis.md
#
# 功耗取每个阶段 1Hz 采样值的中位数（不是均值）：机器上偶发的
# Defender/浏览器/系统任务会把个别秒的包功耗抬到 8-12W，均值会被污染；
# 中位数对这类短时尖峰稳健。
#
# 两个关键指标：
#   residual_cores  挂起后末 2s 的残余 CPU（由基准程序自己的记账 Job 统计，
#                   与背景负载无关）——策略到底有没有把应用停住；
#   d_pkg_removed   活跃阶段包功耗 − 挂起阶段包功耗 ——这次挂起实际消掉了
#                   多少 CPU 功耗；不受两个阶段共同背景的影响。
import os
import sys

import pandas as pd

SCENARIO_ORDER = ['control', 'cross', 'orphan']
SCENARIO_CN = {
    'control': '同目录 helper（对照）',
    'cross': '含跨目录 helper',
    'orphan': '主进程退出、helper 存活',
}
VERSION_ORDER = ['pulse', 'sandbox']


def phase_medians(path):
    df = pd.read_csv(path)
    df = df[(df['pkg_mw'] > 0) & (df['pp0_mw'] > 0)]
    if df.empty:
        return {}
    out = {}
    for phase, g in df.groupby('phase'):
        out['med_%s_pkg' % phase] = float(g['pkg_mw'].median())
        out['med_%s_pp0' % phase] = float(g['pp0_mw'].median())
        out['n_%s' % phase] = int(len(g))
    return out


def md_table(df):
    cols = list(df.columns)
    out = ['| ' + ' | '.join(cols) + ' |',
           '|' + '|'.join(['---'] * len(cols)) + '|']
    for _, r in df.iterrows():
        out.append('| ' + ' | '.join(str(r[c]) for c in cols) + ' |')
    return '\n'.join(out)


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    results = sys.argv[1] if len(sys.argv) > 1 else os.path.join(here, 'results_final')
    summary_path = os.path.join(results, 'summary.csv')
    if not os.path.exists(summary_path):
        print('missing ' + summary_path)
        return 1

    s = pd.read_csv(summary_path)
    rows = []
    for _, r in s.iterrows():
        samples = os.path.join(
            results, 'samples_%d_%s_%s.csv' % (r['trial'], r['version'], r['scenario']))
        row = {
            'trial': int(r['trial']),
            'version': r['version'],
            'scenario': r['scenario'],
            'first_code': r['first_code'],
            'matched': int(r['first_matched']),
            'suspended': int(r['first_suspended']),
            'job_members': int(r['first_job_members']),
            'job_sandbox': int(r['first_job_sandbox']),
            'review_calls': int(r['review_calls']),
            'victim_core_s': float(r['victim_cpu_ms_window']) / 1000.0,
            'residual_cores': float(r['victim_cpu_ms_tail']) / 2000.0,
            'driver_mcycles': float(r['driver_mcycles']),
            'first_mcycles': float(r['first_mcycles']),
            'review_mcycles': float(r['review_mcycles']),
        }
        if os.path.exists(samples):
            row.update(phase_medians(samples))
        if 'med_active_pkg' in row and 'med_suspended_pkg' in row:
            row['d_pkg_removed'] = row['med_active_pkg'] - row['med_suspended_pkg']
        if 'med_idle_pkg' in row and 'med_suspended_pkg' in row:
            row['d_pkg_vs_idle'] = row['med_suspended_pkg'] - row['med_idle_pkg']
        if all(k in row for k in ('med_idle_pkg', 'med_suspended_pkg', 'med_post_pkg')):
            row['d_pkg_bracket'] = row['med_suspended_pkg'] - 0.5 * (
                row['med_idle_pkg'] + row['med_post_pkg'])
        if 'med_active_pp0' in row and 'med_suspended_pp0' in row:
            row['d_pp0_removed'] = row['med_active_pp0'] - row['med_suspended_pp0']
        rows.append(row)

    df = pd.DataFrame(rows)
    df['scenario'] = pd.Categorical(df['scenario'], SCENARIO_ORDER, ordered=True)
    df['version'] = pd.Categorical(df['version'], VERSION_ORDER, ordered=True)
    df = df.sort_values(['scenario', 'version', 'trial'])
    df.to_csv(os.path.join(results, 'analysis_trials.csv'), index=False)

    lines = []
    lines.append('# pulse vs sandbox 挂起策略功耗对比\n')
    lines.append('数据：`%s`，共 %d 个 trial。功耗为 Intel RAPL 包功耗 1Hz 采样'
                 '（`\\Energy Meter(*)\\Power`）的阶段中位数；残余 CPU 来自基准'
                 '程序独立记账 Job。\n' % (results, len(df)))
    lines.append('| 场景 | 版本 | n | 首次挂起结果 | 匹配/挂起 | 挂起后残余 CPU（核） | '
                 '30s 残余 CPU（核·秒） | 挂起消除的包功耗（W） | 残余包功耗 vs 空闲（mW） | '
                 '策略 CPU（Mcycles/30s） |')
    lines.append('|---|---|---:|---|---:|---:|---:|---:|---:|---:|')

    for sc in SCENARIO_ORDER:
        for v in VERSION_ORDER:
            g = df[(df['scenario'] == sc) & (df['version'] == v)]
            if g.empty:
                continue
            codes = '/'.join('%s×%d' % (k, n) for k, n in g['first_code'].value_counts().items())
            lines.append('| %s | %s | %d | %s | %d/%d | %.2f | %.1f | %+.2f | %+.0f | %.1f |' % (
                SCENARIO_CN[sc], v, len(g), codes,
                int(g['matched'].median()), int(g['suspended'].median()),
                g['residual_cores'].median(),
                g['victim_core_s'].median(),
                g['d_pkg_removed'].median() / 1000.0,
                g['d_pkg_vs_idle'].median(),
                g['driver_mcycles'].median()))
    lines.append('')

    lines.append('## 每个场景两版差值（sandbox − pulse）\n')
    lines.append('| 场景 | 残余 CPU 核差 | 挂起消除的包功耗差（W） | 策略 CPU 差（Mcycles/30s） |')
    lines.append('|---|---:|---:|---:|')
    for sc in SCENARIO_ORDER:
        p = df[(df['scenario'] == sc) & (df['version'] == 'pulse')]
        q = df[(df['scenario'] == sc) & (df['version'] == 'sandbox')]
        if p.empty or q.empty:
            continue
        lines.append('| %s | %+.2f | %+.2f | %+.1f |' % (
            SCENARIO_CN[sc],
            q['residual_cores'].median() - p['residual_cores'].median(),
            (q['d_pkg_removed'].median() - p['d_pkg_removed'].median()) / 1000.0,
            q['driver_mcycles'].median() - p['driver_mcycles'].median()))
    lines.append('')

    lines.append('## 每次 trial 的原始值\n')
    cols = ['trial', 'scenario', 'version', 'first_code', 'matched', 'suspended',
            'job_members', 'job_sandbox', 'victim_core_s', 'residual_cores',
            'med_idle_pkg', 'med_active_pkg', 'med_suspended_pkg', 'med_post_pkg',
            'd_pkg_removed', 'd_pkg_vs_idle', 'd_pp0_removed',
            'driver_mcycles', 'review_mcycles']
    show = df[[c for c in cols if c in df.columns]].copy()
    for c in show.columns:
        if show[c].dtype.kind == 'f':
            show[c] = show[c].round(1)
    lines.append(md_table(show))
    lines.append('')

    with open(os.path.join(results, 'analysis.md'), 'w', encoding='utf-8') as f:
        f.write('\n'.join(lines))
    print('\n'.join(lines[:30]))
    print('\nwrote analysis.md / analysis_trials.csv')
    return 0


if __name__ == '__main__':
    sys.exit(main())
