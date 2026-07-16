# Source-Coverage

Das Profil `coverage-debug` erbt Fuzzer und Sanitizer und verwendet weiterhin
ausschliesslich `build-current/`. GCC und Clang erhalten gcov-kompatible
Instrumentierung. MSVC wird nicht als instrumentierter Compiler ausgegeben,
sondern ueber das mit Visual Studio ausgelieferte dynamische native
Coveragewerkzeug beobachtet.

Nach Konfiguration und Build erzeugt der folgende Befehl den Bericht:

```text
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/quality/write-coverage-report.ps1
```

Der Runner prueft den kanonischen Buildpfad und den von CMake erzeugten
Backendvertrag, leert nur den verifizierten Unterordner
`build-current/coverage/` und fuehrt das vollstaendige CTest-Korpus aus. Unter
MSVC entsteht der Bericht direkt als Cobertura-XML. Fuer GCC/Clang muss `gcovr`
mit einem zum Compiler passenden gcov-Treiber im `PATH` liegen.

Die stabilen Ausgaben sind:

- `build-current/coverage/coverage.cobertura.xml`
- `build-current/coverage/coverage.log`

Fehlende Werkzeuge, ein fehlender Bericht, ein Testfehler oder ein anderes
Buildverzeichnis beenden den Lauf. Coverage ist eine Gate-Diagnose und kein
versioniertes Release-Artefakt. Das Profil wird erst gesammelt in KR-3709
gebaut und ausgefuehrt.
