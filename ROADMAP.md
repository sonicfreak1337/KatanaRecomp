# KatanaRecomp Roadmap

Status: Pre-Alpha
Aktuelle Basis: v0.21.0
Planungsmodell: Semantic Versioning, kleine ueberpruefbare Meilensteine

Diese Roadmap beschreibt die technische Entwicklung von KatanaRecomp vom aktuellen Architektur-Prototyp bis zu einem belastbaren Dreamcast-Recompiler-Framework.

Sie ist absichtlich in kleine, voneinander abhaengige Releases und Task-IDs zerlegt. Dadurch koennen Menschen, Codex und andere Werkzeuge jeweils genau einen begrenzten Arbeitsauftrag uebernehmen, ohne nebenbei die halbe Architektur neu zu erfinden.

## Leitprinzipien

1. Korrektheit vor Geschwindigkeit.
2. Jede neue Semantik braucht automatische Tests.
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
| Werkzeuge und Qualitaet | 0.35 bis 0.37 | Manifest, Diagnostik, CI, Fuzzing und reproduzierbare Builds |
| Kompatibilitaet und Leistung | 0.38 bis 0.40 | Homebrew-Vertical-Slice und erster oeffentlicher Pre-Alpha-Stand |
| Alpha | 0.50.0 | zusammenhaengende Dreamcast-Programme laufen reproduzierbar |
| Beta | 0.75.0 | ausgewaehlte reale Programme sind spielbar und debuggbar |
| Stabil | 1.0.0 | dokumentierter, reproduzierbarer und stabiler Framework-Release |

## Phase 1: SH-4 Integer-Kern

### v0.11.0 - ALU und Statussemantik

Ziel: haeufige Integer- und Bitoperationen vollstaendig durch Decoder, IR, Codegenerator und End-to-End-Tests fuehren.

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

### v0.12.0 - Shifts und Rotationen
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

### v0.13.0 - Multiplikation, Division und MAC
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

### v0.14.0 - Adressierungsarten und Systemregister

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

### v0.15.0 - Decoder-Haertung und ISA-Abdeckung

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

### v0.16.0 - Executable-Image-Modell

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

### v0.17.0 - Rekursive Codeentdeckung

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

### v0.18.0 - Indirekter Kontrollfluss und Jump Tables

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

### v0.19.0 - IR Version 2

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

### v0.20.0 - Sichere Basisoptimierungen

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

### v0.21.0 - Runtime-Trennung und vollstaendiger CPU-Zustand

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

### v0.22.0 - Dreamcast-Speicherbus

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

### v0.23.0 - Ausnahmen und Interrupts

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

## Phase 5: SH-4 FPU

### v0.24.0 - FPU-Grundoperationen

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

### v0.25.0 - Dreamcast-relevante FPU-Spezialoperationen

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

## Phase 6: Dreamcast-Plattform

### v0.26.0 - Boot und Homebrew-Einstieg

Enthalten:

- Plattformkonfiguration
- definierter Bootzustand
- Start selbst bereitgestellter Raw- und ELF-Homebrew
- minimales Logging
- Fehlerberichte fuer fehlende Segmente oder Einstiegspunkte
- reproduzierbare Beispielprojekte ohne kommerzielle Daten

### v0.27.0 - Maple und Eingabe

Enthalten:

- Maple-Bus-Abstraktion
- Controller
- Tasten und Analogachsen
- minimale VMU-Unterstuetzung
- Host-Input-Backend
- deterministische Input-Replays fuer Tests

### v0.28.0 - PVR Minimum Viable Video

Enthalten:

- relevante PVR-Register
- Framebuffer-Pfad
- grundlegende Tile-Accelerator-Kommandos
- primitive Listen
- erste Texturformate
- Render-Backend-Abstraktion
- Frame-Synchronisation

### v0.29.0 - AICA Minimum Viable Audio

Enthalten:

- AICA-Registermodell
- PCM und ADPCM
- Mixer
- Timer und Interrupts
- Host-Audio-Backend
- dokumentierte Strategie fuer den AICA-ARM7

### v0.30.0 - GD-ROM und Dateisystem

Enthalten:

- Image- und Dateiquellen-Abstraktion
- kein Disc-Image im Repository
- relevante GD-ROM-Kommandos
- ISO9660-Lesezugriff
- asynchrone Reads
- nachvollziehbare Timing-Strategie

### v0.31.0 - Scheduling, Timer und DMA

Enthalten:

- zentraler Event-Scheduler
- TMU
- RTC
- DMA-Kanaele
- Interruptintegration
- Frame- und Audio-Taktung
- deterministische Testuhr

Release-Gate fuer Phase 6:

- ein frei lizenzierter Homebrew-Vertical-Slice zeigt Bild, nimmt Eingabe an und erzeugt Audio
- alle verwendeten Testprogramme duerfen verteilt werden
- Plattformmodule koennen einzeln getestet werden

## Phase 7: Codegenerator und Dispatch

### v0.32.0 - Modulare Backend-Schnittstelle

Enthalten:

- Backend-Interface
- C++-Backend als Referenz
- Trennung von Deklarationen, Funktionen und Metadaten
- versionierte Runtime-ABI
- Backend-Faehigkeitsabfrage

### v0.33.0 - Skalierbare Codeausgabe und Build-Cache

Enthalten:

- Aufteilung grosser Programme in Translation Units
- deterministische Dateinamen
- inkrementeller Codegen-Cache
- parallele Ausgabe
- Compile Commands
- Projektgenerierung fuer Ninja und CMake

### v0.34.0 - Indirekter Dispatch und Fallback

Enthalten:

- Laufzeit-Funktionstabelle
- Dispatch fuer aufgeloeste indirekte Calls und Jumps
- kontrollierter Fallback fuer ungeloeste Stellen
- optionaler kleiner Interpreter nur fuer nicht rekonstruierbare Pfade
- Erkennung selbstmodifizierenden Codes
- klare Abbruchstrategie, wenn Sicherheit nicht garantiert werden kann

## Phase 8: Werkzeuge und Qualitaet

### v0.35.0 - Projektmanifest und stabile CLI

Enthalten:

- versioniertes Manifestformat
- Eingabedateien
- Segmentlayout
- Einstiegspunkte
- Overrides
- Runtime-Optionen
- stabile Exitcodes
- maschinenlesbare CLI-Ausgabe

### v0.36.0 - Diagnostik und Debugging

Enthalten:

- Symbolnamen
- Adress-zu-Quelle-Mapping
- Crashberichte
- IR- und Runtime-Tracing
- CFG-Export als DOT und JSON
- Callgraph-Export
- Watchpoints
- deterministische Replays

### v0.37.0 - CI, Fuzzing und reproduzierbare Builds

Enthalten:

- Windows-CI
- Linux-CI
- Debug- und Release-Builds
- AddressSanitizer und UndefinedBehaviorSanitizer
- Decoder-, Loader- und IR-Fuzzer
- Coverage-Berichte
- Formatierung und statische Analyse
- reproduzierbare Release-Artefakte
- Third-Party- und Lizenzbericht

## Phase 9: Kompatibilitaet und Leistung

### v0.38.0 - Homebrew-Vertical-Slice

Pflichtkorpus:

- CPU-Konformitaetstest
- Konsolenausgabe
- Controller-Beispiel
- 2D-Grafik-Beispiel
- Audio-Beispiel
- kleines zusammenhaengendes Testspiel

Release-Gate:

- alle Programme werden automatisch gebaut und getestet
- Screenshots, Audio-Hashes oder semantische Zustandspruefungen sind reproduzierbar

### v0.39.0 - Performance-Basis

Enthalten:

- Benchmark-Suite
- Hot-Block-Analyse
- Inlining-Strategie
- Speicher-Fastpaths
- Dispatch-Optimierung
- optional LTO und PGO
- definierte Regressionsbudgets
- Performance darf Korrektheit nicht verdecken

### v0.40.0 - Oeffentlicher Pre-Alpha-Release

Enthalten:

- Installationsanleitung
- Architekturuebersicht
- Manifestreferenz
- bekannte Einschraenkungen
- reproduzierbares Releasepaket
- klarer Rechts- und Datenhinweis
- veroeffentlichter Homebrew-Kompatibilitaetsbericht

## Alpha-Gate: v0.50.0

Voraussetzungen:

- nahezu vollstaendiger SH-4-Integer-Kern
- belastbare FPU-Grundlage
- Runtime und generierter Code sauber getrennt
- rekursive Analyse und Jump-Table-Unterstuetzung
- mindestens ein zusammenhaengender Homebrew-Vertical-Slice
- stabile Manifestversion
- CI auf Windows und Linux
- reproduzierbare Builds

## Beta-Gate: v0.75.0

Voraussetzungen:

- ausgewaehlte reale Programme laufen bis in interaktive Szenen
- Grafik, Audio und Eingabe funktionieren zusammen
- ungeloeste Kontrollflussstellen sind diagnostizierbar
- Abstuerze erzeugen verwertbare Berichte
- Performance ist fuer Testtitel praktikabel
- Kompatibilitaetsmatrix und bekannte Fehler sind gepflegt

## Release-Gate: v1.0.0

Voraussetzungen:

- dokumentierter und stabiler CLI- und Manifestvertrag
- versionierte Runtime-ABI
- reproduzierbarer Build
- keine bekannten stillen Fehlkompilierungen im unterstuetzten Bereich
- definierte und dokumentierte SH-4- und Dreamcast-Abdeckung
- automatisierte CPU-, IR-, Runtime- und Plattformtests
- Homebrew-Referenzkorpus laeuft reproduzierbar
- Lizenz-, Rechts- und Drittanbieterhinweise sind vollstaendig
- keine BIOS-, Disc-, Spiel- oder Assetdaten im Repository oder Release
- Upgrade- und Migrationshinweise fuer kuenftige Versionen

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
- optionale grafische Analyseoberflaeche

## Nicht-Ziele vor v1.0

- perfekte Zyklusgenauigkeit fuer jede Dreamcast-Komponente
- vollstaendige Emulation aller seltenen Peripheriegeraete
- automatische Verteilung urheberrechtlich geschuetzter Eingaben
- unkontrollierte Ausfuehrung unbekannter oder selbstmodifizierender Pfade
- mehrere experimentelle Backends, bevor das C++-Backend stabil ist

## Definition of Done

Ein Roadmap-Task gilt nur als abgeschlossen, wenn:

1. der Code kompiliert
2. bestehende Tests unveraendert bestehen
3. neue Semantik passende Tests besitzt
4. Fehlerfaelle getestet sind
5. relevante Dokumentation aktualisiert ist
6. keine generierten Builddateien versehentlich committed wurden
7. der Scope des Tasks eingehalten wurde
8. die Aenderung unabhaengig implementiert wurde
9. der Commit oder PR die Task-ID nennt
10. ein spaeterer Bearbeiter aus Tests und Dokumentation erkennen kann, was garantiert wird

Die detaillierte Task-Liste steht in `docs/TASKS.md`.
Die Arbeitsregeln fuer Codex stehen in `docs/CODEX_HANDOFF.md`.
