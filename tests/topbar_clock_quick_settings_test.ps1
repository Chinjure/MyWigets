# tests/topbar_clock_quick_settings_test.ps1
#
# 回归验证：顶栏时钟的点击语义
#   1) 左键点击时钟  → 打开系统「快速设置」面板
#      （系统快捷键 Win+A，前台窗口变为 ControlCenterWindow）
#   2) 再次左键点击  → 收起该面板（前台回到点击前的应用窗口）
#   3) 右键点击时钟  → 打开系统「通知中心/日历」浮出窗口
#      （系统快捷键 Win+N，前台窗口变为 Windows.UI.Core.CoreWindow），
#      右键不得弹顶栏空白处的退出菜单。
#
# 判定手段：真鼠标点击（SetCursorPos + mouse_event）打到顶栏时钟区域中心，
# 「面板是否打开」以系统前台窗口类名为准 —— 这是面板真实被 shell 拉起的
# 直接证据，不依赖顶栏自身状态。
#
# 用例不依赖窗口标题/DPI：时钟几何由顶栏窗口矩形 + 代码中同一组常量
# （按钮宽 46、间隔 4、时钟宽 112，96 DPI 基准）按 GetDpiForWindow 缩放算出。
#
# 退出码：0 = 全部 PASS，1 = 有 FAIL。
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms,System.Drawing

Add-Type -TypeDefinition @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public class QST {
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint flags, uint dx, uint dy, uint data, UIntPtr extra);
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("gdi32.dll")] public static extern IntPtr CreateRectRgn(int a, int b, int c, int d);
  [DllImport("gdi32.dll")] public static extern int GetRgnBox(IntPtr rgn, out RECT r);
  [DllImport("gdi32.dll")] public static extern bool DeleteObject(IntPtr o);
  [DllImport("user32.dll")] public static extern int GetWindowRgn(IntPtr h, IntPtr rgn);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  public struct RECT { public int L, T, R, B; }
  public const uint LEFTDOWN = 0x0002, LEFTUP = 0x0004, RIGHTDOWN = 0x0008, RIGHTUP = 0x0010;
  public const uint KEYUP = 0x0002, EXT = 0x0001;

  public static bool DpiAware() { return SetProcessDPIAware(); }
  public static string ClassOf(IntPtr h) {
    var c = new StringBuilder(256); GetClassNameW(h, c, 256); return c.ToString();
  }
  public static string Fg() { return ClassOf(GetForegroundWindow()); }

  // 前台窗口（应为面板宿主）的窗口区域上缘（宿主相对坐标）
  public static int FgRegionTop() {
    IntPtr h = GetForegroundWindow();
    IntPtr rgn = CreateRectRgn(0, 0, 0, 0);
    if (rgn == IntPtr.Zero) return int.MinValue;
    int kind = GetWindowRgn(h, rgn);
    RECT rb;
    bool ok = GetRgnBox(rgn, out rb) != 0;
    DeleteObject(rgn);
    if (kind == 0 || kind == 1 || !ok) return int.MinValue;
    return rb.T;
  }

  // 前台窗口（面板宿主）的窗口区域框（宿主相对坐标）→ {left, top, right, bottom}
  public static int[] FgRegionBox() {
    IntPtr h = GetForegroundWindow();
    IntPtr rgn = CreateRectRgn(0, 0, 0, 0);
    if (rgn == IntPtr.Zero) return new int[] { 0, 0, 0, 0 };
    int kind = GetWindowRgn(h, rgn);
    RECT rb;
    bool ok = GetRgnBox(rgn, out rb) != 0;
    DeleteObject(rgn);
    if (kind == 0 || kind == 1 || !ok) return new int[] { 0, 0, 0, 0 };
    return new int[] { rb.L, rb.T, rb.R, rb.B };
  }

  // 工作区上缘（= 面板默认锚点的宿主顶）
  [DllImport("user32.dll")] public static extern bool SystemParametersInfoW(uint act, uint p, out RECT r, uint win);
  public static int WorkTop() {
    RECT wa;
    if (!SystemParametersInfoW(0x0030 /*SPI_GETWORKAREA*/, 0, out wa, 0)) return 0;
    return wa.T;
  }

  // 快速设置面板宿主（前台窗口）的"对齐偏差"：
  //   宿主窗口顶 + GetWindowRgn 区域顶 - 顶栏下缘
  // = 0 表示面板上缘正好贴在顶栏下缘（见 src/topbar_main.cpp
  //   AlignQuickSettingsToBar 的同一算法）。取不到时返回 int.MinValue。
  public static int AlignDelta(int barBottom) {
    IntPtr h = GetForegroundWindow();
    if (ClassOf(h) != "ControlCenterWindow") return int.MinValue;
    RECT wr;
    if (!GetWindowRect(h, out wr)) return int.MinValue;
    IntPtr rgn = CreateRectRgn(0, 0, 0, 0);
    if (rgn == IntPtr.Zero) return int.MinValue;
    int kind = GetWindowRgn(h, rgn);
    RECT rb;
    bool ok = GetRgnBox(rgn, out rb) != 0;
    DeleteObject(rgn);
    if (kind == 0 || kind == 1 || !ok) return int.MinValue;
    return wr.T + rb.T - barBottom;
  }

  // 注意：本环境里 FindWindowW 找不到 MyWigets 的窗口（Shell_TrayWnd 可以），
  // 因此统一用 EnumWindows + 类名匹配（仓库既有探针同样如此）。
  static IntPtr barFound = IntPtr.Zero;
  static bool MatchBar(IntPtr h, IntPtr l) {
    if (ClassOf(h) == "DesktopTopBarWindow") { barFound = h; return false; }
    return true;
  }
  public static IntPtr FindTopBar() {
    barFound = IntPtr.Zero;
    EnumWindows(new EnumProc(MatchBar), IntPtr.Zero);
    return barFound;
  }

  public static void Click(int x, int y, bool right) {
    SetCursorPos(x, y);
    System.Threading.Thread.Sleep(120);
    if (right) {
      mouse_event(RIGHTDOWN, 0, 0, 0, UIntPtr.Zero);
      mouse_event(RIGHTUP, 0, 0, 0, UIntPtr.Zero);
    } else {
      mouse_event(LEFTDOWN, 0, 0, 0, UIntPtr.Zero);
      mouse_event(LEFTUP, 0, 0, 0, UIntPtr.Zero);
    }
  }
  public static void Hover(int x, int y) {
    SetCursorPos(x, y);
    System.Threading.Thread.Sleep(250);
  }
  public static bool FgIsQS() { return ClassOf(GetForegroundWindow()) == "ControlCenterWindow"; }
  // 前台窗口矩形 → {left, top, width, height}
  public static int[] FgRect() {
    RECT r; GetWindowRect(GetForegroundWindow(), out r);
    return new int[] { r.L, r.T, r.R - r.L, r.B - r.T };
  }
  public static void Esc() {
    keybd_event(0x1B, 0, 0, UIntPtr.Zero);
    keybd_event(0x1B, 0, KEYUP, UIntPtr.Zero);
  }
}
'@ -Language CSharp

# 像素级校验用：把"面板关闭"底图与"面板打开"截图相减，找可见卡片的上缘行。
# 这是端到端判据 —— 直接量"顶栏下缘到卡片上缘"的缝隙像素数，不依赖实现常量。
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
public class Pix {
  static byte[] Load(string path, out int w, out int h, out int stride) {
    var bmp = new Bitmap(path);
    w = bmp.Width; h = bmp.Height;
    var r = new Rectangle(0, 0, w, h);
    var d = bmp.LockBits(r, ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
    stride = d.Stride;
    byte[] b = new byte[stride * h];
    Marshal.Copy(d.Scan0, b, 0, b.Length);
    bmp.UnlockBits(d); bmp.Dispose();
    return b;
  }
  // 在 x0..x1 列带、从 yStart 行起，返回第一行"连续变化像素 >= minRun"的 y，
  // 即卡片可见上缘；找不到返回 -1。
  public static int FirstCardRow(string closedPng, string openPng, int x0, int x1,
                                 int yStart, int minRun, int thresh) {
    int w, h, sa, w2, h2, sb;
    byte[] a = Load(closedPng, out w, out h, out sa);
    byte[] b = Load(openPng, out w2, out h2, out sb);
    if (w != w2 || h != h2 || x1 > w || x0 < 0) return -1;
    for (int y = yStart; y < h; y++) {
      int run = 0, best = 0, rowa = y * sa, rowb = y * sb;
      for (int x = x0; x < x1; x++) {
        int i = x * 4;
        int d = Math.Abs(a[rowa+i]-b[rowb+i]) + Math.Abs(a[rowa+i+1]-b[rowb+i+1]) + Math.Abs(a[rowa+i+2]-b[rowb+i+2]);
        if (d > thresh) { if (++run > best) best = run; } else run = 0;
      }
      if (best >= minRun) return y;
    }
    return -1;
  }

  // ---- "打开瞬间不能在默认位置露脸"用：高频抓默认卡片区做差 ----
  // 只测窗口位置不够 —— shell 可能在窗口刚 show、内容还没绘制时就被我们
  // 挪走，位置样本上"到过默认位"但屏幕上什么都没有。这里直接抓那一块
  // 屏幕像素：只要卡片内容从未在那里渲染出来，用户就看不见闪跳。
  [DllImport("user32.dll")] static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] static extern void mouse_event(uint f, uint dx, uint dy, uint d, UIntPtr e);

  // 抓屏幕一小块（System.Drawing 的 CopyFromScreen，能抓到合成后的面板）
  public static int[] Grab(int x, int y, int w, int h) {
    using (var bmp = new Bitmap(w, h, PixelFormat.Format32bppArgb)) {
      using (var g = Graphics.FromImage(bmp)) {
        g.CopyFromScreen(x, y, 0, 0, new Size(w, h), CopyPixelOperation.SourceCopy);
      }
      var rect = new Rectangle(0, 0, w, h);
      var d = bmp.LockBits(rect, ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
      int[] px = new int[w * h];
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
  // 点时钟并高频盯住"默认卡片区"，返回 {最大差异占比, 出现时刻ms, 采样帧数}
  public static double[] WatchDefaultCard(int cx, int cy, int rx, int ry, int rw, int rh,
                                          int[] basePx, int ms) {
    SetCursorPos(cx, cy);
    System.Threading.Thread.Sleep(120);
    mouse_event(0x0002, 0, 0, 0, UIntPtr.Zero);
    mouse_event(0x0004, 0, 0, 0, UIntPtr.Zero);
    var sw = System.Diagnostics.Stopwatch.StartNew();
    double maxR = 0; int maxAt = -1; int frames = 0;
    while (sw.ElapsedMilliseconds < ms) {
      double r = DiffRatio(basePx, Grab(rx, ry, rw, rh));
      frames++;
      if (r > maxR) { maxR = r; maxAt = (int)sw.ElapsedMilliseconds; }
    }
    return new double[] { maxR, maxAt, frames };
  }
}
'@

[void][QST]::DpiAware()

$bar = [QST]::FindTopBar()
if ($bar -eq [IntPtr]::Zero) { Write-Host "FAIL: 顶栏窗口 DesktopTopBarWindow 不存在（MyWigets 未运行？）"; exit 1 }

$rc = New-Object QST+RECT
[void][QST]::GetWindowRect($bar, [ref]$rc)
$barW = $rc.R - $rc.L
$barH = $rc.B - $rc.T
$dpi = [QST]::GetDpiForWindow($bar)
if ($dpi -le 0) { $dpi = 96 }
$k = $dpi / 96.0
Write-Host ("顶栏 rect = {0},{1} {2}x{3} dpi={4} scale={5}" -f $rc.L, $rc.T, $barW, $barH, $dpi, $k)

# 与 src/topbar_main.cpp 同一组布局常量（96 DPI 基准）
$bw = 46.0 * $k      # kButtonWidthBase
$gap = 4.0 * $k      # kButtonGap
$clockW = 112.0 * $k # kClockWidthBase
$minX = $barW - 3 * $bw - 2 * $gap
$clockX = $minX - $gap - $clockW
$clockY = 0.0
$clockH = 40.0 * $k  # kBaseTabHeight
$cx = $rc.L + [int]($clockX + $clockW / 2)
$cy = $rc.T + [int]($clockY + $clockH / 2)
Write-Host ("时钟区域客户区 x={0:N1}..{1:N1}，点击屏幕点 = ({2},{3})" -f $clockX, ($clockX + $clockW), $cx, $cy)

$shots = "C:\Users\may\Desktop\clock\logs"
New-Item -ItemType Directory -Force -Path $shots | Out-Null
$scr = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
function Shot([string]$name) {
  $bmp = New-Object System.Drawing.Bitmap $scr.Width, $scr.Height
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.CopyFromScreen($scr.X, $scr.Y, 0, 0, $bmp.Size)
  $bmp.Save((Join-Path $shots $name))
  $g.Dispose(); $bmp.Dispose()
}

$fail = 0
function Check([string]$name, [bool]$ok, [string]$detail) {
  if ($ok) { Write-Host ("PASS: {0} ({1})" -f $name, $detail) -ForegroundColor Green }
  else { Write-Host ("FAIL: {0} ({1})" -f $name, $detail) -ForegroundColor Red; $script:fail++ }
}

$fgBefore = [QST]::Fg()
Write-Host ("点击前前台 = {0}" -f $fgBefore)

# 先把光标停到时钟上（悬停高亮状态在底图与打开图中保持一致），再抓
# "面板关闭"底图：后面用它与"面板打开"截图相减，量出真实的缝隙像素
[QST]::Hover($cx, $cy)
Shot 'clock_baseline.png'
$baselinePng = Join-Path $shots 'clock_baseline.png'

# ---- 1) 左键：打开系统快速设置面板 ----
[QST]::Click($cx, $cy, $false)
$openFg = ""
$deadline = (Get-Date).AddSeconds(4)
while ((Get-Date) -lt $deadline) {
  $openFg = [QST]::Fg()
  if ($openFg -eq "ControlCenterWindow") { break }
  Start-Sleep -Milliseconds 150
}
Shot 'clock_quick_settings_open.png'
Check "左键时钟打开系统快速设置面板" ($openFg -eq "ControlCenterWindow") ("前台=$openFg")

# ---- 1b) 面板上缘必须紧贴顶栏下缘（区域算法）----
# 对齐由顶栏的对齐定时器完成，给它一点时间收敛。
# 期望值 = -卡片投影留白（8 逻辑像素，区域上缘在可见卡片上缘之上这么多）
$inset = [int][Math]::Round(8 * $k)
$delta = [int]::MinValue
$deadline = (Get-Date).AddSeconds(3)
while ((Get-Date) -lt $deadline) {
  $delta = [QST]::AlignDelta($rc.B)
  if ($delta -ne [int]::MinValue -and [Math]::Abs($delta + $inset) -le 2) { break }
  Start-Sleep -Milliseconds 150
}
Shot 'clock_quick_settings_aligned.png'
Check "面板区域按留白补偿对齐" ($delta -ne [int]::MinValue -and [Math]::Abs($delta + $inset) -le 2) `
  ("顶栏下缘={0} 期望偏差={1}px 实际={2}px" -f $rc.B, (-$inset), $delta)

# 面板已就位：趁它开着记下宿主矩形、窗口区域框与工作区顶 ——
# 2b 要用它们算出"面板默认锚点"那一块屏幕区域
$hostRc = [QST]::FgRect()
$rbox = [QST]::FgRegionBox()
$workTop = [QST]::WorkTop()
Write-Host ("面板宿主 rect=({0},{1}) {2}x{3}  区域框={4}  工作区顶={5}" -f $hostRc[0], $hostRc[1], $hostRc[2], $hostRc[3], ($rbox -join ','), $workTop)

# ---- 1c) 可见缝隙 = 0（像素级端到端）----
# 在面板宿主的横向条带内，从顶栏下缘往下找第一行"卡片边缘"（连续变化像素
# 足够长）。它应当就落在顶栏下缘那一行 —— 差 0 像素才算紧贴。
$alignedPng = Join-Path $shots 'clock_quick_settings_aligned.png'
$hr = [QST]::FgRect()
$bandX0 = $hr[0] + [int]($hr[2] * 0.10)
$bandX1 = $hr[0] + [int]($hr[2] * 0.70)
$minRun = [int](($bandX1 - $bandX0) * 0.5)
$cardTop = [Pix]::FirstCardRow($baselinePng, $alignedPng, $bandX0, $bandX1, $rc.B, $minRun, 20)
$gap = if ($cardTop -lt 0) { "未测到卡片" } else { $cardTop - $rc.B }
Check "可见卡片上缘与顶栏下缘 0 缝隙" ($cardTop -ge $rc.B -and ($cardTop - $rc.B) -le 2) `
  ("顶栏下缘={0} 卡片上缘={1} 缝隙={2}px" -f $rc.B, $cardTop, $gap)

# ---- 2) 再次左键：收起面板，前台回到点击前的应用 ----
[QST]::Click($cx, $cy, $false)
$closeFg = $openFg
$deadline = (Get-Date).AddSeconds(4)
while ((Get-Date) -lt $deadline) {
  $closeFg = [QST]::Fg()
  if ($closeFg -ne "ControlCenterWindow") { break }
  Start-Sleep -Milliseconds 150
}
Shot 'clock_quick_settings_closed.png'
Check "再次左键收起面板" ($closeFg -ne "ControlCenterWindow") ("前台=$closeFg")
Check "收起后前台回到原应用" ($closeFg -eq $fgBefore) ("期望=$fgBefore 实际=$closeFg")

# ---- 2b) 打开瞬间面板不能先在右下角露脸（用户报的"先下方闪一下再跳上来"）----
# 只测窗口位置不够：shell 可能已经 show、内容还没绘制时就被我们挪走，
# 位置采样上"到过默认位"但屏幕上什么都没有。这里直接高频抓**默认卡片区**
# 的屏幕像素：只要卡片内容从未在那里渲染出来，用户就看不见闪跳。
# 默认卡片区 = 宿主处于默认锚点（工作区顶）时的窗口区域框
Start-Sleep -Milliseconds 500
$dfltX = $hostRc[0] + $rbox[0]
$dfltY = $workTop + $rbox[1]
$dfltW = $rbox[2] - $rbox[0]
$dfltH = $rbox[3] - $rbox[1]
$dfltBase = [Pix]::Grab($dfltX, $dfltY, $dfltW, $dfltH)
$watch = [Pix]::WatchDefaultCard($cx, $cy, $dfltX, $dfltY, $dfltW, $dfltH, $dfltBase, 800)
Check "打开瞬间卡片内容未在默认位置出现（无闪跳）" ($watch[0] -lt 0.25) `
  ("默认区最大差异占比={0:P1} 采样帧数={1} 默认区=({2},{3}) {4}x{5}" -f $watch[0], $watch[2], $dfltX, $dfltY, $dfltW, $dfltH)
# 收起（供后续右键用例）
[QST]::Click($cx, $cy, $false)
$deadline = (Get-Date).AddSeconds(4)
while ((Get-Date) -lt $deadline) {
  if ([QST]::Fg() -ne "ControlCenterWindow") { break }
  Start-Sleep -Milliseconds 150
}

# ---- 3) 右键：打开系统通知中心/日历 ----
Start-Sleep -Milliseconds 300
[QST]::Click($cx, $cy, $true)
$calFg = ""
$deadline = (Get-Date).AddSeconds(4)
while ((Get-Date) -lt $deadline) {
  $calFg = [QST]::Fg()
  if ($calFg -eq "Windows.UI.Core.CoreWindow") { break }
  Start-Sleep -Milliseconds 150
}
Shot 'clock_calendar_open.png'
Check "右键时钟打开系统通知中心/日历" ($calFg -eq "Windows.UI.Core.CoreWindow") ("前台=$calFg")
[QST]::Esc()
Start-Sleep -Milliseconds 400

if ($fail -eq 0) { Write-Host "`n全部 PASS" -ForegroundColor Green; exit 0 }
Write-Host ("`n{0} 项 FAIL" -f $fail) -ForegroundColor Red
exit 1
