[CmdletBinding()]
param(
    [ValidateRange(1, 256)]
    [int] $Jobs = 24,
    [ValidatePattern('^build-[A-Za-z0-9][A-Za-z0-9._-]*$')]
    [string] $BuildDirectory = 'build-contextual-dirty'
)

$ErrorActionPreference = 'Stop'

function Import-KatanaVisualStudioEnvironment {
    $hasEnvironment =
        -not [string]::IsNullOrWhiteSpace($env:INCLUDE) -and
        -not [string]::IsNullOrWhiteSpace($env:LIB) -and
        $env:VSCMD_ARG_TGT_ARCH -eq 'x64' -and
        $env:VSCMD_ARG_HOST_ARCH -eq 'x64'
    if ($hasEnvironment) { return }

    $vswhere = Join-Path ${env:ProgramFiles(x86)} `
        'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
        throw "Visual Studio locator is missing: $vswhere"
    }
    $installation = & $vswhere -latest -products '*' `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath | Select-Object -First 1
    if ([string]::IsNullOrWhiteSpace($installation)) {
        throw 'Visual Studio with the x64 C++ toolchain is not installed.'
    }
    $devCmd = Join-Path $installation 'Common7\Tools\VsDevCmd.bat'
    if (-not (Test-Path -LiteralPath $devCmd -PathType Leaf)) {
        throw "Visual Studio environment launcher is missing: $devCmd"
    }

    $commandLine =
        'call "' + $devCmd + '" -no_logo -arch=x64 -host_arch=x64 >nul && set'
    $environmentRows = @(& $env:ComSpec /d /s /c $commandLine)
    if ($LASTEXITCODE -ne 0) {
        throw "Visual Studio environment import failed: $LASTEXITCODE"
    }
    foreach ($row in $environmentRows) {
        $separator = $row.IndexOf('=')
        if ($separator -le 0) { continue }
        $name = $row.Substring(0, $separator)
        if ($name.Equals('Path', [StringComparison]::OrdinalIgnoreCase)) {
            continue
        }
        [Environment]::SetEnvironmentVariable(
            $name,
            $row.Substring($separator + 1),
            [EnvironmentVariableTarget]::Process)
    }
    # Environment variable names are case-insensitive on Windows, but cmd.exe
    # preserves the spelling inherited from its parent. Accept the one Path
    # row by name instead of requiring a particular casing; all Path aliases
    # were deliberately skipped above so only this imported value survives.
    $pathRow = $environmentRows |
        Where-Object { $_ -clike 'PATH=*' } |
        Select-Object -First 1
    if ([string]::IsNullOrWhiteSpace($pathRow)) {
        $pathRow = $environmentRows |
            Where-Object {
                $separator = $_.IndexOf('=')
                $separator -gt 0 -and
                    $_.Substring(0, $separator).Equals(
                        'Path', [StringComparison]::OrdinalIgnoreCase)
            } |
            Select-Object -First 1
    }
    if ([string]::IsNullOrWhiteSpace($pathRow)) {
        throw 'Visual Studio environment did not publish Path.'
    }
    # pwsh can preserve both `Path` and `PATH` rows inherited from a native
    # parent. Command discovery follows the uppercase row on this host, so
    # replace that exact alias with VsDevCmd's authoritative value.
    $env:PATH = $pathRow.Substring($pathRow.IndexOf('=') + 1)
}

function Get-KatanaCMakeCacheValue {
    param(
        [Parameter(Mandatory = $true)]
        [string] $CachePath,
        [Parameter(Mandatory = $true)]
        [string] $Name
    )
    $escaped = [Regex]::Escape($Name)
    $match = Select-String -LiteralPath $CachePath `
        -Pattern ('^' + $escaped + ':[^=]*=(.*)$') |
        Select-Object -First 1
    if ($null -eq $match) { return $null }
    return $match.Matches[0].Groups[1].Value
}

$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$buildRoot = [IO.Path]::GetFullPath((Join-Path $repositoryRoot $BuildDirectory))
if (-not $buildRoot.StartsWith(
        $repositoryRoot + [IO.Path]::DirectorySeparatorChar,
        [StringComparison]::OrdinalIgnoreCase)) {
    throw 'The requested build directory escapes the repository root.'
}
if (Test-Path -LiteralPath $buildRoot) {
    $buildItem = Get-Item -LiteralPath $buildRoot -Force
    if (-not $buildItem.PSIsContainer) {
        throw "The requested build path is not a directory: $buildRoot"
    }
    if (($buildItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "The requested build directory is a reparse point: $buildRoot"
    }
}

Import-KatanaVisualStudioEnvironment
if ([string]::IsNullOrWhiteSpace($env:INCLUDE) -or
    [string]::IsNullOrWhiteSpace($env:LIB)) {
    throw 'MSVC environment is incomplete: INCLUDE or LIB is empty.'
}
$compiler = Get-Command cl.exe -ErrorAction Stop
if ($compiler.Source -notmatch 'Hostx64\\x64\\cl\.exe$') {
    throw "Unexpected compiler environment: $($compiler.Source)"
}

$cachePath = Join-Path $buildRoot 'CMakeCache.txt'
if (Test-Path -LiteralPath $cachePath -PathType Leaf) {
    $cacheHome = Get-KatanaCMakeCacheValue $cachePath 'CMAKE_HOME_DIRECTORY'
    $cacheGenerator = Get-KatanaCMakeCacheValue $cachePath 'CMAKE_GENERATOR'
    $cacheCompiler = Get-KatanaCMakeCacheValue $cachePath 'CMAKE_CXX_COMPILER'
    $cacheBuildType = Get-KatanaCMakeCacheValue $cachePath 'CMAKE_BUILD_TYPE'
    $expectedHome = [IO.Path]::GetFullPath($repositoryRoot)
    $observedHome = if ([string]::IsNullOrWhiteSpace($cacheHome)) {
        $null
    } else {
        [IO.Path]::GetFullPath(($cacheHome -replace '/', '\'))
    }
    $observedCompiler = if ([string]::IsNullOrWhiteSpace($cacheCompiler)) {
        $null
    } else {
        [IO.Path]::GetFullPath(($cacheCompiler -replace '/', '\'))
    }
    $expectedCompiler = [IO.Path]::GetFullPath($compiler.Source)
    if (-not [string]::Equals(
            $observedHome, $expectedHome,
            [StringComparison]::OrdinalIgnoreCase) -or
        $cacheGenerator -ne 'Ninja' -or
        -not [string]::Equals(
            $observedCompiler, $expectedCompiler,
            [StringComparison]::OrdinalIgnoreCase) -or
        $cacheBuildType -ne 'Release') {
        throw "Stale or foreign CMake cache at $cachePath. " +
            "Expected source=$expectedHome generator=Ninja compiler=$expectedCompiler " +
            "build_type=Release; observed source=$cacheHome generator=$cacheGenerator " +
            "compiler=$cacheCompiler build_type=$cacheBuildType. " +
            "Use a fresh validated build-* directory."
    }
}

Push-Location -LiteralPath $repositoryRoot
try {
    $configure = @(
        '-S', $repositoryRoot,
        '-B', $buildRoot,
        '-G', 'Ninja',
        '-DCMAKE_BUILD_TYPE=Release',
        '-DKATANA_BUILD_DESKTOP_GUI=OFF',
        '-DKATANA_BUILD_FUZZERS=OFF',
        '-DKATANA_ENABLE_COVERAGE=OFF',
        '-DKATANA_ENABLE_SANITIZERS=OFF',
        '-DKATANA_ENABLE_STATIC_ANALYSIS=OFF',
        '-DKATANA_REPRODUCIBLE_ARTIFACTS=OFF'
    )
    & cmake @configure
    if ($LASTEXITCODE -ne 0) {
        throw "Katana CLI configure failed: $LASTEXITCODE"
    }
    & cmake --build $buildRoot --target katana-recomp --parallel $Jobs
    if ($LASTEXITCODE -ne 0) {
        throw "Katana CLI build failed: $LASTEXITCODE"
    }
}
finally {
    Pop-Location
}

Write-Output "KATANA_CLI_BUILD_SUCCESS jobs=$Jobs build=$buildRoot"
