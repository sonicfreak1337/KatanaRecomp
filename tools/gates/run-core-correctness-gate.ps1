[CmdletBinding()]
param([switch]$BuildGuiPackage, [switch]$AllowDirtyWorktree)

$ErrorActionPreference = 'Stop'

function Initialize-MsvcEnvironment {
    $ready = -not [string]::IsNullOrWhiteSpace($env:INCLUDE) -and
        -not [string]::IsNullOrWhiteSpace($env:LIB) -and
        $env:VSCMD_ARG_TGT_ARCH -eq 'x64' -and
        $env:VSCMD_ARG_HOST_ARCH -eq 'x64'
    if ($ready) {
        return
    }

    $vswhere = Join-Path ${env:ProgramFiles(x86)} `
        'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
        return
    }
    $installation = & $vswhere -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath | Select-Object -First 1
    if ([string]::IsNullOrWhiteSpace($installation)) {
        return
    }
    $devCmd = Join-Path $installation 'Common7\Tools\VsDevCmd.bat'
    if (-not (Test-Path -LiteralPath $devCmd -PathType Leaf)) {
        return
    }

    $command = "call `"$devCmd`" -no_logo -arch=x64 -host_arch=x64 >nul && set"
    $environment = & $env:ComSpec /d /s /c $command
    if ($LASTEXITCODE -ne 0) {
        throw 'Die Visual-Studio-Buildumgebung konnte nicht geladen werden.'
    }
    $pathEntry = $environment | Where-Object {
        $_.StartsWith('PATH=', [StringComparison]::Ordinal)
    } | Select-Object -First 1
    if ([string]::IsNullOrWhiteSpace($pathEntry)) {
        throw 'Die Visual-Studio-Buildumgebung besitzt keinen PATH.'
    }
    # Windows environment blocks may legally contain PATH and Path at once.
    # MSBuild rejects that block before CL starts, so remove both exact entries
    # before publishing the single VsDevCmd value.
    [Environment]::SetEnvironmentVariable('PATH', $null, 'Process')
    [Environment]::SetEnvironmentVariable('Path', $null, 'Process')
    [Environment]::SetEnvironmentVariable('PATH', $pathEntry.Substring(5), 'Process')
    foreach ($entry in $environment) {
        $separator = $entry.IndexOf('=')
        if ($separator -gt 0) {
            $name = $entry.Substring(0, $separator)
            # Some hosts expose both PATH and Path. VsDevCmd updates PATH, while
            # the stale mixed-case duplicate would otherwise overwrite it here.
            if ($name -ieq 'Path') {
                continue
            }
            [Environment]::SetEnvironmentVariable(
                $name,
                $entry.Substring($separator + 1),
                'Process'
            )
        }
    }
}

$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$build = [IO.Path]::GetFullPath((Join-Path $root 'build-current'))
$parallelism = 8
$maximumBuildAttempts = 8
if (-not $build.StartsWith(
        $root + [IO.Path]::DirectorySeparatorChar,
        [StringComparison]::OrdinalIgnoreCase
    )) {
    throw 'KR-4618-Buildziel liegt ausserhalb des Repositorys.'
}

$unexpected = @(Get-ChildItem -LiteralPath $root -Directory -Force | Where-Object {
    $_.Name -ne 'build-current' -and
    ($_.Name -eq 'build' -or $_.Name -like 'build-*' -or $_.Name -like 'cmake-build-*')
})
if ($unexpected.Count -ne 0) {
    throw "Unerwartete Buildverzeichnisse: $(($unexpected.Name | Sort-Object) -join ', ')"
}

$git = Get-Command git.exe -ErrorAction Stop | Select-Object -First 1 -ExpandProperty Source
$dirty = @(& $git -C $root status --porcelain=v1 --untracked-files=all)
if ($LASTEXITCODE -ne 0 -or ($dirty.Count -ne 0 -and -not $AllowDirtyWorktree)) {
    throw 'KR-4618 verlangt vor dem Gate einen sauberen vorbereiteten Commit.'
}

Initialize-MsvcEnvironment

function Find-CMakeNinja {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} `
        'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $vswhere -PathType Leaf) {
        $installation = & $vswhere -latest -products * `
            -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
            -property installationPath | Select-Object -First 1
        if (-not [string]::IsNullOrWhiteSpace($installation)) {
            $bundledNinja = Join-Path $installation `
                'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'
            if (Test-Path -LiteralPath $bundledNinja -PathType Leaf) {
                return $bundledNinja
            }
        }
    }

    return Get-Command ninja.exe -ErrorAction Stop |
        Select-Object -First 1 -ExpandProperty Source
}

$ninja = Find-CMakeNinja
& $ninja --version | Out-Null
if ($LASTEXITCODE -ne 0) {
    throw "Ninja-Werkzeugpruefung fehlgeschlagen: $LASTEXITCODE"
}
$ninjaDirectory = Split-Path -Parent $ninja
if (($env:PATH -split ';') -notcontains $ninjaDirectory) {
    $env:PATH = $ninjaDirectory + ';' + $env:PATH
}

function Reset-BuildDirectory {
    if (Test-Path -LiteralPath $build) {
        Remove-Item -LiteralPath $build -Recurse -Force
    }
}

function Require-NativeSuccess([string]$description) {
    if ($LASTEXITCODE -ne 0) {
        throw "$description fehlgeschlagen: $LASTEXITCODE"
    }
}

function Get-TransientBuildFailureReason([string]$output) {
    if ($output -match '(?im)LNK1168:') {
        return 'windows-link-output-locked-lnk1168'
    }
    if ($output -match '(?im)LNK1104:[^\r\n]*\.exe(?:["'']|\s|$)') {
        return 'windows-executable-locked-lnk1104'
    }
    return $null
}

function Invoke-GateBuild([string]$preset, [string]$description) {
    $records = @()
    for ($attempt = 1; $attempt -le $maximumBuildAttempts; ++$attempt) {
        $nativeOutput = @(& cmake --build --preset $preset --parallel $parallelism 2>&1)
        $exitCode = $LASTEXITCODE
        foreach ($line in $nativeOutput) {
            Write-Host $line
        }
        $outputText = ($nativeOutput | Out-String)
        if ($exitCode -eq 0) {
            $records += [ordered]@{
                attempt = $attempt
                exit_code = 0
                outcome = 'success'
                retry_reason = $null
            }
            return $records
        }
        $retryReason = Get-TransientBuildFailureReason $outputText
        $records += [ordered]@{
            attempt = $attempt
            exit_code = $exitCode
            outcome = if ($null -eq $retryReason) { 'non-transient-failure' } else { 'retry' }
            retry_reason = $retryReason
        }
        if ($null -eq $retryReason) {
            throw "$description nicht transient fehlgeschlagen: $exitCode"
        }
        if ($attempt -ge $maximumBuildAttempts) {
            throw "$description nach $attempt transienten Versuchen fehlgeschlagen: $exitCode"
        }
        Write-Warning ("$description wird nach $retryReason erneut versucht " +
            "($attempt/$maximumBuildAttempts).")
        Start-Sleep -Milliseconds 750
    }
}

function Get-TestNames {
    $json = (& ctest --test-dir $build --show-only=json-v1 | Out-String) | ConvertFrom-Json
    Require-NativeSuccess 'CTest-Inventar'
    return @($json.tests.name | Sort-Object)
}

Push-Location $root
try {
    Reset-BuildDirectory
    $debugWatch = [Diagnostics.Stopwatch]::StartNew()
    & cmake --preset quality-debug --fresh "-DCMAKE_MAKE_PROGRAM=$ninja"
    Require-NativeSuccess 'Instrumentierte Debug-Konfiguration'
    $debugBuildAttempts = @(Invoke-GateBuild 'quality-debug' 'Instrumentierter Debug-Build')

    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools\quality\check-format.ps1
    Require-NativeSuccess 'Formatpruefung'
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
        tools\quality\audit-quality-contract.ps1 -SelfTest
    Require-NativeSuccess 'Qualitaetsvertragaudit'
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
        tools\quality\audit-reference-provenance.ps1 -SelfTest
    Require-NativeSuccess 'Referenz-/Lizenzaudit'
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
        tools\quality\audit-retail-content.ps1 -SelfTest
    Require-NativeSuccess 'Retail-Content-Audit'

    $debugTests = Get-TestNames
    & ctest --preset quality-debug --parallel 8
    Require-NativeSuccess 'Instrumentierte Debug-Regression'
    $debugWatch.Stop()

    Reset-BuildDirectory
    $relWatch = [Diagnostics.Stopwatch]::StartNew()
    & cmake --preset relwithdebinfo-gate --fresh "-DCMAKE_MAKE_PROGRAM=$ninja"
    Require-NativeSuccess 'RelWithDebInfo-Konfiguration'
    $relBuildAttempts = @(Invoke-GateBuild 'relwithdebinfo-gate' 'RelWithDebInfo-Build')
    $relTests = Get-TestNames
    $configurationSpecificTests = @('katana-phase10-msvc-asan-runtime')
    $debugCoreTests = @($debugTests | Where-Object { $_ -notin $configurationSpecificTests })
    $relCoreTests = @($relTests | Where-Object { $_ -notin $configurationSpecificTests })
    $testDifference = @(Compare-Object $debugCoreTests $relCoreTests)
    if ($testDifference.Count -ne 0) {
        throw 'Debug und RelWithDebInfo besitzen unterschiedliche Core-Testinventare.'
    }
    & ctest --preset relwithdebinfo-gate --parallel 8
    Require-NativeSuccess 'RelWithDebInfo-Regression'
    $guiPackage = $null
    $guiPackageManifestSha256 = $null
    if ($BuildGuiPackage) {
        & cmake -S $root -B $build -DKATANA_BUILD_DESKTOP_GUI=ON `
            "-DCMAKE_MAKE_PROGRAM=$ninja"
        Require-NativeSuccess 'RelWithDebInfo-GUI-Konfiguration'
        $guiBuildOutput = @(& cmake --build $build --parallel $parallelism 2>&1)
        foreach ($line in $guiBuildOutput) { Write-Host $line }
        Require-NativeSuccess 'RelWithDebInfo-GUI-Build'
        $guiSmoke = @(& (Join-Path $build 'katana-recomp-gui.exe') --smoke)
        Require-NativeSuccess 'RelWithDebInfo-GUI-Smoke'
        if ($guiSmoke -notcontains 'KR_PHASE10_GUI_MINIMAL_START') {
            throw 'RelWithDebInfo-GUI besitzt keinen stabilen Smoke-Marker.'
        }
        $guiPackage = Join-Path $build 'artifacts\v047-gui-current'
        & cmake "-DSOURCE_DIR=$root" "-DBUILD_DIR=$build" "-DOUTPUT_DIR=$guiPackage" `
            -P tools\phase10\package-gui.cmake
        Require-NativeSuccess 'Relocatable v0.47-GUI-Paket'
        $guiPackageManifestSha256 = (Get-FileHash `
            (Join-Path $guiPackage 'package-manifest.json') -Algorithm SHA256).Hash.ToLowerInvariant()
    }
    $relWatch.Stop()

    $reportDirectory = Join-Path $build 'reports'
    New-Item -ItemType Directory -Path $reportDirectory -Force | Out-Null
    $report = [ordered]@{
        schema = 'katana-core-correctness-gate'
        report_version = 1
        status = 'success'
        source_commit = (& $git -C $root rev-parse HEAD).Trim()
        source_worktree = if ($dirty.Count -eq 0) { 'clean' } else { 'uncommitted-local-state' }
        configurations = @('Debug', 'RelWithDebInfo')
        debug_profile = 'quality-debug'
        debug_sanitizer = 'msvc-address-or-clang-address-undefined'
        debug_static_analysis = 'active'
        relwithdebinfo_profile = 'relwithdebinfo-gate'
        build_parallelism = $parallelism
        maximum_build_attempts = $maximumBuildAttempts
        debug_build_attempt_count = $debugBuildAttempts.Count
        debug_build_attempts = $debugBuildAttempts
        relwithdebinfo_build_attempt_count = $relBuildAttempts.Count
        relwithdebinfo_build_attempts = $relBuildAttempts
        debug_test_count = $debugTests.Count
        relwithdebinfo_test_count = $relTests.Count
        shared_core_test_count = $relCoreTests.Count
        identical_core_test_inventory = $true
        configuration_specific_tests = @(
            $debugTests + $relTests |
                Where-Object { $_ -in $configurationSpecificTests } |
                Sort-Object -Unique
        )
        exact_reference_vectors = 'passed-in-both-configurations'
        format_check = 'success'
        quality_contract_audit = 'success'
        reference_license_audit = 'success'
        debug_elapsed_seconds = [Math]::Round($debugWatch.Elapsed.TotalSeconds, 3)
        relwithdebinfo_elapsed_seconds = [Math]::Round($relWatch.Elapsed.TotalSeconds, 3)
        fresh_build_directory = 'build-current'
        private_retail_data = 'not-used'
        gui_package = if ($BuildGuiPackage) { 'verified-relocatable' } else { 'not-requested' }
        gui_package_manifest_sha256 = $guiPackageManifestSha256
    }
    $report | ConvertTo-Json -Depth 4 | Set-Content `
        (Join-Path $reportDirectory 'core-correctness-gate.json') -Encoding utf8
}
finally {
    Pop-Location
}

Write-Output 'KR_CORE_CORRECTNESS_GATE_SUCCESS'
