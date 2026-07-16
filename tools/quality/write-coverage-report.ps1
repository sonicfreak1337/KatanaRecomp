[CmdletBinding()]
param(
    [string]$BuildDirectory = "",
    [string]$SourceDirectory = ""
)

$ErrorActionPreference = "Stop"

$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot "..\.."))
$expectedBuild = [IO.Path]::GetFullPath((Join-Path $repositoryRoot "build-current"))
if ([string]::IsNullOrWhiteSpace($BuildDirectory)) {
    $BuildDirectory = $expectedBuild
}
if ([string]::IsNullOrWhiteSpace($SourceDirectory)) {
    $SourceDirectory = $repositoryRoot
}

$resolvedBuild = [IO.Path]::GetFullPath($BuildDirectory)
$resolvedSource = [IO.Path]::GetFullPath($SourceDirectory)
if (-not $resolvedBuild.Equals($expectedBuild, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Coverage akzeptiert ausschliesslich das Buildverzeichnis build-current/."
}
if (-not $resolvedSource.Equals($repositoryRoot, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Coverage akzeptiert ausschliesslich den KatanaRecomp-Quellbaum."
}
if (-not (Test-Path -LiteralPath $resolvedBuild -PathType Container)) {
    throw "build-current/ fehlt. Zuerst das coverage-debug-Profil konfigurieren und bauen."
}

$backendFile = Join-Path $resolvedBuild "katana-coverage-backend.txt"
if (-not (Test-Path -LiteralPath $backendFile -PathType Leaf)) {
    throw "Der CMake-Coveragevertrag fehlt in build-current/."
}
$backend = (Get-Content -LiteralPath $backendFile -Raw).Trim()
$outputDirectory = [IO.Path]::GetFullPath((Join-Path $resolvedBuild "coverage"))
if (-not $outputDirectory.StartsWith(
        $resolvedBuild + [IO.Path]::DirectorySeparatorChar,
        [StringComparison]::OrdinalIgnoreCase
    )) {
    throw "Coverage-Ausgabe verlaesst build-current/."
}
if (Test-Path -LiteralPath $outputDirectory) {
    Remove-Item -LiteralPath $outputDirectory -Recurse -Force
}
New-Item -ItemType Directory -Path $outputDirectory | Out-Null

$report = Join-Path $outputDirectory "coverage.cobertura.xml"
$log = Join-Path $outputDirectory "coverage.log"

function Invoke-Checked {
    param(
        [Parameter(Mandatory = $true)][string]$Command,
        [Parameter(Mandatory = $true)][string[]]$Arguments
    )
    & $Command @Arguments 2>&1 | Tee-Object -FilePath $log -Append
    if ($LASTEXITCODE -ne 0) {
        throw "Coverage-Befehl '$Command' ist mit Exitcode $LASTEXITCODE fehlgeschlagen."
    }
}

if ($backend -eq "msvc-dynamic") {
    $coverageTool = Get-Command Microsoft.CodeCoverage.Console.exe -ErrorAction SilentlyContinue |
        Select-Object -First 1 -ExpandProperty Source
    if (-not $coverageTool) {
        $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
        if (Test-Path -LiteralPath $vswhere) {
            $installation = (& $vswhere -latest -products * -property installationPath | Select-Object -First 1)
            if ($installation) {
                $candidate = Join-Path $installation "Common7\IDE\Extensions\Microsoft\CodeCoverage.Console\Microsoft.CodeCoverage.Console.exe"
                if (Test-Path -LiteralPath $candidate) {
                    $coverageTool = $candidate
                }
            }
        }
    }
    if (-not $coverageTool) {
        throw "Microsoft.CodeCoverage.Console.exe wurde nicht gefunden."
    }
    Invoke-Checked -Command $coverageTool -Arguments @(
        "collect",
        "--output", $report,
        "--output-format", "cobertura",
        "--include-files", (Join-Path $resolvedSource "*"),
        "--nologo",
        "ctest",
        "--test-dir", $resolvedBuild,
        "--output-on-failure",
        "--no-tests=error"
    )
} elseif ($backend -eq "gcov") {
    $gcovr = Get-Command gcovr -ErrorAction SilentlyContinue |
        Select-Object -First 1 -ExpandProperty Source
    if (-not $gcovr) {
        throw "gcovr wurde fuer das GCC/Clang-Coverageprofil nicht gefunden."
    }
    Invoke-Checked -Command "ctest" -Arguments @(
        "--test-dir", $resolvedBuild,
        "--output-on-failure",
        "--no-tests=error"
    )
    Invoke-Checked -Command $gcovr -Arguments @(
        "--root", $resolvedSource,
        "--object-directory", $resolvedBuild,
        "--exclude", ([Regex]::Escape($resolvedBuild)),
        "--xml-pretty",
        "--output", $report
    )
} else {
    throw "Unbekanntes Coveragebackend '$backend'."
}

if (-not (Test-Path -LiteralPath $report -PathType Leaf) -or
    (Get-Item -LiteralPath $report).Length -eq 0) {
    throw "Coverage hat keinen nichtleeren Cobertura-Bericht erzeugt."
}

Write-Output "Coverage-Bericht: $report"
