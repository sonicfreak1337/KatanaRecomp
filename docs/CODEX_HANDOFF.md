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
3. Build konfigurieren
4. komplette Testsuite ausfuehren
5. erst danach Dateien aendern

Windows-Basisbefehle:

```powershell
git status
git branch --show-current

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

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

Jeder Semantik-Task braucht normalerweise:

1. Decoder-Test
2. IR-Lowering-Test
3. Codegenerator-Test
4. generierten End-to-End-Test
5. Grenz- oder Fehlerfall

Tests muessen deterministisch sein.

### Lokaler Sonic-Adventure-Akzeptanztest

Ab Phase 6 gilt zusaetzlich die verbindliche Strategie in
`docs/SONIC_ADVENTURE_ACCEPTANCE.md`:

- Der vollstaendige lokale Test laeuft nur an den Phasenabschluessen v0.31.0,
  v0.34.0, v0.37.0, v0.40.0 und v0.44.0 sowie am Alpha-Gate v0.50.0.
- Der v0.30.0-GDI-Smoke wird nicht separat wiederholt, sondern bei v0.31.0
  kumulativ nach den neuen messbaren Kriterien geprueft.
- Einzelne Tasks, Commits und Zwischenreleases erhalten keinen vollstaendigen
  Sonic-Adventure-Test. Ihre Unit-, Integrations- und Regressionstests bleiben
  verpflichtend.
- Fehlt der lokale Dump, wird der optionale lokale Test sauber uebersprungen;
  oeffentliche CI und verteilbare Tests duerfen ihn nie voraussetzen.
- Jede kuenftige Phase implementiert nur die dort vorgesehenen allgemeinen
  Zaehler, Checkpoints und versionierten maschinenlesbaren Berichte.
- Keine Spieldaten, Captures, Audioinhalte, Dump-Hashes oder lokalen Pfade
  committen oder in Release-Artefakte aufnehmen.
- Keine fest codierten Sonic-Adventure-Adressen, Remaps, Patches oder
  titelbezogenen Runtime-Sonderfaelle implementieren.
- Vor jedem Gate-Commit Git-Diff und Index ausdruecklich auf geschuetzte Daten,
  lokale Pfade und generierte Builddateien pruefen.

Bevor ein Task als fertig gilt:

```powershell
cmake --build build
ctest --test-dir build --output-on-failure
```

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

1. Task-ID lesen
2. Abhaengigkeiten pruefen
3. betroffene Schichten nennen
4. bestehende Tests ausfuehren
5. minimale Implementierung
6. neue Tests
7. komplette Regression
8. Dokumentation aktualisieren
9. Task in `docs/TASKS.md` auf `[x]` setzen
10. CHANGELOG unter `Unreleased` aktualisieren
11. Commit mit Task-ID

Ein vollstaendiger Sonic-Adventure-Lauf wird in diesem Ablauf nur ergaenzt,
wenn der Task das verbindliche Phasen- oder Alpha-Gate abschliesst.

Commit-Beispiel:

```text
KR-1101 SUB NEG und NOT implementieren
```

## Erwartete Abschlussmeldung

Die Abschlussmeldung enthaelt:

- Task-ID
- geaenderte Schichten
- neue Semantik
- neue Tests
- Testergebnis
- bekannte Einschraenkungen
- naechsten nicht blockierten Task

## Pull-Request-Checkliste

```text
Task: KR-XXXX

- [ ] Scope eingehalten
- [ ] bestehende Tests bestanden
- [ ] neue Tests hinzugefuegt
- [ ] Grenzfaelle getestet
- [ ] Dokumentation aktualisiert
- [ ] CHANGELOG aktualisiert
- [ ] keine Referenzimplementierung kopiert
- [ ] keine geschuetzten Binaerdaten hinzugefuegt
- [ ] keine Buildartefakte committed
```

## Aktuell empfohlener Einstieg

```text
v0.30.0 - Release-Gate fuer GD-ROM und Dateisystem
```

Danach:

```text
KR-3101 - Event-Scheduler
```
