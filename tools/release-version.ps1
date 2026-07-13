param(
    [Parameter(Mandatory = $true)]
    [string]$Version,

    [switch]$Tag,
    [switch]$Push
)

$ErrorActionPreference = "Stop"

if ($Version -notmatch '^\d+\.\d+\.\d+$') {
    throw "Version muss dem Format MAJOR.MINOR.PATCH entsprechen."
}

$projekt = (Get-Location).Path
$cmakeDatei = Join-Path $projekt "CMakeLists.txt"
$versionDatei = Join-Path $projekt "VERSION"

if (-not (Test-Path $cmakeDatei)) {
    throw "CMakeLists.txt wurde nicht gefunden."
}

$cmake = [System.IO.File]::ReadAllText($cmakeDatei)

$muster = '(?ms)(project\s*\(\s*KatanaRecomp\s*)VERSION\s+\d+\.\d+\.\d+'

if ($cmake -notmatch $muster) {
    throw "Projektversion wurde in CMakeLists.txt nicht gefunden."
}

$cmake = [regex]::Replace(
    $cmake,
    $muster,
    '${1}VERSION ' + $Version,
    1
)

[System.IO.File]::WriteAllText(
    $cmakeDatei,
    $cmake,
    [System.Text.Encoding]::ASCII
)

[System.IO.File]::WriteAllText(
    $versionDatei,
    $Version + [Environment]::NewLine,
    [System.Text.Encoding]::ASCII
)

Write-Host "Version auf $Version gesetzt." -ForegroundColor Green
Write-Host "CHANGELOG.md vor dem Tagging manuell aktualisieren."

if ($Tag) {
    if (-not (Test-Path ".git")) {
        throw "Kein Git-Repository gefunden."
    }

    $tagName = "v$Version"

    git add VERSION CMakeLists.txt CHANGELOG.md README.md

    git diff --cached --quiet

    if ($LASTEXITCODE -ne 0) {
        git commit -m "Release $tagName vorbereiten"
    }

    $vorhanden = git tag --list $tagName

    if (-not $vorhanden) {
        git tag -a $tagName -m "KatanaRecomp $tagName"
        Write-Host "Git-Tag $tagName wurde erstellt." -ForegroundColor Green
    } else {
        Write-Host "Git-Tag $tagName existiert bereits."
    }

    if ($Push) {
        git push
        git push origin $tagName
    }
}