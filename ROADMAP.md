# KatanaRecomp Roadmap

Status: Pre-Alpha
Aktueller interner Meilenstein: `v0.46.0`
Aktuelle Phase: `v0.47.0` - generische Retail-Runtime
Erster oeffentlicher Release: `v0.50.0` Alpha
Weitere Gates: `v0.75.0` Beta und `v1.0.0` Stable

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
   Arbeit darf den scheinbaren Fortschritt nicht rueckwaerts rechnen.

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

Vor weiterer Retail-Codeentdeckung werden alle bereits vorhandenen CPU-,
Runtime-, Analyse-, Speicher- und Buildvertraege korrigiert, differenziell
abgesichert und vermessen. Danach muss die generische Pipeline jede erreichbare
indirekte Stelle entweder statisch beweisen, mit einer endlichen Zielmenge
bewachen oder durch einen expliziten Runtime-only-Vertrag behandeln.

Eine Sonic-`game.exe` darf am Ende dieses Meilensteins gebaut, aber nicht
gestartet werden.

### Stufe A: P0-Core-Korrektheit

- [ ] `KR-4611` - SH-4-Kontrollzustand, Delay Slots, RTE, SLEEP und Interrupts
- [ ] `KR-4612` - Store Queue und Cacheadressierung
- [ ] `KR-4613` - einheitliche Gastwrites und Codeinvalidierung
- [ ] `KR-4614` - sounde Kontrollfluss- und Wertanalyse
- [ ] `KR-4615` - stabile und skalierbare Runtime-Blockregistry
- [ ] `KR-4616` - einheitliches Gasttiming und Scheduler-/Geraeteintegration
- [ ] `KR-4617` - unabhaengige Cross-Engine-Konformitaetstests
- [ ] `KR-4618` - Core-Korrektheitsgate

### Stufe B: P1-Performance und Build

- [ ] `KR-4621` - Speicher-, Dispatch- und Invalidierungs-Hotpaths
- [ ] `KR-4622` - inkrementelle Analyse, IR und Codegen
- [ ] `KR-4623` - Disc-, GDI-, ISO- und GD-ROM-I/O
- [ ] `KR-4624` - Buildgraph, Runtime-SDK, Cache und Testmatrix
- [ ] `KR-4625` - Performance-/Buildgate

### Stufe C: Retail-Kontrollfluss und Build

- [ ] `KR-4715` - ungeloeste Kontrollflussfront inventarisieren
- [ ] `KR-4716` - ABI-erhaltene Callbacks, Parameter und Stackwerte
- [ ] `KR-4717` - Objekt-, Feld- und VTable-Points-to
- [ ] `KR-4718` - expliziter Runtime-only-Dispatch
- [ ] `KR-4719` - privater Retail-Buildnachweis ohne Ausfuehrung
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

Unabhaengige Tasks innerhalb einer Stufe duerfen parallel entwickelt werden.
Eine spaetere Stufe beginnt erst, wenn das vorherige Gate vollstaendig besteht.

### Gate

- Debug und RelWithDebInfo liefern dieselben Gastresultate
- Registerbanken, Call-Delay-Slots, RTE, SLEEP, Exceptions und Interrupts
  bestehen unabhaengige SH-4-Konformitaetsvektoren
- Store Queues waehlen SQ0/SQ1 ueber Adressbit 5
- alle Gastwrites invalidieren ueberdeckten generierten Code korrekt
- keine Analysezielmenge ignoriert unbekannte Caller oder Callkontexte
- Blockregistry und Invalidierung besitzen stabile Handles und skalierende Indizes
- Scheduler, TMU, RTC, DMA und GD-ROM verwenden einen gemeinsamen Gastzyklusvertrag
- definierte Speicher-, Analyse-, Codegen-, Disc- und Buildbudgets bestehen
- `unresolved == 0`
- eine frei lizenzierte Anwendung erreicht `KR_V047_NATIVE_HOST_READY`
- der private Sonic-Workflow erzeugt reproduzierbar eine `game.exe`, startet sie
  aber nicht
- keine proprietaeren Daten oder privaten Identitaetsmerkmale gelangen ins Repo

## v0.50.0 Alpha - Sonic Adventure Bring-up

### Ziel

Der offizielle Workflow erzeugt aus der lokal bereitgestellten Sonic-Adventure-
GDI ein externes Portprojekt und eine eigenstaendige `game.exe`. Diese erreicht
reproduzierbar eine sichtbare und kontrollierbare Spielszene.

Der erste Prozessstart und `SA_MAIN_ENTERED` sind Alpha-Arbeit. Der oeffentliche
Alpha-Release erfolgt erst bei `SA_ALPHA_PLAYABLE`.

### Noch fehlende, bisher unzureichend dokumentierte Arbeit

- Runtimebeobachtung, deterministische Replays und verwertbare Fehlerpakete
- nach dem Boot geladene ausfuehrbare Bereiche, Module und Overlays
- reale SH-4-, FPU-, MMU-/Cache-, BIOS-, GD-ROM-, DMA- und Timingblocker
- der echte Gast-PVR-Pfad statt eines synthetisch beschriebenen VRAM-Frames
- Menue-, Controller-, Scheduler- und Disc-I/O-Fortschritt bis in eine Szene
- Alpha-Paketierung, CI, Installation, Diagnostik, Datenschutz und Rechtsaudit

### Tasks

- [ ] `KR-4801` - Runtimebeobachtung, Replay und Fehlerpakete
- [ ] `KR-4802` - dynamische Codebereiche, Module und Overlays
- [ ] `KR-4803` - CPU-/Plattform-Bring-up bis `SA_MAIN_ENTERED`
- [ ] `KR-4804` - Gast-PVR-Pfad bis `SA_FIRST_FRAME`
- [ ] `KR-4805` - Menue, Eingabe und spielbare Szene
- [ ] `KR-4901` - Alpha-Haertung, Paketierung, CI und Audit
- [ ] `KR-4999` - Alpha-Gate-Vorbereitung
- [ ] `KR-5000` - v0.50.0 Alpha-Release

### Alpha-Gate

- zwei identische Laeufe erreichen `SA_ALPHA_PLAYABLE`
- Boot, Auswahl und mindestens eine kontrollierbare Spielszene funktionieren
- Video und Eingabe funktionieren gemeinsam
- Disc-I/O, Scheduler, DMA und Interrupts machen messbaren Fortschritt
- `silent_failures == 0`
- Fehler- und Budgetpfade liefern redigierte Diagnoseberichte
- Audio darf klar dokumentierte Alpha-Abweichungen besitzen
- ein kompletter Spieldurchlauf ist noch kein Alpha-Kriterium
- Windows ist die Alpha-Zielplattform; Linux muss Core, CLI und Tests bauen,
  aber keine fertige Desktop-GUI liefern

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
