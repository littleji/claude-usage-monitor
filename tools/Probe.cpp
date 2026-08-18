/*
 * 命令行探针：不进 TrafficMonitor，直接跑一遍插件的取数与格式化逻辑。
 *
 *   Probe.exe          实际访问 Anthropic 接口，打印用量
 *   Probe.exe --selftest  只跑离线解析与格式化的自检，不联网
 *
 * 注意：本程序只打印用量数值，绝不打印 access token。
 */
#include "../src/TimeUtil.h"
#include "../src/UsageApi.h"

#include <windows.h>

#include <cstdio>
#include <string>

namespace {

/** 用 WriteConsoleW 输出，避免控制台代码页导致的乱码 */
void PrintW(const std::wstring& text)
{
    HANDLE out = ::GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (out != INVALID_HANDLE_VALUE && ::GetConsoleMode(out, &mode))
    {
        DWORD written = 0;
        ::WriteConsoleW(out, text.c_str(), static_cast<DWORD>(text.size()), &written, nullptr);
        return;
    }
    // 被重定向到文件/管道时输出 UTF-8
    const int needed = ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
                                             static_cast<int>(text.size()),
                                             nullptr, 0, nullptr, nullptr);
    if (needed <= 0)
        return;
    std::string utf8(static_cast<size_t>(needed), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                          &utf8[0], needed, nullptr, nullptr);
    ::fwrite(utf8.data(), 1, utf8.size(), stdout);
}

void PrintLine(const std::wstring& text)
{
    PrintW(text + L"\n");
}

std::wstring FormatPeriod(const wchar_t* label, const usage::Period& period, time_t now)
{
    if (!period.present)
        return std::wstring(label) + L": N/A (接口未返回此窗口)";

    wchar_t percent[16]{};
    _snwprintf_s(percent, _TRUNCATE, L"%.0f%%", period.utilization);

    std::wstring line = std::wstring(label) + L": " + percent +
                        L" (" + timeutil::FormatTimeToReset(period.resets_at, now) + L")";
    if (period.resets_at != 0)
        line += L"   重置于 " + timeutil::FormatResetAt(period.resets_at, now);
    return line;
}

int Check(bool condition, const wchar_t* name)
{
    PrintLine(std::wstring(condition ? L"  [ok]   " : L"  [FAIL] ") + name);
    return condition ? 0 : 1;
}

/** 离线自检：解析与格式化，不联网 */
int SelfTest()
{
    int failures = 0;
    PrintLine(L"离线自检");

    // 1. 标准响应
    {
        const std::string json = R"({
            "five_hour": {"utilization": 21.0, "resets_at": "2026-08-17T12:00:00+00:00"},
            "seven_day": {"utilization": 47.5, "resets_at": "2026-08-20T00:00:00+00:00"},
            "seven_day_opus": {"utilization": 12.0, "resets_at": "2026-08-20T00:00:00+00:00"},
            "seven_day_sonnet": null,
            "iguana_necktie": null,
            "extra_usage": {"is_enabled": true, "monthly_limit": 20000, "used_credits": 6195.0, "utilization": 30.975}
        })";
        usage::Data data;
        failures += Check(usage::ParseUsageJson(json, data), L"解析标准响应");
        failures += Check(data.five_hour.present && data.five_hour.utilization == 21.0, L"five_hour 百分比");
        failures += Check(data.five_hour.resets_at != 0, L"five_hour 重置时间");
        failures += Check(data.seven_day.present && data.seven_day.utilization == 47.5, L"seven_day 百分比");
        failures += Check(data.seven_day_opus.present, L"seven_day_opus 存在");
        failures += Check(!data.seven_day_sonnet.present, L"seven_day_sonnet 为 null 时不置位");
        failures += Check(data.extra.present && data.extra.enabled, L"extra_usage 解析");
    }

    // 2. Enterprise 形态：标准字段全为 null
    {
        const std::string json = R"({
            "five_hour": null, "seven_day": null, "seven_day_opus": null,
            "extra_usage": {"is_enabled": true, "monthly_limit": 20000, "used_credits": 19411.0, "utilization": 97.055, "currency": "USD"}
        })";
        usage::Data data;
        failures += Check(usage::ParseUsageJson(json, data), L"解析 Enterprise 响应");
        failures += Check(!data.five_hour.present, L"Enterprise 下 five_hour 不置位");
        failures += Check(data.extra.utilization > 97.0, L"Enterprise 下 extra_usage 可用");
    }

    // 3. 畸形输入不应崩溃、必须返回失败
    {
        usage::Data data;
        failures += Check(!usage::ParseUsageJson("not json", data), L"拒绝非 JSON");
        failures += Check(!usage::ParseUsageJson("{\"a\":", data), L"拒绝截断的 JSON");
        failures += Check(usage::ParseUsageJson("{}", data), L"接受空对象");
    }

    // 4. 时间解析
    {
        // 2026-08-17T09:30:00Z 对应的 Unix 纪元秒
        const time_t kExpected = 1786959000;
        time_t epoch = 0;
        failures += Check(timeutil::Iso8601ToEpoch("2026-08-17T09:30:00+00:00", epoch) &&
                              epoch == kExpected,
                          L"ISO8601 带 +00:00 偏移");
        failures += Check(timeutil::Iso8601ToEpoch("2026-08-17T09:30:00Z", epoch) &&
                              epoch == kExpected,
                          L"ISO8601 带 Z");
        failures += Check(timeutil::Iso8601ToEpoch("2026-08-17T10:30:00+01:00", epoch) &&
                              epoch == kExpected,
                          L"ISO8601 带 +01:00 偏移");
        failures += Check(timeutil::Iso8601ToEpoch("2026-08-17T09:30:00.123456Z", epoch) &&
                              epoch == kExpected,
                          L"ISO8601 带小数秒");
        failures += Check(!timeutil::Iso8601ToEpoch("", epoch), L"拒绝空时间串");
        failures += Check(!timeutil::Iso8601ToEpoch("nonsense", epoch), L"拒绝无效时间串");
    }

    // 5. 剩余时间格式，对齐 cship 的 format_remaining_secs
    {
        failures += Check(timeutil::FormatRemaining(45 * 60) == L"45m", L"45m");
        failures += Check(timeutil::FormatRemaining(4 * 3600 + 12 * 60) == L"4h12m", L"4h12m");
        failures += Check(timeutil::FormatRemaining(3 * 86400 + 2 * 3600) == L"3d2h", L"3d2h");
        failures += Check(timeutil::FormatRemaining(0) == L"0m", L"0m");
        failures += Check(timeutil::FormatTimeToReset(0, 1000) == L"?", L"重置时间未知显示 ?");
        failures += Check(timeutil::FormatTimeToReset(500, 1000) == L"now", L"已过期显示 now");
        failures += Check(timeutil::FormatTimeToReset(1000 + 6600, 1000) == L"1h50m", L"1h50m");
    }

    PrintLine(L"");
    if (failures == 0)
    {
        PrintLine(L"自检全部通过。");
        return 0;
    }
    wchar_t buffer[64]{};
    _snwprintf_s(buffer, _TRUNCATE, L"自检失败 %d 项。", failures);
    PrintLine(buffer);
    return 1;
}

int LiveFetch()
{
    PrintLine(L"凭据文件：" + usage::GetCredentialsPath());
    PrintLine(L"请求 https://api.anthropic.com/api/oauth/usage ...");
    PrintLine(L"");

    const usage::Result result = usage::Fetch();
    if (result.status != usage::Status::Ok)
    {
        PrintLine(L"取数失败：");
        PrintLine(result.message);
        return 1;
    }

    const time_t now = ::time(nullptr);
    PrintLine(FormatPeriod(L"5h    ", result.data.five_hour, now));
    PrintLine(FormatPeriod(L"7d    ", result.data.seven_day, now));
    PrintLine(FormatPeriod(L"7d Opus  ", result.data.seven_day_opus, now));
    PrintLine(FormatPeriod(L"7d Sonnet", result.data.seven_day_sonnet, now));

    if (result.data.extra.present)
    {
        wchar_t buffer[160]{};
        _snwprintf_s(buffer, _TRUNCATE,
                     L"额外用量: %s  %.1f%% ($%.0f / $%.0f)",
                     result.data.extra.enabled ? L"已开通" : L"未开通",
                     result.data.extra.utilization,
                     result.data.extra.used_credits,
                     result.data.extra.monthly_limit);
        PrintLine(buffer);
    }

    PrintLine(L"");
    PrintLine(L"任务栏上将显示：");
    wchar_t preview[128]{};
    _snwprintf_s(preview, _TRUNCATE, L"  5h  %.0f%% (%s)",
                 result.data.five_hour.utilization,
                 timeutil::FormatTimeToReset(result.data.five_hour.resets_at, now).c_str());
    PrintLine(preview);
    _snwprintf_s(preview, _TRUNCATE, L"  7d  %.0f%% (%s)",
                 result.data.seven_day.utilization,
                 timeutil::FormatTimeToReset(result.data.seven_day.resets_at, now).c_str());
    PrintLine(preview);
    return 0;
}

}   // namespace

int wmain(int argc, wchar_t* argv[])
{
    if (argc > 1 && (std::wstring(argv[1]) == L"--selftest" || std::wstring(argv[1]) == L"-s"))
        return SelfTest();
    return LiveFetch();
}
