# probe_band.ps1 -- step-by-step check of the dock-band reservation path
#   1. reset: keep-top off, dock auto-collapse on
#   2. keep-top on            -> work area top = top bar height, bottom still screen bottom
#   3. auto-collapse off      -> dock must expand and the work area bottom must stop
#                                at the dock's top edge
#   4. keep-top off           -> work area back to the full monitor
# State is left as it was found (keep-top off, auto-collapse on).
$ErrorActionPreference = 'Stop'
Add-Type -TypeDefinition @'
using System;using System.Text;using System.Runtime.InteropServices;
public class PB {
 [DllImport("user32.dll")] public static extern bool EnumWindows(P cb,IntPtr l);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h,StringBuilder s,int n);
 [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h,out RECT r);
 [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h,uint m,IntPtr w,IntPtr l);
 [DllImport("user32.dll",SetLastError=true)] public static extern bool SystemParametersInfoW(uint a,uint b,out RECT r,uint f);
 [StructLayout(LayoutKind.Sequential)] public struct RECT{public int L,T,R,B;}
 public delegate bool P(IntPtr h,IntPtr l);
 public static IntPtr Find(string cls){IntPtr f=IntPtr.Zero;EnumWindows((h,l)=>{var c=new StringBuilder(128);GetClassNameW(h,c,128);if(c.ToString()==cls){f=h;return false;}return true;},IntPtr.Zero);return f;}
 public static string R(RECT r){return string.Format("({0},{1},{2},{3})",r.L,r.T,r.R,r.B);}
 public static RECT Wa(){RECT r;SystemParametersInfoW(0x0030,0,out r,0);return r;}
}
'@
Add-Type -TypeDefinition @'
using System;using System.Text;using System.Runtime.InteropServices;
public class PB2 {
 [DllImport("user32.dll")] public static extern bool EnumWindows(P cb,IntPtr l);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h,StringBuilder s,int n);
 [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
 [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
 [DllImport("user32.dll")] public static extern bool IsZoomed(IntPtr h);
 [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h,out RECT r);
 [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h,int c);
 [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h,IntPtr a,int x,int y,int cx,int cy,uint f);
 [StructLayout(LayoutKind.Sequential)] public struct RECT{public int L,T,R,B;}
 public delegate bool P(IntPtr h,IntPtr l);
 public static string R(RECT r){return string.Format("({0},{1},{2},{3})",r.L,r.T,r.R,r.B);}
 public static IntPtr Pick(){
   IntPtr best=IntPtr.Zero;int bs=-1;
   EnumWindows((h,l)=>{
     if(!IsWindowVisible(h)||IsIconic(h)||IsZoomed(h))return true;
     var c=new StringBuilder(128);GetClassNameW(h,c,128);string cl=c.ToString();
     if(cl.StartsWith("Desktop")||cl=="MyWigetsTrayWindow"||cl=="Progman"||cl=="WorkerW"||cl=="Shell_TrayWnd")return true;
     RECT r;if(!GetWindowRect(h,out r))return true;
     if(r.B-r.T<500||r.R-r.L<500)return true;
     int sc=(cl=="Chrome_WidgetWin_1")?100:10;
     if(sc>bs){bs=sc;best=h;}
     return true;
   },IntPtr.Zero);
   return best;
 }
 public static string MaxRect(){
   string s="";
   EnumWindows((h,l)=>{
     if(!IsWindowVisible(h)||IsIconic(h)||!IsZoomed(h))return true;
     var c=new StringBuilder(128);GetClassNameW(h,c,128);string cl=c.ToString();
     if(cl.StartsWith("Desktop")||cl=="MyWigetsTrayWindow")return true;
     RECT r;GetWindowRect(h,out r);s=cl+" "+R(r);return false;
   },IntPtr.Zero);
   return s;
 }
}
'@

$DOCKREG='HKCU:\Software\DesktopSuite\Dock'; $TOPREG='HKCU:\Software\DesktopSuite\TopBar'
function Set-DockAuto([int]$v){ New-ItemProperty -Path $DOCKREG -Name 'AutoCollapse' -Value $v -PropertyType DWord -Force | Out-Null }
function Set-KeepTop([int]$v){ if(-not(Test-Path $TOPREG)){New-Item -Path $TOPREG -Force|Out-Null}; New-ItemProperty -Path $TOPREG -Name 'KeepTop' -Value $v -PropertyType DWord -Force | Out-Null }
function Notify([int]$v){ $d=[PB]::Find('DesktopDockWindow'); $b=[PB]::Find('DesktopTopBarWindow'); if($d -ne [IntPtr]::Zero){[void][PB]::PostMessageW($d,0x8000+18,[IntPtr]$v,[IntPtr]::Zero)}; if($b -ne [IntPtr]::Zero){[void][PB]::PostMessageW($b,0x8000+18,[IntPtr]$v,[IntPtr]::Zero)}; Start-Sleep -Milliseconds 900 }
function Snap($tag){
  $d=[PB]::Find('DesktopDockWindow'); $b=[PB]::Find('DesktopTopBarWindow')
  $dr=New-Object PB+RECT; if($d -ne [IntPtr]::Zero){[void][PB]::GetWindowRect($d,[ref]$dr)}
  $br=New-Object PB+RECT; if($b -ne [IntPtr]::Zero){[void][PB]::GetWindowRect($b,[ref]$br)}
  $wa=[PB]::Wa()
  $sh=[PB2]::R   # unused
  "$tag"
  "    workarea = " + [PB]::R($wa)
  "    dock     = " + [PB]::R($dr) + "  (top=" + $dr.T + " bottom=" + $dr.B + ")"
  "    topbar   = " + [PB]::R($br)
  "    maximized= " + [PB2]::MaxRect()
  return @{wa=$wa;dr=$dr;br=$br}
}

"--- step 1: reset (keep-top off, dock auto-collapse on) ---"
Set-DockAuto 1; Set-KeepTop 0; Notify 2
$s1 = Snap "  [reset]"

"--- step 2: keep-top ON ---"
Set-KeepTop 1; Notify 1
$s2 = Snap "  [keep-top on]"
if ($s2.wa.T -eq ($s2.br.B - $s2.br.T)) { "PASS  work area top == topbar height ($($s2.wa.T))" } else { "FAIL  work area top=$($s2.wa.T) barH=$($s2.br.B - $s2.br.T)" }

"--- step 3: dock auto-collapse OFF (dock must become always visible) ---"
Set-DockAuto 0
Start-Sleep -Seconds 6
$s3 = Snap "  [auto-collapse off]"
# Dock state comes from the dock's OWN log ("[展开] 完成 offset=0" /
# "[收起] 完成 offset=<winH>"): after BottomGap switched to measuring from the
# glass, the window rect alone no longer tells whether the dock is up, and the
# old formula (screenH - gap - winH) is off by the shadow margin.
function Get-DockState([int]$withinMs) {
    $log = 'C:\Users\Mayn\Desktop\clock\logs\dock.log'
    $deadline = (Get-Date).AddMilliseconds($withinMs)
    do {
        if (Test-Path $log) {
            $m = Select-String -Path $log -Pattern '\[(展开|收起)\] 完成 offset=([0-9.]+)' -Encoding UTF8 | Select-Object -Last 1
            if ($m) {
                return @($m.Matches[0].Groups[1].Value, [double]$m.Matches[0].Groups[2].Value)
            }
        }
        Start-Sleep -Milliseconds 300
    } while ((Get-Date) -lt $deadline)
    return @('unknown', -1)
}
$gap = (Get-ItemProperty -Path 'HKCU:\Software\DesktopSuite\Dock' -Name 'BottomGap' -ErrorAction SilentlyContinue).BottomGap
if ($null -eq $gap) { $gap = 0 }
$screenH = 1080
$st = Get-DockState 4000
if ($st[0] -eq '展开') {
    "PASS  dock expanded/visible (dock 日志：展开完成 offset=$($st[1])，窗口=" + [PB]::R($s3.dr) + ")"
} elseif ($st[0] -eq '收起') {
    "FAIL  dock still parked collapsed (dock 日志：收起完成 offset=$($st[1]))"
} else {
    "SKIP  dock state unknown (日志里没有展开/收起完成记录)"
}
# Ground truth for the reserved band: the dock logs, every time the reservation
# changes, "[保持顶栏] Dock 高度带：… 毛玻璃顶边(局部)=47 屏幕=<glass> … → 让出=<n>"
# and the work area bottom must sit exactly 2px above that glass top.
# (Comparing against a live window rect instead is unreliable: the dock window
# grows while hovered and moves during animations, which is what made this check
# look 6px off while the dock's own numbers were exact.)
$log = 'C:\Users\Mayn\Desktop\clock\logs\dock.log'
$m = Select-String -Path $log -Pattern 'Dock 高度带.*毛玻璃顶边\(局部\)=\d+ 屏幕=(\d+).*让出=(\d+)' -Encoding UTF8 | Select-Object -Last 1
if ($null -eq $m) {
    "SKIP  band ledger (dock log has no 高度带 line yet)"
} else {
    $glassTop = [int]$m.Matches[0].Groups[1].Value
    $reserve  = [int]$m.Matches[0].Groups[2].Value
    $expectedWaB = $glassTop - 2
    if ([math]::Abs($s3.wa.B - $expectedWaB) -le 3) {
        "PASS  2px gap above the dock glass (wa.B=$($s3.wa.B) glassTop=$glassTop expected~$expectedWaB reserve=$reserve)"
    } else {
        "FAIL  wa.B=$($s3.wa.B) glassTop=$glassTop expected~$expectedWaB reserve=$reserve"
    }
}

"--- step 4: keep-top OFF ---"
Set-KeepTop 0; Notify 2
Start-Sleep -Milliseconds 1500
$s4 = Snap "  [keep-top off]"
if ($s4.wa.T -eq 0 -and $s4.wa.B -ge 1080) { "PASS  work area back to the full monitor" } else { "FAIL  wa=" + [PB]::R($s4.wa) }

"--- cleanup: restore dock auto-collapse on ---"
Set-DockAuto 1
Start-Sleep -Seconds 3
"restored: AutoCollapse=1 KeepTop=0"
