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
    param(
        [Parameter(Mandatory = $true)][string]$Content,
        [bool]$ScanPaths = $true
    )
    $component = '[A-Za-z0-9._-]+'
    $posixRoot = '(?:tmp|root|mnt|var|home|Users|workspace|work|builds|__w)'
    if ($ScanPaths -and $Content -match "(?i)([A-Z]:[\\/](?:$component[\\/])+$component|/$posixRoot/(?:$component/)*$component)") {
        return "absoluter lokaler Pfad"
    }
    if ($Content -match 'KATANA_SYNTHETIC_SECRET_MARKER' -or
        ($ScanPaths -and $Content -match '(?i)(factory[_-]?data|network[_-]?id|serial[_-]?number)')) {
        return "unredigiertes Geheimnis oder Identifikator"
    }
    if ($ScanPaths -and $Content -match '(?i)(DC_BOOT\.BIN|DC_FLASH\.BIN|firmware[_-]?bytes|bios[_-]?bytes|flash[_-]?bytes)') {
        return "Firmware-, Boot- oder Flashdatenmarker"
    }
    if ($ScanPaths -and $Content -match '(?i)"(trace_bytes|memory_dump|firmware_payload|source_bytes)"\s*:') {
        return "unredigierter Quell-, Trace- oder Speicherinhalt"
    }
    return $null
}

if ($SelfTest) {
    foreach ($marker in @(
        'KATANA_SYNTHETIC_SECRET_MARKER',
        'DC_FLASH.BIN',
        '"source_bytes":"001122"',
        ('C:' + '\Users\example\private\disc.gdi'),
        ('/' + 'tmp' + '/ci/disc.gdi')
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
        $bytes = [IO.File]::ReadAllBytes($file.FullName)
        $content = [Text.Encoding]::GetEncoding(28591).GetString($bytes)
        $scanPaths = $file.Extension -in @('.json', '.txt', '.log', '.md', '.cmake', '.cpp', '.hpp', '.c', '.h', '.ninja', '.conf')
        $violation = Get-Phase10DataViolation $content $scanPaths
        if ($violation) {
            $relative = $file.FullName.Substring($build.Length).TrimStart('\', '/')
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
