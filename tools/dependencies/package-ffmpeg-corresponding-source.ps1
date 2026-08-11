param(
    [Parameter(Mandatory = $true)][string]$FFmpegSourceArchive,
    [Parameter(Mandatory = $true)][string]$BtbNBuildRecipeArchive,
    [Parameter(Mandatory = $true)][string]$BtbNSourceCacheArtifact,
    [Parameter(Mandatory = $true)][string]$OutputArchive
)

$ErrorActionPreference = 'Stop'

$expected = @{
    ffmpeg = '7e779215eae16ad7e93ddad59bd82822bd3d34e4dc61f9996f9481b2c0605bc3'
    recipe = '7deac4a5190b2be84d4d548db2885d05152f9e3d77069d0e34841a46efd95e2b'
    cache = 'dd9e5cf1278e07532ffd2cb32f50d252394f1757cb4fc34a899f81395e153a0b'
}

function Resolve-SourceFile([string]$Path, [string]$ExpectedHash, [string]$Label) {
    $resolved = (Resolve-Path -LiteralPath $Path).Path
    $item = Get-Item -LiteralPath $resolved -Force
    if (-not $item.PSIsContainer -and
        (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) -and
        $item.Length -gt 0) {
        $actual = (Get-FileHash -LiteralPath $resolved -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($actual -eq $ExpectedHash) {
            return $item
        }
        throw "$Label has the wrong SHA-256: $actual"
    }
    throw "$Label must be a non-empty regular non-reparse file"
}

$ffmpeg = Resolve-SourceFile $FFmpegSourceArchive $expected.ffmpeg 'FFmpeg source archive'
$recipe = Resolve-SourceFile $BtbNBuildRecipeArchive $expected.recipe 'BtbN build-recipe archive'
$cache = Resolve-SourceFile $BtbNSourceCacheArtifact $expected.cache 'BtbN source-cache artifact'

$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$configurationPath = Join-Path $repositoryRoot 'third_party\ffmpeg\BUILD-CONFIGURATION.txt'
$noticePath = Join-Path $repositoryRoot 'third_party\ffmpeg\NOTICE.txt'
$configuration = Resolve-SourceFile $configurationPath `
    ((Get-FileHash -LiteralPath $configurationPath -Algorithm SHA256).Hash.ToLowerInvariant()) `
    'FFmpeg build configuration'
$notice = Resolve-SourceFile $noticePath `
    ((Get-FileHash -LiteralPath $noticePath -Algorithm SHA256).Hash.ToLowerInvariant()) `
    'FFmpeg notice'

$output = [IO.Path]::GetFullPath($OutputArchive)
if (-not $output.EndsWith('.zip', [StringComparison]::OrdinalIgnoreCase)) {
    throw 'OutputArchive must use the .zip extension'
}
if (Test-Path -LiteralPath $output) {
    throw "OutputArchive already exists: $output"
}
$parent = Split-Path -Parent $output
[IO.Directory]::CreateDirectory($parent) | Out-Null
$temporary = "$output.partial-$([Guid]::NewGuid().ToString('N'))"
$timestamp = [DateTimeOffset]::new(2026, 7, 31, 0, 0, 0, [TimeSpan]::Zero)

Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem

function Add-FileEntry(
    [IO.Compression.ZipArchive]$Zip,
    [IO.FileInfo]$Source,
    [string]$EntryName
) {
    $entry = $Zip.CreateEntry($EntryName, [IO.Compression.CompressionLevel]::NoCompression)
    $entry.LastWriteTime = $timestamp
    $input = $null
    $target = $null
    try {
        $input = $Source.OpenRead()
        $target = $entry.Open()
        $input.CopyTo($target, 4MB)
    } finally {
        if ($null -ne $target) {
            $target.Dispose()
        }
        if ($null -ne $input) {
            $input.Dispose()
        }
    }
}

function Add-TextEntry(
    [IO.Compression.ZipArchive]$Zip,
    [string]$EntryName,
    [string]$Content
) {
    $entry = $Zip.CreateEntry($EntryName, [IO.Compression.CompressionLevel]::Optimal)
    $entry.LastWriteTime = $timestamp
    $stream = $null
    $writer = $null
    try {
        $stream = $entry.Open()
        $writer = [IO.StreamWriter]::new($stream, [Text.UTF8Encoding]::new($false))
        $writer.Write($Content)
    } finally {
        if ($null -ne $writer) {
            $writer.Dispose()
            $stream = $null
        }
        if ($null -ne $stream) {
            $stream.Dispose()
        }
    }
}

$stream = $null
$zip = $null
try {
    $stream = [IO.File]::Open($temporary, [IO.FileMode]::CreateNew, [IO.FileAccess]::ReadWrite,
                              [IO.FileShare]::None)
    $zip = [IO.Compression.ZipArchive]::new($stream, [IO.Compression.ZipArchiveMode]::Create, $true)
    try {
        Add-FileEntry $zip $ffmpeg 'sources/ffmpeg-9b6c8969e05b4f0b29f0f85cd501be6b3e582e6b.tar.gz'
        Add-FileEntry $zip $recipe 'build-recipe/FFmpeg-Builds-a99e8230eae00d1cee38f23076a7a1f55cd984e2.tar.gz'
        Add-FileEntry $zip $cache 'sources/BtbN-download-cache-workflow-30631423920.zip'
        Add-FileEntry $zip $configuration 'BUILD-CONFIGURATION.txt'
        Add-FileEntry $zip $notice 'NOTICE.txt'
        Add-TextEntry $zip 'changes.diff' ''
        $manifest = [ordered]@{
            schema = 'katana-ffmpeg-corresponding-source'
            version = 1
            binary_archive_sha256 = 'c222a490dde4e7059f45495deef6bfb98dbcacc2b43df5b607546252037aa95c'
            ffmpeg_commit = '9b6c8969e05b4f0b29f0f85cd501be6b3e582e6b'
            ffmpeg_source_archive_sha256 = $expected.ffmpeg
            btbn_recipe_commit = 'a99e8230eae00d1cee38f23076a7a1f55cd984e2'
            btbn_recipe_archive_sha256 = $expected.recipe
            btbn_workflow_run = 30631423920
            btbn_source_cache_artifact = 8793574986
            btbn_source_cache_artifact_sha256 = $expected.cache
            source_changes = 'none'
        } | ConvertTo-Json -Depth 3
        Add-TextEntry $zip 'manifest.json' ($manifest + "`n")
    } finally {
        if ($null -ne $zip) {
            $zip.Dispose()
            $zip = $null
        }
        if ($null -ne $stream) {
            $stream.Dispose()
            $stream = $null
        }
    }
    Move-Item -LiteralPath $temporary -Destination $output
} catch {
    if ($null -ne $zip) {
        $zip.Dispose()
        $zip = $null
    }
    if ($null -ne $stream) {
        $stream.Dispose()
        $stream = $null
    }
    if (Test-Path -LiteralPath $temporary) {
        Remove-Item -LiteralPath $temporary -Force
    }
    throw
}

$bundleHash = (Get-FileHash -LiteralPath $output -Algorithm SHA256).Hash.ToLowerInvariant()
Write-Output "FFmpeg corresponding-source bundle: $output"
Write-Output "SHA-256: $bundleHash"
Write-Output 'Configure with:'
Write-Output "  -DKATANA_FFMPEG_CORRESPONDING_SOURCE_ARCHIVE=$output"
Write-Output "  -DKATANA_FFMPEG_CORRESPONDING_SOURCE_SHA256=$bundleHash"
