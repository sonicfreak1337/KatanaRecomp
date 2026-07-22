# Codex Handoff

Dieses Dokument definiert, wie Codex oder ein anderer automatisierter Bearbeiter an KatanaRecomp arbeiten soll.

## Pflichtlekture vor jeder Aenderung

1. `README.md`
2. `ROADMAP.md`
3. `docs/STATUS.md`
4. `docs/TASKS.md`
5. `CHANGELOG.md`
6. ab Phase 6 `docs/SONIC_ADVENTURE_ACCEPTANCE.md`
7. relevante Header, Implementierungen und Tests des ausgewaehlten Tasks

## Arbeitsmodell

Codex bearbeitet immer genau eine Task-ID aus `docs/TASKS.md`.

Beispiel:

```text
KR-1101 - SUB, NEG und NOT
```

Keine benachbarten Roadmap-Punkte werden nebenbei implementiert, ausser sie sind fuer den Task zwingend notwendig und im Ergebnis klar dokumentiert.

## Startprozedur

1. nach jedem neuen Lauf und nach jeder Kontextkomprimierung die gebuendelten
   Workspace-Runtimes neu laden sowie vor Windows-Builds die MSVC-x64-
   Entwicklungsumgebung neu initialisieren
2. sauberen Git-Status pruefen
3. aktuellen Branch und Version erfassen
4. Tasktyp bestimmen: Implementierung, Gate-Vorbereitung, interne
   Meilenstein-Freigabe oder Release-Gate
5. Abhaengigkeiten, aktuellen Status und vorhandene Gate-Berichte erfassen
6. erst danach Dateien aendern

Jeder gestartete Prozess besitzt eine harte Laufzeitgrenze von hoechstens
15 Minuten und wird danach mitsamt seinem Prozessbaum beendet. Fokussierte
Builds nutzen die verfuegbaren Hostressourcen parallel; auf dem primaeren
Entwicklungsrechner gilt `--parallel 12`.

Windows-Basisbefehle:

```powershell
git status
git branch --show-current
```

Regulaere Implementierungs-Tasks konfigurieren oder bauen beim Start nicht und
fuehren keine Tests aus. Diese Arbeit wird im letzten Gate-Vorbereitungstask
gebuendelt.

## Branch-Namen

Empfohlen:

```text
codex/KR-1101-sub-neg-not
```

Schema:

```text
codex/<TASK-ID>-<kurzer-name>
```

## Schichtentrennung

Der normale Pfad einer neuen Instruktion lautet:

```text
InstructionKind
    -> Decoder
    -> DecodedInstruction
    -> Disassembly
    -> Katana-IR
    -> C++-Emitter
    -> generierter End-to-End-Test
```

Nicht jede Aenderung betrifft jede Schicht. Codex muss aber ausdruecklich pruefen, welche Schichten betroffen sind.

### Decoder

Zustaendig fuer:

- Opcode-Maske
- Operanden
- Immediate- und Displacement-Dekodierung
- Instruktionsmetadaten
- lesbare Disassembly

Nicht zustaendig fuer:

- Runtime-Speicher
- Kontrollflussgraph-Strategie
- C++-Codeausgabe

### Analyse

Zustaendig fuer:

- Sprungziele
- Delay Slots
- Basic Blocks
- Funktionen
- indirekten Kontrollfluss
- Code-Daten-Trennung

### IR

Zustaendig fuer:

- semantische, backendunabhaengige Operationen
- Operandbreiten
- Status- und Speichereffekte
- Verifikation

### Codegenerator

Zustaendig fuer:

- Uebersetzung gueltiger IR
- Runtime-ABI-Nutzung
- keine erneute SH-4-Dekodierung
- keine versteckte Analyse

### Runtime

Zustaendig fuer:

- CPU-Zustand
- Speicherbus
- MMIO
- Ausnahmen
- Plattformkomponenten

## Testpflichten

Jeder Semantik-Task dokumentiert normalerweise folgende spaeter umzusetzende
Testanforderungen:

1. Decoder-Test
2. IR-Lowering-Test
3. Codegenerator-Test
4. generierten End-to-End-Test
5. Grenz- oder Fehlerfall

Tests muessen deterministisch sein. Regulaere Implementierungs-Tasks sammeln
diese Anforderungen, erstellen und starten die Tests aber noch nicht. Erst der
letzte Gate-Vorbereitungstask einer Phase setzt alle gesammelten Anforderungen
um, erstellt genau einen frischen Build in `build-current/` und fuehrt die
vollstaendige Regression aus.

### Sonic Adventure als private Retail-Testbench

Es gilt die verbindliche Strategie in
`docs/SONIC_ADVENTURE_ACCEPTANCE.md`:

- Ab Phase 11 sind lokale, budgetierte Sonic-Adventure-Debuglaeufe ausdruecklich
  erlaubt. Verteilbare Gates verwenden weiterhin ausschliesslich synthetische
  Fixtures und frei lizenzierte Homebrew-Programme.
- Jeder Retail-Befund muss in eine allgemeine Fehlerklasse und eine
  synthetische oder frei verteilbare Regression ueberfuehrt werden.
- Private Probes duerfen den generischen Pfad GDI -> externes Portprojekt ->
  Hostprogramm -> `KR_CONTROLLED_RETAIL_SCENE` pruefen. Sie definieren keinen
  titelbezogenen oeffentlichen Produktvertrag.
- Oeffentliche Gates verwenden ausschliesslich generische Checkpoints und
  synthetische oder frei lizenzierte Evidenz.
- Assetextraktion fuer eine spaetere Installation ohne GDI gehoert in das
  titelbezogene Folgeprojekt, nicht in KatanaRecomp.
- Keine Spieldaten, Captures, Audioinhalte, Dump-Hashes oder lokalen Pfade
  committen oder in Release-Artefakte aufnehmen.
- Keine fest codierten Sonic-Adventure-Adressen, Remaps, Patches oder
  titelbezogenen Runtime-Sonderfaelle implementieren.
- Oeffentliche CI und verteilbare Tests duerfen proprietaere Eingaben nie
  voraussetzen.

Bei Runtime- oder Speicheraenderungen zusaetzlich:

- Little-Endian-Faelle
- Wraparound
- Vorzeichenerweiterung
- ungueltige Adressen
- Seiteneffektreihenfolge

## Fixtures

Erlaubt:

- handgeschriebene synthetische Opcodefolgen
- selbst entwickelte Testprogramme
- frei lizenzierte Homebrew-Testprogramme mit dokumentierter Lizenz

Nicht erlaubt:

- kommerzielle Executables
- BIOS-Dateien
- Disc-Images
- extrahierte Assets
- kopierter Code aus Referenzprojekten

## Referenzprojekte

Die Ordner unter `reference` sind nur zum Verstaendnis allgemeiner Architekturen und Workflows gedacht.

Codex darf:

- Konzepte vergleichen
- Dateiformate und beobachtbares Verhalten recherchieren
- unabhaengige Tests ableiten

Codex darf nicht:

- Code kopieren
- Funktionen mechanisch uebersetzen
- Kommentare oder Tabellen ohne Lizenzpruefung uebernehmen
- Referenzdateien veraendern

## Scope-Regeln

Codex stoppt und dokumentiert den Grund, wenn:

- eine benoetigte Architekturentscheidung nicht in der Roadmap steht
- zwei bestehende Tests widerspruechliche Semantik verlangen
- eine Referenz nur durch Codekopie nutzbar waere
- der Task eine grosse API-Aenderung ausserhalb seines Scopes erfordert
- eine unbekannte SH-4-Sonderregel nicht sicher belegt werden kann

In diesem Fall wird kein plausibel klingender Unsinn eingebaut. Der Markt ist bereits ausreichend versorgt.

## Dateiregeln

Nicht committen:

- `build/`
- lokale Backup-Ordner
- temporaere generierte Testdateien
- persoenliche Binaerdateien
- kommerzielle Inhalte

Generierte Quellen werden nur committed, wenn die Roadmap dies fuer ein reproduzierbares Beispiel ausdruecklich verlangt. Normalerweise werden sie waehrend des Builds erzeugt.

## Backup-Regel

- Im Arbeitsbereich darf immer genau ein lokales KatanaRecomp-Quellbackup
  existieren: ein Snapshot des neuesten committed Stands.
- Vor dem Erzeugen eines neuen Backups werden alle aelteren KatanaRecomp-Backups
  im festgelegten Backup-Verzeichnis entfernt. Zielpfad und Dateiliste muessen
  vorher geprueft werden.
- Der Dateiname nennt mindestens Projektversion und Commit-ID. Das Backup wird
  aus `HEAD` erzeugt, damit keine uncommitteten Aenderungen oder lokalen Daten
  hineingeraten.
- Backups liegen ausserhalb des Repository-Arbeitsbaums oder in einem ignorierten
  `backups/`-Verzeichnis. `.katana_backup_*`-Verzeichnisse und andere
  Sicherungskopien werden niemals versioniert.
- Ein Quellbackup enthaelt weder `.git`, Build- und Generatorausgaben,
  Toolchains, Referenz-Repositories noch private BIOS-, Flash-, Disc-, Capture-
  oder Trace-Daten.
- Git bleibt die Versionshistorie. Eine Bereinigung des aktuellen Baums darf
  alte Backup-Dateien entfernen; ein destruktives Umschreiben bereits
  veroeffentlichter Historie erfordert einen ausdruecklichen separaten Auftrag.

## Build-Verzeichnis-Regel

- Auf der lokalen Festplatte darf fuer KatanaRecomp immer genau ein aktuelles
  Build-Verzeichnis existieren: `build-current/` im Repository-Arbeitsbaum.
- Nur der letzte Gate-Vorbereitungstask einer Phase darf `build-current/`
  anlegen, neu konfigurieren oder bauen. Regulaere Implementierungs- und reine
  Freigabe-Tasks erzeugen keine Buildartefakte.
- Pre-Alpha-Gate-Vorbereitungen erzeugen den jeweils dokumentierten frischen
  Build. Erst KR-4999 ergaenzt regulaere Debug-/Release-Konfigurationen und die
  verpflichtende Windows-/Linux-CI.
- Separate Verzeichnisse pro Task, Version oder Konfiguration sind nicht erlaubt.
- Vor oder unmittelbar nach dem Anlegen von `build-current/` werden alle alten
  `build/`-, `build-*`- und `cmake-build-*`-Verzeichnisse nach gepruefter
  absoluter Pfadauflosung entfernt.
- Buildartefakte bleiben ignoriert und werden weder in Git noch in das einzelne
  Quellbackup aufgenommen.

## Stil

- C++20
- bestehende Namensraeume und Verzeichnisstruktur beibehalten
- Warnungen als Fehlerquelle ernst nehmen
- kleine Funktionen
- keine globalen versteckten Zustaende
- `std::uint32_t` und andere feste Breiten fuer CPU-Semantik
- signed und unsigned explizit behandeln
- keine Host-UB als CPU-Wraparound missbrauchen
- Kommentare erklaeren SH-4-Sonderregeln, nicht offensichtliche Syntax

## Erwarteter Task-Ablauf

Regulaerer Implementierungs-Task:

1. Task-ID, Abhaengigkeiten und betroffene Schichten erfassen
2. minimalen Scope implementieren
3. Erfolgs-, Grenz- und Fehlerfaelle als Testanforderungen dokumentieren
4. Dokumentation und CHANGELOG aktualisieren
5. Taskstatus aktualisieren und Commit mit Task-ID erstellen

Letzter Gate-Vorbereitungstask:

1. alle seit dem letzten Gate gesammelten Testanforderungen umsetzen
2. `build-current/` frisch konfigurieren und genau einen Gate-Build erstellen
3. vollstaendige Regression und vorgesehene Audits/Gateprofile ausfuehren
4. Gate-Berichte, Dokumentation, CHANGELOG und Status aktualisieren
5. anschliessend zwingend fuer das Nutzerreview stoppen

Interne Meilenstein- oder Release-Freigabe:

1. ausdrueckliche Nutzerfreigabe des unveraenderten Gate-Berichts pruefen
2. keine neue Semantik, Tests oder Builds hinzufuegen
3. bis einschliesslich v0.49.0 nur den naechsten internen Meilenstein
   freigeben; keine Versionierung, kein Release-Commit, kein Tag und keine
   Veroeffentlichung ausfuehren
4. erst bei v0.50.0 und spaeteren oeffentlichen Release-Gates die vorgesehenen
   Release-Aktionen ausfuehren

Verlangt das Review Aenderungen, endet der Gate-Task und die
Gate-Vorbereitung wird nach den Korrekturen vollstaendig wiederholt. Private
Sonic-Adventure-Debuglaeufe duerfen ab Phase 11 als lokale Testbench
stattfinden; sie sind keine verteilbare Alpha-Gateevidenz.

Commit-Beispiel:

```text
KR-1101 SUB NEG und NOT implementieren
```

## Erwartete Abschlussmeldung

Die Abschlussmeldung enthaelt:

- Task-ID
- geaenderte Schichten
- neue Semantik
- bei Implementierungs-Tasks: gesammelte, noch nicht ausgefuehrte
  Testanforderungen
- bei Gate-Vorbereitungen: neue Tests, Build- und Testergebnis sowie Pfad zum
  Gate-Bericht
- bekannte Einschraenkungen
- naechsten nicht blockierten Task
- bei Gate-Vorbereitungen: ausdruecklicher Hinweis auf den Review-Stopp

## Pull-Request-Checkliste

```text
Task: KR-XXXX
Typ: Implementierung | Gate-Vorbereitung | interne Freigabe | Release-Gate

- [ ] Scope eingehalten
- [ ] Testanforderungen dokumentiert
- [ ] Gate-Vorbereitung: gesammelte Tests umgesetzt
- [ ] Gate-Vorbereitung: frischer Build und vollstaendige Regression bestanden
- [ ] Freigabe: unveraenderter Gate-Bericht vom Nutzer freigegeben
- [ ] vor v0.50.0: keine Release-Aktionen ausgefuehrt
- [ ] Dokumentation aktualisiert
- [ ] CHANGELOG aktualisiert
- [ ] keine Referenzimplementierung kopiert
- [ ] keine geschuetzten Binaerdaten hinzugefuegt
- [ ] keine Buildartefakte committed
```

## Aktuell empfohlener Einstieg

```text
v0.48 P0 - kombinierten KR-4847- bis KR-4850-Kernblock fokussiert validieren
```

Abgeschlossen und in Roadmap/Taskliste markiert sind `KR-4841`, `KR-4843`,
`KR-4844`, `KR-4845` und `KR-4846`. Der aktuelle Runtimevertrag steht auf
Runtime-ABI 35, BIOS-ABI 9 und Portprojektvertrag 21. Das verbindliche
XenonRecomp-artige Produktmodell rekompiliert `IP.BIN` und BootExecutable
statisch aus SH-4 in nativen PC-Code. Dreamcast-Komponenten bleiben typisierte,
titelunabhaengige Plattformgrenzen; das Freigabegate verbietet Interpreter/
JIT, Discplayer und Titelhacks. Der aktuelle Export linkt
`runtime-sh4-interpreter` jedoch noch bedingungslos. Das ist eine offene
`KR-4848`-Produktluecke, keine bereits erreichte Nicht-Emulationsgarantie.

Command 28/37 besitzt gastzeitgebundene PIO-/G1-DMA-Teiltransfers; Selector 5
ist ein DMA-IRQ-Handoff, Selector 11 die persistente PIO-
Callbackregistrierung. Das Taskfile trennt phasenweises PIO-DataIn/DataOut von
`CD_READ`-`DmaIn`: DMA besitzt kein PIO-DRQ und keinen Zwischen-IRQ, sondern
genau einen finalen Status-IRQ nach dem letzten Byte. SPI 11 bis 14, der
32-Byte-Modepuffer, der 10-Byte-`REQ_STAT`, persistenter Sense/CHECK,
gemeinsamer Laufwerksbesitz sowie
kontrollierte SET-FEATURES-Fehler liegen im aktuellen Block. Ausfuehrbare
Module halten kanonische aktive Extents und einen 4-KiB-Fast-Reject-Index.
Ein vorvalidierter Ersatz derselben Modul-ID ist atomar; seine
Negativregression erhaelt bei Ablehnung Katalog, Bloecke, Tracker, Provenienz
und Metriken.
Asynchrone BIOS-Completions umgehen das quittierbare Taskfile-IRQ-Latch;
persistenter Sense und ATA-`ERR` des aktuellen Kommandos sind getrennt. Der
GD-ROM-Fokustest ist nach diesem Integrationsfix 1/1 gruen. Read- und
TOC-Gastziele werden MMU-bewusst ueber ihre gesamte Laenge vorvalidiert;
ungueltige, ueberlaufende oder MMIO-Ziele enden ohne Teilwrite und Host-
Exception als `InvalidField`.

Der PVR-Nachweis laeuft am echten Scheduler-VBlank-In und friert erst nach
Wertrevalidierung einen exakten Frame ein. PAL/Interlace verwendet das aktive
SPG-Feld mit `FB_R_SOF1/2`; `SCALER_CTL` Bit 17/18 waehlt `FB_W_SOF1/2` fuer
den Render. Die Evidenz ist auf 256 Generationen, 64 MiB und 2.097.152
Pixelpruefungen pro VBlank begrenzt und besitzt einen Range-Fast-Reject. Der
Hintergrund-Overscan-Quad bildet HScale, D-Attribute und texturierte X/U-
Erweiterung ab. Der gemeinsame Proof-Pump trennt Gastbeweis und Host-Present;
die vier fokussierten PVR-/Framebuffer-/Hostvideo-/Portexporttests sind mit 12
Buildjobs 4/4 in 0,66 Sekunden gruen. Die kombinierte fokussierte Validierung
des gesamten Blocks besteht 12/12 mit 12 Buildjobs. Das ist nicht das spaetere
180-Test-Gate und beweist noch keinen privaten Sonic-Adventure-Frame.

Weiter offen:

```text
KR-4842: Wait-Loop-Klassifikation vervollstaendigen
KR-4847: EX-38/39-Vertrag und belegte Timeout-/Overrun-Grenzen schliessen
KR-4848: strukturierte Disc-Ladetransaktionen und latentes natives AOT
KR-4849/KR-4850: TA/PVR bis zum scanoutgebundenen echten Gastframe
```

Ein erster Gastframe ist noch nicht nachgewiesen. Moderne Xbox-, DualSense-
und vergleichbare Hostcontroller bleiben strikt hinter `KR-4850`. Waehrend
der Kernrunde nur fokussierte Targets ausfuehren; das konsolidierte Gate und
den privaten PAL-Lauf erst im vorgesehenen Block starten. Jeder Prozess endet
spaetestens nach 15 Minuten. Original-GDI und Tracks sind immer read-only und
werden nie geloescht; veraltete erzeugte Portordner duerfen nach eindeutiger
Pfadpruefung ersetzt werden. Keine Tags vor ausdruecklicher Nutzerfreigabe.
