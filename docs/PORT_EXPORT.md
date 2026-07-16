# Port-Projektexport

Der offizielle Exportpfad erzeugt aus einer validierten GDI ein getrenntes,
reproduzierbares Portprojekt und baut das Hosttarget unmittelbar im Debugprofil:

```powershell
katana-recomp port .\disc\game.gdi --output .\port --target-name game
```

Die GDI wird ueber Bootmetadaten und ISO9660 bis zur Dreamcast-Bootdatei
gelesen. Danach folgen Executable Image, Kontrollflussanalyse, Katana-IR,
Optimierung und deterministische Translation-Unit-Partitionierung. Der Export
kopiert weder GDI-Tracks noch allgemeine Spielassets.

## Layout

```text
port/
  .gitignore                  ignoriert den getrennten Hostbuild
  CMakeLists.txt              einmalig angelegter Nutzer-Bootstrap
  src/main.cpp                einmalig angelegte Integrationsschicht
  generated/
    .katana-generated-artifacts
    CMakeLists.txt
    build.ninja
    compile_commands.json
    katana-port.cmake
    code/unit-*.cpp
    include/katana_port.hpp
    metadata/port-project.json
    metadata/provenance.json
    metadata/source-map.json
```

Nur Dateien im Katana-Artefaktmanifest unter `generated/` werden bei einer
erneuten Generierung ersetzt oder als veraltet entfernt. Unbekannte Dateien und
insbesondere `src/` bleiben erhalten. Symbolische Links in verwalteten Pfaden
werden abgelehnt.

## Hostbuild und Runtimevertrag

Der CLI-Aufruf bindet KatanaRecomp fuer den lokalen Debugbuild ueber den
expliziten CMake-Parameter
`KATANA_RUNTIME_ROOT` ein. Die generierten Quellen pruefen Runtime-ABI 8 beim
Kompilieren; portable Dateien enthalten keinen absoluten lokalen Quellpfad. Der
Build liegt getrennt unter `port/build/`. Konfigurations- oder Buildfehler enden
mit dem stabilen CLI-Exitcode `7` (`build-failure`). Die folgenden Befehle zeigen
den entsprechenden manuellen Wiederholungsweg:

```powershell
cmake -S .\port -B .\port\build -G Ninja `
  -DCMAKE_BUILD_TYPE=Debug `
  -DKATANA_RUNTIME_ROOT=<KatanaRecomp-Quellbaum>
cmake --build .\port\build --target game
```

Das Hostprogramm akzeptiert zur lokalen Validierung `--gdi <Quelle>`. Diese
Anbindung verwendet die austauschbare `DiscSource`-Grenze. Ein Folgeprojekt kann
`src/main.cpp` durch eine eigene DiscSource- und Assetintegration ersetzen,
ohne generierten Spielcode zu aendern. KatanaRecomp verspricht ohne diese
Folgeintegration nicht, dass die GDI nach dem Export geloescht werden kann.

Metadaten und Provenienz enthalten nur Zielkennung, ABI-/Partitionsdaten,
Groessen und SHA-256-Werte. Absolute GDI-, Track-, Build- und Hostpfade werden
nicht serialisiert.
