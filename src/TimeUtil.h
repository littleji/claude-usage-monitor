/*
 * 时间解析与格式化。
 *
 * 剩余时间的格式严格对齐 cship 的 format_remaining_secs：
 *   < 1 小时  -> "45m"
 *   < 1 天    -> "4h12m"
 *   >= 1 天   -> "3d2h"
 * 重置时刻未知显示 "?"，已经过去显示 "now"。
 */
#pragma once

#include <ctime>
#include <string>

namespace timeutil {

/**
 * 解析 ISO 8601 时间串（如 "2026-08-17T09:30:00+00:00" / "...Z" / 带小数秒）。
 * 成功返回 true 并写出 Unix 纪元秒（UTC）。
 */
bool Iso8601ToEpoch(const std::string& iso, time_t& out_epoch);

/** 把剩余秒数格式化为 "45m" / "4h12m" / "3d2h" */
std::wstring FormatRemaining(long long seconds);

/**
 * 格式化距离 reset_epoch 还有多久。
 * reset_epoch 为 0（未知）返回 L"?"，已过期返回 L"now"。
 */
std::wstring FormatTimeToReset(time_t reset_epoch, time_t now);

/**
 * 格式化重置的本地时刻：当天显示 "19:42"，跨天显示 "08-19 09:00"。
 * reset_epoch 为 0 返回 L"?"。
 */
std::wstring FormatResetAt(time_t reset_epoch, time_t now);

/** 格式化本地时刻为 "14:03:22" */
std::wstring FormatClock(time_t epoch);

}   // namespace timeutil
