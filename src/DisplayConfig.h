/*
 * 显示相关的配置，从 <config_dir>\ClaudeUsage.ini 的 [display] 节读取。
 *
 * 格式串的写法参考 cship 的 five_hour_format / seven_day_format，
 * 支持四个占位符：
 *   {pct}       已用百分比取整，如 "2"
 *   {remaining} 剩余百分比取整，如 "98"
 *   {reset}     距离重置的剩余时间，如 "4h19m"
 *   {reset_at}  重置的本地时刻，如 "19:42" 或 "08-19 09:00"
 *
 * 默认渲染出 "5h 2% (4h19m)"。
 */
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <string>

namespace usage {

/** 单个显示项的文本组成：标签 + 数值格式 */
struct ItemDisplay
{
    std::wstring label;
    std::wstring format;
};

struct DisplayConfig
{
    /** 是否由插件自绘（自绘才有阈值颜色和进度条） */
    bool custom_draw{ true };
    /** 是否在文字下方画一条表示额度占比的细条 */
    bool show_bar{ true };

    /** 超过该百分比后进度条转为警告色 */
    double warn_threshold{ 60.0 };
    /** 超过该百分比后转为危险色 */
    double critical_threshold{ 80.0 };

    /**
     * 是否按阈值给进度条上色。文字颜色始终保持 base 色不变——
     * 白底配红字模糊看不清，只有进度条（绿/黄/红）随用量变化。
     * 关掉后进度条退回 base 色，效果等同旧版本未上色时的样子。
     */
    bool bar_color_enabled{ true };

    // 默认取 cship 用的配色
    COLORREF normal_color{ RGB(0x9E, 0xCE, 0x6A) };
    COLORREF warn_color{ RGB(0xE0, 0xAF, 0x68) };
    COLORREF critical_color{ RGB(0xF7, 0x76, 0x8E) };

    ItemDisplay five_hour{ L"5h", L"{pct}% ({reset})" };
    ItemDisplay seven_day{ L"7d", L"{pct}% ({reset})" };

    /** 读取配置；文件或键不存在时保持默认值，并把生效值写回文件 */
    void Load(const std::wstring& config_dir);
};

/** 用实际数值替换格式串中的占位符 */
std::wstring ApplyFormat(const std::wstring& format, double used_percent,
                         const std::wstring& reset, const std::wstring& reset_at);

}   // namespace usage
