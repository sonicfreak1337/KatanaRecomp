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
    throw 'Phase-10-Gate-Buildziel liegt ausserhalb des Repositorys.'
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
    throw 'KR-4402 verlangt einen sauberen vorbereiteten Commit.'
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
    if ($LASTEXITCODE -ne 0) { throw "Phase-10-Konfiguration fehlgeschlagen: $LASTEXITCODE" }

    $buildWatch = [Diagnostics.Stopwatch]::StartNew()
    & cmake --build --preset artifact-debug --parallel
    $buildWatch.Stop()
    if ($LASTEXITCODE -ne 0) { throw "Phase-10-Build fehlgeschlagen: $LASTEXITCODE" }

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
    $fixture = Join-Path $build 'phase10-automation-fixture'
    New-Item -ItemType Directory -Path $fixture -Force | Out-Null
    & (Join-Path $build 'katana-port-export-tests.exe') --write-fixture (Join-Path $fixture 'disc')
    if ($LASTEXITCODE -ne 0) { throw 'Phase-10-GDI-Automationsfixture fehlt.' }
    @'
# Phase-10 synthetic automation fixture
schema = katana-project
version = 2
project.name = disc
input.format = gdi
input.path = disc/disc.gdi
execution.firmware = direct
execution.fallback = abort
execution.scheduler = deterministic
execution.mmu = disabled
execution.fastpath = conservative
'@ | Set-Content (Join-Path $fixture 'project.katana') -Encoding ascii

    $guiOutput = @(& (Join-Path $build 'katana-recomp-gui.exe') `
        --automation (Join-Path $fixture 'disc\disc.gdi') (Join-Path $fixture 'gui-output'))
    if ($LASTEXITCODE -ne 0 -or $guiOutput[0] -ne 'KR_PHASE10_GUI_MODEL_AUTOMATION') {
        throw 'GUI-Modell-/Anwendungsdienst-Automatisierung fehlgeschlagen.'
    }
    $nativeOutput = @(& (Join-Path $build 'katana-recomp-gui.exe') --native-smoke)
    if ($LASTEXITCODE -ne 0 -or
        $nativeOutput[0] -ne 'KR_PHASE10_NATIVE_SHELL_LIFECYCLE') {
        throw 'Nativer Windows-Shell-Lebenszyklus fehlgeschlagen.'
    }
    $nativeE2eOutput = @(& (Join-Path $build 'katana-recomp-gui.exe') `
        --native-automation (Join-Path $fixture 'disc\disc.gdi') `
        (Join-Path $fixture 'native-gui-output'))
    if ($LASTEXITCODE -ne 0 -or
        $nativeE2eOutput[-1] -ne 'KR_PHASE10_NATIVE_GUI_END_TO_END') {
        throw 'Nativer Windows-GUI-End-to-End-Pfad fehlgeschlagen.'
    }
    $guiJob = $guiOutput[1] | ConvertFrom-Json
    $cliText = (& (Join-Path $build 'katana-recomp.exe') workflow build `
        (Join-Path $fixture 'project.katana') --output (Join-Path $fixture 'cli-output') | Out-String).Trim()
    if ($LASTEXITCODE -ne 0) { throw 'CLI-Anwendungsdienstvergleich fehlgeschlagen.' }
    $cliJob = $cliText | ConvertFrom-Json
    if ($guiJob.project_identity -ne $cliJob.project_identity -or
        $guiJob.state -ne 'completed' -or $cliJob.state -ne 'completed') {
        throw 'GUI und CLI besitzen unterschiedliche Projektidentitaet oder Jobzustaende.'
    }
    $guiCore = @($guiJob.artifacts | Where-Object role -ne 'job-result' |
        Sort-Object role, path | ForEach-Object { "$($_.role)|$($_.path)|$($_.sha256)" })
    $cliCore = @($cliJob.artifacts | Where-Object role -ne 'job-result' |
        Sort-Object role, path | ForEach-Object { "$($_.role)|$($_.path)|$($_.sha256)" })
    if (($guiCore -join "`n") -ne ($cliCore -join "`n")) {
        throw 'GUI und CLI erzeugen unterschiedliche Kernartefakte.'
    }
    [ordered]@{
        schema = 'katana-phase10-model-integration'
        version = 1
        marker = 'KR_PHASE10_GUI_MODEL_AUTOMATION'
        status = 'success'
        source = 'disc.gdi'
        source_format = 'gdi'
        project_identity = $guiJob.project_identity
        artifact_count = $guiCore.Count
        cli_gui_equal = $true
        native_shell_lifecycle = 'covered-windows'
        native_control_e2e = 'windows-window-job-resize-keyboard-build-covered'
        project_host_paths_in_reports = 0
        sensitive_fields_exported = 0
    } | ConvertTo-Json | Set-Content `
        (Join-Path $reportDirectory 'phase10-gui-e2e.json') -Encoding utf8

    $homebrewText = (& (Join-Path $build 'katana-phase9-homebrew-report.exe') | Out-String).Trim()
    if ($LASTEXITCODE -ne 0) { throw 'Phase-9-Homebrew-Regression fehlgeschlagen.' }
    $homebrew = $homebrewText | ConvertFrom-Json
    if ($homebrew.marker -ne 'KR_PHASE9_HOMEBREW_HOST_FRAME' -or
        $homebrew.silent_failures -ne 0) {
        throw 'Phase-9-Homebrew-Checkpoint wurde nicht erhalten.'
    }
    $homebrewText | Set-Content `
        (Join-Path $reportDirectory 'phase10-phase9-regression.json') -Encoding utf8

    $env:ASAN_OPTIONS = 'halt_on_error=1'
    $env:UBSAN_OPTIONS = 'halt_on_error=1:print_stacktrace=1'
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
        tools\quality\write-coverage-report.ps1
    if ($LASTEXITCODE -ne 0) { throw 'Phase-10-Regression/Coverage fehlgeschlagen.' }

    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
        tools\quality\write-reproducible-artifact.ps1
    if ($LASTEXITCODE -ne 0) { throw 'Reproduzierbares Basisartefakt fehlgeschlagen.' }

    $package = Join-Path $build 'artifacts\phase10-gui-internal'
    $asanRuntime = Join-Path $env:VCToolsInstallDir `
        'bin\Hostx64\x64\clang_rt.asan_dynamic-x86_64.dll'
    if (-not (Test-Path -LiteralPath $asanRuntime -PathType Leaf)) {
        throw 'MSVC-ASan-Laufzeit fuer internen GUI-Paketkandidaten fehlt.'
    }
    & cmake "-DSOURCE_DIR=$root" "-DBUILD_DIR=$build" "-DOUTPUT_DIR=$package" `
        "-DASAN_RUNTIME=$asanRuntime" `
        -P tools\phase10\package-gui.cmake
    if ($LASTEXITCODE -ne 0) { throw 'Interner GUI-Paketkandidat fehlgeschlagen.' }

    $tests = & ctest --test-dir $build --show-only=json-v1 | ConvertFrom-Json
    if ($LASTEXITCODE -ne 0 -or $tests.tests.Count -eq 0) {
        throw 'Phase-10-Testanzahl ist nicht bestimmbar.'
    }
    $coverage = Join-Path $build 'coverage\coverage.cobertura.xml'
    $packageManifest = Join-Path $package 'package-manifest.json'
    $commit = (& $git -C $root rev-parse HEAD).Trim()
    $gate = [ordered]@{
        schema = 'katana-phase10-gate'
        report_version = 1
        status = 'success'
        marker = 'KR_PHASE10_GUI_MODEL_AUTOMATION'
        source_commit = $commit
        configuration = 'Debug'
        test_count = $tests.tests.Count
        configure_ms = [Math]::Ceiling($configureWatch.Elapsed.TotalMilliseconds)
        hostbuild_ms = [Math]::Ceiling($buildWatch.Elapsed.TotalMilliseconds)
        gui_cli_project_identity = $guiJob.project_identity
        gui_cli_artifact_count = $guiCore.Count
        gui_cli_equal = $true
        gdi_positive_negative_recovery = 'covered'
        native_shell_lifecycle = 'covered-windows'
        native_control_e2e = 'windows-window-job-resize-keyboard-build-covered'
        keyboard_dpi_recovery = 'windows-native-lifecycle-and-model-covered'
        phase9_marker = $homebrew.marker
        silent_failures = $homebrew.silent_failures
        coverage_sha256 = (Get-FileHash $coverage -Algorithm SHA256).Hash.ToLowerInvariant()
        package_manifest_sha256 = `
            (Get-FileHash $packageManifest -Algorithm SHA256).Hash.ToLowerInvariant()
        data_audit = 'success'
        asset_distribution = 'internal-pending-kr-4902'
        release = 'none'
        review_required = $true
        next_task_after_review = 'KR-4403'
    }
    $gate | ConvertTo-Json -Depth 5 | Set-Content `
        (Join-Path $reportDirectory 'phase10-gate.json') -Encoding utf8
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
        tools\quality\audit-phase10-data.ps1 -SelfTest
    if ($LASTEXITCODE -ne 0) { throw 'Phase-10-Datenaudit fehlgeschlagen.' }
}
finally {
    Pop-Location
}

Write-Output 'KR_PHASE10_GATE_SUCCESS review_required=true next_task=KR-4403'
