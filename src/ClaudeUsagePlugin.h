/*
 * TrafficMonitor 插件：Claude 用量监控
 *
 * 提供两个显示项：
 *   5h -> Claude 5 小时窗口已用额度 + 距离重置的剩余时间
 *   7d -> Claude 7 天窗口已用额度   + 距离重置的剩余时间
 * 默认渲染成 "5h 2% (4h19m)"，格式对齐 cship，可在 ini 中改。
 *
 * 默认由插件自绘（IsCustomDraw 返回 true），以便：
 *   - 按阈值给文字上色（≥60% 转黄，≥80% 转红，与 cship 的阈值一致）
 *   - 在文字下方画一条表示额度占比的细条
 *   - 完全控制标签与数值之间的间距（主程序拼接时不留空格）
 */
#pragma once

// 官方接口头文件里的默认实现有大量未使用的形参，在 /W4 下会刷屏，
// 这里单独把 C4100 关掉，不影响本项目自己的代码。
#pragma warning(push)
#pragma warning(disable : 4100)
#include "../include/PluginInterface.h"
#pragma warning(pop)

#include "DisplayConfig.h"
#include "UsageApi.h"
#include "UsageService.h"

#include <string>

class CClaudeUsagePlugin;

/** 一个用量窗口对应的显示项 */
class CUsageItem : public IPluginItem
{
public:
    enum Kind
    {
        K_FIVE_HOUR,
        K_SEVEN_DAY,
    };

    CUsageItem(CClaudeUsagePlugin& owner, Kind kind) : m_owner(owner), m_kind(kind) {}

    const wchar_t* GetItemName() const override;
    const wchar_t* GetItemId() const override;
    const wchar_t* GetItemLableText() const override;
    const wchar_t* GetItemValueText() const override;
    const wchar_t* GetItemValueSampleText() const override;
    bool IsCustomDraw() const override;
    int GetItemWidth() const override;
    int GetItemWidthEx(void* hDC) const override;
    void DrawItem(void* hDC, int x, int y, int w, int h, bool dark_mode) override;
    int OnMouseEvent(MouseEventType type, int x, int y, void* hWnd, int flag) override;
    int IsDrawResourceUsageGraph() const override;
    float GetResourceUsageGraphValue() const override;

    /** 由插件在 DataRequired 中调用，刷新本项要显示的文本 */
    void Update(const usage::Snapshot& snapshot, time_t now);

private:
    const usage::Period* PickPeriod(const usage::Data& data) const;
    const usage::ItemDisplay& Layout() const;
    /** 数值部分的最坏情况展开，如 "100% (23h59m)"（非自绘时标签由主程序画） */
    std::wstring BuildValueSampleText() const;
    /** 含标签的完整最坏情况展开，如 "7d 100% (23h59m)"，用于自绘时算宽度 */
    std::wstring BuildFullSampleText() const;
    /** 根据阈值选择文字与进度条的颜色 */
    COLORREF PickBarColor(COLORREF base) const;

    CClaudeUsagePlugin& m_owner;
    Kind m_kind;

    std::wstring m_full_text{ L"--" };   /**< 自绘时画的完整文本 */
    std::wstring m_value_text{ L"--" };  /**< 非自绘时交给主程序的数值文本 */
    double m_percent{ -1.0 };            /**< 已用百分比；小于 0 表示无数据 */

    // 主程序可能在首次 DataRequired 之前就来问示例文本，因此这里按需重算，
    // 只是为了让返回的指针在调用之间保持有效才存成成员。
    mutable std::wstring m_sample_text;
};

/** 插件对象本身 */
class CClaudeUsagePlugin : public ITMPlugin
{
public:
    static CClaudeUsagePlugin& Instance();

    IPluginItem* GetItem(int index) override;
    void DataRequired() override;
    const wchar_t* GetInfo(PluginInfoIndex index) override;
    const wchar_t* GetTooltipInfo() override;
    void OnExtenedInfo(ExtendedInfoIndex index, const wchar_t* data) override;
    void OnInitialize(ITrafficMonitor* pApp) override;
    int GetCommandCount() override;
    const wchar_t* GetCommandName(int command_index) override;
    void OnPluginCommand(int command_index, void* hWnd, void* para) override;

    const usage::DisplayConfig& Display() const { return m_display; }

    /**
     * 自绘时使用的基准文字颜色。
     * 主程序会在调用 DrawItem 之前通过 EI_VALUE_TEXT_COLOR 传来当前皮肤的颜色，
     * 拿不到时按深浅色模式取黑白兜底。
     */
    COLORREF GetBaseTextColor(bool dark_mode) const;

private:
    CClaudeUsagePlugin() = default;

    void BuildTooltip(const usage::Snapshot& snapshot, time_t now);

    CUsageItem m_item_five_hour{ *this, CUsageItem::K_FIVE_HOUR };
    CUsageItem m_item_seven_day{ *this, CUsageItem::K_SEVEN_DAY };
    std::wstring m_tooltip;
    ITrafficMonitor* m_app{ nullptr };

    usage::DisplayConfig m_display;
    COLORREF m_value_text_color{ 0 };
    bool m_has_value_text_color{ false };
};

/** 判断界面语言是否为中文（供各处选择显示文本） */
bool PluginUseChinese();

#ifdef __cplusplus
extern "C" {
#endif
__declspec(dllexport) ITMPlugin* TMPluginGetInstance();
#ifdef __cplusplus
}
#endif
