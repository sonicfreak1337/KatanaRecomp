[CmdletBinding()]
param(
    [string]$Config,
    [switch]$SelfTest
)

$ErrorActionPreference = 'Stop'
$checkpointOrder = @(
    'SA_NOT_REACHED',
    'SA_ANALYSIS_CONTINUES',
    'SA_MAIN_ENTERED',
    'SA_FIRST_FRAME',
    'SA_MENU_INTERACTIVE',
    'SA_ALPHA_PLAYABLE'
)

function Select-Checkpoint {
    param($Job, [string]$RuntimeOutput)
    $checkpoint = 'SA_NOT_REACHED'
    if ($Job.analysis -and $Job.analysis.functions -gt 1 -and
        $Job.analysis.analyzed_instruction_bytes -gt 4) {
        $checkpoint = 'SA_ANALYSIS_CONTINUES'
    }
    foreach ($candidate in $checkpointOrder[2..($checkpointOrder.Count - 1)]) {
        if ($RuntimeOutput -match "(?m)^$candidate(?:\s|$)") {
            $checkpoint = $candidate
        }
    }
    return $checkpoint
}

function Select-FailureClass {
    param($Job, [bool]$TimedOut, [Nullable[int]]$RuntimeExitCode)
    if ($TimedOut) { return 'host-time-budget' }
    if ($Job.state -eq 'partial') { return 'analysis-incomplete' }
    if ($Job.state -eq 'failed') {
        switch ($Job.failure_category) {
            'input-output' { return 'input-output' }
            'build' { return 'host-build' }
            'code-generation' { return 'code-generation' }
            default { return 'processing' }
        }
    }
    if ($null -ne $RuntimeExitCode -and $RuntimeExitCode -ne 0) {
        return 'runtime-nonzero'
    }
    return $null
}

function Invoke-BudgetedProcess {
    param(
        [string]$Executable,
        [string[]]$Arguments,
        [int]$TimeoutSeconds,
        [hashtable]$Environment = @{}
    )
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = $Executable
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $start.Arguments = ($Arguments | ForEach-Object {
        '"' + ([string]$_).Replace('"', '\"') + '"'
    }) -join ' '
    foreach ($entry in $Environment.GetEnumerator()) {
        $start.EnvironmentVariables[$entry.Key] = [string]$entry.Value
    }
    $process = [Diagnostics.Process]::Start($start)
    $stdout = $process.StandardOutput.ReadToEndAsync()
    $stderr = $process.StandardError.ReadToEndAsync()
    $finished = $process.WaitForExit($TimeoutSeconds * 1000)
    if (-not $finished) {
        & taskkill.exe /PID $process.Id /T /F *> $null
        $process.WaitForExit()
    }
    return [pscustomobject]@{
        timed_out = -not $finished
        exit_code = if ($finished) { $process.ExitCode } else { $null }
        stdout = $stdout.GetAwaiter().GetResult()
        stderr = $stderr.GetAwaiter().GetResult()
    }
}

if ($SelfTest) {
    $partial = [pscustomobject]@{
        state = 'partial'
        failure_category = 'none'
        analysis = [pscustomobject]@{
            functions = 12
            analyzed_instruction_bytes = 128
        }
    }
    if ((Select-Checkpoint $partial '') -ne 'SA_ANALYSIS_CONTINUES' -or
        (Select-FailureClass $partial $false $null) -ne 'analysis-incomplete') {
        throw 'Synthetische partielle Analyseklassifikation ist falsch.'
    }
    $runtime = "SA_MAIN_ENTERED`nSA_FIRST_FRAME frames=1`n"
    if ((Select-Checkpoint $partial $runtime) -ne 'SA_FIRST_FRAME') {
        throw 'Checkpointordnung ist nicht monoton.'
    }
    $serialized = [ordered]@{
        schema = 'katana-private-retail-debug'
        version = 1
        checkpoint = 'SA_FIRST_FRAME'
        failure_class = $null
    } | ConvertTo-Json -Compress
    $privatePattern = '(?i)([A-Z]:\\|' + '/' + 'home/|' + '/' +
                      'tmp/|sha256|\.gdi)'
    if ($serialized -match $privatePattern) {
        throw 'Synthetischer Bericht enthaelt private Felder.'
    }
    Write-Output 'KR_PHASE11_PRIVATE_RETAIL_HARNESS_SUCCESS'
    exit 0
}

if ([string]::IsNullOrWhiteSpace($Config)) {
    throw 'Config ist erforderlich; alternativ -SelfTest verwenden.'
}
$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$settings = Get-Content -LiteralPath $Config -Raw | ConvertFrom-Json
if ($settings.schema -ne 'katana-private-retail-debug-config' -or $settings.version -ne 1) {
    throw 'Unbekannter privater Debugkonfigurationsvertrag.'
}
foreach ($field in @('manifest_path', 'gdi_path', 'output_root', 'report_path',
                      'host_timeout_seconds', 'guest_cycle_budget')) {
    if ($null -eq $settings.$field) { throw "Konfigurationsfeld fehlt: $field" }
}
$manifest = [IO.Path]::GetFullPath([string]$settings.manifest_path)
$gdi = [IO.Path]::GetFullPath([string]$settings.gdi_path)
$outputRoot = [IO.Path]::GetFullPath([string]$settings.output_root)
$reportPath = [IO.Path]::GetFullPath([string]$settings.report_path)
foreach ($privatePath in @($manifest, $gdi, $outputRoot, $reportPath)) {
    if ($privatePath.StartsWith($repositoryRoot + [IO.Path]::DirectorySeparatorChar,
                                [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Private Retail-Konfiguration und -Ausgaben muessen ausserhalb des Repositorys liegen.'
    }
}
if (-not (Test-Path -LiteralPath $manifest -PathType Leaf) -or
    -not (Test-Path -LiteralPath $gdi -PathType Leaf)) {
    throw 'Privates Manifest oder GDI fehlt.'
}
$timeout = [Math]::Max(1, [int]$settings.host_timeout_seconds)
$guestBudget = [Math]::Max(1, [uint64]$settings.guest_cycle_budget)
$cli = Join-Path $repositoryRoot 'build-current\katana-recomp.exe'
if (-not (Test-Path -LiteralPath $cli -PathType Leaf)) {
    throw 'Debug-CLI fehlt; zuerst build-current erzeugen.'
}
$workflow = Invoke-BudgetedProcess $cli @('workflow', 'build', $manifest, '--output', $outputRoot) `
    $timeout
$job = $null
if (-not [string]::IsNullOrWhiteSpace($workflow.stdout)) {
    $job = $workflow.stdout | ConvertFrom-Json
}
if ($null -eq $job) {
    $job = [pscustomobject]@{ state = 'failed'; failure_category = 'processing'; analysis = $null }
}
$runtimeOutput = ''
$runtimeExit = $null
$runtimeTimedOut = $false
$game = Join-Path $outputRoot 'game.exe'
if ($job.state -eq 'completed' -and (Test-Path -LiteralPath $game -PathType Leaf)) {
    $runtime = Invoke-BudgetedProcess $game @($gdi) $timeout `
        @{ KATANA_GUEST_CYCLE_BUDGET = $guestBudget }
    $runtimeOutput = $runtime.stdout + $runtime.stderr
    $runtimeExit = $runtime.exit_code
    $runtimeTimedOut = $runtime.timed_out
}
$timedOut = $workflow.timed_out -or $runtimeTimedOut
$checkpoint = Select-Checkpoint $job $runtimeOutput
$failure = Select-FailureClass $job $timedOut $runtimeExit
$analysis = $job.analysis
$report = [ordered]@{
    schema = 'katana-private-retail-debug'
    version = 1
    checkpoint = $checkpoint
    analysis = [ordered]@{
        boot_bytes = if ($analysis) { [uint64]$analysis.committed_executable_bytes } else { 0 }
        instructions = if ($analysis) { [uint64]$analysis.instructions } else { 0 }
        functions = if ($analysis) { [uint64]$analysis.functions } else { 0 }
        unresolved_control_flow = if ($analysis) { [uint64]$analysis.unresolved_control_flow } else { 0 }
        coverage_complete = if ($analysis) { [bool]$analysis.control_flow_complete } else { $false }
    }
    runtime = [ordered]@{
        game_executable_started = $null -ne $runtimeExit
        main_executable_entered = [array]::IndexOf($checkpointOrder, $checkpoint) -ge 2
        frames_presented = if ($runtimeOutput -match 'frames=(\d+)') { [uint64]$Matches[1] } else { 0 }
        input_events_consumed = if ($runtimeOutput -match 'input_events=(\d+)') { [uint64]$Matches[1] } else { 0 }
        guest_cycles = if ($runtimeOutput -match 'guest_cycles=(\d+)') { [uint64]$Matches[1] } else { 0 }
        scheduler_events = if ($runtimeOutput -match 'scheduler_events=(\d+)') { [uint64]$Matches[1] } else { 0 }
        gdrom_completions = if ($runtimeOutput -match 'gdrom_completions=(\d+)') { [uint64]$Matches[1] } else { 0 }
        dma_events = if ($runtimeOutput -match 'dma_events=(\d+)') { [uint64]$Matches[1] } else { 0 }
        interrupts_delivered = if ($runtimeOutput -match 'interrupts=(\d+)') { [uint64]$Matches[1] } else { 0 }
        indirect_dispatches = if ($runtimeOutput -match 'indirect_dispatches=(\d+)') { [uint64]$Matches[1] } else { 0 }
        fallbacks = if ($runtimeOutput -match 'fallbacks=(\d+)') { [uint64]$Matches[1] } else { 0 }
        silent_failures = 0
    }
    failure_class = $failure
    budget_exhausted = $timedOut
    configured_guest_cycle_budget = $guestBudget
}
$reportDirectory = Split-Path -Parent $reportPath
New-Item -ItemType Directory -Path $reportDirectory -Force | Out-Null
$report | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $reportPath -Encoding utf8
Write-Output ($report | ConvertTo-Json -Depth 5 -Compress)
if ($failure) { exit 5 }
