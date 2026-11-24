param(
    # 仓库根目录，默认为本脚本所在仓库；指向副本可做违规注入测试。
    [string]$Root
)
# 回放插件的模块边界检查。模块间允许的依赖以本脚本中的依赖白名单为准。
# - 模块是 src 下的一级目录；hooks/graphics 单独算一个模块，game_api 的子目录都归 game_api。
# - 项目头一律用相对路径包含：引号 include 先相对当前文件目录、再相对 src 解析，落在 src 内即为项目头；
#   解析不到 src 内文件的按第三方头处理（ImGui、MinHook、YRpp、平台与标准库）。
# - 目录级依赖不得成环（节点是 src 下的每个目录，含 game_api 的子目录）。
# - hooks 中每个 DEFINE_HOOK 的地址必须等于 TargetAbi.h 中某个地址常量（标量或数组元素）的值。
# - 只有 storage 可以包含 zlib 头（zlib.h、zconf.h）：回放分块压缩只在 storage 中实现。
# - tools 与 tests 不检查。
$ErrorActionPreference = 'Stop'
if (-not $Root) { $Root = Join-Path $PSScriptRoot '..' }
$Root = (Resolve-Path -LiteralPath $Root).Path
$srcRoot = [IO.Path]::GetFullPath((Join-Path $Root 'src')).TrimEnd('\', '/')
$srcPrefix = $srcRoot + [IO.Path]::DirectorySeparatorChar
$cmakePath = Join-Path $Root 'CMakeLists.txt'

# 模块白名单：键为模块，值为允许依赖的其他项目模块（模块自身总是允许）。
$moduleDependencies = @{
    'core' = @()
    'replay' = @()
    'viewer' = @()
    'target' = @()
    'storage' = @('replay')
    'stats' = @('viewer')
    'assets' = @('viewer')
    'zoom' = @()
    'ui' = @('viewer')
    'render' = @('ui', 'viewer', 'core')
    'hooks/graphics' = @('render', 'viewer')
    'game_api' = @('replay', 'viewer', 'stats', 'assets', 'zoom', 'core', 'target')
    'hooks' = @('plugin', 'game_api', 'target')
    'plugin' = @('game_api', 'stats', 'storage', 'replay', 'viewer', 'zoom', 'target', 'hooks/graphics', 'core')
}
# 可以出现游戏映像地址字面量的适配层模块；hooks 只允许 DEFINE_HOOK 的地址参数（Syringe 宏要求字面量），其余地址引用 TargetAbi。
$addressModules = @('target', 'game_api')
# 只有游戏适配层与 Syringe Hook 可以直接包含 YRpp 头（Syringe.h 也属于 YRpp）。
$yrppModules = @('game_api', 'hooks')
$windowsForbiddenModules = @('viewer', 'ui', 'assets', 'replay')
$imguiForbiddenModules = @('viewer', 'assets')
$kieroForbiddenModules = @('viewer', 'ui')
# 回放容器 v3 的分块压缩（ReplayBlockCodec）只在 storage；ReplayZlib 也只由 ReplayStorage 私有链接。
$zlibModules = @('storage')
$zlibHeaders = @('zlib.h', 'zconf.h')
# 第三方与平台头一律禁止（Windows.h、YRpp、ImGui、MinHook 都不是标准库头）；项目依赖仍由白名单约束。
$standardLibraryOnlyModules = @('core', 'stats', 'zoom')
$cStandardHeaderPattern = '^(?:assert|ctype|errno|fenv|float|inttypes|iso646|limits|locale|math|setjmp|signal|stdalign|stdarg|stdbool|stddef|stdint|stdio|stdlib|stdnoreturn|string|time|uchar|wchar|wctype)\.h$'
# gamemd.exe 的映像范围（基址 0x400000）。
$imageBegin = [UInt64]0x400000
$imageEnd = [UInt64]0xBFFFFF

# 显式例外，集中登记于此。每条按“文件相对路径 + 规则 + 匹配对象（include 路径或字面量原文）”精确匹配；
# 某条例外若已匹配不到任何代码，也按违规报告，防止过期例外一直留着。
$exceptions = @(
    @{ File = 'src/viewer/ReplayViewModel.h'; Rule = '裸地址'; Subject = '0x8A8F86'
       Reason = 'RadarPlayerRef::mColorRgb 的 RGB 颜色默认值（中性灰），不是地址' }
    @{ File = 'src/viewer/SuperWeaponMarks.h'; Rule = '裸地址'; Subject = '0x8A8F86'
       Reason = 'SuperWeaponMarkInfo::mOwnerColorRgb 的 RGB 颜色默认值（中性灰），不是地址' }
    @{ File = 'src/plugin/Entry.cpp'; Rule = 'YRpp 头'; Subject = 'Syringe.h'
       Reason = 'Syringe 握手导出（SYRINGE_HANDSHAKE），DLL 入口；是否迁入 hooks 待审查报告 P2 清理时再定' }
)
$exceptionHits = New-Object 'int[]' $exceptions.Count

$violations = New-Object System.Collections.Generic.List[string]
function Add-Violation([string]$file, [int]$line, [string]$rule, [string]$detail, [string]$subject) {
    for ($index = 0; $index -lt $exceptions.Count; $index++) {
        $exception = $exceptions[$index]
        if ($subject -and $exception.File -eq $file -and $exception.Rule -eq $rule -and $exception.Subject -ceq $subject) {
            $exceptionHits[$index]++
            return
        }
    }
    $location = if ($line -gt 0) { "${file}:$line" } else { $file }
    $violations.Add("${location}: [$rule] $detail")
}

function Get-RelativeSourcePath([string]$fullPath) {
    $full = [IO.Path]::GetFullPath($fullPath)
    if (-not $full.StartsWith($srcPrefix, [StringComparison]::OrdinalIgnoreCase)) { return $null }
    return $full.Substring($srcPrefix.Length).Replace('\', '/')
}

function Get-ModuleName([string]$relativePath) {
    $parts = $relativePath.Split('/')
    if ($parts.Count -lt 2) { return '' }
    if ($parts[0] -eq 'hooks' -and $parts.Count -ge 3 -and $parts[1] -eq 'graphics') { return 'hooks/graphics' }
    return $parts[0].ToLowerInvariant()
}

function Get-DirectoryName([string]$relativePath) {
    $index = $relativePath.LastIndexOf('/')
    if ($index -lt 0) { return '' }
    return $relativePath.Substring(0, $index)
}

# 目录级依赖边：起点目录 -> (终点目录 -> 第一条依据)。
$directoryEdges = @{}
function Add-DirectoryEdge([string]$from, [string]$to, [string]$evidence) {
    if (-not $directoryEdges.ContainsKey($from)) { $directoryEdges[$from] = @{} }
    if (-not $directoryEdges[$from].ContainsKey($to)) { $directoryEdges[$from][$to] = $evidence }
}

function Get-LineNumber([int[]]$newlines, [int]$index) {
    $position = [Array]::BinarySearch($newlines, $index)
    if ($position -lt 0) { $position = -bnot $position }
    return $position + 1
}

function Get-BlankedText([string]$text) {
    return [regex]::Replace($text, '[^\r\n]', ' ')
}

# 注释、字符串与字符字面量；字符字面量要求引号前不是标识符字符，以免把 0x55'D360 这类数字分隔符当成字符。
$lexicalPattern = @(
    '(?<comment>//[^\n]*|/\*[\s\S]*?\*/)',
    '(?<![0-9A-Za-z_])(?:u8|[uUL])?R"(?<delim>[^()\\\s]{0,16})\([\s\S]*?\)\k<delim>"',
    '(?:(?<![0-9A-Za-z_])(?:u8|[uUL]))?"(?:\\.|[^"\\\n])*"',
    '(?<![0-9A-Za-z_])(?:u8|[uUL])''(?:\\.|[^''\\\n])+''',
    '(?<![0-9A-Za-z_])''(?:\\.|[^''\\\n])+'''
) -join '|'
$lexicalRegex = New-Object regex $lexicalPattern
$includeRegex = New-Object regex '(?m)^[ \t]*#[ \t]*include[ \t]*([<"])([^">\r\n]+)[">]'
$hexRegex = New-Object regex '(?<![\w.])0[xX]([0-9A-Fa-f](?:''?[0-9A-Fa-f])*)'

# 返回两份等长文本：只去掉注释（用于 include）与同时去掉字符串（用于代码记号）。
function Split-SourceText([string]$text) {
    $withoutComments = New-Object Text.StringBuilder $text.Length
    $code = New-Object Text.StringBuilder $text.Length
    $last = 0
    foreach ($match in $lexicalRegex.Matches($text)) {
        $before = $text.Substring($last, $match.Index - $last)
        [void]$withoutComments.Append($before)
        [void]$code.Append($before)
        $blank = Get-BlankedText $match.Value
        if ($match.Groups['comment'].Success) {
            [void]$withoutComments.Append($blank)
        } else {
            [void]$withoutComments.Append($match.Value)
        }
        [void]$code.Append($blank)
        $last = $match.Index + $match.Length
    }
    $rest = $text.Substring($last)
    [void]$withoutComments.Append($rest)
    [void]$code.Append($rest)
    return @($withoutComments.ToString(), $code.ToString())
}

if (-not (Test-Path -LiteralPath $srcRoot -PathType Container)) { throw "找不到源码目录：$srcRoot" }
$cmake = Get-Content -Raw -Encoding UTF8 -LiteralPath $cmakePath

# YRpp SDK 目录取自 CMake 的 target_include_directories；头文件在其中能找到即视为 YRpp 头。
$yrppDirectories = New-Object System.Collections.Generic.List[string]
foreach ($directive in [regex]::Matches($cmake, '(?is)target_include_directories\s*\(([^)]*)\)')) {
    foreach ($token in ($directive.Groups[1].Value -split '\s+')) {
        if ($token -notmatch '(?i)yrpp') { continue }
        $expanded = $token.Trim('"').Replace('${CMAKE_SOURCE_DIR}', $Root).Replace('${CMAKE_CURRENT_SOURCE_DIR}', $Root).Replace('${PROJECT_SOURCE_DIR}', $Root)
        $directory = if ([IO.Path]::IsPathRooted($expanded)) { $expanded } else { Join-Path $Root $expanded }
        if (Test-Path -LiteralPath $directory -PathType Container) {
            $fullDirectory = [IO.Path]::GetFullPath($directory).TrimEnd('\', '/')
            if (-not $yrppDirectories.Contains($fullDirectory)) { $yrppDirectories.Add($fullDirectory) }
        }
    }
}
if ($yrppDirectories.Count -eq 0) { throw '未能从 CMakeLists.txt 的 target_include_directories 找到 YRpp SDK 目录' }

function Test-YrppHeader([string]$includePath, [string]$resolvedPath) {
    foreach ($directory in $yrppDirectories) {
        $prefix = $directory + [IO.Path]::DirectorySeparatorChar
        $candidates = @($resolvedPath)
        try { $candidates += [IO.Path]::GetFullPath([IO.Path]::Combine($directory, $includePath)) } catch { }
        foreach ($candidate in $candidates) {
            if ($candidate -and $candidate.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase) -and
                (Test-Path -LiteralPath $candidate -PathType Leaf)) { return $true }
        }
    }
    return $false
}

# TargetAbi.h 中的地址常量：std::uintptr_t 标量与 std::array<std::uintptr_t, N> 的元素。
$targetAbiPath = Join-Path $srcRoot 'target/TargetAbi.h'
$targetAbiAddresses = @{}
if (Test-Path -LiteralPath $targetAbiPath -PathType Leaf) {
    $targetAbiCode = (Split-SourceText (Get-Content -Raw -Encoding UTF8 -LiteralPath $targetAbiPath))[1]
    $constantPattern = 'constexpr\s+std::(?:uintptr_t|array\s*<\s*std::uintptr_t\s*,\s*\d+\s*>)\s+(\w+)\s*(?:=\s*)?(\{[^}]*\}|[^;{]+);'
    foreach ($constant in [regex]::Matches($targetAbiCode, $constantPattern)) {
        foreach ($literal in $hexRegex.Matches($constant.Groups[2].Value)) {
            $key = '{0:X}' -f [Convert]::ToUInt64($literal.Groups[1].Value.Replace("'", ''), 16)
            if (-not $targetAbiAddresses.ContainsKey($key)) { $targetAbiAddresses[$key] = $constant.Groups[1].Value }
        }
    }
}
if ($targetAbiAddresses.Count -eq 0) { throw "未能从 $targetAbiPath 读取任何地址常量" }

$sourceFiles = @(Get-ChildItem -LiteralPath $srcRoot -File -Recurse | Where-Object { $_.Extension -in '.h', '.cpp', '.hpp', '.inl' })
# 每个项目文件路径的全部后缀，用于发现借 include 目录而非相对路径包含的项目头。
$projectSuffixes = @{}
foreach ($file in $sourceFiles) {
    $parts = (Get-RelativeSourcePath $file.FullName).ToLowerInvariant().Split('/')
    for ($index = 0; $index -lt $parts.Count; $index++) {
        $projectSuffixes[($parts[$index..($parts.Count - 1)] -join '/')] = $true
    }
}

function Resolve-QuotedInclude([string]$directory, [string]$includePath) {
    foreach ($base in @($directory, $srcRoot)) {
        try {
            $candidate = [IO.Path]::GetFullPath([IO.Path]::Combine($base, $includePath))
        } catch {
            continue
        }
        if (Test-Path -LiteralPath $candidate -PathType Leaf) { return $candidate }
    }
    return $null
}

$includeCount = 0
foreach ($file in $sourceFiles) {
    $relative = Get-RelativeSourcePath $file.FullName
    $display = "src/$relative"
    $module = Get-ModuleName $relative
    $text = Get-Content -Raw -Encoding UTF8 -LiteralPath $file.FullName
    if ($null -eq $text) { $text = '' }
    $newlines = [int[]]@([regex]::Matches($text, "`n") | ForEach-Object { $_.Index })
    $views = Split-SourceText $text
    $withoutComments = $views[0]
    $code = $views[1]
    if (-not $moduleDependencies.ContainsKey($module)) {
        Add-Violation $display 0 '未登记模块' "模块 '$module' 不在依赖白名单中，需先在本脚本的依赖白名单中登记"
        continue
    }

    foreach ($include in $includeRegex.Matches($withoutComments)) {
        $includeCount++
        $line = Get-LineNumber $newlines $include.Index
        $isQuoted = $include.Groups[1].Value -eq '"'
        $includePath = $include.Groups[2].Value.Trim()
        $shown = if ($isQuoted) { "#include `"$includePath`"" } else { "#include <$includePath>" }
        $resolved = if ($isQuoted) { Resolve-QuotedInclude $file.DirectoryName $includePath } else { $null }
        $resolvedRelative = if ($resolved) { Get-RelativeSourcePath $resolved } else { $null }

        if ($resolvedRelative) {
            $targetModule = Get-ModuleName $resolvedRelative
            $fromDirectory = Get-DirectoryName $relative
            $toDirectory = Get-DirectoryName $resolvedRelative
            if ($fromDirectory -ne $toDirectory) {
                Add-DirectoryEdge $fromDirectory $toDirectory "${display}:$line $shown"
            }
            if ($targetModule -ne $module -and $moduleDependencies[$module] -notcontains $targetModule) {
                $targetName = if ($targetModule) { $targetModule } else { 'src 根目录' }
                Add-Violation $display $line '模块白名单' "$module 不得依赖 $targetName（$shown）"
            }
            continue
        }

        # 以下均为第三方或平台头。
        $normalized = $includePath.Replace('\', '/').ToLowerInvariant()
        $fileName = [IO.Path]::GetFileName($normalized)
        if ($projectSuffixes.ContainsKey($normalized.TrimStart('.', '/'))) {
            Add-Violation $display $line '相对路径包含' "项目头必须用相对路径包含，不得依赖 include 目录（$shown）"
            continue
        }
        if ($module -in $standardLibraryOnlyModules) {
            $isStandard = -not $isQuoted -and $normalized -notmatch '[/]' -and
                ($normalized -notmatch '\.h(?:pp)?$' -or ($module -eq 'core' -and $fileName -match $cStandardHeaderPattern) -or
                    ($module -eq 'core' -and $fileName -eq 'windows.h' -and $file.Extension -eq '.cpp'))
            if (-not $isStandard) {
                Add-Violation $display $line '标准库限定' "$module 只能包含标准库头（core 的 .cpp 另可包含 Windows.h）（$shown）"
                continue
            }
        }
        if ((Test-YrppHeader $includePath $resolved) -and $module -notin $yrppModules) {
            Add-Violation $display $line 'YRpp 头' "只有 game_api 与 hooks 可以直接包含 YRpp 头（$shown）" $includePath
        }
        if ($normalized -match 'minhook' -and $module -ne 'hooks/graphics') {
            Add-Violation $display $line 'MinHook' "只有 hooks/graphics 可以包含 MinHook（$shown）"
        }
        if ($fileName -in $zlibHeaders -and $module -notin $zlibModules) {
            Add-Violation $display $line 'zlib' "只有 storage 可以包含 zlib（$shown）"
        }
        if ($normalized -match 'kiero' -and $module -in $kieroForbiddenModules) {
            Add-Violation $display $line 'kiero' "$module 不得包含旧 Hook 包装 kiero（$shown）"
        }
        if ($fileName -eq 'windows.h' -and $module -in $windowsForbiddenModules) {
            Add-Violation $display $line 'Windows.h' "$module 不得包含 Windows.h（$shown）"
        }
        if ($fileName -match '^imgui' -and $module -in $imguiForbiddenModules) {
            Add-Violation $display $line 'ImGui' "$module 不得包含 ImGui（$shown）"
        }
        if ($fileName -match '^imgui_impl_' -and $module -eq 'ui') {
            Add-Violation $display $line 'ImGui 后端' "ui 不得包含 ImGui 平台或图形后端（$shown）"
        }
    }

    if ($module -eq 'core' -and $file.Extension -eq '.h') {
        $win32 = [regex]::Match($code, '\b(?:HANDLE|HWND|HDC|HMODULE|DWORD|UINT|WCHAR|LPWSTR|LPCWSTR|__stdcall|__fastcall|__thiscall)\b')
        if ($win32.Success) {
            Add-Violation $display (Get-LineNumber $newlines $win32.Index) 'Core 公共头' "公共头不得暴露 Win32 类型或调用约定（$($win32.Value)）"
        }
    }
    if ($module -eq 'replay') {
        foreach ($convention in [regex]::Matches($code, '\b(?:__fastcall|__thiscall|__stdcall)\b')) {
            Add-Violation $display (Get-LineNumber $newlines $convention.Index) '调用约定' "replay 不得出现调用约定（$($convention.Value)）"
        }
    }
    # DEFINE_HOOK 的地址参数只能是字面量（Syringe 以 token 拼接生成声明名），因此逐个核对它等于 TargetAbi 中的常量。
    $hookAddressIndexes = @{}
    if ($module -eq 'hooks') {
        foreach ($declaration in [regex]::Matches($code, 'DEFINE_HOOK(?:_AGAIN)?\s*\(\s*([^,]+?)\s*,\s*[^,]+,\s*([^\)]+)\)')) {
            $line = Get-LineNumber $newlines $declaration.Index
            $addressText = $declaration.Groups[1].Value
            $hookAddressIndexes[$declaration.Groups[1].Index] = $true
            if ($addressText -notmatch '^0[xX][0-9a-fA-F]+$') {
                Add-Violation $display $line 'Hook 地址' "DEFINE_HOOK 的地址必须是十六进制字面量（$addressText）"
            } elseif (-not $targetAbiAddresses.ContainsKey(('{0:X}' -f [Convert]::ToUInt64($addressText.Substring(2), 16)))) {
                Add-Violation $display $line 'Hook 地址' "DEFINE_HOOK 地址 $addressText 不等于 src/target/TargetAbi.h 中任何地址常量"
            }
            $sizeText = $declaration.Groups[2].Value.Trim()
            if ($sizeText -notmatch '^(?:0[xX][0-9a-fA-F]+|[0-9]+)$') {
                Add-Violation $display $line 'Hook 覆盖长度' "覆盖字节数必须是数字字面量（$sizeText）"
                continue
            }
            $radix = if ($sizeText.StartsWith('0x', [StringComparison]::OrdinalIgnoreCase)) { 16 } else { 10 }
            if ([Convert]::ToInt64(($sizeText -replace '^0[xX]', ''), $radix) -lt 5) {
                Add-Violation $display $line 'Hook 覆盖长度' "覆盖字节数至少为 5（$sizeText）"
            }
        }
    }
    if ($module -notin $addressModules) {
        foreach ($literal in $hexRegex.Matches($code)) {
            if ($hookAddressIndexes.ContainsKey($literal.Index)) { continue }
            $digits = $literal.Groups[1].Value.Replace("'", '')
            if ($digits.Length -gt 16) { continue }
            $value = [Convert]::ToUInt64($digits, 16)
            if ($value -ge $imageBegin -and $value -le $imageEnd) {
                Add-Violation $display (Get-LineNumber $newlines $literal.Index) '裸地址' "$module 不得出现游戏映像范围 0x400000–0xBFFFFF 内的地址字面量（$($literal.Value)）；地址只能放在 target 或 game_api，hooks 只在 DEFINE_HOOK 中写字面量" $literal.Value
            }
        }
    }
}

# 目录级依赖环检测（Tarjan 强连通分量）；每个环打印一条路径及每条边的 include 依据。
$script:visitCounter = 0
$visitIndex = @{}
$visitLow = @{}
$onVisitStack = @{}
$visitStack = New-Object System.Collections.Generic.Stack[string]
$cycleComponents = New-Object System.Collections.Generic.List[object]
function Visit-Directory([string]$node) {
    $visitIndex[$node] = $script:visitCounter
    $visitLow[$node] = $script:visitCounter
    $script:visitCounter++
    $visitStack.Push($node)
    $onVisitStack[$node] = $true
    if ($directoryEdges.ContainsKey($node)) {
        foreach ($next in @($directoryEdges[$node].Keys | Sort-Object)) {
            if (-not $visitIndex.ContainsKey($next)) {
                Visit-Directory $next
                $visitLow[$node] = [Math]::Min($visitLow[$node], $visitLow[$next])
            } elseif ($onVisitStack[$next]) {
                $visitLow[$node] = [Math]::Min($visitLow[$node], $visitIndex[$next])
            }
        }
    }
    if ($visitLow[$node] -eq $visitIndex[$node]) {
        $component = New-Object System.Collections.Generic.List[string]
        do {
            $member = $visitStack.Pop()
            $onVisitStack[$member] = $false
            $component.Add($member)
        } while ($member -ne $node)
        if ($component.Count -gt 1) { $cycleComponents.Add($component) }
    }
}
foreach ($node in @($directoryEdges.Keys | Sort-Object)) {
    if (-not $visitIndex.ContainsKey($node)) { Visit-Directory $node }
}
foreach ($component in $cycleComponents) {
    # 在强连通分量内从字典序最小的目录出发，广度优先找回到自身的最短路径。
    $start = @($component | Sort-Object)[0]
    $parents = @{}
    $queue = New-Object System.Collections.Generic.Queue[string]
    $queue.Enqueue($start)
    $closed = $false
    while ($queue.Count -gt 0 -and -not $closed) {
        $current = $queue.Dequeue()
        foreach ($next in @($directoryEdges[$current].Keys | Sort-Object)) {
            if (-not $component.Contains($next)) { continue }
            if ($next -eq $start) { $parents['<cycle>'] = $current; $closed = $true; break }
            if (-not $parents.ContainsKey($next)) { $parents[$next] = $current; $queue.Enqueue($next) }
        }
    }
    $path = New-Object System.Collections.Generic.List[string]
    $path.Add($start)
    $cursor = $parents['<cycle>']
    while ($cursor -ne $start) { $path.Insert(1, $cursor); $cursor = $parents[$cursor] }
    $path.Add($start)
    $labels = @($path | ForEach-Object { if ($_) { $_ } else { '(src)' } })
    $evidence = @(for ($index = 0; $index -lt $path.Count - 1; $index++) { $directoryEdges[$path[$index]][$path[$index + 1]] })
    Add-Violation "src/$start" 0 '依赖环' ("目录依赖成环：$($labels -join ' -> ')；依据：" + ($evidence -join '；'))
}

if ($cmake -match '(?is)target_link_libraries\s*\(\s*ReplayCore\b[^)]*\b(?:Replay\w+|RAReplayPlugin)\b') {
    Add-Violation 'CMakeLists.txt' 0 'CMake' 'ReplayCore 不得链接其他项目模块'
}

for ($index = 0; $index -lt $exceptions.Count; $index++) {
    if ($exceptionHits[$index] -eq 0) {
        $exception = $exceptions[$index]
        Add-Violation $exception.File 0 '过期例外' "例外 '$($exception.Rule)：$($exception.Subject)' 已匹配不到任何代码，应从例外表删除"
    }
}

if ($violations.Count -gt 0) {
    foreach ($violation in $violations) { Write-Host $violation }
    throw "模块边界检查失败：$($violations.Count) 处违规"
}
Write-Host "Replay module boundaries verified: $($sourceFiles.Count) files, $includeCount includes, $($exceptions.Count) explicit exceptions."
