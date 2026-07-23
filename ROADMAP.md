# KatanaRecomp Roadmap

Status: Pre-Alpha
Aktueller interner Meilenstein: `v0.47.0`
Aktuelle Phase: `v0.48.0` - Native Disc Boot und erster echter Gastframe
Erster oeffentlicher Release: `v0.50.0` Alpha
Weitere interne Gates: `v0.48.0` und `v0.49.0`; danach `v0.75.0` Beta und `v1.0.0` Stable

## Produktziel

KatanaRecomp ist ein allgemeines Dreamcast-Recompiler-Framework mit Runtime-SDK
und generischer Port- und Installer-API. Titelbezogene Installer-, Integrations-
oder Enhancementlogik ist ausdruecklich kein Produktbestandteil.

KatanaRecomp wandelt rechtmaessig lokal bereitgestellte Dreamcast-Programme in
eigenstaendige native Portprojekte um. Analyzer, generierter Code und Runtime
bleiben getrennt. Proprietaere Spiel-, BIOS- oder Assetdaten gehoeren weder in
das Repository noch in verteilbare Pakete.

Das verbindliche Architekturmodell ist XenonRecomp-artige statische
Rekompilierung: Der allgemeine Werkzeugpfad uebersetzt die aus der lokalen
Nutzerdisc nachgewiesenen Programme `IP.BIN` und BootExecutable vorab aus SH-4
in C++ beziehungsweise nativen PC-Code. Die getrennte Runtime implementiert
nur typisierte Dreamcast-Plattformgrenzen; ein freigegebener normaler Portlauf
darf weder SH-4-Interpreter/JIT noch einen virtuellen Discplayer oder
Titelhacks enthalten. Der Produktport erfuellt diese Grenze jetzt: Er emittiert
und linkt keinen Interpreter, und unbekannter oder veraenderter Code aktiviert
nur vorab gebundene AOT-Module oder endet kontrolliert. Der begrenzte
Interpreter bleibt ausschliesslich im expliziten `diagnostic_partial`-Profil.
Strukturierte Disc-Ladetransaktionen und die vorab erzeugte Registry latenter
nativer Module bleiben offene `KR-4848`-Arbeit.

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
| Private Retailanalyse | 55.504 Instruktionen, 815 Funktionen; keine unbekannte SH-4-Instruktion und keine harte statische Hardwareluecke |

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

## v0.48.0 - Native Disc Boot und erster echter Gastframe

### Ziel

Der Recompiler fuehrt den disc-eigenen Systembootstrap und die Bootdatei als
native AOT-Segmente aus und erreicht einen scanoutgebundenen, vom Gast
erzeugten Frame. Sonic Adventure PAL ist die private Haupttestbench; Sonic
Shuffle PAL und Ecco dienen dem allgemeinen Architekturabgleich. Implementiert
werden nur titelunabhaengige SH-4-, BIOS-, GD-ROM-, DMA-, TA- und PVR-Vertraege.

### Grundlage und migrierte Bring-up-Tasks

- [x] `KR-4831` - Generischer Originaldisc-Installer ohne Retaildaten im Portpaket
- [ ] `KR-4911` - Runtimebeobachtung, Replay und Fehlerpakete
- [ ] `KR-4912` - Dynamische Codebereiche, Module und Overlays
- [ ] `KR-4913` - CPU-/Plattform-Bring-up bis `KR_GUEST_PROGRAM_ENTERED`
- [ ] `KR-4915` - Gast-PVR-Pfad bis `KR_FIRST_GUEST_FRAME`

### Native-Boot-Tasks

- [x] `KR-4841` - Clean-Room-Referenz- und Nicht-Emulationsvertrag
- [ ] `KR-4842` - Seiteneffektfreie Bootdiagnostik und Wait-Loop-Klassifikation
- [x] `KR-4843` - Alias-korrekter nativer Disc-Systembootstrap
- [x] `KR-4844` - Gastzeit, Interruptreihenfolge und vollstaendiger AOT-Chaining-Guard
- [x] `KR-4845` - BIOS-Lifecycle, HLE-Bridges, Flash, Sysinfo und Region
- [x] `KR-4846` - GD-ROM-BIOS-Requestqueue, Status und TOC
- [ ] `KR-4847` - GD-ROM-MMIO, PIO, G1-DMA und Disc-Streaming
- [ ] `KR-4848` - Runtimecode, Disc-Module, Overlays und latentes AOT
- [ ] `KR-4849` - TA-Eingang und PVR-Kommandopfad
- [ ] `KR-4850` - Erster scanoutgebundener Gastframe
- [ ] `KR-4851` - Boot- und Frame-Hotpath
- [ ] `KR-4852` - Konsolidierte v0.48-Validierung
- [ ] `KR-4853` - v0.48 Boot-Gate-Vorbereitung
- [ ] `KR-4854` - v0.48 interne Freigabe

Die Controlleraufgaben `KR-4814` und `KR-4914` beginnen weiterhin erst nach
dem nachgewiesenen echten Gastframe aus `KR-4850`, gehoeren aber zur
anschliessenden v0.49-Integration. Der Frame bleibt das alleinige P0-Ziel von
v0.48.

Aktueller Kernteilstand: Byteidentische BIOS-/GD-Reloads erhalten vorhandene
native AOT-Bloecke, waehrend geaenderte Bytes exakt einmal invalidieren. Der
SH-4-DMAC-Channel-2-Pfad verwendet den oeffentlichen Dreamcast-Vertrag `RS=2`,
32-Byte-Bursts, inkrementierende Quelle, festes Ziel und `DMAOR.DME+DDT` und ist
bis TA-Object-List/EOL synthetisch verbunden. Fuer Direct-Texture-Ziele
`0x11`/`0x13` bleibt die Zielprogression mehrteiliger Transfers als generische
P1-Luecke offen. P1-/P2-Codealiase dispatchen denselben nativen Block.
Beschreibbare absolute Pointertabellen und Fixed-Stride-`BSRF`-Handlerinseln des
garantierten Anfangssnapshots liefern nur vorab kompilierbare `RuntimeOnly`-
Kandidaten: Sie erzeugen keine erfundenen CFG-Kanten und ihre Ziele werden als
Basic-Block-Leader, nicht als Funktionsseeds behandelt. Der damit exportierte
private PAL-Port wurde aus der unveraenderten Original-GDI lokal installiert
und verliess den frueheren `BSRF`-Dispatchstopp. Die anschliessend belegten
BIOS-Kommandos `NOP`, `REQ_MODE` und `SET_MODE` teilen ihren persistenten
Modezustand mit der Paketoberflaeche. Der folgende budgetierte Lauf erreichte
345.568.225 Gastzyklen, 7.421.380 native Bloecke und spaetere PVR-
Registerwrites, bevor der native Inneneinstieg `0x8C654F5C` fehlte.

Dieser Einstieg gehoert zu einer begrenzten `MOV.W`-/`BRAF`-Relative16-Tabelle
und nicht zu einer weiteren `BSRF`-Insel. Der aktuelle statische PAL-Audit
liefert 87 Tabelleneintraege, 76 eindeutige AOT-Kandidaten und 73 Ziele, die im
vorherigen Port fehlten. Sie werden nun als native Blockleader vorbereitet,
waehrend der live geladene Dispatch `RuntimeOnly` bleibt. Der Snapshotcache ist
imagegebunden, P2-Tabellenaliase werden physisch aufgeloest und lokale
AOT-Blockketten reichen die exakte tatsaechliche Terminatorquelle samt
Siteklasse an den externen Dispatch weiter. Der frische PAL-Lauf bestaetigt den
Fix: Der alte Fehler bei `0x8C654F5C` ist verschwunden, und 761.011 beobachtete
Dispatchereignisse bleiben ohne Fehler. Der neue Haupthotspot
`0x8C6658D0 -> 0x8C65247E` ist mit 696.053 Aufrufen ein endlicher
4-Byte-Kopier-/Initialisierungsloop und kein fehlender Zielblock. TA-Eingang,
Rendergeneration und echter Gastframe bleiben unbelegt; `KR-4848`, `KR-4849`
und `KR-4850` bleiben offen. Runtime-ABI 38, Backend-Interface-ABI 3 und
Portprojektvertrag 23 versionieren den aktuellen Vertrag.

Der Export-Hotpath baut globale CFG-, Kanten- und Writer-Slice-Indizes einmalig
auf; Codegen und Projektausgabe reichen die Hostparallelitaet durch. Der CLI-
Hostbuild waehlt dynamisch die CPU-Threadzahl, akzeptiert
`KATANA_HOST_BUILD_JOBS` und kann unter Windows einen getrennten Ninja-Build
verwenden; auf dem primaeren Rechner laufen zwoelf Jobs. Grosse
Dispatchregistries werden in Shards zu maximal 512 Bloecken zerlegt. Der
balancierte Router und genau ein Wrapper pro Owner und Shard reduzieren beim
aktuellen PAL-Port die zentrale `runtime-dispatch.cpp` von 36.703.886 Byte und
525.996 Zeilen auf 34.879 Byte und 607 Zeilen; 43 Shards bleiben bei maximal
393.454 Byte. Eine 513-Block-Regression prueft zwei Shards samt Entfernung
veralteter Dateien, und das vollstaendige synthetische Ninja-/MSVC-Projekt
linkt in 15 Sekunden. Die fokussierte Suite besteht 6/6. Unter
Windows verwendet die Eingabeprovenienz den nativen BCrypt-SHA-256-Pfad mit
grossen Chunks. Funktionsdiscovery und CFG-Simplifizierung besitzen lineare
Adressindizes; stabile, datenschutzneutrale Export-Subphasen grenzen weitere
budgetierte Laeufe ohne einen unbudgetierten Profiler ein.

Der optimierte 12-Job-Export des neuen PAL-Ports dauerte 140,5 Sekunden und
erzeugte 1.856 Funktionen, 37 Codepartitionen sowie 43 Dispatchshards bei null
Retailsektoren im Portpaket. Die unveraenderte Original-GDI wurde lokal mit
drei Tracks und 521.461 Sektoren installiert; die Quelle blieb erhalten und
der alte `gdrom-mode-fix`-Port wurde erst nach erfolgreichem Ersatz entfernt.
Der 30-Sekunden-Lauf blieb ueber 312.939.023 Zyklen und 1.000.000
Rootdispatches stabil. Der 100-Millionen-Zyklen-Snapshot belegt `IP.BIN`-AOT
mit 48.471 Runtime-only-Treffern ohne Fehler, Fallback oder Materialisierung;
bei 320 Millionen Zyklen sind Spielecode, zwei GD-ROM-Kommandos und ein spaeter
PVR-Registerwrite erreicht, aber weiterhin kein TA-, Render- oder Framebeweis.

`KR-4804` ist `retired` (`superseded_by KR-4853`), `KR-4805` ist `retired`
(`superseded_by KR-4854`). `KR-4831` bleibt als abgeschlossene Grundlage erhalten.

### Verbindliche Reihenfolge

```text
KR-4831 und KR-4841
  -> KR-4843 -> KR-4844 -> KR-4845 -> KR-4846 -> KR-4847
KR-4841 -> KR-4842 -> KR-4911 -> KR-4912
KR-4843 und KR-4912 -> KR-4848 -> KR-4913
KR-4847 und KR-4913 -> KR-4849 -> KR-4915 -> KR-4850
KR-4850 -> KR-4851
KR-4851 -> KR-4852
  -> KR-4853
  -> Nutzerreview
  -> KR-4854
```

Unabhaengige Aufgaben derselben Stufe duerfen parallel entwickelt werden.
Waehrend `KR-4841` bis `KR-4851` laufen nur betroffene Targets und kleine,
fokussierte Regressionen. Vollstaendiges CTest, Sanitizer-Gate, Portexport,
Originaldisc-Installation und privater Bootlauf werden einmal in `KR-4852`
gebuendelt. Jeder Prozess besitzt ein hartes Limit von 15 Minuten.

### Gate

- der Einstieg erfolgt am virtuellen P2-PC `0xAC008300` mit physischer
  Codeherkunft `0x0C008000`; PC-relative Semantik behaelt den Alias
- IP.BIN und Bootdatei laufen als getrennte native AOT-Segmente
- BIOS- und GD-ROM-Aufrufe bilden kleine, typisierte Plattformgrenzen; der
  Produktpfad emuliert weder Firmware noch eine SH-4-CPU
- Runtimecode, Module und Overlays werden nur mit bytebewiesener Herkunft
  aktiviert; unbekannte RAM-Bytes sind nicht ausfuehrbar
- `KR_GUEST_PROGRAM_ENTERED` belegt echten Gastkontrollfluss ausserhalb der
  Hostgrenzen
- `KR_FIRST_GUEST_FRAME` verlangt TA-/Rendergeneration, geaenderte Pixel,
  gueltigen Read-/Write-Framebuffer und aktiven Scanout; Hostpraesentation ist
  ein separater Checkpoint
- Fastpath und Referenzpfad erzeugen bytegleiche Gastresultate
- keine festen Spieladressen, Spielbytes, Titelhacks oder uebernommenen
  Emulatorimplementierungen gelangen in den Produktpfad
- Quell-GDIs werden nie geloescht; Retaildaten und private Identitaeten bleiben
  ausserhalb von Repository, CI und verteilbaren Paketen
- vor `KR-4854` wird zwingend fuer Nutzerreview gestoppt; v0.48 wird nur intern
  freigegeben und nicht getaggt. Tags beginnen erst mit der Alpha.

## v0.49.0 - Controller-, Port-, Harness-, GUI-Integration und Alpha-Candidate

### Ziel

Nach dem nativen Boot- und Frame-Gate werden Controllerunterstuetzung,
Runtime-SDK, Portworkflow, Harness, GUI, CI und Paketierung zu einem allgemeinen
Alpha-Candidate integriert. Die v0.48-Basis bleibt dabei unveraendert und Sonic
Adventure liefert keine titelspezifischen Produktvertraege.

### Post-Frame-Controller-Tasks

- [ ] `KR-4814` - Nativer Controller und gastzeitgebundene Maple-Eingabe
- [ ] `KR-4914` - Private interaktive Runtime-Sitzung mit Controller

Beide Controlleraufgaben gehoeren zu v0.49 und beginnen erst auf der durch
`KR-4854` freigegebenen Framebasis. Damit bleibt Controllerarbeit klar hinter
dem P0-Nachweis aus `KR-4850`.

### Migrierte Integrationsaufgaben

- [ ] `KR-4801` - Versioniertes Runtime-SDK fuer externe Port-Projekte
- [ ] `KR-4802` - Gemeinsamer CLI-/GUI-Portexport und Buildworkflow
- [ ] `KR-4803` - Out-of-Tree-`game.exe`-Integration
- [ ] `KR-4811` - Private Harnessmodi und technisch erzwungener No-run-Vertrag
- [ ] `KR-4812` - Strukturierte Runtimeevidenz, Budgets, Replay und Datenschutz
- [ ] `KR-4813` - Content-addressed Harness- und Portbuildbeschleunigung
- [ ] `KR-4821` - Versionierte Jobtelemetrie und belastbarer Fortschritt
- [ ] `KR-4822` - GUI-Informationsarchitektur und responsives Layout
- [ ] `KR-4823` - Diagnostik-, Ergebnis-, Log- und Workflow-QOL
- [ ] `KR-4824` - Unveraenderliche Task-ID-Registry und Roadmaplinter
- [ ] `KR-4916` - Menue, Eingabe und spielbare Szene

### Urspruengliche, wiederhergestellte Release-Candidate-Tasks

- [ ] `KR-4901` - Alpha-CI-Konfiguration fuer Windows und Linux
- [ ] `KR-4902` - Reproduzierbare Pakete sowie Daten- und Lizenzaudit
- [ ] `KR-4903` - Alpha-Checkpoint- und Gate-Automatisierung einfrieren
- [ ] `KR-4904` - v0.49 Gate-Vorbereitung: Tests und Build
- [ ] `KR-4905` - v0.49 interne Kandidaten-Freigabe

### Verbindliche Reihenfolge

```text
KR-4854
  -> KR-4814
  -> KR-4914
KR-4854
  -> KR-4801, KR-4811, KR-4821 und KR-4824
  -> KR-4802
  -> KR-4803
  -> KR-4812 und KR-4813
  -> KR-4822 und KR-4823
KR-4812, KR-4823 und KR-4914
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
