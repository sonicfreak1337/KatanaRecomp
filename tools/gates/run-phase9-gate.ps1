[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"

function Initialize-MsvcEnvironment {
    $ready = -not [string]::IsNullOrWhiteSpace($env:INCLUDE) -and
        -not [string]::IsNullOrWhiteSpace($env:LIB) -and
        $env:VSCMD_ARG_TGT_ARCH -eq 'x64' -and
        $env:VSCMD_ARG_HOST_ARCH -eq 'x64'
    if ($ready) { return }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) { return }
    $installation = & $vswhere -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath | Select-Object -First 1
    if ([string]::IsNullOrWhiteSpace($installation)) { return }
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
if (-not $build.StartsWith(
        $root + [IO.Path]::DirectorySeparatorChar,
        [StringComparison]::OrdinalIgnoreCase
    )) {
    throw 'Phase-9-Gate-Buildziel liegt ausserhalb des Repositorys.'
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
    throw 'KR-4006 verlangt einen sauberen vorbereiteten Commit.'
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
    if ($LASTEXITCODE -ne 0) { throw "Phase-9-Konfiguration fehlgeschlagen: $LASTEXITCODE" }

    $buildWatch = [Diagnostics.Stopwatch]::StartNew()
    & cmake --build --preset artifact-debug --parallel
    $buildWatch.Stop()
    if ($LASTEXITCODE -ne 0) { throw "Phase-9-Build fehlgeschlagen: $LASTEXITCODE" }

    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools\quality\check-format.ps1
    if ($LASTEXITCODE -ne 0) { throw 'Formatprofil fehlgeschlagen.' }
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
        tools\quality\audit-quality-contract.ps1 -SelfTest
    if ($LASTEXITCODE -ne 0) { throw 'Qualitaetsvertragaudit fehlgeschlagen.' }
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
        tools\quality\audit-reference-provenance.ps1 -SelfTest
    if ($LASTEXITCODE -ne 0) { throw 'Referenz-/Lizenzaudit fehlgeschlagen.' }

    $reportDirectory = Join-Path $build 'reports'
    New-Item -ItemType Directory -Path $reportDirectory -Force | Out-Null
    $reporter = Join-Path $build 'katana-phase9-homebrew-report.exe'
    $homebrewFirst = (& $reporter | Out-String).Trim()
    if ($LASTEXITCODE -ne 0) { throw 'Erster Homebrew-Hostframe fehlgeschlagen.' }
    $homebrewSecond = (& $reporter | Out-String).Trim()
    if ($LASTEXITCODE -ne 0 -or $homebrewFirst -ne $homebrewSecond) {
        throw 'Homebrew-Hostframe ist nicht bytegleich reproduzierbar.'
    }
    $homebrew = $homebrewFirst | ConvertFrom-Json
    $homebrewFirst | Set-Content `
        (Join-Path $reportDirectory 'phase9-homebrew-host-frame.json') -Encoding utf8
    $corpusText = (& $reporter --corpus | Out-String).Trim()
    if ($LASTEXITCODE -ne 0) { throw 'Homebrew-Korpusbericht fehlgeschlagen.' }
    $corpus = $corpusText | ConvertFrom-Json
    if ($corpus.schema -ne 'katana-homebrew-corpus' -or $corpus.artifacts.Count -ne 8) {
        throw 'Homebrew-Korpusbericht ist unvollstaendig.'
    }
    $corpusText | Set-Content `
        (Join-Path $reportDirectory 'phase9-homebrew-corpus.json') -Encoding utf8

    $benchmarkText = (& (Join-Path $build 'katana-phase9-benchmark.exe') | Out-String).Trim()
    if ($LASTEXITCODE -ne 0) { throw 'Phase-9-Benchmark fehlgeschlagen.' }
    $benchmark = $benchmarkText | ConvertFrom-Json
    $benchmarkText | Set-Content `
        (Join-Path $reportDirectory 'phase9-benchmark.json') -Encoding utf8

    $budgets = Get-Content tools\phase9\budgets.json -Raw | ConvertFrom-Json
    if ($budgets.schema -ne 'katana-phase9-budgets' -or $budgets.version -ne 1) {
        throw 'Phase-9-Budgets besitzen keinen stabilen Vertrag.'
    }
    $matrix = Get-Content tools\phase9\capability-matrix.json -Raw | ConvertFrom-Json
    if ($matrix.schema -ne 'katana-phase9-capability-matrix' -or $matrix.version -ne 1 -or
        $matrix.capabilities.Count -lt 7 -or
        @($matrix.capabilities | Where-Object {
            [string]::IsNullOrWhiteSpace($_.id) -or
            [string]::IsNullOrWhiteSpace($_.status) -or
            [string]::IsNullOrWhiteSpace($_.test) -or
            [string]::IsNullOrWhiteSpace($_.limitation)
        }).Count -ne 0) {
        throw 'Phase-9-Faehigkeitsmatrix ist unvollstaendig.'
    }
    $hostRoot = Join-Path ([IO.Path]::GetTempPath()) "katana-phase9-host-$PID"
    if (Test-Path -LiteralPath $hostRoot) {
        Remove-Item -LiteralPath $hostRoot -Recurse -Force
    }
    New-Item -ItemType Directory -Path $hostRoot -Force | Out-Null
    try {
        $discRoot = Join-Path $hostRoot 'disc'
        & (Join-Path $build 'katana-port-export-tests.exe') --write-fixture $discRoot
        if ($LASTEXITCODE -ne 0) { throw 'Synthetische Hostbuild-GDI konnte nicht erzeugt werden.' }
        $portRoot = Join-Path $hostRoot 'port'
        $hostBuildWatch = [Diagnostics.Stopwatch]::StartNew()
        & (Join-Path $build 'katana-recomp.exe') port (Join-Path $discRoot 'disc.gdi') `
            --output $portRoot --target-name phase9_host | Out-Null
        $hostBuildWatch.Stop()
        if ($LASTEXITCODE -ne 0) { throw 'Exportierter synthetischer Hostport wurde nicht gebaut.' }
        $hostbuildMs = [Math]::Ceiling($hostBuildWatch.Elapsed.TotalMilliseconds)
        $hostExecutable = Join-Path $portRoot 'phase9_host.exe'
        $startupWatch = [Diagnostics.Stopwatch]::StartNew()
        $startupOutput = (& $hostExecutable | Out-String).Trim()
        $startupWatch.Stop()
        if ($LASTEXITCODE -ne 0 -or $startupOutput -notmatch 'KR_GUEST_PROGRAM_ENTERED') {
            throw 'Exportierter Hostport erreichte den ersten Gastcheckpoint nicht.'
        }
        $startupToGuestCheckpointUs = [Math]::Ceiling($startupWatch.Elapsed.TotalMilliseconds * 1000)
    }
    finally {
        if (Test-Path -LiteralPath $hostRoot) {
            Remove-Item -LiteralPath $hostRoot -Recurse -Force
        }
    }
    $fallbackCount = $homebrew.fallback_count
    if ($benchmark.analysis_us -gt $budgets.analysis_us_max -or
        $benchmark.codegen_us -gt $budgets.codegen_us_max -or
        $hostbuildMs -gt $budgets.port_export_hostbuild_ms_max -or
        $startupToGuestCheckpointUs -gt $budgets.startup_to_guest_checkpoint_us_max -or
        $benchmark.runtime_us -gt $budgets.runtime_us_max -or
        $benchmark.generated_cpp_bytes -gt $budgets.generated_cpp_bytes_max -or
        $homebrew.invalidations -gt $budgets.invalidations_max -or
        $fallbackCount -gt $budgets.fallbacks_max -or
        $homebrew.scheduler_jitter -gt $budgets.scheduler_jitter_max -or
        $homebrew.silent_failures -ne 0) {
        throw 'Phase-9-Performance- oder Korrektheitsbudget wurde ueberschritten.'
    }

    $env:ASAN_OPTIONS = 'halt_on_error=1'
    $env:UBSAN_OPTIONS = 'halt_on_error=1:print_stacktrace=1'
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
        tools\quality\write-coverage-report.ps1
    if ($LASTEXITCODE -ne 0) { throw 'Phase-9-Regression/Coverage fehlgeschlagen.' }

    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
        tools\quality\write-reproducible-artifact.ps1
    if ($LASTEXITCODE -ne 0) { throw 'Reproduzierbares Phase-8-Basisartefakt fehlgeschlagen.' }
    $tests = & ctest --test-dir $build --show-only=json-v1 | ConvertFrom-Json
    if ($LASTEXITCODE -ne 0 -or $tests.tests.Count -eq 0) {
        throw 'Phase-9-Testanzahl ist nicht bestimmbar.'
    }
    $coverage = Join-Path $build 'coverage\coverage.cobertura.xml'
    $artifact = @(Get-ChildItem (Join-Path $build 'artifacts') -Filter '*.zip')
    if ($artifact.Count -ne 1) { throw 'Phase-9-Gate erwartet genau ein Basisartefakt.' }
    $commit = (& $git -C $root rev-parse HEAD).Trim()
    $gate = [ordered]@{
        schema = 'katana-phase9-gate'
        report_version = 1
        status = 'success'
        marker = 'KR_PHASE9_HOMEBREW_HOST_FRAME'
        source_commit = $commit
        configuration = 'Debug'
        test_count = $tests.tests.Count
        configure_ms = [Math]::Ceiling($configureWatch.Elapsed.TotalMilliseconds)
        port_export_hostbuild_ms = $hostbuildMs
        analysis_us = $benchmark.analysis_us
        codegen_us = $benchmark.codegen_us
        startup_to_guest_checkpoint_us = $startupToGuestCheckpointUs
        runtime_us = $benchmark.runtime_us
        generated_cpp_bytes = $benchmark.generated_cpp_bytes
        pvr_frames = $homebrew.pvr_frames
        audio_buffers = $homebrew.audio_buffers
        maple_transactions = $homebrew.maple_transactions
        invalidations = $homebrew.invalidations
        fallback_count = $fallbackCount
        scheduler_jitter = $homebrew.scheduler_jitter
        silent_failures = $homebrew.silent_failures
        state_hash = $homebrew.state_hash
        coverage_sha256 = (Get-FileHash $coverage -Algorithm SHA256).Hash.ToLowerInvariant()
        artifact_sha256 = (Get-FileHash $artifact[0].FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        data_audit = 'success'
        firmware_smoke = 'not-requested-lle-unsupported'
        release = 'none'
        next_phase = 10
        platform_evidence = [ordered]@{
            windows_msvc = 'verified-by-this-report'
            linux = 'not-run-no-local-linux-runner'
        }
    }
    $gate | ConvertTo-Json -Depth 5 | Set-Content `
        (Join-Path $reportDirectory 'phase9-gate.json') -Encoding utf8
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
        tools\quality\audit-phase9-data.ps1 -SelfTest
    if ($LASTEXITCODE -ne 0) { throw 'Phase-9-Datenaudit fehlgeschlagen.' }
}
finally {
    Pop-Location
}

Write-Output 'KR_PHASE9_GATE_SUCCESS next_phase=10'
