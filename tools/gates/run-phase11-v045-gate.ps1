[CmdletBinding()]
param(
    [ValidateRange(1, 32)][int]$Parallelism = 8
)

$ErrorActionPreference = 'Stop'

function Initialize-MsvcEnvironment {
    $ready = -not [string]::IsNullOrWhiteSpace($env:INCLUDE) -and
        -not [string]::IsNullOrWhiteSpace($env:LIB) -and
        $env:VSCMD_ARG_TGT_ARCH -eq 'x64' -and
        $env:VSCMD_ARG_HOST_ARCH -eq 'x64'
    if ($ready) { return }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $installation = & $vswhere -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath | Select-Object -First 1
    if ([string]::IsNullOrWhiteSpace($installation)) {
        throw 'MSVC-x64-Installation fehlt.'
    }
    $devCmd = Join-Path $installation 'Common7\Tools\VsDevCmd.bat'
    $command = "call `"$devCmd`" -no_logo -arch=x64 -host_arch=x64 >nul && set"
    $environment = & $env:ComSpec /d /s /c $command
    if ($LASTEXITCODE -ne 0) { throw 'MSVC-x64-Umgebung konnte nicht geladen werden.' }
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

$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$build = [IO.Path]::GetFullPath((Join-Path $root 'build-current'))
if (-not $build.StartsWith($root + [IO.Path]::DirectorySeparatorChar,
                           [StringComparison]::OrdinalIgnoreCase)) {
    throw 'v0.45-Gate-Buildziel liegt ausserhalb des Repositorys.'
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
    throw 'KR-4504 verlangt einen sauberen vorbereiteten Commit.'
}

Initialize-MsvcEnvironment
if (Test-Path -LiteralPath $build) {
    Remove-Item -LiteralPath $build -Recurse -Force
}

Push-Location -LiteralPath $root
try {
    $configureWatch = [Diagnostics.Stopwatch]::StartNew()
    & cmake --preset artifact-debug --fresh
    $configureWatch.Stop()
    if ($LASTEXITCODE -ne 0) { throw 'v0.45-Konfiguration fehlgeschlagen.' }

    $buildWatch = [Diagnostics.Stopwatch]::StartNew()
    & cmake --build --preset artifact-debug --parallel $Parallelism
    $buildWatch.Stop()
    if ($LASTEXITCODE -ne 0) { throw 'v0.45-Build fehlgeschlagen.' }

    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools\quality\check-format.ps1
    if ($LASTEXITCODE -ne 0) { throw 'Formatprofil fehlgeschlagen.' }
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
        tools\quality\audit-quality-contract.ps1 -SelfTest
    if ($LASTEXITCODE -ne 0) { throw 'Qualitaetsvertragaudit fehlgeschlagen.' }
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
        tools\quality\audit-reference-provenance.ps1 -SelfTest
    if ($LASTEXITCODE -ne 0) { throw 'Referenz-/Lizenzaudit-Selbsttest fehlgeschlagen.' }
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
        tools\quality\audit-reference-provenance.ps1
    if ($LASTEXITCODE -ne 0) { throw 'Referenz-/Lizenzaudit fehlgeschlagen.' }

    $env:ASAN_OPTIONS = 'halt_on_error=1'
    $env:UBSAN_OPTIONS = 'halt_on_error=1:print_stacktrace=1'
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
        tools\quality\write-coverage-report.ps1 -Parallelism $Parallelism
    if ($LASTEXITCODE -ne 0) { throw 'Vollstaendige v0.45-Regression/Coverage fehlgeschlagen.' }

    $homebrewText = (& (Join-Path $build 'katana-phase9-homebrew-report.exe') | Out-String).Trim()
    if ($LASTEXITCODE -ne 0) { throw 'Homebrew-Regression fehlgeschlagen.' }
    $homebrew = $homebrewText | ConvertFrom-Json
    if ($homebrew.marker -ne 'KR_PHASE9_HOMEBREW_HOST_FRAME' -or
        $homebrew.silent_failures -ne 0) {
        throw 'Homebrew-Evidenz besitzt keinen stabilen Frame oder stille Fehler.'
    }

    $isaText = (& (Join-Path $build 'katana-recomp.exe') isa-report --json | Out-String).Trim()
    if ($LASTEXITCODE -ne 0) { throw 'Alpha-ISA-Bericht fehlgeschlagen.' }
    $isa = $isaText | ConvertFrom-Json
    $systemControl = @($isa.families | Where-Object id -eq 'system-control')
    if ($isa.schema -ne 'katana-alpha-isa' -or $isa.unknown_opcode_count -le 0 -or
        $systemControl.Count -ne 1 -or $systemControl[0].status -ne 'restricted' -or
        $systemControl[0].limitation -match '^Privilege violations') {
        throw 'Alpha-ISA-Vertrag ist nicht maschinenlesbar.'
    }

    $harness = @(& powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
        tools\phase11\run-private-retail-debug.ps1 -SelfTest)
    if ($LASTEXITCODE -ne 0 -or
        $harness[-1] -ne 'KR_PHASE11_PRIVATE_RETAIL_HARNESS_SUCCESS') {
        throw 'Privater Retail-Harness besitzt keinen verteilbaren Selbsttest.'
    }
    $qol = @(& (Join-Path $build 'katana-recomp-gui.exe') --qol-smoke)
    if ($LASTEXITCODE -ne 0 -or $qol[-1] -ne 'KR_PHASE11_NATIVE_QOL_READY') {
        throw 'Nativer Phase-11-QoL-Vertrag fehlgeschlagen.'
    }

    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
        tools\quality\audit-phase10-data.ps1 -SelfTest
    if ($LASTEXITCODE -ne 0) { throw 'Datenaudit-Selbsttest fehlgeschlagen.' }
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
        tools\quality\audit-phase10-data.ps1
    if ($LASTEXITCODE -ne 0) { throw 'Datenaudit fehlgeschlagen.' }

    $tests = & ctest --test-dir $build --show-only=json-v1 | ConvertFrom-Json
    $coverage = Join-Path $build 'coverage\coverage.cobertura.xml'
    if ($LASTEXITCODE -ne 0 -or $tests.tests.Count -eq 0 -or
        -not (Test-Path -LiteralPath $coverage -PathType Leaf)) {
        throw 'v0.45-Test- oder Coverageevidenz fehlt.'
    }
    $reports = Join-Path $build 'reports'
    New-Item -ItemType Directory -Path $reports -Force | Out-Null
    $commit = (& $git -C $root rev-parse HEAD).Trim()
    $gate = [ordered]@{
        schema = 'katana-v045-gate'
        version = 1
        status = 'success'
        marker = 'KR_V045_BOOT_ANALYSIS_READY'
        source_commit = $commit
        configuration = 'Debug-ASan-Coverage-StaticAnalysis'
        fresh_build_directory = 'build-current'
        test_count = $tests.tests.Count
        configure_ms = [Math]::Ceiling($configureWatch.Elapsed.TotalMilliseconds)
        build_ms = [Math]::Ceiling($buildWatch.Elapsed.TotalMilliseconds)
        homebrew_marker = $homebrew.marker
        silent_failures = $homebrew.silent_failures
        isa_contract = $isa.schema
        job_contract = 4
        private_retail_harness = 'synthetic-self-test-only'
        private_retail_data_in_gate = $false
        native_qol = 'covered-windows'
        coverage_sha256 = (Get-FileHash $coverage -Algorithm SHA256).Hash.ToLowerInvariant()
        data_audit = 'success'
        release = 'none'
        review_required = $true
        next_task_after_review = 'KR-4505'
    }
    $gate | ConvertTo-Json -Depth 5 | Set-Content `
        (Join-Path $reports 'v045-gate.json') -Encoding utf8
}
finally {
    Pop-Location
}

Write-Output 'KR_V045_BOOT_ANALYSIS_READY review_required=true next_task=KR-4505'
