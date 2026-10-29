// DesktopTopBar - 类 macOS 桌面顶栏（独立程序）
//
// 功能：
//   1. 固定在桌面顶部的一条半透明顶栏，外观与其他组件一致（GDI+ 逐像素透明）
//   2. 层级：默认只不遮挡"当前聚焦窗口"（顶栏自己的聚焦目标，见 UpdateTarget）——
//      目标窗口与顶栏矩形重叠时让位到它之下（被它盖住，仍高于其它非聚焦窗口）；
//      不重叠（含无目标/目标最小化/目标在别的虚拟桌面）时置顶，
//      覆盖除目标外的所有窗口。不再挂 Progman 属主贴桌面层：
//      顶栏是独立顶层窗口，WS_EX_TOOLWINDOW 保证不进任务栏/Alt-Tab
//   3. 从音量键右侧到右侧三键之间显示 Chrome 风格标签：
//      - 普通应用：标签为聚焦窗口所属应用打开的全部窗口，名字为对应窗口名
//      - Chrome/Edge（安装了 chrome-tab-sync 扩展并连接后）：标签为浏览器内
//        当前聚焦窗口的真实标签页（标题同步、点击切换、中键关闭、
//        右键新建标签页）。未连接扩展时回退为窗口枚举。
//      - 收录与聚焦只认"任务栏中看得见的窗口"（判据见 IsTaskbarWindow）：
//        隐藏窗、工具窗、属主浮层、其它虚拟桌面上的 cloaked 窗都不算。
//        子进程的隐藏消息窗与托盘图标应用的驻留窗因此既不会成为标签，
//        也不可能被聚焦（聚焦它们等于把幽灵窗口"打开"），更不会被顶栏
//        收作"当前聚焦目标" —— 否则层级会跟着它重估，被最大化窗口遮住的
//        顶栏会凭空弹出来（如 hotspot 的 WS_EX_TOOLWINDOW 启动器弹出窗）
//      - 按住标签左右拖动即可随意重排（超过 4px 阈值才算拖动）：
//        拖动中被拖标签浮在光标处、其余标签实时补位（原位置留空即落点提示），
//        前进只要拖动距离占相邻标签宽度的 20%（kTabDragSwapRatio）就换位；
//        往回（朝原来的位置）一动（超过防抖量）就让刚被挤开的标签回原位
//        （此时被拖标签仍覆盖着它 80% 宽度）。松手即落定这个顺序。
//        **顶栏标签顺序与浏览器里的真实标签顺序解绑**：拖动只改顶栏自己的
//        展示顺序（按浏览器窗口分别保留），不下发任何命令、不动浏览器的标签；
//        浏览器那侧的标签移动（tabMoved）也不会回过来重排顶栏。
//        固定标签恒在最前：普通标签拖不进固定区、固定标签也拖不出。
//   4. 顶栏右侧提供 Chrome 浏览器风格的 最小化 / 最大化 / 关闭 三个按钮，
//      用于控制当前聚焦窗口；双击顶栏空白处让当前聚焦窗口填充"顶栏以外"
//      的区域（上缘压在顶栏下缘，顶栏保持可见；再次双击还原到填充前的
//      位置），与最大化键的系统最大化（会连顶栏一起盖住）不同；
//      按住顶栏空白处拖动可移动当前聚焦窗口（等同标题栏拖动）
//   5. 最小化键左侧提供 Win11 风格任务栏时钟：时间/日期两行显示
//      （跟随系统区域格式与"显示秒"设置），悬停有药丸高亮，
//      左键打开/收起系统「快速设置」面板 —— 点击 Win11 任务栏右侧
//      网络/音量/电池三合一按钮弹出的同一个系统面板（系统快捷键 Win+A）；
//      面板由 shell 绘制、内容也全归系统，但顶栏会把它的宿主窗口挪到
//      顶栏正下方，使面板上缘紧贴顶栏下缘（见 AlignQuickSettingsToBar）；
//      右键打开/收起系统「日期和时间」日历浮出窗口（原来的左键行为）
//   6. 高度等于 Chrome 浏览器标签栏的高度（约 40px，随 DPI 缩放）
//   7. 与 Dock 自动收起联动：Dock 开启自动收起时，顶栏层级跟随 Dock ——
//      Dock 展开：顶栏与 Dock 一样提升为置顶层，覆盖所有窗口
//      （含当前聚焦窗口，此时不让位）；
//      Dock 收起：回到默认策略（只不遮挡当前聚焦窗口）。
//   8. 「保持顶栏」模式（托盘图标右键菜单勾选，注册表持久化，见 keep_top.h）：
//      顶栏恒贴在屏幕最上方、恒为置顶层，不再为当前聚焦窗口让位；
//      工作区由 Dock 统一让出顶栏高度（最大化窗口只填充顶栏以下），
//      因此本模式下顶栏与最大化窗口天然不重叠；
//      全屏应用（游戏全屏 / 全屏视频 / 演示模式，判定见 fullscreen_guard.h）
//      占用本屏时顶栏整体隐藏，退出全屏自动恢复。

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

// WinSock2 必须先于 windows.h 引入；本文件用 WIN32_LEAN_AND_MEAN 排除
// windows.h 内的旧 winsock.h，因此这里（windows.h 之后）引入是安全的
#include <winsock2.h>
#include <ws2tcpip.h>

#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <dwmapi.h>  // 扩展边框：填充窗口时补偿不可见边缘（否则四周漏缝）
#include <objidl.h>  // GDI+ 需要 IStream 等 COM 类型，先于 gdiplus.h 包含
#include <gdiplus.h>
#include <dwmapi.h>  // DWMWA_CLOAKED：判断目标窗口是否被系统隐藏（虚拟桌面/最小化 UWP）

// Core Audio：按进程设置应用音量（类似 Windows 音量合成器）
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <tlhelp32.h>   // 进程快照：解析应用子进程的音频会话
#include <unordered_map>
#include <vector>

// Chrome 标签同步：纯逻辑层（JSON / SHA-1 / Base64 / WS 帧 / 同步模型）
#include "fullscreen_guard.h"  // 保持顶栏：全屏应用让位判定（与 Dock 共用口径）
#include "keep_top.h"          // 「保持顶栏」模式（注册表开关，与 Dock 共用）
#include "topbar_ws_proto.h"
#include "widgets.h"

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "Mmdevapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "dwmapi.lib")  // DwmGetWindowAttribute：可见边框换算
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "dwmapi.lib")  // 目标窗口 cloaked（被系统隐藏）判定

using namespace Gdiplus;
using Microsoft::WRL::ComPtr;

// SDK 头文件未定义的窗口状态事件（值来自 WinUser.h 文档，稳定）
#ifndef EVENT_SYSTEM_RESTORE
#define EVENT_SYSTEM_RESTORE 0x000A
#endif
#ifndef EVENT_SYSTEM_MAXIMIZESTART
#define EVENT_SYSTEM_MAXIMIZESTART 0x0018
#endif
#ifndef EVENT_SYSTEM_MAXIMIZEEND
#define EVENT_SYSTEM_MAXIMIZEEND 0x0019
#endif

namespace {

constexpr wchar_t kWindowClass[] = L"DesktopTopBarWindow";

// Chrome 浏览器标签栏的高度（96 DPI 下约 40 像素），随 DPI 缩放
constexpr int kBaseTabHeight = 40;
// 每个窗口控制按钮的宽度（Chrome 风格，约等于标签栏高度）
constexpr int kButtonWidthBase = 46;
constexpr int kButtonGap = 4;

// Win11 风格任务栏时钟区域（时间/日期两行），位于最小化按钮左侧
constexpr int kClockWidthBase = 112;   // 时钟区域宽度（96 DPI 基准，随 DPI 缩放）
constexpr UINT_PTR kClockTimerId = 5;  // 时钟刷新定时器：每秒检查文本变化
constexpr UINT kClockTickMs = 1000;    // 与任务栏时钟一致；文本未变化不重绘

// Chrome 风格标签的视觉留白（相对 96 DPI，绘制时乘 scale）
constexpr float kTabTopInset = 5.0f;        // 活动标签顶部留白，底部贴住标签栏
constexpr float kTabDividerInset = 8.0f;    // 未激活标签之间竖线的上下留白

// 统一标签宽度（96 DPI 基准，随 DPI 缩放）：空间充裕时所有标签等宽，
// 不随标题长度变化；标签多到放不下时沿用自动缩小逻辑
constexpr float kUniformTabWidth = 240.0f;   // 1.5 × 160 基准宽度

// 拖动标签重排：按住标签横向拖动的启动阈值（96 DPI 基准，随 DPI 缩放）。
// 阈值内松手依旧只是"点击切换标签"，避免手抖把标签拖乱。
constexpr int kTabDragThreshold = 4;
// 换位阈值：被拖标签与相邻标签的重叠达到**相邻标签宽度的这个比例**就换位。
// 0.2 = 拖动距离占相邻标签 20% 的宽度即交换位置（旧行为要拖满整格才换）。
constexpr float kTabDragSwapRatio = 0.2f;
// 回退阈值：**朝原来的位置**移动时，被拖标签覆盖到刚被挤开的那个标签宽度的
// 这个比例（默认 80% —— 换位瞬间正好就是这个姿态），被挤开的标签就回原位：
// 方向一反就回，不需要再往回拖 80% 的距离。注意这是"覆盖比例"，不是拖动距离。
constexpr float kTabDragReturnRatio = 0.8f;

// 左上角音量按钮与展开面板
constexpr int kVolumeButtonW = 46;      // 音量按钮宽度（与窗口控制按钮一致，便于点击）
constexpr int kVolumePanelW = 260;      // 展开的音量面板宽度
constexpr int kVolumePanelH = 60;       // 展开的音量面板高度
constexpr int kVolumePanelMargin = 2;   // 按键/面板贴合左上角的小边距
constexpr int kMuteButtonW = 30;        // 面板内静音按钮宽度
constexpr UINT kVolumeApplyDelayMs = 40;
// 目标进程尚无音频会话时的补解析轮询：应用可能先无声（如浏览器未播放任何东西），
// 之后才开始出声（如开始播放视频）。仅在音量面板展开期间以 1.5s 短间隔轮询，
// 面板关闭即停（零轮询）：音量调节只能在面板内进行，打开面板瞬间会补解析一次；
// 解析成功即停。
constexpr UINT_PTR kVolumeRetryTimerId = 6;
constexpr UINT kVolumeRetryMs = 1500;
// 挂起态短轮询：右键新建窗口待插入 / 抢前台重试期间每 200ms 刷新一次，
// 状态结束即停（不常驻）
constexpr UINT_PTR kTabRefreshTimerId = 1;
constexpr UINT kPendingPollMs = 200;
// 前台窗口创建瞬间尚未可见时的延迟复查（一次性定时器）
constexpr UINT_PTR kTargetRetryTimerId = 2;
// 前台窗口创建中不可见时最多复查次数（每次间隔 150ms，
// 资源管理器等窗口从激活到可见可能需要数百毫秒）
constexpr int kMaxTargetRetry = 5;
// 事件驱动刷新的抖动合并定时器（一次性）：高频窗口事件合并为一次刷新
constexpr UINT_PTR kTabRefreshDebounceTimerId = 3;
constexpr UINT kTabRefreshDebounceMs = 100;
// 低频兜底自检（一次性，每次刷新后重置）：事件驱动正常时基本不触发，
// 防止窗口事件丢失导致标签过期；无目标时停用（零轮询）。
// 10s 是功耗与兜底时延的折中（EnumWindows 全量枚举，频率越低越省电）
constexpr UINT_PTR kSlowRefreshTimerId = 4;
constexpr UINT kSlowRefreshMs = 10000;
// 右键新建窗口后抢前台的失败重试次数（随挂起态短轮询触发，约每 200ms 一次）
constexpr int kMaxPendingFocusAttempts = 5;
// 扩展断连优雅期（一次性定时器）：短暂掉线（扩展热重载、宿主重启、SW 被
// 系统回收后 alarm 复活）时不立刻丢弃已同步的浏览器标签、回退成窗口枚举
// ——那会让顶栏"只剩当前标签页"，正是本次要修的现象。等到优雅期结束仍
// 未重连才真正回退；期间一旦重连则取消。
constexpr UINT_PTR kChromeSyncGraceTimerId = 7;
constexpr UINT kChromeSyncGraceMs = 4000;
// 系统「快速设置」面板对齐定时器（面板打开期间常驻，关闭即停）：
// 轮询面板宿主的窗口区域，把面板上缘持续贴到顶栏下缘。
// 面板位置由 shell 决定（任务栏上方），只能由顶栏把它的宿主窗口挪到位；
// 面板内容变化（展开 Wi-Fi 等）会改变区域，故用轮询而非只对一次。
// 详见 ToggleQuickSettings / AlignQuickSettingsToBar。
constexpr UINT_PTR kQuickSettingsAlignTimerId = 8;
// 等面板出现 / 刚出现时的高频节拍：面板由 shell 拉起，show 之后才轮到我们
// 摆位置，节拍必须够快才抢在首帧之前；出现并稳定后再降到低频跟随。
constexpr UINT kQuickSettingsAlignFastMs = 20;
constexpr UINT kQuickSettingsAlignPollMs = 250;
// 等面板出现的最长时间（期间高频重试），以及出现后保持高频的时长
// （shell 可能在 show 之后再补一次定位，需要盯住）
constexpr ULONGLONG kQuickSettingsAlignWaitMs = 3000;
constexpr ULONGLONG kQuickSettingsAlignSettleMs = 800;
// 宿主窗口区域上缘比"可见卡片上缘"高出的留白（卡片投影，8 逻辑像素；
// 本机 150% DPI 实测 12px）。要对齐的是可见卡片，所以必须把这圈留白
// 一并算进去，否则卡片上方会留一条与留白等宽的缝（0 间距 = 紧贴）。
constexpr int kQuickSettingsCardInsetBase = 8;

// Dock 点击"已打开的应用"并激活后,显式请求顶栏聚焦该应用窗口。
// 顶栏的目标粘住逻辑不认 Dock 窗口上的点击(套件窗口),若不绕过,
// 用户从 Dock 切换应用时顶栏会停留在旧目标(如被最小化的应用)。
// lParam = 目标窗口 HWND;0 = 无直接句柄(异步托盘触发),仅解除粘住
// 并跟随当前前台。与 dock_main.cpp 的 kMsgTopBarFocus 同值。
constexpr UINT kDockFocusMsg = WM_APP + 12;
// 屏幕左上角触发角点击:展开/收起音量面板(当前目标应用的音量)。
// 与 dock_main.cpp 的 kMsgTopBarVolumePanel 同值。
constexpr UINT kDockVolumePanelMsg = WM_APP + 13;
// Dock 顶栏联动：Dock 开启自动收起时，顶栏层级跟随 Dock 展开/收起 ——
// Dock 展开时顶栏与 Dock 一样提升为置顶层并覆盖所有窗口（含当前
// 聚焦窗口，此时不让位）；Dock 收起时回到默认策略（只不遮挡当前
// 聚焦窗口）。自动收起关闭 / Dock 退出时收到 0 = 回到默认策略。
// wParam = 1 覆盖所有窗口 / 0 默认策略。与 dock_main.cpp 同值。
constexpr UINT kDockStateMsg = WM_APP + 14;
// ← Dock:查询"顶栏当前把哪个应用当作聚焦目标"。
// Dock 原本只按系统前台(GetForegroundWindow)判断"点图标 = 最小化还是聚焦"，
// 这与顶栏目标会打架：目标应用被最小化后 Windows 自动把前台让给别的应用、
// 而顶栏按设计"粘住"仍盯着被最小化的应用，此时点 Dock 图标会被判定成
// "该应用已在前台"→走最小化分支，顶栏继续停在旧应用上
// （用户现象：开完 Windows Terminal 再点浏览器，顶栏聚焦仍在终端）。
// 以顶栏目标为准，套件内语义才一致：wParam = 待判定的窗口 HWND，
// 返回值 1 = 该窗口属于顶栏当前目标应用 / 0 = 否。与 dock_main.cpp 同值。
constexpr UINT kDockFocusQueryMsg = WM_APP + 16;
// 顶栏启动查询：向 Dock 询问当前联动展开态（SendMessageTimeout 的返回值 =
// 1 覆盖所有窗口 / 0 默认策略；Dock 未运行/超时则按默认策略处理）。
// 与 dock_main.cpp 同值。
constexpr UINT kDockStateQueryMsg = WM_APP + 15;
// ← 宿主托盘菜单「保持顶栏」：请求立即按注册表重读开关并生效（顶栏层级、
// 全屏让位）。wParam = 0 仅重读（以注册表为准）。与 mywigets_main.cpp 同值。
constexpr UINT kKeepTopRefreshMsg = WM_APP + 18;
// ← Dock：查询顶栏当前焦点窗口（返回 targetHwnd；0 = 当前无目标）。
// Dock 的「未聚焦应用挂起」只认顶栏目标，必须能直接取到窗口句柄，
// 再按 Dock 自己的 exe 分组口径映射成应用 key。与 dock_main.cpp 同值。
constexpr UINT kDockTargetQueryMsg = WM_APP + 19;
// → Dock：顶栏目标变化通知（lParam = 新目标 HWND，0 = 已清空）。
// 收到后 Dock 立即恢复新目标、旧目标进入挂起宽限期。与 dock_main.cpp 同值。
constexpr UINT kDockTargetChangedMsg = WM_APP + 20;
// → Dock：Alt+Tab 键盘钩子事件（不携带目标窗口）。
//   wParam = 0：Tab 按下，提前恢复可能被选中的挂起应用；
//   wParam = 3：Alt 松开，会话结束，通知 Dock 进入落定期。
// 本钩子在 shell 处理 Tab 之前运行，是能拿到的最早时机；松手通知则用于
// Win11 XAML 切换器不发 SWITCHEND 的情况。与 dock_main.cpp 同值。
constexpr UINT kDockAltTabMsg = WM_APP + 21;
// 「保持顶栏」模式下的全屏让位复核周期（ms）。全屏应用占用本屏时顶栏整体
// 隐藏（用户要求：全屏游戏等场景顶栏不参与显示）；SHQueryUserNotificationState
// 约 75µs，只在模式开启期间按 2s 复核，不进任何高频路径。
constexpr UINT kKeepTopFullscreenCheckMs = 2000;

constexpr int kMenuExit = 1001;

// ---- Chrome 标签同步（WebSocket 服务端）----
// 本地回环端口：chrome-tab-sync 扩展连接此端口推送/接收标签数据
constexpr int kChromeSyncPort = 9786;
// 收到扩展消息后通知 UI 线程（lParam = new wchar_t[] JSON，UI 线程负责释放）
constexpr UINT kChromeSyncMsg = WM_APP + 10;
// 扩展连接/断开状态通知（wParam = 1 连接 / 0 断开）
constexpr UINT kChromeSyncStateMsg = WM_APP + 11;
// 客户端 socket 接收超时：空闲时向扩展推送**应用层 JSON 心跳**探活。
//
// 为什么必须是应用层消息而不是 WS 协议层 ping：
// MV3 的 Service Worker 空闲约 30s 会被回收（连接随之关闭，与省电模式无关），
// 而只有「JS 收到一条 WS 消息」才会重置该回收计时器——WS 协议的 ping/pong
// 由网络栈直接应答，不会派发到 JS，无法保活。因此探活间隔必须 < 30s；
// 这里取 15s（2 倍余量），并按 8 次无应答（120s）才判失联。
// 详见 chrome-tab-sync/background.js 文件头说明。
constexpr int kChromeSyncRecvTimeoutMs = 15000;
constexpr int kChromeSyncMaxIdleTimeouts = 8;
// 应用层心跳负载：扩展侧 handleMessage 收到 ping 回 pong。
// 两侧都必须走 JS 可见的文本帧，UI 线程无需处理（在帧层拦截，不惊动 UI）。
constexpr char kChromeSyncPingJson[] = "{\"type\":\"ping\"}";
constexpr char kChromeSyncPongJson[] = "{\"type\":\"pong\"}";
// 握手阶段读取超时（5s）与请求头上限（8KB）
constexpr int kChromeSyncHandshakeTimeoutMs = 5000;
constexpr size_t kChromeSyncMaxHeaderBytes = 8192;

enum ButtonHit {
    kHitNone = -1,
    kHitVolume = 3,      // 左上角音量按钮（展开/收起）
    kHitMute = 4,        // 面板内静音开关
    kHitSlider = 5,      // 面板内音量滑条
    kHitMinimize = 0,
    kHitMaximize = 1,
    kHitClose = 2,
    kHitTab = 6,        // Chrome 风格标签区域
    kHitClock = 7,      // 最小化键左侧的 Win11 风格时钟（时间/日期）
};

struct TabInfo {
    HWND hwnd = nullptr;
    DWORD pid = 0;
    std::wstring title;
    RectF rect;  // 顶栏客户区坐标

    // Chrome 同步模式（isChrome 为 true 时 hwnd/pid 无效，使用以下字段）
    bool isChrome = false;
    int chromeTabId = 0;
    bool chromeActive = false;
    bool chromePinned = false;
};

struct AppState {
    HWND hwnd = nullptr;
    int width = 0;
    int height = 0;
    int dpi = 96;
    float scale = 1.0f;

    HDC hdcMem = nullptr;
    HBITMAP hbmDib = nullptr;
    HBITMAP hbmOld = nullptr;
    void* pvBits = nullptr;
    Gdiplus::Bitmap* bitmap = nullptr;

    // 当前要控制的"前台/最后一次聚焦"窗口
    HWND targetHwnd = nullptr;
    bool hasTarget = false;              // 是否有可操作的有效目标窗口
    bool targetMaximized = false;        // 目标当前是否处于最大化
    bool targetSticky = false;           // 目标最小化后保持跟踪，不跟随系统自动转移的焦点

    // 双击顶栏空白处：目标窗口"填充顶栏以外区域"（自定义缩放，非系统最大化，
    // 故 IsZoomed() 不会为真），这里记录各窗口填充前的位置供再次双击还原。
    // 用 HWND 映射而不是单槽位：用户可能先后填充多个窗口并在其间来回切换，
    // 只记最后一个会让先填充的窗口丢失还原位置（第二次双击退化成"还原到
    // 填充后的矩形"，看起来像没反应）。
    std::unordered_map<HWND, WINDOWPLACEMENT> filledWindows;

    int hoverButton = kHitNone;          // 当前悬停的按钮
    int hoverTab = -1;                   // 当前悬停的 Chrome 标签索引
    bool trackingMouse = false;
    // Dock 顶栏联动：Dock 是否处于展开态（true = 与 Dock 一致提升置顶层，
    // 覆盖所有窗口，含当前聚焦窗口，此时不做让位）。
    // Dock 未运行/自动收起关闭时恒为 false（走默认策略）。
    bool dockLinkExpanded = false;
    // 「保持顶栏」模式（见 keep_top.h）：恒置顶层 + 从不为聚焦窗口让位。
    // 由托盘菜单切换（注册表持久化），Dock 侧消息与启动时读注册表两路同步。
    bool keepTopEnabled = false;
    // 模式下的全屏让位：前台出现铺满本屏的应用（游戏全屏等）时整体隐藏，
    // 退出全屏恢复显示。keepTopHidden 记录"是本功能把顶栏藏起来的"。
    bool keepTopHidden = false;
    ULONGLONG lastKeepTopFullscreenCheck = 0;
    // 默认层级策略（Dock 未联动展开时）：只不遮挡当前聚焦窗口 ——
    //   barYielding = true  让位态：目标窗口与顶栏矩形重叠，顶栏摘掉置顶位
    //       并紧贴 barAnchorHwnd 之下（被它盖住），仍高于其它非聚焦窗口；
    //   barYielding = false 覆盖态：置顶层，覆盖除目标外的所有窗口。
    // 让位判定只看顶栏自己的聚焦目标（targetHwnd），与按钮/标签同源。
    bool barYielding = false;
    HWND barAnchorHwnd = nullptr;   // 让位态锚点：顶栏紧贴其下（= 目标窗口）
    // 已应用的层级（-1 未应用 / 0 覆盖态 / 1 让位态）：启动时强制对齐一次，
    // 之后状态未变就不做任何 Z 序操作（LOCATIONCHANGE 事件高频触发）
    int barLayerApplied = -1;

    // Win11 风格任务栏时钟（显示在最小化键左侧：时间在上、日期在下两行）
    std::wstring clockTimeText;          // 当前时间文本（如 "21:45"）
    std::wstring clockDateText;          // 当前日期文本（如 "2026/8/28"）
    bool clockShowSeconds = false;       // 是否显示秒（跟随系统"显示秒"设置）

    // 系统「快速设置」面板对齐轮询（见 ToggleQuickSettings/对齐定时器）：
    // quickAlignSeen 区分"面板还没被 shell 拉起来"与"面板已经关掉"——
    // 前者要再等一会儿，后者要立刻停表（零轮询）
    bool quickAlignSeen = false;
    bool quickAlignSettled = false;        // 是否已从高频节拍降到低频
    ULONGLONG quickAlignDeadline = 0;      // 等面板出现的截止时刻
    ULONGLONG quickAlignFastUntil = 0;     // 出现后保持高频节拍的截止时刻

    // 按住空白处拖动（等同标题栏拖动，移动当前目标窗口）
    bool dragWindow = false;             // 正在拖动（按下空白处）
    bool dragMoving = false;             // 已超过阈值，真正移动窗口
    HWND dragHwnd = nullptr;             // 被拖动的窗口
    POINT dragStartPt{};                 // 按下点（屏幕坐标）
    POINT dragOffset{};                  // 光标相对窗口左上角的偏移

    // 按住标签左右拖动重排（"随意拖动标签页"）：
    //   按下标签 -> tabPressIndex 记住位置；超过阈值 -> tabDragging
    //   拖动中按光标位置实时"让位"（被拖标签在 s.tabs 中逐格移动，
    //   其余标签自动补位），松手时落定并重绘。
    // 被拖标签用身份（Chrome tabId / 窗口句柄）跟踪而不是索引：
    // 拖动途中扩展推送一条 tabUpdated 就会重建列表，索引会漂移。
    int tabPressIndex = -1;      // 按住的标签索引（-1 = 没有按住）
    bool tabDragging = false;    // 是否已超过阈值、进入拖动重排
    int tabDragIndex = -1;       // 被拖标签当前所处索引
    int tabDragOriginIndex = -1; // 本次拖动开始时被拖标签的位置（"原来的位置"）
    bool tabDragIsChrome = false;  // 被拖标签是否来自 Chrome 同步
    int tabDragChromeId = 0;     // Chrome 模式：被拖标签的 tabId
    HWND tabDragHwnd = nullptr;  // 窗口模式：被拖标签的窗口句柄
    POINT tabPressPt{};          // 按下点（客户区坐标，判定阈值用）
    POINT tabDragPt{};           // 拖动中的光标（客户区坐标，浮起标签跟随用）
    float tabDragGrabDx = 0.0f;  // 按下点相对标签左边缘的偏移
    // 浮起标签的"身体"左边界（客户区坐标，= 光标 - 抓点偏移）：换位判定按
    // 相邻标签宽度的 20% 计算重叠，所以判定要用身体的实际位置而不是光标
    float tabDragBodyX = 0.0f;
    // 上一次换位时的身体位置：同方向还要再拖 20% 才允许下次换位（防回抖）
    float tabDragAnchorX = 0.0f;

    // Chrome 风格标签：聚焦窗口所属应用打开的全部顶层窗口
    std::vector<TabInfo> tabs;

    // Chrome 标签同步状态（UI 线程维护，数据来自 chrome-tab-sync 扩展）
    wsproto::ChromeSyncModel chromeSync;

    // 右键“新建窗口”后的待插入状态：新窗口出现时插入到该标签右侧
    HWND insertAfterTab = nullptr;
    bool insertPending = false;
    ULONGLONG insertPendingSince = 0;  // 挂起起始时间：超时未出现则放弃

    // 右键新建窗口的聚焦重试：新窗口出现后反复尝试抢前台，
    // 直到窗口真正成为前台、用户已切到其他应用或达到重试上限
    HWND pendingFocusHwnd = nullptr;
    HWND pendingFocusOriginHwnd = nullptr;  // 挂起时的前台窗口：用户未切换则继续抢
    int pendingFocusAttempts = 0;
    int targetRetryCount = 0;   // 前台窗口创建中不可见的复查次数

    // ---- 音量调节（类似 Windows 音量合成器，控制前台窗口所在进程）----
    bool volumeOpen = false;             // 音量面板是否展开
    bool volumeReady = false;            // 是否有可控制的有效前台会话
    bool volumeDragging = false;         // 正在拖动滑条
    bool volumeHoverMute = false;        // 悬停在静音按钮上
    bool volumeHoverSlider = false;      // 悬停在滑条上
    float volumeValue = 0.0f;            // 0..1 目标进程当前音量
    bool volumeMuted = false;            // 目标进程是否静音
    DWORD volumePid = 0;                 // 当前目标进程 PID
    ComPtr<ISimpleAudioVolume> volumeSession;  // 解析到的目标音频会话
    DWORD volumeSessionPid = 0;          // volumeSession 对应的 PID
    HWND volumeTargetHwnd = nullptr;     // 已为其解析会话的目标窗口

    // 独立的音量面板窗口（顶栏保持 40px 高度，避免窗口矩形变化干扰其他组件/窗口）
    HWND volumePanelHwnd = nullptr;
    HDC panelHdcMem = nullptr;
    HBITMAP panelHbmDib = nullptr;
    HBITMAP panelHbmOld = nullptr;
    void* panelPvBits = nullptr;
    Gdiplus::Bitmap* panelBitmap = nullptr;
};

using SetProcessDpiAwarenessContextFn = BOOL(WINAPI*)(HANDLE);
using GetDpiForWindowFn = UINT(WINAPI*)(HWND);

void EnableDpiAwareness() {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        auto pSetContext = reinterpret_cast<SetProcessDpiAwarenessContextFn>(
            GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
        if (pSetContext) {
            // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 == -4
            if (pSetContext(reinterpret_cast<HANDLE>(static_cast<INT_PTR>(-4)))) {
                return;
            }
        }
    }
    SetProcessDPIAware();
}

int GetWindowDpi(HWND hwnd) {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        auto pGetDpi = reinterpret_cast<GetDpiForWindowFn>(
            GetProcAddress(user32, "GetDpiForWindow"));
        if (pGetDpi) {
            return static_cast<int>(pGetDpi(hwnd));
        }
    }
    HDC hdc = GetDC(hwnd);
    int dpi = GetDeviceCaps(hdc, LOGPIXELSX);
    ReleaseDC(hwnd, hdc);
    return dpi;
}

void DestroyBacking(AppState& s) {
    delete s.bitmap;
    s.bitmap = nullptr;

    if (s.hdcMem) {
        if (s.hbmDib) {
            if (s.hbmOld) {
                SelectObject(s.hdcMem, s.hbmOld);
            }
            DeleteObject(s.hbmDib);
            s.hbmDib = nullptr;
        }
        DeleteDC(s.hdcMem);
        s.hdcMem = nullptr;
    }
    s.hbmOld = nullptr;
    s.pvBits = nullptr;
}

bool CreateBacking(AppState& s, int width, int height) {
    DestroyBacking(s);

    HDC screenDc = GetDC(s.hwnd);
    if (!screenDc) {
        return false;
    }

    s.hdcMem = CreateCompatibleDC(screenDc);
    if (!s.hdcMem) {
        ReleaseDC(s.hwnd, screenDc);
        return false;
    }

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height; // 自上而下
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    s.hbmDib = CreateDIBSection(screenDc, &bmi, DIB_RGB_COLORS, &s.pvBits,
                                nullptr, 0);
    ReleaseDC(s.hwnd, screenDc);

    if (!s.hbmDib) {
        DeleteDC(s.hdcMem);
        s.hdcMem = nullptr;
        return false;
    }

    s.hbmOld = static_cast<HBITMAP>(SelectObject(s.hdcMem, s.hbmDib));
    s.bitmap = new Gdiplus::Bitmap(width, height, PixelFormat32bppPARGB);
    if (!s.bitmap) {
        DestroyBacking(s);
        return false;
    }

    s.width = width;
    s.height = height;
    s.scale = static_cast<float>(s.dpi) / 96.0f;
    return true;
}

// 计算右侧三个按钮的水平布局（从右往左：关闭 | 最大化 | 最小化）
void ComputeButtonRects(AppState& s,
                        RectF& minRect, RectF& maxRect, RectF& closeRect) {
    float bw = kButtonWidthBase * s.scale;
    float gap = kButtonGap * s.scale;
    const float top = 0.0f;
    // 窗口控制按钮只占顶栏高度（下面还有音量面板时会变高）
    const float h = static_cast<float>(MulDiv(kBaseTabHeight, s.dpi, 96));

    closeRect.X = s.width - bw;
    closeRect.Y = top;
    closeRect.Width = bw;
    closeRect.Height = h;

    maxRect.X = closeRect.X - gap - bw;
    maxRect.Y = top;
    maxRect.Width = bw;
    maxRect.Height = h;

    minRect.X = maxRect.X - gap - bw;
    minRect.Y = top;
    minRect.Width = bw;
    minRect.Height = h;
}

// 计算 Win11 风格时钟区域：紧贴最小化按钮左侧
// （布局从右往左：关闭 | 最大化 | 最小化 | 时钟 | 标签）
void ComputeClockRect(AppState& s, RectF& clockR) {
    const float k = s.scale;
    RectF minR, maxR, closeR;
    ComputeButtonRects(s, minR, maxR, closeR);
    const float gap = kButtonGap * k;
    clockR.X = minR.X - gap - kClockWidthBase * k;
    clockR.Y = 0.0f;
    clockR.Width = kClockWidthBase * k;
    // BarAreaHeight 定义在下方（此处仅有前置声明），直接按同式展开
    clockR.Height = static_cast<float>(MulDiv(kBaseTabHeight, s.dpi, 96));
}

// 读取系统"在系统托盘时钟中显示秒"设置（Windows 设置 > 个性化 > 任务栏），
// 与任务栏时钟保持一致；读取失败时默认不显示秒（Win11 默认行为）。
bool ReadShowSecondsSetting() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(
            HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced",
            0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
        return false;
    }

    bool enabled = false;
    DWORD type = 0;
    DWORD size = 0;
    if (RegQueryValueExW(key, L"ShowSecondsInSystemClock", nullptr, &type,
                         nullptr, &size) == ERROR_SUCCESS) {
        if (type == REG_SZ && size >= 2 && size <= 8) {
            // 新旧 Windows 均以字符串 "1"/"0" 存储
            wchar_t buf[4] = {};
            DWORD bufSize = static_cast<DWORD>(sizeof(buf));
            if (RegQueryValueExW(key, L"ShowSecondsInSystemClock", nullptr,
                                 nullptr, reinterpret_cast<BYTE*>(buf),
                                 &bufSize) == ERROR_SUCCESS) {
                enabled = buf[0] == L'1';
            }
        } else if (type == REG_DWORD && size >= sizeof(DWORD)) {
            // 兼容个别版本以 DWORD 存储的情况
            DWORD val = 0;
            DWORD valSize = static_cast<DWORD>(sizeof(val));
            if (RegQueryValueExW(key, L"ShowSecondsInSystemClock", nullptr,
                                 nullptr, reinterpret_cast<BYTE*>(&val),
                                 &valSize) == ERROR_SUCCESS) {
                enabled = val != 0;
            }
        }
    }
    RegCloseKey(key);
    return enabled;
}

// 刷新时钟文本：时间/日期按系统区域格式生成；仅当文本变化时返回 true，
// 供每秒定时器决定是否需要重绘（分钟跳变 / 跨午夜 / 秒显示时无谓重绘）。
bool UpdateClock(AppState& s) {
    SYSTEMTIME st{};
    GetLocalTime(&st);

    wchar_t timeBuf[64] = {};
    wchar_t dateBuf[64] = {};
    if (s.clockShowSeconds) {
        // 跟随系统设置显示秒：按用户 12/24 小时制偏好补上 ":ss"
        DWORD itime = 0;
        GetLocaleInfoW(LOCALE_USER_DEFAULT, LOCALE_ITIME | LOCALE_RETURN_NUMBER,
                       reinterpret_cast<LPWSTR>(&itime),
                       static_cast<int>(sizeof(itime) / sizeof(wchar_t)));
        const wchar_t* fmt =
            (itime != 0) ? L"HH:mm:ss" : L"h:mm:ss tt";
        GetTimeFormatW(LOCALE_USER_DEFAULT, 0, &st, fmt, timeBuf, 64);
    } else {
        GetTimeFormatW(LOCALE_USER_DEFAULT, TIME_NOSECONDS, &st, nullptr,
                       timeBuf, 64);
    }
    GetDateFormatW(LOCALE_USER_DEFAULT, DATE_SHORTDATE, &st, nullptr,
                   dateBuf, 64);

    if (s.clockTimeText == timeBuf && s.clockDateText == dateBuf) {
        return false;
    }
    s.clockTimeText = timeBuf;
    s.clockDateText = dateBuf;
    return true;
}

// 模拟系统级 Win+<键> 组合快捷键。
//
// 为什么必须用 SendInput 而不是 PostMessage 投递给某个窗口：
// 顶栏是 WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW 的置顶浮层，点击它不抢焦点
// （前台仍是用户的应用窗口），而系统快捷键由 shell 在全局热键层处理，
// 只有注入到全局输入流（SendInput）才会被系统当作真实的 Win 组合键。
//
//   Win+A → 系统「快速设置」面板（任务栏网络/音量/电池三合一按钮同一面板）
//   Win+N → 系统「通知中心 / 日历」浮出窗口（= 点击 Win11 任务栏时钟的窗口）
void SendWinChord(WORD vk) {
    INPUT inputs[4] = {};
    int count = 0;
    auto keyEvent = [&inputs, &count](WORD key, bool up) {
        inputs[count].type = INPUT_KEYBOARD;
        inputs[count].ki.wVk = key;
        inputs[count].ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
        ++count;
    };
    keyEvent(VK_LWIN, false);
    keyEvent(vk, false);
    keyEvent(vk, true);
    keyEvent(VK_LWIN, true);
    SendInput(static_cast<UINT>(count), inputs, sizeof(INPUT));
}

// 系统「快速设置」面板的宿主窗口判定（类名固定 ControlCenterWindow，
// 窗口名"快速设置"）。面板由 shell 托管，可传任意候选窗口。
bool IsQuickSettingsHost(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) {
        return false;
    }
    wchar_t cls[64] = {};
    if (GetClassNameW(hwnd, cls, 63) <= 0) {
        return false;
    }
    return wcscmp(cls, L"ControlCenterWindow") == 0;
}

// 宿主窗口是否在前台（= 面板当前打开）
bool IsQuickSettingsForeground() {
    return IsQuickSettingsHost(GetForegroundWindow());
}

// 把系统「快速设置」面板挪到"上缘紧贴顶栏下缘"。
//
// 为什么需要挪：面板的摆放由 shell 决定 —— 它把面板锚在任务栏上方（本机
// 系统任务栏被 Dock 隐藏，于是落在屏幕右下角），顶栏无法通过任何公开接口
// 指定锚点。能改的只有面板宿主的窗口位置：
//   * 宿主是屏幕右侧一条高约整屏的透明浮层（本机 150% DPI 下 576x1528、
//     位于 (1984,0)），可见卡片贴在宿主底部；
//   * GetWindowRgn(宿主) 返回的窗口区域上缘在"可见卡片上缘"之上 8 逻辑
//     像素（卡片投影留白；本机 150% DPI 实测 12px），且随内容变化移动；
//   * 于是把宿主上移"区域上缘 + 留白 - 顶栏下缘"，可见卡片上缘正好落在
//     顶栏下缘（严格 0 间距，不靠肉眼估）。
// 代价：宿主区域会因此向上越过顶栏下缘 8 逻辑像素（那圈投影留白与顶栏
// 下缘重叠）。那一条是半透明投影、可见卡片并未被遮住；期间点在那条里
// 等同于"点在面板外"，shell 会把面板收起（与再点一次时钟同样的效果）。
// 面板关闭/宿主不在前台时本函数不做任何事；取不到窗口区域时也保持系统
// 原位（宁可不挪，也不猜一个位置乱放）。
// 返回 true = 这次已把它摆到位（或本来就到位）。
bool AlignQuickSettingsToBar(AppState& s, HWND host) {
    if (!IsQuickSettingsHost(host)) {
        return false;
    }
    RECT hostRc{};
    RECT barRc{};
    if (!GetWindowRect(host, &hostRc) || !GetWindowRect(s.hwnd, &barRc)) {
        return false;
    }

    // 窗口区域（宿主是无边框弹窗，客户区原点与窗口原点一致）
    HRGN rgn = CreateRectRgn(0, 0, 0, 0);
    if (!rgn) {
        return false;
    }
    const int kind = GetWindowRgn(host, rgn);
    RECT rgnBox{};
    const BOOL haveBox = GetRgnBox(rgn, &rgnBox) != 0;
    DeleteObject(rgn);
    if (kind == ERROR || kind == NULLREGION || !haveBox) {
        return false;  // 区域还没就绪（刚 show）：下次高频节拍再来
    }

    const int targetTop = barRc.bottom - rgnBox.top -
                          MulDiv(kQuickSettingsCardInsetBase, s.dpi, 96);
    if (targetTop != hostRc.top) {
        SetWindowPos(host, HWND_TOPMOST, hostRc.left, targetTop,
                     hostRc.right - hostRc.left, hostRc.bottom - hostRc.top,
                     SWP_NOACTIVATE);
    }
    return true;
}

// 起表：高频等面板出现（见 kQuickSettingsAlignFastMs）。时钟左键、
// 以及 WinEvent 收到面板 show 时都会调用；重复调用只是重置节拍。
void ArmQuickSettingsAlign(AppState& s, bool alreadyShown) {
    const ULONGLONG now = GetTickCount64();
    if (!alreadyShown) {
        s.quickAlignSeen = false;
        s.quickAlignDeadline = now + kQuickSettingsAlignWaitMs;
    } else {
        s.quickAlignSeen = true;
    }
    s.quickAlignSettled = false;
    s.quickAlignFastUntil = now + kQuickSettingsAlignSettleMs;
    SetTimer(s.hwnd, kQuickSettingsAlignTimerId, kQuickSettingsAlignFastMs,
             nullptr);
}

void StopQuickSettingsAlign(AppState& s) {
    KillTimer(s.hwnd, kQuickSettingsAlignTimerId);
}

// 打开/收起系统「快速设置」面板（Win+A）：就是点击 Win11 任务栏右侧
// 网络/音量/电池三合一按钮弹出的那个面板。左键时钟即调用本函数。
//
// 位置由 kQuickSettingsAlignTimerId 对齐（见 AlignQuickSettingsToBar）。
// 这里只"起表"；真正抢在首帧之前摆好位置的是 WinEventProc 里的
// EVENT_OBJECT_SHOW 分支（面板一 show 立刻挪），定时器负责兜底 +
// 跟随展开内容（展开 Wi-Fi / 蓝牙列表时区域上缘会变，需要持续复查）。
void ToggleQuickSettings(AppState& s) {
    if (IsQuickSettingsForeground()) {
        // 已打开：收起并停掉对齐轮询
        StopQuickSettingsAlign(s);
        SendWinChord('A');
        return;
    }
    SendWinChord('A');
    ArmQuickSettingsAlign(s, false);
}

// 对齐轮询节拍：面板在 → 对齐并（稳定后）降到低频跟随；面板关 → 停表。
// 面板还没出现时按 kQuickSettingsAlignWaitMs 宽限等待 —— shell 拉起有
// 延迟，第一拍还没出现不能立刻停表。
void TickQuickSettingsAlign(AppState& s) {
    const ULONGLONG now = GetTickCount64();
    HWND fg = GetForegroundWindow();
    if (!IsQuickSettingsHost(fg)) {
        if (s.quickAlignSeen || now > s.quickAlignDeadline) {
            StopQuickSettingsAlign(s);
        }
        return;
    }
    AlignQuickSettingsToBar(s, fg);
    if (!s.quickAlignSeen) {
        s.quickAlignSeen = true;
        s.quickAlignFastUntil = now + kQuickSettingsAlignSettleMs;
    } else if (!s.quickAlignSettled && now > s.quickAlignFastUntil) {
        // 已稳定：降到低频（内容变化时才需要再动，零浪费）
        s.quickAlignSettled = true;
        SetTimer(s.hwnd, kQuickSettingsAlignTimerId, kQuickSettingsAlignPollMs,
                 nullptr);
    }
}

// 打开/收起系统「通知中心 / 日历」浮出窗口（Win+N，含日期时间日历）。
// 顶栏时钟的左键已改为快速设置，这里保留为时钟右键的日历入口。
//
// 为什么不是原先的 Win+Alt+D：该组合在本机实测不产生任何窗口（发送前后
// 前台窗口不变、截图无浮出层），且它本就不是 Windows 官方快捷键；而 Dock
// 运行时会隐藏系统任务栏，那条"点击任务栏时钟"的路径也不存在。Win+N 是
// 官方快捷键，实测能稳定唤出与点击任务栏时钟相同的窗口。
void ToggleCalendarFlyout() { SendWinChord('N'); }

// 全局钩子：用户"显式选择窗口"的信号（鼠标点击 / Alt+Tab），
// 供目标跟踪区分系统自动转移焦点与用户主动切换窗口
HWND g_lastClickWindow = nullptr;
bool g_altTabPressed = false;
// Alt+Tab 会话是否仍在进行（Alt 键还没松开）。Win11 的 XAML 切换器
// 不一定会发 EVENT_SYSTEM_SWITCHSTART/SWITCHEND，所以由低级键盘钩子
// 自己记录会话，并在 Alt 松开时通知 Dock “落定”—— 否则 Dock 只能等
// 10s 会话上限才恢复挂起，非目标应用会被无谓地多唤醒几秒。
bool g_altTabSessionActive = false;
// 用户点击了套件窗口（Dock/启动台/时钟等）——落点本身不能当目标（不是
// 任务栏级应用窗），但它同样表明"用户在主动切换应用"：目标粘住态必须
// 立即解除，否则顶栏会永久停在被最小化的旧目标上
// （现象：最小化终端后从 Dock/启动台点浏览器，顶栏聚焦仍在终端）。
bool g_suiteClickSwitch = false;
HHOOK g_keyHook = nullptr;

bool IsCloaked(HWND hwnd);  // 前置声明（定义在让位判定节）

// 窗口是否"在任务栏中看得见"（= 系统任务栏会给它一个按钮）。
//
// 这是顶栏标签收录与聚焦动作的统一准入判据，判据与 Windows 任务栏一致：
//   1. 可见：隐藏窗口没有任务栏按钮（最小化窗口 IsWindowVisible 仍为真，
//      仍算任务栏可见，因此最小化的窗口照常可被顶栏聚焦/还原）；
//   2. 顶层且无属主：属主的对话框/浮层不单独占任务栏按钮。
//      例外：WS_EX_APPWINDOW 是应用显式要求"进任务栏"的风格，带属主也算；
//   3. 非 WS_EX_TOOLWINDOW：工具窗（托盘图标应用的辅助窗、子进程的消息窗、
//      应用的浮动调色板）永远不进任务栏；
//   4. 非 DWM cloaked：位于其它虚拟桌面、或被应用自己藏起来的 UWP 窗口
//      在当前桌面的任务栏里看不见。
//
// 为什么顶栏必须只认这类窗口：聚焦一个任务栏里看不见的窗口时，
// SetForegroundWindow 会把它"显示出来"（隐藏窗被激活即现身），
// 表现就是点在顶栏标签上突然冒出一个没头没尾的窗口 —— 子进程的
// 隐藏消息窗、托盘图标应用的驻留窗都属于这一类。任务栏看不见的一律
// 不收录、不可聚焦，这类误开窗就不可能发生。
bool IsTaskbarWindow(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd) || !IsWindowVisible(hwnd)) {
        return false;
    }
    const LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if ((ex & WS_EX_APPWINDOW) == 0) {
        // GA_ROOTOWNER 顺属主链上溯：带属主的窗口返回属主（≠ 自身），
        // 子窗口同样返回其顶层窗口，二者都不单独占任务栏按钮
        if (GetAncestor(hwnd, GA_ROOTOWNER) != hwnd) {
            return false;
        }
        if ((ex & WS_EX_TOOLWINDOW) != 0) {
            return false;
        }
    }
    return !IsCloaked(hwnd);
}

// 判断一个窗口是否应视为"可控制的有效目标窗口"
bool IsControlTarget(HWND hwnd, HWND self) {    if (!hwnd || hwnd == self) {
        return false;
    }
    if (!IsWindow(hwnd) || !IsWindowVisible(hwnd)) {
        return false;
    }
    // 忽略桌面 / 任务栏等系统窗口
    wchar_t cls[64] = {};
    const int len = GetClassNameW(hwnd, cls, 63);
    if (len <= 0) {
        return false;
    }
    if (wcscmp(cls, L"Progman") == 0 ||
        wcscmp(cls, L"WorkerW") == 0 ||
        wcscmp(cls, L"SHELLDLL_DefView") == 0 ||
        wcscmp(cls, L"Shell_TrayWnd") == 0 ||
        wcscmp(cls, L"Shell_SecondaryTrayWnd") == 0) {
        return false;
    }
    // 忽略本套件组件窗口（Dock/时钟/日历/启动台/托盘宿主）：
    // 独立 exe 模式下与顶栏不同进程，pid 检查拦不住；点击套件窗口
    // （如 Dock 图标切换应用）不应被视为"显式选择了另一个窗口"
    static const wchar_t* kSuiteCls[] = {
        L"DesktopTopBarWindow", L"DesktopDockWindow",
        L"DesktopAnalogClockWindow", L"DesktopCalendarWindow",
        L"DesktopLauncherWindow", L"MyWigetsTrayWindow",
    };
    for (const wchar_t* c : kSuiteCls) {
        if (wcscmp(cls, c) == 0) {
            return false;
        }
    }
    // 忽略我们自己进程创建的组件窗口
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId()) {
        return false;
    }
    // 任务栏中看不见的窗口不作为顶栏目标：托盘图标应用的启动器/工具窗
    // （如 hotspot 的 WS_EX_TOPMOST|WS_EX_TOOLWINDOW 弹出窗）、子进程消息窗、
    // cloaked 窗都属于此类。它们不是任务栏级的应用窗口，标签栏里也不会有
    // 它们的标签；一旦被顶栏接管，顶栏就会为"只不遮挡聚焦窗口"重排层级 ——
    // 从被最大化窗口遮住的让位态顶回最前，凭空弹出一条没有标签的顶栏。
    if (!IsTaskbarWindow(hwnd)) {
        return false;
    }
    return true;
}

void ResolveVolumeSession(AppState& s);  // 前置声明（定义在前台窗口轮询之后）
// 目标应用是否还有"看得见且未最小化"的窗口（整组最小化判定，定义在下方）
bool AppHasVisibleWindow(HWND self, HWND target);
bool ProcessTreeContains(DWORD rootPid, DWORD sessionPid);  // 前置声明（定义在下方）
bool RefreshTabs(AppState& s);             // 前置声明（定义在下方）
int HitTestTab(AppState& s, int x, int y); // 前置声明（定义在下方）
// 拖动标签重排相关（定义在下方）
void TabStripRange(AppState& s, float& left, float& right);
int TabDragLo(AppState& s, bool draggingPinned);  // 让位区间下限（固定标签约束）
int TabDragHi(AppState& s, bool draggingPinned);  // 让位区间上限（固定标签约束）
void CancelTabDrag(AppState& s);
void CommitTabReorder(AppState& s, int movedIndex);

// Chrome 标签同步相关前置声明（定义在下方）
bool IsChromeTarget(HWND hwnd);
bool ChromeSyncMode(AppState& s);
bool ChromeSyncSendJson(const std::string& utf8Json);
void ChromeSyncSendActivateTab(int tabId);
void ChromeSyncSendCloseTab(int tabId);
void ChromeSyncSendNewTab();
void ChromeSyncStart(HWND hwnd);
void ChromeSyncStop();

// 音量面板几何 / 操作的前置声明（定义在下方，供命中测试与消息处理使用）
int BarHeight(AppState& s);
int BarAreaHeight(AppState& s);
int WindowHeight(AppState& s);
void VolumeButtonRect(AppState& s, RectF& r);
void VolumePanelInner(AppState& s, RectF& panel, RectF& mute, RectF& track);
void ApplyVolume(AppState& s, float value);
void ToggleMute(AppState& s);
void DrawBarAndPresent(AppState& s);        // 前置声明（定义在下方）
// 顶栏层级：默认只不遮挡当前聚焦窗口（见文件头说明），定义在下方
bool TargetOverlapsBar(AppState& s);
void ApplyBarLayer(AppState& s, bool force);
void HealBarLayer(AppState& s);
// 「保持顶栏」模式：开关落地与全屏让位复核（定义在下方）
void SetKeepTopEnabled(AppState& s, bool enabled);
bool CheckKeepTopFullscreen(AppState& s, bool force);
void RetargetLocationHook(HWND target);
void SetVolumeOpen(AppState& s, bool open);

// 独立音量面板窗口相关前置声明
void DrawButtonHover(Graphics& g, const RectF& r, ButtonHit hit, int hover);
void DrawVolumeGlyph(Graphics& g, const RectF& r, float scale, const Color& col,
                     bool muted);
void DrawVolumePercent(Graphics& g, AppState& s, const RectF& r, float value,
                       bool ready);
void AddRoundedRectPath(GraphicsPath& path, const RectF& r, float radius);
void DrawVolumePanelContent(Graphics& g, AppState& s);
void PresentVolumePanel(AppState& s);
void ShowVolumePanel(AppState& s);
void HideVolumePanel(AppState& s);

// 通知 Dock：顶栏焦点窗口变了。Dock 的未聚焦应用挂起以顶栏目标为准；
// 没有这条消息，Dock 只能等 2s 保底，切换应用时会多冻结旧应用两秒。
void NotifyDockTargetChanged(HWND target) {
    HWND dock = FindWindowW(L"DesktopDockWindow", nullptr);
    if (!dock || !IsWindow(dock)) return;
    PostMessageW(dock, kDockTargetChangedMsg, 0,
                 reinterpret_cast<LPARAM>(target));
}

// 把某个窗口设为当前目标并刷新最大化状态、音量会话与 Chrome 标签
void ApplyTargetInfo(AppState& s, HWND hwnd) {
    const bool hadTarget = s.hasTarget;
    const HWND previous = s.targetHwnd;
    s.targetHwnd = hwnd;
    s.hasTarget = true;
    s.targetMaximized = IsZoomed(hwnd) != FALSE;

    // 目标窗口变化时重新解析其进程的音频会话，供音量面板使用
    if (s.targetHwnd != s.volumeTargetHwnd) {
        s.volumeTargetHwnd = s.targetHwnd;
        ResolveVolumeSession(s);
    }

    // 刷新 Chrome 风格标签：显示该应用打开的全部窗口
    RefreshTabs(s);

    // 跟着目标走：移动/缩放事件只订阅目标进程（拖动时每秒上百条，
    // 全系统订阅会把噪音灌进本线程消息队列），并立即重估让位层级
    RetargetLocationHook(hwnd);
    ApplyBarLayer(s, false);
    // 同一窗口的高频重入（最大化/位置事件）不重复通知；真正切换才发。
    if (!hadTarget || previous != hwnd) NotifyDockTargetChanged(hwnd);
}

// 轮询前台窗口，更新目标与 Chrome 标签。轮询而不是事件绑定，
// 是为了在 WS_EX_NOACTIVATE（点击不抢占焦点）的前提下，
// 稳定跟踪"鼠标最后一次聚焦"的窗口。
//
// 特殊处理：当当前目标被最小化时，Windows 会自动把前台让给下一个窗口
// （如 A 最小化后 B 成为前台）。此时顶栏不跟随系统转移——目标保持 A，
// 直到用户显式点击其他窗口或 Alt+Tab 切换，避免"最小化 A 却变成操作 B"。
//
// 例外（本函数入口的整组最小化判定）：被最小化的是该应用的**最后**一张
// 窗口（如 Dock 点图标"最小化全部窗口"，或最小化单窗口应用的唯一窗口）时，
// 应用已经没有可展示的窗口，粘在它身上只会让顶栏聚焦一个看不见的应用。
// 此时不再粘住，改为跟随 Windows 当前聚焦的窗口。
void UpdateTarget(AppState& s) {
    HWND fg = GetForegroundWindow();

    // 目标粘住：当前目标因最小化而失去前台时，保持跟踪它
    if (s.hasTarget && s.targetHwnd && IsWindow(s.targetHwnd)) {
        if (IsIconic(s.targetHwnd)) {
            s.targetSticky = true;
        }
        if (s.targetSticky) {
            // 整组最小化：目标应用已无任何"看得见且未最小化"的窗口
            // （含最小化动作尚未落到 IsIconic 的竞态帧——那时前台早已
            // 让给别人，同样按整组最小化处理）。解粘并落到下方通用逻辑：
            // 跟随 Windows 当前聚焦的窗口；若系统没有给出可聚焦的前台
            // 窗口，则清空目标（标签清空），而不是继续显示看不见的应用。
            if (IsIconic(s.targetHwnd) || fg != s.targetHwnd) {
                if (!AppHasVisibleWindow(s.hwnd, s.targetHwnd)) {
                    s.targetSticky = false;
                }
            }
        }
        if (s.targetSticky) {
            // 用户显式选择：鼠标点击其他窗口 / Alt+Tab
            HWND explicitHwnd = nullptr;
            if (g_lastClickWindow && g_lastClickWindow != s.targetHwnd &&
                IsControlTarget(g_lastClickWindow, s.hwnd)) {
                explicitHwnd = g_lastClickWindow;
            } else if (g_altTabPressed && IsControlTarget(fg, s.hwnd)) {
                explicitHwnd = fg;
            }
            const bool suiteSwitch = g_suiteClickSwitch;
            g_lastClickWindow = nullptr;
            g_altTabPressed = false;
            g_suiteClickSwitch = false;

            if (explicitHwnd) {
                // 用户主动切到其他窗口：解除粘住并切换
                ApplyTargetInfo(s, explicitHwnd);
                s.targetSticky = false;
            } else if (suiteSwitch && IsControlTarget(fg, s.hwnd) &&
                       fg != s.targetHwnd) {
                // 用户从套件窗口（Dock 图标 / 启动台）发起的切换：落点不是
                // 应用窗口，但前台此刻已是用户要去的应用（如 Dock 点浏览器
                // 时浏览器已在前台）→ 跟随当前前台，解除粘住
                ApplyTargetInfo(s, fg);
                s.targetSticky = false;
            } else if (suiteSwitch) {
                // 套件操作引发的前台切换可能尚未生效（如启动台正在拉起应用）：
                // 先解除粘住，目标随后由前台事件 / Dock 通知自然接管
                s.targetSticky = false;
            } else if (fg == s.targetHwnd) {
                // 目标被恢复（win+↑ / 点击任务栏）
                s.targetSticky = false;
                ApplyTargetInfo(s, s.targetHwnd);
            } else {
                // 系统自动转移（最小化目标导致前台跳到别的窗口）：保持目标
                s.targetMaximized = IsZoomed(s.targetHwnd) != FALSE;
            }
            return;
        }
    }

    // 非粘住：正常跟随前台窗口；顺带清掉历史显式选择信号
    g_lastClickWindow = nullptr;
    g_altTabPressed = false;
    g_suiteClickSwitch = false;

    if (IsControlTarget(fg, s.hwnd)) {
        ApplyTargetInfo(s, fg);
        s.targetSticky = false;
    } else {
        // 前台是顶栏自身：忽略（顶栏 WS_EX_NOACTIVATE 本不应成为前台，
        // 若发生不应因此清掉目标）
        if (fg == s.hwnd) {
            return;
        }
        // 前台窗口激活瞬间可能尚未可见（新建窗口创建中）：
        // 保持当前目标，由 fg 事件处理里的延迟定时器在可见后复查，
        // 避免"右键新建资源管理器窗口"等场景误清目标导致标签全丢
        if (fg && IsWindow(fg) && !IsWindowVisible(fg)) {
            return;
        }
        // 前台是"任务栏中看不见"的窗口（托盘应用的启动器/工具窗、子进程
        // 消息窗、cloaked 窗，如 hotspot 弹出窗）：顶栏保持原目标与原层级
        // 完全不动。它既不是任务栏级的应用窗口（没有标签可显示），也不该
        // 触发层级重估 —— 否则这种临时弹窗一冒出来，顶栏就会从"被最大化
        // 窗口遮住"的让位态顶回最前，凭空弹出一条空标签顶栏。
        // 原目标已失效（应用已退出）时不能死守，继续走下面的清空逻辑。
        const bool targetAlive =
            s.hasTarget && s.targetHwnd && IsWindow(s.targetHwnd);
        if (targetAlive && fg && IsWindow(fg) && !IsTaskbarWindow(fg)) {
            return;
        }
        const bool hadTarget = s.hasTarget || s.targetHwnd != nullptr;
        s.targetHwnd = nullptr;
        s.hasTarget = false;
        s.targetSticky = false;
        s.targetMaximized = false;
        s.tabs.clear();
        s.hoverButton = kHitNone;
        s.hoverTab = -1;
        if (s.volumeTargetHwnd != nullptr) {
            s.volumeTargetHwnd = nullptr;
            ResolveVolumeSession(s);
        }
        // 没有聚焦目标 = 无需让位：退回覆盖态（置顶），并退订移动事件
        RetargetLocationHook(nullptr);
        ApplyBarLayer(s, false);
        if (hadTarget) NotifyDockTargetChanged(nullptr);
    }
}

// 按钮点击动作
void HitButton(HWND self, HWND target, ButtonHit hit) {
    if (target == self || !IsWindow(target)) {
        return;
    }
    switch (hit) {
    case kHitMinimize:
        ShowWindow(target, SW_MINIMIZE);
        break;
    case kHitMaximize:
        if (IsIconic(target)) {
            // 最小化时先还原显示（窗口化），不直接最大化；
            // 再次点击才执行最大化/还原切换
            ShowWindow(target, SW_RESTORE);
        } else if (IsZoomed(target)) {
            ShowWindow(target, SW_RESTORE);
        } else {
            ShowWindow(target, SW_MAXIMIZE);
        }
        break;
    case kHitClose:
        // 标准优雅关闭消息，与 Alt+F4 一致
        PostMessageW(target, WM_CLOSE, 0, 0);
        break;
    default:
        break;
    }
}

// ---- 双击顶栏：让目标窗口"填充顶栏以外区域" ----
//
// 与右侧最大化键的系统最大化（SW_MAXIMIZE）不同：窗口只填满顶栏之外的
// 屏幕区域——上缘压在顶栏下缘，顶栏本身始终可见（类 macOS 的"缩放"）。
// 因为是顶栏自己摆的窗口，IsZoomed() 不会为真，所以填充前的位置由
// AppState::filledWindows 自持，再次双击据此还原。

// 目标窗口的"填充"矩形（屏幕坐标）：目标所在显示器的工作区，
// 同屏时上缘让位给顶栏（顶栏下缘 = 可见区域上缘）。
RECT FillRectFor(HWND bar, HWND target) {
    RECT r{};
    const HMONITOR targetMon = MonitorFromWindow(target, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (targetMon != nullptr && GetMonitorInfoW(targetMon, &mi)) {
        r = mi.rcWork;  // 与系统最大化一致：铺满工作区（任务栏/Dock 之外）
    } else {
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &r, 0);
    }
    RECT barRc{};
    const HMONITOR barMon =
        (bar && IsWindow(bar)) ? MonitorFromWindow(bar, MONITOR_DEFAULTTONEAREST)
                               : nullptr;
    // 仅当目标与顶栏同屏时才给顶栏让位；窗口在副屏时顶栏不在那里，
    // 按该屏工作区铺满即可
    if (bar && IsWindow(bar) && barMon == targetMon &&
        GetWindowRect(bar, &barRc) && barRc.bottom > r.top) {
        r.top = barRc.bottom;
    }
    return r;
}

// 目标窗口当前是否处于填充态（窗口已销毁/已被系统最大化都视为否）
bool TargetIsFilled(const AppState& s, HWND target) {
    return target != nullptr && IsWindow(target) && !IsZoomed(target) &&
           s.filledWindows.find(target) != s.filledWindows.end();
}

// 把"想让窗口可见边框占据的矩形"换算成要传给 SetWindowPos 的窗口矩形。
//
// Win10+ 标准边框窗口的窗口矩形在左/右/下三边比可见边框多出一圈不可见边缘
// （阴影/缩放边框，本机 150% DPI 下量得 9px）。直接按工作区设置窗口矩形，
// 可见边框就会三边内缩、四周露出桌面背景（这正是"自己摆窗口"与系统最大化
// 的差别：系统最大化会自动把这圈边缘溢出到屏幕外）。这里用 DWM 的扩展边框
// （可见边框）反推内缩量补偿回去；自定义边框窗口（如本套件组件、无边框窗口）
// 差值为 0，取不到 DWM 数据时按原样返回。
RECT InflateForInvisibleFrame(HWND target, const RECT& visible) {
    RECT wr{};
    RECT ext{};
    if (!GetWindowRect(target, &wr) ||
        FAILED(DwmGetWindowAttribute(target, DWMWA_EXTENDED_FRAME_BOUNDS, &ext,
                                     sizeof(ext)))) {
        return visible;
    }
    RECT out = visible;
    out.left -= (std::max)(0L, ext.left - wr.left);
    out.top -= (std::max)(0L, ext.top - wr.top);
    out.right += (std::max)(0L, wr.right - ext.right);
    out.bottom += (std::max)(0L, wr.bottom - ext.bottom);
    return out;
}

// 进入填充：先记住当前位置，再把窗口摆到"顶栏以外"的区域。
// 若目标是系统最大化状态：还原时仍回到最大化之前的普通位置（而不是又弹回
// 最大化），故保存的 showCmd 一律归为 SW_SHOWNORMAL。
void FillTarget(AppState& s, HWND target) {
    WINDOWPLACEMENT wp{};
    wp.length = sizeof(wp);
    if (!GetWindowPlacement(target, &wp)) {
        return;
    }
    wp.showCmd = SW_SHOWNORMAL;
    s.filledWindows[target] = wp;

    if (IsZoomed(target)) {
        ShowWindow(target, SW_RESTORE);  // 先退出系统最大化，否则 SetWindowPos 无效
    }
    RECT r = FillRectFor(s.hwnd, target);
    r = InflateForInvisibleFrame(target, r);  // 可见边框正好贴住目标区域
    SetWindowPos(target, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top,
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

// 退出填充：还原到双击前的位置
void UnfillTarget(AppState& s, HWND target) {
    const auto it = s.filledWindows.find(target);
    if (it == s.filledWindows.end()) {
        return;
    }
    WINDOWPLACEMENT wp = it->second;
    wp.showCmd = SW_SHOWNORMAL;
    s.filledWindows.erase(it);
    if (target && IsWindow(target)) {
        SetWindowPlacement(target, &wp);
    }
}

// 双击顶栏空白处：填充 ↔ 还原
void ToggleFillTarget(AppState& s, HWND target) {
    if (!target || !IsWindow(target)) {
        return;
    }
    // 顺手清理已销毁窗口的陈旧记录（数量极少，代价可忽略）
    for (auto it = s.filledWindows.begin(); it != s.filledWindows.end();) {
        if (IsWindow(it->first)) {
            ++it;
        } else {
            it = s.filledWindows.erase(it);
        }
    }

    if (IsIconic(target)) {
        // 最小化：只还原显示（保持既有"先还原不直接缩放"的语义），
        // 若它原本是填充态，SW_RESTORE 会回到填充矩形，再次双击才还原
        ShowWindow(target, SW_RESTORE);
        return;
    }
    if (TargetIsFilled(s, target)) {
        UnfillTarget(s, target);
    } else {
        FillTarget(s, target);
    }
}

int HitTestTab(AppState& s, int x, int y) {
    if (y < 0 || y >= BarAreaHeight(s) || x < 0 || x >= s.width) {
        return -1;
    }
    for (size_t i = 0; i < s.tabs.size(); ++i) {
        const RectF& r = s.tabs[i].rect;
        if (x >= static_cast<int>(r.X) && x < static_cast<int>(r.X + r.Width)) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// 强制把窗口带到前台：附加到前台线程输入队列后依次执行
// BringWindowToTop / SetForegroundWindow / SetActiveWindow / SetFocus。
// SetActiveWindow / SetFocus 跨进程调用必须依赖输入队列附加，否则直接失败；
// 顶栏自身是 WS_EX_NOACTIVATE 背景窗口，单独调 SetForegroundWindow
// 很容易被系统前台锁拦截。全程不做按键模拟。
//
// 准入前置条件（唯一的聚焦闸门）：任务栏中看不见的窗口一律不聚焦 ——
// 隐藏窗/工具窗被 SetForegroundWindow 激活时会"现身"，等于把它打开。
// 列表侧已由 IsTaskbarWindow 过滤，这里是兜底：标签列表可能残留
// 枚举之后才隐藏/被藏起来的窗口，此时聚焦请求直接丢弃。
// 返回窗口是否已成为前台窗口。
bool ForceForegroundWindow(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) {
        return false;
    }
    if (!IsTaskbarWindow(hwnd)) {
        return false;
    }
    if (IsIconic(hwnd)) {
        ShowWindow(hwnd, SW_RESTORE);
    }
    const DWORD curThread = GetCurrentThreadId();
    const HWND fg = GetForegroundWindow();
    const DWORD fgThread = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
    const bool attached = fgThread && fgThread != curThread &&
                          AttachThreadInput(curThread, fgThread, TRUE) != FALSE;
    BringWindowToTop(hwnd);
    SetForegroundWindow(hwnd);
    if (attached) {
        // 附加后目标窗口与当前线程共享输入队列，跨进程激活/聚焦才被允许
        SetActiveWindow(hwnd);
        if (IsWindow(hwnd)) {
            SetFocus(hwnd);
        }
        AttachThreadInput(curThread, fgThread, FALSE);
    }
    return GetForegroundWindow() == hwnd;
}

// 右键标签：启动该应用的新进程/新窗口，并记录待插入位置
std::wstring GetProcessPath(DWORD pid);  // 定义在下方
void OpenNewAppWindow(AppState& s, HWND tabHwnd) {
    DWORD pid = 0;
    GetWindowThreadProcessId(tabHwnd, &pid);
    if (pid == 0) {
        return;
    }
    const std::wstring path = GetProcessPath(pid);
    if (path.empty()) {
        return;
    }

    // 顶栏是 WS_EX_NOACTIVATE 背景窗口，平时没有设置前台的权限，
    // 直接 AllowSetForegroundWindow 不会生效（调用者必须自己能设置前台）。
    // 先附加到前台线程输入队列，使本线程被视为前台进程，
    // 再授权后，资源管理器等由 Shell 代建窗口的系统应用才能把新窗口带到前台。
    const DWORD curThread = GetCurrentThreadId();
    HWND fg = GetForegroundWindow();
    const DWORD fgThread = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
    const bool attached = fgThread && fgThread != curThread &&
                          AttachThreadInput(curThread, fgThread, TRUE) != FALSE;
    AllowSetForegroundWindow(ASFW_ANY);
    // 用 ShellExecute 启动，交给 Shell 处理前台/焦点
    HINSTANCE result = ShellExecuteW(nullptr, L"open", path.c_str(),
                                     nullptr, nullptr, SW_SHOWNORMAL);
    if (attached) {
        AttachThreadInput(curThread, fgThread, FALSE);
    }
    if (reinterpret_cast<INT_PTR>(result) > 32) {
        s.insertAfterTab = tabHwnd;
        s.insertPending = true;
        s.insertPendingSince = GetTickCount64();
        // 立即启动挂起态短轮询：新窗口可能出现得较慢，不依赖事件
        SetTimer(s.hwnd, kTabRefreshTimerId, kPendingPollMs, nullptr);
    }
}

ButtonHit HitTestButton(AppState& s, int x, int y) {
    if (y < 0 || y >= s.height || x < 0 || x >= s.width) {
        return kHitNone;
    }

    // 左上角音量按钮（始终可用，展开/收起面板）
    RectF volR;
    VolumeButtonRect(s, volR);
    if (x >= static_cast<int>(volR.X) && x < static_cast<int>(volR.X + volR.Width) &&
        y < BarAreaHeight(s)) {
        return kHitVolume;
    }

    // Chrome 风格标签区域
    if (HitTestTab(s, x, y) >= 0) {
        return kHitTab;
    }

    // 最小化键左侧的 Win11 风格时钟区域
    // （左键 = 系统快速设置面板，右键 = 系统日历浮出窗口，见各点击分支）
    RectF clockR;
    ComputeClockRect(s, clockR);
    if (x >= static_cast<int>(clockR.X) &&
        x < static_cast<int>(clockR.X + clockR.Width)) {
        return kHitClock;
    }

    RectF minR, maxR, closeR;
    ComputeButtonRects(s, minR, maxR, closeR);
    if (x >= static_cast<int>(minR.X) && x < static_cast<int>(minR.X + minR.Width)) {
        return kHitMinimize;
    }
    if (x >= static_cast<int>(maxR.X) && x < static_cast<int>(maxR.X + maxR.Width)) {
        return kHitMaximize;
    }
    if (x >= static_cast<int>(closeR.X) && x < static_cast<int>(closeR.X + closeR.Width)) {
        return kHitClose;
    }
    return kHitNone;
}

// ---- 音量面板几何 ----

// 顶栏基础高度
int BarHeight(AppState& s) {
    return MulDiv(kBaseTabHeight, s.dpi, 96);
}

// 音量面板上方的顶栏高度；音量按钮位于此区域左上角
int BarAreaHeight(AppState& s) {
    return BarHeight(s);
}

// 当前窗口总高：音量面板是独立窗口，顶栏高度始终等于 Chrome 标签栏高度
int WindowHeight(AppState& s) {
    return BarHeight(s);
}

void VolumeButtonRect(AppState& s, RectF& r) {
    const float k = s.scale;
    // 从窗口最左缘(x=0)开始，贴合左上角，避免屏幕最左边触发不到
    r.X = 0;
    r.Y = 0;
    r.Width = kVolumeButtonW * k;
    r.Height = static_cast<float>(BarAreaHeight(s));
}

// 面板内布局：静音按钮（左）+ 滑条（主体）
void VolumePanelInner(AppState& s, RectF& panel, RectF& mute, RectF& track) {
    const float k = s.scale;
    const float pad = 6.0f * k;
    mute.X = panel.X + pad;
    mute.Y = panel.Y + (panel.Height - kMuteButtonW * k) * 0.5f;
    mute.Width = kMuteButtonW * k;
    mute.Height = kMuteButtonW * k;

    const float trackX0 = mute.X + mute.Width + pad;
    // 右侧预留百分比数字显示空间（约 60px），避免轨道与百分比重合/贴边
    const float percentReserve = 60.0f * k;
    const float trackX1 = panel.X + panel.Width - pad - percentReserve;
    track.X = trackX0;
    track.Y = panel.Y + panel.Height * 0.5f - 2.0f * k;
    track.Width = trackX1 - trackX0;
    track.Height = 4.0f * k;
}

// ---- Core Audio：按进程设置应用音量（类似 Windows 音量合成器）----

// 判断 sessionPid 是否属于以 rootPid 为根的进程树（rootPid 本身或其后代）
bool ProcessTreeContains(DWORD rootPid, DWORD sessionPid) {
    if (rootPid == sessionPid) {
        return true;
    }

    std::unordered_map<DWORD, DWORD> parentMap;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        return false;
    }

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            parentMap[pe.th32ProcessID] = pe.th32ParentProcessID;
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);

    // 沿父链向上走，直到找到 rootPid
    DWORD cur = sessionPid;
    for (int i = 0; i < 128; ++i) {
        if (cur == rootPid) {
            return true;
        }
        auto it = parentMap.find(cur);
        if (it == parentMap.end()) {
            return false;
        }
        const DWORD parent = it->second;
        if (parent == cur || parent == 0) {
            return false;
        }
        cur = parent;
    }
    return false;
}

// ---- Chrome 标签同步：WebSocket 服务端 ----
//
// 顶栏内置一个 127.0.0.1:kChromeSyncPort 的 WebSocket 服务端。
// chrome-tab-sync 扩展（MV3）连接后推送当前聚焦 Chrome 窗口的标签列表与
// 增量事件；顶栏向扩展发送 激活/关闭/新建标签 命令。
// 网络工作在线程（g_chromeServer.thread），消息经 PostMessage 回到 UI 线程；
// 命令发送由 sendMutex 串行化。

std::wstring GetProcessName(DWORD pid);  // 定义在下方（同应用窗口枚举节）

struct ChromeSyncServer {
    HWND notifyHwnd = nullptr;
    SOCKET listenSock = INVALID_SOCKET;
    SOCKET clientSock = INVALID_SOCKET;  // 由 sendMutex 保护
    std::mutex sendMutex;
    std::thread thread;
    std::atomic<bool> stop{false};
    std::atomic<bool> connected{false};
    std::string recvBuf;        // 客户端数据缓冲（握手残留 + 帧流）
    std::string pendingText;    // 分片文本消息累积
    int idleTimeouts = 0;       // 连续接收超时计数（心跳探活）
};

ChromeSyncServer g_chromeServer;

// 判断目标窗口是否属于 Chrome/Edge（Chromium）进程
bool IsChromeTarget(HWND hwnd) {
    if (!hwnd) {
        return false;
    }
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0) {
        return false;
    }
    std::wstring name = GetProcessName(pid);
    for (auto& ch : name) {
        ch = static_cast<wchar_t>(towlower(ch));
    }
    return name == L"chrome.exe" || name == L"msedge.exe";
}

// 当前是否处于 Chrome 同步模式：扩展已连接 且 目标是 Chromium 窗口
bool ChromeSyncMode(AppState& s) {
    return s.chromeSync.connected && IsChromeTarget(s.targetHwnd);
}

// 向扩展发送一条 JSON 命令（UTF-8 文本帧）；未连接时返回 false
bool ChromeSyncSendJson(const std::string& utf8Json) {
    if (!g_chromeServer.connected.load()) {
        return false;
    }
    const std::string frame =
        wsproto::WsEncodeFrame(0x1, utf8Json, true);
    std::lock_guard<std::mutex> lock(g_chromeServer.sendMutex);
    if (g_chromeServer.clientSock == INVALID_SOCKET) {
        return false;
    }
    const int sent = send(g_chromeServer.clientSock, frame.data(),
                          static_cast<int>(frame.size()), 0);
    return sent == static_cast<int>(frame.size());
}

void ChromeSyncSendActivateTab(int tabId) {
    ChromeSyncSendJson(wsproto::ChromeSyncBuildCommand(L"activateTab", tabId));
}

void ChromeSyncSendCloseTab(int tabId) {
    ChromeSyncSendJson(wsproto::ChromeSyncBuildCommand(L"closeTab", tabId));
}

void ChromeSyncSendNewTab() {
    ChromeSyncSendJson(wsproto::ChromeSyncBuildCommand(L"newTab"));
}

// 关闭当前客户端连接（可跨线程调用；sendMutex 串行化与 send 的竞争）
void ChromeSyncCloseClient() {
    std::lock_guard<std::mutex> lock(g_chromeServer.sendMutex);
    if (g_chromeServer.clientSock != INVALID_SOCKET) {
        shutdown(g_chromeServer.clientSock, SD_BOTH);
        closesocket(g_chromeServer.clientSock);
        g_chromeServer.clientSock = INVALID_SOCKET;
    }
    g_chromeServer.recvBuf.clear();
    g_chromeServer.pendingText.clear();
    if (g_chromeServer.connected.exchange(false)) {
        PostMessageW(g_chromeServer.notifyHwnd, kChromeSyncStateMsg, 0, 0);
    }
}

// 发送一个原始 WS 帧（服务端线程内部使用，不经过 sendMutex）
bool ChromeSyncSendFrameRaw(int opcode, const std::string& payload) {
    if (g_chromeServer.clientSock == INVALID_SOCKET) {
        return false;
    }
    const std::string frame = wsproto::WsEncodeFrame(opcode, payload, true);
    const int sent = send(g_chromeServer.clientSock, frame.data(),
                          static_cast<int>(frame.size()), 0);
    return sent == static_cast<int>(frame.size());
}

// 大小写不敏感地在一段文本中查找子串
std::string::size_type ChromeSyncFindCI(const std::string& hay,
                                        const char* needle) {
    std::string low = hay;
    std::transform(low.begin(), low.end(), low.begin(),
                   [](unsigned char c) { return static_cast<char>(tolower(c)); });
    const std::string nl = needle;
    std::string lowN = nl;
    std::transform(lowN.begin(), lowN.end(), lowN.begin(),
                   [](unsigned char c) { return static_cast<char>(tolower(c)); });
    return low.find(lowN);
}

// WebSocket 握手：读取请求头，校验并回复 101
int ChromeSyncHandshake() {
    SOCKET c = g_chromeServer.clientSock;
    if (c == INVALID_SOCKET) {
        return -1;
    }
    // 握手阶段较短超时
    DWORD timeout = kChromeSyncHandshakeTimeoutMs;
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&timeout), sizeof(timeout));

    std::string req;
    char tmp[2048];
    while (req.find("\r\n\r\n") == std::string::npos &&
           req.size() < kChromeSyncMaxHeaderBytes) {
        const int r = recv(c, tmp, sizeof(tmp), 0);
        if (r <= 0) {
            return -1;
        }
        req.append(tmp, static_cast<size_t>(r));
    }
    const size_t headerEnd = req.find("\r\n\r\n");
    if (headerEnd == std::string::npos) {
        return -1;
    }

    // 提取 Sec-WebSocket-Key（Chrome 发送的头部大小写固定，仍做容错查找）
    std::string key;
    size_t pos = ChromeSyncFindCI(req, "Sec-WebSocket-Key:");
    if (pos == std::string::npos) {
        return -1;
    }
    pos += 19;
    const size_t lineEnd = req.find("\r\n", pos);
    if (lineEnd == std::string::npos) {
        return -1;
    }
    key = req.substr(pos, lineEnd - pos);
    // 去首尾空白
    size_t b = key.find_first_not_of(" \t");
    size_t e = key.find_last_not_of(" \t");
    if (b == std::string::npos || e == std::string::npos || e < b) {
        return -1;
    }
    key = key.substr(b, e - b + 1);

    const std::string accept = wsproto::ComputeWsAccept(key);
    const std::string resp =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: " +
        accept + "\r\n\r\n";

    size_t sent = 0;
    while (sent < resp.size()) {
        const int r = send(c, resp.data() + sent,
                           static_cast<int>(resp.size() - sent), 0);
        if (r <= 0) {
            return -1;
        }
        sent += static_cast<size_t>(r);
    }

    // 握手请求里可能已夹带首帧（极少见），保留到帧缓冲
    if (headerEnd + 4 < req.size()) {
        g_chromeServer.recvBuf = req.substr(headerEnd + 4);
    }

    // 恢复常规接收超时（探活用）
    timeout = kChromeSyncRecvTimeoutMs;
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    return 0;
}

// 把一条完整文本消息交给 UI 线程（堆上复制，UI 线程释放）
void ChromeSyncDeliverText(const std::string& utf8) {
    const std::wstring wide = wsproto::Utf8ToWide(utf8);
    if (wide.empty()) {
        return;
    }
    auto* copy = new wchar_t[wide.size() + 1];
    wcscpy_s(copy, wide.size() + 1, wide.c_str());
    PostMessageW(g_chromeServer.notifyHwnd, kChromeSyncMsg, 0,
                 reinterpret_cast<LPARAM>(copy));
}

// 顶栏 <-> 扩展的应用层心跳（保活 MV3 Service Worker，见常量注释）。
// 返回 true 表示该文本消息已被心跳处理，调用方不要再投递到 UI 线程。
bool ChromeSyncHandleKeepAlive(const std::string& utf8) {
    if (utf8 == kChromeSyncPingJson) {
        ChromeSyncSendFrameRaw(0x1, kChromeSyncPongJson);
        return true;
    }
    if (utf8 == kChromeSyncPongJson) {
        return true;
    }
    return false;
}

// 解析帧缓冲中的全部完整帧；返回 false 表示协议错误需断开
bool ChromeSyncProcessFrames() {
    while (!g_chromeServer.stop.load()) {
        size_t consumed = 0;
        wsproto::WsFrame frame;
        std::string err;
        const int rc = wsproto::WsDecodeFrame(g_chromeServer.recvBuf, consumed,
                                              frame, err);
        if (rc == 0) {
            return true;  // 数据不足，等更多数据
        }
        if (rc < 0) {
            return false;
        }
        g_chromeServer.recvBuf.erase(0, consumed);

        switch (frame.opcode) {
        case 0x1:  // 文本
            if (frame.fin) {
                // 心跳在帧层消化，不占用 UI 线程（避免无谓重绘/唤醒）
                if (!ChromeSyncHandleKeepAlive(frame.payload)) {
                    ChromeSyncDeliverText(frame.payload);
                }
            } else {
                g_chromeServer.pendingText += frame.payload;
            }
            break;
        case 0x0:  // 延续帧
            g_chromeServer.pendingText += frame.payload;
            if (frame.fin) {
                if (!ChromeSyncHandleKeepAlive(g_chromeServer.pendingText)) {
                    ChromeSyncDeliverText(g_chromeServer.pendingText);
                }
                g_chromeServer.pendingText.clear();
            }
            break;
        case 0x8:  // 关闭
            ChromeSyncSendFrameRaw(0x8, frame.payload);
            ChromeSyncCloseClient();
            return true;
        case 0x9:  // ping -> pong
            ChromeSyncSendFrameRaw(0xA, frame.payload);
            break;
        case 0xA:  // pong：忽略
            break;
        default:   // 二进制等：忽略
            break;
        }
    }
    return true;
}

void ChromeSyncServerThread() {
    while (!g_chromeServer.stop.load()) {
        if (g_chromeServer.clientSock == INVALID_SOCKET) {
            // 等待新连接（可被 stop / closesocket 打断）
            fd_set rf;
            FD_ZERO(&rf);
            FD_SET(g_chromeServer.listenSock, &rf);
            timeval tv{0, 500000};
            if (select(0, &rf, nullptr, nullptr, &tv) <= 0) {
                continue;
            }
            SOCKET c = accept(g_chromeServer.listenSock, nullptr, nullptr);
            if (c == INVALID_SOCKET) {
                continue;
            }
            {
                std::lock_guard<std::mutex> lock(g_chromeServer.sendMutex);
                if (g_chromeServer.stop.load()) {
                    closesocket(c);
                    break;
                }
                g_chromeServer.clientSock = c;
            }
            if (ChromeSyncHandshake() != 0) {
                ChromeSyncCloseClient();
                continue;
            }
            g_chromeServer.idleTimeouts = 0;
            g_chromeServer.connected.store(true);
            PostMessageW(g_chromeServer.notifyHwnd, kChromeSyncStateMsg, 1, 0);
            continue;
        }

        // 客户端循环
        char tmp[8192];
        const int r = recv(g_chromeServer.clientSock, tmp, sizeof(tmp), 0);
        if (r > 0) {
            g_chromeServer.recvBuf.append(tmp, static_cast<size_t>(r));
            g_chromeServer.idleTimeouts = 0;
            if (!ChromeSyncProcessFrames()) {
                ChromeSyncCloseClient();
            }
            continue;
        }
        if (r == 0) {
            ChromeSyncCloseClient();  // 对端关闭
            continue;
        }
        const int err = WSAGetLastError();
        if (err == WSAETIMEDOUT) {
            // 空闲探活：推应用层 JSON ping（扩展回 pong）。
            // 该消息是 MV3 Service Worker 保活的关键：JS 收到消息才会重置
            // 30s 空闲回收计时器，WS 协议层 ping 不经过 JS、无法保活。
            ChromeSyncSendFrameRaw(0x1, kChromeSyncPingJson);
            if (++g_chromeServer.idleTimeouts > kChromeSyncMaxIdleTimeouts) {
                ChromeSyncCloseClient();
            }
            continue;
        }
        if (err == WSAEWOULDBLOCK) {
            continue;
        }
        ChromeSyncCloseClient();
    }
}

// 启动服务端（幂等）：绑定 127.0.0.1:kChromeSyncPort 并启动监听线程。
// 端口被占用等失败情形只禁用该功能，不影响顶栏其余部分。
void ChromeSyncStart(HWND hwnd) {
    if (g_chromeServer.thread.joinable()) {
        return;
    }
    g_chromeServer.notifyHwnd = hwnd;

    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        return;
    }
    BOOL reuse = TRUE;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // 仅本机
    addr.sin_port = htons(static_cast<u_short>(kChromeSyncPort));
    if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        closesocket(s);
        return;
    }
    if (listen(s, 4) != 0) {
        closesocket(s);
        return;
    }
    g_chromeServer.listenSock = s;
    g_chromeServer.stop.store(false);
    g_chromeServer.thread = std::thread(ChromeSyncServerThread);
}

// 停止服务端并回收线程（WM_DESTROY 时调用；幂等）
void ChromeSyncStop() {
    g_chromeServer.stop.store(true);
    if (g_chromeServer.listenSock != INVALID_SOCKET) {
        closesocket(g_chromeServer.listenSock);
        g_chromeServer.listenSock = INVALID_SOCKET;
    }
    ChromeSyncCloseClient();
    if (g_chromeServer.thread.joinable()) {
        g_chromeServer.thread.join();
    }
}

// ---- Chrome 风格标签：同应用窗口枚举 / 布局 ----

std::wstring GetWindowTitleText(HWND hwnd) {
    wchar_t buf[256] = {};
    const int n = GetWindowTextW(hwnd, buf, 255);
    if (n > 0) {
        return std::wstring(buf, n);
    }
    wchar_t cls[64] = {};
    if (GetClassNameW(hwnd, cls, 63) > 0) {
        return std::wstring(cls);
    }
    return L"窗口";
}

std::wstring GetProcessName(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) {
        return L"";
    }
    wchar_t path[MAX_PATH] = {};
    DWORD size = MAX_PATH;
    std::wstring name;
    if (QueryFullProcessImageNameW(h, 0, path, &size)) {
        wchar_t* slash = wcsrchr(path, L'\\');
        name = slash ? (slash + 1) : path;
    }
    CloseHandle(h);
    return name;
}

std::wstring GetProcessPath(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) {
        return L"";
    }
    wchar_t path[MAX_PATH] = {};
    DWORD size = MAX_PATH;
    if (QueryFullProcessImageNameW(h, 0, path, &size)) {
        CloseHandle(h);
        return std::wstring(path);
    }
    CloseHandle(h);
    return L"";
}

// 判断两个顶层窗口是否属于同一个"应用"：
// 同进程，或同可执行文件名（覆盖多进程/多窗口应用）。
// 注意不能使用进程树判断：Explorer 启动的子进程会把资源管理器和其他应用误判为同一应用。
bool IsSameApplication(HWND candidate, HWND target) {
    DWORD candPid = 0;
    DWORD targetPid = 0;
    GetWindowThreadProcessId(candidate, &candPid);
    GetWindowThreadProcessId(target, &targetPid);
    if (candPid == 0 || targetPid == 0) {
        return false;
    }
    if (candPid == targetPid) {
        return true;
    }
    const std::wstring candName = GetProcessName(candPid);
    const std::wstring targetName = GetProcessName(targetPid);
    return !candName.empty() && candName == targetName;
}

bool IsShellSystemWindow(HWND hwnd) {
    wchar_t cls[64] = {};
    if (GetClassNameW(hwnd, cls, 63) <= 0) {
        return false;
    }
    return wcscmp(cls, L"Progman") == 0 ||
           wcscmp(cls, L"WorkerW") == 0 ||
           wcscmp(cls, L"SHELLDLL_DefView") == 0 ||
           wcscmp(cls, L"Shell_TrayWnd") == 0 ||
           wcscmp(cls, L"Shell_SecondaryTrayWnd") == 0;
}

// 该窗口是否算目标"应用"的一张任务栏窗口（标签枚举与最小化判定共用）
bool IsAppWindowCandidate(HWND hwnd, HWND self, HWND target) {
    if (!hwnd || hwnd == self || !target) {
        return false;
    }
    if (!IsTaskbarWindow(hwnd)) {
        return false;
    }
    // 忽略桌面 / 任务栏等系统 Shell 窗口，避免资源管理器标签混入系统窗口
    if (IsShellSystemWindow(hwnd)) {
        return false;
    }
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0 || pid == GetCurrentProcessId()) {
        return false;
    }
    return IsSameApplication(hwnd, target);
}

struct EnumAppWindowsContext {
    HWND self = nullptr;
    HWND target = nullptr;
    std::vector<HWND>* windows = nullptr;
};

BOOL CALLBACK EnumAppWindowListProc(HWND hwnd, LPARAM lParam) {
    auto* ctx = reinterpret_cast<EnumAppWindowsContext*>(lParam);
    if (!ctx || !ctx->windows) {
        return TRUE;
    }
    if (!IsAppWindowCandidate(hwnd, ctx->self, ctx->target)) {
        return TRUE;
    }
    ctx->windows->push_back(hwnd);
    return TRUE;
}

// 目标应用是否还有"看得见、没被最小化"的窗口。
//
// 用于区分"最小化该应用的一张窗口"与"最小化该应用的全部窗口"：
// 前者保持顶栏粘住（不跟随系统自动转移的前台，避免"最小化 A 却操作 B"）；
// 后者应用已经没有可展示的窗口，粘住毫无意义 —— 顶栏应改跟 Windows
// 当前聚焦的窗口。
bool AppHasVisibleWindow(HWND self, HWND target) {
    std::vector<HWND> windows;
    EnumAppWindowsContext ctx;
    ctx.self = self;
    ctx.target = target;
    ctx.windows = &windows;
    EnumWindows(EnumAppWindowListProc, reinterpret_cast<LPARAM>(&ctx));
    for (HWND w : windows) {
        if (IsWindow(w) && !IsIconic(w)) {
            return true;
        }
    }
    return false;
}

struct EnumTabsContext {
    HWND self = nullptr;
    HWND target = nullptr;
    std::vector<TabInfo>* tabs = nullptr;
};

BOOL CALLBACK EnumAppWindowsProc(HWND hwnd, LPARAM lParam) {
    auto* ctx = reinterpret_cast<EnumTabsContext*>(lParam);
    if (!ctx || !ctx->tabs) {
        return TRUE;
    }
    if (!IsAppWindowCandidate(hwnd, ctx->self, ctx->target)) {
        return TRUE;
    }
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);

    TabInfo info;
    info.hwnd = hwnd;
    info.pid = pid;
    info.title = GetWindowTitleText(hwnd);
    ctx->tabs->push_back(info);
    return TRUE;
}

// 标签条可用区间：音量键右侧 6px 到时钟左侧 6px（LayoutTabs 与拖动重排共用，
// 保证"浮起的标签"和正常布局落在同一段可用宽度里）
void TabStripRange(AppState& s, float& left, float& right) {
    const float k = s.scale;
    RectF volR;
    VolumeButtonRect(s, volR);
    RectF clockR;
    ComputeClockRect(s, clockR);
    left = volR.X + volR.Width + 6.0f * k;
    // 标签区右边界让位给最小化键左侧的时钟区域
    right = clockR.X - 6.0f * k;
}

// 拖动让位的允许区间（Chrome 固定标签恒在最前）：
//   被拖的是普通标签 -> 只能落在固定区之后；
//   被拖的是固定标签 -> 只能在固定区内移动；
//   窗口枚举模式（无固定标签概念）-> 整段都行。
int TabDragLo(AppState& s, bool draggingPinned) {
    if (!ChromeSyncMode(s) || draggingPinned) {
        return 0;
    }
    int pinned = 0;
    for (const auto& t : s.tabs) {
        if (t.isChrome && t.chromePinned) {
            ++pinned;
        } else {
            break;
        }
    }
    return pinned;
}

int TabDragHi(AppState& s, bool draggingPinned) {
    if (!ChromeSyncMode(s) || !draggingPinned) {
        return -1;  // -1 = 不限制（取列表末尾）
    }
    int pinned = 0;
    for (const auto& t : s.tabs) {
        if (t.isChrome && t.chromePinned) {
            ++pinned;
        } else {
            break;
        }
    }
    return pinned - 1;
}

// 结束拖动重排状态（不改顺序；顺序在拖动过程中已实时生效）
void CancelTabDrag(AppState& s) {
    s.tabPressIndex = -1;
    s.tabDragging = false;
    s.tabDragIndex = -1;
    s.tabDragOriginIndex = -1;
    s.tabDragChromeId = 0;
    s.tabDragHwnd = nullptr;
    s.tabDragGrabDx = 0.0f;
    s.tabDragBodyX = 0.0f;
    s.tabDragAnchorX = 0.0f;
}

// 松手落定：把拖动后的顺序记到顶栏自己这边（**不写回浏览器**）。
//   Chrome 同步模式：顶栏顺序与浏览器里的真实标签顺序解绑 —— 这里只把
//     新顺序写回同步模型的本地顺序（顺序即位置，index 跟着重编号），
//     这样随后的单条 tabUpdated 或同窗口全量快照都不会把用户排好的顺序
//     按浏览器的 index 重排。不下发 moveTab，浏览器的标签顺序保持不动。
//   窗口枚举模式：s.tabs 的顺序本身就是展示顺序，RefreshTabs 会保留
//     既有相对顺序（见其中的 ordered 重建），无需额外持久化。
void CommitTabReorder(AppState& s, int movedIndex) {
    if (!ChromeSyncMode(s) || movedIndex < 0 ||
        movedIndex >= static_cast<int>(s.tabs.size())) {
        return;
    }
    if (!s.tabs[movedIndex].isChrome) {
        return;
    }

    // 顶栏展示顺序 -> 同步模型的本地顺序（只改顶栏这边）
    std::vector<int> order;
    order.reserve(s.tabs.size());
    for (const auto& t : s.tabs) {
        if (t.isChrome) {
            order.push_back(t.chromeTabId);
        }
    }
    wsproto::ChromeSyncApplyOrder(s.chromeSync, order);
}

// 根据当前宽度计算每个标签的矩形（从音量键右侧到右侧三键左侧）。
// 所有标签始终等宽，不随标题长度变化：
//   - 空间充裕（统一宽度放得下所有标签）时，全部使用统一宽度；
//   - 标签多到放不下时，每个标签同时缩小相同程度，
//     总长度恰好铺满标签区（每格 = 可用宽度 / 标签数），
//     即整体缩小时正好是容纳所有标签的最大宽度。
// measureG 保留仅为兼容调用方（已不再需要测量标题）。
void LayoutTabs(AppState& s, std::vector<TabInfo>& tabs,
                Graphics* measureG = nullptr) {
    (void)measureG;
    if (tabs.empty()) {
        return;
    }
    const float k = s.scale;

    float left = 0.0f;
    float right = 0.0f;
    TabStripRange(s, left, right);
    const float available = right - left;
    if (available <= 0.0f) {
        return;
    }

    // 空间充裕取统一宽度；不足则等分铺满（每个标签缩小相同程度）
    const float w = (std::min)(kUniformTabWidth * k,
                               available / static_cast<float>(tabs.size()));
    float x = left;
    for (auto& tab : tabs) {
        tab.rect.X = x;
        tab.rect.Y = 0.0f;
        tab.rect.Width = w;
        tab.rect.Height = static_cast<float>(BarAreaHeight(s));
        x += w;
    }
}

// 重新枚举当前目标应用的全部顶层窗口并刷新标签列表。
// 返回 true 表示列表内容有变化（供定时器决定是否需要重绘）。
bool RefreshTabs(AppState& s) {
    const bool chromeMode = ChromeSyncMode(s);
    std::vector<TabInfo> next;
    if (s.hasTarget && s.targetHwnd && IsWindow(s.targetHwnd)) {
        if (chromeMode) {
            // Chrome 同步模式：标签直接来自扩展推送的浏览器标签页，
            // 顺序即浏览器内顺序（扩展按 index 排序维护）
            next.reserve(s.chromeSync.tabs.size());
            for (const auto& ct : s.chromeSync.tabs) {
                TabInfo ti;
                ti.isChrome = true;
                ti.chromeTabId = ct.id;
                ti.chromeActive = ct.active;
                ti.chromePinned = ct.pinned;
                ti.title = ct.title;
                if (ti.title.empty()) {
                    ti.title = ct.url;
                }
                if (ti.title.empty()) {
                    ti.title = L"新标签页";
                }
                next.push_back(std::move(ti));
            }
        } else {
            EnumTabsContext ctx;
            ctx.self = s.hwnd;
            ctx.target = s.targetHwnd;
            ctx.tabs = &next;
            EnumWindows(EnumAppWindowsProc, reinterpret_cast<LPARAM>(&ctx));
        }
    }

    // 保持已有标签顺序：切换聚焦/置顶不会让窗口跳到第一位，新窗口追加到末尾。
    // EnumWindows 返回的是 Z 序，直接用会让聚焦窗口总排在第一。
    // （Chrome 同步模式的顺序由扩展维护，跳过重排）
    bool hasCommonWindow = false;
    if (!chromeMode && !s.tabs.empty() && !next.empty()) {
        for (const auto& old : s.tabs) {
            for (const auto& n : next) {
                if (old.hwnd == n.hwnd) {
                    hasCommonWindow = true;
                    break;
                }
            }
            if (hasCommonWindow) {
                break;
            }
        }
    }
    if (!hasCommonWindow && !next.empty()) {
        // 首次遇到该应用（或刚从别的应用切换过来）时按 Z 序倒序排列，
        // 近似“打开顺序”，避免聚焦窗口固定第一。
        // （Chrome 同步模式同样跳过：扩展已按浏览器顺序给出）
        if (!chromeMode) {
            std::reverse(next.begin(), next.end());
        }
    }
    if (!chromeMode && !s.tabs.empty() && !next.empty()) {
        std::vector<TabInfo> ordered;
        ordered.reserve(next.size());
        std::vector<bool> used(next.size(), false);
        for (const auto& old : s.tabs) {
            for (size_t i = 0; i < next.size(); ++i) {
                if (!used[i] && next[i].hwnd == old.hwnd) {
                    ordered.push_back(next[i]);
                    used[i] = true;
                    break;
                }
            }
        }

        // 右键新建窗口的待插入：新出现的标签插入到指定标签右侧
        if (s.insertAfterTab && s.insertPending) {
            bool foundAnchor = false;
            bool inserted = false;
            HWND newHwnd = nullptr;
            for (size_t pos = 0; pos < ordered.size(); ++pos) {
                if (ordered[pos].hwnd == s.insertAfterTab) {
                    foundAnchor = true;
                    for (size_t i = 0; i < next.size(); ++i) {
                        if (!used[i]) {
                            newHwnd = next[i].hwnd;
                            ordered.insert(ordered.begin() + pos + 1, next[i]);
                            used[i] = true;
                            inserted = true;
                            break;
                        }
                    }
                    break;
                }
            }
            if (inserted && newHwnd) {
                // 新窗口出现后主动抢前台，解决 ShellExecute 打开后无法聚焦的问题；
                // 单次尝试可能撞上前台锁 / 窗口初始化等瞬时状态，
                // 因此挂起重试，直到窗口真正成为前台、用户切走或达到上限
                s.pendingFocusHwnd = newHwnd;
                s.pendingFocusOriginHwnd = GetForegroundWindow();
                s.pendingFocusAttempts = 0;
                ForceForegroundWindow(newHwnd);
            }
            if (!foundAnchor || inserted) {
                s.insertAfterTab = nullptr;
                s.insertPending = false;
            }
        }

        for (size_t i = 0; i < next.size(); ++i) {
            if (!used[i]) {
                ordered.push_back(next[i]);
            }
        }
        next.swap(ordered);
    }

    // 拖动重排进行中：让这次枚举结果跟随顶栏当前的本地顺序。
    // 否则拖动途中扩展随便推一条 tabUpdated（标题变化等）就会重建列表，
    // 按浏览器旧顺序重排，用户拖到一半的顺序被打回原样。
    // 匹配按身份（Chrome tabId / 窗口句柄），不依赖索引。
    if (s.tabPressIndex >= 0 && !next.empty() && !s.tabs.empty()) {
        std::vector<TabInfo> ordered;
        ordered.reserve(next.size());
        std::vector<bool> used(next.size(), false);
        for (const auto& old : s.tabs) {
            for (size_t i = 0; i < next.size(); ++i) {
                if (used[i]) {
                    continue;
                }
                const bool same =
                    old.isChrome
                        ? (next[i].isChrome &&
                           next[i].chromeTabId == old.chromeTabId)
                        : (!next[i].isChrome && next[i].hwnd == old.hwnd);
                if (same) {
                    ordered.push_back(next[i]);
                    used[i] = true;
                    break;
                }
            }
        }
        for (size_t i = 0; i < next.size(); ++i) {
            if (!used[i]) {
                ordered.push_back(next[i]);
            }
        }
        next.swap(ordered);
    }

    // 右键新建窗口的聚焦重试：新窗口出现后可能因前台锁 / 初始化时序
    // 第一次抢前台失败，随标签刷新定时器（约 1s）重试。
    // 窗口消失、已成为前台、用户已切到其他应用或达到次数上限即停止。
    if (s.pendingFocusHwnd) {
        const HWND fg = GetForegroundWindow();
        const bool done = !IsWindow(s.pendingFocusHwnd) || fg == s.pendingFocusHwnd;
        if (done || ++s.pendingFocusAttempts > kMaxPendingFocusAttempts) {
            s.pendingFocusHwnd = nullptr;
            s.pendingFocusOriginHwnd = nullptr;
            s.pendingFocusAttempts = 0;
        } else {
            // 用户已主动切到其他窗口（不是挂起时的前台窗口，
            // 也不是桌面/任务栏等系统窗口）：不再抢前台，避免打扰用户
            if (fg && fg != s.pendingFocusOriginHwnd &&
                !IsShellSystemWindow(fg)) {
                s.pendingFocusHwnd = nullptr;
                s.pendingFocusOriginHwnd = nullptr;
                s.pendingFocusAttempts = 0;
            } else {
                ForceForegroundWindow(s.pendingFocusHwnd);
            }
        }
    }

    if (s.bitmap) {
        Graphics g(s.bitmap);
        LayoutTabs(s, next, &g);
    } else {
        LayoutTabs(s, next, nullptr);
    }

    bool changed = s.tabs.size() != next.size();
    if (!changed) {
        for (size_t i = 0; i < next.size(); ++i) {
            const TabInfo& a = s.tabs[i];
            const TabInfo& b = next[i];
            if (a.isChrome != b.isChrome || a.hwnd != b.hwnd ||
                a.title != b.title ||
                (a.isChrome &&
                 (a.chromeTabId != b.chromeTabId ||
                  a.chromeActive != b.chromeActive ||
                  a.chromePinned != b.chromePinned))) {
                changed = true;
                break;
            }
        }
    }

    if (changed) {
        s.tabs.swap(next);
        if (s.hoverTab >= static_cast<int>(s.tabs.size())) {
            s.hoverTab = -1;
        }
        // 拖动重排中：列表被重建后按身份重新定位被拖标签，避免索引漂移；
        // 被拖的标签已经消失（被关掉等）时直接结束拖动
        if (s.tabPressIndex >= 0) {
            int found = -1;
            for (size_t i = 0; i < s.tabs.size(); ++i) {
                const TabInfo& t = s.tabs[i];
                const bool same =
                    s.tabDragIsChrome
                        ? (t.isChrome && t.chromeTabId == s.tabDragChromeId)
                        : (!t.isChrome && t.hwnd == s.tabDragHwnd);
                if (same) {
                    found = static_cast<int>(i);
                    break;
                }
            }
            if (found < 0) {
                CancelTabDrag(s);
                if (GetCapture() == s.hwnd) {
                    ReleaseCapture();
                }
            } else {
                s.tabPressIndex = found;
                s.tabDragIndex = found;
            }
        }
    }

    // 定时器状态维护：
    // - 挂起态（右键新建窗口待插入 / 抢前台重试）保持 200ms 短轮询，结束即停
    // - 有目标时保持 10s 低频兜底自检（一次性，每次刷新重置，
    //   事件驱动正常时基本不触发，仅防窗口事件丢失）；无目标时零轮询
    if (s.insertPending) {
        // 新窗口长时间未出现（启动失败等）：超时放弃，避免短轮询常驻
        if (s.insertPendingSince &&
            GetTickCount64() - s.insertPendingSince > 10000) {
            s.insertAfterTab = nullptr;
            s.insertPending = false;
        }
    }
    if (s.insertPending || s.pendingFocusHwnd) {
        SetTimer(s.hwnd, kTabRefreshTimerId, kPendingPollMs, nullptr);
    } else {
        KillTimer(s.hwnd, kTabRefreshTimerId);
    }
    if (s.hasTarget && s.targetHwnd && IsWindow(s.targetHwnd)) {
        SetTimer(s.hwnd, kSlowRefreshTimerId, kSlowRefreshMs, nullptr);
    } else {
        KillTimer(s.hwnd, kSlowRefreshTimerId);
    }
    return changed;
}

// 枚举默认渲染端点的音频会话，返回与目标进程匹配的会话。
// Chrome/Edge 等多进程应用的窗口属于主进程，但音频会话在渲染子进程，
// 因此匹配目标进程的整个进程树（自身 + 所有后代）。
ComPtr<ISimpleAudioVolume> FindProcessVolumeSession(DWORD pid) {
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                CLSCTX_ALL, IID_PPV_ARGS(&enumerator)))) {
        return nullptr;
    }

    ComPtr<IMMDevice> device;
    if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device))) {
        return nullptr;
    }

    ComPtr<IAudioSessionManager2> manager;
    void* managerRaw = nullptr;
    if (FAILED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL,
                                nullptr, &managerRaw))) {
        return nullptr;
    }
    manager.Attach(static_cast<IAudioSessionManager2*>(managerRaw));

    ComPtr<IAudioSessionEnumerator> sessionEnum;
    if (FAILED(manager->GetSessionEnumerator(&sessionEnum))) {
        return nullptr;
    }

    int count = 0;
    if (FAILED(sessionEnum->GetCount(&count))) {
        return nullptr;
    }

    for (int i = 0; i < count; ++i) {
        ComPtr<IAudioSessionControl> control;
        if (FAILED(sessionEnum->GetSession(i, &control))) {
            continue;
        }
        ComPtr<IAudioSessionControl2> ctrl2;
        if (FAILED(control->QueryInterface(IID_PPV_ARGS(&ctrl2)))) {
            continue;
        }
        DWORD sessionPid = 0;
        if (FAILED(ctrl2->GetProcessId(&sessionPid))) {
            continue;
        }
        if (ProcessTreeContains(pid, sessionPid)) {
            ComPtr<ISimpleAudioVolume> vol;
            if (SUCCEEDED(control->QueryInterface(IID_PPV_ARGS(&vol)))) {
                return vol;
            }
        }
    }
    return nullptr;
}

// 刷新音量会话：目标窗口变化时重新解析其进程的音频会话。
// 解析不到会话（应用当前没有声音）时，仅当音量面板展开才启动短间隔补解析轮询，
// 应用之后才开始出声（如浏览器开始播放视频）会自动被捕获；
// 面板关闭即停（零轮询），下次展开面板会立即再补解析一次。
void ResolveVolumeSession(AppState& s) {
    KillTimer(s.hwnd, kVolumeRetryTimerId);
    s.volumeSession.Reset();
    s.volumeSessionPid = 0;
    s.volumeReady = false;

    if (!s.hasTarget || !s.targetHwnd) {
        return;
    }
    DWORD pid = 0;
    GetWindowThreadProcessId(s.targetHwnd, &pid);
    if (pid == 0) {
        return;
    }

    ComPtr<ISimpleAudioVolume> vol = FindProcessVolumeSession(pid);
    if (!vol) {
        // 应用尚无音频会话：面板展开期间短间隔重试，等它出声
        if (s.volumeOpen) {
            SetTimer(s.hwnd, kVolumeRetryTimerId, kVolumeRetryMs, nullptr);
        }
        return;
    }

    s.volumeSession = vol;
    s.volumeSessionPid = pid;
    s.volumePid = pid;

    float v = 0.0f;
    BOOL muted = FALSE;
    vol->GetMasterVolume(&v);
    vol->GetMute(&muted);

    s.volumeValue = v;
    s.volumeMuted = muted != FALSE;
    s.volumeReady = true;
}

void ApplyVolume(AppState& s, float value) {
    if (!s.volumeSession) {
        return;
    }
    value = std::clamp(value, 0.0f, 1.0f);
    s.volumeValue = value;
    s.volumeSession->SetMasterVolume(value, nullptr);
}

void ToggleMute(AppState& s) {
    if (!s.volumeSession) {
        return;
    }
    s.volumeMuted = !s.volumeMuted;
    s.volumeSession->SetMute(BOOL(s.volumeMuted), nullptr);
}

// ---- 全局鼠标/键盘钩子 ----
// 用途：1) 点击面板外任意处（含桌面/其他窗口）时收起音量面板；
//      2) 记录用户"显式"点击 / Alt+Tab 切换窗口的信号，
//         供目标跟踪区分"系统自动转移焦点"与"用户主动切换窗口"。
constexpr UINT kCloseVolumeMsg = WM_APP + 2;
constexpr UINT kUserSwitchMsg = WM_APP + 3;  // 用户显式点击/Alt+Tab 其他窗口时通知刷新目标
HHOOK g_volumeHook = nullptr;
HWND g_volumeHookHwnd = nullptr;

// 左上角触发角（与 Dock 左下/右下角同规格：12×48 逻辑像素，贴屏幕左上缘）。
// 点击 = 展开/收起音量面板。该区域由 Dock 的角部钩子统一吞掉并投递
// kDockVolumePanelMsg，顶栏自身的 LL 钩子必须跳过它——否则面板打开时
// 点角区会同时触发"点面板外收起"（kCloseVolumeMsg）与 Dock 的 toggle
// 消息，造成"收起后立刻又打开"；且角区下方可能恰是应用窗口，
// 把角区点击记录成"显式切换窗口"会误解除目标粘住。
constexpr int kTriggerCornerW = 12;  // 逻辑像素（96 DPI 基准）
constexpr int kTriggerCornerH = 48;

bool InTriggerCornerZone(POINT pt, const AppState& s) {
    const int w = MulDiv(kTriggerCornerW, s.dpi, 96);
    const int h = MulDiv(kTriggerCornerH, s.dpi, 96);
    return pt.x >= 0 && pt.x < w && pt.y >= 0 && pt.y < h;
}

LRESULT CALLBACK VolumeLowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION && wParam == WM_LBUTTONDOWN) {
        const auto* info = reinterpret_cast<const MSLLHOOKSTRUCT*>(lParam);
        HWND hwnd = g_volumeHookHwnd;
        AppState* s = hwnd ? reinterpret_cast<AppState*>(
            GetWindowLongPtrW(hwnd, GWLP_USERDATA)) : nullptr;

        // 左上角触发角：Dock 角部钩子先/后处理均只有一条路径生效
        // （先装钩子时本钩子跳过 + Dock 吞掉；后装钩子时事件已被 Dock 吞掉，
        // 本钩子收不到），双钩子不会重复 toggle。
        if (s && InTriggerCornerZone(info->pt, *s)) {
            return CallNextHookEx(nullptr, nCode, wParam, lParam);
        }

        // 记录用户显式点击的顶层窗口（排除顶栏自身与音量面板）
        HWND clicked = WindowFromPoint(info->pt);
        if (clicked) {
            clicked = GetAncestor(clicked, GA_ROOT);
            if (clicked == hwnd ||
                (s && s->volumePanelHwnd && clicked == s->volumePanelHwnd)) {
                clicked = nullptr;
            }
        }
        g_lastClickWindow = clicked;

        // 目标粘住时，用户显式点击其他窗口应立即解除粘住并切换，
        // 不依赖 WinEvent 事件（事件可能延迟或不触发）
        if (s && s->targetSticky && clicked &&
            clicked != s->targetHwnd && IsControlTarget(clicked, hwnd)) {
            PostMessageW(hwnd, kUserSwitchMsg, 0, 0);
        } else if (s && s->targetSticky && clicked &&
                   clicked != s->targetHwnd) {
            // 落点是"不能当目标"的窗口（套件 Dock/启动台/时钟，或桌面等）：
            // 用户仍在主动切换应用 —— 置套件切换信号并立即刷新，解除粘住。
            // 少了这条，最小化目标后从 Dock/启动台切换应用，顶栏会永久
            // 停留在被最小化的旧目标上。
            g_suiteClickSwitch = true;
            PostMessageW(hwnd, kUserSwitchMsg, 0, 0);
        }

        if (s && s->volumeOpen) {
            // 音量按钮屏幕区域（点它不收起，由顶栏自己 toggle）
            RECT rc{};
            GetWindowRect(hwnd, &rc);
            RectF volR;
            VolumeButtonRect(*s, volR);
            RECT volScreen = {
                rc.left + static_cast<LONG>(volR.X),
                rc.top + static_cast<LONG>(volR.Y),
                rc.left + static_cast<LONG>(volR.X + volR.Width),
                rc.top + static_cast<LONG>(volR.Y + volR.Height)
            };

            // 独立音量面板窗口屏幕区域
            bool inPanel = false;
            if (s->volumePanelHwnd) {
                RECT panelScreen{};
                GetWindowRect(s->volumePanelHwnd, &panelScreen);
                inPanel = PtInRect(&panelScreen, info->pt) != FALSE;
            }

            const bool inVolBtn = PtInRect(&volScreen, info->pt) != FALSE;
            if (!inPanel && !inVolBtn) {
                PostMessageW(hwnd, kCloseVolumeMsg, 0, 0);
            }
        }
    }
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        const auto* info = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
        // Alt+Tab：系统键按下 Tab 且 Alt 处于按下状态
        if ((wParam == WM_SYSKEYDOWN || wParam == WM_KEYDOWN) &&
            info->vkCode == VK_TAB &&
            (GetAsyncKeyState(VK_MENU) & 0x8000) != 0) {
            g_altTabPressed = true;
            g_altTabSessionActive = true;
            // 先于 shell 通知 Dock：Alt+Tab 要切换了，立刻恢复挂起应用。
            // 每次 Tab 都投递（幂等）：Dock 已恢复过的应用不会重复恢复，
            // 同时顺延 Alt+Tab 会话的保护期，覆盖用户按住 Alt 连续循环选择。
            if (HWND dock = g_dockHwnd.load(std::memory_order_relaxed)) {
                PostMessageW(dock, kDockAltTabMsg, 0, 0);
            }
            HWND bar = g_volumeHookHwnd;
            AppState* s = bar ? reinterpret_cast<AppState*>(
                GetWindowLongPtrW(bar, GWLP_USERDATA)) : nullptr;
            // 目标粘住时，Alt+Tab 也立即通知刷新（不依赖 WinEvent）
            if (s && s->targetSticky) {
                PostMessageW(bar, kUserSwitchMsg, 0, 0);
            }
        } else if ((wParam == WM_KEYUP || wParam == WM_SYSKEYUP) &&
                   (info->vkCode == VK_MENU || info->vkCode == VK_LMENU ||
                    info->vkCode == VK_RMENU) &&
                   g_altTabSessionActive) {
            // Alt+Tab 会话结束（用户松开 Alt）：通知 Dock 进入落定期。
            // 不依赖 EVENT_SYSTEM_SWITCHEND —— Win11 XAML 切换器可能不发它。
            g_altTabSessionActive = false;
            if (HWND dock = g_dockHwnd.load(std::memory_order_relaxed)) {
                PostMessageW(dock, kDockAltTabMsg, 3, 0);
            }
        }
    }
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

// ---- WinEvent 钩子：替代前台窗口 / 最大化状态 / 标签列表的轮询 ----
// 前台切换、最小化/最大化/还原、目标应用窗口增删/显隐/标题变化
// 均由系统事件驱动，不再用常驻定时器轮询。
HWINEVENTHOOK g_winEventHook[5] = {nullptr, nullptr, nullptr,
                                   nullptr, nullptr};
// 目标窗口移动/缩放（EVENT_OBJECT_LOCATIONCHANGE）钩子：按目标进程订阅，
// 只用来驱动让位判定的即时重估（拖动/贴边/最大化动画跟手），
// 目标变化时用 RetargetLocationHook 重新订阅（pid=0 表示已退订）
HWINEVENTHOOK g_locationHook = nullptr;
DWORD g_locationHookPid = 0;

void CALLBACK WinEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd,
                           LONG idObject, LONG idChild, DWORD, DWORD) {
    if (idObject != OBJID_WINDOW || idChild != CHILDID_SELF) {
        return;
    }
    HWND bar = g_volumeHookHwnd;
    AppState* s = bar ? reinterpret_cast<AppState*>(
        GetWindowLongPtrW(bar, GWLP_USERDATA)) : nullptr;
    if (!s) {
        return;
    }

    if (event == EVENT_SYSTEM_FOREGROUND) {
        // 新窗口激活瞬间可能尚未可见（正在创建）：立即按前台处理会
        // 把目标误清（见 UpdateTarget）。延迟 150ms 等窗口可见后再复查，
        // 期间保持当前目标不变；复查时仍不可见则继续（见 kMaxTargetRetry）。
        if (hwnd && IsWindow(hwnd) && !IsWindowVisible(hwnd)) {
            s->targetRetryCount = 0;
            SetTimer(bar, kTargetRetryTimerId, 150, nullptr);
            return;
        }
        UpdateTarget(*s);
        DrawBarAndPresent(*s);
        return;
    }

    // 系统「快速设置」面板被 show：这一刻就把它摆到顶栏正下方。
    //
    // 为什么必须在这里做：面板由 shell 先摆到任务栏上方（屏幕右下角）再
    // 显示，等定时器（哪怕 20ms）去挪，用户就能看到"先在下方闪一下、再跳
    // 到顶栏下"。EVENT_OBJECT_SHOW 是系统能给出的最早时机，在它之后立刻
    // SetWindowPos，通常赶在面板首帧绘制之前，视觉上就是"一出现就在顶栏下"。
    // 这里只做一次抢先；后续跟随（区域随内容变化）交给对齐定时器。
    if (event == EVENT_OBJECT_SHOW && hwnd && hwnd != bar &&
        IsQuickSettingsHost(hwnd) && IsWindowVisible(hwnd)) {
        if (AlignQuickSettingsToBar(*s, hwnd)) {
            ArmQuickSettingsAlign(*s, true);
        } else {
            // 区域/位置还没就绪：保持高频重试（下一拍通常就能摆好）
            ArmQuickSettingsAlign(*s, false);
        }
        return;
    }

    // 窗口状态事件：只关心当前目标或前台窗口
    HWND top = GetAncestor(hwnd, GA_ROOT);
    switch (event) {
    case EVENT_SYSTEM_MINIMIZESTART:
    case EVENT_SYSTEM_MINIMIZEEND:
    case EVENT_SYSTEM_MAXIMIZESTART:
    case EVENT_SYSTEM_MAXIMIZEEND:
    case EVENT_SYSTEM_RESTORE:
        if (top == s->targetHwnd || top == GetForegroundWindow()) {
            UpdateTarget(*s);
            // 最小化/最大化/还原都会改变目标窗口矩形：重估让位层级
            // （最小化不再占屏面 → 退回覆盖态；最大化通常压住顶栏 → 让位）
            ApplyBarLayer(*s, false);
            DrawBarAndPresent(*s);
        }
        break;
    case EVENT_OBJECT_LOCATIONCHANGE:
        // 目标窗口移动/缩放：让位判定只取决于目标窗口矩形与顶栏是否相交，
        // 所以这里幂等重估（状态未变不做任何 Z 序操作）。
        // 钩子已按目标进程订阅（见 RetargetLocationHook），噪音有限
        if (s->hasTarget && s->targetHwnd && IsWindow(s->targetHwnd) &&
            GetAncestor(hwnd, GA_ROOT) == s->targetHwnd) {
            ApplyBarLayer(*s, false);
        }
        break;
    case EVENT_OBJECT_CREATE:
    case EVENT_OBJECT_DESTROY:
    case EVENT_OBJECT_SHOW:
    case EVENT_OBJECT_HIDE:
    case EVENT_OBJECT_NAMECHANGE:
        // 目标应用顶层窗口增删/显隐/标题变化：事件驱动刷新标签。
        // 只处理目标应用的顶层窗口，降低高频对象事件的噪音；
        // 经 100ms 抖动合并，避免连续事件反复刷新。
        if (!s->hasTarget || !s->targetHwnd || !IsWindow(s->targetHwnd)) {
            break;
        }
        // 只看顶层窗口（排除子控件）；销毁事件中句柄可能已部分失效，放宽
        if (event != EVENT_OBJECT_DESTROY && hwnd &&
            GetAncestor(hwnd, GA_ROOT) != hwnd) {
            break;
        }
        if (hwnd) {
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            // 查不到进程（窗口正在销毁）时保守刷新
            if (pid && !IsSameApplication(hwnd, s->targetHwnd)) {
                break;
            }
        }
        SetTimer(bar, kTabRefreshDebounceTimerId, kTabRefreshDebounceMs,
                 nullptr);
        break;
    default:
        break;
    }
}

// 目标窗口移动/缩放订阅：EVENT_OBJECT_LOCATIONCHANGE 是全系统高频事件，
// 直接全量订阅会把每个窗口的每次移动都灌进本线程消息队列；它只用于
// 让位判定的即时重估，因此按目标进程订阅，目标一变就换订阅。
void RetargetLocationHook(HWND target) {
    DWORD pid = 0;
    if (target && IsWindow(target)) {
        GetWindowThreadProcessId(target, &pid);
    }
    if (pid == g_locationHookPid && (pid == 0) == (g_locationHook == nullptr)) {
        return;  // 订阅未变
    }
    if (g_locationHook) {
        UnhookWinEvent(g_locationHook);
        g_locationHook = nullptr;
    }
    g_locationHookPid = pid;
    if (pid) {
        g_locationHook = SetWinEventHook(
            EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE,
            nullptr, WinEventProc, pid, 0, WINEVENT_OUTOFCONTEXT);
    }
}

void InstallVolumeHook(HWND hwnd) {
    if (!g_volumeHook) {
        g_volumeHookHwnd = hwnd;
        g_volumeHook = SetWindowsHookExW(WH_MOUSE_LL, VolumeLowLevelMouseProc,
                                         GetModuleHandleW(nullptr), 0);
    }
    if (!g_keyHook) {
        g_keyHook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc,
                                      GetModuleHandleW(nullptr), 0);
    }
    if (!g_winEventHook[0]) {
        // 前台窗口切换
        g_winEventHook[0] = SetWinEventHook(
            EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
            nullptr, WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT);
        // 最小化 / 最大化
        g_winEventHook[1] = SetWinEventHook(
            EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MAXIMIZEEND,
            nullptr, WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT);
        // 还原
        g_winEventHook[2] = SetWinEventHook(
            EVENT_SYSTEM_RESTORE, EVENT_SYSTEM_RESTORE,
            nullptr, WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT);
        // 目标应用窗口创建 / 销毁 / 显示 / 隐藏：事件驱动刷新标签
        g_winEventHook[3] = SetWinEventHook(
            EVENT_OBJECT_CREATE, EVENT_OBJECT_HIDE,
            nullptr, WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT);
        // 目标应用窗口标题变化
        g_winEventHook[4] = SetWinEventHook(
            EVENT_OBJECT_NAMECHANGE, EVENT_OBJECT_NAMECHANGE,
            nullptr, WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT);
    }
}

void UninstallVolumeHook() {
    if (g_volumeHook) {
        UnhookWindowsHookEx(g_volumeHook);
        g_volumeHook = nullptr;
    }
    if (g_keyHook) {
        UnhookWindowsHookEx(g_keyHook);
        g_keyHook = nullptr;
    }
    for (auto& h : g_winEventHook) {
        if (h) {
            UnhookWinEvent(h);
            h = nullptr;
        }
    }
    if (g_locationHook) {
        UnhookWinEvent(g_locationHook);
        g_locationHook = nullptr;
    }
    g_locationHookPid = 0;
    g_volumeHookHwnd = nullptr;
    g_lastClickWindow = nullptr;
    g_altTabPressed = false;
    g_altTabSessionActive = false;
}

// ---- 独立音量面板窗口 ----
void DestroyPanelBacking(AppState& s) {
    delete s.panelBitmap;
    s.panelBitmap = nullptr;

    if (s.panelHdcMem) {
        if (s.panelHbmDib) {
            if (s.panelHbmOld) {
                SelectObject(s.panelHdcMem, s.panelHbmOld);
            }
            DeleteObject(s.panelHbmDib);
            s.panelHbmDib = nullptr;
        }
        DeleteDC(s.panelHdcMem);
        s.panelHdcMem = nullptr;
    }
    s.panelHbmOld = nullptr;
    s.panelPvBits = nullptr;
}

bool CreatePanelBacking(AppState& s, int w, int h) {
    DestroyPanelBacking(s);

    HDC screenDc = GetDC(s.volumePanelHwnd ? s.volumePanelHwnd : s.hwnd);
    if (!screenDc) {
        return false;
    }
    s.panelHdcMem = CreateCompatibleDC(screenDc);
    if (!s.panelHdcMem) {
        ReleaseDC(s.volumePanelHwnd, screenDc);
        return false;
    }

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    s.panelHbmDib = CreateDIBSection(screenDc, &bmi, DIB_RGB_COLORS, &s.panelPvBits,
                                     nullptr, 0);
    ReleaseDC(s.volumePanelHwnd ? s.volumePanelHwnd : s.hwnd, screenDc);
    if (!s.panelHbmDib) {
        DeleteDC(s.panelHdcMem);
        s.panelHdcMem = nullptr;
        return false;
    }

    s.panelHbmOld = static_cast<HBITMAP>(SelectObject(s.panelHdcMem, s.panelHbmDib));
    s.panelBitmap = new Gdiplus::Bitmap(w, h, PixelFormat32bppPARGB);
    if (!s.panelBitmap) {
        DestroyPanelBacking(s);
        return false;
    }
    return true;
}

// 绘制音量面板内容（面板窗口客户区，panel 从 (0,0) 开始）
void DrawVolumePanelContent(Graphics& g, AppState& s) {
    const float k = s.scale;
    const int pw = MulDiv(kVolumePanelW, s.dpi, 96);
    const int ph = MulDiv(kVolumePanelH, s.dpi, 96);
    RectF panel(0, 0, static_cast<float>(pw), static_cast<float>(ph));
    RectF mute, track;
    VolumePanelInner(s, panel, mute, track);

    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(PixelOffsetModeHighQuality);
    g.Clear(Color(0, 0, 0, 0));

    // 面板背景：圆角矩形 + 细边框
    GraphicsPath panelPath;
    AddRoundedRectPath(panelPath, panel, 10.0f * k);
    SolidBrush panelBg(Color(210, 44, 48, 56));
    g.FillPath(&panelBg, &panelPath);
    Pen panelBorder(Color(100, 255, 255, 255), 1.0f);
    g.DrawPath(&panelBorder, &panelPath);

    // 静音按钮
    DrawButtonHover(g, mute, kHitMute, s.hoverButton);
    DrawVolumeGlyph(g, mute, k,
                    s.volumeMuted ? Color(255, 96, 92, 84)
                                  : Color(220, 255, 255, 255),
                    s.volumeMuted);

    // 滑条轨道（两端圆角的小胶囊）
    const float trackRadius = track.Height * 0.5f;
    if (!s.volumeReady) {
        GraphicsPath disPath;
        AddRoundedRectPath(disPath, track, trackRadius);
        SolidBrush dis(Color(50, 200, 200, 200));
        g.FillPath(&dis, &disPath);
    } else {
        GraphicsPath basePath;
        AddRoundedRectPath(basePath, track, trackRadius);
        SolidBrush base(Color(90, 90, 90, 100));
        g.FillPath(&base, &basePath);

        RectF filled(track.X, track.Y, track.Width * s.volumeValue, track.Height);
        if (filled.Width > trackRadius * 2.0f) {
            GraphicsPath fillPath;
            AddRoundedRectPath(fillPath, filled, trackRadius);
            SolidBrush fillBrush(Color(150, 90, 200, 250));
            g.FillPath(&fillBrush, &fillPath);
        } else {
            SolidBrush fillBrush(Color(150, 90, 200, 250));
            g.FillRectangle(&fillBrush, filled);
        }

        const float thumbR = 6.0f * k;
        const float thumbX = track.X + track.Width * s.volumeValue;
        const float thumbY = track.Y + track.Height * 0.5f;
        SolidBrush thumbCol(Color(255, 255, 255, 255));
        g.FillEllipse(&thumbCol, thumbX - thumbR, thumbY - thumbR,
                      thumbR * 2.0f, thumbR * 2.0f);
    }

    RectF pctR(panel.X + panel.Width - 56.0f * k, panel.Y, 50.0f * k, panel.Height);
    DrawVolumePercent(g, s, pctR, s.volumeValue, s.volumeReady);
}

void PresentVolumePanel(AppState& s) {
    if (!s.panelBitmap || !s.panelHdcMem || !s.panelPvBits || !s.volumePanelHwnd) {
        return;
    }
    const int w = MulDiv(kVolumePanelW, s.dpi, 96);
    const int h = MulDiv(kVolumePanelH, s.dpi, 96);

    Gdiplus::Rect lockRect(0, 0, w, h);
    BitmapData bd{};
    if (s.panelBitmap->LockBits(&lockRect, ImageLockModeRead,
                                PixelFormat32bppPARGB, &bd) != Ok) {
        return;
    }
    auto* dst = static_cast<BYTE*>(s.panelPvBits);
    const auto* src = static_cast<const BYTE*>(bd.Scan0);
    const size_t rowBytes = static_cast<size_t>(w) * 4u;
    for (int y = 0; y < h; ++y) {
        std::memcpy(dst + static_cast<size_t>(y) * rowBytes,
                    src + static_cast<size_t>(y) * static_cast<size_t>(bd.Stride),
                    rowBytes);
    }
    s.panelBitmap->UnlockBits(&bd);

    RECT rc{};
    GetWindowRect(s.volumePanelHwnd, &rc);
    POINT ptDst{rc.left, rc.top};
    POINT ptSrc{0, 0};
    SIZE size{w, h};

    BLENDFUNCTION blend{};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;

    HDC screenDc = GetDC(nullptr);
    if (screenDc) {
        UpdateLayeredWindow(s.volumePanelHwnd, screenDc, &ptDst, &size,
                            s.panelHdcMem, &ptSrc, 0, &blend, ULW_ALPHA);
        ReleaseDC(nullptr, screenDc);
    }
}

// 面板窗口命中：客户区坐标 -> 静音/滑条
ButtonHit VolumePanelHitTest(AppState& s, int x, int y) {
    const int pw = MulDiv(kVolumePanelW, s.dpi, 96);
    const int ph = MulDiv(kVolumePanelH, s.dpi, 96);
    if (x < 0 || y < 0 || x >= pw || y >= ph) {
        return kHitNone;
    }
    RectF panel(0, 0, static_cast<float>(pw), static_cast<float>(ph));
    RectF mute, track;
    VolumePanelInner(s, panel, mute, track);
    if (x >= static_cast<int>(mute.X) && x < static_cast<int>(mute.X + mute.Width) &&
        y >= static_cast<int>(mute.Y) && y < static_cast<int>(mute.Y + mute.Height)) {
        return kHitMute;
    }
    return kHitSlider;
}

LRESULT CALLBACK VolumePanelWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    AppState* s = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        s = static_cast<AppState*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(s));
    } else {
        s = reinterpret_cast<AppState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    switch (msg) {
    case WM_NCCREATE:
        return DefWindowProcW(hwnd, msg, wParam, lParam);

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps{};
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_NCHITTEST:
        return HTCLIENT;

    case WM_WINDOWPOSCHANGING: {
        // 音量面板是置顶浮层：任何 Z 序变化都强制保持置顶，防止被普通窗口覆盖
        auto* wp = reinterpret_cast<WINDOWPOS*>(lParam);
        if ((wp->flags & SWP_NOZORDER) == 0 && wp->hwndInsertAfter != HWND_TOPMOST) {
            wp->hwndInsertAfter = HWND_TOPMOST;
            wp->flags |= SWP_NOACTIVATE;
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    case WM_MOUSEMOVE: {
        if (!s) {
            return 0;
        }
        if (!s->trackingMouse) {
            TRACKMOUSEEVENT tme{};
            tme.cbSize = sizeof(tme);
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = hwnd;
            TrackMouseEvent(&tme);
            s->trackingMouse = true;
        }
        POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (s->volumeDragging) {
            RectF panel(0, 0,
                        static_cast<float>(MulDiv(kVolumePanelW, s->dpi, 96)),
                        static_cast<float>(MulDiv(kVolumePanelH, s->dpi, 96)));
            RectF mute, track;
            VolumePanelInner(*s, panel, mute, track);
            const float span = track.Width > 1.0f ? track.Width : 1.0f;
            const float ratio = (static_cast<float>(pt.x) - track.X) / span;
            ApplyVolume(*s, std::clamp(ratio, 0.0f, 1.0f));
            Graphics g(s->panelBitmap);
            DrawVolumePanelContent(g, *s);
            PresentVolumePanel(*s);
        }
        const ButtonHit hit = VolumePanelHitTest(*s, pt.x, pt.y);
        if (hit != s->hoverButton) {
            s->hoverButton = hit;
            Graphics g(s->panelBitmap);
            DrawVolumePanelContent(g, *s);
            PresentVolumePanel(*s);
        }
        return 0;
    }

    case WM_MOUSELEAVE:
        if (s) {
            s->trackingMouse = false;
            if (!s->volumeDragging && s->hoverButton != kHitNone) {
                s->hoverButton = kHitNone;
                Graphics g(s->panelBitmap);
                DrawVolumePanelContent(g, *s);
                PresentVolumePanel(*s);
            }
        }
        return 0;

    case WM_LBUTTONDOWN: {
        if (!s) {
            return 0;
        }
        POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        const ButtonHit hit = VolumePanelHitTest(*s, pt.x, pt.y);
        if (hit == kHitMute) {
            ToggleMute(*s);
            Graphics g(s->panelBitmap);
            DrawVolumePanelContent(g, *s);
            PresentVolumePanel(*s);
        } else if (hit == kHitSlider && s->volumeReady && s->volumeSession) {
            s->volumeDragging = true;
            SetCapture(hwnd);
            RectF panel(0, 0,
                        static_cast<float>(MulDiv(kVolumePanelW, s->dpi, 96)),
                        static_cast<float>(MulDiv(kVolumePanelH, s->dpi, 96)));
            RectF mute, track;
            VolumePanelInner(*s, panel, mute, track);
            const float span = track.Width > 1.0f ? track.Width : 1.0f;
            const float ratio = (static_cast<float>(pt.x) - track.X) / span;
            ApplyVolume(*s, std::clamp(ratio, 0.0f, 1.0f));
            Graphics g(s->panelBitmap);
            DrawVolumePanelContent(g, *s);
            PresentVolumePanel(*s);
        }
        return 0;
    }

    case WM_LBUTTONUP:
        if (s && s->volumeDragging) {
            s->volumeDragging = false;
            ReleaseCapture();
        }
        return 0;

    case WM_CAPTURECHANGED:
        // 捕获被系统或其他窗口夺走（如面板被隐藏/销毁）时结束滑块拖动，
        // 避免残留拖拽状态；捕获此刻已不属于本窗口，无需再 ReleaseCapture。
        if (s) {
            s->volumeDragging = false;
        }
        return 0;

    case WM_DESTROY:
        if (s) {
            s->volumePanelHwnd = nullptr;
        }
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool RegisterVolumePanelClass() {
    static bool registered = false;
    if (registered) {
        return true;
    }
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = VolumePanelWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"DesktopTopBarVolumePanelWindow";
    registered = RegisterClassExW(&wc) != 0;
    return registered;
}

void ShowVolumePanel(AppState& s) {
    if (!RegisterVolumePanelClass()) {
        return;
    }
    const int pw = MulDiv(kVolumePanelW, s.dpi, 96);
    const int ph = MulDiv(kVolumePanelH, s.dpi, 96);

    RECT barRc{};
    GetWindowRect(s.hwnd, &barRc);
    const int x = barRc.left + static_cast<int>(kVolumePanelMargin * s.scale);
    const int y = barRc.top + BarAreaHeight(s);

    if (!s.volumePanelHwnd) {
        s.volumePanelHwnd = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST,
            L"DesktopTopBarVolumePanelWindow", L"",
            WS_POPUP,
            x, y, pw, ph,
            nullptr, nullptr, GetModuleHandleW(nullptr), &s);

        if (!s.volumePanelHwnd) {
            return;
        }
        // 音量面板是独立置顶窗口：不挂到桌面层，始终显示在普通窗口之上
    }

    if (!CreatePanelBacking(s, pw, ph)) {
        return;
    }
    Graphics g(s.panelBitmap);
    DrawVolumePanelContent(g, s);
    PresentVolumePanel(s);

    // 显示并保持置顶（音量面板与顶栏不同，允许覆盖普通窗口）
    ShowWindow(s.volumePanelHwnd, SW_SHOWNOACTIVATE);
    SetWindowPos(s.volumePanelHwnd, HWND_TOPMOST, x, y, pw, ph,
                 SWP_NOACTIVATE);
}

void HideVolumePanel(AppState& s) {
    if (s.volumePanelHwnd) {
        // 正在拖动滑块时收起：先释放捕获再隐藏，避免捕获留在隐藏窗口上，
        // 把后续全部鼠标输入都吞进不可见面板（全局点不动、键盘正常）。
        if (s.volumeDragging) {
            s.volumeDragging = false;
            ReleaseCapture();
        }
        ShowWindow(s.volumePanelHwnd, SW_HIDE);
    }
}

// 展开 / 收起音量面板；顶栏自身高度保持不变，面板为独立小窗口
void SetVolumeOpen(AppState& s, bool open) {
    if (s.volumeOpen == open) {
        return;
    }
    s.volumeOpen = open;
    if (open) {
        // 展开时立即补解析一次：即使目标应用先前无声（解析不到会话），
        // 此时也会解析成功/启动面板展开期轮询，滑条尽快恢复可用
        ResolveVolumeSession(s);
        ShowVolumePanel(s);
    } else {
        // 面板关闭即停轮询（零轮询：无声应用常驻聚焦也无后台开销）
        KillTimer(s.hwnd, kVolumeRetryTimerId);
        HideVolumePanel(s);
    }
}

// ---- 绘制 ----

// 绘制单个按钮的悬停/点击高亮背景。
void DrawButtonHover(Graphics& g, const RectF& r, ButtonHit hit, int hover) {
    if (hover != hit) {
        return;
    }
    if (hit == kHitClock) {
        // Win11 任务栏时钟：悬停时显示内缩的圆角药丸高亮
        const float inset = r.Height * 0.08f;
        RectF pill(r.X + inset, r.Y + inset,
                   r.Width - inset * 2.0f, r.Height - inset * 2.0f);
        GraphicsPath path;
        AddRoundedRectPath(path, pill, pill.Height * 0.28f);
        SolidBrush bg(Color(50, 255, 255, 255));
        g.FillPath(&bg, &path);
        return;
    }
    Color hoverColor(70, 255, 255, 255);
    if (hit == kHitClose) {
        // Chrome 关闭按钮：红色背景
        hoverColor = Color(200, 224, 60, 49);
    }
    SolidBrush bg(hoverColor);
    g.FillRectangle(&bg, r);
}

// 最小化字形：横线
void DrawMinimizeGlyph(Graphics& g, const RectF& r, float scale, const Color& col) {
    const float cx = r.X + r.Width * 0.5f;
    const float cy = r.Y + r.Height * 0.5f;
    Pen pen(col, 1.6f * scale);
    pen.SetStartCap(LineCapRound);
    pen.SetEndCap(LineCapRound);
    g.DrawLine(&pen, cx - 5.0f * scale, cy, cx + 5.0f * scale, cy);
}

// 最大化 / 还原字形（需知窗口是否最大化）
void DrawMaximizeGlyph(Graphics& g, const RectF& r, float scale, bool maximized,
                       const Color& col) {
    const float cx = r.X + r.Width * 0.5f;
    const float cy = r.Y + r.Height * 0.5f;
    Pen pen(col, 1.4f * scale);
    const float box = 4.6f * scale;
    if (maximized) {
        // 还原：两个交叠方块
        RectF r1(cx - box, cy - box + 1.5f * scale, box * 2.0f, box * 2.0f);
        g.DrawRectangle(&pen, r1);
        RectF r2(cx - box + 2.2f * scale, cy - box - 1.5f * scale,
                 box * 2.0f, box * 2.0f);
        g.DrawRectangle(&pen, r2);
    } else {
        RectF r1(cx - box, cy - box, box * 2.0f, box * 2.0f);
        g.DrawRectangle(&pen, r1);
    }
}

// 关闭字形：X
void DrawCloseGlyph(Graphics& g, const RectF& r, float scale, const Color& col) {
    const float cx = r.X + r.Width * 0.5f;
    const float cy = r.Y + r.Height * 0.5f;
    Pen pen(col, 1.6f * scale);
    pen.SetStartCap(LineCapRound);
    pen.SetEndCap(LineCapRound);
    const float d = 5.0f * scale;
    g.DrawLine(&pen, cx - d, cy - d, cx + d, cy + d);
    g.DrawLine(&pen, cx + d, cy - d, cx - d, cy + d);
}

// 使用 Windows 自带的 Segoe MDL2 Assets 图标字体绘制喇叭/音量图标，
// 与系统音量按钮完全一致（E767=喇叭，E74F=静音喇叭）。
void DrawVolumeGlyph(Graphics& g, const RectF& r, float scale, const Color& col,
                     bool muted) {
    (void)scale;  // 字体字形已按 r.Height 缩放，无需手动 scale
    // 优先 Segoe Fluent Icons（Win11），回退 Segoe MDL2 Assets（Win10）
    FontFamily famFluent(L"Segoe Fluent Icons");
    FontFamily famMdl2(L"Segoe MDL2 Assets");
    FontFamily* fam = &famMdl2;
    if (famFluent.GetLastStatus() == Ok) {
        fam = &famFluent;
    }

    // 字形尺寸要明显小于按钮高度，并给四周留内边距，
    // 避免贴左上角时图标超出左/上边缘。
    const float fontSize = r.Height * 0.62f;
    Gdiplus::Font font(fam, fontSize, FontStyleRegular, UnitPixel);

    wchar_t ch = muted ? 0xE74F : 0xE767;
    StringFormat sf;
    sf.SetAlignment(StringAlignmentCenter);
    sf.SetLineAlignment(StringAlignmentCenter);

    const float inset = r.Height * 0.12f;
    RectF drawR(r.X + inset, r.Y + inset,
                r.Width - inset * 2.0f, r.Height - inset * 2.0f);

    SolidBrush brush(col);
    g.DrawString(&ch, 1, &font, drawR, &sf, &brush);
}

// Win11 风格任务栏时钟：时间在上、日期在下两行，居中显示于时钟区域。
// 字体用圆润的 MiSans（回退 Segoe UI Variable / Segoe UI），
// 并强制灰度抗锯齿：分层窗口上 GDI+ 默认的 ClearType 会产生彩色描边。
void DrawClock(Graphics& g, AppState& s, const RectF& r) {
    const float k = s.scale;

    // 灰度抗锯齿，去掉文本彩色描边（默认 TextRenderingHint 是 ClearType）
    g.SetTextRenderingHint(TextRenderingHintAntiAlias);

    // 圆润字体选择：MiSans → Segoe UI Variable Text Semibold → Segoe UI Semibold。
    // GDI+ FontFamily 拷贝赋值是 private，用多个实例 + 指针选择。
    FontFamily famMiSans(L"MiSans");
    FontFamily famSegVar(L"Segoe UI Variable Text Semibold");
    FontFamily famSeg(L"Segoe UI Semibold");
    const FontFamily* fam = &famMiSans;
    if (famMiSans.GetLastStatus() != Ok) {
        fam = &famSegVar;
        if (famSegVar.GetLastStatus() != Ok) {
            fam = &famSeg;
        }
    }
    Gdiplus::Font font(fam, 12.0f * k, FontStyleRegular, UnitPixel);

    const float half = r.Height * 0.5f;
    RectF timeR(r.X, r.Y, r.Width, half);
    RectF dateR(r.X, r.Y + half, r.Width, half);

    StringFormat sf;
    sf.SetAlignment(StringAlignmentCenter);
    sf.SetLineAlignment(StringAlignmentCenter);
    sf.SetFormatFlags(StringFormatFlagsNoWrap);

    SolidBrush textBrush(Color(255, 255, 255, 255));
    if (!s.clockTimeText.empty()) {
        g.DrawString(s.clockTimeText.c_str(), -1, &font, timeR, &sf,
                     &textBrush);
    }
    if (!s.clockDateText.empty()) {
        g.DrawString(s.clockDateText.c_str(), -1, &font, dateR, &sf,
                     &textBrush);
    }
}

// 滑块上的音量百分比文本
void DrawVolumePercent(Graphics& g, AppState& s, const RectF& r, float value,
                       bool ready) {
    if (!ready) {
        return;
    }
    const float k = s.scale;
    FontFamily family(L"Segoe UI");
    Gdiplus::Font font(&family, 12.0f * k, FontStyleRegular, UnitPixel);

    wchar_t text[16] = {};
    swprintf_s(text, L"%d%%", static_cast<int>(value * 100.0f + 0.5f));

    StringFormat sf;
    sf.SetAlignment(StringAlignmentFar);
    sf.SetLineAlignment(StringAlignmentCenter);

    SolidBrush brush(Color(220, 255, 255, 255));
    g.DrawString(text, -1, &font, r, &sf, &brush);
}

// 向 GraphicsPath 添加一个圆角矩形路径
void AddRoundedRectPath(GraphicsPath& path, const RectF& r, float radius) {
    const float d = radius * 2.0f;
    path.AddArc(r.X, r.Y, d, d, 180.0f, 90.0f);
    path.AddArc(r.X + r.Width - d, r.Y, d, d, 270.0f, 90.0f);
    path.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0.0f, 90.0f);
    path.AddArc(r.X, r.Y + r.Height - d, d, d, 90.0f, 90.0f);
    path.CloseFigure();
}

// Chrome 风格标签背景：顶部两角外凸圆角、底部两角内凹圆角
// （当前标签与拖动中"浮起"的标签共用同一形状，只换填充/描边）
void BuildTabBackgroundPath(GraphicsPath& path, const RectF& visual, float k) {
    const float rTop = (std::min)(8.0f * k, visual.Width * 0.5f);
    const float rBot = (std::min)(8.0f * k, visual.Width * 0.25f);
    const float x0 = visual.X;
    const float y0 = visual.Y;
    const float x1 = visual.X + visual.Width;
    const float y1 = visual.Y + visual.Height;

    // 左上外凸圆角
    path.AddArc(x0, y0, rTop * 2.0f, rTop * 2.0f, 180.0f, 90.0f);
    path.AddLine(x0 + rTop, y0, x1 - rTop, y0);
    // 右上外凸圆角
    path.AddArc(x1 - rTop * 2.0f, y0, rTop * 2.0f, rTop * 2.0f, 270.0f, 90.0f);
    // 右侧边框向下到右下圆角起点
    path.AddLine(x1, y0 + rTop, x1, y1 - rBot);
    // 右下圆角（与未打开标签共用的样式）
    {
        RectF brBox(x1, y1 - rBot * 2.0f, rBot * 2.0f, rBot * 2.0f);
        path.AddArc(brBox, 180.0f, -90.0f);
    }
    // 底边
    path.AddLine(x1 + rBot, y1, x0 - rBot, y1);
    // 左下圆角（与未打开标签共用的样式）
    {
        RectF blBox(x0 - rBot * 2.0f, y1 - rBot * 2.0f, rBot * 2.0f,
                    rBot * 2.0f);
        path.AddArc(blBox, 90.0f, -90.0f);
    }
    path.CloseFigure();
}

// 标签标题：固定标签左侧画小圆点，标题从左开始、超出宽度直接截断
void DrawTabTitle(Graphics& g, AppState& s, const TabInfo& tab, const RectF& r,
                  Gdiplus::Font& font, bool chromeMode) {
    const float k = s.scale;
    float textLeft = r.X + 8.0f * k;
    if (chromeMode && tab.chromePinned) {
        const float dotR = 2.6f * k;
        SolidBrush dotBrush(Color(220, 255, 255, 255));
        g.FillEllipse(&dotBrush, textLeft + 2.0f * k,
                      r.Y + r.Height * 0.5f - dotR, dotR * 2.0f, dotR * 2.0f);
        textLeft += 10.0f * k;
    }
    const float textRight = r.X + r.Width - 8.0f * k;
    RectF textR = RectF(textLeft, r.Y, (std::max)(textRight - textLeft, 1.0f),
                        r.Height);
    StringFormat tabSf;
    tabSf.SetFormatFlags(StringFormatFlagsNoWrap);  // 只显示一行，超出宽度直接截断
    tabSf.SetAlignment(StringAlignmentNear);  // 从左开始，超出部分在右侧截断
    tabSf.SetLineAlignment(StringAlignmentCenter);
    tabSf.SetTrimming(StringTrimmingNone);  // 不用省略号，按渲染宽度截断

    SolidBrush textBrush(Color(255, 255, 255, 255));
    GraphicsState state = g.Save();
    g.SetClip(textR);
    g.DrawString(tab.title.c_str(), -1, &font, textR, &tabSf, &textBrush);
    g.Restore(state);
}

// 拖动重排中"浮起"的标签：跟着光标走、比激活标签更亮，并带一圈浅色描边
// （被拖标签原来的位置留空，后面的标签已经实时补位，即落点提示）
void DrawFloatingTab(Graphics& g, AppState& s, const TabInfo& tab,
                     Gdiplus::Font& font, bool chromeMode, float barH) {
    const float k = s.scale;
    const float w = tab.rect.Width;
    if (w <= 0.0f) {
        return;
    }
    float stripL = 0.0f;
    float stripR = 0.0f;
    TabStripRange(s, stripL, stripR);
    float fx = s.tabDragBodyX;
    if (fx < stripL) {
        fx = stripL;
    }
    if (fx + w > stripR) {
        fx = stripR - w;
    }

    // 顶部再抬高一点，视觉上"离开标签栏"
    const float inset = kTabTopInset * k * 0.35f;
    const float h = barH - inset;
    if (h <= 0.0f) {
        return;
    }
    RectF visual(fx, inset, w, h);

    GraphicsPath path;
    BuildTabBackgroundPath(path, visual, k);
    SolidBrush fill(Color(110, 255, 255, 255));
    g.FillPath(&fill, &path);
    Pen border(Color(130, 255, 255, 255), 1.0f * k);
    g.DrawPath(&border, &path);

    DrawTabTitle(g, s, tab, visual, font, chromeMode);
}

void DrawBar(AppState& s) {
    Gdiplus::Bitmap* bmp = s.bitmap;
    if (!bmp) {
        return;
    }

    const float k = s.scale;
    Graphics g(bmp);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(PixelOffsetModeHighQuality);
    g.Clear(Color(0, 0, 0, 0)); // 全透明背景

    const float barH = static_cast<float>(BarAreaHeight(s));

    // 半透明深色顶栏（macOS 风格），跨全宽
    RectF barRect(0, 0, static_cast<float>(s.width), barH);
    SolidBrush barBrush(Color(150, 44, 48, 56));
    g.FillRectangle(&barBrush, barRect);

    // 底部细边框
    Pen bottomLine(Color(70, 255, 255, 255), 1.0f);
    g.DrawLine(&bottomLine,
               PointF(0.0f, barH - 0.5f),
               PointF(static_cast<float>(s.width), barH - 0.5f));

    // ---- 左上角音量按钮 ----
    RectF volR;
    VolumeButtonRect(s, volR);
    DrawButtonHover(g, volR, kHitVolume, s.hoverButton);
    DrawVolumeGlyph(g, volR, k,
                    s.hoverButton == kHitVolume ? Color(255, 255, 255, 255)
                                                : Color(200, 255, 255, 255),
                    s.volumeMuted);

    // ---- Chrome 风格标签：从音量键右侧到右侧三键之间 ----
    LayoutTabs(s, s.tabs, &g);
    RectF minR, maxR, closeR;
    ComputeButtonRects(s, minR, maxR, closeR);

    FontFamily tabFamily(L"Segoe UI");
    Gdiplus::Font tabFont(&tabFamily, 12.0f * k, FontStyleRegular, UnitPixel);

    // 标签是否处于激活状态（Chrome 同步模式看浏览器内激活态）
    const auto tabActive = [&s](const TabInfo& t) {
        return t.isChrome ? t.chromeActive : (t.hwnd == s.targetHwnd);
    };
    const bool chromeMode = ChromeSyncMode(s);

    // 正在拖动重排的标签：它的正常绘制被跳过（位置留空 = 落点提示），
    // 最后单独画在光标处
    const bool tabDragging = s.tabDragging && s.tabDragIndex >= 0 &&
                             s.tabDragIndex < static_cast<int>(s.tabs.size());
    const int dragTabIndex = tabDragging ? s.tabDragIndex : -1;

    // 未激活标签之间的竖线：高度略小于标签栏高度，上下留白相等
    for (size_t i = 0; i + 1 < s.tabs.size(); ++i) {
        if (static_cast<int>(i) == dragTabIndex ||
            static_cast<int>(i + 1) == dragTabIndex) {
            continue;  // 被拖标签两侧不画分隔线（它已经不在这条缝上了）
        }
        const bool leftActive = tabActive(s.tabs[i]);
        const bool rightActive = tabActive(s.tabs[i + 1]);
        if (leftActive || rightActive) {
            continue;
        }
        const float sepX = s.tabs[i].rect.X + s.tabs[i].rect.Width;
        const float sepInset = kTabDividerInset * k;
        const float sepY0 = sepInset;
        const float sepY1 = barH - sepInset;
        if (sepY1 > sepY0) {
            Pen sepPen(Color(120, 255, 255, 255), 1.0f * k);
            g.DrawLine(&sepPen, sepX, sepY0, sepX, sepY1);
        }
    }

    for (size_t i = 0; i < s.tabs.size(); ++i) {
        if (static_cast<int>(i) == dragTabIndex) {
            continue;  // 拖动中的标签最后单独绘制
        }
        const TabInfo& tab = s.tabs[i];
        const RectF& r = tab.rect;
        if (r.Width <= 0.0f || r.Height <= 0.0f) {
            continue;
        }

        const bool active = tabActive(tab);
        const bool hover = (s.hoverButton == kHitTab &&
                            s.hoverTab == static_cast<int>(i));

        if (active) {
            // 当前标签：两侧始终使用相同的下圆角样式，即使没有相邻未打开标签也不出现平直底部。
            const float inset = kTabTopInset * k;
            RectF visual(r.X, inset, r.Width, barH - inset);
            if (visual.Height > 0.0f) {
                GraphicsPath activePath;
                BuildTabBackgroundPath(activePath, visual, k);
                SolidBrush fillBrush(hover ? Color(80, 255, 255, 255)
                                           : Color(45, 255, 255, 255));
                g.FillPath(&fillBrush, &activePath);
                // 不绘制边框，只保留浅色背景填充
            }
        }

    }

    // 所有标签文字统一在所有背景绘制完之后再绘制，避免已打开标签延伸的背景盖住左侧未打开标签
    for (size_t i = 0; i < s.tabs.size(); ++i) {
        if (static_cast<int>(i) == dragTabIndex) {
            continue;
        }
        const TabInfo& tab = s.tabs[i];
        const RectF& r = tab.rect;
        if (r.Width <= 0.0f || r.Height <= 0.0f) {
            continue;
        }
        DrawTabTitle(g, s, tab, r, tabFont, chromeMode);
    }

    // 拖动中的标签：浮在光标处（原位置留空，其余标签已实时补位）
    if (tabDragging) {
        DrawFloatingTab(g, s, s.tabs[s.tabDragIndex], tabFont, chromeMode, barH);
    }

    // ---- 最小化键左侧：Win11 风格任务栏时钟（时间/日期两行）----
    RectF clockR;
    ComputeClockRect(s, clockR);
    DrawButtonHover(g, clockR, kHitClock, s.hoverButton);
    DrawClock(g, s, clockR);

    // ---- 右侧三个 Chrome 风格按钮 ----

    const Color glyph(200, 255, 255, 255);
    const Color glyphHover(255, 255, 255, 255);

    DrawButtonHover(g, minR, kHitMinimize, s.hoverButton);
    DrawMinimizeGlyph(g, minR, k,
                      s.hoverButton == kHitMinimize ? glyphHover : glyph);

    DrawButtonHover(g, maxR, kHitMaximize, s.hoverButton);
    DrawMaximizeGlyph(g, maxR, k, s.targetMaximized,
                      s.hoverButton == kHitMaximize ? glyphHover : glyph);

    DrawButtonHover(g, closeR, kHitClose, s.hoverButton);
    DrawCloseGlyph(g, closeR, k,
                   s.hoverButton == kHitClose ? glyphHover : glyph);
}

void PresentBar(AppState& s) {
    if (!s.bitmap || !s.hdcMem || !s.pvBits) {
        return;
    }

    Gdiplus::Rect lockRect(0, 0, s.width, s.height);
    BitmapData bitmapData{};
    const Status lockStatus = s.bitmap->LockBits(
        &lockRect, ImageLockModeRead, PixelFormat32bppPARGB, &bitmapData);
    if (lockStatus != Ok) {
        return;
    }

    auto* dst = static_cast<BYTE*>(s.pvBits);
    const auto* src = static_cast<const BYTE*>(bitmapData.Scan0);
    const size_t rowBytes = static_cast<size_t>(s.width) * 4u;
    for (int y = 0; y < s.height; ++y) {
        std::memcpy(dst + static_cast<size_t>(y) * rowBytes,
                    src + static_cast<size_t>(y) * static_cast<size_t>(bitmapData.Stride),
                    rowBytes);
    }
    s.bitmap->UnlockBits(&bitmapData);

    RECT rc{};
    GetWindowRect(s.hwnd, &rc);
    POINT ptDst{rc.left, rc.top};
    POINT ptSrc{0, 0};
    SIZE size{s.width, s.height};

    BLENDFUNCTION blend{};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;

    HDC screenDc = GetDC(nullptr);
    if (screenDc) {
        UpdateLayeredWindow(s.hwnd, screenDc, &ptDst, &size, s.hdcMem,
                            &ptSrc, 0, &blend, ULW_ALPHA);
        ReleaseDC(nullptr, screenDc);
    }
}

void DrawBarAndPresent(AppState& s) {
    DrawBar(s);
    PresentBar(s);
}

void ShowExitMenu(HWND hwnd) {
    POINT pt{};
    GetCursorPos(&pt);

    HMENU menu = CreatePopupMenu();
    if (!menu) {
        return;
    }
    AppendMenuW(menu, MF_STRING, kMenuExit, L"退出 DesktopTopBar");

    SetForegroundWindow(hwnd);
    const UINT flags = TPM_RIGHTBUTTON | TPM_LEFTALIGN | TPM_RETURNCMD | TPM_NONOTIFY;
    const int cmd = TrackPopupMenu(menu, flags, pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(menu);

    if (cmd == kMenuExit) {
        DestroyWindow(hwnd);
    }
}

// 窗口是否被系统 cloaked（隐藏）：最小化到后台的 UWP 应用、以及位于
// 其它虚拟桌面上的窗口都会保留原矩形但实际不可见。这类窗口不该被当成
// "占着屏幕"而让顶栏让位（否则切到别的虚拟桌面后顶栏会莫名消失）。
bool IsCloaked(HWND hwnd) {
    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked,
                                        sizeof(cloaked)))) {
        return cloaked != 0;
    }
    return false;
}

// 顶栏当前聚焦目标是否正与顶栏矩形重叠（= 需要让位）。
// 目标与顶栏同源：就是标签/最小化按钮操作的那个窗口（targetHwnd），
// 含"目标最小化后粘住"的语义。
// 不占屏面的目标（最小化 / 隐藏 / cloaked / 已销毁）一律视为不重叠：
// 它们没有可见矩形，顶栏不必为它们让位。
bool TargetOverlapsBar(AppState& s) {
    if (!s.hasTarget || !s.targetHwnd || !IsWindow(s.targetHwnd) ||
        s.targetHwnd == s.hwnd) {
        return false;
    }
    if (IsIconic(s.targetHwnd) || !IsWindowVisible(s.targetHwnd) ||
        IsCloaked(s.targetHwnd)) {
        return false;
    }
    RECT bar{};
    RECT target{};
    if (!GetWindowRect(s.hwnd, &bar) || !GetWindowRect(s.targetHwnd, &target)) {
        return false;
    }
    RECT inter{};
    return IntersectRect(&inter, &bar, &target) != FALSE;
}

// 顶栏层级对齐（幂等；只切层级与风格，不动位置/尺寸/可见性）：
//   0. 「保持顶栏」模式（keepTopEnabled）：恒置顶层，任何情况都不让位 ——
//      工作区已由 Dock 让出顶栏高度，最大化窗口不会压到顶栏；
//   1. Dock 联动展开（dockLinkExpanded）：置顶层，覆盖所有窗口 ——
//      包含当前聚焦窗口，此时不让位（Dock 展开是用户显式召唤）；
//   2. 默认 + 目标窗口与顶栏重叠：让位态 —— 摘掉置顶位，紧贴目标窗口
//      之下（被它盖住，不遮挡当前聚焦窗口），但仍高于其它非聚焦窗口
//      （只重叠一部分时，其余部分照常可见可点）；
//   3. 默认 + 不重叠（含无目标 / 目标最小化 / 目标在别的虚拟桌面）：
//      置顶层，覆盖除目标外的所有窗口。
// force=true 强制重设一次（启动、DPI 变化后确认风格位与 Z 序真的生效）；
// 状态未变且非 force 时直接返回 —— LOCATIONCHANGE 事件高频触发，不能每次都动窗口。
void ApplyBarLayer(AppState& s, bool force) {
    const bool yield =
        !s.keepTopEnabled && !s.dockLinkExpanded && TargetOverlapsBar(s);
    const HWND anchor = yield ? s.targetHwnd : nullptr;
    const int applied = yield ? 1 : 0;

    if (!force && applied == s.barLayerApplied &&
        (!yield || anchor == s.barAnchorHwnd)) {
        return;
    }
    s.barLayerApplied = applied;
    s.barYielding = yield;
    s.barAnchorHwnd = anchor;

    // 属主窗口无法独立置顶：带 Progman 属主时系统会把 HWND_TOPMOST 请求
    // 撤销并清掉 WS_EX_TOPMOST（实测）。顶栏现在全程是独立顶层窗口，
    // 因此统一脱离属主（WS_EX_TOOLWINDOW 依旧保证不进任务栏/Alt-Tab）。
    if (GetWindow(s.hwnd, GW_OWNER) != nullptr) {
        SetWindowLongPtrW(s.hwnd, GWLP_HWNDPARENT, 0);
    }

    const LONG_PTR style = GetWindowLongPtrW(s.hwnd, GWL_EXSTYLE);
    if (yield) {
        if ((style & WS_EX_TOPMOST) != 0) {
            // 先摘置顶位再定位：置顶窗口永远压在所有普通窗口之上，
            // 不摘掉就无法落到目标窗口下面
            SetWindowLongPtrW(s.hwnd, GWL_EXSTYLE, style & ~WS_EX_TOPMOST);
        }
        // SetWindowPos 的 hwndInsertAfter = "排在本窗口之前的窗口"，
        // 传目标窗口即表示顶栏紧贴它下面一位（不遮挡它）
        SetWindowPos(s.hwnd, anchor, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    } else {
        if ((style & WS_EX_TOPMOST) == 0) {
            SetWindowLongPtrW(s.hwnd, GWL_EXSTYLE, style | WS_EX_TOPMOST);
        }
        SetWindowPos(s.hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
}

// 低频自愈（时钟定时器每秒调用一次）：
//   让位态下若顶栏上方不再是锚点窗口（被别的窗口插了队，或系统重排了 Z 序），
//   重新贴回锚点之下；层级状态与目标窗口几何不一致时重新评估。
// 事件驱动正常时这里是空转（全部幂等判定，不动窗口）。
void HealBarLayer(AppState& s) {
    ApplyBarLayer(s, false);
    // 锚点自身是置顶窗口时不折腾：置顶窗口恒定压在非置顶窗口之上，
    // 顶栏（非置顶）本来就在它下面，反复 SetWindowPos 也贴不到它下面
    if (s.barYielding && s.barAnchorHwnd && IsWindow(s.barAnchorHwnd) &&
        (GetWindowLongPtrW(s.barAnchorHwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) == 0 &&
        GetWindow(s.hwnd, GW_HWNDPREV) != s.barAnchorHwnd) {
        SetWindowPos(s.hwnd, s.barAnchorHwnd, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
}

// ===== 「保持顶栏」模式 =====
// 模式语义与注册表定义见 keep_top.h；工作区（让出顶栏高度、视模式让出 Dock
// 高度带）由 Dock 统一写入，顶栏侧只负责两件事：
//   1. 层级：恒置顶、从不让位（ApplyBarLayer / WM_WINDOWPOSCHANGING 已按开关分支）；
//   2. 全屏让位：前台出现铺满本屏的应用（游戏全屏 / 全屏视频 / 演示模式）时
//      整体隐藏，退出全屏自动恢复 —— 用户要求"全屏除外"。
// 判定口径与 Dock 完全一致（fullscreen_guard.h + 只认本屏），避免两者分歧。

// 全屏让位复核：返回是否发生了变化（调用方据此决定要不要重绘）。
// immediateOnly=false 时按时间节流（默认 2s），避免每秒调 SHQueryUserNotificationState。
bool CheckKeepTopFullscreen(AppState& s, bool force) {
    if (!s.keepTopEnabled) {
        // 模式关闭：若此前因全屏隐藏过，恢复显示
        if (s.keepTopHidden) {
            s.keepTopHidden = false;
            ShowWindow(s.hwnd, SW_SHOWNOACTIVATE);
            ApplyBarLayer(s, true);
        }
        return false;
    }
    const ULONGLONG now = GetTickCount64();
    if (!force && now - s.lastKeepTopFullscreenCheck < kKeepTopFullscreenCheckMs) {
        return false;
    }
    s.lastKeepTopFullscreenCheck = now;

    const fsguard::Result r = fsguard::Detect();
    bool fullscreen = r.fullscreen;
    // 只认本屏（主屏）上的全屏应用：副屏全屏不影响主屏顶栏
    if (fullscreen && r.monitor) {
        const HMONITOR primary = fsguard::PrimaryMonitor();
        if (primary && r.monitor != primary) fullscreen = false;
    }
    // 顶栏自己在前台不算全屏（保险：Detect 已排除套件窗口）
    if (fullscreen && r.hwnd == s.hwnd) fullscreen = false;

    if (fullscreen == s.keepTopHidden) return false;  // 状态未变
    if (fullscreen) {
        s.keepTopHidden = true;
        ShowWindow(s.hwnd, SW_HIDE);  // 全屏应用期间顶栏整体不显示
    } else {
        s.keepTopHidden = false;
        ShowWindow(s.hwnd, SW_SHOWNOACTIVATE);
        ApplyBarLayer(s, true);  // 恢复显示后强制对齐一次层级（恒置顶）
    }
    return true;
}

// 应用「保持顶栏」开关（幂等）。调用点：启动读注册表、Dock 状态消息
// （wParam=2）、宿主托盘 kKeepTopRefreshMsg。
// 关闭时恢复默认层级策略（按 Dock 联动/聚焦目标重新判定）。
void SetKeepTopEnabled(AppState& s, bool enabled) {
    if (enabled == s.keepTopEnabled) {
        if (enabled) CheckKeepTopFullscreen(s, /*force=*/true);
        return;
    }
    s.keepTopEnabled = enabled;
    s.barLayerApplied = -1;  // 层级策略变化：强制重新应用一次
    if (!enabled && s.keepTopHidden) {
        // 关闭模式时若正被全屏藏起：恢复显示（默认策略下全屏本就压着顶栏）
        s.keepTopHidden = false;
        ShowWindow(s.hwnd, SW_SHOWNOACTIVATE);
    }
    ApplyBarLayer(s, true);
    if (enabled) CheckKeepTopFullscreen(s, /*force=*/true);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    AppState* s = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        s = static_cast<AppState*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(s));
    } else {
        s = reinterpret_cast<AppState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    switch (msg) {
    case WM_NCCREATE:
        if (s) {
            s->hwnd = hwnd;
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);

    case WM_CREATE: {
        if (!s) {
            return -1;
        }
        s->hwnd = hwnd;
        s->dpi = GetWindowDpi(hwnd);
        s->scale = static_cast<float>(s->dpi) / 96.0f;
        InstallVolumeHook(hwnd); // 常驻全局钩子：显式点击/Alt+Tab 检测 + 面板外点击收起
        // 标签刷新不设常驻定时器：WinEvent 事件驱动（见 WinEventProc），
        // 仅挂起态（新建窗口/抢前台）启用 200ms 短轮询、静止时 10s 低频兜底
        ChromeSyncStart(hwnd);   // Chrome 标签同步服务端（扩展连接用）
        // Win11 风格时钟：跟随系统"显示秒"设置，每秒检查文本变化
        s->clockShowSeconds = ReadShowSecondsSetting();
        UpdateClock(*s);
        SetTimer(hwnd, kClockTimerId, kClockTickMs, nullptr);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps{};
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_NCHITTEST:
        // 整条顶栏接收鼠标（顶栏在桌面层，点击不会覆盖/抢占任何窗口）
        return HTCLIENT;

    case WM_SETCURSOR:
        // 拖动标签重排时给一个左右箭头的光标反馈（其余情况保持默认箭头）
        if (s && s->tabDragging) {
            SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
            return TRUE;
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);

    case WM_WINDOWPOSCHANGING: {
        // 防御：任何 Z 序变化都强制回到本模式应处的层级 ——
        // 「保持顶栏」模式：恒置顶，绝不让位；
        // 默认让位态（目标窗口压着顶栏）：强制紧贴目标窗口之下，
        //   绝不覆盖当前聚焦窗口；
        // 其余情况（Dock 联动展开 / 目标未与顶栏重叠）：强制置顶，
        //   覆盖除当前聚焦窗口外的窗口。
        auto* wp = reinterpret_cast<WINDOWPOS*>(lParam);
        if ((wp->flags & SWP_NOZORDER) == 0) {
            const bool yieldNow = s && !s->keepTopEnabled &&
                                  !s->dockLinkExpanded && s->barYielding &&
                                  s->barAnchorHwnd &&
                                  IsWindow(s->barAnchorHwnd);
            if (yieldNow) {
                if (wp->hwndInsertAfter != s->barAnchorHwnd) {
                    wp->hwndInsertAfter = s->barAnchorHwnd;
                    wp->flags |= SWP_NOACTIVATE;
                }
            } else if (wp->hwndInsertAfter != HWND_TOPMOST) {
                wp->hwndInsertAfter = HWND_TOPMOST;
                wp->flags |= SWP_NOACTIVATE;
            }
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    case WM_MOUSEMOVE: {
        if (!s) {
            return 0;
        }

        // 拖动标签重排：按住标签横向拖过阈值后，被拖标签与相邻标签的重叠
        // 达到后者宽度的 20% 就交换位置（实时让位），被拖标签浮在光标处
        if (s->tabPressIndex >= 0) {
            POINT cur{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            if (!s->tabDragging) {
                const int threshold =
                    (std::max)(kTabDragThreshold,
                               MulDiv(kTabDragThreshold, s->dpi, 96));
                const int adx = cur.x - s->tabPressPt.x;
                const int ady = cur.y - s->tabPressPt.y;
                if ((adx < 0 ? -adx : adx) < threshold &&
                    (ady < 0 ? -ady : ady) < threshold) {
                    return 0;  // 阈值内：还算"点击"，不做任何移动
                }
                s->tabDragging = true;
            }

            // 按身份重新定位被拖标签：拖动途中列表可能被刷新（扩展推送 /
            // 窗口事件），只认索引会拖错标签
            int idx = -1;
            for (size_t i = 0; i < s->tabs.size(); ++i) {
                const TabInfo& t = s->tabs[i];
                const bool same =
                    s->tabDragIsChrome
                        ? (t.isChrome && t.chromeTabId == s->tabDragChromeId)
                        : (!t.isChrome && t.hwnd == s->tabDragHwnd);
                if (same) {
                    idx = static_cast<int>(i);
                    break;
                }
            }
            if (idx < 0) {
                CancelTabDrag(*s);
                if (GetCapture() == hwnd) {
                    ReleaseCapture();
                }
                DrawBarAndPresent(*s);
                return 0;
            }
            s->tabPressIndex = idx;
            s->tabDragIndex = idx;
            s->tabDragPt = cur;
            // 浮起标签的"身体"跟着光标走：判定与绘制都用这个位置
            s->tabDragBodyX = static_cast<float>(cur.x) - s->tabDragGrabDx;

            // 换位判定：前进只要身体与相邻标签的重叠达到后者宽度的
            // kTabDragSwapRatio（20%）；回退（把刚被挤开的标签放回去）只要
            // 方向反过来、越过一个防抖量，且身体仍覆盖着那个标签
            // kTabDragReturnRatio（80%）的宽度就立刻回位。
            std::vector<float> slotX;
            std::vector<float> slotW;
            slotX.reserve(s->tabs.size());
            slotW.reserve(s->tabs.size());
            for (const auto& t : s->tabs) {
                slotX.push_back(t.rect.X);
                slotW.push_back(t.rect.Width);
            }
            const bool draggingPinned = s->tabs[idx].isChrome &&
                                        s->tabs[idx].chromePinned;
            const int lo = TabDragLo(*s, draggingPinned);
            const int hi = TabDragHi(*s, draggingPinned);
            const float jitterPx = static_cast<float>(
                (std::max)(kTabDragThreshold,
                           MulDiv(kTabDragThreshold, s->dpi, 96)));
            const int next = wsproto::TabDragNextIndex(
                slotX, slotW, idx, s->tabDragOriginIndex, s->tabDragBodyX,
                s->tabs[idx].rect.Width, s->tabDragAnchorX,
                kTabDragSwapRatio, kTabDragReturnRatio, jitterPx, lo, hi);
            if (next != idx && next >= 0 &&
                next < static_cast<int>(s->tabs.size())) {
                std::swap(s->tabs[idx], s->tabs[next]);
                s->tabDragIndex = next;
                s->tabPressIndex = next;
                // 交换后各标签的坑位要立刻重排：下一帧的换位判定、以及下面的
                // 锚点取值都依赖 rect（元素被搬动后还带着旧 rect）
                LayoutTabs(*s, s->tabs, nullptr);
                s->tabDragAnchorX = s->tabDragBodyX;
                if (s->hoverTab >= 0) {
                    s->hoverTab = next;
                }
            }
            DrawBarAndPresent(*s);
            return 0;
        }

        // 拖动中：移动目标窗口（等同标题栏拖动）
        if (s->dragWindow && s->dragHwnd && IsWindow(s->dragHwnd)) {
            POINT cur{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ClientToScreen(hwnd, &cur);
            const int dx = cur.x - s->dragStartPt.x;
            const int dy = cur.y - s->dragStartPt.y;
            if (!s->dragMoving) {
                // 移动阈值：超过才真正开始拖动，避免误触
                const int threshold = (std::max)(4, MulDiv(4, s->dpi, 96));
                const int adx = dx < 0 ? -dx : dx;
                const int ady = dy < 0 ? -dy : dy;
                if (adx < threshold && ady < threshold) {
                    return 0;
                }
                s->dragMoving = true;
                RECT rc{};
                GetWindowRect(s->dragHwnd, &rc);
                const bool wasFilled = TargetIsFilled(*s, s->dragHwnd);
                if (IsZoomed(s->dragHwnd) || wasFilled) {
                    // 最大化/填充窗口拖动：先还原，抓点按比例换算（同 Windows 标题栏）
                    if (wasFilled) {
                        UnfillTarget(*s, s->dragHwnd);
                    } else {
                        ShowWindow(s->dragHwnd, SW_RESTORE);
                    }
                    RECT rc2{};
                    GetWindowRect(s->dragHwnd, &rc2);
                    const float rx =
                        rc.right > rc.left
                            ? static_cast<float>(s->dragStartPt.x - rc.left) /
                                  static_cast<float>(rc.right - rc.left)
                            : 0.5f;
                    const float ry =
                        rc.bottom > rc.top
                            ? static_cast<float>(s->dragStartPt.y - rc.top) /
                                  static_cast<float>(rc.bottom - rc.top)
                            : 0.5f;
                    s->dragOffset.x = static_cast<LONG>(
                        rx * static_cast<float>(rc2.right - rc2.left));
                    s->dragOffset.y = static_cast<LONG>(
                        ry * static_cast<float>(rc2.bottom - rc2.top));
                    s->targetMaximized = false;
                    DrawBarAndPresent(*s);  // 更新右侧最大化按钮字形
                } else {
                    s->dragOffset.x = s->dragStartPt.x - rc.left;
                    s->dragOffset.y = s->dragStartPt.y - rc.top;
                }
            }
            SetWindowPos(s->dragHwnd, nullptr, cur.x - s->dragOffset.x,
                         cur.y - s->dragOffset.y, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }

        if (!s->trackingMouse) {
            TRACKMOUSEEVENT tme{};
            tme.cbSize = sizeof(tme);
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = hwnd;
            TrackMouseEvent(&tme);
            s->trackingMouse = true;
        }
        POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        const int tabIndex = HitTestTab(*s, pt.x, pt.y);
        const ButtonHit hit = HitTestButton(*s, pt.x, pt.y);
        const int newHoverTab = (hit == kHitTab) ? tabIndex : -1;

        if (hit != s->hoverButton || newHoverTab != s->hoverTab) {
            s->hoverButton = hit;
            s->hoverTab = newHoverTab;
            DrawBarAndPresent(*s);
        }
        return 0;
    }

    case WM_CAPTURECHANGED:
        // 捕获被系统或其他窗口夺走时结束拖动，避免状态残留
        if (s && s->dragWindow) {
            s->dragWindow = false;
            s->dragMoving = false;
            s->dragHwnd = nullptr;
        }
        if (s && s->tabPressIndex >= 0) {
            CancelTabDrag(*s);
            DrawBarAndPresent(*s);
        }
        return 0;

    case WM_MOUSELEAVE:
        if (s) {
            s->trackingMouse = false;
            if (!s->volumeDragging &&
                (s->hoverButton != kHitNone || s->hoverTab != -1)) {
                s->hoverButton = kHitNone;
                s->hoverTab = -1;
                DrawBarAndPresent(*s);
            }
        }
        return 0;

    case WM_LBUTTONDOWN: {
        if (!s) {
            return 0;
        }
        POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        const int tabIndex = HitTestTab(*s, pt.x, pt.y);
        const ButtonHit hit = HitTestButton(*s, pt.x, pt.y);
        const bool chromeMode = ChromeSyncMode(*s);

        // 按住空白处：进入标题栏式拖动（移动阈值内只等待，不移动）
        if (hit == kHitNone && s->hasTarget && s->targetHwnd &&
            IsWindow(s->targetHwnd) && !IsIconic(s->targetHwnd)) {
            s->dragWindow = true;
            s->dragMoving = false;
            s->dragHwnd = s->targetHwnd;
            s->dragStartPt = pt;
            ClientToScreen(hwnd, &s->dragStartPt);
            SetCapture(hwnd);
        }

        // 按住标签：记下身份与抓点，超过阈值就变成拖动重排（见 WM_MOUSEMOVE）。
        // 阈值以内松手依旧只是"点击切换标签"（点击本身已在下面的 switch 里
        // 立即生效，与 Chrome 一样：按下即激活，拖动即重排）
        if (hit == kHitTab && tabIndex >= 0 &&
            tabIndex < static_cast<int>(s->tabs.size())) {
            const TabInfo& pressed = s->tabs[tabIndex];
            s->tabPressIndex = tabIndex;
            s->tabDragging = false;
            s->tabDragIndex = tabIndex;
            s->tabDragIsChrome = pressed.isChrome;
            s->tabDragChromeId = pressed.chromeTabId;
            s->tabDragHwnd = pressed.hwnd;
            s->tabPressPt = pt;
            s->tabDragPt = pt;
            float grab = static_cast<float>(pt.x) - pressed.rect.X;
            if (grab < 0.0f) {
                grab = 0.0f;
            }
            if (grab > pressed.rect.Width) {
                grab = pressed.rect.Width;
            }
            s->tabDragGrabDx = grab;
            // 身体从标签自己的坑位出发；锚点也在这里（第一次换位需要 20% 的重叠）
            s->tabDragBodyX = pressed.rect.X;
            s->tabDragAnchorX = pressed.rect.X;
            s->tabDragOriginIndex = tabIndex;  // "原来的位置"：朝它移动时用回退阈值
            SetCapture(hwnd);
        }

        switch (hit) {
        case kHitVolume:
            SetVolumeOpen(*s, !s->volumeOpen);
            break;
        case kHitClock:
            // 与 Win11 任务栏右端「网络/音量/电池」三合一按钮一致：
            // 左键打开/收起系统「快速设置」面板（系统快捷键 Win+A）；
            // 面板打开后由对齐定时器把它挪到顶栏正下方（上缘紧贴顶栏下缘）
            ToggleQuickSettings(*s);
            break;
        case kHitTab:
            if (tabIndex >= 0 && tabIndex < static_cast<int>(s->tabs.size())) {
                const TabInfo& tab = s->tabs[tabIndex];
                if (chromeMode && tab.isChrome) {
                    // Chrome 同步模式：点击标签 = 切换（关闭用中键）
                    ChromeSyncSendActivateTab(tab.chromeTabId);
                    break;
                }
                HWND tabHwnd = tab.hwnd;
                // 任务栏中看不见的窗口不可聚焦（子进程消息窗 / 托盘图标应用的
                // 驻留窗 / 已藏起来的窗口）：既不能聚焦，也不能被顶栏收作目标，
                // 否则最小化/关闭等按钮会落到一个用户根本看不见的窗口上。
                // 列表是上一次枚举的结果，窗口可能在枚举之后才隐藏，
                // 故此处按同一判据复查；刷新一次后该残留标签即被剔除。
                if (!IsTaskbarWindow(tabHwnd)) {
                    RefreshTabs(*s);
                    DrawBarAndPresent(*s);
                    break;
                }
                if (IsIconic(tabHwnd)) {
                    ShowWindow(tabHwnd, SW_RESTORE);
                }
                ForceForegroundWindow(tabHwnd);
                ApplyTargetInfo(*s, tabHwnd);
                s->targetSticky = false;
                DrawBarAndPresent(*s);
            }
            break;
        case kHitMinimize:
        case kHitMaximize:
        case kHitClose:
            if (s->hasTarget && s->targetHwnd) {
                HitButton(s->hwnd, s->targetHwnd, hit);
            }
            break;
        default:
            break;
        }
        return 0;
    }

    case WM_LBUTTONDBLCLK: {
        // 双击顶栏空白处：让目标窗口填充"顶栏以外"的区域（顶栏保持可见），
        // 再次双击还原到填充前的位置。不同于右侧最大化键的系统最大化：
        // 那个会把整条顶栏一起盖住，本手势的目的正是让顶栏不被盖住。
        // 双击落在按钮上时不触发（第二次按下已由各自逻辑处理）。
        if (!s) {
            return 0;
        }
        POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        const ButtonHit hit = HitTestButton(*s, pt.x, pt.y);
        if (hit == kHitNone && s->hasTarget && s->targetHwnd &&
            IsWindow(s->targetHwnd)) {
            ToggleFillTarget(*s, s->targetHwnd);
            // 刷新最大化状态（右侧按钮字形与后续布局）：填充路径可能顺带
            // 退出了系统最大化（SW_RESTORE），字形要跟着变回"最大化"
            s->targetMaximized = IsZoomed(s->targetHwnd) != FALSE;
            DrawBarAndPresent(*s);
        }
        return 0;
    }

    case WM_LBUTTONUP: {
        if (!s) {
            return 0;
        }
        if (s->volumeDragging) {
            s->volumeDragging = false;
            ReleaseCapture();
            DrawBarAndPresent(*s);
        }
        if (s->dragWindow) {
            s->dragWindow = false;
            s->dragMoving = false;
            s->dragHwnd = nullptr;
            ReleaseCapture();
        }
        // 松手：拖动过就落定新顺序（Chrome 模式还要把新位置下发给浏览器），
        // 只是点了一下则什么都不用做（按下时已经切换完成了）
        if (s->tabPressIndex >= 0) {
            const bool dragged = s->tabDragging;
            const int movedIndex = s->tabDragIndex;
            CancelTabDrag(*s);
            if (GetCapture() == hwnd) {
                ReleaseCapture();
            }
            if (dragged) {
                CommitTabReorder(*s, movedIndex);
                DrawBarAndPresent(*s);
            }
        }
        return 0;
    }

    case WM_MBUTTONDOWN: {
        // 中键点击标签：普通应用关闭对应窗口；Chrome 同步模式关闭对应标签
        if (!s) {
            return 0;
        }
        POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        const int tabIndex = HitTestTab(*s, pt.x, pt.y);
        if (tabIndex >= 0 && tabIndex < static_cast<int>(s->tabs.size())) {
            const TabInfo& tab = s->tabs[tabIndex];
            if (ChromeSyncMode(*s) && tab.isChrome) {
                ChromeSyncSendCloseTab(tab.chromeTabId);
            } else {
                PostMessageW(tab.hwnd, WM_CLOSE, 0, 0);
            }
        }
        return 0;
    }

    case WM_RBUTTONUP: {
        // 右键标签：普通应用在该标签右侧打开应用新窗口；
        // Chrome 同步模式 = 新建标签页；右键空白处仍显示退出菜单
        if (!s) {
            return 0;
        }
        POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        // 右键时钟：打开/收起系统「日期和时间」日历浮出窗口
        // （左键已改为系统快速设置面板，日历入口保留在右键）
        if (HitTestButton(*s, pt.x, pt.y) == kHitClock) {
            ToggleCalendarFlyout();
            return 0;
        }
        const int tabIndex = HitTestTab(*s, pt.x, pt.y);
        if (tabIndex >= 0 && tabIndex < static_cast<int>(s->tabs.size())) {
            const TabInfo& tab = s->tabs[tabIndex];
            if (ChromeSyncMode(*s) && tab.isChrome) {
                ChromeSyncSendNewTab();
            } else {
                OpenNewAppWindow(*s, tab.hwnd);
            }
        } else {
            ShowExitMenu(hwnd);
        }
        return 0;
    }

    case kChromeSyncMsg: {
        // 扩展推送的标签数据（JSON，lParam = new wchar_t[]，这里负责释放）
        if (!s) {
            if (lParam) {
                delete[] reinterpret_cast<wchar_t*>(lParam);
            }
            return 0;
        }
        const wchar_t* json = reinterpret_cast<const wchar_t*>(lParam);
        if (json) {
            const wsproto::JsonValue jsonValue = wsproto::JsonParse(json);
            if (wsproto::ChromeSyncApplyMessage(s->chromeSync, jsonValue)) {
                if (RefreshTabs(*s)) {
                    DrawBarAndPresent(*s);
                }
            }
        }
        delete[] reinterpret_cast<wchar_t*>(lParam);
        return 0;
    }

    case kChromeSyncStateMsg: {
        // 扩展连接 / 断开。
        // 连接：立即以现有数据刷新（首帧 hello/windowFocused 稍后到达）。
        // 断开：先进入优雅期保留最后一批浏览器标签（避免短暂掉线时顶栏
        //       闪回窗口枚举、看起来"只剩当前标签页"）；优雅期内未重连
        //       才清空并回退为窗口枚举。
        if (!s) {
            return 0;
        }
        const bool connected = wParam != 0;
        if (connected) {
            KillTimer(hwnd, kChromeSyncGraceTimerId);
            if (!s->chromeSync.connected) {
                s->chromeSync.connected = true;
                if (RefreshTabs(*s)) {
                    DrawBarAndPresent(*s);
                }
            }
            return 0;
        }
        if (!s->chromeSync.connected) {
            return 0;  // 已经是断开态，无需重复处理
        }
        if (s->chromeSync.tabs.empty()) {
            // 本来就没有同步数据（如刚启动就连不上）：直接回退，不挂优雅期
            s->chromeSync.connected = false;
            s->chromeSync.windowId = 0;
            if (RefreshTabs(*s)) {
                DrawBarAndPresent(*s);
            }
        } else {
            SetTimer(hwnd, kChromeSyncGraceTimerId, kChromeSyncGraceMs, nullptr);
        }
        return 0;
    }

    case kCloseVolumeMsg:
        // 全局鼠标钩子检测到点击面板外（桌面/其他窗口）时请求收起
        if (s && s->volumeOpen) {
            SetVolumeOpen(*s, false);
        }
        return 0;

    case kUserSwitchMsg:
        // 用户显式点击/Alt+Tab 其他窗口：立即解除粘住并切换目标
        if (s) {
            UpdateTarget(*s);
            DrawBarAndPresent(*s);
        }
        return 0;

    case kDockFocusMsg: {
        // Dock 点击"已打开的应用"后显式要求聚焦：这是用户发起的应用切换
        // （点击发生在套件窗口上，不被目标粘住逻辑视为显式选择），
        // 直接解除粘住并应用目标，不必等 EVENT_SYSTEM_FOREGROUND。
        if (!s) {
            return 0;
        }
        HWND focus = reinterpret_cast<HWND>(lParam);
        if (focus && IsWindow(focus) && IsControlTarget(focus, hwnd)) {
            s->targetSticky = false;
            g_lastClickWindow = nullptr;
            g_altTabPressed = false;
            ApplyTargetInfo(*s, focus);
        } else {
            // 无直接句柄（异步托盘触发）或窗口已失效：解除粘住。
            // 若旧目标仍处于最小化（粘住的根因），仅解粘会在下一次
            // UpdateTarget 重新粘住它——须一并清空目标，让托盘应用
            // 窗口出现后由 EVENT_SYSTEM_FOREGROUND 自然接管；旧目标
            // 正常（非最小化）时保留现状，不制造无谓的标签清空。
            const bool wasStuck =
                s->targetSticky && s->targetHwnd && IsWindow(s->targetHwnd) &&
                IsIconic(s->targetHwnd);
            s->targetSticky = false;
            g_lastClickWindow = nullptr;
            g_altTabPressed = false;
            if (wasStuck) {
                s->targetHwnd = nullptr;
                s->hasTarget = false;
                s->targetMaximized = false;
                s->tabs.clear();
                s->hoverButton = kHitNone;
                s->hoverTab = -1;
                if (s->volumeTargetHwnd != nullptr) {
                    s->volumeTargetHwnd = nullptr;
                    ResolveVolumeSession(*s);
                }
                NotifyDockTargetChanged(nullptr);
            }
        }
        DrawBarAndPresent(*s);
        return 0;
    }

    case kDockVolumePanelMsg:
        // 左上角触发角点击：展开/收起音量面板（当前目标应用）。
        // 与点击顶栏音量按钮同语义；顶栏自己的 LL 钩子已跳过角区，
        // 只此一条路径生效，不会双 toggle。
        if (s) {
            SetVolumeOpen(*s, !s->volumeOpen);
        }
        return 0;

    // ← Dock 查询：顶栏当前目标应用是否就是 wParam 那个窗口所属的应用。
    // 用于让 Dock 的"点图标 = 最小化 / 聚焦"与用户看到的聚焦指示（顶栏目标）
    // 保持一致，避免目标被最小化后前台自动转移造成的判断分歧。
    case kDockFocusQueryMsg: {
        if (!s || !s->hasTarget || !s->targetHwnd ||
            !IsWindow(s->targetHwnd)) {
            return 0;
        }
        HWND probe = reinterpret_cast<HWND>(wParam);
        if (!probe || !IsWindow(probe)) {
            return 0;
        }
        return IsSameApplication(probe, s->targetHwnd) ? 1 : 0;
    }

    // ← Dock 查询：顶栏当前焦点窗口。Dock 用它按自己的 exe 路径分组口径
    // 映射应用 key（与上面只回 Yes/No 的焦点一致性查询是两件事）。
    case kDockTargetQueryMsg: {
        if (!s || !s->hasTarget || !s->targetHwnd ||
            !IsWindow(s->targetHwnd)) {
            return 0;
        }
        return reinterpret_cast<LRESULT>(s->targetHwnd);
    }

    case kDockStateMsg: {
        // Dock 顶栏联动：自动收起开启期间，Dock 展开 → 顶栏与 Dock 一样
        // 提升为置顶层并覆盖所有窗口（含当前聚焦窗口，不让位）；
        // Dock 收起 → 回到默认策略：只不遮挡当前聚焦窗口。
        // 自动收起关闭 / Dock 退出时收到 0 → 默认策略（行为不变）。
        // wParam = 2 = 「保持顶栏」模式（恒置顶层 + 从不让位 + 全屏让位），
        // 优先级高于联动：Dock 是模式开关的消息中枢（自己菜单 + 宿主托盘都
        // 经它转发）；Dock 退出时回落到本地注册表读数，用户勾的勾不会掉。
        if (!s) {
            return 0;
        }
        const int state = static_cast<int>(wParam);
        if (state == 2) {
            if (!s->keepTopEnabled) SetKeepTopEnabled(*s, true);
            return 0;
        }
        if (s->keepTopEnabled) {
            const bool regOn = keeptop::IsEnabledFromRegistry();
            if (regOn) return 0;  // Dock 说"未开启"但注册表仍勾选：以注册表为准
            SetKeepTopEnabled(*s, false);
        }
        const bool expanded = state != 0;
        if (expanded == s->dockLinkExpanded) {
            return 0;
        }
        s->dockLinkExpanded = expanded;
        ApplyBarLayer(*s, true);  // 模式切换：强制对齐一次（含置顶位）
        return 0;
    }

    // ← 宿主托盘菜单「保持顶栏」：重读注册表并立即生效
    case kKeepTopRefreshMsg: {
        if (!s) {
            return 0;
        }
        const bool enabled = (wParam == 1) ? true
                            : (wParam == 2) ? false
                                            : keeptop::IsEnabledFromRegistry();
        SetKeepTopEnabled(*s, enabled);
        return 0;
    }

    case WM_TIMER:
        if (!s) {
            return 0;
        }
        if (wParam == kTabRefreshTimerId) {
            // 挂起态短轮询：新建窗口待插入 / 抢前台重试
            if (RefreshTabs(*s)) {
                DrawBarAndPresent(*s);
            }
        } else if (wParam == kTargetRetryTimerId) {
            // 前台窗口创建中的延迟复查：窗口应已可见
            KillTimer(hwnd, kTargetRetryTimerId);
            UpdateTarget(*s);
            DrawBarAndPresent(*s);
            // 前台窗口仍在创建中（不可见）：继续复查，最多 kMaxTargetRetry 次
            const HWND fg = GetForegroundWindow();
            if (fg && IsWindow(fg) && !IsWindowVisible(fg) &&
                ++s->targetRetryCount < kMaxTargetRetry) {
                SetTimer(hwnd, kTargetRetryTimerId, 150, nullptr);
            }
        } else if (wParam == kTabRefreshDebounceTimerId) {
            // 事件驱动的标签刷新（抖动合并后）
            KillTimer(hwnd, kTabRefreshDebounceTimerId);
            if (RefreshTabs(*s)) {
                DrawBarAndPresent(*s);
            }
        } else if (wParam == kSlowRefreshTimerId) {
            // 低频兜底自检：事件驱动正常时被刷新重置，很少触发
            KillTimer(hwnd, kSlowRefreshTimerId);
            if (RefreshTabs(*s)) {
                DrawBarAndPresent(*s);
            }
        } else if (wParam == kChromeSyncGraceTimerId) {
            // 扩展断连优雅期结束：仍断线才丢弃同步标签、回退窗口枚举
            KillTimer(hwnd, kChromeSyncGraceTimerId);
            if (!g_chromeServer.connected.load() && s->chromeSync.connected) {
                s->chromeSync.connected = false;
                s->chromeSync.tabs.clear();
                s->chromeSync.windowId = 0;
                if (RefreshTabs(*s)) {
                    DrawBarAndPresent(*s);
                }
            }
        } else if (wParam == kQuickSettingsAlignTimerId) {
            // 系统快速设置面板打开期间：持续把面板上缘对齐顶栏下缘；
            // 面板关闭即在此停表（零轮询）
            if (s) {
                TickQuickSettingsAlign(*s);
            }
        } else if (wParam == kVolumeRetryTimerId) {
            // 目标进程的音频会话晚到（先无声后出声）：面板展开期间补解析。
            // 解析成功即停；成功时刷新顶栏与音量面板，滑条/静音立即恢复可用
            if (s->volumeReady || !s->volumeOpen) {
                KillTimer(hwnd, kVolumeRetryTimerId);
                return 0;
            }
            ResolveVolumeSession(*s);
            if (s->volumeReady) {
                DrawBarAndPresent(*s);
                if (s->volumePanelHwnd && s->panelBitmap) {
                    Graphics g(s->panelBitmap);
                    DrawVolumePanelContent(g, *s);
                    PresentVolumePanel(*s);
                }
            }
        } else if (wParam == kClockTimerId) {
            // 层级低频自愈：让位态下被别的窗口插队时贴回目标窗口之下；
            // 事件驱动正常时全部幂等判定，不动窗口
            HealBarLayer(*s);
            // 「保持顶栏」模式：全屏应用（游戏全屏等）进出复核（内部按 2s
            // 节流；模式关闭时是空转的快速返回）
            CheckKeepTopFullscreen(*s, /*force=*/false);
            // 时钟：每秒检查一次，仅时间/日期文本变化时重绘
            if (UpdateClock(*s)) {
                DrawBarAndPresent(*s);
            }
        }
        return 0;

    case WM_DPICHANGED: {
        if (!s) {
            return 0;
        }
        const UINT newDpi = HIWORD(wParam);
        const auto* suggested = reinterpret_cast<RECT*>(lParam);
        s->dpi = static_cast<int>(newDpi);
        s->scale = static_cast<float>(s->dpi) / 96.0f;

        const int h = WindowHeight(*s);
        const int w = suggested ? (suggested->right - suggested->left) : s->width;
        const int x = suggested ? suggested->left : 0;
        const int y = suggested ? suggested->top : 0;

        // 先按建议矩形落位，再强制对齐层级：置顶/让位风格位与 Z 序
        // 在 DPI 迁移后必须重设一次（窗口可能被系统塞回默认层）
        SetWindowPos(hwnd, HWND_TOP, x, y, w, h,
                     SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        ApplyBarLayer(*s, true);
        if (CreateBacking(*s, w, h)) {
            DrawBarAndPresent(*s);
        }
        return 0;
    }

    case WM_DESTROY:
        KillTimer(hwnd, kTabRefreshTimerId);
        KillTimer(hwnd, kTargetRetryTimerId);
        KillTimer(hwnd, kTabRefreshDebounceTimerId);
        KillTimer(hwnd, kSlowRefreshTimerId);
        KillTimer(hwnd, kVolumeRetryTimerId);
        KillTimer(hwnd, kClockTimerId);
        KillTimer(hwnd, kChromeSyncGraceTimerId);
        KillTimer(hwnd, kQuickSettingsAlignTimerId);
        ChromeSyncStop();  // 先停网络线程（会向本窗口发状态消息，需在窗口销毁前）
        UninstallVolumeHook();
        if (s && (s->hasTarget || s->targetHwnd != nullptr)) {
            NotifyDockTargetChanged(nullptr);
        }
        if (s) {
            if (s->volumePanelHwnd) {
                DestroyWindow(s->volumePanelHwnd);
                s->volumePanelHwnd = nullptr;
            }
            DestroyPanelBacking(*s);
            DestroyBacking(*s);
        }
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace

// ============================== 组件接口 ==============================

std::atomic<HWND> g_topbarHwnd{nullptr};

DWORD WINAPI TopbarThreadProc(LPVOID param) {
    const HINSTANCE hInstance = static_cast<HINSTANCE>(param);

    EnableDpiAwareness();

    // Core Audio（音量合成器）需要 COM；失败不致命，仅音量功能不可用
    HRESULT comInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    ULONG_PTR gdiplusToken = 0;
    GdiplusStartupInput gdiplusStartupInput;
    if (GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, nullptr) != Ok) {
        if (SUCCEEDED(comInit)) CoUninitialize();
        return 1;
    }

    HDC screenDc = GetDC(nullptr);
    const int systemDpi = screenDc ? GetDeviceCaps(screenDc, LOGPIXELSX) : 96;
    if (screenDc) {
        ReleaseDC(nullptr, screenDc);
    }

    // 顶栏固定在主工作区顶部，横跨全宽
    RECT workArea{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
    const int width = workArea.right - workArea.left;
    const int height = MulDiv(kBaseTabHeight, systemDpi, 96);
    const int posX = workArea.left + (workArea.right - workArea.left - width) / 2;
    const int posY = workArea.top;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;  // CS_DBLCLKS：双击空白处填充顶栏以外区域
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kWindowClass;

    if (!RegisterClassExW(&wc)) {
        if (GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            // 组件在进程内被重启（关闭后再次打开）时类已注册，视为成功
            GdiplusShutdown(gdiplusToken);
            if (SUCCEEDED(comInit)) CoUninitialize();
            return 1;
        }
    }

    // Chrome 标签同步服务端需要 WinSock；失败只禁用该功能
    WSADATA wsaData{};
    const bool wsaOk = WSAStartup(MAKEWORD(2, 2), &wsaData) == 0;

    AppState state;
    HWND hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        kWindowClass,
        L"DesktopTopBar",
        WS_POPUP,
        posX, posY, width, height,
        nullptr, nullptr, hInstance, &state);

    if (!hwnd) {
        GdiplusShutdown(gdiplusToken);
        if (SUCCEEDED(comInit)) CoUninitialize();
        if (wsaOk) WSACleanup();
        return 1;
    }

    g_topbarHwnd.store(hwnd);  // 先发布窗口句柄，宿主可立即隐藏/关闭

    if (!CreateBacking(state, width, height)) {
        GdiplusShutdown(gdiplusToken);
        if (SUCCEEDED(comInit)) CoUninitialize();
        if (wsaOk) WSACleanup();
        g_topbarHwnd.store(nullptr);
        return 1;
    }

    UpdateTarget(state);
    DrawBarAndPresent(state);

    // Dock 顶栏联动：启动即查询 Dock 当前展开态，联动展开时顶栏以置顶层
    // 启动并覆盖所有窗口。Dock 未运行/未应答 → 走默认策略。
    // 「保持顶栏」开关先按注册表落地（用户勾选的模式必须重启后依然生效），
    // 再由 Dock 的应答/推送校正（返回值 2 = 模式开启）。
    state.keepTopEnabled = keeptop::IsEnabledFromRegistry();
    {
        HWND dock = FindWindowW(L"DesktopDockWindow", nullptr);
        if (dock && IsWindow(dock)) {
            DWORD_PTR linkState = 0;
            if (SendMessageTimeoutW(dock, kDockStateQueryMsg, 0, 0,
                                    SMTO_ABORTIFHUNG | SMTO_BLOCK, 500,
                                    &linkState) != 0) {
                const int st = static_cast<int>(linkState);
                if (st == 2) {
                    state.keepTopEnabled = true;
                }
                state.dockLinkExpanded = (st == 1);
            }
        }
    }
    // 启动强制对齐一次层级：默认策略下要么置顶（只不遮挡当前聚焦窗口），
    // 要么（启动时前台窗口已压着顶栏）直接让位到它下面
    ApplyBarLayer(state, true);

    ShowWindow(hwnd, SW_SHOWNOACTIVATE);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    GdiplusShutdown(gdiplusToken);
    if (SUCCEEDED(comInit)) {
        CoUninitialize();
    }
    if (wsaOk) {
        WSACleanup();
    }
    g_topbarHwnd.store(nullptr);  // 窗口已销毁，线程即将退出
    return static_cast<DWORD>(msg.wParam);
}
