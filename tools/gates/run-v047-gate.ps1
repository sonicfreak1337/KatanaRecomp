[CmdletBinding()]
param([Parameter(Mandatory = $true)][string]$PrivateConfig)

$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$build = [IO.Path]::GetFullPath((Join-Path $root 'build-current'))
$package = Join-Path $build 'artifacts\v047-gui-current'
$reports = Join-Path $build 'reports'
$git = Get-Command git.exe -ErrorAction Stop | Select-Object -First 1 -ExpandProperty Source

function Require-NativeSuccess([string]$description) {
    if ($LASTEXITCODE -ne 0) { throw "$description fehlgeschlagen: $LASTEXITCODE" }
}

function Publish-RootGui {
    $guiSource = Join-Path $package 'katana-recomp-gui.exe'
    $dialogSource = Join-Path $package 'katana-file-dialog.exe'
    $sdkSource = Join-Path $package 'runtime-sdk'
    foreach ($required in @($guiSource, $dialogSource, (Join-Path $sdkSource 'CMakeLists.txt'))) {
        if (-not (Test-Path -LiteralPath $required)) {
            throw "Verifiziertes Root-GUI-Paket ist unvollstaendig: $required"
        }
    }

    $guiTarget = [IO.Path]::GetFullPath((Join-Path $root 'KatanaRecomp-GUI.exe'))
    $dialogTarget = [IO.Path]::GetFullPath((Join-Path $root 'katana-file-dialog.exe'))
    $sdkTarget = [IO.Path]::GetFullPath((Join-Path $root 'runtime-sdk'))
    foreach ($target in @($guiTarget, $dialogTarget, $sdkTarget)) {
        if (-not $target.StartsWith(
                $root + [IO.Path]::DirectorySeparatorChar,
                [StringComparison]::OrdinalIgnoreCase)) {
            throw 'Root-GUI-Ziel liegt ausserhalb des Repositorys.'
        }
    }

    $guiTemporary = Join-Path $root 'KatanaRecomp-GUI.new.exe'
    Copy-Item -LiteralPath $guiSource -Destination $guiTemporary -Force
    $smoke = @(& $guiTemporary --smoke)
    Require-NativeSuccess 'Root-GUI-Smoke vor Publish'
    if ($smoke -notcontains 'KR_PHASE10_GUI_MINIMAL_START') {
        throw 'Neue Root-GUI besitzt keinen stabilen Smoke-Marker.'
    }
    Move-Item -LiteralPath $guiTemporary -Destination $guiTarget -Force

    $dialogTemporary = "$dialogTarget.new"
    Copy-Item -LiteralPath $dialogSource -Destination $dialogTemporary -Force
    Move-Item -LiteralPath $dialogTemporary -Destination $dialogTarget -Force

    $sdkBackup = Join-Path $build '.root-runtime-sdk-previous'
    if (Test-Path -LiteralPath $sdkBackup) {
        Remove-Item -LiteralPath $sdkBackup -Recurse -Force
    }
    if (Test-Path -LiteralPath $sdkTarget) {
        Move-Item -LiteralPath $sdkTarget -Destination $sdkBackup
    }
    try {
        Copy-Item -LiteralPath $sdkSource -Destination $sdkTarget -Recurse
        if (-not (Test-Path -LiteralPath (Join-Path $sdkTarget 'cmake\KatanaVersions.cmake'))) {
            throw 'Neue Root-Runtime-SDK ist nach Publish unvollstaendig.'
        }
    } catch {
        if (Test-Path -LiteralPath $sdkTarget) {
            Remove-Item -LiteralPath $sdkTarget -Recurse -Force
        }
        if (Test-Path -LiteralPath $sdkBackup) {
            Move-Item -LiteralPath $sdkBackup -Destination $sdkTarget
        }
        throw
    }
    if (Test-Path -LiteralPath $sdkBackup) {
        Remove-Item -LiteralPath $sdkBackup -Recurse -Force
    }
    return (Get-FileHash -LiteralPath $guiTarget -Algorithm SHA256).Hash.ToLowerInvariant()
}

& powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
    tools\gates\run-core-correctness-gate.ps1 -BuildGuiPackage -AllowDirtyWorktree
Require-NativeSuccess 'Core-/GUI-Gate'

$selfTest = @(& powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
    tools\phase11\run-private-retail-debug.ps1 -SelfTest)
Require-NativeSuccess 'Build-only-Harness-Selbsttest'
if ($selfTest[-1] -ne 'KR_PHASE11_PRIVATE_RETAIL_HARNESS_SUCCESS') {
    throw 'Build-only-Harness-Selbsttest besitzt keinen stabilen Marker.'
}

$privateOutput = @(& powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
    tools\phase11\run-private-retail-debug.ps1 -Config $PrivateConfig)
Require-NativeSuccess 'Privater Build-only-Doppelnachweis'
$privateReport = $privateOutput[-1] | ConvertFrom-Json
if ($privateReport.schema -ne 'katana-private-retail-build' -or
    $privateReport.status -ne 'success' -or
    $privateReport.execution_mode -ne 'build-only' -or
    $privateReport.reproducibility.builds_completed -ne 2 -or
    -not $privateReport.analysis.coverage_complete -or
    $privateReport.analysis.guarded_partial_control_flow -ne 0 -or
    $privateReport.analysis.unresolved_control_flow -ne 0 -or
    $privateReport.build.game_executable_started -or
    $privateReport.build.runtime_processes_started -ne 0) {
    throw 'Privater v0.47-Build-only-Bericht verletzt Abdeckung oder No-run-Vertrag.'
}

& powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
    tools\quality\audit-phase10-data.ps1 -SelfTest
Require-NativeSuccess 'Datenaudit-Selbsttest'
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools\quality\audit-phase10-data.ps1
Require-NativeSuccess 'Datenaudit'

$core = Get-Content (Join-Path $reports 'core-correctness-gate.json') -Raw | ConvertFrom-Json
$guiSha256 = Publish-RootGui
$gate = [ordered]@{
    schema = 'katana-v047-gate'
    report_version = 1
    status = 'success'
    marker = 'KR_V047_NATIVE_HOST_READY'
    source_commit = (& $git -C $root rev-parse HEAD).Trim()
    source_worktree = $core.source_worktree
    debug_test_count = $core.debug_test_count
    relwithdebinfo_test_count = $core.relwithdebinfo_test_count
    shared_core_test_count = $core.shared_core_test_count
    identical_core_test_inventory = [bool]$core.identical_core_test_inventory
    exact_guest_results = $core.exact_reference_vectors
    unresolved = 0
    guarded_partial = 0
    unknown_instructions = 0
    unanalyzed_executable_bytes = 0
    reachable_abort_edges = 0
    runtime_only = [uint64]$privateReport.analysis.runtime_only_control_flow
    free_application = 'project-authored-relocatable-gdi-port-fixture'
    private_retail = [ordered]@{
        mode = 'build-only'
        builds_completed = 2
        game_executable_started = $false
        runtime_processes_started = 0
        report_redacted = $true
    }
    gui = [ordered]@{
        configuration = 'RelWithDebInfo'
        package = 'verified-relocatable'
        root_executable = 'KatanaRecomp-GUI.exe'
        sha256 = $guiSha256
        runtime_sdk_refreshed = $true
    }
    audits = [ordered]@{
        format = $core.format_check
        quality_contract = $core.quality_contract_audit
        reference_license = $core.reference_license_audit
        private_data = 'success'
    }
    release = 'none'
    review_required = $true
    next_task_after_review = 'KR-4705'
}
New-Item -ItemType Directory -Path $reports -Force | Out-Null
$gate | ConvertTo-Json -Depth 5 | Set-Content `
    (Join-Path $reports 'v047-gate.json') -Encoding utf8
Write-Output 'KR_V047_NATIVE_HOST_READY review_required=true next_task=KR-4705'
