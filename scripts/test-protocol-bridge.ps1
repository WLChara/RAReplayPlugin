$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$build = Join-Path $repo 'build/protocol-bridge'
$csBuild = Join-Path $build 'csharp'
$cppBuild = Join-Path $build 'cpp'
New-Item -ItemType Directory -Path $csBuild, $cppBuild -Force | Out-Null

$csSources = @(
    (Join-Path $repo 'tests/ProtocolBridge.cs'),
    (Join-Path $repo 'integrations/csharp/RAReplay.Client/ReplayLaunchManifest.cs'),
    (Join-Path $repo 'integrations/csharp/RAReplay.Client/ReplayRuntimeRequest.cs'),
    (Join-Path $repo 'integrations/csharp/RAReplay.Client/ReplayArchive.cs'),
    (Join-Path $repo 'integrations/csharp/RAReplay.Client/ReplayBlockStream.cs')
)
foreach ($source in $csSources) {
    if (-not (Test-Path -LiteralPath $source)) { throw "缺少 C# 接入源码：$source" }
}
$compileItems = ($csSources | ForEach-Object {
    $escaped = [System.Security.SecurityElement]::Escape($_)
    "    <Compile Include=`"$escaped`" />"
}) -join "`n"
$project = Join-Path $csBuild 'ProtocolBridge.csproj'
$projectText = @"
<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup>
    <OutputType>Exe</OutputType>
    <TargetFramework>net9.0</TargetFramework>
    <EnableDefaultCompileItems>false</EnableDefaultCompileItems>
    <GenerateAssemblyInfo>false</GenerateAssemblyInfo>
  </PropertyGroup>
  <ItemGroup>
$compileItems
  </ItemGroup>
</Project>
"@
Set-Content -LiteralPath $project -Value $projectText -Encoding UTF8
& dotnet build $project -c Release --nologo -v quiet
if ($LASTEXITCODE -ne 0) { throw 'C# 协议测试辅助程序编译失败' }
$csExe = Join-Path $csBuild 'bin/Release/net9.0/ProtocolBridge.dll'

& cmake -S $repo -B $cppBuild -G 'Visual Studio 17 2022' -A Win32
if ($LASTEXITCODE -ne 0) { throw 'C++ 协议测试辅助程序配置失败' }
& cmake --build $cppBuild --target ReplayProtocolBridge --config Release --parallel
if ($LASTEXITCODE -ne 0) { throw 'C++ 协议测试辅助程序编译失败' }
$cppExe = Join-Path $cppBuild 'Release/ReplayProtocolBridge.exe'

$tempRoot = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath())
$tempDirectory = Join-Path $tempRoot ('rareplay-protocol-' + [guid]::NewGuid().ToString('N'))
$resolvedTemp = [System.IO.Path]::GetFullPath($tempDirectory)
if (-not $resolvedTemp.StartsWith($tempRoot, [StringComparison]::OrdinalIgnoreCase) -or
    -not [System.IO.Path]::GetFileName($resolvedTemp).StartsWith('rareplay-protocol-', [StringComparison]::Ordinal)) {
    throw '临时目录未通过删除边界验证'
}
New-Item -ItemType Directory -Path $resolvedTemp | Out-Null
try {
    & dotnet $csExe produce $resolvedTemp
    if ($LASTEXITCODE -ne 0) { throw 'C# 清单与请求生成失败' }
    & $cppExe $resolvedTemp
    if ($LASTEXITCODE -ne 0) { throw 'C++ 请求解析与容器写入失败' }
    & dotnet $csExe verify $resolvedTemp
    if ($LASTEXITCODE -ne 0) { throw 'C# 容器与清单校验失败' }
    Write-Host 'RAReplay V2 C#→C++→C# 协议贯通测试通过。'
}
finally {
    if (Test-Path -LiteralPath $resolvedTemp) {
        Remove-Item -LiteralPath $resolvedTemp -Recurse -Force
    }
}
