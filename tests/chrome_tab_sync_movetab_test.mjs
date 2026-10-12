// chrome_tab_sync_movetab_test.mjs —— 扩展侧 moveTab（顶栏拖动标签重排）单元测试
//
// 为什么单独测扩展：background.js 跑在浏览器的 MV3 Service Worker 里，
// 而"拖动重排"的落点夹取规则（固定标签恒在最前）是纯 JS 逻辑，
// 这里用 vm 把 background.js 装进一个带 chrome / WebSocket 桩的环境里，
// 直接喂一条 moveTab 命令，断言 chrome.tabs.move 收到的 index。
//
// 运行：
//   node tests/chrome_tab_sync_movetab_test.mjs
// 全部通过输出 "ALL TESTS PASSED" 并以 0 退出。

import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
import vm from 'node:vm';
import assert from 'node:assert';

const here = dirname(fileURLToPath(import.meta.url));
const backgroundPath = join(here, '..', 'chrome-tab-sync', 'background.js');
const source = readFileSync(backgroundPath, 'utf8');

let failures = 0;
function check(cond, name) {
  if (cond) {
    console.log(`[PASS] ${name}`);
  } else {
    console.log(`[FAIL] ${name}`);
    failures++;
  }
}

// 把 background.js 装进桩环境，返回 { dispatch, moves, queries }
function loadBackground({ windowTabs, tabById }) {
  const moves = [];   // chrome.tabs.move 收到的参数
  const queries = []; // chrome.tabs.query 收到的参数
  const sent = [];    // 扩展 -> 顶栏 的消息

  class FakeWebSocket {
    static CONNECTING = 0;
    static OPEN = 1;
    static CLOSING = 2;
    static CLOSED = 3;
    constructor(url) {
      this.url = url;
      this.readyState = FakeWebSocket.CONNECTING;
      lastSocket = this;
      // 与真实实现一致：onopen 之后扩展才开始推送
      queueMicrotask(() => {
        this.readyState = FakeWebSocket.OPEN;
        if (this.onopen) this.onopen();
      });
    }
    send(data) {
      sent.push(JSON.parse(data));
    }
    close() {
      this.readyState = FakeWebSocket.CLOSED;
      if (this.onclose) this.onclose();
    }
  }
  let lastSocket = null;

  const listeners = () => ({ addListener() {} });
  const chrome = {
    tabs: {
      get: (id) => Promise.resolve(tabById[id] ?? null),
      query: (q) => {
        queries.push(q);
        return Promise.resolve(windowTabs.slice());
      },
      move: (id, opts) => {
        moves.push({ id, ...opts });
        return Promise.resolve({});
      },
      create: () => Promise.resolve({}),
      remove: () => Promise.resolve(),
      update: () => Promise.resolve({}),
      onCreated: listeners(),
      onUpdated: listeners(),
      onRemoved: listeners(),
      onMoved: listeners(),
      onActivated: listeners(),
      onDetached: listeners(),
      onAttached: listeners(),
    },
    windows: {
      get: () => Promise.resolve({ id: 1, type: 'normal', focused: true }),
      getAll: () => Promise.resolve([{ id: 1, type: 'normal', focused: true }]),
      getLastFocused: () => Promise.resolve({ id: 1, type: 'normal' }),
      update: () => Promise.resolve({}),
      onFocusChanged: listeners(),
      onRemoved: listeners(),
      WINDOW_ID_NONE: -1,
    },
    alarms: {
      create() {},
      onAlarm: listeners(),
    },
    runtime: {
      onStartup: listeners(),
      onInstalled: listeners(),
    },
  };

  const sandbox = {
    chrome,
    WebSocket: FakeWebSocket,
    console,
    JSON,
    setTimeout,
    clearTimeout,
    setInterval,
    clearInterval,
    queueMicrotask,
    Promise,
  };
  vm.createContext(sandbox);
  vm.runInContext(source, sandbox, { filename: 'background.js' });

  const dispatch = async (msg) => {
    assert.ok(lastSocket, 'WebSocket 已创建');
    // 等 onopen 跑完（扩展连上顶栏后先发 hello），否则 send() 会因为
    // readyState 还是 CONNECTING 而丢弃回包
    for (let i = 0; i < 4; i++) {
      await new Promise((r) => setImmediate(r));
    }
    lastSocket.onmessage({ data: JSON.stringify(msg) });
    // 让 tabs.get / tabs.query 的 Promise 链跑完
    for (let i = 0; i < 8; i++) {
      await new Promise((r) => setImmediate(r));
    }
  };

  return { dispatch, moves, queries, sent };
}

const flush = () => new Promise((r) => setImmediate(r));

// ---- 用例 ----

async function testUnpinnedClampedToPinnedZone() {
  // 5 个标签，前 2 个固定；拖动 3 号（普通标签）到最前 -> 只能落 2
  const tabs = [
    { id: 1, index: 0, pinned: true, windowId: 1 },
    { id: 2, index: 1, pinned: true, windowId: 1 },
    { id: 3, index: 2, pinned: false, windowId: 1 },
    { id: 4, index: 3, pinned: false, windowId: 1 },
    { id: 5, index: 4, pinned: false, windowId: 1 },
  ];
  const env = loadBackground({
    windowTabs: tabs,
    tabById: { 5: tabs[4] },
  });
  await env.dispatch({ type: 'moveTab', tabId: 5, index: 0 });
  check(
    env.moves.length === 1 && env.moves[0].id === 5 && env.moves[0].index === 2,
    '普通标签拖到最前 -> 夹到固定区之后（index=2）'
  );
  check(env.queries.length >= 1 &&
    env.queries[env.queries.length - 1].windowId === 1,
  '按被拖标签所在窗口查询标签');
}

async function testPinnedStaysInPinnedZone() {
  const tabs = [
    { id: 1, index: 0, pinned: true, windowId: 1 },
    { id: 2, index: 1, pinned: true, windowId: 1 },
    { id: 3, index: 2, pinned: false, windowId: 1 },
  ];
  const env = loadBackground({ windowTabs: tabs, tabById: { 1: tabs[0] } });
  await env.dispatch({ type: 'moveTab', tabId: 1, index: 2 });
  check(env.moves.length === 1 && env.moves[0].index === 1,
    '固定标签拖到普通区 -> 夹回固定区末尾（index=1）');
}

async function testNormalMoveInsideZone() {
  const tabs = [
    { id: 1, index: 0, pinned: true, windowId: 1 },
    { id: 2, index: 1, pinned: false, windowId: 1 },
    { id: 3, index: 2, pinned: false, windowId: 1 },
    { id: 4, index: 3, pinned: false, windowId: 1 },
  ];
  const env = loadBackground({ windowTabs: tabs, tabById: { 2: tabs[1] } });
  await env.dispatch({ type: 'moveTab', tabId: 2, index: 3 });
  check(env.moves.length === 1 && env.moves[0].id === 2 && env.moves[0].index === 3,
    '普通区内正常移动（index=3）');
}

async function testNoMoveWhenAlreadyThere() {
  const tabs = [
    { id: 1, index: 0, pinned: false, windowId: 1 },
    { id: 2, index: 1, pinned: false, windowId: 1 },
  ];
  const env = loadBackground({ windowTabs: tabs, tabById: { 1: tabs[0] } });
  await env.dispatch({ type: 'moveTab', tabId: 1, index: 0 });
  check(env.moves.length === 0, '位置不变 -> 不下发 chrome.tabs.move');
}

async function testBadPayloadIgnored() {
  const tabs = [{ id: 1, index: 0, pinned: false, windowId: 1 }];
  const env = loadBackground({ windowTabs: tabs, tabById: { 1: tabs[0] } });
  await env.dispatch({ type: 'moveTab', tabId: 1 });        // 缺 index
  await env.dispatch({ type: 'moveTab', index: 0 });        // 缺 tabId
  await env.dispatch({ type: 'moveTab', tabId: 99, index: 1 });  // 标签不存在
  check(env.moves.length === 0, '非法/失效的 moveTab 不下发移动');
}

async function testKeepAliveAndOtherCommandsUnaffected() {
  const tabs = [{ id: 1, index: 0, pinned: false, windowId: 1 }];
  const env = loadBackground({ windowTabs: tabs, tabById: { 1: tabs[0] } });
  await env.dispatch({ type: 'ping' });
  check(env.sent.some((m) => m.type === 'pong'), 'ping 仍回 pong（保活不受影响）');
  await env.dispatch({ type: 'unknownCommand' });
  check(env.moves.length === 0, '未知命令不下发移动');
}

await testUnpinnedClampedToPinnedZone();
await testPinnedStaysInPinnedZone();
await testNormalMoveInsideZone();
await testNoMoveWhenAlreadyThere();
await testBadPayloadIgnored();
await testKeepAliveAndOtherCommandsUnaffected();
await flush();

if (failures === 0) {
  console.log('\nALL TESTS PASSED');
  process.exit(0);
}
console.log(`\n${failures} TEST(S) FAILED`);
process.exit(1);
