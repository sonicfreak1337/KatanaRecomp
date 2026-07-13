param(
    [string]$TasksFile = ".\docs\TASKS.md"
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path $TasksFile)) {
    throw "Task-Datei nicht gefunden: $TasksFile"
}

$lines = Get-Content $TasksFile

for ($index = 0; $index -lt $lines.Count; ++$index) {
    if ($lines[$index] -match '^### \[ \] (KR-\d+) - (.+)$') {
        $taskId = $Matches[1]
        $title = $Matches[2]

        Write-Host "$taskId - $title" -ForegroundColor Cyan

        $end = [Math]::Min($index + 18, $lines.Count - 1)

        for ($detail = $index + 1; $detail -le $end; ++$detail) {
            if ($lines[$detail] -match '^### ') {
                break
            }

            Write-Host $lines[$detail]
        }

        exit 0
    }
}

Write-Host "Keine offenen Tasks gefunden." -ForegroundColor Green