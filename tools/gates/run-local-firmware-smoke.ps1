[CmdletBinding()]
param(
    [switch]$Enable,
    [string]$Bios = "",
    [string]$BiosSha256 = "",
    [string]$Flash = "",
    [string]$FlashSha256 = ""
)

$ErrorActionPreference = "Stop"

if (-not $Enable) {
    Write-Output "KR_PHASE9_FIRMWARE_SMOKE_NOT_REQUESTED"
    exit 0
}

if ([string]::IsNullOrWhiteSpace($Bios) -or
    [string]::IsNullOrWhiteSpace($BiosSha256) -or
    [string]::IsNullOrWhiteSpace($Flash) -or
    [string]::IsNullOrWhiteSpace($FlashSha256)) {
    throw "Lokaler Firmware-Smoke braucht BIOS, Flash und beide erwarteten SHA-256-Werte."
}

throw "KR_PHASE9_FIRMWARE_SMOKE_UNAVAILABLE: LLE wird vor KR-4601 nicht unterstuetzt; keine Datei wurde gelesen."
