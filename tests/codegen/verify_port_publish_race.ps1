param(
    [Parameter(Mandatory = $true)]
    [string] $Cli,
    [Parameter(Mandatory = $true)]
    [string] $Disc,
    [Parameter(Mandatory = $true)]
    [string] $Output,
    [Parameter(Mandatory = $true)]
    [string] $GameProject,
    [Parameter(Mandatory = $true)]
    [string] $RuntimeImagePayload,
    [Parameter(Mandatory = $true)]
    [string] $LatentAotEntry,
    [Parameter(Mandatory = $true)]
    [string] $WorkingDirectory
)

$ErrorActionPreference = "Stop"

function ConvertTo-ProcessArgument([string] $Value) {
    if ($Value.Contains('"') -or $Value.EndsWith('\')) {
        throw "Nicht sicher quotbares Testprozessargument."
    }
    if ($Value.Length -ne 0 -and $Value -notmatch '\s') {
        return $Value
    }
    return '"' + $Value + '"'
}

function New-RedirectedProcess(
    [string[]] $Arguments,
    [hashtable] $Environment = @{}) {
    $start = New-Object System.Diagnostics.ProcessStartInfo
    $start.FileName = $Cli
    $start.WorkingDirectory = $WorkingDirectory
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $start.Arguments = (
        $Arguments |
            ForEach-Object { ConvertTo-ProcessArgument $_ }) -join ' '
    foreach ($name in $Environment.Keys) {
        $start.EnvironmentVariables[$name] = [string] $Environment[$name]
    }
    $process = New-Object System.Diagnostics.Process
    $process.StartInfo = $start
    if (-not $process.Start()) {
        throw "Testprozess konnte nicht gestartet werden."
    }
    return $process
}

function Stop-TestProcess([System.Diagnostics.Process] $Process) {
    if (-not $Process.HasExited) {
        $Process.Kill()
        $Process.WaitForExit()
    }
}

function Write-Trace(
    [string] $Name,
    [string] $OutputText,
    [string] $ErrorText) {
    [Console]::Error.WriteLine("--- $Name stdout ---")
    [Console]::Error.Write($OutputText)
    [Console]::Error.WriteLine("--- $Name stderr ---")
    [Console]::Error.Write($ErrorText)
}

$firstArguments = @(
    "port", $Disc,
    "--output", $Output,
    "--target-name", "cli_game",
    "--game-project", $GameProject,
    "--runtime-image-payload", $RuntimeImagePayload,
    "--latent-aot-mode", "exact-only",
    "--latent-aot-entry", $LatentAotEntry
)
$first = New-RedirectedProcess `
    $firstArguments `
    @{
        KATANA_PORT_PUBLISH_TEST_HOLD_LOCK_MS = "10000"
        KATANA_PORT_PUBLISH_TEST_EXIT_AFTER_RECOVERY = "1"
    }
$firstLineTask = $first.StandardOutput.ReadLineAsync()
$lockDeadline = [DateTime]::UtcNow.AddSeconds(15)
while (-not $firstLineTask.IsCompleted -and
       -not $first.HasExited -and
       [DateTime]::UtcNow -lt $lockDeadline) {
    Start-Sleep -Milliseconds 25
}

$firstLine = if ($firstLineTask.IsCompleted) {
    $firstLineTask.Result
} else {
    $null
}
if ($firstLine -ne "KATANA_PORT_PUBLISH_TEST_OUTPUT_LOCK_HELD") {
    Stop-TestProcess $first
    $firstRemainder = if ($firstLineTask.IsCompleted) {
        $first.StandardOutput.ReadToEnd()
    } else {
        ""
    }
    $firstError = $first.StandardError.ReadToEnd()
    $firstTrace = if ($null -eq $firstLine) {
        $firstRemainder
    } else {
        $firstLine + [Environment]::NewLine + $firstRemainder
    }
    Write-Trace "first" $firstTrace $firstError
    exit 91
}

$firstOutputTask = $first.StandardOutput.ReadToEndAsync()
$firstErrorTask = $first.StandardError.ReadToEndAsync()
$secondArguments = @(
    "port", $Disc,
    "--output", $Output,
    "--target-name", "cli_game_race",
    "--game-project", $GameProject,
    "--runtime-image-payload", $RuntimeImagePayload,
    "--latent-aot-mode", "exact-only",
    "--latent-aot-entry", $LatentAotEntry
)
$second = New-RedirectedProcess $secondArguments
$secondOutputTask = $second.StandardOutput.ReadToEndAsync()
$secondErrorTask = $second.StandardError.ReadToEndAsync()
if (-not $second.WaitForExit(180000)) {
    Stop-TestProcess $second
    Stop-TestProcess $first
    exit 92
}
$secondOutput = $secondOutputTask.Result
$secondError = $secondErrorTask.Result
if ($second.ExitCode -eq 0 -or
    $secondError -notmatch
        "Port-Exportpfad wird bereits von einem anderen Export verwendet") {
    Stop-TestProcess $first
    Write-Trace "second" $secondOutput $secondError
    exit 93
}

if (-not $first.WaitForExit(180000)) {
    Stop-TestProcess $first
    Stop-TestProcess $second
    exit 94
}
$firstOutput =
    $firstLine + [Environment]::NewLine + $firstOutputTask.Result
$firstError = $firstErrorTask.Result
if ($first.ExitCode -ne 0) {
    Write-Trace "first" $firstOutput $firstError
    exit 95
}

exit 0
