// keep_top.h —— 「保持顶栏」模式的共享定义与几何计算
//
// 模式语义（用户在托盘图标右键菜单勾选「保持顶栏」）：
//   1. 顶栏恒定贴在屏幕最上方，不被任何普通窗口遮挡（全屏应用除外）；
//   2. 最大化窗口只填充「顶栏以下」的区域 —— 顶栏高度这一段被工作区让出；
//   3. Dock 自动收起关闭（Dock 常驻可见）时，还要让出 Dock 所在的那条
//      水平高度带：最大化窗口的底边停在 Dock 顶边之上；
//      Dock 自动收起开启时不做这条预留（Dock 平时收在屏幕外，最大化窗口
//      直接填满到屏幕底）。
//
// 为什么由 Dock 统一改工作区（而不是顶栏注册 AppBar）：
//   本套件常驻期间会隐藏系统任务栏并自行改写工作区（见 dock_main.cpp
//   EnsureTaskbarHidden），explorer 的 AppBar 协商与这套自管逻辑会互相覆盖；
//   因此工作区一律由 Dock 单点计算与写入，顶栏只负责自己的层级与全屏让位。
//
// 已知系统行为（本机实测，1920×1080 / Win11）：
//   - SPI_SETWORKAREA 本身不会重排「已经最大化」的窗口（几何纹丝不动），
//     必须由调用方自行把握：先 SW_RESTORE，再 SetWindowPos 到新工作区，
//     最后 SW_MAXIMIZE 让系统按新工作区重新最大化（实测有效）；
//   - 直接对最大化窗口 SetWindowPos 会被系统按最大化几何弹回（无效）。
//
// 开关持久化：HKCU\Software\DesktopSuite\TopBar\KeepTop（DWORD，1=开启）。
// 顶栏与 Dock 都读同一份注册表值；运行期以消息互相同步（见 dock_main.cpp
// kMsgTopBarDockState / mywigets_main.cpp kMsgDockKeepTopSet）。

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#include <windows.h>

#include <algorithm>

namespace keeptop {

// 开关注册表位置
constexpr wchar_t kRegPath[] = L"Software\\DesktopSuite\\TopBar";
constexpr wchar_t kRegValue[] = L"KeepTop";

// 顶栏窗口规格：96 DPI 基准高度（与 topbar_main.cpp 的 kBaseTabHeight 一致）
constexpr int kBarHeightBase = 40;
inline int DefaultBarHeightForDpi(int dpi) {
    return MulDiv(kBarHeightBase, dpi > 0 ? dpi : 96, 96);
}

// 「保持顶栏」模式下，最大化窗口底边与 Dock 毛玻璃顶边之间的可见间距
// （逻辑像素，随 DPI 缩放）。实测的窗口矩形比毛玻璃本体大一圈
// （阴影预留 + 顶部留白，见 dock_main.cpp 的 kShadowMargin/kPadTop），
// 所以让出的高度必须按「毛玻璃顶边」而不是「Dock 窗口顶边」来算 ——
// 否则最大化窗口与看得见的 Dock 之间会空出 15px 以上。
constexpr int kMaxWindowToDockGapBase = 2;

// 读开关（缺省/读取失败 = 关闭）
inline bool IsEnabledFromRegistry() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegPath, 0, KEY_READ, &key) !=
        ERROR_SUCCESS) {
        return false;
    }
    DWORD type = 0;
    DWORD value = 0;
    DWORD bytes = sizeof(value);
    const LONG r = RegQueryValueExW(key, kRegValue, nullptr, &type,
                                    reinterpret_cast<BYTE*>(&value), &bytes);
    RegCloseKey(key);
    return r == ERROR_SUCCESS && type == REG_DWORD && value != 0;
}

// 写开关（返回是否写入成功）
inline bool SetEnabledInRegistry(bool enabled) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegPath, 0, nullptr, 0, KEY_WRITE,
                        nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return false;
    }
    const DWORD value = enabled ? 1u : 0u;
    const LONG r = RegSetValueExW(key, kRegValue, 0, REG_DWORD,
                                  reinterpret_cast<const BYTE*>(&value),
                                  sizeof(value));
    RegCloseKey(key);
    return r == ERROR_SUCCESS;
}

// Dock 的「自动收起」开关（决定本模式是否预留 Dock 所在高度带）：
//   1/缺省 = 自动收起开启 → Dock 平时收在屏幕外 → 不预留（最大化窗口填满到屏幕底）
//   0     = 自动收起关闭 → Dock 常驻可见 → 预留 Dock 高度带
// Dock 自己的配置键（HKCU\Software\DesktopSuite\Dock，与 SaveConfig 同键）
inline bool IsAutoCollapseFromRegistry() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\DesktopSuite\\Dock", 0,
                      KEY_READ, &key) != ERROR_SUCCESS) {
        return true;  // 无配置 = Dock 默认（自动收起开启）
    }
    DWORD type = 0;
    DWORD value = 1;
    DWORD bytes = sizeof(value);
    const LONG r = RegQueryValueExW(key, L"AutoCollapse", nullptr, &type,
                                    reinterpret_cast<BYTE*>(&value), &bytes);
    RegCloseKey(key);
    if (r != ERROR_SUCCESS || type != REG_DWORD) return true;
    return value != 0;
}

// 单显示器预留区（物理像素）。
//   full         = 该显示器整幅画面
//   barHeight    = 顶栏高度（>0 时在顶部预留这一段）
//   dockReserve  = 0 表示不预留下缘；>0 表示让出最下面这一段（Dock 高度带）
// 返回工作区：顶部让出 barHeight、底部让出 dockReserve。
// 预留过多（剩余高度连一个最小窗口都放不下）时放弃预留，退回整幅画面，
// 避免把工作区算成空矩形导致所有窗口被挤到 0 高度。
// 下限 120px：足以容纳「顶栏 40 + 紧凑 Dock 带」的组合，同时仍能挡住
// 明显的异常值（例如误把整屏算成预留）。
inline RECT ReserveWorkArea(const RECT& full, int barHeight, int dockReserve) {
    RECT wa = full;
    const int fullH = full.bottom - full.top;
    const int top = (barHeight > 0) ? barHeight : 0;
    const int bottom = (dockReserve > 0) ? dockReserve : 0;
    if (top + bottom > 0 && fullH - top - bottom >= 120) {
        wa.top = full.top + top;
        wa.bottom = full.bottom - bottom;
    }
    return wa;
}

inline bool SameRect(const RECT& a, const RECT& b) {
    return a.left == b.left && a.top == b.top && a.right == b.right &&
           a.bottom == b.bottom;
}

}  // namespace keeptop
