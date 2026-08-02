# KatanaRecomp

Aktuelle Pre-Alpha-Version: `0.49.0`

Aktueller Implementierungsstand: KR-4979; verbindlicher P0-Planstand:
`ffd45ae`. Der eingecheckte Source traegt Runtime-ABI 87, Analyzer-ABI 25
und Portprojektvertrag 75. Der private NativeDisc-v24-Export wurde nach
etwa `3 h 27 min` waehrend der dritten vollstaendigen Function-Value-
Neuberechnung beendet. Er erzeugte kein Portartefakt, keine `game.exe`,
keinen Sonic-Lauf und keinen neuen Screenshot. Der Checkpoint ist kein
P0-Abschluss: Kaltbuild-Performance, abschliessende Gesamtpruefung und
Produktabnahme bleiben offen. Der verbindliche Fahrplan steht in
[`docs/P0_NATIVE_DISC_COLD_BUILD_PERFORMANCE.md`](docs/P0_NATIVE_DISC_COLD_BUILD_PERFORMANCE.md).

```text
Runtime-ABI:                    87
Block-ABI:                       5
Analyzer-ABI:                   25
PlatformServices-ABI:           13
Backend-Interface-ABI:          12
Portprojektvertrag:             75
Native-AOT-Emissionsprofil:     13
AOT-Partitionsschema:            5
```

KatanaRecomp ist ein unabhaengiges C++20-Framework fuer die statische
Rekompilierung von Dreamcast-SH-4-Programmen:

```text
Dreamcast-Programm
  -> SH-4-Analyse
  -> Katana-IR
  -> natives C++
  -> Hostcompiler
  -> natives Spielprojekt
```

Der normale Produktpfad enthaelt keinen allgemeinen SH-4-Interpreter, keinen
JIT und keinen Emulationsfallback. Nicht vorab gebundener Code sowie unbekannte
Hardwarewirkungen scheitern sichtbar und typisiert. Ein begrenzter
Diagnoseinterpreter ist nur in einem ausdruecklich als `diagnostic_partial`
exportierten Diagnoseport vorhanden.

BIOS-Dateien, Disc-Images, Boot-Executables, kommerzielle Assets und aus
Retailspielen erzeugte Quellen gehoeren nicht in dieses Repository oder in
verteilbare Portpakete.

## Architektur

v0.49 trennt drei Verantwortungsbereiche:

1. **KatanaRecomp** analysiert SH-4, erzeugt IR, optimiert und emittiert
   statische native C++-Quellen.
2. **KatanaRuntime** stellt die titelunabhaengigen Dreamcast-Grenzen fuer CPU,
   Speicher, MMU, Scheduler, Interrupts, BIOS, GD-ROM, PVR/TA, AICA, Maple,
   Video, Audio und Eingabe bereit.
3. **Externe Spielprojekte** duerfen hashgebundene Funktionsgrenzen,
   Jump-/Callbacktabellen, Runtimecode-Templates, native Overrides,
   Mid-Function-Hooks, Symbole und Direct-Boot-Konfiguration enthalten.

Titeladressen, Discidentitaeten, private Symbole und Spielpatches bleiben
ausserhalb des generischen Katana-Kerns.

Details: [v0.49-Architektur](docs/ARCHITECTURE_V049.md)

## AOT-Ausfuehrung

Der Static AOT Fast Tier bildet kanonische Codepages und Halfword-Offsets ueber
eine kompakte zweistufige Tabelle direkt auf validierte native
Funktionszeiger ab. Der Dispatcher fuehrt den bereits aufgeloesten
Ausfuehrungsdeskriptor aus; ein zweites Tabellenlookup ist nicht erforderlich.

Der Dynamic AOT Tier bleibt fuer Runtimecode, Overlays, Module, MMU-Varianten,
Relocationen, Materialisierung und Invalidierung verantwortlich. P1-/P2-Ziele
verwenden einen fruehen callsitegebundenen Inline-Cache mit vollstaendigen
Generationguards.

Function-Level-AOT fasst analysierte Gastfunktionen zu nativen Funktionen mit
internen Labels zusammen. Bewiesene Calls koennen direkt zwischen AOT-Funktionen
wechseln; ein Host-Stackwaechter wickelt tiefe Gastrekursion sicher zum
statischen Dispatcher ab. Die planbasierte Registerlokalisierung haelt
ausgewaehlte GPRs sowie T, PR, GBR, MACH, MACL und FPUL ueber native
Funktionsregionen lokal und gibt sie an Architekturgrenzen explizit frei
beziehungsweise laedt sie danach neu. FPU-Registerarrays bleiben bewusst
ausserhalb dieses Vertrags. Direkte Haupt-RAM-Zugriffe und lokalisierte
Register bleiben konservativ an Watchpoint-, Trace-, MMIO-, Exception-,
Interrupt-, SR-/Bank- und Invalidierungsgrenzen gebunden.

## Executable-First-Entwicklung

Die private `.gdi` wird fuer Bring-up nicht mehr bei jeder Iteration analysiert.
Sie dient einmalig zur Extraktion:

```powershell
.\build-current\katana-recomp.exe extract-boot-executable `
  D:\eigene-disc\game.gdi `
  --output D:\private\game-boot
```

Anschliessend arbeiten Analyse, Codegen und Warmbuild mit dem unveraenderlichen
Executable-Artefakt:

```powershell
$env:KATANA_PORT_BUILD_PROFILE = 'bringup'
$env:KATANA_HOST_BUILD_GENERATOR = 'Ninja'
$env:KATANA_PORT_CXX_COMPILER = 'msvc'
$env:KATANA_HOST_BUILD_JOBS = '8'
$env:KATANA_PORT_CODEGEN_JOBS = '8'

.\build-current\katana-recomp.exe port-executable `
  D:\private\game-boot\boot.katana-executable `
  --output D:\private\ports\game-direct `
  --target-name GameDirect `
  --console-profile europe-pal `
  --game-project D:\private\game-project.katana-game-project `
  --game-entry-handoff D:\private\game-boot\game-entry.katana-handoff
```

Das Artefakt enthaelt lokal `boot.bin`; diese Datei ist Retailinhalt und darf
nicht verteilt werden. Der erzeugte Port enthaelt nur AOT-Code, Metadaten und
Hash-/Installationsvertraege.

Der vollstaendige Discpfad bleibt erhalten:

```powershell
.\build-current\katana-recomp.exe port `
  D:\eigene-disc\game.gdi `
  --output D:\private\ports\game-disc `
  --target-name GameDisc `
  --console-profile europe-pal
```

`DirectBootExecutable` ist der executable-first Entwicklungspfad. Ein
bewiesener Spieleinstieg benoetigt dabei einen titel- und
Executable-identitaetsgebundenen `GameEntryHandoff` aus dem externen
Spielprojekt. Der aktuelle Handoff-Vertrag verwendet Schema 3,
Handoff-Artefaktformat 2 und Plattformzustandsvertrag 2; der dokumentierte
Source-Checkpoint `18f8537` verwendet Runtime-ABI 85 und
Portprojektvertrag 75. Davon getrennt verwendet `GameProject` Vertrag 5 und
Artefaktformat 4. `CompletePlatform` erfasst und restauriert den kanonischen
Satz aus 22 Dreamcast-Geraeten einschliesslich Flash sowie die exakte
typisierte Scheduler-Timeline. Capture und Apply sind nur im historischen
Produktport belegt. Der naechste private NativeDisc-Lauf ist erst nach
KR-4974 bis KR-4984 und der dort verlangten Gesamtpruefung zulaessig.

`GameProjectArtifact` Format 4 transportiert fuer Spielprojektvertrag 5 die deklarativen,
hashgebundenen Spielprojektdaten ueber die CLI. Dazu gehoeren exakte
Funktionsgrenzen, Jump-/Callbacktabellen, Runtime-AOT-Templates, Symbole,
Codeidentitaeten und Direct-Boot-Konfiguration. Native Hookzeiger und private
Handoff-Payloads werden nicht serialisiert. Die vollstaendige Definition
steuert Analyse und AOT; ohne native Hooks bindet der erzeugte Port zur
Laufzeit nur Identitaet, Bootkonfiguration und Handoff.

Fuer den schmalen artifact-only Entwicklungsweg bindet
`port-executable --game-project ... --game-entry-handoff ...` beide privaten
Artefakte bereits beim Export an Executable-, Konsolen-, Projekt- und
Descriptoridentitaet. Die Artefakte werden nicht in den Port kopiert. Der
erzeugte Produktport laedt das Handoff lokal ueber
`KATANA_GAME_ENTRY_HANDOFF_PRODUCT`, prueft dieselbe Bindung erneut und wendet
den vollstaendigen Plattformzustand vor dem ersten Spielblock an.
`NativeDiscBoot` kompiliert weiterhin den disc-eigenen Bootstrap und bleibt
Referenz- und finales Genauigkeitsgate sowie Grundlage der
Nutzerinstallation. Beide Pfade verwenden dieselbe Dreamcast-Runtime; keiner
interpretiert SH-4.

Vollstaendiger Vertrag:
[Executable-First-Entwicklung](docs/EXECUTABLE_FIRST_DEVELOPMENT.md)

## Nutzerinstallation

Eine verteilte Port-EXE enthaelt keine Discsektoren. Jeder Nutzer installiert
seine eigene passende Originaldisc einmalig:

```powershell
.\GameDirect.exe --install-disc D:\eigene-disc\game.gdi
.\GameDirect.exe
```

Der Installer validiert Descriptor, Tracks, Geometrie, Hashes,
Contentidentitaet und Bootdatei, bevor er atomar
`user-data/content/game.katana-disc` anlegt. Dieser lokale Cache bleibt bei
Warmbuilds und Republishing erhalten und darf nicht weitergegeben werden.

## Bauen

Voraussetzungen:

- CMake 3.25 oder neuer;
- C++20-Compiler;
- Ninja oder ein anderer von CMake unterstuetzter Generator.

Windows/MSVC:

```powershell
cmake --preset msvc-relwithdebinfo
cmake --build --preset msvc-relwithdebinfo --target katana-recomp
```

Das installierbare CMake-Ziel lautet `KatanaRecomp::runtime_core`. Ein direkt
konfiguriertes Portprojekt kann es per `find_package` verwenden. Die
installierte CLI erkennt das Runtimepaket im gemeinsamen Installationspraefix
automatisch; `KATANA_RUNTIME_PREFIX` waehlt ein anderes installiertes Paket.
`KATANA_RUNTIME_BUILD_TARGETS` kann den
`KatanaRuntimeBuildTargets.cmake`-Export eines lokalen Buildtrees direkt
binden. Single-Config-Baeume muessen als `RelWithDebInfo`, `Release` oder
`MinSizeRel` konfiguriert sein; bei Multi-Config-Baeumen baut die CLI eine
vorhandene optimierte Konfiguration.
`KATANA_RUNTIME_ROOT` bleibt der explizite Quellbaum-Fallback und wird
`EXCLUDE_FROM_ALL` eingebunden. Generierte AOT-TUs verwenden die schmale
`katana/runtime/aot_runtime_abi.hpp` sowie eine PCH.

Profile und Toolchainauswahl:
[Portbuildprofile](docs/PORT_BUILD_PROFILES.md)

## Produkt-Gate

Bootkorrektheit und Performance sind getrennte Ergebnisse. Ein Produktlauf
wird nicht mehr anhand eines festen Drei-Sekunden-Hostlimits bewertet. Das
Budget bezeichnet im aktuellen Quellvertrag die ab Game-Entry auszufuehrende
Gastarbeit:

```powershell
$env:KATANA_GUEST_CYCLE_BUDGET = '600000000'
$env:KATANA_PORT_FINAL_PROGRESS = '1'
$env:KATANA_GAME_ENTRY_HANDOFF_PRODUCT = 'D:\private\game-boot\game-entry.katana-handoff'
.\GameDirect.exe
```

`KR-4966` berechnet daraus
`target_cycle = restored_game_entry_cycle + requested_post_entry_cycles` und
berichtet Restore-, Final- und ausgefuehrte Post-Entry-Zyklen getrennt. Bei
angefordertem Produktbudget ist Exitcode 0 nur mit vollstaendiger Gastarbeit,
erreichtem Pflichtmeilenstein und echtem `KATANA_PRODUCT_GATE` zulaessig. Die
Zusammenfassung nennt Gastzyklen, Hostzeit, Dispatches, technische
Framemarker und das erste neue AOT-, Runtime- oder Geraeteproblem; ein
sichtbarer Bildschirm wird separat anhand einer realen Ausgabeaufnahme
klassifiziert. Dieser Vertrag ist im Source-Checkpoint `18f8537`
implementiert, aber noch nicht mit einem aktuellen Sonic-Port abgenommen.
Vor dem naechsten privaten Produktlauf stehen KR-4974 bis KR-4984.

Die **historische v24-`CompletePlatform`-Vergleichsbasis** endete in beiden Pfaden bei
Schedulerzyklus 600.000.000 ohne erstes neues AOT-, Runtime- oder
Geraeteproblem:

- `NativeDiscBoot`: 6,3161 Sekunden, 94,9954 effektive Gast-MHz,
  17.080.114 zentrale Dispatches und ein sichtbarer IP.BIN-Frame;
- `DirectBootExecutable`: Restore bei 415.233.270, danach 184.766.730
  Post-Entry-Zyklen in 5,01505 Sekunden, also 36,8425 MHz ueber die
  tatsaechlich ausgefuehrte Gastarbeit, 16.033.676 zentrale Dispatches und
  noch kein sichtbarer Frame.

Der Direct-Port meldete aus dem absoluten Zaehler 119,64 MHz; dieser Wert ist
kein gueltiger Performancevergleich. Seine 16.033.676
Dispatches entsprechen 11,52 Post-Entry-Gastzyklen pro Zentraldispatch und
belegen noch keinen Hotpathgewinn.

Der **historische v30-DirectBoot** verwendet weiterhin das externe,
hashgebundene `GameProjectArtifact`. Die darin privat beschriebene exakte
Funktionsgrenze wird durch Analyzer, CFG, IR und AOT transportiert; dadurch
passiert der Produktlauf den bisherigen Blocker aus KR-4971. Der echte Lauf
endet bei Gastzyklus `553.990.562`, also nach `138.757.292`
Post-Entry-Zyklen und `10.079.932` Zentraldispatches. Gegen v26 sind das
`+1.086.915` Gastzyklen. Der historische v28-Fehler-zu-Fehler-Vergleich mass
dieselben `138.757.292` Post-Entry-Zyklen in 5,275792 Sekunden, also
26,3008 MHz gegen 23,9578 MHz bei v26 und provisorisch `+9,78 %`. Der
v30-Sichtlauf ist kein kontrollierter Performancebenchmark; wegen des
vorzeitigen Fehlers gibt es weiterhin keine 600-Millionen-Abnahme.

Sein erster Blocker war KR-4972:
`0x8C11088C -> 0x8C64784E`. Das unveraenderte Ziel beginnt mit einem Sprung
auf einen gemeinsamen Codepfad. Die generische Analyse verfolgt den
Callback jetzt ueber begrenzte Tail-Jump- und Runtime-Frame-Pfade, erkennt
`0x8C64784E` als Funktion und erreicht `0x8C6478C2` als gemeinsamen Body.
Der aktuelle Quellstand transportiert solche bewachten AOT-Einstiege durch
CFG, Source-Map und AOT und erzwingt ihre Exportvollstaendigkeit. Der
aktuelle Produktnachweis steht weiterhin aus und darf erst nach dem
KR-4974-bis-KR-4984-Gate erfolgen.
Die terminale Diagnose unterscheidet diesen Fall jetzt korrekt als
`aot-template-mismatch` von echten Byteidentitaetsfehlern.

Sound-/G2- und technische PVR-Evidenz bleiben erhalten: alle G2-Kanaele sind
inaktiv, zwei Direct-Frames enthalten 302.287 geaenderte Pixel. Der
v30-Sichtlauf bestaetigt mit 15 realen Aufnahmen erneut einen vollstaendig
schwarzen Hostscreen; der Host-Presenter meldet null Frames. Der frische
v30-Gateexport erzeugt 1.959 Funktionen in 42 Partitionen und eine
52.616.192 Byte grosse MSVC-EXE. Sein kalter Export ist nicht mit dem
frueheren warmen v28-Export von 4,209083 Sekunden vergleichbar. Das
200-MHz-Ziel, das relative Gate und ein sichtbarer
DirectBoot-Spielbildnachweis bleiben offen.

Der **historische Runtime-ABI-64-/NativeDisc-v32-Pfad** behebt zwei allgemeine
Sichtluecken: Flag-Poll-Batching ist unter aktiver MMU wieder fail-closed,
und ein gueltiger PVR-VBlank-Scanout wird unabhaengig von einem einmaligen
Diagnoseproof praesentiert. `port <gdi>` akzeptiert nun ausserdem dieselbe
hashgebundene Option `--game-project` wie `port-executable`.

Der kanonische, frisch exportierte und ueber die private Original-GDI
installierte v32-MSVC-Port zeigt ab 2,032 Sekunden den Sega-Lizenzscreen und
praesentiert 127 Hostframes. Er endet bei
Gastzyklus 553.990.562 und damit exakt wie DirectBoot-v30 an
`0x8C11088C -> 0x8C64784E`; NativeDisc ist sichtbar, DirectBoot-v30 blieb am
gleichen Punkt schwarz. Der Lauf dauerte 6,701 Sekunden, fuehrte
11.080.283 Zentraldispatches aus, erreichte 82,67 MHz bis zum Fehler und
erzeugte eine 53.677.056 Byte grosse EXE. Er erreichte wegen KR-4972 nicht
das 600-Millionen-Performancegate. Diese v32-Zahlen bleiben historische
Vergleichsevidenz und sind kein Nachweis fuer den aktuellen
Source-Checkpoint.

## Diagnose

- **Produkt-Performance:** feste Aggregatzaehler; RuntimeOnly-Sitedetails und
  deren Map bleiben im normalen Produktlauf deaktiviert.
- **Crash Capsule:** ein fester allokationsfreier POD-Ring haelt letzten Block,
  MMIO, Schedulerereignis und ersten Fehler; Strings entstehen erst terminal.
- **Fehlerdiagnose:** der bestehende begrenzte Dispatchrecorder wird nur bei
  expliziter tiefer Diagnose an den Dispatcher gebunden.
- **Tiefe Diagnose:** Wait-Loop-Rohwerttrace und vollstaendige
  Dispatchereignisse sind explizite lokale Opt-ins.

Ein automatisch durch Gastzyklus/PC/Fehler begrenztes Triggerfenster ist in
v0.49 noch kein abgeschlossener oeffentlicher Runtimevertrag.

Private Pfade, Identitaeten, Gastbytes und Titeladressen gehoeren nicht in
oeffentliche Berichte.

## Repository

```text
include/   oeffentliche Analyzer-, Codegen- und Runtimevertraege
src/       Implementierungen
tests/     bestehende enge Vertrags- und Regressionstests
docs/      Architektur-, Runtime-, Build- und Sicherheitsdokumentation
cmake/     Paket- und ABI-Versionierung
```

Wichtige Dokumente:

- [Roadmap](ROADMAP.md)
- [v0.49-Architektur](docs/ARCHITECTURE_V049.md)
- [Executable-First-Entwicklung](docs/EXECUTABLE_FIRST_DEVELOPMENT.md)
- [Portbuildprofile](docs/PORT_BUILD_PROFILES.md)
- [Portexport und Originaldisc](docs/PORT_EXPORT.md)
- [Runtime-Vertrauensvertrag](docs/PORT_RUNTIME_TRUST_CONTRACT.md)
- [Runtime](docs/RUNTIME.md)
- [Indirect Control Flow](docs/INDIRECT_CONTROL_FLOW.md)
- [Sonic-Acceptancevertrag](docs/SONIC_ADVENTURE_ACCEPTANCE.md)
- [v0.49.0-Releasehinweise](docs/releases/v0.49.0.md)

## Rechtlicher Rahmen

KatanaRecomp wird unabhaengig entwickelt. Es enthaelt keinen Flycast-,
dcrecomp- oder sonstigen uebernommenen Emulatorcode. Externe Referenzen muessen
den Provenienz- und Lizenzregeln in
[REFERENCE_PROVENANCE.md](docs/REFERENCE_PROVENANCE.md) entsprechen.

Dreamcast und zugehoerige Marken sind Eigentum ihrer jeweiligen Rechteinhaber.
Dieses Projekt liefert keine Spiele, Firmware oder urheberrechtlich
geschuetzten Assets.
