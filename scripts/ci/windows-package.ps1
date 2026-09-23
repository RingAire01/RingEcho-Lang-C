param([Parameter(Mandatory)][ValidateSet('x64', 'x86', 'arm64')][string]$Arch)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$archives = @(Get-ChildItem target/download/*.zip)
if ($archives.Count -ne 1) { throw '必须且只能包含一个便携包' }
$archive = $archives[0]
$checksumLines = Get-Content target/download/SHA256SUMS
$expected = @($checksumLines | Where-Object { $_.EndsWith("  $($archive.Name)") })
if ($expected.Count -ne 1) { throw '缺少唯一的压缩包校验记录' }
if ((Get-FileHash $archive.FullName -Algorithm SHA256).Hash.ToLowerInvariant() -ne $expected[0].Split(' ')[0]) {
    throw '便携包校验失败'
}
Expand-Archive $archive.FullName -DestinationPath target/payload
$directories = @(Get-ChildItem target/payload -Directory)
if ($directories.Count -ne 1) { throw '便携包目录结构不符合预期' }
$payload = $directories[0]
$manifest = Get-Content (Join-Path $payload.FullName manifest.json) -Raw | ConvertFrom-Json
if ($manifest.platform -ne 'windows' -or $manifest.arch -ne $Arch) { throw '安装包平台架构不匹配' }
$candidates = @(
    (Join-Path $env:ProgramFiles 'Inno Setup 6/ISCC.exe'),
    (Join-Path ${env:ProgramFiles(x86)} 'Inno Setup 6/ISCC.exe')
)
$compiler = $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if (!$compiler) { throw 'Runner 缺少 Inno Setup 6' }
$output = New-Item -ItemType Directory target/installers -Force
& $compiler "/DPayload=$($payload.FullName)" "/DOutput=$($output.FullName)" "/DArch=$Arch" `
    "/DVersion=$($manifest.version)" "/DNumericVersion=$($manifest.version.Split('-')[0])" "/DPackageName=$($payload.Name)" packaging/windows.iss
if ($LASTEXITCODE -ne 0) { throw 'Inno Setup 构建失败' }
$installers = @(Get-ChildItem $output.FullName -Filter '*-setup.exe')
if ($installers.Count -ne 1) { throw '安装程序产物缺失或不唯一' }
$hash = (Get-FileHash $installers[0].FullName -Algorithm SHA256).Hash.ToLowerInvariant()
"$hash  $($installers[0].Name)" | Set-Content (Join-Path $output.FullName SHA256SUMS) -Encoding utf8NoBOM
