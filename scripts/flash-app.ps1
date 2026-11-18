param(
    [string]$Port = 'COM7',
    [string]$Python = 'F:\python\python.exe'
)
# 只烧 app 分区（0x20000）。绝不烧 0x0 —— 那会覆盖 NVS（Wi-Fi、OTA 地址、音量）。
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$bin = Join-Path $repoRoot 'firmware\build\xiaozhi.bin'
if (-not (Test-Path $bin)) { throw "没有 $bin，先跑 scripts\build.ps1" }

& $Python -m esptool --chip esp32s3 -p $Port --connect-attempts 3 --after hard-reset write-flash 0x20000 $bin
