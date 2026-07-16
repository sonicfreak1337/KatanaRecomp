[CmdletBinding()]
param(
    [string]$TestRegex = '',
    [ValidateRange(1, 256)]
    [int]$Parallelism = [Math]::Max(1, [Math]::Min(8, [Environment]::ProcessorCount))
)

$ErrorActionPreference = 'Stop'

function Initialize-MsvcEnvironment {
    if (-not [string]::IsNullOrWhiteSpace($env:INCLUDE)) {
        return
    }

    $vswhere = Join-Path ${env:ProgramFiles(x86)} `
        'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
        return
    }

    $installation = & $vswhere -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath | Select-Object -First 1
    if ([string]::IsNullOrWhiteSpace($installation)) {
        return
    }

    $devCmd = Join-Path $installation 'Common7\Tools\VsDevCmd.bat'
    if (-not (Test-Path -LiteralPath $devCmd -PathType Leaf)) {
        return
    }

    $command = "call `"$devCmd`" -no_logo -arch=x64 -host_arch=x64 >nul && set"
    $environment = & $env:ComSpec /d /s /c $command
    if ($LASTEXITCODE -ne 0) {
        throw 'Die Visual-Studio-Buildumgebung konnte nicht geladen werden.'
    }
    foreach ($entry in $environment) {
        $separator = $entry.IndexOf('=')
        if ($separator -gt 0) {
            [Environment]::SetEnvironmentVariable(
                $entry.Substring(0, $separator),
                $entry.Substring($separator + 1),
                'Process'
            )
        }
    }
}

$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$buildRoot = [IO.Path]::GetFullPath((Join-Path $repositoryRoot 'build-current'))
if (-not $buildRoot.StartsWith(
        $repositoryRoot + [IO.Path]::DirectorySeparatorChar,
        [StringComparison]::OrdinalIgnoreCase
    )) {
    throw 'Das Entwicklungs-Buildziel liegt ausserhalb des Repositorys.'
}

Initialize-MsvcEnvironment

Push-Location -LiteralPath $repositoryRoot
try {
    & cmake --preset debug-gate
    if ($LASTEXITCODE -ne 0) {
        throw "Debug-Konfiguration fehlgeschlagen (Exitcode $LASTEXITCODE)."
    }

    & cmake --build --preset debug-gate --parallel $Parallelism
    if ($LASTEXITCODE -ne 0) {
        throw "Inkrementeller Debug-Build fehlgeschlagen (Exitcode $LASTEXITCODE)."
    }

    $testArguments = @('--preset', 'debug-gate', '--parallel', $Parallelism)
    if (-not [string]::IsNullOrWhiteSpace($TestRegex)) {
        $testArguments += @('--tests-regex', $TestRegex)
    }
    & ctest @testArguments
    if ($LASTEXITCODE -ne 0) {
        throw "Debug-Regression fehlgeschlagen (Exitcode $LASTEXITCODE)."
    }
}
finally {
    Pop-Location
}

Write-Output 'KR_DEV_CYCLE_SUCCESS'
