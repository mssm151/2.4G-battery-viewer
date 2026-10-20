# build_tray.ps1 —— 用 MinGW g++ 静态编译原生托盘
$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

$gpp = "D:\mingw64\bin\g++.exe"
if (-not (Test-Path $gpp)) {
    $cmd = Get-Command g++ -ErrorAction SilentlyContinue
    if (-not $cmd) { throw "找不到 g++（MinGW）" }
    $gpp = $cmd.Source
}

& $gpp `
    -std=c++17 -O2 -s -municode -mwindows `
    -DUNICODE -D_UNICODE `
    -o tray_native.exe tray_native.cpp `
    -static -static-libgcc -static-libstdc++ `
    -lsetupapi -lhid -lshell32 -luser32 -lgdi32

if ($LASTEXITCODE -ne 0) { throw "编译失败：$LASTEXITCODE" }
Write-Host "已生成 tray_native.exe"
