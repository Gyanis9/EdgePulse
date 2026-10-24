# 安装 EdgePulse Agent 为 Windows 服务：落二进制与配置模板、收紧目录权限、注册服务并配置失败恢复；
# 加 -Upgrade 时按健康检查窗口替换单个二进制，失败自动恢复上一版。
[CmdletBinding()]
param(
    [string]$InstallDir = 'C:\Program Files\EdgePulse\Agent',
    [string]$DataDir = 'C:\ProgramData\EdgePulse\Agent',
    [string]$ServiceName = 'EdgePulseAgent',
    [switch]$NoStart,
    [string]$Upgrade = '',
    [int]$HealthTimeout = 60,
    [string]$StateDir = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$defaultInstallDir = 'C:\Program Files\EdgePulse\Agent'
$defaultDataDir = 'C:\ProgramData\EdgePulse\Agent'
$administratorsSid = '*S-1-5-32-544'
$systemSid = '*S-1-5-18'
$usersSid = '*S-1-5-32-545'
$scPath = Join-Path $env:SystemRoot 'System32\sc.exe'
$icaclsPath = Join-Path $env:SystemRoot 'System32\icacls.exe'

function Assert-SafeTargetPath {
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
    if ($Path -notmatch '^[A-Za-z]:\\') {
        throw "$Option 只支持带盘符的本地绝对路径：$Path"
    }

    $fullPath = [System.IO.Path]::GetFullPath($Path).TrimEnd('\')
    $rootPath = [System.IO.Path]::GetPathRoot($fullPath).TrimEnd('\')
    if ([string]::IsNullOrWhiteSpace($fullPath) -or $fullPath -eq $rootPath) {
        throw "$Option 不能是盘符根目录：$Path"
    }

    $protectedDirectories = @(
        $env:SystemDrive,
        $env:SystemRoot,
        (Join-Path $env:SystemRoot 'System32'),
        $env:ProgramFiles,
        ${env:ProgramFiles(x86)},
        $env:ProgramData,
        $env:USERPROFILE,
        (Join-Path $env:SystemDrive 'Users')
    )
    foreach ($protectedDirectory in $protectedDirectories) {
        if (-not [string]::IsNullOrWhiteSpace($protectedDirectory) -and $fullPath.Equals($protectedDirectory.TrimEnd('\'), [System.StringComparison]::OrdinalIgnoreCase)) {
            throw "$Option 不能指向系统目录：$fullPath"
        }
    }

    foreach ($systemDirectory in @($env:SystemRoot, (Join-Path $env:SystemDrive 'Users'))) {
        if ($fullPath.StartsWith($systemDirectory.TrimEnd('\') + '\', [System.StringComparison]::OrdinalIgnoreCase)) {
            throw "$Option 不能位于系统目录之下：$fullPath"
        }
    }

    foreach ($sharedRoot in @($env:ProgramFiles, ${env:ProgramFiles(x86)}, $env:ProgramData)) {
        if (-not [string]::IsNullOrWhiteSpace($sharedRoot)) {
            $normalizedRoot = $sharedRoot.TrimEnd('\')
            if ($fullPath.StartsWith($normalizedRoot + '\', [System.StringComparison]::OrdinalIgnoreCase)) {
                $firstComponent = (($fullPath.Substring($normalizedRoot.Length + 1)) -split '\\')[0]
                if (-not $firstComponent.Equals('EdgePulse', [System.StringComparison]::OrdinalIgnoreCase)) {
                    throw "$Option 位于 $normalizedRoot 时第一级目录必须是 EdgePulse：$fullPath"
                }
            }
        }
    }

    $relativePath = $fullPath.Substring($rootPath.Length).Trim('\')
    $components = @($relativePath -split '\\' | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
    if ($components.Count -lt 2) {
        throw "$Option 至少要在盘符下有两级目录：$fullPath"
    }
    return $fullPath
}

function Invoke-NativeCapture {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [Parameter(Mandatory = $true)][string[]]$Arguments
    )
    # 原生命令写 stderr 时，PS 5.1 在 Stop 偏好下会把错误记录当成终止错误；先降为 Continue，
    # 再把错误记录还原成原生文本，最后统一检查退出码
    $previousErrorAction = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $lines = @()
        & $FilePath @Arguments 2>&1 | ForEach-Object {
            if ($_ -is [System.Management.Automation.ErrorRecord]) {
                $lines += $_.Exception.Message
            } else {
                $lines += [string]$_
            }
        }
        $exitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previousErrorAction
    }
    return [pscustomobject]@{ ExitCode = $exitCode; Output = (($lines -join [Environment]::NewLine).Trim()) }
}

function Invoke-IcaclsCommand {
    param(
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [Parameter(Mandatory = $true)][string]$Description
    )
    $result = Invoke-NativeCapture -FilePath $icaclsPath -Arguments $Arguments
    if ($result.ExitCode -ne 0) {
        $details = $result.Output
        if ([string]::IsNullOrWhiteSpace($details)) {
            $details = '未返回错误文本'
        }
        throw "设置 $Description 的访问控制失败（icacls 退出码 $($result.ExitCode)）：$details。请确认当前账户是管理员且目标路径存在"
    }
}

function Set-RestrictedDirectoryAcl {
    param([Parameter(Mandatory = $true)][string]$Path)
    # 先复位到继承默认值，清掉历史上可能残留的显式授权，再关闭继承并只保留管理员与 SYSTEM
    Invoke-IcaclsCommand -Arguments @($Path, '/reset') -Description "目录 $Path"
    Invoke-IcaclsCommand -Arguments @($Path, '/inheritance:r') -Description "目录 $Path"
    Invoke-IcaclsCommand -Arguments @($Path, '/grant:r', $administratorsSid + ':(OI)(CI)F', $systemSid + ':(OI)(CI)F') -Description "目录 $Path"
}

function Set-RestrictedFileAcl {
    param([Parameter(Mandatory = $true)][string]$Path)
    Invoke-IcaclsCommand -Arguments @($Path, '/reset') -Description "文件 $Path"
    Invoke-IcaclsCommand -Arguments @($Path, '/inheritance:r') -Description "文件 $Path"
    Invoke-IcaclsCommand -Arguments @($Path, '/grant:r', $administratorsSid + ':F', $systemSid + ':F') -Description "文件 $Path"
}

function Set-InstallDirectoryAcl {
    param([Parameter(Mandatory = $true)][string]$Path)
    Invoke-IcaclsCommand -Arguments @($Path, '/reset') -Description "安装目录 $Path"
    Invoke-IcaclsCommand -Arguments @($Path, '/inheritance:r') -Description "安装目录 $Path"
    Invoke-IcaclsCommand -Arguments @($Path, '/grant:r', $administratorsSid + ':(OI)(CI)F', $systemSid + ':(OI)(CI)F', $usersSid + ':(OI)(CI)RX') -Description "安装目录 $Path"
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

function Get-ScQueryExText {
    param([Parameter(Mandatory = $true)][string]$ServiceName)
    $result = Invoke-NativeCapture -FilePath $scPath -Arguments @('queryex', $ServiceName)
    if ($result.ExitCode -ne 0) {
        $details = $result.Output
        if ([string]::IsNullOrWhiteSpace($details)) {
            $details = '未返回错误文本'
        }
        throw "查询服务 $ServiceName 状态失败（sc.exe 退出码 $($result.ExitCode)）：$details"
    }
    return $result.Output
}

function Wait-ServiceThroughStartPending {
    param(
        [Parameter(Mandatory = $true)]$Service,
        [Parameter(Mandatory = $true)][string]$ServiceName,
        [int]$TimeoutSeconds = 30
    )
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while ($Service.Status -eq 'StartPending') {
        if ((Get-Date) -gt $deadline) {
            throw "服务 $ServiceName 在 $TimeoutSeconds 秒内仍未结束启动阶段：请手动确认服务状态后重新运行安装脚本"
        }
        Start-Sleep -Milliseconds 250
        $Service.Refresh()
    }
}

function Assert-ServiceRunning {
    param(
        [Parameter(Mandatory = $true)][string]$ServiceName,
        [Parameter(Mandatory = $true)][string]$LogPath
    )
    Start-Sleep -Seconds 2
    $queryText = Get-ScQueryExText -ServiceName $ServiceName
    $stateMatch = [regex]::Match($queryText, 'STATE\s*:\s*(\d+)')
    $exitMatch = [regex]::Match($queryText, 'SERVICE_EXIT_CODE\s*:\s*(\d+)')
    $state = -1
    if ($stateMatch.Success) {
        $state = [int]$stateMatch.Groups[1].Value
    }
    $serviceExitCode = -1
    if ($exitMatch.Success) {
        $serviceExitCode = [int]$exitMatch.Groups[1].Value
    }
    if ($state -ne 4 -or $serviceExitCode -ne 0) {
        throw "服务 $ServiceName 启动后未保持运行（STATE=$state，SERVICE_EXIT_CODE=$serviceExitCode）：sc.exe queryex 输出：`n$queryText`n请查看 $LogPath 与事件日志（Get-EventLog -LogName System -Source 'Service Control Manager'）"
    }
    Write-Host "服务状态读数：`n$queryText"
}

function Assert-Administrator {
    $identity = [System.Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object System.Security.Principal.WindowsPrincipal($identity)
    if (-not $principal.IsInRole([System.Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw '安装服务需要管理员权限：请以管理员身份打开 PowerShell 后重新运行 install-agent.ps1'
    }
}

function Write-UpgradeLog {
    param(
        [Parameter(Mandatory = $true)][string]$LogPath,
        [Parameter(Mandatory = $true)][string]$Message
    )
    $line = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ') + ' ' + $Message
    [System.IO.File]::AppendAllText($LogPath, $line + [Environment]::NewLine)
    Write-Host $line
}

function Test-UpgradeArchiveEntries {
    param([Parameter(Mandatory = $true)][string]$ArchivePath)
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [System.IO.Compression.ZipFile]::OpenRead($ArchivePath)
    try {
        foreach ($entry in $archive.Entries) {
            $name = $entry.FullName
            $segments = @($name -split '[\\/]')
            if ([System.IO.Path]::IsPathRooted($name) -or ($segments -contains '..')) {
                throw "升级包包含不安全路径：$name"
            }
        }
    } finally {
        $archive.Dispose()
    }
}

function Test-UpgradeChecksums {
    param([Parameter(Mandatory = $true)][string]$Directory)
    # 自动升级的归档真实性由 Agent 启动本脚本前的签名检查建立：签名覆盖归档 SHA-256，
    # 因此这里校验的是已认证归档内的文件。手工 -Upgrade 没有签名门槛，只按这里的哈希核对，
    # 调用方需要自行确认归档来源。
    $checksumsPath = Join-Path $Directory 'checksums.sha256'
    if (-not (Test-Path -LiteralPath $checksumsPath -PathType Leaf)) {
        throw '升级包不完整，缺少 checksums.sha256'
    }
    $agentCovered = $false
    foreach ($line in [System.IO.File]::ReadAllLines($checksumsPath)) {
        if ([string]::IsNullOrWhiteSpace($line)) {
            continue
        }
        $match = [regex]::Match($line, '^([0-9a-fA-F]{64})\s+\*?(.+)$')
        if (-not $match.Success) {
            throw "checksums.sha256 行格式不合法：$line"
        }
        $expected = $match.Groups[1].Value.ToLowerInvariant()
        $name = $match.Groups[2].Value.Trim()
        if ($name -match '[\\/]' -or $name -eq '.' -or $name -eq '..') {
            throw "checksums.sha256 含不安全的文件名：$name"
        }
        $filePath = Join-Path $Directory $name
        if (-not (Test-Path -LiteralPath $filePath -PathType Leaf)) {
            throw "checksums.sha256 指向的文件不存在：$name"
        }
        $actual = (Get-FileHash -LiteralPath $filePath -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($actual -ne $expected) {
            throw "文件 $name 的 SHA-256 与 checksums.sha256 不符"
        }
        if ($name -eq 'edgepulse-agent.exe') {
            $agentCovered = $true
        }
    }
    if (-not $agentCovered) {
        throw 'checksums.sha256 没有覆盖 edgepulse-agent.exe'
    }
}

function Invoke-AgentUpgrade {
    Assert-Administrator
    if ($ServiceName -ne 'EdgePulseAgent') {
        throw "升级模式只支持默认服务名 EdgePulseAgent，收到 $ServiceName：自定义服务名需要手工升级"
    }
    if ($NoStart) {
        throw '升级模式不接受 -NoStart：安装器需要启动服务并等待健康标记'
    }
    if ($HealthTimeout -lt 1) {
        throw "-HealthTimeout 必须是正整数：$HealthTimeout"
    }
    if ([string]::IsNullOrWhiteSpace($Upgrade)) {
        throw '升级模式缺少 -Upgrade 安装包路径'
    }
    if ([string]::IsNullOrWhiteSpace($StateDir)) {
        throw '升级模式缺少 -StateDir 状态目录'
    }

    $InstallDir = Assert-SafeTargetPath -Path $InstallDir -Option '-InstallDir'
    $StateDir = Assert-SafeTargetPath -Path $StateDir -Option '-StateDir'
    $upgradePackage = [System.IO.Path]::GetFullPath($Upgrade)
    $logPath = Join-Path $StateDir 'update-installer.log'
    $healthMarker = Join-Path $StateDir 'update-health'
    $rollbackMarker = Join-Path $StateDir 'update-rollback'
    $binaryPath = Join-Path $InstallDir 'edgepulse-agent.exe'
    $backupPath = $binaryPath + '.previous'
    $version = ''
    $binaryReplaced = $false

    $null = New-Item -ItemType Directory -Path $StateDir -Force
    $workDirectory = Join-Path ([System.IO.Path]::GetTempPath()) ('edgepulse-agent-upgrade-' + [Guid]::NewGuid().ToString('N'))
    $null = New-Item -ItemType Directory -Path $workDirectory -Force

    try {
        Write-UpgradeLog -LogPath $logPath -Message "开始升级：包 $upgradePackage，安装目录 $InstallDir，健康时限 $HealthTimeout 秒"
        if (-not (Test-Path -LiteralPath $upgradePackage -PathType Leaf)) {
            throw "升级包不存在或不是普通文件：$upgradePackage"
        }

        Test-UpgradeArchiveEntries -ArchivePath $upgradePackage
        $zipCopy = Join-Path $workDirectory 'package.zip'
        Copy-Item -LiteralPath $upgradePackage -Destination $zipCopy -Force
        $extractDirectory = Join-Path $workDirectory 'package'
        $null = New-Item -ItemType Directory -Path $extractDirectory -Force
        Expand-Archive -LiteralPath $zipCopy -DestinationPath $extractDirectory -Force

        Test-UpgradeChecksums -Directory $extractDirectory
        $manifestPath = Join-Path $extractDirectory 'manifest.json'
        if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
            throw '升级包不完整，缺少 manifest.json'
        }
        $manifest = Get-Content -LiteralPath $manifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
        $version = [string]$manifest.version
        if ([string]::IsNullOrWhiteSpace($version)) {
            throw 'manifest.json 缺少 version'
        }
        if ([string]$manifest.platform -notmatch '^windows-') {
            throw "manifest.json 的 platform 不是 Windows 包：$($manifest.platform)"
        }
        $newBinary = Join-Path $extractDirectory 'edgepulse-agent.exe'
        if (-not (Test-Path -LiteralPath $newBinary -PathType Leaf)) {
            throw '升级包不完整，缺少 edgepulse-agent.exe'
        }
        if (-not (Test-Path -LiteralPath $binaryPath -PathType Leaf)) {
            throw "找不到现有二进制 $binaryPath"
        }

        # 旧标记必须在启动新版本之前清掉；清理失败时服务尚未停止，直接结束即可
        if (Test-Path -LiteralPath $healthMarker) {
            Remove-Item -LiteralPath $healthMarker -Force -ErrorAction Stop
        }
        if (Test-Path -LiteralPath $rollbackMarker) {
            Remove-Item -LiteralPath $rollbackMarker -Force -ErrorAction Stop
        }

        # 备份在停服务之前完成；停服务失败时二进制未改动，回滚分支不会触发
        Copy-Item -LiteralPath $binaryPath -Destination $backupPath -Force
        Write-UpgradeLog -LogPath $logPath -Message "已备份现有二进制到 $backupPath"

        $existingService = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
        if ($null -eq $existingService) {
            throw "服务 $ServiceName 不存在：升级模式要求 Agent 已注册为 Windows 服务"
        }
        try {
            Wait-ServiceThroughStartPending -Service $existingService -ServiceName $ServiceName
            if ($existingService.Status -ne 'Stopped') {
                if ($existingService.Status -ne 'StopPending') {
                    Write-UpgradeLog -LogPath $logPath -Message "停止服务 $ServiceName"
                    Invoke-Sc -Arguments "stop $ServiceName" -Description "停止服务 $ServiceName"
                }
                try {
                    $existingService.WaitForStatus('Stopped', [TimeSpan]::FromSeconds(30))
                } catch {
                    throw "停止服务 $ServiceName 超时：请手动停止后重试升级"
                }
            }
        } finally {
            $existingService.Dispose()
        }

        $binaryReplaced = $true
        Copy-Item -LiteralPath $newBinary -Destination $binaryPath -Force
        Write-UpgradeLog -LogPath $logPath -Message "已替换二进制，等待健康标记 $healthMarker 出现版本 $version"

        Invoke-Sc -Arguments "start $ServiceName" -Description "启动服务 $ServiceName"
        $startedService = Get-Service -Name $ServiceName
        try {
            $startedService.WaitForStatus('Running', [TimeSpan]::FromSeconds(30))
        } finally {
            $startedService.Dispose()
        }

        $deadline = (Get-Date).AddSeconds($HealthTimeout)
        $healthy = $false
        while ((Get-Date) -lt $deadline) {
            if (Test-Path -LiteralPath $healthMarker -PathType Leaf) {
                $observed = [System.IO.File]::ReadAllText($healthMarker, [System.Text.Encoding]::UTF8).Trim()
                if ($observed -eq $version) {
                    $healthy = $true
                    break
                }
            }
            Start-Sleep -Milliseconds 250
        }
        if (-not $healthy) {
            throw "健康检查超时：$HealthTimeout 秒内未看到版本 $version 的健康标记"
        }

        try {
            Remove-Item -LiteralPath $backupPath -Force -ErrorAction Stop
            Remove-Item -LiteralPath $healthMarker -Force -ErrorAction Stop
        } catch {
            Write-UpgradeLog -LogPath $logPath -Message "警告：清理备份或健康标记失败：$($_.Exception.Message)"
        }
        Write-UpgradeLog -LogPath $logPath -Message "升级成功：版本 $version 已通过健康检查"
    } catch {
        $failure = $_.Exception.Message
        Write-UpgradeLog -LogPath $logPath -Message "升级失败：$failure"
        if ($binaryReplaced) {
            try {
                Invoke-Sc -Arguments "stop $ServiceName" -Description "停止服务 $ServiceName" | Out-Null
                $service = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
                if ($null -ne $service) {
                    try {
                        $service.WaitForStatus('Stopped', [TimeSpan]::FromSeconds(30))
                    } finally {
                        $service.Dispose()
                    }
                }
            } catch {
                Write-UpgradeLog -LogPath $logPath -Message "警告：停止新版本服务失败：$($_.Exception.Message)"
            }
            try {
                Copy-Item -LiteralPath $backupPath -Destination $binaryPath -Force
                Remove-Item -LiteralPath $backupPath -Force -ErrorAction SilentlyContinue
                Write-UpgradeLog -LogPath $logPath -Message "已恢复旧二进制 $binaryPath"
            } catch {
                Write-UpgradeLog -LogPath $logPath -Message "错误：恢复旧二进制失败，备份保留在 $backupPath：$($_.Exception.Message)"
            }
            if (-not [string]::IsNullOrWhiteSpace($version)) {
                try {
                    $encoding = New-Object System.Text.UTF8Encoding($false)
                    [System.IO.File]::WriteAllText($rollbackMarker, $version + [Environment]::NewLine, $encoding)
                } catch {
                    Write-UpgradeLog -LogPath $logPath -Message "错误：写入回滚标记 $rollbackMarker 失败：$($_.Exception.Message)"
                }
            }
            try {
                Invoke-Sc -Arguments "start $ServiceName" -Description "启动回滚后的服务 $ServiceName" | Out-Null
                Write-UpgradeLog -LogPath $logPath -Message "回滚完成：目标版本 $version 未通过健康检查"
            } catch {
                Write-UpgradeLog -LogPath $logPath -Message "错误：启动旧版本服务失败：$($_.Exception.Message)"
            }
        } else {
            if (Test-Path -LiteralPath $backupPath -PathType Leaf) {
                Remove-Item -LiteralPath $backupPath -Force -ErrorAction SilentlyContinue
            }
            # 启动器进程已经退出，旧服务可能处于停止状态；替换前失败也要把它重新拉起来
            $oldService = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
            if ($null -eq $oldService) {
                Write-UpgradeLog -LogPath $logPath -Message "旧版本服务 $ServiceName 不存在，无法重新启动；二进制未改动"
            } else {
                try {
                    if ($oldService.Status -eq 'Running') {
                        Write-UpgradeLog -LogPath $logPath -Message '旧版本服务仍在运行，二进制未改动'
                    } else {
                        Start-Service -Name $ServiceName -ErrorAction Stop
                        Write-UpgradeLog -LogPath $logPath -Message '旧版本服务已重新启动，二进制未改动'
                    }
                } catch {
                    Write-UpgradeLog -LogPath $logPath -Message "错误：重新启动旧版本服务失败：$($_.Exception.Message)"
                } finally {
                    $oldService.Dispose()
                }
            }
        }
        throw
    } finally {
        if (Test-Path -LiteralPath $workDirectory) {
            Remove-Item -LiteralPath $workDirectory -Recurse -Force -ErrorAction SilentlyContinue
        }
    }
}

function Install-Agent {
    Assert-Administrator

    if ($ServiceName -notmatch '^[A-Za-z0-9][A-Za-z0-9_.-]{0,255}$') {
        throw "服务名不合法：$ServiceName（必须以字母或数字开头，只允许字母、数字、点、下划线与连字符，最长 256 个字符）"
    }
    $InstallDir = Assert-SafeTargetPath -Path $InstallDir -Option '-InstallDir'
    $DataDir = Assert-SafeTargetPath -Path $DataDir -Option '-DataDir'
    if ($InstallDir -eq $DataDir -or $InstallDir.StartsWith($DataDir + '\', [System.StringComparison]::OrdinalIgnoreCase)) {
        throw '安装目录不能与数据目录相同，也不能位于数据目录之下：卸载时 -Purge 只删除数据目录'
    }

    if (-not (Test-Path -LiteralPath $scPath -PathType Leaf)) {
        throw "找不到 sc.exe：$scPath"
    }
    if (-not (Test-Path -LiteralPath $icaclsPath -PathType Leaf)) {
        throw "找不到 icacls.exe：$icaclsPath"
    }

    $scriptDirectory = $PSScriptRoot
    if ([string]::IsNullOrWhiteSpace($scriptDirectory)) {
        throw '无法确定安装脚本所在目录：请用 powershell -File install-agent.ps1 运行'
    }
    $executableSource = Join-Path $scriptDirectory 'edgepulse-agent.exe'
    $templateSource = Join-Path $scriptDirectory 'agent.yaml.example'
    if (-not (Test-Path -LiteralPath $executableSource -PathType Leaf)) {
        throw "安装包不完整，缺少 $executableSource"
    }

    foreach ($directory in @($InstallDir, $DataDir)) {
        if (Test-Path -LiteralPath $directory -PathType Leaf) {
            throw "目标路径已存在同名文件：$directory"
        }
    }

    $configPath = Join-Path $DataDir 'agent.yaml'
    $executablePath = Join-Path $InstallDir 'edgepulse-agent.exe'
    $logPath = Join-Path $DataDir 'agent.log'

    $existingService = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
    $serviceExists = $null -ne $existingService
    if ($serviceExists) {
        # StartPending 阶段 SCM 尚未接受停止控制，先等它落定再决定停止
        Wait-ServiceThroughStartPending -Service $existingService -ServiceName $ServiceName
        if ($existingService.Status -ne 'Stopped') {
            # StopPending 阶段再发停止控制会被 SCM 拒绝，直接等待其完成
            if ($existingService.Status -ne 'StopPending') {
                Write-Host "正在停止已有服务：$ServiceName"
                Invoke-Sc -Arguments "stop $ServiceName" -Description "停止服务 $ServiceName"
            }
            try {
                $existingService.WaitForStatus('Stopped', [TimeSpan]::FromSeconds(30))
            } catch {
                throw "停止服务 $ServiceName 超时：请手动停止后重新运行安装脚本"
            }
        }
        $existingService.Dispose()
    }

    $null = New-Item -ItemType Directory -Path $InstallDir -Force
    $null = New-Item -ItemType Directory -Path $DataDir -Force

    Set-RestrictedDirectoryAcl -Path $DataDir
    $dataSubdirectories = @('certs', 'firmware', 'firmware-keys', 'update', (Join-Path 'update' 'keys'))
    foreach ($relativeDirectory in $dataSubdirectories) {
        $directory = Join-Path $DataDir $relativeDirectory
        $null = New-Item -ItemType Directory -Path $directory -Force
        Set-RestrictedDirectoryAcl -Path $directory
    }

    if (Test-Path -LiteralPath $configPath) {
        if (-not (Test-Path -LiteralPath $configPath -PathType Leaf)) {
            throw "配置路径已被目录占用：$configPath"
        }
        Write-Host "保留已有配置：$configPath"
    } else {
        if (-not (Test-Path -LiteralPath $templateSource -PathType Leaf)) {
            throw "安装包不完整，缺少 $templateSource（仅在需要生成 agent.yaml 时必须）"
        }
        $template = Get-Content -LiteralPath $templateSource -Raw -Encoding UTF8
        if ($DataDir -ne $defaultDataDir) {
            $template = $template.Replace($defaultDataDir, $DataDir)
        }
        if ($InstallDir -ne $defaultInstallDir) {
            $template = $template.Replace($defaultInstallDir, $InstallDir)
        }
        $encoding = New-Object System.Text.UTF8Encoding($false)
        [System.IO.File]::WriteAllText($configPath, $template, $encoding)
        Write-Host "已写入配置模板：$configPath"
    }
    Set-RestrictedFileAcl -Path $configPath

    Set-InstallDirectoryAcl -Path $InstallDir
    if ($executableSource -ne $executablePath) {
        try {
            Copy-Item -LiteralPath $executableSource -Destination $executablePath -Force
        } catch {
            throw "替换二进制失败：$($_.Exception.Message)。请确认 $ServiceName 已停止且没有其他进程占用 $executablePath"
        }
    }

    $installedScript = Join-Path $InstallDir 'install-agent.ps1'
    if ($scriptDirectory -ne $InstallDir) {
        try {
            Copy-Item -LiteralPath (Join-Path $scriptDirectory 'install-agent.ps1') -Destination $installedScript -Force
        } catch {
            throw "复制安装脚本副本失败：$($_.Exception.Message)。请确认 $InstallDir 可写且没有其他进程占用 $installedScript"
        }
    }

    # sc.exe 的 binPath 需要保留内层引号：外层引号交给 sc 拆参数，内层引号属于 CreateService 的命令行
    # 服务名必须同时传给 Agent，否则服务端注册名与 --service-name 不一致，SCM 会拒绝控制处理函数
    $quotedBinPath = '"' + $executablePath + '" --config "' + $configPath + '" --service --service-name "' + $ServiceName + '"'
    $escapedBinPath = $quotedBinPath.Replace('"', '\"')
    if (-not $serviceExists) {
        Invoke-Sc -Arguments ('create ' + $ServiceName + ' binPath= "' + $escapedBinPath + '" start= auto DisplayName= "EdgePulse Agent"') -Description "创建服务 $ServiceName"
    } else {
        Invoke-Sc -Arguments ('config ' + $ServiceName + ' binPath= "' + $escapedBinPath + '" start= auto DisplayName= "EdgePulse Agent"') -Description "更新服务 $ServiceName 的启动命令"
    }
    Invoke-Sc -Arguments ('failure ' + $ServiceName + ' reset= 86400 actions= restart/5000/restart/10000/restart/30000') -Description "配置服务 $ServiceName 的失败恢复动作"

    Write-Host "已安装二进制：$executablePath"
    Write-Host "服务已注册：$ServiceName（LocalSystem，自动启动，失败后按 5 秒、10 秒、30 秒重启）"

    if ($NoStart) {
        Write-Host "已跳过启动（-NoStart）：配置好 $configPath 与证书后运行 sc.exe start $ServiceName"
        return
    }

    try {
        Invoke-Sc -Arguments "start $ServiceName" -Description "启动服务 $ServiceName"
        $startedService = Get-Service -Name $ServiceName
        try {
            $startedService.WaitForStatus('Running', [TimeSpan]::FromSeconds(30))
        } finally {
            $startedService.Dispose()
        }
    } catch {
        throw "服务 $ServiceName 启动失败或未在 30 秒内进入运行状态：$($_.Exception.Message)。请查看 $logPath 与事件日志（Get-EventLog -LogName System -Source 'Service Control Manager'）"
    }
    # 服务可能进入 RUNNING 后立刻因配置或证书问题退出，再等两秒核对状态与退出码
    Assert-ServiceRunning -ServiceName $ServiceName -LogPath $logPath
    Write-Host "服务已启动：$ServiceName"
    Write-Host "首次注册的令牌请写入服务作用域环境变量后重启：reg add `"HKLM\SYSTEM\CurrentControlSet\Services\$ServiceName`" /v Environment /t REG_MULTI_SZ /d `"EDGEPULSE_AGENT_ENROLLMENT_TOKEN=<令牌>`" /f"
}

try {
    if (-not [string]::IsNullOrWhiteSpace($Upgrade)) {
        Invoke-AgentUpgrade
    } else {
        Install-Agent
    }
} catch {
    [Console]::Error.WriteLine('安装失败：' + $_.Exception.Message)
    exit 1
}
