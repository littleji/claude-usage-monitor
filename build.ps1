<#
.SYNOPSIS
    构建 TrafficMonitor 的 Claude 用量监控插件。

.DESCRIPTION
    只依赖 Visual Studio 2022 的 MSVC 工具链（不需要 MFC、不需要 CMake、
    不需要 Node.js）。会自动通过 vswhere 找到 VS 并导入 vcvars 环境。

.PARAMETER Arch
    目标架构，x64 或 x86。必须与 TrafficMonitor.exe 的位数一致。

.PARAMETER Config
    Release 或 Debug。

.PARAMETER Install
    构建后把 DLL 复制到指定的 TrafficMonitor plugins 目录。只能配合单一架构使用。

.PARAMETER Zip
    构建后把 DLL 单独打包成 build\ClaudeUsageMonitor-<版本>-<架构>.zip，
    版本号取自 ClaudeUsagePlugin.cpp 里的 TMI_VERSION，方便直接传到 GitHub Release。
    配合 -Arch all 会分别产出 x64 和 x86 两个 zip。

.EXAMPLE
    .\build.ps1
    .\build.ps1 -Install D:\tools\TrafficMonitor\plugins
    .\build.ps1 -Arch all -Zip
#>
[CmdletBinding()]
param(
    [ValidateSet('x64', 'x86', 'all')]
    [string]$Arch = 'x64',

    [ValidateSet('Release', 'Debug')]
    [string]$Config = 'Release',

    [string]$Install,

    [switch]$SkipProbe,

    [switch]$Zip,

    # 配合 -Install 使用：把 tools\claude-hook-status.ps1 拷到安装目录并在
    # %USERPROFILE%\.claude\settings.json 里注册七个 hooks（幂等，已存在则跳过）。
    [switch]$SetupHooks,

    # 配合 -Install 使用：把 tools\claude-thinking-watchdog.ps1 拷到安装目录
    # 并注册每分钟执行的计划任务。一般三方模型用户不需要——hook 脚本本身已经
    # 带 piggyback cleanup，下次事件触发时自动翻 stale thinking。仅当需要
    # 「终端完全空闲、无新事件时也主动清理」才开这个开关；开了会每分钟闪一次
    # powershell 窗口（除非额外加 -WindowStyle Hidden）。
    [switch]$SetupWatchdog
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$archList = if ($Arch -eq 'all') { @('x64', 'x86') } else { @($Arch) }

# Import-VcVars 会把当前会话的 PATH 整个替换成一份精简版，并且把 vcvarsall.bat
# 设置的 INCLUDE/LIB/LIBPATH/VSCMD_* 等一大堆环境变量原样搬进当前会话（见该
# 函数内的注释）。这个脚本经常被直接在日常交互用的 PowerShell 窗口里手动跑，
# 而不是专门开一个用完就关的临时窗口——如果不还原，构建一结束，同一个窗口里
# claude/npm/nvm 之类装在别处的命令就全都找不到了，而且这些改动会一直留到
# 关掉那个窗口为止，非常隐蔽。所以这里先把整个环境变量表快照下来，下面用
# try/finally 包住整个构建过程，保证不管成功还是中途报错退出，都会把会话的
# 环境变量精确还原成运行前的样子（脚本新增的变量会被删掉，改过的变量会被
# 改回原值）。
$originalEnv = @{}
Get-ChildItem Env: | ForEach-Object { $originalEnv[$_.Name] = $_.Value }

if ($Install -and $archList.Count -gt 1) {
    throw "-Install 只能配合单一架构使用（-Arch x64 或 -Arch x86），因为它要求 DLL 位数与 TrafficMonitor.exe 一致。"
}

function Get-PluginVersion {
    $source = Get-Content (Join-Path $root 'src\ClaudeUsagePlugin.cpp') -Raw
    if ($source -match 'TMI_VERSION:\s*\r?\n\s*return L"([^"]+)"') {
        return $Matches[1]
    }
    Write-Host "在 ClaudeUsagePlugin.cpp 里没找到 TMI_VERSION，zip 文件名将使用 'dev'" -ForegroundColor Yellow
    return 'dev'
}

function Import-VcVars {
    param([string]$Architecture)

    # 同一个会话里重复构建时直接复用已经初始化好的环境，省掉一次 cmd 往返
    if ($env:VSCMD_ARG_TGT_ARCH -eq $Architecture -and
        (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
        Write-Host "MSVC 环境就绪：$Architecture（复用当前会话）" -ForegroundColor DarkGray
        return
    }

    # 每次都从一份干净的系统 PATH 出发，而不是复用当前会话继承来的 PATH。
    # 早先的写法是"记下第一次调用时的 PATH，切换架构时还原回它"，但如果这个
    # PowerShell 会话本身已经开了很久、PATH 被各种工具或者本脚本改动前的
    # 历史运行撑到了将近 8191 字符，vcvarsall.bat 在其上再拼接自己那份路径时，
    # 单行长度一超限就会报 "The input line is too long"——而且这次是在
    # vcvarsall.bat 内部炸的，不是我们这边累积出来的，"还原回第一次的 PATH"
    # 这招根本救不了。构建过程里唯一靠 PATH 按名字查找的只有 vcvarsall
    # 加进去的 cl.exe / link.exe，系统目录之外的那部分 PATH 用不上，
    # 所以干脆不继承，每次都从系统目录重新拼。
    if ($null -eq $global:ClaudeUsageCleanPath) {
        $systemRoot = if ($env:SystemRoot) { $env:SystemRoot } else { 'C:\Windows' }
        $global:ClaudeUsageCleanPath = "$systemRoot\System32;$systemRoot;" +
            "$systemRoot\System32\Wbem;$systemRoot\System32\WindowsPowerShell\v1.0\"
    }
    $env:PATH = $global:ClaudeUsageCleanPath

    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) {
        throw "找不到 vswhere.exe，请先安装 Visual Studio 2022（含 [使用 C++ 的桌面开发] 工作负载）。"
    }

    $vsPath = & $vswhere -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath
    if (-not $vsPath) {
        throw "未找到带 MSVC C++ 工具链的 Visual Studio 安装。"
    }

    $vcvars = Join-Path $vsPath "VC\Auxiliary\Build\vcvarsall.bat"
    if (-not (Test-Path $vcvars)) {
        throw "找不到 vcvarsall.bat：$vcvars"
    }

    # 在 cmd 里跑 vcvarsall，再把它设置的环境变量搬回当前 PowerShell 会话。
    # 用一行标记把 vcvarsall 自己的输出和随后的 set 转储分开，
    # 失败时才有原始诊断信息可看（以前这里是 >nul 2>&1，出错只能看到一句"执行失败"）。
    $marker = '__VCVARS_OK__'
    $output = & "$env:COMSPEC" /c "`"$vcvars`" $Architecture 2>&1 && echo $marker && set"
    # cmd 的 `echo x && y` 会把 && 前面那个空格也一并输出，所以要 Trim 后再比
    $split = -1
    for ($i = 0; $i -lt $output.Count; $i++) {
        if ($output[$i].Trim() -eq $marker) { $split = $i; break }
    }
    if ($LASTEXITCODE -ne 0 -or $split -lt 0) {
        $detail = ($output | Select-Object -First 20) -join [Environment]::NewLine
        throw "vcvarsall.bat $Architecture 执行失败（退出码 $LASTEXITCODE）。`n$detail"
    }
    foreach ($line in $output[($split + 1)..($output.Count - 1)]) {
        if ($line -match '^([^=]+)=(.*)$') {
            Set-Item -Path "env:$($Matches[1])" -Value $Matches[2] -ErrorAction SilentlyContinue
        }
    }
    Write-Host "MSVC 环境就绪：$Architecture" -ForegroundColor DarkGray
}

# 把 hooks 注册进 %USERPROFILE%\.claude\settings.json：合并写入而不是覆盖——
# 已经有的事件 / 其他来源的 hooks 全保留。幂等：脚本路径匹配且事件名匹配就
# 当成已经装过，直接跳过；路径不一致（旧路径迁移）才追加新的并打 warning。
function Install-ClaudeHooks {
    param(
        [string]$ScriptPath,
        [string]$SettingsPath = (Join-Path $env:USERPROFILE '.claude\settings.json')
    )
    $settingsDir = Split-Path $settingsPath -Parent
    if (-not (Test-Path $settingsDir)) {
        New-Item -ItemType Directory -Force -Path $settingsDir | Out-Null
    }

    if (Test-Path $settingsPath) {
        $raw = Get-Content -Path $settingsPath -Raw -Encoding utf8
        try {
            $settings = $raw | ConvertFrom-Json -AsHashtable
        } catch {
            throw "settings.json 不是合法的 JSON：$settingsPath。请先备份再修正格式后重试。"
        }
    } else {
        $settings = @{}
    }
    if (-not $settings.ContainsKey('hooks')) { $settings['hooks'] = @{} }

    # 修复旧版本写错的占位符：build.ps1 的某个中间版本在 PowerShell 双引号
    # 字符串里写了 "{{Event}}"，本意是像 .NET 那样转义成 "{Event}"，但 PS
    # 双引号里 {} 是字面字符，结果写进 settings.json 的是 "-Event {Event}"，
    # Claude Code 把整个字符串作为参数原样传进来，脚本收到的事件名变成
    # 字面 "{Event}"，$statusMap 里查不到，整条链路就废了。检测并替换掉：
    # 对每个事件，把 command 里残留的 "{Event}" 换成当前事件名。
    foreach ($ev in @('SessionStart','UserPromptSubmit','PreToolUse','Notification','Stop','StopFailure','SessionEnd')) {
        if (-not $settings['hooks'].ContainsKey($ev)) { continue }
        foreach ($group in $settings['hooks'][$ev]) {
            if (-not $group.hooks) { continue }
            foreach ($h in $group.hooks) {
                if ($h.command -and $h.command -like '*{-Event}*') {
                    $h.command = $h.command -replace '\{Event\}', $ev
                }
            }
        }
    }

    $events = 'SessionStart','UserPromptSubmit','PreToolUse','Notification','Stop','StopFailure','SessionEnd'
    # {Event} 是占位符,后面 -replace 把它换成真实事件名。PowerShell 双引号
    # 字符串里 {} 是字面字符,不用像 format-string 那样写 {{}}
    $psCommand = "powershell -NoProfile -ExecutionPolicy Bypass -File `"$ScriptPath`" -Event {Event}"

    $added = 0; $skipped = 0
    foreach ($ev in $events) {
        if (-not $settings['hooks'].ContainsKey($ev)) { $settings['hooks'][$ev] = @() }
        $entry = [ordered]@{
            hooks = @(
                [ordered]@{
                    type    = 'command'
                    command = $psCommand -replace '\{Event\}', $ev
                }
            )
        }
        # 匹配：已存在任何一条 hook 的 command 同时含此脚本路径和此事件名 -> 跳过
        $already = $false
        foreach ($group in $settings['hooks'][$ev]) {
            if (-not $group.hooks) { continue }
            foreach ($h in $group.hooks) {
                if ($h.command -and $h.command -like "*`"$ScriptPath`"*" -and $h.command -like "*-Event $ev*") {
                    $already = $true; break
                }
            }
            if ($already) { break }
        }
        if ($already) {
            $skipped++
            continue
        }
        $settings['hooks'][$ev] += ,$entry
        $added++
    }

    # hashtable 转回 JSON：用深度 -2 让 @{} 渲染成 {}，@() 渲染成 []
    $json = $settings | ConvertTo-Json -Depth 10
    [System.IO.File]::WriteAllText($settingsPath, $json, [System.Text.UTF8Encoding]::new($false))
    Write-Host "Hooks: 新增 $added 个事件, 跳过 $skipped 个已存在" -ForegroundColor Green
    Write-Host "请重启已开的 Claude Code 终端 —— hooks 只在新会话里生效。" -ForegroundColor Yellow
}

function Install-ClaudeWatchdog {
    param([string]$ScriptPath)

    $taskName = 'ClaudeThinkingWatchdog'
    $existing = Get-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue
    if ($existing) {
        Unregister-ScheduledTask -TaskName $taskName -Confirm:$false
        Write-Host "已移除旧的 $taskName 任务" -ForegroundColor DarkGray
    }
    $action = New-ScheduledTaskAction -Execute 'powershell.exe' `
        -Argument "-NoProfile -ExecutionPolicy Bypass -File `"$ScriptPath`""
    $trigger = New-ScheduledTaskTrigger -Once -At (Get-Date) `
        -RepetitionInterval (New-TimeSpan -Minutes 1) `
        -RepetitionDuration (New-TimeSpan -Days 3650)
    Register-ScheduledTask -TaskName $taskName `
        -Action $action -Trigger $trigger `
        -RunLevel Limited `
        -Description 'Flip stuck Claude thinking status to done' | Out-Null
    Write-Host "看门狗已注册：$taskName（每分钟一次，路径 $ScriptPath）" -ForegroundColor Green
}

try {

$version = if ($Zip) { Get-PluginVersion } else { $null }
$zipPaths = @()

foreach ($buildArch in $archList) {

Import-VcVars -Architecture $buildArch

$outDir = Join-Path $root "build\$buildArch-$Config"
$objDir = Join-Path $outDir 'obj'
New-Item -ItemType Directory -Force $outDir | Out-Null
New-Item -ItemType Directory -Force $objDir | Out-Null

# /utf-8 是必须的：源码里有中文字符串字面量
$commonFlags = @(
    '/nologo', '/std:c++17', '/utf-8', '/W4', '/EHsc', '/GS',
    '/D_UNICODE', '/DUNICODE', '/DNOMINMAX', '/DWIN32_LEAN_AND_MEAN',
    '/D_WIN32_WINNT=0x0601',
    "/Fo$objDir\"
)
if ($Config -eq 'Release') {
    $commonFlags += @('/O2', '/DNDEBUG', '/MT', '/GL')
    $linkOpt = @('/LTCG', '/OPT:REF', '/OPT:ICF')
} else {
    $commonFlags += @('/Od', '/Zi', '/D_DEBUG', '/MTd', "/Fd$objDir\vc.pdb")
    $linkOpt = @('/DEBUG')
}

$dllSources = @(
    "$root\src\ClaudeUsagePlugin.cpp",
    "$root\src\UsageService.cpp",
    "$root\src\UsageApi.cpp",
    "$root\src\DisplayConfig.cpp",
    "$root\src\TerminalStatus.cpp",
    "$root\src\TimeUtil.cpp",
    "$root\src\Json.cpp",
    "$root\src\DllMain.cpp"
)

$dllPath = Join-Path $outDir 'ClaudeUsageMonitor.dll'

Write-Host "编译插件 -> $dllPath" -ForegroundColor Cyan
# 用 Write-Host 逐行输出：这样即便随后 throw，编译器的诊断信息也已经打出来了
& cl @commonFlags /LD $dllSources `
    /link /DLL @linkOpt "/OUT:$dllPath" `
    "/IMPLIB:$objDir\ClaudeUsageMonitor.lib" `
    winhttp.lib kernel32.lib user32.lib gdi32.lib advapi32.lib 2>&1 |
    ForEach-Object { Write-Host $_ }
if ($LASTEXITCODE -ne 0) { throw "插件编译失败（退出码 $LASTEXITCODE）。" }

if (-not $SkipProbe) {
    $probeSources = @(
        "$root\tools\Probe.cpp",
        "$root\src\UsageApi.cpp",
        "$root\src\DisplayConfig.cpp",
        "$root\src\TimeUtil.cpp",
        "$root\src\Json.cpp"
    )
    $probePath = Join-Path $outDir 'Probe.exe'
    Write-Host "编译探针 -> $probePath" -ForegroundColor Cyan
    & cl @commonFlags $probeSources `
        /link @linkOpt "/OUT:$probePath" `
        winhttp.lib kernel32.lib user32.lib 2>&1 |
        ForEach-Object { Write-Host $_ }
    if ($LASTEXITCODE -ne 0) { throw "探针编译失败（退出码 $LASTEXITCODE）。" }

    # 宿主测试：只通过 LoadLibrary 调用插件，不链接插件源码
    $hostTestPath = Join-Path $outDir 'HostTest.exe'
    Write-Host "编译宿主测试 -> $hostTestPath" -ForegroundColor Cyan
    & cl @commonFlags "$root\tools\HostTest.cpp" `
        /link @linkOpt "/OUT:$hostTestPath" kernel32.lib user32.lib gdi32.lib 2>&1 |
        ForEach-Object { Write-Host $_ }
    if ($LASTEXITCODE -ne 0) { throw "宿主测试编译失败（退出码 $LASTEXITCODE）。" }
}

Write-Host ""
Write-Host "构建完成：" -ForegroundColor Green
Get-ChildItem $outDir -File | Where-Object { $_.Extension -in '.dll', '.exe', '.pdb' } |
    Select-Object Name, @{n = 'KB'; e = { [math]::Round($_.Length / 1KB, 1) } } |
    Format-Table -AutoSize | Out-String | Write-Host

if ($Zip) {
    # 只打包 DLL 本身：装插件只需要这一个文件，塞探针/pdb 进去只会让用户糊涂
    $zipPath = Join-Path $root "build\ClaudeUsageMonitor-$version-$buildArch.zip"
    if (Test-Path $zipPath) { Remove-Item $zipPath -Force }
    Compress-Archive -Path $dllPath -DestinationPath $zipPath
    Write-Host "已打包 -> $zipPath" -ForegroundColor Green
    $zipPaths += $zipPath
}

if ($Install) {
    if (-not (Test-Path $Install)) {
        throw "安装目录不存在：$Install"
    }
    $target = Join-Path $Install 'ClaudeUsageMonitor.dll'
    # TrafficMonitor 运行期间会一直把插件 DLL 映射在自己进程里，这时覆盖不了。
    # 不拦的话只会抛一句 .NET 的"另一个进程正在使用"，看不出要关谁。
    $running = @(Get-Process TrafficMonitor -ErrorAction SilentlyContinue)
    if ($running.Count -gt 0) {
        throw ("TrafficMonitor 正在运行（PID {0}），插件 DLL 被占用。" -f ($running.Id -join ', ') +
               "请先退出 TrafficMonitor 再执行 -Install。")
    }
    Copy-Item $dllPath $target -Force
    Write-Host "已安装到 $target" -ForegroundColor Green
    Write-Host "请重启 TrafficMonitor 后在「选项 -> 插件管理」中启用。" -ForegroundColor Yellow

    if ($SetupHooks) {
        # PS1 装到 DLL 同目录，让 hooks 自包含；升级重跑会顺手覆盖到最新版
        $ps1Dest = Join-Path $Install 'claude-hook-status.ps1'
        Copy-Item (Join-Path $root 'tools\claude-hook-status.ps1') $ps1Dest -Force
        Install-ClaudeHooks -ScriptPath $ps1Dest
    }

    if ($SetupWatchdog) {
        $wdDest = Join-Path $Install 'claude-thinking-watchdog.ps1'
        Copy-Item (Join-Path $root 'tools\claude-thinking-watchdog.ps1') $wdDest -Force
        Install-ClaudeWatchdog -ScriptPath $wdDest
    }
}

}   # foreach ($buildArch in $archList)

if ($Zip -and $zipPaths.Count -gt 1) {
    Write-Host ""
    Write-Host "全部打包完成：" -ForegroundColor Green
    $zipPaths | ForEach-Object { Write-Host "  $_" }
}

} finally {
    # 不管上面成功还是中途 throw，都把调用者的环境变量精确还原成运行前的样子：
    # 构建过程中新增的变量删掉，改过的变量改回原值。先把当前变量名整个物化成
    # 数组再删——直接在 Get-ChildItem 的管道里删会一边遍历一边改集合。
    $currentNames = @(Get-ChildItem Env: | ForEach-Object { $_.Name })
    foreach ($name in $currentNames) {
        if (-not $originalEnv.ContainsKey($name)) {
            Remove-Item "Env:$name" -ErrorAction SilentlyContinue
        }
    }
    foreach ($name in $originalEnv.Keys) {
        Set-Item "Env:$name" -Value $originalEnv[$name] -ErrorAction SilentlyContinue
    }
}
