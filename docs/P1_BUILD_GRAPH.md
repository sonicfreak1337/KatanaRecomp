# P1-Buildgraph, Pakete und Testmatrix

Stand: KR-4625

## Zielgraph

Der Standardbuild umfasst Core und CLI; die Desktop-GUI ist ein ausdruecklicher
Opt-in ueber `KATANA_BUILD_DESKTOP_GUI=ON`. Alle lokalen Presets verwenden gemaess
Build-Vertrag ausschliesslich `build-current/` und werden beim Wechsel von
Compiler oder Profil frisch konfiguriert.

```text
KatanaRecomp::runtime  (Runtime plus Provenienzgrundlage, runtime-sdk)
          ^
          |
KatanaRecomp::analyzer (Decoder, IO, Plattformanalyse, CFG, IR, Codegen)
          ^
          |
     katana-recomp      (CLI-Werkzeug)
```

Ein Port darf `find_package(KatanaRecomp CONFIG REQUIRED)` und
`KatanaRecomp::runtime` verwenden, ohne Analyzerquellen zu kompilieren. Das
`runtime-sdk` enthaelt nur Runtimeheader, den generierten Buildvertrag, die
Runtimebibliothek und das CMake-Paket. `analyzer-sdk` ergaenzt
`KatanaRecomp::analyzer` und alle Analyseheader. Ein Out-of-Tree-Consumer wird
als `katana-package-contract-tests` installiert, konfiguriert und gebaut.

## Versionen

`VERSION` ist die kanonische Projekt- und Paketversion.
`cmake/KatanaVersions.cmake` validiert sie und ist die einzige Quelle fuer
Runtime-, Block-, Plattformdienst-, Backend- und Port-ABI-Werte. CMake erzeugt
daraus `katana/build_contract.hpp` und die Packagevariablen. Inkompatible
Vertraege werden damit bereits beim Konfigurieren beziehungsweise durch
`static_assert` sichtbar.

## Compiler- und Profilmatrix

Die Presets `msvc-*`, `gcc-*` und `clang-*` decken jeweils `Debug` und
`RelWithDebInfo` ab. Die GitHub-Matrix baut und testet alle sechs Kombinationen
mit ausgeschalteter GUI. Damit bleibt insbesondere die Releaseoptimierung ein
dauerhafter Regressionspfad. `core-debug` und `core-relwithdebinfo` verwenden den
jeweiligen Hostcompiler; `gui-debug` ist der explizite GUI-Opt-in.

Ein eigener Windows-Buildgate-Job fuehrt auf jedem Push nach `main` und in jedem
Pull Request zusaetzlich das lokale Core-Correctness-Gate unveraendert aus. Der
maschinenlesbare Bericht `core-correctness-gate.json` und die CTest-Protokolle
werden als GitHub-Actions-Artifact veroeffentlicht. Die sechs Matrixjobs legen
ihre JUnit- und CTest-Berichte ebenfalls als getrennte Artifacts ab. Damit sind
Commit, Testinventar und beide frischen Gatekonfigurationen ausserhalb des
lokalen `build-current/` nachvollziehbar.

## Cache und Testshards

`KATANA_COMPILER_CACHE` nimmt einen absoluten Pfad oder einen Programmnamen wie
`ccache`/`sccache` an. CI trennt den Cache nach Compiler und Profil, meldet
Cachetreffer und schreibt Build- sowie Testdauer in die Jobzusammenfassung.

Alle Tests tragen `gate` und genau ein primaeres Subsystemlabel. Lokale
Testpresets stellen `runtime-shard`, `analysis-shard`, `codegen-shard` und
`performance-shard` bereit; nicht speziell zugeordnete Tests bleiben `core`.
Der vollstaendige Gate-Lauf verwendet weiterhin alle Tests.

## Baseline und Budgets

Die letzte gruen gepruefte Ausgangsbasis KR-4618 umfasst 171 Quality-Debug- und
170 RelWithDebInfo-Tests. Frische Gesamt-, Shard-, Portbuild- und Cachezeiten
werden in KR-4625 unter identischer Parallelitaet protokolliert. Regressionen
werden gegen folgende Regeln bewertet:

- kein Test- oder Portbuild darf sein bestehendes Timeout erreichen;
- Debug und RelWithDebInfo muessen dieselben 170 gemeinsamen Gastregressionen
  bestehen;
- Cache an/aus darf Artefakte und Testergebnisse nicht veraendern;
- eine Optimierung bleibt nur aktiv, wenn ihr instrumentierter Vergleichstest
  identische Resultate und weniger Hotpath-Arbeit nachweist.

## KR-4625-Gateergebnis

Das frische Windows-Gate bestand 168 Quality-Debug- und 167
RelWithDebInfo-Tests. Der zusaetzliche Debugtest prueft die ausgelieferte
MSVC-ASan-Runtime; beide Profile teilen damit exakt 167 Core-Regressionen.
Buildparallelitaet 8 vermeidet ungebremste Windows-Linkkonkurrenz und behaelt
fuer transiente Dateisperren begrenzte Wiederholungen bei. Format-,
Qualitaetsvertrags-, Referenz- und Lizenzaudit bestanden; private Retaildaten
wurden nicht verwendet. Die exakten Laufzeiten und der Quellcommit stehen im
maschinenlesbaren Gatebericht.
