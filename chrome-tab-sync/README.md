# chrome-tab-sync — DesktopTopBar Chrome 标签同步扩展

把 Chrome 当前聚焦窗口的**真实标签页**实时同步到 DesktopTopBar 桌面顶栏，
顶栏可直接操作这些标签：点击切换、悬停 × / 中键关闭、右侧 + / 右键新建。

## 安装（一次即可）

1. 打开 Chrome，地址栏进入 `chrome://extensions/`
2. 右上角打开 **开发者模式**
3. 点击 **加载已解压的扩展程序**，选择本目录 `chrome-tab-sync`
4. 完成。Chrome 重启后扩展自动随浏览器启动并重连顶栏

> 无需注册表、无需 Chrome 启动参数、无需重启浏览器。

## 使用

| 操作 | 效果 |
| --- | --- |
| 聚焦任意 Chrome/Edge 窗口 | 顶栏标签变为该窗口内的真实标签页（标题实时同步） |
| 左键点击标签 | 切换到该标签页（并聚焦对应 Chrome 窗口） |
| 悬停标签右侧的 × | 关闭该标签页 |
| 鼠标中键点击标签 | 关闭该标签页 |
| 标签区末尾的 ＋ | 新建标签页 |
| 右键标签 | 新建标签页 |
| 关闭 Chrome / 卸载扩展 | 顶栏自动回退为原来的「窗口枚举」模式 |

多 Chrome 窗口场景：顶栏始终显示**当前聚焦**的 Chrome 窗口的标签；
通过顶栏切换到另一窗口的标签时，同步目标自动跟随。

## 工作原理

```
Chrome 扩展（chrome.tabs API）
   │  WebSocket ws://127.0.0.1:9786
   ▼
DesktopTopBar 内置 WS 服务端（顶栏进程内，仅绑定本机回环地址）
   │  事件推送：hello / windowFocused / tabCreated|Updated|Removed|Moved|Activated
   │  命令下发：activateTab / closeTab / newTab
   ▼
顶栏标签区渲染（Chrome 风格，含激活高亮 / 固定标签圆点 / 关闭× / ＋按钮）
```

- 扩展只向顶栏推送**当前聚焦 Chrome 窗口**的标签（`tabs` + `alarms` 权限，
  无需任何网站权限）
- 顶栏内置的 WS 服务端只监听 `127.0.0.1:9786`，不对外网开放
- 心跳：顶栏空闲 25s 发一次 ping，扩展自动 pong；扩展侧每 15s 保活一次，
  断线 2s 后自动重连

## 文件

- `manifest.json` — MV3 清单（权限：`tabs`、`alarms`）
- `background.js` — 后台服务：WS 桥 + tabs/windows 事件监听 + 命令处理

## 故障排查

- **顶栏没显示 Chrome 标签**：确认扩展已加载（`chrome://extensions` 中无报错），
  且顶栏进程在运行；检查端口是否被占用：`netstat -ano | findstr :9786`
  （占用者应是 `DesktopTopBar-x64.exe`）。
- **标签不更新**：刷新扩展页面即可重连；或重启顶栏（运行 `start_topbar.bat`）。
- **不想用扩展时**：在 `chrome://extensions` 停用即可，顶栏自动回到原模式。
