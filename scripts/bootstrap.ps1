# 拉取上游固件源码（xiaozhi-esp32 v2.6.0）并应用 Foxy-OS 的补丁与 overlay
# 用法: powershell -ExecutionPolicy Bypass -File scripts\bootstrap.ps1
$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$fw = Join-Path $repoRoot 'firmware'
$mirror = 'https://gh-proxy.com/https://github.com/78/xiaozhi-esp32.git'
$tag = 'v2.6.0'

if (-not (Test-Path $fw)) {
    Write-Host "[1/3] clone $tag ..."
    git clone --branch $tag --depth 1 $mirror $fw
} else {
    Write-Host "[1/3] firmware/ 已存在，跳过 clone"
}

Write-Host "[2/3] 应用补丁"
$patch = Join-Path $repoRoot 'patches\firmware-v2.6.0-foxy.patch'
Push-Location $fw
git apply --ignore-whitespace --whitespace=nowarn $patch
if ($LASTEXITCODE -ne 0) { Write-Warning "补丁可能已应用过，继续" }
Pop-Location

Write-Host "[3/3] 复制 overlay"
Copy-Item -Path (Join-Path $repoRoot 'overlay\*') -Destination $fw -Recurse -Force

Write-Host "完成。接下来装 ESP-IDF release/v6.1，然后运行 scripts\build.ps1"
