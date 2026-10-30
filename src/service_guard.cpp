// service_guard.cpp - 应用服务守护（一次性安装 + 按需提权 broker）
//
// 设计：
//   --install   复制自身到 Program Files\DesktopSuite，创建“按需运行、
//               最高权限”的计划任务；只在用户显式 UAC 时执行。
//   --uninstall 恢复所有被本守护禁用的服务，删除任务/文件。
//   --broker    由计划任务以最高权限运行：读取 Dock 写入的 request.txt，
//               停止并临时禁用匹配的应用私有服务；应用启动时恢复原
//               启动类型并视情况 StartService。
//
// 不是常驻服务，不轮询；每次只处理一条请求。

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <sddl.h>
#include <shellapi.h>
#include <winsvc.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "app_cleanup.h"

namespace {

constexpr wchar_t kTaskName[] = L"DesktopSuite\\ServiceGuardBroker";
constexpr wchar_t kTaskLeaf[] = L"ServiceGuardBroker";
constexpr wchar_t kMarkerName[] = L"installed.flag";

std::wstring Env(const wchar_t* name) {
    wchar_t buf[32768] = {};
    const DWORD n = GetEnvironmentVariableW(name, buf, 32760);
    return (n > 0 && n < 32760) ? std::wstring(buf) : std::wstring();
}

std::wstring Join(const std::wstring& a, const std::wstring& b) {
    if (a.empty()) return b;
    if (a.back() == L'\\') return a + b;
    return a + L"\\" + b;
}

std::wstring TrimSlash(std::wstring s) {
    while (s.size() > 3 && s.back() == L'\\') s.pop_back();
    return s;
}

std::wstring InstallDir() {
    return TrimSlash(Join(Env(L"ProgramFiles"), L"DesktopSuite"));
}
std::wstring InstalledExe() {
    return Join(InstallDir(), L"MyWigetsServiceGuard.exe");
}
std::wstring DataDir() {
    return TrimSlash(Join(Env(L"ProgramData"), L"DesktopSuite\\ServiceGuard"));
}
std::wstring LocalDir() {
    std::wstring base = Env(L"LOCALAPPDATA");
    if (base.empty()) {
        base = Join(Env(L"USERPROFILE"), L"AppData\\Local");
    }
    return TrimSlash(Join(base, L"DesktopSuite\\ServiceGuard"));
}
std::wstring RequestPath() { return Join(LocalDir(), L"request.txt"); }
std::wstring ResultPath() { return Join(LocalDir(), L"result.txt"); }
std::wstring StatePath() { return Join(DataDir(), L"state.txt"); }
std::wstring MarkerPath() { return Join(LocalDir(), kMarkerName); }
std::wstring LogPath() { return Join(LocalDir(), L"guard.log"); }

void EnsureDir(const std::wstring& dir) {
    if (dir.empty()) return;
    std::wstring cur;
    for (size_t i = 0; i < dir.size(); ++i) {
        cur.push_back(dir[i]);
        if (dir[i] == L'\\' && cur.size() > 3) {
            CreateDirectoryW(cur.c_str(), nullptr);
        }
    }
    CreateDirectoryW(dir.c_str(), nullptr);
}

void Logf(const wchar_t* fmt, ...) {
    EnsureDir(LocalDir());
    FILE* f = _wfsopen(LogPath().c_str(), L"a, ccs=UTF-8", _SH_DENYNO);
    if (!f) return;
    SYSTEMTIME st{};
    GetLocalTime(&st);
    fwprintf(f, L"[%02u:%02u:%02u.%03u] ", st.wHour, st.wMinute, st.wSecond,
             st.wMilliseconds);
    va_list args;
    va_start(args, fmt);
    vfwprintf(f, fmt, args);
    va_end(args);
    fputwc(L'\n', f);
    fclose(f);
}

std::string ToUtf8(const std::wstring& s) {
    if (s.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(),
                                      static_cast<int>(s.size()), nullptr, 0,
                                      nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                        out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring FromUtf8(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(),
                                      static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                        out.data(), n);
    if (!out.empty() && out[0] == 0xFEFF) out.erase(out.begin());
    return out;
}

bool ReadFileUtf8(const std::wstring& path, std::wstring* out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size) || size.QuadPart <= 0 ||
        size.QuadPart > 1024 * 1024) {
        CloseHandle(h);
        return false;
    }
    std::string buf(static_cast<size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    const bool ok = ReadFile(h, buf.data(), static_cast<DWORD>(buf.size()),
                             &read, nullptr) != FALSE;
    CloseHandle(h);
    if (!ok) return false;
    buf.resize(read);
    *out = FromUtf8(buf);
    return true;
}

bool WriteFileUtf8(const std::wstring& path, const std::wstring& text) {
    EnsureDir(LocalDir());
    const std::string utf8 = ToUtf8(text);
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(h, utf8.data(),
                              static_cast<DWORD>(utf8.size()), &written,
                              nullptr) != FALSE;
    CloseHandle(h);
    return ok;
}

std::vector<std::wstring> Split(const std::wstring& s, wchar_t sep) {
    std::vector<std::wstring> out;
    size_t start = 0;
    for (size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == sep) {
            out.push_back(s.substr(start, i - start));
            start = i + 1;
        }
    }
    return out;
}

struct StateEntry {
    std::wstring app;
    std::wstring service;
    DWORD startType = SERVICE_DEMAND_START;
    bool wasRunning = false;
};

std::vector<StateEntry> ReadState() {
    std::vector<StateEntry> out;
    std::wstring text;
    if (!ReadFileUtf8(StatePath(), &text)) return out;
    for (const std::wstring& line : Split(text, L'\n')) {
        if (line.empty()) continue;
        const std::vector<std::wstring> f = Split(line, L'|');
        if (f.size() < 4) continue;
        StateEntry e;
        e.app = f[0];
        e.service = f[1];
        e.startType = static_cast<DWORD>(_wtoi(f[2].c_str()));
        e.wasRunning = f[3] == L"1";
        out.push_back(std::move(e));
    }
    return out;
}

void WriteState(const std::vector<StateEntry>& entries) {
    std::wstring text;
    for (const StateEntry& e : entries) {
        text += e.app + L"|" + e.service + L"|" +
                std::to_wstring(e.startType) + L"|" +
                (e.wasRunning ? L"1" : L"0") + L"\r\n";
    }
    WriteFileUtf8(StatePath(), text);
}

VOID CALLBACK NotifyCallback(PVOID ctx) {
    SetEvent(static_cast<HANDLE>(ctx));
}

bool WaitStopped(SC_HANDLE svc) {
    HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ev) return false;
    SERVICE_NOTIFY notify{};
    notify.dwVersion = SERVICE_NOTIFY_STATUS_CHANGE;
    notify.pfnNotifyCallback = NotifyCallback;
    notify.pContext = ev;
    DWORD err = NotifyServiceStatusChangeW(svc, SERVICE_NOTIFY_STOPPED,
                                           &notify);
    bool stopped = false;
    if (err == ERROR_SUCCESS) {
        stopped = WaitForSingleObject(ev, 5000) == WAIT_OBJECT_0;
    } else {
        WaitForSingleObject(ev, 300);
    }
    if (!stopped) {
        SERVICE_STATUS_PROCESS st{};
        DWORD needed = 0;
        if (QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO,
                                 reinterpret_cast<LPBYTE>(&st), sizeof(st),
                                 &needed) &&
            st.dwCurrentState == SERVICE_STOPPED) {
            stopped = true;
        }
    }
    CloseHandle(ev);
    return stopped;
}

struct TaskResult {
    int disabled = 0;
    int stopped = 0;
    int restored = 0;
    int failed = 0;
    std::wstring detail;
};

bool IsSystemServicePath(const std::wstring& path) {
    wchar_t win[MAX_PATH] = {};
    GetWindowsDirectoryW(win, MAX_PATH);
    const std::wstring w = appsuspend::NormalizePath(win);
    return !w.empty() &&
           (path == w || path.rfind(w + L"\\", 0) == 0);
}

bool ValidateApp(const std::wstring& app) {
    if (app.empty() || app.size() > 32760) return false;
    const std::wstring norm = appsuspend::NormalizePath(app);
    if (norm.empty() || norm.size() < 4 ||
        _wcsicmp(norm.c_str() + norm.size() - 4, L".exe") != 0) {
        return false;
    }
    if (IsSystemServicePath(norm)) return false;
    const DWORD attr = GetFileAttributesW(app.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY)) {
        return false;
    }
    bool broad = false;
    const std::wstring root =
        appsuspend::InstallRootForApp(norm, &broad);
    return !root.empty() && !broad;
}

std::wstring ReadLineValue(const std::wstring& text, const wchar_t* key) {
    const std::wstring prefix = std::wstring(key) + L"=";
    for (const std::wstring& line : Split(text, L'\n')) {
        std::wstring l = line;
        while (!l.empty() && (l.back() == L'\r' || l.back() == L' ')) {
            l.pop_back();
        }
        if (l.rfind(prefix, 0) == 0) return l.substr(prefix.size());
    }
    return L"";
}

TaskResult DisableApp(const std::wstring& app, const std::wstring& pin) {
    TaskResult r;
    std::vector<StateEntry> state = ReadState();
    const std::wstring norm = appsuspend::NormalizePath(app);
    const std::vector<appcleanup::ServiceInfo> services =
        appcleanup::ServicesForApp(app, pin);
    r.detail = L"匹配服务 " + std::to_wstring(services.size());
    for (const auto& svc : services) {
        SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
        if (!scm) {
            ++r.failed;
            r.detail += L"；SCM 打开失败";
            break;
        }
        SC_HANDLE h = OpenServiceW(
            scm, svc.name.c_str(),
            SERVICE_CHANGE_CONFIG | SERVICE_STOP | SERVICE_QUERY_STATUS |
                SERVICE_START | SERVICE_QUERY_CONFIG);
        if (!h) {
            ++r.failed;
            r.detail += L"；" + svc.name + L" 打开失败(" +
                        std::to_wstring(GetLastError()) + L")";
            CloseServiceHandle(scm);
            continue;
        }
        DWORD originalStart = svc.startType;
        bool already = false;
        for (const StateEntry& e : state) {
            if (e.app == norm && e.service == svc.name) {
                already = true;
                break;
            }
        }
        if (!already) {
            SERVICE_STATUS_PROCESS st{};
            DWORD needed = 0;
            const bool running =
                QueryServiceStatusEx(h, SC_STATUS_PROCESS_INFO,
                                     reinterpret_cast<LPBYTE>(&st),
                                     sizeof(st), &needed) &&
                (st.dwCurrentState == SERVICE_RUNNING ||
                 st.dwCurrentState == SERVICE_START_PENDING);
            StateEntry e;
            e.app = norm;
            e.service = svc.name;
            e.startType = originalStart;
            e.wasRunning = running;
            state.push_back(std::move(e));
            WriteState(state);
        }
        if (svc.state == SERVICE_RUNNING ||
            svc.state == SERVICE_START_PENDING) {
            SERVICE_STATUS status{};
            if (ControlService(h, SERVICE_CONTROL_STOP, &status)) {
                ++r.stopped;
                if (!WaitStopped(h)) {
                    r.detail += L"；" + svc.name + L" 停止等待超时";
                }
            } else if (GetLastError() != ERROR_SERVICE_NOT_ACTIVE) {
                r.detail += L"；" + svc.name + L" 停止失败(" +
                            std::to_wstring(GetLastError()) + L")";
            }
        }
        if (ChangeServiceConfigW(h, SERVICE_NO_CHANGE, SERVICE_DISABLED,
                                 SERVICE_NO_CHANGE, nullptr, nullptr, nullptr,
                                 nullptr, nullptr, nullptr, nullptr)) {
            ++r.disabled;
        } else {
            ++r.failed;
            r.detail += L"；" + svc.name + L" 禁用失败(" +
                        std::to_wstring(GetLastError()) + L")";
        }
        CloseServiceHandle(h);
        CloseServiceHandle(scm);
    }
    return r;
}

TaskResult RestoreApp(const std::wstring& app) {
    TaskResult r;
    std::vector<StateEntry> state = ReadState();
    std::vector<StateEntry> keep;
    const std::wstring norm = appsuspend::NormalizePath(app);
    for (const StateEntry& e : state) {
        if (e.app != norm) {
            keep.push_back(e);
            continue;
        }
        SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
        if (!scm) {
            ++r.failed;
            keep.push_back(e);
            continue;
        }
        SC_HANDLE h = OpenServiceW(
            scm, e.service.c_str(),
            SERVICE_CHANGE_CONFIG | SERVICE_START | SERVICE_QUERY_STATUS);
        bool ok = false;
        if (h) {
            if (ChangeServiceConfigW(
                    h, SERVICE_NO_CHANGE, e.startType, SERVICE_NO_CHANGE,
                    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                    nullptr)) {
                ++r.restored;
                ok = true;
                if (e.wasRunning) {
                    if (!StartServiceW(h, 0, nullptr) &&
                        GetLastError() != ERROR_SERVICE_ALREADY_RUNNING) {
                        r.detail += L"；" + e.service + L" 启动失败(" +
                                    std::to_wstring(GetLastError()) + L")";
                    }
                }
            } else {
                ++r.failed;
                r.detail += L"；" + e.service + L" 恢复启动类型失败(" +
                            std::to_wstring(GetLastError()) + L")";
            }
            CloseServiceHandle(h);
        } else {
            ++r.failed;
            r.detail += L"；" + e.service + L" 打开失败";
        }
        CloseServiceHandle(scm);
        if (!ok) keep.push_back(e);
    }
    WriteState(keep);
    return r;
}

TaskResult RestoreAll() {
    TaskResult r;
    std::vector<StateEntry> state = ReadState();
    if (state.empty()) return r;
    std::vector<std::wstring> apps;
    for (const auto& e : state) {
        bool seen = false;
        for (const auto& a : apps) {
            if (a == e.app) { seen = true; break; }
        }
        if (!seen) apps.push_back(e.app);
    }
    for (const auto& a : apps) {
        TaskResult one = RestoreApp(a);
        r.restored += one.restored;
        r.failed += one.failed;
        r.detail += one.detail;
    }
    return r;
}

// 修复旧版本把 originalStartType 记成 Manual 的遗留状态：
// 先把匹配服务设回 Auto（或用户显式指定的原类型），清掉旧 state，再执行
// 一次 disable，让 state 记录正确的 Auto。
TaskResult RepairAuto(const std::wstring& app, const std::wstring& pin) {
    TaskResult r;
    const std::wstring norm = appsuspend::NormalizePath(app);
    const std::vector<appcleanup::ServiceInfo> services =
        appcleanup::ServicesForApp(app, pin);
    for (const auto& svc : services) {
        SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
        if (!scm) { ++r.failed; continue; }
        SC_HANDLE h = OpenServiceW(
            scm, svc.name.c_str(),
            SERVICE_CHANGE_CONFIG | SERVICE_QUERY_STATUS);
        if (!h) {
            ++r.failed;
            r.detail += L"；" + svc.name + L" 打开失败";
            CloseServiceHandle(scm);
            continue;
        }
        if (!ChangeServiceConfigW(h, SERVICE_NO_CHANGE, SERVICE_AUTO_START,
                                  SERVICE_NO_CHANGE, nullptr, nullptr, nullptr,
                                  nullptr, nullptr, nullptr, nullptr)) {
            ++r.failed;
            r.detail += L"；" + svc.name + L" 设为 Auto 失败(" +
                        std::to_wstring(GetLastError()) + L")";
        }
        CloseServiceHandle(h);
        CloseServiceHandle(scm);
    }
    // 丢掉旧 state 中该应用的行，再让 DisableApp 重新记录当前启动类型。
    std::vector<StateEntry> state = ReadState();
    std::vector<StateEntry> keep;
    for (const StateEntry& e : state) {
        if (e.app != norm) keep.push_back(e);
    }
    WriteState(keep);
    TaskResult disabled = DisableApp(norm, pin);
    r.disabled = disabled.disabled;
    r.stopped = disabled.stopped;
    r.failed += disabled.failed;
    r.detail += disabled.detail;
    return r;
}

bool HasArg(const std::wstring& cmd, const wchar_t* name) {
    return cmd.find(name) != std::wstring::npos;
}

std::wstring ArgValue(const wchar_t* name) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return L"";
    std::wstring value;
    for (int i = 1; i + 1 < argc; ++i) {
        if (_wcsicmp(argv[i], name) == 0) {
            value = argv[i + 1];
            break;
        }
    }
    LocalFree(argv);
    return value;
}

bool RunSchTasks(const std::wstring& args, DWORD* exitCode) {
    wchar_t sys[MAX_PATH] = {};
    GetSystemDirectoryW(sys, MAX_PATH);
    const std::wstring app = Join(sys, L"schtasks.exe");
    std::wstring cmd = L"\"" + app + L"\" " + args;
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(app.c_str(), cmd.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        return false;
    }
    WaitForSingleObject(pi.hProcess, 30000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (exitCode) *exitCode = code;
    return true;
}

std::wstring EscapeXml(std::wstring s) {
    std::wstring out;
    out.reserve(s.size());
    for (wchar_t ch : s) {
        switch (ch) {
            case L'&': out += L"&amp;"; break;
            case L'<': out += L"&lt;"; break;
            case L'>': out += L"&gt;"; break;
            case L'"': out += L"&quot;"; break;
            case L'\'': out += L"&apos;"; break;
            default: out.push_back(ch); break;
        }
    }
    return out;
}

std::wstring CurrentUserSid() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return L"";
    DWORD needed = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
    std::vector<BYTE> buffer(needed);
    std::wstring sid;
    if (needed > 0 &&
        GetTokenInformation(token, TokenUser, buffer.data(), needed, &needed)) {
        auto* user = reinterpret_cast<TOKEN_USER*>(buffer.data());
        wchar_t* str = nullptr;
        if (ConvertSidToStringSidW(user->User.Sid, &str)) {
            sid = str;
            LocalFree(str);
        }
    }
    CloseHandle(token);
    return sid;
}

bool Install() {
    EnsureDir(InstallDir());
    EnsureDir(DataDir());
    EnsureDir(LocalDir());
    wchar_t self[32768] = {};
    GetModuleFileNameW(nullptr, self, 32760);
    const std::wstring installed = InstalledExe();
    if (_wcsicmp(self, installed.c_str()) != 0) {
        if (!CopyFileW(self, installed.c_str(), FALSE)) {
            Logf(L"install copy failed %lu", GetLastError());
            return false;
        }
    }
    WriteFileUtf8(MarkerPath(), L"1\r\n");

    const std::wstring sid = CurrentUserSid();
    if (sid.empty()) {
        Logf(L"install: cannot query current SID");
        DeleteFileW(MarkerPath().c_str());
        return false;
    }
    // 用 XML 创建任务，显式关掉「电池上不启动 / 切电池就停止」，否则笔记本
    // 上 schtasks 默认设置会让任务一直 Queued，broker 根本不执行。
    const std::wstring xml =
        L"<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\r\n"
        L"  <RegistrationInfo><Author>DesktopSuite</Author></RegistrationInfo>\r\n"
        L"  <Principals><Principal id=\"Author\">\r\n"
        L"    <UserId>" + EscapeXml(sid) + L"</UserId>\r\n"
        L"    <LogonType>InteractiveToken</LogonType>\r\n"
        L"    <RunLevel>HighestAvailable</RunLevel>\r\n"
        L"  </Principal></Principals>\r\n"
        L"  <Settings>\r\n"
        L"    <MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>\r\n"
        L"    <DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>\r\n"
        L"    <StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>\r\n"
        L"    <AllowHardTerminate>true</AllowHardTerminate>\r\n"
        L"    <StartWhenAvailable>true</StartWhenAvailable>\r\n"
        L"    <RunOnlyIfNetworkAvailable>false</RunOnlyIfNetworkAvailable>\r\n"
        L"    <IdleSettings><StopOnIdleEnd>false</StopOnIdleEnd>"
        L"<RestartOnIdle>false</RestartOnIdle></IdleSettings>\r\n"
        L"    <AllowStartOnDemand>true</AllowStartOnDemand>\r\n"
        L"    <Enabled>true</Enabled>\r\n"
        L"    <Hidden>false</Hidden>\r\n"
        L"    <RunOnlyIfIdle>false</RunOnlyIfIdle>\r\n"
        L"    <WakeToRun>false</WakeToRun>\r\n"
        L"    <ExecutionTimeLimit>PT0S</ExecutionTimeLimit>\r\n"
        L"    <Priority>7</Priority>\r\n"
        L"  </Settings>\r\n"
        L"  <Actions Context=\"Author\"><Exec>\r\n"
        L"    <Command>" + EscapeXml(installed) + L"</Command>\r\n"
        L"    <Arguments>--broker</Arguments>\r\n"
        L"  </Exec></Actions>\r\n"
        L"</Task>\r\n";
    const std::wstring xmlPath = Join(LocalDir(), L"task.xml");
    if (!WriteFileUtf8(xmlPath, xml)) {
        Logf(L"install: write task xml failed");
        DeleteFileW(MarkerPath().c_str());
        return false;
    }
    const std::wstring args =
        L"/create /tn \"" + std::wstring(kTaskName) + L"\" /xml \"" +
        xmlPath + L"\" /f";
    DWORD code = 1;
    if (!RunSchTasks(args, &code) || code != 0) {
        Logf(L"schtasks xml create failed code=%lu args=%ls", code,
             args.c_str());
        DeleteFileW(xmlPath.c_str());
        DeleteFileW(MarkerPath().c_str());
        return false;
    }
    DeleteFileW(xmlPath.c_str());
    Logf(L"installed task=%ls exe=%ls (battery allowed)", kTaskName,
         installed.c_str());
    return true;
}

bool Uninstall() {
    RestoreAll();
    DWORD code = 0;
    RunSchTasks(L"/delete /tn \"" + std::wstring(kTaskName) + L"\" /f",
                &code);
    DeleteFileW(MarkerPath().c_str());
    DeleteFileW(InstalledExe().c_str());
    Logf(L"uninstalled code=%lu", code);
    return true;
}

void NotifyDock(bool ok) {
    const UINT msg = RegisterWindowMessageW(L"DesktopSuite.ServiceGuardDone");
    if (!msg) return;
    struct Ctx { HWND hwnd; };
    Ctx ctx{nullptr};
    EnumWindows(
        [](HWND h, LPARAM lp) -> BOOL {
            auto* c = reinterpret_cast<Ctx*>(lp);
            wchar_t cls[128] = {};
            GetClassNameW(h, cls, 127);
            if (_wcsicmp(cls, L"DesktopDockWindow") == 0) {
                c->hwnd = h;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&ctx));
    if (ctx.hwnd) PostMessageW(ctx.hwnd, msg, ok ? 1 : 0, 0);
}

int Broker() {
    HANDLE mutex = CreateMutexW(nullptr, TRUE,
                                L"Local\\DesktopSuite.ServiceGuardBroker");
    if (!mutex) return 0;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(mutex);
        return 0;
    }
    std::wstring request;
    if (!ReadFileUtf8(RequestPath(), &request) || request.empty()) {
        CloseHandle(mutex);
        return 0;
    }
    DeleteFileW(RequestPath().c_str());
    const std::wstring action = ReadLineValue(request, L"action");
    const std::wstring app = ReadLineValue(request, L"app");
    const std::wstring pin = ReadLineValue(request, L"pin");
    const std::wstring appNorm = appsuspend::NormalizePath(app);
    const std::wstring pinNorm = appsuspend::NormalizePath(pin);
    TaskResult r;
    bool ok = false;
    if (action == L"restore-all") {
        r = RestoreAll();
        ok = r.failed == 0;
    } else if (!ValidateApp(app)) {
        r.detail = L"拒绝：应用路径不在允许范围";
    } else if (action == L"disable") {
        r = DisableApp(appNorm, pinNorm);
        ok = r.failed == 0;
    } else if (action == L"restore") {
        r = RestoreApp(appNorm);
        ok = r.failed == 0;
    } else {
        r.detail = L"未知 action";
    }
    const std::wstring result =
        std::wstring(ok ? L"OK" : L"FAIL") + L"|" + action + L"|" + appNorm +
        L"|" + std::to_wstring(r.disabled) + L"|" +
        std::to_wstring(r.stopped) + L"|" + std::to_wstring(r.restored) +
        L"|" + std::to_wstring(r.failed) + L"|" + r.detail + L"\r\n";
    WriteFileUtf8(ResultPath(), result);
    Logf(L"broker action=%ls app=%ls ok=%d detail=%ls", action.c_str(),
         appNorm.c_str(), ok ? 1 : 0, r.detail.c_str());
    NotifyDock(ok);
    CloseHandle(mutex);
    return ok ? 0 : 1;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    const std::wstring cmd = GetCommandLineW();
    if (HasArg(cmd, L"--install")) {
        if (!Install()) return 1;
    }
    if (HasArg(cmd, L"--repair-auto")) {
        const std::wstring app = ArgValue(L"--repair-auto");
        if (app.empty()) {
            Logf(L"repair-auto: missing app");
            return 2;
        }
        const std::wstring pin = ArgValue(L"--pin");
        const TaskResult r = RepairAuto(app, pin);
        Logf(L"repair-auto app=%ls disabled=%d stopped=%d failed=%d detail=%ls",
             app.c_str(), r.disabled, r.stopped, r.failed, r.detail.c_str());
        return r.failed == 0 ? 0 : 1;
    }
    if (HasArg(cmd, L"--install")) return 0;
    if (HasArg(cmd, L"--uninstall")) {
        return Uninstall() ? 0 : 1;
    }
    if (HasArg(cmd, L"--broker")) {
        return Broker();
    }
    return 0;
}
