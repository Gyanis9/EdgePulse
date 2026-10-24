# 卸载 EdgePulse Agent：停止并删除 Windows 服务、删除二进制；数据目录默认保留，-Purge 才删除。
[CmdletBinding()]
param(
    [string]$InstallDir = 'C:\Program Files\EdgePulse\Agent',
    [string]$DataDir = 'C:\ProgramData\EdgePulse\Agent',
    [string]$ServiceName = 'EdgePulseAgent',
    [switch]$Purge
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$scPath = Join-Path $env:SystemRoot 'System32\sc.exe'

function Resolve-DirectoryPath {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Option
    )
    if ([string]::IsNullOrWhiteSpace($Path)) {
        throw "$Option 不能为空"
    }
    if (-not [System.IO.Path]::IsPathRooted($Path)) {
        throw "$Option 必须是绝对路径：$Path"
    }
    if ($Path -match '[\r\n"]') {
        throw "$Option 不能包含换行或双引号：$Path"
    }
    $fullPath = [System.IO.Path]::GetFullPath($Path).TrimEnd('\')
    $rootPath = [System.IO.Path]::GetPathRoot($fullPath).TrimEnd('\')
    if ([string]::IsNullOrWhiteSpace($fullPath) -or $fullPath -eq $rootPath) {
        throw "$Option 不能是盘符根目录：$Path"
    }
    return $fullPath
}

function Assert-PurgeTarget {
    param([Parameter(Mandatory = $true)][string]$Path)
    $normalized = $Path.TrimEnd('\')
    $protectedDirectories = @()
    foreach ($candidate in @($env:SystemDrive, $env:SystemRoot, $env:ProgramFiles, ${env:ProgramFiles(x86)}, $env:ProgramData, $env:USERPROFILE)) {
        if (-not [string]::IsNullOrWhiteSpace($candidate)) {
            $protectedDirectories += $candidate.TrimEnd('\')
        }
    }
    foreach ($protectedDirectory in $protectedDirectories) {
        if ($normalized.Equals($protectedDirectory, [System.StringComparison]::OrdinalIgnoreCase)) {
            throw "拒绝删除系统目录：$Path"
        }
    }
    $parent = Split-Path -Parent $normalized
    if ([string]::IsNullOrWhiteSpace($parent) -or $parent -eq $normalized) {
        throw "拒绝删除过浅的目录：$Path"
    }
}

function Invoke-Sc {
    param(
        [Parameter(Mandatory = $true)][string]$Arguments,
        [Parameter(Mandatory = $true)][string]$Description
    )
    $standardOutputPath = [System.IO.Path]::GetTempFileName()
    $standardErrorPath = [System.IO.Path]::GetTempFileName()
    try {
        $process = Start-Process -FilePath $scPath -ArgumentList $Arguments -Wait -PassThru -NoNewWindow -RedirectStandardOutput $standardOutputPath -RedirectStandardError $standardErrorPath
        if ($process.ExitCode -ne 0) {
            $details = ''
            if (Test-Path -LiteralPath $standardErrorPath) {
                $details = [System.IO.File]::ReadAllText($standardErrorPath).Trim()
            }
            if ([string]::IsNullOrWhiteSpace($details) -and (Test-Path -LiteralPath $standardOutputPath)) {
                $details = [System.IO.File]::ReadAllText($standardOutputPath).Trim()
            }
            if ([string]::IsNullOrWhiteSpace($details)) {
                $details = '未返回错误文本'
            }
            throw "$Description 失败（sc.exe 退出码 $($process.ExitCode)）：$details"
        }
    } finally {
        Remove-Item -LiteralPath $standardOutputPath -Force -ErrorAction SilentlyContinue
        Remove-Item -LiteralPath $standardErrorPath -Force -ErrorAction SilentlyContinue
    }
}

function Assert-Administrator {
    $identity = [System.Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object System.Security.Principal.WindowsPrincipal($identity)
    if (-not $principal.IsInRole([System.Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw '卸载服务需要管理员权限：请以管理员身份打开 PowerShell 后重新运行 uninstall-agent.ps1'
    }
}

function Uninstall-Agent {
    Assert-Administrator

    if ($ServiceName -notmatch '^[A-Za-z0-9][A-Za-z0-9_.-]{0,255}$') {
        throw "服务名不合法：$ServiceName（必须以字母或数字开头，只允许字母、数字、点、下划线与连字符，最长 256 个字符）"
    }
    $InstallDir = Resolve-DirectoryPath -Path $InstallDir -Option '-InstallDir'
    $DataDir = Resolve-DirectoryPath -Path $DataDir -Option '-DataDir'
    if ($InstallDir -eq $DataDir -or $InstallDir.StartsWith($DataDir + '\', [System.StringComparison]::OrdinalIgnoreCase)) {
        throw '安装目录不能与数据目录相同，也不能位于数据目录之下：拒绝执行避免误删程序'
    }
    if ($Purge) {
        Assert-PurgeTarget -Path $DataDir
    }
    if (-not (Test-Path -LiteralPath $scPath -PathType Leaf)) {
        throw "找不到 sc.exe：$scPath"
    }

    $service = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
    if ($null -eq $service) {
        Write-Host "服务不存在，跳过停止与删除：$ServiceName"
    } else {
        if ($service.Status -ne 'Stopped') {
            # StopPending 阶段再发停止控制会被 SCM 拒绝，直接等待其完成
            if ($service.Status -ne 'StopPending') {
                Write-Host "正在停止服务：$ServiceName"
                Invoke-Sc -Arguments "stop $ServiceName" -Description "停止服务 $ServiceName"
            }
            try {
                $service.WaitForStatus('Stopped', [TimeSpan]::FromSeconds(30))
            } catch {
                throw "停止服务 $ServiceName 超时：请手动停止后重新运行卸载脚本"
            }
        }
        $service.Dispose()
        Invoke-Sc -Arguments "delete $ServiceName" -Description "删除服务 $ServiceName"

        $deadline = (Get-Date).AddSeconds(15)
        while ($null -ne (Get-Service -Name $ServiceName -ErrorAction SilentlyContinue)) {
            if ((Get-Date) -gt $deadline) {
                throw "服务 $ServiceName 已标记删除但仍在系统中：请重启后重新运行卸载脚本"
            }
            Start-Sleep -Milliseconds 500
        }
        Write-Host "已删除服务：$ServiceName"
    }

    foreach ($fileName in @('edgepulse-agent.exe', 'install-agent.ps1')) {
        $filePath = Join-Path $InstallDir $fileName
        if (Test-Path -LiteralPath $filePath -PathType Leaf) {
            Remove-Item -LiteralPath $filePath -Force
            Write-Host "已删除：$filePath"
        } else {
            Write-Host "文件不存在，跳过：$filePath"
        }
    }

    if ($Purge) {
        if (Test-Path -LiteralPath $DataDir) {
            Remove-Item -LiteralPath $DataDir -Recurse -Force
            Write-Host "已删除数据目录：$DataDir"
        } else {
            Write-Host "数据目录不存在，跳过：$DataDir"
        }
    } else {
        Write-Host "已保留数据目录：$DataDir（需要一并删除时加 -Purge）"
    }
}

try {
    Uninstall-Agent
} catch {
    [Console]::Error.WriteLine('卸载失败：' + $_.Exception.Message)
    exit 1
}
