# build_tray.ps1 —— 用 MinGW g++ 静态编译原生托盘
#
# 脚本位于 native\，源码在同一目录，构建产物统一输出到项目根的 dist\：
#   dist\tray_native.exe
$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
$src = Join-Path $PSScriptRoot "tray_native.cpp"
$outDir = Join-Path $root "dist"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$out = Join-Path $outDir "tray_native.exe"

$gpp = "D:\mingw64\bin\g++.exe"
if (-not (Test-Path $gpp)) {
    $cmd = Get-Command g++ -ErrorAction SilentlyContinue
    if (-not $cmd) { throw "找不到 g++（MinGW）" }
    $gpp = $cmd.Source
}

Set-Location $root

& $gpp `
    -std=c++17 -O2 -s -municode -mwindows `
    -DUNICODE -D_UNICODE `
    -o $out $src `
    -static -static-libgcc -static-libstdc++ `
    -lsetupapi -lhid -lshell32 -luser32 -lgdi32

if ($LASTEXITCODE -ne 0) { throw "编译失败：$LASTEXITCODE" }
Write-Host "已生成 dist\tray_native.exe"
