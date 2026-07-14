# KatanaRecomp Task-Katalog

Dieses Dokument zerlegt die Roadmap in issue-taugliche Arbeitspakete.

## Regeln

- Eine Aenderung bearbeitet normalerweise genau eine Task-ID.
- Abhaengigkeiten muessen vor Beginn abgeschlossen sein.
- Jeder Task braucht Tests.
- Scope-Erweiterungen werden als neuer Task dokumentiert.
- Status wird mit `[ ]`, `[~]`, `[x]`, `[!]` gepflegt.

## Empfohlene naechste Reihenfolge

1. KR-1101
2. KR-1102
3. KR-1103
4. KR-1104
5. KR-1105
6. KR-1106
7. KR-1107
8. KR-1201
9. KR-1202
10. KR-1203
11. KR-1204
12. KR-1205
13. KR-1301
14. KR-1302
15. KR-1303
16. KR-1304
17. KR-1305

---

## v0.11.0 - ALU und Statussemantik

### [x] KR-1101 - SUB, NEG und NOT

Abhaengigkeiten: v0.10.0

Umfang:

- `SUB Rm,Rn`
- `NEG Rm,Rn`
- `NOT Rm,Rn`
- Decoder
- IR
- C++-Codegen
- Decoder-, IR- und End-to-End-Tests

Akzeptanz:

- 32-Bit-Wraparound ist getestet
- Quellregister bleiben unveraendert
- bekannte Alt-Tests bestehen

### [x] KR-1102 - AND, OR und XOR

Abhaengigkeiten: KR-1101

Umfang:

- Registerformen
- Immediate-Formen fuer R0
- optional GBR-Speicherformen als separater Folgetask, falls Scope zu gross wird

Akzeptanz:

- Bitmuster `0x00000000`, `0xFFFFFFFF`, alternierende Bits
- Immediate-Werte werden ohne Vorzeichenerweiterung behandelt

### [x] KR-1103 - CMP-Varianten

Abhaengigkeiten: KR-1101

Umfang:

- `CMP/HS`
- `CMP/GE`
- `CMP/HI`
- `CMP/GT`
- `CMP/PZ`
- `CMP/PL`
- `CMP/STR`

Akzeptanz:

- signed und unsigned sind getrennt
- Gleichheit und Grenzwerte sind getestet
- `CMP/STR` testet Bytegruppen korrekt

### [x] KR-1104 - Carry und Overflow

Abhaengigkeiten: KR-1101

Umfang:

- `ADDC`
- `ADDV`
- `SUBC`
- `SUBV`
- `NEGC`

Akzeptanz:

- T-Bit als Carry, Borrow oder Overflow ist pro Instruktion dokumentiert
- Grenzwerte fuer signed und unsigned sind getestet

### [x] KR-1105 - Extend, Swap und XTRCT

Abhaengigkeiten: KR-1101

Umfang:

- `EXTU.B`, `EXTU.W`
- `EXTS.B`, `EXTS.W`
- `SWAP.B`, `SWAP.W`
- `XTRCT`

Akzeptanz:

- Byte- und Word-Grenzwerte
- Vorzeichenerweiterung ist plattformunabhaengig

### [x] KR-1106 - DT und MOVT

Abhaengigkeiten: KR-1103

Umfang:

- `DT Rn`
- `MOVT Rn`
- Schleifentest mit `BF`

Akzeptanz:

- `DT` setzt T nur bei Ergebnis null
- End-to-End-Schleife terminiert mit erwartetem Registerzustand

### [x] KR-1107 - v0.11 Release-Gate

Abhaengigkeiten: KR-1101 bis KR-1106

Umfang:

- Abdeckungsdokument aktualisieren
- CHANGELOG aktualisieren
- Version auf 0.11.0 setzen
- kompletter Regressionstest
- Git-Tag vorbereiten

---

## v0.12.0 - Shifts und Rotationen

### [x] KR-1201 - Ein-Bit-Shifts

Abhaengigkeiten: KR-1107

Umfang:

- `SHLL`, `SHLR`
- `SHAL`, `SHAR`

Akzeptanz:

- herausgeschobenes Bit landet korrekt in T
- arithmetischer und logischer Shift sind getrennt getestet

### [x] KR-1202 - Feste Mehrfach-Shifts

Abhaengigkeiten: KR-1201

Umfang:

- Shift um 2, 8 und 16 Bit
- links und rechts

### [x] KR-1203 - Rotationen

Abhaengigkeiten: KR-1201

Umfang:

- `ROTL`, `ROTR`
- `ROTCL`, `ROTCR`

Akzeptanz:

- Rotationen mit und ohne T-Bit
- bitgenaue Testvektoren

### [x] KR-1204 - Dynamische Shifts

Abhaengigkeiten: KR-1201

Umfang:

- `SHAD`
- `SHLD`

Akzeptanz:

- positive, negative und Grenz-Shiftwerte
- Verhalten bei grossen Shiftzaehlern dokumentiert

### [x] KR-1205 - v0.12 Release-Gate

Abhaengigkeiten: KR-1201 bis KR-1204

---

## v0.13.0 - Multiplikation, Division und MAC

### [x] KR-1301 - Einfache Multiplikation

Abhaengigkeiten: KR-1205

Umfang:

- `MUL.L`
- `MULS.W`
- `MULU.W`

### [x] KR-1302 - Doppelte Multiplikation

Abhaengigkeiten: KR-1301

Umfang:

- `DMULS.L`
- `DMULU.L`
- `MACH`, `MACL`

### [x] KR-1303 - MAC-Instruktionen

Abhaengigkeiten: KR-1302

Umfang:

- `MAC.W`
- `MAC.L`
- Speicheradressierung und Registerfortschaltung

### [x] KR-1304 - Division

Abhaengigkeiten: KR-1104

Umfang:

- `DIV0U`
- `DIV0S`
- `DIV1`
- Q-, M- und T-Bit

### [x] KR-1305 - v0.13 Release-Gate

Abhaengigkeiten: KR-1301 bis KR-1304

---

## v0.14.0 - Adressierung und Systemregister

### [x] KR-1401 - Pre-Decrement und Post-Increment

Abhaengigkeiten: KR-1305

Umfang:

- Byte-, Word- und Long-Formen
- Sonderfall identisches Quell- und Zielregister

### [x] KR-1402 - Register-Displacements

Abhaengigkeiten: KR-1401

### [x] KR-1403 - R0-indexierte Adressierung

Abhaengigkeiten: KR-1402

### [x] KR-1404 - GBR-relative Adressierung

Abhaengigkeiten: KR-1402

### [x] KR-1405 - PC-relative Loads und MOVA

Abhaengigkeiten: KR-1402

Akzeptanz:

- PC-Ausrichtung und Displacement-Skalierung sind getestet

### [x] KR-1406 - Systemregistertransfers

Abhaengigkeiten: KR-1401

Umfang:

- `STS`, `LDS`
- `STC`, `LDC`
- direkte und Speicherformen

### [x] KR-1407 - Privilegierte Kontrollinstruktionen

Abhaengigkeiten: KR-1406

Umfang:

- `TRAPA`
- `RTE`
- `SLEEP`
- vorerst klarer Runtime-Vertrag, auch wenn Plattformlogik spaeter folgt

### [x] KR-1408 - v0.14 Release-Gate

Abhaengigkeiten: KR-1401 bis KR-1407

Akzeptanz:

- Debug- und Release-Build sind sauber
- vollstaendige Regression besteht
- KR-1402 bis KR-1405 laufen aus committed Binaer-Fixtures ueber Binary Reader und CLI
- Version, Changelog, Status und Release Notes stimmen ueberein

---

## v0.15.0 - Decoder-Haertung

### [x] KR-1501 - Zentrale Instruktionsmetadaten

Abhaengigkeiten: KR-1408

### [x] KR-1502 - Decoder-Kollisionspruefung

Abhaengigkeiten: KR-1501

### [x] KR-1503 - ISA-Abdeckungsbericht

Abhaengigkeiten: KR-1501

### [x] KR-1504 - Spezifikations-Testvektoren

Abhaengigkeiten: KR-1501

### [x] KR-1505 - Decoder-Fuzzer

Abhaengigkeiten: KR-1502

### [x] KR-1506 - v0.15 Release-Gate

Abhaengigkeiten: KR-1501 bis KR-1505

---

## v0.16.0 - Executable Images und Loader

### [x] KR-1601 - Image- und Segmentmodell

Abhaengigkeiten: KR-1506

### [x] KR-1602 - Raw-Binary-Loader

Abhaengigkeiten: KR-1601

### [x] KR-1603 - ELF32-SH-Loader

Abhaengigkeiten: KR-1601

### [x] KR-1604 - Symbole und Map-Dateien

Abhaengigkeiten: KR-1603

### [x] KR-1605 - Relocations

Abhaengigkeiten: KR-1603

### [x] KR-1606 - Projektmanifest Version 1

Abhaengigkeiten: KR-1602, KR-1603

### [x] KR-1607 - v0.16 Release-Gate

Abhaengigkeiten: KR-1601 bis KR-1606

---

## v0.17.0 - Rekursive Analyse

### [x] KR-1701 - Worklist ab Einstiegspunkten

Abhaengigkeiten: KR-1607

### [x] KR-1702 - Code-Daten-Klassifikation

Abhaengigkeiten: KR-1701

### [x] KR-1703 - Herkunft und Konfidenz von Funktionen

Abhaengigkeiten: KR-1701

### [x] KR-1704 - Nicht erreichbare Bereiche

Abhaengigkeiten: KR-1702

### [x] KR-1705 - Ueberlappende Bereiche

Abhaengigkeiten: KR-1702

### [x] KR-1706 - Analysebericht

Abhaengigkeiten: KR-1701 bis KR-1705

### [x] KR-1707 - v0.17 Release-Gate

Abhaengigkeiten: KR-1701 bis KR-1706

---

## v0.18.0 - Indirekter Kontrollfluss

### [x] KR-1801 - Lokale Konstantenpropagation

Abhaengigkeiten: KR-1707

### [x] KR-1802 - Registerwertanalyse

Abhaengigkeiten: KR-1801

### [x] KR-1803 - Einfache indirekte Calls und Jumps

Abhaengigkeiten: KR-1802

### [x] KR-1804 - Jump Tables

Abhaengigkeiten: KR-1802

### [x] KR-1805 - Override-Datei

Abhaengigkeiten: KR-1606

### [x] KR-1806 - Bericht ungeloester Kontrollflussstellen

Abhaengigkeiten: KR-1803, KR-1804

### [x] KR-1807 - v0.18 Release-Gate

Abhaengigkeiten: KR-1801 bis KR-1806

---

## v0.19.0 - IR Version 2

### [x] KR-1901 - Explizite Operandbreiten

Abhaengigkeiten: KR-1807

### [x] KR-1902 - Explizite Statusregistereffekte

Abhaengigkeiten: KR-1901

### [x] KR-1903 - Speicher-Seiteneffekte

Abhaengigkeiten: KR-1901

### [x] KR-1904 - Delay-Slot-Normalisierung

Abhaengigkeiten: KR-1901

### [x] KR-1905 - IR-Verifier

Abhaengigkeiten: KR-1901 bis KR-1904

### [x] KR-1906 - Deterministische Text- und JSON-Ausgabe

Abhaengigkeiten: KR-1905

### [x] KR-1907 - v0.19 Release-Gate

Abhaengigkeiten: KR-1901 bis KR-1906

---

## v0.20.0 - IR-Optimierungen

### [x] KR-2001 - Constant Folding

Abhaengigkeiten: KR-1907

### [x] KR-2002 - Copy Propagation

Abhaengigkeiten: KR-2001

### [x] KR-2003 - Dead-Code-Elimination

Abhaengigkeiten: KR-2002

### [x] KR-2004 - CFG-Simplifizierung

Abhaengigkeiten: KR-2003

### [x] KR-2005 - Load-Store-Vereinfachung

Abhaengigkeiten: KR-1903, KR-2001

### [x] KR-2006 - Pass-Pipeline und Debug-Schalter

Abhaengigkeiten: KR-2001 bis KR-2005

### [x] KR-2007 - v0.20 Release-Gate

Abhaengigkeiten: KR-2001 bis KR-2006

---

## v0.21.0 - Runtime und CPU-Zustand

### [x] KR-2101 - Runtime aus generiertem Code auslagern

Abhaengigkeiten: KR-2007

### [x] KR-2102 - Vollstaendigen CPU-Zustand zentralisieren

Abhaengigkeiten: KR-2101

### [x] KR-2103 - Deterministischen Reset-Zustand definieren

Abhaengigkeiten: KR-2102

### [x] KR-2104 - v0.21.0 Release-Gate

Abhaengigkeiten: KR-2101 bis KR-2103

Konsolidierung gegenueber der frueheren Feinplanung:

- KR-2102 umfasst die frueheren Aufgaben KR-2102 bis KR-2105:
  Integer-Zustand, banked Register, vorbereiteten FPU-Zustand und Runtime-ABI.
- KR-2103 entspricht dem frueheren KR-2106 fuer den deterministischen Reset.
- KR-2104 entspricht dem frueheren KR-2107 als v0.21.0 Release-Gate.

---

## v0.22.0 - Speicherbus

### [x] KR-2201 - Regionbasierter Bus

Abhaengigkeiten: KR-2104

Umfang:

- zentrale 32-Bit-Adressdekodierung
- benannte, deterministisch sortierte Speicherregionen
- registrierbare Speichergeraete
- Little-Endian-Mehrbytezugriffe
- Read-only-Schutz und sichtbare Adressfehler

Akzeptanz:

- leere, ueberlappende und ueberlaufende Regionen werden abgelehnt
- ein Zugriff darf keine Regionsgrenze ueberschreiten
- nicht zugeordnete Adressen schlagen sichtbar fehl
- bestehende generierte Programme verwenden weiterhin die Runtime-API

### [x] KR-2202 - RAM und Spiegelungen

Abhaengigkeiten: KR-2201

Umfang:

- 16 MiB Dreamcast-Haupt-RAM als gemeinsames lineares Backing
- vier physische Area-3-Fenster pro direktem SH-4-Segment
- U0/P0-, P1-, P2- und derzeitiger P3-No-MMU-Zugriff
- keine Haupt-RAM-Abbildung im P4-Bereich
- atomare Ablehnung kollidierender Buskonfigurationen

Akzeptanz:

- alle 28 direkten Aliasfenster lesen und schreiben dasselbe Backing
- erstes und letztes RAM-Byte sind ueber Aliase erreichbar
- Little-Endian-Mehrbytezugriffe bleiben zwischen Aliasen konsistent
- fehlgeschlagene Konfigurationen hinterlassen keine Teilabbildungen
- Flycast und dcrecomp wurden nur zum unabhaengigen Abgleich des beobachtbaren Adresslayouts verwendet

### [x] KR-2203 - VRAM und AICA-RAM-Abstraktionen

Abhaengigkeiten: KR-2201

Umfang:

- 8 MiB Dreamcast-VRAM als gemeinsames lineares Backing
- 28 lineare 64-Bit-Pfad-Aliase und 28 bankinterleavte 32-Bit-Pfad-Aliase
- 2 MiB AICA-RAM mit 28 direkten Aliasfenstern
- U0/P0-, P1-, P2- und derzeitiger P3-No-MMU-Zugriff ohne P4-Abbildung
- atomare Ablehnung kollidierender Buskonfigurationen

Akzeptanz:

- lineare und interleavte VRAM-Sichten greifen auf dasselbe Backing zu
- der 32-Bit-Pfad verteilt aufeinanderfolgende Woerter korrekt auf beide VRAM-Baenke
- alle AICA-RAM-Aliase lesen und schreiben dasselbe Backing
- erstes und letztes Byte sowie Little-Endian-Mehrbytezugriffe sind abgedeckt
- Haupt-RAM, VRAM und AICA-RAM lassen sich gemeinsam ohne Ueberlappung abbilden
- fehlgeschlagene Konfigurationen hinterlassen keine Teilabbildungen
- Flycast wurde nur zum unabhaengigen Abgleich von Adresslayout und beobachtbarem Bankverhalten verwendet

### [x] KR-2204 - BIOS- und Flash-Abstraktionen

Abhaengigkeiten: KR-2201

Umfang:

- 2 MiB Dreamcast-BIOS mit sieben direkten read-only Aliasfenstern
- 128 KiB Dreamcast-Flash mit sieben direkten beschreibbaren Aliasfenstern
- optionale, exakt grosse BIOS- und Flash-Abbilder ohne proprietaere Fixtures
- deterministische `0xFF`-Initialisierung ohne bereitgestelltes Abbild
- atomare Ablehnung falscher Abbildgroessen und kollidierender Konfigurationen

Akzeptanz:

- alle BIOS-Aliase lesen dasselbe Backing und lehnen Bus-Schreibzugriffe ab
- alle Flash-Aliase lesen und schreiben dasselbe persistente Backing
- erstes und letztes Byte sowie Little-Endian-Mehrbytezugriffe sind abgedeckt
- P4 enthaelt weder BIOS noch Flash
- RAM, VRAM, AICA-RAM, BIOS und Flash lassen sich gemeinsam abbilden
- fehlgeschlagene Konfigurationen hinterlassen keine Teilabbildungen
- Flycast wurde nur zum unabhaengigen Abgleich von Adresslayout und Zugriffsrechten verwendet
- keine BIOS-, Flash- oder anderen geschuetzten Binaerdaten wurden hinzugefuegt

### [x] KR-2205 - MMIO-Handler

Abhaengigkeiten: KR-2201

Umfang:

- breitenbewusste virtuelle 8-, 16- und 32-Bit-Zugriffe auf `MemoryDevice`
- zentraler Little-Endian-Fallback fuer bestehende bytebasierte Geraete
- `MmioMemoryDevice` mit optionalen Lese- und Schreibcallbacks
- lokaler Geraeteoffset, maskierter Wert und explizite Zugriffsbreite pro Callback
- Runtime-ABI Version 4 fuer die erweiterte virtuelle Geraete-API

Akzeptanz:

- ein 8-, 16- oder 32-Bit-Buszugriff loest genau einen passenden MMIO-Callback aus
- MMIO-Aliase verwenden denselben Handler mit identischen lokalen Offsets
- fehlende Lese- oder Schreibhandler schlagen sichtbar fehl
- Read-only-Regionsschutz greift vor dem Schreibcallback
- ueberlaufende Zugriffe rufen keinen Handler auf
- bestehende bytebasierte Speichergeraete behalten Little-Endian-Mehrbytezugriffe
- generierter Code prueft Runtime-ABI Version 4

### [x] KR-2206 - Ausrichtung, Fehler und Watchpoints

Abhaengigkeiten: KR-2201

Umfang:

- standardmaessig strikte natuerliche Ausrichtung fuer 16- und 32-Bit-Zugriffe
- expliziter permissiver Modus fuer Diagnose und Kompatibilitaet
- `MemoryAccessError` mit Operation, Adresse, Breite, Regionsname und Fehlergrund
- globale Trace-Handler fuer alle erfolgreichen Speicherzugriffe
- Watchpoints mit Adressbereich und Read-, Write- oder ReadWrite-Filter
- Runtime-ABI Version 5 fuer den erweiterten `Memory`-Zustand
- permissiver historischer `CpuState`-Kompatibilitaetsspeicher bis zur SH-4-Adressfehlerbehandlung

Akzeptanz:

- fehlplatzierte Halfword- und Word-Zugriffe werden vor dem Geraet abgelehnt
- permissive Busse behalten unaligned Little-Endian-Zugriffe
- ungemappte, regionsueberschreitende, schreibgeschuetzte und ueberlaufende Zugriffe sind unterscheidbar
- erfolgreiche Zugriffe melden Operation, absolute Adresse, Breite, Wert und Regionsname
- Watchpoints reagieren auf ueberlappende Zugriffsbereiche und passende Zugriffstypen
- Aliase bleiben ueber den gemeldeten Regionsnamen unterscheidbar
- fehlgeschlagene Zugriffe erzeugen keine Trace- oder Watchpoint-Ereignisse
- Watchpoints lassen sich eindeutig entfernen und vollstaendig leeren
- generierte Speicher-Grenzfalltests pruefen strukturierte Fehlergruende statt veralteter Standardausnahmen
- generierter Code prueft Runtime-ABI Version 5

### [x] KR-2207 - v0.22 Release-Gate

Abhaengigkeiten: KR-2201 bis KR-2206

Umfang:

- Version und Release-Dokumentation auf v0.22.0 aktualisieren
- 32-Bit-VRAM-Offsetabbildung als zentrale, testbare Runtime-Funktion formulieren
- alle 8.388.608 VRAM-Byte-Offsets exhaustiv und bijektiv pruefen
- Bank-, Byte-, Halfword-, Word-, Sequenz- und Aliasgrenzen zusaetzlich ueber den Bus pruefen
- frische lokale Debug- und Release-Builds
- GitHub-Actions-Regression unter Linux/GCC und Windows/MSVC
- annotierten Tag `v0.22.0` erst nach erfolgreichen CI-Checks erstellen

Akzeptanz:

- alle 89 Tests bestehen in frischen lokalen Debug- und Release-Builds
- die exhaustive VRAM-Pruefung deckt beide Baenke und den kompletten 8-MiB-Offsetraum ab
- Linux- und Windows-CI bauen und testen Debug sowie Release erfolgreich
- der Release-Commit besitzt echte GitHub-Statuschecks
- `VERSION`, CMake, README, Roadmap, Status, Changelog und Release Notes stimmen auf 0.22.0 ueberein
- keine Buildartefakte, ROMs, BIOS-Dateien, Disc-Images oder geschuetzten Assets werden committed
- der Release-Tag zeigt exakt auf den von CI bestaetigten Release-Commit

---

## v0.23.0 - Ausnahmen und Interrupts

### [x] KR-2301 - SR-Felder und Interruptmasken

Abhaengigkeiten: KR-2104

### [x] KR-2302 - Exception-Eintritt

Abhaengigkeiten: KR-2301, KR-2207

### [x] KR-2303 - Interrupt-Controller

Abhaengigkeiten: KR-2301

### [x] KR-2304 - TRAPA und RTE

Abhaengigkeiten: KR-2302

### [x] KR-2305 - Delay-Slot-Ausnahmen

Abhaengigkeiten: KR-2302

### [x] KR-2306 - v0.23 Release-Gate

Abhaengigkeiten: KR-2301 bis KR-2305

### [x] KR-2307 - Ausfuehrbare Delay-Slot- und Interrupt-Review-Regressionen

Abhaengigkeiten: KR-2303, KR-2305

---

## v0.24.0 und v0.25.0 - FPU

### [ ] KR-2401 - FR- und XF-Baenke

Abhaengigkeiten: KR-2306

### [ ] KR-2402 - Single-Precision-Arithmetik

Abhaengigkeiten: KR-2401

### [ ] KR-2403 - Vergleiche und Konvertierungen

Abhaengigkeiten: KR-2402

### [ ] KR-2404 - FPSCR-Modi

Abhaengigkeiten: KR-2401

### [ ] KR-2405 - Double-Precision

Abhaengigkeiten: KR-2404

### [ ] KR-2501 - FSCA und FSRRA

Abhaengigkeiten: KR-2405

### [ ] KR-2502 - FIPR und FTRV

Abhaengigkeiten: KR-2401

### [ ] KR-2503 - NaN, Rundung und Sonderwerte

Abhaengigkeiten: KR-2402, KR-2405

### [ ] KR-2504 - FPU-Konformitaetssuite

Abhaengigkeiten: KR-2401 bis KR-2503

---

## v0.26.0 bis v0.31.0 - Dreamcast-Plattform

### [ ] KR-2601 - Plattformkonfiguration und Bootzustand

Abhaengigkeiten: KR-2306

### [ ] KR-2602 - Homebrew-Raw- und ELF-Start

Abhaengigkeiten: KR-1607, KR-2601

### [ ] KR-2603 - Minimales Plattformlogging

Abhaengigkeiten: KR-2601

### [ ] KR-2701 - Maple-Bus

Abhaengigkeiten: KR-2601

### [ ] KR-2702 - Controller

Abhaengigkeiten: KR-2701

### [ ] KR-2703 - VMU-Minimum

Abhaengigkeiten: KR-2701

### [ ] KR-2801 - PVR-Registerminimum

Abhaengigkeiten: KR-2207, KR-2601

### [ ] KR-2802 - Framebuffer-Ausgabe

Abhaengigkeiten: KR-2801

### [ ] KR-2803 - Tile-Accelerator-Grundpfad

Abhaengigkeiten: KR-2801

### [ ] KR-2804 - Texturformate und Render-Backend

Abhaengigkeiten: KR-2803

### [ ] KR-2901 - AICA-Registerminimum

Abhaengigkeiten: KR-2207

### [ ] KR-2902 - PCM und ADPCM

Abhaengigkeiten: KR-2901

### [ ] KR-2903 - Mixer und Host-Audio

Abhaengigkeiten: KR-2902

### [ ] KR-2904 - ARM7-Strategie dokumentieren und implementieren

Abhaengigkeiten: KR-2901

### [ ] KR-3001 - Disc- und Dateiquellen-Abstraktion

Abhaengigkeiten: KR-2601

### [ ] KR-3002 - GD-ROM-Kommandos

Abhaengigkeiten: KR-3001

### [ ] KR-3003 - ISO9660

Abhaengigkeiten: KR-3001

### [ ] KR-3004 - Asynchrone Reads und Timing

Abhaengigkeiten: KR-3002

### [ ] KR-3101 - Event-Scheduler

Abhaengigkeiten: KR-2601

### [ ] KR-3102 - TMU und RTC

Abhaengigkeiten: KR-3101

### [ ] KR-3103 - DMA

Abhaengigkeiten: KR-3101, KR-2207

### [ ] KR-3104 - Plattform-Interruptintegration

Abhaengigkeiten: KR-2306, KR-3101

### [ ] KR-3105 - Frame- und Audio-Taktung

Abhaengigkeiten: KR-2802, KR-2903, KR-3101

---

## v0.32.0 bis v0.34.0 - Codegen und Dispatch

### [ ] KR-3201 - Backend-Interface

Abhaengigkeiten: KR-2104

### [ ] KR-3202 - C++-Backend migrieren

Abhaengigkeiten: KR-3201

### [ ] KR-3203 - ABI-Faehigkeitspruefung

Abhaengigkeiten: KR-3201, KR-2102

### [ ] KR-3301 - Translation-Unit-Partitionierung

Abhaengigkeiten: KR-3202

### [ ] KR-3302 - Deterministische Dateinamen

Abhaengigkeiten: KR-3301

### [ ] KR-3303 - Inkrementeller Codegen-Cache

Abhaengigkeiten: KR-3302

### [ ] KR-3304 - Parallele Ausgabe und Buildintegration

Abhaengigkeiten: KR-3303

### [ ] KR-3401 - Laufzeit-Funktionstabelle

Abhaengigkeiten: KR-1807, KR-2102

### [ ] KR-3402 - Indirekter Call- und Jump-Dispatch

Abhaengigkeiten: KR-3401

### [ ] KR-3403 - Kontrollierter Fallback

Abhaengigkeiten: KR-3402

### [ ] KR-3404 - Selbstmodifizierenden Code erkennen

Abhaengigkeiten: KR-2207, KR-3402

---

## v0.35.0 bis v0.37.0 - Werkzeuge und Qualitaet

### [ ] KR-3501 - Manifest-Schema und Versionierung

Abhaengigkeiten: KR-1606

### [ ] KR-3502 - Stabile CLI und Exitcodes

Abhaengigkeiten: KR-3501

### [ ] KR-3503 - JSON-Berichte

Abhaengigkeiten: KR-1706, KR-1906

### [ ] KR-3504 - Override- und Hint-Dateien

Abhaengigkeiten: KR-1805, KR-3501

### [ ] KR-3601 - Symbolische Namen

Abhaengigkeiten: KR-1604

### [ ] KR-3602 - Adress-zu-Quelle-Mapping

Abhaengigkeiten: KR-3202

### [ ] KR-3603 - Crashberichte

Abhaengigkeiten: KR-2102

### [ ] KR-3604 - Tracing und Watchpoints

Abhaengigkeiten: KR-2206, KR-3603

### [ ] KR-3605 - CFG- und Callgraph-Export

Abhaengigkeiten: KR-1706

### [ ] KR-3701 - Windows- und Linux-CI

Abhaengigkeiten: keine

### [ ] KR-3702 - Sanitizer-Builds

Abhaengigkeiten: KR-3701

### [ ] KR-3703 - Fuzzer

Abhaengigkeiten: KR-1505, KR-1905

### [ ] KR-3704 - Coverage

Abhaengigkeiten: KR-3701

### [ ] KR-3705 - Formatierung und statische Analyse

Abhaengigkeiten: KR-3701

### [ ] KR-3706 - Reproduzierbare Release-Artefakte

Abhaengigkeiten: KR-3304, KR-3502, KR-3701

---

## v0.38.0 bis v0.40.0 - Kompatibilitaet und Leistung

### [ ] KR-3801 - Rechtlich sauberes Homebrew-Testkorpus

Abhaengigkeiten: KR-2602

### [ ] KR-3802 - CPU-Konformitaetsprogramm

Abhaengigkeiten: KR-1506, KR-2504

### [ ] KR-3803 - Eingabe-Beispiel

Abhaengigkeiten: KR-2702

### [ ] KR-3804 - 2D-Grafik-Beispiel

Abhaengigkeiten: KR-2804

### [ ] KR-3805 - Audio-Beispiel

Abhaengigkeiten: KR-2903

### [ ] KR-3806 - Zusammenhaengendes Testspiel

Abhaengigkeiten: KR-3803, KR-3804, KR-3805, KR-3105

### [ ] KR-3901 - Benchmark-Suite

Abhaengigkeiten: KR-3806

### [ ] KR-3902 - Hot-Block-Analyse

Abhaengigkeiten: KR-3901

### [ ] KR-3903 - Dispatch- und Speicher-Fastpaths

Abhaengigkeiten: KR-3902, KR-3402

### [ ] KR-3904 - Inlining und Codegroessenstrategie

Abhaengigkeiten: KR-3301, KR-3901

### [ ] KR-3905 - LTO und PGO

Abhaengigkeiten: KR-3901

### [ ] KR-4001 - Oeffentliche Installationsdokumentation

Abhaengigkeiten: KR-3502, KR-3706

### [ ] KR-4002 - Architektur- und Manifestreferenz

Abhaengigkeiten: KR-3501

### [ ] KR-4003 - Lizenz- und Rechtspruefung

Abhaengigkeiten: KR-3801

### [ ] KR-4004 - Kompatibilitaetsbericht

Abhaengigkeiten: KR-3806

### [ ] KR-4005 - v0.40.0 Pre-Alpha-Release

Abhaengigkeiten: KR-4001 bis KR-4004

---

## Spaetere Release-Gates

### [ ] KR-5000 - v0.50.0 Alpha-Gate

Siehe `ROADMAP.md`.

### [ ] KR-7500 - v0.75.0 Beta-Gate

Siehe `ROADMAP.md`.

### [ ] KR-10000 - v1.0.0 Release-Gate

Siehe `ROADMAP.md`.
