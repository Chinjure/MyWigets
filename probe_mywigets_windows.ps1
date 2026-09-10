# 列出某个进程（默认 MyWigets-x64）的全部顶层窗口：类名/标题/可见性。
# 用于确认 MyWigets 宿主窗口类名（WM_CLOSE 优雅退出需要）。
param([int]$Pid_ = 0, [string]$Name = 'MyWigets-x64')

$code = @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public class WinDump {
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr lp);
  public delegate bool EnumProc(IntPtr h, IntPtr lp);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr GetWindow(IntPtr h, uint cmd);

  public static System.Collections.Generic.List<string> Dump(uint wantPid) {
    var outp = new System.Collections.Generic.List<string>();
    EnumWindows((h, lp) => {
      uint pid; GetWindowThreadProcessId(h, out pid);
      if (pid != wantPid) return true;
      var c = new StringBuilder(128); GetClassNameW(h, c, 128);
      var t = new StringBuilder(160); GetWindowTextW(h, t, 160);
      outp.Add(string.Format("0x{0:X8} vis={1} owner=0x{2:X8} cls={3} title={4}",
        h.ToInt64(), IsWindowVisible(h) ? 1 : 0, GetWindow(h, 4).ToInt64(),
        c.ToString(), t.ToString()));
      return true;
    }, IntPtr.Zero);
    return outp;
  }
}
'@
Add-Type -TypeDefinition $code -Language CSharp

$proc = if ($Pid_ -gt 0) { Get-Process -Id $Pid_ -ErrorAction Stop }
        else { Get-Process -Name $Name -ErrorAction Stop | Select-Object -First 1 }
$lines = [WinDump]::Dump([uint32]$proc.Id)
Write-Host ("pid={0} name={1} windows={2}" -f $proc.Id, $proc.ProcessName, $lines.Count)
$lines | ForEach-Object { Write-Host $_ }
