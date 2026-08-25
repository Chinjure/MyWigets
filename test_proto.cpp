// test_proto.cpp - DesktopTopBar Chrome 标签同步协议层单元测试（控制台）
//
// 用 MSVC 编译运行（与顶栏同一工具链）：
//   cl /nologo /std:c++17 /EHsc /W4 /utf-8 test_proto.cpp /Fe:test_proto.exe
// 或：powershell -File build_test_proto.ps1
// 全部断言通过输出 "ALL TESTS PASSED"，否则返回非零退出码。

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "topbar_ws_proto.h"

using namespace wsproto;

static int g_failures = 0;

#define CHECK(cond, name)                                                     \
    do {                                                                      \
        if (cond) {                                                           \
            std::printf("[PASS] %s\n", name);                                 \
        } else {                                                              \
            std::printf("[FAIL] %s\n", name);                                 \
            ++g_failures;                                                     \
        }                                                                     \
    } while (0)

static std::string Hex(const uint8_t* d, size_t n) {
    static const char* t = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; ++i) {
        s.push_back(t[d[i] >> 4]);
        s.push_back(t[d[i] & 0xF]);
    }
    return s;
}

static void TestSha1() {
    uint8_t out[20];
    Sha1(reinterpret_cast<const uint8_t*>("abc"), 3, out);
    CHECK(Hex(out, 20) == "a9993e364706816aba3e25717850c26c9cd0d89d",
          "SHA-1(\"abc\")");

    Sha1(reinterpret_cast<const uint8_t*>(""), 0, out);
    CHECK(Hex(out, 20) == "da39a3ee5e6b4b0d3255bfef95601890afd80709",
          "SHA-1(\"\")");

    const std::string longMsg(1000000, 'a');
    Sha1(reinterpret_cast<const uint8_t*>(longMsg.data()), longMsg.size(), out);
    CHECK(Hex(out, 20) == "34aa973cd4c4daa4f61eeb2bdbad27316534016f",
          "SHA-1(1M 'a')");
}

static void TestBase64() {
    CHECK(Base64Encode(reinterpret_cast<const uint8_t*>(""), 0) == "",
          "Base64(\"\")");
    CHECK(Base64Encode(reinterpret_cast<const uint8_t*>("f"), 1) == "Zg==",
          "Base64(\"f\")");
    CHECK(Base64Encode(reinterpret_cast<const uint8_t*>("fo"), 2) == "Zm8=",
          "Base64(\"fo\")");
    CHECK(Base64Encode(reinterpret_cast<const uint8_t*>("foo"), 3) == "Zm9v",
          "Base64(\"foo\")");
    CHECK(Base64Encode(reinterpret_cast<const uint8_t*>("foob"), 4) == "Zm9vYg==",
          "Base64(\"foob\")");
}

static void TestWsAccept() {
    // RFC 6455 §1.3 示例
    CHECK(ComputeWsAccept("dGhlIHNhbXBsZSBub25jZQ==") ==
              "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=",
          "Sec-WebSocket-Accept (RFC 6455 vector)");
}

static void TestUtf8() {
    CHECK(Utf8ToWide("hello") == L"hello", "UTF-8 ascii -> wide");
    CHECK(WideToUtf8(L"hello") == "hello", "wide -> UTF-8 ascii");

    const std::string zh = "\xe6\xa1\x8c\xe9\x9d\xa2";  // 桌面
    const std::wstring wzh = Utf8ToWide(zh);
    CHECK(wzh == L"\u684c\u9762", "UTF-8 中文 -> wide");
    CHECK(WideToUtf8(wzh) == zh, "wide -> UTF-8 中文");

    // 4 字节码点（emoji）
    const std::string emoji = "\xf0\x9f\x93\x8c";  // U+1F4CC 📌
    const std::wstring wemoji = Utf8ToWide(emoji);
    CHECK(wemoji.size() == 2 && wemoji[0] == 0xD83D && wemoji[1] == 0xDCCC,
          "UTF-8 emoji -> surrogate pair");
    CHECK(WideToUtf8(wemoji) == emoji, "surrogate pair -> UTF-8 emoji");

    // 非法字节
    CHECK(Utf8ToWide("\xff\xfe") == L"\xfffd\xfffd", "invalid UTF-8 -> U+FFFD");
}

static void TestJson() {
    const JsonValue v1 = JsonParse(LR"({"type":"hello","windowId":5,"tabs":[{"id":1,"index":0,"title":"A \"B\"","url":"https://x.com/\u4e2d","active":true,"pinned":false},{"id":2,"index":1,"title":"","url":"about:blank","active":false,"pinned":true}]})");
    CHECK(v1.kind == JsonValue::Kind::Object, "JSON object parse");
    const std::wstring* type = v1.GetStr(L"type");
    CHECK(type && *type == L"hello", "JSON string field");
    CHECK(v1.GetInt(L"windowId") == 5, "JSON int field");
    const std::vector<JsonValue>* tabs = v1.GetArr(L"tabs");
    CHECK(tabs && tabs->size() == 2, "JSON array field");
    if (tabs && tabs->size() == 2) {
        CHECK((*tabs)[0].GetInt(L"id") == 1, "JSON nested int");
        const std::wstring* title = (*tabs)[0].GetStr(L"title");
        CHECK(title && *title == L"A \"B\"", "JSON escaped quote");
        const std::wstring* url = (*tabs)[0].GetStr(L"url");
        CHECK(url && *url == L"https://x.com/\u4e2d", "JSON \\u escape");
        CHECK((*tabs)[0].GetBool(L"active"), "JSON bool true");
        CHECK(!(*tabs)[1].GetBool(L"active") && (*tabs)[1].GetBool(L"pinned"),
              "JSON bool false/pinned");
    }

    CHECK(JsonParse(L"true").kind == JsonValue::Kind::Bool &&
              JsonParse(L"true").boolVal,
          "JSON true");
    CHECK(JsonParse(L"null").kind == JsonValue::Kind::Null, "JSON null");
    CHECK(JsonParse(L"[1,2.5,-3]").kind == JsonValue::Kind::Array,
          "JSON number array");
    CHECK(JsonParse(L"{").kind == JsonValue::Kind::Null, "JSON broken -> Null");
    CHECK(JsonParse(L"{\"a\":1} junk").kind == JsonValue::Kind::Null,
          "JSON trailing junk -> Null");
    CHECK(JsonParse(L"{\"a\":1,}").kind == JsonValue::Kind::Null,
          "JSON trailing comma -> Null");
    CHECK(JsonParse(L"[1,2,]").kind == JsonValue::Kind::Null,
          "JSON array trailing comma -> Null");
}

static void TestFrames() {
    // RFC 6455 §5.7 客户端掩码帧示例：0x81 0x85 0x37 0xfa 0x21 0x3d ... -> "Hello"
    const std::string raw("\x81\x85\x37\xfa\x21\x3d\x7f\x9f\x4d\x51\x58", 11);
    size_t consumed = 0;
    WsFrame frame;
    std::string err;
    const int rc = WsDecodeFrame(raw, consumed, frame, err);
    CHECK(rc == 1 && consumed == 11 && frame.opcode == 0x1 && frame.fin &&
              frame.payload == "Hello",
          "WS client masked frame decode (RFC vector)");

    // 非掩码客户端帧 -> 协议错误
    const std::string unmasked("\x81\x05Hello", 7);
    CHECK(WsDecodeFrame(unmasked, consumed, frame, err) == -1,
          "WS unmasked client frame rejected");

    // 服务端编码 -> 客户端掩码帧解码（不带掩码：双向独立验证）
    const std::string enc = WsEncodeFrame(0x1, "{\"type\":\"ping\"}", true);
    CHECK(enc.size() == 2 + 15 && static_cast<uint8_t>(enc[0]) == 0x81 &&
              static_cast<uint8_t>(enc[1]) == 15,
          "WS server frame encode header");

    // 长负载 16 位长度
    std::string big(300, 'x');
    const std::string encBig = WsEncodeFrame(0x1, big, true);
    CHECK(static_cast<uint8_t>(encBig[1]) == 126 &&
              (static_cast<uint8_t>(encBig[2]) << 8 |
               static_cast<uint8_t>(encBig[3])) == 300,
          "WS 16-bit length frame");

    // 分片文本消息
    const std::string f1 = WsEncodeFrame(0x1, "hel", false);
    const std::string f2 = WsEncodeFrame(0x0, "lo", true);
    CHECK(static_cast<uint8_t>(f1[0]) == 0x01 && static_cast<uint8_t>(f2[0]) == 0x80,
          "WS fragmented frame headers");
}

static void TestModel() {
    ChromeSyncModel m;
    m.connected = true;

    // hello 全量替换
    std::wstring hello = L"{\"type\":\"hello\",\"windowId\":7,\"tabs\":["
                         L"{\"id\":11,\"index\":0,\"title\":\"A\",\"url\":\"u1\",\"active\":true,\"pinned\":false},"
                         L"{\"id\":12,\"index\":1,\"title\":\"B\",\"url\":\"u2\",\"active\":false,\"pinned\":false}]}";
    CHECK(ChromeSyncApplyMessage(m, JsonParse(hello)), "model hello applied");
    CHECK(m.windowId == 7 && m.tabs.size() == 2 && m.tabs[0].id == 11 &&
              m.tabs[0].active,
          "model hello state");

    // tabUpdated：更新标题
    std::wstring upd = L"{\"type\":\"tabUpdated\",\"tab\":{\"id\":12,\"index\":1,\"title\":\"B2\",\"url\":\"u2\",\"active\":false,\"pinned\":true}}";
    CHECK(ChromeSyncApplyMessage(m, JsonParse(upd)), "model tabUpdated");
    CHECK(m.tabs[1].title == L"B2" && m.tabs[1].pinned, "model tabUpdated state");

    // tabCreated：插入并按 index 排序（真实场景 index 互不相同：
    // 新标签插到 0 号位，原 0 号标签后移为 1）
    std::wstring created = L"{\"type\":\"tabCreated\",\"tab\":{\"id\":13,\"index\":0,\"title\":\"N\",\"url\":\"u3\",\"active\":false,\"pinned\":false}}";
    CHECK(ChromeSyncApplyMessage(m, JsonParse(created)), "model tabCreated");
    std::wstring shifted = L"{\"type\":\"tabUpdated\",\"tab\":{\"id\":11,\"index\":1,\"title\":\"A\",\"url\":\"u1\",\"active\":true,\"pinned\":false}}";
    ChromeSyncApplyMessage(m, JsonParse(shifted));
    CHECK(m.tabs.size() == 3 && m.tabs[0].id == 13 && m.tabs[1].id == 11 &&
              m.tabs[2].id == 12,
          "model tabCreated order");

    // tabActivated：激活态转移
    std::wstring act = L"{\"type\":\"tabActivated\",\"tabId\":13}";
    CHECK(ChromeSyncApplyMessage(m, JsonParse(act)), "model tabActivated");
    CHECK(m.tabs[0].active && !m.tabs[1].active && !m.tabs[2].active,
          "model tabActivated state");

    // tabMoved：13 移到末尾（index 2）
    std::wstring moved = L"{\"type\":\"tabMoved\",\"tabId\":13,\"index\":2}";
    CHECK(ChromeSyncApplyMessage(m, JsonParse(moved)), "model tabMoved");
    CHECK(m.tabs[2].id == 13, "model tabMoved order");

    // tabRemoved
    std::wstring removed = L"{\"type\":\"tabRemoved\",\"tabId\":12,\"windowId\":7}";
    CHECK(ChromeSyncApplyMessage(m, JsonParse(removed)), "model tabRemoved");
    CHECK(m.tabs.size() == 2 && m.tabs[1].id == 13, "model tabRemoved state");

    // windowFocused：换窗口全量替换
    std::wstring focused = L"{\"type\":\"windowFocused\",\"windowId\":9,\"tabs\":["
                           L"{\"id\":21,\"index\":0,\"title\":\"W2\",\"url\":\"x\",\"active\":true,\"pinned\":false}]}";
    CHECK(ChromeSyncApplyMessage(m, JsonParse(focused)), "model windowFocused");
    CHECK(m.windowId == 9 && m.tabs.size() == 1 && m.tabs[0].id == 21,
          "model windowFocused state");

    // 命令构造
    CHECK(ChromeSyncBuildCommand(L"activateTab", 42) ==
              "{\"type\":\"activateTab\",\"tabId\":42}",
          "command activateTab");
    CHECK(ChromeSyncBuildCommand(L"closeTab", 7) ==
              "{\"type\":\"closeTab\",\"tabId\":7}",
          "command closeTab");
    CHECK(ChromeSyncBuildCommand(L"newTab") == "{\"type\":\"newTab\"}",
          "command newTab");

    // 中文标题经命令/模型往返
    m.tabs.clear();
    std::wstring zh = L"{\"type\":\"tabCreated\",\"tab\":{\"id\":1,\"index\":0,\"title\":\"\u684c\u9762\u9876\u680f\",\"url\":\"\",\"active\":true,\"pinned\":false}}";
    ChromeSyncApplyMessage(m, JsonParse(zh));
    CHECK(m.tabs[0].title == L"\u684c\u9762\u9876\u680f", "model CJK title");
}

int wmain() {
    TestSha1();
    TestBase64();
    TestWsAccept();
    TestUtf8();
    TestJson();
    TestFrames();
    TestModel();

    if (g_failures == 0) {
        std::printf("\nALL TESTS PASSED\n");
        return 0;
    }
    std::printf("\n%d TEST(S) FAILED\n", g_failures);
    return 1;
}
