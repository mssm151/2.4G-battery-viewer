# build_tray.ps1 —— 用 MinGW g++ 静态编译原生托盘
#
# 构建产物统一放到 dist\（源码与配置仍在项目根目录）：
#   dist\tray_native.exe
$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

$gpp = "D:\mingw64\bin\g++.exe"
if (-not (Test-Path $gpp)) {
    $cmd = Get-Command g++ -ErrorAction SilentlyContinue
    if (-not $cmd) { throw "找不到 g++（MinGW）" }
    $gpp = $cmd.Source
}

$outDir = Join-Path $PSScriptRoot "dist"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$out = Join-Path $outDir "tray_native.exe"

& $gpp `
    -std=c++17 -O2 -s -municode -mwindows `
    -DUNICODE -D_UNICODE `
    -o $out (Join-Path $PSScriptRoot "tray_native.cpp") `
    -static -static-libgcc -static-libstdc++ `
    -lsetupapi -lhid -lshell32 -luser32 -lgdi32

if ($LASTEXITCODE -ne 0) { throw "编译失败：$LASTEXITCODE" }
Write-Host "已生成 dist\tray_native.exe"
