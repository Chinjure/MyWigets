# 探针：按 Z 序列举顶层窗口（含 class / style / owner / 进程），用于诊断
# “点击 Dock 图标只把一个窗口提到最前，另一个仍在其他应用之下”的问题。
# 用法：powershell -NoProfile -ExecutionPolicy Bypass -File probe_zorder.ps1 [-Filter ding]
param([string]$Filter = "")

$code = @'
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;

public class ZProbe {
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc cb, IntPtr lp);
  public delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);
  [DllImport("user32.dll")] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern IntPtr GetWindow(IntPtr h, uint cmd);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll", EntryPoint="GetWindowLongPtrW")] public static extern IntPtr GetWindowLongPtrW(IntPtr h, int idx);
  [DllImport("user32.dll", EntryPoint="GetWindowLongW")] public static extern int GetWindowLongW(IntPtr h, int idx);
  [DllImport("dwmapi.dll")] public static extern int DwmGetWindowAttribute(IntPtr h, int attr, out int val, int sz);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }

  public static IntPtr ExStyle(IntPtr h) {
    return IntPtr.Size == 8 ? GetWindowLongPtrW(h, -20) : (IntPtr)GetWindowLongW(h, -20);
  }
  public static IntPtr Style(IntPtr h) {
    return IntPtr.Size == 8 ? GetWindowLongPtrW(h, -16) : (IntPtr)GetWindowLongW(h, -16);
  }
  public static List<string> Dump(string filter) {
    var outp = new List<string>();
    int z = 0;
    EnumWindows((h, lp) => {
      uint pid; GetWindowThreadProcessId(h, out pid);
      string proc = "";
      try { proc = System.Diagnostics.Process.GetProcessById((int)pid).ProcessName; } catch {}
      if (!string.IsNullOrEmpty(filter) &&
          proc.IndexOf(filter, StringComparison.OrdinalIgnoreCase) < 0) return true;
      var cls = new StringBuilder(128); GetClassNameW(h, cls, 128);
      var txt = new StringBuilder(256); GetWindowTextW(h, txt, 256);
      RECT r; GetWindowRect(h, out r);
      IntPtr owner = GetWindow(h, 4); // GW_OWNER
      long st = Style(h).ToInt64(), ex = ExStyle(h).ToInt64();
      int cloaked = 0; DwmGetWindowAttribute(h, 14, out cloaked, 4);
      outp.Add(string.Format(
        "z={0,3} hwnd=0x{1:X8} pid={2,-6} vis={3} min={4} cloak={5} own=0x{6:X8} rect=({7},{8},{9},{10}) {11}x{12} style=0x{13:X8} ex=0x{14:X8} cls={15} title={16}",
        z++, h.ToInt64(), pid, IsWindowVisible(h) ? 1 : 0, IsIconic(h) ? 1 : 0, cloaked,
        owner.ToInt64(), r.L, r.T, r.R, r.B, r.R - r.L, r.B - r.T, st, ex,
        cls.ToString(), txt.ToString().Replace("\n", " ")));
      return true;
    }, IntPtr.Zero);
    return outp;
  }
}
'@
Add-Type -TypeDefinition $code -Language CSharp

$out = [ZProbe]::Dump($Filter)
$path = Join-Path $PSScriptRoot "zorder_dump.txt"
[System.IO.File]::WriteAllLines($path, $out, (New-Object System.Text.UTF8Encoding($false)))
Write-Host "wrote $($out.Count) lines -> $path"
