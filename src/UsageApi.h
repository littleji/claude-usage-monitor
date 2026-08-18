/*
 * Claude 用量数据的获取。
 *
 * 取数路径完全对齐 cship（https://github.com/stephenleo/cship）：
 *   1. 读取 %USERPROFILE%\.claude\.credentials.json
 *      （若设置了 CLAUDE_CONFIG_DIR 则以该目录为准）
 *   2. 从中取出 claudeAiOauth.accessToken
 *   3. GET https://api.anthropic.com/api/oauth/usage
 *      Authorization: Bearer <token>
 *      anthropic-beta: oauth-2025-04-20
 *   4. 解析 five_hour / seven_day / seven_day_opus / seven_day_sonnet / extra_usage
 *
 * access token 只在一次请求期间以局部变量形式存在，用完立刻清零，
 * 不写入任何缓存、配置或日志。
 */
#pragma once

#include <ctime>
#include <string>

namespace usage {

/** 一个用量窗口 */
struct Period
{
    bool present{ false };          /**< API 是否返回了这个窗口（Enterprise 账号可能全为 null） */
    double utilization{ 0.0 };      /**< 已消耗百分比，0~100 */
    time_t resets_at{ 0 };          /**< 窗口重置的 Unix 纪元秒；0 表示未知 */
};

/** 额外用量（Extra usage / 按量付费额度） */
struct ExtraUsage
{
    bool present{ false };
    bool enabled{ false };
    double monthly_limit{ 0.0 };
    double used_credits{ 0.0 };
    double utilization{ 0.0 };
};

struct Data
{
    Period five_hour;
    Period seven_day;
    Period seven_day_opus;
    Period seven_day_sonnet;
    ExtraUsage extra;
};

enum class Status
{
    Ok,
    NoCredentials,      /**< 找不到 .credentials.json */
    BadCredentials,     /**< 文件存在但取不出 accessToken */
    Unauthorized,       /**< HTTP 401/403，通常是 token 过期 */
    RateLimited,        /**< HTTP 429，请求过于频繁 */
    HttpError,          /**< 其他 HTTP 状态码 */
    NetworkError,       /**< 连接失败 / 超时 */
    ParseError,         /**< 响应不是预期的 JSON */
};

struct Result
{
    Status status{ Status::NetworkError };
    Data data;
    std::wstring message;   /**< 面向用户的中文错误描述，Ok 时为空 */
    int retry_after{ 0 };   /**< 被限流时服务端建议的等待秒数；0 表示未提供 */
};

/** 同步获取一次用量。调用者必须在后台线程中调用，本函数会阻塞数秒。 */
Result Fetch();

/** 供单元测试 / 探针使用：解析一段 API 响应 JSON */
bool ParseUsageJson(const std::string& json, Data& out);

/** 凭据文件的完整路径（用于错误提示） */
std::wstring GetCredentialsPath();

}   // namespace usage
