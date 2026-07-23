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

Meilensteinspezifische Ausnahme fuer v0.48: Bis alle v0.48-
Implementierungsaufgaben abgeschlossen sind, laufen ausschliesslich
fokussierte Builds und Regressionen. Das danach ausgefuehrte vollstaendige
Freigabegate besitzt seit 23.07.2026 eine Standing Approval. Ist der
unveraenderte finale Gatebericht vollstaendig gruen, gilt das Nutzerreview
automatisch als bestanden und v0.48 als erreicht sowie release-ready; ein
weiterer Review-Stopp oder eine erneute Freigabefrage entfaellt.

Interne Meilenstein- oder Release-Freigabe:

1. ausdrueckliche Nutzerfreigabe des unveraenderten Gate-Berichts pruefen;
   fuer v0.48 gilt stattdessen die dokumentierte Standing Approval
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
- bei Gate-Vorbereitungen: ausdruecklicher Hinweis auf den Review-Stopp oder
  die fuer den Meilenstein dokumentierte Standing Approval

## Pull-Request-Checkliste

```text
Task: KR-XXXX
Typ: Implementierung | Gate-Vorbereitung | interne Freigabe | Release-Gate

- [ ] Scope eingehalten
- [ ] Testanforderungen dokumentiert
- [ ] Gate-Vorbereitung: gesammelte Tests umgesetzt
- [ ] Gate-Vorbereitung: frischer Build und vollstaendige Regression bestanden
- [ ] Freigabe: unveraenderter Gate-Bericht freigegeben oder dokumentierte
      Standing Approval durch vollstaendig gruenes Gate erfuellt
- [ ] vor v0.50.0: keine Release-Aktionen ausgefuehrt
- [ ] Dokumentation aktualisiert
- [ ] CHANGELOG aktualisiert
- [ ] keine Referenzimplementierung kopiert
- [ ] keine geschuetzten Binaerdaten hinzugefuegt
- [ ] keine Buildartefakte committed
```

## Aktuell empfohlener Einstieg

```text
v0.48 P0 - nativen Spielhotspot nach dem Sega-Logo in KR-4851 aufloesen
```

Abgeschlossen und in Roadmap/Taskliste markiert sind `KR-4831`, `KR-4841`,
`KR-4842`, `KR-4843`, `KR-4844`, `KR-4845`, `KR-4846`, `KR-4848`,
`KR-4911`, `KR-4912`, `KR-4913`, `KR-4915` und `KR-4850`. Der aktuelle
Runtimevertrag steht auf Runtime-ABI 47, Block-ABI 3,
Backend-Interface-ABI 3, PlatformServices-ABI 10, BIOS-ABI 9,
Portprojektvertrag 31 und Host-Video-Vertrag 2.
Das verbindliche
XenonRecomp-artige Produktmodell rekompiliert `IP.BIN` und BootExecutable
statisch aus SH-4 in nativen PC-Code. Dreamcast-Komponenten bleiben typisierte,
titelunabhaengige Plattformgrenzen; das Freigabegate verbietet Interpreter/
JIT, Discplayer und Titelhacks. Der normale Produktport emittiert oder linkt
keinen SH-4-Interpreter mehr; ein fehlendes AOT-Ziel endet typisiert. Nur
`diagnostic_partial` enthaelt den begrenzten Diagnoseinterpreter und weist ihn
im Manifest aus. `KR-4848` ist mit strukturierten Disc-Ladetransaktionen und
der vorab erzeugten Registry latenter nativer Module abgeschlossen.

Der mit ABI 38 eingefuehrte Block bindet G1-DMA-Faults mit Phase, Adresse, exakt
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

Der vorangegangene optimierte ABI-38-PAL-Export ist abgeschlossen: 140,9
Sekunden mit zwoelf Jobs, 1.856 Funktionen, 37 Codepartitionen und Vertrag 23.
Der inkrementelle Reexport dauerte 29,2 Sekunden. Das Portpaket enthaelt null
Retailsektoren; die lokale Discinstallation war erfolgreich und die
unveraenderte Original-GDI blieb erhalten.

Ein vorangegangener Produktlauf erreicht 345.609.251 Gastzyklen und einen echten
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
Eine am Ende des 64-Bit-Gastzeitraums nicht mehr planbare PACKET-Completion
endet jetzt kontrolliert mit `READY|ERR`, ABRT-Sense, finalem Command-IRQ und
freigegebenem Laufwerksbesitz. Dieselbe Admission-Grenze liefert fuer
BIOS-Read und -Streaming einen einmaligen `Aborted`-Vierwortstatus mit null
Bytes; ein Folgerequest bleibt zulaessig. Das ist noch nicht der vollstaendige
laufende G1-Timeout-/Overrun-Vertrag.

Freie Speicherprobes sind MMU-bewusst und strukturell auf echte lineare
Haupt-RAM-, VRAM- und AICA-RAM-Backings begrenzt. `Memory::peek_u32` weist
auch ein versehentlich erlaubtes `MmioMemoryDevice` vor dessen Handler ab.
Flash wird ohne einen eigenen expliziten Side-Effect-Free-Peek-Vertrag nicht
mehr angeboten. Peek-Aufloesung veraendert weder CPU-/Exceptionzustand noch
MMIO-Handler, Observer, Watchpoints oder Speicherzaehler. Das Last-MMIO-
Tracking ist im Gast-Hotpath ein allokationsfreier POD; erst der terminale
Bericht materialisiert den Regionsstring. PVR- und Systembus-Snapshots bewegen
auch pending Render-/Channel-2-Zustaende nicht.

Runtime-ABI 42 bindet einen POD-Zugriffssink fuer bereits ausgefuehrte
Gastzugriffe. AOT und begrenzter Diagnoseinterpreter tragen Quell- und
Laufzeit-PC; PlatformServices-ABI 10 reicht die `PREF`-Herkunft bis zur Store
Queue. PVR-Render und PVR-YUV bleiben getrennte Writer-Urspruenge, VRAM32 wird
auf das gemeinsame lineare Backing projiziert. Der Sink darf beobachtete
Readwerte oder MMIO-Handler nicht erneut abfragen. Nur fuer die
No-op-Klassifikation eines Wrapperwrites darf der aktive Trace vor dem Write
das seiteneffektfreie lineare Backing vergleichen. Produktobserver und
Scanout-Evidenz muessen dabei konservativ und bei Trace aus/an identisch
bleiben.

`RuntimeWaitLoopTrace` v1 verdichtet Wertlaeufe und Writer begrenzt. Der
Portexport erzeugt aus genau einem Hardwareaudit generische, deterministisch
deduplizierte Guard- und Kandidatendeskriptoren; reine Counterloops werden
ausgelassen. Ein vorab sortierter Read-Site-Index verhindert lineare
Deskriptorscans, und auch MMIO-Werte stammen nur aus dem bereits ausgefuehrten
Zugriff. Lineare bytegenaue Writerlinks muessen
`exact-backing-bytes`, nichtlineare physische MMIO-Ueberschneidungen dagegen
`physical-range-candidate` melden. Backing-indizierte Locations muessen
unbeteiligte lineare Writes ohne Vollscan verwerfen. Der aktive Trace muss
skalare und Range-Wrapperaenderungen bytegenau bestimmen und No-op-Writer
verwerfen, ohne den konservativen Produktvertrag umzuschreiben.
Ausschliesslich
`KATANA_PORT_WAIT_LOOP_TRACE=1` aktiviert den
Rohwerttrace, unabhaengig von `KATANA_PORT_DIAGNOSTICS`. Bei leerer
Deskriptorliste werden weder Recorder noch Sink erzeugt. Sonst muss der Port
einmalig auf `stderr` vor nur lokaler Nutzung, rohen Gastwerten und
ungeprueftem Teilen warnen. Das JSON muss
`contains_raw_guest_values:true`, `writer_scope:"since-previous-sample"` und
ungueltige skalare Range-Werte als `scalar_value_valid:false` mit `value:null`
ausweisen. Strukturell ungueltige Access-Events muessen
`invalid_access_events` erhoehen und `complete:false` erzwingen; sie duerfen
nicht als bloss irrelevante gueltige Events gelten. Der RAII-Besitzer entfernt
den Sink vor der terminalen JSON-Ausgabe. Ohne Trace-Opt-in muss der Fastpath
ohne Recorderallokation und Zugriffsprojektion bleiben.
Keine Titeladressen, privaten Pfade oder Retaildaten in Deskriptoren,
Regressionen oder Dokumentation uebernehmen.

Die Registervarianten von `PREF`, `OCBI`, `OCBP`, `OCBWB` und `TAS.B` sind
auch im begrenzten Interpreter geschlossen; doppelte `FMOV`-Speicherzugriffe
laufen low nach high. Der normale Produktport bleibt davon unberuehrt und
AOT-only.

Hardware-Audit-Schema 4 erkennt Natural Loops ueber skalierbare
Dominatorberechnung, klassifiziert Counter-, RAM-Poll-, MMIO-Poll-, Mixed- und
Unknown-Loops und liefert Access-/Guard-Evidenz. Der Auditor deckt GBR-MOVs,
`TST.B` als Read, `AND.B`/`XOR.B`/`OR.B` und `TAS.B` als RMW, FMOV,
PC-relative `MOV.W`/`MOV.L`, `STC.L`/`LDC.L` und `MAC.W`/`MAC.L` ab; die
unbekannte FPSCR.SZ-Lage wird fuer FMOV konservativ als Adressunion ausgegeben.
Teilweise bekannte MAC-Basen bleiben einzeln sichtbar; Predecrement wrappt auf
32 Bit. OCRAM ist kein linearer RAM-Poll. Guard-Provenienz folgt T-neutralen
Instruktionen und eindeutigen Vorgaengern und stoppt an echten T-Schreibern
oder Merges. Unaufgeloeste Reads und konservative Kandidaten einer
unvollstaendigen Condition-Domaene bleiben sichtbar. FMOV-/FCMP-Faelle ohne
vollstaendigen FPU-Modus-/Bankbeweis bleiben `unknown` und erhalten kein
`guards_loop`. `--strict` lehnt partielle Hardwareadressen und diese
unresolved Poll-/Guard-Loops ab, `--fail-on-gap` bleibt unveraendert.
Einzelbilder tragen `scope=executable_image`, Disc-Audits
`scope=native_disc_aot_boot_graph`. Area-3-Haupt-RAM-Spiegel werden
kanonisiert; Delay-Slot-Doppelkontexte, wurzellose SCCs und ein
4.096-Block-Graph besitzen Regressionen.

Der aktuelle private CLI-Disc-Audit ist unter Schema 4 und
`native_disc_aot_boot_graph` im normalen Modus gruen. Er berichtet 142.380
Instruktionen, 1.542 Funktionen, null unbekannte Instruktionen, null bekannte
Luecken, zwei partielle Adressen, 1.095 Loops und 492
`unresolved_poll_guard_loops`. `--strict` bleibt damit erwartungsgemaess rot.
Der vorangegangene fokussierte Zwischenblock bestand 22/22 in 1,57 Sekunden;
der Port-CLI-Nachweis bestand 1/1 in 151,12 Sekunden.

Runtime-ABI 43 und Portprojektvertrag 27 binden
`katana.runtime-probe` Version 1 mit Device-Schema 1, 35 produktiven
Geraeteinstanzen, 867 kanonischen Feldern und `fnv1a64-le-v1`. Der private
Diagnose=0/1-A/B-Runner bestand zwei Laeufe mit 100.000 Gastzyklen und 120
Sekunden Hosttimeout. Die normativen Felder waren gleich, Executable und
Disc-Pack blieben unveraendert, Systemreplay v3 war vollstaendig und
versiegelt, und beide Laeufe erzeugten null Wait-Loop-Tracezeilen. Damit ist
`KR-4842` abgeschlossen; `KR-4911` war an diesem Zwischenstand freigegeben,
aber noch offen.
Es lief keine Vollsuite und kein `KR-4852`.

Der damalige Systemreplay-Zwischenstand unter Schema 3 besitzt eine feste,
konfigurierbare Kapazitaet von
standardmaessig 4.096 und hoechstens 65.536 Ereignissen; portable
Ereigniscodes sind auf 64 Zeichen begrenzt. Ein gesaettigter Recordversuch
zaehlt exakt einen Drop. Jeder Drop verbietet Versiegelung und Replay, waehrend
ein bereits versiegelter Log unveraenderlich bleibt. Codes, Adressen, Werte,
numerische Payloads und Hashes bleiben intern fuer Ereignishash und
Replayvergleich exakt. Das Standard-JSON redigiert `code`, `address`, `value`,
`detail`, `auxiliary`, `event_hash` und `final_guest_state_hash`; nur ein
ausdrueckliches lokales Opt-in serialisiert sie.

Runtime-ABI 44 und Portprojektvertrag 28 schliessen darauf aufbauend
`KR-4911`. Systemreplay-Schema 4 erweitert `deterministic-v1` auf zwoelf
Pflichtklassen und bindet Blockdispatch, Gastexception, kontrollierten Fallback
und Gastcheckpoint an die bisherigen acht Klassen. Die zentrale
Observation-Session schreibt diese Ereignisse gegen Gastzyklus und Resetepoche;
GD-ROM-, DMA-, PVR- und AICA-Schedulercallbacks besitzen stabile Codes.
Checkpoints laufen strikt monoton von `runtime-started` ueber
`guest-program-entered`, `first-guest-frame` und `guest-input-interactive` bis
`controlled-retail-scene`.

Die typisierten Endklassen umfassen `budget-reached`, `hang`,
`guest-exception`, `dispatch-miss` und `failed`. First-Fault und letzter
stabiler Checkpoint sichern intern vollstaendige CPU-Snapshots und frieren nach
dem ersten Fehler ein; das Fault-v1-JSON gibt nur allowlist-redigierte Klassen-
und Checkpointfelder aus. Der private A/B-Runner validiert die parsebaren
Fault- und Checkpointzeilen strikt und schreibt private Fehlerpakete ausserhalb
des Repositorys atomar und write-once.

Das fokussierte Gate bestand 8/8 in 6,60 Sekunden,
`katana-port-cli-tests` 1/1 in 155,67 Sekunden. Der frische private PAL-A/B-
Lauf bestand 2/2 mit 100.000 Gastzyklen und 120 Sekunden Hosttimeout:
normative Felder und letzter Checkpoint waren gleich, Executable, Disc-Pack,
Original-GDI und Tracks blieben unveraendert, beide Replays vollstaendig und
versiegelt und die Tracezaehler null/null. Es lief keine Vollsuite und kein
`KR-4852`. `KR-4912` ist freigegeben.

Runtime-ABI 45 und Portprojektvertrag 29 schliessen `KR-4912`. Load,
Relocation, Replace und Unload erzeugen monotone Modulinkarnationen;
byteidentische Multi-Extent-Loads sowie byteidentische CPU-, FPU-,
Store-Queue-, Copy- und DMA-Writes erhalten bestehende Bloecke und Provenienz.
Ein bewiesener Runtime-Write-Snapshot darf kontrolliert um einen
zusammenhaengenden Tail samt Delay Slot wachsen. MMU-sichere P0-/P1-/P2-Aliase
teilen dieselbe physische Blockherkunft.

Replace und Unload gleichen Materializer-Origins, Runtime-Blocktabellen und
Code-Tracker gemeinsam ab, ohne fremde Owner in Multi-Extent-Luecken zu
invalidieren. Ein ueberlaufender Relocation-Generation-Zaehler wird vor jeder
Mutation atomar abgelehnt. Identische Validierungssnapshots werden geteilt,
gegen das Speicherbudget gerechnet und nach der letzten Origin freigegeben;
retained, peak und reclaimed Bytes bleiben sichtbar. Eine tatsaechliche
Materialisierung erzeugt ihr Replay-Ereignis unabhaengig vom
Diagnose-Sampling. Oeffentliche Probe-, Fault- und
Materialisierungsberichte redigieren Identitaeten und Gastbytes. Der normale
Produktport bleibt interpreterfrei und beendet unbekannten Code typisiert.

Die fokussierten Regressionen bestanden 10/10 in 1,27 Sekunden; der
interpreterfreie Produkt-E2E bestand 1/1 in 229,03 Sekunden. Fuer `KR-4912`
lief weder ein privater Retaillauf noch eine Vollsuite oder `KR-4852`.
An diesem damaligen Zwischenstand blieb `KR-4848` fuer strukturierte Disc-
Ladetransaktionen und die Registry vorab erzeugter latenter AOT-Module offen;
der Task ist inzwischen abgeschlossen.

Runtime-ABI 47 und Portprojektvertrag 31 schliessen `KR-4913`.
Systemreplay-Schema 5 trennt `ExactEvents` von `DigestStream`; der
Produktprobe behaelt 4.096 Praefixzeugen, bindet aber jedes Ereignis an
Zeitvalidierung, Coverage, Klassenzaehler und den geordneten FNV-Digest.
Runtime-Probe-Schema 2 weist Gesamt-, behaltene und zusammengefasste
Ereignisse sowie `exact_event_stream` aus. Der ungekeyte Digest ist
deterministische Evidenz, keine Authentisierung. Der vorbereitete
Gastprogrammbereich prueft den MMU-bewussten BootExecutable-Eintritt ohne
wiederholte Kanonisierung direkter P1-/P2-Pfade.

Zwei frische Diagnose-aus/an-Probes mit je 356.000.000 Gastzyklen banden je
137.057.656 Ereignisse ohne Drop und waren zusammen nach 175,3 Sekunden
vollstaendig und versiegelt. Normative Felder und letzter Checkpoint waren
identisch; die Artefakte blieben unveraendert. Ein direkter
Bestaetigungslauf emittierte `runtime-started` und
`guest-program-entered` und endete nach 83,9 Sekunden kontrolliert. Kein
Fallback, Trap oder stiller Fehler galt als Erfolg. Der frische Port umfasst
1.873 Funktionen, 37 Partitionen und null Retailsektoren; die lokale
Installation umfasst drei Tracks und 521.461 Sektoren, die Original-GDI blieb
unveraendert. Der weitere sichtbare Stillstand nach dem Sega-Logo ist der
naechste native Hotspot in `KR-4851`.

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
Read- und Write-Framebuffer teilen die logische 32-Bit-VRAM-Abbildung.
Backing-Byte-adressierte Dirty-Evidenz plus das vorherige Scanout-Abbild
verhindern sichtbare False-Proofs durch Offscreen-Writes, unveraenderte
Bilddaten oder Blanking.

Der aktuelle private Sonic-Adventure-PAL-AOT-Lauf erreicht aus dem
recompilierten `IP.BIN`-Direct-Framebuffer innerhalb eines
50-Millionen-Gastzyklusbudgets in 5,3 Sekunden `KR_FIRST_GUEST_FRAME` und
`KR_FIRST_PRESENTED_FRAME`; TA bleibt null. Der anschliessende Budget-Exit ist
erwartet. BootExecutable, Spielboot, `KR-4848` und der produktive TA-Pfad
bleiben offen.

Das vorangegangene fokussierte Kern-Gate bestand 11/11. Der x64-Kern-/Runtime-Build
der Desktop-GUI-off-Konfiguration ist mit zwoelf parallelen Jobs gruen; deren
vollstaendiges CTest-Zwischengate auf Quellstand `924ea89` besteht 183/183
Eintraege in 312,97 Sekunden, darunter 181 regulaere Passes und zwei erwartete
Regex-`PASS_REGULAR_EXPRESSION`-Erfolge. Desktop-GUI- und Harness-Tests sind
nicht Teil dieser 183; der Runner-Selbsttest ist separat gruen. Ein einmaliger
Vorlauf ohne korrekt
geladene x64-`VsDevCmd`-Umgebung scheiterte bereits am
Standardbibliotheksinclude `cstdint`; der identische Lauf in der korrekt
geladenen Toolchain ist vollstaendig gruen. Dies war ein Umgebungsfehler und
kein Quellcodefehler. Das Zwischengate ist keine Erledigung von `KR-4852`,
`KR-4853` oder `KR-4854`.

Der private Retail-Runner bezieht Runtime-ABI und Portprojektvertrag strikt und
jeweils genau einmal aus der kanonischen `cmake/KatanaVersions.cmake`.
Fehlende, doppelte, ungueltige oder nullwertige Definitionen sowie JSON-Strings
und Gleitkommazahlen anstelle ganzzahliger Vertragswerte werden ohne
Typkoerzierung abgelehnt. Ein mit Sanitizern gebautes statisches Runtime-SDK
exportiert seine erforderlichen Compiler- und Linkoptionen als
`KatanaRecomp::runtime`-Usage-Requirements; der installierte Out-of-Tree-
Verbraucher erbt damit dasselbe ASan-ABI-Profil. Das ist ein Teilstand von
`KR-4801`, nicht dessen Abschluss.

Historische ABI-39-Portevidenz: Nach dem damaligen 178/178-Zwischengate wurde
der Vertrag-24-Port unter Runtime-ABI 39 und Block-ABI 3 frisch neu exportiert
und gebaut: 1.860 Funktionen, 37
Codepartitionen und null Retailsektoren. Die lokale read-only Installation der
Originaldisc umfasst drei Tracks und 521.461 Sektoren. Der abschliessende
50-Millionen-Lauf reproduziert beide Framemarker mit `frames=2`,
`pvr_guest_frames=2`, `pvr_direct_frames=2` und 302.287 geaenderten
Direct-FB-Pixeln. TA, Rendergeneration und Materializer bleiben null; der
Budget-Exit ist erwartet.

Weiter offen:

```text
KR-4847: EX-38/39-Vertrag und laufende G1-Timeout-/Overrun-Grenzen schliessen
KR-4848: strukturierte Disc-Ladetransaktionen und Registry latenter nativer Module
KR-4849: Direct-Texture-Zielprogression und restliche TA/PVR-Eingangskette
```

`KR-4842`, `KR-4911` und `KR-4912` sind als Vorbedingungen erfuellt. Die
verbindliche Abhaengigkeitsfolge fuer die offenen Bootpfade beginnt jetzt mit
`KR-4848`; `KR-4849` schliesst den
produktiven TA/PVR-Vertrag. `KR-4915` und `KR-4850` sind durch den legitimen
vorgezogenen Direct-Framebuffer-Pfad bereits erfuellt. `KR-4814` und
`KR-4914` folgen als verbindliche Post-Frame-Arbeit in v0.48 auf `KR-4850`
und muessen vor `KR-4852` abgeschlossen sein.

Ein erster Gastframe und sein Host-Present sind nachgewiesen; BootExecutable
und Spielboot sind es noch nicht. Moderne Xbox-, DualSense- und vergleichbare
Hostcontroller gehoeren zu `KR-4814`/`KR-4914` in v0.48; Spielboot bleibt
dabei P0, Controllerintegration P1. In der Kernrunde nur fokussierte Targets
ausfuehren. Das einzige finale Vollgate laeuft nach allen v0.48-
Implementierungen in `KR-4852`; `KR-4853` uebernimmt dessen unveraenderten
Bericht und fuehrt weder Build noch Test erneut aus. Der bereits ausgefuehrte
vorgezogene Lauf ist Diagnoseevidenz, kein Ersatz fuer Spielboot oder
Freigabe. Jeder Prozess endet
spaetestens nach 15 Minuten. Original-GDI und Tracks sind immer read-only und
werden nie geloescht; veraltete erzeugte Portordner duerfen nach eindeutiger
Pfadpruefung ersetzt werden. Keine Tags vor der Alpha.
