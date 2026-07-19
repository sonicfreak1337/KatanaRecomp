[CmdletBinding()]
param(
    [string]$BuildDirectory = "",
    [string]$SourceDirectory = ""
)

$ErrorActionPreference = "Stop"

$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot "..\.."))
$expectedBuild = [IO.Path]::GetFullPath((Join-Path $repositoryRoot "build-current"))
if ([string]::IsNullOrWhiteSpace($BuildDirectory)) {
    $BuildDirectory = $expectedBuild
}
if ([string]::IsNullOrWhiteSpace($SourceDirectory)) {
    $SourceDirectory = $repositoryRoot
}
$resolvedBuild = [IO.Path]::GetFullPath($BuildDirectory)
$resolvedSource = [IO.Path]::GetFullPath($SourceDirectory)
if (-not $resolvedBuild.Equals($expectedBuild, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Artefakte akzeptieren ausschliesslich build-current/."
}
if (-not $resolvedSource.Equals($repositoryRoot, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Artefakte akzeptieren ausschliesslich den KatanaRecomp-Quellbaum."
}
if (-not (Test-Path -LiteralPath $resolvedBuild -PathType Container)) {
    throw "build-current/ fehlt. Zuerst artifact-debug konfigurieren und bauen."
}
& (Join-Path $repositoryRoot 'tools\quality\audit-retail-content.ps1') `
    -SourceDirectory $resolvedSource -ArtifactDirectory (Join-Path $resolvedBuild 'artifacts')
if ($LASTEXITCODE -ne 0) {
    throw 'Retail-Content-Audit vor Artefakterzeugung fehlgeschlagen.'
}

$git = Get-Command git.exe -ErrorAction SilentlyContinue |
    Select-Object -First 1 -ExpandProperty Source
if (-not $git) {
    throw "git.exe wurde fuer den Artefakt-Provenienzvertrag nicht gefunden."
}
$dirty = @(& $git -C $resolvedSource status --porcelain=v1 --untracked-files=all)
if ($LASTEXITCODE -ne 0) {
    throw "Git-Status fuer den Artefaktvertrag ist fehlgeschlagen."
}
if ($dirty.Count -ne 0) {
    throw "Reproduzierbare Artefakte verlangen einen sauberen Git-Stand."
}
$commit = (& $git -C $resolvedSource rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or $commit -notmatch '^[0-9a-f]{40}$') {
    throw "Git-Commit fuer den Artefaktvertrag ist nicht aufloesbar."
}

$version = (Get-Content -LiteralPath (Join-Path $resolvedSource "VERSION") -Raw).Trim()
if ($version -notmatch '^\d+\.\d+\.\d+$') {
    throw "VERSION besitzt kein erwartetes semantisches Format."
}
$packageName = "KatanaRecomp-$version-dev.zip"

function Find-UniqueBinary {
    param([Parameter(Mandatory = $true)][string]$Name)
    $matches = @(Get-ChildItem -LiteralPath $resolvedBuild -Recurse -File | Where-Object {
        $_.Name.Equals($Name, [StringComparison]::OrdinalIgnoreCase)
    })
    if ($matches.Count -ne 1) {
        throw "Artefakt erwartet genau eine Binaerdatei '$Name', gefunden: $($matches.Count)."
    }
    return $matches[0].FullName
}

$executableSuffix = if ($IsWindows -or $env:OS -eq "Windows_NT") { ".exe" } else { "" }
$sourceEntries = @(
    [pscustomobject]@{ Path = "bin/katana-recomp$executableSuffix"; Source = (Find-UniqueBinary "katana-recomp$executableSuffix") },
    [pscustomobject]@{ Path = "bin/katana-fuzz$executableSuffix"; Source = (Find-UniqueBinary "katana-fuzz$executableSuffix") },
    [pscustomobject]@{ Path = "VERSION"; Source = (Join-Path $resolvedSource "VERSION") },
    [pscustomobject]@{ Path = "README.md"; Source = (Join-Path $resolvedSource "README.md") },
    [pscustomobject]@{ Path = "CHANGELOG.md"; Source = (Join-Path $resolvedSource "CHANGELOG.md") },
    [pscustomobject]@{ Path = "ROADMAP.md"; Source = (Join-Path $resolvedSource "ROADMAP.md") }
)

function Get-Sha256 {
    param([Parameter(Mandatory = $true)][byte[]]$Bytes)
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        return -join ($sha.ComputeHash($Bytes) | ForEach-Object { $_.ToString("x2") })
    }
    finally {
        $sha.Dispose()
    }
}

$entries = foreach ($entry in $sourceEntries) {
    $fullPath = [IO.Path]::GetFullPath($entry.Source)
    if (-not (Test-Path -LiteralPath $fullPath -PathType Leaf)) {
        throw "Artefakteingabe fehlt: $($entry.Path)."
    }
    $bytes = [IO.File]::ReadAllBytes($fullPath)
    [pscustomobject]@{
        Path = $entry.Path
        Bytes = $bytes
        Size = $bytes.LongLength
        Sha256 = Get-Sha256 $bytes
    }
}
$entries = @($entries | Sort-Object Path)

$manifestObject = [ordered]@{
    schema = "katana-pre-alpha-artifact"
    version = 1
    project_version = "$version-dev"
    source_commit = $commit
    configuration = "Debug"
    entries = @($entries | ForEach-Object {
        [ordered]@{ path = $_.Path; size = $_.Size; sha256 = $_.Sha256 }
    })
}
$manifestText = ($manifestObject | ConvertTo-Json -Depth 5) + "`n"
$manifestBytes = [Text.UTF8Encoding]::new($false).GetBytes($manifestText)
$entries += [pscustomobject]@{
    Path = "artifact-manifest.json"
    Bytes = $manifestBytes
    Size = $manifestBytes.LongLength
    Sha256 = Get-Sha256 $manifestBytes
}
$entries = @($entries | Sort-Object Path)

$outputDirectory = [IO.Path]::GetFullPath((Join-Path $resolvedBuild "artifacts"))
if (-not $outputDirectory.StartsWith(
        $resolvedBuild + [IO.Path]::DirectorySeparatorChar,
        [StringComparison]::OrdinalIgnoreCase
    )) {
    throw "Artefaktausgabe verlaesst build-current/."
}
if (Test-Path -LiteralPath $outputDirectory) {
    Remove-Item -LiteralPath $outputDirectory -Recurse -Force
}
New-Item -ItemType Directory -Path $outputDirectory | Out-Null

Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
function Write-DeterministicZip {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][object[]]$Items
    )
    $stream = [IO.File]::Open($Path, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write)
    try {
        $archive = [IO.Compression.ZipArchive]::new(
            $stream,
            [IO.Compression.ZipArchiveMode]::Create,
            $false
        )
        try {
            foreach ($item in $Items) {
                $zipEntry = $archive.CreateEntry(
                    $item.Path,
                    [IO.Compression.CompressionLevel]::NoCompression
                )
                $zipEntry.LastWriteTime = [DateTimeOffset]::new(
                    1980, 1, 1, 0, 0, 0, [TimeSpan]::Zero
                )
                $entryStream = $zipEntry.Open()
                try {
                    $entryStream.Write($item.Bytes, 0, $item.Bytes.Length)
                }
                finally {
                    $entryStream.Dispose()
                }
            }
        }
        finally {
            $archive.Dispose()
        }
    }
    finally {
        $stream.Dispose()
    }
}

$candidateA = Join-Path $outputDirectory "candidate-a.zip"
$candidateB = Join-Path $outputDirectory "candidate-b.zip"
Write-DeterministicZip -Path $candidateA -Items $entries
Write-DeterministicZip -Path $candidateB -Items $entries
$bytesA = [IO.File]::ReadAllBytes($candidateA)
$bytesB = [IO.File]::ReadAllBytes($candidateB)
$hashA = Get-Sha256 $bytesA
$hashB = Get-Sha256 $bytesB
if ($hashA -ne $hashB -or $bytesA.Length -ne $bytesB.Length) {
    throw "Doppelte Artefakterzeugung ist nicht bytegleich."
}

$packagePath = Join-Path $outputDirectory $packageName
Move-Item -LiteralPath $candidateA -Destination $packagePath
Remove-Item -LiteralPath $candidateB -Force
[IO.File]::WriteAllBytes(
    (Join-Path $outputDirectory "artifact-manifest.json"),
    $manifestBytes
)
$checksum = [Text.UTF8Encoding]::new($false).GetBytes("$hashA  $packageName`n")
[IO.File]::WriteAllBytes("$packagePath.sha256", $checksum)

Write-Output "Reproduzierbares Pre-Alpha-Artefakt: $packagePath"
Write-Output "SHA-256: $hashA"
