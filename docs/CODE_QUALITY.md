# Formatierung und statische Analyse

`.clang-format` definiert den C++20-Stil fuer handgeschriebenen Code unter
`include/`, `src/`, `tests/` und `tools/`. Buildausgaben, exportierte Projekte
und sonstige generierte Artefakte liegen bewusst ausserhalb des Scopes. Die
Pruefung ist read-only:

```text
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/quality/check-format.ps1
```

Der Runner loest die Repositorywurzel kanonisch auf, sucht `clang-format`
zuerst im `PATH` und danach in der aktuellen Visual-Studio-Installation und
verwendet `--dry-run --Werror`. Er formatiert keine Datei automatisch.

Das Profil `quality-debug` erbt Coverage, Fuzzer und Sanitizer. Unter MSVC wird
`/analyze` ohne externe Header aktiviert; unter GCC/Clang ist `clang-tidy` mit
den in `.clang-tidy` festgelegten Bug-, Analyzer-, Performance- und
Portabilitaetspruefungen zwingend. Befunde werden als Fehler behandelt. Profil
und Formatpruefung laufen erstmals gesammelt in KR-3709.
