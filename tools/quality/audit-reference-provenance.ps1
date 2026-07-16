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

$allowlistPath = Join-Path $resolvedSource "tools\quality\reference-binary-allowlist.json"
$allowlist = Get-Content -LiteralPath $allowlistPath -Raw | ConvertFrom-Json
if ($allowlist.schema -ne "katana-reference-binary-allowlist" -or $allowlist.version -ne 1) {
    throw "Binaer-Allowlist besitzt keinen stabilen Schemavertrag."
}
$allowedBinaries = @{}
foreach ($entry in $allowlist.files) {
    $key = $entry.path.Replace('\', '/').ToLowerInvariant()
    if ($allowedBinaries.ContainsKey($key) -or
        $entry.size -lt 0 -or $entry.sha256 -notmatch '^[0-9a-f]{64}$') {
        throw "Binaer-Allowlist enthaelt einen ungueltigen oder doppelten Eintrag: $key"
    }
    $allowedBinaries[$key] = $entry
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
    if ($path -notmatch '^tests/' -and
        ($path -match '(^|/)generated([^/]*)\.(c|cc|cpp|cxx|h|hpp)$' -or
         $path -match '(^|/)generated(/|$)')) {
        return "generierter Quellpfad ohne freigegebene Provenienz"
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
    if ($Content.StartsWith("version https://git-lfs.github.com/spec/v1")) {
        return "Git-LFS-Zeiger statt auditierbarem Dateiinhalt"
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
    if (-not (Get-ProhibitionReason "src/generated_game_code.cpp")) {
        throw "Referenzaudit erkennt harmlos benannten generierten Spielcode nicht."
    }
    if (Get-ForbiddenContentReason "asset.dat" "version https://git-lfs.github.com/spec/v1") {
        # Expected: LFS pointers are not accepted as audited binary contents.
    } else {
        throw "Referenzaudit erkennt einen Git-LFS-Zeiger nicht."
    }
    if ($allowedBinaries.ContainsKey("assets/data.dat")) {
        throw "Selbsttestfixture ist unerwartet als Binaerdatei freigegeben."
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
    ".cmake", ".txt", ".md", ".ps1", ".py", ".json", ".yml", ".yaml",
    ".katana", ".overrides"
)
$textNames = @(".gitignore", ".clang-format", ".clang-tidy", "version", "license")
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
    $extension = [IO.Path]::GetExtension($fullPath).ToLowerInvariant()
    $name = [IO.Path]::GetFileName($fullPath).ToLowerInvariant()
    if ($textExtensions -notcontains $extension -and $textNames -notcontains $name) {
        $key = $relative.Replace('\', '/').ToLowerInvariant()
        $entry = $allowedBinaries[$key]
        if (-not $entry) {
            $failures.Add("$relative`: unbekannte Binaerdatei fehlt in Hash-/Groessen-Allowlist")
            continue
        }
        $item = Get-Item -LiteralPath $fullPath
        $hash = (Get-FileHash -LiteralPath $fullPath -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($item.Length -ne $entry.size -or $hash -ne $entry.sha256) {
            $failures.Add("$relative`: Binaerdatei weicht von Hash-/Groessen-Allowlist ab")
        }
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
