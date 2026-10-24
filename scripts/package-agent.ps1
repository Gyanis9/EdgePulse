# 打包 Windows Agent：收集二进制、安装/卸载脚本与配置模板，生成带 SHA-256 清单的 zip。
[CmdletBinding()]
param(
    [string]$Binary,
    [string]$Version,
    [string]$Output
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot

function Get-ProjectVersion {
    $cmakeLists = Join-Path $repoRoot 'CMakeLists.txt'
    if (-not (Test-Path -LiteralPath $cmakeLists -PathType Leaf)) {
        throw "找不到项目文件：$cmakeLists"
    }
    foreach ($line in Get-Content -LiteralPath $cmakeLists -Encoding UTF8) {
        if ($line -match '^\s*VERSION\s+([0-9]+(?:\.[0-9]+)+)\s*$') {
            return $Matches[1]
        }
    }
    throw '无法从 CMakeLists.txt 读取项目版本：请用 -Version 指定'
}

function Get-PlatformName {
    $architecture = 'x86_64'
    if ($env:PROCESSOR_ARCHITECTURE -eq 'ARM64') {
        $architecture = 'aarch64'
    } elseif ($env:PROCESSOR_ARCHITECTURE -and $env:PROCESSOR_ARCHITECTURE -ne 'AMD64') {
        $architecture = $env:PROCESSOR_ARCHITECTURE.ToLowerInvariant()
    }
    return 'windows-' + $architecture
}

function Invoke-ReleaseBuild {
    if ([System.Environment]::OSVersion.Platform -ne [System.PlatformID]::Win32NT) {
        throw '当前主机不是 Windows，无法执行 Release 构建：请用 -Binary 指定已构建的 Windows Agent 二进制'
    }
    if ($null -eq (Get-Command cmake -ErrorAction SilentlyContinue)) {
        throw '未找到 cmake：请把 CMake 加入 PATH，或用 -Binary 指定已构建的 Windows Agent 二进制'
    }
    Push-Location $repoRoot
    try {
        Write-Host '构建 Release Agent ...'
        # cmake 的输出走主机而不是管道，否则它会混进本函数的返回值
        & cmake --preset release 2>&1 | Write-Host
        if ($LASTEXITCODE -ne 0) {
            throw "Release 配置失败（cmake 退出码 $LASTEXITCODE）"
        }
        & cmake --build --preset release --target edgepulse-agent 2>&1 | Write-Host
        if ($LASTEXITCODE -ne 0) {
            throw "Release 构建失败（cmake 退出码 $LASTEXITCODE）"
        }
    } finally {
        Pop-Location
    }
    $builtBinary = Join-Path $repoRoot 'build\release\bin\edgepulse-agent.exe'
    if (-not (Test-Path -LiteralPath $builtBinary -PathType Leaf)) {
        throw "Release 构建未产出预期的二进制：$builtBinary"
    }
    return $builtBinary
}

function New-ManifestText {
    param(
        [Parameter(Mandatory = $true)][string]$ManifestVersion,
        [Parameter(Mandatory = $true)][string]$ManifestPlatform,
        [Parameter(Mandatory = $true)][object[]]$Files
    )
    $builder = New-Object System.Text.StringBuilder
    $null = $builder.Append("{`n")
    $null = $builder.Append('  "version": "' + $ManifestVersion + '",' + "`n")
    $null = $builder.Append('  "platform": "' + $ManifestPlatform + '",' + "`n")
    $null = $builder.Append('  "files": [' + "`n")
    for ($index = 0; $index -lt $Files.Count; $index++) {
        $file = $Files[$index]
        $digest = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        $size = (Get-Item -LiteralPath $file.FullName).Length
        $line = '    {"path": "' + $file.Name + '", "size": ' + $size + ', "sha256": "' + $digest + '"}'
        if ($index -lt $Files.Count - 1) {
            $line += ','
        }
        $null = $builder.Append($line + "`n")
    }
    $null = $builder.Append("  ]`n}`n")
    return $builder.ToString()
}

function Build-Package {
    if ([string]::IsNullOrWhiteSpace($Version)) {
        $Version = Get-ProjectVersion
    }
    if ($Version -notmatch '^[0-9]+(\.[0-9]+)*([-+][0-9A-Za-z.-]+)?$') {
        throw "版本号不合法：$Version"
    }
    $platform = Get-PlatformName

    if ([string]::IsNullOrWhiteSpace($Binary)) {
        $Binary = Invoke-ReleaseBuild
    }
    if (-not (Test-Path -LiteralPath $Binary -PathType Leaf)) {
        throw "找不到 Agent 二进制：$Binary"
    }
    $Binary = (Resolve-Path -LiteralPath $Binary).ProviderPath

    if ([string]::IsNullOrWhiteSpace($Output)) {
        $Output = Join-Path $repoRoot 'build\agent-package'
    }
    $Output = [System.IO.Path]::GetFullPath($Output)
    $null = New-Item -ItemType Directory -Path $Output -Force

    $packageInputs = @(
        [pscustomobject]@{ Name = 'edgepulse-agent.exe'; Source = $Binary },
        [pscustomobject]@{ Name = 'agent.yaml.example'; Source = (Join-Path $repoRoot 'agent\packaging\windows\agent.yaml.example') },
        [pscustomobject]@{ Name = 'install-agent.ps1'; Source = (Join-Path $repoRoot 'agent\packaging\windows\install-agent.ps1') },
        [pscustomobject]@{ Name = 'uninstall-agent.ps1'; Source = (Join-Path $repoRoot 'agent\packaging\windows\uninstall-agent.ps1') }
    )
    foreach ($inputFile in $packageInputs) {
        if (-not (Test-Path -LiteralPath $inputFile.Source -PathType Leaf)) {
            throw "缺少打包输入：$($inputFile.Source)"
        }
    }

    $stageDirectory = Join-Path ([System.IO.Path]::GetTempPath()) ('edgepulse-agent-package-' + [Guid]::NewGuid().ToString('N'))
    try {
        $null = New-Item -ItemType Directory -Path $stageDirectory -Force
        foreach ($inputFile in $packageInputs) {
            Copy-Item -LiteralPath $inputFile.Source -Destination (Join-Path $stageDirectory $inputFile.Name) -Force
        }

        $manifestFiles = @($packageInputs | ForEach-Object { Get-Item -LiteralPath (Join-Path $stageDirectory $_.Name) } | Sort-Object -Property Name)
        $manifestText = New-ManifestText -ManifestVersion $Version -ManifestPlatform $platform -Files $manifestFiles
        $encoding = New-Object System.Text.UTF8Encoding($false)
        [System.IO.File]::WriteAllText((Join-Path $stageDirectory 'manifest.json'), $manifestText, $encoding)

        $archiveName = 'edgepulse-agent-' + $Version + '-' + $platform + '.zip'
        $archivePath = Join-Path $Output $archiveName
        Compress-Archive -Path (Join-Path $stageDirectory '*') -DestinationPath $archivePath -Force

        Write-Host "安装包：$archivePath"
        Write-Host "清单：$($manifestFiles.Count) 个文件，逐项记录 size 与 sha256"
    } finally {
        if (Test-Path -LiteralPath $stageDirectory) {
            Remove-Item -LiteralPath $stageDirectory -Recurse -Force
        }
    }
}

try {
    Build-Package
} catch {
    [Console]::Error.WriteLine('打包失败：' + $_.Exception.Message)
    exit 1
}
