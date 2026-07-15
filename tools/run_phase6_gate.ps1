[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$GdiPath,

    [Parameter(Mandatory = $true)]
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration,

    [string]$BuildDirectory = 'build-current',
    [string]$ReportPath = ''
)

$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$build = [IO.Path]::GetFullPath((Join-Path $repo $BuildDirectory))
$gdi = [IO.Path]::GetFullPath($GdiPath)
$discDirectory = Split-Path -Parent $gdi
$configurationDirectory = Join-Path $build $Configuration
$gateDirectory = Join-Path $build 'phase6-gate'
$probeSource = Join-Path $gateDirectory "probe-$($Configuration.ToLowerInvariant()).cpp"
$probeObject = Join-Path $gateDirectory "probe-$($Configuration.ToLowerInvariant()).obj"
$probeExe = Join-Path $gateDirectory "probe-$($Configuration.ToLowerInvariant()).exe"
$firstReport = Join-Path $gateDirectory "run-$($Configuration.ToLowerInvariant())-1.json"
$secondReport = Join-Path $gateDirectory "run-$($Configuration.ToLowerInvariant())-2.json"
if ([string]::IsNullOrWhiteSpace($ReportPath)) {
    $ReportPath = Join-Path $gateDirectory "phase6-$($Configuration.ToLowerInvariant()).json"
} else {
    $ReportPath = [IO.Path]::GetFullPath((Join-Path $repo $ReportPath))
}

function Get-DiscSnapshot {
    param([string]$Directory)
    $rows = foreach ($file in Get-ChildItem -LiteralPath $Directory -File | Sort-Object Name) {
        "$($file.Name):$((Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash)"
    }
    return $rows -join '|'
}

function Initialize-MsvcEnvironment {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) {
        throw 'vswhere.exe wurde nicht gefunden.'
    }
    $installation = & $vswhere -latest -products '*' `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath
    if ([string]::IsNullOrWhiteSpace($installation)) {
        throw 'Keine MSVC-x64-Toolchain wurde gefunden.'
    }
    $versionFile = Join-Path $installation 'VC\Auxiliary\Build\Microsoft.VCToolsVersion.default.txt'
    $toolsVersion = (Get-Content -LiteralPath $versionFile -Raw).Trim()
    $vc = Join-Path $installation "VC\Tools\MSVC\$toolsVersion"
    $sdk = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10'
    $sdkVersion = (Get-ChildItem -LiteralPath (Join-Path $sdk 'Lib') -Directory |
        Sort-Object Name -Descending | Select-Object -First 1).Name
    if ([string]::IsNullOrWhiteSpace($sdkVersion)) {
        throw 'Kein Windows-10/11-SDK wurde gefunden.'
    }

    $env:PATH = "$(Join-Path $vc 'bin\Hostx64\x64');$(Join-Path $sdk "bin\$sdkVersion\x64");$env:PATH"
    $env:INCLUDE = @(
        (Join-Path $vc 'include')
        (Join-Path $sdk "Include\$sdkVersion\ucrt")
        (Join-Path $sdk "Include\$sdkVersion\shared")
        (Join-Path $sdk "Include\$sdkVersion\um")
        (Join-Path $sdk "Include\$sdkVersion\winrt")
        (Join-Path $sdk "Include\$sdkVersion\cppwinrt")
    ) -join ';'
    $env:LIB = @(
        (Join-Path $vc 'lib\x64')
        (Join-Path $sdk "Lib\$sdkVersion\ucrt\x64")
        (Join-Path $sdk "Lib\$sdkVersion\um\x64")
    ) -join ';'
    $env:LIBPATH = @(
        (Join-Path $vc 'lib\x64')
        (Join-Path $sdk "UnionMetadata\$sdkVersion")
        (Join-Path $sdk "References\$sdkVersion")
    ) -join ';'
    return Join-Path $vc 'bin\Hostx64\x64\cl.exe'
}

if (-not (Test-Path -LiteralPath $gdi -PathType Leaf)) {
    throw 'Die GDI-Quelle wurde nicht gefunden.'
}
$cli = Join-Path $configurationDirectory 'katana-recomp.exe'
$coreLibrary = Join-Path $configurationDirectory 'katana_core.lib'
$runtimeLibrary = Join-Path $configurationDirectory 'katana_runtime.lib'
foreach ($required in @($cli, $coreLibrary, $runtimeLibrary)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Fehlendes Gate-Buildartefakt fuer $Configuration."
    }
}

New-Item -ItemType Directory -Path $gateDirectory -Force | Out-Null
$before = Get-DiscSnapshot -Directory $discDirectory
$temporary = @($probeSource, $probeObject, $probeExe, $firstReport, $secondReport)
try {
    & $cli phase6-probe-source $gdi $probeSource
    if ($LASTEXITCODE -ne 0) {
        throw 'Die lokale Phase-6-Probe konnte nicht erzeugt werden.'
    }

    $compiler = Initialize-MsvcEnvironment
    $runtimeFlag = if ($Configuration -eq 'Debug') { '/MDd' } else { '/MD' }
    & $compiler /nologo /std:c++20 /EHsc /utf-8 $runtimeFlag `
        "/I$($repo)\include" $probeSource "/Fo$probeObject" "/Fe$probeExe" `
        /link "/LIBPATH:$configurationDirectory" katana_core.lib katana_runtime.lib
    if ($LASTEXITCODE -ne 0) {
        throw 'Die lokale Phase-6-Probe konnte nicht kompiliert werden.'
    }

    & $probeExe $gdi $firstReport
    if ($LASTEXITCODE -ne 0) { throw 'Der erste Phase-6-Lauf ist fehlgeschlagen.' }
    & $probeExe $gdi $secondReport
    if ($LASTEXITCODE -ne 0) { throw 'Der zweite Phase-6-Lauf ist fehlgeschlagen.' }

    $firstHash = (Get-FileHash -LiteralPath $firstReport -Algorithm SHA256).Hash
    $secondHash = (Get-FileHash -LiteralPath $secondReport -Algorithm SHA256).Hash
    if ($firstHash -ne $secondHash) {
        throw 'Die beiden redigierten Gate-Berichte sind nicht bytegleich.'
    }
    $jsonText = Get-Content -LiteralPath $firstReport -Raw
    $report = $jsonText | ConvertFrom-Json
    if ($report.checkpoint -ne 'SA_PHASE6_MAIN_EXECUTION_STARTED' -or
        $report.executed_blocks -lt 1 -or $report.guest_cycles -lt 1 -or
        $report.scheduler_events -lt 1 -or $report.gdrom_completions -lt 1 -or
        $report.tmu_events -lt 1 -or $report.dma_events -lt 1 -or
        $report.interrupts_delivered -lt 1 -or $report.silent_failures -ne 0) {
        throw 'Der redigierte Bericht erfuellt die Phase-6-Kriterien nicht.'
    }
    $sensitive = @(
        $gdi,
        $discDirectory,
        (Split-Path -Leaf $gdi)
    ) + (Get-ChildItem -LiteralPath $discDirectory -File | ForEach-Object Name)
    foreach ($value in $sensitive) {
        if (-not [string]::IsNullOrEmpty($value) -and $jsonText.Contains($value)) {
            throw 'Der Gate-Bericht enthaelt einen lokalen Pfad oder Disc-Dateinamen.'
        }
    }
    $reportParent = Split-Path -Parent $ReportPath
    if (-not [string]::IsNullOrEmpty($reportParent)) {
        New-Item -ItemType Directory -Path $reportParent -Force | Out-Null
    }
    Copy-Item -LiteralPath $firstReport -Destination $ReportPath -Force

    $after = Get-DiscSnapshot -Directory $discDirectory
    if ($before -ne $after) {
        throw 'Die lokale Disc-Quelle wurde waehrend des Gates veraendert.'
    }
    Write-Output "checkpoint=$($report.checkpoint)"
    Write-Output 'deterministic_runs=true'
    Write-Output 'disc_unchanged=true'
    Write-Output "report=$ReportPath"
} finally {
    foreach ($path in $temporary) {
        Remove-Item -LiteralPath $path -Force -ErrorAction SilentlyContinue
    }
    Get-ChildItem -LiteralPath $gateDirectory -File -Filter 'probe-*.pdb' -ErrorAction SilentlyContinue |
        Remove-Item -Force -ErrorAction SilentlyContinue
}
