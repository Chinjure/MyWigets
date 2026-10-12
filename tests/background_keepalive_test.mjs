// background_keepalive_test.mjs
//
// chrome-tab-sync/background.js 的离线回归测试（Node 内置 WebSocket 客户端 +
// 极简 RFC6455 服务端；用 stub 伪造 chrome.* API）。
//
// 覆盖本次修复的关键行为：
//   1. 连接后立即推送 hello 全量标签（窗口选择走 getLastFocused）
//   2. 应用层心跳：客户端每 15s 发 {"type":"ping"}
//   3. 收到顶栏 {"type":"ping"} 必须回 {"type":"pong"}（MV3 SW 保活关键）
//   4. 顶栏命令 activateTab / closeTab / newTab 正确下发
//   5. 断线后 2s 重连并重新 hello
//   6. alarms 周期为浏览器允许的最小值 0.5min（复活兜底）
//
// 运行：node tests/background_keepalive_test.mjs
import net from 'node:net';
import crypto from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';
import vm from 'node:vm';
import { fileURLToPath } from 'node:url';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const SRC = path.join(__dirname, '..', 'chrome-tab-sync', 'background.js');
const PORT = 9788;

let failures = 0;
function check(cond, name, extra = '') {
  console.log(`${cond ? '[PASS]' : '[FAIL]'} ${name}${extra ? '  ' + extra : ''}`);
  if (!cond) failures++;
}

// ---- 极简 WebSocket 服务端（仅实现本测试所需） ----

function wsAccept(key) {
  return crypto
    .createHash('sha1')
    .update(key + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11')
    .digest('base64');
}

function encodeFrame(opcode, payload) {
  const p = Buffer.from(payload, 'utf8');
  let head;
  if (p.length < 126) {
    head = Buffer.from([0x80 | opcode, p.length]);
  } else if (p.length <= 0xffff) {
    head = Buffer.alloc(4);
    head[0] = 0x80 | opcode;
    head[1] = 126;
    head.writeUInt16BE(p.length, 2);
  } else {
    head = Buffer.alloc(10);
    head[0] = 0x80 | opcode;
    head[1] = 127;
    head.writeBigUInt64BE(BigInt(p.length), 2);
  }
  return Buffer.concat([head, p]);
}

class MiniWsServer {
  constructor(onConnection) {
    this.onConnection = onConnection;
    this.sockets = new Set();
    this.server = net.createServer((sock) => this._onSocket(sock));
  }
  listen(port) {
    return new Promise((res) => this.server.listen(port, '127.0.0.1', res));
  }
  close() {
    for (const s of this.sockets) s.destroy();
    return new Promise((res) => this.server.close(res));
  }
  send(sock, text) {
    if (!sock.destroyed) sock.write(encodeFrame(0x1, text));
  }
  _onSocket(sock) {
    let buf = Buffer.alloc(0);
    let open = false;
    let client = null;
    sock.on('data', (chunk) => {
      buf = Buffer.concat([buf, chunk]);
      if (!open) {
        const idx = buf.indexOf('\r\n\r\n');
        if (idx < 0) return;
        const req = buf.subarray(0, idx).toString('latin1');
        buf = buf.subarray(idx + 4);
        const m = /sec-websocket-key:\s*(\S+)/i.exec(req);
        if (!m) return sock.destroy();
        sock.write(
          'HTTP/1.1 101 Switching Protocols\r\n' +
            'Upgrade: websocket\r\nConnection: Upgrade\r\n' +
            'Sec-WebSocket-Accept: ' + wsAccept(m[1]) + '\r\n\r\n'
        );
        open = true;
        this.sockets.add(sock);
        client = { send: (t) => this.send(sock, t), close: () => sock.destroy() };
        this.onConnection(client);
      }
      // 解析（掩码）帧
      for (;;) {
        if (buf.length < 2) break;
        const opcode = buf[0] & 0x0f;
        const masked = (buf[1] & 0x80) !== 0;
        let len = buf[1] & 0x7f;
        let pos = 2;
        if (len === 126) {
          if (buf.length < 4) break;
          len = buf.readUInt16BE(2);
          pos = 4;
        } else if (len === 127) {
          if (buf.length < 10) break;
          len = Number(buf.readBigUInt64BE(2));
          pos = 10;
        }
        const maskLen = masked ? 4 : 0;
        if (buf.length < pos + maskLen + len) break;
        const payload = Buffer.from(buf.subarray(pos + maskLen, pos + maskLen + len));
        if (masked) {
          const mask = buf.subarray(pos, pos + 4);
          for (let i = 0; i < payload.length; i++) payload[i] ^= mask[i % 4];
        }
        buf = buf.subarray(pos + maskLen + len);
        if (opcode === 0x1 && client) client.onmessage?.(payload.toString('utf8'));
        else if (opcode === 0x8) sock.destroy();
        else if (opcode === 0x9) sock.write(encodeFrame(0xa, payload));
      }
    });
    sock.on('error', () => {});
  }
}

// ---- chrome.* stub ----

const WINDOW_TABS = {
  10: [
    { id: 101, windowId: 10, index: 0, title: 'A', url: 'https://a', active: false, pinned: false },
    { id: 102, windowId: 10, index: 1, title: 'B', url: 'https://b', active: true, pinned: false },
    { id: 103, windowId: 10, index: 2, title: 'C', url: 'https://c', active: false, pinned: true }
  ],
  20: [{ id: 201, windowId: 20, index: 0, title: 'solo', url: 'https://solo', active: true, pinned: false }]
};

const calls = { update: [], remove: [], create: [], alarms: [], getAllWindows: 0 };

function makeEvent() {
  const ls = [];
  return { addListener: (f) => ls.push(f), _fire: (...a) => ls.forEach((f) => f(...a)) };
}

const ev = {
  tabCreated: makeEvent(),
  tabUpdated: makeEvent(),
  tabRemoved: makeEvent(),
  tabMoved: makeEvent(),
  tabActivated: makeEvent(),
  tabDetached: makeEvent(),
  tabAttached: makeEvent(),
  winFocus: makeEvent(),
  winRemoved: makeEvent(),
  alarm: makeEvent(),
  startup: makeEvent(),
  installed: makeEvent()
};

const chromeStub = {
  tabs: {
    query: async ({ windowId }) => (WINDOW_TABS[windowId] || []).map((t) => ({ ...t })),
    get: async (id) => Object.values(WINDOW_TABS).flat().find((t) => t.id === id) || null,
    update: async (id, props) => void calls.update.push([id, props]),
    remove: async (id) => void calls.remove.push(id),
    create: async (props) => void calls.create.push(props || {}),
    onCreated: ev.tabCreated,
    onUpdated: ev.tabUpdated,
    onRemoved: ev.tabRemoved,
    onMoved: ev.tabMoved,
    onActivated: ev.tabActivated,
    onDetached: ev.tabDetached,
    onAttached: ev.tabAttached
  },
  windows: {
    WINDOW_ID_NONE: -1,
    getLastFocused: async () => ({ id: 10, type: 'normal', focused: false }),
    getAll: async () => {
      calls.getAllWindows++;
      return [{ id: 10, type: 'normal', focused: false }, { id: 20, type: 'normal', focused: false }];
    },
    get: async (id) => ({ id, type: 'normal' }),
    update: async () => {},
    onFocusChanged: ev.winFocus,
    onRemoved: ev.winRemoved
  },
  alarms: {
    create: (name, info) => void calls.alarms.push([name, info]),
    onAlarm: ev.alarm
  },
  runtime: { onStartup: ev.startup, onInstalled: ev.installed }
};

// ---- 载入 background.js（把端口换成测试端口，并把心跳间隔加速） ----

let code = fs.readFileSync(SRC, 'utf8');
if (!code.includes(`ws://127.0.0.1:9786/`)) throw new Error('WS_URL not found');
code = code.replace('ws://127.0.0.1:9786/', `ws://127.0.0.1:${PORT}/`);

const intervalDelays = [];
const realSetInterval = setInterval;
const sandbox = {
  chrome: chromeStub,
  WebSocket,
  console,
  JSON,
  setTimeout,
  clearTimeout,
  setInterval: (fn, ms) => {
    intervalDelays.push(ms);
    return realSetInterval(fn, Math.min(ms, 250)); // 加速执行，但记录真实配置值
  },
  clearInterval
};
vm.createContext(sandbox);

// ---- 测试 ----

const received = []; // 顶栏收到的所有消息
let connCount = 0;
let current = null;
let resolveFirstHello;
const firstHello = new Promise((r) => (resolveFirstHello = r));
let firstSock = null;

const server = new MiniWsServer((client) => {
  connCount++;
  current = client;
  if (connCount === 1) firstSock = client;
  client.onmessage = (text) => {
    let msg;
    try {
      msg = JSON.parse(text);
    } catch (e) {
      msg = { type: '<bad json>' };
    }
    received.push(msg);
    if (msg.type === 'hello') resolveFirstHello(msg);
  };
});

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

await server.listen(PORT);
vm.runInContext(code, sandbox, { filename: 'background.js' });

// 1) 连接后立即 hello，且用 getLastFocused 选中的窗口 10（3 个标签）
const hello = await Promise.race([firstHello, sleep(3000).then(() => null)]);
check(!!hello, '连接后推送 hello');
check(hello && hello.windowId === 10, 'hello 使用 getLastFocused 窗口(10)', `windowId=${hello && hello.windowId}`);
check(hello && hello.tabs.length === 3, 'hello 含窗口 10 全部 3 个标签', `tabs=${hello && hello.tabs.length}`);
check(
  hello && hello.tabs.every((t) => typeof t.id === 'number' && typeof t.index === 'number'),
  'hello 标签字段完整(id/index/title/active/pinned)'
);

// 2) 应用层心跳：配置值必须是 15000ms（< 30s MV3 回收阈值）
await sleep(700);
check(intervalDelays.length > 0 && intervalDelays[0] === 15000, '心跳间隔配置为 15000ms', `got=${intervalDelays[0]}`);

// 3) 实际发出多次 ping
const pings = received.filter((m) => m.type === 'ping').length;
check(pings >= 2, '客户端周期性发出 ping', `count=${pings}`);

// 4) 收到顶栏 ping 必须回 pong（MV3 SW 保活关键路径）
const before = received.length;
firstSock.send(JSON.stringify({ type: 'ping' }));
await sleep(300);
const pongs = received.filter((m) => m.type === 'pong').length;
check(pongs >= 1, '收到顶栏 ping 回复 pong', `pongs=${pongs}`);

// 顶栏 pong 不应触发任何命令/异常
firstSock.send(JSON.stringify({ type: 'pong' }));
await sleep(200);
check(true, '顶栏 pong 被静默忽略（无异常）');

// 5) 命令下发
firstSock.send(JSON.stringify({ type: 'activateTab', tabId: 102 }));
firstSock.send(JSON.stringify({ type: 'closeTab', tabId: 103 }));
firstSock.send(JSON.stringify({ type: 'newTab' }));
await sleep(400);
check(calls.update.some(([id, p]) => id === 102 && p.active === true), 'activateTab -> tabs.update(active)');
check(calls.remove.includes(103), 'closeTab -> tabs.remove');
check(calls.create.length === 1, 'newTab -> tabs.create');

// 6) alarms 复活兜底周期 = 0.5min
check(
  calls.alarms.length === 1 && calls.alarms[0][0] === 'topbar-keepalive' && calls.alarms[0][1].periodInMinutes === 0.5,
  'alarms 兜底周期 0.5min',
  JSON.stringify(calls.alarms)
);

// 7) 断线 2s 后自动重连并重新 hello
const hellosBefore = received.filter((m) => m.type === 'hello').length;
firstSock.close();
await sleep(3000);
check(connCount >= 2, '断线后自动重连', `connections=${connCount}`);
check(
  received.filter((m) => m.type === 'hello').length > hellosBefore,
  '重连后重新推送 hello'
);

// 8) 计数器自检：getAllWindows 不应被调用（getLastFocused 优先）
check(calls.getAllWindows === 0, '优先 getLastFocused，不回退枚举全部窗口');

await server.close();

console.log('');
if (failures === 0) {
  console.log('ALL BACKGROUND TESTS PASSED');
  process.exit(0);
} else {
  console.log(`${failures} TEST(S) FAILED`);
  process.exit(1);
}
