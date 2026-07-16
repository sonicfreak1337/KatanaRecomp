[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'

$repositoryRoot = [System.IO.Path]::GetFullPath(
    (Join-Path -Path $PSScriptRoot -ChildPath '..\..')
)
$buildRoot = [System.IO.Path]::GetFullPath(
    (Join-Path -Path $repositoryRoot -ChildPath 'build-current')
)

if (-not $buildRoot.StartsWith(
    $repositoryRoot + [System.IO.Path]::DirectorySeparatorChar,
    [System.StringComparison]::OrdinalIgnoreCase
)) {
    throw 'Das Debug-Gate-Buildziel liegt ausserhalb des Repositorys.'
}

$unexpectedBuilds = Get-ChildItem -LiteralPath $repositoryRoot -Directory -Force |
    Where-Object {
        $_.Name -ne 'build-current' -and
        ($_.Name -eq 'build' -or $_.Name -like 'build-*')
    }
if ($unexpectedBuilds) {
    $names = ($unexpectedBuilds | ForEach-Object Name) -join ', '
    throw "Unerwartete Build-Verzeichnisse gefunden: $names"
}

if (Test-Path -LiteralPath $buildRoot) {
    Remove-Item -LiteralPath $buildRoot -Recurse -Force
}

Push-Location -LiteralPath $repositoryRoot
try {
    & cmake --preset debug-gate
    if ($LASTEXITCODE -ne 0) {
        throw "Debug-Konfiguration fehlgeschlagen (Exitcode $LASTEXITCODE)."
    }

    & cmake --build --preset debug-gate --parallel
    if ($LASTEXITCODE -ne 0) {
        throw "Debug-Build fehlgeschlagen (Exitcode $LASTEXITCODE)."
    }

    & ctest --preset debug-gate
    if ($LASTEXITCODE -ne 0) {
        throw "Debug-Regression fehlgeschlagen (Exitcode $LASTEXITCODE)."
    }
}
finally {
    Pop-Location
}

Write-Host 'KR_DEBUG_GATE_SUCCESS'
