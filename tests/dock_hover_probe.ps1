# tests/dock_hover_probe.ps1
#
# Dock 悬停命中探针（被动测量：不改 App、不改注册表）
#
# 原理：Dock 的「悬停放大」只在 pointerActive（光标在毛玻璃本体内 ∪ 底边触发条）
# 时为真，而放大直接改变呈现宽度。所以「窗口宽度」就是悬停是否生效的观测量：
#   悬停生效 → 宽度 = 静止宽 + Σ(scaleAnim-1)·44·dpi/96
#   悬停丢失 → 宽度回落到静止宽
#
# 用法：
#   powershell -NoProfile -ExecutionPolicy Bypass -File tests/dock_hover_probe.ps1
#   ... -ScanY            # 垂直扫描（x=Dock 水平中心）
#   ... -ScanX            # 水平扫描（y=图标中心）
#   ... -Shot <path.png>  # 在「悬停态」截取 Dock 区域
param(
    [switch]$ScanX,
    [switch]$ScanY,
    [switch]$Assert,
    [string]$Shot = '',
    [int]$ScanXAt = 0,
    [int]$Step = 8,
    [int]$Dwell = 260,
    [int]$Settle = 750
)

$ErrorActionPreference = 'Stop'

Add-Type @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public class DockHoverProbe {
  public delegate bool Proc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(Proc cb, IntPtr l);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT p);
  [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern int GetSystemMetrics(int i);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
  public static string Cls(IntPtr h) { var s = new StringBuilder(256); GetClassNameW(h, s, 256); return s.ToString(); }
  public static IntPtr FindDock() {
    IntPtr found = IntPtr.Zero;
    EnumWindows(delegate(IntPtr h, IntPtr l) {
      if (Cls(h) == "DesktopDockWindow") { found = h; return false; }
      return true;
    }, IntPtr.Zero);
    return found;
  }
  public static RECT Rect(IntPtr h) { RECT r; GetWindowRect(h, out r); return r; }
  public static int W(IntPtr h) { RECT r; GetWindowRect(h, out r); return r.Right - r.Left; }
  public static int H(IntPtr h) { RECT r; GetWindowRect(h, out r); return r.Bottom - r.Top; }
}
'@

[void][DockHoverProbe]::SetProcessDPIAware()
$h = [DockHoverProbe]::FindDock()
if ($h -eq [IntPtr]::Zero) { throw 'Dock window not found (DesktopDockWindow)' }

$dpi = [DockHoverProbe]::GetDpiForWindow($h)
$k = $dpi / 96.0
$SW = [DockHoverProbe]::GetSystemMetrics(0)
$SH = [DockHoverProbe]::GetSystemMetrics(1)

# src/dock_main.cpp 常量
$kShadowBottom = 7.0; $kPadTop = 5.0; $kIconSize = 44.0; $kPadBottom = 8.0
$kMaxScale = 1.66; $kIconGapHalf = 3.5; $kShadowMargin = 14.0; $kBarPadX = 0.0

function Show-Rect([string]$tag, $r) {
    Write-Host ("{0}: x={1} y={2} w={3} h={4} bottom={5}" -f $tag,
        $r.Left, $r.Top, ($r.Right - $r.Left), ($r.Bottom - $r.Top), $r.Bottom)
}

# ---- 1) 静止宽：光标移开，等自动收起 ----
[void][DockHoverProbe]::SetCursorPos([int]($SW / 2), [int]($SH / 2))
Start-Sleep -Milliseconds 1400
$rIdle = [DockHoverProbe]::Rect($h)
$idleW = $rIdle.Right - $rIdle.Left
Show-Rect 'idle(collapsed)' $rIdle

# ---- 2) 从底边触发条展开（轮询等待真正展开，别用固定 sleep：
#         固定等待曾在机器忙/动画慢时读到收起位，后面所有 y 全算到屏幕外）----
function Wait-Expanded([int]$tries = 20) {
    for ($i = 0; $i -lt $tries; $i++) {
        $r = [DockHoverProbe]::Rect($h)
        if ($r.Top -lt $SH - 10) { return $r }
        [void][DockHoverProbe]::SetCursorPos([int]($SW / 2), $SH - 1)
        Start-Sleep -Milliseconds 200
    }
    throw 'Dock 未能展开（下缘触发条悬停无效），无法测量悬停几何'
}

[void][DockHoverProbe]::SetCursorPos([int]($SW / 2), $SH - 1)
Start-Sleep -Milliseconds 300
$rExp = Wait-Expanded
Show-Rect 'expanded(strip)' $rExp
$winW = $rExp.Right - $rExp.Left
$winH = $rExp.Bottom - $rExp.Top
$contentH = ($kPadTop + $kIconSize + $kPadBottom) * $k
$glassTopLocal = $winH - $kShadowBottom * $k - $contentH
$glassBottomLocal = $winH - $kShadowBottom * $k
$iconBottomLocal = $glassTopLocal + $contentH - $kPadBottom * $k
$iconCenterIdle = $iconBottomLocal - $kIconSize * $k / 2
$iconTopMaxLocal = $iconBottomLocal - $kIconSize * $k * $kMaxScale
$glassTopScreen = $rExp.Top + $glassTopLocal

Write-Host ("dpi={0} k={1:N2} winW={2} winH={3} idleW={4}" -f $dpi, $k, $winW, $winH, $idleW)
Write-Host ("glass: local top={0:N1} bottom={1:N1} | screen top={2:N1}" -f $glassTopLocal, $glassBottomLocal, $glassTopScreen)
Write-Host ("icon idle top={0:N1} bottom={1:N1} center={2:N1} | peak top={3:N1} (overhang above glass {4:N1}px)" -f `
    ($iconBottomLocal - $kIconSize * $k), $iconBottomLocal, $iconCenterIdle, `
    $iconTopMaxLocal, ($glassTopLocal - $iconTopMaxLocal))

$cx = [int](($rExp.Left + $rExp.Right) / 2)
$baseY = [int]($rExp.Top + $iconCenterIdle)

if ($Shot -ne '') {
    Add-Type -AssemblyName System.Drawing
    [void][DockHoverProbe]::SetCursorPos($cx, $baseY)
    Start-Sleep -Milliseconds 600
    $rNow = [DockHoverProbe]::Rect($h)
    $pad = 40
    $x0 = [Math]::Max(0, $rNow.Left - $pad)
    $y0 = [Math]::Max(0, $rNow.Top - $pad)
    $w = [Math]::Min($SW - $x0, ($rNow.Right - $x0) + $pad)
    $hh = [Math]::Min($SH - $y0, ($rNow.Bottom - $y0) + $pad)
    $bmp = New-Object System.Drawing.Bitmap($w, $hh)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($x0, $y0, 0, 0, (New-Object System.Drawing.Size($w, $hh)))
    $bmp.Save($Shot, [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $bmp.Dispose()
    Write-Host ("shot saved: {0} (region {1},{2} {3}x{4}, winTop={5})" -f $Shot, $x0, $y0, $w, $hh, $rNow.Top)
}

# 基准：悬停到图标中心高度
[void][DockHoverProbe]::SetCursorPos($cx, $baseY)
Start-Sleep -Milliseconds 600
$hoverW = [DockHoverProbe]::W($h)
Write-Host ("baseline hover (x={0}, y={1}) w={2} (idle {3}, magnify +{4})" -f `
    $cx, $baseY, $hoverW, $idleW, ($hoverW - $idleW))

function Test-HoverAt([int]$x, [int]$y) {
    # 先回触发条：既保证「已展开」（否则读到的是收起位/上一态的残留），
    # 也避免上一点的收起动画污染本次读数
    [void][DockHoverProbe]::SetCursorPos($cx, $SH - 1)
    for ($i = 0; $i -lt 15; $i++) {
        Start-Sleep -Milliseconds 150
        if ([DockHoverProbe]::Rect($h).Top -lt $SH - 10) { break }
        [void][DockHoverProbe]::SetCursorPos($cx, $SH - 1)
    }
    [void][DockHoverProbe]::SetCursorPos($x, $y)
    Start-Sleep -Milliseconds $Dwell
    return [DockHoverProbe]::Rect($h)
}

$ExpTop = $rExp.Top

if ($Assert) {
    Write-Host ''
    Write-Host '--- assertions (hover region = glass body + bottom strip) ---'
    # 悬停区口径：真实毛玻璃矩形（含 1px 容差）∪ 屏幕下缘触发条。
    # 回归守卫：帧内自愈曾经把整条判定矩形抬高 kShadowBottom*k（150% DPI=10px），
    # 于是「毛玻璃底边往上 10px」被判成不在 Dock —— 悬停放大在贴近屏幕底边的
    # 一整条带里失效（用户报的就是这个）；同时「毛玻璃顶边往上 10px」被判成
    # 在 Dock（没碰到就 hover）。以下两条各守一边。
    $Dwell = 700   # 让缩放缓动彻底收敛，避免读到衰减尾巴
    $cases = @(
        @{ name = 'above glass top (-6px) expect OFF'; y = [int]($ExpTop + $glassTopLocal - 6); want = $false },
        @{ name = 'inside glass top (+4px) expect ON'; y = [int]($ExpTop + $glassTopLocal + 4); want = $true },
        @{ name = 'icon center expect ON'; y = $baseY; want = $true },
        @{ name = 'near bottom (SH-6, old dead band) expect ON'; y = $SH - 6; want = $true },
        @{ name = 'screen last row (SH-1) expect ON'; y = $SH - 1; want = $true }
    )
    $fail = 0
    foreach ($c in $cases) {
        $r = Test-HoverAt $cx $c.y
        $w = $r.Right - $r.Left
        $on = $w -gt $idleW + 8
        $ok = ($on -eq $c.want)
        if (-not $ok) { $fail++ }
        Write-Host ("[{0}] {1}: y={2} w={3} hover={4}" -f `
            $(if ($ok) { 'PASS' } else { 'FAIL' }), $c.name, $c.y, $w, `
            $(if ($on) { 'ON' } else { 'OFF' }))
    }
    if ($fail -gt 0) {
        Write-Host ("assertions FAILED: {0}" -f $fail)
        exit 1
    }
    Write-Host 'all assertions PASS'
}

if ($ScanY) {
    Write-Host ''
    Write-Host '--- vertical scan (x = dock center; collapse = 相对展开位的顶边偏移) ---'
    for ($y = $ExpTop + 2; $y -lt $ExpTop + 170; $y += 4) {
        $r = Test-HoverAt $cx $y
        $w = $r.Right - $r.Left
        $localExp = $y - $ExpTop      # 相对「展开态」窗口顶边
        $mark = ''
        if ([Math]::Abs($localExp - $glassTopLocal) -lt 2) { $mark = '  <- glass top' }
        if ([Math]::Abs($localExp - $iconTopMaxLocal) -lt 2) { $mark = '  <- peak icon top' }
        Write-Host ("y={0,5} localExp={1,6:N1} collapse={2,4} w={3,5} hover={4}{5}" -f `
            $y, $localExp, ($r.Top - $ExpTop), $w, `
            $(if ($w -gt $idleW + 8) { 'ON ' } else { 'OFF' }), $mark)
    }
}

if ($ScanX) {
    $y = if ($ScanXAt -gt 0) { $ScanXAt } else { $baseY }
    Write-Host ''
    Write-Host ("--- horizontal scan (y = {0}) ---" -f $y)
    for ($x = $rExp.Left + 4; $x -lt $rExp.Right - 4; $x += $Step) {
        $r = Test-HoverAt ([int]$x) $y
        $w = $r.Right - $r.Left
        Write-Host ("x={0,5} localExp={1,6:N1} collapse={2,4} w={3,5} hover={4}" -f `
            $x, ($x - $rExp.Left), ($r.Top - $ExpTop), $w, `
            $(if ($w -gt $idleW + 8) { 'ON ' } else { 'OFF' }))
    }
}

[void][DockHoverProbe]::SetCursorPos([int]($SW / 2), [int]($SH / 2))
