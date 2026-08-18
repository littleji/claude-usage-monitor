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
    构建后把 DLL 复制到指定的 TrafficMonitor plugins 目录。

.EXAMPLE
    .\build.ps1
    .\build.ps1 -Install D:\tools\TrafficMonitor\plugins
#>
[CmdletBinding()]
param(
    [ValidateSet('x64', 'x86')]
    [string]$Arch = 'x64',

    [ValidateSet('Release', 'Debug')]
    [string]$Config = 'Release',

    [string]$Install,

    [switch]$SkipProbe
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$outDir = Join-Path $root "build\$Arch-$Config"
$objDir = Join-Path $outDir 'obj'

function Import-VcVars {
    param([string]$Architecture)

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

    # 在 cmd 里跑 vcvarsall，再把它设置的环境变量搬回当前 PowerShell 会话
    $output = & "$env:COMSPEC" /c "`"$vcvars`" $Architecture >nul 2>&1 && set"
    if ($LASTEXITCODE -ne 0) {
        throw "vcvarsall.bat $Architecture 执行失败。"
    }
    foreach ($line in $output) {
        if ($line -match '^([^=]+)=(.*)$') {
            Set-Item -Path "env:$($Matches[1])" -Value $Matches[2] -ErrorAction SilentlyContinue
        }
    }
    Write-Host "MSVC 环境就绪：$Arch" -ForegroundColor DarkGray
}

Import-VcVars -Architecture $Arch

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

if ($Install) {
    if (-not (Test-Path $Install)) {
        throw "安装目录不存在：$Install"
    }
    $target = Join-Path $Install 'ClaudeUsageMonitor.dll'
    Copy-Item $dllPath $target -Force
    Write-Host "已安装到 $target" -ForegroundColor Green
    Write-Host "请重启 TrafficMonitor 后在「选项 -> 插件管理」中启用。" -ForegroundColor Yellow
}
