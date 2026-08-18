# Claude Usage Monitor — TrafficMonitor 插件
<img width="156" height="57" alt="image" src="https://github.com/user-attachments/assets/1286733b-767e-450a-a473-92120f3a329a" />



在 [TrafficMonitor](https://github.com/zhongyang219/TrafficMonitor) 的主窗口/任务栏上显示
Claude 的额度用量与距离窗口重置的剩余时间：

```
5h  21% (1h50m)      7d  47% (3d2h)
```

取数方式与 [cship](https://github.com/stephenleo/cship) 完全一致：直接复用 Claude Code
已有的本地登录凭据，调用 Anthropic 的 OAuth 用量接口。**不需要 Node.js，不需要浏览器，
不需要二次登录，也不抓取任何 Cookie。** 整个插件就是一个自包含的 DLL。

---

## 显示内容

| 显示项 | 渲染结果 | 说明 |
| --- | --- | --- |
| Claude 5小时用量 | `5h 21% (1h50m)` | 5 小时窗口已用百分比，括号内是距离重置的剩余时间 |
| Claude 7天用量 | `7d 47% (3d2h)` | 7 天窗口已用百分比与剩余时间 |

两个显示项相互独立，可以在 TrafficMonitor 的「显示设置」里分别勾选。

插件默认**自绘**整个显示区域（`IsCustomDraw` 返回 `true`），这样能做到三件主程序
默认排版做不到的事：

- **标签和数值之间有稳定的间距**。交给主程序拼接时中间不留空格，会挤成 `5h21%(1h50m)`。
- **按阈值上色**：额度 ≥60% 转黄、≥80% 转红，阈值与配色默认取 cship 的设定；
  低于警告线时沿用当前皮肤的文字颜色，不破坏主题。
- **文字下方一条细进度条**表示额度占比，不看数字也能感知。

```
  5h 85% (1h50m)          <- 文字转为危险色
  ███████████████░░░░     <- 底部细条，填充部分同色
```

不喜欢自绘可以在配置里关掉（`custom_draw=0`），退回主程序的默认排版。

剩余时间的格式对齐 cship：

| 剩余 | 显示 |
| --- | --- |
| 不足 1 小时 | `45m` |
| 不足 1 天 | `4h12m` |
| 1 天以上 | `3d2h` |
| 接口未返回重置时刻 | `?` |
| 已经过了重置时刻 | `now` |

其他状态：

| 数值显示 | 含义 |
| --- | --- |
| `--` | 还没成功取到过数据（刚启动，或凭据/网络有问题，详见鼠标提示） |
| `N/A` | 接口没有返回这个窗口（部分 Enterprise 账号是这样） |

**鼠标提示（tooltip）** 里还会显示 5h/7d 的绝对重置时刻、Opus / Sonnet 的 7 天分项额度、
额外用量（Extra usage）、最近一次更新时间，以及取数失败时的具体原因。

**双击**显示项可以立即刷新，也可以在插件右键菜单里选「立即刷新用量」。

---

## 数据从哪来

```
%USERPROFILE%\.claude\.credentials.json
        │
        │  取出 claudeAiOauth.accessToken
        ▼
GET https://api.anthropic.com/api/oauth/usage
        Authorization: Bearer <token>
        anthropic-beta: oauth-2025-04-20
        │
        ▼
{ "five_hour":  { "utilization": 21.0, "resets_at": "..." },
  "seven_day":  { "utilization": 47.5, "resets_at": "..." },
  "seven_day_opus":   { ... },
  "seven_day_sonnet": { ... },
  "extra_usage":      { "is_enabled": true, "monthly_limit": ..., ... } }
```

若设置了环境变量 `CLAUDE_CONFIG_DIR`，则以该目录代替 `%USERPROFILE%\.claude`。

### 关于凭据的处理

- access token 只在一次请求期间以局部变量存在，用完立即 `SecureZeroMemory` 清零。
- token **不会**被写入配置文件、缓存或日志，也不会出现在鼠标提示里。
- 插件只做一个 GET 请求，不修改任何 Claude Code 的文件。
- token 过期时接口返回 401，插件会在提示里告知「请在 Claude Code 中重新登录」。
  插件**不会**自己拿 refreshToken 去续期（与 cship 的行为一致），续期交给 Claude Code。

---

## 安装

### 方式一：用现成的 DLL

1. 把 `ClaudeUsageMonitor.dll` 复制到 TrafficMonitor 的 `plugins` 目录，例如
   `D:\tools\TrafficMonitor\plugins\`。
2. 重启 TrafficMonitor。
3. 「选项」→「插件管理」里应能看到 *Claude 用量监控*。
4. 在「显示设置」（主窗口 / 任务栏窗口分别设置）里勾选 `Claude 5小时用量`、`Claude 7天用量`。

> DLL 的位数必须和 TrafficMonitor.exe 一致。本仓库默认构建 x64；32 位版本用
> `.\build.ps1 -Arch x86`。

### 方式二：从源码构建并安装

```powershell
# 只需要 Visual Studio 2022 的 MSVC C++ 工具链，不需要 MFC / CMake / Node.js
.\build.ps1 -Install D:\tools\TrafficMonitor\plugins
```

---

## 配置

首次运行后，TrafficMonitor 的插件配置目录（通常是
`<TrafficMonitor 所在目录>\plugins\`）下会生成 `ClaudeUsage.ini`，
所有可调项都会被写回文件，改完重启 TrafficMonitor 生效。

```ini
[general]
refresh_interval=300

[display]
custom_draw=1
show_bar=1
warn_threshold=60
critical_threshold=80
bar_color_enabled=1
normal_color=9ECE6A
warn_color=E0AF68
critical_color=F7768E
five_hour_label=5h
five_hour_format={pct}% ({reset})
seven_day_label=7d
seven_day_format={pct}% ({reset})
```

| 键 | 默认 | 说明 |
| --- | --- | --- |
| `refresh_interval` | `300` | 访问接口的间隔（秒），允许范围 60 ~ 3600 |
| `custom_draw` | `1` | 是否由插件自绘。设为 `0` 则退回主程序排版，阈值颜色和进度条随之失效 |
| `show_bar` | `1` | 是否在文字下方画进度条。显示区域太矮时会自动省略，优先保证文字可读 |
| `warn_threshold` | `60` | 超过该百分比进度条转为警告色 |
| `critical_threshold` | `80` | 超过该百分比进度条转为危险色 |
| `bar_color_enabled` | `1` | 是否让进度条按阈值上色（绿/黄/红）。设为 `0` 关闭，进度条退回文字同色 |
| `normal_color` | `9ECE6A` | 正常色（低于警告阈值时的进度条颜色），`RRGGBB` 十六进制 |
| `warn_color` | `E0AF68` | 警告色，`RRGGBB` 十六进制 |
| `critical_color` | `F7768E` | 危险色，`RRGGBB` 十六进制 |

> 文字颜色始终使用主题的默认前景色，不会随阈值变化——白底配红/黄字容易糊成一团、看不清具体数值。
> 阈值配色只体现在文字下方的细进度条上。
| `*_label` | `5h` / `7d` | 显示项前缀。留空则只显示数值 |
| `*_format` | `{pct}% ({reset})` | 数值格式串，占位符见下 |

格式串支持的占位符（与 cship 的 `five_hour_format` 一致）：

| 占位符 | 含义 | 示例 |
| --- | --- | --- |
| `{pct}` | 已用百分比，取整 | `21` |
| `{remaining}` | 剩余百分比，取整 | `79` |
| `{reset}` | 距离重置的剩余时间 | `4h19m` |
| `{reset_at}` | 重置的本地时刻 | `19:42` 或 `08-19 09:00` |

几个排版示例：

```ini
five_hour_format={pct}% ({reset})      ; 5h 21% (4h19m)   默认
five_hour_format={pct}% · {reset}      ; 5h 21% · 4h19m   中点分隔，更窄
five_hour_format={pct}%                ; 5h 21%           只看额度
five_hour_format={remaining}% left     ; 5h 79% left      换成"还剩多少"的视角
five_hour_format={pct}% →{reset_at}    ; 5h 21% →19:42    显示绝对重置时刻
```

任务栏上的宽度按格式串的**最坏情况**（`100%`、`23h59m`）计算，
所以数值变化时显示项宽度是稳定的，不会左右抖动。

**为什么默认是 5 分钟而不是 1 分钟**：括号里的倒计时是插件在本地按 `resets_at`
推算的，每秒都会刷新，并不依赖请求频率；只有百分比需要请求。而
`/api/oauth/usage` 的限流相当紧（多个客户端共用同一账号时尤其容易撞上 HTTP 429），
所以拉长间隔几乎没有体感损失，却能明显减少被限流的概率。

---

## 构建

前置条件：Visual Studio 2022，勾选「使用 C++ 的桌面开发」工作负载。

```powershell
.\build.ps1                          # x64 Release，输出到 build\x64-Release\
.\build.ps1 -Arch x86                # 32 位
.\build.ps1 -Config Debug            # 带调试信息
.\build.ps1 -Install <plugins 目录>  # 构建后直接安装
.\build.ps1 -Arch all -Zip           # x64 + x86 各出一个发布用的 zip
```

`-Zip` 会把 DLL 单独打包成 `build\ClaudeUsageMonitor-<版本>-<架构>.zip`
（版本号取自 `ClaudeUsagePlugin.cpp` 里的 `TMI_VERSION`），压缩包里只有
`ClaudeUsageMonitor.dll` 一个文件，解压直接扔进 `plugins` 目录就能用，
方便原封不动传到 GitHub Release。配合 `-Arch all` 一次构建两个架构，
分别产出两个 zip；`-Install` 则要求单一架构，不能跟 `-Arch all` 同用。

产物：

- `ClaudeUsageMonitor.dll` — 插件本体
- `Probe.exe` — 命令行探针，见下
- `HostTest.exe` — 宿主测试，见下

也可以直接用 Visual Studio 打开 `ClaudeUsageMonitor.sln`（不含探针与宿主测试，
那两个只在 `build.ps1` 里构建）。

没有任何第三方依赖：HTTPS 用系统自带的 WinHTTP，JSON 用仓库内约 300 行的
只读解析器（`src/Json.*`）。使用 `/MT` 静态链接 CRT，因此目标机器不需要额外的
运行库。

---

## 排错：Probe.exe

`Probe.exe` 跑的是和插件完全相同的取数与格式化代码，但输出到控制台，
方便在不启动 TrafficMonitor 的情况下定位问题。它只打印用量数值，**不会打印 token**。

```powershell
# 离线自检：JSON 解析、ISO8601 解析、时间格式化
.\build\x64-Release\Probe.exe --selftest

# 实际访问接口并打印用量
.\build\x64-Release\Probe.exe
```

### 预览排版与配色

阈值颜色平时看不到——要等真的用到 60% / 80% 才会出现。设置环境变量
`CLAUDE_USAGE_MONITOR_DEMO` 可以直接喂进指定的百分比，跳过接口访问：

```powershell
# 5 小时窗口 85%（危险色），7 天窗口 42%（正常色）
$env:CLAUDE_USAGE_MONITOR_DEMO = "85,42"
.\build\x64-Release\Probe.exe
```

要在 TrafficMonitor 里预览，就把这个变量设成系统环境变量后重启 TrafficMonitor，
调完格式和配色再删掉它。

常见输出与含义：

| 输出 | 原因与处理 |
| --- | --- |
| `未找到凭据文件` | 还没在 Claude Code 里登录过，或 `CLAUDE_CONFIG_DIR` 指向了别处 |
| `凭据文件中读不到 claudeAiOauth.accessToken` | 凭据格式不符合预期，在 Claude Code 里重新登录 |
| `访问令牌已失效（HTTP 401）` | token 过期，在 Claude Code 里重新登录即可 |
| `请求过于频繁（HTTP 429）` | 接口限流，见下节 |
| `无法连接到 api.anthropic.com` | 网络 / 代理问题。插件走 WinHTTP，会使用系统代理设置 |

### 关于 HTTP 429

`/api/oauth/usage` 的限流发生在**鉴权之前**：即使不带任何 `Authorization` 头，
被限流时也会直接返回

```json
{ "error": { "type": "rate_limit_error", "message": "Rate limited. Please try again later." } }
```

也就是说这跟你的 token 是否有效无关，而是当前出口 IP / 账号在这个端点上的
配额被打满了（同一账号上跑着 Claude Code、cship、其他用量脚本时都会消耗它）。

这个端点的配额很紧，**在 TrafficMonitor 已经加载着本插件的时候再跑 `Probe.exe`，
探针经常会拿到 429**——两者共用同一个账号配额，而插件那边通常已经握着刚取到的
数据了。想用探针排查时先退出 TrafficMonitor，或者直接看插件的鼠标提示。
可以这样确认它与本插件无关：

```powershell
Invoke-WebRequest https://api.anthropic.com/api/oauth/usage -SkipHttpErrorCheck | Select-Object StatusCode
```

若不带凭据也返回 429，就只能等配额恢复。插件在这种情况下会自动退避重试
（30 秒起步逐次翻倍，上限 10 分钟），并保留上一次成功取到的数据继续显示。

个别代理/网关会按 User-Agent 拦截请求，可以用环境变量覆盖：

```powershell
$env:CLAUDE_USAGE_MONITOR_UA = "ureq/3.1.2"
```

（默认是 `TrafficMonitor-ClaudeUsage/1.0`。）

---

## 验证：HostTest.exe

`HostTest.exe` 用和 TrafficMonitor **完全相同的方式**加载插件——`LoadLibrary` +
`GetProcAddress("TMPluginGetInstance")`，然后按主程序的调用顺序驱动一遍完整生命周期。
它不链接插件的任何源码，因此能真实验证导出函数、虚表布局和调用约定。

```powershell
.\build\x64-Release\HostTest.exe .\build\x64-Release\ClaudeUsageMonitor.dll
```

覆盖的内容：接口版本为 7、六项插件信息均非空、两个显示项的名称/ID/标签/示例文本、
显示项 ID 只含字母数字、越界与负索引返回空指针、插件命令、模拟 15 秒的每秒
`DataRequired` 循环、鼠标提示内容、双击返回 1 而右键返回 0、
`ShowOptionsDialog` 返回 `OR_OPTION_NOT_PROVIDED`。

自绘部分借助上面的演示模式喂入确定的百分比，然后把显示项真的画到一张内存位图上，
在像素层面校验：宽度合理、`hDC` 为空时返回 0 让主程序回退、确实往画布上写了内容、
底部进度条铺满整行、出现了预期的阈值颜色（85% 出危险色、65% 出警告色）、
以及 `DrawItem` 返回后没有弄脏 DC 里选中的字体。深色和浅色两种主题都跑一遍。

---

## 实现要点

```
src/
  Json.h / Json.cpp              只读 JSON 解析器（无外部依赖）
  TimeUtil.h / TimeUtil.cpp      ISO8601 解析、剩余时间格式化（对齐 cship）
  UsageApi.h / UsageApi.cpp      读凭据 + WinHTTP 请求 + 响应解析 + 演示模式
  UsageService.h / UsageService.cpp   后台取数线程、快照、退避重试
  DisplayConfig.h / DisplayConfig.cpp 排版与配色配置、格式串占位符替换
  ClaudeUsagePlugin.h / .cpp     ITMPlugin / IPluginItem 实现与导出函数
  DllMain.cpp                    模块 PIN，避免卸载时线程悬空
include/
  PluginInterface.h              TrafficMonitor 官方插件接口（API version 7）
tools/
  Probe.cpp                      命令行探针与离线自检
  HostTest.cpp                   宿主测试（LoadLibrary 驱动插件全生命周期）
```

几个刻意的设计选择：

- **绝不在 `DataRequired` 里发网络请求。** 该函数由 TrafficMonitor 的界面线程每秒调用，
  阻塞它会直接卡住主程序。网络请求跑在独立线程里，`DataRequired` 只读内存快照，
  并重新计算一次倒计时文本。
- **取数失败时保留上一次的数据继续显示**，只在鼠标提示里说明当前取数失败，
  避免网络抖动导致任务栏数字反复闪成 `--`。
- **DllMain 里把模块 PIN 住**。插件持有一个常驻线程，若主程序在运行期间
  `FreeLibrary` 掉本 DLL，线程会执行到已卸载的代码上而崩溃。PIN 之后
  DLL 只随进程一起退出，也就不需要在 DllMain 里等待线程结束
  （那样会持有加载器锁，容易死锁）。
- **自绘的文字必须用 `DrawTextW`**。TrafficMonitor 会 patch 插件 DLL 导入表里
  user32 的 `DrawText` 系列函数，任务栏的 Direct2D 渲染依赖这个拦截点；
  改用 `TextOut` / `ExtTextOut` 在 D2D 模式下画不出来。
  同理，宽度也用 `DrawTextW(DT_CALCRECT)` 测——主程序自己的代码里就注明了
  `GetTextExtent` 拿到的是理论宽度、不够准。
- **自绘时的基准文字颜色取自 `EI_VALUE_TEXT_COLOR`**。主程序在每次 `DrawItem`
  之前通过 `OnExtenedInfo` 把当前皮肤的颜色以十进制 `COLORREF` 字符串传进来。
  之所以不直接读 DC 的 `GetTextColor`：任务栏走 Direct2D 时，`DrawItem` 拿到的是
  临时的 GDI DC，上面并没有设过文字颜色。
- **失败退避**：30 秒起步，逐次翻倍，上限 10 分钟；服务端给了 `Retry-After` 就照它来。
- **显示项 ID 使用 `ClaudeUsageOAuth5h` / `ClaudeUsageOAuth7d`**，与其他 Claude 相关
  插件区分开，可以共存。

---

## 与 bemaru/trafficmonitor-ai-usage-plugin 的区别

两者都在 TrafficMonitor 上显示 Claude 用量，但取数路径完全不同：

| | 本插件（cship 方式） | bemaru 插件 |
| --- | --- | --- |
| 数据来源 | Anthropic OAuth 用量接口 | claude.ai 网页 + Cookie |
| 运行时依赖 | 无 | Node.js 22+、Edge/Chrome |
| 登录 | 复用 Claude Code 凭据 | 需要单独跑一次浏览器登录 |
| 组成 | 单个 DLL | DLL + PowerShell + Node 应用 + 浏览器配置目录 |
| Codex 用量 | 不支持 | 支持 |

两个插件的 DLL 名与显示项 ID 都不同，可以同时安装。

---

## 许可

MIT
