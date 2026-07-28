# KatanaRecomp

Aktuelle Pre-Alpha-Version: `0.49.0`

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
statischen Dispatcher ab. Die aktuelle Registerlokalisierung ist bewusst auf
ausgewaehlte GPRs reiner Leaf-Funktionen begrenzt. Direkte Haupt-RAM-Zugriffe
und lokalisierte Register bleiben konservativ an Watchpoint-, Trace-, MMIO-,
Exception-, Interrupt-, SR-/Bank- und Invalidierungsgrenzen gebunden.

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
Spielprojekt. Der aktuelle Vertrag verwendet Handoff-Schema 3,
Artefaktformat 2, Runtime-ABI 63, Portprojektvertrag 53 und
Plattformzustandsvertrag 2. `CompletePlatform` erfasst und restauriert den
kanonischen Satz aus 22 Dreamcast-Geraeten einschliesslich Flash sowie die
exakte typisierte Scheduler-Timeline. Capture und Apply sind im realen
Produktport belegt.

`GameProjectArtifact` Format 1 transportiert die deklarativen,
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
`KATANA_RUNTIME_ROOT` bleibt der explizite Quellbaum-Fallback und wird
`EXCLUDE_FROM_ALL` eingebunden. Generierte AOT-TUs verwenden die schmale
`katana/runtime/aot_runtime_abi.hpp` sowie eine PCH.

Profile und Toolchainauswahl:
[Portbuildprofile](docs/PORT_BUILD_PROFILES.md)

## Produkt-Gate

Bootkorrektheit und Performance sind getrennte Ergebnisse. Ein Produktlauf
wird nicht mehr anhand eines festen Drei-Sekunden-Hostlimits bewertet. Der
aktuelle Schalter setzt noch ein absolutes Schedulermaximum:

```powershell
$env:KATANA_GUEST_CYCLE_BUDGET = '600000000'
$env:KATANA_PORT_FINAL_PROGRESS = '1'
$env:KATANA_GAME_ENTRY_HANDOFF_PRODUCT = 'D:\private\game-boot\game-entry.katana-handoff'
.\GameDirect.exe
```

Ohne restaurierten Scheduler ist das ein 600-Millionen-Gastzyklus-Gate. Nach
einem Game-Entry-Handoff muss `KR-4966` daraus noch eine Laufdauer relativ zum
restaurierten Entry-Zyklus machen. Bis dahin darf die terminal aus dem
absoluten Zaehler berechnete MHz-Zahl eines Handoff-Laufs nicht mit einem
Lauf ab Zyklus null verglichen werden. Die Zusammenfassung nennt Gastzyklen,
Hostzeit, Dispatches, technische Framemarker und das erste neue AOT-,
Runtime- oder Geraeteproblem; ein sichtbarer Bildschirm wird separat anhand
einer realen Ausgabeaufnahme klassifiziert.

Die v24-`CompletePlatform`-Vergleichsbasis endete in beiden Pfaden bei
Schedulerzyklus 600.000.000 ohne erstes neues AOT-, Runtime- oder
Geraeteproblem:

- `NativeDiscBoot`: 6,3161 Sekunden, 94,9954 effektive Gast-MHz,
  17.080.114 zentrale Dispatches und ein sichtbarer IP.BIN-Frame;
- `DirectBootExecutable`: Restore bei 415.233.270, danach 184.766.730
  Post-Entry-Zyklen in 5,01505 Sekunden, also 36,8425 MHz ueber die
  tatsaechlich ausgefuehrte Gastarbeit, 16.033.676 zentrale Dispatches und
  noch kein sichtbarer Frame.

Der Direct-Port meldete aus dem absoluten Zaehler 119,64 MHz; dieser Wert ist
bis `KR-4966` kein gueltiger Performancevergleich. Seine 16.033.676
Dispatches entsprechen 11,52 Post-Entry-Gastzyklen pro Zentraldispatch und
belegen noch keinen Hotpathgewinn.

Der aktuelle v28-DirectBoot verwendet ein externes,
hashgebundenes `GameProjectArtifact`. Die darin privat beschriebene exakte
Funktionsgrenze wird durch Analyzer, CFG, IR und AOT transportiert; dadurch
passiert der Produktlauf den bisherigen Blocker aus KR-4971. Der echte Lauf
endet bei Gastzyklus `553.990.562`, also nach `138.757.292`
Post-Entry-Zyklen und `10.079.932` Zentraldispatches. Gegen v26 sind das
`+1.086.915` Gastzyklen. Die `138.757.292` tatsaechlichen
Post-Entry-Zyklen in 5,275792 Sekunden ergeben 26,3008 MHz gegen 23,9578 MHz
bei v26, also provisorisch `+9,78 %`; das ist wegen des vorzeitigen Fehlers
keine 600-Millionen-Performanceabnahme.

Der neue erste Blocker KR-4972 ist
`0x8C11088C -> 0x8C64784E`. Das unveraenderte Ziel beginnt mit einem Sprung
auf einen gemeinsamen Codepfad und benoetigt eine bewiesene
Callback-/Shared-Tail-/Thunk-Modellierung; seine Grenze wird nicht geraten.
Die terminale Diagnose unterscheidet diesen Fall jetzt korrekt als
`aot-template-mismatch` von echten Byteidentitaetsfehlern.

Sound-/G2- und technische PVR-Evidenz bleiben erhalten: alle G2-Kanaele sind
inaktiv, zwei Direct-Frames enthalten 302.287 geaenderte Pixel. Sechzehn
reale Fensteraufnahmen bleiben schwarz und der Host-Presenter meldet null
Frames. Der warme Gesamtexport dauerte 4,209083 Sekunden, der unveraenderte
Hostbuild 0,219272 Sekunden. Das 200-MHz-Ziel, das relative Gate und ein
sichtbarer DirectBoot-Spielbildnachweis bleiben offen.

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
