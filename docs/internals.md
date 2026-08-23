# Internals and design notes

**English** | [简体中文](internals.zh-CN.md)

Back to the [README](../README.md).

This document collects the reasoning behind the non-obvious choices in the plugin — the
kind of thing that looks arbitrary until you hit the bug it was written to avoid.

---

## Source layout

```
src/
  Json.h / Json.cpp                     Read-only JSON parser (no external dependencies)
  TimeUtil.h / TimeUtil.cpp             ISO8601 parsing, remaining-time formatting (matches cship)
  UsageApi.h / UsageApi.cpp             Credential reading, WinHTTP request, response parsing, demo mode
  UsageService.h / UsageService.cpp     Background fetch thread, snapshot, retry backoff
  DisplayConfig.h / DisplayConfig.cpp   Layout and color config, format-string substitution
  TerminalStatus.h / TerminalStatus.cpp Scans the terminal state files written by the hooks
  ClaudeUsagePlugin.h / .cpp            ITMPlugin / IPluginItem implementation, exported functions
  DllMain.cpp                           Pins the module so no thread is left dangling on unload
include/
  PluginInterface.h                     TrafficMonitor's official plugin interface (API version 7)
tools/
  Probe.cpp                             Command-line probe and offline self-test
  HostTest.cpp                          Host test, drives the full lifecycle via LoadLibrary
  claude-hook-status.ps1                Claude Code hook script, reports terminal status
```

No third-party dependencies: HTTPS goes through the system's WinHTTP, JSON through the
~300-line read-only parser in `src/Json.*`, and the CRT is statically linked with `/MT`, so
the target machine needs no extra runtime.

---

## Plugin-side design choices

**Never make a network request inside `DataRequired`.** TrafficMonitor calls that function
from its UI thread once per second; blocking it would freeze the whole host program. The
request runs on its own thread, and `DataRequired` only reads the in-memory snapshot and
recomputes the countdown text.

**Keep showing the previous data when a fetch fails**, noting the failure in the tooltip
only. Otherwise the taskbar number flickers to `--` on every network hiccup.

**The module is pinned in `DllMain`.** The plugin holds a long-lived thread. If the host
called `FreeLibrary` on the DLL while that thread was running, it would execute unloaded
code and crash. Pinning means the DLL only goes away with the process, which also avoids
waiting for the thread inside `DllMain` — that would hold the loader lock and risk a
deadlock.

**Custom-drawn text must use `DrawTextW`.** TrafficMonitor patches the `DrawText` family in
the plugin DLL's user32 import table; the taskbar's Direct2D rendering depends on that
interception point. `TextOut` / `ExtTextOut` render nothing in D2D mode. Width is measured
with `DrawTextW(DT_CALCRECT)` for the same reason — the host's own code notes that
`GetTextExtent` gives only a theoretical width that isn't accurate enough.

**The baseline text color comes from `EI_VALUE_TEXT_COLOR`.** The host passes the current
skin's color as a decimal `COLORREF` string via `OnExtenedInfo` before every `DrawItem`
call. Reading `GetTextColor` off the DC doesn't work: in Direct2D mode `DrawItem` receives
a temporary GDI DC that never had a text color set on it.

**Failure backoff** starts at 30 seconds and doubles up to a 10-minute cap, honoring the
server's `Retry-After` when one is present.

**Display item IDs are `ClaudeUsageOAuth5h` / `ClaudeUsageOAuth7d`**, deliberately distinct
from other Claude-related plugins so they can coexist.

**The refresh interval defaults to 60 seconds, not something faster.** The countdown in
parentheses is computed locally from `resets_at` and updates every second regardless of
request frequency — only the percentage needs a request at all. Meanwhile
`/api/oauth/usage` is rate-limited fairly tightly, especially when several clients share an
account, so a relaxed interval costs nothing in practice and noticeably lowers the odds of
a 429.

---

## Why the dots are drawn, not typed

The status dots are drawn by the plugin with GDI (`Ellipse` plus a solid brush) rather than
being colored emoji characters.

An earlier version did use 🔵🟡🟢🔴 color-block emoji, and every one of them came out as
the same black-and-white outline. GDI's text drawing (`DrawTextW` and friends) doesn't
understand a font's built-in color palette — the COLR/CPAL tables that color emoji rely on
— so it draws glyph outlines in whatever single color `SetTextColor` last set. Real color
font rendering requires the dedicated DirectWrite/Direct2D APIs, which GDI text drawing
can't reach.

Drawing shapes sidesteps the problem entirely: the color comes purely from an RGB value in
code, using the same mechanism as the existing progress bar (`FillRect`), which was already
verified to render correctly on the taskbar. No dependency on fonts or the rendering path.

Color assignment follows the same logic. "Waiting" and "Done" and "Error" reuse the
existing `warn_color` / `normal_color` / `critical_color` tiers, so changing one place
updates both the bar and the dots. "Thinking" and "Idle" have no matching threshold color,
so they get their own `terminal_thinking_color` / `terminal_idle_color` settings.

---

## Why each hook event is watched

The plugin can't see inside the Claude Code process, so state has to be reported. Each of
the seven events earns its place:

**`StopFailure`, not just `Stop`.** `Stop` fires only when a turn ends *normally* and
carries no error information at all. The actual failure signal — `rate_limit`,
`overloaded`, `authentication_failed` and friends, arriving as `error_type` — appears only
in `StopFailure`. Without it, the red state would be unreachable.

**`PreToolUse` fixes a "stuck yellow, never turns blue" bug.** Approving a permission
prompt doesn't trigger a new `UserPromptSubmit` — it isn't a new turn of user input, Claude
is simply continuing the current one. So there was no event that could pull the state back
from waiting to thinking, and the dot stayed yellow until the turn ended and `Stop` turned
it green, which was wrong for the entire time Claude was actively working again.
`PreToolUse` fires right before a tool actually runs, which is guaranteed once permission
is granted, so it serves as the "yes, working again" signal.

**`Notification` is not treated as a single state.** Its `notification_type` values are
varied (`permission_prompt`, `idle_prompt`, `auth_success`, `elicitation_*`,
`agent_completed`, …) and most don't mean "needs your attention" at all:

- Permission confirmations and MCP dialogs map to yellow.
- `idle_prompt` — Claude Code's own "are you still there" nudge — maps to **gray**, not
  yellow. It really means "this terminal is sitting idle," not "a decision is pending."
  Mapping it to yellow would incorrectly bump a finished, green conversation back to
  yellow after it sat untouched for a while.
- Everything else (`auth_success`, `elicitation_complete`, …) is ignored outright and
  leaves the current color alone.

---

## Cleaning up dead terminals

Exiting a terminal normally (`/exit`, Ctrl+D) fires `SessionEnd`, which deletes that
session's state file.

Closing the window directly or force-killing the process gives `SessionEnd` no chance to
run — but the plugin doesn't fall back to waiting for a timeout. The state file records the
PID of the process that owns the terminal, found by the hook script walking up the process
tree and skipping transient shells like `powershell` and `cmd`. On every scan the plugin
checks whether that PID is still alive via `OpenProcess` / `GetExitCodeProcess`; if the
process is gone, the file is deleted immediately and the dot disappears at once, with no
wait for `terminal_stale_minutes` (default 360). Only state files written by an older
version, which lack a `pid` field, fall back to timeout-based cleanup.

Sub-agents spawned through the Task/Agent tool aren't terminal windows a user can see, so
the hook script skips writing a state file when it sees `agent_id` / `agent_type` in the
event payload. Running sub-tasks therefore don't add extra dots.

The status directory is scanned at most once per second internally, independent of how
often TrafficMonitor calls `DataRequired`.

---

## Verification: HostTest.exe

`HostTest.exe` loads the plugin exactly the way TrafficMonitor does — `LoadLibrary` plus
`GetProcAddress("TMPluginGetInstance")` — and then drives it through a full lifecycle in
the host's own call order. It links against none of the plugin's source, so it genuinely
verifies the exported functions, the vtable layout, and the calling convention.

```powershell
.\build\x64-Release\HostTest.exe .\build\x64-Release\ClaudeUsageMonitor.dll
```

It covers: interface version is 7; all six pieces of plugin info are non-empty; the name,
ID, label and sample text of both display items; item IDs contain only alphanumerics;
out-of-range and negative indices return null; plugin commands; a simulated 15-second
per-second `DataRequired` loop; tooltip content; double-click returns 1 while right-click
returns 0; `ShowOptionsDialog` returns `OR_OPTION_NOT_PROVIDED`.

The custom-drawing half uses demo mode to feed in fixed percentages, then actually draws
the item onto an in-memory bitmap and verifies it pixel by pixel: the width is reasonable,
a null `hDC` returns 0 so the host falls back correctly, something was actually written to
the canvas, the bottom progress bar fills its full row, the expected threshold colors show
up (85% critical, 65% warning), and `DrawItem` leaves the DC's selected font clean on
return. Both dark and light themes are exercised.
