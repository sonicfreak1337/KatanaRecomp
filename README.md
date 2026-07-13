# KatanaRecomp

Aktuelle Pre-Alpha-Version: `0.12.0`

KatanaRecomp ist ein unabhaengiges, in C++20 entwickeltes Framework fuer die statische Rekompilierung von Sega-Dreamcast-SH-4-Code.

Das Projekt befindet sich in einer fruehen Pre-Alpha-Phase. Der aktuelle Stand ist **Version 0.12.0**.

KatanaRecomp ist kein Emulator, kein ISO-Loader und kein Paket fuer kommerzielle Spieldaten. BIOS-Dateien, Disc-Images, urheberrechtlich geschuetzte Assets und automatisch erzeugter Code aus kommerziellen Spielen gehoeren nicht in dieses Repository.

## Aktueller Status

Der aktuelle Prototyp kann kleine SH-4-Binaerprogramme einlesen, analysieren, in eine eigene Zwischenrepraesentation ueberfuehren, als C++ ausgeben, kompilieren und automatisiert ausfuehren.

Der End-to-End-Pfad lautet derzeit:

`	ext
SH-4-Binaerdaten
    -> Little-Endian-Reader
    -> SH-4-Decoder
    -> Disassembly
    -> Kontrollflussanalyse
    -> Basic Blocks
    -> Funktionsanalyse
    -> Katana-IR
    -> C++-Codegenerator
    -> nativer Test-Build
    -> semantischer Laufzeittest
`

Der aktuelle Teststand umfasst **30 automatische Tests**.

## Implementierte SH-4-Instruktionen

### Allgemein

- NOP
- RTS

### Register und Immediate

- MOV #imm,Rn
- ADD #imm,Rn
- MOV Rm,Rn
- ADD Rm,Rn

### Speicherzugriffe

- MOV.B Rm,@Rn
- MOV.W Rm,@Rn
- MOV.L Rm,@Rn
- MOV.B @Rm,Rn
- MOV.W @Rm,Rn
- MOV.L @Rm,Rn

Byte- und Word-Loads werden mit korrekter Vorzeichenerweiterung behandelt. Mehrbyte-Zugriffe verwenden Little Endian.

### Kontrollfluss

- BRA
- BSR
- BT
- BF
- BT/S
- BF/S
- JMP @Rn
- JSR @Rn
- RTS

Direkte Sprungziele werden berechnet. Delay Slots werden erkannt und in der Analyse sowie im generierten C++ beruecksichtigt.

### Statusregister und T-Bit

- CLRT
- SETT
- CMP/EQ #imm,R0
- CMP/EQ Rm,Rn
- TST #imm,R0
- TST Rm,Rn

Bedingte Spruenge koennen damit bereits auf zuvor berechnete T-Bit-Zustaende reagieren.

### Ein-Bit-Shifts

- SHLL Rn
- SHLR Rn
- SHAL Rn
- SHAR Rn

Das herausgeschobene Bit wird in T uebernommen. SHAR erhaelt das Vorzeichenbit ohne implementation-defined signed Shift in C++.
### Feste Mehrfach-Shifts

- SHLL2 Rn
- SHLL8 Rn
- SHLL16 Rn
- SHLR2 Rn
- SHLR8 Rn
- SHLR16 Rn

Diese Instruktionen verschieben um eine feste Distanz und veraendern das T-Bit nicht.
### Rotationen

- ROTL Rn
- ROTR Rn
- ROTCL Rn
- ROTCR Rn

ROTL und ROTR rotieren innerhalb des Registers. ROTCL und ROTCR verwenden T als zusaetzliches Carry-Bit und schreiben das herausrotierte Bit zurueck nach T.
### Dynamische Shifts

- SHAD Rm,Rn
- SHLD Rm,Rn

Positive Zaehler verschieben nach links, negative Zaehler nach rechts. Nur die relevanten unteren Zaehlerbits werden verwendet. Negative Vielfache von 32 besitzen definierte Sonderfaelle. Die vollstaendige Semantik steht in `docs/SH4_SHIFT_SEMANTICS.md`.
## Analysefunktionen

KatanaRecomp unterstuetzt aktuell:

- lineare Disassembly mit virtueller Basisadresse
- Erkennung unbekannter Opcodes
- direkte Sprungzielberechnung
- Delay-Slot-Markierung
- Basic-Block-Bildung
- intraprozedurale Kontrollflusskanten
- Trennung von Callgraph und normalem Kontrollfluss
- Erkennung direkter BSR-Aufrufziele
- Erfassung indirekter JSR-Aufrufstellen
- Zuordnung von Basic Blocks zu Funktionen
- Absenkung in eine eigene Katana-IR

## C++-Codegenerator

Der C++-Emitter erzeugt aktuell:

- einen einfachen SH-4-CPU-Zustand
- 16 allgemeine Register
- PC, PR und T-Bit
- einen begrenzten Runtime-Speicher
- Bounds-Checks
- Register- und Immediate-Operationen
- Speicherzugriffe
- direkte Funktionsaufrufe
- bedingte und unbedingte Spruenge
- Returns
- Delay-Slot-Ausfuehrung
- Fehlerpfade fuer noch nicht aufgeloeste indirekte Spruenge und Calls

Generierter Code wird in den End-to-End-Tests automatisch kompiliert und ausgefuehrt.

## T-Bit-Semantik

Die Carry-, Borrow- und signed-Overflow-Regeln sind in `docs/SH4_STATUS_SEMANTICS.md` dokumentiert und durch Grenzwerttests abgesichert.
## Noch nicht implementiert

KatanaRecomp ist noch kein vollstaendiger Dreamcast-Recompiler. Unter anderem fehlen:

- grosse Teile des SH-4-Befehlssatzes
- Multiplikation, Division, weitere Shifts und erweiterte Integer-Flags
- FPU- und Vektoroperationen
- vollstaendiges Statusregister
- Dreamcast-Speicherabbild
- MMIO und Hardware-Register
- Ausnahmen und Interrupts
- MMU und Cache-Verhalten
- PVR, AICA, GD-ROM und weitere Dreamcast-Hardware
- Loader fuer reale Executable-Formate
- Relocations und Symbole
- robuste indirekte Sprungzielanalyse
- Jump Tables
- Selbstmodifizierender Code
- Optimierungspasses
- produktionsreife Runtime

## Voraussetzungen

Unter Windows wird derzeit folgende Toolchain verwendet:

- Visual Studio 2022 Build Tools
- MSVC x64
- CMake 3.25 oder neuer
- Ninja
- Git
- C++20

## Build

In einer x64 Developer PowerShell:

`powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
`

## CLI

Einzelnen Opcode dekodieren:

`powershell
.\build\katana-recomp.exe E1FF
`

Binaerdatei disassemblieren:

`powershell
.\build\katana-recomp.exe disasm ".\samples\decoder_demo.bin" 8C010000
`

Basic Blocks anzeigen:

`powershell
.\build\katana-recomp.exe blocks ".\samples\basic_blocks_demo.bin" 8C010000
`

Funktionen analysieren:

`powershell
.\build\katana-recomp.exe functions ".\samples\functions_demo.bin" 8C010000 8C010000
`

Katana-IR anzeigen:

`powershell
.\build\katana-recomp.exe ir ".\samples\ir_demo.bin" 8C010000 8C010000
`

C++ erzeugen:

`powershell
.\build\katana-recomp.exe emit-cpp
    ".\samples\codegen_demo.bin"
    8C010000
    ".\generated\codegen_demo.cpp"
    8C010000
`

## Projektstruktur

`	ext
include/katana/
    analysis/      Oeffentliche Analyse-APIs
    codegen/       C++-Emitter
    io/            Binaerdatei-Leser
    ir/            Katana-Zwischenrepraesentation
    sh4/           SH-4-Instruktionen und Decoder

src/
    analysis/      Disassembly, CFG und Funktionsanalyse
    cli/           Kommandozeilenprogramm
    codegen/       C++-Codegenerator
    decoder/       SH-4-Decoder
    io/            Binaerdatei-Zugriff
    ir/            IR-Lowering
    runtime/       Zukuenftige Runtime-Komponenten

tests/
    basic_blocks/
    codegen/
    control_flow/
    decoder/
    disassembler/
    fixtures/
    functions/
    ir/
    memory/
    status/
`

## Versionierung

KatanaRecomp verwendet ab Version $version Semantic Versioning:

- MAJOR: inkompatible Architektur- oder API-Aenderungen
- MINOR: neue Decoder-, Analyse-, IR- oder Runtime-Funktionen
- PATCH: Fehlerkorrekturen ohne neue Funktionalitaet

Solange das Projekt unter 1.0.0 liegt, sind interne APIs ausdruecklich instabil.

Die aktuelle Version steht gleichzeitig in:

- VERSION
- CMakeLists.txt
- Git-Tag X.Y.Z
- CHANGELOG.md

Der Helfer 	ools\release-version.ps1 aktualisiert VERSION und CMake und kann einen Git-Tag erzeugen.

## Naechste technische Ziele

1. weitere Integer- und Bitoperationen
2. Shifts und Rotationen
3. Multiplikation und MAC-Instruktionen
4. weitere Adressierungsarten
5. echtes Dreamcast-Speicherabbild
6. Aufloesung indirekter Calls und Jump Tables
7. Trennung von generierter Runtime und generiertem Programmcode
8. erste Optimierungspasses auf Katana-IR
9. FPU-Unterstuetzung
10. Loader fuer eigenstaendig bereitgestellte Testprogramme


## Roadmap und Arbeitsuebergabe

- `ROADMAP.md`: langfristige technische Phasen und Release-Gates
- `docs/TASKS.md`: issue-taugliche Task-IDs mit Abhaengigkeiten
- `docs/CODEX_HANDOFF.md`: verbindliche Arbeitsregeln fuer Codex
- `docs/TASK_TEMPLATE.md`: Vorlage fuer neue Arbeitspakete
- `tools/next-roadmap-task.ps1`: zeigt den naechsten offenen Task

Naechsten offenen Task anzeigen:

```powershell
.\tools\next-roadmap-task.ps1
```
## Rechtlicher Rahmen

KatanaRecomp wird unabhaengig entwickelt. Referenzprojekte duerfen zum Verstaendnis allgemeiner Arbeitsablaeufe untersucht werden, ihr Code wird jedoch nicht uebernommen.

Das Repository soll ausschliesslich selbst entwickelte Framework-, Test- und Dokumentationsdateien enthalten.