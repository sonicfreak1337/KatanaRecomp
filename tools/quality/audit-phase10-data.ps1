[CmdletBinding()]
param(
    [string]$BuildDirectory = "",
    [switch]$SelfTest
)

$ErrorActionPreference = "Stop"
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot "..\.."))
if ([string]::IsNullOrWhiteSpace($BuildDirectory)) {
    $BuildDirectory = Join-Path $root "build-current"
}
$build = [IO.Path]::GetFullPath($BuildDirectory)
if (-not $build.StartsWith(
        $root + [IO.Path]::DirectorySeparatorChar,
        [StringComparison]::OrdinalIgnoreCase
    )) {
    throw "Phase-10-Datenaudit akzeptiert nur das Buildziel im Repository."
}

function Get-Phase10DataViolation {
    param([Parameter(Mandatory = $true)][string]$Content)
    $windowsUserRoot = [regex]::Escape(('C:' + '\' + 'Users' + '\'))
    $posixHomeRoot = [regex]::Escape(('/' + 'home' + '/'))
    if ($Content -match "(?i)($windowsUserRoot[^\\]+\\|$posixHomeRoot[^/]+/)") {
        return "absoluter lokaler Pfad"
    }
    if ($Content -match '(?i)(KATANA_SYNTHETIC_SECRET_MARKER|factory[_-]?data|network[_-]?id|serial[_-]?number)') {
        return "unredigiertes Geheimnis oder Identifikator"
    }
    if ($Content -match '(?i)(DC_BOOT\.BIN|DC_FLASH\.BIN|firmware[_-]?bytes|bios[_-]?bytes|flash[_-]?bytes)') {
        return "Firmware-, Boot- oder Flashdatenmarker"
    }
    if ($Content -match '(?i)"(trace_bytes|memory_dump|firmware_payload|source_bytes)"\s*:') {
        return "unredigierter Quell-, Trace- oder Speicherinhalt"
    }
    return $null
}

if ($SelfTest) {
    foreach ($marker in @(
        'KATANA_SYNTHETIC_SECRET_MARKER',
        'DC_FLASH.BIN',
        '"source_bytes":"001122"',
        ('C:' + '\Users\example\private\disc.gdi')
    )) {
        if (-not (Get-Phase10DataViolation $marker)) {
            throw "Phase-10-Datenaudit erkennt Testmarker nicht."
        }
    }
    if (Get-Phase10DataViolation '{"marker":"KR_PHASE10_GUI_END_TO_END","source":"disc.gdi"}') {
        throw "Phase-10-Datenaudit lehnt redigierten GUI-Bericht ab."
    }
}

if (-not (Test-Path -LiteralPath $build -PathType Container)) {
    throw "Phase-10-Buildverzeichnis fehlt: $build"
}
$scanRoots = @(
    (Join-Path $build "reports"),
    (Join-Path $build "artifacts\phase10-gui-internal")
)
$failures = [Collections.Generic.List[string]]::new()
foreach ($scanRoot in $scanRoots) {
    if (-not (Test-Path -LiteralPath $scanRoot -PathType Container)) { continue }
    foreach ($file in Get-ChildItem -LiteralPath $scanRoot -Recurse -File) {
        if ($file.Length -gt 8MB -or $file.Extension -notin @('.json', '.txt', '.log', '.md', '.cmake')) {
            continue
        }
        $violation = Get-Phase10DataViolation (Get-Content -LiteralPath $file.FullName -Raw)
        if ($violation) {
            $relative = [IO.Path]::GetRelativePath($build, $file.FullName)
            $failures.Add("$relative`: $violation")
        }
    }
}
if ($failures.Count -ne 0) {
    throw "Phase-10-Datenaudit fehlgeschlagen:`n$($failures -join "`n")"
}

$assetManifest = Join-Path $root 'assets\gui\asset-manifest.json'
$asset = Get-Content -LiteralPath $assetManifest -Raw | ConvertFrom-Json
$logo = Join-Path $root 'assets\gui\KatanaLogo.png'
$expected = $asset.assets[0].sha256
$actual = (Get-FileHash -LiteralPath $logo -Algorithm SHA256).Hash.ToLowerInvariant()
if ($asset.schema -ne 'katana-gui-assets' -or $asset.version -ne 1 -or
    $asset.assets[0].distribution -ne 'internal-pending-kr-4902' -or $actual -ne $expected) {
    throw 'GUI-Assetprovenienz oder Logohash ist ungueltig.'
}

Write-Output "KR_PHASE10_DATA_AUDIT_SUCCESS self_test=$($SelfTest.IsPresent)"
