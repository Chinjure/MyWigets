// DesktopLauncher - 类 iOS / Android 桌面应用管理组件
//
// 功能：
//   - 固定网格排列应用图标
//   - 支持把 .exe / .lnk 从资源管理器拖入组件添加应用
//   - 单击图标打开应用
//   - 组件内拖动应用排序；应用拖到应用上释放可创建文件夹
//   - 打开文件夹后在组件内显示文件夹子面板
//   - 文件夹内应用可拖出，仅剩一个应用时自动解散文件夹
//   - 空白区域左右拖动翻页
//   - 拖动应用到右边缘停留可切页/新建页；左边缘停留可切上一页
//   - 顶部手柄可拖动整个组件；右键菜单可移除应用或退出
//   - 数据保存在 HKCU\Software\DesktopLauncher

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <commctrl.h>
#include <commoncontrols.h>
#include <objidl.h>
#include <gdiplus.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <map>
#include <string>
#include <vector>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "ole32.lib")

using namespace Gdiplus;

namespace {

constexpr wchar_t kWindowClass[] = L"DesktopLauncherWindow";
constexpr wchar_t kMutexName[] = L"Local\\DesktopLauncher_SingleInstance";
constexpr wchar_t kRegPath[] = L"Software\\DesktopLauncher";

constexpr int kBaseWidth = 420;
constexpr int kBaseHeight = 720; // 高度翻倍（360 -> 720），主网格容纳更多行
constexpr int kDefaultMargin = 32;
constexpr int kCols = 4;
constexpr int kRows = 6;
constexpr int kSlots = kCols * kRows;
constexpr int kMaxPages = 16;
constexpr int kMaxFolderApps = 12; // 文件夹子面板一屏 3x4
constexpr int kFolderCols = 3;
constexpr int kFolderRows = 4;
constexpr float kFolderPanelHeight = 306.0f; // 文件夹面板固定高度（不随组件变高）
constexpr UINT kDrawTimerId = 1;
constexpr UINT kDrawIntervalMs = 16; // 约 60fps，保证翻页动画流畅
constexpr UINT kPageAnimMs = 240;
constexpr UINT kRenameCommand = WM_APP + 3;

// 点击按压动画（安卓风格：按下缩小，松手弹簧回弹带过冲）
constexpr float kPressScale = 0.85f;      // 按下时图标缩放
constexpr UINT kPressMs = 90;             // 按下动画时长
constexpr float kSpringStiffness = 400.0f; // 回弹弹簧刚度
constexpr float kSpringDamping = 13.0f;    // 回弹阻尼
constexpr int kPressTintAlpha = 85;        // 按下时暗色遮罩强度

constexpr int kMenuAppOpen = 2001;
constexpr int kMenuAppRemove = 2002;
constexpr int kMenuExit = 2003;
constexpr int kMenuFolderOpen = 2004;
constexpr int kMenuFolderRename = 2005;

struct AppEntry {
    int type = 0; // 0 空，1 应用，2 文件夹
    std::wstring path;
    std::vector<std::wstring> folderApps;
    std::wstring folderName; // type == 2 时使用
};

struct PageData {
    AppEntry slots[kSlots];
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

    std::vector<PageData> pages;
    int currentPage = 0;

    // 组件拖动（顶部手柄）
    bool draggingWidget = false;
    POINT widgetCursor = {};
    POINT widgetOrigin = {};

    // 交互状态
    enum class DragMode { None, Widget, MainApp, FolderChild, PageSwipe };
    DragMode dragMode = DragMode::None;
    int dragPage = -1;
    int dragSlot = -1;
    int dragChild = -1;
    int swipeStartX = 0;
    int swipeLastX = 0;
    POINT lastCursor = {};
    POINT downClient = {};
    bool dragMoved = false;
    bool folderOpen = false;
    int folderPage = -1;
    int folderSlot = -1;

    // 文件夹重命名
    bool renamingFolder = false;
    int renamePage = -1;
    int renameSlot = -1;
    std::wstring renameText;
    HWND renameEdit = nullptr;
    HFONT renameFont = nullptr;
    HBRUSH renameBrush = nullptr;
    LONG_PTR renameOldExStyle = 0;
    WNDPROC renameOldEditProc = nullptr;

    // 右边缘/左边缘停留翻页
    int edgeSide = 0; // 0 none, 1 right, -1 left
    ULONGLONG edgeStart = 0;
    bool edgeHandled = false;

    // 分页过渡动画
    bool pageAnimating = false;
    int animFromPage = 0;
    int animToPage = 0;
    float animProgress = 0.0f;
    ULONGLONG animStart = 0;

    // 应用图标缓存：动画期间避免每帧重新从系统图标列表取图
    std::map<std::wstring, Gdiplus::Bitmap*> iconCache;

    bool dirty = false;

    // 点击按压动画（安卓风格图标回弹）
    enum class PressPhase { None, Pressing, Releasing };
    PressPhase pressPhase = PressPhase::None;
    bool pressInFolder = false; // true: 文件夹子面板中的应用
    int pressPage = -1;
    int pressSlot = -1;
    int pressChild = -1;
    float pressScale = 1.0f;    // 当前绘制缩放
    float pressOffset = 0.0f;   // 弹簧位移（scale - 1）
    float pressVel = 0.0f;      // 弹簧速度
    ULONGLONG pressStart = 0;
};

// ---------- 基础工具 ----------

using SetProcessDpiAwarenessContextFn = BOOL(WINAPI*)(HANDLE);
using GetDpiForWindowFn = UINT(WINAPI*)(HWND);

void EnableDpiAwareness() {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        auto pSet = reinterpret_cast<SetProcessDpiAwarenessContextFn>(
            GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
        if (pSet && pSet(reinterpret_cast<HANDLE>(static_cast<INT_PTR>(-4)))) {
            return;
        }
    }
    SetProcessDPIAware();
}

int GetWindowDpi(HWND hwnd) {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        auto pGet = reinterpret_cast<GetDpiForWindowFn>(
            GetProcAddress(user32, "GetDpiForWindow"));
        if (pGet) return static_cast<int>(pGet(hwnd));
    }
    HDC hdc = GetDC(hwnd);
    int dpi = GetDeviceCaps(hdc, LOGPIXELSX);
    ReleaseDC(hwnd, hdc);
    return dpi;
}

void ClampToWorkArea(int width, int height, POINT& pt) {
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    HMONITOR monitor = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    if (!GetMonitorInfoW(monitor, &mi)) return;
    const RECT& work = mi.rcWork;
    const LONG aw = work.right - work.left;
    const LONG ah = work.bottom - work.top;
    pt.x = (width >= aw) ? work.left
                         : std::clamp<LONG>(pt.x, work.left, work.right - width);
    pt.y = (height >= ah) ? work.top
                          : std::clamp<LONG>(pt.y, work.top, work.bottom - height);
}

std::wstring FileNameOf(const std::wstring& path) {
    size_t p = path.find_last_of(L"\\/");
    return (p == std::wstring::npos) ? path : path.substr(p + 1);
}

std::wstring DisplayNameOf(const std::wstring& path) {
    std::wstring name = FileNameOf(path);
    size_t dot = name.find_last_of(L'.');
    if (dot != std::wstring::npos && dot != 0) name = name.substr(0, dot);
    return name.empty() ? path : name;
}

bool IsAcceptableDrop(const std::wstring& path) {
    std::wstring lower = path;
    for (auto& ch : lower) ch = static_cast<wchar_t>(towlower(ch));
    return lower.size() > 4 &&
           (lower.compare(lower.size() - 4, 4, L".exe") == 0 ||
            lower.compare(lower.size() - 4, 4, L".lnk") == 0);
}

void SetWidgetScreenPos(HWND hwnd, int screenX, int screenY,
                       int width, int height, UINT flags) {
    POINT pt{screenX, screenY};
    HWND parent = GetParent(hwnd);
    if (parent) ScreenToClient(parent, &pt);
    SetWindowPos(hwnd, nullptr, pt.x, pt.y, width, height, flags);
}

bool AttachToDesktop(HWND hwnd, int screenX, int screenY,
                     int width, int height) {
    HWND hProgman = FindWindowW(L"Progman", nullptr);
    if (!hProgman) return false;
    SetWindowLongPtrW(hwnd, GWLP_HWNDPARENT,
                      reinterpret_cast<LONG_PTR>(hProgman));
    SetWindowPos(hwnd, HWND_BOTTOM, screenX, screenY, width, height,
                 SWP_NOACTIVATE);
    return true;
}

// ---------- 图标 ----------

Gdiplus::Bitmap* HiconToArgbBitmap(HICON hIcon) {
    ICONINFO ii{};
    if (!GetIconInfo(hIcon, &ii)) return nullptr;

    BITMAP bm{};
    if (!ii.hbmColor || !GetObjectW(ii.hbmColor, sizeof(bm), &bm) ||
        bm.bmWidth <= 0 || bm.bmHeight <= 0) {
        if (ii.hbmColor) DeleteObject(ii.hbmColor);
        if (ii.hbmMask) DeleteObject(ii.hbmMask);
        return nullptr;
    }

    const int w = bm.bmWidth;
    const int h = bm.bmHeight;
    HDC screenDc = GetDC(nullptr);
    if (!screenDc) {
        DeleteObject(ii.hbmColor);
        DeleteObject(ii.hbmMask);
        return nullptr;
    }

    // 读取 32bpp 颜色通道（BGRA）
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h; // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    std::vector<BYTE> color(static_cast<size_t>(w) * h * 4u, 0);
    const bool gotColor = GetDIBits(screenDc, ii.hbmColor, 0, h, color.data(),
                                    &bi, DIB_RGB_COLORS) != 0;

    // 读取 1bpp 掩码通道
    BITMAPINFO maskBi{};
    maskBi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    maskBi.bmiHeader.biWidth = w;
    maskBi.bmiHeader.biHeight = -h;
    maskBi.bmiHeader.biPlanes = 1;
    maskBi.bmiHeader.biBitCount = 1;
    maskBi.bmiHeader.biCompression = BI_RGB;
    const size_t maskStride = ((static_cast<size_t>(w) + 31u) / 32u) * 4u;
    std::vector<BYTE> mask(maskStride * h, 0);
    const bool gotMask = ii.hbmMask && GetDIBits(screenDc, ii.hbmMask, 0, h,
                                                  mask.data(), &maskBi,
                                                  DIB_RGB_COLORS) != 0;

    // 现代图标通常自带 alpha；否则退回掩码决定透明度
    bool hasAlpha = false;
    if (gotColor) {
        for (size_t i = 3; i < color.size(); i += 4) {
            if (color[i] != 0) {
                hasAlpha = true;
                break;
            }
        }
    }

    Gdiplus::Bitmap* bmp = new Gdiplus::Bitmap(w, h, PixelFormat32bppPARGB);
    if (!bmp) {
        ReleaseDC(nullptr, screenDc);
        DeleteObject(ii.hbmColor);
        DeleteObject(ii.hbmMask);
        return nullptr;
    }

    Gdiplus::Rect rect(0, 0, w, h);
    BitmapData data{};
    if (bmp->LockBits(&rect, ImageLockModeWrite, PixelFormat32bppPARGB, &data) == Ok) {
        auto* dst = static_cast<BYTE*>(data.Scan0);
        for (int y = 0; y < h; ++y) {
            const size_t maskRow = static_cast<size_t>(y) * maskStride;
            for (int x = 0; x < w; ++x) {
                const size_t c = (static_cast<size_t>(y) * w + x) * 4u;
                BYTE alpha = 0;
                if (hasAlpha) {
                    alpha = color[c + 3];
                } else if (gotMask) {
                    const BYTE bit = mask[maskRow + static_cast<size_t>(x) / 8u] &
                                     static_cast<BYTE>(0x80u >> (x & 7));
                    alpha = bit ? 255 : 0;
                } else if (gotColor) {
                    alpha = (color[c] || color[c + 1] || color[c + 2]) ? 255 : 0;
                }

                const size_t d = static_cast<size_t>(y) * data.Stride + static_cast<size_t>(x) * 4u;
                if (alpha == 0) {
                    dst[d] = dst[d + 1] = dst[d + 2] = 0;
                } else {
                    // DIB 字节序是 BGRA；PARGB 要求预乘 alpha
                    const BYTE b = static_cast<BYTE>(color[c + 0] * alpha / 255);
                    const BYTE g = static_cast<BYTE>(color[c + 1] * alpha / 255);
                    const BYTE r = static_cast<BYTE>(color[c + 2] * alpha / 255);
                    dst[d] = b;
                    dst[d + 1] = g;
                    dst[d + 2] = r;
                }
                dst[d + 3] = alpha;
            }
        }
        bmp->UnlockBits(&data);
    }

    ReleaseDC(nullptr, screenDc);
    DeleteObject(ii.hbmColor);
    DeleteObject(ii.hbmMask);
    return bmp;
}

// 部分 .lnk（如必剪）的图标源只有 16/32px 帧，系统大图标列表返回的是
// 256x256 画布、内容只占左上角一小块。按内容包围盒裁剪，让图标撑满显示区域。
// 内容已占画布大部分时原样返回。
Gdiplus::Bitmap* CropToIconContent(Gdiplus::Bitmap* bmp) {
    if (!bmp) return nullptr;
    const int W = bmp->GetWidth();
    const int H = bmp->GetHeight();
    if (W <= 0 || H <= 0) return bmp;

    Gdiplus::Rect r(0, 0, W, H);
    BitmapData bd{};
    if (bmp->LockBits(&r, ImageLockModeRead, PixelFormat32bppPARGB, &bd) != Ok) {
        return bmp;
    }
    int minX = W, minY = H, maxX = -1, maxY = -1;
    for (int y = 0; y < H; ++y) {
        const BYTE* row = static_cast<const BYTE*>(bd.Scan0) +
                          static_cast<size_t>(y) * static_cast<size_t>(bd.Stride);
        for (int x = 0; x < W; ++x) {
            if (row[static_cast<size_t>(x) * 4u + 3u] > 8) {
                if (x < minX) minX = x;
                if (x > maxX) maxX = x;
                if (y < minY) minY = y;
                if (y > maxY) maxY = y;
            }
        }
    }
    bmp->UnlockBits(&bd);

    if (maxX < 0) return bmp; // 全透明，保持原样

    const int cw = maxX - minX + 1;
    const int ch = maxY - minY + 1;
    // 内容已覆盖大部分画布：正常图标，无需裁剪
    if (cw >= W * 0.55f && ch >= H * 0.55f) return bmp;

    // 内容周围留一点边距再裁
    const int pad = std::max(1, static_cast<int>(
        std::lround(std::max(cw, ch) * 0.06f)));
    const int sx = std::max(0, minX - pad);
    const int sy = std::max(0, minY - pad);
    const int sw = std::min(W - sx, cw + pad * 2);
    const int sh = std::min(H - sy, ch + pad * 2);

    Gdiplus::Bitmap* cropped = new Gdiplus::Bitmap(sw, sh, PixelFormat32bppPARGB);
    if (!cropped) return bmp;
    Graphics g(cropped);
    g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    g.DrawImage(bmp, Gdiplus::Rect(0, 0, sw, sh), sx, sy, sw, sh, UnitPixel);
    return cropped;
}

Gdiplus::Bitmap* GetIconBitmap(const std::wstring& path) {
    SHFILEINFOW sfi{};
    const DWORD_PTR res = SHGetFileInfoW(path.c_str(), 0, &sfi, sizeof(sfi),
                                         SHGFI_SYSICONINDEX);
    if (!res) return nullptr;

    IImageList* imageList = nullptr;
    if (SUCCEEDED(SHGetImageList(SHIL_JUMBO, IID_IImageList,
                                 reinterpret_cast<void**>(&imageList))) &&
        imageList) {
        HICON hIcon = nullptr;
        if (SUCCEEDED(imageList->GetIcon(sfi.iIcon, ILD_TRANSPARENT, &hIcon)) && hIcon) {
            Gdiplus::Bitmap* bmp = HiconToArgbBitmap(hIcon);
            DestroyIcon(hIcon);
            imageList->Release();
            if (bmp) return bmp;
        } else {
            imageList->Release();
        }
    }

    // 极端情况回退：普通大图标 + 手动 alpha 合成
    SHFILEINFOW fallback{};
    if (SHGetFileInfoW(path.c_str(), 0, &fallback, sizeof(fallback),
                       SHGFI_ICON | SHGFI_LARGEICON) && fallback.hIcon) {
        Gdiplus::Bitmap* bmp = HiconToArgbBitmap(fallback.hIcon);
        DestroyIcon(fallback.hIcon);
        return bmp;
    }
    return nullptr;
}

Gdiplus::Bitmap* GetOrLoadIcon(AppState& s, const std::wstring& path) {
    auto it = s.iconCache.find(path);
    if (it != s.iconCache.end()) {
        return it->second; // 可能为 nullptr（提取失败），走占位绘制
    }
    Gdiplus::Bitmap* bmp = GetIconBitmap(path);
    if (bmp) {
        Gdiplus::Bitmap* cropped = CropToIconContent(bmp);
        if (cropped != bmp) {
            delete bmp;
            bmp = cropped;
        }
    }
    s.iconCache[path] = bmp;
    return bmp;
}

void DrawAppIcon(Graphics& g, AppState& s, const std::wstring& path,
                 const RectF& rect, float k) {
    Gdiplus::Bitmap* icon = GetOrLoadIcon(s, path);
    if (icon) {
        const float bw = static_cast<float>(icon->GetWidth());
        const float bh = static_cast<float>(icon->GetHeight());
        if (bw > 0 && bh > 0) {
            const float scale = (std::min)(rect.Width / bw, rect.Height / bh);
            const float drawW = bw * scale;
            const float drawH = bh * scale;
            RectF dst(rect.X + (rect.Width - drawW) * 0.5f,
                      rect.Y + (rect.Height - drawH) * 0.5f,
                      drawW, drawH);
            g.DrawImage(icon, dst);
        } else {
            g.DrawImage(icon, rect);
        }
    } else {
        SolidBrush bg(Color(255, 72, 76, 86));
        g.FillRectangle(&bg, rect);
        Pen border(Color(255, 220, 222, 228), 1.0f * k);
        g.DrawRectangle(&border, rect);
        const std::wstring name = DisplayNameOf(path);
        FontFamily ff(L"Microsoft YaHei UI");
        Gdiplus::Font font(&ff, std::round(20.0f * k), FontStyleBold, UnitPixel);
        StringFormat sf;
        sf.SetAlignment(StringAlignmentCenter);
        sf.SetLineAlignment(StringAlignmentCenter);
        SolidBrush text(Color(255, 255, 255, 255));
        g.DrawString(name.empty() ? L"?" : name.substr(0, 1).c_str(), -1,
                     &font, rect, &sf, &text);
    }
}

void DrawRoundedCardPath(GraphicsPath& path, const RectF& rect, float radius);

// 圆角文件夹图标：不透明深色底（透明度低于组件卡片）+ 3x3 圆角缩略图
void DrawFolderIcon(Graphics& g, AppState& s,
                    const std::vector<std::wstring>& apps,
                    const RectF& rect, float k) {
    const float radius = rect.Width * 0.22f;

    // 背景：alpha 235，比组件卡片（alpha 94~112）更不透明
    GraphicsPath bgPath;
    DrawRoundedCardPath(bgPath, rect, radius);
    SolidBrush bg(Color(235, 32, 35, 42));
    g.FillPath(&bg, &bgPath);

    // 细边框
    Pen border(Color(130, 255, 255, 255), 1.2f * k);
    GraphicsPath borderPath;
    DrawRoundedCardPath(borderPath, rect, radius);
    g.DrawPath(&border, &borderPath);

    // 3x3 圆角缩略图（带间隙）
    const float pad = rect.Width * 0.085f;
    const float gap = rect.Width * 0.04f;
    const float cell = (rect.Width - pad * 2.0f - gap * 2.0f) / 3.0f;
    const float miniR = cell * 0.24f;
    for (int i = 0; i < 9; ++i) {
        const int col = i % 3;
        const int row = i / 3;
        const RectF mini(rect.X + pad + col * (cell + gap),
                         rect.Y + pad + row * (cell + gap),
                         cell, cell);
        if (i < static_cast<int>(apps.size())) {
            GraphicsState st = g.Save();
            GraphicsPath mp;
            DrawRoundedCardPath(mp, mini, miniR);
            g.SetClip(&mp);
            DrawAppIcon(g, s, apps[i], mini, k);
            g.Restore(st);
        }
    }
}

// ---------- 持久化 ----------

void SlotValueName(wchar_t* buf, size_t cap, int page, int slot,
                   const wchar_t* suffix) {
    swprintf_s(buf, cap, L"P%d_%d_%s", page, slot, suffix);
}

std::wstring ReadRegString(HKEY key, const wchar_t* name) {
    DWORD type = 0;
    DWORD bytes = 0;
    if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS ||
        (type != REG_SZ && type != REG_EXPAND_SZ) || bytes < sizeof(wchar_t)) {
        return L"";
    }
    std::wstring value((bytes / sizeof(wchar_t)) + 1, L'\0');
    if (RegQueryValueExW(key, name, nullptr, &type,
                         reinterpret_cast<LPBYTE>(&value[0]), &bytes) != ERROR_SUCCESS) {
        return L"";
    }
    value.resize((bytes / sizeof(wchar_t)));
    if (!value.empty() && value.back() == L'\0') value.pop_back();
    return value;
}

DWORD ReadRegDword(HKEY key, const wchar_t* name, DWORD def) {
    DWORD type = 0;
    DWORD value = 0;
    DWORD bytes = sizeof(value);
    if (RegQueryValueExW(key, name, nullptr, &type,
                         reinterpret_cast<LPBYTE>(&value), &bytes) == ERROR_SUCCESS &&
        type == REG_DWORD) {
        return value;
    }
    return def;
}

bool LoadPages(AppState& s) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegPath, 0, KEY_READ, &key) != ERROR_SUCCESS) {
        s.pages.push_back(PageData{});
        return true;
    }
    const DWORD pageCount = std::clamp<DWORD>(ReadRegDword(key, L"PageCount", 1),
                                              1, kMaxPages);
    s.pages.clear();
    for (DWORD p = 0; p < pageCount; ++p) {
        PageData page;
        for (int slot = 0; slot < kSlots; ++slot) {
            wchar_t typeName[64] = {};
            wchar_t pathName[64] = {};
            wchar_t countName[64] = {};
            wchar_t nameName[64] = {};
            SlotValueName(typeName, 64, p, slot, L"Type");
            SlotValueName(pathName, 64, p, slot, L"Path");
            SlotValueName(countName, 64, p, slot, L"ChildCount");
            SlotValueName(nameName, 64, p, slot, L"FolderName");
            const DWORD type = ReadRegDword(key, typeName, 0);
            std::wstring path = ReadRegString(key, pathName);
            if (type == 1 && !path.empty()) {
                page.slots[slot].type = 1;
                page.slots[slot].path = path;
            } else if (type == 2) {
                page.slots[slot].type = 2;
                std::wstring folderName = ReadRegString(key, nameName);
                page.slots[slot].folderName =
                    folderName.empty() ? L"文件夹" : folderName;
                const DWORD count = std::clamp<DWORD>(
                    ReadRegDword(key, countName, 0), 0, kMaxFolderApps);
                for (DWORD c = 0; c < count; ++c) {
                    wchar_t childName[64] = {};
                    swprintf_s(childName, L"P%d_%d_C%d_Path", p, slot, c);
                    std::wstring child = ReadRegString(key, childName);
                    if (!child.empty()) page.slots[slot].folderApps.push_back(child);
                }
                if (page.slots[slot].folderApps.empty()) {
                    page.slots[slot].type = 0;
                }
            }
        }
        s.pages.push_back(std::move(page));
    }
    if (s.pages.empty()) s.pages.push_back(PageData{});
    s.currentPage = std::clamp<int>(static_cast<int>(ReadRegDword(key, L"CurrentPage", 0)),
                                    0, static_cast<int>(s.pages.size()) - 1);
    RegCloseKey(key);
    return true;
}

void WriteRegString(HKEY key, const wchar_t* name, const std::wstring& value) {
    RegSetValueExW(key, name, 0, REG_SZ,
                   reinterpret_cast<const BYTE*>(value.c_str()),
                   static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
}

void SavePages(const AppState& s) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegPath, 0, nullptr, 0, KEY_WRITE,
                        nullptr, &key, nullptr) != ERROR_SUCCESS) return;
    const DWORD pageCount = static_cast<DWORD>(s.pages.size());
    RegSetValueExW(key, L"PageCount", 0, REG_DWORD,
                   reinterpret_cast<const BYTE*>(&pageCount), sizeof(pageCount));
    const DWORD current = static_cast<DWORD>(s.currentPage);
    RegSetValueExW(key, L"CurrentPage", 0, REG_DWORD,
                   reinterpret_cast<const BYTE*>(&current), sizeof(current));
    for (DWORD p = 0; p < pageCount; ++p) {
        const PageData& page = s.pages[p];
        for (int slot = 0; slot < kSlots; ++slot) {
            wchar_t typeName[64] = {};
            wchar_t pathName[64] = {};
            wchar_t countName[64] = {};
            wchar_t nameName[64] = {};
            SlotValueName(typeName, 64, p, slot, L"Type");
            SlotValueName(pathName, 64, p, slot, L"Path");
            SlotValueName(countName, 64, p, slot, L"ChildCount");
            SlotValueName(nameName, 64, p, slot, L"FolderName");
            const DWORD type = static_cast<DWORD>(page.slots[slot].type);
            RegSetValueExW(key, typeName, 0, REG_DWORD,
                           reinterpret_cast<const BYTE*>(&type), sizeof(type));
            WriteRegString(key, pathName, page.slots[slot].path);
            WriteRegString(key, nameName,
                           page.slots[slot].folderName.empty()
                               ? L"文件夹"
                               : page.slots[slot].folderName);
            const DWORD count = static_cast<DWORD>(page.slots[slot].folderApps.size());
            RegSetValueExW(key, countName, 0, REG_DWORD,
                           reinterpret_cast<const BYTE*>(&count), sizeof(count));
            for (DWORD c = 0; c < count; ++c) {
                wchar_t childName[64] = {};
                swprintf_s(childName, L"P%d_%d_C%d_Path", p, slot, c);
                WriteRegString(key, childName, page.slots[slot].folderApps[c]);
            }
        }
    }
    RegCloseKey(key);
}

bool LoadSavedPosition(POINT& pt) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegPath, 0, KEY_READ, &key) != ERROR_SUCCESS) return false;
    bool ok = false;
    DWORD x = 0, y = 0;
    DWORD size = sizeof(x), type = 0;
    if (RegQueryValueExW(key, L"WindowX", nullptr, &type,
                         reinterpret_cast<LPBYTE>(&x), &size) == ERROR_SUCCESS &&
        type == REG_DWORD) {
        size = sizeof(y); type = 0;
        if (RegQueryValueExW(key, L"WindowY", nullptr, &type,
                             reinterpret_cast<LPBYTE>(&y), &size) == ERROR_SUCCESS &&
            type == REG_DWORD) {
            pt.x = static_cast<LONG>(x);
            pt.y = static_cast<LONG>(y);
            ok = true;
        }
    }
    RegCloseKey(key);
    return ok;
}

void SavePosition(HWND hwnd) {
    RECT rc{};
    if (!GetWindowRect(hwnd, &rc)) return;
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegPath, 0, nullptr, 0, KEY_WRITE,
                        nullptr, &key, nullptr) != ERROR_SUCCESS) return;
    DWORD x = static_cast<DWORD>(rc.left);
    DWORD y = static_cast<DWORD>(rc.top);
    RegSetValueExW(key, L"WindowX", 0, REG_DWORD,
                   reinterpret_cast<const BYTE*>(&x), sizeof(x));
    RegSetValueExW(key, L"WindowY", 0, REG_DWORD,
                   reinterpret_cast<const BYTE*>(&y), sizeof(y));
    RegCloseKey(key);
}

// ---------- 绘图缓冲 ----------

void DestroyBacking(AppState& s) {
    for (auto& item : s.iconCache) {
        delete item.second;
    }
    s.iconCache.clear();

    delete s.bitmap;
    s.bitmap = nullptr;
    if (s.hdcMem) {
        if (s.hbmDib) {
            if (s.hbmOld) SelectObject(s.hdcMem, s.hbmOld);
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
    if (!screenDc) return false;
    s.hdcMem = CreateCompatibleDC(screenDc);
    if (!s.hdcMem) { ReleaseDC(s.hwnd, screenDc); return false; }
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    s.hbmDib = CreateDIBSection(screenDc, &bmi, DIB_RGB_COLORS, &s.pvBits, nullptr, 0);
    ReleaseDC(s.hwnd, screenDc);
    if (!s.hbmDib) { DeleteDC(s.hdcMem); s.hdcMem = nullptr; return false; }
    s.hbmOld = static_cast<HBITMAP>(SelectObject(s.hdcMem, s.hbmDib));
    s.bitmap = new Gdiplus::Bitmap(width, height, PixelFormat32bppPARGB);
    if (!s.bitmap) { DestroyBacking(s); return false; }
    s.width = width;
    s.height = height;
    s.scale = static_cast<float>(s.dpi) / 96.0f;
    return true;
}

void PresentLauncher(AppState& s) {
    if (!s.bitmap || !s.hdcMem || !s.pvBits) return;
    Gdiplus::Rect lockRect(0, 0, s.width, s.height);
    BitmapData bd{};
    if (s.bitmap->LockBits(&lockRect, ImageLockModeRead, PixelFormat32bppPARGB, &bd) != Ok) return;
    auto* dst = static_cast<BYTE*>(s.pvBits);
    const auto* src = static_cast<const BYTE*>(bd.Scan0);
    const size_t rowBytes = static_cast<size_t>(s.width) * 4u;
    for (int y = 0; y < s.height; ++y) {
        std::memcpy(dst + static_cast<size_t>(y) * rowBytes,
                    src + static_cast<size_t>(y) * static_cast<size_t>(bd.Stride),
                    rowBytes);
    }
    s.bitmap->UnlockBits(&bd);

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

// ---------- 分页动画 ----------

float EaseOutCubic(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    const float inv = 1.0f - t;
    return 1.0f - inv * inv * inv;
}

void StartPageAnimation(AppState& s, int targetPage, float initialProgress = 0.0f) {
    if (targetPage < 0 || targetPage >= static_cast<int>(s.pages.size())) return;
    if (targetPage == s.currentPage) return;

    initialProgress = std::clamp(initialProgress, 0.0f, 1.0f);
    s.animFromPage = s.currentPage;
    s.animToPage = targetPage;
    s.currentPage = targetPage; // 逻辑上立即切到新页，画面用动画补过渡
    s.animProgress = initialProgress;
    s.animStart = GetTickCount64() -
                  static_cast<ULONGLONG>(initialProgress * kPageAnimMs);
    s.pageAnimating = true;
}

// ---------- 布局 ----------

RectF MainGridRect(const AppState& s) {
    const float k = s.scale;
    const float left = 14.0f * k;
    const float top = 44.0f * k;
    const float right = s.width - 14.0f * k;
    const float bottom = s.height - 24.0f * k;
    return RectF(left, top, right - left, bottom - top);
}

RectF MainCellRect(const AppState& s, int slot) {
    const RectF grid = MainGridRect(s);
    const int col = slot % kCols;
    const int row = slot / kCols;
    const float cw = grid.Width / kCols;
    const float ch = grid.Height / kRows;
    return RectF(grid.X + col * cw + 4.0f * s.scale,
                 grid.Y + row * ch + 4.0f * s.scale,
                 cw - 8.0f * s.scale, ch - 8.0f * s.scale);
}

int HitMainSlot(const AppState& s, POINT client) {
    const RectF grid = MainGridRect(s);
    const float x = static_cast<float>(client.x);
    const float y = static_cast<float>(client.y);
    if (x < grid.X || x >= grid.X + grid.Width ||
        y < grid.Y || y >= grid.Y + grid.Height) return -1;
    const int col = static_cast<int>((x - grid.X) / (grid.Width / kCols));
    const int row = static_cast<int>((y - grid.Y) / (grid.Height / kRows));
    return row * kCols + col;
}

RectF FolderPanelRect(const AppState& s) {
    const float k = s.scale;
    const float left = 16.0f * k;
    // 面板尺寸固定（不随组件高度翻倍变化），在新组件内垂直居中
    const float height = kFolderPanelHeight * k;
    const float top = (s.height - height) * 0.5f;
    return RectF(left, top, s.width - left * 2.0f, height);
}

RectF FolderCellRect(const AppState& s, int index) {
    const RectF panel = FolderPanelRect(s);
    const float k = s.scale;
    const float gridLeft = panel.X + 10.0f * k;
    const float gridTop = panel.Y + 40.0f * k;
    const float gridRight = panel.X + panel.Width - 10.0f * k;
    const float gridBottom = panel.Y + panel.Height - 8.0f * k;
    const int col = index % kFolderCols;
    const int row = index / kFolderCols;
    const float cw = (gridRight - gridLeft) / kFolderCols;
    const float ch = (gridBottom - gridTop) / kFolderRows;
    return RectF(gridLeft + col * cw + 3.0f * k, gridTop + row * ch + 3.0f * k,
                 cw - 6.0f * k, ch - 6.0f * k);
}

int HitFolderChild(const AppState& s, POINT client) {
    const RectF panel = FolderPanelRect(s);
    const float x = static_cast<float>(client.x);
    const float y = static_cast<float>(client.y);
    if (x < panel.X || x >= panel.X + panel.Width ||
        y < panel.Y || y >= panel.Y + panel.Height) return -1;
    const float k = s.scale;
    const float gridLeft = panel.X + 10.0f * k;
    const float gridTop = panel.Y + 40.0f * k;
    const float gridRight = panel.X + panel.Width - 10.0f * k;
    const float gridBottom = panel.Y + panel.Height - 8.0f * k;
    if (x < gridLeft || x >= gridRight || y < gridTop || y >= gridBottom) return -1;
    const int col = static_cast<int>((x - gridLeft) / ((gridRight - gridLeft) / kFolderCols));
    const int row = static_cast<int>((y - gridTop) / ((gridBottom - gridTop) / kFolderRows));
    return row * kFolderCols + col;
}

bool IsInTopHandle(const AppState& s, POINT client) {
    return client.y < 34.0f * s.scale;
}

bool HitTestCard(HWND hwnd, const AppState& s, LPARAM lParam) {
    POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
    ScreenToClient(hwnd, &pt);
    const float k = s.scale;
    const float left = 8.0f * k;
    const float top = 8.0f * k;
    const float right = s.width - left;
    const float bottom = s.height - top;
    const float r = 22.0f * k;
    if (pt.x < left + r || pt.x > right - r) {
        if (pt.y < top + r || pt.y > bottom - r) return false;
    }
    if (pt.x < left || pt.x > right || pt.y < top || pt.y > bottom) return false;
    return true;
}

// ---------- 数据操作 ----------

bool IsPageEmpty(const PageData& page) {
    for (int i = 0; i < kSlots; ++i) {
        if (page.slots[i].type != 0) return false;
    }
    return true;
}

void PruneEmptyPages(AppState& s) {
    if (s.pages.empty()) {
        s.pages.push_back(PageData{});
        s.currentPage = 0;
        return;
    }
    s.currentPage = std::clamp<int>(s.currentPage, 0,
                                    static_cast<int>(s.pages.size()) - 1);
    const int oldPage = s.currentPage;
    const bool currentWasEmpty = IsPageEmpty(s.pages[oldPage]);

    int nonEmptyBefore = 0;
    for (int i = 0; i < oldPage; ++i) {
        if (!IsPageEmpty(s.pages[i])) ++nonEmptyBefore;
    }

    std::vector<PageData> kept;
    kept.reserve(s.pages.size());
    for (auto& page : s.pages) {
        if (!IsPageEmpty(page)) kept.push_back(std::move(page));
    }

    // 至少要保留一个空白页作为初始页
    if (kept.empty()) kept.push_back(PageData{});

    int newPage = 0;
    if (currentWasEmpty) {
        // 当前空白页被删除：优先落到它之后的第一个非空页，否则落到最后一页
        newPage = std::clamp(nonEmptyBefore, 0, static_cast<int>(kept.size()) - 1);
    } else {
        // 当前页仍在，重新映射到删除前方空白页后的新下标
        newPage = std::clamp(nonEmptyBefore, 0, static_cast<int>(kept.size()) - 1);
    }

    s.pages = std::move(kept);
    s.currentPage = newPage;
}

int FirstEmptySlot(const PageData& page) {
    for (int i = 0; i < kSlots; ++i) if (page.slots[i].type == 0) return i;
    return -1;
}

int AddAppToPage(AppState& s, int page, const std::wstring& path) {
    if (page < 0 || page >= static_cast<int>(s.pages.size())) return -1;
    int slot = FirstEmptySlot(s.pages[page]);
    if (slot >= 0) {
        s.pages[page].slots[slot].type = 1;
        s.pages[page].slots[slot].path = path;
        s.pages[page].slots[slot].folderApps.clear();
        return slot;
    }
    return -1;
}

int AddAppSomewhere(AppState& s, const std::wstring& path) {
    int slot = AddAppToPage(s, s.currentPage, path);
    if (slot >= 0) return slot;
    for (int p = 0; p < static_cast<int>(s.pages.size()); ++p) {
        slot = AddAppToPage(s, p, path);
        if (slot >= 0) return slot;
    }
    if (static_cast<int>(s.pages.size()) < kMaxPages) {
        s.pages.push_back(PageData{});
        s.currentPage = static_cast<int>(s.pages.size()) - 1;
        return AddAppToPage(s, s.currentPage, path);
    }
    return -1;
}

void RemoveAppFromSlot(AppState& s, int page, int slot) {
    if (page < 0 || page >= static_cast<int>(s.pages.size()) || slot < 0 || slot >= kSlots) return;
    s.pages[page].slots[slot] = AppEntry{};
    PruneEmptyPages(s);
}

void DissolveFolderIfNeeded(AppState& s, int page, int slot) {
    AppEntry& entry = s.pages[page].slots[slot];
    if (entry.type != 2) return;
    if (entry.folderApps.size() == 1) {
        const std::wstring sole = entry.folderApps[0];
        entry = AppEntry{};
        entry.type = 1;
        entry.path = sole;
    } else if (entry.folderApps.empty()) {
        entry = AppEntry{};
    }
}

void DrawLauncher(AppState& s);

LRESULT CALLBACK RenameEditProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    HWND parent = GetParent(hwnd);
    auto* s = reinterpret_cast<AppState*>(
        GetWindowLongPtrW(parent, GWLP_USERDATA));

    if (s && msg == WM_KEYDOWN &&
        (wParam == VK_RETURN || wParam == VK_ESCAPE)) {
        PostMessageW(parent, kRenameCommand, wParam, 0);
        return 0;
    }

    if (s && s->renameOldEditProc) {
        return CallWindowProcW(s->renameOldEditProc, hwnd, msg, wParam, lParam);
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void CleanupRenameEdit(AppState& s) {
    if (s.renameEdit) {
        DestroyWindow(s.renameEdit);
        s.renameEdit = nullptr;
    }
    s.renameOldEditProc = nullptr;
    if (s.renameFont) {
        DeleteObject(s.renameFont);
        s.renameFont = nullptr;
    }
    if (s.renameBrush) {
        DeleteObject(s.renameBrush);
        s.renameBrush = nullptr;
    }
    if (s.renameOldExStyle) {
        SetWindowLongPtrW(s.hwnd, GWL_EXSTYLE, s.renameOldExStyle);
        s.renameOldExStyle = 0;
        SetWindowPos(s.hwnd, HWND_BOTTOM, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
}

void StartFolderRename(AppState& s, int page, int slot) {
    if (page < 0 || page >= static_cast<int>(s.pages.size())) return;
    if (slot < 0 || slot >= kSlots || s.pages[page].slots[slot].type != 2) return;

    s.renamingFolder = true;
    s.renamePage = page;
    s.renameSlot = slot;
    s.renameText = s.pages[page].slots[slot].folderName.empty()
                       ? L"文件夹"
                       : s.pages[page].slots[slot].folderName;

    // 离屏 EDIT 只用来接收键盘输入；可见输入框由 GDI+ 自绘
    s.renameBrush = CreateSolidBrush(RGB(20, 22, 26));
    s.renameFont = CreateFontW(-MulDiv(12, s.dpi, 72), 0, 0, 0,
                               FW_NORMAL, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                               CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                               DEFAULT_PITCH | FF_DONTCARE,
                               L"Microsoft YaHei UI");

    s.renameEdit = CreateWindowExW(
        0, L"EDIT", s.renameText.c_str(),
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_LEFT,
        -100, -100, 40, 22,
        s.hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);

    if (s.renameFont && s.renameEdit) {
        SendMessageW(s.renameEdit, WM_SETFONT,
                     reinterpret_cast<WPARAM>(s.renameFont), TRUE);
    }

    if (s.renameEdit) {
        s.renameOldEditProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
            s.renameEdit, GWLP_WNDPROC,
            reinterpret_cast<LONG_PTR>(RenameEditProc)));
    }

    // 重命名期间临时允许激活，确保能拿到键盘输入
    s.renameOldExStyle = GetWindowLongPtrW(s.hwnd, GWL_EXSTYLE);
    SetWindowLongPtrW(s.hwnd, GWL_EXSTYLE,
                      s.renameOldExStyle & ~WS_EX_NOACTIVATE);
    SetWindowPos(s.hwnd, HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SetForegroundWindow(s.hwnd);
    if (s.renameEdit) {
        SetFocus(s.renameEdit);
        SendMessageW(s.renameEdit, EM_SETSEL, 0, -1);
    }
}

void CommitFolderRename(AppState& s) {
    if (s.renamingFolder && s.renameEdit) {
        wchar_t buf[64] = {};
        GetWindowTextW(s.renameEdit, buf, 64);
        s.renameText = buf;
    }

    if (s.renamingFolder && s.renamePage >= 0 && s.renameSlot >= 0 &&
        s.renamePage < static_cast<int>(s.pages.size())) {
        std::wstring name = s.renameText;
        while (!name.empty() && name.front() == L' ') name.erase(name.begin());
        while (!name.empty() && name.back() == L' ') name.pop_back();
        if (name.empty()) name = L"文件夹";
        if (name.size() > 16) name = name.substr(0, 16);

        AppEntry& entry = s.pages[s.renamePage].slots[s.renameSlot];
        if (entry.type == 2) {
            entry.folderName = name;
            SavePages(s);
        }
    }

    s.renamingFolder = false;
    s.renamePage = -1;
    s.renameSlot = -1;
    s.renameText.clear();
    CleanupRenameEdit(s);
    DrawLauncher(s);
}

void CancelFolderRename(AppState& s) {
    s.renamingFolder = false;
    s.renamePage = -1;
    s.renameSlot = -1;
    s.renameText.clear();
    CleanupRenameEdit(s);
    DrawLauncher(s);
}

void OpenFolder(AppState& s, int page, int slot) {
    if (page < 0 || page >= static_cast<int>(s.pages.size())) return;
    if (slot < 0 || slot >= kSlots || s.pages[page].slots[slot].type != 2) return;
    s.folderOpen = true;
    s.folderPage = page;
    s.folderSlot = slot;
}

void CloseFolder(AppState& s) {
    s.folderOpen = false;
    s.folderPage = -1;
    s.folderSlot = -1;
}

void RemoveChildFromFolder(AppState& s, int childIndex) {
    if (!s.folderOpen || s.folderPage < 0 || s.folderSlot < 0) return;
    const int page = s.folderPage;
    const int slot = s.folderSlot;
    AppEntry& folder = s.pages[page].slots[slot];
    if (folder.type != 2 || childIndex < 0 ||
        childIndex >= static_cast<int>(folder.folderApps.size())) return;

    const std::wstring moved = folder.folderApps[childIndex];
    folder.folderApps.erase(folder.folderApps.begin() + childIndex);

    // 拖出的应用放回主页面（优先当前页空格）
    int target = FirstEmptySlot(s.pages[s.currentPage]);
    if (target < 0) {
        if (static_cast<int>(s.pages.size()) < kMaxPages) {
            s.pages.push_back(PageData{});
            s.currentPage = static_cast<int>(s.pages.size()) - 1;
            target = FirstEmptySlot(s.pages[s.currentPage]);
        }
    }
    if (target >= 0) {
        s.pages[s.currentPage].slots[target].type = 1;
        s.pages[s.currentPage].slots[target].path = moved;
    }

    // 文件夹只剩一个时解散
    if (folder.folderApps.size() == 1) {
        const std::wstring sole = folder.folderApps[0];
        folder = AppEntry{};
        folder.type = 1;
        folder.path = sole;
        CloseFolder(s);
    } else if (folder.folderApps.empty()) {
        folder = AppEntry{};
        CloseFolder(s);
    }
    PruneEmptyPages(s);
}

// ---------- 绘制 ----------

void DrawRoundedCardPath(GraphicsPath& path, const RectF& rect, float radius) {
    const float d = radius * 2.0f;
    path.AddArc(rect.X, rect.Y, d, d, 180.0f, 90.0f);
    path.AddArc(rect.X + rect.Width - d, rect.Y, d, d, 270.0f, 90.0f);
    path.AddArc(rect.X + rect.Width - d, rect.Y + rect.Height - d, d, d, 0.0f, 90.0f);
    path.AddArc(rect.X, rect.Y + rect.Height - d, d, d, 90.0f, 90.0f);
    path.CloseFigure();
}

// ---------- 点击按压动画（安卓风格） ----------

void StartPress(AppState& s, bool inFolder, int page, int slot, int child) {
    s.pressPhase = AppState::PressPhase::Pressing;
    s.pressInFolder = inFolder;
    s.pressPage = page;
    s.pressSlot = slot;
    s.pressChild = child;
    s.pressScale = 1.0f;
    s.pressOffset = 0.0f;
    s.pressVel = 0.0f;
    s.pressStart = GetTickCount64();
}

void CancelPress(AppState& s) {
    s.pressPhase = AppState::PressPhase::None;
    s.pressInFolder = false;
    s.pressPage = -1;
    s.pressSlot = -1;
    s.pressChild = -1;
    s.pressScale = 1.0f;
    s.pressOffset = 0.0f;
    s.pressVel = 0.0f;
}

void StartPressRelease(AppState& s) {
    if (s.pressPhase == AppState::PressPhase::None) return;
    s.pressPhase = AppState::PressPhase::Releasing;
    s.pressOffset = s.pressScale - 1.0f; // 从当前缩放开始回弹
    s.pressVel = 0.0f;
    s.pressStart = GetTickCount64();
}

// 推进动画一帧；返回 true 表示画面有变化需要重绘
bool UpdatePressAnim(AppState& s) {
    const ULONGLONG now = GetTickCount64();
    if (s.pressPhase == AppState::PressPhase::Pressing) {
        const float t = static_cast<float>(now - s.pressStart) /
                        static_cast<float>(kPressMs);
        if (t >= 1.0f) {
            s.pressScale = kPressScale; // 按住保持，无需继续重绘
            return false;
        }
        s.pressScale = 1.0f + (kPressScale - 1.0f) * EaseOutCubic(t);
        return true;
    }

    if (s.pressPhase == AppState::PressPhase::Releasing) {
        // 欠阻尼弹簧：先过冲到 1.0 以上再回落，模拟安卓回弹
        const float dt = std::min(
            static_cast<float>(now - s.pressStart) / 1000.0f, 0.05f);
        constexpr float h = 1.0f / 240.0f; // 固定小步长，保证积分稳定
        for (float t = 0.0f; t < dt; t += h) {
            const float a = -kSpringStiffness * s.pressOffset -
                            kSpringDamping * s.pressVel;
            s.pressVel += a * h;
            s.pressOffset += s.pressVel * h;
        }
        if (std::abs(s.pressOffset) < 0.0004f &&
            std::abs(s.pressVel) < 0.01f) {
            CancelPress(s);
            return true;
        }
        s.pressScale = 1.0f + s.pressOffset;
        return true;
    }
    return false;
}

// 按下的图标是否与释放位置一致（不一致则取消动画，避免错位回弹）
bool PressTargetMatches(const AppState& s, POINT client) {
    if (s.pressPhase == AppState::PressPhase::None) return false;
    if (s.pressInFolder) {
        return HitFolderChild(s, client) == s.pressChild;
    }
    return HitMainSlot(s, client) == s.pressSlot &&
           s.currentPage == s.pressPage;
}

// 围绕中心缩放矩形（按压动画用）
RectF ScaleRectAroundCenter(const RectF& rect, float scale) {
    if (scale <= 0.0f) return rect;
    const float w = rect.Width * scale;
    const float h = rect.Height * scale;
    return RectF(rect.X + (rect.Width - w) * 0.5f,
                 rect.Y + (rect.Height - h) * 0.5f, w, h);
}

// 按压中的暗色遮罩（类似安卓触摸反馈），松手后快速淡出
int PressTintAlpha(const AppState& s) {
    if (s.pressPhase == AppState::PressPhase::Pressing) {
        return kPressTintAlpha;
    }
    if (s.pressPhase == AppState::PressPhase::Releasing) {
        const float el = static_cast<float>(GetTickCount64() - s.pressStart);
        return static_cast<int>(kPressTintAlpha *
                                std::max(0.0f, 1.0f - el / 130.0f));
    }
    return 0;
}

void DrawPressTint(Graphics& g, const RectF& rect, float k, int alpha) {
    if (alpha <= 0) return;
    GraphicsPath path;
    DrawRoundedCardPath(path, rect, 14.0f * k);
    SolidBrush tint(Color(static_cast<BYTE>(alpha), 0, 0, 0));
    g.FillPath(&tint, &path);
}

void DrawCardBackground(Graphics& g, const AppState& s) {
    const float k = s.scale;
    RectF card(8.0f * k, 8.0f * k, s.width - 16.0f * k, s.height - 16.0f * k);
    GraphicsPath path;
    DrawRoundedCardPath(path, card, 22.0f * k);
    LinearGradientBrush bg(card, Color(112, 70, 74, 84), Color(94, 16, 18, 24),
                           LinearGradientModeForwardDiagonal);
    g.FillPath(&bg, &path);
    Pen border(Color(110, 255, 255, 255), 1.2f * k);
    g.DrawPath(&border, &path);

    // 顶部手柄
    const float handleY = 12.0f * k;
    Pen handlePen(Color(150, 255, 255, 255), 3.0f * k);
    handlePen.SetStartCap(LineCapRound);
    handlePen.SetEndCap(LineCapRound);
    const float hw = 36.0f * k;
    g.DrawLine(&handlePen, PointF(s.width / 2.0f - hw, handleY),
               PointF(s.width / 2.0f + hw, handleY));
}

void DrawStringCentered(Graphics& g, const std::wstring& text,
                        Gdiplus::Font* font, const RectF& rect, const Color& color) {
    StringFormat sf;
    sf.SetAlignment(StringAlignmentCenter);
    sf.SetLineAlignment(StringAlignmentCenter);
    sf.SetTrimming(StringTrimmingEllipsisCharacter);
    sf.SetFormatFlags(StringFormatFlagsNoWrap | StringFormatFlagsNoFitBlackBox);
    SolidBrush brush(color);

    // 文字区域对齐到整数像素，减少亚像素模糊
    RectF snapped(std::round(rect.X), std::round(rect.Y),
                  std::round(rect.Width), std::round(rect.Height));
    g.DrawString(text.c_str(), -1, font, snapped, &sf, &brush);
}

void DrawRenameBox(Graphics& g, AppState& s, const RectF& rect,
                   float k, Gdiplus::Font* font) {
    RectF box(std::round(rect.X), std::round(rect.Y),
              std::round(rect.Width), std::round(rect.Height));
    SolidBrush bg(Color(255, 18, 20, 24));
    g.FillRectangle(&bg, box);
    Pen border(Color(255, 120, 170, 255), 1.2f * k);
    g.DrawRectangle(&border, box);

    StringFormat sf;
    sf.SetAlignment(StringAlignmentCenter);
    sf.SetLineAlignment(StringAlignmentCenter);
    sf.SetTrimming(StringTrimmingEllipsisCharacter);
    sf.SetFormatFlags(StringFormatFlagsNoWrap | StringFormatFlagsNoFitBlackBox);
    SolidBrush text(Color(255, 255, 255, 255));
    g.DrawString(s.renameText.c_str(), -1, font, box, &sf, &text);

    // 闪烁光标
    if ((GetTickCount64() / 400) % 2 == 0) {
        RectF measured;
        g.MeasureString(s.renameText.c_str(), -1, font,
                        PointF(0.0f, 0.0f), &measured);
        const float cx = box.X + box.Width * 0.5f + measured.Width * 0.5f + 1.5f * k;
        Pen cursor(Color(255, 150, 190, 255), 1.4f * k);
        g.DrawLine(&cursor, PointF(cx, box.Y + 3.0f * k),
                   PointF(cx, box.Y + box.Height - 3.0f * k));
    }
}

void DrawPageGridAt(Graphics& g, AppState& s, int pageIndex, float offsetX) {
    if (pageIndex < 0 || pageIndex >= static_cast<int>(s.pages.size())) return;

    const float k = s.scale;
    FontFamily family(L"Microsoft YaHei UI");
    Gdiplus::Font labelFont(&family, std::round(12.5f * k), FontStyleBold, UnitPixel);

    const PageData& page = s.pages[pageIndex];
    RectF grid = MainGridRect(s);
    grid.X += offsetX;

    for (int slot = 0; slot < kSlots; ++slot) {
        const AppEntry& entry = page.slots[slot];
        if (entry.type == 0) continue;

        const int col = slot % kCols;
        const int row = slot / kCols;
        const float cw = grid.Width / kCols;
        const float ch = grid.Height / kRows;
        const RectF cell(grid.X + col * cw + 4.0f * k,
                         grid.Y + row * ch + 4.0f * k,
                         cw - 8.0f * k, ch - 8.0f * k);

        const float iconSize = std::min(cell.Width, cell.Height) * 0.62f;
        const RectF iconRect(cell.X + (cell.Width - iconSize) / 2.0f,
                             cell.Y + 7.0f * k,
                             iconSize, iconSize);

        // 安卓风格按压动画：图标围绕中心缩放 + 触摸遮罩
        const bool pressed =
            s.pressPhase != AppState::PressPhase::None &&
            !s.pressInFolder && s.pressPage == pageIndex &&
            s.pressSlot == slot;
        const RectF drawIcon = pressed
                                   ? ScaleRectAroundCenter(iconRect, s.pressScale)
                                   : iconRect;
        if (entry.type == 2) {
            DrawFolderIcon(g, s, entry.folderApps, drawIcon, k);
        } else {
            DrawAppIcon(g, s, entry.path, drawIcon, k);
        }
        if (pressed) {
            DrawPressTint(g, drawIcon, k, PressTintAlpha(s));
        }

        const std::wstring label = (entry.type == 2)
                                         ? (entry.folderName.empty()
                                                ? L"文件夹"
                                                : entry.folderName)
                                         : DisplayNameOf(entry.path);
        RectF textRect(cell.X + 2.0f * k,
                       iconRect.Y + iconRect.Height + 3.0f * k,
                       cell.Width - 4.0f * k,
                       cell.Height - iconRect.Height - 8.0f * k);

        if (s.renamingFolder && s.renamePage == pageIndex &&
            s.renameSlot == slot) {
            DrawRenameBox(g, s, textRect, k, &labelFont);
        } else {
            DrawStringCentered(g, label, &labelFont, textRect,
                               Color(235, 245, 245, 247));
        }
    }

    bool empty = true;
    for (int slot = 0; slot < kSlots; ++slot) {
        if (page.slots[slot].type != 0) { empty = false; break; }
    }
    if (empty) {
        Gdiplus::Font hintFont(&family, std::round(13.0f * k), FontStyleRegular, UnitPixel);
        DrawStringCentered(g, L"将 .exe 或 .lnk 拖入此处",
                           &hintFont, grid, Color(140, 255, 255, 255));
    }
}

void DrawMainGrid(Graphics& g, AppState& s) {
    const float k = s.scale;

    if (s.currentPage >= static_cast<int>(s.pages.size())) s.currentPage = 0;

    RectF card(8.0f * k, 8.0f * k,
               s.width - 16.0f * k, s.height - 16.0f * k);

    if (s.dragMode == AppState::DragMode::PageSwipe) {
        // 未松开鼠标：页面位移实时跟随手指
        const float dx = static_cast<float>(s.swipeLastX - s.swipeStartX);
        const float w = static_cast<float>(s.width);
        const bool hasNext = s.currentPage < static_cast<int>(s.pages.size()) - 1;
        const bool hasPrev = s.currentPage > 0;

        GraphicsState state = g.Save();
        g.SetClip(card);

        if (dx < 0.0f && hasNext) {
            DrawPageGridAt(g, s, s.currentPage, dx);
            DrawPageGridAt(g, s, s.currentPage + 1, w + dx);
        } else if (dx > 0.0f && hasPrev) {
            DrawPageGridAt(g, s, s.currentPage, dx);
            DrawPageGridAt(g, s, s.currentPage - 1, -w + dx);
        } else {
            // 第一页/最后一页继续拖动时给一点阻尼
            DrawPageGridAt(g, s, s.currentPage, dx * 0.28f);
        }

        g.Restore(state);
    } else if (s.pageAnimating) {
        // 用 ease-out 位移模拟左右滑动翻页
        const float p = EaseOutCubic(s.animProgress);
        const float w = static_cast<float>(s.width);
        float fromOffset = 0.0f;
        float toOffset = 0.0f;
        if (s.animToPage > s.animFromPage) {
            // 下一页从右侧进入，旧页向左退出
            fromOffset = -p * w;
            toOffset = (1.0f - p) * w;
        } else {
            // 上一页从左侧进入，旧页向右退出
            fromOffset = p * w;
            toOffset = -(1.0f - p) * w;
        }

        GraphicsState state = g.Save();
        g.SetClip(card);
        DrawPageGridAt(g, s, s.animFromPage, fromOffset);
        DrawPageGridAt(g, s, s.animToPage, toOffset);
        g.Restore(state);
    } else {
        DrawPageGridAt(g, s, s.currentPage, 0.0f);
    }

    // 分页圆点
    const float dotR = 3.2f * k;
    const float totalW = s.pages.size() * 12.0f * k;
    float x = s.width / 2.0f - totalW / 2.0f;
    const float y = s.height - 12.0f * k;
    for (int i = 0; i < static_cast<int>(s.pages.size()); ++i) {
        SolidBrush dot(i == s.currentPage ? Color(255, 255, 255, 255)
                                          : Color(120, 255, 255, 255));
        g.FillEllipse(&dot, x - dotR, y - dotR, dotR * 2.0f, dotR * 2.0f);
        x += 12.0f * k;
    }
}

void DrawFolderPanel(Graphics& g, AppState& s) {
    const float k = s.scale;
    const RectF panel = FolderPanelRect(s);
    GraphicsPath path;
    DrawRoundedCardPath(path, panel, 18.0f * k);
    SolidBrush bg(Color(235, 28, 30, 36));
    g.FillPath(&bg, &path);
    Pen border(Color(160, 255, 255, 255), 1.2f * k);
    g.DrawPath(&border, &path);

    FontFamily family(L"Microsoft YaHei UI");
    Gdiplus::Font titleFont(&family, std::round(14.0f * k), FontStyleBold, UnitPixel);

    std::wstring title = L"文件夹";
    if (s.folderPage >= 0 && s.folderSlot >= 0 &&
        s.folderPage < static_cast<int>(s.pages.size())) {
        const AppEntry& folder = s.pages[s.folderPage].slots[s.folderSlot];
        if (!folder.folderName.empty()) title = folder.folderName;
    }
    DrawStringCentered(g, title, &titleFont,
                       RectF(panel.X, panel.Y + 6.0f * k, panel.Width, 26.0f * k),
                       Color(255, 250, 250, 252));

    if (s.folderPage < 0 || s.folderSlot < 0 ||
        s.folderPage >= static_cast<int>(s.pages.size())) return;
    AppEntry& folder = s.pages[s.folderPage].slots[s.folderSlot];
    if (folder.type != 2) return;

    Gdiplus::Font labelFont(&family, std::round(11.5f * k), FontStyleBold, UnitPixel);
    for (int i = 0; i < static_cast<int>(folder.folderApps.size()); ++i) {
        const RectF cell = FolderCellRect(s, i);
        const float iconSize = std::min(cell.Width, cell.Height) * 0.58f;
        const RectF iconRect(cell.X + (cell.Width - iconSize) / 2.0f,
                             cell.Y + 4.0f * k, iconSize, iconSize);
        const bool pressed =
            s.pressPhase != AppState::PressPhase::None &&
            s.pressInFolder && s.pressChild == i;
        const RectF drawIcon = pressed
                                   ? ScaleRectAroundCenter(iconRect, s.pressScale)
                                   : iconRect;
        DrawAppIcon(g, s, folder.folderApps[i], drawIcon, k);
        if (pressed) {
            DrawPressTint(g, drawIcon, k, PressTintAlpha(s));
        }
        RectF textRect(cell.X + 1.0f * k, iconRect.Y + iconRect.Height + 2.0f * k,
                       cell.Width - 2.0f * k, cell.Height - iconRect.Height - 4.0f * k);
        DrawStringCentered(g, DisplayNameOf(folder.folderApps[i]), &labelFont,
                           textRect, Color(230, 245, 245, 247));
    }
}

void DrawFloatingIcon(Graphics& g, AppState& s) {
    if ((s.dragMode == AppState::DragMode::MainApp ||
         s.dragMode == AppState::DragMode::FolderChild) &&
        !s.dragMoved) {
        return; // 尚未超过拖拽阈值，仍是单击，不显示浮起图标
    }

    const std::wstring* path = nullptr;
    const std::vector<std::wstring>* folderApps = nullptr;
    bool isFolder = false;

    if (s.dragMode == AppState::DragMode::MainApp && s.dragPage >= 0 &&
        s.dragSlot >= 0 && s.dragPage < static_cast<int>(s.pages.size())) {
        const AppEntry& entry = s.pages[s.dragPage].slots[s.dragSlot];
        path = &entry.path;
        isFolder = (entry.type == 2);
        if (isFolder) folderApps = &entry.folderApps;
    } else if (s.dragMode == AppState::DragMode::FolderChild && s.folderPage >= 0 &&
               s.folderSlot >= 0 && s.dragChild >= 0 &&
               s.folderPage < static_cast<int>(s.pages.size())) {
        const AppEntry& folder = s.pages[s.folderPage].slots[s.folderSlot];
        if (folder.type == 2 && s.dragChild < static_cast<int>(folder.folderApps.size())) {
            path = &folder.folderApps[s.dragChild];
        }
    }

    if ((!isFolder && !path) || (isFolder && !folderApps)) return;
    if (!isFolder && path->empty()) return;

    POINT pt = s.lastCursor;
    ScreenToClient(s.hwnd, &pt);
    const float k = s.scale;
    const float size = 52.0f * k;
    RectF rect(pt.x - size / 2.0f, pt.y - size / 2.0f, size, size);
    if (isFolder) {
        DrawFolderIcon(g, s, *folderApps, rect, k);
    } else {
        DrawAppIcon(g, s, *path, rect, k);
    }
}

void DrawLauncher(AppState& s) {
    Gdiplus::Bitmap* bmp = s.bitmap;
    if (!bmp) return;
    Graphics g(bmp);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    g.SetPixelOffsetMode(PixelOffsetModeHighQuality);
    g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    g.SetTextContrast(6);
    g.Clear(Color(0, 0, 0, 0));

    DrawCardBackground(g, s);
    if (s.folderOpen) {
        DrawFolderPanel(g, s);
    } else {
        DrawMainGrid(g, s);
    }
    DrawFloatingIcon(g, s);
    PresentLauncher(s);
}

// ---------- 拖拽逻辑 ----------

int HitMainAppSlot(const AppState& s, POINT client) {
    const int slot = HitMainSlot(s, client);
    if (slot < 0 || s.currentPage < 0 ||
        s.currentPage >= static_cast<int>(s.pages.size())) return -1;
    return (s.pages[s.currentPage].slots[slot].type != 0) ? slot : -1;
}

void OnClick(AppState& s, POINT client);

void ResetDrag(AppState& s) {
    s.dragMode = AppState::DragMode::None;
    s.dragPage = -1;
    s.dragSlot = -1;
    s.dragChild = -1;
    s.edgeSide = 0;
    s.edgeHandled = false;
    s.edgeStart = 0;
    s.dragMoved = false;
}

void HandleEdgeHold(AppState& s) {
    if (s.dragMode != AppState::DragMode::MainApp) return;
    POINT pt = s.lastCursor;
    ScreenToClient(s.hwnd, &pt);
    const int edge = static_cast<int>(26.0f * s.scale);
    int side = 0;
    if (pt.x >= s.width - edge) side = 1;
    else if (pt.x <= edge) side = -1;

    if (side != s.edgeSide) {
        s.edgeSide = side;
        s.edgeStart = GetTickCount64();
        s.edgeHandled = false;
        return;
    }
    if (side == 0 || s.edgeHandled) return;
    if (GetTickCount64() - s.edgeStart < 650) return;

    if (side == 1) {
        if (s.currentPage < static_cast<int>(s.pages.size()) - 1) {
            StartPageAnimation(s, s.currentPage + 1);
        } else if (static_cast<int>(s.pages.size()) < kMaxPages) {
            s.pages.push_back(PageData{});
            StartPageAnimation(s, static_cast<int>(s.pages.size()) - 1);
        }
    } else if (side == -1 && s.currentPage > 0) {
        StartPageAnimation(s, s.currentPage - 1);
    }
    s.edgeHandled = true;
}

void DropMainDrag(AppState& s, POINT client) {
    if (s.dragMode != AppState::DragMode::MainApp) return;
    const int srcPage = s.dragPage;
    const int srcSlot = s.dragSlot;
    if (srcPage < 0 || srcSlot < 0 ||
        srcPage >= static_cast<int>(s.pages.size()) ||
        srcSlot >= kSlots) { ResetDrag(s); return; }

    AppEntry dragged = s.pages[srcPage].slots[srcSlot];
    const int target = HitMainSlot(s, client);

    // 目标为空则移动；目标为应用则创建文件夹；目标为文件夹则加入
    if (target >= 0 && target != srcSlot) {
        AppEntry& dst = s.pages[s.currentPage].slots[target];
        if (dst.type == 0) {
            if (srcPage == s.currentPage && srcSlot == target) {
                // same
            } else {
                dst = dragged;
                s.pages[srcPage].slots[srcSlot] = AppEntry{};
            }
        } else if (dst.type == 1) {
            if (dragged.type == 1) {
                AppEntry folder;
                folder.type = 2;
                folder.folderName = L"文件夹";
                folder.folderApps.push_back(dst.path);
                folder.folderApps.push_back(dragged.path);
                s.pages[s.currentPage].slots[target] = folder;
                s.pages[srcPage].slots[srcSlot] = AppEntry{};
            } else if (dragged.type == 2) {
                // 文件夹不能与普通应用继续堆叠，忽略
            }
        } else if (dst.type == 2 && dragged.type == 1) {
            if (static_cast<int>(dst.folderApps.size()) < kMaxFolderApps) {
                dst.folderApps.push_back(dragged.path);
                s.pages[srcPage].slots[srcSlot] = AppEntry{};
            }
        }
    } else if (target < 0 && srcPage != s.currentPage) {
        // 翻页后落到页面空白区域：放到当前页第一个空位
        int slot = FirstEmptySlot(s.pages[s.currentPage]);
        if (slot < 0 && static_cast<int>(s.pages.size()) < kMaxPages) {
            s.pages.push_back(PageData{});
            s.currentPage = static_cast<int>(s.pages.size()) - 1;
            slot = FirstEmptySlot(s.pages[s.currentPage]);
        }
        if (slot >= 0) {
            s.pages[s.currentPage].slots[slot] = dragged;
            s.pages[srcPage].slots[srcSlot] = AppEntry{};
        }
    }
    PruneEmptyPages(s);
    SavePages(s);
    ResetDrag(s);
}

void DropFolderChildDrag(AppState& s, POINT client) {
    if (s.dragMode != AppState::DragMode::FolderChild) return;
    const int child = s.dragChild;
    if (child < 0 || s.folderPage < 0 || s.folderSlot < 0 ||
        s.folderPage >= static_cast<int>(s.pages.size())) { ResetDrag(s); return; }

    AppEntry& folder = s.pages[s.folderPage].slots[s.folderSlot];
    if (folder.type != 2 || child >= static_cast<int>(folder.folderApps.size())) {
        ResetDrag(s); return;
    }

    const RectF panel = FolderPanelRect(s);
    const float x = static_cast<float>(client.x);
    const float y = static_cast<float>(client.y);

    if (x < panel.X || x >= panel.X + panel.Width ||
        y < panel.Y || y >= panel.Y + panel.Height) {
        // 拖出文件夹 -> 回到主界面
        RemoveChildFromFolder(s, child);
    } else {
        // 文件夹内部排序：与目标子应用交换
        const int target = HitFolderChild(s, client);
        if (target >= 0 && target != child &&
            target < static_cast<int>(folder.folderApps.size())) {
            std::swap(folder.folderApps[child], folder.folderApps[target]);
        }
    }
    SavePages(s);
    ResetDrag(s);
}

void OnLeftButtonDown(AppState& s, WPARAM wParam, LPARAM lParam) {
    POINT client{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
    (void)wParam;
    s.lastCursor = client;
    s.downClient = client;
    s.dragMoved = false;
    GetCursorPos(&s.lastCursor);

    if (s.renamingFolder) {
        CommitFolderRename(s);
        return; // 本次点击只用于结束重命名
    }

    // 文件夹打开时：点击面板外任意区域关闭文件夹（无关闭按钮）
    if (s.folderOpen) {
        const RectF panel = FolderPanelRect(s);
        const float x = static_cast<float>(client.x);
        const float y = static_cast<float>(client.y);
        if (x < panel.X || x >= panel.X + panel.Width ||
            y < panel.Y || y >= panel.Y + panel.Height) {
            CloseFolder(s);
            DrawLauncher(s);
            return;
        }
        const int child = HitFolderChild(s, client);
        if (child >= 0 && s.folderPage >= 0 && s.folderSlot >= 0 &&
            s.folderPage < static_cast<int>(s.pages.size())) {
            const AppEntry& folder = s.pages[s.folderPage].slots[s.folderSlot];
            if (child < static_cast<int>(folder.folderApps.size())) {
                s.dragMode = AppState::DragMode::FolderChild;
                s.dragChild = child;
                StartPress(s, true, s.folderPage, s.folderSlot, child);
                SetCapture(s.hwnd);
                return;
            }
        }
        return;
    }

    // 顶部手柄：拖动组件
    if (IsInTopHandle(s, client)) {
        s.dragMode = AppState::DragMode::Widget;
        s.draggingWidget = true;
        POINT cursor{};
        GetCursorPos(&cursor);
        s.widgetCursor = cursor;
        RECT rc{};
        GetWindowRect(s.hwnd, &rc);
        s.widgetOrigin.x = rc.left;
        s.widgetOrigin.y = rc.top;
        SetCapture(s.hwnd);
        return;
    }

    if (s.pageAnimating) {
        return; // 翻页动画过程中忽略网格点击
    }

    const int slot = HitMainSlot(s, client);
    if (slot >= 0 && s.pages[s.currentPage].slots[slot].type != 0) {
        s.dragMode = AppState::DragMode::MainApp;
        s.dragPage = s.currentPage;
        s.dragSlot = slot;
        StartPress(s, false, s.currentPage, slot, -1);
        SetCapture(s.hwnd);
    } else {
        s.dragMode = AppState::DragMode::PageSwipe;
        s.swipeStartX = client.x;
        s.swipeLastX = client.x;
        SetCapture(s.hwnd);
    }
}

void OnMouseMove(AppState& s, WPARAM wParam, LPARAM lParam) {
    POINT client{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
    (void)wParam;
    GetCursorPos(&s.lastCursor);

    const int dx = client.x - s.downClient.x;
    const int dy = client.y - s.downClient.y;
    if (std::abs(dx) > 4.0f * s.scale || std::abs(dy) > 4.0f * s.scale) {
        s.dragMoved = true;
        CancelPress(s); // 开始拖动：取消按压动画
    }

    if (s.dragMode == AppState::DragMode::Widget) {
        POINT cursor = s.lastCursor;
        const int x = s.widgetOrigin.x + (cursor.x - s.widgetCursor.x);
        const int y = s.widgetOrigin.y + (cursor.y - s.widgetCursor.y);
        SetWidgetScreenPos(s.hwnd, x, y, 0, 0,
                           SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        DrawLauncher(s);
        return;
    }

    if (s.dragMode == AppState::DragMode::MainApp) {
        if (!s.dragMoved) {
            return; // 还没超过阈值，不显示拖动状态
        }
        HandleEdgeHold(s);
        DrawLauncher(s);
        return;
    }

    if (s.dragMode == AppState::DragMode::FolderChild) {
        if (!s.dragMoved) {
            return;
        }
        DrawLauncher(s);
        return;
    }

    if (s.dragMode == AppState::DragMode::PageSwipe) {
        s.swipeLastX = client.x;
        DrawLauncher(s);
    }
}

void OnLeftButtonUp(AppState& s, WPARAM wParam, LPARAM lParam) {
    POINT client{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
    (void)wParam;
    GetCursorPos(&s.lastCursor);

    if (s.dragMode == AppState::DragMode::Widget) {
        s.draggingWidget = false;
        ReleaseCapture();
        SavePosition(s.hwnd);
        ResetDrag(s);
        return;
    }

    if (s.dragMode == AppState::DragMode::MainApp) {
        if (!s.dragMoved) {
            if (PressTargetMatches(s, client)) {
                StartPressRelease(s); // 松手：弹簧回弹
            } else {
                CancelPress(s);
            }
            ReleaseCapture();
            ResetDrag(s);
            OnClick(s, client);
            DrawLauncher(s);
        } else {
            DropMainDrag(s, client);
            DrawLauncher(s);
            ReleaseCapture();
        }
        return;
    }

    if (s.dragMode == AppState::DragMode::FolderChild) {
        if (!s.dragMoved) {
            if (PressTargetMatches(s, client)) {
                StartPressRelease(s); // 松手：弹簧回弹
            } else {
                CancelPress(s);
            }
            ReleaseCapture();
            ResetDrag(s);
            OnClick(s, client);
            DrawLauncher(s);
        } else {
            DropFolderChildDrag(s, client);
            DrawLauncher(s);
            ReleaseCapture();
        }
        return;
    }

    if (s.dragMode == AppState::DragMode::PageSwipe) {
        const int dx = client.x - s.swipeStartX;
        if (s.dragMoved && std::abs(dx) > 45.0f * s.scale) {
            const float startRatio = std::clamp(
                std::abs(static_cast<float>(dx)) / static_cast<float>(s.width),
                0.0f, 1.0f);
            if (dx < 0 && s.currentPage < static_cast<int>(s.pages.size()) - 1) {
                StartPageAnimation(s, s.currentPage + 1, startRatio);
            } else if (dx > 0 && s.currentPage > 0) {
                StartPageAnimation(s, s.currentPage - 1, startRatio);
            }
        } else {
            // 短按空白区域：不做任何事
        }
        ReleaseCapture();
        ResetDrag(s);
        SavePages(s);
        DrawLauncher(s);
    }
}

void OnClick(AppState& s, POINT client) {
    // 单击打开应用或文件夹
    if (s.folderOpen) {
        const int child = HitFolderChild(s, client);
        if (child >= 0 && s.folderPage >= 0 && s.folderSlot >= 0 &&
            s.folderPage < static_cast<int>(s.pages.size())) {
            const AppEntry& folder = s.pages[s.folderPage].slots[s.folderSlot];
            if (folder.type == 2 && child < static_cast<int>(folder.folderApps.size())) {
                ShellExecuteW(s.hwnd, L"open", folder.folderApps[child].c_str(),
                              nullptr, nullptr, SW_SHOWNORMAL);
            }
        }
        return;
    }

    const int slot = HitMainSlot(s, client);
    if (slot >= 0 && s.currentPage < static_cast<int>(s.pages.size())) {
        const AppEntry& entry = s.pages[s.currentPage].slots[slot];
        if (entry.type == 1) {
            ShellExecuteW(s.hwnd, L"open", entry.path.c_str(),
                          nullptr, nullptr, SW_SHOWNORMAL);
        } else if (entry.type == 2) {
            OpenFolder(s, s.currentPage, slot);
            DrawLauncher(s);
        }
    }
}

// ---------- 右键菜单 ----------

void ShowContextMenu(AppState& s, POINT client) {
    HMENU menu = CreatePopupMenu();
    if (!menu) return;

    int slot = -1;
    bool folderOpen = s.folderOpen;
    if (!folderOpen) slot = HitMainSlot(s, client);

    if (slot >= 0 && s.currentPage < static_cast<int>(s.pages.size())) {
        const AppEntry& entry = s.pages[s.currentPage].slots[slot];
        if (entry.type == 1) {
            AppendMenuW(menu, MF_STRING, kMenuAppOpen, L"打开");
            AppendMenuW(menu, MF_STRING, kMenuAppRemove, L"移除应用");
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        } else if (entry.type == 2) {
            AppendMenuW(menu, MF_STRING, kMenuFolderOpen, L"打开");
            AppendMenuW(menu, MF_STRING, kMenuFolderRename, L"重命名");
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        }
    }
    AppendMenuW(menu, MF_STRING, kMenuExit, L"退出 DesktopLauncher");

    POINT pt{};
    GetCursorPos(&pt);
    SetForegroundWindow(s.hwnd);
    const UINT flags = TPM_RIGHTBUTTON | TPM_LEFTALIGN | TPM_RETURNCMD | TPM_NONOTIFY;
    const int cmd = TrackPopupMenu(menu, flags, pt.x, pt.y, 0, s.hwnd, nullptr);
    DestroyMenu(menu);

    if (cmd == kMenuAppOpen && slot >= 0) {
        ShellExecuteW(s.hwnd, L"open",
                      s.pages[s.currentPage].slots[slot].path.c_str(),
                      nullptr, nullptr, SW_SHOWNORMAL);
    } else if (cmd == kMenuAppRemove && slot >= 0) {
        RemoveAppFromSlot(s, s.currentPage, slot);
        SavePages(s);
        DrawLauncher(s);
    } else if (cmd == kMenuFolderOpen && slot >= 0) {
        OpenFolder(s, s.currentPage, slot);
        DrawLauncher(s);
    } else if (cmd == kMenuFolderRename && slot >= 0) {
        StartFolderRename(s, s.currentPage, slot);
    } else if (cmd == kMenuExit) {
        DestroyWindow(s.hwnd);
    }
}

// ---------- 文件拖入 ----------

void HandleDroppedFiles(AppState& s, WPARAM wParam) {
    HDROP drop = reinterpret_cast<HDROP>(wParam);
    const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    for (UINT i = 0; i < count; ++i) {
        const UINT len = DragQueryFileW(drop, i, nullptr, 0);
        if (len == 0) continue;
        std::wstring path(len + 1, L'\0');
        DragQueryFileW(drop, i, &path[0], static_cast<UINT>(path.size()));
        while (!path.empty() && path.back() == L'\0') path.pop_back();
        if (IsAcceptableDrop(path)) {
            AddAppSomewhere(s, path);
        }
    }
    DragFinish(drop);
    SavePages(s);
    DrawLauncher(s);
}

// ---------- WndProc ----------

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
        s->hwnd = hwnd;
        return DefWindowProcW(hwnd, msg, wParam, lParam);

    case WM_CREATE: {
        if (!s) return -1;
        s->hwnd = hwnd;
        s->dpi = GetWindowDpi(hwnd);
        const int w = MulDiv(kBaseWidth, s->dpi, 96);
        const int h = MulDiv(kBaseHeight, s->dpi, 96);

        RECT initial{};
        GetWindowRect(hwnd, &initial);
        if (!AttachToDesktop(hwnd, initial.left, initial.top, w, h)) {
            SetWindowPos(hwnd, HWND_BOTTOM, initial.left, initial.top, w, h,
                         SWP_NOACTIVATE);
        }

        LoadPages(*s);
        PruneEmptyPages(*s);
        SavePages(*s);
        if (!CreateBacking(*s, w, h)) {
            MessageBoxW(hwnd, L"创建绘图缓冲失败。", L"DesktopLauncher",
                        MB_OK | MB_ICONERROR);
            return -1;
        }
        DragAcceptFiles(hwnd, TRUE);
        SetTimer(hwnd, kDrawTimerId, kDrawIntervalMs, nullptr);
        DrawLauncher(*s);
        return 0;
    }

    case WM_COMMAND:
        if (s && s->renameEdit && reinterpret_cast<HWND>(lParam) == s->renameEdit &&
            HIWORD(wParam) == EN_CHANGE) {
            wchar_t buf[64] = {};
            GetWindowTextW(s->renameEdit, buf, 64);
            s->renameText = buf;
            DrawLauncher(*s);
            return 0;
        }
        break;

    case kRenameCommand:
        if (s && s->renamingFolder) {
            if (wParam == VK_RETURN) {
                CommitFolderRename(*s);
            } else {
                CancelFolderRename(*s);
            }
            return 0;
        }
        break;

    case WM_CTLCOLOREDIT:
        if (s && s->renameEdit && reinterpret_cast<HWND>(lParam) == s->renameEdit) {
            HDC dc = reinterpret_cast<HDC>(wParam);
            SetTextColor(dc, RGB(255, 255, 255));
            SetBkColor(dc, RGB(20, 22, 26));
            return reinterpret_cast<LRESULT>(s->renameBrush);
        }
        break;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps{};
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_WINDOWPOSCHANGING: {
        // 防御：任何 Z 序变化都强制回到桌面层底部，防止覆盖普通窗口
        auto* wp = reinterpret_cast<WINDOWPOS*>(lParam);
        if ((wp->flags & SWP_NOZORDER) == 0 && wp->hwndInsertAfter != HWND_BOTTOM) {
            wp->hwndInsertAfter = HWND_BOTTOM;
            wp->flags |= SWP_NOACTIVATE;
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    case WM_NCHITTEST:
        if (!s || s->width <= 0) return HTTRANSPARENT;
        return HitTestCard(hwnd, *s, lParam) ? HTCLIENT : HTTRANSPARENT;

    case WM_LBUTTONDOWN:
        if (!s) return 0;
        OnLeftButtonDown(*s, wParam, lParam);
        return 0;

    case WM_MOUSEMOVE:
        if (!s) return 0;
        OnMouseMove(*s, wParam, lParam);
        return 0;

    case WM_LBUTTONUP:
        if (!s) return 0;
        OnLeftButtonUp(*s, wParam, lParam);
        return 0;

    case WM_CAPTURECHANGED:
        if (s) {
            if (s->pressPhase == AppState::PressPhase::Pressing) {
                CancelPress(*s); // 捕获丢失（如切换窗口）：取消按压
            }
            ResetDrag(*s);
        }
        return 0;

    case WM_RBUTTONUP: {
        if (!s) return 0;
        POINT client{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ShowContextMenu(*s, client);
        return 0;
    }

    case WM_DROPFILES:
        if (s) HandleDroppedFiles(*s, wParam);
        return 0;

    case WM_KEYDOWN:
        if (!s || !s->renamingFolder) break;
        if (wParam == VK_RETURN) {
            CommitFolderRename(*s);
            return 0;
        }
        if (wParam == VK_ESCAPE) {
            CancelFolderRename(*s);
            return 0;
        }
        if (wParam == VK_BACK && !s->renameText.empty()) {
            s->renameText.pop_back();
            DrawLauncher(*s);
            return 0;
        }
        break;

    case WM_CHAR:
        if (s && s->renamingFolder && wParam >= 32) {
            if (s->renameText.size() < 16) {
                s->renameText.push_back(static_cast<wchar_t>(wParam));
                DrawLauncher(*s);
            }
            return 0;
        }
        break;

    case WM_TIMER:
        if (!s) return 0;
        if (wParam == kDrawTimerId) {
            if (s->renamingFolder) {
                // 光标闪烁和输入回显
                DrawLauncher(*s);
            } else if (s->pageAnimating) {
                const ULONGLONG now = GetTickCount64();
                const float raw = static_cast<float>(now - s->animStart) /
                                  static_cast<float>(kPageAnimMs);
                if (raw >= 1.0f) {
                    s->animProgress = 1.0f;
                    s->pageAnimating = false;
                } else {
                    s->animProgress = raw;
                }
                DrawLauncher(*s);
            } else if (s->dragMode == AppState::DragMode::MainApp) {
                HandleEdgeHold(*s);
                DrawLauncher(*s);
            } else if (s->pressPhase != AppState::PressPhase::None &&
                       UpdatePressAnim(*s)) {
                DrawLauncher(*s);
            } else if (s->dirty) {
                s->dirty = false;
                DrawLauncher(*s);
            }
        }
        return 0;

    case WM_DPICHANGED: {
        if (!s) return 0;
        const UINT newDpi = HIWORD(wParam);
        const auto* suggested = reinterpret_cast<RECT*>(lParam);
        s->dpi = static_cast<int>(newDpi);
        const int w = MulDiv(kBaseWidth, newDpi, 96);
        const int h = MulDiv(kBaseHeight, newDpi, 96);
        s->dragMode = AppState::DragMode::None;
        CancelPress(*s);
        if (suggested) {
            SetWidgetScreenPos(hwnd, suggested->left, suggested->top, w, h,
                               SWP_NOZORDER | SWP_NOACTIVATE);
        } else {
            RECT rc{};
            GetWindowRect(hwnd, &rc);
            SetWidgetScreenPos(hwnd, rc.left, rc.top, w, h,
                               SWP_NOZORDER | SWP_NOACTIVATE);
        }
        if (CreateBacking(*s, w, h)) DrawLauncher(*s);
        return 0;
    }

    case WM_DESTROY:
        KillTimer(hwnd, kDrawTimerId);
        SavePosition(hwnd);
        if (s && s->renamingFolder) {
            CommitFolderRename(*s);
        }
        SavePages(*s);
        if (s) DestroyBacking(*s);
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace

int APIENTRY WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int) {
    HANDLE mutex = CreateMutexW(nullptr, TRUE, kMutexName);
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(mutex);
        return 0;
    }

    EnableDpiAwareness();

    // SHGetFileInfoW 等 Shell API 需要 STA COM 才能正确解析 .lnk 的自定义图标；
    // 未初始化时图标索引会退回 0（系统默认空白文件图标）
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
        MessageBoxW(nullptr, L"COM 初始化失败。", L"DesktopLauncher",
                    MB_OK | MB_ICONERROR);
        CloseHandle(mutex);
        return 1;
    }

    ULONG_PTR gdiplusToken = 0;
    GdiplusStartupInput gsi;
    if (GdiplusStartup(&gdiplusToken, &gsi, nullptr) != Ok) {
        MessageBoxW(nullptr, L"GDI+ 初始化失败。", L"DesktopLauncher",
                    MB_OK | MB_ICONERROR);
        CloseHandle(mutex);
        return 1;
    }

    HDC screenDc = GetDC(nullptr);
    const int systemDpi = screenDc ? GetDeviceCaps(screenDc, LOGPIXELSX) : 96;
    if (screenDc) ReleaseDC(nullptr, screenDc);
    const int width = MulDiv(kBaseWidth, systemDpi, 96);
    const int height = MulDiv(kBaseHeight, systemDpi, 96);

    RECT workArea{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
    POINT pos{workArea.left + kDefaultMargin,
              workArea.bottom - height - kDefaultMargin};
    LoadSavedPosition(pos);
    ClampToWorkArea(width, height, pos);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kWindowClass;

    if (!RegisterClassExW(&wc)) {
        GdiplusShutdown(gdiplusToken);
        CloseHandle(mutex);
        return 1;
    }

    AppState state;
    HWND hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        kWindowClass, L"DesktopLauncher", WS_POPUP,
        pos.x, pos.y, width, height,
        nullptr, nullptr, hInstance, &state);

    if (!hwnd) {
        GdiplusShutdown(gdiplusToken);
        CloseHandle(mutex);
        return 1;
    }

    ShowWindow(hwnd, SW_SHOWNOACTIVATE);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    GdiplusShutdown(gdiplusToken);
    CoUninitialize();
    CloseHandle(mutex);
    return static_cast<int>(msg.wParam);
}
