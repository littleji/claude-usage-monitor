<div align="center">

# Claude Usage Monitor

**把 Claude Code 的额度、以及每个终端正在干什么，直接显示在 Windows 任务栏上。**

一个自包含的 [TrafficMonitor](https://github.com/zhongyang219/TrafficMonitor) 插件 DLL。
不需要 Node.js，不需要浏览器，不需要二次登录。

[![Release](https://img.shields.io/github/v/release/littleji/claude-usage-monitor?style=flat-square)](https://github.com/littleji/claude-usage-monitor/releases/latest)
[![Downloads](https://img.shields.io/github/downloads/littleji/claude-usage-monitor/total?style=flat-square)](https://github.com/littleji/claude-usage-monitor/releases)
[![License](https://img.shields.io/badge/license-MIT-blue?style=flat-square)](LICENSE)
[![Platform](https://img.shields.io/badge/Windows-x64%20%7C%20x86-0078d4?style=flat-square&logo=windows)](https://github.com/littleji/claude-usage-monitor/releases/latest)
[![Stars](https://img.shields.io/github/stars/littleji/claude-usage-monitor?style=flat-square)](https://github.com/littleji/claude-usage-monitor/stargazers)

[English](README.md) | **简体中文**

![任务栏上的 Claude Usage Monitor](example.png)

</div>

---

## 为什么需要它

正在 Claude Code 里干活的时候，你想知道两件事，但又不想中断手头的思路：

1. **额度还剩多少**，以及窗口什么时候重置。
2. **同时开着的六个终端里，哪个真的在等你** —— 一个在思考，一个卡在权限确认上，
   还有一个已经悄悄报错了。

现在这两件事都在任务栏上。不用切窗口，不用敲 `/usage`，不用开浏览器。

```
● ● ●        5h 21% (1h50m)        7d 47% (3d2h)
 ▲            ▲                     ▲
 三个终端     5 小时窗口已用 21%，   7 天窗口已用 47%，
 及各自状态   1 小时 50 分后重置     3 天 2 小时后重置
```

## 功能

- **🔋 任务栏上的实时额度** —— 5 小时和 7 天两个窗口的已用百分比，加上到重置的倒计时。
  倒计时在本地每秒刷新，接口每分钟才请求一次。
- **🚦 每个终端一个状态圆点** —— 每个正在跑的 Claude Code 终端一个彩色圆点：
  🔵 正在思考 · 🟡 等你做决定 · 🟢 已完成 · 🔴 出错 · ⚪ 空闲。*（这是别的工具都没有的）*
- **🎨 阈值配色 + 进度条** —— 60% 转黄、80% 转红，不看数字也能感知额度还剩多少。
- **🔐 不需要任何额外凭据** —— 复用 Claude Code 已有的登录态，只发一个 `GET`，
  用完立刻把 token 从内存里抹掉。不写日志、不落盘、不出现在鼠标提示里。
- **📦 单文件、零运行时** —— 就一个 DLL。HTTPS 走系统自带的 WinHTTP，JSON 用仓库内
  约 300 行的解析器，CRT 静态链接。扔进 `plugins\` 重启即可。
- **🖱️ 信息量很足的鼠标提示** —— 绝对重置时刻、Opus / Sonnet 的 7 天分项额度、
  额外用量、最近更新时间，以及取数失败时的具体原因。

## 和同类方案的对比

| | **本插件** | [cship](https://github.com/stephenleo/cship) | [bemaru 插件](https://github.com/bemaru/trafficmonitor-ai-usage-plugin) |
| --- | --- | --- | --- |
| 数据来源 | OAuth 用量接口 | OAuth 用量接口 | claude.ai 页面 + Cookie |
| 运行时依赖 | **无** | Rust 二进制 | Node.js 22+、Edge/Chrome |
| 登录方式 | 复用 Claude Code | 复用 Claude Code | 需要单独跑一次浏览器登录 |
| 常驻任务栏 | ✅ | ❌（终端里看） | ✅ |
| 终端状态显示 | ✅ | ❌ | ❌ |
| Codex 用量 | ❌ | ❌ | ✅ |

本插件的 DLL 名和显示项 ID 都和 bemaru 插件不同，两者可以同时安装。

---

## 安装

**前置条件**：已经装好并运行 [TrafficMonitor](https://github.com/zhongyang219/TrafficMonitor)。

1. 下载 **[最新版本](https://github.com/littleji/claude-usage-monitor/releases/latest)**，
   把解压出来的 `ClaudeUsageMonitor.dll` 放进 TrafficMonitor 的 `plugins` 目录
   （例如 `D:\tools\TrafficMonitor\plugins\`）。
2. 重启 TrafficMonitor，「选项」→「插件管理」里就能看到 *Claude 用量监控*。
3. 在「显示设置」（主窗口 / 任务栏窗口分别设置）里勾选
   **Claude 5小时用量** 和 **Claude 7天用量**。

> ⚠️ DLL 的位数必须和 `TrafficMonitor.exe` 一致：常见的 64 位版本下载 `x64` 包，
> 32 位版本下载 `x86` 包。

两个用量显示项到这里就能用了。**终端状态圆点还需要多做一步**，见下。

---

## 终端状态圆点

插件本身看不到 Claude Code 进程内部发生了什么，只能由 Claude Code 主动上报，这正是
hooks 的用途：每次事件触发时，一个小脚本把当前会话的状态写到
`<CLAUDE_CONFIG_DIR 或 %USERPROFILE%\.claude>\status\<session_id>.json`，
插件每秒扫一次这个目录。**不产生任何额外网络请求。**

| 圆点 | 状态 | 触发时机 |
| --- | --- | --- |
| ⚪ 灰 | 空闲中 | 会话刚打开还没提交过指令；或者已经聊完，放着没人管 |
| 🔵 蓝 | 正在思考 | 已提交请求，Claude 正在处理 |
| 🟡 黄 | 等待用户命令 | Claude 真的需要你现在做个决定（权限确认、MCP 弹窗要输入等） |
| 🟢 绿 | 已完成 | 一轮回复正常结束 |
| 🔴 红 | 出错 | 一轮回复因 API 错误异常终止（限流、服务端错误、鉴权失败等） |

鼠标悬停可以看到完整列表：每个终端的状态、工作目录、错误类型，以及 session id 的前
8 位（用来分辨终端）。终端数超过 `terminal_max_icons`（默认 12）时，任务栏只显示前几个
加一个 `+N`。一个终端都没跑的时候，这一项不会空着，而是显示「终端无AI应用」。

### 配置 hooks

1. hooks 调用的脚本已经在仓库里：`tools\claude-hook-status.ps1`。
2. 把下面这段加进 `%USERPROFILE%\.claude\settings.json` 的 `hooks` 一节，把 `<repo>`
   换成本仓库的实际路径。如果里面已经配了别的 hooks，追加到对应事件的数组里即可，
   不用整块替换。

```jsonc
{
  "hooks": {
    "SessionStart": [
      { "hooks": [ { "type": "command", "command": "powershell -NoProfile -ExecutionPolicy Bypass -File \"<repo>\\tools\\claude-hook-status.ps1\" -Event SessionStart" } ] }
    ],
    "UserPromptSubmit": [
      { "hooks": [ { "type": "command", "command": "powershell -NoProfile -ExecutionPolicy Bypass -File \"<repo>\\tools\\claude-hook-status.ps1\" -Event UserPromptSubmit" } ] }
    ],
    "PreToolUse": [
      { "hooks": [ { "type": "command", "command": "powershell -NoProfile -ExecutionPolicy Bypass -File \"<repo>\\tools\\claude-hook-status.ps1\" -Event PreToolUse" } ] }
    ],
    "Notification": [
      { "hooks": [ { "type": "command", "command": "powershell -NoProfile -ExecutionPolicy Bypass -File \"<repo>\\tools\\claude-hook-status.ps1\" -Event Notification" } ] }
    ],
    "Stop": [
      { "hooks": [ { "type": "command", "command": "powershell -NoProfile -ExecutionPolicy Bypass -File \"<repo>\\tools\\claude-hook-status.ps1\" -Event Stop" } ] }
    ],
    "StopFailure": [
      { "hooks": [ { "type": "command", "command": "powershell -NoProfile -ExecutionPolicy Bypass -File \"<repo>\\tools\\claude-hook-status.ps1\" -Event StopFailure" } ] }
    ],
    "SessionEnd": [
      { "hooks": [ { "type": "command", "command": "powershell -NoProfile -ExecutionPolicy Bypass -File \"<repo>\\tools\\claude-hook-status.ps1\" -Event SessionEnd" } ] }
    ]
  }
}
```

3. 重启已经开着的 Claude Code 终端 —— hooks 只在新会话里生效。

配置完成后，每开一个终端任务栏上就多一个圆点。正常退出（`/exit`、Ctrl+D）会触发
`SessionEnd` 并清理状态文件；直接关窗口或强杀进程也不会留下残影，状态文件里记了终端的
PID，插件发现进程没了就立刻把它去掉。Task 工具派生的子智能体会被有意跳过，
跑后台任务不会多出圆点。

想知道为什么这七个事件缺一不可、以及圆点为什么是 GDI 画出来而不是 emoji 字符？
见 **[docs/internals.zh-CN.md](docs/internals.zh-CN.md)**。

---

## 配置

首次运行后，TrafficMonitor 的插件配置目录（通常是 `<TrafficMonitor 所在目录>\plugins\`）
下会生成 `ClaudeUsage.ini`，改完重启 TrafficMonitor 生效。

```ini
[general]
refresh_interval=60

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
terminal_stale_minutes=360
terminal_max_icons=12
terminal_thinking_color=3B82F6
terminal_idle_color=9E9E9E
```

<details>
<summary><b>全部配置项说明</b></summary>

| 键 | 默认 | 说明 |
| --- | --- | --- |
| `refresh_interval` | `60` | 访问接口的间隔（秒），允许范围 60 ~ 3600 |
| `custom_draw` | `1` | 是否由插件自绘。设为 `0` 则退回主程序排版，阈值颜色和进度条随之失效 |
| `show_bar` | `1` | 是否在文字下方画进度条。显示区域太矮时会自动省略，优先保证文字可读 |
| `warn_threshold` | `60` | 超过该百分比进度条转为警告色 |
| `critical_threshold` | `80` | 超过该百分比进度条转为危险色 |
| `bar_color_enabled` | `1` | 是否让进度条按阈值上色（绿/黄/红）。设为 `0` 关闭，进度条退回文字同色 |
| `normal_color` | `9ECE6A` | 正常色，`RRGGBB` 十六进制，🟢「已完成」圆点也用它 |
| `warn_color` | `E0AF68` | 警告色，🟡「等待用户命令」圆点也用它 |
| `critical_color` | `F7768E` | 危险色，🔴「出错」圆点也用它 |
| `*_label` | `5h` / `7d` | 显示项前缀。留空则只显示数值 |
| `*_format` | `{pct}% ({reset})` | 数值格式串，占位符见下 |
| `terminal_stale_minutes` | `360` | 状态文件超过这么多分钟没更新，视为已死会话并忽略 |
| `terminal_max_icons` | `12` | 任务栏最多显示几个圆点，超出的折叠成 `+N` |
| `terminal_thinking_color` | `3B82F6` | 🔵「正在思考」圆点的颜色 |
| `terminal_idle_color` | `9E9E9E` | ⚪「空闲中」圆点的颜色 |

圆点复用的就是进度条那套颜色，改一处两边都跟着变。

文字颜色始终使用主题的默认前景色，不会随阈值变化 —— 白底配红/黄字容易糊成一团、
看不清具体数值。阈值配色只体现在文字下方的细进度条上。

</details>

### 格式串

| 占位符 | 含义 | 示例 |
| --- | --- | --- |
| `{pct}` | 已用百分比，取整 | `21` |
| `{remaining}` | 剩余百分比，取整 | `79` |
| `{reset}` | 距离重置的剩余时间 | `4h19m` |
| `{reset_at}` | 重置的本地时刻 | `19:42` 或 `08-19 09:00` |

```ini
five_hour_format={pct}% ({reset})      ; 5h 21% (4h19m)   默认
five_hour_format={pct}% · {reset}      ; 5h 21% · 4h19m   中点分隔，更窄
five_hour_format={pct}%                ; 5h 21%           只看额度
five_hour_format={remaining}% left     ; 5h 79% left      换成"还剩多少"的视角
five_hour_format={pct}% →{reset_at}    ; 5h 21% →19:42    显示绝对重置时刻
```

任务栏上的宽度按格式串的**最坏情况**（`100%`、`23h59m`）计算，所以数值变化时显示项
宽度是稳定的，不会左右抖动。

剩余时间的显示：不足 1 小时是 `45m`，不足 1 天是 `4h12m`，1 天以上是 `3d2h`；
接口没返回重置时刻显示 `?`，已经过了重置时刻显示 `now`。数值本身在首次取数成功前显示
`--`，接口没有返回这个窗口时显示 `N/A`（部分 Enterprise 账号是这样）。

**双击**显示项可以立即刷新，也可以在插件右键菜单里选「立即刷新用量」。

---

## 排错

`Probe.exe`（从源码构建，见下）跑的是和插件完全相同的取数与格式化代码，但输出到控制台。
它不会打印 token。

```powershell
.\build\x64-Release\Probe.exe --selftest   # 离线自检：JSON、ISO8601、时间格式化
.\build\x64-Release\Probe.exe              # 实际访问接口并打印用量
```

| 提示 | 原因与处理 |
| --- | --- |
| 找不到凭据文件 | 还没登录过 Claude Code，或 `CLAUDE_CONFIG_DIR` 指向了别处 |
| 读不到 `claudeAiOauth.accessToken` | 凭据文件格式不符合预期，重新登录一次 Claude Code |
| 访问令牌已过期（HTTP 401） | 重新登录一次 Claude Code。插件有意不自己拿 refresh token 去续期，续期交给 Claude Code |
| 请求过于频繁（HTTP 429） | 见下 |
| 无法连接 api.anthropic.com | 网络/代理问题。插件走 WinHTTP，遵循系统代理设置 |

<details>
<summary><b>关于 HTTP 429</b></summary>

`/api/oauth/usage` 的限流发生在**鉴权之前** —— 哪怕完全不带 `Authorization` 头，
被限流时也只会返回：

```json
{ "error": { "type": "rate_limit_error", "message": "Rate limited. Please try again later." } }
```

也就是说这跟 token 是否有效没有关系，它意味着这个接口在你当前出口 IP / 账号下的配额
用完了（Claude Code、cship 以及同账号的其他用量脚本都在消耗它）。

尤其注意：**TrafficMonitor 已经加载了插件时再去跑 `Probe.exe`，经常直接吃 429**，
因为两边共用同一份配额。要用探针调试就先退出 TrafficMonitor，或者干脆直接看插件的
鼠标提示。想确认跟插件无关可以这样验证：

```powershell
Invoke-WebRequest https://api.anthropic.com/api/oauth/usage -SkipHttpErrorCheck | Select-Object StatusCode
```

如果不带凭据也返回 429，那就只能等配额恢复。这种情况下插件会自动退避重试
（起步 30 秒，逐次翻倍，上限 10 分钟），期间继续显示上一次成功取到的数据。

有些代理/网关会按 User-Agent 拦请求，可以用
`$env:CLAUDE_USAGE_MONITOR_UA = "ureq/3.1.2"` 覆盖（默认是
`TrafficMonitor-ClaudeUsage/1.0`）。

</details>

<details>
<summary><b>不用真跑到 60% / 80% 也能预览配色</b></summary>

设置 `CLAUDE_USAGE_MONITOR_DEMO` 环境变量可以绕过接口，直接喂入指定百分比：

```powershell
$env:CLAUDE_USAGE_MONITOR_DEMO = "85,42"   # 5 小时窗口 85%（危险色），7 天窗口 42%（正常色）
.\build\x64-Release\Probe.exe
```

想在 TrafficMonitor 里预览，就把它设成系统环境变量，重启 TrafficMonitor，
调完排版和配色再删掉。

</details>

---

## 从源码构建

只需要 Visual Studio 2022 的 MSVC C++ 工具链，不需要 MFC / CMake / Node.js。

```powershell
.\build.ps1                          # x64 Release，输出到 build\x64-Release\
.\build.ps1 -Arch x86                # 32 位
.\build.ps1 -Config Debug            # 带调试信息
.\build.ps1 -Install <plugins 目录>  # 构建后直接安装
.\build.ps1 -Arch all -Zip           # x64 + x86 各出一个发布用的 zip
```

产物是 `ClaudeUsageMonitor.dll`（插件本体）、`Probe.exe`（命令行探针）和
`HostTest.exe`（用 TrafficMonitor 完全相同的方式加载 DLL 并跑完整生命周期，
包括对自绘结果的逐像素检查）。也可以直接用 Visual Studio 打开
`ClaudeUsageMonitor.sln`，但那样不会构建后两个工具。

---

## 数据从哪来

```
%USERPROFILE%\.claude\.credentials.json        （或 $CLAUDE_CONFIG_DIR）
        │  读取 claudeAiOauth.accessToken
        ▼
GET https://api.anthropic.com/api/oauth/usage
        Authorization: Bearer <token>
        anthropic-beta: oauth-2025-04-20
        │
        ▼
{ "five_hour":  { "utilization": 21.0, "resets_at": "..." },
  "seven_day":  { "utilization": 47.5, "resets_at": "..." },
  "seven_day_opus": { ... }, "seven_day_sonnet": { ... },
  "extra_usage": { "is_enabled": true, "monthly_limit": ..., ... } }
```

访问令牌只在一次请求期间作为局部变量存在，用完立刻用 `SecureZeroMemory` 抹掉。
它不会被写进配置文件、缓存或日志，也不会出现在鼠标提示里。插件只发这一个 GET 请求，
不会修改 Claude Code 的任何文件。

架构、设计取舍，以及每个 hook 事件背后的理由，见
**[docs/internals.zh-CN.md](docs/internals.zh-CN.md)**。

---

## 参与贡献

欢迎提 issue 和 PR —— 尤其是 bug 反馈、TrafficMonitor 皮肤适配问题，以及格式串的新点子。
如果这个插件对你有用，点个 ⭐ 能帮更多 Claude Code 用户发现它。

## 致谢

- [TrafficMonitor](https://github.com/zhongyang219/TrafficMonitor) —— 宿主程序。
- [cship](https://github.com/stephenleo/cship) —— 取数方式、时间格式和默认阈值配色的来源。

## 许可证

MIT
