# Claude Usage Monitor — a TrafficMonitor plugin

**English** | [简体中文](README.zh-CN.md)

![](https://github.com/littleji/claude-usage-monitor/blob/main/example.png)



Shows Claude's usage quota and the time remaining until each window resets, right on the
Windows taskbar via [TrafficMonitor](https://github.com/zhongyang219/TrafficMonitor):

```
5h  21% (1h50m)      7d  47% (3d2h)
```

Data is fetched the exact same way as [cship](https://github.com/stephenleo/cship): it
reuses the local login credentials Claude Code already has and calls Anthropic's OAuth
usage API directly. **No Node.js, no browser, no second login, no cookie scraping.**
The whole plugin is a single self-contained DLL.

---

## What it shows

| Item | Rendered as | Notes |
| --- | --- | --- |
| Claude terminal status | `● ● ●` / `No AI running` | One colored dot (blue/yellow/green/red/gray) per running Claude Code terminal, showing its current state; shows a hint text when no terminal is running. Requires hooks to be configured — see below |
| Claude 5-hour usage | `5h 21% (1h50m)` | Percentage used in the 5-hour window, with time until reset in parentheses |
| Claude 7-day usage | `7d 47% (3d2h)` | Percentage used in the 7-day window and time until reset |

The three items are independent and can be toggled separately in TrafficMonitor's
display settings.

By default the plugin **custom-draws** the entire display area (`IsCustomDraw` returns
`true`), which lets it do three things the host program's default layout cannot:

- **Stable spacing between the label and the value.** Letting the host program
  concatenate them leaves no gap, squashing it into `5h21%(1h50m)`.
- **Threshold-based coloring**: usage ≥60% turns yellow, ≥80% turns red — thresholds and
  colors default to cship's settings; below the warning line it follows the current
  skin's text color and doesn't fight the theme.
- **A thin progress bar under the text** for the usage ratio, so you can gauge it
  without reading the number.

```
  5h 85% (1h50m)          <- text turns to the danger color
  ███████████████░░░░     <- thin bar underneath, filled portion in the same color
```

If you don't like the custom drawing, turn it off in the config (`custom_draw=0`) to
fall back to the host program's default layout.

The remaining-time format matches cship:

| Remaining | Shown as |
| --- | --- |
| Less than 1 hour | `45m` |
| Less than 1 day | `4h12m` |
| 1 day or more | `3d2h` |
| API didn't return a reset time | `?` |
| Reset time has already passed | `now` |

Other states:

| Value shown | Meaning |
| --- | --- |
| `--` | No successful fetch yet (just started, or a credential/network issue — see the tooltip) |
| `N/A` | The API didn't return this window (happens for some Enterprise accounts) |

The **tooltip** also shows the absolute reset time for the 5h/7d windows, the per-model
7-day sub-quotas for Opus/Sonnet, extra usage, the last update time, and the specific
reason when a fetch fails.

**Double-click** the item to refresh immediately, or pick "Refresh usage now" from the
plugin's right-click menu.

---

## Claude terminal status

Shows how many Claude Code terminals are currently running on this machine, and each
one's status:

| Color / status | Triggered by | Default color |
| --- | --- | --- |
| Gray, Idle | Session just opened and hasn't submitted anything yet; or the conversation is done and sitting untouched | `terminal_idle_color`, default `9E9E9E` |
| Blue, Thinking | A request has been submitted and Claude is processing it | `terminal_thinking_color`, default `3B82F6` |
| Yellow/Orange, Waiting for input | Claude genuinely needs you to decide something right now (permission confirmation, an MCP dialog asking for input, etc.) | Reuses `warn_color`, default `E0AF68` |
| Green, Done | A turn finished normally | Reuses `normal_color`, default `9ECE6A` |
| Red, Error/interrupted | A turn was aborted by an API error (rate limit, server error, auth failure, etc.) | Reuses `critical_color`, default `F7768E` |

The dots are drawn by the plugin itself with GDI (`Ellipse` + a solid brush), **not**
colored emoji characters. An earlier version used color-block emoji like 🔵🟡🟢🔴, but
GDI's text drawing (`DrawTextW` and friends) doesn't understand a font's built-in color
palette (the COLR/CPAL color table emoji use) — it only draws glyph outlines in whatever
single color `SetTextColor` set, so every colored emoji came out as the same black-and-white
outline, making the whole thing pointless. Real color-font rendering needs the dedicated
DirectWrite/Direct2D APIs, which GDI text drawing can't reach, so the plugin draws its own
shapes instead: the color is decided purely by the RGB value in code, using the same
mechanism the existing progress bar (`FillRect`) already uses and that's been verified to
render correctly on the taskbar — no dependency on fonts or the rendering path.

"Waiting" and "Done" reuse the three usage-bar color tiers `warn_color` / `normal_color` /
`critical_color` that already exist (change one place, both follow); "Thinking" and
"Idle" don't map to an existing threshold color, so they get their own configurable
`terminal_thinking_color` / `terminal_idle_color` — see below.

If no terminal is currently running (hooks not configured, or every terminal has been
closed), the item doesn't sit empty — it shows a line of text: `No AI running` (in a
Chinese environment this is `终端无AI应用`).

Hovering over the item shows a tooltip listing every terminal's full status, its working
directory (if the hook reported `cwd`), its error type (error state only), and the first
8 characters of its session id, to help tell terminals apart; when the number of icons
exceeds `terminal_max_icons` (see below), the taskbar only shows the first few plus a
`+N`, with the full list still available in the tooltip. The status directory is only
actually scanned once per second internally (throttled), independent of how often
TrafficMonitor calls `DataRequired` (once per second).

The plugin itself can't see what's happening inside the Claude Code process — that
information has to be reported by Claude Code proactively. It's reported via Claude
Code's hooks: every time a `SessionStart` / `UserPromptSubmit` / `PreToolUse` /
`Notification` / `Stop` / `StopFailure` / `SessionEnd` event fires, a small script is
invoked that writes the state to

```
<CLAUDE_CONFIG_DIR or %USERPROFILE%\.claude>\status\<session_id>.json
```

The plugin periodically scans this directory to count terminals and their states — no
extra network requests needed.

Why `StopFailure` is watched specifically: `Stop` only fires when a turn ends
**normally** and carries no error information at all; the actual failure signal
(rate limit `rate_limit`, server overload `overloaded`, auth failure
`authentication_failed`, etc. as `error_type`) only appears in `StopFailure`, so the
error state (🔴) has to be picked up from that event specifically.

`Notification` isn't treated as a single state: its `notification_type` values are quite
varied (`permission_prompt`, `idle_prompt`, `auth_success`, `elicitation_*`,
`agent_completed`, …), and most of them don't actually mean "needs your attention" at
all. Only permission confirmations / MCP dialogs map to yellow; `idle_prompt` (Claude
Code's own "are you still there" nudge) maps to gray rather than yellow, because it's
really just "the terminal is sitting idle," not genuinely waiting on a decision —
otherwise a conversation that already finished (🟢) sitting untouched for a while would
get incorrectly bumped back to yellow by this nudge. Other types
(`auth_success`/`elicitation_complete` etc.) are ignored outright and don't change the
current color.

Watching `PreToolUse` specifically fixes a "stuck yellow, never turns blue" bug:
approving a permission prompt (waiting, yellow) doesn't trigger a new
`UserPromptSubmit` — it's not a new turn of user input, Claude just continues the
current turn — so without this hook there was no event that could pull the state back
from waiting to thinking; the dot would stay yellow until the turn actually ended and
`Stop` turned it green, which was wrong for the whole time Claude was actively working
again. `PreToolUse` fires right before a tool actually runs, which is guaranteed to
happen once permission is granted, so it's used as the "yes, it's working again" signal.

### Configuring the hooks

1. The repo ships `tools\claude-hook-status.ps1`, the script the hooks call; the mapping
   from event to state is documented in the script's comments.
2. Add the following to the `hooks` section of Claude Code's global settings
   `%USERPROFILE%\.claude\settings.json` (swap the script path for the actual path on
   your machine):

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

   Replace `<repo>` with this repository's actual path, e.g.
   `D:\projects\1-test-space\6-rust\claude-usage-monitor`. If `settings.json` already
   has other hooks, just append this `hooks` entry into the matching event array —
   no need to replace the whole block.
3. Restart any already-open Claude Code terminals (hooks only take effect in new
   sessions).

Once configured, every Claude Code terminal you open adds a dot on the taskbar; exiting
a terminal normally (`/exit` or Ctrl+D) triggers `SessionEnd`, which automatically
cleans up the corresponding state file.

If a terminal's window is closed directly, or its process is force-killed,
`SessionEnd` doesn't get a chance to run — the plugin doesn't rely on waiting for a
timeout in this case: the state file records the PID of the process that owns this
terminal (found by the hook script walking up the process tree, skipping transient
shells like `powershell`/`cmd`), and on every scan the plugin checks whether that PID
is still alive via `OpenProcess`/`GetExitCodeProcess`; if the process is gone, the state
file is deleted immediately and the dot disappears right away, without waiting for
`terminal_stale_minutes` (default 360 minutes) to expire. Only state files written by an
older version, which lack a `pid` field, fall back to the timeout-based cleanup.

Sub-agents spawned via the Task/Agent tool aren't a terminal window a user can see; the
hook script skips writing a state file when it sees `agent_id`/`agent_type` in the event
payload (which is specific to sub-agents), so running sub-tasks doesn't add extra dots
to the taskbar.

### Related settings

```ini
[display]
terminal_stale_minutes=360
terminal_max_icons=12
terminal_thinking_color=3B82F6
terminal_idle_color=9E9E9E
```

| Key | Default | Description |
| --- | --- | --- |
| `terminal_stale_minutes` | `360` | A state file not updated for this many minutes is treated as a dead session and ignored |
| `terminal_max_icons` | `12` | Maximum number of icons shown on the taskbar; anything beyond that only appears in the tooltip (a `+N` follows the icons) |
| `terminal_thinking_color` | `3B82F6` | Color of the "Thinking" dot, `RRGGBB` hex |
| `terminal_idle_color` | `9E9E9E` | Color of the "Idle" dot, `RRGGBB` hex |

> "Waiting for input" uses `warn_color`, "Done" uses `normal_color`, "Error" uses
> `critical_color` — these are the same color settings used by the usage progress bar
> above; changing them affects both places.

---

## Where the data comes from

```
%USERPROFILE%\.claude\.credentials.json
        │
        │  reads claudeAiOauth.accessToken
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

If the `CLAUDE_CONFIG_DIR` environment variable is set, that directory is used instead
of `%USERPROFILE%\.claude`.

### How credentials are handled

- The access token only exists as a local variable for the duration of one request, and
  is zeroed with `SecureZeroMemory` immediately after use.
- The token is **never** written to a config file, cache, or log, and never shows up in
  the tooltip.
- The plugin only ever makes a single GET request; it never modifies any Claude Code
  file.
- When the token has expired, the API returns 401 and the plugin tells you in the
  tooltip to "please log in again in Claude Code." The plugin **doesn't** refresh the
  token itself using the refresh token (same behavior as cship) — renewal is left to
  Claude Code.

---

## Installation

### Option 1: use the prebuilt DLL

1. Copy `ClaudeUsageMonitor.dll` into TrafficMonitor's `plugins` directory, e.g.
   `D:\tools\TrafficMonitor\plugins\`.
2. Restart TrafficMonitor.
3. You should see *Claude Usage Monitor* under Options → Plugin Management.
4. In "Display settings" (set separately for the main window / taskbar window), check
   `Claude 5-hour usage` and `Claude 7-day usage`.

> The DLL's bitness must match `TrafficMonitor.exe`'s. This repo builds x64 by default;
> for the 32-bit build use `.\build.ps1 -Arch x86`.

### Option 2: build from source and install

```powershell
# Only the Visual Studio 2022 MSVC C++ toolchain is needed — no MFC / CMake / Node.js
.\build.ps1 -Install D:\tools\TrafficMonitor\plugins
```

---

## Configuration

After the first run, `ClaudeUsage.ini` is generated in TrafficMonitor's plugin config
directory (usually `<TrafficMonitor directory>\plugins\`), and every tunable value gets
written back to it; edit it and restart TrafficMonitor for changes to take effect.

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
```

| Key | Default | Description |
| --- | --- | --- |
| `refresh_interval` | `60` | Interval (seconds) between API requests; allowed range 60 ~ 3600 |
| `custom_draw` | `1` | Whether the plugin custom-draws the item. Set to `0` to fall back to the host program's layout, which loses threshold colors and the progress bar |
| `show_bar` | `1` | Whether to draw the progress bar under the text. Automatically skipped if the display area is too short, to keep the text readable |
| `warn_threshold` | `60` | Percentage above which the progress bar switches to the warning color |
| `critical_threshold` | `80` | Percentage above which the progress bar switches to the critical color |
| `bar_color_enabled` | `1` | Whether the progress bar is colored by threshold (green/yellow/red). Set to `0` to disable it and match the text color instead |
| `normal_color` | `9ECE6A` | Normal color (progress bar color below the warning threshold), `RRGGBB` hex |
| `warn_color` | `E0AF68` | Warning color, `RRGGBB` hex |
| `critical_color` | `F7768E` | Critical color, `RRGGBB` hex |

> The text color always uses the theme's default foreground color and never changes
> with the threshold — a light background with red/yellow text tends to blur together
> and become hard to read at a glance. Threshold coloring only shows up in the thin
> progress bar under the text.
| `*_label` | `5h` / `7d` | Prefix for the item. Leave empty to show only the value |
| `*_format` | `{pct}% ({reset})` | Value format string, see placeholders below |

Placeholders supported in the format string (matches cship's `five_hour_format`):

| Placeholder | Meaning | Example |
| --- | --- | --- |
| `{pct}` | Percentage used, rounded | `21` |
| `{remaining}` | Percentage remaining, rounded | `79` |
| `{reset}` | Time remaining until reset | `4h19m` |
| `{reset_at}` | Local wall-clock time of the reset | `19:42` or `08-19 09:00` |

A few layout examples:

```ini
five_hour_format={pct}% ({reset})      ; 5h 21% (4h19m)   default
five_hour_format={pct}% · {reset}      ; 5h 21% · 4h19m   middle-dot separator, narrower
five_hour_format={pct}%                ; 5h 21%           just the quota
five_hour_format={remaining}% left     ; 5h 79% left       "how much is left" framing
five_hour_format={pct}% →{reset_at}    ; 5h 21% →19:42    show the absolute reset time
```

The taskbar width is computed from the **worst case** of the format string (`100%`,
`23h59m`), so the item's width stays stable as the value changes and doesn't jitter
left and right.

**Why the default is 60 seconds and not something faster**: the countdown in parentheses
is computed locally from `resets_at` and refreshes every second regardless of request
frequency — only the percentage actually needs a request. And `/api/oauth/usage` is
rate-limited fairly tightly (especially easy to hit when multiple clients share the same
account), so keeping the interval from being too aggressive costs almost nothing in
practice while noticeably lowering the odds of getting rate-limited.

---

## Building

Prerequisite: Visual Studio 2022 with the "Desktop development with C++" workload.

```powershell
.\build.ps1                          # x64 Release, output to build\x64-Release\
.\build.ps1 -Arch x86                # 32-bit
.\build.ps1 -Config Debug            # with debug info
.\build.ps1 -Install <plugins dir>   # install right after building
.\build.ps1 -Arch all -Zip           # produce a release zip for both x64 and x86
```

`-Zip` packages the DLL alone into
`build\ClaudeUsageMonitor-<version>-<arch>.zip` (the version comes from `TMI_VERSION`
in `ClaudeUsagePlugin.cpp`); the zip contains only `ClaudeUsageMonitor.dll` — unzip it
straight into the `plugins` directory and it works, convenient for uploading as-is to a
GitHub Release. Combined with `-Arch all` it builds both architectures in one go and
produces two zips; `-Install` requires a single architecture and can't be combined with
`-Arch all`.

Build output:

- `ClaudeUsageMonitor.dll` — the plugin itself
- `Probe.exe` — a command-line probe, see below
- `HostTest.exe` — host test, see below

You can also open `ClaudeUsageMonitor.sln` directly in Visual Studio (it doesn't include
the probe or host test, which are only built via `build.ps1`).

No third-party dependencies: HTTPS uses the system's built-in WinHTTP, JSON uses a
~300-line read-only parser in this repo (`src/Json.*`). The CRT is statically linked
via `/MT`, so the target machine doesn't need any extra runtime.

---

## Troubleshooting: Probe.exe

`Probe.exe` runs the exact same fetch-and-format code as the plugin, but prints to the
console, so you can debug without launching TrafficMonitor. It only prints usage
numbers — **it never prints the token**.

```powershell
# Offline self-test: JSON parsing, ISO8601 parsing, time formatting
.\build\x64-Release\Probe.exe --selftest

# Actually hit the API and print usage
.\build\x64-Release\Probe.exe
```

### Previewing layout and colors

The threshold colors normally aren't visible — they only show up once you actually hit
60% / 80%. Set the `CLAUDE_USAGE_MONITOR_DEMO` environment variable to feed in specific
percentages directly, bypassing the API:

```powershell
# 5-hour window at 85% (critical color), 7-day window at 42% (normal color)
$env:CLAUDE_USAGE_MONITOR_DEMO = "85,42"
.\build\x64-Release\Probe.exe
```

To preview it inside TrafficMonitor, set this as a system environment variable, restart
TrafficMonitor, tune the format/colors, then remove it.

Common outputs and what they mean:

| Output | Cause and fix |
| --- | --- |
| Credentials file not found | You haven't logged into Claude Code yet, or `CLAUDE_CONFIG_DIR` points somewhere else |
| Can't read claudeAiOauth.accessToken from the credentials file | Credentials file format doesn't match what's expected — log into Claude Code again |
| Access token expired (HTTP 401) | Token expired, just log into Claude Code again |
| Too many requests (HTTP 429) | API is rate-limited, see the next section |
| Cannot connect to api.anthropic.com | Network/proxy issue. The plugin uses WinHTTP, which respects the system proxy settings |

### About HTTP 429

`/api/oauth/usage` rate-limits **before authentication**: even with no `Authorization`
header at all, a rate-limited request just returns

```json
{ "error": { "type": "rate_limit_error", "message": "Rate limited. Please try again later." } }
```

In other words this has nothing to do with whether your token is valid — it means the
quota for this endpoint, tied to your current egress IP / account, is used up (running
Claude Code, cship, and other usage scripts on the same account all draw from it).

This endpoint's quota is tight, and **running `Probe.exe` while TrafficMonitor already
has this plugin loaded often gets a 429** — they share the same account quota, and the
plugin side is usually already holding onto data it just fetched. If you want to debug
with the probe, quit TrafficMonitor first, or just check the plugin's tooltip directly.
You can confirm this has nothing to do with the plugin like this:

```powershell
Invoke-WebRequest https://api.anthropic.com/api/oauth/usage -SkipHttpErrorCheck | Select-Object StatusCode
```

If it returns 429 even without credentials, all you can do is wait for the quota to
recover. In this situation the plugin automatically backs off and retries (starting at
30 seconds, doubling each time, capped at 10 minutes), while keeping the last
successfully fetched data on display.

Some proxies/gateways block requests based on User-Agent; you can override it via an
environment variable:

```powershell
$env:CLAUDE_USAGE_MONITOR_UA = "ureq/3.1.2"
```

(Default is `TrafficMonitor-ClaudeUsage/1.0`.)

---

## Verification: HostTest.exe

`HostTest.exe` loads the plugin **exactly the way TrafficMonitor does** — `LoadLibrary`
+ `GetProcAddress("TMPluginGetInstance")` — then drives it through a full lifecycle in
the same order the host program does. It doesn't link against any of the plugin's
source code, so it genuinely verifies the exported functions, vtable layout, and
calling convention.

```powershell
.\build\x64-Release\HostTest.exe .\build\x64-Release\ClaudeUsageMonitor.dll
```

What it covers: interface version is 7, all six pieces of plugin info are non-empty, the
name/ID/label/sample text of both display items, item IDs contain only alphanumerics,
out-of-range and negative indices return null, plugin commands, a simulated 15-second
per-second `DataRequired` loop, tooltip content, double-click returns 1 while right-click
returns 0, `ShowOptionsDialog` returns `OR_OPTION_NOT_PROVIDED`.

The custom-drawing part uses the demo mode above to feed in fixed percentages, then
actually draws the item onto an in-memory bitmap and verifies it pixel by pixel: the
width is reasonable, a null `hDC` returns 0 so the host program falls back correctly,
something was actually written to the canvas, the bottom progress bar fills the full
row, the expected threshold colors appear (85% shows the critical color, 65% shows the
warning color), and `DrawItem` doesn't leave the DC's selected font dirty after
returning. Both dark and light themes are exercised.

---

## Implementation notes

```
src/
  Json.h / Json.cpp              Read-only JSON parser (no external dependencies)
  TimeUtil.h / TimeUtil.cpp      ISO8601 parsing, remaining-time formatting (matches cship)
  UsageApi.h / UsageApi.cpp      Reading credentials + WinHTTP request + response parsing + demo mode
  UsageService.h / UsageService.cpp   Background fetch thread, snapshot, retry backoff
  DisplayConfig.h / DisplayConfig.cpp Layout/color config, format-string placeholder substitution
  TerminalStatus.h / TerminalStatus.cpp Scans the terminal state files written by hooks
  ClaudeUsagePlugin.h / .cpp     ITMPlugin / IPluginItem implementation and exported functions
  DllMain.cpp                    Pins the module so no thread is left dangling on unload
include/
  PluginInterface.h              TrafficMonitor's official plugin interface (API version 7)
tools/
  Probe.cpp                      Command-line probe and offline self-test
  HostTest.cpp                   Host test (drives the plugin's full lifecycle via LoadLibrary)
  claude-hook-status.ps1         Claude Code hook script, reports terminal status (see "Claude terminal status" above)
```

A few deliberate design choices:

- **Never make a network request inside `DataRequired`.** That function is called by
  TrafficMonitor's UI thread once per second; blocking it would freeze the whole host
  program. The network request runs on its own thread, and `DataRequired` only reads the
  in-memory snapshot and recomputes the countdown text.
- **Keep showing the previous data if a fetch fails**, only noting the failure in the
  tooltip, to avoid the taskbar number flickering to `--` on every network hiccup.
- **The module is pinned in `DllMain`.** The plugin holds a long-lived thread; if the
  host program calls `FreeLibrary` on this DLL while it's running, that thread would
  execute code that's already been unloaded and crash. Pinning means the DLL only exits
  along with the process, so there's no need to wait for the thread to finish inside
  `DllMain` (which would hold the loader lock and risk a deadlock).
- **Custom-drawn text must use `DrawTextW`.** TrafficMonitor patches the `DrawText`
  family of functions in the plugin DLL's user32 import table — the taskbar's Direct2D
  rendering depends on this interception point; using `TextOut`/`ExtTextOut` instead
  produces nothing in D2D mode. Likewise, width is measured with
  `DrawTextW(DT_CALCRECT)` — the host program's own code notes that `GetTextExtent`
  gives only a theoretical width that isn't accurate enough.
- **The baseline text color for custom drawing comes from `EI_VALUE_TEXT_COLOR`.** The
  host program passes the current skin's color as a decimal `COLORREF` string via
  `OnExtenedInfo` before every `DrawItem` call. It's not read directly from the DC's
  `GetTextColor` because when the taskbar is running in Direct2D mode, the `DrawItem`
  call receives a temporary GDI DC that never had a text color set on it.
- **Failure backoff**: starts at 30 seconds, doubles each time, capped at 10 minutes;
  follows the server's `Retry-After` if one is given.
- **Display item IDs are `ClaudeUsageOAuth5h` / `ClaudeUsageOAuth7d`**, distinct from
  other Claude-related plugins so they can coexist.

---

## Differences from bemaru/trafficmonitor-ai-usage-plugin

Both show Claude usage on TrafficMonitor, but they fetch data in completely different
ways:

| | This plugin (cship approach) | bemaru plugin |
| --- | --- | --- |
| Data source | Anthropic OAuth usage API | claude.ai web page + cookies |
| Runtime dependencies | None | Node.js 22+, Edge/Chrome |
| Login | Reuses Claude Code credentials | Requires a separate browser login run |
| Composition | Single DLL | DLL + PowerShell + Node app + browser profile directory |
| Codex usage | Not supported | Supported |

The two plugins use different DLL names and display item IDs, so they can be installed
side by side.

---

## License

MIT
