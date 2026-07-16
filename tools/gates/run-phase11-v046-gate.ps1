[CmdletBinding()]
param([ValidateRange(1, 32)][int]$Parallelism = 8)

$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$git = Get-Command git.exe -ErrorAction Stop | Select-Object -First 1 -ExpandProperty Source
$dirty = @(& $git -C $root status --porcelain=v1 --untracked-files=all)
if ($LASTEXITCODE -ne 0 -or $dirty.Count -ne 0) {
    throw 'KR-4604 verlangt einen sauberen vorbereiteten Commit.'
}

& powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
    (Join-Path $PSScriptRoot 'run-phase11-v045-gate.ps1') -Parallelism $Parallelism
if ($LASTEXITCODE -ne 0) { throw 'Kumulatives v0.46-Gate fehlgeschlagen.' }

$build = Join-Path $root 'build-current'
$retail = @(& (Join-Path $build 'katana-phase11-retail-boot-services-tests.exe'))
if ($LASTEXITCODE -ne 0 -or $retail[-1] -ne 'KR_V046_RETAIL_BOOT_SERVICES_READY') {
    throw 'Synthetischer Retail-Boot-Service-Vertical-Slice fehlgeschlagen.'
}
$base = Get-Content (Join-Path $build 'reports\v045-gate.json') -Raw | ConvertFrom-Json
if ($base.status -ne 'success' -or $base.test_count -lt 168 -or $base.silent_failures -ne 0) {
    throw 'Kumulative v0.46-Gateevidenz ist unvollstaendig.'
}
$report = [ordered]@{
    schema = 'katana-v046-gate'
    version = 1
    status = 'success'
    marker = 'KR_V046_RETAIL_BOOT_SERVICES_READY'
    source_commit = (& $git -C $root rev-parse HEAD).Trim()
    configuration = $base.configuration
    fresh_build_directory = 'build-current'
    fresh_build_count = 1
    test_count = $base.test_count
    homebrew_marker = $base.homebrew_marker
    silent_failures = $base.silent_failures
    firmware_contract = 'katana-alpha-firmware-v1'
    bios_abi_contract = 'katana-bios-abi-v1'
    system_asic = 'synthetic-retail-vertical-slice'
    private_retail_data_in_gate = $false
    release = 'none'
    review_required = $true
    next_task_after_review = 'KR-4605'
}
$report | ConvertTo-Json -Depth 4 | Set-Content `
    (Join-Path $build 'reports\v046-gate.json') -Encoding utf8
Write-Output 'KR_V046_RETAIL_BOOT_SERVICES_READY review_required=true next_task=KR-4605'
