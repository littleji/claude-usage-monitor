<#
.SYNOPSIS
    Claude Code hook 脚本：把当前会话的状态写到 ClaudeUsageMonitor 插件能读到的地方。

.DESCRIPTION
    注意：默认安装用的是同等功能的原生版本 claude-hook-status.exe
    （tools/ClaudeHookStatus.cpp）。PostToolUse 每次工具调用都会触发，powershell
    每次启动要几百毫秒，会明显拖慢 Claude Code；本脚本只作为用不了 exe 时的备选
    保留。改动事件映射规则时两边要同步改。

    配合 ClaudeUsageMonitor 插件的"终端状态"显示项使用。插件本身看不到 Claude
    Code 内部在做什么，需要 Claude Code 通过 hooks 主动上报——本脚本就是被
    hooks 调用的那个命令，每次事件发生时把状态写到
        <CLAUDE_CONFIG_DIR 或 %USERPROFILE%\.claude>\status\<session_id>.json
    插件会定期扫描这个目录，按文件数量画圆点。

    子代理（Task/Agent 工具调用出去的子会话）不算用户看到的终端窗口——上报
    payload 里带 agent_id/agent_type 字段的就是子代理事件，本脚本直接跳过、
    不写文件，否则每跑一次子代理就会在任务栏多出一个圆点，数量对不上实际开着
    的终端数。

    终端被关掉（不管是 /exit 正常退出还是直接把窗口叉掉/强杀进程）之后，
    对应的圆点应该马上消失，不能指望 SessionEnd hook——窗口被强杀时进程都没了，
    hook 根本来不及跑。所以本脚本还会顺着进程树往上找到拥有这个终端的长驻进程
    （跳过 powershell/cmd/conhost 这类中转用的 shell），把它的 PID 写进状态
    文件；插件扫描时用这个 PID 检查进程是否还活着，死了就直接删文件，
    不用等超时。PID 只在第一次（通常是 SessionStart）用 WMI 查一次，
    后续事件复用已写入文件里的 PID，避免每条消息都发一次 WMI 查询拖慢响应。

    -Event 由 settings.json 里的 hook 配置传入，取值与 Claude Code 的 hook
    事件名一致：SessionStart / UserPromptSubmit / PostToolUse /
    PostToolUseFailure / Notification / Stop / StopFailure / SessionEnd。
    事件到状态的映射：
        SessionStart       -> idle      （会话刚打开，等待第一条输入）
        UserPromptSubmit   -> thinking  （已提交请求，正在处理）
        PostToolUse        -> thinking  （工具执行完，Claude 接着干活；见下方说明）
        PostToolUseFailure -> thinking  （同上；用户按 Esc 打断的除外）
        Stop               -> done      （一轮回复正常结束）
        StopFailure        -> error     （一轮回复因 API 错误异常终止）
        SessionEnd         -> 删除状态文件（会话正常退出）

    专门监听 PostToolUse 是为了修复"黄色卡死不变蓝"的问题：权限确认或
    AskUserQuestion 提问（Notification -> waiting）之后，用户点"允许"或答完
    问题并不会触发新的 UserPromptSubmit（这不是一轮新的用户输入，Claude 只是
    接着跑当前这轮），没有任何 hook 事件把状态从 waiting 拉回 thinking，
    圆点就一直黄着，直到这一轮真正结束才因为 Stop 变绿。
    早期版本用的是 PreToolUse，但它的触发顺序是
        PreToolUse -> 弹权限框(Notification) -> 用户批准 -> 工具执行 -> PostToolUse
    PreToolUse 在弹框之前就已经跑完了，批准后不会再触发，根本修不了这个问题。
    PostToolUse / PostToolUseFailure 在用户做完决定、工具跑完之后才触发，
    才是"确实又开始干活了"的信号。PostToolUseFailure 带 is_interrupt=true
    时是用户按 Esc 打断，这一轮已经停了，不能翻回 thinking。

    Notification 不是单一状态，得按 notification_type 细分——它的取值很杂，
    大部分根本不代表"需要用户处理"：
        permission_prompt / elicitation_dialog / elicitation_url_dialog /
        agent_needs_input                -> waiting （真的需要用户做决定）
        idle_prompt                      -> idle    （Claude Code 自己发的
                                                       "你还在吗"提示，本质就是
                                                       终端在空闲，不是真在等
                                                       用户处理什么）
        auth_success / elicitation_complete / elicitation_response /
        agent_completed 等其他类型       -> 不写文件，保持当前状态不变
    早期版本把所有 Notification 一律映射成 waiting，结果一个刚完成的对话
    （done，绿色）只要放着不动一段时间，Claude Code 发个 idle_prompt 提示，
    颜色就被错误地顶回黄色；同时 SessionStart 早期也误映射成了 waiting，
    导致刚打开、还没输入过的窗口显示黄色而不是灰色。这里都已经改过来。

    专门监听 StopFailure 是因为 Stop 事件只在正常结束时触发，本身不带错误信息；
    真正的失败信号（限流 rate_limit、服务端过载 overloaded、鉴权失败
    authentication_failed 等）只出现在 StopFailure 的 error_type 字段里。

    Claude Code 会把本次事件的 JSON 通过 stdin 传进来（包含 session_id、cwd、
    以及 StopFailure 独有的 error_type 等），本脚本从里面取 session_id 作为
    文件名、取 cwd 写进文件方便鼠标提示里辨认目录，取 error_type 写进文件方便
    鼠标提示里显示具体的错误类型。

.PARAMETER Event
    触发本脚本的 hook 事件名。
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Event
)

$ErrorActionPreference = 'SilentlyContinue'

# 临时诊断日志：每次调用都追加一行，记录事件名和最终写了什么状态（或者为什么
# 没写）。用来确认 Stop 到底有没有真的触发、走到了哪一步——排查完"完成后不
# 变绿"这个问题就可以把这段删掉。
$debugLogPath = Join-Path (Join-Path $(if ($env:CLAUDE_CONFIG_DIR) { $env:CLAUDE_CONFIG_DIR } else { Join-Path $env:USERPROFILE '.claude' }) '') 'hook-debug.log'
function Write-DebugLog {
    param([string]$Message)
    $line = "$([DateTime]::Now.ToString('HH:mm:ss.fff')) pid=$PID event=$Event $Message"
    Add-Content -Path $debugLogPath -Value $line -Encoding utf8 -ErrorAction SilentlyContinue
}
Write-DebugLog "invoked"

$statusMap = @{
    'SessionStart'     = 'idle'
    'UserPromptSubmit'   = 'thinking'
    'PreToolUse'         = 'thinking'   # 旧版安装留下的配置，保留映射以免报未知事件
    'PostToolUse'        = 'thinking'
    'PostToolUseFailure' = 'thinking'
    'Stop'               = 'done'
    'StopFailure'        = 'error'
}

# Notification 需要按 notification_type 细分，不能整体归到一种状态——
# 见上面 .DESCRIPTION 里的说明。
$notificationNeedsAttention = @('permission_prompt', 'elicitation_dialog',
                                 'elicitation_url_dialog', 'agent_needs_input')

# 顺着进程树往上找拥有这个终端的长驻进程：hook 命令本身跑在一个短命的
# powershell（可能还套了一层 cmd）里，这些中转层退出后 PID 就失效了，
# 得跳过它们，找到真正会陪终端一起活到关闭的那个进程。
function Get-OwnerProcessId {
    $shellNames = @('powershell.exe', 'pwsh.exe', 'cmd.exe', 'conhost.exe',
                     'sh.exe', 'bash.exe', 'wsl.exe')
    $currentId = $PID
    for ($i = 0; $i -lt 8; $i++) {
        $parentId = (Get-CimInstance Win32_Process -Filter "ProcessId=$currentId" `
                     -ErrorAction SilentlyContinue).ParentProcessId
        if (-not $parentId) { return $currentId }
        $parent = Get-CimInstance Win32_Process -Filter "ProcessId=$parentId" `
                  -ErrorAction SilentlyContinue
        if (-not $parent) { return $currentId }
        if ($parent.Name -notin $shellNames) {
            # 后台会话（claude --bg）由 daemon 的 claude.exe --bg-pty-host 拉起，
            # 会话进程的直接父进程也是 claude.exe；前台终端的父进程是 shell 或
            # headroom 之类的包装器。
            $grand = Get-CimInstance Win32_Process -Filter "ProcessId=$($parent.ParentProcessId)" `
                     -ErrorAction SilentlyContinue
            $script:isBackground = ($parent.Name -ieq 'claude.exe') -and $grand -and ($grand.Name -ieq 'claude.exe')
            return [int]$parentId
        }
        $currentId = [int]$parentId
    }
    return $currentId   # 找了 8 层还没找到就放弃，返回最后一层，好过没有
}

# 不能用 [Console]::In——它按系统代码页（中文 Windows 通常是 GB2312/936）解码,
# 而 Claude Code 传进来的 stdin 固定是 UTF-8。回复文本一带中文,多字节序列被
# 错误代码页拆开就会产生野字符,把 JSON 字符串截断,ConvertFrom-Json 直接失败——
# 解析失败后拿不到真实 session_id,只能落到"生成新 GUID 单开一个文件"的兜底
# 分支,真正那个会话的状态文件就再也不会被这次事件更新到（旧状态一直卡住,
# 圆点颜色对不上）。绕开 Console 的代码页,直接用 UTF8 编码读原始字节流。
$stdinReader = New-Object System.IO.StreamReader([Console]::OpenStandardInput(), [System.Text.Encoding]::UTF8)
$stdin = $stdinReader.ReadToEnd()
$payload = $null
$parseError = $null
if ($stdin) {
    try { $payload = $stdin | ConvertFrom-Json } catch { $parseError = $_.Exception.Message }
}
Write-DebugLog "stdin_len=$($stdin.Length) parsed=$($null -ne $payload) parse_error=$parseError"

# 子代理事件不代表一个用户看得见的终端窗口——但只有 agent_type 是显式的
# 子代理标记，单凭 agent_id 不能判。某些三方网关会让普通事件也带 agent_id
# （用于内部路由/计费），老版本只要看到 agent_id 就跳过，结果三方模型下
# 所有事件被一律跳过，圆点根本不出现。现在只看 agent_type，并依赖 C++ 端
# 按 pid 去重（同进程的子代理和主会话共享 pid，去重时主会话因为有 cwd 会被
# 选中，子代理自然被顶掉），不用在这里提前过滤。
if ($payload -and $payload.agent_type) {
    Write-DebugLog "skip: subagent event (agent_id=$($payload.agent_id) agent_type=$($payload.agent_type))"
    exit 0
}

$sessionId = if ($payload -and $payload.session_id) { [string]$payload.session_id } else { '' }
if (-not $sessionId) {
    # 拿不到 session_id 时不要用一个固定的占位字符串（比如 "unknown"）——
    # 不同终端都落到同一个文件名上会互相覆盖，状态显示就变成一笔糊涂账。
    # 随机生成一个，效果是这个事件单独占一个文件，过期后按 stale 规则自然消失。
    $sessionId = [guid]::NewGuid().ToString()
    Write-DebugLog "WARNING: no session_id in payload, generated fallback guid=$sessionId"
}
$cwd = if ($payload -and $payload.cwd) { [string]$payload.cwd } else { '' }
$errorType = if ($payload -and $payload.error_type) { [string]$payload.error_type } else { '' }

$configDir = $env:CLAUDE_CONFIG_DIR
if (-not $configDir) { $configDir = Join-Path $env:USERPROFILE '.claude' }
$statusDir = Join-Path $configDir 'status'
New-Item -ItemType Directory -Force -Path $statusDir | Out-Null

# 三方网关偶尔会吞掉 stop_reason,客户端识别不到一轮结束,Stop 事件不发,
# 状态文件就卡在 thinking。靠单独的计划任务去扫会每分钟弹一个 powershell
# 窗口——直接挂在这个 hook 里:每次事件触发(用户提交 / 工具执行 / 通知等)
# 顺手扫一遍状态目录,超过 -StaleThinkingMinutes 分钟还在 thinking 的就翻
# done。零额外进程,终端不闪。代价是空闲终端(没有任何事件)里的卡死
# thinking 不会被翻——但用户下一次发新 prompt 时这条 hook 必然触发,
# 上一轮的卡死也会一起被处理掉,所以覆盖绝大多数场景。
function Cleanup-StaleThinking {
    param([string]$StatusDir, [int]$StaleMinutes)
    $thresholdSec = $StaleMinutes * 60
    $now = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
    Get-ChildItem -Path $StatusDir -Filter '*.json' -File -ErrorAction SilentlyContinue | ForEach-Object {
        $raw = Get-Content -Path $_.FullName -Raw -ErrorAction SilentlyContinue
        if (-not $raw) { return }
        $obj = $null
        try { $obj = $raw | ConvertFrom-Json -ErrorAction Stop } catch { return }
        if ($obj.status -ne 'thinking') { return }
        if (($now - [int]$obj.updated_at) -le $thresholdSec) { return }

        $ownerPid = 0
        if ($obj.pid) { $ownerPid = [int]$obj.pid }
        # 进程已死:终端被叉掉 / 进程被强杀,直接删文件,圆点立刻消失
        if ($ownerPid -gt 0) {
            $alive = $true
            try {
                $proc = [System.Diagnostics.Process]::GetProcessById($ownerPid)
                $alive = -not $proc.HasExited
            } catch { $alive = $false }
            if (-not $alive) {
                Remove-Item -Path $_.FullName -Force -ErrorAction SilentlyContinue
                return
            }
        }
        # 翻 done,不翻 error——客户端没报错才走到 Stop,Stop 不发大概率是网关
        # 吞了 stop_reason,不是真出错;真出错 StopFailure 已经写了 error
        $newRecord = [ordered]@{
            session_id    = $obj.session_id
            status        = 'done'
            updated_at    = $now
            cwd           = $obj.cwd
            error_type    = $obj.error_type
            pid           = $ownerPid
            watchdog_flip = $true
        }
        ($newRecord | ConvertTo-Json -Compress) | Set-Content -Path $_.FullName -Encoding utf8 -NoNewline
    }
}
Cleanup-StaleThinking -StatusDir $statusDir -StaleMinutes 5

# 文件名只用会话 id，不掺目录名之类的东西，避免非法字符
$safeId = ($sessionId -replace '[^A-Za-z0-9\-]', '_')
$file = Join-Path $statusDir "$safeId.json"

Write-DebugLog "session_id=$sessionId file=$file"

if ($Event -eq 'SessionEnd') {
    Remove-Item -Path $file -Force -ErrorAction SilentlyContinue
    Write-DebugLog "SessionEnd: deleted $file"
    exit 0
}

if ($Event -eq 'Notification') {
    $notificationType = if ($payload -and $payload.notification_type) {
        [string]$payload.notification_type
    } else {
        ''
    }
    if ($notificationType -eq 'idle_prompt') {
        $status = 'idle'
    } elseif ($notificationNeedsAttention -contains $notificationType) {
        $status = 'waiting'
    } else {
        # auth_success / elicitation_complete / agent_completed 等不代表
        # "需要用户处理"，不改变当前已经写好的状态，直接不写文件
        Write-DebugLog "skip: notification_type=$notificationType (不改变状态)"
        exit 0
    }
} elseif ($Event -eq 'PostToolUseFailure' -and $payload -and $payload.is_interrupt) {
    # 用户按 Esc 打断工具：这一轮已经停了，不会再有 Stop，别翻回 thinking
    Write-DebugLog "skip: PostToolUseFailure is_interrupt=true (不改变状态)"
    exit 0
} else {
    $status = $statusMap[$Event]
    if (-not $status) {
        # 未知事件名：不写文件，也不报错，避免因为 hook 配置改动而炸掉整个事件链
        Write-DebugLog "skip: unknown event, no mapping"
        exit 0
    }
}

# 同一个会话只在第一次事件时用 WMI 查一次拥有者 PID，后续事件复用文件里
# 已经记好的 PID——WMI 查询比较慢，没必要每条消息都查一遍。
$ownerPid = 0
if (Test-Path $file) {
    try {
        $existing = Get-Content $file -Raw | ConvertFrom-Json
        if ($existing.pid) { $ownerPid = [int]$existing.pid }
    } catch {}
}
# 后台会话不需要用户操作，不上报（顺手清掉已经写出去的旧状态文件）。
# 每次事件都要判断，所以这里总是走一遍进程树。
$script:isBackground = $false
$treeOwnerPid = Get-OwnerProcessId
if ($script:isBackground) {
    Remove-Item -Path $file -Force -ErrorAction SilentlyContinue
    Write-DebugLog "skip: background session owner_pid=$treeOwnerPid"
    exit 0
}
if ($ownerPid -le 0) {
    $ownerPid = $treeOwnerPid
}

$now = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
$record = [ordered]@{
    session_id = $sessionId
    status     = $status
    updated_at = $now
    cwd        = $cwd
    error_type = $errorType
    pid        = $ownerPid
}
($record | ConvertTo-Json -Compress) | Set-Content -Path $file -Encoding utf8 -NoNewline
Write-DebugLog "wrote: status=$status pid=$ownerPid -> $file"

exit 0
