# 把固件仓库里的改动同步回 Foxy-OS：
#   1) overlay/ 下保留可直接覆盖的原文件
#   2) patches/ 下生成相对 v2.6.0 tag 的补丁
# 用法：pwsh -File scripts/sync-overlay.ps1 [-Firmware F:\esp\xiaozhi-esp32] [-Repo F:\foxy-os]
param(
    [string]$Firmware = 'F:\esp\xiaozhi-esp32',
    [string]$Repo = 'F:\foxy-os'
)

$ErrorActionPreference = 'Stop'

$files = @(
    'main/boards/m5stack/core-s3/cores3_display.h',
    'main/boards/m5stack/core-s3/cores3_display.cc',
    'main/boards/m5stack/core-s3/foxy_ui.h',
    'main/boards/m5stack/core-s3/foxy_ui.cc',
    'main/boards/m5stack/core-s3/m5stack_core_s3.cc',
    'main/display/lcd_display.cc',
    'main/display/display.h',
    'main/mcp_server.cc',
    'main/audio/audio_codec.cc',
    'main/application.cc',
    'main/application.h',
    'main/protocols/protocol.cc'
)

foreach ($f in $files) {
    $src = Join-Path $Firmware $f
    if (-not (Test-Path $src)) { Write-Warning "skip missing: $f"; continue }
    $dst = Join-Path $Repo (Join-Path 'overlay' $f)
    New-Item -ItemType Directory -Force -Path (Split-Path $dst) | Out-Null
    Copy-Item $src $dst -Force
    Write-Host "overlay <- $f"
}

Push-Location $Firmware
try {
    git add -N ($files | ForEach-Object { $_ }) 2>$null | Out-Null
    $patch = Join-Path $Repo 'patches\firmware-v2.6.0-foxy.patch'
    git diff v2.6.0 -- $files | Set-Content -Path $patch -Encoding utf8
    Write-Host ("patch -> {0} ({1} bytes)" -f $patch, (Get-Item $patch).Length)
} finally {
    Pop-Location
}
