#include "DisplayConfig.h"

#include <algorithm>
#include <cstdio>

namespace usage {
namespace {

const wchar_t* const kSection = L"display";

std::wstring ReadString(const std::wstring& path, const wchar_t* key,
                        const std::wstring& fallback)
{
    wchar_t buffer[256]{};
    const DWORD length = ::GetPrivateProfileStringW(kSection, key, fallback.c_str(),
                                                    buffer, ARRAYSIZE(buffer), path.c_str());
    if (length == 0)
        return fallback;
    return std::wstring(buffer, length);
}

void WriteString(const std::wstring& path, const wchar_t* key, const std::wstring& value)
{
    ::WritePrivateProfileStringW(kSection, key, value.c_str(), path.c_str());
}

void WriteInt(const std::wstring& path, const wchar_t* key, int value)
{
    wchar_t buffer[16]{};
    _snwprintf_s(buffer, _TRUNCATE, L"%d", value);
    ::WritePrivateProfileStringW(kSection, key, buffer, path.c_str());
}

/** 把 "E0AF68" / "#E0AF68" 解析成 COLORREF；失败时返回 fallback */
COLORREF ParseColor(const std::wstring& text, COLORREF fallback)
{
    std::wstring hex = text;
    if (!hex.empty() && hex.front() == L'#')
        hex.erase(0, 1);
    if (hex.size() != 6)
        return fallback;

    wchar_t* end = nullptr;
    const unsigned long value = ::wcstoul(hex.c_str(), &end, 16);
    if (end == nullptr || *end != L'\0')
        return fallback;

    // 配置里写的是 RRGGBB，COLORREF 是 0x00BBGGRR，需要换个字节序
    const BYTE r = static_cast<BYTE>((value >> 16) & 0xFF);
    const BYTE g = static_cast<BYTE>((value >> 8) & 0xFF);
    const BYTE b = static_cast<BYTE>(value & 0xFF);
    return RGB(r, g, b);
}

std::wstring ColorToString(COLORREF color)
{
    wchar_t buffer[16]{};
    _snwprintf_s(buffer, _TRUNCATE, L"%02X%02X%02X",
                 GetRValue(color), GetGValue(color), GetBValue(color));
    return buffer;
}

void ReplaceAll(std::wstring& text, const wchar_t* token, const std::wstring& value)
{
    const size_t token_length = ::wcslen(token);
    size_t pos = text.find(token);
    while (pos != std::wstring::npos)
    {
        text.replace(pos, token_length, value);
        pos = text.find(token, pos + value.size());
    }
}

std::wstring FormatIntegerPercent(double value)
{
    if (value < 0.0)
        value = 0.0;
    wchar_t buffer[16]{};
    _snwprintf_s(buffer, _TRUNCATE, L"%.0f", value);
    return buffer;
}

}   // namespace

std::wstring ApplyFormat(const std::wstring& format, double used_percent,
                         const std::wstring& reset, const std::wstring& reset_at)
{
    std::wstring text = format;
    ReplaceAll(text, L"{pct}", FormatIntegerPercent(used_percent));
    ReplaceAll(text, L"{remaining}", FormatIntegerPercent(100.0 - used_percent));
    ReplaceAll(text, L"{reset}", reset);
    ReplaceAll(text, L"{reset_at}", reset_at);
    return text;
}

void DisplayConfig::Load(const std::wstring& config_dir)
{
    if (config_dir.empty())
        return;

    std::wstring path = config_dir;
    if (path.back() != L'\\' && path.back() != L'/')
        path += L'\\';
    path += L"ClaudeUsage.ini";

    custom_draw = ::GetPrivateProfileIntW(kSection, L"custom_draw", custom_draw ? 1 : 0,
                                          path.c_str()) != 0;
    show_bar = ::GetPrivateProfileIntW(kSection, L"show_bar", show_bar ? 1 : 0,
                                       path.c_str()) != 0;

    const int warn = ::GetPrivateProfileIntW(kSection, L"warn_threshold",
                                             static_cast<int>(warn_threshold), path.c_str());
    const int critical = ::GetPrivateProfileIntW(kSection, L"critical_threshold",
                                                 static_cast<int>(critical_threshold), path.c_str());
    warn_threshold = std::max(0, std::min(100, warn));
    critical_threshold = std::max(0, std::min(100, critical));

    bar_color_enabled = ::GetPrivateProfileIntW(kSection, L"bar_color_enabled",
                                                bar_color_enabled ? 1 : 0, path.c_str()) != 0;

    normal_color = ParseColor(ReadString(path, L"normal_color", ColorToString(normal_color)),
                              normal_color);
    warn_color = ParseColor(ReadString(path, L"warn_color", ColorToString(warn_color)), warn_color);
    critical_color = ParseColor(ReadString(path, L"critical_color", ColorToString(critical_color)),
                                critical_color);

    five_hour.label = ReadString(path, L"five_hour_label", five_hour.label);
    five_hour.format = ReadString(path, L"five_hour_format", five_hour.format);
    seven_day.label = ReadString(path, L"seven_day_label", seven_day.label);
    seven_day.format = ReadString(path, L"seven_day_format", seven_day.format);

    const int stale = ::GetPrivateProfileIntW(kSection, L"terminal_stale_minutes",
                                              terminal_stale_minutes, path.c_str());
    terminal_stale_minutes = std::max(1, stale);
    const int max_icons = ::GetPrivateProfileIntW(kSection, L"terminal_max_icons",
                                                  terminal_max_icons, path.c_str());
    terminal_max_icons = std::max(1, std::min(64, max_icons));

    terminal_thinking_color = ParseColor(
        ReadString(path, L"terminal_thinking_color", ColorToString(terminal_thinking_color)),
        terminal_thinking_color);
    terminal_idle_color = ParseColor(
        ReadString(path, L"terminal_idle_color", ColorToString(terminal_idle_color)),
        terminal_idle_color);

    // 把生效的配置写回，让用户看得到有哪些可调项
    WriteInt(path, L"custom_draw", custom_draw ? 1 : 0);
    WriteInt(path, L"show_bar", show_bar ? 1 : 0);
    WriteInt(path, L"warn_threshold", static_cast<int>(warn_threshold));
    WriteInt(path, L"critical_threshold", static_cast<int>(critical_threshold));
    WriteInt(path, L"bar_color_enabled", bar_color_enabled ? 1 : 0);
    WriteString(path, L"normal_color", ColorToString(normal_color));
    WriteString(path, L"warn_color", ColorToString(warn_color));
    WriteString(path, L"critical_color", ColorToString(critical_color));
    WriteString(path, L"five_hour_label", five_hour.label);
    WriteString(path, L"five_hour_format", five_hour.format);
    WriteString(path, L"seven_day_label", seven_day.label);
    WriteString(path, L"seven_day_format", seven_day.format);
    WriteInt(path, L"terminal_stale_minutes", terminal_stale_minutes);
    WriteInt(path, L"terminal_max_icons", terminal_max_icons);
    WriteString(path, L"terminal_thinking_color", ColorToString(terminal_thinking_color));
    WriteString(path, L"terminal_idle_color", ColorToString(terminal_idle_color));
}

}   // namespace usage
