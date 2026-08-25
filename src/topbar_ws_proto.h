// topbar_ws_proto.h - DesktopTopBar Chrome 标签同步：纯逻辑层
//
// 本头文件只包含与平台无关的部分（无 Windows API）：
//   - 极简 JSON 解析器（wchar_t 输入）
//   - SHA-1 / Base64（WebSocket 握手 Sec-WebSocket-Accept 用）
//   - UTF-8 <-> UTF-16 互转（消息负载与界面字符串）
//   - WebSocket 帧编解码（客户端掩码帧 -> 服务端非掩码帧）
//   - Chrome 标签同步模型（hello/tabCreated/tabUpdated/tabRemoved/
//     tabMoved/tabActivated/windowFocused 的增量应用）与命令构造
//
// 便于在非 Windows 环境用 g++ 单独做单元验证（见 test_proto.cpp）。

#ifndef TOPBAR_WS_PROTO_H
#define TOPBAR_WS_PROTO_H

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace wsproto {

// ---- UTF-8 <-> UTF-16 ----

inline std::wstring Utf8ToWide(const std::string& s) {
    std::wstring out;
    out.reserve(s.size());
    size_t i = 0;
    const size_t n = s.size();
    while (i < n) {
        const uint8_t c = static_cast<uint8_t>(s[i]);
        uint32_t cp = 0;
        size_t len = 0;
        if (c < 0x80) {
            cp = c;
            len = 1;
        } else if ((c >> 5) == 0x6) {
            cp = c & 0x1F;
            len = 2;
        } else if ((c >> 4) == 0xE) {
            cp = c & 0x0F;
            len = 3;
        } else if ((c >> 3) == 0x1E) {
            cp = c & 0x07;
            len = 4;
        } else {
            out.push_back(0xFFFD);
            ++i;
            continue;
        }
        if (i + len > n) {
            out.push_back(0xFFFD);
            break;
        }
        bool ok = true;
        for (size_t j = 1; j < len; ++j) {
            const uint8_t cc = static_cast<uint8_t>(s[i + j]);
            if ((cc >> 6) != 0x2) {
                ok = false;
                break;
            }
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (!ok) {
            out.push_back(0xFFFD);
            ++i;
            continue;
        }
        i += len;
        if (cp <= 0xFFFF) {
            out.push_back(static_cast<wchar_t>(cp));
        } else if (cp <= 0x10FFFF) {
            cp -= 0x10000;
            out.push_back(static_cast<wchar_t>(0xD800 + (cp >> 10)));
            out.push_back(static_cast<wchar_t>(0xDC00 + (cp & 0x3FF)));
        } else {
            out.push_back(0xFFFD);
        }
    }
    return out;
}

inline std::string WideToUtf8(const std::wstring& s) {
    std::string out;
    out.reserve(s.size() * 3);
    for (size_t i = 0; i < s.size(); ++i) {
        uint32_t cp = static_cast<uint32_t>(s[i]);
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < s.size()) {
            const uint32_t lo = static_cast<uint32_t>(s[i + 1]);
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                ++i;
            } else {
                cp = 0xFFFD;
            }
        } else if (cp >= 0xD800 && cp <= 0xDFFF) {
            cp = 0xFFFD;
        }
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

// ---- SHA-1 ----

inline void Sha1(const uint8_t* data, size_t len, uint8_t out[20]) {
    uint32_t h0 = 0x67452301, h1 = 0xEFCDAB89, h2 = 0x98BADCFE,
             h3 = 0x10325476, h4 = 0xC3D2E1F0;
    const uint64_t bitLen = static_cast<uint64_t>(len) * 8;
    std::vector<uint8_t> msg(data, data + len);
    msg.push_back(0x80);
    while (msg.size() % 64 != 56) {
        msg.push_back(0);
    }
    for (int i = 7; i >= 0; --i) {
        msg.push_back(static_cast<uint8_t>((bitLen >> (i * 8)) & 0xFF));
    }
    for (size_t off = 0; off < msg.size(); off += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<uint32_t>(msg[off + i * 4]) << 24) |
                   (static_cast<uint32_t>(msg[off + i * 4 + 1]) << 16) |
                   (static_cast<uint32_t>(msg[off + i * 4 + 2]) << 8) |
                   static_cast<uint32_t>(msg[off + i * 4 + 3]);
        }
        for (int i = 16; i < 80; ++i) {
            const uint32_t x = w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16];
            w[i] = (x << 1) | (x >> 31);
        }
        uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20) {
                f = (b & c) | ((~b) & d);
                k = 0x5A827999;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDC;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6;
            }
            const uint32_t tmp = ((a << 5) | (a >> 27)) + f + e + k + w[i];
            e = d;
            d = c;
            c = (b << 30) | (b >> 2);
            b = a;
            a = tmp;
        }
        h0 += a;
        h1 += b;
        h2 += c;
        h3 += d;
        h4 += e;
    }
    const uint32_t hs[5] = {h0, h1, h2, h3, h4};
    for (int i = 0; i < 5; ++i) {
        out[i * 4] = static_cast<uint8_t>(hs[i] >> 24);
        out[i * 4 + 1] = static_cast<uint8_t>((hs[i] >> 16) & 0xFF);
        out[i * 4 + 2] = static_cast<uint8_t>((hs[i] >> 8) & 0xFF);
        out[i * 4 + 3] = static_cast<uint8_t>(hs[i] & 0xFF);
    }
}

// ---- Base64（无换行）----

inline std::string Base64Encode(const uint8_t* data, size_t len) {
    static const char kTable[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((len + 2) / 3 * 4);
    for (size_t i = 0; i < len; i += 3) {
        const uint32_t v =
            (static_cast<uint32_t>(data[i]) << 16) |
            (i + 1 < len ? static_cast<uint32_t>(data[i + 1]) << 8 : 0) |
            (i + 2 < len ? static_cast<uint32_t>(data[i + 2]) : 0);
        out.push_back(kTable[(v >> 18) & 0x3F]);
        out.push_back(kTable[(v >> 12) & 0x3F]);
        out.push_back(i + 1 < len ? kTable[(v >> 6) & 0x3F] : '=');
        out.push_back(i + 2 < len ? kTable[v & 0x3F] : '=');
    }
    return out;
}

// 计算 WebSocket 握手的 Sec-WebSocket-Accept 值（RFC 6455 示例向量可验证）
inline std::string ComputeWsAccept(const std::string& secWebSocketKey) {
    static const char kGuid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    const std::string combined = secWebSocketKey + kGuid;
    uint8_t digest[20];
    Sha1(reinterpret_cast<const uint8_t*>(combined.data()), combined.size(),
         digest);
    return Base64Encode(digest, 20);
}

// ---- WebSocket 帧 ----

struct WsFrame {
    bool fin = true;
    int opcode = 0;  // 0x1 text / 0x8 close / 0x9 ping / 0xA pong ...
    std::string payload;
};

// 解析一个（客户端->服务端，必须掩码的）帧。
// 返回：1 = 解析出完整帧（consumed 为字节数）；0 = 数据不足；-1 = 协议错误。
inline int WsDecodeFrame(const std::string& buf, size_t& consumed,
                         WsFrame& out, std::string& err) {
    if (buf.size() < 2) {
        return 0;
    }
    const uint8_t b0 = static_cast<uint8_t>(buf[0]);
    const uint8_t b1 = static_cast<uint8_t>(buf[1]);
    out.fin = (b0 & 0x80) != 0;
    if ((b0 & 0x70) != 0) {
        err = "rsv bits set";
        return -1;
    }
    out.opcode = b0 & 0x0F;
    const bool masked = (b1 & 0x80) != 0;
    if (!masked) {
        err = "client frame not masked";
        return -1;
    }
    uint64_t len = b1 & 0x7F;
    size_t pos = 2;
    if (len == 126) {
        if (buf.size() < 4) {
            return 0;
        }
        len = (static_cast<uint8_t>(buf[2]) << 8) | static_cast<uint8_t>(buf[3]);
        pos = 4;
    } else if (len == 127) {
        if (buf.size() < 10) {
            return 0;
        }
        len = 0;
        for (int i = 0; i < 8; ++i) {
            len = (len << 8) | static_cast<uint8_t>(buf[2 + i]);
        }
        pos = 10;
    }
    if (len > (1u << 20)) {
        err = "frame too large";
        return -1;
    }
    if (buf.size() < pos + 4) {
        return 0;
    }
    uint8_t mask[4];
    for (int i = 0; i < 4; ++i) {
        mask[i] = static_cast<uint8_t>(buf[pos + i]);
    }
    pos += 4;
    if (buf.size() < pos + static_cast<size_t>(len)) {
        return 0;
    }
    out.payload.assign(buf, pos, static_cast<size_t>(len));
    for (size_t i = 0; i < out.payload.size(); ++i) {
        out.payload[i] = static_cast<char>(
            static_cast<uint8_t>(out.payload[i]) ^ mask[i % 4]);
    }
    consumed = pos + static_cast<size_t>(len);
    return 1;
}

// 构造服务端帧（不掩码）
inline std::string WsEncodeFrame(int opcode, const std::string& payload,
                                 bool fin = true) {
    std::string out;
    out.push_back(static_cast<char>((fin ? 0x80 : 0) | (opcode & 0x0F)));
    const size_t n = payload.size();
    if (n < 126) {
        out.push_back(static_cast<char>(n));
    } else if (n <= 0xFFFF) {
        out.push_back(126);
        out.push_back(static_cast<char>((n >> 8) & 0xFF));
        out.push_back(static_cast<char>(n & 0xFF));
    } else {
        out.push_back(127);
        for (int i = 7; i >= 0; --i) {
            out.push_back(static_cast<char>((n >> (i * 8)) & 0xFF));
        }
    }
    out += payload;
    return out;
}

// ---- JSON 极简解析器 ----

struct JsonValue {
    enum class Kind { Null, Bool, Number, String, Array, Object };

    Kind kind = Kind::Null;
    bool boolVal = false;
    double numVal = 0.0;
    std::wstring strVal;
    std::vector<JsonValue> arr;
    std::vector<std::pair<std::wstring, JsonValue>> obj;

    const JsonValue* Get(const wchar_t* key) const {
        if (kind != Kind::Object) {
            return nullptr;
        }
        for (const auto& kv : obj) {
            if (kv.first == key) {
                return &kv.second;
            }
        }
        return nullptr;
    }

    bool GetBool(const wchar_t* key, bool def = false) const {
        const JsonValue* v = Get(key);
        return v && v->kind == Kind::Bool ? v->boolVal : def;
    }

    int GetInt(const wchar_t* key, int def = 0) const {
        const JsonValue* v = Get(key);
        return v && v->kind == Kind::Number ? static_cast<int>(v->numVal) : def;
    }

    const std::wstring* GetStr(const wchar_t* key) const {
        const JsonValue* v = Get(key);
        return v && v->kind == Kind::String ? &v->strVal : nullptr;
    }

    const std::vector<JsonValue>* GetArr(const wchar_t* key) const {
        const JsonValue* v = Get(key);
        return v && v->kind == Kind::Array ? &v->arr : nullptr;
    }
};

namespace detail {

struct JsonParser {
    const wchar_t* p;
    const wchar_t* end;

    void SkipWs() {
        while (p < end && (*p == L' ' || *p == L'\t' || *p == L'\r' ||
                           *p == L'\n')) {
            ++p;
        }
    }

    bool Match(const wchar_t* word) {
        const size_t n = wcslen(word);
        if (static_cast<size_t>(end - p) < n) {
            return false;
        }
        for (size_t i = 0; i < n; ++i) {
            if (p[i] != word[i]) {
                return false;
            }
        }
        p += n;
        return true;
    }

    bool ParseHex4(uint32_t& out) {
        if (end - p < 4) {
            return false;
        }
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            const wchar_t c = p[i];
            uint32_t d;
            if (c >= L'0' && c <= L'9') {
                d = static_cast<uint32_t>(c - L'0');
            } else if (c >= L'a' && c <= L'f') {
                d = static_cast<uint32_t>(c - L'a' + 10);
            } else if (c >= L'A' && c <= L'F') {
                d = static_cast<uint32_t>(c - L'A' + 10);
            } else {
                return false;
            }
            v = (v << 4) | d;
        }
        p += 4;
        out = v;
        return true;
    }

    bool ParseStringBody(std::wstring& out) {
        // 调用时 *p == '"'，返回时 p 指向结束引号之后
        ++p;
        while (p < end) {
            const wchar_t c = *p;
            if (c == L'"') {
                ++p;
                return true;
            }
            if (c == L'\\') {
                ++p;
                if (p >= end) {
                    return false;
                }
                const wchar_t e = *p;
                switch (e) {
                case L'"': out.push_back(L'"'); ++p; break;
                case L'\\': out.push_back(L'\\'); ++p; break;
                case L'/': out.push_back(L'/'); ++p; break;
                case L'b': out.push_back(L'\b'); ++p; break;
                case L'f': out.push_back(L'\f'); ++p; break;
                case L'n': out.push_back(L'\n'); ++p; break;
                case L'r': out.push_back(L'\r'); ++p; break;
                case L't': out.push_back(L'\t'); ++p; break;
                case L'u': {
                    ++p;  // 指向 4 位十六进制
                    uint32_t cp = 0;
                    if (!ParseHex4(cp)) {
                        return false;
                    }
                    // ParseHex4 已把 p 推进到 4 位之后，不再额外 ++p
                    if (cp >= 0xD800 && cp <= 0xDBFF && p < end && *p == L'\\' &&
                        p + 1 < end && p[1] == L'u') {
                        p += 2;
                        uint32_t lo = 0;
                        if (!ParseHex4(lo) || lo < 0xDC00 || lo > 0xDFFF) {
                            return false;
                        }
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    if (cp <= 0xFFFF) {
                        out.push_back(static_cast<wchar_t>(cp));
                    } else if (cp <= 0x10FFFF) {
                        cp -= 0x10000;
                        out.push_back(
                            static_cast<wchar_t>(0xD800 + (cp >> 10)));
                        out.push_back(
                            static_cast<wchar_t>(0xDC00 + (cp & 0x3FF)));
                    } else {
                        return false;
                    }
                    break;
                }
                default:
                    return false;
                }
            } else {
                out.push_back(c);
                ++p;
            }
        }
        return false;
    }

    bool IsValueStart(wchar_t c) const {
        return c == L'{' || c == L'[' || c == L'"' || c == L't' || c == L'f' ||
               c == L'n' || c == L'-' || (c >= L'0' && c <= L'9');
    }

    JsonValue ParseValue() {
        JsonValue v;
        SkipWs();
        if (p >= end) {
            return v;  // Null
        }
        const wchar_t c = *p;
        if (c == L'{') {
            ++p;
            v.kind = JsonValue::Kind::Object;
            SkipWs();
            if (p < end && *p == L'}') {
                ++p;
                return v;
            }
            while (p < end) {
                SkipWs();
                if (p >= end || *p != L'"') {
                    return JsonValue();
                }
                std::wstring key;
                if (!ParseStringBody(key)) {
                    return JsonValue();
                }
                SkipWs();
                if (p >= end || *p != L':') {
                    return JsonValue();
                }
                ++p;
                JsonValue val = ParseValue();
                if (val.kind == JsonValue::Kind::Null && p < end && *p != L',' &&
                    *p != L'}') {
                    // 解析失败（不是合法 null 字面量）
                    return JsonValue();
                }
                v.obj.emplace_back(std::move(key), std::move(val));
                SkipWs();
                if (p >= end) {
                    return JsonValue();
                }
                if (*p == L',') {
                    ++p;
                    // 尾随逗号（{,} / {, ]）不合法
                    SkipWs();
                    if (p >= end || !IsValueStart(*p)) {
                        return JsonValue();
                    }
                    continue;
                }
                if (*p == L'}') {
                    ++p;
                    return v;
                }
                return JsonValue();
            }
            return JsonValue();
        }
        if (c == L'[') {
            ++p;
            v.kind = JsonValue::Kind::Array;
            SkipWs();
            if (p < end && *p == L']') {
                ++p;
                return v;
            }
            while (p < end) {
                JsonValue val = ParseValue();
                if (val.kind == JsonValue::Kind::Null && p < end && *p != L',' &&
                    *p != L']') {
                    return JsonValue();
                }
                v.arr.push_back(std::move(val));
                SkipWs();
                if (p >= end) {
                    return JsonValue();
                }
                if (*p == L',') {
                    ++p;
                    SkipWs();
                    if (p >= end || !IsValueStart(*p)) {
                        return JsonValue();
                    }
                    continue;
                }
                if (*p == L']') {
                    ++p;
                    return v;
                }
                return JsonValue();
            }
            return JsonValue();
        }
        if (c == L'"') {
            v.kind = JsonValue::Kind::String;
            if (!ParseStringBody(v.strVal)) {
                return JsonValue();
            }
            return v;
        }
        if (c == L't' && Match(L"true")) {
            v.kind = JsonValue::Kind::Bool;
            v.boolVal = true;
            return v;
        }
        if (c == L'f' && Match(L"false")) {
            v.kind = JsonValue::Kind::Bool;
            v.boolVal = false;
            return v;
        }
        if (c == L'n' && Match(L"null")) {
            return v;  // Null
        }
        if (c == L'-' || (c >= L'0' && c <= L'9')) {
            const wchar_t* start = p;
            if (*p == L'-') {
                ++p;
            }
            bool any = false;
            while (p < end && ((*p >= L'0' && *p <= L'9') || *p == L'.' ||
                               *p == L'e' || *p == L'E' || *p == L'+' ||
                               *p == L'-')) {
                if (*p >= L'0' && *p <= L'9') {
                    any = true;
                }
                ++p;
            }
            if (!any) {
                return JsonValue();
            }
            std::wstring num(start, p - start);
            v.kind = JsonValue::Kind::Number;
            v.numVal = wcstod(num.c_str(), nullptr);
            return v;
        }
        return JsonValue();
    }
};

}  // namespace detail

// 解析 JSON 文本；失败返回 Kind::Null
inline JsonValue JsonParse(const std::wstring& text) {
    detail::JsonParser parser{text.data(), text.data() + text.size()};
    JsonValue v = parser.ParseValue();
    parser.SkipWs();
    if (parser.p != parser.end) {
        return JsonValue();  // 尾部有垃圾
    }
    return v;
}

// ---- Chrome 标签同步模型 ----

struct ChromeTabModel {
    int id = 0;
    int index = 0;
    std::wstring title;
    std::wstring url;
    bool active = false;
    bool pinned = false;
};

struct ChromeSyncModel {
    bool connected = false;  // UI 线程镜像：扩展已连接
    int windowId = 0;        // 当前同步的 Chrome 窗口
    std::vector<ChromeTabModel> tabs;  // 按 index 升序
};

inline bool ChromeSyncModelValid(const ChromeSyncModel& m) {
    return m.connected && !m.tabs.empty();
}

// 应用一条来自扩展的消息；返回 true 表示标签列表有变化
inline bool ChromeSyncApplyMessage(ChromeSyncModel& m, const JsonValue& msg) {
    const std::wstring* type = msg.GetStr(L"type");
    if (!type) {
        return false;
    }
    const auto upsert = [&m](const JsonValue& t) {
        ChromeTabModel tab;
        tab.id = t.GetInt(L"id", -1);
        if (tab.id < 0) {
            return false;
        }
        tab.index = t.GetInt(L"index", 0);
        if (const std::wstring* s = t.GetStr(L"title")) {
            tab.title = *s;
        }
        if (const std::wstring* s = t.GetStr(L"url")) {
            tab.url = *s;
        }
        tab.active = t.GetBool(L"active");
        tab.pinned = t.GetBool(L"pinned");
        for (auto& e : m.tabs) {
            if (e.id == tab.id) {
                e = tab;
                std::sort(m.tabs.begin(), m.tabs.end(),
                          [](const ChromeTabModel& a, const ChromeTabModel& b) {
                              return a.index < b.index;
                          });
                return true;
            }
        }
        m.tabs.push_back(tab);
        std::sort(m.tabs.begin(), m.tabs.end(),
                  [](const ChromeTabModel& a, const ChromeTabModel& b) {
                      return a.index < b.index;
                  });
        return true;
    };

    if (*type == L"hello" || *type == L"windowFocused") {
        m.windowId = msg.GetInt(L"windowId", 0);
        m.tabs.clear();
        if (const std::vector<JsonValue>* arr = msg.GetArr(L"tabs")) {
            for (const auto& t : *arr) {
                upsert(t);
            }
        }
        return true;
    }
    if (*type == L"tabCreated" || *type == L"tabUpdated") {
        const JsonValue* tab = msg.Get(L"tab");
        return tab ? upsert(*tab) : false;
    }
    if (*type == L"tabRemoved") {
        const int id = msg.GetInt(L"tabId", -1);
        const auto it = std::find_if(
            m.tabs.begin(), m.tabs.end(),
            [id](const ChromeTabModel& t) { return t.id == id; });
        if (it != m.tabs.end()) {
            m.tabs.erase(it);
            return true;
        }
        return false;
    }
    if (*type == L"tabMoved") {
        const int id = msg.GetInt(L"tabId", -1);
        const int index = msg.GetInt(L"index", 0);
        auto it = std::find_if(m.tabs.begin(), m.tabs.end(),
                               [id](const ChromeTabModel& t) {
                                   return t.id == id;
                               });
        if (it != m.tabs.end()) {
            it->index = index;
            std::sort(m.tabs.begin(), m.tabs.end(),
                      [](const ChromeTabModel& a, const ChromeTabModel& b) {
                          return a.index < b.index;
                      });
            return true;
        }
        return false;
    }
    if (*type == L"tabActivated") {
        const int id = msg.GetInt(L"tabId", -1);
        bool changed = false;
        for (auto& t : m.tabs) {
            const bool active = (t.id == id);
            if (t.active != active) {
                t.active = active;
                changed = true;
            }
        }
        return changed;
    }
    return false;
}

// 构造顶栏 -> 扩展 的命令 JSON（UTF-8）
inline std::string ChromeSyncBuildCommand(const wchar_t* type, int tabId = -1) {
    std::wstring s = L"{\"type\":\"";
    s += type;
    s += L"\"";
    if (tabId >= 0) {
        s += L",\"tabId\":";
        s += std::to_wstring(tabId);
    }
    s += L"}";
    return WideToUtf8(s);
}

}  // namespace wsproto

#endif  // TOPBAR_WS_PROTO_H
