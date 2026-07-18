[CmdletBinding()]
param(
    [string]$Config,
    [switch]$SelfTest
)

$ErrorActionPreference = 'Stop'
$buildOnlyMode = 'build-only'
$requiredRuntimeAbi = 14
$requiredPortContract = 6
$requiredApplicationContract = 7
$checkpointOrder = @('SA_NOT_REACHED', 'SA_ANALYSIS_CONTINUES')
$script:RuntimeProcessStarts = 0

function Resolve-PhysicalPath {
    param([string]$Path)
    $full = [IO.Path]::GetFullPath($Path)
    $suffix = [Collections.Generic.Stack[string]]::new()
    $existing = $full
    while (-not (Test-Path -LiteralPath $existing)) {
        $leaf = Split-Path -Leaf $existing
        if ([string]::IsNullOrEmpty($leaf)) {
            throw "Pfad besitzt keinen aufloesbaren Elternpfad: $Path"
        }
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
    Microsoft.Win32.SafeHandles.SafeFileHandle handle, System.Text.StringBuilder path, uint size, uint flags);
'@
        }
        $handle = [KatanaPath.Native]::CreateFile(
            $existing, 0x80, 7, [IntPtr]::Zero, 3, 0x02000000, [IntPtr]::Zero)
        if ($handle.IsInvalid) { throw "Physischer Pfad konnte nicht geoeffnet werden: $Path" }
        try {
            $buffer = [Text.StringBuilder]::new(32768)
            $length = [KatanaPath.Native]::GetFinalPathNameByHandle(
                $handle, $buffer, $buffer.Capacity, 0)
            if ($length -eq 0 -or $length -ge $buffer.Capacity) {
                throw "Physischer Pfad konnte nicht aufgeloest werden: $Path"
            }
            $resolved = $buffer.ToString()
            if ($resolved.StartsWith('\\?\UNC\')) {
                $resolved = '\\' + $resolved.Substring(8)
            } elseif ($resolved.StartsWith('\\?\')) {
                $resolved = $resolved.Substring(4)
            }
        } finally {
            $handle.Dispose()
        }
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
        $lexicalCandidate.StartsWith(
            $lexicalRoot + [IO.Path]::DirectorySeparatorChar,
            [StringComparison]::OrdinalIgnoreCase) -or
        $candidate.Equals($root, [StringComparison]::OrdinalIgnoreCase) -or
        $candidate.StartsWith(
            $root + [IO.Path]::DirectorySeparatorChar,
            [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Private Retail-Konfiguration und -Ausgaben muessen ausserhalb des Repositorys liegen.'
    }
    return $candidate
}

function Assert-BuildOnlyProcessAllowed {
    param([string]$ExecutionMode, [ValidateSet('tool', 'runtime')][string]$Role)
    if ($ExecutionMode -ne $buildOnlyMode) {
        throw 'Dieser v0.47-Harness akzeptiert ausschliesslich execution_mode=build-only.'
    }
    if ($Role -eq 'runtime') {
        throw 'Build-only verbietet Runtimeprozesse vor Process.Start.'
    }
}

function Invoke-BudgetedProcess {
    param(
        [string]$ExecutionMode,
        [ValidateSet('tool', 'runtime')][string]$Role,
        [string]$Executable,
        [string[]]$Arguments,
        [int]$TimeoutSeconds
    )
    Assert-BuildOnlyProcessAllowed $ExecutionMode $Role
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $restorePayloadEnvironment = $false
    $previousPayloadEnvironment = $null
    if ($start.PSObject.Properties.Name -contains 'ArgumentList') {
        $start.FileName = $Executable
        foreach ($argument in $Arguments) {
            [void]$start.ArgumentList.Add([string]$argument)
        }
    } else {
        $payload = [ordered]@{
            executable = $Executable
            arguments = @($Arguments)
        } | ConvertTo-Json -Compress
        $previousPayloadEnvironment =
            [Environment]::GetEnvironmentVariable('KATANA_PROCESS_PAYLOAD', 'Process')
        [Environment]::SetEnvironmentVariable(
            'KATANA_PROCESS_PAYLOAD',
            [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($payload)),
            'Process')
        $restorePayloadEnvironment = $true
        $wrapper = @'
$payload = [Text.Encoding]::UTF8.GetString(
    [Convert]::FromBase64String($env:KATANA_PROCESS_PAYLOAD)) | ConvertFrom-Json
$arguments = @($payload.arguments | ForEach-Object { [string]$_ })
& ([string]$payload.executable) @arguments
exit $LASTEXITCODE
'@
        $encodedWrapper =
            [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($wrapper))
        $start.FileName = [Diagnostics.Process]::GetCurrentProcess().MainModule.FileName
        $start.Arguments = '-NoProfile -NonInteractive -EncodedCommand ' + $encodedWrapper
    }
    try {
        $process = [Diagnostics.Process]::Start($start)
    } finally {
        if ($restorePayloadEnvironment) {
            [Environment]::SetEnvironmentVariable(
                'KATANA_PROCESS_PAYLOAD', $previousPayloadEnvironment, 'Process')
        }
    }
    if ($Role -eq 'runtime') { $script:RuntimeProcessStarts++ }
    $stdout = $process.StandardOutput.ReadToEndAsync()
    $stderr = $process.StandardError.ReadToEndAsync()
    $finished = $process.WaitForExit($TimeoutSeconds * 1000)
    if (-not $finished) {
        if ($IsLinux -or $IsMacOS) {
            & kill -TERM $process.Id 2>$null
        } else {
            & taskkill.exe /PID $process.Id /T /F *> $null
        }
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

function Get-ManifestInputPath {
    param([string]$ManifestPath)
    $text = Get-Content -LiteralPath $ManifestPath -Raw
    $format = [regex]::Matches($text, '(?m)^\s*input\.format\s*=\s*([^\r\n]+?)\s*$')
    $input = [regex]::Matches($text, '(?m)^\s*input\.path\s*=\s*([^\r\n]+?)\s*$')
    if ($format.Count -ne 1 -or $format[0].Groups[1].Value.Trim() -ne 'gdi') {
        throw 'Privates Manifest muss genau ein input.format = gdi enthalten.'
    }
    if ($input.Count -ne 1 -or [string]::IsNullOrWhiteSpace($input[0].Groups[1].Value)) {
        throw 'Privates Manifest muss genau einen GDI-Eingabepfad enthalten.'
    }
    $value = $input[0].Groups[1].Value.Trim()
    if ([IO.Path]::IsPathRooted($value)) { return [IO.Path]::GetFullPath($value) }
    return [IO.Path]::GetFullPath((Join-Path (Split-Path -Parent $ManifestPath) $value))
}

function Assert-ManifestGdiBinding {
    param([string]$ManifestPath, [string]$GdiPath)
    $manifestInput = Resolve-PhysicalPath (Get-ManifestInputPath $ManifestPath)
    $configuredGdi = Resolve-PhysicalPath $GdiPath
    if (-not $manifestInput.Equals($configuredGdi, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Harness-GDI und Manifest-GDI sind nicht dieselbe physische Eingabe.'
    }
}

function Get-PortableInventory {
    param([string]$GeneratedRoot)
    $inventory = @()
    foreach ($role in @('code', 'include', 'metadata')) {
        $root = Join-Path $GeneratedRoot $role
        if (-not (Test-Path -LiteralPath $root -PathType Container)) {
            throw "Portables Erzeugnisverzeichnis fehlt: $role"
        }
        foreach ($file in @(Get-ChildItem -LiteralPath $root -File -Recurse | Sort-Object FullName)) {
            if (($file.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw 'Portable Erzeugnisse duerfen keine Reparse Points enthalten.'
            }
            $relative = $file.FullName.Substring(
                [IO.Path]::GetFullPath($GeneratedRoot).TrimEnd('\', '/').Length).TrimStart('\', '/')
            $inventory += [pscustomobject]@{
                path = $relative.Replace('\', '/')
                hash = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
            }
        }
    }
    if ($inventory.Count -eq 0) { throw 'Portables Erzeugnisinventar ist leer.' }
    return @($inventory | Sort-Object path)
}

function Test-SameInventory {
    param([object[]]$Left, [object[]]$Right)
    if ($Left.Count -ne $Right.Count) { return $false }
    for ($index = 0; $index -lt $Left.Count; $index++) {
        if ($Left[$index].path -cne $Right[$index].path -or
            $Left[$index].hash -cne $Right[$index].hash) {
            return $false
        }
    }
    return $true
}

function Invoke-BuildJob {
    param(
        [string]$Cli,
        [string]$Manifest,
        [string]$Output,
        [int]$TimeoutSeconds,
        [string]$ExecutionMode
    )
    if (Test-Path -LiteralPath $Output) {
        throw 'Frisches Build-only-Jobziel existiert bereits.'
    }
    $process = Invoke-BudgetedProcess $ExecutionMode 'tool' $Cli `
        @('workflow', 'build', $Manifest, '--output', $Output) $TimeoutSeconds
    $job = $null
    if (-not [string]::IsNullOrWhiteSpace($process.stdout)) {
        try { $job = $process.stdout | ConvertFrom-Json }
        catch { $job = $null }
    }
    return [pscustomobject]@{ process = $process; job = $job; output = $Output }
}

function Get-WorkflowFailureClass {
    param($Run)
    if ($null -eq $Run) { return 'not-started' }
    if ($Run.process.timed_out) { return 'host-time-budget' }
    if ($null -eq $Run.job) { return 'invalid-job-result' }
    if ($Run.job.state -eq 'partial') { return 'analysis-incomplete' }
    if ($Run.job.state -eq 'failed') {
        switch ($Run.job.failure_category) {
            'input-output' { return 'input-output' }
            'build' { return 'host-build' }
            'code-generation' { return 'code-generation' }
            default { return 'processing' }
        }
    }
    if ($Run.process.exit_code -ne 0) { return 'tool-nonzero' }
    return $null
}

function Assert-BuildEvidence {
    param($Run)
    $failure = Get-WorkflowFailureClass $Run
    if ($failure) { throw "Build-only-Workflow fehlgeschlagen: $failure" }
    $job = $Run.job
    if ([int]$job.version -ne $requiredApplicationContract -or
        $job.kind -ne 'build' -or $job.state -ne 'completed' -or
        $job.project_identity -notmatch '^[0-9a-fA-F]{64}$') {
        throw 'Buildjob besitzt keinen kompatiblen Vertrag, Abschluss oder portable Identitaet.'
    }
    if ($null -eq $job.analysis -or -not [bool]$job.analysis.control_flow_complete -or
        [uint64]$job.analysis.unresolved_control_flow -ne 0 -or
        [uint64]$job.analysis.guarded_partial_control_flow -ne 0 -or
        [uint64]$job.analysis.unknown_instructions -ne 0 -or
        [uint64]$job.analysis.reachable_abort_edges -ne 0 -or
        [uint64]$job.analysis.incomplete_initial_required_code_bytes -ne 0 -or
        [uint64]$job.analysis.uncovered_runtime_materializable_bytes -ne 0 -or
        [uint64]$job.analysis.uncovered_control_targets -ne 0 -or
        [uint64]$job.analysis.dispatch_paths_without_validation -ne 0) {
        throw 'Buildjob besitzt keine vollstaendige Kontrollflussabdeckung.'
    }
    foreach ($checkpoint in @('analysis-complete', 'codegen-complete', 'host-build-complete')) {
        if (@($job.checkpoints | Where-Object { $_ -eq $checkpoint }).Count -ne 1) {
            throw "Buildjob besitzt keinen eindeutigen Checkpoint: $checkpoint"
        }
    }
    if (@($job.checkpoints | Where-Object { $_ -like 'run-*' }).Count -ne 0) {
        throw 'Build-only-Job enthaelt einen Runtimecheckpoint.'
    }
    $executables = @($job.artifacts | Where-Object { $_.role -eq 'host-executable' })
    if ($executables.Count -ne 1 -or $executables[0].path -notin @('game.exe', 'game') -or
        $executables[0].sha256 -notmatch '^[0-9a-fA-F]{64}$') {
        throw 'Aktuelles Host-Executable-Artefakt fehlt oder ist nicht eindeutig.'
    }
    $executable = Join-Path $Run.output ([string]$executables[0].path)
    if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
        throw 'Gemeldetes Host-Executable fehlt im aktuellen Jobziel.'
    }
    $actualExecutableHash =
        (Get-FileHash -LiteralPath $executable -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actualExecutableHash -cne ([string]$executables[0].sha256).ToLowerInvariant()) {
        throw 'Host-Executable stimmt nicht mit dem aktuellen Jobartefakt ueberein.'
    }
    $jobResultPath = Join-Path $Run.output 'job-result.json'
    $portMetadataPath = Join-Path $Run.output `
        'sourcecode\generated\metadata\port-project.json'
    $resultIndexPath = Join-Path $Run.output 'result-index.json'
    $buildPlanPath = Join-Path $Run.output 'build-plan.json'
    foreach ($path in @($jobResultPath, $portMetadataPath, $resultIndexPath, $buildPlanPath)) {
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            throw 'Identitaets- oder Buildvertragsartefakt fehlt im aktuellen Job.'
        }
    }
    $jobResult = Get-Content -LiteralPath $jobResultPath -Raw | ConvertFrom-Json
    $portMetadata = Get-Content -LiteralPath $portMetadataPath -Raw | ConvertFrom-Json
    $resultIndex = Get-Content -LiteralPath $resultIndexPath -Raw | ConvertFrom-Json
    $buildPlan = Get-Content -LiteralPath $buildPlanPath -Raw | ConvertFrom-Json
    if ($jobResult.project_identity -cne $job.project_identity -or
        $portMetadata.project_identity -cne $job.project_identity -or
        $resultIndex.project_identity -cne $job.project_identity) {
        throw 'Manifest, GDI, Portmetadaten und aktueller Job verlieren ihre Identitaetsbindung.'
    }
    if ([int]$jobResult.version -ne $requiredApplicationContract -or
        [int]$buildPlan.version -ne $requiredApplicationContract -or
        [int]$portMetadata.contract_version -ne $requiredPortContract -or
        [int]$portMetadata.runtime_abi -ne $requiredRuntimeAbi) {
        throw 'Runtime-, Port- oder Anwendungsvertrag des aktuellen Jobs ist inkompatibel.'
    }
    if ($buildPlan.status -ne 'built' -or -not [bool]$buildPlan.host_compilation -or
        [bool]$buildPlan.native_execution) {
        throw 'Buildplan verletzt den Build-only-Vertrag.'
    }
    $generatedRoot = Join-Path $Run.output 'sourcecode\generated'
    return [pscustomobject]@{
        identity = [string]$job.project_identity
        analysis = $job.analysis
        executable_verified = $true
        inventory = @(Get-PortableInventory $generatedRoot)
    }
}

function Assert-RedactedReport {
    param([string]$Json)
    $privatePattern = '(?i)([A-Z]:\\|/' + 'home/|/tmp/|\.gdi|sha256|' +
        'project_identity\s*":\s*")'
    if ($Json -match $privatePattern) {
        throw 'Build-only-Bericht enthaelt private Pfade, Hashes oder Eingabeidentitaeten.'
    }
}

function ConvertTo-PrivateSafeText {
    param([string]$Text)
    if ([string]::IsNullOrWhiteSpace($Text)) { return $null }
    $safe = $Text -replace '(?i)[A-Z]:\\[^\r\n"'']+', '<redacted-path>'
    $safe = $safe -replace '(?i)[0-9a-f]{64}', '<redacted-hash>'
    $safe = $safe -replace '(?i)[^\s"'']+\.gdi', '<redacted-gdi>'
    return $safe
}

function Write-AtomicReport {
    param([string]$Path, $Report)
    $directory = Split-Path -Parent $Path
    New-Item -ItemType Directory -Path $directory -Force | Out-Null
    $json = $Report | ConvertTo-Json -Depth 6
    Assert-RedactedReport $json
    $temporary = Join-Path $directory ('.' + (Split-Path -Leaf $Path) + '.' +
        [guid]::NewGuid().ToString('N') + '.tmp')
    $backup = Join-Path $directory ('.' + (Split-Path -Leaf $Path) + '.' +
        [guid]::NewGuid().ToString('N') + '.bak')
    try {
        Set-Content -LiteralPath $temporary -Value $json -Encoding utf8
        if (Test-Path -LiteralPath $Path -PathType Leaf) {
            [IO.File]::Replace($temporary, $Path, $backup)
            Remove-Item -LiteralPath $backup -Force
        } else {
            [IO.File]::Move($temporary, $Path)
        }
    } finally {
        if (Test-Path -LiteralPath $temporary) {
            Remove-Item -LiteralPath $temporary -Force
        }
        if (Test-Path -LiteralPath $backup) {
            Remove-Item -LiteralPath $backup -Force
        }
    }
    return ($Report | ConvertTo-Json -Depth 6 -Compress)
}

if ($SelfTest) {
    Assert-BuildOnlyProcessAllowed 'build-only' 'tool'
    $toolProbe = Invoke-BudgetedProcess 'build-only' 'tool' `
        ([Diagnostics.Process]::GetCurrentProcess().MainModule.FileName) `
        @('-NoProfile', '-NonInteractive', '-Command', 'exit 0') 10
    if ($toolProbe.timed_out -or $toolProbe.exit_code -ne 0) {
        throw 'Strukturierter Build-only-Werkzeugstart ist fehlgeschlagen.'
    }
    try {
        Assert-BuildOnlyProcessAllowed 'build-only' 'runtime'
        throw 'runtime-akzeptiert'
    } catch {
        if ($_.Exception.Message -eq 'runtime-akzeptiert') {
            throw 'Build-only akzeptiert einen Runtimeprozess.'
        }
    }
    if ($script:RuntimeProcessStarts -ne 0) {
        throw 'Build-only-Prozesspolicy erfasste einen Runtimeprozess.'
    }
    $fakeRoot = Join-Path ([IO.Path]::GetTempPath()) ('katana-build-only-' + [guid]::NewGuid())
    $fakeRepo = Join-Path $fakeRoot 'repo'
    $outside = Join-Path $fakeRoot 'outside'
    New-Item -ItemType Directory -Path $fakeRepo, $outside -Force | Out-Null
    try {
        try {
            [void](Assert-OutsideRepository $fakeRepo $fakeRepo)
            throw 'root-akzeptiert'
        } catch {
            if ($_.Exception.Message -eq 'root-akzeptiert') {
                throw 'Repositorywurzel wurde als privater Pfad akzeptiert.'
            }
        }
        $junction = Join-Path $outside 'repo-junction'
        New-Item -ItemType Junction -Path $junction -Target $fakeRepo | Out-Null
        try {
            [void](Assert-OutsideRepository (Join-Path $junction 'future.json') $fakeRepo)
            throw 'junction-akzeptiert'
        } catch {
            if ($_.Exception.Message -eq 'junction-akzeptiert') {
                throw 'Junction ins Repository wurde akzeptiert.'
            }
        }
        $stale = Join-Path $outside 'game.exe'
        Set-Content -LiteralPath $stale -Value 'stale executable must not start' -Encoding ascii
        try {
            [void](Invoke-BudgetedProcess 'build-only' 'runtime' $stale @() 1)
            throw 'stale-akzeptiert'
        } catch {
            if ($_.Exception.Message -eq 'stale-akzeptiert') {
                throw 'Stale game.exe konnte den Build-only-Prozessschutz umgehen.'
            }
            if ($_.Exception.Message -ne 'Build-only verbietet Runtimeprozesse vor Process.Start.') {
                throw
            }
        }
        $generatedA = Join-Path $outside 'a'
        $generatedB = Join-Path $outside 'b'
        foreach ($generated in @($generatedA, $generatedB)) {
            foreach ($role in @('code', 'include', 'metadata')) {
                New-Item -ItemType Directory -Path (Join-Path $generated $role) -Force | Out-Null
                Set-Content -LiteralPath (Join-Path $generated "$role\fixture.txt") `
                    -Value "portable-$role" -Encoding ascii
            }
        }
        if (-not (Test-SameInventory (Get-PortableInventory $generatedA) `
                                     (Get-PortableInventory $generatedB))) {
            throw 'Bytegleiche portable Inventare werden als verschieden gemeldet.'
        }
        Set-Content -LiteralPath (Join-Path $generatedB 'code\fixture.txt') `
            -Value 'changed' -Encoding ascii
        if (Test-SameInventory (Get-PortableInventory $generatedA) `
                               (Get-PortableInventory $generatedB)) {
            throw 'Abweichendes portables Inventar wurde akzeptiert.'
        }
        $reportPath = Join-Path $outside 'report.json'
        $synthetic = [ordered]@{
            schema = 'katana-private-retail-build'
            version = 1
            execution_mode = 'build-only'
            checkpoint = 'SA_ANALYSIS_CONTINUES'
            identity_consistent = $true
            game_executable_started = $false
        }
        [void](Write-AtomicReport $reportPath $synthetic)
        if (-not (Test-Path -LiteralPath $reportPath -PathType Leaf)) {
            throw 'Atomarer synthetischer Bericht fehlt.'
        }
    } finally {
        Remove-Item -LiteralPath $fakeRoot -Recurse -Force
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
$requiredFields = @(
    'execution_mode', 'gdi_path', 'host_timeout_seconds', 'manifest_path',
    'output_root', 'report_path', 'schema', 'version')
$allowedFields = @($requiredFields + 'guest_cycle_budget')
$actualFields = @($settings.PSObject.Properties.Name | Sort-Object)
$unknownFields = @($actualFields | Where-Object { $_ -notin $allowedFields })
$missingFields = @($requiredFields | Where-Object { $_ -notin $actualFields })
if ($unknownFields.Count -ne 0 -or $missingFields.Count -ne 0) {
    throw 'Private Build-only-Konfiguration besitzt fehlende oder unbekannte Felder.'
}
if ($settings.schema -ne 'katana-private-retail-debug-config' -or
    [int]$settings.version -ne 2 -or [string]$settings.execution_mode -ne $buildOnlyMode) {
    throw 'Privater Harness verlangt Configversion 2 und execution_mode=build-only.'
}
$manifest = Assert-OutsideRepository ([string]$settings.manifest_path) $repositoryRoot
$gdi = Assert-OutsideRepository ([string]$settings.gdi_path) $repositoryRoot
$outputRoot = Assert-OutsideRepository ([string]$settings.output_root) $repositoryRoot
$reportPath = Assert-OutsideRepository ([string]$settings.report_path) $repositoryRoot
if (-not (Test-Path -LiteralPath $manifest -PathType Leaf) -or
    -not (Test-Path -LiteralPath $gdi -PathType Leaf)) {
    throw 'Privates Manifest oder GDI fehlt.'
}
$timeout = [int]$settings.host_timeout_seconds
if ($timeout -le 0) { throw 'host_timeout_seconds muss positiv sein.' }
if ($null -ne $settings.guest_cycle_budget -and [uint64]$settings.guest_cycle_budget -eq 0) {
    throw 'guest_cycle_budget muss positiv sein, wird in build-only aber nicht verbraucht.'
}
Assert-ManifestGdiBinding $manifest $gdi
$cli = Join-Path $repositoryRoot 'build-current\katana-recomp.exe'
if (-not (Test-Path -LiteralPath $cli -PathType Leaf)) {
    throw 'Gate-CLI fehlt; zuerst build-current erzeugen.'
}
New-Item -ItemType Directory -Path $outputRoot -Force | Out-Null
$runToken = [guid]::NewGuid().ToString('N')
$firstOutput = Join-Path $outputRoot ("job-$runToken-a")
$secondOutput = Join-Path $outputRoot ("job-$runToken-b")
$firstRun = $null
$secondRun = $null
$firstEvidence = $null
$secondEvidence = $null
$failure = $null
$failureDetail = $null
$completedBuilds = 0
$identityConsistent = $false
$portableMetadataEqual = $false
$generatedSourcesEqual = $false
$executablesVerified = $false
try {
    $firstRun = Invoke-BuildJob $cli $manifest $firstOutput $timeout $buildOnlyMode
    $failure = Get-WorkflowFailureClass $firstRun
    if ($failure) { throw 'erster-build-fehlgeschlagen' }
    $firstEvidence = Assert-BuildEvidence $firstRun
    $completedBuilds++
    $secondRun = Invoke-BuildJob $cli $manifest $secondOutput $timeout $buildOnlyMode
    $failure = Get-WorkflowFailureClass $secondRun
    if ($failure) { throw 'zweiter-build-fehlgeschlagen' }
    $secondEvidence = Assert-BuildEvidence $secondRun
    $completedBuilds++
    if ($firstEvidence.identity -cne $secondEvidence.identity) {
        $failure = 'identity-mismatch'
        throw 'identitaet-abweichend'
    }
    $identityConsistent = $true
    $firstMetadata = @($firstEvidence.inventory | Where-Object { $_.path -like 'metadata/*' })
    $secondMetadata = @($secondEvidence.inventory | Where-Object { $_.path -like 'metadata/*' })
    $firstSources = @($firstEvidence.inventory | Where-Object { $_.path -notlike 'metadata/*' })
    $secondSources = @($secondEvidence.inventory | Where-Object { $_.path -notlike 'metadata/*' })
    $portableMetadataEqual = Test-SameInventory $firstMetadata $secondMetadata
    if (-not $portableMetadataEqual) {
        $failure = 'portable-metadata-mismatch'
        throw 'portable-metadaten-abweichend'
    }
    $generatedSourcesEqual = Test-SameInventory $firstSources $secondSources
    if (-not $generatedSourcesEqual) {
        $failure = 'generated-sources-mismatch'
        throw 'generierte-quellen-abweichend'
    }
    $executablesVerified = [bool]($firstEvidence.executable_verified -and
                                  $secondEvidence.executable_verified)
} catch {
    if (-not $failure) { $failure = 'build-evidence-failed' }
    $failureDetail = ConvertTo-PrivateSafeText $_.Exception.Message
}
if ($script:RuntimeProcessStarts -ne 0 -and -not $failure) {
    $failure = 'runtime-process-started'
}
$success = $null -eq $failure -and $completedBuilds -eq 2 -and $identityConsistent -and
    $portableMetadataEqual -and $generatedSourcesEqual -and $executablesVerified -and
    $script:RuntimeProcessStarts -eq 0
$analysis = if ($secondEvidence) { $secondEvidence.analysis }
            elseif ($firstEvidence) { $firstEvidence.analysis }
            else { $null }
$report = [ordered]@{
    schema = 'katana-private-retail-build'
    version = 1
    config_version = 2
    execution_mode = $buildOnlyMode
    status = if ($success) { 'success' } else { 'failed' }
    checkpoint = if ($analysis) { 'SA_ANALYSIS_CONTINUES' } else { 'SA_NOT_REACHED' }
    identity_consistent = $identityConsistent
    contracts = [ordered]@{
        runtime_abi = $requiredRuntimeAbi
        port_project = $requiredPortContract
        application = $requiredApplicationContract
    }
    analysis = [ordered]@{
        committed_executable_permission_bytes = if ($analysis) {
            [uint64]$analysis.committed_executable_permission_bytes
        } else { 0 }
        initially_required_bytes = if ($analysis) {
            [uint64]$analysis.initially_required_bytes
        } else { 0 }
        static_precompiled_bytes = if ($analysis) {
            [uint64]$analysis.static_precompiled_bytes
        } else { 0 }
        runtime_materializable_bytes = if ($analysis) {
            [uint64]$analysis.runtime_materializable_bytes
        } else { 0 }
        unknown_storage_bytes = if ($analysis) {
            [uint64]$analysis.unknown_storage_bytes
        } else { 0 }
        currently_dispatchable_bytes = if ($analysis) {
            [uint64]$analysis.currently_dispatchable_bytes
        } else { 0 }
        uncovered_control_targets = if ($analysis) {
            [uint64]$analysis.uncovered_control_targets
        } else { 0 }
        dispatch_paths_without_validation = if ($analysis) {
            [uint64]$analysis.dispatch_paths_without_validation
        } else { 0 }
        materialization_attempts = if ($analysis) {
            [uint64]$analysis.materialization_attempts
        } else { 0 }
        materialization_successes = if ($analysis) {
            [uint64]$analysis.materialization_successes
        } else { 0 }
        materialization_rejections = if ($analysis) {
            [uint64]$analysis.materialization_rejections
        } else { 0 }
        materialization_budget_failures = if ($analysis) {
            [uint64]$analysis.materialization_budget_failures
        } else { 0 }
        generation_revalidation_failures = if ($analysis) {
            [uint64]$analysis.generation_revalidation_failures
        } else { 0 }
        byte_identity_failures = if ($analysis) {
            [uint64]$analysis.byte_identity_failures
        } else { 0 }
        dispatch_validation_failures = if ($analysis) {
            [uint64]$analysis.dispatch_validation_failures
        } else { 0 }
        committed_executable_bytes = if ($analysis) {
            [uint64]$analysis.committed_executable_bytes
        } else { 0 }
        instructions = if ($analysis) { [uint64]$analysis.instructions } else { 0 }
        functions = if ($analysis) { [uint64]$analysis.functions } else { 0 }
        resolved_control_flow = if ($analysis) {
            [uint64]$analysis.resolved_control_flow
        } else { 0 }
        guarded_complete_control_flow = if ($analysis) {
            [uint64]$analysis.guarded_complete_control_flow
        } else { 0 }
        guarded_partial_control_flow = if ($analysis) {
            [uint64]$analysis.guarded_partial_control_flow
        } else { 0 }
        runtime_only_control_flow = if ($analysis) {
            [uint64]$analysis.runtime_only_control_flow
        } else { 0 }
        unresolved_control_flow = if ($analysis) {
            [uint64]$analysis.unresolved_control_flow
        } else { 0 }
        unknown_instructions = if ($analysis) {
            [uint64]$analysis.unknown_instructions
        } else { 0 }
        unanalyzed_executable_bytes = if ($analysis) {
            [uint64]$analysis.unanalyzed_executable_bytes
        } else { 0 }
        runtime_deferred_executable_bytes = if ($analysis) {
            [uint64]$analysis.runtime_deferred_executable_bytes
        } else { 0 }
        never_executed_data_bytes = if ($analysis) {
            [uint64]$analysis.never_executed_data_bytes
        } else { 0 }
        unknown_executable_bytes = if ($analysis) {
            [uint64]$analysis.unknown_executable_bytes
        } else { 0 }
        unproven_padding_bytes = if ($analysis) {
            [uint64]$analysis.unproven_padding_bytes
        } else { 0 }
        incomplete_initial_required_code_bytes = if ($analysis) {
            [uint64]$analysis.incomplete_initial_required_code_bytes
        } else { 0 }
        uncovered_runtime_materializable_bytes = if ($analysis) {
            [uint64]$analysis.uncovered_runtime_materializable_bytes
        } else { 0 }
        reachable_abort_edges = if ($analysis) {
            [uint64]$analysis.reachable_abort_edges
        } else { 0 }
        coverage_complete = if ($analysis) { [bool]$analysis.control_flow_complete } else { $false }
    }
    reproducibility = [ordered]@{
        builds_requested = 2
        builds_completed = $completedBuilds
        portable_metadata_equal = $portableMetadataEqual
        generated_sources_equal = $generatedSourcesEqual
    }
    build = [ordered]@{
        host_compilation_complete = [bool]($completedBuilds -eq 2)
        current_executable_verified = $executablesVerified
        game_executable_started = $false
        runtime_processes_started = $script:RuntimeProcessStarts
    }
    failure_class = $failure
    failure_detail = $failureDetail
}
$serialized = Write-AtomicReport $reportPath $report
Write-Output $serialized
if (-not $success) { exit 5 }
