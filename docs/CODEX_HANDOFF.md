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
Entwicklungsrechner gilt `--parallel 12`. Der CLI-Portbuild verwendet dafuer
`KATANA_HOST_BUILD_JOBS`; ohne expliziten Wert folgen
`KATANA_PORT_CODEGEN_JOBS` und danach die gemeldete CPU-Threadzahl. Unter
Windows kann `KATANA_HOST_BUILD_GENERATOR=Ninja` zusammen mit
`KATANA_HOST_BUILD_MAKE_PROGRAM` einen getrennten `build-ninja`-Hostbuild
waehlen.

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
Runtime-ABI 38, Backend-Interface-ABI 3, BIOS-ABI 9 und Portprojektvertrag 23.
Das verbindliche
XenonRecomp-artige Produktmodell rekompiliert `IP.BIN` und BootExecutable
statisch aus SH-4 in nativen PC-Code. Dreamcast-Komponenten bleiben typisierte,
titelunabhaengige Plattformgrenzen; das Freigabegate verbietet Interpreter/
JIT, Discplayer und Titelhacks. Der normale Produktport emittiert oder linkt
keinen SH-4-Interpreter mehr; ein fehlendes AOT-Ziel endet typisiert. Nur
`diagnostic_partial` enthaelt den begrenzten Diagnoseinterpreter und weist ihn
im Manifest aus. `KR-4848` bleibt trotzdem offen, bis strukturierte Disc-
Ladetransaktionen und die vorab erzeugte Registry latenter nativer Module
vorliegen.

Der laufende ABI-38-Block bindet G1-DMA-Faults mit Phase, Adresse, exakt
committed Praefix und Residue an den GD-ROM-CHECK-/Sense- und Requestzustand.
G1 wird vor jeder Benachrichtigung angehalten; interne Backend-/Schedulerfehler
duerfen kein ASIC-Hardwareereignis vortaeuschen. Store-Queue-`PREF` verwendet
bei `MMUCR.AT=0` QACR und bei `AT=1` die UTLB. Ausrichtung, `SQMD`, ASID/SV,
Schreibschutz, Dirty-Bit, Miss und Multiple-Hit werden atomar vor TA- oder
Speicherwirkung geprueft. `PREF` muss im C++-Emitter trotz leerem,
adressabhaengigem IR-Speichereffekt innerhalb der `MemoryAccessError`-Grenze
bleiben; die Regression prueft den SH-4-Exceptioneintritt explizit.

Nachfolgerlose generierte C++-Bloecke schreiben nun in jedem Backendmodus den
architektonischen Fallthrough-PC. Die kompilierte Regression deckt
Einzelblock, lokales Chaining und normalen Backendpfad ab. Die
Produktinvariante berechnet den erwarteten PC aus der tatsaechlichen
Terminatorquelle (`source + 2`) statt aus dem Wrapper-Eintritt; den frueheren
Fehlalarm einer gueltigen lokalen Blockkette nicht wieder einfuehren.

Der Relative16-Audit weist 87 Eintraege, 76 eindeutige Kandidaten und 73 im
vorherigen Port fehlende Ziele aus. Sie sind native Blockleader, waehrend der
live geladene `MOV.W`-/`BRAF`-Dispatch `RuntimeOnly` bleibt und keine erfundene
CFG-Kante erhaelt. Snapshotcache und P2-Aliasaufloesung sind imagegebunden;
lokale AOT-Blockketten melden die exakte letzte Terminatorquelle, Callsite,
Transferart und Siteklasse.

Der frische optimierte ABI-38-PAL-Export ist abgeschlossen: 140,9 Sekunden mit
zwoelf Jobs, 1.856 Funktionen, 37 Codepartitionen und Vertrag 23. Der
inkrementelle Reexport dauerte 29,2 Sekunden. Das Portpaket enthaelt null
Retailsektoren; die lokale Discinstallation war erfolgreich und die
unveraenderte Original-GDI blieb erhalten.

Der aktuelle Produktlauf erreicht 345.609.251 Gastzyklen und einen echten
Runtimehandler am architektonischen SH-4-Interruptvektor `VBR + 0x600`.
Frames, TA-Transfers und Gast-PVR-Frames bleiben null. Eine getrennte begrenzte
Diagnose beweist ueber Gastwriteprovenienz ein 56-Byte-Copy-plus-Patch-
Codetemplate und fuehrt daraus 19 bytebewiesene Runtimeinstruktionen aus. Sie
stoppt danach am naechsten noch nicht statisch gebundenen AOT-Einstieg. Keine
privaten Bytes oder Adressen als Produkthardcode uebernehmen; die allgemeine
native Bindung dieses Runtimecodes ist die naechste `KR-4848`-Arbeit und der
Task bleibt offen.

Der Projektschreiber shardet Dispatchregistries nach jeweils maximal 512
Bloecken, emittiert pro Owner und Shard genau einen Wrapper und routet
balanciert. Beim PAL-Port misst die zentrale `runtime-dispatch.cpp` dadurch
34.879 Byte/607 Zeilen statt 36.703.886 Byte/525.996 Zeilen; der groesste der
43 Shards misst 393.454 Byte. Die 513-Block-Regression prueft zwei Shards und
stale Cleanup; das vollstaendige synthetische Ninja-/MSVC-Projekt linkt in 15
Sekunden. Die fokussierte Suite besteht 6/6. Diesen Toolingfortschritt bei
weiteren grossen Ports beibehalten.

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
und Metriken. Ein Load-Write-Tracker bindet BIOS-/GD-Reloads an deren reale
Copy-/DMA-Writes: identische Bootbytes erhalten vorhandenes natives AOT;
geaenderte Bytes invalidieren exakt einmal.
Asynchrone BIOS-Completions umgehen das quittierbare Taskfile-IRQ-Latch;
persistenter Sense und ATA-`ERR` des aktuellen Kommandos sind getrennt. Der
GD-ROM-Fokustest ist nach diesem Integrationsfix 1/1 gruen. Read- und
TOC-Gastziele werden MMU-bewusst ueber ihre gesamte Laenge vorvalidiert;
ungueltige, ueberlaufende oder MMIO-Ziele enden ohne Teilwrite und Host-
Exception als `InvalidField`.

Der SH-4-DMAC-Channel-2-Pfad verwendet fuer TA den oeffentlichen externen
Memory-to-Device-Vertrag `RS=2`, 32-Byte-Einheiten, inkrementierende Quelle,
festes Ziel, Burstmodus und `DMAOR.DME+DDT`. Eine Runtime-End-to-End-Regression
fuehrt Haupt-RAM bis TA-Object-List/EOL; falsche Richtung und Cycle-Steal werden
sichtbar abgelehnt. Direct-Texture-Ziele `0x11`/`0x13` benoetigen fuer
mehrteilige Transfers noch eine fortschreitende Zieladresse und bleiben P1.

Der PVR-Nachweis laeuft am echten Scheduler-VBlank-In und friert erst nach
Wertrevalidierung einen exakten Frame ein. PAL/Interlace verwendet das aktive
SPG-Feld mit `FB_R_SOF1/2`; `SCALER_CTL` Bit 17/18 waehlt `FB_W_SOF1/2` fuer
den Render. Die Evidenz ist auf 256 Generationen, 64 MiB und 2.097.152
Pixelpruefungen pro VBlank begrenzt und besitzt einen Range-Fast-Reject. Der
Hintergrund-Overscan-Quad bildet HScale, D-Attribute und texturierte X/U-
Erweiterung ab. Der gemeinsame Proof-Pump trennt Gastbeweis und Host-Present;
die vier fokussierten PVR-/Framebuffer-/Hostvideo-/Portexporttests waren mit 12
Buildjobs 4/4 in 0,66 Sekunden gruen. Das aktuelle fokussierte Kern-Gate
besteht 9/9; nach der source-relativen Fallthroughkorrektur besteht der
Portexporttest zusaetzlich 1/1. Der konfigurierte 181-Test-Gesamtbestand wurde
in diesem Zwischenblock nicht vollstaendig ausgefuehrt und ist nicht als Gate
bestanden. Ein privater Sonic-Adventure-Frame ist weiterhin nicht bewiesen.

Weiter offen:

```text
KR-4842: Wait-Loop-Klassifikation vervollstaendigen
KR-4847: EX-38/39-Vertrag und belegte Timeout-/Overrun-Grenzen schliessen
KR-4848: strukturierte Disc-Ladetransaktionen und Registry latenter nativer Module
KR-4849: Direct-Texture-Zielprogression und restliche TA/PVR-Eingangskette
KR-4850: scanoutgebundener echter Gastframe
```

Die verbindliche Abhaengigkeitsfolge lautet fuer die offenen Pfade
`KR-4842 -> KR-4911 -> KR-4912 -> KR-4848`; danach implementiert `KR-4849`
den TA/PVR-Vertrag, `KR-4915` beweist dessen echten Gastpfad und erst
`KR-4850` bestaetigt den Frame. `KR-4814` und `KR-4914` folgen danach in
v0.49 auf der freigegebenen Framebasis.

Ein erster Gastframe ist noch nicht nachgewiesen. Moderne Xbox-, DualSense-
und vergleichbare Hostcontroller gehoeren zu `KR-4814`/`KR-4914` in v0.49 und
bleiben strikt hinter dem v0.48-Frame-Gate aus `KR-4850`. Waehrend
der Kernrunde nur fokussierte Targets ausfuehren; das konsolidierte Gate und
die abschliessende private PAL-Wiederholung erst im vorgesehenen Block starten.
Der bereits ausgefuehrte vorgezogene Lauf ist Diagnoseevidenz, kein Ersatz fuer
dieses Gate. Jeder Prozess endet
spaetestens nach 15 Minuten. Original-GDI und Tracks sind immer read-only und
werden nie geloescht; veraltete erzeugte Portordner duerfen nach eindeutiger
Pfadpruefung ersetzt werden. Keine Tags vor der Alpha.
