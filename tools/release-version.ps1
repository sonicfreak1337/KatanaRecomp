param(
    [Parameter(Mandatory = $true)]
    [string]$Version,

    [switch]$Tag,
    [switch]$Push
)

$ErrorActionPreference = "Stop"
$git = Get-Command git -ErrorAction Stop | Select-Object -First 1 -ExpandProperty Source

function Invoke-GitChecked {
    param([Parameter(Mandatory = $true)][string[]]$GitArguments)

    $commandOutput = @(& $git @GitArguments)
    $exitCode = $LASTEXITCODE
    if ($exitCode -ne 0) {
        throw "git $($GitArguments -join ' ') ist mit Exitcode $exitCode fehlgeschlagen."
    }
    return ($commandOutput -join [Environment]::NewLine)
}

if ($Version -notmatch '^[0-9]+\.[0-9]+\.[0-9]+$') {
    throw "Version muss dem Format MAJOR.MINOR.PATCH entsprechen."
}

$projekt = (Get-Location).Path
$versionDatei = Join-Path $projekt "VERSION"

if (-not (Test-Path -LiteralPath $versionDatei -PathType Leaf)) {
    throw "VERSION wurde nicht gefunden."
}

$aktuelleVersion = [System.IO.File]::ReadAllText($versionDatei).Trim()
if ($aktuelleVersion -notmatch '^[0-9]+\.[0-9]+\.[0-9]+$') {
    throw "VERSION enthaelt keine gueltige kanonische Projektversion."
}

$tempVersion = Join-Path $projekt (".VERSION." + [Guid]::NewGuid().ToString("N") + ".tmp")
try {
    [System.IO.File]::WriteAllText(
        $tempVersion,
        $Version + [Environment]::NewLine,
        [System.Text.Encoding]::ASCII
    )
    [System.IO.File]::Replace($tempVersion, $versionDatei, $null)
}
finally {
    if (Test-Path -LiteralPath $tempVersion) {
        Remove-Item -LiteralPath $tempVersion -Force
    }
}

Write-Host "Version auf $Version gesetzt." -ForegroundColor Green
Write-Host "CHANGELOG.md vor dem Tagging manuell aktualisieren."

if ($Tag) {
    if (-not (Test-Path ".git")) {
        throw "Kein Git-Repository gefunden."
    }

    $tagName = "v$Version"

    [void](Invoke-GitChecked @("add", "VERSION", "CHANGELOG.md", "README.md"))

    & $git diff --cached --quiet
    $diffExitCode = $LASTEXITCODE

    if ($diffExitCode -eq 1) {
        [void](Invoke-GitChecked @("commit", "-m", "Release $tagName vorbereiten"))
    } elseif ($diffExitCode -ne 0) {
        throw "git diff --cached --quiet ist mit Exitcode $diffExitCode fehlgeschlagen."
    }

    $headCommit = (Invoke-GitChecked @("rev-parse", "HEAD")).Trim()
    if ($headCommit -notmatch '^[0-9a-f]{40}$') {
        throw "HEAD ist nicht auf einen kanonischen Git-Commit aufloesbar."
    }

    & $git show-ref --verify --quiet "refs/tags/$tagName"
    $tagLookupExitCode = $LASTEXITCODE
    if ($tagLookupExitCode -eq 1) {
        [void](Invoke-GitChecked @("tag", "-a", $tagName, "-m", "KatanaRecomp $tagName"))
        Write-Host "Git-Tag $tagName wurde erstellt." -ForegroundColor Green
    } elseif ($tagLookupExitCode -eq 0) {
        $tagCommit = (Invoke-GitChecked @("rev-parse", "$tagName^{}" )).Trim()
        if ($tagCommit -ne $headCommit) {
            throw "Git-Tag $tagName zeigt auf $tagCommit statt auf HEAD $headCommit."
        }
        Write-Host "Git-Tag $tagName existiert bereits."
    } else {
        throw "Git-Tag $tagName konnte nicht sicher abgefragt werden."
    }

    if ($Push) {
        [void](Invoke-GitChecked @("push"))
        [void](Invoke-GitChecked @("push", "origin", $tagName))
    }
}
