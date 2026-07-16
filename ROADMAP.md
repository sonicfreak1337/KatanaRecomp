# KatanaRecomp Roadmap

Status: Pre-Alpha
Aktuelle Basis: v0.34.0
Planungsmodell: Semantic Versioning, kleine ueberpruefbare Meilensteine

Diese Roadmap beschreibt die technische Entwicklung von KatanaRecomp vom aktuellen Architektur-Prototyp bis zu einem belastbaren Dreamcast-Recompiler-Framework.

Sie ist absichtlich in kleine, voneinander abhaengige Releases und Task-IDs zerlegt. Dadurch koennen Menschen, Codex und andere Werkzeuge jeweils genau einen begrenzten Arbeitsauftrag uebernehmen, ohne nebenbei die halbe Architektur neu zu erfinden.

## Leitprinzipien

1. Korrektheit vor Geschwindigkeit.
2. Jede neue Semantik braucht automatische Tests; sie werden gesammelt im
   letzten Gate-Vorbereitungstask einer Phase erstellt und ausgefuehrt.
3. Decoder, Analyse, IR, Codegenerator und Runtime bleiben getrennte Schichten.
4. Generierter Code darf keine versteckten Abhaengigkeiten auf den Analyzer besitzen.
5. Unbekannte oder nicht sicher aufgeloeste Faelle muessen sichtbar fehlschlagen.
6. Keine kommerziellen Binaerdateien, BIOS-Dateien oder urheberrechtlich geschuetzten Assets im Repository.
7. Referenzprojekte dienen nur zum Verstaendnis allgemeiner Ablaeufe. Code wird unabhaengig implementiert.
8. Synthetische Fixtures und frei lizenzierte Homebrew-Programme bilden die Testbasis.
9. Keine grosse Refaktorierung ohne eigenen Roadmap-Task und Regressionstests.
10. Ein Release gilt erst als fertig, wenn Build, Tests und Dokumentation gemeinsam passen.

## Statuslegende

- `[x]` abgeschlossen
- `[ ]` offen
- `[~]` in Arbeit
- `[!]` blockiert
- `[?]` Forschungsaufgabe oder Architekturentscheidung

## Fertiggestellte Basis

### v0.1.0 bis v0.10.0

- [x] C++20-Projektstruktur mit CMake und Ninja
- [x] erster SH-4-Decoder
- [x] Little-Endian-Binaerdatei-Leser
- [x] lineare Disassembly
- [x] direkte Sprungzielberechnung
- [x] Delay-Slot-Erkennung
- [x] Basic Blocks
- [x] intraprozeduraler Kontrollflussgraph
- [x] Funktionsanalyse und direkter Callgraph
- [x] eigene Katana-IR
- [x] C++-Codegenerator
- [x] automatisch kompilierter End-to-End-Test
- [x] erste Speicherzugriffe
- [x] T-Bit, Vergleiche und bedingte Spruenge
- [x] 10 automatische Tests
- [x] README, CHANGELOG, VERSION und Statusdokument

## Release-Linie

| Bereich | Zielversionen | Ergebnis |
|---|---:|---|
| SH-4 Integer-Kern | 0.11 bis 0.15 | belastbare Integer-ISA und Decoder-Validierung |
| Loader und Programmanalyse | 0.16 bis 0.18 | echte Images, rekursive Analyse, indirekter Kontrollfluss |
| IR und Optimierung | 0.19 bis 0.20 | verifizierbare IR und erste sichere Optimierungen |
| Runtime und Speicherbus | 0.21 bis 0.23 | vollstaendiger CPU-Zustand, Bus, Ausnahmen und Interrupts |
| SH-4 FPU | 0.24 bis 0.25 | FPU-Grundlage und Dreamcast-relevante Spezialoperationen |
| Dreamcast-Plattform | 0.26 bis 0.31 | Boot, Eingabe, Grafik, Audio, GD-ROM und Scheduling |
| Codegen und Buildsystem | 0.32 bis 0.34 | modulare Backends, Cache, indirekter Dispatch |
| Werkzeuge und Qualitaet | 0.35 bis 0.37 | Manifest, Port-Projektexport, Diagnostik, Fuzzing und reproduzierbare Debug-Gates |
| Kompatibilitaet und Leistung | 0.38 bis 0.40 | Homebrew-Vertical-Slice und erster oeffentlicher Pre-Alpha-Stand |
| Desktop-GUI und Quellworkflow | 0.41 bis 0.44 | vollstaendiger Alpha-Workflow fuer Projektanlage, `.gdi`-Quellen und Analyse |
| Alpha-Integration und Haertung | 0.45 bis 0.49 | ISA-Abdeckung, Retail-Boot, native Hostruntime, Portintegration und Alpha-CI |
| Alpha | 0.50.0 | zusammenhaengende Dreamcast-Programme laufen reproduzierbar |
| Beta | 0.75.0 | ausgewaehlte reale Programme sind spielbar und debuggbar |
| Stabil | 1.0.0 | dokumentierter, reproduzierbarer und stabiler Framework-Release |

## Phase 1: SH-4 Integer-Kern

### [x] v0.11.0 - ALU und Statussemantik

Ziel: haeufige Integer- und Bitoperationen vollstaendig durch Decoder, IR, Codegenerator und End-to-End-Tests fuehren.

Fortschritt:

- [x] KR-1101 - SUB, NEG und NOT
- [x] KR-1102 - AND, OR und XOR
- [x] KR-1103 - CMP-Varianten
- [x] KR-1104 - Carry und Overflow
- [x] KR-1105 - Extend, Swap und XTRCT
- [x] KR-1106 - DT und MOVT
- [x] KR-1107 - v0.11 Release-Gate

Enthalten:

- `SUB`, `NEG`, `NEGC`, `NOT`
- `AND`, `OR`, `XOR`
- `ADDC`, `ADDV`, `SUBC`, `SUBV`
- weitere `CMP`-Varianten
- `DT`, `MOVT`
- `EXTU`, `EXTS`
- `SWAP`, `XTRCT`
- explizite T-Bit-, Carry- und Overflow-Semantik

Release-Gate:

- jede neue Instruktion besitzt Decoder-, IR- und Laufzeittests
- vorzeichenbehaftete und vorzeichenlose Grenzwerte werden getestet
- kein bestehender End-to-End-Test veraendert sein Ergebnis

### [x] v0.12.0 - Shifts und Rotationen
Fortschritt:

- [x] KR-1201 - Ein-Bit-Shifts
- [x] KR-1202 - Feste Mehrfach-Shifts
- [x] KR-1203 - Rotationen
- [x] KR-1204 - Dynamische Shifts
- [x] KR-1205 - v0.12 Release-Gate

Enthalten:

- `SHLL`, `SHLR`, `SHAL`, `SHAR`
- `SHLL2`, `SHLL8`, `SHLL16`
- `SHLR2`, `SHLR8`, `SHLR16`
- `ROTL`, `ROTR`, `ROTCL`, `ROTCR`
- `SHAD`, `SHLD`

Release-Gate:

- T-Bit-Aenderungen sind bitgenau getestet
- dynamische Shift-Werte decken positive, negative und Grenzfaelle ab

### [x] v0.13.0 - Multiplikation, Division und MAC
Fortschritt:

- [x] KR-1301 - Einfache Multiplikation
- [x] KR-1302 - Doppelte Multiplikation
- [x] KR-1303 - MAC-Instruktionen
- [x] KR-1304 - Division
- [x] KR-1305 - v0.13 Release-Gate

Enthalten:

- `MUL.L`
- `MULS.W`, `MULU.W`
- `DMULS.L`, `DMULU.L`
- `MAC.W`, `MAC.L`
- `DIV0U`, `DIV0S`, `DIV1`
- `MACH` und `MACL`

Release-Gate:

- 64-Bit-Zwischenergebnisse sind plattformunabhaengig
- Q-, M- und T-Bit-Semantik ist explizit modelliert
- Division besitzt Referenzvektoren fuer Sonderfaelle

### [x] v0.14.0 - Adressierungsarten und Systemregister

Fortschritt:

- [x] KR-1401 - Pre-Decrement und Post-Increment
- [x] KR-1402 - Register-Displacements
- [x] KR-1403 - R0-indexierte Adressierung
- [x] KR-1404 - GBR-relative Adressierung
- [x] KR-1405 - PC-relative Loads und MOVA
- [x] KR-1406 - Systemregistertransfers
- [x] KR-1407 - Privilegierte Kontrollinstruktionen
- [x] KR-1408 - v0.14 Release-Gate

Enthalten:

- Pre-Decrement-Stores
- Post-Increment-Loads
- Register-Displacements
- R0-indexierte Adressierung
- GBR-relative Adressierung
- PC-relative Loads
- `MOVA`
- `STS`, `LDS`, `STC`, `LDC`
- `TRAPA`, `RTE`, `SLEEP` als dekodierte und klar modellierte Runtime-Pfade

Release-Gate:

- Adressberechnung ist aus der Instruktionssemantik herausgeloest
- Ausrichtungs- und Wraparound-Faelle sind getestet
- privilegierte Instruktionen sind markiert
- KR-1402 bis KR-1405 besitzen Binaer-Fixtures und durchlaufen den normalen CLI-Codegen-Pfad

### [x] v0.15.0 - Decoder-Haertung und ISA-Abdeckung

Fortschritt:

- [x] KR-1501 - Zentrale Instruktionsmetadaten
- [x] KR-1502 - Decoder-Kollisionspruefung
- [x] KR-1503 - ISA-Abdeckungsbericht
- [x] KR-1504 - Spezifikations-Testvektoren
- [x] KR-1505 - Decoder-Fuzzer
- [x] KR-1506 - v0.15 Release-Gate

Enthalten:

- zentrale Instruktionsmetadaten
- Opcode-Masken, Format, Operanden und Privilegstatus in einer Quelle
- automatischer Abdeckungsbericht
- Tests fuer kollidierende Masken
- Tests fuer ungueltige Opcodes
- Fuzzing des Decoders
- unabhaengige Testvektoren gegen die SH-4-Spezifikation

Release-Gate:

- keine mehrdeutigen Decoder-Regeln
- jede implementierte Instruktion ist im Abdeckungsbericht sichtbar
- unbekannte Opcodes bleiben deterministisch unbekannt

## Phase 2: Loader und Programmanalyse

### [x] v0.16.0 - Executable-Image-Modell

Fortschritt:

- [x] KR-1601 - Image- und Segmentmodell
- [x] KR-1602 - Raw-Binary-Loader
- [x] KR-1603 - ELF32-SH-Loader
- [x] KR-1604 - Symbole und Map-Dateien
- [x] KR-1605 - Relocations
- [x] KR-1606 - Projektmanifest Version 1
- [x] KR-1607 - v0.16 Release-Gate

Enthalten:

- neutrales Image-Modell fuer Segmente
- Basisadresse, Dateioffset, Groesse und Berechtigungen
- Raw-Binary-Loader
- ELF32-SH-Loader
- Einstiegspunkte
- Symbole und optionale Map-Dateien
- minimale Relocation-Unterstuetzung
- Projektmanifest fuer Eingabedateien und Adresslayout

Release-Gate:

- Analyzer arbeitet nicht mehr direkt auf einer einzigen flachen Datei
- Segmente koennen Code, Daten oder unbekannt sein
- Loaderfehler nennen Datei, Offset und Ursache

### [x] v0.17.0 - Rekursive Codeentdeckung

Fortschritt:

- [x] KR-1701 - Worklist ab Einstiegspunkten
- [x] KR-1702 - Code-Daten-Klassifikation
- [x] KR-1703 - Herkunft und Konfidenz von Funktionen
- [x] KR-1704 - Nicht erreichbare Bereiche
- [x] KR-1705 - Ueberlappende Bereiche
- [x] KR-1706 - Analysebericht
- [x] KR-1707 - v0.17 Release-Gate

Enthalten:

- Worklist-Analyse ab bekannten Einstiegspunkten
- Trennung von erreichbarem Code, Daten und unbekannten Bytes
- Erkennung nicht erreichbarer Codebereiche
- Funktionskandidaten mit Konfidenz
- kontrollierter Umgang mit ueberlappenden Bereichen
- Analysebericht mit Gruenden fuer jede entdeckte Funktion

Release-Gate:

- Datenbytes werden nicht mehr blind als Instruktionen dekodiert
- jede entdeckte Funktion besitzt eine nachvollziehbare Herkunft
- lineare Analyse bleibt als Diagnosemodus erhalten

### [x] v0.18.0 - Indirekter Kontrollfluss und Jump Tables

Fortschritt:

- [x] KR-1801 - Lokale Konstantenpropagation
- [x] KR-1802 - Registerwertanalyse
- [x] KR-1803 - Einfache indirekte Calls und Jumps
- [x] KR-1804 - Jump Tables
- [x] KR-1805 - Override-Datei
- [x] KR-1806 - Bericht ungeloester Kontrollflussstellen
- [x] KR-1807 - v0.18 Release-Gate

Enthalten:

- lokale Konstanten- und Registerwertanalyse
- Aufloesung einfacher `JMP @Rn`- und `JSR @Rn`-Ziele
- Jump-Table-Erkennung
- Bereichsanalyse fuer Sprungziele
- Nutzerhinweise fuer nicht aufloesbare Stellen
- Override-Datei fuer manuelle Funktions- und Sprungziele
- Bericht ueber ungeloeste Kontrollflussstellen

Release-Gate:

- aufgeloeste und nicht aufgeloeste Ziele sind getrennt sichtbar
- kein Ziel wird ohne Beweis als sicher markiert
- Overrides sind versionierbar und deterministisch

## Phase 3: Katana-IR

### [x] v0.19.0 - IR Version 2

Fortschritt:

- [x] KR-1901 - Explizite Operandbreiten
- [x] KR-1902 - Explizite Statusregistereffekte
- [x] KR-1903 - Speicher-Seiteneffekte
- [x] KR-1904 - Delay-Slot-Normalisierung
- [x] KR-1905 - IR-Verifier
- [x] KR-1906 - Deterministische Text- und JSON-Ausgabe
- [x] KR-1907 - v0.19 Release-Gate

Enthalten:

- explizite Operandbreiten
- explizite vorzeichenbehaftete und vorzeichenlose Operationen
- explizite Statusregistereffekte
- explizite Speicher-Seiteneffekte
- normalisierte Delay-Slot-Darstellung
- IR-Verifier
- stabile Textausgabe
- optionale maschinenlesbare JSON-Ausgabe

Release-Gate:

- ungueltige IR wird vor Codegen abgelehnt
- jede Funktion kann unabhaengig verifiziert werden
- IR-Dumps sind deterministisch

### [x] v0.20.0 - Sichere Basisoptimierungen

Fortschritt:

- [x] KR-2001 - Constant Folding
- [x] KR-2002 - Copy Propagation
- [x] KR-2003 - Dead-Code-Elimination
- [x] KR-2004 - CFG-Simplifizierung
- [x] KR-2005 - Load-Store-Vereinfachung
- [x] KR-2006 - Pass-Pipeline und Debug-Schalter
- [x] KR-2007 - v0.20 Release-Gate

Enthalten:

- Constant Folding
- Copy Propagation
- Dead-Code-Elimination
- CFG-Simplifizierung
- Entfernen leerer Bloecke
- einfache Load-Store-Vereinfachung
- Pass-Pipeline mit einzeln schaltbaren Paessen
- Vorher-Nachher-Dumps

Release-Gate:

- jede Optimierung besitzt Aequivalenztests
- Optimierungen duerfen keine sichtbaren CPU- oder Speichereffekte entfernen
- Debug-Build kann alle Paesse deaktivieren

## Phase 4: Runtime-Grundlage

### [x] v0.21.0 - Runtime-Trennung und vollstaendiger CPU-Zustand

Fortschritt:

- [x] KR-2101 - Runtime aus generiertem Code auslagern
- [x] KR-2102 - Vollstaendigen CPU-Zustand zentralisieren
- [x] KR-2103 - Deterministischen Reset-Zustand definieren
- [x] KR-2104 - v0.21.0 Release-Gate

Enthalten:

- Runtime als eigene Bibliothek
- generierter Code enthaelt keine Runtime-Implementierung mehr
- allgemeine Register
- banked Register
- `SR`, `GBR`, `VBR`, `SSR`, `SPC`, `SGR`, `DBR`
- `MACH`, `MACL`, `PR`
- `FPUL`, `FPSCR`
- FPU-Registerspeicher
- deterministischer Reset-Zustand
- versionierte Runtime-ABI

Release-Gate:

- generierte Programme koennen gegen eine feste Runtime gelinkt werden
- CPU-Zustand ist zentral definiert
- ABI-Inkompatibilitaeten werden erkannt

### [x] v0.22.0 - Dreamcast-Speicherbus

Fortschritt:

- [x] KR-2201 - Regionbasierter Bus
- [x] KR-2202 - RAM und Spiegelungen
- [x] KR-2203 - VRAM und AICA-RAM-Abstraktionen
- [x] KR-2204 - BIOS- und Flash-Abstraktionen
- [x] KR-2205 - MMIO-Handler
- [x] KR-2206 - Ausrichtung, Fehler und Watchpoints
- [x] KR-2207 - v0.22 Release-Gate

Enthalten:

- zentrale Adressdekodierung
- RAM, VRAM, AICA-RAM, BIOS- und Flash-Abstraktionen
- Spiegelungen und Aliase
- MMIO-Handler
- Ausrichtungsregeln
- lesbare Fehler fuer ungueltige Bereiche
- optionale Watchpoints und Tracing

Release-Gate:

- direkter Zugriff auf einen flachen Byte-Vektor verschwindet aus generiertem Code
- jeder Speicherbereich ist registriert und testbar
- Little-Endian-Verhalten ist zentral garantiert

### [x] v0.23.0 - Ausnahmen und Interrupts

Fortschritt:

- [x] KR-2301 - SR-Felder und Interruptmasken
- [x] KR-2302 - Exception-Eintritt
- [x] KR-2303 - Interrupt-Controller
- [x] KR-2304 - TRAPA und RTE
- [x] KR-2305 - Delay-Slot-Ausnahmen
- [x] KR-2306 - v0.23 Release-Gate
- [x] KR-2307 - Ausfuehrbare Delay-Slot- und Interrupt-Review-Regressionen

Enthalten:

- relevante SR-Felder
- Exception-Vektoren
- Interruptprioritaeten
- `TRAPA`
- `RTE`
- Fehler im Delay Slot
- Bus- und Adressfehler
- testbarer Interrupt-Controller

Release-Gate:

- Exceptions speichern und restaurieren den definierten CPU-Zustand
- Delay-Slot-Ausnahmen besitzen eigene Tests
- keine Ausnahme verschwindet als generische C++-Exception

Nachgelagerte Review-Absicherung nach v0.23.0:

- Delay-Slot-Ausnahmen werden im kompilierten generierten C++ fuer alle sechs
  verzoegerten unbedingten Kontrollfluesse ausgefuehrt
- RTE-Registerbankzustand, illegale Slot-Instruktionen und verschachtelte
  Exception-Propagation sind regressionsgesichert
- Interrupt-Prioritaetsgleichstand und Grenzlevel sind regressionsgesichert

## Phase 5: SH-4 FPU

### [x] v0.24.0 - FPU-Grundoperationen

Fortschritt:

- [x] KR-2401 - FR- und XF-Baenke
- [x] KR-2402 - Single-Precision-Arithmetik
- [x] KR-2403 - Vergleiche und Konvertierungen
- [x] KR-2404 - FPSCR-Modi
- [x] KR-2405 - Double-Precision
- [x] KR-2406 - v0.24.0 Release-Gate

Enthalten:

- FR- und XF-Baenke
- Single-Precision-Arithmetik
- Double-Precision-Modus
- Vergleiche
- Konvertierungen mit FPUL
- Loads und Stores
- `FPSCR`-Modi `PR`, `SZ` und `FR`

Release-Gate:

- Host-Floating-Point wird nur verwendet, wenn die SH-4-Semantik erhalten bleibt
- NaN-, Infinity- und Rundungsfaelle sind getestet
- frische lokale Debug- und Release-Builds bestehen vollstaendig; CI ist erst
  zum Alpha-Gate wieder verpflichtend

### [x] v0.25.0 - Dreamcast-relevante FPU-Spezialoperationen

Fortschritt:

- [x] KR-2501 - FSCA und FSRRA
- [x] KR-2502 - FIPR und FTRV
- [x] KR-2503 - NaN, Rundung und Sonderwerte
- [x] KR-2504 - FPU-Konformitaetssuite

Enthalten:

- `FSCA`
- `FIPR`
- `FTRV`
- `FSRRA`
- Vektor- und Matrixregistersicht
- Bankwechsel
- Denormal- und Sonderwertverhalten

Release-Gate:

- bekannte Grafik- und Mathe-Testvektoren laufen bitnah oder dokumentiert toleranzbasiert
- Abweichungen zwischen Hostplattformen werden erkannt
- frische lokale Debug- und Release-Builds bestehen vollstaendig; CI ist erst
  zum Alpha-Gate wieder verpflichtend

## Build-, Test- und Gate-Strategie

Innerhalb einer Phase werden die Fach-Tasks zuerst ohne routinemaessigen Build
und ohne Testlauf abgearbeitet. Testanforderungen werden dabei nur gesammelt.
Der ausdruecklich benannte letzte Gate-Vorbereitungstask erstellt oder
vervollstaendigt anschliessend alle Tests der Phase und fuehrt genau einen
frischen Build sowie die vollstaendige Regression aus. Auf der Festplatte
bleibt nur `build-current/` mit den aktuellen Gate-Artefakten.

Vor jedem Phasen-Release-Gate folgt nach erfolgreicher Gate-Vorbereitung ein
verpflichtender Review-Stop. Das Release-Gate, Versionsaenderungen,
Release-Commit, Tag und Veroeffentlichung duerfen erst nach ausdruecklicher
Freigabe durch den Nutzer begonnen werden.
Verlangt das Review Aenderungen, wird die Gate-Vorbereitung nach den Korrekturen
erneut vollstaendig ausgefuehrt. Regulare Release-Builds sowie verpflichtende
Windows- und Linux-CI werden erst in der Alpha-Vorbereitung fuer v0.50.0
aktiviert.

## Sonic-Adventure- und Phasengate-Strategie

Vor v0.50.0 wird Sonic Adventure nicht ausgefuehrt und ist kein
Phasenakzeptanztest. Die Gates von Phase 6 bis zur Alpha-Vorbereitung verwenden
ausschliesslich synthetische Fixtures und frei lizenzierte Homebrew-Programme.
Eine lokale GDI darf vor Alpha read-only validiert, statisch analysiert und bis
zum lokalen Port-Build verarbeitet werden; ihr Programm wird dabei nicht als
Gate ausgefuehrt.

Die fruehere lokale Phase-6-GDI-Blockprobe bleibt als historische
Quellen-/Bootblockdiagnose dokumentiert, gilt aber nicht als Nachweis einer
Sonic-Adventure-Ausfuehrung. Erst der letzte Alpha-Gate-Vorbereitungstask darf
die lokal bereitgestellte Sonic-Adventure-GDI ausfuehren. Das Alpha-Gate ist
erreicht, wenn daraus ein Port-Projekt und `game.exe` entstehen, `game.exe`
tatsaechlich startet und innerhalb eines endlichen Gastzyklusbudgets den
reproduzierbaren Checkpoint `SA_ALPHA_BOOTED` ohne stille Fehler erreicht.

Interaktives Gameplay, Hauptmenue, spielbare Szene, vollstaendige Grafik- oder
Audiokorrektheit und spielbare Performance bleiben Beta-Ziele. Spieldaten,
Captures, Dump-Hashes und lokale Pfade bleiben ausserhalb von Repository und
Releases; titelbezogene Adressen, Remaps, Patches und Runtime-Sonderfaelle sind
untersagt. Die verbindlichen Kriterien stehen in
`docs/SONIC_ADVENTURE_ACCEPTANCE.md`.

## Phase 6: Dreamcast-Plattform

### [x] v0.26.0 - Boot und Homebrew-Einstieg

Fortschritt:

- [x] KR-2601 - Plattformkonfiguration und Bootzustand
- [x] KR-2602 - Homebrew-Raw- und ELF-Start
- [x] KR-2603 - Minimales Plattformlogging
- [x] KR-2604 - Firmware-Betriebsart und BIOS-ABI festlegen
- [x] KR-2605 - PREF und bootrelevante Cacheeffekte
- [x] KR-2606 - Zustandsbehaftetes Flash-Geraetemodell
- [x] KR-2607 - FCNVDS-DN-Review-Regression

Enthalten:

- Plattformkonfiguration
- definierter Bootzustand
- standardmaessig BIOS-freier Direkteinstieg fuer Homebrew
- dokumentierte Entscheidung zwischen Direkteinstieg, HLE-BIOS-ABI und optionalem LLE-Firmwarepfad
- optionale, ausschliesslich vom Nutzer bereitgestellte BIOS- und Flash-Abbilder als externe Eingaben
- `PREF @Rn` und die fuer den gewaehlten Bootpfad beobachtbaren Cacheeffekte
- Flash-Programmierung und -Loeschung ueber ein zustandsbehaftetes Geraetemodell mit unveraendertem Quellabbild
- Start selbst bereitgestellter Raw- und ELF-Homebrew
- minimales Logging
- Fehlerberichte fuer fehlende Segmente oder Einstiegspunkte
- reproduzierbare Beispielprojekte ohne kommerzielle Daten

Release-Gate:

- BIOS-freier Raw-/ELF-Boot, PREF und Flashprotokoll sind mit ausschliesslich synthetischen Fixtures getestet
- HLE-/LLE-Grenzen und nicht modellierte Cacheeffekte sind sichtbar dokumentiert
- frische lokale Debug- und Release-Builds bestehen mit 97/97 Tests; CI ist erst
  zum Alpha-Gate wieder verpflichtend

### [x] v0.27.0 - Maple und Eingabe

Fortschritt:

- [x] KR-2701 - Maple-Bus
- [x] KR-2702 - Controller und deterministische Host-Eingabe
- [x] KR-2703 - VMU-Minimum

Enthalten:

- Maple-Bus-Abstraktion
- Controller
- Tasten und Analogachsen
- minimale VMU-Unterstuetzung
- Host-Input-Backend
- deterministische Input-Replays fuer Tests

Release-Gate:

- Maple-Adressierung, Controllerzustand und VMU-Blocktransfers sind ausschliesslich mit synthetischen Fixtures getestet
- endliche Input-Replays sind reproduzierbar und fallen nicht auf nichtdeterministische Hosteingabe zurueck
- frische lokale Debug- und Release-Builds bestehen vollstaendig; CI ist erst zum Alpha-Gate verpflichtend

### [x] v0.28.0 - PVR Minimum Viable Video

Fortschritt:

- [x] KR-2801 - PVR-Registerminimum
- [x] KR-2802 - Framebuffer-Ausgabe
- [x] KR-2803 - Tile-Accelerator-Grundpfad
- [x] KR-2804 - Texturformate und Render-Backend

Enthalten:

- relevante PVR-Register
- Framebuffer-Pfad
- grundlegende Tile-Accelerator-Kommandos
- primitive Listen
- erste Texturformate
- Render-Backend-Abstraktion
- Frame-Synchronisation

Release-Gate:

- PVR-Register, Framebuffer, TA-Listen, lineare Texturformate und Backend-Uebergabe sind mit synthetischen Fixtures getestet
- alle Grafikpfade bleiben ohne Host-Grafik-API einzeln und deterministisch pruefbar
- frische lokale Debug- und Release-Builds bestehen mit 104/104 Tests; CI ist erst zum Alpha-Gate verpflichtend

Post-Release-Nacharbeit:

- [x] Framebuffer-Groessen-, Stride- und VRAM-Endberechnungen sind gegen Integerueberlauf regressionsgesichert

### [x] v0.29.0 - AICA Minimum Viable Audio

Fortschritt:

- [x] KR-2901 - AICA-Registerminimum
- [x] KR-2902 - PCM und ADPCM
- [x] KR-2903 - Mixer und Host-Audio
- [x] KR-2904 - ARM7-Strategie

Enthalten:

- AICA-Registermodell
- PCM und ADPCM
- Mixer
- Timer und Interrupts
- Host-Audio-Backend
- dokumentierte Strategie fuer den AICA-ARM7

Release-Gate:

- AICA-Register, PCM8, PCM16, ADPCM, Stereo-Mixer und Backend-Uebergabe sind mit synthetischen Fixtures getestet
- drei deterministische Timer erzeugen maskierbare und quittierbare Interrupts; ARM7-LLE wird statt stiller Emulation sichtbar abgewiesen
- frische lokale Debug- und Release-Builds bestehen mit 108/108 Tests; CI ist erst zum Alpha-Gate verpflichtend

### [x] v0.30.0 - GD-ROM und Dateisystem

Fortschritt:

- [x] KR-3001 - Disc- und Dateiquellen-Abstraktion
- [x] KR-3002 - GD-ROM-Kommandos
- [x] KR-3003 - ISO9660
- [x] KR-3004 - Asynchrone Reads und Timing
- [x] KR-3005 - GDI-Deskriptoren und Trackmodell
- [x] KR-3006 - GDI-Quellenintegration

Enthalten:

- Image- und Dateiquellen-Abstraktion
- `.gdi`-Deskriptoren mit relativer Trackauflosung und validierter Mehrdateiquelle
- kein Disc-Image im Repository
- relevante GD-ROM-Kommandos
- ISO9660-Lesezugriff
- asynchrone Reads
- nachvollziehbare Timing-Strategie

Release-Gate:

- DiscSource, GD-ROM-Kommandos, ISO9660, asynchrone Gastzyklus-Timings sowie GDI-Parser und Mehrdateiquelle sind mit ausschliesslich synthetischen Fixtures getestet
- GDI-Quellen bleiben read-only; Hostpfade sind keine semantische Identitaet und kein Disc-Image liegt im Repository oder Release
- frische lokale Debug- und Release-Builds bestehen mit 114/114 Tests; CI ist erst zum Alpha-Gate verpflichtend
- die GDI-Kriterien werden beim kumulativen Phase-6-Gate v0.31.0 als
  historische lokale Quellen-/Bootblockprobe erneut geprueft; dies ist kein
  Sonic-Adventure-Ausfuehrungsnachweis

### [x] v0.31.0 - Scheduling, Timer und DMA

Fortschritt:

- [x] KR-3101 - Event-Scheduler
- [x] KR-3102 - TMU und RTC
- [x] KR-3103 - DMA
- [x] KR-3104 - Plattform-Interruptintegration
- [x] KR-3105 - Frame- und Audio-Taktung

Release-Stand: Alle fuenf v0.31.0-Tasks, die Review-Nacharbeit und das
kumulative Phase-6-Gate sind abgeschlossen.

Review-Nacharbeit: Die Medienuhr trennt alte und callback-intern neugestartete
Laeufe per Generation-ID; Stop/Start/Reset in Video- oder Audio-Callbacks kann
keine doppelten beziehungsweise unkontrollierbaren Scheduler-Ereignisse erzeugen.

Enthalten:

- zentraler, reentrancy-geschuetzter Event-Scheduler mit monotoner Gastzeit
- TMU
- RTC
- DMA-Kanaele
- Interruptintegration
- Frame- und Audio-Taktung
- deterministische Testuhr

Kumulatives Phase-6-Abschlussgate bei v0.31.0:

- ein frei lizenzierter Homebrew-Vertical-Slice zeigt Bild, nimmt Eingabe an und erzeugt Audio
- alle verwendeten Testprogramme duerfen verteilt werden
- Plattformmodule koennen einzeln getestet werden
- der normale Homebrew-Pfad benoetigt kein proprietaeres BIOS- oder Flash-Abbild
- optionale Firmwarepfade veraendern niemals das vom Nutzer bereitgestellte Quellabbild
- `.gdi`-Quellen koennen ohne manuelle Trackumbauten geladen, validiert und ueber dieselbe Disc-Abstraktion wie andere Dateiquellen genutzt werden
- der fruehere v0.30.0-GDI-Smoke wird kumulativ erneut geprueft: alle Tracks und Sektorformate sind validiert, ISO9660 und Bootdatei sind ueber den normalen DiscSource-Pfad lesbar und der Dump bleibt unveraendert
- eine frei verteilbare Homebrew-Bootdatei ist in den Gastadressraum geladen und mindestens ein Block ihres allgemein bestimmten Programmbereichs wurde innerhalb eines festen Gastzyklusbudgets ausgefuehrt
- Scheduler, asynchrones GD-ROM sowie zugehoerige Abschluss-, DMA- oder Interruptpfade liefern messbare Ereignisse; `executed_blocks > 0`, `guest_cycles > 0` und `silent_failures == 0`
- zwei identische Homebrew-Laeufe erreichen `KR_PHASE6_PLATFORM_INTEGRATED` mit demselben letzten Gast-PC und denselben deterministischen Scheduler-Kernzustaenden
- ein frischer Debug-Build besteht mit 122/122 Tests und erreicht in zwei
  bytegleichen Laeufen denselben Checkpoint sowie dieselben strukturellen Ergebnisse
- Gate-Ergebnis: `KR_PHASE6_PLATFORM_INTEGRATED`, ein ausgefuehrter Block,
  16 Gastzyklen, drei Scheduler-Ereignisse sowie je ein GD-ROM-, TMU-, DMA-,
  Interrupt- und Cache-Invalidierungsereignis bei `silent_failures == 0`

## Phase 7: Codegenerator und Dispatch

Planungsgrundlage fuer Phase 7 und spaeter:

- die BIOS-Analyse belegt reale Wechsel zwischen P2- und P1-Alias, ROM-nach-RAM-Codekopien und erst zur Laufzeit installierte BIOS-Vektoren
- Flycast zeigt als Referenzarchitektur getrennte virtuelle und physische Blockadressen, explizite Blockendklassen, zustandsabhaengige Blockvarianten, seitenweise Codeinvalidierung und einen Gastzyklus-Scheduler
- dcrecomp bestaetigt den Nutzen aufgeteilter AOT-Ausgabe und einer zentralen Adresstabelle; hart kodierte Forced Entries, titelbezogene Remaps, stille BIOS-No-ops und Wall-Clock-Timing werden nicht als belastbare Architektur uebernommen
- alle Implementierungen bleiben unabhaengig; Referenzcode, proprietaere Firmware und daraus extrahierte Assets werden nicht uebernommen

### [x] v0.32.0 - Modulare Backend-Schnittstelle

Fortschritt:

- [x] KR-3201 - Backend-Interface
- [x] KR-3202 - C++-Backend migrieren
- [x] KR-3203 - ABI-Faehigkeitspruefung
- [x] KR-3204 - Block-ABI und Zustandsuebergaben
- [x] KR-3205 - Plattformdienst-Schnittstelle

Enthalten:

- Backend-Interface
- C++-Backend als Referenz
- Trennung von Deklarationen, Funktionen und Metadaten
- versionierte Runtime-ABI
- Backend-Faehigkeitsabfrage
- expliziter Blockeintritt und -austritt mit Gast-PC, PR, Delay-Slot-, Ausnahme- und FPU-Zustand
- Plattformdienste fuer Speicher, Scheduler, Interrupts, DMA und kontrollierten Fallback
- Gastadressen bleiben 32-Bit-Werte und werden nie durch Hostzeiger als Identitaet ersetzt

Release-Stand: Alle fuenf Tasks sind abgeschlossen. Das lokale Debug-Gate
bestand mit 127/127 Tests; gemaess Phasenstrategie wurde fuer dieses
Zwischenrelease kein vollstaendiger Sonic-Adventure-Test ausgefuehrt.

### [x] v0.33.0 - Skalierbare Codeausgabe und Build-Cache

Fortschritt:

- [x] KR-3301 - Translation-Unit-Partitionierung
- [x] KR-3302 - Deterministische Dateinamen
- [x] KR-3303 - Inkrementeller Codegen-Cache
- [x] KR-3304 - Parallele Ausgabe und Buildintegration
- [x] KR-3305 - Deterministische Blockmetadaten

Enthalten:

- Aufteilung grosser Programme in Translation Units
- deterministische Dateinamen
- inkrementeller Codegen-Cache
- parallele Ausgabe
- Compile Commands
- Projektgenerierung fuer Ninja und CMake
- getrennte Ausgabe von Code, Konstantdaten, Symbolen und Laufzeitmetadaten
- Blockmetadaten mit virtueller und kanonischer physischer Adresse, Quellsegment, Gastzyklen, Blockendtyp und Zustandswaechtern
- Cache-Schluessel aus Eingabehashes, Manifest, Overrides, IR-/Optimierungsversion sowie Runtime- und Backend-ABI

Release-Stand: Alle fuenf Tasks sind abgeschlossen. Das lokale Debug-Gate
bestand mit 132/132 Tests; der 10.000-Block-Synthetiklauf partitioniert
reproduzierbar. Als Zwischenrelease wurde kein Sonic-Adventure-Test ausgefuehrt.

### [x] v0.34.0 - Indirekter Dispatch und Fallback

Fortschritt:

- [x] KR-3401 - Laufzeit-Blocktabelle
- [x] KR-3402 - Indirekter Call- und Jump-Dispatch
- [x] KR-3403 - Kontrollierter Fallback
- [x] KR-3404 - Selbstmodifizierenden Code erkennen
- [x] KR-3405 - Alias- und kopierbewusster Firmware-Handoff
- [x] KR-3406 - Kanonischer Block-Dispatch und Blockendklassen
- [x] KR-3407 - Scheduler-Safepoints und Gastzyklen
- [x] KR-3408 - MMU- und Zustandswaechter fuer Blockvarianten
- [x] KR-3409 - Praezise Fallback- und Interpretergrenze
- [x] KR-3410 - Cache-/Store-Queue-Vertrag und v0.34 Gate-Vorbereitung
- [x] KR-3411 - v0.34 Release-Gate

Review-Nacharbeit vor KR-3411: Scheduler-Resets verwenden keine fremd
wiederverwendbaren oder doppelten TMU-Ereignisse; DMAC-NMI und -Adressfehler
verwerfen externe Anforderungen, waehrend DME-Pausen sie erhalten. Dynamische
Interpreterbloecke bleiben idempotent. Generiertes `PREF` erreicht beide
Store Queues ueber Plattformdienste bis zu RAM und TA, Call-/Return-Delay-Slots
bewahren die architektonische PR-Reihenfolge, und Codegen-Artefakte koennen
ueber Symlink-Komponenten weder geschrieben noch geloescht werden. Die
vollstaendige KR-3410-Gate-Vorbereitung wurde danach in einem frischen
Debug-Build mit 142/142 Tests erneut erstellt und vom Nutzer fuer KR-3411
freigegeben. Der Release-Commit ist als v0.34.0 versioniert und der Tag zeigt
exakt auf diesen bestaetigten Stand; Sonic Adventure bleibt bis KR-4999
bewusst unausgefuehrt.

Enthalten:

- Laufzeit-Blocktabelle nach kanonischer Gastadresse
- getrennte Blockenden fuer statische, dynamische und bedingte Spruenge, Calls, Returns und Interrupt-Safepoints
- Dispatch fuer aufgeloeste indirekte Calls, Jumps und Returns
- monomorphe Callsite-Caches erst nach korrektem generischem Dispatch
- kontrollierter Fallback fuer ungeloeste Stellen
- optionaler kleiner Interpreter nur fuer nicht rekonstruierbare Pfade
- Synchronisation des vollstaendigen CPU-Zustands an jeder Backend-/Fallback-Grenze
- Erkennung und seitenweise Invalidierung selbstmodifizierenden oder nach RAM kopierten Codes
- KR-3405: kontrollierte Normalisierung physisch identischer Firmware-Aliase
- explizite Behandlung nach RAM kopierter oder zur Laufzeit erzeugter Codebereiche
- optionaler LLE-Firmware-Handoff von ROM nach RAM, falls KR-2604 diesen Betriebsmodus vorsieht
- Gastzyklusbudgets und deterministische Scheduler-/Interrupt-Safepoints
- MMU-, FPSCR-, Adressraum- und Watchpoint-Waechter fuer spezialisierte Block- und Speicherpfade
- zwei Store Queues, QACR-Zielbildung und adressabhaengiges `PREF`-Verhalten
- expliziter Vertrag fuer Cachewartungsbefehle, CCR-Modi und Operand-Cache-RAM
- klare Abbruchstrategie, wenn Sicherheit nicht garantiert werden kann

Kumulatives Phase-7-Abschlussgate bei v0.34.0:

- jeder Blockaustritt besitzt einen expliziten, getesteten Vertrag
- unbekannte Dispatchziele werden nie still ignoriert oder titelbezogen umgebogen
- ein Schreibzugriff auf ausfuehrbaren RAM invalidiert alle betroffenen Blockvarianten vor der naechsten Ausfuehrung
- Scheduler, Interrupts und Ausnahmen beobachten an Backend- und Fallback-Grenzen denselben CPU-Zustand
- der synthetische Alias-/ROM-RAM-Handoff funktioniert ohne proprietaeres BIOS
- alle verteilbaren Phase-6-Kriterien und Checkpoints bestehen weiterhin
- Analyse, IR, deterministisch partitionierter Codegen, Hostbuild und generierter Start laufen ueber die modulare Backend- und Runtime-ABI
- mindestens ein generierter Block und ein generisch aufgeloestes indirektes Ziel werden ausgefuehrt; `indirect_dispatches > 0` und `silent_failures == 0`
- identische Homebrew-Laeufe erreichen `KR_PHASE7_GENERATED_RUNTIME_ACTIVE` mit denselben deterministischen Dispatch-Kernmetriken

## Phase 8: Werkzeuge und Qualitaet

### v0.35.0 - Projektmanifest und stabile CLI

Fortschritt:

- [x] KR-3501 - Manifest-Schema und Versionierung
- [x] KR-3502 - Stabile CLI und Exitcodes
- [x] KR-3503 - JSON-Berichte
- [x] KR-3504 - Override- und Hint-Dateien
- [x] KR-3505 - Ausfuehrungs- und Firmwareprofil im Manifest
- [x] KR-3506 - Eingabeprovenienz und Cache-Identitaet
- [x] KR-3507 - Reproduzierbarer Port-Projektexport

Enthalten:

- versioniertes Manifestformat
- Eingabedateien
- Segmentlayout
- Aliasgruppen, kanonische physische Bereiche und zur Laufzeit beschreibbare ausfuehrbare Segmente
- Einstiegspunkte
- Overrides
- Runtime-Optionen
- Firmwarebetriebsart, Fallback-Regel, Schedulerprofil und erforderliche Backend-Faehigkeiten
- deklarierte Groessen und Hashes externer lokaler Eingaben ohne Einbettung ihrer Daten
- stabile Exitcodes
- maschinenlesbare CLI-Ausgabe
- offizieller CLI-Pfad von einer validierten `.gdi`-Quelle zu einem eigenstaendig
  buildbaren Port-Projekt in einem frei waehlbaren Ausgabeordner
- stabiles Port-Layout mit getrennten generierten Quellen, handgeschriebener
  Integrationsschicht, Metadaten und Buildausgaben
- ausfuehrbares Host-Target `game` beziehungsweise `game.exe` neben der bereits
  vorhandenen statischen Ausgabe
- wiederholbare Generierung, die ausschliesslich als Katana-generiert markierte
  Artefakte ersetzt und handgeschriebenen Portcode erhaelt
- explizite, versionierte Runtime-Abhaengigkeit sowie austauschbare
  `DiscSource`-Anbindung ohne eingebettete absolute Quellpfade
- titelbezogene Assetextraktion und ein Installer, nach dem die Nutzer-GDI
  geloescht werden kann, bleiben Aufgabe des jeweiligen Spieleport-Folgeprojekts

### v0.36.0 - Diagnostik und Debugging

Fortschritt:

- [x] KR-3601 - Symbolische Namen
- [x] KR-3602 - Adress-zu-Quelle-Mapping
- [x] KR-3603 - Crashberichte
- [x] KR-3604 - Tracing und Watchpoints
- [x] KR-3605 - CFG- und Callgraph-Export
- [x] KR-3606 - Sichere Firmware- und Flash-Diagnostik
- [x] KR-3607 - Dispatch- und Fallbackdiagnostik
- [x] KR-3608 - Block-, Alias- und Invalidierungsprovenienz
- [ ] KR-3609 - Deterministische Systemereignis-Replays

Enthalten:

- Symbolnamen
- Adress-zu-Quelle-Mapping
- Crashberichte mit virtuellem PC, kanonischer Adresse, Blockvariante, Delay-Slot- und Ausnahmezustand
- IR- und Runtime-Tracing
- CFG-Export als DOT und JSON
- Callgraph-Export
- Watchpoints
- Dispatchberichte mit Callsite, Ziel, PR, Beweisherkunft und gewaehlter Fallbackaktion
- Provenienz fuer Aliasnormalisierung, ROM-RAM-Kopien, dynamische Vektoren und Codeinvalidierungen
- deterministische Replays fuer CPU-, MMIO-, DMA-, Interrupt- und Schedulerereignisse
- KR-3606: read-only Firmware- und Flash-Inspektion mit Groessen-/Hashpruefung, Partitions-/CRC-Diagnose und standardmaessiger Redaktion sensibler Felder

### v0.37.0 - Fuzzing und reproduzierbare Debug-Gates

Fortschritt:

- [ ] KR-3701 - Lokale Debug-Gate-Automatisierung
- [ ] KR-3702 - Sanitizer-Builds
- [ ] KR-3703 - Fuzzer
- [ ] KR-3704 - Coverage
- [ ] KR-3705 - Formatierung und statische Analyse
- [ ] KR-3706 - Reproduzierbare Release-Artefakte
- [ ] KR-3707 - Differenztests der Ausfuehrungswege
- [ ] KR-3708 - Mehrsegment-, Dispatch- und Invalidierungsfuzzing
- [ ] KR-3709 - Referenz-/Lizenzprovenienz und v0.37 Gate-Vorbereitung
- [ ] KR-3710 - v0.37 Release-Gate

Enthalten:

- lokaler frischer Debug-Build mit vollstaendiger Regression
- portable Testprofile als Vorbereitung fuer die Alpha-CI
- AddressSanitizer und UndefinedBehaviorSanitizer
- Decoder-, Loader- und IR-Fuzzer
- Differenztests zwischen IR-Referenzausfuehrung, generiertem C++ und kontrolliertem Fallback
- Fuzzing fuer Aliasgruppen, Multi-Segment-Images, indirekten Dispatch, ROM-RAM-Kopien und Schreibinvalidierung
- Coverage-Berichte
- Formatierung und statische Analyse
- reproduzierbare Pre-Alpha-Artefakte
- Third-Party-, Referenzprovenienz- und Lizenzbericht

Kumulatives Phase-8-Abschlussgate bei v0.37.0:

- jeder nicht aufgeloeste Kontrollflussfall ist maschinenlesbar und reproduzierbar diagnostizierbar
- Replays enthalten keine Hostzeit als versteckte Wahrheitsquelle
- Firmware- und Flash-Berichte sind standardmaessig redigiert und veraendern keine Eingabe
- Flycast und dcrecomp bleiben dokumentierte Referenzen; ihre Implementierungen oder GPL-pflichtigen Subsysteme werden ohne bewusste Lizenzentscheidung nicht eingebunden
- gleiche Eingaben und Optionen erzeugen bytegleiche Metadaten und Release-Artefakte
- alle Phase-7-Kriterien und Checkpoints bestehen weiterhin
- Manifest und stabile CLI fuehren Analyse, Codegen, Build und begrenzten Lauf mit versionierten, redigierten JSON-Berichten aus
- der offizielle Port-Export verarbeitet eine synthetische oder frei lizenzierte `.gdi`-Quelle ueber Bootdatei,
  Analyse, IR und partitionierten Codegen, erzeugt ausserhalb des
  KatanaRecomp-Quellbaums ein eigenstaendig buildbares Projekt und baut dort das
  ausfuehrbare Host-Target `game` beziehungsweise `game.exe`
- eine erneute Generierung veraendert keine handgeschriebenen Dateien des
  Port-Projekts; absolute GDI- und Trackpfade sind weder in generierten Quellen
  noch in portablen Metadaten fest eingebettet
- ein kontrollierter Abbruch dokumentiert Gast-PC, Block-, Delay-Slot-, Ausnahme-, Scheduler- und Dispatchzustand ohne lokale Pfade oder Spieldaten
- identische Laeufe erreichen `KR_PHASE8_REPRODUCIBLE_TOOLCHAIN` und erzeugen bytegleiche Manifeste und Blockmetadaten

## Phase 9: Kompatibilitaet und Leistung

### v0.38.0 - Homebrew-Vertical-Slice

Fortschritt:

- [ ] KR-3801 - Rechtlich sauberes Homebrew-Testkorpus
- [ ] KR-3802 - CPU-Konformitaetsprogramm
- [ ] KR-3803 - Eingabe-Beispiel
- [ ] KR-3804 - 2D-Grafik-Beispiel
- [ ] KR-3805 - Audio-Beispiel
- [ ] KR-3806 - Zusammenhaengendes Testspiel
- [ ] KR-3807 - Synthetischer Firmware-Handoff-Test
- [ ] KR-3808 - Scheduler-, DMA- und Interrupt-Vertical-Slice
- [?] KR-3809 - Optionales lokales Firmware-Smoke-Profil

Pflichtkorpus:

- CPU-Konformitaetstest
- Konsolenausgabe
- Controller-Beispiel
- 2D-Grafik-Beispiel
- Audio-Beispiel
- kleines zusammenhaengendes Testspiel
- synthetischer Resetpfad mit `PREF`, P2-/P1-Aliaswechsel, ROM-RAM-Kopie und dynamischen RAM-Vektoren
- deterministische Scheduler-, DMA- und Interruptsequenz ueber mindestens einen Frame

Release-Gate:

- alle Programme werden automatisch gebaut und getestet
- Screenshots, Audio-Hashes oder semantische Zustandspruefungen sind reproduzierbar
- der Pflichtkorpus benoetigt weder BIOS noch Flash noch Disc-Image
- ein optionales lokales Firmware-Smoke-Profil darf nur explizit aktivierte Hash-/Statusresultate, niemals Firmwarebytes oder sensible Flashfelder ausgeben

### v0.39.0 - Performance-Basis

Fortschritt:

- [ ] KR-3901 - Benchmark-Suite
- [ ] KR-3902 - Hot-Block-Analyse
- [ ] KR-3903 - Dispatch- und Speicher-Fastpaths
- [ ] KR-3904 - Inlining und Codegroessenstrategie
- [ ] KR-3905 - LTO und PGO
- [ ] KR-3906 - Block-, Edge- und Dispatch-Profiling
- [ ] KR-3907 - Fastpath- und Inline-Cache-Waechter
- [ ] KR-3908 - Budgets und v0.39 Gate-Vorbereitung
- [ ] KR-3909 - v0.39 Release-Gate

Enthalten:

- Benchmark-Suite
- Hot-Block-Analyse
- Block-, Kanten-, Dispatch-, Fallback- und Invalidierungszaehler
- Inlining-Strategie
- Speicher-Fastpaths nur fuer bewiesene lineare RAM-Zugriffe mit MMU-, Alias-, Watchpoint- und Ausrichtungswaechtern
- Dispatch-Optimierung mit messbaren monomorphen Callsite-Caches und sicherem generischem Rueckfall
- optional LTO und PGO
- definierte Budgets fuer Laufzeit, Codegroesse, Schedulerjitter, Invalidierungen und Fallbackrate
- Performance darf Korrektheit nicht verdecken

Release-Gate:

- deaktivierte Fastpaths und Inline-Caches liefern denselben beobachtbaren Gastzustand
- jede Spezialisierung nennt ihre Waechter und ihre Invalidierungsursachen
- Benchmarks trennen Codegenzeit, Host-Kompilierzeit, Startzeit und Laufzeit
- kein Performancepfad umgeht MMIO-Nebenwirkungen, Watchpoints, Ausnahmen oder Codeinvalidierung

### v0.40.0 - Oeffentlicher Pre-Alpha-Release

Fortschritt:

- [ ] KR-4001 - Oeffentliche Installationsdokumentation
- [ ] KR-4002 - Architektur- und Manifestreferenz
- [ ] KR-4003 - Lizenz- und Rechtspruefung
- [ ] KR-4004 - Kompatibilitaetsbericht
- [ ] KR-4006 - Faehigkeits-/Datenaudit und v0.40 Gate-Vorbereitung
- [ ] KR-4005 - v0.40.0 Pre-Alpha-Release

Enthalten:

- Installationsanleitung
- Architekturuebersicht
- Manifestreferenz
- bekannte Einschraenkungen
- reproduzierbares Releasepaket
- klarer Rechts- und Datenhinweis
- Faehigkeitsmatrix fuer Direkteinstieg, HLE, optionales LLE, MMU, Fallback, selbstmodifizierenden Code und Schedulerpraezision
- Referenzprovenienz fuer Flycast, dcrecomp, Spezifikationen und synthetische Testvektoren
- automatisierter Audit, dass keine Firmwarebytes, extrahierten Assets, persoenlichen Flashdaten oder lokalen Pfade im Paket liegen
- veroeffentlichter Homebrew-Kompatibilitaetsbericht

Kumulatives Phase-9-Abschlussgate bei v0.40.0:

- alle Phase-8-Kriterien und Checkpoints bestehen weiterhin; das Homebrew-Korpus bleibt der oeffentliche Pflichtnachweis
- mindestens ein Frame des frei lizenzierten Homebrew-Testspiels erreicht den vollstaendigen PVR-Pfad; `pvr_frames >= 1`, plausible Framegeometrie und gueltige VRAM-Grenzen werden berichtet
- Audio- und Maple-Zaehler werden aus demselben zusammenhaengenden Lauf erfasst, sofern der erreichte Spielpfad diese Subsysteme bereits nutzt
- der Lauf ist gastzyklusbegrenzt, verarbeitet mindestens zwei Frameintervalle und hat `silent_failures == 0`
- ein verteilbares Referenz-Capture und getrennte Analyse-, Codegen-, Build-, Start- und Laufzeitmetriken belegen `KR_PHASE9_HOMEBREW_HOST_FRAME`
- aktivierte und deaktivierte Fastpaths liefern denselben beobachtbaren Gastzustand

## Phase 10: Desktop-GUI und Alpha-Workflow

Die Erkenntnisse aus BIOS-Analyse, Flycast und dcrecomp beeinflussen diese Phase direkt:

- die GUI darf keine eigene Quell- oder Laufzeitsemantik duplizieren, sondern muss denselben Manifest-, Analyse-, Dispatch- und Firmwareprofilpfad wie CLI und Automatisierung verwenden
- `.gdi`-Quellen muessen als erstklassiger Mehrdatei-Einstieg mit transparenter Trackprovenienz, klaren Fehlern und Null-Write-Garantie behandelt werden
- Firmware-, Alias-, ROM-RAM- und Schedulerdiagnosen muessen fuer Alpha auch ohne CLI nutzbar, aber weiterhin datensparsam und read-only bleiben

### v0.41.0 - GUI-Grundlage und Anwendungsdienste

Enthalten:

- Desktop-Framework- und Packaging-Entscheidung fuer den unterstuetzten Alpha-Scope
- gemeinsamer Anwendungsdienst ueber CLI, GUI und Automatisierung
- Hauptfenster, Navigation, Projekteinstellungen und persistente Nutzerkonfiguration
- stabiler Aufrufpfad fuer Analyse-, Codegen-, Build- und Run-Jobs ohne doppelte Geschaeftslogik

### v0.42.0 - Projekt- und Quellenworkflow

Enthalten:

- Projekt anlegen, oeffnen, speichern und wiederherstellen
- Dateiquellen fuer Raw-, ELF- und `.gdi`-Einstiege
- `.gdi`-Trackinspektor mit relativer Pfadauflosung, Dateigroessenpruefung, Sektorformatdiagnosen und Vorschau der Quellprovenienz
- Manifest-, Firmwareprofil- und Override-Bearbeitung ueber denselben Datenvertrag wie die CLI

### v0.43.0 - Analyse-, Build- und Diagnostik-Workflow

Enthalten:

- Jobs fuer Analyse, Codegen, Build und optionalen Lauf aus der GUI starten, abbrechen und wiederaufnehmen
- Fortschritt, Logs, Fehler, Warnungen und redigierte Firmwarediagnosen sichtbar machen
- Ansichten fuer Funktionen, Quellen-/Segmentzuordnung, Dispatch-/Fallbackereignisse und Invalidierungsprovenienz
- keine Hostpfade, Firmwarebytes oder sensiblen Flashfelder in exportierten Berichten oder Standardansichten

### v0.44.0 - Alpha-Readiness fuer GUI und GDI

Enthalten:

- End-to-End-Automatisierung fuer GUI-Hauptpfade unter Windows und Linux
- synthetische `.gdi`-Fixtures fuer Positiv-, Negativ- und Recovery-Faelle
- DPI-, Tastatur-, Fehlerrecovery- und Packaging-Haertung fuer den Alpha-Workflow
- dokumentierter Standardpfad, bei dem neue Nutzer Alpha-relevante Projekte ohne CLI-Zwang anlegen und ausfuehren koennen

Kumulatives Phase-10-Abschlussgate bei v0.44.0:

- die GUI deckt den Alpha-Hauptworkflow fuer Projektanlage, Quellenwahl, Analyse, Build und Diagnostik vollstaendig ab
- `.gdi`-Quellen sind in GUI, CLI und Automatisierung dieselbe validierte Quelle und besitzen dieselben Fehlermeldungen und Identitaetsregeln
- ein fehlerhafter Track, Descriptor oder relativer Pfad erzeugt reproduzierbare, nutzbare Diagnosen statt stiller Ausweichpfade
- GUI- und CLI-Lauf fuer identische Projekte erzeugen dieselben Manifeste, Jobs und Ergebnisartefakte
- alle Phase-9-Kriterien und Checkpoints bestehen weiterhin
- der lokale GDI-Quellenworkflow kann in der GUI angelegt, validiert, gespeichert und wieder geoeffnet werden
- GUI und CLI starten mit synthetischen oder frei lizenzierten Quellen ueber dieselben Anwendungsdienste Analyse, Codegen, Build und Lauf und erreichen denselben Checkpoint mit denselben Artefakten
- Abbruch, Fehleranzeige und ein ungueltiger Trackpfad liefern dieselben strukturierten Fehlerklassen; exportierte Berichte bleiben redigiert
- `KR_PHASE10_GUI_END_TO_END` wird auf den fuer Alpha unterstuetzten Windows- und Linux-Konfigurationen automatisiert erreicht, ohne Dreamcast-Testlogik in der GUI zu duplizieren

## Phase 11: Alpha-Integration und Haertung

Diese Phase schliesst bewusst die Luecke zwischen dem GUI-/Quellworkflow in
v0.44.0 und dem ersten ausfuehrbaren Sonic-Adventure-Alpha in v0.50.0. Alle
Gates bis einschliesslich v0.49.0 verwenden nur synthetische Fixtures und frei
lizenzierte Homebrew-Programme. Eine lokale Sonic-Adventure-GDI darf bis zum
Port-Build verarbeitet, aber erst in KR-4999 ausgefuehrt werden.

### v0.45.0 - SH-4-Alpha-ISA-Abdeckung

Fortschritt:

- [ ] KR-4501 - Messbarer SH-4-Alpha-ISA-Vertrag
- [ ] KR-4502 - Fehlende Integer- und Kontrollinstruktionen
- [ ] KR-4503 - Status-, Exception- und Systemsemantik
- [ ] KR-4504 - v0.45 Gate-Vorbereitung: Tests und Build
- [ ] KR-4505 - v0.45 Release-Gate

Gate-Ergebnis: Das dokumentierte Alpha-ISA-Profil ist ohne stille Opcode-
Luecken ueber Decoder, IR, Backend und Runtime abgedeckt; der verteilbare
Nachweis erreicht `KR_V045_ISA_ALPHA_PROFILE_READY`.

### v0.46.0 - Retail-Boot- und Systemdienste

Fortschritt:

- [ ] KR-4601 - Alpha-Firmwaremodus und Retail-Bootvertrag
- [ ] KR-4602 - BIOS-ABI und dynamische Firmwarevektoren
- [ ] KR-4603 - Dreamcast-System-ASIC sowie MMIO- und Interruptintegration
- [ ] KR-4604 - v0.46 Gate-Vorbereitung: Tests und Build
- [ ] KR-4605 - v0.46 Release-Gate

Gate-Ergebnis: Ein synthetischer Retail-Boot-Vertical-Slice nutzt denselben
Firmware-, ASIC-, MMIO- und Interruptvertrag wie spaetere lokale Ports und
erreicht `KR_V046_RETAIL_BOOT_SERVICES_READY`.

### v0.47.0 - Native Hostruntime

Fortschritt:

- [ ] KR-4701 - Native Fenster- und Videoausgabe
- [ ] KR-4702 - Native Audio-, Eingabe- und Hostlebenszyklusintegration
- [ ] KR-4703 - Persistente VMU-/Flash-Arbeitskopien und Host-Pacing
- [ ] KR-4704 - v0.47 Gate-Vorbereitung: Tests und Build
- [ ] KR-4705 - v0.47 Release-Gate

Gate-Ergebnis: Ein frei lizenzierter Port laeuft als eigenstaendige
Hostanwendung mit Video, Audio, Eingabe und kontrolliertem Lebenszyklus und
erreicht `KR_V047_NATIVE_HOST_READY`.

### v0.48.0 - Port- und GUI-Integration

Fortschritt:

- [ ] KR-4801 - Versioniertes Runtime-SDK fuer externe Port-Projekte
- [ ] KR-4802 - Gemeinsamer CLI-/GUI-Portexport und Buildworkflow
- [ ] KR-4803 - Out-of-Tree-`game.exe`-Integration
- [ ] KR-4804 - v0.48 Gate-Vorbereitung: Tests und Build
- [ ] KR-4805 - v0.48 Release-Gate

Gate-Ergebnis: Eine synthetische oder frei lizenzierte GDI wird ausserhalb des
KatanaRecomp-Baums reproduzierbar zu einem Port-Projekt und einer startbaren
`game.exe`; GUI und CLI erreichen `KR_V048_PORT_WORKFLOW_READY` ueber denselben
Anwendungsdienst. Eine lokale Sonic-Adventure-GDI darf denselben Buildpfad
durchlaufen, ihre `game.exe` wird in diesem Gate jedoch nicht gestartet.

### v0.49.0 - Alpha-Release-Candidate

Fortschritt:

- [ ] KR-4901 - Alpha-CI-Konfiguration fuer Windows und Linux
- [ ] KR-4902 - Reproduzierbare Pakete sowie Daten- und Lizenzaudit
- [ ] KR-4903 - Alpha-Checkpoint- und Gate-Automatisierung einfrieren
- [ ] KR-4904 - v0.49 Gate-Vorbereitung: Tests und Build
- [ ] KR-4905 - v0.49 Release-Gate

Gate-Ergebnis: Der vollstaendige verteilbare Alpha-Kandidat erreicht
`KR_V049_ALPHA_CANDIDATE_READY`; alle oeffentlichen Tests, Berichte und Pakete
bleiben frei von proprietaeren Daten. Erst nach dem Review von KR-4904 darf
KR-4905 beginnen.

## Alpha-Gate: v0.50.0

Voraussetzungen:

- nahezu vollstaendiger SH-4-Integer-Kern
- belastbare FPU-Grundlage
- Runtime und generierter Code sauber getrennt
- rekursive Analyse und Jump-Table-Unterstuetzung
- alle Blockendtypen und Backend-/Fallback-Zustandsuebergaben sind explizit getestet
- keine stille Behandlung unbekannter Opcodes, Dispatchziele, BIOS-Aufrufe oder MMIO-Zugriffe im unterstuetzten Profil
- Scheduler und Interruptzustellung sind fuer identische Eingaben deterministisch
- ausfuehrbarer RAM besitzt getestete Schreibinvalidierung und nachvollziehbare Codeprovenienz
- mindestens ein zusammenhaengender Homebrew-Vertical-Slice
- synthetischer Firmware-Handoff ohne proprietaere Daten
- stabile Manifestversion
- vollstaendige Desktop-GUI fuer den Alpha-Hauptworkflow ohne CLI-Zwang
- `.gdi`-Dateien koennen als offizielle Quelle geladen, validiert und reproduzierbar verarbeitet werden
- CI auf Windows und Linux
- reproduzierbare Builds

Fortschritt:

- [ ] KR-4999 - Alpha-Gate-Vorbereitung: Tests, Builds und lokaler Sonic-Boot
- [ ] KR-5000 - v0.50.0 Alpha-Gate

Kumulatives Sonic-Adventure-Alpha-Gate:

- alle verteilbaren Checkpoints der Phasen 6 bis 11 bestehen in der Alpha-Gate-Vorbereitung erneut
- der offizielle Quellenworkflow verarbeitet die lokal bereitgestellte GDI, Tracks, ISO9660 und Bootdatei und erzeugt ausserhalb des KatanaRecomp-Quellbaums ein Port-Projekt mit `game.exe`
- die erzeugte `game.exe` startet tatsaechlich; Hauptprogramm, Scheduler, GD-ROM, DMA, Interrupts und generischer indirekter Dispatch machen innerhalb eines festen Gastzyklusbudgets messbaren Fortschritt
- zwei identische Laeufe erreichen `SA_ALPHA_BOOTED` mit denselben deterministischen Kernmetriken und `silent_failures == 0`
- ein Spiel-Frame, Hauptmenue oder eine interaktive Szene ist kein Alpha-Pflichtkriterium; visuelle Ausgabe darf als zusaetzlicher Nachweis erfasst werden
- ein begrenzt beendeter Fehllauf erzeugt einen verwertbaren redigierten Diagnosebericht
- Repository und Releasepaket enthalten keine Spieldaten, Captures, Dump-Hashes oder lokalen Dump-Pfade
- KR-4999 erstellt beziehungsweise vervollstaendigt alle Alpha-Tests, fuehrt die frischen Debug- und Release-Builds, die vollstaendige Regression und die erforderliche Windows-/Linux-CI aus und stoppt danach fuer das Nutzerreview
- KR-5000 darf erst nach ausdruecklicher Nutzerfreigabe beginnen; Review-Aenderungen erzwingen eine vollstaendige Wiederholung von KR-4999

## Beta-Gate: v0.75.0

Fortschritt:

- [ ] KR-7499 - Beta-Gate-Vorbereitung: Tests und Builds
- [ ] KR-7500 - v0.75.0 Beta-Gate

Voraussetzungen:

- ausgewaehlte reale Programme laufen bis in interaktive Szenen
- Grafik, Audio und Eingabe funktionieren zusammen
- ungeloeste Kontrollflussstellen sind diagnostizierbar
- Fallbackrate, Schedulerjitter und Codeinvalidierungen sind pro Testtitel messbar
- MMU-, Cache-/Store-Queue- und DMA-relevante Pfade besitzen integrierte Regressionen
- Abstuerze erzeugen verwertbare Berichte
- Performance ist fuer Testtitel praktikabel
- Kompatibilitaetsmatrix und bekannte Fehler sind gepflegt
- optionale lokale LLE-Tests sind strikt von verteilbaren Pflichttests getrennt
- KR-7499 stoppt nach Tests, Builds, CI und Gate-Bericht fuer das Nutzerreview;
  KR-7500 beginnt erst nach ausdruecklicher Freigabe

## Release-Gate: v1.0.0

Fortschritt:

- [ ] KR-9999 - v1.0 Gate-Vorbereitung: Tests und Builds
- [ ] KR-10000 - v1.0.0 Release-Gate

Voraussetzungen:

- dokumentierter und stabiler CLI- und Manifestvertrag
- versionierte Runtime-ABI
- reproduzierbarer Build
- keine bekannten stillen Fehlkompilierungen im unterstuetzten Bereich
- definierte und dokumentierte SH-4- und Dreamcast-Abdeckung
- dokumentierte Vertrage fuer Dispatch, Fallback, Scheduler, MMU-/Fastpath-Waechter und Codeinvalidierung
- automatisierte CPU-, IR-, Runtime- und Plattformtests
- Homebrew-Referenzkorpus laeuft reproduzierbar
- jede unterstuetzte Firmwarebetriebsart besitzt eine veroeffentlichte Faehigkeits- und Datenschutzbeschreibung
- Lizenz-, Rechts- und Drittanbieterhinweise sind vollstaendig
- keine BIOS-, Disc-, Spiel- oder Assetdaten im Repository oder Release
- Upgrade- und Migrationshinweise fuer kuenftige Versionen
- KR-9999 stoppt nach Tests, Builds, CI und Audits fuer das Nutzerreview;
  KR-10000 beginnt erst nach ausdruecklicher Freigabe

## Nach v1.0

Moegliche Bereiche:

- weitere Codegen-Backends
- aggressivere IR-Optimierungen
- Debugger-Integration
- Remote-Debugging
- Hardware-Testintegration
- verbesserte PVR- und AICA-Genauigkeit
- Netzwerkhardware
- Modem und Broadband Adapter
- Tooling fuer Modding und Forschung

## Nicht-Ziele vor v1.0

- perfekte Zyklusgenauigkeit fuer jede Dreamcast-Komponente
- vollstaendige Emulation aller seltenen Peripheriegeraete
- automatische Verteilung urheberrechtlich geschuetzter Eingaben
- unkontrollierte Ausfuehrung unbekannter oder selbstmodifizierender Pfade
- mehrere experimentelle Backends, bevor das C++-Backend stabil ist

## Definition of Done

Ein regulaerer Implementierungs-Task gilt als inhaltlich abgeschlossen, wenn:

1. sein vereinbarter Scope implementiert ist
2. Erfolgs-, Grenz- und Fehlerfaelle als konkrete Testanforderungen fuer die
   Gate-Vorbereitung dokumentiert sind
3. relevante Dokumentation aktualisiert ist
4. keine generierten Builddateien versehentlich committed wurden
5. die Aenderung unabhaengig implementiert wurde
6. Commit oder PR die Task-ID nennt

Der letzte Gate-Vorbereitungstask einer Phase gilt erst als abgeschlossen, wenn:

1. alle gesammelten Testanforderungen als automatische Tests umgesetzt sind
2. genau ein frischer Build in `build-current/` erfolgreich erstellt wurde
3. die vollstaendige Regression einschliesslich Erfolgs-, Grenz- und
   Fehlerfaellen besteht
4. erforderliche Gate-Berichte und Artefakte reproduzierbar vorliegen
5. Dokumentation und Taskstatus den tatsaechlichen Gate-Stand wiedergeben
6. anschliessend vor dem Phasen-Release-Gate fuer das Nutzerreview gestoppt wird

Ein Phasen-Release-Gate gilt erst als abgeschlossen, wenn die
Gate-Vorbereitung unveraendert gueltig ist und der Nutzer das Gate ausdruecklich
freigegeben hat. Verlangt das Review Aenderungen, ist die Gate-Vorbereitung nach
den Korrekturen vollstaendig zu wiederholen.

Die detaillierte Task-Liste steht in `docs/TASKS.md`.
Die Arbeitsregeln fuer Codex stehen in `docs/CODEX_HANDOFF.md`.
