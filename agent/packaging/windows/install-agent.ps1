# 安装 EdgePulse Agent 为 Windows 服务：落二进制与配置模板、收紧目录权限、注册服务并配置失败恢复。
[CmdletBinding()]
param(
    [string]$InstallDir = 'C:\Program Files\EdgePulse\Agent',
    [string]$DataDir = 'C:\ProgramData\EdgePulse\Agent',
    [string]$ServiceName = 'EdgePulseAgent',
    [switch]$NoStart
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

function Invoke-IcaclsCommand {
    param(
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [Parameter(Mandatory = $true)][string]$Description
    )
    $null = & $icaclsPath @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "设置 $Description 的访问控制失败（icacls 退出码 $LASTEXITCODE）：请确认当前账户是管理员且目标路径存在"
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

function Assert-Administrator {
    $identity = [System.Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object System.Security.Principal.WindowsPrincipal($identity)
    if (-not $principal.IsInRole([System.Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw '安装服务需要管理员权限：请以管理员身份打开 PowerShell 后重新运行 install-agent.ps1'
    }
}

function Install-Agent {
    Assert-Administrator

    if ($ServiceName -notmatch '^[A-Za-z0-9][A-Za-z0-9_.-]{0,255}$') {
        throw "服务名不合法：$ServiceName（必须以字母或数字开头，只允许字母、数字、点、下划线与连字符，最长 256 个字符）"
    }
    $InstallDir = Resolve-DirectoryPath -Path $InstallDir -Option '-InstallDir'
    $DataDir = Resolve-DirectoryPath -Path $DataDir -Option '-DataDir'
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
            throw "替换二进制失败：请确认 $ServiceName 已停止且没有其他进程占用 $executablePath"
        }
    }

    $installedScript = Join-Path $InstallDir 'install-agent.ps1'
    if ($scriptDirectory -ne $InstallDir) {
        Copy-Item -LiteralPath (Join-Path $scriptDirectory 'install-agent.ps1') -Destination $installedScript -Force
    }

    # sc.exe 的 binPath 需要保留内层引号：外层引号交给 sc 拆参数，内层引号属于 CreateService 的命令行
    $quotedBinPath = '"' + $executablePath + '" --config "' + $configPath + '" --service'
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
        $startedService.WaitForStatus('Running', [TimeSpan]::FromSeconds(30))
        $startedService.Dispose()
    } catch {
        throw "服务 $ServiceName 启动失败或未在 30 秒内进入运行状态：$($_.Exception.Message)。请查看 $logPath 与事件日志（Get-EventLog -LogName System -Source 'Service Control Manager'）"
    }
    Write-Host "服务已启动：$ServiceName"
    Write-Host "首次注册的令牌请写入服务作用域环境变量后重启：reg add `"HKLM\SYSTEM\CurrentControlSet\Services\$ServiceName`" /v Environment /t REG_MULTI_SZ /d `"EDGEPULSE_AGENT_ENROLLMENT_TOKEN=<令牌>`" /f"
}

try {
    Install-Agent
} catch {
    [Console]::Error.WriteLine('安装失败：' + $_.Exception.Message)
    exit 1
}
