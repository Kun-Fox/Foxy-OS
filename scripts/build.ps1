param(
    [string]$Port = 'COM7',
    [string]$IdfPath = 'F:\esp\esp-idf',
    [string]$IdfTools = 'F:\esp\idf-tools'
)
# 编译 CoreS3 固件。只编 app，不碰 NVS。
$ErrorActionPreference = 'Continue'
$repoRoot = Split-Path -Parent $PSScriptRoot
$fw = Join-Path $repoRoot 'firmware'

$env:IDF_TOOLS_PATH = $IdfTools
$env:PYTHONUTF8 = '1'
. (Join-Path $IdfPath 'export.ps1') *> (Join-Path $env:TEMP 'foxy-export.log')

$py = Join-Path $IdfTools 'python_env\idf6.1_py3.12_env\Scripts\python.exe'
if (-not (Test-Path $py)) { throw "找不到 IDF 的 python: $py（先装 ESP-IDF release/v6.1）" }

Push-Location $fw
& $py scripts\build.py m5stack/core-s3 *> (Join-Path $repoRoot 'build.log')
$code = $LASTEXITCODE
Pop-Location

if ($code -ne 0) {
    Write-Host "编译失败，看 build.log。exit=$code"
    exit $code
}

$app = Join-Path $fw 'build\xiaozhi.bin'
Write-Host "编译成功: $app"
Write-Host "烧录（只烧 app 分区，保留配置）: scripts\flash-app.ps1 -Port $Port"
