# 打包 Windows Agent：收集二进制、安装/卸载脚本与配置模板，生成带 SHA-256 清单的 zip。
# 提供签名私钥时先签 manifest，再签归档本身；归档签名供 Agent 验签，manifest.sig 是包内纵深防御。
# checksums.sha256 只列 manifest.files 里的数据文件。
[CmdletBinding()]
param(
    [string]$Binary,
    [string]$Version,
    [string]$Output,
    [string]$SigningKey,
    [string]$SigningKeyId
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
        [Parameter(Mandatory = $true)][object[]]$Files,
        [string]$KeyId
    )
    $builder = New-Object System.Text.StringBuilder
    $null = $builder.Append("{`n")
    $null = $builder.Append('  "version": "' + $ManifestVersion + '",' + "`n")
    $null = $builder.Append('  "platform": "' + $ManifestPlatform + '",' + "`n")
    if (-not [string]::IsNullOrWhiteSpace($KeyId)) {
        $null = $builder.Append('  "signingKeyId": "' + $KeyId + '",' + "`n")
        $null = $builder.Append('  "signatureAlgorithm": "ed25519-sha256",' + "`n")
    }
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

function Invoke-Ed25519Sign {
    param(
        [Parameter(Mandatory = $true)][string]$KeyPath,
        [Parameter(Mandatory = $true)][string]$InputPath,
        [Parameter(Mandatory = $true)][string]$SignaturePath
    )
    $digestFile = [System.IO.Path]::GetTempFileName()
    $rawSignatureFile = [System.IO.Path]::GetTempFileName()
    try {
        & openssl dgst -sha256 -binary -out $digestFile $InputPath
        if ($LASTEXITCODE -ne 0) {
            throw "openssl 计算摘要失败（退出码 $LASTEXITCODE）：$InputPath"
        }
        & openssl pkeyutl -sign -inkey $KeyPath -rawin -in $digestFile -out $rawSignatureFile
        if ($LASTEXITCODE -ne 0) {
            throw "openssl 签名失败（退出码 $LASTEXITCODE）：$InputPath"
        }
        if ((Get-Item -LiteralPath $rawSignatureFile).Length -ne 64) {
            throw "Ed25519 签名长度不是 64 字节：$InputPath"
        }
        & openssl base64 -A -in $rawSignatureFile -out $SignaturePath
        if ($LASTEXITCODE -ne 0) {
            throw "openssl Base64 编码失败（退出码 $LASTEXITCODE）"
        }
    } finally {
        Remove-Item -LiteralPath $digestFile -Force -ErrorAction SilentlyContinue
        Remove-Item -LiteralPath $rawSignatureFile -Force -ErrorAction SilentlyContinue
    }
}

function Build-Package {
    if ([string]::IsNullOrWhiteSpace($Version)) {
        $Version = Get-ProjectVersion
    }
    if ($Version -notmatch '^[0-9]+(\.[0-9]+)*([-+][0-9A-Za-z.-]+)?$') {
        throw "版本号不合法：$Version"
    }
    $platform = Get-PlatformName

    $hasSigningKey   = -not [string]::IsNullOrWhiteSpace($SigningKey)
    $hasSigningKeyId = -not [string]::IsNullOrWhiteSpace($SigningKeyId)
    if ($hasSigningKey -ne $hasSigningKeyId) {
        throw '-SigningKey 与 -SigningKeyId 必须同时给出'
    }
    if ($hasSigningKey) {
        if (-not (Test-Path -LiteralPath $SigningKey -PathType Leaf)) {
            throw "找不到签名私钥：$SigningKey"
        }
        if ($SigningKeyId -notmatch '^[A-Za-z0-9][A-Za-z0-9._-]*$' -or $SigningKeyId.Contains('..')) {
            throw "key id 只能由字母、数字、点、下划线与连字符组成，且不能包含 ..：$SigningKeyId"
        }
        if ($null -eq (Get-Command openssl -ErrorAction SilentlyContinue)) {
            throw '缺少 openssl：签名需要 OpenSSL 3 的 pkeyutl'
        }
        $SigningKey = (Resolve-Path -LiteralPath $SigningKey).ProviderPath
        $keyText = & openssl pkey -in $SigningKey -noout -text 2>$null | Select-Object -First 1
        if ($LASTEXITCODE -ne 0 -or [string]$keyText -notmatch '^ED25519') {
            throw "签名私钥不是 Ed25519：$SigningKey"
        }
    }

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
        $manifestText = New-ManifestText -ManifestVersion $Version -ManifestPlatform $platform -Files $manifestFiles -KeyId $(if ($hasSigningKey) { $SigningKeyId } else { '' })
        $encoding = New-Object System.Text.UTF8Encoding($false)
        $manifestPath = Join-Path $stageDirectory 'manifest.json'
        [System.IO.File]::WriteAllText($manifestPath, $manifestText, $encoding)

        # 校验文件与 manifest 覆盖同一批数据文件：清单自身、签名与校验文件不列进去，避免互相引用
        $checksumBuilder = New-Object System.Text.StringBuilder
        foreach ($file in $manifestFiles) {
            $digest = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
            $null = $checksumBuilder.Append($digest + '  ' + $file.Name + "`n")
        }
        $checksumsPath = Join-Path $stageDirectory 'checksums.sha256'
        [System.IO.File]::WriteAllText($checksumsPath, $checksumBuilder.ToString(), $encoding)

        $signaturePath = Join-Path $stageDirectory 'manifest.sig'
        if ($hasSigningKey) {
            Invoke-Ed25519Sign -KeyPath $SigningKey -InputPath $manifestPath -SignaturePath $signaturePath
        }

        $archiveName = 'edgepulse-agent-' + $Version + '-' + $platform + '.zip'
        $archivePath = Join-Path $Output $archiveName
        $archiveSignaturePath = $archivePath + '.sig'
        $metadataPath = $archivePath + '.json'
        # 同一版本重打包会覆盖归档；旧的签名旁路必须一起清掉，否则会留下与新手包不匹配的签名
        foreach ($stale in @($archiveSignaturePath, $metadataPath, ($metadataPath + '.tmp'))) {
            Remove-Item -LiteralPath $stale -Force -ErrorAction SilentlyContinue
        }
        Compress-Archive -Path (Join-Path $stageDirectory '*') -DestinationPath $archivePath -Force

        $baseName = 'edgepulse-agent-' + $Version + '-' + $platform
        Copy-Item -LiteralPath $manifestPath -Destination (Join-Path $Output ($baseName + '.manifest.json')) -Force
        Copy-Item -LiteralPath $checksumsPath -Destination (Join-Path $Output ($baseName + '.checksums.sha256')) -Force
        # 旧版本曾把 manifest.sig 复制到输出目录；归档内仍保留它，输出目录不再提供这份旁路
        $legacyManifestSignature = Join-Path $Output ($baseName + '.manifest.sig')
        Remove-Item -LiteralPath $legacyManifestSignature -Force -ErrorAction SilentlyContinue

        $archiveDigest = (Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash.ToLowerInvariant()
        Write-Host "安装包：$archivePath"
        Write-Host "清单：$($manifestFiles.Count) 个文件，逐项记录 size 与 sha256"
        Write-Host "校验：$baseName.checksums.sha256"
        Write-Host "sha256=$archiveDigest"
        if ($hasSigningKey) {
            # 签名对象是归档文件本身；先完成归档再签，避免签名覆盖的字节与落盘内容不一致
            Invoke-Ed25519Sign -KeyPath $SigningKey -InputPath $archivePath -SignaturePath $archiveSignaturePath
            $archiveSignature = [System.IO.File]::ReadAllText($archiveSignaturePath).Trim()
            $metadata = '{"version":"' + $Version + '","platform":"' + $platform + '","sha256":"' + $archiveDigest +
                        '","signature":"' + $archiveSignature + '","signingKeyId":"' + $SigningKeyId + '","algorithm":"ed25519-sha256"}' + "`n"
            [System.IO.File]::WriteAllText($metadataPath, $metadata, $encoding)
            Write-Host "signature=$archiveSignature"
            Write-Host "签名：$baseName.zip.sig（key id $SigningKeyId）"
            Write-Host "元数据：$baseName.zip.json"
        } else {
            Write-Host '未提供签名私钥：本次生成的是未签名安装包，只用于本地或手工安装，不能用于需要验签的升级'
        }
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
