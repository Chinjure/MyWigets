# tests/topbar_tab_drag_test.ps1 —— 顶栏"随意拖动标签页"端到端回归
#
# 判定手段（不依赖 OCR，全部是真鼠标 + 真前台窗口）：
#   窗口枚举模式下一个应用的每个窗口 = 一个标签，点击标签会把对应窗口带到前台，
#   于是"顺序"可以用点标签后拿到的前台窗口句柄直接读出来：
#     1) 先依次点第 0/1/2 个标签，记下 slot -> 窗口句柄 的映射 [T0,T1,T2]；
#     2) 把第 2 个标签拖到标签区最左端（真鼠标按下 + 分步移动 + 抬起）；
#     3) 再点第 0/1/2 个标签，句柄顺序应当是 [T2,T0,T1]（整体右移一格）；
#     4) 反例控制：只在同一位置点击、不移动，顺序不得变化。
#   另外抓图对比拖动前/后的标签区像素（确认"确实重排过"），并留档拖动中
#   "浮起标签"的截图。
#
# 被当成标签的应用：资源管理器（CabinetWClass）—— 同一进程多个窗口，
# 正好是顶栏窗口枚举模式的分组场景；脚本自己开 3 个窗口，结束时关掉。
# （不用记事本：Win11 记事本是标签页式的，开 3 个文件只出 1 个窗口。）
#
# 前置：MyWigets-x64.exe 正在运行、顶栏窗口存在。
#
# 运行：
#   powershell -NoProfile -ExecutionPolicy Bypass -File tests\topbar_tab_drag_test.ps1

$ErrorActionPreference = 'Stop'

$cs = @'
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
using System.Text;

public class TabDrag {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint dx, uint dy, uint d, UIntPtr e);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint msg, IntPtr wp, IntPtr lp);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  public struct RECT { public int L, T, R, B; }

  const uint LEFTDOWN = 0x0002, LEFTUP = 0x0004;

  public static IntPtr FindByClass(string cls) {
    IntPtr found = IntPtr.Zero;
    EnumWindows((h, l) => {
      var c = new StringBuilder(256); GetClassName(h, c, 256);
      if (c.ToString() == cls) { found = h; return false; }
      return true;
    }, IntPtr.Zero);
    return found;
  }
  public static string Cls(IntPtr h) { var s = new StringBuilder(256); GetClassName(h, s, 256); return s.ToString(); }
  public static string Txt(IntPtr h) { var s = new StringBuilder(512); GetWindowTextW(h, s, 512); return s.ToString(); }
  // 窗口所属进程名（判断"点标签确实聚焦到记事本"，不依赖窗口标题）
  public static string ProcName(IntPtr h) {
    uint pid; GetWindowThreadProcessId(h, out pid);
    if (pid == 0) return "";
    try { return Process.GetProcessById((int)pid).ProcessName; } catch (Exception) { return ""; }
  }

  // 某个进程的所有可见顶层窗口
  public static List<IntPtr> WindowsOfPids(int[] pids) {
    var set = new HashSet<int>(pids);
    var list = new List<IntPtr>();
    EnumWindows((h, l) => {
      uint pid; GetWindowThreadProcessId(h, out pid);
      if (set.Contains((int)pid) && IsWindowVisible(h)) {
        RECT r; GetWindowRect(h, out r);
        if (r.R - r.L > 80 && r.B - r.T > 80) list.Add(h);
      }
      return true;
    }, IntPtr.Zero);
    return list;
  }

  // 某个窗口类名的所有可见顶层窗口（按标题过滤：只要标题里含 key 的）
  public static List<IntPtr> WindowsOfClass(string cls, string key) {
    var list = new List<IntPtr>();
    EnumWindows((h, l) => {
      if (!IsWindowVisible(h)) return true;
      var c = new StringBuilder(256); GetClassName(h, c, 256);
      if (c.ToString() != cls) return true;
      if (!string.IsNullOrEmpty(key)) {
        var t = new StringBuilder(512); GetWindowTextW(h, t, 512);
        if (t.ToString().IndexOf(key, StringComparison.OrdinalIgnoreCase) < 0) return true;
      }
      RECT r; GetWindowRect(h, out r);
      if (r.R - r.L > 80 && r.B - r.T > 80) list.Add(h);
      return true;
    }, IntPtr.Zero);
    return list;
  }

  public static void Move(IntPtr h, int x, int y, int w, int hh) {
    SetWindowPos(h, IntPtr.Zero, x, y, w, hh, 0x0004 /*SWP_NOZORDER*/);
  }
  public static void Click(int x, int y) {
    SetCursorPos(x, y);
    System.Threading.Thread.Sleep(120);
    mouse_event(LEFTDOWN, 0, 0, 0, UIntPtr.Zero);
    System.Threading.Thread.Sleep(60);
    mouse_event(LEFTUP, 0, 0, 0, UIntPtr.Zero);
    System.Threading.Thread.Sleep(350);
  }
  public static void Close(IntPtr h) { PostMessageW(h, 0x0010, IntPtr.Zero, IntPtr.Zero); }

  static void Shot(string path, int x, int y, int w, int hh) {
    using (var bmp = new Bitmap(w, hh, PixelFormat.Format32bppArgb)) {
      using (var g = Graphics.FromImage(bmp)) {
        g.CopyFromScreen(x, y, 0, 0, new Size(w, hh), CopyPixelOperation.SourceCopy);
      }
      bmp.Save(path, ImageFormat.Png);
    }
  }

  // 拖动：按下 -> 分步移动（中途抓一张"拖动中"截图）-> 抬起
  public static void Drag(int fromX, int fromY, int toX, int toY, int steps,
                          string midShotPath, int shotX, int shotY, int shotW, int shotH) {
    SetCursorPos(fromX, fromY);
    System.Threading.Thread.Sleep(150);
    mouse_event(LEFTDOWN, 0, 0, 0, UIntPtr.Zero);
    System.Threading.Thread.Sleep(120);
    for (int i = 1; i <= steps; i++) {
      int x = fromX + (toX - fromX) * i / steps;
      int y = fromY + (toY - fromY) * i / steps;
      SetCursorPos(x, y);
      System.Threading.Thread.Sleep(25);
      if (!string.IsNullOrEmpty(midShotPath) && i == steps / 2) {
        Shot(midShotPath, shotX, shotY, shotW, shotH);
      }
    }
    System.Threading.Thread.Sleep(120);
    mouse_event(LEFTUP, 0, 0, 0, UIntPtr.Zero);
    System.Threading.Thread.Sleep(400);
  }

  public static void ShotBar(string path, int x, int y, int w, int hh) { Shot(path, x, y, w, hh); }

  public static int[] Grab(int x, int y, int w, int hh) {
    using (var bmp = new Bitmap(w, hh, PixelFormat.Format32bppArgb)) {
      using (var g = Graphics.FromImage(bmp)) {
        g.CopyFromScreen(x, y, 0, 0, new Size(w, hh), CopyPixelOperation.SourceCopy);
      }
      var rect = new Rectangle(0, 0, w, hh);
      var d = bmp.LockBits(rect, ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
      int[] px = new int[w * hh];
      Marshal.Copy(d.Scan0, px, 0, px.Length);
      bmp.UnlockBits(d);
      return px;
    }
  }
  public static double DiffRatio(int[] a, int[] b) {
    int n = Math.Min(a.Length, b.Length), c = 0;
    for (int i = 0; i < n; i++) {
      int x = a[i], y = b[i];
      int d = Math.Abs((x & 0xFF) - (y & 0xFF)) + Math.Abs(((x >> 8) & 0xFF) - ((y >> 8) & 0xFF))
            + Math.Abs(((x >> 16) & 0xFF) - ((y >> 16) & 0xFF));
      if (d > 40) c++;
    }
    return n == 0 ? 0 : (double)c / n;
  }
}
'@

Add-Type -AssemblyName System.Windows.Forms,System.Drawing
Add-Type -TypeDefinition $cs -ReferencedAssemblies System.dll,System.Core.dll,System.Drawing.dll,System.Windows.Forms.dll
[void][TabDrag]::SetProcessDPIAware()

$fail = 0
function Check([string]$name, [bool]$ok, [string]$detail) {
  if ($ok) { Write-Host ("PASS: {0} ({1})" -f $name, $detail) -ForegroundColor Green }
  else { Write-Host ("FAIL: {0} ({1})" -f $name, $detail) -ForegroundColor Red; $script:fail++ }
}

$logs = Join-Path $PSScriptRoot '..\logs'
New-Item -ItemType Directory -Force -Path $logs | Out-Null
$logs = (Resolve-Path $logs).Path

# ---- 0) 顶栏几何（与 src/topbar_main.cpp 同一组布局常量） ----
$bar = [TabDrag]::FindByClass('DesktopTopBarWindow')
if ($bar -eq [IntPtr]::Zero) { Write-Host 'FAIL: 顶栏窗口 DesktopTopBarWindow 不存在（MyWigets 未运行？）'; exit 1 }
$rc = New-Object TabDrag+RECT
[void][TabDrag]::GetWindowRect($bar, [ref]$rc)
$barW = $rc.R - $rc.L
$barH = $rc.B - $rc.T
$dpi = [TabDrag]::GetDpiForWindow($bar); if ($dpi -le 0) { $dpi = 96 }
$k = $dpi / 96.0
$bw = 46.0 * $k; $gap = 4.0 * $k; $clockW = 112.0 * $k
$minX = $barW - 3 * $bw - 2 * $gap
$clockX = $minX - $gap - $clockW
$tabLeft = 46.0 * $k + 6.0 * $k
$tabRight = $clockX - 6.0 * $k
$tabY = $rc.T + [int]($barH / 2)
Write-Host ("顶栏 = ({0},{1}) {2}x{3} dpi={4} k={5}；标签区客户区 x={6:N1}..{7:N1}" -f `
  $rc.L, $rc.T, $barW, $barH, $dpi, $k, $tabLeft, $tabRight)

$tabCount = 3
$tabW = [Math]::Min(240.0 * $k, ($tabRight - $tabLeft) / $tabCount)
function TabX([int]$i) { return $rc.L + [int]($tabLeft + $tabW * ($i + 0.5)) }
$grabX = (0..($tabCount - 1) | ForEach-Object { TabX $_ }) -join ','
Write-Host ("假设 {0} 个标签：每个宽 {1:N1}，屏幕中心 x = {2}" -f $tabCount, $tabW, $grabX)

# ---- 1) 开 3 个资源管理器窗口当标签 ----
# 三个目录名带唯一时间戳：窗口标题（= 目录名）因此唯一，既能把"本次新建的
# 窗口"和用户自己的资源管理器窗口精确区分开，也不会把上次跑测试的残留
# 当成自己的窗口（清理时只关本次新建的三个）。
$stamp = Get-Date -Format 'HHmmss'
$tag = "TRD-$stamp"
$baseDir = Join-Path $env:TEMP "topbar_tab_drag_$stamp"
$dirs = @("$tag-A", "$tag-B", "$tag-C") | ForEach-Object {
  $p = Join-Path $baseDir $_
  New-Item -ItemType Directory -Force -Path $p | Out-Null
  $p
}
foreach ($d in $dirs) { Start-Process explorer.exe -ArgumentList $d }
Start-Sleep -Seconds 3

$wins = @([TabDrag]::WindowsOfClass('CabinetWClass', $tag))
Write-Host ("测试用资源管理器窗口数 = {0}（目录前缀 {1}）" -f $wins.Count, $tag)
if ($wins.Count -lt 3) {
  Write-Host 'FAIL: 资源管理器窗口不足 3 个，无法验证拖动重排'
  foreach ($w in $wins) { [TabDrag]::Close($w) }
  exit 1
}
$wins = @($wins[0], $wins[1], $wins[2])

# 前置：这个应用必须只有这 3 个窗口 —— 多一个（比如用户自己开着的文件夹窗口）
# 就会让"槽位 -> 窗口"的固定映射失效，判定结果不可信：直接说清楚而不是误报
$allCabinet = @([TabDrag]::WindowsOfClass('CabinetWClass', ''))
if ($allCabinet.Count -ne 3) {
  Write-Host ("SKIP: 当前共有 {0} 个资源管理器窗口（含用户自己的）," -f $allCabinet.Count)
  Write-Host '      "槽位 -> 窗口"映射不成立；请先关闭其它文件夹窗口再跑本测试。'
  foreach ($w in $wins) { [TabDrag]::Close($w) }
  Remove-Item -Recurse -Force $baseDir -ErrorAction SilentlyContinue
  exit 2
}

# 三个窗口摆到互不重叠的位置，方便精确点击与聚焦
$slots = @(@(120, 320), @(680, 320), @(1240, 320))
for ($i = 0; $i -lt 3; $i++) {
  [TabDrag]::Move($wins[$i], $slots[$i][0], $slots[$i][1], 480, 320)
}
Start-Sleep -Milliseconds 600

function FgHwnd() { return ([TabDrag]::GetForegroundWindow()).ToInt64() }
function Hx([int64]$h) { return ('0x{0:X}' -f $h) }

try {
  # 聚焦第一个测试窗口，让顶栏枚举到"资源管理器"这个应用的全部窗口
  [TabDrag]::Click($slots[0][0] + 240, $slots[0][1] + 60)
  Start-Sleep -Milliseconds 900

  # ---- 2) 记录当前顺序：依次点 0/1/2 号标签，用前台窗口句柄当身份 ----
  #（句柄而不是标题：资源管理器窗口标题可能带同名目录，句柄才是"哪个窗口"）
  $T = @()
  foreach ($i in 0..2) {
    [TabDrag]::Click((TabX $i), $tabY)
    $T += (FgHwnd)
    Write-Host ("  点标签 {0} -> 前台 {1} '{2}'" -f $i, (Hx $T[$i]), [TabDrag]::Txt([IntPtr]$T[$i]))
  }
  $procs3 = @($T | ForEach-Object { [TabDrag]::ProcName([IntPtr]$_) } | Select-Object -Unique)
  Check '点标签确实聚焦到了资源管理器窗口（顶栏显示的是该应用的标签）' `
    (($procs3.Count -eq 1) -and ($procs3[0] -match '(?i)explorer')) `
    ("进程 = " + ($procs3 -join ','))
  Check '三个标签指向三个不同窗口' (($T | Select-Object -Unique).Count -eq 3) `
    ("句柄 = " + (($T | ForEach-Object { Hx $_ }) -join ' '))

  # 稳定态基准（两次同状态截图，确认像素差指标本身不抖）
  [TabDrag]::Click((TabX 2), $tabY)
  Start-Sleep -Milliseconds 500
  [TabDrag]::ShotBar((Join-Path $logs 'tab_drag_before.png'), $rc.L, $rc.T, $barW, $barH)
  $pxBefore1 = [TabDrag]::Grab($rc.L, $rc.T, $barW, $barH)
  Start-Sleep -Milliseconds 400
  $pxBefore2 = [TabDrag]::Grab($rc.L, $rc.T, $barW, $barH)
  $noise = [TabDrag]::DiffRatio($pxBefore1, $pxBefore2)
  Write-Host ("同状态两次截图的像素差 = {0:P2}（噪声基线）" -f $noise)

  # ---- 3) 把 2 号标签拖到最前（拖到标签区最左端，保证越过所有中点）----
  $fromX = TabX 2
  $toX = $rc.L + [int]$tabLeft + 12
  [TabDrag]::Drag($fromX, $tabY, $toX, $tabY, 16,
    (Join-Path $logs 'tab_drag_mid.png'), $rc.L, $rc.T, $barW, $barH)
  Start-Sleep -Milliseconds 600
  [TabDrag]::ShotBar((Join-Path $logs 'tab_drag_after.png'), $rc.L, $rc.T, $barW, $barH)
  $pxAfter = [TabDrag]::Grab($rc.L, $rc.T, $barW, $barH)
  $moved = [TabDrag]::DiffRatio($pxBefore1, $pxAfter)
  Write-Host ("拖动前后标签区像素差 = {0:P2}" -f $moved)
  Check '拖动后标签区确实变了样' ($moved -gt 0.02) ("{0:P2} > 2%" -f $moved)

  # ---- 4) 重排后顺序应为 [T2, T0, T1] ----
  $order = @()
  foreach ($i in 0..2) {
    [TabDrag]::Click((TabX $i), $tabY)
    $order += (FgHwnd)
  }
  Write-Host ("  拖动后再点 0/1/2 -> " + (($order | ForEach-Object { Hx $_ }) -join ' '))
  Check '2 号标签已被拖到最前（新 0 号 = 原 2 号）' ($order[0] -eq $T[2]) `
    ("期望 {0}，实际 {1}" -f (Hx $T[2]), (Hx $order[0]))
  Check '原 0 号顺移到 1 号位' ($order[1] -eq $T[0]) `
    ("期望 {0}，实际 {1}" -f (Hx $T[0]), (Hx $order[1]))
  Check '原 1 号顺移到 2 号位' ($order[2] -eq $T[1]) `
    ("期望 {0}，实际 {1}" -f (Hx $T[1]), (Hx $order[2]))

  # ---- 5) 反例控制：只点击不拖动，顺序不得变化 ----
  [TabDrag]::Click((TabX 1), $tabY)
  [TabDrag]::Click((TabX 1), $tabY)   # 同一位置再点一次（无位移）
  $order2 = @()
  foreach ($i in 0..2) {
    [TabDrag]::Click((TabX $i), $tabY)
    $order2 += (FgHwnd)
  }
  Write-Host ("  只点击不拖动后再点 0/1/2 -> " + (($order2 | ForEach-Object { Hx $_ }) -join ' '))
  Check '只点击不拖动不会改变顺序' `
    (($order2[0] -eq $order[0]) -and ($order2[1] -eq $order[1]) -and ($order2[2] -eq $order[2])) `
    ("[" + (($order2 | ForEach-Object { Hx $_ }) -join ' ') + "]")

  # ---- 6) 往回拖一次：把 2 号拖回最前，顺序应为 [T1, T2, T0] 的期望轮转 ----
  [TabDrag]::Drag((TabX 2), $tabY, $toX, $tabY, 16, '', 0, 0, 0, 0)
  Start-Sleep -Milliseconds 500
  $order3 = @()
  foreach ($i in 0..2) {
    [TabDrag]::Click((TabX $i), $tabY)
    $order3 += (FgHwnd)
  }
  Write-Host ("  再次拖动（2 -> 0）后 -> " + (($order3 | ForEach-Object { Hx $_ }) -join ' '))
  Check '第二次拖动把当前 2 号拖到最前' `
    (($order3[0] -eq $order[2]) -and ($order3[1] -eq $order[0]) -and ($order3[2] -eq $order[1])) `
    ("[" + (($order3 | ForEach-Object { Hx $_ }) -join ' ') + "]")
}
finally {
  # ---- 清理：关掉本脚本创建的 3 个资源管理器窗口 ----
  foreach ($w in $wins) { [TabDrag]::Close($w) }
  Start-Sleep -Milliseconds 800
  Remove-Item -Recurse -Force $baseDir -ErrorAction SilentlyContinue
}

Write-Host ''
if ($fail -eq 0) {
  Write-Host 'ALL TESTS PASSED' -ForegroundColor Green
  Write-Host ("截图：{0}\tab_drag_before.png / tab_drag_mid.png / tab_drag_after.png" -f '')
  exit 0
}
Write-Host ("{0} TEST(S) FAILED" -f $fail) -ForegroundColor Red
exit 1
