# probe_fs_verify.ps1 -- end-to-end: web page playing FULLSCREEN VIDEO must hide
# the top bar and the dock (yield), and a plain maximized window must NOT.
$ErrorActionPreference='Stop'
Add-Type -TypeDefinition @'
using System;using System.Text;using System.Runtime.InteropServices;
public class FV2 {
 [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
 [DllImport("user32.dll")] public static extern bool EnumWindows(P cb,IntPtr l);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h,StringBuilder s,int n);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h,StringBuilder s,int n);
 [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h,out RECT r);
 [DllImport("user32.dll")] public static extern bool IsZoomed(IntPtr h);
 [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
 [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h,int c);
 [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h,uint m,IntPtr w,IntPtr l);
 [DllImport("user32.dll")] public static extern void keybd_event(byte vk,byte scan,uint flags,UIntPtr extra);
 [DllImport("dwmapi.dll")] public static extern int DwmGetWindowAttribute(IntPtr h,int a,out RECT r,int s);
 [DllImport("dwmapi.dll",EntryPoint="DwmGetWindowAttribute")] public static extern int DwmGetWindowAttributeInt32(IntPtr h,int a,ref int v,int s);
 [DllImport("shell32.dll")] public static extern int SHQueryUserNotificationState(out int st);
 [DllImport("user32.dll")] public static extern IntPtr MonitorFromWindow(IntPtr h,uint f);
 [DllImport("user32.dll")] public static extern bool GetMonitorInfoW(IntPtr m,ref MONITORINFO mi);
 [StructLayout(LayoutKind.Sequential)] public struct RECT{public int L,T,R,B;}
 [StructLayout(LayoutKind.Sequential)] public struct MONITORINFO{public int cbSize;public RECT rcMonitor;public RECT rcWork;public uint dwFlags;}
 public delegate bool P(IntPtr h,IntPtr l);
 public static string Cls(IntPtr h){var s=new StringBuilder(128);GetClassNameW(h,s,128);return s.ToString();}
 public static string Title(IntPtr h){var s=new StringBuilder(200);GetWindowTextW(h,s,200);return s.ToString();}
 public static string R(RECT r){return string.Format("({0},{1},{2},{3})",r.L,r.T,r.R,r.B);}
 public static IntPtr Find(string cls){IntPtr f=IntPtr.Zero;EnumWindows((h,l)=>{var c=new StringBuilder(128);GetClassNameW(h,c,128);if(c.ToString()==cls){f=h;return false;}return true;},IntPtr.Zero);return f;}
 public static IntPtr FindChrome(){
   IntPtr f=IntPtr.Zero;
   EnumWindows((h,l)=>{ if(!IsWindowVisible(h))return true;
     var c=new StringBuilder(128);GetClassNameW(h,c,128);
     if(c.ToString()=="Chrome_WidgetWin_1"){var t=new StringBuilder(200);GetWindowTextW(h,t,200);
       if(t.ToString().Contains("FS-VIDEO-TEST")){f=h;return false;}}
     return true;},IntPtr.Zero);
   return f;
 }
 public static RECT Mon(IntPtr h){var mi=new MONITORINFO();mi.cbSize=Marshal.SizeOf(typeof(MONITORINFO));var m=MonitorFromWindow(h,2);if(m!=IntPtr.Zero)GetMonitorInfoW(m,ref mi);return mi.rcMonitor;}
 public static int Quns(){int st=-1;SHQueryUserNotificationState(out st);return st;}
 public static RECT Frame(IntPtr h){RECT r;if(DwmGetWindowAttribute(h,9,out r,Marshal.SizeOf(typeof(RECT)))!=0)GetWindowRect(h,out r);return r;}
 public static int Cloaked(IntPtr h){int c=0;DwmGetWindowAttributeInt32(h,14,ref c,4);return c;}
 public static bool Covers(RECT r,RECT m,int tol){return r.L<=m.L+tol&&r.T<=m.T+tol&&r.R>=m.R-tol&&r.B>=m.B-tol;}
 public static bool Verdict(){
   IntPtr h=GetForegroundWindow();RECT wr;GetWindowRect(h,out wr);RECT fr=Frame(h);RECT m=Mon(h);
   bool wc=Covers(wr,m,1), fc=Covers(fr,m,1);
   bool overhang = wr.L<m.L-1||wr.T<m.T-1||wr.R>m.R+1||wr.B>m.B+1;
   return (wc||fc) && !overhang;
 }
 public static string Desc(){
   IntPtr h=GetForegroundWindow();RECT wr;GetWindowRect(h,out wr);RECT fr=Frame(h);RECT m=Mon(h);
   return string.Format("fg={0} window={1} frame={2} mon={3} IsZoomed={4} QUNS={5} => 判定全屏={6}",
     Cls(h),R(wr),R(fr),R(m),IsZoomed(h),Quns(),Verdict());
 }
 public static string Suite(){
   IntPtr d=Find("DesktopDockWindow"),b=Find("DesktopTopBarWindow");
   return "dock="+(d==IntPtr.Zero?"none":(IsWindowVisible(d)?"visible":"hidden"))+
          "  topbar="+(b==IntPtr.Zero?"none":(IsWindowVisible(b)?"visible":"hidden"));
 }
 public static void Esc(){ keybd_event(0x1B,0,0,UIntPtr.Zero); keybd_event(0x1B,0,2,UIntPtr.Zero); }
 public static void ClickAt(IntPtr h,int cx,int cy){
   IntPtr lp=(IntPtr)((cy<<16)|(cx&0xFFFF));
   PostMessageW(h,0x0200,IntPtr.Zero,lp);
   System.Threading.Thread.Sleep(120);
   PostMessageW(h,0x0201,(IntPtr)1,lp);
   System.Threading.Thread.Sleep(60);
   PostMessageW(h,0x0202,IntPtr.Zero,lp);
 }
}
'@
$chrome='C:\Program Files\Google\Chrome\Application\chrome.exe'
$page='file:///C:/Users/Mayn/Desktop/clock/fs_video_test.html'
$fails=0
function Chk($name,$cond,$detail){ if($cond){"PASS  $name  ($detail)"}else{$script:fails++;"FAIL  $name  ($detail)"} }
Get-Process chrome -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowTitle -like '*FS-VIDEO-TEST*' } | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 800
Start-Process -FilePath $chrome -ArgumentList @('--new-window',$page) | Out-Null
Start-Sleep -Seconds 7
"[baseline: 普通窗口]" ; [FV2]::Desc() ; [FV2]::Suite()
$h=[FV2]::FindChrome()
[FV2]::ClickAt($h, 900, 500)      # 真实点击 → HTML5 全屏
Start-Sleep -Seconds 4
"[网页播放全屏视频]" ; [FV2]::Desc()
Chk "判据认为这是全屏" ([FV2]::Verdict()) ([FV2]::Desc())
$suite=[FV2]::Suite()
Chk "Dock 已让位隐藏" ($suite -match 'dock=hidden') $suite
Chk "顶栏已让位隐藏" ($suite -match 'topbar=hidden') $suite
# 退出全屏：Esc（HTML5 Fullscreen API 的标准退出方式），确认恢复
[FV2]::Esc()
Start-Sleep -Seconds 3
"[退出全屏后]" ; [FV2]::Desc() ; [FV2]::Suite()
$suite2=[FV2]::Suite()
Chk "Dock 已恢复常驻" ($suite2 -match 'dock=visible') $suite2
Chk "顶栏已恢复显示" ($suite2 -match 'topbar=visible') $suite2
# 对照：普通最大化窗口不该被判成全屏
Get-Process chrome -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowTitle -like '*FS-VIDEO-TEST*' } | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 800
Start-Process -FilePath $chrome -ArgumentList @('--new-window',$page) | Out-Null
Start-Sleep -Seconds 6
$h=[FV2]::FindChrome()
$dummy=[FV2]::Find("Shell_TrayWnd")
[void][FV2]::ShowWindow($h,3)      # SW_MAXIMIZE
Start-Sleep -Seconds 3
"[对照：普通最大化窗口]"; [FV2]::Desc()
Chk "最大化窗口不被判成全屏" (-not [FV2]::Verdict()) ([FV2]::Desc())
$suite3=[FV2]::Suite()
Chk "最大化时 Dock 未让位（保持常驻）" ($suite3 -match 'dock=visible') $suite3
""
"=== summary: $fails failure(s) ==="
