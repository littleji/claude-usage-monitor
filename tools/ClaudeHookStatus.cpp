/*
 * claude-hook-status.exe：Claude Code hook 命令，把当前会话的状态写到插件能读到的地方。
 *
 *   claude-hook-status.exe <Event>
 *
 * 和 tools/claude-hook-status.ps1 行为一致，只是换成原生程序：每个 hook 事件都要
 * 同步跑一次这个命令，PostToolUse 更是每次工具调用都触发，powershell 光启动就要
 * 几百毫秒，会明显拖慢 Claude Code；原生 exe 启动只要十几毫秒。事件映射、
 * 子代理跳过、owner PID、卡死 thinking 清理等规则的来龙去脉见 ps1 顶部的说明，
 * 这里不重复。
 *
 * 用 /SUBSYSTEM:WINDOWS 链接（入口仍是 wmain），被 Claude Code 拉起时不会闪控制台
 * 窗口；stdin 是 Claude Code 给的管道，GetStdHandle 照样能读。
 *
 * 设置环境变量 CLAUDE_HOOK_STATUS_DEBUG=1 时，把每次调用的处理结果追加到
 * <配置目录>\hook-debug.log，用于排查。
 */
#include "../src/Json.h"

#include <windows.h>
#include <objbase.h>
#include <tlhelp32.h>

#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

namespace {

constexpr int kStaleThinkingSeconds = 5 * 60;

std::wstring GetEnvVar(const wchar_t* name)
{
    const DWORD needed = ::GetEnvironmentVariableW(name, nullptr, 0);
    if (needed == 0)
        return std::wstring();
    std::vector<wchar_t> buffer(needed);
    const DWORD written = ::GetEnvironmentVariableW(name, buffer.data(), needed);
    if (written == 0 || written >= needed)
        return std::wstring();
    return std::wstring(buffer.data(), written);
}

std::wstring Utf8ToWide(const std::string& utf8)
{
    if (utf8.empty())
        return std::wstring();
    const int needed = ::MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
                                             static_cast<int>(utf8.size()), nullptr, 0);
    if (needed <= 0)
        return std::wstring();
    std::wstring result(static_cast<size_t>(needed), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()),
                          &result[0], needed);
    return result;
}

std::string WideToUtf8(const std::wstring& wide)
{
    if (wide.empty())
        return std::string();
    const int needed = ::WideCharToMultiByte(CP_UTF8, 0, wide.c_str(),
                                             static_cast<int>(wide.size()),
                                             nullptr, 0, nullptr, nullptr);
    if (needed <= 0)
        return std::string();
    std::string result(static_cast<size_t>(needed), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()),
                          &result[0], needed, nullptr, nullptr);
    return result;
}

std::wstring g_debug_log_path;
std::string g_event_utf8;

void DebugLog(const std::string& message)
{
    if (g_debug_log_path.empty())
        return;
    SYSTEMTIME st{};
    ::GetLocalTime(&st);
    char prefix[64]{};
    _snprintf_s(prefix, _TRUNCATE, "%02u:%02u:%02u.%03u pid=%lu ",
                st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, ::GetCurrentProcessId());
    const std::string line = prefix + std::string("event=") + g_event_utf8 + " " + message + "\r\n";
    HANDLE file = ::CreateFileW(g_debug_log_path.c_str(), FILE_APPEND_DATA,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return;
    DWORD written = 0;
    ::WriteFile(file, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    ::CloseHandle(file);
}

/** 读完 stdin（Claude Code 传进来的事件 JSON，固定 UTF-8）。 */
std::string ReadStdin()
{
    std::string result;
    HANDLE in = ::GetStdHandle(STD_INPUT_HANDLE);
    if (in == nullptr || in == INVALID_HANDLE_VALUE)
        return result;
    char buffer[4096];
    for (;;)
    {
        DWORD read = 0;
        if (!::ReadFile(in, buffer, sizeof(buffer), &read, nullptr) || read == 0)
            break;
        result.append(buffer, read);
        if (result.size() > 16 * 1024 * 1024)
            break;   // 防御：正常事件远小于这个量级
    }
    return result;
}

bool ReadWholeFile(const std::wstring& path, std::string& out)
{
    HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    LARGE_INTEGER size{};
    if (!::GetFileSizeEx(file, &size) || size.QuadPart < 0 || size.QuadPart > 64 * 1024)
    {
        ::CloseHandle(file);
        return false;
    }
    out.resize(static_cast<size_t>(size.QuadPart));
    DWORD total = 0;
    while (total < out.size())
    {
        DWORD chunk = 0;
        if (!::ReadFile(file, &out[total], static_cast<DWORD>(out.size() - total), &chunk, nullptr) ||
            chunk == 0)
        {
            ::CloseHandle(file);
            return false;
        }
        total += chunk;
    }
    ::CloseHandle(file);
    // ps1 版本用 Set-Content -Encoding utf8 写文件，Windows PowerShell 5 会带 BOM
    if (out.size() >= 3 && static_cast<unsigned char>(out[0]) == 0xEF &&
        static_cast<unsigned char>(out[1]) == 0xBB && static_cast<unsigned char>(out[2]) == 0xBF)
        out.erase(0, 3);
    return true;
}

void AppendJsonString(std::string& out, const std::string& value)
{
    out += '"';
    for (const char c : value)
    {
        const unsigned char uc = static_cast<unsigned char>(c);
        switch (c)
        {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (uc < 0x20)
            {
                char esc[8]{};
                _snprintf_s(esc, _TRUNCATE, "\\u%04x", uc);
                out += esc;
            }
            else
            {
                out += c;
            }
        }
    }
    out += '"';
}

struct Record
{
    std::string session_id;
    std::string status;
    long long updated_at{ 0 };
    std::string cwd;
    std::string error_type;
    unsigned long pid{ 0 };
    bool watchdog_flip{ false };
};

/** 先写临时文件再整体替换：插件扫描时不会读到写了一半的 JSON。 */
bool WriteRecord(const std::wstring& path, const Record& r)
{
    std::string json = "{\"session_id\":";
    AppendJsonString(json, r.session_id);
    json += ",\"status\":";
    AppendJsonString(json, r.status);
    json += ",\"updated_at\":" + std::to_string(r.updated_at);
    json += ",\"cwd\":";
    AppendJsonString(json, r.cwd);
    json += ",\"error_type\":";
    AppendJsonString(json, r.error_type);
    json += ",\"pid\":" + std::to_string(r.pid);
    if (r.watchdog_flip)
        json += ",\"watchdog_flip\":true";
    json += "}";

    const std::wstring temp_path = path + L"." + std::to_wstring(::GetCurrentProcessId()) + L".tmp";
    HANDLE file = ::CreateFileW(temp_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    DWORD written = 0;
    const bool ok = ::WriteFile(file, json.data(), static_cast<DWORD>(json.size()), &written, nullptr) &&
                    written == json.size();
    ::CloseHandle(file);
    if (!ok || !::MoveFileExW(temp_path.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING))
    {
        ::DeleteFileW(temp_path.c_str());
        return false;
    }
    return true;
}

bool IsProcessAlive(unsigned long pid)
{
    HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process == nullptr)
        return false;
    DWORD exit_code = 0;
    const bool got = ::GetExitCodeProcess(process, &exit_code) != FALSE;
    ::CloseHandle(process);
    return got && exit_code == STILL_ACTIVE;
}

/**
 * 顺着进程树往上找拥有这个终端的长驻进程（通常就是 claude.exe），跳过
 * powershell/cmd/bash 这类中转 shell。用 ToolHelp 快照一次拿全表，
 * 不像 ps1 版本那样每层发一次 WMI 查询。
 */
unsigned long GetOwnerProcessId(bool& background)
{
    background = false;
    static const wchar_t* const kShellNames[] = {
        L"powershell.exe", L"pwsh.exe", L"cmd.exe", L"conhost.exe",
        L"sh.exe", L"bash.exe", L"wsl.exe",
    };

    unsigned long current = ::GetCurrentProcessId();
    HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return current;

    struct Proc { unsigned long pid; unsigned long parent; std::wstring name; };
    std::vector<Proc> procs;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (::Process32FirstW(snapshot, &entry))
    {
        do
        {
            procs.push_back({ entry.th32ProcessID, entry.th32ParentProcessID, entry.szExeFile });
        } while (::Process32NextW(snapshot, &entry));
    }
    ::CloseHandle(snapshot);

    auto find = [&](unsigned long pid) -> const Proc* {
        for (const Proc& p : procs)
            if (p.pid == pid)
                return &p;
        return nullptr;
    };

    for (int i = 0; i < 8; ++i)
    {
        const Proc* self = find(current);
        if (self == nullptr || self->parent == 0)
            return current;
        const Proc* parent = find(self->parent);
        if (parent == nullptr)
            return current;
        bool is_shell = false;
        for (const wchar_t* shell : kShellNames)
            if (::_wcsicmp(parent->name.c_str(), shell) == 0)
                is_shell = true;
        if (!is_shell)
        {
            // 后台会话（claude --bg）由 daemon 的 claude.exe --bg-pty-host 拉起，
            // 会话进程的直接父进程也是 claude.exe；前台终端的父进程是 shell 或
            // headroom 之类的包装器，不会是 claude.exe。
            const Proc* grand = find(parent->parent);
            background = ::_wcsicmp(parent->name.c_str(), L"claude.exe") == 0 &&
                         grand != nullptr &&
                         ::_wcsicmp(grand->name.c_str(), L"claude.exe") == 0;
            return parent->pid;
        }
        current = parent->pid;
    }
    return current;
}

bool ParseRecord(const std::string& text, Record& r)
{
    mjson::Value root;
    if (!mjson::Parse(text, root) || root.type != mjson::Value::T_OBJECT)
        return false;
    root.GetString("session_id", r.session_id);
    root.GetString("status", r.status);
    root.GetString("cwd", r.cwd);
    root.GetString("error_type", r.error_type);
    double number = 0.0;
    if (root.GetNumber("updated_at", number))
        r.updated_at = static_cast<long long>(number);
    if (root.GetNumber("pid", number) && number > 0)
        r.pid = static_cast<unsigned long>(number);
    return true;
}

/**
 * 三方网关偶尔吞掉 stop_reason，Stop 不发，状态卡在 thinking。每次事件顺手
 * 扫一遍：超过 5 分钟还在 thinking 的，进程死了就删文件，活着就翻 done。
 */
void CleanupStaleThinking(const std::wstring& status_dir, long long now)
{
    WIN32_FIND_DATAW find_data{};
    HANDLE handle = ::FindFirstFileW((status_dir + L"\\*.json").c_str(), &find_data);
    if (handle == INVALID_HANDLE_VALUE)
        return;
    do
    {
        if (find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;
        const std::wstring path = status_dir + L"\\" + find_data.cFileName;
        std::string text;
        Record r;
        if (!ReadWholeFile(path, text) || !ParseRecord(text, r))
            continue;
        if (r.status != "thinking" || now - r.updated_at <= kStaleThinkingSeconds)
            continue;
        if (r.pid > 0 && !IsProcessAlive(r.pid))
        {
            ::DeleteFileW(path.c_str());
            continue;
        }
        // 翻 done 不翻 error：真出错时 StopFailure 已经写了 error
        r.status = "done";
        r.updated_at = now;
        r.watchdog_flip = true;
        WriteRecord(path, r);
    } while (::FindNextFileW(handle, &find_data));
    ::FindClose(handle);
}

std::string NewGuidString()
{
    GUID guid{};
    ::CoCreateGuid(&guid);
    char buffer[40]{};
    _snprintf_s(buffer, _TRUNCATE, "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                guid.Data1, guid.Data2, guid.Data3, guid.Data4[0], guid.Data4[1],
                guid.Data4[2], guid.Data4[3], guid.Data4[4], guid.Data4[5],
                guid.Data4[6], guid.Data4[7]);
    return buffer;
}

}   // namespace

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2)
        return 0;
    const std::wstring event = argv[1];
    g_event_utf8 = WideToUtf8(event);

    std::wstring config_dir = GetEnvVar(L"CLAUDE_CONFIG_DIR");
    if (config_dir.empty())
    {
        const std::wstring home = GetEnvVar(L"USERPROFILE");
        if (home.empty())
            return 0;
        config_dir = home + L"\\.claude";
    }
    if (config_dir.back() == L'\\' || config_dir.back() == L'/')
        config_dir.pop_back();
    if (GetEnvVar(L"CLAUDE_HOOK_STATUS_DEBUG") == L"1")
        g_debug_log_path = config_dir + L"\\hook-debug.log";

    const std::string stdin_text = ReadStdin();
    mjson::Value payload;
    const bool parsed = !stdin_text.empty() && mjson::Parse(stdin_text, payload) &&
                        payload.type == mjson::Value::T_OBJECT;
    DebugLog("stdin_len=" + std::to_string(stdin_text.size()) + " parsed=" + (parsed ? "1" : "0"));

    // 子代理事件不代表用户看得见的终端窗口；只认 agent_type，原因见 ps1
    std::string agent_type;
    if (parsed && payload.GetString("agent_type", agent_type) && !agent_type.empty())
    {
        DebugLog("skip: subagent event agent_type=" + agent_type);
        return 0;
    }

    Record record;
    if (!parsed || !payload.GetString("session_id", record.session_id) || record.session_id.empty())
    {
        // 不用固定占位名，否则不同终端会互相覆盖
        record.session_id = NewGuidString();
        DebugLog("WARNING: no session_id, fallback guid=" + record.session_id);
    }
    if (parsed)
    {
        payload.GetString("cwd", record.cwd);
        payload.GetString("error_type", record.error_type);
    }

    const std::wstring status_dir = config_dir + L"\\status";
    ::CreateDirectoryW(status_dir.c_str(), nullptr);

    const long long now = static_cast<long long>(::_time64(nullptr));
    CleanupStaleThinking(status_dir, now);

    // 文件名只用会话 id，非 [A-Za-z0-9-] 字符替换成下划线
    std::wstring safe_id = Utf8ToWide(record.session_id);
    for (wchar_t& c : safe_id)
    {
        const bool ok = (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z') ||
                        (c >= L'0' && c <= L'9') || c == L'-';
        if (!ok)
            c = L'_';
    }
    const std::wstring file = status_dir + L"\\" + safe_id + L".json";

    if (event == L"SessionEnd")
    {
        ::DeleteFileW(file.c_str());
        DebugLog("SessionEnd: deleted");
        return 0;
    }

    if (event == L"Notification")
    {
        std::string type;
        if (parsed)
            payload.GetString("notification_type", type);
        if (type == "idle_prompt")
        {
            record.status = "idle";
        }
        else if (type == "permission_prompt" || type == "elicitation_dialog" ||
                 type == "elicitation_url_dialog" || type == "agent_needs_input")
        {
            record.status = "waiting";
        }
        else
        {
            DebugLog("skip: notification_type=" + type);
            return 0;
        }
    }
    else if (event == L"PostToolUseFailure")
    {
        // 用户按 Esc 打断：这一轮已经停了，不会再有 Stop，别翻回 thinking
        bool is_interrupt = false;
        if (parsed && payload.GetBool("is_interrupt", is_interrupt) && is_interrupt)
        {
            DebugLog("skip: PostToolUseFailure is_interrupt=true");
            return 0;
        }
        record.status = "thinking";
    }
    else if (event == L"SessionStart")
    {
        record.status = "idle";
    }
    else if (event == L"UserPromptSubmit" || event == L"PreToolUse" || event == L"PostToolUse")
    {
        record.status = "thinking";
    }
    else if (event == L"Stop")
    {
        record.status = "done";
    }
    else if (event == L"StopFailure")
    {
        record.status = "error";
    }
    else
    {
        DebugLog("skip: unknown event");
        return 0;
    }

    // 后台会话不需要用户操作，不上报；每次事件都判断（ToolHelp 快照很便宜），
    // 这样已经写出去的旧状态文件也会在下一次事件时被清掉
    bool background = false;
    const unsigned long owner_pid = GetOwnerProcessId(background);
    if (background)
    {
        ::DeleteFileW(file.c_str());
        DebugLog("skip: background session owner_pid=" + std::to_string(owner_pid));
        return 0;
    }

    // 同一会话优先复用文件里记好的 owner PID
    std::string existing_text;
    Record existing;
    if (ReadWholeFile(file, existing_text) && ParseRecord(existing_text, existing) && existing.pid > 0)
        record.pid = existing.pid;
    if (record.pid == 0)
        record.pid = owner_pid;

    record.updated_at = now;
    const bool written = WriteRecord(file, record);
    DebugLog("wrote: status=" + record.status + " pid=" + std::to_string(record.pid) +
             (written ? "" : " FAILED"));
    return 0;
}
