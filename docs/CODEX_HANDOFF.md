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

1. sauberen Git-Status pruefen
2. aktuellen Branch und Version erfassen
3. Tasktyp bestimmen: Implementierung, Gate-Vorbereitung, interne
   Meilenstein-Freigabe oder Release-Gate
4. Abhaengigkeiten, aktuellen Status und vorhandene Gate-Berichte erfassen
5. erst danach Dateien aendern

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

### Lokaler Sonic-Adventure-Akzeptanztest

Es gilt die verbindliche Strategie in
`docs/SONIC_ADVENTURE_ACCEPTANCE.md`:

- Sonic Adventure wird vor v0.50.0 nicht ausgefuehrt. Alle frueheren Gates
  verwenden ausschliesslich synthetische Fixtures und frei lizenzierte
  Homebrew-Programme.
- Eine lokale GDI darf vor Alpha read-only validiert, analysiert, rekompiliert
  und bis `game.exe` gebaut werden; gestartet wird sie erstmals in KR-4999.
- KR-4999 muss den Pfad GDI -> externes Port-Projekt -> `game.exe` ->
  `SA_ALPHA_BOOTED` reproduzierbar nachweisen.
- Ein Frame, Menue oder interaktives Gameplay ist kein Alpha-Pflichtkriterium.
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
Gate-Vorbereitung wird nach den Korrekturen vollstaendig wiederholt. Der lokale
Sonic-Adventure-Lauf findet ausschliesslich in KR-4999 statt.

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
Review-Stopp nach KR-4402 - Phase-10-Gate-Bericht mit dem Nutzer pruefen
```

Danach:

```text
Erst nach ausdruecklicher Freigabe: KR-4403 - interne GUI-/GDI-Meilenstein-Freigabe
```

Phase 10 erreicht im unveraenderten Gate-Commit `024990b` mit 159/159 Tests
`KR_PHASE10_GUI_END_TO_END`. GUI und CLI erzeugen dieselbe Projektidentitaet
und dieselben Kernartefakte; das interne GUI-Paket besteht den standalone
Smoke-Test. Das Logo ist hash- und herkunftsgebunden eingebunden, bleibt aber
bis KR-4902 von oeffentlicher Verteilung ausgeschlossen. KR-4403 wurde nicht
begonnen und darf ohne ausdrueckliche Nutzerfreigabe nicht starten.
