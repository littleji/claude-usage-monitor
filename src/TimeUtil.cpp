#include "TimeUtil.h"

#include <cstdio>
#include <cstdlib>

namespace timeutil {
namespace {

bool ReadInt(const char*& p, const char* end, int digits, int& out)
{
    if (end - p < digits)
        return false;
    int value = 0;
    for (int i = 0; i < digits; ++i)
    {
        if (p[i] < '0' || p[i] > '9')
            return false;
        value = value * 10 + (p[i] - '0');
    }
    p += digits;
    out = value;
    return true;
}

}   // namespace

bool Iso8601ToEpoch(const std::string& iso, time_t& out_epoch)
{
    if (iso.empty())
        return false;

    const char* p = iso.c_str();
    const char* end = p + iso.size();

    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    if (!ReadInt(p, end, 4, year)) return false;
    if (p >= end || *p++ != '-') return false;
    if (!ReadInt(p, end, 2, month)) return false;
    if (p >= end || *p++ != '-') return false;
    if (!ReadInt(p, end, 2, day)) return false;
    if (p >= end || (*p != 'T' && *p != 't' && *p != ' ')) return false;
    ++p;
    if (!ReadInt(p, end, 2, hour)) return false;
    if (p >= end || *p++ != ':') return false;
    if (!ReadInt(p, end, 2, minute)) return false;
    if (p < end && *p == ':')
    {
        ++p;
        if (!ReadInt(p, end, 2, second)) return false;
    }

    // 小数秒直接丢弃
    if (p < end && *p == '.')
    {
        ++p;
        while (p < end && *p >= '0' && *p <= '9')
            ++p;
    }

    // 时区偏移：缺省、'Z' 均视为 UTC
    int offset_seconds = 0;
    if (p < end && (*p == '+' || *p == '-'))
    {
        const int sign = (*p == '-') ? -1 : 1;
        ++p;
        int off_hour = 0, off_minute = 0;
        if (!ReadInt(p, end, 2, off_hour)) return false;
        if (p < end && *p == ':')
            ++p;
        if (p < end)
        {
            if (!ReadInt(p, end, 2, off_minute)) return false;
        }
        offset_seconds = sign * (off_hour * 3600 + off_minute * 60);
    }

    std::tm tm_utc{};
    tm_utc.tm_year = year - 1900;
    tm_utc.tm_mon = month - 1;
    tm_utc.tm_mday = day;
    tm_utc.tm_hour = hour;
    tm_utc.tm_min = minute;
    tm_utc.tm_sec = second;
    tm_utc.tm_isdst = 0;

    const time_t as_utc = _mkgmtime(&tm_utc);
    if (as_utc == static_cast<time_t>(-1))
        return false;

    out_epoch = as_utc - offset_seconds;
    return true;
}

std::wstring FormatRemaining(long long seconds)
{
    if (seconds < 0)
        seconds = 0;
    const long long minutes = seconds / 60;
    const long long hours = minutes / 60;
    const long long days = hours / 24;

    wchar_t buffer[32]{};
    if (days > 0)
        _snwprintf_s(buffer, _TRUNCATE, L"%lldd%lldh", days, hours % 24);
    else if (hours > 0)
        _snwprintf_s(buffer, _TRUNCATE, L"%lldh%lldm", hours, minutes % 60);
    else
        _snwprintf_s(buffer, _TRUNCATE, L"%lldm", minutes);
    return buffer;
}

std::wstring FormatTimeToReset(time_t reset_epoch, time_t now)
{
    if (reset_epoch == 0)
        return L"?";
    if (now >= reset_epoch)
        return L"now";
    return FormatRemaining(static_cast<long long>(reset_epoch - now));
}

std::wstring FormatResetAt(time_t reset_epoch, time_t now)
{
    if (reset_epoch == 0)
        return L"?";

    std::tm reset_tm{};
    std::tm now_tm{};
    if (localtime_s(&reset_tm, &reset_epoch) != 0)
        return L"?";
    if (localtime_s(&now_tm, &now) != 0)
        return L"?";

    wchar_t buffer[32]{};
    const bool same_day = (reset_tm.tm_year == now_tm.tm_year &&
                           reset_tm.tm_yday == now_tm.tm_yday);
    if (same_day)
        _snwprintf_s(buffer, _TRUNCATE, L"%02d:%02d", reset_tm.tm_hour, reset_tm.tm_min);
    else
        _snwprintf_s(buffer, _TRUNCATE, L"%02d-%02d %02d:%02d",
                     reset_tm.tm_mon + 1, reset_tm.tm_mday, reset_tm.tm_hour, reset_tm.tm_min);
    return buffer;
}

std::wstring FormatClock(time_t epoch)
{
    if (epoch == 0)
        return L"--:--:--";
    std::tm local_tm{};
    if (localtime_s(&local_tm, &epoch) != 0)
        return L"--:--:--";
    wchar_t buffer[32]{};
    _snwprintf_s(buffer, _TRUNCATE, L"%02d:%02d:%02d",
                 local_tm.tm_hour, local_tm.tm_min, local_tm.tm_sec);
    return buffer;
}

}   // namespace timeutil
