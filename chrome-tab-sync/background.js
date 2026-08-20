// DesktopTopBar 标签同步 - 后台服务
//
// 职责：
//   1. 通过 WebSocket 连接 DesktopTopBar 内置的本地服务（ws://127.0.0.1:9786）
//   2. 把「当前聚焦的 Chrome 窗口」的标签列表推送过去（hello / windowFocused）
//   3. 监听 chrome.tabs / chrome.windows 事件，增量推送标签变化
//   4. 接收顶栏命令：激活标签 / 关闭标签 / 新建标签
//
// 只同步最后一个聚焦的普通 Chrome 窗口：顶栏展示的是「聚焦窗口」所属
// 应用的标签，聚焦窗口变化时通过 windowFocused 全量刷新。

const WS_URL = 'ws://127.0.0.1:9786/';

let ws = null;
let reconnectTimer = null;
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
    sendHello();
  };
  ws.onclose = () => {
    ws = null;
    scheduleReconnect();
  };
  ws.onerror = () => {
    try {
      ws.close();
    } catch (e) {}
  };
  ws.onmessage = (ev) => {
    handleCommand(ev.data);
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

async function lastFocusedChromeWindow() {
  try {
    const wins = await chrome.windows.getAll({ windowTypes: ['normal'] });
    if (!wins.length) return null;
    let best = wins[0];
    for (const w of wins) {
      if (w.focused) {
        best = w;
        break;
      }
      if (w.id === chrome.windows.WINDOW_ID_NONE) continue;
    }
    return best;
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

// ---- 顶栏命令 ----

function handleCommand(data) {
  let msg;
  try {
    msg = JSON.parse(data);
  } catch (e) {
    return;
  }
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
    case 'ping':
      break;
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

// ---- 保活与重启恢复 ----

chrome.alarms.create('topbar-keepalive', { periodInMinutes: 0.25 });

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
