[CmdletBinding()]
param(
    [string]$SourceDirectory = "",
    [switch]$SelfTest
)

$ErrorActionPreference = "Stop"

$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot "..\.."))
if ([string]::IsNullOrWhiteSpace($SourceDirectory)) {
    $SourceDirectory = $repositoryRoot
}
$resolvedSource = [IO.Path]::GetFullPath($SourceDirectory)
if (-not $resolvedSource.Equals($repositoryRoot, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Referenzaudit akzeptiert ausschliesslich den KatanaRecomp-Quellbaum."
}

function Get-ProhibitionReason {
    param([Parameter(Mandatory = $true)][string]$RelativePath)
    $path = $RelativePath.Replace('\', '/').TrimStart('./').ToLowerInvariant()
    if ($path -match '(^|/)(private|discs?|bios|flash|dumps?)(/|$)') {
        return "privater Firmware- oder Disc-Pfad"
    }
    if ($path -match '(^|/)(vendor|third_party|external)/(flycast|dcrecomp)(/|$)') {
        return "eingebetteter Referenzprojektpfad"
    }
    if ($path -match '(^|/)(dc_boot|dc_flash|bios|flash)\.(bin|rom|img)$') {
        return "bekannter Firmwaredateiname"
    }
    if ($path -match '(^|/)track[0-9]+\.(bin|raw)$') {
        return "kommerzieller Trackdateiname"
    }
    if ($path -match '\.(gdi|cdi|chd|iso)$') {
        return "Disc-Image- oder Deskriptorformat"
    }
    return $null
}

function Get-ForbiddenContentReason {
    param(
        [Parameter(Mandatory = $true)][string]$RelativePath,
        [Parameter(Mandatory = $true)][string]$Content
    )
    if ($RelativePath.Replace('\', '/') -eq 'tools/quality/audit-reference-provenance.ps1') {
        return $null
    }
    if ($Content -match '(?im)^\s*#\s*include\s*[<"][^>"]*(flycast|dcrecomp)[^>"]*[>"]') {
        return "direkter Referenzprojekt-Include"
    }
    if ($Content -match '(?i)(C:\\Users\\[^\\\r\n]+\\|/home/[^/\r\n]+/)') {
        return "absoluter Benutzerpfad"
    }
    return $null
}

if ($SelfTest) {
    $fixturePath = Join-Path $resolvedSource "tests\fixtures\reference_audit_forbidden_paths.txt"
    $fixtures = @(Get-Content -LiteralPath $fixturePath | Where-Object {
        -not [string]::IsNullOrWhiteSpace($_) -and -not $_.StartsWith('#')
    })
    if ($fixtures.Count -lt 7) {
        throw "Referenzaudit-Selbsttest besitzt zu wenige verbotene Marker."
    }
    foreach ($fixture in $fixtures) {
        if (-not (Get-ProhibitionReason $fixture)) {
            throw "Referenzaudit erkennt verbotenen Fixturepfad nicht: $fixture"
        }
    }
    foreach ($safe in @(
        "tests/fixtures/codegen_execution.bin",
        "docs/REFERENCE_PROVENANCE.md",
        "src/platform/firmware_diagnostics.cpp"
    )) {
        if (Get-ProhibitionReason $safe) {
            throw "Referenzaudit lehnt synthetischen oder dokumentierenden Pfad ab: $safe"
        }
    }
    if (-not (Get-ForbiddenContentReason "synthetic.cpp" '#include "flycast/core.hpp"')) {
        throw "Referenzaudit erkennt einen direkten Referenz-Include nicht."
    }
    $syntheticAbsolutePath = 'C:' + '\Users\example\private\disc.gdi'
    if (-not (Get-ForbiddenContentReason "synthetic.txt" $syntheticAbsolutePath)) {
        throw "Referenzaudit erkennt einen absoluten Benutzerpfad nicht."
    }
}

$git = Get-Command git.exe -ErrorAction SilentlyContinue |
    Select-Object -First 1 -ExpandProperty Source
if (-not $git) {
    throw "git.exe wurde fuer den tracked-file Audit nicht gefunden."
}
$tracked = @(& $git -C $resolvedSource ls-files)
if ($LASTEXITCODE -ne 0 -or $tracked.Count -eq 0) {
    throw "Referenzaudit konnte die versionierten Dateien nicht bestimmen."
}

$failures = [Collections.Generic.List[string]]::new()
$textExtensions = @(
    ".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx",
    ".cmake", ".txt", ".md", ".ps1", ".py", ".json", ".yml", ".yaml"
)
foreach ($relative in $tracked) {
    $reason = Get-ProhibitionReason $relative
    if ($reason) {
        $failures.Add("$relative`: $reason")
        continue
    }
    $fullPath = [IO.Path]::GetFullPath((Join-Path $resolvedSource $relative))
    if (-not $fullPath.StartsWith(
            $resolvedSource + [IO.Path]::DirectorySeparatorChar,
            [StringComparison]::OrdinalIgnoreCase
        )) {
        $failures.Add("$relative`: Pfad verlaesst den Quellbaum")
        continue
    }
    if ($textExtensions -notcontains [IO.Path]::GetExtension($fullPath).ToLowerInvariant()) {
        continue
    }
    if ((Get-Item -LiteralPath $fullPath).Length -gt 4MB) {
        $failures.Add("$relative`: Textdatei ist fuer den Audit unerwartet gross")
        continue
    }
    $contentReason = Get-ForbiddenContentReason $relative (Get-Content -LiteralPath $fullPath -Raw)
    if ($contentReason) {
        $failures.Add("$relative`: $contentReason")
    }
}

if ($failures.Count -ne 0) {
    throw "Referenz-/Lizenz-/Firmwareaudit fehlgeschlagen:`n$($failures -join "`n")"
}

Write-Output "KR_REFERENCE_LICENSE_AUDIT_SUCCESS tracked_files=$($tracked.Count) self_test=$($SelfTest.IsPresent)"
