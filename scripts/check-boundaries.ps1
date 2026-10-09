$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
# Core 只提供通用能力；Win32 机制可以留在实现内，但不得进入公共头文件。
$coreRoot = Join-Path $repo 'src/core'
if (Test-Path -LiteralPath $coreRoot) {
    $corePrefix = [IO.Path]::GetFullPath($coreRoot).TrimEnd([IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
    foreach ($file in (Get-ChildItem -LiteralPath $coreRoot -File -Recurse -Include '*.h','*.cpp')) {
        $content = Get-Content -Raw -Encoding UTF8 -LiteralPath $file.FullName
        foreach ($include in [regex]::Matches($content, '(?m)^\s*#\s*include\s*([<"])([^">]+)[">]')) {
            $includePath = $include.Groups[2].Value
            if ($includePath -eq 'Windows.h' -and $file.Extension -eq '.cpp') {
                continue
            } elseif ($include.Groups[1].Value -eq '"') {
                $resolvedInclude = [IO.Path]::GetFullPath((Join-Path $file.DirectoryName $includePath))
                if (-not $resolvedInclude.StartsWith($corePrefix, [StringComparison]::OrdinalIgnoreCase) -or
                    -not (Test-Path -LiteralPath $resolvedInclude -PathType Leaf)) {
                    throw "Core depends on another module: $($file.FullName): $includePath"
                }
            } elseif ($includePath -match '^(?:assert|ctype|errno|fenv|float|inttypes|iso646|limits|locale|math|setjmp|signal|stdalign|stdarg|stdbool|stddef|stdint|stdio|stdlib|stdnoreturn|string|time|uchar|wchar|wctype)\.h$') {
                continue
            } elseif ($includePath -match '(?i)[/\\]|\.h(?:pp)?$|imgui|kiero|MinHook|yrpp') {
                throw "Core exposes or depends on an SDK/UI/platform header: $($file.FullName): $includePath"
            }
        }
        if ($file.Extension -eq '.h' -and $content -match '\b(?:HANDLE|HWND|HDC|HMODULE|DWORD|UINT|WCHAR|LPWSTR|LPCWSTR|__stdcall|__fastcall|__thiscall)\b') {
            throw "Core header exposes Win32 types or calling conventions: $($file.FullName)"
        }
    }
}
$hookFiles = Get-ChildItem -LiteralPath (Join-Path $repo 'src/hooks') -File -Include '*.cpp' -Recurse
foreach ($file in $hookFiles) {
    $content = Get-Content -Raw -Encoding UTF8 -LiteralPath $file.FullName
    foreach ($declaration in [regex]::Matches($content, 'DEFINE_HOOK(?:_AGAIN)?\s*\(\s*[^,]+,\s*[^,]+,\s*([^\)]+)\)')) {
        $sizeText = $declaration.Groups[1].Value.Trim()
        if ($sizeText -notmatch '^(?:0[xX][0-9a-fA-F]+|[0-9]+)$') {
            throw "Hook coverage must be a numeric literal: $($file.FullName): $sizeText"
        }
        $radix = if ($sizeText.StartsWith('0x', [System.StringComparison]::OrdinalIgnoreCase)) { 16 } else { 10 }
        $size = [Convert]::ToInt64($sizeText.Replace('0x', '').Replace('0X', ''), $radix)
        if ($size -lt 5) {
            throw "Hook coverage must be at least 5 bytes: $($file.FullName): $sizeText"
        }
    }
}
$replayFiles = Get-ChildItem -LiteralPath (Join-Path $repo 'src/replay') -File -Include '*.h','*.cpp' -Recurse
$forbidden = '(?i)#\s*include\s*[<"](?:Windows\.h|.*(?:yrpp|MinHook|target|game_api)[/\\])|\b(?:__fastcall|__thiscall|__stdcall|0x(?:0040|00A8|008B|00B0)[0-9A-F]{4})'
foreach ($file in $replayFiles) {
    $content = Get-Content -Raw -Encoding UTF8 -LiteralPath $file.FullName
    if ($content -match $forbidden) { throw "ReplayDomain boundary violated: $($file.FullName)" }
}
$cmake = Get-Content -Raw -Encoding UTF8 -LiteralPath (Join-Path $repo 'CMakeLists.txt')
if ($cmake -match '(?is)target_link_libraries\s*\(\s*ReplayCore\b[^)]*\b(?:Replay\w+|RAReplayPlugin)\b') {
    throw 'ReplayCore depends on another project module'
}
if ($cmake -match '(?i)src/launcher|src/game/|NativeReplay|ReplayInitialize') {
    throw 'Obsolete launcher or native hook source remains in production CMake'
}
# 图形呈现允许在 hooks/graphics 使用 kiero/MinHook；领域和 UI 仍不得访问引擎。
foreach ($folder in @('src/viewer', 'src/ui', 'src/render')) {
    $directory = Join-Path $repo $folder
    if (-not (Test-Path -LiteralPath $directory)) { continue }
    foreach ($file in (Get-ChildItem -LiteralPath $directory -File -Recurse -Include '*.h','*.cpp')) {
        $content = Get-Content -Raw -Encoding UTF8 -LiteralPath $file.FullName
        if ($content -match '(?i)#\s*include\s*[<"][^">]*(?:yrpp|game_api|target)[/\\]|#\s*include\s*[<"](?:HouseClass|TechnoClass|Unsorted|Syringe)\.h') {
            throw "Viewer/render engine boundary violated: $($file.FullName)"
        }
        if ($folder -ne 'src/render' -and $content -match '(?i)#\s*include\s*[<"](?:Windows\.h|.*(?:kiero|MinHook|imgui_impl_|hooks[/\\]|render[/\\]))') {
            throw "UI platform boundary violated: $($file.FullName)"
        }
        if ($folder -eq 'src/viewer' -and $content -match '(?i)#\s*include\s*[<"][^">]*imgui') {
            throw "Viewer model depends on ImGui: $($file.FullName)"
        }
    }
}
$productionFiles = Get-ChildItem -LiteralPath (Join-Path $repo 'src') -File -Include '*.h','*.cpp' -Recurse
foreach ($file in $productionFiles) {
    $content = Get-Content -Raw -Encoding UTF8 -LiteralPath $file.FullName
    if ($content -match '(?i)NativeReplay|HookOpen|HookWrite|ReplayInitialize') {
        throw "Obsolete replay entry remains: $($file.FullName)"
    }
}
Write-Host 'Replay module boundaries verified.'
