$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$installers = @(Get-ChildItem target/installers/*-setup.exe)
if ($installers.Count -ne 1) { throw '安装程序产物缺失或不唯一' }
$installer = $installers[0]
$expected = (Get-Content target/installers/SHA256SUMS).Split(' ')[0]
if ((Get-FileHash $installer.FullName -Algorithm SHA256).Hash.ToLowerInvariant() -ne $expected) {
    throw '安装程序校验失败'
}
$destination = Join-Path $env:RUNNER_TEMP 'RingEcho Installation Test'
$arguments = @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/SP-', "/DIR=`"$destination`"")
$process = Start-Process $installer.FullName -ArgumentList $arguments -Wait -PassThru
if ($process.ExitCode -ne 0) { throw "安装失败：$($process.ExitCode)" }
try {
    python scripts/ci/smoke.py (Join-Path $destination bin)
    if ($LASTEXITCODE -ne 0) { throw '安装后启动或语义检查失败' }
} finally {
    $uninstaller = Join-Path $destination unins000.exe
    if (Test-Path $uninstaller) {
        $process = Start-Process $uninstaller -ArgumentList '/VERYSILENT /SUPPRESSMSGBOXES /NORESTART' -Wait -PassThru
        if ($process.ExitCode -ne 0) { throw "卸载失败：$($process.ExitCode)" }
    }
}
if (Test-Path (Join-Path $destination bin/rev.exe)) { throw '卸载后仍残留编译器' }
