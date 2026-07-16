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
    throw "Phase-9-Datenaudit akzeptiert nur das Buildziel im Repository."
}

function Get-Phase9DataViolation {
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
        return "unredigiertes synthetisches Geheimnis"
    }
    if ($ScanPaths -and $Content -match '(?i)(DC_BOOT\.BIN|DC_FLASH\.BIN|firmware[_-]?bytes|bios[_-]?bytes)') {
        return "Firmware- oder Bootdatenmarker"
    }
    if ($ScanPaths -and $Content -match '(?i)"(trace_bytes|memory_dump|firmware_payload)"\s*:') {
        return "unredigierter Trace- oder Speicherinhalt"
    }
    return $null
}

if ($SelfTest) {
    foreach ($marker in @(
        'KATANA_SYNTHETIC_SECRET_MARKER',
        'DC_BOOT.BIN',
        '"trace_bytes":"001122"',
        ('C:' + '\Users\example\private\capture.bin'),
        ('/' + 'tmp' + '/ci/capture.bin')
    )) {
        if (-not (Get-Phase9DataViolation $marker)) {
            throw "Phase-9-Datenaudit erkennt Testmarker nicht."
        }
    }
    if (Get-Phase9DataViolation '{"marker":"KR_PHASE9_HOMEBREW_HOST_FRAME","state_hash":1}') {
        throw "Phase-9-Datenaudit lehnt redigierten Zustandsbericht ab."
    }
}

if (-not (Test-Path -LiteralPath $build -PathType Container)) {
    throw "Phase-9-Buildverzeichnis fehlt: $build"
}

$scanRoots = @(
    (Join-Path $build "reports"),
    (Join-Path $build "phase9-hostbuild")
)
$failures = [Collections.Generic.List[string]]::new()
foreach ($scanRoot in $scanRoots) {
    if (-not (Test-Path -LiteralPath $scanRoot -PathType Container)) { continue }
    foreach ($file in Get-ChildItem -LiteralPath $scanRoot -Recurse -File) {
        # Scan every generated artifact, including binaries and files larger than 8 MiB.  The
        # markers are deliberately ASCII-compatible so a byte-preserving Latin-1 decode cannot
        # hide them behind an extension or an invalid UTF-8 sequence.
        $bytes = [IO.File]::ReadAllBytes($file.FullName)
        $content = [Text.Encoding]::GetEncoding(28591).GetString($bytes)
        $scanPaths = $file.Extension -in @('.json', '.txt', '.log', '.md', '.cmake', '.cpp', '.hpp', '.c', '.h', '.ninja', '.conf')
        $violation = Get-Phase9DataViolation $content $scanPaths
        if ($violation) {
            $relative = $file.FullName.Substring($build.Length).TrimStart('\', '/')
            $failures.Add("$relative`: $violation")
        }
    }
}
if ($failures.Count -ne 0) {
    throw "Phase-9-Datenaudit fehlgeschlagen:`n$($failures -join "`n")"
}

Write-Output "KR_PHASE9_DATA_AUDIT_SUCCESS self_test=$($SelfTest.IsPresent)"
