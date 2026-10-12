// DesktopTopBar 标签同步 - 后台服务
//
// 职责：
//   1. 通过 WebSocket 连接 DesktopTopBar 内置的本地服务（ws://127.0.0.1:9786）
//   2. 把「当前聚焦的 Chrome 窗口」的标签列表推送过去（hello / windowFocused）
//   3. 监听 chrome.tabs / chrome.windows 事件，增量推送标签变化
//   4. 接收顶栏命令：激活标签 / 关闭标签 / 新建标签 / 移动标签（拖动重排）
//
// 只同步最后一个聚焦的普通 Chrome 窗口：顶栏展示的是「聚焦窗口」所属
// 应用的标签，聚焦窗口变化时通过 windowFocused 全量刷新。
//
// 【MV3 保活，必须理解】
// MV3 的 Service Worker 空闲约 30s 就会被浏览器回收，SW 持有的 WebSocket
// 会被一起关闭——也就是说"省电模式"与否无关，这是 MV3 的固有行为。
// 关键点（Chrome/Edge 116+）：只有「JS 收到一条 WS 消息」才会重置这 30s
// 回收计时器；WS 协议层的 ping/pong 帧由网络栈直接应答，不会派发到 JS，
// 所以对保活无效（旧版扩展 60s 才发一次心跳，SW 早已被回收，顶栏每 60s
// 只有约一半时间能拿到标签，断连期间回退成"窗口枚举"，看起来就像
// "顶栏只剩当前标签页"）。
// 因此本扩展与顶栏互发**应用层 JSON 心跳**（15s，30s 阈值的 2 倍余量）：
// 只要顶栏在运行，SW 就常驻、连接就不断。alarm（30s）仅作为 SW 真被
// 回收后的复活兜底。

const WS_URL = 'ws://127.0.0.1:9786/';

// 应用层心跳间隔：必须 < SW 空闲回收阈值（约 30s），15s 留 2 倍余量。
// 每次心跳都是"收到消息 + 发出消息"，双向都会重置回收计时器。
const KEEPALIVE_MS = 15000;

let ws = null;
let reconnectTimer = null;
let keepAliveTimer = null;
let syncWindowId = null; // 当前同步的 Chrome 窗口 id

// ---- WebSocket 连接管理 ----

function connect() {
  if (ws && (ws.readyState === WebSocket.OPEN || ws.readyState === WebSocket.CONNECTING)) {
    return;
  }
  try {
    ws = new WebSocket(WS_URL);
  } catch (e) {
    scheduleReconnect();
    return;
  }
  ws.onopen = () => {
    startKeepAlive();
    sendHello();
  };
  ws.onclose = () => {
    stopKeepAlive();
    ws = null;
    scheduleReconnect();
  };
  ws.onerror = () => {
    try {
      ws.close();
    } catch (e) {}
  };
  ws.onmessage = (ev) => {
    handleMessage(ev.data);
  };
}

function scheduleReconnect() {
  clearTimeout(reconnectTimer);
  reconnectTimer = setTimeout(connect, 2000);
}

function send(obj) {
  if (ws && ws.readyState === WebSocket.OPEN) {
    try {
      ws.send(JSON.stringify(obj));
    } catch (e) {}
  }
}

// 顶栏运行期间用应用层心跳把 SW 钉住（见文件头说明）
function startKeepAlive() {
  stopKeepAlive();
  keepAliveTimer = setInterval(() => {
    send({ type: 'ping' });
  }, KEEPALIVE_MS);
}

function stopKeepAlive() {
  if (keepAliveTimer !== null) {
    clearInterval(keepAliveTimer);
    keepAliveTimer = null;
  }
}

// ---- 数据 ----

function tabPayload(tab) {
  return {
    id: tab.id,
    index: tab.index,
    title: tab.title || '',
    url: tab.url || '',
    active: !!tab.active,
    pinned: !!tab.pinned
  };
}

function isSyncWindow(windowId) {
  return syncWindowId !== null && windowId === syncWindowId;
}

// 选出要同步的窗口：优先"用户最后使用的窗口"。
// getLastFocused 在浏览器整体失焦（例如用户正操作顶栏）时仍返回上次使用的
// 窗口；旧的实现直接退回 wins[0]，SW 重启时可能随机选中另一个只开了一个
// 标签的窗口，同样会表现为"顶栏只剩一个标签"。
async function lastFocusedChromeWindow() {
  try {
    const last = await chrome.windows.getLastFocused({ windowTypes: ['normal'] });
    if (last && last.id !== chrome.windows.WINDOW_ID_NONE) {
      return last;
    }
  } catch (e) {}
  try {
    const wins = await chrome.windows.getAll({ windowTypes: ['normal'] });
    if (!wins.length) return null;
    for (const w of wins) {
      if (w.focused) return w;
    }
    if (syncWindowId !== null) {
      const keep = wins.find((w) => w.id === syncWindowId);
      if (keep) return keep;
    }
    return wins[0];
  } catch (e) {
    return null;
  }
}

// 全量快照：当前聚焦 Chrome 窗口的全部标签
async function sendHello() {
  const win = await lastFocusedChromeWindow();
  if (!win) return;
  syncWindowId = win.id;
  try {
    const tabs = await chrome.tabs.query({ windowId: win.id });
    send({ type: 'hello', windowId: win.id, tabs: tabs.map(tabPayload) });
  } catch (e) {}
}

async function sendWindowSnapshot(windowId) {
  try {
    const w = await chrome.windows.get(windowId);
    if (!w || w.type !== 'normal') return;
    syncWindowId = windowId;
    const tabs = await chrome.tabs.query({ windowId });
    send({ type: 'windowFocused', windowId, tabs: tabs.map(tabPayload) });
  } catch (e) {}
}

// ---- 顶栏消息 ----

function handleMessage(data) {
  let msg;
  try {
    msg = JSON.parse(data);
  } catch (e) {
    return;
  }
  if (!msg || typeof msg !== 'object') return;
  // 应用层心跳：顶栏 15s 一次 ping，这里回 pong。
  // 这一来一回都会重置 SW 空闲回收计时器，是连接常驻的关键。
  if (msg.type === 'ping') {
    send({ type: 'pong' });
    return;
  }
  if (msg.type === 'pong') {
    return;
  }
  handleCommand(msg);
}

// ---- 顶栏命令 ----

function handleCommand(msg) {
  switch (msg.type) {
    case 'activateTab': {
      if (msg.tabId == null) break;
      chrome.tabs.get(msg.tabId).then((tab) => {
        if (!tab) return;
        chrome.tabs.update(tab.id, { active: true }).catch(() => {});
        chrome.windows.update(tab.windowId, { focused: true }).catch(() => {});
      }).catch(() => {});
      break;
    }
    case 'closeTab': {
      if (msg.tabId == null) break;
      chrome.tabs.remove(msg.tabId).catch(() => {});
      break;
    }
    case 'newTab': {
      // 新建标签页（顶栏右侧 + 按钮 / 右键标签）
      chrome.tabs.create({}).catch(() => {});
      break;
    }
    case 'moveTab': {
      // 顶栏拖动标签重排：把标签移动到指定位置。
      // 固定标签（pinned）永远排在窗口最前，chrome.tabs.move 对跨区
      // 移动会报错，所以这里按浏览器规则把落点夹到合法区间。
      if (msg.tabId == null || msg.index == null) break;
      const index = msg.index;
      chrome.tabs.get(msg.tabId).then((tab) => {
        if (!tab) return;
        chrome.tabs.query({ windowId: tab.windowId }).then((tabs) => {
          const pinnedCount = tabs.filter((t) => t.pinned).length;
          let target = index;
          if (tab.pinned) {
            target = Math.max(0, Math.min(target, pinnedCount - 1));
          } else {
            target = Math.max(pinnedCount, Math.min(target, tabs.length - 1));
          }
          if (target === tab.index) return;
          chrome.tabs.move(tab.id, { index: target }).catch(() => {});
        }).catch(() => {});
      }).catch(() => {});
      break;
    }
    default:
      break;
  }
}

// ---- tabs / windows 事件 -> 增量推送 ----

chrome.tabs.onCreated.addListener((tab) => {
  if (isSyncWindow(tab.windowId)) {
    send({ type: 'tabCreated', tab: tabPayload(tab) });
  }
});

chrome.tabs.onUpdated.addListener((tabId, changeInfo, tab) => {
  if (!isSyncWindow(tab.windowId)) return;
  const interesting =
    changeInfo.title !== undefined ||
    changeInfo.url !== undefined ||
    changeInfo.active !== undefined ||
    changeInfo.pinned !== undefined ||
    changeInfo.favIconUrl !== undefined;
  if (interesting) {
    send({ type: 'tabUpdated', tab: tabPayload(tab) });
  }
});

chrome.tabs.onRemoved.addListener((tabId, removeInfo) => {
  if (isSyncWindow(removeInfo.windowId)) {
    send({ type: 'tabRemoved', tabId, windowId: removeInfo.windowId });
  }
});

chrome.tabs.onMoved.addListener((tabId, moveInfo) => {
  if (isSyncWindow(moveInfo.windowId)) {
    send({ type: 'tabMoved', tabId, index: moveInfo.toIndex, windowId: moveInfo.windowId });
  }
});

chrome.tabs.onActivated.addListener((info) => {
  if (isSyncWindow(info.windowId)) {
    send({ type: 'tabActivated', tabId: info.tabId, windowId: info.windowId });
  }
});

// 标签跨窗口移动：涉及当前同步窗口时全量刷新（保持模型一致）
chrome.tabs.onDetached.addListener((tabId, detachInfo) => {
  if (isSyncWindow(detachInfo.oldWindowId)) {
    sendHello();
  }
});

chrome.tabs.onAttached.addListener((tabId, attachInfo) => {
  if (isSyncWindow(attachInfo.newWindowId)) {
    sendHello();
  }
});

// 聚焦窗口变化：切到 Chrome 窗口时全量推送该窗口标签
chrome.windows.onFocusChanged.addListener((windowId) => {
  if (windowId === chrome.windows.WINDOW_ID_NONE) return;
  sendWindowSnapshot(windowId);
});

// 同步窗口被关闭：回退到剩余聚焦窗口
chrome.windows.onRemoved.addListener((windowId) => {
  if (windowId === syncWindowId) {
    sendHello();
  }
});

// ---- 复活兜底 ----
//
// 正常情况下 SW 由应用层心跳保活（连接常驻，alarm 基本不会触发到"重连"分支）；
// 一旦 SW 仍被浏览器回收（例如系统休眠唤醒、浏览器强制清理），靠 alarm
// 把 SW 拉起来重连。0.5min 是 Chrome/Edge 允许的最小周期。
chrome.alarms.create('topbar-keepalive', { periodInMinutes: 0.5 });

chrome.alarms.onAlarm.addListener((alarm) => {
  if (alarm.name !== 'topbar-keepalive') return;
  if (ws && ws.readyState === WebSocket.OPEN) {
    send({ type: 'ping' });
  } else {
    connect();
  }
});

chrome.runtime.onStartup.addListener(() => connect());
chrome.runtime.onInstalled.addListener(() => connect());

// 立即连接
connect();
