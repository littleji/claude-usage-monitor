#include "UsageApi.h"

#include "Json.h"
#include "TimeUtil.h"

#include <windows.h>
#include <winhttp.h>

#include <vector>

namespace usage {
namespace {

const wchar_t* const kApiHost = L"api.anthropic.com";
const wchar_t* const kApiPath = L"/api/oauth/usage";
const wchar_t* const kOAuthBetaHeader = L"oauth-2025-04-20";
const wchar_t* const kDefaultUserAgent = L"TrafficMonitor-ClaudeUsage/1.0";

// 超时（毫秒）。cship 是状态栏程序所以用 1.5s，这里跑在后台线程，
// 可以给得宽松一些，避免网络抖动时频繁显示错误。
const DWORD kResolveTimeout = 5000;
const DWORD kConnectTimeout = 5000;
const DWORD kSendTimeout = 5000;
const DWORD kReceiveTimeout = 8000;

const DWORD kMaxResponseBytes = 1 * 1024 * 1024;

/** HINTERNET 的 RAII 包装 */
class Handle
{
public:
    Handle() = default;
    explicit Handle(HINTERNET h) : m_h(h) {}
    ~Handle() { Reset(); }

    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;

    void Reset(HINTERNET h = nullptr)
    {
        if (m_h != nullptr)
            ::WinHttpCloseHandle(m_h);
        m_h = h;
    }

    HINTERNET Get() const { return m_h; }
    explicit operator bool() const { return m_h != nullptr; }

private:
    HINTERNET m_h{ nullptr };
};

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

/** 读取整个文件为字节串。失败返回 false。 */
bool ReadWholeFile(const std::wstring& path, std::string& out)
{
    HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;

    LARGE_INTEGER size{};
    if (!::GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > 4 * 1024 * 1024)
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

/** 就地清零一段字符串占用的内存，避免 token 残留在进程内存里 */
void WipeString(std::string& s)
{
    if (!s.empty())
        ::SecureZeroMemory(&s[0], s.size());
    s.clear();
}

void WipeWString(std::wstring& s)
{
    if (!s.empty())
        ::SecureZeroMemory(&s[0], s.size() * sizeof(wchar_t));
    s.clear();
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

/** 从凭据 JSON 中取出 claudeAiOauth.accessToken */
bool ExtractAccessToken(const std::string& json_text, std::string& out_token)
{
    mjson::Value root;
    if (!mjson::Parse(json_text, root))
        return false;
    const mjson::Value* oauth = root.FindObject("claudeAiOauth");
    if (oauth == nullptr)
        return false;
    std::string token;
    if (!oauth->GetString("accessToken", token) || token.empty())
        return false;
    out_token.swap(token);
    return true;
}

/** 解析形如 {"utilization": 47.0, "resets_at": "..."} 的窗口对象 */
void ReadPeriod(const mjson::Value& root, const char* key, Period& out)
{
    const mjson::Value* node = root.FindObject(key);
    if (node == nullptr)
        return;   // 键缺失或为 null —— 该窗口对本账号不适用

    double utilization = 0.0;
    if (!node->GetNumber("utilization", utilization))
        return;

    out.present = true;
    out.utilization = utilization;

    std::string resets_at;
    if (node->GetString("resets_at", resets_at))
    {
        time_t epoch = 0;
        if (timeutil::Iso8601ToEpoch(resets_at, epoch))
            out.resets_at = epoch;
    }
}

/**
 * 从错误响应中提取可读信息。
 * Anthropic 的错误体形如 {"type":"error","error":{"type":"...","message":"..."}}，
 * 解析不出来时退回到原始正文的前若干字符。
 */
std::wstring DescribeErrorBody(const std::string& body)
{
    if (body.empty())
        return std::wstring();

    mjson::Value root;
    if (mjson::Parse(body, root))
    {
        if (const mjson::Value* error = root.FindObject("error"))
        {
            std::string message;
            std::string type;
            error->GetString("message", message);
            error->GetString("type", type);
            if (!message.empty())
                return Utf8ToWide(message);
            if (!type.empty())
                return Utf8ToWide(type);
        }
    }

    std::string snippet = body.substr(0, 200);
    // 去掉换行，避免把提示框撑得太乱
    for (char& c : snippet)
    {
        if (c == '\r' || c == '\n')
            c = ' ';
    }
    return Utf8ToWide(snippet);
}

std::wstring DescribeWinHttpError(DWORD code)
{
    switch (code)
    {
    case ERROR_WINHTTP_TIMEOUT:
        return L"请求超时";
    case ERROR_WINHTTP_NAME_NOT_RESOLVED:
        return L"无法解析 api.anthropic.com";
    case ERROR_WINHTTP_CANNOT_CONNECT:
        return L"无法连接到 api.anthropic.com";
    case ERROR_WINHTTP_CONNECTION_ERROR:
        return L"连接被中断";
    case ERROR_WINHTTP_SECURE_FAILURE:
        return L"TLS 握手失败";
    default:
    {
        wchar_t buffer[64]{};
        _snwprintf_s(buffer, _TRUNCATE, L"网络错误（代码 %lu）", code);
        return buffer;
    }
    }
}

/**
 * 请求使用的 User-Agent。
 * 少数代理/网关会按 User-Agent 做拦截或限流，允许用环境变量
 * CLAUDE_USAGE_MONITOR_UA 覆盖，便于排查这类问题。
 */
std::wstring GetUserAgent()
{
    const std::wstring override_ua = GetEnvVar(L"CLAUDE_USAGE_MONITOR_UA");
    return override_ua.empty() ? std::wstring(kDefaultUserAgent) : override_ua;
}

/** 读取 Retry-After 响应头（秒）。没有该头或无法解析时返回 0。 */
int ReadRetryAfter(HINTERNET request)
{
    wchar_t buffer[64]{};
    DWORD size = sizeof(buffer);
    if (!::WinHttpQueryHeaders(request, WINHTTP_QUERY_CUSTOM,
                               L"Retry-After", buffer, &size, WINHTTP_NO_HEADER_INDEX))
    {
        return 0;
    }
    const long seconds = ::wcstol(buffer, nullptr, 10);
    if (seconds <= 0 || seconds > 3600)
        return 0;
    return static_cast<int>(seconds);
}

/** 执行一次 HTTPS GET。成功返回 true 并写出状态码与响应体。 */
bool HttpGet(const std::wstring& bearer_token, DWORD& out_status,
             int& out_retry_after, std::string& out_body, std::wstring& out_error)
{
    const std::wstring user_agent = GetUserAgent();

    Handle session(::WinHttpOpen(user_agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                 WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session)
    {
        // 较老的系统不支持 AUTOMATIC_PROXY，退回到系统默认代理配置
        session.Reset(::WinHttpOpen(user_agent.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    }
    if (!session)
    {
        out_error = DescribeWinHttpError(::GetLastError());
        return false;
    }

    ::WinHttpSetTimeouts(session.Get(), kResolveTimeout, kConnectTimeout,
                         kSendTimeout, kReceiveTimeout);

    // 我们没有主动发 Accept-Encoding，但服务端仍可能返回压缩内容。
    // 打开自动解压，拿到的就一定是明文 JSON。老系统不支持时忽略失败即可。
    DWORD decompression = WINHTTP_DECOMPRESSION_FLAG_ALL;
    ::WinHttpSetOption(session.Get(), WINHTTP_OPTION_DECOMPRESSION,
                       &decompression, sizeof(decompression));

    Handle connect(::WinHttpConnect(session.Get(), kApiHost, INTERNET_DEFAULT_HTTPS_PORT, 0));
    if (!connect)
    {
        out_error = DescribeWinHttpError(::GetLastError());
        return false;
    }

    Handle request(::WinHttpOpenRequest(connect.Get(), L"GET", kApiPath, nullptr,
                                        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                        WINHTTP_FLAG_SECURE));
    if (!request)
    {
        out_error = DescribeWinHttpError(::GetLastError());
        return false;
    }

    std::wstring headers;
    headers += L"Authorization: Bearer ";
    headers += bearer_token;
    headers += L"\r\nanthropic-beta: ";
    headers += kOAuthBetaHeader;
    headers += L"\r\nAccept: application/json\r\n";

    const BOOL sent = ::WinHttpSendRequest(request.Get(), headers.c_str(),
                                           static_cast<DWORD>(headers.size()),
                                           WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    WipeWString(headers);
    if (!sent)
    {
        out_error = DescribeWinHttpError(::GetLastError());
        return false;
    }

    if (!::WinHttpReceiveResponse(request.Get(), nullptr))
    {
        out_error = DescribeWinHttpError(::GetLastError());
        return false;
    }

    DWORD status = 0;
    DWORD status_size = sizeof(status);
    if (!::WinHttpQueryHeaders(request.Get(),
                               WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                               WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
                               WINHTTP_NO_HEADER_INDEX))
    {
        out_error = DescribeWinHttpError(::GetLastError());
        return false;
    }
    out_status = status;
    out_retry_after = ReadRetryAfter(request.Get());

    out_body.clear();
    for (;;)
    {
        DWORD available = 0;
        if (!::WinHttpQueryDataAvailable(request.Get(), &available))
        {
            out_error = DescribeWinHttpError(::GetLastError());
            return false;
        }
        if (available == 0)
            break;
        if (out_body.size() + available > kMaxResponseBytes)
        {
            out_error = L"响应内容异常过大";
            return false;
        }

        const size_t offset = out_body.size();
        out_body.resize(offset + available);
        DWORD read = 0;
        if (!::WinHttpReadData(request.Get(), &out_body[offset], available, &read))
        {
            out_error = DescribeWinHttpError(::GetLastError());
            return false;
        }
        out_body.resize(offset + read);
        if (read == 0)
            break;
    }
    return true;
}

}   // namespace

std::wstring GetCredentialsPath()
{
    // Claude Code 支持用 CLAUDE_CONFIG_DIR 改写配置目录；未设置时用 ~/.claude
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
    return dir + L"\\.credentials.json";
}

bool ParseUsageJson(const std::string& json, Data& out)
{
    mjson::Value root;
    if (!mjson::Parse(json, root) || root.type != mjson::Value::T_OBJECT)
        return false;

    ReadPeriod(root, "five_hour", out.five_hour);
    ReadPeriod(root, "seven_day", out.seven_day);
    ReadPeriod(root, "seven_day_opus", out.seven_day_opus);
    ReadPeriod(root, "seven_day_sonnet", out.seven_day_sonnet);

    if (const mjson::Value* extra = root.FindObject("extra_usage"))
    {
        out.extra.present = true;
        extra->GetBool("is_enabled", out.extra.enabled);
        extra->GetNumber("monthly_limit", out.extra.monthly_limit);
        extra->GetNumber("used_credits", out.extra.used_credits);
        extra->GetNumber("utilization", out.extra.utilization);
    }
    return true;
}

/**
 * 演示模式：设置环境变量 CLAUDE_USAGE_MONITOR_DEMO="85,42" 后不再访问接口，
 * 直接产出 5 小时窗口 85%、7 天窗口 42% 的假数据。
 *
 * 用来预览阈值颜色和进度条的效果——否则要等真的用到 80% 额度才看得到危险色。
 * 解析失败时返回 false，照常走真实取数流程。
 */
bool TryDemoData(Data& out)
{
    const std::wstring spec = GetEnvVar(L"CLAUDE_USAGE_MONITOR_DEMO");
    if (spec.empty())
        return false;

    wchar_t* end = nullptr;
    const double five = ::wcstod(spec.c_str(), &end);
    if (end == spec.c_str())
        return false;
    while (end != nullptr && (*end == L',' || *end == L' '))
        ++end;
    const double seven = (end != nullptr && *end != L'\0') ? ::wcstod(end, nullptr) : five;

    const time_t now = ::time(nullptr);
    out = Data{};
    out.five_hour.present = true;
    out.five_hour.utilization = five;
    out.five_hour.resets_at = now + 6600;              // 1h50m
    out.seven_day.present = true;
    out.seven_day.utilization = seven;
    out.seven_day.resets_at = now + 3 * 86400 + 7200;  // 3d2h
    return true;
}

Result Fetch()
{
    Result result;

    if (TryDemoData(result.data))
    {
        result.status = Status::Ok;
        return result;
    }

    const std::wstring credentials_path = GetCredentialsPath();
    if (credentials_path.empty())
    {
        result.status = Status::NoCredentials;
        result.message = L"无法确定用户主目录，找不到 Claude 凭据";
        return result;
    }

    std::string credentials_text;
    if (!ReadWholeFile(credentials_path, credentials_text))
    {
        result.status = Status::NoCredentials;
        result.message = L"未找到凭据文件：" + credentials_path +
                         L"\n请先在 Claude Code 中登录";
        return result;
    }

    std::string token_utf8;
    const bool has_token = ExtractAccessToken(credentials_text, token_utf8);
    WipeString(credentials_text);
    if (!has_token)
    {
        result.status = Status::BadCredentials;
        result.message = L"凭据文件中读不到 claudeAiOauth.accessToken\n请在 Claude Code 中重新登录";
        return result;
    }

    std::wstring token = Utf8ToWide(token_utf8);
    WipeString(token_utf8);

    DWORD http_status = 0;
    int retry_after = 0;
    std::string body;
    std::wstring network_error;
    const bool ok = HttpGet(token, http_status, retry_after, body, network_error);
    WipeWString(token);

    if (!ok)
    {
        result.status = Status::NetworkError;
        result.message = network_error;
        return result;
    }

    if (http_status != 200)
    {
        const std::wstring detail = DescribeErrorBody(body);

        if (http_status == 401 || http_status == 403)
        {
            result.status = Status::Unauthorized;
            result.message = L"访问令牌已失效（HTTP " + std::to_wstring(http_status) +
                             L"）\n请在 Claude Code 中重新登录以刷新凭据";
        }
        else if (http_status == 429)
        {
            // 用量接口的限流比较紧，多个客户端（Claude Code、cship、本插件）
            // 共用同一个账号时容易撞上。这不是配置错误，退避后重试即可。
            result.status = Status::RateLimited;
            result.retry_after = retry_after;
            result.message = L"请求过于频繁（HTTP 429）";
            result.message += (retry_after > 0)
                                  ? L"，将在 " + std::to_wstring(retry_after) + L" 秒后重试"
                                  : L"，稍后自动重试";
        }
        else
        {
            result.status = Status::HttpError;
            result.message = L"接口返回 HTTP " + std::to_wstring(http_status);
        }

        if (!detail.empty())
            result.message += L"\n" + detail;
        return result;
    }

    if (!ParseUsageJson(body, result.data))
    {
        result.status = Status::ParseError;
        result.message = L"接口返回的内容无法解析";
        return result;
    }

    result.status = Status::Ok;
    return result;
}

}   // namespace usage
