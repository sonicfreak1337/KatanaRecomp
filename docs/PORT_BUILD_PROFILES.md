# Portbuildprofile und Cachevertrag

> v0.49.1: `native-port` ist seit KR-5000 das einzige Produkt-/Releaseprofil.
> Die nachfolgende RuntimeOnly-Beschreibung bleibt historische Bring-up-
> Referenz. Ein Produktartifact darf weder ARM7-/SkyEmu-, CPU-PVR- noch
> Diagnoseinterpretercode linken und nicht auf diese Pfade zurueckfallen.

KatanaRecomp v0.49 trennt den schnellen Bring-up-Build vom finalen Gatebuild.

## Bring-up

`KATANA_PORT_BUILD_PROFILE=bringup` verwendet standardmaessig `Release`, aber
kompiliert ausschliesslich die sehr grossen generierten AOT-Einheiten mit
`/Od /Ob0`. Runtime, Titeladapter und Produktbootstrap bleiben normal
optimiert. Das ist der schnelle Iterationsbuild; das voll optimierte Produkt
ist weiterhin das Gateprofil.

MSVC-AOT-Compiles schreiben keine gemeinsame Program Database. Ein expliziter
`RelWithDebInfo`-Bring-up verwendet eingebettete `/Z7`-Informationen statt
`/Zi` plus `/FS`. Ninja begrenzt nur den schweren `katana_generated`-Pool auf
standardmaessig vier Compiler, waehrend Runtime, Adapter, Tools und Link den
vollen Hostworker-Vertrag behalten. `KATANA_AOT_COMPILE_JOBS` kann bei einem
direkt konfigurierten Projekt zwischen `1` und `KATANA_HOST_COMPILE_JOBS`
gesetzt werden. Native Dispatchquellen enthalten 4.096 statt 512 Eintraege
pro Shard, damit ein Vollspielport nicht Hunderte triviale Compilerstarts und
Linkobjekte bezahlt.

Die Auswahl beruht auf dem realen 12C/24T-Sonic-Vollport: vier identische
schwere AOT-Einheiten brauchten mit vier Workern und `/O1` `16,542 s`, mit
`/Od` nur `4,560 s`; zwoelf `/O1`-Worker verschlechterten den Durchsatz auf
`67,456 s` fuer zwoelf Einheiten. Der alte `/O2 + /Zi + /FS`-Pfad lag bei
median `148,606 s` pro grosser Einheit und war kein zulaessiger
Bring-up-Standard. Der darauffolgende kalte Vollport kompilierte 233 Host-TUs,
bestand den Post-Link-Audit und beendete den kompletten Export in `408,278 s`;
der eigentliche Hostbuild benoetigte `337,205 s`. Das erzeugte Produktbinary
ist `473.506.304` Byte gross.

## Gate

`KATANA_PORT_BUILD_PROFILE=gate` verlangt Interprocedural Optimization fuer
generierte AOT-Quellen und das Spielbinary. Wenn CMake/Hostcompiler IPO nicht
bestaetigen, scheitert Configure fail-closed. Das Gate bleibt
`RelWithDebInfo`; mit dem Microsoft-Linker erzwingt es
`/INCREMENTAL:NO`, `/OPT:REF` und `/OPT:ICF`.

## Runtimeprofil und Produktlinkgrenze

`KATANA_PORT_RUNTIME_PROFILE` trennt die Runtimeauswahl vom Compilerprofil:

```text
native-port                Standard und einziger Produktpfad
diagnostic-interpreter     nur fuer Diagnoseexporte
```

Normale Exporte akzeptieren ausschliesslich `native-port`; Diagnoseexporte
erzwingen `diagnostic-interpreter`. Der historische Geraetepfad ist nur ein
internes Buildbaum-Orakel und kein generiertes Portprofil. Compilerbuildordner
und Telemetrie sind an das Runtimeprofil gebunden. `native-port` linkt
ausschliesslich `KatanaRecomp::native_port_runtime`, verlangt Native-Port-
Profilvertrag `10` und Portprojektvertrag `86` und erzeugt eine Linkmap. Ein
Post-Link-Audit verwirft das Binary bei Legacy-Runtime-, ARM7-/SkyEmu-,
CPU-PVR-/TA- oder Interpreterbestandteilen. Es existiert kein automatischer
Rueckfall.

KR-5000 stellt die Produktlinkgrenze, NativePort-Artefakte, read-only
Content-Mappings und den validierenden Runner bereit. Der Produktlink oder
Produktlauf darf bei unvollstaendiger Hook-/Hardware-Closure typisiert
fail-closed enden; der explizite Bring-up-Schalter ist darauf begrenzt.

KR-5002 fuehrt die installierbare Media-Closure ein: WinMM PCM und ein
in-process LGPL-Shared-FFmpeg/libav-Provider ohne `ffmpeg.exe`. Der Provider
prueft Header, ABI und Lizenz der fuenf benoetigten DLLs und deployt nur diese
Closure samt Notices; ein User-Override wird nicht veraendert, der automatische
Cache wird auf die benoetigte Closure begrenzt.

KR-5003 bindet den hardware-only-D3D11-GPU-Pfad ohne WARP/REF/GDI/CPU-
Rasterizer, PVR/TA oder historische Geraeteruntime. Render-/Outputaufloesung
und Game-/UI-/Kamera-Viewports sind getrennte Vertraege; Standard ist
1920x1080. Oeffentliche FFmpeg-Pakete benoetigen die vollstaendige
entsprechende Source-Closure und bleiben ohne sie `redistribution_ready=false`.

KR-5004 fuehrt native Datei-, Eingabe- und Save-Dienste ein. Read-only-
Content-Ranges sind SHA-256-identitaetsgebunden; XInput bietet vier Gamepads,
und Saves sind atomar projekt-/slot-/schema-gebunden mit Backup-Recovery.

## Historischer RuntimeOnly-Pfad und Windows-Hostbuild

Der historische Portmodus `--analysis-mode runtime-only` war nur mit
`--game-project` zulaessig und bleibt jetzt ausschliesslich internes
Diagnoseorakel. RuntimeOnly verwendet fuer die Bootanalyse
`GuestCallAbi::Unknown`, erzeugt weiterhin nativen AOT-Code und bindet
RuntimeOnly-Dispatch an eine exakte statische Guest->Host-Tabelle. Der
Whole-Export-Cache ist an diesen Modus gebunden.

Der Windows-Hostwrapper uebergibt `_spawnvp`-Argumente CRT-konform, auch bei
Program-Paths mit Leerzeichen. Fuer Windows mit Ninja plant `vs_link_exe`
maximal zwei physische Linkpaesse. Ein fehlender optionaler Pass wird nur
durch vorhandenen erfolgreichen Build-, Artefakt- oder Up-to-date-Nachweis
geschlossen; mehr als zwei physische Paesse bleiben fail-closed.

Der historische RuntimeOnly-Produktstand erreicht `FirstVisibleGameFrame` ohne
Skip oder kuenstlichen Moviepfad. Die identische Vergleichsreihe stieg von
`23,7959 MHz` ueber `24,1885 MHz` und `24,2825 MHz` auf `24,2926 MHz`; das
Native-AOT-Emissionsprofil ist `27` mit AOT-Partitionsschema `7`.

Der letzte Lauf brachte `341` Renderrequests/-completions/-frames, `15.680`
YUV-Makrobloecke sowie `470` Audiopuffer mit `345.450` Audiobildern. `100 MHz`
und das weitere Bring-up bis Memory-Card-Screen/Hauptmenue blieben offen. Die
Messung ist keine Abnahmebasis des nativen Produktpfads.

## Compiler und Linker

Unter Windows:

```text
KATANA_PORT_CXX_COMPILER=msvc|clang-cl
KATANA_PORT_LINKER=default|msvc|lld
KATANA_HOST_BUILD_GENERATOR=Ninja
KATANA_HOST_BUILD_MAKE_PROGRAM=<optionaler Ninja-Pfad>
KATANA_HOST_BUILD_JOBS=<Hostbuild-Worker>
KATANA_PORT_CODEGEN_JOBS=<Codegen-Worker>
KATANA_AOT_COMPILE_JOBS=<direkter CMake-Override fuer schwere AOT-TUs>
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

Ein direkt konfiguriertes generiertes Produktprojekt verwendet aus dem
installierten CMake-Paket standardmaessig
`KatanaRecomp::native_port_runtime`. `KatanaRecomp::runtime` und
`KatanaRecomp::runtime_core` werden nicht mehr installiert oder exportiert.
Eine
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
einen Quellbaum-SDK enthalten. Generierte AOT-TUs verwenden die explizite
Produktheader-Allowlist und eine gemeinsame PCH. Der alte
`katana/runtime/aot_runtime_abi.hpp`-Vertrag bleibt bis zur direkten
KR-5001-Hook-/Bootstrap-Emission eine Buildbaum-Diagnoseschnittstelle und wird
nicht im Produkt-SDK installiert; deshalb ist der Produktconfigure in diesem
Zwischenstand absichtlich gesperrt. Analyzer-, Fortschritts-,
Input-Provenienz- und historische Geraeteheader gehoeren nicht zum
`runtime-sdk`.

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
