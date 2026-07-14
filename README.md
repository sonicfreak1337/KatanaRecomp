# KatanaRecomp

Aktuelle Pre-Alpha-Version: `0.20.0`

KatanaRecomp ist ein unabhaengiges, in C++20 entwickeltes Framework fuer die statische Rekompilierung von Sega-Dreamcast-SH-4-Code.

Das Projekt befindet sich in einer fruehen Pre-Alpha-Phase. Der aktuelle Stand ist **Version 0.20.0**.

KatanaRecomp ist kein Emulator, kein ISO-Loader und kein Paket fuer kommerzielle Spieldaten. BIOS-Dateien, Disc-Images, urheberrechtlich geschuetzte Assets und automatisch erzeugter Code aus kommerziellen Spielen gehoeren nicht in dieses Repository.

## Aktueller Status

Der aktuelle Prototyp kann kleine SH-4-Binaerprogramme einlesen, analysieren, in eine eigene Zwischenrepraesentation ueberfuehren, als C++ ausgeben, kompilieren und automatisiert ausfuehren.

Der End-to-End-Pfad lautet derzeit:

`	ext
SH-4-Binaerdaten
    -> Raw- oder ELF32-SH-Loader
    -> Executable Image mit Segmenten
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

Der aktuelle Teststand umfasst **83 automatische Tests**.

Der v0.15-Decoder verwendet eine zentrale Metadatenquelle fuer alle implementierten Opcode-Masken, Operandenformate, Kontrollfluss- und Privileginformationen. `katana-recomp isa-report` berichtet deterministisch ueber den gesamten 16-Bit-Opcode-Raum; Kollisions-, Spezifikations- und Fuzztests sichern die Regeln ab.

v0.16 fuehrt ein formatneutrales Executable Image mit Code-, Daten- und Unknown-Segmenten ein. Raw-Binaries und Little-Endian-ELF32-SH-Dateien werden mit virtuellen Adressen, Dateioffsets, Berechtigungen und Einstiegspunkten geladen. ELF-Symbole, optionale Map-Dateien, minimale `R_SH_DIR32`-/`R_SH_REL32`-Relocations und ein strikt versioniertes Projektmanifest v1 sind abgedeckt. Der normale Analyzerpfad dekodiert nur ausfuehrbare Code-Segmente.

v0.17 entdeckt Code rekursiv ab Image-Einstiegspunkten, direkten Calls und Funktionssymbolen. Analyseberichte trennen Code, Daten und unbekannte beziehungsweise unerreichbare Bereiche, begruenden Funktionskandidaten und melden mehrdeutige Delay-Slot-Rollen. `katana-recomp analyze <Manifest>` gibt diesen Bericht aus; `disasm` bleibt der lineare Diagnosemodus.

v0.18 verfolgt lokale Registerkonstanten und loest einfache indirekte `JMP`-/`JSR`-Ziele nur bei nachgewiesenem ausfuehrbarem Code auf. Bekannte, begrenzte absolute Jump Tables werden vollstaendig validiert. `katana-recomp analyze <Manifest> [Override-Datei]` trennt sichere und offene Stellen und unterstuetzt deterministische, versionierte Nutzerhinweise.

v0.19 fuehrt Katana-IR Version 2 mit expliziten Operandbreiten, Status-, Speicher- und Akkumulatoreffekten sowie normalisierten Delay Slots ein. Ein verpflichtender Verifier lehnt ungueltige Funktionen vor Codegen ab. `katana-recomp ir` und `ir-json` liefern deterministisch sortierte, vollstaendige IR-Dumps.

v0.20 fuehrt eine feste Pipeline fuer konservatives Constant Folding, Copy
Propagation, Dead-Code-Elimination, CFG- und Load-Store-Vereinfachungen ein.
Alle Paesse bleiben einzeln schaltbar, verifizieren ihre IR-Grenzen und erhalten
sichtbare CPU- und Speichereffekte. `emit-cpp --no-opt` deaktiviert die Pipeline;
`--dump-ir` schreibt deterministische Vorher-/Nachher-Dumps.

KR-2101 beginnt die v0.21-Runtime-Grundlage: Speicher, CPU-Zustand und sichtbare
Fehlerpfade liegen in `KatanaRecomp::runtime`. Generierter C++-Code enthaelt nur
noch die SH-4-Programmlogik, bindet `katana/runtime/runtime.hpp` ein und prueft
die Runtime-ABI beim Kompilieren.

KR-2102 vervollstaendigt den zentralen CPU-Zustand um getrennte 16er-Baenke fuer
`FR` und `XF`, das Interrupt-Ereignisregister `INTEVT` und benannte
Registeranzahlen. Die Layoutaenderung hebt die Runtime-ABI auf Version 2.

KR-2103 fuehrt einen wiederholbaren CPU-Reset ein. Der Standardzustand setzt alle
CPU-Register und Runtime-Flags auf null; eine Reset-Konfiguration kann PC, R15,
VBR, SR und FPSCR vorgeben. Der Runtime-Speicher bleibt bei einem CPU-Reset
erhalten. Details stehen in `docs/RUNTIME.md`.

## Implementierte SH-4-Instruktionen

### Allgemein

- NOP
- RTS
- RTE
- TRAPA #imm
- SLEEP

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

`TRAPA` sichert SR, PC und R15 und erzeugt einen sichtbaren Trapzustand. `RTE` stellt SSR und SPC mit spezifikationsgemaesser Delay-Slot-Reihenfolge wieder her. `SLEEP` haelt die generierte Ausfuehrung in einem expliziten Schlafzustand an. Details stehen in `docs/SH4_SYSTEM_CONTROL_SEMANTICS.md`.

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
### Einfache Multiplikation

- MUL.L Rm,Rn
- MULS.W Rm,Rn
- MULU.W Rm,Rn

Die Ergebnisse werden nach MACL geschrieben. MUL.L behaelt die unteren 32 Produktbits. MULS.W und MULU.W verwenden nur die unteren 16 Bit der beiden Quellregister. Die vollstaendige Semantik steht in `docs/SH4_MULTIPLICATION_SEMANTICS.md`.
### Doppelte Multiplikation

- DMULS.L Rm,Rn
- DMULU.L Rm,Rn

Die vollstaendigen 64-Bit-Produkte werden in `MACH:MACL` abgelegt. DMULS.L verwendet vorzeichenbehaftete 32-Bit-Operanden, DMULU.L vorzeichenlose. Die Semantik und Grenzwerte stehen in `docs/SH4_MULTIPLICATION_SEMANTICS.md`.
### Multiply-Accumulate

- MAC.W @Rm+,@Rn+
- MAC.L @Rm+,@Rn+
- SETS
- CLRS

Die MAC-Instruktionen lesen signed Operanden aus dem Speicher, schalten beide Adressregister fort und addieren das Produkt zu `MACH:MACL`. Das S-Bit aktiviert die 32-Bit-Saettigung fuer MAC.W beziehungsweise die 48-Bit-Saettigung fuer MAC.L. Die vollstaendige Semantik steht in `docs/SH4_MULTIPLICATION_SEMANTICS.md`.
### Division

- DIV0U
- DIV0S Rm,Rn
- DIV1 Rm,Rn

Der generierte CPU-Zustand modelliert Q, M und T explizit. DIV0U und DIV0S initialisieren den iterativen Divisionszustand; DIV1 fuehrt exakt einen bitweisen Hardware-Divisionsschritt aus. Die vollstaendige Zustandstabelle und die Referenzvektoren stehen in `docs/SH4_DIVISION_SEMANTICS.md`.

### Pre-Decrement und Post-Increment

- MOV.B Rm,@-Rn
- MOV.W Rm,@-Rn
- MOV.L Rm,@-Rn
- MOV.B @Rm+,Rn
- MOV.W @Rm+,Rn
- MOV.L @Rm+,Rn

Pre-Decrement sichert den alten Quellwert und verringert das Adressregister vor dem Store. Post-Increment liest zuerst und erhoeht das Adressregister nur dann, wenn Quell- und Zielregister verschieden sind. Die vollstaendige Semantik steht in `docs/SH4_ADDRESSING_SEMANTICS.md`.

### Register-Displacements

- MOV.B R0,@(disp,Rn)
- MOV.W R0,@(disp,Rn)
- MOV.L Rm,@(disp,Rn)
- MOV.B @(disp,Rm),R0
- MOV.W @(disp,Rm),R0
- MOV.L @(disp,Rm),Rn

Das unsigned 4-Bit-Displacement wird fuer Byte, Word und Long mit 1, 2 beziehungsweise 4 skaliert. Die effektive Adresse verwendet definiertes 32-Bit-Wraparound; Byte- und Word-Loads werden vorzeichenerweitert. Details und Grenzfaelle stehen in `docs/SH4_ADDRESSING_SEMANTICS.md`.

### R0-indexierte Adressierung

- MOV.B Rm,@(R0,Rn)
- MOV.W Rm,@(R0,Rn)
- MOV.L Rm,@(R0,Rn)
- MOV.B @(R0,Rm),Rn
- MOV.W @(R0,Rm),Rn
- MOV.L @(R0,Rm),Rn

Die effektive Adresse ist die modulo-2-hoch-32-Summe aus R0 und dem Basisregister. Alle Basis- und Quellregister bleiben unveraendert; auch Ueberlappungen mit R0 werden in definierter Reihenfolge ausgewertet.

### GBR-relative Adressierung

- MOV.B R0,@(disp,GBR)
- MOV.W R0,@(disp,GBR)
- MOV.L R0,@(disp,GBR)
- MOV.B @(disp,GBR),R0
- MOV.W @(disp,GBR),R0
- MOV.L @(disp,GBR),R0

Das unsigned 8-Bit-Displacement wird breitenabhaengig skaliert. Der generierte CPU-Zustand modelliert GBR explizit; GBR bleibt bei allen Zugriffen unveraendert.

### PC-relative Loads und MOVA

- MOV.W @(disp,PC),Rn
- MOV.L @(disp,PC),Rn
- MOVA @(disp,PC),R0

`MOV.W` verwendet `PC + 4` und skaliert das unsigned 8-Bit-Displacement mit zwei. `MOV.L` und `MOVA` richten den Instruktions-PC zuerst auf vier Byte aus und skalieren mit vier. Word-Loads werden vorzeichenerweitert; `MOVA` schreibt die berechnete Adresse ohne Speicherzugriff nach R0.

### Systemregistertransfers

- STS und STS.L
- LDS und LDS.L
- STC und STC.L
- LDC und LDC.L

Direkte und speicherbasierte Transfers decken MACH, MACL, PR, FPUL, FPSCR sowie die SH-4-Kontrollregister und Registerbanken ab. SR- und FPSCR-Schreibwerte werden spezifikationsgemaess maskiert; SR.RB wechselt die aktive Registerbank. Privilegierte Formen sind in Decoder und IR markiert. Der genaue Runtime-Vertrag steht in `docs/SH4_SYSTEM_CONTROL_SEMANTICS.md`.

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
- PC, System- und Kontrollregister, Registerbanken sowie T-, S-, Q- und M-Bit
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
- weitere Integer- und Systemregister-Flags
- FPU- und Vektoroperationen
- vollstaendiges Statusregister
- Dreamcast-Speicherabbild
- MMIO und Hardware-Register
- Ausnahmen und Interrupts
- MMU und Cache-Verhalten
- PVR, AICA, GD-ROM und weitere Dreamcast-Hardware
- weitere Executable-Formate und dynamisches Linken
- Relocation-Typen jenseits der minimalen SH-32-Bit-Unterstuetzung
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

ISA-Abdeckungsbericht erzeugen:

`powershell
.\build\katana-recomp.exe isa-report
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

Katana-IR als maschinenlesbares JSON anzeigen:

`powershell
.\build\katana-recomp.exe ir-json ".\samples\ir_demo.bin" 8C010000 8C010000
`

C++ erzeugen:

`powershell
.\build\katana-recomp.exe emit-cpp
    ".\samples\codegen_demo.bin"
    8C010000
    ".\generated\codegen_demo.cpp"
    8C010000
`

`emit-cpp` fuehrt die sicheren IR-Optimierungen standardmaessig aus. Fuer einen
unoptimierten Debug-Lauf kann `--no-opt` angehaengt werden. Mit
`--dump-ir ".\generated\pipeline"` entstehen deterministische Dumps als
`pipeline.before.ir` und `pipeline.after.ir`.

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

1. neutrales Executable-Image- und Segmentmodell
2. Raw-Binary-Loader auf dem Image-Modell
3. ELF32-SH-Loader
4. Symbole, Map-Dateien und minimale Relocations
5. versioniertes Projektmanifest fuer Eingaben und Adresslayout
6. rekursive Codeentdeckung
7. Aufloesung indirekter Calls und Jump Tables


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
