# probe_frame_inset.ps1 —— 只读探针：对比 GetWindowRect 与 DWM 扩展边框，
# 量出「窗口矩形」相对「可见边框」多出的不可见边缘（左/上/右/下）。
# 用于确认顶栏「填充顶栏以外区域」时是否需要补偿这圈边缘，否则窗口四周会漏缝。
$cs = @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public class FI {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("dwmapi.dll")] public static extern int DwmGetWindowAttribute(IntPtr h, int attr, out RECT r, int size);
  [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr ctx);
  public struct RECT { public int L, T, R, B; }
  public const int DWMWA_EXTENDED_FRAME_BOUNDS = 9;
}
'@
Add-Type -TypeDefinition $cs

# 关键：进程必须先声明 DPI 感知。否则 GetWindowRect 返回被系统虚拟化过的
# 逻辑坐标，而 DwmGetWindowAttribute 恒为物理像素，两者相差缩放倍率，量出来
# 的「内缩量」是假的（本机 150% 下会看到上千像素的离谱差值）。
[FI]::SetProcessDpiAwarenessContext([IntPtr](-4)) | Out-Null  # PER_MONITOR_AWARE_V2

$rows = New-Object System.Collections.ArrayList
$cb = [FI+EnumProc]{
  param($h, $l)
  if (-not [FI]::IsWindowVisible($h)) { return $true }
  if ([FI]::IsIconic($h)) { return $true }
  $wr = New-Object FI+RECT
  if (-not [FI]::GetWindowRect($h, [ref]$wr)) { return $true }
  $ext = New-Object FI+RECT
  $hr = [FI]::DwmGetWindowAttribute($h, [FI]::DWMWA_EXTENDED_FRAME_BOUNDS, [ref]$ext, 16)
  if ($hr -ne 0) { return $true }
  $cls = New-Object System.Text.StringBuilder 256
  [FI]::GetClassName($h, $cls, 256) | Out-Null
  $ttl = New-Object System.Text.StringBuilder 256
  [FI]::GetWindowText($h, $ttl, 256) | Out-Null
  $null = $rows.Add([pscustomobject]@{
    Class = $cls.ToString()
    Title = $ttl.ToString()
    InL = $ext.L - $wr.L
    InT = $ext.T - $wr.T
    InR = $wr.R - $ext.R
    InB = $wr.B - $ext.B
    SizeW = $wr.R - $wr.L
    SizeH = $wr.B - $wr.T
  })
  return $true
}
[FI]::EnumWindows($cb, [IntPtr]::Zero) | Out-Null
$rows | Sort-Object -Property InL -Descending | Format-Table -AutoSize | Out-String -Width 200
