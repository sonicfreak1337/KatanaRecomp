# KatanaRecomp Roadmap

Status: Pre-Alpha
Aktueller interner Meilenstein: `v0.46.0`
Aktuelle Phase: `v0.47.0` - generische Retail-Runtime
Erster oeffentlicher Release: `v0.50.0` Alpha
Weitere interne Gates: `v0.48.0` und `v0.49.0`; danach `v0.75.0` Beta und `v1.0.0` Stable

## Produktziel

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
5. Vor dem Alpha-Bereich darf Sonic Adventure analysiert und bis zu einer
   privaten `game.exe` gebaut, aber nicht gestartet werden.
6. Der erste echte Sonic-Runtimelauf gehoert zur Alpha-Entwicklung.
7. Gate-Vorbereitung und Freigabe bleiben getrennte Tasks.
8. Ein globaler Projektprozentsatz wird nicht mehr gepflegt. Neue zukuenftige
   Arbeit darf den scheinbaren Fortschritt nicht rueckwaerts rechnen.9. Task-IDs sind ab dem ersten Merge unveraenderlich. Entfallene oder ersetzte
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
| Private Retailanalyse | 55.104 Instruktionen, 813 Funktionen, 117 Stellen ohne endliche Zielmenge |

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
- [ ] `KR-4616` - einheitliches Gasttiming und Scheduler-/Geraeteintegration
- [ ] `KR-4617` - unabhaengige Cross-Engine- und CFG-Konformitaetstests
- [ ] `KR-4618` - Core-Korrektheitsgate

### Stufe B: P1-Performance und Build

- [ ] `KR-4621` - Speicher-, Dispatch- und Invalidierungs-Hotpaths
- [ ] `KR-4622` - inkrementelle Kontrollflussanalyse, IR und Codegen
- [ ] `KR-4623` - Disc-, GDI-, ISO- und GD-ROM-I/O
- [ ] `KR-4624` - Buildgraph, Cache und Testmatrix
- [ ] `KR-4625` - Performance-/Buildgate

### Stufe C: Retail-Kontrollfluss und Build

- [ ] `KR-4715` - ungeloeste Kontrollflussfront inventarisieren
- [ ] `KR-4716` - ABI-erhaltene Callbacks, Parameter und Stackwerte
- [ ] `KR-4717` - Objekt-, Feld- und VTable-Points-to
- [ ] `KR-4718` - expliziter Runtime-only-Dispatch
- [ ] `KR-4719` - privater Retail-Buildnachweis mit erzwungenem Build-only-Modus
- [ ] `KR-4703` - VMU-/Flash-Arbeitskopien und Host-Pacing
- [ ] `KR-4704` - v0.47 Gate-Vorbereitung
- [ ] `KR-4705` - v0.47 interne Freigabe

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

## v0.49.0 - Sonic-Alpha-Bring-up und interner Release-Candidate

### Ziel

Nach dem v0.48-Integrationsgate beginnt der erste private Sonic-Runtimelauf.
Der Produktpfad soll reproduzierbar das Hauptprogramm, einen echten Gastframe,
interaktive Eingabe und anschliessend eine kontrollierbare Szene erreichen.

### Bring-up-Tasks mit neuen, konfliktfreien IDs

- [ ] `KR-4911` - Runtimebeobachtung, Replay und Fehlerpakete
- [ ] `KR-4912` - dynamische Codebereiche, Module und Overlays
- [ ] `KR-4913` - CPU-/Plattform-Bring-up bis `SA_MAIN_ENTERED`
- [ ] `KR-4914` - private interaktive Runtime-Sitzung mit Controller
- [ ] `KR-4915` - Gast-PVR-Pfad bis `SA_FIRST_FRAME`
- [ ] `KR-4916` - Menue, Eingabe und spielbare Szene

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
- `SA_MAIN_ENTERED`, `SA_FIRST_FRAME`, `SA_MENU_INTERACTIVE` und
  `SA_ALPHA_PLAYABLE` beruhen auf versionierten Gastereignissen
- Hostsmokes werden nicht als Gastframe, Gastaudio oder Gasteingabe gezaehlt
- dynamische Module und ersetzter RAM-Code koennen nicht still stale Bloecke
  ausfuehren
- Boot, Menue und mindestens eine Szene funktionieren mit Video und Controller
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
- zwei private deterministische Laeufe erreichen `SA_ALPHA_PLAYABLE`
- eine getrennte interaktive Sitzung bestaetigt praktische Controllerbedienung
- Boot, Auswahl und mindestens eine kontrollierbare Spielszene funktionieren
- Video, Eingabe, Disc-I/O, Scheduler, DMA und Interrupts machen messbaren
  Gastfortschritt
- `silent_failures == 0`
- Fehler- und Budgetpfade liefern redigierte Diagnoseberichte
- Windows ist Alpha-Zielplattform; Linux baut Core, CLI und Tests
- Release und Repository enthalten keine Retaildaten

## v0.75.0 Beta - Breite Spielbarkeit

### Ziel

Sonic Adventure ist nicht nur in einer Szene spielbar, sondern laeuft ueber
lange Sitzungen mit belastbaren Saves. Mehrere weitere Titel erreichen
interaktive Szenen. Grafik, Audio und Performance sind fuer reale Nutzung
brauchbar.

### Tasks

- [ ] `KR-6001` - Sonic-Adventure-Abdeckung und Save-Kompatibilitaet
- [ ] `KR-6002` - PVR- und AICA-Genauigkeit
- [ ] `KR-6003` - Performance, Pacing und Langzeitstabilitaet
- [ ] `KR-6004` - Mehrtitel-Kompatibilitaet und Debuggerwerkzeuge
- [ ] `KR-7499` - Beta-Gate-Vorbereitung
- [ ] `KR-7500` - v0.75.0 Beta-Release

### Beta-Gate

- mindestens eine Sonic-Adventure-Story laeuft von neuem Save bis zu den Credits
- weitere Storypfade und Sondermodi besitzen eine gepflegte Statusmatrix
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
