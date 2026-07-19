[CmdletBinding()]
param(
    [string]$SourceDirectory = "",
    [string]$ArtifactDirectory = "",
    [switch]$SelfTest
)

$ErrorActionPreference = "Stop"
$defaultRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot "..\.."))
$root = if ([string]::IsNullOrWhiteSpace($SourceDirectory)) {
    $defaultRoot
} else {
    [IO.Path]::GetFullPath($SourceDirectory)
}

function Test-RetailDiscPath {
    param([Parameter(Mandatory = $true)][string]$Path)
    return [IO.Path]::GetExtension($Path).Equals(
        '.katana-disc', [StringComparison]::OrdinalIgnoreCase)
}

if ($SelfTest) {
    if (-not (Test-RetailDiscPath 'content/game.katana-disc') -or
        -not (Test-RetailDiscPath 'CONTENT/GAME.KATANA-DISC') -or
        (Test-RetailDiscPath 'content/game.katana-disc.json')) {
        throw 'Retail-Content-Audit-Selbsttest erkennt Disc-Pack-Pfade nicht korrekt.'
    }
}

$violations = [Collections.Generic.List[string]]::new()
foreach ($file in Get-ChildItem -LiteralPath $root -Recurse -File -Filter '*.katana-disc' `
        -ErrorAction Stop) {
    $relative = $file.FullName.Substring($root.Length).TrimStart('\', '/')
    if (-not $relative.StartsWith('.git' + [IO.Path]::DirectorySeparatorChar,
            [StringComparison]::OrdinalIgnoreCase)) {
        $violations.Add("Repositorydatei: $relative")
    }
}

if (-not [string]::IsNullOrWhiteSpace($ArtifactDirectory)) {
    $artifactRoot = [IO.Path]::GetFullPath($ArtifactDirectory)
    if (Test-Path -LiteralPath $artifactRoot -PathType Container) {
        foreach ($file in Get-ChildItem -LiteralPath $artifactRoot -Recurse -File) {
            $relative = $file.FullName.Substring($artifactRoot.Length).TrimStart('\', '/')
            if (Test-RetailDiscPath $relative) {
                $violations.Add("Artefaktdatei: $relative")
            }
            if ($file.Extension.Equals('.zip', [StringComparison]::OrdinalIgnoreCase)) {
                Add-Type -AssemblyName System.IO.Compression
                Add-Type -AssemblyName System.IO.Compression.FileSystem
                $archive = [IO.Compression.ZipFile]::OpenRead($file.FullName)
                try {
                    foreach ($entry in $archive.Entries) {
                        if (Test-RetailDiscPath $entry.FullName) {
                            $violations.Add("Zip-Eintrag: $relative::$($entry.FullName)")
                        }
                    }
                }
                finally {
                    $archive.Dispose()
                }
            }
        }
    }
}

if ($violations.Count -ne 0) {
    throw "Vollstaendige Retail-Disc-Packs sind in Repository und verteilbaren Artefakten verboten:`n$($violations -join "`n")"
}

Write-Output "KR_RETAIL_CONTENT_AUDIT_SUCCESS self_test=$($SelfTest.IsPresent)"
