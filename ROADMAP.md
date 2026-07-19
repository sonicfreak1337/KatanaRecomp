# KatanaRecomp Roadmap

Status: Pre-Alpha
Aktueller interner Meilenstein: `v0.47.0`
Aktuelle Phase: `v0.48.0` - Integration
Erster oeffentlicher Release: `v0.50.0` Alpha
Weitere interne Gates: `v0.48.0` und `v0.49.0`; danach `v0.75.0` Beta und `v1.0.0` Stable

## Produktziel

KatanaRecomp ist ein allgemeines Dreamcast-Recompiler-Framework mit Runtime-SDK
und generischer Port- und Installer-API. Titelbezogene Installer-, Integrations-
oder Enhancementlogik ist ausdruecklich kein Produktbestandteil.

KatanaRecomp wandelt rechtmaessig lokal bereitgestellte Dreamcast-Programme in
eigenstaendige native Portprojekte um. Analyzer, generierter Code und Runtime
bleiben getrennt. ProprietÃ¤re Spiel-, BIOS- oder Assetdaten gehoeren weder in
das Repository noch in verteilbare Pakete.

## Planungsregeln

1. Allgemeine Semantik vor Titelsonderfaellen.
2. Jede neue Semantik erhaelt synthetische oder frei lizenzierte Regressionen.
3. Unbekannte Ziele, Opcodes, BIOS-Aufrufe und MMIO-Zugriffe duerfen nicht still
   erfolgreich sein.
4. Private Retaildaten bleiben ausserhalb von Repository, CI, Paketen und
   oeffentlichen Berichten.
5. Sonic Adventure dient ausschliesslich als private Retail-Testbench; seine
   Produktlogik gehoert in ein spaeteres eigenstaendiges Portprojekt.
6. Private Retail-Laeufe duerfen nur allgemeine Frameworkfehler aufdecken; jeder
   Fix erhaelt eine synthetische oder frei lizenzierte Regression.
7. Gate-Vorbereitung und Freigabe bleiben getrennte Tasks.
8. Ein globaler Projektprozentsatz wird nicht mehr gepflegt. Neue zukuenftige
   Arbeit darf den scheinbaren Fortschritt nicht rueckwaerts rechnen.
9. Task-IDs sind ab dem ersten Merge unveraenderlich. Entfallene oder ersetzte
   Aufgaben bleiben in `docs/TASK_ID_REGISTRY.md` registriert.

## Fertiggestellte Grundlage

Die historischen Detailtasks bleiben in Git nachvollziehbar und werden hier
nicht mehr einzeln wiederholt.

| Bereich | Stand |
|---|---|
| SH-4 Integer, Systemregister und FPU-Grundlage | umgesetzt und getestet |
| Loader, GDI, ISO9660 und rekursive Analyse | umgesetzt |
| Katana-IR, C++-Backend und Blockdispatch | umgesetzt |
| Speicherbus, Exceptions, Interrupts, Scheduler und DMA | umgesetzt |
| BIOS-HLE, System-ASIC, Maple, PVR-Minimalpfad, AICA-HLE und GD-ROM | umgesetzt, Genauigkeit noch begrenzt |
| Windows-GUI, GDI-Workflow, Portexport und native Hostruntime | umgesetzt |
| Private Retailanalyse | 55.202 Instruktionen, 813 Funktionen; Kontrollflussfront geschlossen, 6.608.338 offene Bytes |

## v0.47.0 - Core-Stabilisierung und generische Retail-Runtime

### Ziel

Vor weiterer Retail-Codeentdeckung werden alle vorhandenen CPU-, Runtime-,
Kontrollfluss-, Speicher- und Buildvertraege korrigiert, differenziell
abgesichert und vermessen. Danach muss jede erreichbare indirekte Stelle
statisch bewiesen, vollstaendig bewacht oder durch einen expliziten
Runtime-only-Vertrag behandelt sein.

Eine private Sonic-`game.exe` darf am Ende dieses Meilensteins gebaut, aber
nicht gestartet werden.

### Stufe A: P0-Core-Korrektheit

- [x] `KR-4611` - SH-4-Kontrollzustand, Delay Slots, RTE, SLEEP und Interrupts
- [x] `KR-4612` - Store Queue und Cacheadressierung
- [x] `KR-4613` - einheitliche Gastwrites und Codeinvalidierung
- [x] `KR-4614` - kontexttreue, sounde Kontrollfluss- und Wertanalyse
- [x] `KR-4615` - stabile und skalierbare Runtime-Blockregistry
- [x] `KR-4616` - einheitliches Gasttiming und Scheduler-/Geraeteintegration
- [x] `KR-4617` - unabhaengige Cross-Engine- und CFG-Konformitaetstests
- [x] `KR-4618` - Core-Korrektheitsgate

### Stufe B: P1-Performance und Build

- [x] `KR-4621` - Speicher-, Dispatch- und Invalidierungs-Hotpaths
- [x] `KR-4622` - inkrementelle Kontrollflussanalyse, IR und Codegen
- [x] `KR-4623` - Disc-, GDI-, ISO- und GD-ROM-I/O
- [x] `KR-4624` - Buildgraph, Cache und Testmatrix
- [x] `KR-4625` - Performance-/Buildgate

### Stufe C: Retail-Kontrollfluss und Build

- [x] `KR-4715` - ungeloeste Kontrollflussfront inventarisieren
- [x] `KR-4716` - ABI-erhaltene Callbacks, Parameter und Stackwerte
- [x] `KR-4717` - Objekt-, Feld- und VTable-Points-to
- [x] `KR-4718` - expliziter Runtime-only-Dispatch
- [x] `KR-4719` - privater Retail-Buildnachweis mit erzwungenem Build-only-Modus
- [x] `KR-4703` - VMU-/Flash-Arbeitskopien und Host-Pacing
- [x] `KR-4704` - v0.47 Gate-Vorbereitung
- [x] `KR-4705` - v0.47 interne Freigabe

`KR-4704` ist technisch bestanden. Das Gate trennt ausfuehrbare
Speicherberechtigung von statischer, materialisierbarer und aktuell
dispatchbarer Abdeckung. Unbekannte Speicherbytes bleiben unbekannt und nicht
implizit ausfuehrbar; jeder Kontrolltransfer erreicht einen gueltigen Block,
den validierten Demand-Pfad oder bricht vor Gastwirkung strukturiert ab.
Der private doppelte Build-only-Nachweis meldet `unknown_instructions=0`,
`guarded_partial=0`, `unresolved=0`, `reachable_abort_edges=0`,
`uncovered_control_targets=0` und `dispatch_paths_without_validation=0`.
Beide frischen Hostbuilds besitzen identische portable Metadaten und Quellen;
kein Runtimeprozess wurde fuer die Gateevidenz gestartet. `KR-4705` ist
freigegeben und abgeschlossen; die aktive Entwicklung liegt in v0.48.

### Verbindliche Reihenfolge

```text
KR-4611 bis KR-4617
  -> KR-4618
  -> KR-4621 bis KR-4624
  -> KR-4625
  -> KR-4715
  -> KR-4716 und KR-4717
  -> KR-4718
  -> KR-4719
  -> KR-4703
  -> KR-4704
  -> KR-4705
```

### Gate

- Debug und RelWithDebInfo liefern dieselben Gastresultate
- Hint-Direktiven koennen keine Beweise oder Exportvollstaendigkeit erzeugen
- Delay Slots, Fallthroughs und Funktionsgrenzen sind kontexttreu
- Site-Vollstaendigkeit wird getrennt von einzelnen Kandidatenkanten modelliert
- unbekannte Caller und Callkontexte koennen keine zu kleine Zielmenge erzeugen
- alle Gastwrites invalidieren ueberdeckten generierten Code korrekt
- Scheduler, TMU, RTC, DMA und GD-ROM verwenden einen gemeinsamen Gastzyklusvertrag
- Analyse-, Codegen-, Disc- und Buildbudgets bestehen
- `unresolved == 0`
- eine frei lizenzierte Anwendung erreicht `KR_V047_NATIVE_HOST_READY`
- der private Sonic-Workflow erzeugt reproduzierbar eine `game.exe`
- der Harness beweist `execution_mode=build-only` und
  `game_executable_started=false`
- keine proprietaeren Daten oder privaten Identitaetsmerkmale gelangen ins Repo

## v0.48.0 - Port-, Harness-, Controller- und GUI-Integration

### Ziel

Der eigenstaendige Portworkflow, der private Harness, native Eingabe und die
Desktop-GUI werden vor dem ersten Sonic-Runtimelauf zu einem belastbaren,
informativen und schnellen Produktpfad verbunden. Verteilbare Nachweise
verwenden weiterhin ausschliesslich synthetische oder frei lizenzierte Quellen.

### Urspruengliche, wiederhergestellte Tasks

- [ ] `KR-4801` - versioniertes Runtime-SDK fuer externe Port-Projekte
- [ ] `KR-4802` - gemeinsamer CLI-/GUI-Portexport und Buildworkflow
- [ ] `KR-4803` - Out-of-Tree-`game.exe`-Integration
- [ ] `KR-4804` - v0.48 Gate-Vorbereitung
- [ ] `KR-4805` - v0.48 interne Meilenstein-Freigabe

### Neue Integrationsaufgaben

- [ ] `KR-4831` - generischer Originaldisc-Installer ohne Retaildaten im Portpaket
- [ ] `KR-4811` - private Harnessmodi und technisch erzwungener No-run-Vertrag
- [ ] `KR-4812` - strukturierte Runtimeevidenz, Budgets, Replay und Datenschutz
- [ ] `KR-4813` - content-addressed Harness- und Portbuildbeschleunigung
- [ ] `KR-4814` - nativer Controller und gastzeitgebundene Maple-Eingabe
- [ ] `KR-4821` - versionierte Jobtelemetrie und belastbarer Fortschritt
- [ ] `KR-4822` - GUI-Informationsarchitektur und responsives Layout
- [ ] `KR-4823` - Diagnostik-, Ergebnis-, Log- und Workflow-QOL
- [ ] `KR-4824` - unveraenderliche Task-ID-Registry und Roadmaplinter

### Verbindliche Reihenfolge

```text
KR-4705
  -> KR-4831
  -> KR-4801, KR-4811, KR-4821 und KR-4824
  -> KR-4802
  -> KR-4803
  -> KR-4812, KR-4813 und KR-4814
  -> KR-4822
  -> KR-4823
  -> KR-4804
  -> KR-4805
```

Unabhaengige Aufgaben derselben Stufe duerfen parallel entwickelt werden.

### Gate

- verteilbare Ports enthalten nur AOT-Code und eine generische Originaldisc-
  Recipe; vollstaendige Retaildaten entstehen erst lokal beim Nutzer
- externe Ports bauen gegen ein versioniertes minimales Runtime-SDK
- CLI und GUI erzeugen denselben Port- und Buildplan
- `game.exe` stammt nachweisbar aus dem aktuellen Job und nicht aus einem
  veralteten Ausgabeordner
- Harnessmodi `build-only`, `runtime-probe` und `interactive` sind getrennt
- deterministische Probes verwenden strukturierte, streng sequenzierte Metriken
- interaktive Sitzungen gelten nie als Gateevidenz
- Controllerbuttons, Trigger und Analogachsen erreichen Maple in einem
  frei lizenzierten Test
- Keyboardfallback, Hotplug und Fokusverhalten sind getestet
- die GUI zeigt reale Stufen, Arbeitsmengen, Kontrollflussklassen, Diagnosen,
  Cachehits und Artefakte
- GUI-Aktualisierung ist eventgetrieben und kopiert keine unbeschraenkten
  Verlaufsmengen pro Refresh
- Task-IDs sind registriert und koennen nicht semantisch wiederverwendet werden
- private Quellen und Runtimeberichte bleiben ausserhalb des Repositorys
- `KR_V048_PORT_WORKFLOW_READY` wird reproduzierbar erreicht

## v0.49.0 - Generischer Runtime-Bring-up und interner Release-Candidate

### Ziel

Nach dem v0.48-Integrationsgate beginnt der kontrollierte Runtime-Bring-up unter
echter Gastlast. Sonic Adventure darf dafuer privat als wichtigste
End-to-End-Testbench dienen. Implementiert und versioniert werden nur
allgemeine Mechanismen fuer Programmeinstieg, Gastframes, Eingabe und
kontrollierbaren Gastfortschritt.

### Bring-up-Tasks mit neuen, konfliktfreien IDs

- [ ] `KR-4911` - Runtimebeobachtung, Replay und Fehlerpakete
- [ ] `KR-4912` - dynamische Codebereiche, Module und Overlays
- [ ] `KR-4913` - CPU-/Plattform-Bring-up bis `KR_GUEST_PROGRAM_ENTERED`
- [ ] `KR-4914` - private interaktive Runtime-Sitzung mit Controller
- [ ] `KR-4915` - Gast-PVR-Pfad bis `KR_FIRST_GUEST_FRAME`
- [ ] `KR-4916` - Gastinput und kontrollierter Retail-Fortschritt

### Urspruengliche, wiederhergestellte Release-Candidate-Tasks

- [ ] `KR-4901` - Alpha-CI-Konfiguration fuer Windows und Linux
- [ ] `KR-4902` - reproduzierbare Pakete sowie Daten- und Lizenzaudit
- [ ] `KR-4903` - Alpha-Checkpoint- und Gate-Automatisierung einfrieren
- [ ] `KR-4904` - v0.49 Gate-Vorbereitung
- [ ] `KR-4905` - v0.49 interne Kandidaten-Freigabe

### Verbindliche Reihenfolge

```text
KR-4805
  -> KR-4911
  -> KR-4912
  -> KR-4913
  -> KR-4914 und KR-4915
  -> KR-4916
  -> KR-4901, KR-4902 und KR-4903
  -> KR-4904
  -> KR-4905
```

### Gate

- zwei deterministische Probes erreichen dieselben Checkpoints und Kernmetriken
- ein separater interaktiver Lauf erlaubt lokale Controllersteuerung
- `KR_GUEST_PROGRAM_ENTERED`, `KR_FIRST_GUEST_FRAME`,
  `KR_GUEST_INPUT_INTERACTIVE` und `KR_CONTROLLED_RETAIL_SCENE` beruhen auf
  versionierten titelunabhaengigen Gastereignissen
- Hostsmokes werden nicht als Gastframe, Gastaudio oder Gasteingabe gezaehlt
- dynamische Module und ersetzter RAM-Code koennen nicht still stale Bloecke
  ausfuehren
- Boot, Gastvideo und Gastinput machen unter echter Gastlast gemeinsam Fortschritt
- fruehe SH-4-/Holly-Initialisierung verwendet geschlossene Registervertraege;
  ungebundene DMA-Starts duerfen keinen Erfolg simulieren
- Maple-, PVR- und weitere DMA-Pfade muessen Gastdeskriptoren, Gastzeit,
  Speicherwrites und ASIC-Completion gemeinsam nachweisen
- CI, Pakete, Datenschutz-, Lizenz- und Referenzaudits bestehen
- keine Retaildaten gelangen in Pakete, CI, Repository oder oeffentliche Berichte
- `KR_V049_ALPHA_CANDIDATE_READY` wird erreicht
- KR-4904 stoppt zwingend fuer Nutzerreview

## v0.50.0 Alpha - Oeffentliches Release

### Tasks

- [ ] `KR-4999` - Alpha-Gate-Vorbereitung
- [ ] `KR-5000` - v0.50.0 Alpha-Release

### Alpha-Gate

- die unveraenderte v0.49-Kandidatenbasis besteht frische Debug- und
  RelWithDebInfo-/Release-Builds
- private deterministische Testbench-Laeufe erreichen denselben generischen
  Runtimecheckpoint; oeffentliche Gates verwenden verteilbare Regressionen
- eine getrennte interaktive Sitzung bestaetigt praktische Controllerbedienung
- Boot, Auswahl und mindestens eine kontrollierbare Spielszene funktionieren
- Video, Eingabe, Disc-I/O, Scheduler, DMA und Interrupts machen messbaren
  Gastfortschritt
- `silent_failures == 0`
- Fehler- und Budgetpfade liefern redigierte Diagnoseberichte
- Windows ist Alpha-Zielplattform; Linux baut Core, CLI und Tests
- Release und Repository enthalten keine Retaildaten

## v0.75.0 Beta - Breite Frameworkkompatibilitaet

### Ziel

Mehrere rechtmaessig lokal bereitgestellte Dreamcast-Programme laufen ueber
lange Sitzungen mit belastbaren persistenten Daten. Private Retail-Testbenches
decken unterschiedliche Lastprofile ab. Grafik, Audio und Performance sind
fuer den dokumentierten Frameworkumfang brauchbar.

### Tasks

- [ ] `KR-6001` - Langzeit-Retailabdeckung und Save-Kompatibilitaet
- [ ] `KR-6002` - PVR- und AICA-Genauigkeit
- [ ] `KR-6003` - Performance, Pacing und Langzeitstabilitaet
- [ ] `KR-6004` - Mehrtitel-Kompatibilitaet und Debuggerwerkzeuge
- [ ] `KR-7499` - Beta-Gate-Vorbereitung
- [ ] `KR-7500` - v0.75.0 Beta-Release

### Beta-Gate

- mindestens ein privates Retail-Testprofil laeuft ueber eine definierte lange
  Sitzung ohne titelbezogene Frameworkausnahme
- mehrere Last-, Save- und Modulszenarien besitzen eine adressfreie Statusmatrix
- Save, Laden, Neustart und VMU-Arbeitskopien sind belastbar
- mehrere rechtmaessig lokal bereitgestellte Titel erreichen interaktive Szenen
- Grafik, Audio, Eingabe, DMA, Timer und Interrupts arbeiten zusammen
- Performancebudgets, Fallbackrate, Invalidierungen und Schedulerjitter werden
  pro Testprofil berichtet
- Abstuerze und Haenger erzeugen verwertbare Berichte

## v1.0.0 Stable - Stabiles Recompiler-Framework

### Ziel

v1.0 verspricht keine vollstaendige Dreamcast-Kompatibilitaet. Es verspricht
einen stabilen, dokumentierten und reproduzierbaren Rahmen fuer den klar
ausgewiesenen unterstuetzten Bereich.

### Tasks

- [ ] `KR-9001` - oeffentliche Vertrage und unterstuetzten Umfang einfrieren
- [ ] `KR-9002` - Plattformpakete, Installation und Migration
- [ ] `KR-9003` - Langzeit-QA, Dokumentation, Datenschutz und Wartung
- [ ] `KR-9999` - v1.0 Gate-Vorbereitung
- [ ] `KR-10000` - v1.0.0 Release

### Stable-Gate

- CLI, Manifest, Runtime-ABI, SDK und Replayformat sind versioniert
- keine bekannte stille Fehlkompilierung besteht im unterstuetzten Bereich
- der unterstuetzte Spiele- und Hardwareumfang ist explizit dokumentiert
- Windows-Pakete und der definierte Linux-Support sind reproduzierbar
- Upgrade- und Migrationspfade sind getestet
- Kompatibilitaetskorpus, Langzeitlaeufe, Audits und Crashberichte bestehen
- Repository und Pakete enthalten keine BIOS-, Disc-, Spiel- oder Assetdaten

## Nach v1.0

- weitere Spiele und Firmwareprofile
- genauere PVR-, AICA-, MMU- und Cachemodelle
- weitere Codegen-Backends und Plattformen
- integrierter Debugger und Remote-Debugging
- Hardwarevergleich und automatisierte Referenztraces
- Netzwerkhardware, Modem und Broadband Adapter
- Modding- und Forschungswerkzeuge

## Nicht-Ziele bis v1.0

- perfekte Zyklusgenauigkeit aller Dreamcast-Komponenten
- vollstaendige Kompatibilitaet mit jedem Dreamcast-Titel
- vollstaendige LLE aller Firmware- und Audiopfade
- jede seltene Peripherie und Netzwerkhardware
- titelbezogene Patches als Ersatz fuer allgemeine Semantik
