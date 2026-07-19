[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$build = [IO.Path]::GetFullPath((Join-Path $root 'build-current'))
if (-not $build.StartsWith(
        $root + [IO.Path]::DirectorySeparatorChar,
        [StringComparison]::OrdinalIgnoreCase
    )) {
    throw 'Phase-8-Gate-Buildziel liegt ausserhalb des Repositorys.'
}
$unexpected = @(Get-ChildItem -LiteralPath $root -Directory -Force | Where-Object {
    $_.Name -ne 'build-current' -and ($_.Name -eq 'build' -or $_.Name -like 'build-*')
})
if ($unexpected.Count -ne 0) {
    throw "Unerwartete Buildverzeichnisse: $(($unexpected.Name | Sort-Object) -join ', ')"
}

$git = Get-Command git.exe -ErrorAction Stop | Select-Object -First 1 -ExpandProperty Source
$dirty = @(& $git -C $root status --porcelain=v1 --untracked-files=all)
if ($LASTEXITCODE -ne 0 -or $dirty.Count -ne 0) {
    throw 'KR-3709 verlangt vor dem frischen Build einen sauberen vorbereiteten Commit.'
}

if (Test-Path -LiteralPath $build) {
    Remove-Item -LiteralPath $build -Recurse -Force
}

Push-Location $root
try {
    & cmake --preset artifact-debug --fresh
    if ($LASTEXITCODE -ne 0) { throw "Phase-8-Konfiguration fehlgeschlagen: $LASTEXITCODE" }
    & cmake --build --preset artifact-debug --parallel
    if ($LASTEXITCODE -ne 0) { throw "Phase-8-Debugbuild fehlgeschlagen: $LASTEXITCODE" }

    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
        tools\quality\check-format.ps1
    if ($LASTEXITCODE -ne 0) { throw 'Formatprofil fehlgeschlagen.' }
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
        tools\quality\audit-quality-contract.ps1 -SelfTest
    if ($LASTEXITCODE -ne 0) { throw 'Qualitaetsvertragaudit fehlgeschlagen.' }
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
        tools\quality\audit-reference-provenance.ps1 -SelfTest
    if ($LASTEXITCODE -ne 0) { throw 'Referenz-/Lizenzaudit fehlgeschlagen.' }

    $env:ASAN_OPTIONS = 'halt_on_error=1'
    $env:UBSAN_OPTIONS = 'halt_on_error=1:print_stacktrace=1'
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
        tools\quality\write-coverage-report.ps1
    if ($LASTEXITCODE -ne 0) { throw 'Kumulative Regression/Coverage fehlgeschlagen.' }

    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
        tools\quality\write-reproducible-artifact.ps1
    if ($LASTEXITCODE -ne 0) { throw 'Reproduzierbares Artefaktprofil fehlgeschlagen.' }

    $tests = & ctest --test-dir $build --show-only=json-v1 | ConvertFrom-Json
    if ($LASTEXITCODE -ne 0 -or $tests.tests.Count -eq 0) {
        throw 'Testanzahl fuer den Gatebericht ist nicht bestimmbar.'
    }
    $artifact = @(Get-ChildItem (Join-Path $build 'artifacts') -Filter '*.zip')
    if ($artifact.Count -ne 1) { throw 'Gatebericht erwartet genau ein reproduzierbares ZIP.' }
    $coverage = Join-Path $build 'coverage\coverage.cobertura.xml'
    $reportDirectory = Join-Path $build 'reports'
    New-Item -ItemType Directory -Path $reportDirectory -Force | Out-Null
    $version = (Get-Content VERSION -Raw).Trim()
    $report = [ordered]@{
        schema = 'katana-phase8-gate-preparation'
        report_version = 1
        status = 'success'
        source_commit = (& $git -C $root rev-parse HEAD).Trim()
        project_version = "$version-dev"
        configuration = 'Debug'
        fresh_build_directory = 'build-current'
        test_count = $tests.tests.Count
        sanitizer_profile = 'active'
        fuzz_seed = '0x3703'
        coverage_sha256 = (Get-FileHash $coverage -Algorithm SHA256).Hash.ToLowerInvariant()
        artifact_sha256 = (Get-FileHash $artifact[0].FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        reference_license_audit = 'success'
        release_build = 'disabled-until-alpha'
        ci = 'disabled-until-alpha'
        private_retail_testbench = 'not-run-before-runtime-bring-up'
    }
    $report | ConvertTo-Json -Depth 4 | Set-Content `
        (Join-Path $reportDirectory 'phase8-gate-preparation.json') -Encoding utf8
}
finally {
    Pop-Location
}

Write-Output 'KR_PHASE8_GATE_PREPARATION_SUCCESS'
