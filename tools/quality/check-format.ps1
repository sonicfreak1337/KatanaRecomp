[CmdletBinding()]
param(
    [string]$SourceDirectory = ""
)

$ErrorActionPreference = "Stop"

$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot "..\.."))
if ([string]::IsNullOrWhiteSpace($SourceDirectory)) {
    $SourceDirectory = $repositoryRoot
}
$resolvedSource = [IO.Path]::GetFullPath($SourceDirectory)
if (-not $resolvedSource.Equals($repositoryRoot, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Formatpruefung akzeptiert ausschliesslich den KatanaRecomp-Quellbaum."
}

$clangFormat = Get-Command clang-format -ErrorAction SilentlyContinue |
    Select-Object -First 1 -ExpandProperty Source
if (-not $clangFormat) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path -LiteralPath $vswhere) {
        $installation = (& $vswhere -latest -products * -property installationPath | Select-Object -First 1)
        if ($installation) {
            $candidates = @(
                (Join-Path $installation "VC\Tools\Llvm\x64\bin\clang-format.exe"),
                (Join-Path $installation "VC\Tools\Llvm\bin\clang-format.exe")
            )
            $clangFormat = $candidates | Where-Object {
                Test-Path -LiteralPath $_ -PathType Leaf
            } | Select-Object -First 1
        }
    }
}
if (-not $clangFormat) {
    throw "clang-format wurde weder im PATH noch in Visual Studio gefunden."
}

$roots = @("include", "src", "tests", "tools")
$extensions = @(".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx")
$files = foreach ($root in $roots) {
    $path = Join-Path $resolvedSource $root
    if (Test-Path -LiteralPath $path -PathType Container) {
        Get-ChildItem -LiteralPath $path -Recurse -File | Where-Object {
            $extensions -contains $_.Extension.ToLowerInvariant()
        } | Select-Object -ExpandProperty FullName
    }
}
$files = @($files | Sort-Object -Unique)
if ($files.Count -eq 0) {
    throw "Formatpruefung hat keine C++-Dateien gefunden."
}

for ($offset = 0; $offset -lt $files.Count; $offset += 64) {
    $last = [Math]::Min($offset + 63, $files.Count - 1)
    $batch = $files[$offset..$last]
    & $clangFormat --style=file --dry-run --Werror @batch
    if ($LASTEXITCODE -ne 0) {
        throw "clang-format meldet Abweichungen im handgeschriebenen C++-Code."
    }
}

Write-Output "Formatpruefung erfolgreich: $($files.Count) C++-Dateien."
