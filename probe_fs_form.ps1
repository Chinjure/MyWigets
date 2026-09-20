# probe_fs_form.ps1 -- "except fullscreen" check: with keep-top ON, a borderless
# fullscreen window on the primary monitor must make the top bar hide; closing it
# must bring the bar back. Uses a WinForms form (same trick as verify_fullscreen.ps1)
# so no app has to be launched or killed.
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
Add-Type -TypeDefinition @'
using System;using System.Text;using System.Runtime.InteropServices;
public class FF {
 [DllImport("user32.dll")] public static extern bool EnumWindows(P cb,IntPtr l);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h,StringBuilder s,int n);
 [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
 [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h,out RECT r);
 [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h,uint m,IntPtr w,IntPtr l);
 [DllImport("user32.dll",SetLastError=true)] public static extern bool SystemParametersInfoW(uint a,uint b,out RECT r,uint f);
 [StructLayout(LayoutKind.Sequential)] public struct RECT{public int L,T,R,B;}
 public delegate bool P(IntPtr h,IntPtr l);
 public static IntPtr Find(string cls){IntPtr f=IntPtr.Zero;EnumWindows((h,l)=>{var c=new StringBuilder(128);GetClassNameW(h,c,128);if(c.ToString()==cls){f=h;return false;}return true;},IntPtr.Zero);return f;}
 public static string R(RECT r){return string.Format("({0},{1},{2},{3})",r.L,r.T,r.R,r.B);}
 public static RECT Wa(){RECT r;SystemParametersInfoW(0x0030,0,out r,0);return r;}
 public static string BarState(){
   IntPtr f=IntPtr.Zero;
   EnumWindows((h,l)=>{var c=new StringBuilder(128);GetClassNameW(h,c,128);if(c.ToString()=="DesktopTopBarWindow"){f=h;return false;}return true;},IntPtr.Zero);
   if(f==IntPtr.Zero) return "MISSING";
   RECT r;GetWindowRect(f,out r);
   return (IsWindowVisible(f)?"visible":"hidden")+" "+R(r);
 }
}
'@

$TOPREG='HKCU:\Software\DesktopSuite\TopBar'
function Set-KeepTop([int]$v){ if(-not(Test-Path $TOPREG)){New-Item -Path $TOPREG -Force|Out-Null}; New-ItemProperty -Path $TOPREG -Name 'KeepTop' -Value $v -PropertyType DWord -Force | Out-Null }
function Notify([int]$v){ $d=[FF]::Find('DesktopDockWindow'); $b=[FF]::Find('DesktopTopBarWindow'); if($d -ne [IntPtr]::Zero){[void][FF]::PostMessageW($d,0x8000+18,[IntPtr]$v,[IntPtr]::Zero)}; if($b -ne [IntPtr]::Zero){[void][FF]::PostMessageW($b,0x8000+18,[IntPtr]$v,[IntPtr]::Zero)}; Start-Sleep -Milliseconds 900 }

$was = (Get-ItemProperty -Path $TOPREG -Name 'KeepTop' -ErrorAction SilentlyContinue).KeepTop
if ($null -eq $was) { $was = 0 }
$screen = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
"screen = $($screen.Width)x$($screen.Height)"

"--- keep-top ON ---"
Set-KeepTop 1; Notify 1
"topbar before: " + [FF]::BarState()

"--- showing a borderless fullscreen window ---"
$form = New-Object System.Windows.Forms.Form
$form.FormBorderStyle = [System.Windows.Forms.FormBorderStyle]::None
$form.StartPosition = [System.Windows.Forms.FormStartPosition]::Manual
$form.Bounds = $screen
$form.TopMost = $true
$form.BackColor = [System.Drawing.Color]::DarkSlateBlue
$form.Text = 'KEEPTOP-FS-PROBE'
$form.Show(); $form.Activate()
Start-Sleep -Milliseconds 5000
$during = [FF]::BarState()
"topbar during fullscreen: $during"
if ($during -like 'hidden*' -or $during -eq 'MISSING') { "PASS  top bar yields to the fullscreen app" }
else { "FAIL  top bar still shown during fullscreen ($during)" }

"--- closing it ---"
$form.Close(); $form.Dispose()
Start-Sleep -Milliseconds 4000
$after = [FF]::BarState()
"topbar after: $after"
if ($after -like 'visible*') { "PASS  top bar restored after fullscreen" } else { "FAIL  top bar not restored ($after)" }

"--- restore keep-top to $was ---"
Set-KeepTop $was
Notify $(if ($was -eq 1) { 1 } else { 2 })
"final topbar: " + [FF]::BarState() + "  work area = " + [FF]::R([FF]::Wa())
