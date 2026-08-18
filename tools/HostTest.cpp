/*
 * 宿主测试：用和 TrafficMonitor 完全相同的方式加载并驱动插件 DLL。
 *
 * 本程序不链接插件的任何源码，只通过 LoadLibrary + GetProcAddress 调用，
 * 因此能真实地验证导出函数、虚表布局和整个调用生命周期。
 *
 *   HostTest.exe <插件DLL路径>
 */
#pragma warning(push)
#pragma warning(disable : 4100)
#include "../include/PluginInterface.h"
#pragma warning(pop)

#include <windows.h>

#include <cstdio>
#include <string>

namespace {

int g_failures = 0;

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

void Line(const std::wstring& text) { PrintW(text + L"\n"); }

void Check(bool condition, const std::wstring& name)
{
    Line((condition ? L"  [ok]   " : L"  [FAIL] ") + name);
    if (!condition)
        ++g_failures;
}

/** 一个最小的主程序接口实现，模拟 TrafficMonitor 传给插件的对象 */
class StubApp : public ITrafficMonitor
{
public:
    int GetAPIVersion() override { return 7; }
    const wchar_t* GetVersion() override { return L"1.85.0"; }
    double GetMonitorValue(MonitorItem) override { return 0.0; }
    const wchar_t* GetMonitorValueString(MonitorItem, int) override { return L"0"; }
    void ShowNotifyMessage(const wchar_t* msg) override
    {
        Line(std::wstring(L"  [通知] ") + (msg != nullptr ? msg : L""));
    }
    unsigned short GetLanguageId() const override { return 0x0804; }   // 简体中文
    const wchar_t* GetPluginConfigDir() const override { return m_config_dir.c_str(); }
    int GetDPI(DPIType) const override { return 96; }
    unsigned int GetThemeColor() const override { return 0x00D77800; }

    std::wstring m_config_dir;
};

std::wstring MakeTempConfigDir()
{
    wchar_t temp[MAX_PATH]{};
    ::GetTempPathW(MAX_PATH, temp);
    std::wstring dir = std::wstring(temp) + L"ClaudeUsageMonitorTest";
    ::CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

const wchar_t* Safe(const wchar_t* s) { return s != nullptr ? s : L"<nullptr>"; }

/**
 * 把显示项画到一张内存位图上，验证自绘路径不崩溃、宽度合理、
 * 并且确实往像素上写了东西（背景被涂成洋红，数出非洋红的像素）。
 */
void ExerciseCustomDraw(ITMPlugin* plugin, IPluginItem* item, const wchar_t* label,
                        bool dark_mode, COLORREF expected_accent = 0, bool expect_bar = false)
{
    if (item == nullptr || !item->IsCustomDraw())
        return;

    HDC screen = ::GetDC(nullptr);
    HDC mem = ::CreateCompatibleDC(screen);

    // 用主程序在任务栏上用的那种字体去测量与绘制
    LOGFONTW lf{};
    lf.lfHeight = -16;
    lf.lfWeight = FW_NORMAL;
    lf.lfCharSet = DEFAULT_CHARSET;
    ::wcscpy_s(lf.lfFaceName, L"Microsoft YaHei UI");
    HFONT font = ::CreateFontIndirectW(&lf);
    HGDIOBJ old_font = ::SelectObject(mem, font);

    const int width = item->GetItemWidthEx(mem);
    Line(std::wstring(L"    ") + label + L" GetItemWidthEx = " + std::to_wstring(width) + L" px");
    Check(width > 0, std::wstring(label) + L" 自绘宽度大于 0");
    Check(width < 2000, std::wstring(label) + L" 自绘宽度在合理范围内");
    Check(item->GetItemWidth() > 0, std::wstring(label) + L" GetItemWidth 兜底值大于 0");
    Check(item->GetItemWidthEx(nullptr) == 0,
          std::wstring(label) + L" hDC 为空时返回 0（让主程序回退）");

    const int height = 24;
    const int canvas_w = (width > 0 ? width : 100) + 8;
    HBITMAP bitmap = ::CreateCompatibleBitmap(screen, canvas_w, height);
    HGDIOBJ old_bitmap = ::SelectObject(mem, bitmap);

    // 洋红背景：任何绘制都会覆盖掉它
    const COLORREF kBackground = RGB(255, 0, 255);
    RECT all{ 0, 0, canvas_w, height };
    HBRUSH bg = ::CreateSolidBrush(kBackground);
    ::FillRect(mem, &all, bg);
    ::DeleteObject(bg);

    // 主程序在调用 DrawItem 之前会设置文字颜色，这里照做
    ::SetTextColor(mem, dark_mode ? RGB(255, 255, 255) : RGB(0, 0, 0));
    item->DrawItem(mem, 0, 0, canvas_w, height, dark_mode);

    int painted = 0;
    int accent = 0;
    int bottom_row_painted = 0;
    for (int py = 0; py < height; ++py)
    {
        for (int px = 0; px < canvas_w; ++px)
        {
            const COLORREF pixel = ::GetPixel(mem, px, py);
            if (pixel == kBackground)
                continue;
            ++painted;
            if (expected_accent != 0 && pixel == expected_accent)
                ++accent;
            if (py == height - 1)
                ++bottom_row_painted;
        }
    }
    Line(std::wstring(L"    ") + label + L" 绘制像素数 = " + std::to_wstring(painted) +
         L" / " + std::to_wstring(canvas_w * height));
    Check(painted > 0, std::wstring(label) + L" DrawItem 确实画了内容");

    if (expect_bar)
    {
        Line(std::wstring(L"    ") + label + L" 底部一行绘制像素数 = " +
             std::to_wstring(bottom_row_painted) + L" / " + std::to_wstring(canvas_w));
        // 进度条铺满整行（底槽 + 填充），所以底部一行应当被完全覆盖
        Check(bottom_row_painted == canvas_w, std::wstring(label) + L" 底部进度条铺满整行");
    }
    if (expected_accent != 0)
    {
        Line(std::wstring(L"    ") + label + L" 阈值色像素数 = " + std::to_wstring(accent));
        Check(accent > 0, std::wstring(label) + L" 出现了预期的阈值颜色");
    }

    // DrawItem 不应该把 DC 的状态弄脏（字体必须还是我们选进去的那个）
    Check(::GetCurrentObject(mem, OBJ_FONT) == font,
          std::wstring(label) + L" DrawItem 之后 DC 的字体未被改动");

    ::SelectObject(mem, old_bitmap);
    ::DeleteObject(bitmap);
    ::SelectObject(mem, old_font);
    ::DeleteObject(font);
    ::DeleteDC(mem);
    ::ReleaseDC(nullptr, screen);
    (void)plugin;
}

void ExerciseItem(IPluginItem* item, const wchar_t* label)
{
    Line(L"");
    Line(std::wstring(L"显示项 ") + label);
    Check(item != nullptr, L"GetItem 返回非空");
    if (item == nullptr)
        return;

    Check(item->GetItemName() != nullptr, L"GetItemName 非空");
    Check(item->GetItemId() != nullptr, L"GetItemId 非空");
    Check(item->GetItemLableText() != nullptr, L"GetItemLableText 非空");
    Check(item->GetItemValueText() != nullptr, L"GetItemValueText 非空");
    Check(item->GetItemValueSampleText() != nullptr, L"GetItemValueSampleText 非空");

    Line(std::wstring(L"    名称   : ") + Safe(item->GetItemName()));
    Line(std::wstring(L"    ID     : ") + Safe(item->GetItemId()));
    Line(std::wstring(L"    标签   : ") + Safe(item->GetItemLableText()));
    Line(std::wstring(L"    示例值 : ") + Safe(item->GetItemValueSampleText()));

    // ID 必须只含字母和数字（插件开发指南的要求）
    bool id_valid = true;
    for (const wchar_t* p = item->GetItemId(); p != nullptr && *p != L'\0'; ++p)
    {
        if (!((*p >= L'a' && *p <= L'z') || (*p >= L'A' && *p <= L'Z') ||
              (*p >= L'0' && *p <= L'9')))
        {
            id_valid = false;
            break;
        }
    }
    Check(id_valid, L"ID 只包含字母和数字");

    const float graph = item->GetResourceUsageGraphValue();
    Check(graph >= 0.0f && graph <= 1.0f, L"资源占用图数值在 0.0~1.0 之间");
}

}   // namespace

int wmain(int argc, wchar_t* argv[])
{
    if (argc < 2)
    {
        Line(L"用法: HostTest.exe <插件DLL路径>");
        return 2;
    }

    const std::wstring dll_path = argv[1];
    Line(L"加载 " + dll_path);

    HMODULE module = ::LoadLibraryW(dll_path.c_str());
    Check(module != nullptr, L"LoadLibrary 成功");
    if (module == nullptr)
    {
        Line(L"  GetLastError = " + std::to_wstring(::GetLastError()));
        return 1;
    }

    using GetInstanceFn = ITMPlugin* (*)();
    auto get_instance = reinterpret_cast<GetInstanceFn>(
        ::GetProcAddress(module, "TMPluginGetInstance"));
    Check(get_instance != nullptr, L"导出了 TMPluginGetInstance");
    if (get_instance == nullptr)
        return 1;

    ITMPlugin* plugin = get_instance();
    Check(plugin != nullptr, L"TMPluginGetInstance 返回非空");
    if (plugin == nullptr)
        return 1;

    Check(plugin->GetAPIVersion() == 7, L"接口版本为 7");

    Line(L"");
    Line(L"插件信息");
    const wchar_t* const info_names[] = { L"名称", L"描述", L"作者", L"版权", L"版本", L"主页" };
    for (int i = 0; i < ITMPlugin::TMI_MAX; ++i)
    {
        const wchar_t* value = plugin->GetInfo(static_cast<ITMPlugin::PluginInfoIndex>(i));
        Check(value != nullptr, std::wstring(L"GetInfo(") + info_names[i] + L") 非空");
        Line(std::wstring(L"    ") + info_names[i] + L": " + Safe(value));
    }

    // TrafficMonitor 的调用顺序：先传配置目录，再 OnInitialize
    StubApp app;
    app.m_config_dir = MakeTempConfigDir();
    Line(L"");
    Line(L"配置目录: " + app.m_config_dir);
    plugin->OnExtenedInfo(ITMPlugin::EI_CONFIG_DIR, app.m_config_dir.c_str());
    plugin->OnInitialize(&app);

    IPluginItem* item0 = plugin->GetItem(0);
    IPluginItem* item1 = plugin->GetItem(1);
    ExerciseItem(item0, L"0");
    ExerciseItem(item1, L"1");

    Line(L"");
    Check(plugin->GetItem(2) == nullptr, L"越界索引返回空指针");
    Check(plugin->GetItem(-1) == nullptr, L"负索引返回空指针");

    Line(L"");
    Line(L"插件命令");
    const int command_count = plugin->GetCommandCount();
    Line(L"    命令数量: " + std::to_wstring(command_count));
    for (int i = 0; i < command_count; ++i)
    {
        Check(plugin->GetCommandName(i) != nullptr, L"命令名称非空");
        Line(std::wstring(L"    [") + std::to_wstring(i) + L"] " +
             Safe(plugin->GetCommandName(i)));
    }
    Check(plugin->GetCommandName(command_count) == nullptr, L"越界命令返回空指针");

    // 模拟主程序每秒一次的刷新循环
    Line(L"");
    Line(L"模拟 15 秒刷新循环（每秒一次 DataRequired）");
    for (int tick = 0; tick < 15; ++tick)
    {
        plugin->DataRequired();
        const wchar_t* v0 = (item0 != nullptr) ? item0->GetItemValueText() : L"<null>";
        const wchar_t* v1 = (item1 != nullptr) ? item1->GetItemValueText() : L"<null>";
        wchar_t line[128]{};
        _snwprintf_s(line, _TRUNCATE, L"    t=%2ds   5h: %-14s  7d: %-14s",
                     tick, Safe(v0), Safe(v1));
        Line(line);
        ::Sleep(1000);
    }

    Line(L"");
    Line(L"自绘（模拟主程序：先传文字颜色，再调 DrawItem）");
    Line(std::wstring(L"    IsCustomDraw = ") +
         ((item0 != nullptr && item0->IsCustomDraw()) ? L"true" : L"false"));
    if (item0 != nullptr && item0->IsCustomDraw())
    {
        // 借助演示模式喂进确定的百分比，这样阈值颜色和进度条都能被真正验证，
        // 不必依赖接口当下是否可用。
        auto load_demo = [&](const wchar_t* spec)
        {
            ::SetEnvironmentVariableW(L"CLAUDE_USAGE_MONITOR_DEMO", spec);
            plugin->OnPluginCommand(0, nullptr, nullptr);   // 立即刷新
            ::Sleep(1200);
            plugin->DataRequired();
        };

        // 85% -> 危险色；42% -> 低于警告阈值，保持主题色
        load_demo(L"85,42");
        Line(std::wstring(L"    演示数据 5h = ") + Safe(item0->GetItemValueText()) +
             L"   7d = " + Safe(item1 != nullptr ? item1->GetItemValueText() : L""));
        Check(std::wstring(item0->GetItemValueText()).find(L"85%") == 0,
              L"5h 数值以 85% 开头");
        // 演示数据把重置时刻设在 1h50m 之后，但取整是向下取的，
        // 走到这里已经过去一两秒，所以只校验形态是 "(1h??m)"
        const std::wstring five_text = item0->GetItemValueText();
        Check(five_text.find(L"(1h") != std::wstring::npos && five_text.back() == L')',
              L"5h 数值包含 (1h??m) 形态的剩余时间");

        const COLORREF kDark = RGB(230, 230, 230);
        plugin->OnExtenedInfo(ITMPlugin::EI_VALUE_TEXT_COLOR,
                              std::to_wstring(static_cast<unsigned long>(kDark)).c_str());
        // 默认危险色 F7768E
        ExerciseCustomDraw(plugin, item0, L"5h@85%/深色", true, RGB(0xF7, 0x76, 0x8E), true);
        ExerciseCustomDraw(plugin, item1, L"7d@42%/深色", true, kDark, true);

        // 65% -> 警告色 E0AF68
        load_demo(L"65,65");
        ExerciseCustomDraw(plugin, item0, L"5h@65%/深色", true, RGB(0xE0, 0xAF, 0x68), true);

        // 浅色任务栏
        plugin->OnExtenedInfo(ITMPlugin::EI_VALUE_TEXT_COLOR,
                              std::to_wstring(static_cast<unsigned long>(RGB(32, 32, 32))).c_str());
        ExerciseCustomDraw(plugin, item0, L"5h@65%/浅色", false, RGB(0xE0, 0xAF, 0x68), true);

        ::SetEnvironmentVariableW(L"CLAUDE_USAGE_MONITOR_DEMO", nullptr);
    }

    Line(L"");
    Line(L"鼠标提示内容：");
    const wchar_t* tooltip = plugin->GetTooltipInfo();
    Check(tooltip != nullptr, L"GetTooltipInfo 非空");
    if (tooltip != nullptr)
    {
        Line(L"--------------------------------");
        Line(tooltip);
        Line(L"--------------------------------");
    }

    // 双击应触发立即刷新并返回 1（表示已完全处理）
    if (item0 != nullptr)
    {
        Check(item0->OnMouseEvent(IPluginItem::MT_DBCLICKED, 0, 0, nullptr, 0) == 1,
              L"双击返回 1（已处理）");
        Check(item0->OnMouseEvent(IPluginItem::MT_RCLICKED, 0, 0, nullptr, 0) == 0,
              L"右键返回 0（交回主程序菜单）");
    }

    plugin->OnPluginCommand(0, nullptr, nullptr);
    Line(L"  已触发命令 0（立即刷新）");
    ::Sleep(2000);
    plugin->DataRequired();

    // 主程序退出时也会走这些调用，确认不崩溃
    ITMPlugin::MonitorInfo monitor_info{};
    plugin->OnMonitorInfo(monitor_info);
    Check(plugin->ShowOptionsDialog(nullptr) == ITMPlugin::OR_OPTION_NOT_PROVIDED,
          L"未提供选项对话框时返回 OR_OPTION_NOT_PROVIDED");

    Line(L"");
    if (g_failures == 0)
    {
        Line(L"宿主测试全部通过。");
        return 0;
    }
    Line(L"宿主测试失败 " + std::to_wstring(g_failures) + L" 项。");
    return 1;
}
