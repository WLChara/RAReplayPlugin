param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [switch]$SkipTests
)

$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$buildDirectory = Join-Path $repo 'build/v2'
& (Join-Path $PSScriptRoot 'check-boundaries.ps1')
& cmake -S $repo -B $buildDirectory -G 'Visual Studio 17 2022' -A Win32
if ($LASTEXITCODE) { throw 'CMake 配置失败' }
& cmake --build $buildDirectory --config $Configuration --parallel
if ($LASTEXITCODE) { throw '编译失败' }
if (-not $SkipTests) {
    & ctest --test-dir $buildDirectory -C $Configuration --output-on-failure
    if ($LASTEXITCODE) { throw '测试失败' }
}
