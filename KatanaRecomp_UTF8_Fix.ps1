$ErrorActionPreference = "Stop"

$projekt = (Get-Location).Path
$mainDatei = Join-Path $projekt "src\cli\main.cpp"
$cmakeDatei = Join-Path $projekt "CMakeLists.txt"

if (-not (Test-Path $mainDatei)) {
    throw "src\cli\main.cpp wurde nicht gefunden. Starte das Skript im KatanaRecomp-Projektordner."
}

if (-not (Test-Path $cmakeDatei)) {
    throw "CMakeLists.txt wurde nicht gefunden. Starte das Skript im KatanaRecomp-Projektordner."
}

$zeit = Get-Date -Format "yyyyMMdd_HHmmss"
$backup = Join-Path $projekt ".katana_backup_utf8_$zeit"

New-Item -ItemType Directory -Force (Join-Path $backup "src\cli") | Out-Null

Copy-Item $mainDatei (Join-Path $backup "src\cli\main.cpp")
Copy-Item $cmakeDatei (Join-Path $backup "CMakeLists.txt")

$main = Get-Content $mainDatei -Raw

if ($main -notmatch '#include <windows\.h>') {
    $marker = "namespace {"

    if ($main -notmatch [regex]::Escape($marker)) {
        throw "Einfügepunkt für den Windows-Header wurde in main.cpp nicht gefunden."
    }

    $windowsInclude = @'
#ifdef _WIN32
#include <windows.h>
#endif

'@

    $main = $main.Replace($marker, $windowsInclude + $marker)
}

if ($main -notmatch 'SetConsoleOutputCP\(CP_UTF8\)') {
    $pattern = 'int main\(const int argc, char\* argv\[\]\) \{\s*try \{'

    if ($main -notmatch $pattern) {
        throw "Die main()-Funktion wurde nicht im erwarteten Format gefunden."
    }

    $replacement = @'
int main(const int argc, char* argv[]) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif

    try {
'@

    $main = [regex]::Replace(
        $main,
        $pattern,
        $replacement,
        [System.Text.RegularExpressions.RegexOptions]::Singleline
    )
}

Set-Content $mainDatei $main -Encoding utf8

$cmake = Get-Content $cmakeDatei -Raw

if ($cmake -notmatch '/utf-8') {
    $pattern = '(?m)^(\s*)/EHsc\s*$'

    if ($cmake -notmatch $pattern) {
        throw "Der MSVC-Schalter /EHsc wurde in CMakeLists.txt nicht gefunden."
    }

    $cmake = [regex]::Replace(
        $cmake,
        $pattern,
        '${1}/EHsc' + [Environment]::NewLine + '${1}/utf-8',
        1
    )
}

Set-Content $cmakeDatei $cmake -Encoding utf8

Write-Host ""
Write-Host "UTF-8-Konsolenfix wurde angewendet." -ForegroundColor Green
Write-Host "Sicherung: $backup"
Write-Host ""

if (Test-Path ".\build") {
    Write-Host "Build wird aktualisiert..."
    cmake --build build

    Write-Host ""
    Write-Host "Tests werden ausgeführt..."
    ctest --test-dir build --output-on-failure
} else {
    Write-Host "Kein build-Ordner gefunden. Konfiguriere und baue jetzt..."

    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
    cmake --build build
    ctest --test-dir build --output-on-failure
}

Write-Host ""
Write-Host "Fertig. Teste anschließend:" -ForegroundColor Cyan
Write-Host '  .\build\katana-recomp.exe disasm ".\samples\control_flow_demo.bin" 8C010000'
