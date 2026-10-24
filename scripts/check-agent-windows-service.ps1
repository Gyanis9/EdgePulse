# 门控的 Windows 服务冒烟：把 Agent 装成一个可丢弃的服务，验证启动、查询、停止与卸载。
# 默认只打印 SKIP；只有显式设置 EDGEPULSE_RUN_SERVICE_SMOKE=1 才会真正创建服务。
[CmdletBinding()]
param(
    [string]$ConfigPath = 'var\agent.yaml',
    [string]$CertificateDirectory = 'var\certs'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if ($env:EDGEPULSE_RUN_SERVICE_SMOKE -ne '1') {
    Write-Host 'SKIP：未设置 EDGEPULSE_RUN_SERVICE_SMOKE=1，跳过 Windows 服务冒烟；该脚本默认不运行'
    exit 0
}

$repoRoot = Split-Path -Parent $PSScriptRoot
$powershellPath = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
$scPath = Join-Path $env:SystemRoot 'System32\sc.exe'
$defaultInstallDir = 'C:\Program Files\EdgePulse\Agent'
$defaultDataDir = 'C:\ProgramData\EdgePulse\Agent'
$serviceName = "EdgePulseAgentSmoke$PID"
$requiredCertificates = @('ca.pem', 'agent.pem', 'agent-key.pem')

function Assert-Administrator {
    $identity = [System.Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object System.Security.Principal.WindowsPrincipal($identity)
    if (-not $principal.IsInRole([System.Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw '服务冒烟需要管理员权限：请以管理员身份打开 PowerShell 后重新运行 scripts\check-agent-windows-service.ps1'
    }
}

function Write-SkipAndExit {
    param([Parameter(Mandatory = $true)][string]$Reason)
    Write-Host "SKIP：$Reason"
    exit 0
}

function Resolve-RepoPath {
    param([Parameter(Mandatory = $true)][string]$Path)
    if ([System.IO.Path]::IsPathRooted($Path)) {
        return [System.IO.Path]::GetFullPath($Path)
    }
    return [System.IO.Path]::GetFullPath((Join-Path $repoRoot $Path))
}

function Get-UnusedLoopbackPort {
    $listener = New-Object -TypeName System.Net.Sockets.TcpListener -ArgumentList @([System.Net.IPAddress]::Loopback, 0)
    $listener.Start()
    try {
        $endpoint = [System.Net.IPEndPoint]$listener.LocalEndpoint
        return $endpoint.Port
    } finally {
        $listener.Stop()
    }
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

function Get-ScQueryExText {
    param([Parameter(Mandatory = $true)][string]$Name)
    $result = Invoke-NativeCapture -FilePath $scPath -Arguments @('queryex', $Name)
    if ($result.ExitCode -ne 0) {
        $details = $result.Output
        if ([string]::IsNullOrWhiteSpace($details)) {
            $details = '未返回错误文本'
        }
        throw "查询服务 $Name 状态失败（sc.exe 退出码 $($result.ExitCode)）：$details"
    }
    return $result.Output
}

function New-SmokeConfigurationText {
    param(
        [Parameter(Mandatory = $true)][string]$Text,
        [Parameter(Mandatory = $true)][string]$BaseUrl,
        [Parameter(Mandatory = $true)][string]$DataDirectory,
        [Parameter(Mandatory = $true)][string]$InstallDirectory
    )
    # 与安装器一致：先把默认数据目录前缀换成临时目录，再把证书与注册地址指到本次冒烟的位置
    $localized = $Text.Replace($defaultDataDir, $DataDirectory).Replace($defaultInstallDir, $InstallDirectory)
    $localized = [regex]::Replace($localized, '(?m)^([ \t]*base_url[ \t]*:[ \t]*).*$', '${1}' + $BaseUrl)
    $localized = [regex]::Replace($localized, '(?m)^([ \t]*ca_file[ \t]*:[ \t]*).*$', '${1}' + (Join-Path $DataDirectory 'certs\ca.pem'))
    $localized = [regex]::Replace($localized, '(?m)^([ \t]*certificate_file[ \t]*:[ \t]*).*$', '${1}' + (Join-Path $DataDirectory 'certs\agent.pem'))
    $localized = [regex]::Replace($localized, '(?m)^([ \t]*private_key_file[ \t]*:[ \t]*).*$', '${1}' + (Join-Path $DataDirectory 'certs\agent-key.pem'))
    return $localized
}

function Invoke-ServiceSmoke {
    if ([string]::IsNullOrWhiteSpace($ConfigPath)) {
        Write-SkipAndExit '未指定 -ConfigPath，找不到可用的 Agent 配置'
    }
    if ([string]::IsNullOrWhiteSpace($CertificateDirectory)) {
        Write-SkipAndExit '未指定 -CertificateDirectory，找不到可用于冒烟的证书'
    }
    $configFullPath = Resolve-RepoPath -Path $ConfigPath
    if (-not (Test-Path -LiteralPath $configFullPath -PathType Leaf)) {
        Write-SkipAndExit "找不到可用的 Agent 配置：$configFullPath。请用 -ConfigPath 指定，或先复制 agent\packaging\windows\agent.yaml.example"
    }
    $certificateDirectory = Resolve-RepoPath -Path $CertificateDirectory
    $missingCertificates = @($requiredCertificates | Where-Object { -not (Test-Path -LiteralPath (Join-Path $certificateDirectory $_) -PathType Leaf) })
    if ($missingCertificates.Count -gt 0) {
        Write-SkipAndExit "证书目录 $certificateDirectory 缺少 $($missingCertificates -join '、')；请先运行 bash scripts/dev-certs.sh 或用 -CertificateDirectory 指定"
    }
    $binaryPath = $null
    foreach ($candidate in @((Join-Path $repoRoot 'build\release\bin\edgepulse-agent.exe'), (Join-Path $repoRoot 'build\debug\bin\edgepulse-agent.exe'))) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            $binaryPath = $candidate
            break
        }
    }
    if ($null -eq $binaryPath) {
        Write-SkipAndExit '找不到已构建的 edgepulse-agent.exe：请先构建 build\release 或 build\debug 后重试'
    }

    $configText = [System.IO.File]::ReadAllText($configFullPath)
    foreach ($key in @('base_url', 'ca_file', 'certificate_file', 'private_key_file')) {
        if (-not [regex]::IsMatch($configText, '(?m)^[ \t]*' + $key + '[ \t]*:')) {
            Write-SkipAndExit "配置 $configFullPath 缺少 $key，无法作为冒烟配置"
        }
    }

    $baseUrl = 'https://127.0.0.1:' + (Get-UnusedLoopbackPort)
    # 临时目录放在 ProgramData 的 EdgePulse 命名空间下：安装器的路径守卫会拒绝 C:\Users 下的目标
    $smokeRoot = Join-Path $env:ProgramData ('EdgePulse\Smoke\agent-service-' + [Guid]::NewGuid().ToString('N'))
    $installDir = Join-Path $smokeRoot 'install'
    $dataDir = Join-Path $smokeRoot 'data'
    $stageDir = Join-Path $smokeRoot 'stage'
    $stagedInstaller = Join-Path $stageDir 'install-agent.ps1'
    $stagedUninstaller = Join-Path $stageDir 'uninstall-agent.ps1'
    $installedConfigPath = Join-Path $dataDir 'agent.yaml'

    try {
        $null = New-Item -ItemType Directory -Path $stageDir -Force
        Copy-Item -LiteralPath $binaryPath -Destination (Join-Path $stageDir 'edgepulse-agent.exe') -Force
        foreach ($scriptName in @('install-agent.ps1', 'uninstall-agent.ps1')) {
            Copy-Item -LiteralPath (Join-Path $repoRoot ('agent\packaging\windows\' + $scriptName)) -Destination (Join-Path $stageDir $scriptName) -Force
        }
        $smokeConfigText = New-SmokeConfigurationText -Text $configText -BaseUrl $baseUrl -DataDirectory $dataDir -InstallDirectory $installDir
        $encoding = New-Object System.Text.UTF8Encoding($false)
        [System.IO.File]::WriteAllText((Join-Path $stageDir 'agent.yaml.example'), $smokeConfigText, $encoding)

        Write-Host "冒烟服务名：$serviceName"
        Write-Host "临时安装目录：$installDir"
        Write-Host "临时数据目录：$dataDir"
        Write-Host "冒烟注册地址：$baseUrl"

        & $powershellPath -NoProfile -ExecutionPolicy Bypass -File $stagedInstaller -InstallDir $installDir -DataDir $dataDir -ServiceName $serviceName -NoStart
        if ($LASTEXITCODE -ne 0) {
            throw "安装脚本失败（退出码 $LASTEXITCODE），未得到可启动的服务"
        }
        Write-Host "安装读数：退出码 $LASTEXITCODE"

        if (-not (Test-Path -LiteralPath $installedConfigPath -PathType Leaf)) {
            throw "安装后缺少配置：$installedConfigPath"
        }
        $installedText = [System.IO.File]::ReadAllText($installedConfigPath)
        if (-not $installedText.Contains($baseUrl)) {
            throw "安装后的配置没有使用冒烟注册地址 $baseUrl，拒绝继续启动"
        }
        foreach ($certificateName in $requiredCertificates) {
            Copy-Item -LiteralPath (Join-Path $certificateDirectory $certificateName) -Destination (Join-Path $dataDir ('certs\' + $certificateName)) -Force
        }
        Write-Host "已复制证书：$($requiredCertificates -join '、')"

        $startResult = Invoke-NativeCapture -FilePath $scPath -Arguments @('start', $serviceName)
        if ($startResult.ExitCode -ne 0) {
            throw "启动服务失败（sc.exe 退出码 $($startResult.ExitCode)）：$($startResult.Output)"
        }
        Write-Host "启动读数：`n$($startResult.Output)"

        $deadline = (Get-Date).AddSeconds(30)
        $queryText = ''
        $lastStateLine = ''
        while ((Get-Date) -lt $deadline) {
            $queryText = Get-ScQueryExText -Name $serviceName
            $stateMatch = [regex]::Match($queryText, 'STATE\s*:\s*\d+\s+\S+')
            if ($stateMatch.Success -and $stateMatch.Value.Trim() -ne $lastStateLine) {
                $lastStateLine = $stateMatch.Value.Trim()
                Write-Host "状态读数：$lastStateLine"
            }
            if ($queryText -match 'STATE\s*:\s*4\s+RUNNING') {
                break
            }
            if ($queryText -match 'STATE\s*:\s*1\s+STOPPED') {
                break
            }
            Start-Sleep -Milliseconds 500
        }
        if ($queryText -notmatch 'STATE\s*:\s*4\s+RUNNING') {
            throw "服务 $serviceName 未进入 RUNNING：`n$queryText`n请查看 $dataDir\agent.log 与事件日志"
        }

        # 服务进入 RUNNING 后仍可能立刻因证书或配置问题退出，再等两秒复核
        Start-Sleep -Seconds 2
        $queryText = Get-ScQueryExText -Name $serviceName
        Write-Host "运行读数：`n$queryText"
        if ($queryText -notmatch 'STATE\s*:\s*4\s+RUNNING' -or $queryText -notmatch 'SERVICE_EXIT_CODE\s*:\s*0') {
            throw "服务 $serviceName 启动后未保持运行：`n$queryText`n请查看 $dataDir\agent.log 与事件日志"
        }

        $stopResult = Invoke-NativeCapture -FilePath $scPath -Arguments @('stop', $serviceName)
        if ($stopResult.ExitCode -ne 0) {
            throw "停止服务失败（sc.exe 退出码 $($stopResult.ExitCode)）：$($stopResult.Output)"
        }
        Write-Host "停止读数：`n$($stopResult.Output)"

        $deadline = (Get-Date).AddSeconds(30)
        while ((Get-Date) -lt $deadline) {
            $queryText = Get-ScQueryExText -Name $serviceName
            if ($queryText -match 'STATE\s*:\s*1\s+STOPPED') {
                break
            }
            Start-Sleep -Milliseconds 500
        }
        if ($queryText -notmatch 'STATE\s*:\s*1\s+STOPPED') {
            throw "服务 $serviceName 未在 30 秒内停止：`n$queryText"
        }
        Write-Host "停止后读数：`n$queryText"

        & $powershellPath -NoProfile -ExecutionPolicy Bypass -File $stagedUninstaller -InstallDir $installDir -DataDir $dataDir -ServiceName $serviceName -Purge
        if ($LASTEXITCODE -ne 0) {
            throw "卸载脚本失败（退出码 $LASTEXITCODE），请手动清理服务 $serviceName 与目录 $smokeRoot"
        }
        Write-Host "卸载读数：退出码 $LASTEXITCODE"
    } finally {
        $remainingService = Get-Service -Name $serviceName -ErrorAction SilentlyContinue
        if ($null -ne $remainingService) {
            $remainingService.Dispose()
            Write-Warning "服务 $serviceName 仍在系统中，执行兜底停止与删除"
            $null = Invoke-NativeCapture -FilePath $scPath -Arguments @('stop', $serviceName)
            Start-Sleep -Seconds 1
            $null = Invoke-NativeCapture -FilePath $scPath -Arguments @('delete', $serviceName)
        }
        if (Test-Path -LiteralPath $smokeRoot) {
            Remove-Item -LiteralPath $smokeRoot -Recurse -Force -ErrorAction SilentlyContinue
        }
    }
}

try {
    Assert-Administrator
    Invoke-ServiceSmoke
} catch {
    [Console]::Error.WriteLine('服务冒烟失败：' + $_.Exception.Message)
    exit 1
}
