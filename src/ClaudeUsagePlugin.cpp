#include "ClaudeUsagePlugin.h"

#include "TimeUtil.h"
#include "UsageService.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>

namespace {

// -1 = 尚未确定，0 = 非中文，1 = 中文
volatile LONG g_use_chinese = -1;

const wchar_t* Pick(const wchar_t* zh, const wchar_t* en)
{
    return PluginUseChinese() ? zh : en;
}

/** 把百分比按 cship 的方式取整成字符串 */
std::wstring FormatPercent(double utilization)
{
    if (utilization < 0.0)
        utilization = 0.0;
    wchar_t buffer[16]{};
    _snwprintf_s(buffer, _TRUNCATE, L"%.0f%%", utilization);
    return buffer;
}

/** 用纯色填充一块矩形 */
void FillSolid(HDC dc, const RECT& rect, COLORREF color)
{
    if (rect.right <= rect.left || rect.bottom <= rect.top)
        return;
    HBRUSH brush = ::CreateSolidBrush(color);
    if (brush == nullptr)
        return;
    ::FillRect(dc, &rect, brush);
    ::DeleteObject(brush);
}

/**
 * 把颜色向背景方向压暗，用作进度条的底槽。
 * 深色背景往黑压，浅色背景往白提，这样底槽在两种主题下都只是"淡一档"。
 */
COLORREF TowardBackground(COLORREF color, bool dark_mode)
{
    const double keep = 0.30;
    auto mix = [&](BYTE channel) -> BYTE
    {
        const double target = dark_mode ? 0.0 : 255.0;
        return static_cast<BYTE>(channel * keep + target * (1.0 - keep));
    };
    return RGB(mix(GetRValue(color)), mix(GetGValue(color)), mix(GetBValue(color)));
}

}   // namespace

bool PluginUseChinese()
{
    LONG cached = ::InterlockedCompareExchange(&g_use_chinese, -1, -1);
    if (cached < 0)
    {
        const LANGID lang = ::GetUserDefaultUILanguage();
        cached = (PRIMARYLANGID(lang) == LANG_CHINESE) ? 1 : 0;
        ::InterlockedExchange(&g_use_chinese, cached);
    }
    return cached != 0;
}

//////////////////////////////////////////////////////////////////////////
// CUsageItem

const wchar_t* CUsageItem::GetItemName() const
{
    if (m_kind == K_FIVE_HOUR)
        return Pick(L"Claude 5小时用量", L"Claude 5h Usage");
    return Pick(L"Claude 7天用量", L"Claude 7d Usage");
}

const wchar_t* CUsageItem::GetItemId() const
{
    // 唯一 ID，只能包含字母和数字。刻意与其他 Claude 插件区分开，
    // 这样本插件可以和已安装的插件共存。
    return m_kind == K_FIVE_HOUR ? L"ClaudeUsageOAuth5h" : L"ClaudeUsageOAuth7d";
}

const usage::ItemDisplay& CUsageItem::Layout() const
{
    const usage::DisplayConfig& cfg = m_owner.Display();
    return m_kind == K_FIVE_HOUR ? cfg.five_hour : cfg.seven_day;
}

const wchar_t* CUsageItem::GetItemLableText() const
{
    return Layout().label.c_str();
}

const wchar_t* CUsageItem::GetItemValueText() const
{
    return m_value_text.c_str();
}

const wchar_t* CUsageItem::GetItemValueSampleText() const
{
    // 非自绘时标签由主程序单独绘制，这里只给数值部分的宽度基准
    m_sample_text = BuildValueSampleText();
    return m_sample_text.c_str();
}

bool CUsageItem::IsCustomDraw() const
{
    return m_owner.Display().custom_draw;
}

std::wstring CUsageItem::BuildValueSampleText() const
{
    // 最坏情况：三位数百分比、最长的剩余时间串、最长的绝对时刻串
    return usage::ApplyFormat(Layout().format, 100.0, L"23h59m", L"88-88 88:88");
}

std::wstring CUsageItem::BuildFullSampleText() const
{
    const std::wstring& label = Layout().label;
    const std::wstring value = BuildValueSampleText();
    return label.empty() ? value : label + L" " + value;
}

int CUsageItem::GetItemWidth() const
{
    // 只在 GetItemWidthEx 返回 0 时才会用到，按 96 DPI 给一个够用的估值
    return 100;
}

int CUsageItem::GetItemWidthEx(void* hDC) const
{
    HDC dc = static_cast<HDC>(hDC);
    if (dc == nullptr)
        return 0;   // 返回 0 让主程序退回到 GetItemWidth

    // 主程序自己也用 DrawText(DT_CALCRECT) 测宽度，GetTextExtent 得到的是理论宽度、不够准
    const std::wstring sample = BuildFullSampleText();
    RECT measured{ 0, 0, 0, 0 };
    ::DrawTextW(dc, sample.c_str(), -1, &measured,
                DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);

    // 右侧留一点呼吸空间，避免和相邻显示项贴死。用字高推算，天然跟随 DPI。
    TEXTMETRICW metrics{};
    int padding = 4;
    if (::GetTextMetricsW(dc, &metrics))
        padding = std::max(4, static_cast<int>(metrics.tmHeight / 3));

    return (measured.right - measured.left) + padding;
}

COLORREF CUsageItem::PickBarColor(COLORREF base) const
{
    if (m_percent < 0.0)
        return base;   // 无数据时不做颜色提示
    const usage::DisplayConfig& cfg = m_owner.Display();
    if (!cfg.bar_color_enabled)
        return base;   // 用户关掉了进度条配色，退回 base 色
    if (m_percent >= cfg.critical_threshold)
        return cfg.critical_color;
    if (m_percent >= cfg.warn_threshold)
        return cfg.warn_color;
    return cfg.normal_color;
}

void CUsageItem::DrawItem(void* hDC, int x, int y, int w, int h, bool dark_mode)
{
    HDC dc = static_cast<HDC>(hDC);
    if (dc == nullptr || w <= 0 || h <= 0)
        return;

    const usage::DisplayConfig& cfg = m_owner.Display();
    const COLORREF base = m_owner.GetBaseTextColor(dark_mode);
    // 文字颜色固定用 base，不随阈值变化：白底配红/黄字太糊，看不清具体数值。
    // 阈值配色只体现在下面的进度条上（绿/黄/红）。
    const COLORREF bar_color = PickBarColor(base);

    const int saved_dc = ::SaveDC(dc);

    RECT text_rect{ x, y, x + w, y + h };

    // 底部细条：高度取字高的十分之一左右，太矮的显示区域就不画，优先保证文字可读
    int bar_height = 0;
    if (cfg.show_bar && m_percent >= 0.0)
    {
        const int wanted = std::max(2, h / 12);
        if (h - wanted - 1 >= 9)
        {
            bar_height = wanted;
            text_rect.bottom -= bar_height + 1;
        }
    }

    ::SetBkMode(dc, TRANSPARENT);
    ::SetTextColor(dc, base);
    // 必须用 DrawTextW：主程序会替换插件导入表中 user32 的 DrawText 系列函数，
    // 任务栏的 Direct2D 渲染依赖这个拦截点，改用 TextOut 会画不出来。
    ::DrawTextW(dc, m_full_text.c_str(), -1, &text_rect,
                DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX | DT_END_ELLIPSIS);

    if (bar_height > 0)
    {
        const RECT track{ x, y + h - bar_height, x + w, y + h };
        FillSolid(dc, track, TowardBackground(base, dark_mode));

        const double ratio = std::min(1.0, std::max(0.0, m_percent / 100.0));
        const int filled = static_cast<int>(w * ratio + 0.5);
        if (filled > 0)
        {
            const RECT fill{ x, track.top, x + filled, track.bottom };
            FillSolid(dc, fill, bar_color);
        }
    }

    ::RestoreDC(dc, saved_dc);
}

int CUsageItem::OnMouseEvent(MouseEventType type, int /*x*/, int /*y*/,
                             void* /*hWnd*/, int /*flag*/)
{
    if (type == MT_DBCLICKED)
    {
        usage::Service::Instance().RequestRefresh();
        return 1;
    }
    return 0;   // 其余事件交回主程序（右键仍然弹出主程序菜单）
}

int CUsageItem::IsDrawResourceUsageGraph() const
{
    return m_percent >= 0.0 ? 1 : 0;
}

float CUsageItem::GetResourceUsageGraphValue() const
{
    if (m_percent < 0.0)
        return 0.0f;
    return static_cast<float>(std::min(1.0, std::max(0.0, m_percent / 100.0)));
}

const usage::Period* CUsageItem::PickPeriod(const usage::Data& data) const
{
    return m_kind == K_FIVE_HOUR ? &data.five_hour : &data.seven_day;
}

void CUsageItem::Update(const usage::Snapshot& snapshot, time_t now)
{
    const usage::ItemDisplay& layout = Layout();

    if (!snapshot.has_data)
    {
        m_value_text = L"--";
        m_percent = -1.0;
    }
    else
    {
        const usage::Period* period = PickPeriod(snapshot.data);
        if (!period->present)
        {
            // Enterprise 等账号类型可能不返回标准窗口
            m_value_text = L"N/A";
            m_percent = -1.0;
        }
        else
        {
            m_percent = period->utilization;
            m_value_text = usage::ApplyFormat(
                layout.format, period->utilization,
                timeutil::FormatTimeToReset(period->resets_at, now),
                timeutil::FormatResetAt(period->resets_at, now));
        }
    }

    m_full_text = layout.label.empty() ? m_value_text : layout.label + L" " + m_value_text;
}

//////////////////////////////////////////////////////////////////////////
// CClaudeUsagePlugin

CClaudeUsagePlugin& CClaudeUsagePlugin::Instance()
{
    static CClaudeUsagePlugin instance;
    return instance;
}

COLORREF CClaudeUsagePlugin::GetBaseTextColor(bool dark_mode) const
{
    if (m_has_value_text_color)
        return m_value_text_color;
    return dark_mode ? RGB(255, 255, 255) : RGB(0, 0, 0);
}

IPluginItem* CClaudeUsagePlugin::GetItem(int index)
{
    switch (index)
    {
    case 0:
        return &m_item_five_hour;
    case 1:
        return &m_item_seven_day;
    default:
        return nullptr;
    }
}

void CClaudeUsagePlugin::DataRequired()
{
    // 主程序的界面线程会频繁调用这里，绝不能做网络请求，
    // 只从后台线程维护的快照里取数并重新格式化倒计时。
    usage::Service::Instance().Start();

    const usage::Snapshot snapshot = usage::Service::Instance().GetSnapshot();
    const time_t now = ::time(nullptr);

    m_item_five_hour.Update(snapshot, now);
    m_item_seven_day.Update(snapshot, now);
    BuildTooltip(snapshot, now);
}

void CClaudeUsagePlugin::BuildTooltip(const usage::Snapshot& snapshot, time_t now)
{
    const bool zh = PluginUseChinese();
    std::wstring text;

    auto append_period = [&](const wchar_t* label, const usage::Period& period)
    {
        if (!period.present)
            return;
        text += label;
        text += FormatPercent(period.utilization);
        if (period.resets_at != 0)
        {
            text += zh ? L"，" : L", ";
            text += zh ? L"重置于 " : L"resets at ";
            text += timeutil::FormatResetAt(period.resets_at, now);
            text += L" (";
            text += timeutil::FormatTimeToReset(period.resets_at, now);
            text += L")";
        }
        text += L"\n";
    };

    if (snapshot.has_data)
    {
        append_period(zh ? L"5 小时窗口：" : L"5-hour window: ", snapshot.data.five_hour);
        append_period(zh ? L"7 天窗口：" : L"7-day window: ", snapshot.data.seven_day);
        append_period(zh ? L"  Opus 7 天：" : L"  Opus 7d: ", snapshot.data.seven_day_opus);
        append_period(zh ? L"  Sonnet 7 天：" : L"  Sonnet 7d: ", snapshot.data.seven_day_sonnet);

        if (snapshot.data.extra.present && snapshot.data.extra.enabled)
        {
            wchar_t buffer[128]{};
            _snwprintf_s(buffer, _TRUNCATE, L"%s%.0f%% ($%.0f / $%.0f)\n",
                         zh ? L"额外用量：" : L"Extra usage: ",
                         snapshot.data.extra.utilization,
                         snapshot.data.extra.used_credits,
                         snapshot.data.extra.monthly_limit);
            text += buffer;
        }

        text += zh ? L"更新于 " : L"Updated ";
        text += timeutil::FormatClock(snapshot.updated_at);
    }
    else
    {
        text += zh ? L"尚未取得 Claude 用量数据" : L"No Claude usage data yet";
    }

    if (!snapshot.last_error.empty())
    {
        text += L"\n";
        text += zh ? L"最近一次取数失败：" : L"Last fetch failed: ";
        text += L"\n";
        text += snapshot.last_error;
    }

    text += L"\n";
    text += zh ? L"（双击立即刷新）" : L"(double-click to refresh now)";

    m_tooltip.swap(text);
}

const wchar_t* CClaudeUsagePlugin::GetInfo(PluginInfoIndex index)
{
    switch (index)
    {
    case TMI_NAME:
        return Pick(L"Claude 用量监控", L"Claude Usage Monitor");
    case TMI_DESCRIPTION:
        return Pick(L"显示 Claude 5 小时 / 7 天窗口的已用额度与剩余重置时间。"
                    L"数据取自 Claude Code 的本地登录凭据（与 cship 相同的方式），无需额外登录。",
                    L"Shows Claude 5-hour / 7-day usage and time until reset. "
                    L"Reads Claude Code's local credentials, the same way cship does.");
    case TMI_AUTHOR:
        return L"littleji";
    case TMI_COPYRIGHT:
        return L"MIT License";
    case TMI_VERSION:
        return L"0.1.0";
    case TMI_URL:
        // 本项目的仓库地址。cship 只是取数方式的参考，它的地址写在 UsageApi.h 的注释里
        return L"https://github.com/littleji/claude-usage-monitor";
    default:
        return L"";
    }
}

const wchar_t* CClaudeUsagePlugin::GetTooltipInfo()
{
    return m_tooltip.c_str();
}

void CClaudeUsagePlugin::OnExtenedInfo(ExtendedInfoIndex index, const wchar_t* data)
{
    if (data == nullptr)
        return;

    switch (index)
    {
    case EI_CONFIG_DIR:
        usage::Service::Instance().LoadConfig(std::wstring(data));
        m_display.Load(std::wstring(data));
        break;

    case EI_VALUE_TEXT_COLOR:
        // 主程序传的是 std::to_wstring(COLORREF)，即十进制的 0x00BBGGRR
        m_value_text_color = static_cast<COLORREF>(::wcstoul(data, nullptr, 10));
        m_has_value_text_color = true;
        break;

    default:
        break;
    }
}

void CClaudeUsagePlugin::OnInitialize(ITrafficMonitor* pApp)
{
    m_app = pApp;
    if (pApp != nullptr && pApp->GetAPIVersion() >= 1)
    {
        const LANGID lang = static_cast<LANGID>(pApp->GetLanguageId());
        if (lang != 0)
            ::InterlockedExchange(&g_use_chinese, PRIMARYLANGID(lang) == LANG_CHINESE ? 1 : 0);
    }
    usage::Service::Instance().Start();
}

int CClaudeUsagePlugin::GetCommandCount()
{
    return 1;
}

const wchar_t* CClaudeUsagePlugin::GetCommandName(int command_index)
{
    if (command_index == 0)
        return Pick(L"立即刷新用量", L"Refresh usage now");
    return nullptr;
}

void CClaudeUsagePlugin::OnPluginCommand(int command_index, void* /*hWnd*/, void* /*para*/)
{
    if (command_index == 0)
        usage::Service::Instance().RequestRefresh();
}

//////////////////////////////////////////////////////////////////////////

ITMPlugin* TMPluginGetInstance()
{
    return &CClaudeUsagePlugin::Instance();
}
