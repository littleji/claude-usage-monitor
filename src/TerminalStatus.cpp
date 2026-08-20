#include "TerminalStatus.h"

#include "Json.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <map>
#include <vector>

namespace terminals {
namespace {

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

/** 读取整个文件为字节串（状态文件很小，几百字节量级）。失败返回 false。 */
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
    DWORD total_read = 0;
    while (total_read < out.size())
    {
        DWORD chunk = 0;
        if (!::ReadFile(file, &out[total_read],
                        static_cast<DWORD>(out.size() - total_read), &chunk, nullptr) ||
            chunk == 0)
        {
            ::CloseHandle(file);
            return false;
        }
        total_read += chunk;
    }
    ::CloseHandle(file);
    return true;
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

bool ParseState(const std::string& text, State& out)
{
    if (text == "idle") { out = State::Idle; return true; }
    if (text == "thinking") { out = State::Thinking; return true; }
    if (text == "waiting") { out = State::Waiting; return true; }
    if (text == "done" || text == "completed") { out = State::Done; return true; }
    if (text == "error") { out = State::Error; return true; }
    return false;
}

/** 从文件名（不含扩展名）取会话 id，作为 JSON 里缺失 session_id 时的兜底 */
std::wstring StemOf(const std::wstring& filename)
{
    const size_t dot = filename.rfind(L'.');
    return dot == std::wstring::npos ? filename : filename.substr(0, dot);
}

/**
 * 同一个 pid 下多条记录时，判断 candidate 是否比 current_best 更适合代表这个
 * 终端。优先选有 cwd 的那条——真正的终端会话（SessionStart/UserPromptSubmit/
 * Stop 等）总是带着工作目录，内部子调用观察到的样本 cwd 都是空的，比"哪条
 * 更新"更可靠：内部子调用完全可能在真正的终端会话之后才落盘，单纯比
 * updated_at 会把真会话顶掉、留下一个鼠标提示里看不出目录的空壳。
 * 都有 cwd 或都没有时，才回退到比 updated_at，选更新的那条。
 */
bool IsBetterForPid(const Entry& candidate, const Entry& current_best)
{
    const bool candidate_has_cwd = !candidate.cwd.empty();
    const bool best_has_cwd = !current_best.cwd.empty();
    if (candidate_has_cwd != best_has_cwd)
        return candidate_has_cwd;
    return candidate.updated_at > current_best.updated_at;
}

/**
 * 拥有这个终端的进程是否还活着。pid==0（旧格式文件，没记录 pid）时无法判断，
 * 返回 true 交给调用方走 updated_at 超时兜底，不误删。
 */
bool IsProcessAlive(unsigned long pid)
{
    if (pid == 0)
        return true;
    HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process == nullptr)
        return false;   // 打不开：进程已经不存在了（PID 有极小概率被别的进程复用，可接受）
    DWORD exit_code = 0;
    const bool got_exit_code = ::GetExitCodeProcess(process, &exit_code) != FALSE;
    ::CloseHandle(process);
    return got_exit_code && exit_code == STILL_ACTIVE;
}

}   // namespace

std::wstring GetStatusDir()
{
    std::wstring dir = GetEnvVar(L"CLAUDE_CONFIG_DIR");
    if (dir.empty())
    {
        std::wstring home = GetEnvVar(L"USERPROFILE");
        if (home.empty())
            return std::wstring();
        dir = home + L"\\.claude";
    }
    if (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/'))
        dir.pop_back();
    return dir + L"\\status";
}

std::vector<Entry> Scan(time_t now, int stale_seconds)
{
    std::vector<Entry> result;
    std::vector<std::wstring> paths;   // 与 result 一一对应，去重阶段要删被顶掉的文件

    const std::wstring dir = GetStatusDir();
    if (dir.empty())
        return result;

    const std::wstring pattern = dir + L"\\*.json";
    WIN32_FIND_DATAW find_data{};
    HANDLE handle = ::FindFirstFileW(pattern.c_str(), &find_data);
    if (handle == INVALID_HANDLE_VALUE)
        return result;   // 目录不存在或没有状态文件——都是正常情况（没开终端，或用户没配 hooks）

    do
    {
        if (find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;

        const std::wstring path = dir + L"\\" + find_data.cFileName;
        std::string json_text;
        if (!ReadWholeFile(path, json_text))
            continue;

        mjson::Value root;
        if (!mjson::Parse(json_text, root) || root.type != mjson::Value::T_OBJECT)
            continue;

        std::string status_text;
        if (!root.GetString("status", status_text))
            continue;
        State state{};
        if (!ParseState(status_text, state))
            continue;

        double updated_at_raw = 0.0;
        if (!root.GetNumber("updated_at", updated_at_raw))
            continue;
        const time_t updated_at = static_cast<time_t>(updated_at_raw);

        double pid_raw = 0.0;
        const unsigned long pid = root.GetNumber("pid", pid_raw)
                                       ? static_cast<unsigned long>(pid_raw)
                                       : 0;

        if (!IsProcessAlive(pid))
        {
            // 拥有这个终端的进程已经不在了（窗口被叉掉/进程被强杀，SessionEnd
            // 来不及跑）——直接删掉状态文件，不等 updated_at 超时，终端一关
            // 图标马上消失。
            ::DeleteFileW(path.c_str());
            continue;
        }
        if (pid == 0 && now - updated_at > stale_seconds)
            continue;   // 没有 pid 可查（旧格式文件）：退回超时兜底，长时间没更新就当死会话忽略

        Entry entry;
        entry.state = state;
        entry.updated_at = updated_at;
        entry.pid = pid;

        std::string session_id_utf8;
        entry.session_id = root.GetString("session_id", session_id_utf8)
                                ? Utf8ToWide(session_id_utf8)
                                : StemOf(find_data.cFileName);

        std::string cwd_utf8;
        if (root.GetString("cwd", cwd_utf8))
            entry.cwd = Utf8ToWide(cwd_utf8);

        std::string error_type_utf8;
        if (root.GetString("error_type", error_type_utf8))
            entry.error_type = Utf8ToWide(error_type_utf8);

        result.push_back(std::move(entry));
        paths.push_back(path);
    } while (::FindNextFileW(handle, &find_data));

    ::FindClose(handle);

    // 按 pid 去重：同一个进程（同一个终端）可能因为内部子调用（子代理、后台
    // 辅助模型调用等）产生多个 session_id 各自的状态文件，它们共享同一个
    // owner pid，按 session_id 数的话一个终端会被数成好几个圆点。这里只保留
    // 每个 pid 下"最适合代表这个终端"的一条（挑选规则见 IsBetterForPid）；
    // 被顶掉的文件直接删除，避免状态目录无限堆积。没有 pid 的旧格式文件无法
    // 判断是否属于同一个终端，各自独立显示，不参与去重。
    std::map<unsigned long, size_t> best_index_by_pid;
    for (size_t i = 0; i < result.size(); ++i)
    {
        if (result[i].pid == 0)
            continue;
        const auto it = best_index_by_pid.find(result[i].pid);
        if (it == best_index_by_pid.end())
        {
            best_index_by_pid.emplace(result[i].pid, i);
        }
        else if (IsBetterForPid(result[i], result[it->second]))
        {
            ::DeleteFileW(paths[it->second].c_str());
            it->second = i;
        }
        else
        {
            ::DeleteFileW(paths[i].c_str());
        }
    }

    std::vector<Entry> deduped;
    deduped.reserve(result.size());
    for (size_t i = 0; i < result.size(); ++i)
    {
        if (result[i].pid == 0)
        {
            deduped.push_back(std::move(result[i]));
            continue;
        }
        const auto it = best_index_by_pid.find(result[i].pid);
        if (it != best_index_by_pid.end() && it->second == i)
            deduped.push_back(std::move(result[i]));
    }

    // 按 session_id 排序，保证图标顺序稳定：状态变化时图标位置不会跳来跳去
    std::sort(deduped.begin(), deduped.end(),
              [](const Entry& a, const Entry& b) { return a.session_id < b.session_id; });
    return deduped;
}

const wchar_t* StateEmoji(State state)
{
    // 用纯色圆块而不是人脸/手势 emoji：任务栏字号很小，色块比表情图形更容易一眼分辨颜色
    switch (state)
    {
    // 空闲用 🔘 而不是纯色的 ⚪：白色圆块在浅色/白色任务栏背景下几乎看不见，
    // 🔘 自带一圈深色描边，深浅两种主题下都能看清轮廓。
    case State::Idle:     return L"\U0001F518";   // 🔘 空闲——没有任务在跑，也没有需要关注的事
    case State::Thinking: return L"\U0001F535";   // 🔵 正在思考/生成
    case State::Waiting:  return L"\U0001F7E1";   // 🟡 需要用户关注
    case State::Done:     return L"\U0001F7E2";   // 🟢 正常完成
    case State::Error:    return L"\U0001F534";   // 🔴 出错/异常中断
    }
    return L"?";
}

const wchar_t* StateLabel(State state, bool zh)
{
    switch (state)
    {
    case State::Idle:     return zh ? L"空闲中" : L"Idle";
    case State::Thinking: return zh ? L"正在思考" : L"Thinking";
    case State::Waiting:  return zh ? L"等待用户命令" : L"Waiting for input";
    case State::Done:     return zh ? L"已完成" : L"Done";
    case State::Error:    return zh ? L"出错/异常中断" : L"Error / interrupted";
    }
    return zh ? L"未知" : L"Unknown";
}

}   // namespace terminals
