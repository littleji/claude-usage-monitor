# 实现与设计说明

[English](internals.md) | **简体中文**

返回 [README](../README.zh-CN.md)。

这份文档记录插件里那些不太显然的选择背后的理由 —— 通常在你踩到它想规避的那个坑之前，
都会显得有点莫名其妙。

---

## 源码结构

```
src/
  Json.h / Json.cpp                     只读 JSON 解析器（无外部依赖）
  TimeUtil.h / TimeUtil.cpp             ISO8601 解析、剩余时间格式化（对齐 cship）
  UsageApi.h / UsageApi.cpp             读取凭据 + WinHTTP 请求 + 响应解析 + demo 模式
  UsageService.h / UsageService.cpp     后台取数线程、快照、重试退避
  DisplayConfig.h / DisplayConfig.cpp   排版与配色配置、格式串占位符替换
  TerminalStatus.h / TerminalStatus.cpp 扫描 hooks 写出的终端状态文件
  ClaudeUsagePlugin.h / .cpp            ITMPlugin / IPluginItem 实现与导出函数
  DllMain.cpp                           固定模块，避免卸载时留下悬空线程
include/
  PluginInterface.h                     TrafficMonitor 官方插件接口（API 版本 7）
tools/
  Probe.cpp                             命令行探针与离线自检
  HostTest.cpp                          宿主测试，通过 LoadLibrary 跑完整生命周期
  claude-hook-status.ps1                Claude Code hook 脚本，上报终端状态
```

没有任何第三方依赖：HTTPS 用系统自带的 WinHTTP，JSON 用 `src/Json.*` 里约 300 行的
只读解析器，CRT 用 `/MT` 静态链接，因此目标机器不需要额外的运行库。

---

## 插件侧的设计取舍

**绝不在 `DataRequired` 里发网络请求。** 这个函数由 TrafficMonitor 的 UI 线程每秒调用一次，
阻塞它会让整个宿主程序卡死。请求跑在独立线程上，`DataRequired` 只读内存里的快照并
重算倒计时文字。

**取数失败时继续显示上一次的数据**，只在鼠标提示里说明失败原因。否则每次网络抖动，
任务栏上的数字都会闪成 `--`。

**在 `DllMain` 里固定模块。** 插件持有一个长期存活的线程，如果宿主在它运行期间对这个 DLL
调用 `FreeLibrary`，那个线程就会去执行已经被卸载的代码然后崩溃。固定之后 DLL 只随进程一起
退出，也就不需要在 `DllMain` 里等线程结束 —— 那样会持有 loader lock，有死锁风险。

**自绘文字必须用 `DrawTextW`。** TrafficMonitor 会 patch 插件 DLL 的 user32 导入表里
`DrawText` 系列函数，任务栏的 Direct2D 渲染依赖这个拦截点。改用 `TextOut` / `ExtTextOut`
在 D2D 模式下什么都画不出来。测宽同理用 `DrawTextW(DT_CALCRECT)` —— 宿主自己的代码里就注明
`GetTextExtent` 只是理论宽度，不够准。

**自绘的基准文字颜色取自 `EI_VALUE_TEXT_COLOR`。** 宿主在每次 `DrawItem` 之前通过
`OnExtenedInfo` 把当前皮肤的颜色以十进制 `COLORREF` 字符串传进来。不直接读 DC 的
`GetTextColor` 是因为任务栏跑在 Direct2D 模式时，`DrawItem` 拿到的是一个临时 GDI DC，
上面从来没有设置过文字颜色。

**失败退避**起步 30 秒，逐次翻倍，上限 10 分钟；服务端给了 `Retry-After` 就按它来。

**显示项 ID 是 `ClaudeUsageOAuth5h` / `ClaudeUsageOAuth7d`**，有意和其他 Claude 相关插件
区分开，以便共存。

**刷新间隔默认 60 秒而不是更快。** 括号里的倒计时是插件在本地按 `resets_at` 推算的，
每秒都会刷新，不依赖请求频率；只有百分比需要请求。而 `/api/oauth/usage` 的限流相当紧，
多个客户端共用同一账号时尤其容易撞上，所以间隔放宽几乎没有体感损失，却能明显减少
撞上 429 的概率。

---

## 圆点为什么是画出来的，不是打出来的

状态圆点是插件自己用 GDI 画的（`Ellipse` + 纯色画刷），而不是彩色 emoji 字符。

早期版本确实用过 🔵🟡🟢🔴 这类色块 emoji，结果所有颜色的 emoji 全部变成清一色的黑白轮廓。
原因是 GDI 的文字绘制（`DrawTextW` 等）不认字体自带的调色板 —— 也就是彩色 emoji 依赖的
COLR/CPAL 彩色字体表 —— 它只按 `SetTextColor` 设的单一颜色画字形轮廓。真正的彩色字体渲染
要走 DirectWrite/Direct2D 的专门接口，GDI 画文字拿不到。

改成自己画图形就彻底绕开了这个问题：颜色完全由代码里指定的 RGB 值决定，用的是和现有
进度条（`FillRect`）同一套、已经在任务栏验证过能正确显示颜色的机制，不依赖字体或渲染路径。

配色的分配也遵循同样的逻辑。「等待用户命令」「已完成」「出错」直接复用已有的
`warn_color` / `normal_color` / `critical_color` 三档，改一处进度条和圆点一起变；
「正在思考」和「空闲中」没有对应的阈值颜色，所以各自有独立的
`terminal_thinking_color` / `terminal_idle_color` 配置项。

---

## 每个 hook 事件为什么都需要

插件看不到 Claude Code 进程内部，状态只能靠上报。这七个事件各有各的不可替代性：

**要 `StopFailure`，不能只有 `Stop`。** `Stop` 只在一轮回复**正常**结束时触发，且完全不带
错误信息。真正的失败信号 —— `rate_limit`、`overloaded`、`authentication_failed` 等，
以 `error_type` 的形式出现 —— 只在 `StopFailure` 里有。没有它，红色状态根本无法到达。

**`PreToolUse` 修的是「一直黄着不变蓝」的 bug。** 通过权限确认之后不会触发新的
`UserPromptSubmit` —— 那不是新一轮用户输入，Claude 只是在继续当前这一轮。于是就没有任何
事件能把状态从「等待」拉回「思考」，圆点会一直黄到这轮结束、被 `Stop` 转绿为止，
而这期间 Claude 明明在干活，显示是错的。`PreToolUse` 在工具真正执行前触发，权限一旦通过
它必然会来，所以拿它当「确实又在干活了」的信号。

**`Notification` 不能当成单一状态处理。** 它的 `notification_type` 取值相当杂
（`permission_prompt`、`idle_prompt`、`auth_success`、`elicitation_*`、`agent_completed` 等），
其中大部分根本不代表「需要你关注」：

- 权限确认和 MCP 弹窗才映射到黄色。
- `idle_prompt` —— Claude Code 自己的「你还在吗」提醒 —— 映射到**灰色**而不是黄色。
  它真正的含义是「这个终端闲着」，而不是「有个决定等你做」。映射成黄色的话，
  一个已经聊完变绿的会话放着一会儿，就会被这个提醒错误地顶回黄色。
- 其余类型（`auth_success`、`elicitation_complete` 等）直接忽略，不改变当前颜色。

---

## 已死终端的清理

正常退出终端（`/exit`、Ctrl+D）会触发 `SessionEnd`，删掉该会话的状态文件。

直接关窗口或者强杀进程时 `SessionEnd` 没机会执行 —— 但插件并不退化成干等超时。
状态文件里记录了拥有这个终端的进程 PID（由 hook 脚本沿进程树往上找，跳过
`powershell`、`cmd` 这类临时 shell 得到）；插件每次扫描时通过
`OpenProcess` / `GetExitCodeProcess` 检查该 PID 是否还活着，进程没了就立刻删掉状态文件、
圆点马上消失，不用等 `terminal_stale_minutes`（默认 360 分钟）。只有旧版本写出的、
没有 `pid` 字段的状态文件才会退回超时清理。

通过 Task/Agent 工具派生的子智能体并不是用户能看到的终端窗口，所以 hook 脚本在事件负载里
看到 `agent_id` / `agent_type`（这是子智能体特有的）时就跳过写状态文件，跑子任务不会多出圆点。

状态目录内部最多每秒扫描一次，与 TrafficMonitor 调用 `DataRequired` 的频率无关。

---

## 验证：HostTest.exe

`HostTest.exe` 用和 TrafficMonitor 完全一致的方式加载插件 —— `LoadLibrary` +
`GetProcAddress("TMPluginGetInstance")` —— 然后按宿主自己的调用顺序跑完整个生命周期。
它不链接插件的任何源码，所以是在真正地验证导出函数、vtable 布局和调用约定。

```powershell
.\build\x64-Release\HostTest.exe .\build\x64-Release\ClaudeUsageMonitor.dll
```

覆盖内容：接口版本为 7；六项插件信息都非空；两个显示项的名称、ID、标签与示例文本；
显示项 ID 只含字母数字；越界与负数索引返回空；插件命令；模拟 15 秒的每秒 `DataRequired`
循环；鼠标提示内容；双击返回 1 而右键返回 0；`ShowOptionsDialog` 返回
`OR_OPTION_NOT_PROVIDED`。

自绘部分用 demo 模式喂入固定百分比，然后真的把显示项画到内存位图上逐像素校验：
宽度合理、`hDC` 为空时返回 0 以便宿主正确回退、画布上确实写入了内容、底部进度条铺满整行、
出现预期的阈值配色（85% 危险色、65% 警告色），以及 `DrawItem` 返回后不会弄脏 DC 里选中的
字体。深色和浅色主题都跑一遍。
