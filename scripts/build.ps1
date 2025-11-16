param([string]$Config = "RelWithDebInfo")
$ErrorActionPreference = "Stop"
if (!(Test-Path build)) { New-Item -ItemType Directory build | Out-Null }
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config $Config
$exe = Join-Path "build" $Config "DX12MeshSkinningMinimal.exe"
Write-Host "Built: $exe"
