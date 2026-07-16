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
    $expectedIndex = if ($checkpoint -eq 'SA_ANALYSIS_CONTINUES') { 2 } else { 1 }
    $seen = @{}
    foreach ($line in ($RuntimeOutput -split "`r?`n")) {
        if ($line -notmatch '^(SA_[A-Z_]+)(?:\s|$)') { continue }
        $candidate = $Matches[1]
        $index = [array]::IndexOf($checkpointOrder, $candidate)
        if ($index -lt 2) { throw "Ungueltiger Runtimecheckpoint: $candidate" }
        if ($seen.ContainsKey($candidate)) { throw "Doppelter Runtimecheckpoint: $candidate" }
        if ($index -ne $expectedIndex) {
            throw "Fehlender oder vertauschter Runtimecheckpoint vor $candidate"
        }
        $seen[$candidate] = $true
        $checkpoint = $candidate
        $expectedIndex++
    }
    return $checkpoint
}

function Select-SilentFailures {
    param([string]$RuntimeOutput)
    $matches = [regex]::Matches($RuntimeOutput, '(?m)^KATANA_RUNTIME_METRICS\s+[^\r\n]*\bsilent_failures=(\d+)\b')
    if ($matches.Count -eq 0) { return $null }
    if ($matches.Count -ne 1) { throw 'Runtimemetrikenmarker ist doppelt.' }
    return [uint64]$matches[0].Groups[1].Value
}

function Resolve-PhysicalPath {
    param([string]$Path)
    $full = [IO.Path]::GetFullPath($Path)
    $suffix = [Collections.Generic.Stack[string]]::new()
    $existing = $full
    while (-not (Test-Path -LiteralPath $existing)) {
        $leaf = Split-Path -Leaf $existing
        if ([string]::IsNullOrEmpty($leaf)) { throw "Pfad besitzt keinen aufloesbaren Elternpfad: $Path" }
        $suffix.Push($leaf)
        $existing = Split-Path -Parent $existing
    }
    if ($IsLinux -or $IsMacOS) {
        $resolved = (& readlink -f -- $existing).Trim()
    } else {
        if (-not ('KatanaPath.Native' -as [type])) {
            Add-Type -Namespace KatanaPath -Name Native -MemberDefinition @'
[DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
public static extern Microsoft.Win32.SafeHandles.SafeFileHandle CreateFile(
    string name, uint access, uint share, IntPtr security, uint creation, uint flags, IntPtr templateFile);
[DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
public static extern uint GetFinalPathNameByHandle(
    Microsoft.Win32.SafeHandles.SafeFileHandle handle, StringBuilder path, uint size, uint flags);
'@
        }
        $handle = [KatanaPath.Native]::CreateFile($existing, 0x80, 7, [IntPtr]::Zero, 3, 0x02000000, [IntPtr]::Zero)
        if ($handle.IsInvalid) { throw "Physischer Pfad konnte nicht geoeffnet werden: $Path" }
        try {
            $buffer = [Text.StringBuilder]::new(32768)
            $length = [KatanaPath.Native]::GetFinalPathNameByHandle($handle, $buffer, $buffer.Capacity, 0)
            if ($length -eq 0 -or $length -ge $buffer.Capacity) { throw "Physischer Pfad konnte nicht aufgeloest werden: $Path" }
            $resolved = $buffer.ToString()
            if ($resolved.StartsWith('\\?\UNC\')) { $resolved = '\\' + $resolved.Substring(8) }
            elseif ($resolved.StartsWith('\\?\')) { $resolved = $resolved.Substring(4) }
        } finally { $handle.Dispose() }
    }
    while ($suffix.Count -gt 0) { $resolved = Join-Path $resolved $suffix.Pop() }
    return [IO.Path]::GetFullPath($resolved)
}

function Assert-OutsideRepository {
    param([string]$Path, [string]$RepositoryRoot)
    $lexicalCandidate = [IO.Path]::GetFullPath($Path)
    $lexicalRoot = [IO.Path]::GetFullPath($RepositoryRoot)
    $candidate = Resolve-PhysicalPath $Path
    $root = Resolve-PhysicalPath $RepositoryRoot
    if ($lexicalCandidate.Equals($lexicalRoot, [StringComparison]::OrdinalIgnoreCase) -or
        $lexicalCandidate.StartsWith($lexicalRoot + [IO.Path]::DirectorySeparatorChar,
                                     [StringComparison]::OrdinalIgnoreCase) -or
        $candidate.Equals($root, [StringComparison]::OrdinalIgnoreCase) -or
        $candidate.StartsWith($root + [IO.Path]::DirectorySeparatorChar,
                             [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Private Retail-Konfiguration und -Ausgaben muessen ausserhalb des Repositorys liegen.'
    }
    return $candidate
}

function Select-FailureClass {
    param($Job, [bool]$TimedOut, [Nullable[int]]$RuntimeExitCode,
          [Nullable[uint64]]$SilentFailures = $null)
    if ($TimedOut) { return 'host-time-budget' }
    if ($null -ne $SilentFailures -and $SilentFailures -ne 0) { return 'silent-failures' }
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
        process_started = $true
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
    foreach ($invalid in @(
        "SA_FIRST_FRAME`n",
        "SA_MAIN_ENTERED`nSA_MAIN_ENTERED`n",
        "SA_FIRST_FRAME`nSA_MAIN_ENTERED`n"
    )) {
        try { [void](Select-Checkpoint $partial $invalid); throw 'ungueltig-akzeptiert' }
        catch { if ($_.Exception.Message -eq 'ungueltig-akzeptiert') { throw 'Ungueltige Checkpointfolge wurde akzeptiert.' } }
    }
    if ($null -ne (Select-SilentFailures '') -or
        (Select-SilentFailures 'KATANA_RUNTIME_METRICS silent_failures=2') -ne 2) {
        throw 'silent_failures wird erfunden oder nicht aus dem Runtimevertrag gelesen.'
    }
    $fakeRoot = Join-Path ([IO.Path]::GetTempPath()) ('katana-retail-path-' + [guid]::NewGuid())
    $fakeRepo = Join-Path $fakeRoot 'repo'
    $outside = Join-Path $fakeRoot 'outside'
    New-Item -ItemType Directory -Path $fakeRepo,$outside -Force | Out-Null
    try {
        try { [void](Assert-OutsideRepository $fakeRepo $fakeRepo); throw 'root-akzeptiert' }
        catch { if ($_.Exception.Message -eq 'root-akzeptiert') { throw 'Repositorywurzel wurde als privater Pfad akzeptiert.' } }
        $insideConfig = Join-Path $fakeRepo 'retail-config.json'
        Set-Content $insideConfig '{}' -Encoding utf8
        try { [void](Assert-OutsideRepository $insideConfig $fakeRepo); throw 'config-akzeptiert' }
        catch { if ($_.Exception.Message -eq 'config-akzeptiert') { throw 'Konfiguration im Repository wurde akzeptiert.' } }
        $junction = Join-Path $outside 'repo-junction'
        New-Item -ItemType Junction -Path $junction -Target $fakeRepo | Out-Null
        try { [void](Assert-OutsideRepository (Join-Path $junction 'future.json') $fakeRepo); throw 'junction-akzeptiert' }
        catch { if ($_.Exception.Message -eq 'junction-akzeptiert') { throw 'Junction/Reparse Point ins Repository wurde akzeptiert.' } }
        $outwardJunction = Join-Path $fakeRepo 'outside-junction'
        New-Item -ItemType Junction -Path $outwardJunction -Target $outside | Out-Null
        try { [void](Assert-OutsideRepository (Join-Path $outwardJunction 'future.json') $fakeRepo); throw 'outward-akzeptiert' }
        catch { if ($_.Exception.Message -eq 'outward-akzeptiert') { throw 'Junction/Reparse Point aus dem Repository wurde akzeptiert.' } }
    } finally { Remove-Item -LiteralPath $fakeRoot -Recurse -Force }
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
$configPath = Assert-OutsideRepository $Config $repositoryRoot
$settings = Get-Content -LiteralPath $configPath -Raw | ConvertFrom-Json
if ($settings.schema -ne 'katana-private-retail-debug-config' -or $settings.version -ne 1) {
    throw 'Unbekannter privater Debugkonfigurationsvertrag.'
}
foreach ($field in @('manifest_path', 'gdi_path', 'output_root', 'report_path',
                      'host_timeout_seconds', 'guest_cycle_budget')) {
    if ($null -eq $settings.$field) { throw "Konfigurationsfeld fehlt: $field" }
}
$manifest = Assert-OutsideRepository ([string]$settings.manifest_path) $repositoryRoot
$gdi = Assert-OutsideRepository ([string]$settings.gdi_path) $repositoryRoot
$outputRoot = Assert-OutsideRepository ([string]$settings.output_root) $repositoryRoot
$reportPath = Assert-OutsideRepository ([string]$settings.report_path) $repositoryRoot
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
$runtimeStarted = $false
$game = Join-Path $outputRoot 'game.exe'
if ($job.state -eq 'completed' -and (Test-Path -LiteralPath $game -PathType Leaf)) {
    $runtime = Invoke-BudgetedProcess $game @($gdi) $timeout `
        @{ KATANA_GUEST_CYCLE_BUDGET = $guestBudget }
    $runtimeOutput = $runtime.stdout + $runtime.stderr
    $runtimeStarted = $runtime.process_started
    $runtimeExit = $runtime.exit_code
    $runtimeTimedOut = $runtime.timed_out
}
$timedOut = $workflow.timed_out -or $runtimeTimedOut
$checkpoint = Select-Checkpoint $job $runtimeOutput
$silentFailures = Select-SilentFailures $runtimeOutput
$failure = Select-FailureClass $job $timedOut $runtimeExit $silentFailures
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
        game_executable_started = $runtimeStarted
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
        silent_failures = $silentFailures
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
