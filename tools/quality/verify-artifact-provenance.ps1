[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Archive,
    [Parameter(Mandatory = $true)][string]$GitRef,
    [string]$ExpectedSha256 = ""
)

$ErrorActionPreference = "Stop"
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot "..\.."))
$archivePath = [IO.Path]::GetFullPath($Archive)
if (-not (Test-Path -LiteralPath $archivePath -PathType Leaf)) {
    throw "Zu pruefendes Artefakt fehlt: $archivePath"
}

$git = Get-Command git.exe -ErrorAction Stop | Select-Object -First 1 -ExpandProperty Source
$tagCommit = (& $git -C $root rev-parse "$GitRef^{}").Trim()
if ($LASTEXITCODE -ne 0 -or $tagCommit -notmatch '^[0-9a-f]{40}$') {
    throw "Git-Referenz ist nicht auf einen Commit aufloesbar: $GitRef"
}

Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [IO.Compression.ZipFile]::OpenRead($archivePath)
try {
    $manifestEntry = $zip.GetEntry("artifact-manifest.json")
    if (-not $manifestEntry) {
        throw "Artefakt enthaelt kein artifact-manifest.json."
    }
    $reader = [IO.StreamReader]::new($manifestEntry.Open(), [Text.Encoding]::UTF8)
    try {
        $manifest = $reader.ReadToEnd() | ConvertFrom-Json
    }
    finally {
        $reader.Dispose()
    }
}
finally {
    $zip.Dispose()
}

if ($manifest.source_commit -ne $tagCommit) {
    throw "Artefaktcommit $($manifest.source_commit) stimmt nicht mit $GitRef ($tagCommit) ueberein."
}
if (-not [string]::IsNullOrWhiteSpace($ExpectedSha256)) {
    $actualHash = (Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actualHash -ne $ExpectedSha256.ToLowerInvariant()) {
        throw "Artefakthash $actualHash stimmt nicht mit dem erwarteten SHA-256 ueberein."
    }
}

Write-Output "KR_ARTIFACT_PROVENANCE_SUCCESS ref=$GitRef commit=$tagCommit"
