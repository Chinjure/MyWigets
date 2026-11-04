Add-Type @'
using System;
using System.Runtime.InteropServices;
public class MC {
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint flags, uint dx, uint dy, uint data, UIntPtr extra);
}
'@
[MC]::SetProcessDPIAware() | Out-Null
# middle click at a dock icon area (non-running pinned item, safe)
[MC]::SetCursorPos(890, 1545) | Out-Null
Start-Sleep -Milliseconds 200
[MC]::mouse_event(0x0020, 0, 0, 0, [UIntPtr]::Zero) | Out-Null  # MIDDLEDOWN
Start-Sleep -Milliseconds 50
[MC]::mouse_event(0x0040, 0, 0, 0, [UIntPtr]::Zero) | Out-Null  # MIDDLEUP
Start-Sleep -Milliseconds 200
