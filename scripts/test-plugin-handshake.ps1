param(
    [string]$GameExe = 'C:\Program Files (x86)\Mental Omega\gamemd.exe'
)

$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$gamePath = (Resolve-Path -LiteralPath $GameExe).Path
$expectedHash = '7CD005D263FDE203D9C84548200A057A8DF61D724DA3C6BD1E521EEB61CD0747'
if ((Get-Item -LiteralPath $gamePath).Length -ne 4816864 -or
    (Get-FileHash -Algorithm SHA256 -LiteralPath $gamePath).Hash -ne $expectedHash) {
    throw '游戏可执行文件不符合已验证版本'
}

$buildDirectory = Join-Path $repo 'build/v2'
& cmake -S $repo -B $buildDirectory -G 'Visual Studio 17 2022' -A Win32
if ($LASTEXITCODE) { throw 'CMake 配置失败' }
& cmake --build $buildDirectory --config Release --target RAReplayPlugin SyringeHandshakeTests --parallel
if ($LASTEXITCODE) { throw '握手测试编译失败' }

$plugin = (Resolve-Path -LiteralPath (Join-Path $buildDirectory 'Release/RAReplayPlugin.dll')).Path
$helper = (Resolve-Path -LiteralPath (Join-Path $repo 'build/handshake-smoke/SyringeHandshakeTests.exe')).Path
$tempRoot = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath())
$tempDirectory = Join-Path $tempRoot ('rareplay-handshake-' + [Guid]::NewGuid().ToString('N'))
$resolvedTemp = [System.IO.Path]::GetFullPath($tempDirectory)
if (-not $resolvedTemp.StartsWith($tempRoot, [StringComparison]::OrdinalIgnoreCase)) {
    throw '临时目录越界'
}
New-Item -ItemType Directory -Path $resolvedTemp | Out-Null
try {
    Copy-Item -LiteralPath $gamePath -Destination (Join-Path $resolvedTemp 'gamemd.exe')
    Push-Location -LiteralPath $resolvedTemp
    try {
        & $helper $plugin
        if ($LASTEXITCODE) { throw "Syringe 握手烟测失败：$LASTEXITCODE" }
    }
    finally {
        Pop-Location
    }
}
finally {
    if ($resolvedTemp.StartsWith($tempRoot, [StringComparison]::OrdinalIgnoreCase) -and
        (Test-Path -LiteralPath $resolvedTemp)) {
        Remove-Item -LiteralPath $resolvedTemp -Recurse -Force
    }
}
