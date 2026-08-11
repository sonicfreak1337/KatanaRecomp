# Portbuildprofile und Cachevertrag

KatanaRecomp v0.49 trennt den schnellen Bring-up-Build vom finalen Gatebuild.

## Bring-up

`KATANA_PORT_BUILD_PROFILE=bringup` verwendet einen optimierten
`RelWithDebInfo`-Build ohne LTCG/IPO. Mit dem Microsoft-Linker bleiben
schnelle Debugsymbole und inkrementelles Linking aktiv; fuer LLD wird kein
inkrementeller Link versprochen. Nur das angeforderte Spieltarget wird gebaut.

## Gate

`KATANA_PORT_BUILD_PROFILE=gate` verlangt Interprocedural Optimization fuer
generierte AOT-Quellen und das Spielbinary. Wenn CMake/Hostcompiler IPO nicht
bestaetigen, scheitert Configure fail-closed. Das Gate bleibt
`RelWithDebInfo`; mit dem Microsoft-Linker erzwingt es
`/INCREMENTAL:NO`, `/OPT:REF` und `/OPT:ICF`.

## RuntimeOnly und Windows-Hostbuild

Der opt-in Portmodus `--analysis-mode runtime-only` ist nur fuer den
vollstaendigen NativeDisc-Produktport mit `--game-project` zulaessig; der
Default bleibt `platform`. RuntimeOnly verwendet fuer die Bootanalyse
`GuestCallAbi::Unknown`, erzeugt weiterhin nativen AOT-Code und bindet
RuntimeOnly-Dispatch an eine exakte statische Guest->Host-Tabelle. Der
Whole-Export-Cache ist an diesen Modus gebunden.

Der Windows-Hostwrapper uebergibt `_spawnvp`-Argumente CRT-konform, auch bei
Program-Paths mit Leerzeichen. Fuer Windows mit Ninja plant `vs_link_exe`
maximal zwei physische Linkpaesse. Ein fehlender optionaler Pass wird nur
durch vorhandenen erfolgreichen Build-, Artefakt- oder Up-to-date-Nachweis
geschlossen; mehr als zwei physische Paesse bleiben fail-closed.

Der aktuelle RuntimeOnly-Produktstand erreicht `FirstVisibleGameFrame` ohne
Skip oder kuenstlichen Moviepfad. Die identische Vergleichsreihe stieg von
`23,7959 MHz` ueber `24,1885 MHz` und `24,2825 MHz` auf `24,2926 MHz`; das
Native-AOT-Emissionsprofil ist `25` mit AOT-Partitionsschema `5`.

Der letzte Lauf brachte `341` Renderrequests/-completions/-frames, `15.680`
YUV-Makrobloecke sowie `470` Audiopuffer mit `345.450` Audiobildern. `100 MHz`
und das weitere Bring-up bis Memory-Card-Screen/Hauptmenue bleiben offen; der
serielle Runtime-/Dispatch-Overhead ist der aktuelle Performance-P0.

## Compiler und Linker

Unter Windows:

```text
KATANA_PORT_CXX_COMPILER=msvc|clang-cl
KATANA_PORT_LINKER=default|msvc|lld
KATANA_HOST_BUILD_GENERATOR=Ninja
KATANA_HOST_BUILD_MAKE_PROGRAM=<optionaler Ninja-Pfad>
KATANA_HOST_BUILD_JOBS=<Hostbuild-Worker>
KATANA_PORT_CODEGEN_JOBS=<Codegen-Worker>
```

Compiler, Linker, Profil und Generator besitzen getrennte Buildordner. Ein
fehlgeschlagenes Configure darf nur genau seinen abgeleiteten Buildordner
entfernen. MSVC-/clang-cl-`RelWithDebInfo` wird fail-closed abgelehnt, wenn
keine wirksame Optimierung konfiguriert ist.

`KATANA_HOST_BUILD_GENERATOR=Ninja` waehlt unter Windows explizit Ninja; ohne
diesen exakten Wert verwendet die CLI Visual Studio 2022. Ein nicht
standardmaessiger `KATANA_PORT_LINKER` benoetigt wegen
`CMAKE_LINKER_TYPE` mindestens CMake 3.29. Auf Nicht-Windows-Systemen verwendet
der CLI-Portbuild den nativen Compiler mit Ninja und lehnt die beiden
Windows-Toolchainvariablen ab.

## Runtimepaket

Ein direkt konfiguriertes generiertes Projekt kann das installierte
CMake-Paket `KatanaRecomp::runtime_core` per `find_package` verwenden. Eine
installierte CLI erkennt dieses Paket im gemeinsamen Installationspraefix
automatisch. `KATANA_RUNTIME_PREFIX=<Praefix>` waehlt explizit ein anderes
installiertes Paket; der Pfad muss dessen `include/katana/runtime` und
eine `KatanaRecompConfig.cmake` unter `lib/cmake/KatanaRecomp`,
`lib64/cmake/KatanaRecomp` oder `share/KatanaRecomp` enthalten.

`KATANA_RUNTIME_ROOT=<Quellbaum>` ist der explizite Fallback fuer die
Runtimeentwicklung. Ohne Paket und ohne explizite Variable verwendet eine aus
dem Repository gestartete CLI den erkannten Quellbaum. Dieser wird mit
`add_subdirectory(... EXCLUDE_FROM_ALL)` eingebunden und baut dadurch nicht
den gesamten Analyzer als normalen Portbestandteil. Ein neben der CLI
liegendes `runtime-sdk` darf entweder dieselbe installierte Paketstruktur oder
einen Quellbaum-SDK enthalten. Generierte AOT-TUs inkludieren die schmale
`katana/runtime/aot_runtime_abi.hpp` und verwenden eine gemeinsame PCH.
Der Produkt-Launcher erhaelt zusaetzlich den kleinen oeffentlichen
`katana/io/input_provenance.hpp`-Vertrag aus demselben Runtimepaket; weitere
Analyzerheader gehoeren nicht zum `runtime-sdk`.

`KATANA_RUNTIME_BUILD_TARGETS=<Buildtree>/KatanaRuntimeBuildTargets.cmake`
bindet stattdessen die bereits konfigurierte lokale Runtime und aktualisiert
deren Runtime-Target vor dem Portlink inkrementell. Der zugehoerige
`CMakeCache.txt` und das von CMake erzeugte
`KatanaRuntimeBuildProfile.txt` sind Teil dieser Bindung. Ein
Single-Config-Buildtree wird nur mit `RelWithDebInfo`, `Release` oder
`MinSizeRel` akzeptiert; `--config` kann einen als `Debug` konfigurierten
Ninja-/Make-Baum nicht nachtraeglich optimieren. Ein Multi-Config-Baum waehlt
in dieser Reihenfolge `RelWithDebInfo`, `Release`, dann `MinSizeRel`.
`KATANA_RUNTIME_PREFIX`, `KATANA_RUNTIME_BUILD_TARGETS` und
`KATANA_RUNTIME_ROOT` sind gegenseitig exklusiv.

Eine reine, ABI-kompatible Runtimeaenderung im verwendeten Quellbaum-SDK
erneuert Runtimeobjekte und den finalen Link. Solange die gebundene
Exporteridentitaet unveraendert bleibt, kann der verifizierte Whole-Export
dabei bestehen bleiben; ein neuer commitgebundener Toolbuild invalidiert
diesen aeusseren Cache bewusst. Der Whole-Export-Cache bindet
Eingabeartefakt, Exportoptionen, Git-/Toolidentitaet und die kanonischen
ABI-/Profilversionen. Deshalb muss jede ausgaberelevante inkompatible
Aenderung die passende Version erhoehen.
Partitions- und Metadatencaches besitzen zusaetzliche eigene Identitaeten.
Auf langen Hostpfaden bleiben die logischen SHA-256-Identitaeten vollstaendig;
nur das lokale physische Cachelayout verwendet kurze gehashte Komponenten, um
die Windows-Pfadgrenze nicht zu ueberschreiten.

## Was als Warmbuild gilt

Eine belastbare Warmbuildmessung:

1. verwendet dasselbe Boot-Executable-Artefakt und dieselben Exportoptionen;
2. laesst den lokalen Workspace und `user-data` bestehen;
3. aendert zwischen den beiden Laeufen keine Quelle;
4. misst den vollstaendigen realen Portexport samt Hostbuild;
5. berichtet Cachetreffer, Compiler, Linker und Profil.

Ein synthetischer Compilerbenchmark ersetzt diese Messung nicht.

## Reale v0.49-Messung

Der executable-first Produktport wurde mit demselben Eingabeartefakt,
Bring-up-Profil, Ninja-Generator und Microsoft-Linker gebaut. Der zweite
identische Export dauerte mit MSVC 2,258 Sekunden und mit clang-cl 2,297
Sekunden. Die EXE-Groessen betrugen 67.062.272 und 69.149.696 Bytes.

Bei exakt 600.000.000 Gastzyklen erreichte MSVC 40,3869 MHz in 14,8563
Sekunden, clang-cl 42,4662 MHz in 14,1289 Sekunden. clang-cl war damit 5,15
Prozent schneller, erzeugte aber eine 3,11 Prozent groessere EXE. Beide
Produktlaeufe hatten identische 52.329.316 zentrale Dispatches und keinen
neuen technischen Fehler. Der erste Compilerbuild ist nicht symmetrisch
vergleichbar: Beim MSVC-Lauf waren Analyse und Codeemission kalt, beim
clang-cl-Lauf traf der Whole-Export-Cache bereits.
