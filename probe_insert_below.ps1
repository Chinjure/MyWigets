# probe_insert_below.ps1 —— 验证“激活后再整组抬升”的两种手法哪种被窗口管理器允许。
#
# 已确认的背景（见 probe_raise_steps.ps1）：
#   后台进程 SetWindowPos(HWND_TOP) 抬另一个进程的窗口时，窗口管理器只允许
#   它排到“当前前台窗口”之下 —— Edge 在前台时怎么抬都上不去，所以旧实现里
#   “先抬其它窗口、再 SetForegroundWindow(target)” 必然留下一个窗口在 Edge 下。
#
# 本脚本用 AttachThreadInput 抢前台（仅测试工具使用；Dock 本体不能用，会卡死），
# 把钉钉主窗变成前台后再试两种抬升手法：
#   A) SetWindowPos(viewer, HWND_TOP, SWP_NOACTIVATE)
#   B) SetWindowPos(viewer, main, SWP_NOACTIVATE)   ← 插到前台窗口正下方
# 看哪种能让 viewer 落在 main 与 Edge 之间（两者都在 Edge 之上）。
$code = @'
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public class InsProbe {
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
  [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
  [DllImport("user32.dll")] public static extern uint SendInput(uint n, INPUT[] inputs, int size);
  [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a, uint b, bool attach);
  [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
  [DllImport("dwmapi.dll")] public static extern int DwmGetWindowAttribute(IntPtr h, int attr, out int val, int sz);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [StructLayout(LayoutKind.Sequential)] public struct INPUT { public uint type; public KEYBDINPUT ki; public int pad1, pad2, pad3; }
  [StructLayout(LayoutKind.Sequential)] public struct KEYBDINPUT { public ushort wVk, wScan; public uint dwFlags, time; public IntPtr dwExtraInfo; }

  const uint SWP_NOMOVE = 2, SWP_NOSIZE = 1, SWP_NOACTIVATE = 0x10;
  static readonly IntPtr HWND_TOP = IntPtr.Zero;

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
  public static string Name(IntPtr h) { int i = Z.IndexOf(h); return i < 0 ? "(none)" : i + ":" + N[i]; }

  // Test-only foreground grab: attach to the foreground thread so the call is
  // accepted (Dock must NOT do this: AttachThreadInput can block its UI thread)
  public static bool ForceForegroundAttached(IntPtr h) {
    IntPtr fg = GetForegroundWindow();
    uint fgPid = 0;
    uint fgThread = GetWindowThreadProcessId(fg, out fgPid);
    uint myThread = GetCurrentThreadId();
    bool ok;
    if (fgThread != myThread) {
      AttachThreadInput(myThread, fgThread, true);
      ok = SetForegroundWindow(h);
      AttachThreadInput(myThread, fgThread, false);
    } else {
      ok = SetForegroundWindow(h);
    }
    return ok;
  }

  public static bool ForceForegroundInject(IntPtr h) {
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
    return string.Format("{0,-34} fg={1,-32} viewer={2,-6} Edge={3,-6} main={4,-6}",
      tag, Name(GetForegroundWindow()), Idx(view), Idx(edge), Idx(main));
  }
}
'@
Add-Type -TypeDefinition $code -Language CSharp
[InsProbe]::SetProcessDPIAware() | Out-Null

$edge = [InsProbe]::Find('Chrome_WidgetWin_1', 'msedge', $true)
$main = [InsProbe]::Find('StandardFrame_DingTalk', '', $false)
$view = [InsProbe]::Find('DingImgViewWnd', '', $false)

# 0) 先把 Edge 弄到前台（用注入法；失败就用挂接法）
if (-not [InsProbe]::ForceForegroundInject($edge)) { [InsProbe]::ForceForegroundAttached($edge) | Out-Null }
Start-Sleep -Milliseconds 500
Write-Host ([InsProbe]::Line('[0] Edge 前台', $edge, $main, $view))

# 1) 激活钉钉主窗（整组流程的第一步）
$ok = [InsProbe]::ForceForegroundAttached($main)
Start-Sleep -Milliseconds 500
Write-Host ("    激活 main 返回 {0}" -f $ok)
Write-Host ([InsProbe]::Line('[1] 激活 main 后', $edge, $main, $view))

# 2A) 手法 A：激活后用 HWND_TOP 抬 viewer
[InsProbe]::SetWindowPos($view, [IntPtr]::Zero, 0, 0, 0, 0, 0x0002 -bor 0x0001 -bor 0x0010) | Out-Null
Start-Sleep -Milliseconds 400
Write-Host ([InsProbe]::Line('[2A] viewer ← HWND_TOP', $edge, $main, $view))

# 2B) 手法 B：把 viewer 插到 main 正下方
[InsProbe]::SetWindowPos($view, $main, 0, 0, 0, 0, 0x0002 -bor 0x0001 -bor 0x0010) | Out-Null
Start-Sleep -Milliseconds 400
Write-Host ([InsProbe]::Line('[2B] viewer ← main（插到前台窗下方）', $edge, $main, $view))

# 3) 再激活一次 main，看整组是否稳定保持
[InsProbe]::ForceForegroundAttached($main) | Out-Null
Start-Sleep -Milliseconds 600
Write-Host ([InsProbe]::Line('[3] 再次激活 main 后', $edge, $main, $view))
