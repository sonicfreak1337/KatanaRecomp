[CmdletBinding()]
param([switch]$SelfTest)

$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))

function Assert-QualityContract {
    param(
        [Parameter(Mandatory = $true)]$Profiles,
        [Parameter(Mandatory = $true)]$Presets
    )
    if ($Profiles.schema -ne 'katana-quality-profiles' -or $Profiles.version -ne 1) {
        throw 'Qualitaetsprofil besitzt keinen stabilen Schemavertrag.'
    }
    $required = @(
        'local-debug-gate', 'sanitizers', 'fuzzing', 'coverage',
        'format-static-analysis', 'artifact-audit', 'reference-license-audit',
        'phase8-gate-preparation'
    )
    foreach ($id in $required) {
        $profile = @($Profiles.profiles | Where-Object id -eq $id)
        if ($profile.Count -ne 1 -or -not $profile[0].active) {
            throw "Aktives Qualitaetsprofil fehlt oder ist doppelt: $id"
        }
    }
    foreach ($id in @('release-build', 'windows-linux-ci')) {
        $profile = @($Profiles.profiles | Where-Object id -eq $id)
        if ($profile.Count -ne 1 -or $profile[0].active) {
            throw "Pre-Alpha-Vertrag verlangt deaktiviertes Profil: $id"
        }
    }
    $artifact = @($Presets.configurePresets | Where-Object name -eq 'artifact-debug')
    if ($artifact.Count -ne 1) {
        throw 'Kumulatives artifact-debug-Preset fehlt oder ist doppelt.'
    }
    $debug = @($Presets.configurePresets | Where-Object name -eq 'debug-gate')
    if ($debug.Count -ne 1 -or $debug[0].binaryDir -ne '${sourceDir}/build-current') {
        throw 'Debug-Preset verwendet nicht ausschliesslich build-current/.'
    }
    $inheritance = @{
        'sanitizer-debug' = 'debug-gate'
        'fuzz-debug' = 'sanitizer-debug'
        'coverage-debug' = 'fuzz-debug'
        'quality-debug' = 'coverage-debug'
        'artifact-debug' = 'quality-debug'
    }
    foreach ($name in $inheritance.Keys) {
        $preset = @($Presets.configurePresets | Where-Object name -eq $name)
        if ($preset.Count -ne 1 -or $preset[0].inherits -ne $inheritance[$name]) {
            throw "Presetvererbung ist nicht kumulativ: $name"
        }
    }
}

$profiles = Get-Content (Join-Path $root 'tools\quality\profiles.json') -Raw |
    ConvertFrom-Json
$presets = Get-Content (Join-Path $root 'CMakePresets.json') -Raw |
    ConvertFrom-Json
Assert-QualityContract -Profiles $profiles -Presets $presets

if ($SelfTest) {
    $broken = $profiles | ConvertTo-Json -Depth 20 | ConvertFrom-Json
    $broken.profiles = @($broken.profiles | Where-Object id -ne 'coverage')
    $rejected = $false
    try {
        Assert-QualityContract -Profiles $broken -Presets $presets
    } catch {
        $rejected = $true
    }
    if (-not $rejected) {
        throw 'Qualitaetsaudit-Selbsttest erkennt ein fehlendes Coverageprofil nicht.'
    }
}

Write-Output "KR_QUALITY_CONTRACT_SUCCESS self_test=$($SelfTest.IsPresent)"
