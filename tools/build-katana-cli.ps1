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
    # A parent process can expose both Path and PATH. VsDevCmd's uppercase row
    # is authoritative; never let a stale mixed-case alias drop the SDK paths.
    $pathRow = $environmentRows |
        Where-Object { $_ -clike 'PATH=*' } |
        Select-Object -First 1
    if ([string]::IsNullOrWhiteSpace($pathRow)) {
        throw 'Visual Studio environment did not publish PATH.'
    }
    $env:Path = $pathRow.Substring(5)
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
