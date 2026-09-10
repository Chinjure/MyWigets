# probe_raise_steps.ps1 —— 分步复现 Dock 的“整组抬升 + 抢前台”流程，逐步打印 Z 序，
# 定位到底哪一步没把钉钉主窗抬到 Edge 之上。
#
# 步骤：Edge 抢前台 → 抬 viewer → 抬 main → ALT+SetForegroundWindow(main)，
# 每步后打印 [viewer / Edge / main] 的 Z 序名次与前台窗口。
param([int]$StepSettleMs = 350)

$code = @'
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public class StepProbe {
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr lp);
  public delegate bool EnumProc(IntPtr h, IntPtr lp);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern IntPtr GetWindow(IntPtr h, uint cmd);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
  [DllImport("user32.dll")] public static extern uint SendInput(uint n, INPUT[] inputs, int size);
  [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a, uint b, bool attach);
  [DllImport("user32.dll")] public static extern IntPtr SetActiveWindow(IntPtr h);
  [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
  [DllImport("dwmapi.dll")] public static extern int DwmGetWindowAttribute(IntPtr h, int attr, out int val, int sz);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [StructLayout(LayoutKind.Sequential)] public struct INPUT { public uint type; public KEYBDINPUT ki; public int pad1, pad2, pad3; }
  [StructLayout(LayoutKind.Sequential)] public struct KEYBDINPUT { public ushort wVk, wScan; public uint dwFlags, time; public IntPtr dwExtraInfo; }

  static readonly IntPtr HWND_TOP = IntPtr.Zero;
  const uint SWP_NOMOVE = 2, SWP_NOSIZE = 1, SWP_NOACTIVATE = 0x10;

  public static List<IntPtr> Z = new List<IntPtr>();
  public static List<string> N = new List<string>();

  public static void Snapshot() {
    Z.Clear(); N.Clear();
    EnumWindows((h, lp) => {
      if (!IsWindowVisible(h) || IsIconic(h)) return true;
      if (GetWindow(h, 4) != IntPtr.Zero) return true;
      int c = 0; DwmGetWindowAttribute(h, 14, out c, 4); if (c != 0) return true;
      RECT r; GetWindowRect(h, out r);
      if (r.R - r.L <= 0 || r.B - r.T <= 0) return true;
      uint pid; GetWindowThreadProcessId(h, out pid);
      string p = ""; try { p = System.Diagnostics.Process.GetProcessById((int)pid).ProcessName; } catch {}
      var cls = new StringBuilder(96); GetClassNameW(h, cls, 96);
      Z.Add(h); N.Add(p + "/" + cls.ToString());
      return true;
    }, IntPtr.Zero);
  }

  public static int Idx(IntPtr h) { return Z.IndexOf(h); }
  public static string Name(IntPtr h) { int i = Z.IndexOf(h); return i < 0 ? "(not visible)" : i + ":" + N[i]; }

  public static bool Raise(IntPtr h, out int err) {
    bool ok = SetWindowPos(h, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    err = Marshal.GetLastWin32Error();
    return ok;
  }

  public static bool ForceForeground(IntPtr h) {
    INPUT[] alt = new INPUT[2];
    alt[0].type = 1; alt[0].ki.wVk = 0x12;
    alt[1].type = 1; alt[1].ki.wVk = 0x12; alt[1].ki.dwFlags = 2;
    SendInput(2, alt, Marshal.SizeOf(typeof(INPUT)));
    return SetForegroundWindow(h);
  }

  public static IntPtr Find(string cls, string proc, bool largest) {
    IntPtr found = IntPtr.Zero; long best = -1;
    EnumWindows((h, lp) => {
      if (!IsWindowVisible(h) || IsIconic(h)) return true;
      var sb = new StringBuilder(96); GetClassNameW(h, sb, 96);
      if (sb.ToString() != cls) return true;
      if (proc.Length > 0) {
        uint pid; GetWindowThreadProcessId(h, out pid);
        string p = ""; try { p = System.Diagnostics.Process.GetProcessById((int)pid).ProcessName; } catch {}
        if (p.IndexOf(proc, StringComparison.OrdinalIgnoreCase) < 0) return true;
      }
      RECT r; GetWindowRect(h, out r);
      long a = (long)(r.R - r.L) * (r.B - r.T);
      if (!largest) { found = h; return false; }
      if (a > best) { best = a; found = h; }
      return true;
    }, IntPtr.Zero);
    return found;
  }

  public static string Line(string tag, IntPtr edge, IntPtr main, IntPtr view) {
    Snapshot();
    return string.Format("{0,-22} fg={1,-34} viewer={2,-6} Edge={3,-6} main={4,-6}",
      tag, Name(GetForegroundWindow()), Idx(view), Idx(edge), Idx(main));
  }
}
'@
Add-Type -TypeDefinition $code -Language CSharp
[StepProbe]::SetProcessDPIAware() | Out-Null

$edge  = [StepProbe]::Find('Chrome_WidgetWin_1', 'msedge', $true)
$main  = [StepProbe]::Find('StandardFrame_DingTalk', '', $false)
$view  = [StepProbe]::Find('DingImgViewWnd', '', $false)
Write-Host ("Edge=0x{0:X} main=0x{1:X} viewer=0x{2:X}" -f $edge.ToInt64(), $main.ToInt64(), $view.ToInt64())

[StepProbe]::ForceForeground($edge) | Out-Null
Start-Sleep -Milliseconds $StepSettleMs
Write-Host ([StepProbe]::Line('[1] Edge 抢前台', $edge, $main, $view))

$err = 0
$ok = [StepProbe]::Raise($view, [ref]$err)
Write-Host ("    抬 viewer: ok={0} err={1}" -f $ok, $err)
Start-Sleep -Milliseconds $StepSettleMs
Write-Host ([StepProbe]::Line('[2] 抬 viewer', $edge, $main, $view))

$ok = [StepProbe]::Raise($main, [ref]$err)
Write-Host ("    抬 main  : ok={0} err={1}" -f $ok, $err)
Start-Sleep -Milliseconds $StepSettleMs
Write-Host ([StepProbe]::Line('[3] 抬 main', $edge, $main, $view))

$ok = [StepProbe]::ForceForeground($main)
Write-Host ("    ALT+SetForegroundWindow(main) 返回 {0}" -f $ok)
Start-Sleep -Milliseconds $StepSettleMs
Write-Host ([StepProbe]::Line('[4] 激活 main 后', $edge, $main, $view))
Start-Sleep -Milliseconds 900
Write-Host ([StepProbe]::Line('[5] 稳定后', $edge, $main, $view))
