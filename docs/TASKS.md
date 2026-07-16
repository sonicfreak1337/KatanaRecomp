# KatanaRecomp Task-Katalog

Dieses Dokument zerlegt die Roadmap in issue-taugliche Arbeitspakete.

## Regeln

- Eine Aenderung bearbeitet normalerweise genau eine Task-ID.
- Abhaengigkeiten muessen vor Beginn abgeschlossen sein.
- Jeder Task braucht Tests.
- Scope-Erweiterungen werden als neuer Task dokumentiert.
- Status wird mit `[ ]`, `[~]`, `[x]`, `[!]` oder `[?]` fuer offene Architekturentscheidungen gepflegt.

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

### [x] KR-2401 - FR- und XF-Baenke

Abhaengigkeiten: KR-2306

### [x] KR-2402 - Single-Precision-Arithmetik

Abhaengigkeiten: KR-2401

### [x] KR-2403 - Vergleiche und Konvertierungen

Abhaengigkeiten: KR-2402

### [x] KR-2404 - FPSCR-Modi

Abhaengigkeiten: KR-2401

### [x] KR-2405 - Double-Precision

Abhaengigkeiten: KR-2404

### [x] KR-2406 - v0.24.0 Release-Gate

Abhaengigkeiten: KR-2401 bis KR-2405

Akzeptanz:

- NaN-, Infinity- und beide unterstuetzten Rundungsmodi sind regressionsgesichert
- FTRC-Grenzen, Nullvorzeichen, Signaling-NaN, FMAC-Ueberlappung und Double-/Konvertierungsrundung sind regressionsgesichert
- DN=1 spuelt denormalisierte Single- und Double-Werte mit korrektem Vorzeichen; Transfers, FNEG und FABS bleiben rohbitgetreu
- reservierte RM-Werte erreichen keine FPU-Rechenwirkung
- 64-Bit-FMOV ist ueber angrenzende Regionsgrenzen und am nicht abgebildeten Rand getestet
- FPU-Sperren und ungueltige Moduskombinationen nehmen den strukturierten Exception-Pfad
- generierter FPU-Code wird kompiliert und ausgefuehrt, einschliesslich Delay-Slot-Fehlern
- alle 95 Tests bestehen in frischen lokalen Debug- und Release-Builds
- Roadmap, Status, Changelog, Versionsdateien und Release-Notiz stimmen ueberein
- CI wird gemaess Projektentscheidung erst am Alpha-Gate wieder verpflichtend

### [x] KR-2501 - FSCA und FSRRA

Abhaengigkeiten: KR-2405

### [x] KR-2502 - FIPR und FTRV

Abhaengigkeiten: KR-2401

### [x] KR-2503 - NaN, Rundung und Sonderwerte

Abhaengigkeiten: KR-2402, KR-2405

### [x] KR-2504 - FPU-Konformitaetssuite

Abhaengigkeiten: KR-2401 bis KR-2503

Akzeptanz:

- Decoder, IR, Runtime und generierter C++-Pfad decken `FSCA`, `FSRRA`, `FIPR` und `FTRV` ab
- generierter Code wird kompiliert und mit sichtbaren Vektor-, Matrix- und Bankergebnissen ausgefuehrt
- Quadrantenanker sind exakt; weitere FSCA-/FSRRA-Ergebnisse besitzen dokumentierte, plattformuebergreifend gepruefte Toleranzen
- NaN wird kanonisiert, DN und die unterstuetzten Rundungsmodi wirken auch auf Vektoroperationen deterministisch
- frische lokale Debug- und Release-Builds bestehen vollstaendig; CI wird erst am Alpha-Gate wieder verpflichtend

---

## v0.26.0 bis v0.31.0 - Dreamcast-Plattform

### [x] KR-2601 - Plattformkonfiguration und Bootzustand

Abhaengigkeiten: KR-2306

### [x] KR-2602 - Homebrew-Raw- und ELF-Start

Abhaengigkeiten: KR-1607, KR-2601

### [x] KR-2603 - Minimales Plattformlogging

Abhaengigkeiten: KR-2601

### [x] KR-2604 - Firmware-Betriebsart und BIOS-ABI festlegen

Abhaengigkeiten: KR-2204, KR-2601

Umfang:

- BIOS-freien Homebrew-Direkteinstieg als verpflichtenden Standardpfad festhalten
- HLE-BIOS-ABI und optionalen LLE-Firmwarepfad anhand Nutzen, Komplexitaet und Testbarkeit bewerten
- dynamische BIOS-Sprungvektoren im RAM als Bootergebnis statt als statische ROM-Funktionen modellieren
- benoetigte Reset-, Cache-, MMIO- und Syscall-Vertraege pro unterstuetztem Pfad dokumentieren
- proprietaere Abbilder ausschliesslich als optionale, lokale Nutzereingaben behandeln

Akzeptanz:

- die gewaehlten Firmware-Betriebsarten und ihre Grenzen sind als Architekturentscheidung dokumentiert
- der Standardpfad funktioniert ohne BIOS- oder Flash-Datei
- fuer jeden optionalen Pfad sind konkrete Folgeabhaengigkeiten und eine sichtbare Fehlerstrategie benannt
- BIOS-, Flash-, Font-, PVR- oder andere Firmwaredaten gelangen weder in Tests noch in Releases

### [x] KR-2605 - PREF und bootrelevante Cacheeffekte

Abhaengigkeiten: KR-1506, KR-2207, KR-2604

Umfang:

- `PREF @Rn` in Metadaten, Decoder, IR, C++-Codegen und Runtime abbilden
- normales Prefetch-Verhalten und adressabhaengige Store-Queue-Nebenwirkungen explizit unterscheiden
- zusaetzliche Cacheoperationen nur aufnehmen, wenn der unterstuetzte Bootpfad sie nachweislich benoetigt
- Instruktions-, IR-, Runtime- und End-to-End-Tests ausschliesslich mit synthetischen Fixtures

Akzeptanz:

- Opcode-Muster `0000nnnn10000011` wird fuer alle Register korrekt dekodiert
- `PREF` beendet die rekursive Analyse eines sonst gueltigen Pfades nicht mehr als unbekannte Instruktion
- beobachtbare Speicher- oder Store-Queue-Effekte sind deterministisch getestet
- nicht modellierte Cacheeffekte werden dokumentiert und nicht stillschweigend als vollstaendig emuliert ausgegeben

### [x] KR-2606 - Zustandsbehaftetes Flash-Geraetemodell

Abhaengigkeiten: KR-2204, KR-2205, KR-2604

Umfang:

- das lineare Flash-Backing um die fuer den gewaehlten Plattformpfad erforderlichen Programmier-, Loesch- und Kommandozustaende ergaenzen
- Lesezugriffe, Schreibschutz, ungueltige Sequenzen und Reset des Geraetezustands sichtbar definieren
- persistente Aenderungen nur in einer Arbeitskopie oder Copy-on-write-Schicht zulassen
- synthetische Tests fuer erlaubte Bituebergaenge, Loeschen, fehlerhafte Sequenzen und Neustartverhalten

Akzeptanz:

- normale Bus-Schreibzugriffe umgehen das aktivierte Flash-Protokoll nicht
- das urspruengliche Nutzerabbild bleibt bytegenau unveraendert
- deterministische Tests benoetigen keine echte Flash-Datei und keine konsolenspezifischen Daten
- nicht unterstuetzte Herstellerkommandos schlagen sichtbar und reproduzierbar fehl

### [x] KR-2607 - FCNVDS-DN-Review-Regression

Abhaengigkeiten: KR-2403, KR-2503

Akzeptanz:

- das zu Single Precision konvertierte `FCNVDS`-Ergebnis durchlaeuft die zentrale DN-Behandlung
- positive und negative subnormale Ergebnisse werden bei `FPSCR.DN=1` auf `+0` beziehungsweise `-0` gespült
- der unveroeffentlichte Nacharbeitsstand bleibt im Changelog unter `Unreleased`; bestehende Release-Tags werden nicht verschoben

### [x] KR-2701 - Maple-Bus

Abhaengigkeiten: KR-2601

### [x] KR-2702 - Controller

Abhaengigkeiten: KR-2701

### [x] KR-2703 - VMU-Minimum

Abhaengigkeiten: KR-2701

### [x] KR-2801 - PVR-Registerminimum

Abhaengigkeiten: KR-2207, KR-2601

### [x] KR-2802 - Framebuffer-Ausgabe

Abhaengigkeiten: KR-2801

### [x] KR-2803 - Tile-Accelerator-Grundpfad

Abhaengigkeiten: KR-2801

### [x] KR-2804 - Texturformate und Render-Backend

Abhaengigkeiten: KR-2803

### [x] KR-2901 - AICA-Registerminimum

Abhaengigkeiten: KR-2207

### [x] KR-2902 - PCM und ADPCM

Abhaengigkeiten: KR-2901

### [x] KR-2903 - Mixer und Host-Audio

Abhaengigkeiten: KR-2902

### [x] KR-2904 - ARM7-Strategie dokumentieren und implementieren

Abhaengigkeiten: KR-2901

### [x] KR-3001 - Disc- und Dateiquellen-Abstraktion

Abhaengigkeiten: KR-2601

### [x] KR-3002 - GD-ROM-Kommandos

Abhaengigkeiten: KR-3001

### [x] KR-3003 - ISO9660

Abhaengigkeiten: KR-3001

### [x] KR-3004 - Asynchrone Reads und Timing

Abhaengigkeiten: KR-3002

### [x] KR-3005 - GDI-Deskriptoren und Trackmodell

Abhaengigkeiten: KR-3001

Umfang:

- Textformat von `.gdi`-Dateien mit Trackanzahl, relativen Dateipfaden, Sektorformat, Offset und Reihenfolge parsern
- Trackeintraege in ein stabiles internes Modell mit Provenienz je Descriptor-Zeile ueberfuehren
- Dateipfade relativ zur `.gdi`-Datei, nicht zum Arbeitsverzeichnis, aufloesen
- fehlende Dateien, ungueltige Zeilen, doppelte Tracks, unplausible Formate und Groessenkonflikte frueh diagnostizieren

Akzeptanz:

- gueltige `.gdi`-Fixtures laden deterministisch dasselbe Trackmodell auf Windows und Linux
- jeder Descriptorfehler nennt mindestens Track oder Zeile und die betroffene Quelldatei
- Parser und Modell schreiben nie in Quelldateien oder Trackabbilder
- synthetische Testdaten genuegen; keine echten Dreamcast-Discs landen im Repository

### [x] KR-3006 - GDI-Quellenintegration

Abhaengigkeiten: KR-3002, KR-3003, KR-3004, KR-3005

Umfang:

- `.gdi` als erstklassigen Einstieg in die bestehende Disc- und Dateiquellen-Abstraktion einhaengen
- Daten- und Audiotracks mit stabiler Zuordnung fuer GD-ROM- und ISO9660-Lesezugriffe bereitstellen
- einheitliche Quellidentitaet, Hashing und Fehlermeldungen fuer CLI, GUI und spaetere Automatisierung sicherstellen
- Read-only-Vertrag fuer Mehrdateiquellen dokumentieren und testen

Akzeptanz:

- dieselbe `.gdi`-Quelle kann ohne manuelle Dateiumbauten analysiert und ueber die Plattformpfade verwendet werden
- relative Tracks, Sektorformate und Dateigroessenfehler bleiben bis in die oeffentliche Diagnostik nachvollziehbar
- Disc-Operationen verwenden keine Hostpfade als semantische Identitaet
- synthetische Positiv- und Negativtests decken Descriptor, Trackmapping und Dateisystemzugriffe ab

### [x] KR-3101 - Event-Scheduler

Abhaengigkeiten: KR-2601

Umfang:

- eine zentrale, hostzeitfreie 64-Bit-Gastzyklusuhr und stabile Ereignis-IDs bereitstellen
- Ereignisse nach Frist und ID deterministisch ordnen, abbrechen und mit explizitem Budget ausfuehren
- Planung und Cancellation aus Callbacks erlauben, ohne die Zeitmonotonie zu verletzen
- rekursives Advancing und Reset waehrend eines laufenden Advances sichtbar abweisen

Akzeptanz:

- die Gastzyklusuhr laeuft auch bei Reentrancy-Versuchen niemals rueckwaerts
- `advance_to()`, `advance_by()` und `reset()` aus Callbacks liefern `std::logic_error`
- verschachteltes `schedule_at()` und `cancel()` bleiben erlaubt und stabil geordnet
- Budgetstopp, Callbackfehler, Cancellation, Reset und 64-Bit-Ueberlauf sind regressionsgesichert

### [x] KR-3102 - TMU und RTC

Abhaengigkeiten: KR-3101

### [x] KR-3103 - DMA

Abhaengigkeiten: KR-3101, KR-2207

Umfang:

- vier SH-4-DMAC-Kanaele mit SAR, DAR, DMATCR, CHCR und gemeinsamem DMAOR abbilden
- Transfergroessen und feste, inkrementierende sowie dekrementierende Adressmodi ausfuehren
- Auto-, externe und Modulrequests ueber den zentralen Gastzyklus-Scheduler ordnen
- TE-, IE-, AE- und NMIF-Zustaende sowie Kanalprioritaeten sichtbar bereitstellen
- das 32-Bit-DMAC-Registerfenster ueber P4 und den Area-7-Alias an den Memory-Bus anbinden

Akzeptanz:

- Teilfortschritt, Adressfeedback und Abschluss sind an festen Gastzyklen messbar
- gleichzeitige Kanaele folgen deterministisch dem gewaehlten DMAOR-Prioritaetsmodus
- Fehladressierung stoppt alle Kanaele mit AE, ohne TE oder stillen Teilabschluss vorzutaueschen
- Master-Disable, explizite Requests, Write-zero-to-clear und Registerbreiten sind regressionsgesichert
- DDT und reale Pinprotokolle werden sichtbar abgewiesen statt als funktionierend vorgetaeuscht

### [x] KR-3104 - Plattform-Interruptintegration

Abhaengigkeiten: KR-2306, KR-3101

Umfang:

- TMU-, RTC-, DMTE- und DMAE-Pending-Zustaende am zentralen Interrupt-Controller spiegeln
- offizielle SH-4-INTEVT-Codes und die gemeinsamen IPRA-/IPRC-Prioritaetsgruppen abbilden
- drei feste Dreamcast-IRL-Leitungen fuer spaetere System-ASIC-Quellen bereitstellen
- Synchronisation und Annahme als expliziten CPU-Safepoint definieren
- Quittierung am Ursprungsgeraet als einzige Deassert-Bedingung erhalten

Akzeptanz:

- gleichzeitig gesetzte Quellen werden nach Level und stabiler Quellordnung angenommen
- BL und IMASK bleiben wirksam; SPC, INTEVT und VBR-Vektor stammen aus dem zentralen Exception-Pfad
- nicht quittierte Levelquellen erscheinen nach Annahme erneut, quittierte Quellen verschwinden
- TMU-, RTC-, DMTE-, DMAE- und externe IRL-Pfade besitzen eine gemeinsame Laufzeitregression
- externe Geraete werden nicht unter Umgehung des noch fehlenden Dreamcast-System-ASIC direkt verdrahtet

### [x] KR-3105 - Frame- und Audio-Taktung

Abhaengigkeiten: KR-2802, KR-2903, KR-3101

Umfang:

- Video- und Audiokadenzen als unabhaengige Ereignisse auf dem zentralen Scheduler ausfuehren
- Frame- und Audiopuffer-Callbacks mit Gastzyklus, Sequenz und Samplebereich typisieren
- nicht ganzzahlige Taktverhaeltnisse ohne dauerhafte Rundungsdrift akkumulieren
- Stop, Neustart, Reset, Ereignisbudget und Callbackfehler deterministisch behandeln
- vorhandene PVR- und AICA-Recording-Backends ueber den gemeinsamen Medienpfad treiben

Akzeptanz:

- feste Eingaben erzeugen identische Video-/Audiofristen und stabile Gleichzyklusreihenfolge
- rationale Kadenzen erreichen ihre exakten Langzeitfristen ohne Hostzeit
- Budgetstopp ist sichtbar fortsetzbar; Stop und Callbackfehler hinterlassen keine Teilkadenz
- Neustart ankert am aktuellen Gastzyklus, Reset loescht alle Medienzaehler
- callback-internes Stop, Stop/Start und Reset erzeugen weder Doppel- noch Geisterereignisse
- der lokale Sonic-Adventure-Test bleibt ausschliesslich dem spaeter freigegebenen v0.31.0-Gate vorbehalten

---

## v0.32.0 bis v0.34.0 - Codegen und Dispatch

### [x] KR-3201 - Backend-Interface

Abhaengigkeiten: KR-2104

### [x] KR-3202 - C++-Backend migrieren

Abhaengigkeiten: KR-3201

### [x] KR-3203 - ABI-Faehigkeitspruefung

Abhaengigkeiten: KR-3201, KR-2102

### [x] KR-3204 - Block-ABI und Zustandsuebergaben

Abhaengigkeiten: KR-1907, KR-2306, KR-2504, KR-3101, KR-3201

Umfang:

- Eintritts- und Austrittsvertrag fuer generierte Bloecke definieren
- Gast-PC, PR, SR, FPSCR, Delay-Slot-, Ausnahme-, Schlaf- und Schedulerzustand explizit uebergeben
- Blockenden fuer Fallthrough, statischen/dynamischen Sprung, Call, Return, Ausnahme und Interrupt-Safepoint typisieren
- Synchronisationspunkte zwischen generiertem Code, Runtime und Fallback festlegen
- virtuelle und kanonische physische Gastadresse getrennt von jedem Hostzeiger halten

Akzeptanz:

- jeder Blockendtyp besitzt einen synthetischen End-to-End-Test
- ein Backendwechsel erhaelt den vollstaendigen beobachtbaren CPU-Zustand
- Delay-Slot-Ausnahmen melden den korrekten Gast-PC unabhaengig vom Backend
- Hostadressen erscheinen weder als persistente Block-ID noch in reproduzierbaren Metadaten

### [x] KR-3205 - Plattformdienst-Schnittstelle

Abhaengigkeiten: KR-2207, KR-3104, KR-3201, KR-3204

Umfang:

- versionierte Dienste fuer Speicherzugriff, Scheduler, Interruptabfrage, DMA und kontrollierten Fallback definieren
- generierten Code von konkreten PVR-, AICA-, Maple-, GD-ROM- oder Referenzprojekt-Typen entkoppeln
- Faehigkeitsabfrage fuer MMU, Watchpoints, ausfuehrbaren RAM, Firmwaremodus und Fallback bereitstellen
- Fehler fuer fehlende Plattformdienste vor Ausfuehrungsbeginn melden

Akzeptanz:

- ein synthetisches Mock-Backend kann alle Dienstgrenzen ohne Dreamcast-Subsystem implementieren
- fehlende Pflichtfaehigkeiten werden mit Name, ABI-Version und Ursache abgelehnt
- kein generiertes Translation Unit inkludiert interne Plattformheader

### [x] KR-3301 - Translation-Unit-Partitionierung

Abhaengigkeiten: KR-3202

### [x] KR-3302 - Deterministische Dateinamen

Abhaengigkeiten: KR-3301

### [x] KR-3303 - Inkrementeller Codegen-Cache

Abhaengigkeiten: KR-3302

### [x] KR-3304 - Parallele Ausgabe und Buildintegration

Abhaengigkeiten: KR-3303

### [x] KR-3305 - Deterministische Blockmetadaten

Abhaengigkeiten: KR-1706, KR-3204, KR-3301, KR-3304

Umfang:

- Code, Konstantdaten, Symbole und Laufzeitmetadaten getrennt ausgeben
- pro Block virtuelle Adresse, kanonische physische Adresse, Quellsegment, Bytebereich und Provenienz speichern
- Gastopcodes, geschaetzte Gastzyklen, Blockendtyp, direkte Nachfolger und Zustandswaechter erfassen
- Metadatenformat versionieren und unabhaengig von Hostzeigern oder Buildpfaden halten
- Cache-Schluessel um Eingabehashes, Manifest, Overrides, IR-/Optimierungsversion sowie Runtime-/Backend-ABI erweitern

Akzeptanz:

- identische Eingaben erzeugen bei serieller und paralleler Ausgabe bytegleiche Metadaten
- eine Aenderung jeder Cache-Schluesselkomponente invalidiert nur die betroffenen Artefakte
- lokale absolute Pfade und Hostadressen erscheinen nicht in der Ausgabe
- ein 10.000-Block-Synthetikprojekt wird reproduzierbar partitioniert

### [x] KR-3401 - Laufzeit-Blocktabelle

Abhaengigkeiten: KR-1807, KR-2102

Umfang:

- Blockeintraege nach kanonischer 32-Bit-Gastadresse verwalten
- virtuelle Startadresse, physische Herkunft, Blockgroesse, Endtyp und Backendfunktion verbinden
- mehrere gueltige Varianten fuer relevante MMU-/FPSCR-/Runtime-Zustaende zulassen
- statisch erzeugte und zur Laufzeit registrierte Bloecke mit derselben Lookup-Schnittstelle behandeln

Akzeptanz:

- Lookup ist fuer identische Tabellen deterministisch und unabhaengig von Hostzeigern
- Aliase koennen dieselbe physische Herkunft teilen, ohne virtuelle Diagnosen zu verlieren
- doppelte oder ueberlappende Eintraege schlagen mit beiden Provenienzen sichtbar fehl

### [x] KR-3402 - Indirekter Call- und Jump-Dispatch

Abhaengigkeiten: KR-3401

Umfang:

- indirekte Calls, Jumps und Returns getrennt dispatchen
- Zieladressen ueber den aktiven Adressraumvertrag kanonisieren
- generischen Tabellenlookup vor jeder Callsite-Spezialisierung bereitstellen
- Callsite, Ziel, PR, Quellblock und Lookup-Ergebnis fuer Diagnosen erfassen

Akzeptanz:

- P1-/P2-Aliase erreichen nach dokumentierter Kanonisierung denselben physischen Block
- Call, Tail-Jump und Return erhalten jeweils ihre korrekte PR-/PC-Semantik
- ein unbekanntes Ziel wird nie als erfolgreicher No-op oder Nullfunktionsaufruf behandelt
- titelbezogene Zielverschiebungen oder hart kodierte Forced Entries sind nicht Teil der Runtime

### [x] KR-3403 - Kontrollierter Fallback

Abhaengigkeiten: KR-3402

Umfang:

- Richtlinien `abort`, `diagnose`, `interpreter` und optionaler expliziter Nutzerhook definieren
- unbekannte Opcodes, nicht rekonstruierbare Kontrollfluesse und dynamischen Code getrennt klassifizieren
- vollstaendigen CPU-, Speicher-, Ausnahme- und Schedulerzustand an der Grenze synchronisieren
- Fallbacknutzung zaehlen und mit stabilen Gruenden berichten

Akzeptanz:

- kein Fallbackpfad setzt Ausfuehrung nach einem Fehler still fort
- Rueckkehr aus einem Interpreter-Fallback stimmt an einer definierten Blockgrenze mit der Referenzausfuehrung ueberein
- ein deaktivierter Fallback bricht mit Gast-PC, Opcode/Ziel und Ursache ab
- BIOS-Bereiche erhalten keine pauschale Sonderregel zum Ignorieren unbekannter Calls

### [x] KR-3404 - Selbstmodifizierenden Code erkennen

Abhaengigkeiten: KR-2207, KR-3402

Umfang:

- ausfuehrbare RAM-Seiten und alle sie ueberdeckenden Blockvarianten registrieren
- CPU-, DMA- und Copy-Pfad-Schreibzugriffe auf ausfuehrbare Bereiche beobachten
- Seitengenerationen erhoehen, betroffene Bloecke invalidieren und eingehende Links loesen
- unveraenderte Schreibzugriffe optional erst nach bewiesener Bytegleichheit von einer Invalidierung ausnehmen
- Invalidierungsstuerme und wiederholt veraenderte Hotspots diagnostizieren

Akzeptanz:

- ein Schreibzugriff invalidiert jeden ueberlappenden Block vor dessen naechster Ausfuehrung
- Schreibzugriffe ueber P1-/P2-Aliase und DMA treffen dieselbe physische Seitengeneration
- nicht ueberlappende Bloecke bleiben gueltig
- keine Betriebssystem-Seitenschutztechnik ist fuer die Korrektheit zwingend erforderlich

### [x] KR-3405 - Alias- und kopierbewusster Firmware-Handoff

Abhaengigkeiten: KR-1807, KR-2207, KR-2605, KR-3402, KR-3404

Umfang:

- physisch identische P1-/P2-Firmware-Aliase kontrolliert auf eine kanonische Herkunft abbilden
- ROM-nach-RAM-Codekopien mit Quellbereich, Zielbereich und Provenienz darstellen
- Analyse und Dispatch ueber gleichzeitig gemappte ROM-, RAM-, Flash- und MMIO-Segmente verbinden
- indirekte Spruenge in kopierten Code aufloesen oder mit einem kontrollierten Fallback ausfuehren
- dynamisch installierte BIOS-ABI-Vektoren im RAM als Laufzeitsymbole kenntlich machen

Akzeptanz:

- ein synthetischer Resetpfad kann zwischen P2- und P1-ROM-Alias wechseln, ohne Funktionen doppelt zu erfinden
- ein kopierter Bootstrap-Block wird an seiner RAM-Zieladresse ausgefuehrt und behaelt nachvollziehbare ROM-Provenienz
- unsichere oder veraenderte Kopien werden nicht als statisch bewiesen markiert
- der Task ist fuer den BIOS-freien Direkteinstieg optional und wird nur fuer einen laut KR-2604 unterstuetzten LLE-Pfad zum Release-Blocker

### [x] KR-3406 - Kanonischer Block-Dispatch und Blockendklassen

Abhaengigkeiten: KR-3204, KR-3401, KR-3402

Umfang:

- die in KR-3204 definierten statischen, dynamischen, bedingten, Return- und Interrupt-Blockenden implementieren
- direkte Nachfolger und Fallthrough getrennt speichern
- virtuelle Diagnoseadresse und physische Lookup-Adresse durchgehend erhalten
- generische Relink-/Unlink-Hooks fuer spaetere Inline-Caches bereitstellen

Akzeptanz:

- jede Blockendklasse besitzt einen Ausfuehrungs- und Metadatentest
- bedingte Bloecke waehlen exakt einen von zwei dokumentierten Nachfolgern
- ein dynamischer Return verwendet PR und wird nicht als normaler Call behandelt
- das Loesen eines Zielblocks entfernt alle eingehenden Direktlinks sicher

### [x] KR-3407 - Scheduler-Safepoints und Gastzyklen

Abhaengigkeiten: KR-3101, KR-3104, KR-3204, KR-3406

Umfang:

- pro Block eine konservative, reproduzierbare Gastzyklusschaetzung erfassen
- Eventbudget an Blockgrenzen und bei langen Schleifen abbauen
- faellige Schedulerereignisse und Interrupts an expliziten Safepoints zustellen
- Jitter und Ueberziehung maschinenlesbar berichten
- Host-Wall-Clock nur fuer Presentation/Pacing, nicht fuer die logische Ereignisreihenfolge verwenden

Akzeptanz:

- identische Eingaben erzeugen dieselbe Reihenfolge von Timer-, DMA- und Interrupt-Ereignissen
- eine lange Schleife kann ein faelliges Ereignis nicht unbegrenzt verhungern lassen
- Backend und Fallback verbrauchen kompatible Gastzyklusbudgets
- Tests decken Event genau am Blockende sowie vor und nach einem Delay Slot ab

### [x] KR-3408 - MMU- und Zustandswaechter fuer Blockvarianten

Abhaengigkeiten: KR-2207, KR-2306, KR-2504, KR-3401, KR-3404

Umfang:

- Instruktions- und Datenadressuebersetzung als expliziten Runtime-Vertrag modellieren
- MMUCR, UTLB-/ITLB-Zustand und `LDTLB` mit strukturierten Uebersetzungsfehlern verbinden
- MMUCR-/TLB-Aenderungen, Seitenrechte und Adressraumgeneration als Blockwaechter erfassen
- FPSCR-relevante PR-, SZ-, FR- und Rundungsmodi fuer spezialisierte FPU-Bloecke beruecksichtigen
- Blockgrenzen bei aktiver MMU konservativ an Uebersetzungs- und Seitengrenzen schneiden
- Aenderungen der Waechter invalidieren oder redispatchen, statt veralteten Code weiterzuverwenden

Akzeptanz:

- dieselbe virtuelle Adresse kann nach TLB-Aenderung einen anderen physischen Block erreichen
- eine ungueltige Instruktionsuebersetzung erzeugt die strukturierte SH-4-Ausnahme
- FPSCR-Wechsel fuehren nicht zur Wiederverwendung einer inkompatiblen Blockvariante
- No-MMU-Fastpaths bleiben getrennt und behaupten keine volle MMU-Genauigkeit

### [ ] KR-3409 - Praezise Fallback- und Interpretergrenze

Abhaengigkeiten: KR-3204, KR-3403, KR-3407, KR-3408

Umfang:

- minimalen Interpretervertrag fuer einzelne Instruktionen oder begrenzte Bloecke definieren
- Eintritt nur an synchronisiertem Gast-PC und Austritt nur an einer dokumentierten Blockgrenze erlauben
- Delay Slots, Ausnahmen, Speicherfehler, FPU-Modi und Schedulerbudget mit generiertem Code teilen
- dynamisch erzeugten Code ueber dieselbe Invalidierungs- und Provenienzschicht fuehren

Akzeptanz:

- Differenztests liefern fuer unterstuetzte Instruktionen identischen CPU- und Speicherzustand
- eine Ausnahme im Fallback besitzt denselben Ereigniscode und Gast-PC wie generierter Code
- Fallback kann weder MMIO-Nebenwirkungen noch Watchpoints umgehen
- jeder Eintritt wird mit stabilem Grund gezaehlt und kann per Manifest verboten werden

### [ ] KR-3410 - Cache-, Store-Queue- und On-Chip-RAM-Vertrag

Abhaengigkeiten: KR-2205, KR-2605, KR-2803, KR-3103, KR-3404, KR-3408

Umfang:

- zwei getrennte 32-Byte-Store-Queues und ihre P4-Schreibfenster modellieren
- QACR0/QACR1, Queueauswahl, 32-Byte-Ausrichtung und physische Zielbildung definieren
- `PREF @Rn` zwischen normalem Prefetch und Store-Queue-Transfer unterscheiden
- `OCBI`, `OCBP`, `OCBWB`, `ICBI`, `MOVCA.L` und CCR-relevante Cacheeffekte fuer das unterstuetzte Profil explizit einstufen
- Operand-Cache-RAM-Modus und seine Beziehung zu physischem Speicher, Aliasen und Codeinvalidierung dokumentieren
- nicht modellierte Mikrocacheeffekte als Faehigkeitsgrenze ausweisen, nicht als vollstaendige Emulation behaupten

Akzeptanz:

- SQ0 und SQ1 uebertragen jeweils exakt den gewaehlten 32-Byte-Inhalt an das aus QACR und Adresse gebildete Ziel
- normales `PREF` ausserhalb des Store-Queue-Bereichs loest keinen Transfer aus
- RAM- und TA-Ziele durchlaufen ihre jeweiligen Speicher-/MMIO-Nebenwirkungen
- Cachewartung an ausfuehrbarem RAM kann keinen stale Block hinterlassen
- CCR-Operand-Cache-RAM wird entweder korrekt modelliert oder der betreffende LLE-Pfad vor Ausfuehrung sichtbar abgelehnt

### [ ] KR-3411 - v0.34 Release-Gate

Abhaengigkeiten: KR-3401 bis KR-3410

Akzeptanz:

- synthetische Tests decken alle Blockendklassen, Aliasdispatch, ROM-RAM-Handoff und Schreibinvalidierung ab
- unbekannte Ziele und Opcodes besitzen keine stillen Erfolgswege
- Scheduler-/Interruptreihenfolge ist zwischen generiertem Code und Fallback identisch
- MMU-/FPSCR-Waechter und ihre Invalidierungswege sind dokumentiert
- keine proprietaere Firmware und kein Referenzprojektcode wurde aufgenommen

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

### [ ] KR-3505 - Ausfuehrungs- und Firmwareprofil im Manifest

Abhaengigkeiten: KR-2604, KR-3403, KR-3408, KR-3501

Umfang:

- Direkteinstieg, HLE und optionales LLE als explizite Firmwarebetriebsarten beschreiben
- Aliasgruppen, kanonische physische Bereiche und beschreibbare ausfuehrbare Segmente deklarieren
- Fallbackrichtlinie, Schedulerprofil, erwartete Einstiegspunkte und dynamische BIOS-ABI-Vektoren konfigurieren
- erforderliche Backend-Faehigkeiten und erlaubte MMU-/Fastpath-Profile festhalten
- sichere Defaults fuer fehlende optionale Firmware- oder Flash-Eingaben definieren

Akzeptanz:

- ein Manifest kann keinen LLE-Pfad aktivieren, ohne alle Pflichtsegmente und Faehigkeiten zu deklarieren
- der Default bleibt BIOS-freier Homebrew-Direkteinstieg mit verbotenem stillem Fallback
- unbekannte Profile oder widerspruechliche Aliasgruppen werden vor der Analyse abgelehnt
- dynamische RAM-Vektoren werden nicht als statische ROM-Symbole serialisiert

### [ ] KR-3506 - Eingabeprovenienz und Cache-Identitaet

Abhaengigkeiten: KR-3303, KR-3305, KR-3501

Umfang:

- Groesse und kryptographischen Hash jeder externen Eingabe in einer redigierten Provenienzstruktur erfassen
- Manifest, Overrides, Tool-, IR-, Runtime- und Backend-Version in die Buildidentitaet aufnehmen
- lokale Pfade getrennt von portabler Identitaet behandeln
- veraenderte Eingaben vor Wiederverwendung gecachter Analyse- oder Codegenartefakte erkennen

Akzeptanz:

- gleiche Inhalte an unterschiedlichen lokalen Pfaden besitzen dieselbe portable Identitaet
- ein einzelnes geaendertes Eingabebyte invalidiert betroffene Caches
- Berichte enthalten ohne Opt-in weder absolute Pfade noch Firmwarestrings oder Flash-Nutzdaten
- Provenienzdaten reichen zur Reproduktion der Werkzeugkonfiguration, nicht zur Rekonstruktion geschuetzter Eingaben

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

### [ ] KR-3606 - Sichere Firmware- und Flash-Diagnostik

Abhaengigkeiten: KR-2204, KR-3503

Umfang:

- Groesse und optional deklarierte Hashes lokaler BIOS- und Flash-Eingaben pruefen
- Firmwarebereiche konservativ als Code, Daten oder unbekannt berichten, ohne das gesamte ROM linear zu dekodieren
- Flash-Header, Partitionen, logische Blockgenerationen und CRCs read-only validieren
- Serien-, Factory-, Netzwerk- und andere konsolenspezifische Felder standardmaessig redigieren
- maschinenlesbare Berichte ohne eingebettete Firmwarebytes oder extrahierte Assets erzeugen

Akzeptanz:

- die Diagnose veraendert weder Eingabedatei noch Runtime-Arbeitskopie
- Rohwerte sensibler Flash-Felder erscheinen nur nach einer ausdruecklichen lokalen Opt-in-Option
- Tests verwenden ausschliesslich kleine synthetische Abbilder
- Berichte enthalten keine BIOS-Schriften, PVR-Texturen oder andere urheberrechtlich geschuetzte Nutzdaten

### [ ] KR-3607 - Dispatch- und Fallbackdiagnostik

Abhaengigkeiten: KR-3403, KR-3406, KR-3503

Umfang:

- fuer jeden ungeloesten oder dynamisch behandelten Kontrollfluss Callsite, Ziel, PR und Blockendtyp berichten
- virtuelle Zieladresse, kanonische physische Adresse und Aliasherkunft getrennt ausgeben
- statischen Beweis, Override, Tabellenlookup, Inline-Cache und Fallback als Herkunftsklassen unterscheiden
- Fallbackgrund, ausgefuehrte Gastinstruktionen und Austrittspunkt erfassen
- wiederholte identische Ereignisse zaehlen, ohne die erste vollstaendige Diagnose zu verlieren

Akzeptanz:

- ein unbekanntes Ziel ist aus einem JSON-Bericht bis zur Quellinstruktion rueckverfolgbar
- Berichte unterscheiden unbekannten Code, ungemappten Speicher, verbotenen Firmwarepfad und ungueltige Ausrichtung
- Aliasnormalisierung verdeckt die urspruengliche virtuelle Adresse nicht
- Diagnostik veraendert Dispatchentscheidung und Gastzustand nicht

### [ ] KR-3608 - Block-, Alias- und Invalidierungsprovenienz

Abhaengigkeiten: KR-3305, KR-3404, KR-3405, KR-3503

Umfang:

- Blockherkunft aus Image-Segment, ROM-RAM-Kopie, Fallbackdecode oder Laufzeitschreibzugriff erfassen
- Aliasgruppen und kanonische physische Seiten visualisierbar ausgeben
- Seitengeneration, invalidierte Bloecke, geloeste Links und ausloesenden Schreibpfad berichten
- dynamisch installierte BIOS-ABI-Vektoren als zeitabhaengige Symbole darstellen

Akzeptanz:

- fuer jeden invalidierten Block sind Quellseite, Generation und Schreibereignis nachvollziehbar
- CPU- und DMA-Schreibzugriffe besitzen unterscheidbare Provenienz
- ROM-Quellblock und RAM-Zielblock einer Kopie bleiben miteinander verknuepft
- Berichte enthalten keine kopierten Firmwarebytes

### [ ] KR-3609 - Deterministische Systemereignis-Replays

Abhaengigkeiten: KR-3105, KR-3407, KR-3604

Umfang:

- CPU-Safepoints, MMIO, DMA, Interrupts, Timer und Schedulercallbacks in einer geordneten Ereignisspur erfassen
- logische Gastzeit getrennt von Presentation- und Hostzeit speichern
- externe Eingaben und nichtdeterministische Hostereignisse explizit injizieren
- Replay bis zu einem Gastzustands- oder Ereignishash verifizieren

Akzeptanz:

- ein Replay reproduziert Ereignisreihenfolge und finalen Gastzustand ohne Zugriff auf die Host-Wall-Clock
- fehlende, zusaetzliche oder anders sortierte Ereignisse schlagen an der ersten Abweichung fehl
- Traceformat ist versioniert und enthaelt keine Firmware- oder Flash-Rohdaten
- Aufzeichnung und Replay funktionieren ueber eine vollstaendige synthetische Frame-Sequenz

### [ ] KR-3701 - Lokale Debug-Gate-Automatisierung

Abhaengigkeiten: keine

Umfang:

- frische lokale Debug-Konfiguration und vollstaendige Regression mit einem reproduzierbaren Befehl ausfuehren
- Test-, Sanitizer-, Fuzzing-, Formatierungs- und Auditprofile portabel beschreiben
- Release-Konfiguration sowie Windows-/Linux-CI erst fuer das Alpha-Gate v0.50.0 aktivieren

Akzeptanz:

- das lokale Debug-Gate scheitert sichtbar bei Build- oder Testfehlern
- bis einschliesslich v0.44.0 wird weder ein regulaerer Release-Build noch CI als Release-Gate ausgefuehrt
- die spaetere Alpha-CI kann dieselben Testprofile ohne abweichende Semantik verwenden

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

### [ ] KR-3707 - Differenztests der Ausfuehrungswege

Abhaengigkeiten: KR-2504, KR-3204, KR-3409, KR-3410, KR-3701

Umfang:

- IR-Referenzausfuehrung, generiertes C++ und kontrollierten Interpreter-Fallback mit denselben Mikrogrammen speisen
- CPU-, Speicher-, Ausnahme-, MMIO- und Schedulerzustand an definierten Grenzen vergleichen
- Seeds und minimale Gegenbeispiele fuer Abweichungen speichern
- spezielle Korpora fuer Delay Slots, FPU-Modi, MMU-Uebersetzung, Store Queues und Busfehler fuehren

Akzeptanz:

- jede Abweichung nennt ersten Gast-PC, Zustandspfad und betroffenes Feld
- Tests sind ohne Flycast-, dcrecomp- oder BIOS-Binaerdaten reproduzierbar
- mindestens ein absichtlich fehlerhaftes Testbackend beweist, dass der Vergleich anschlaegt
- alle lokal ausgefuehrten Debug- und Sanitizerprofile verwenden dieselben semantischen Erwartungen

### [ ] KR-3708 - Mehrsegment-, Dispatch- und Invalidierungsfuzzing

Abhaengigkeiten: KR-3404, KR-3406, KR-3408, KR-3703

Umfang:

- zufaellige, aber valide Multi-Segment-Images mit Aliasgruppen und Berechtigungen erzeugen
- indirekte Ziele, TLB-/FPSCR-Waechter, ROM-RAM-Kopien und Schreibinvalidierungen kombinieren
- Callsite-Caches und generischen Dispatch gegeneinander pruefen
- Crasher mit Seed, Manifest und minimalem synthetischem Abbild reproduzierbar reduzieren

Akzeptanz:

- kein Fuzzerfall darf Hostzeiger als Gastadresse akzeptieren
- stale Bloecke werden nach Seiten- oder Zustandsaenderung nie erneut ausgefuehrt
- ungueltige Aliaszyklen und ueberlappende Provenienz werden sauber abgelehnt
- der automatisierte Kurzlauf besitzt feste Seeds; Langlaeufe koennen extern skaliert werden

### [ ] KR-3709 - Referenz- und Lizenzprovenienz

Abhaengigkeiten: KR-3701

Umfang:

- verwendete Spezifikationen und Referenzprojekte mit Zweck, Version/Commit und geprueften Bereichen dokumentieren
- unabhaengige Implementierung und Herkunft synthetischer Testvektoren nachvollziehbar machen
- Lizenzfolgen einer direkten Flycast-Subsystemeinbindung vor jeder solchen Entscheidung gesondert bewerten
- dcrecomp nur als Architekturvergleich behandeln, solange keine explizit kompatible Codefreigabe vorliegt
- automatisiert nach versehentlich aufgenommenen Referenzdateien und bekannten Firmwarepfaden suchen

Akzeptanz:

- Releasebericht trennt Spezifikation, beobachtbares Verhalten, Referenzvergleich und uebernommenen Drittcode
- eine direkte GPL-pflichtige Einbindung kann nicht ohne dokumentierte Projektlizenzentscheidung aktiviert werden
- der lokale Audit findet absichtlich platzierte verbotene Referenz- und Firmwarefixtures
- Copyright- und Lizenzhinweise aller tatsaechlichen Abhaengigkeiten sind vollstaendig

### [ ] KR-3710 - v0.37 Release-Gate

Abhaengigkeiten: KR-3701 bis KR-3709

Akzeptanz:

- Differenztests, Fuzzer, Sanitizer und reproduzierbare Builds laufen in den vorgesehenen lokalen Debug-Profilen
- Dispatch-, Fallback-, Invalidierungs- und Schedulerdiagnosen besitzen stabile JSON-Schemata
- gleiche Eingaben erzeugen bytegleiche Blockmetadaten und Release-Artefakte
- Datenschutz- und Lizenztests verwenden ausschliesslich synthetische Markerdaten

---

## v0.38.0 bis v0.40.0 - Kompatibilitaet und Leistung

### [ ] KR-3801 - Rechtlich sauberes Homebrew-Testkorpus

Abhaengigkeiten: KR-2602, KR-3709

### [ ] KR-3802 - CPU-Konformitaetsprogramm

Abhaengigkeiten: KR-1506, KR-2504, KR-3707

### [ ] KR-3803 - Eingabe-Beispiel

Abhaengigkeiten: KR-2702

### [ ] KR-3804 - 2D-Grafik-Beispiel

Abhaengigkeiten: KR-2804

### [ ] KR-3805 - Audio-Beispiel

Abhaengigkeiten: KR-2903

### [ ] KR-3806 - Zusammenhaengendes Testspiel

Abhaengigkeiten: KR-3105, KR-3411, KR-3710, KR-3803, KR-3804, KR-3805

### [ ] KR-3807 - Synthetischer Firmware-Handoff-Test

Abhaengigkeiten: KR-2605, KR-3405, KR-3411, KR-3707

Umfang:

- frei erzeugtes Reset-Mikroprogramm mit P2-Start und Wechsel auf den physisch identischen P1-Alias bauen
- normales `PREF @Rn` und einen getrennten Store-Queue-Fall testen
- einen kleinen Bootstrap von read-only ROM in ausfuehrbaren RAM kopieren und dorthin springen
- dynamische System-, Font-, Flash-, GD- und Misc-Vektoren im RAM installieren und symbolisch verfolgen
- optionalen fruehen MMIO-Zugriff mit synthetischem Registermodell einbeziehen

Akzeptanz:

- der komplette Handoff laeuft ueber Analyse, Codegen, Dispatch, Scheduler und Runtime
- ROM- und RAM-Block besitzen getrennte Adressen sowie verknuepfte Provenienz
- ein veraendertes RAM-Byte invalidiert den Zielblock vor erneuter Ausfuehrung
- Fixture, Generator und erwartete Ergebnisse sind frei verteilbar und enthalten keine BIOSbytes

### [ ] KR-3808 - Scheduler-, DMA- und Interrupt-Vertical-Slice

Abhaengigkeiten: KR-3105, KR-3407, KR-3609, KR-3806

Umfang:

- mindestens einen Frame mit CPU-Bloecken, Timer, DMA-Abschluss und Interruptzustellung ausfuehren
- MMIO-Start, geplantes Ende, Interrupt und quittierenden Gastzugriff als Ereigniskette pruefen
- dieselbe Sequenz in normaler Ausfuehrung, Trace und Replay validieren
- Gastzeit, Presentation und Hostpacing getrennt halten

Akzeptanz:

- Ereignisreihenfolge und finaler Zustand sind auf Windows und Linux deterministisch
- DMA-Schreibzugriff auf ausfuehrbaren RAM loest die definierte Invalidierung aus
- ein absichtlich verspaetetes Ereignis meldet reproduzierbaren Schedulerjitter
- der Test benoetigt keine echte Dreamcast-Disc und kein Firmwareabbild

### [?] KR-3809 - Optionales lokales Firmware-Smoke-Profil

Abhaengigkeiten: KR-2604, KR-3405, KR-3606, KR-3607, KR-3807

Umfang:

- nur bei laut KR-2604 unterstuetztem LLE-Pfad ein explizit lokales Smoke-Profil definieren
- Nutzerabbilder vor dem Lauf auf deklarierte Groesse und Hash pruefen
- Fortschritt ausschliesslich als Adressen, Zustandsmeilensteine und redigierte Diagnosen berichten
- Quell-Flash unveraendert halten und alle Schreibzugriffe in Copy-on-write umleiten
- niemals als CI-, Release- oder Downloadvoraussetzung verwenden

Akzeptanz:

- ohne Opt-in und lokale Dateien wird der Test als nicht angefordert, nicht als Fehler behandelt
- Logs enthalten keine Firmwarebytes, Strings, Assets, Serien-, Factory- oder Netzwerkfelder
- ein Hashkonflikt verhindert die Ausfuehrung vor dem ersten Gastschritt
- der Test kann vollstaendig aus dem Build- und Releasepaket ausgeschlossen werden

### [ ] KR-3901 - Benchmark-Suite

Abhaengigkeiten: KR-3806, KR-3807, KR-3808

### [ ] KR-3902 - Hot-Block-Analyse

Abhaengigkeiten: KR-3901

### [ ] KR-3903 - Dispatch- und Speicher-Fastpaths

Abhaengigkeiten: KR-3402, KR-3404, KR-3408, KR-3902

### [ ] KR-3904 - Inlining und Codegroessenstrategie

Abhaengigkeiten: KR-3301, KR-3305, KR-3901

### [ ] KR-3905 - LTO und PGO

Abhaengigkeiten: KR-3901, KR-3903, KR-3904

### [ ] KR-3906 - Block-, Edge- und Dispatch-Profiling

Abhaengigkeiten: KR-3305, KR-3406, KR-3607, KR-3901

Umfang:

- Block-, Kanten-, indirekte Callsite-, Fallback- und Invalidierungszaehler erfassen
- Gastadresse und stabile Block-ID statt Hostadresse als Profilschluessel verwenden
- Sampling- und exakten Instrumentierungsmodus anbieten
- Profile versionieren und auf passende Eingabe-/ABI-Identitaet pruefen

Akzeptanz:

- deaktiviertes Profiling hat keinen semantischen Einfluss
- ein Profil mit falscher Eingabe- oder ABI-Identitaet wird abgelehnt
- Hot-Block- und Hot-Edge-Berichte sind deterministisch sortiert
- Firmware- und Flashinhalte werden nicht in Profilen eingebettet

### [ ] KR-3907 - Fastpath- und Inline-Cache-Waechter

Abhaengigkeiten: KR-3402, KR-3404, KR-3408, KR-3903, KR-3906

Umfang:

- direkte RAM-Zugriffe nur bei bewiesener Region, Ausrichtung, Berechtigung und stabiler Adressraumgeneration zulassen
- MMU-, Alias-, Watchpoint-, MMIO- und Codeinvalidierungswaechter vor Fastmem definieren
- monomorphe indirekte Callsites mit Ziel- und Blockgeneration absichern
- bei jedem Waechterfehler in den generischen Speicher- oder Dispatchpfad wechseln

Akzeptanz:

- aktivierte und deaktivierte Fastpaths liefern in Differenztests denselben Gastzustand
- ein Watchpoint oder TLB-Wechsel wird nicht durch einen gecachten Hostzeiger umgangen
- eine Blockinvalidierung leert alle betroffenen Inline-Caches vor Wiederverwendung
- Waechtertreffer und -fehler sind im Profil getrennt sichtbar

### [ ] KR-3908 - Codegroessen-, Invalidierungs- und Schedulerbudgets

Abhaengigkeiten: KR-3301, KR-3404, KR-3407, KR-3901, KR-3906

Umfang:

- Budgets fuer generierte Quellen, Objektcode, Host-Kompilierzeit und Startzeit definieren
- Invalidierungen, Relinks, Fallbackrate und Schedulerjitter separat begrenzen
- Codegen-, Hostbuild- und Laufzeitmessungen getrennt ausgeben
- Regressionen mit stabilen Schwellen und dokumentierter Hardwareklasse bewerten

Akzeptanz:

- ein absichtlicher Budgetverstoss laesst den Performance-Gate-Test fehlschlagen
- Bericht nennt absolute Werte, Baseline und prozentuale Aenderung
- Korrektheitstests laufen unabhaengig von Performancebudgets weiter
- optionale LLE-Messungen werden nicht mit dem BIOS-freien Pflichtprofil vermischt

### [ ] KR-3909 - v0.39 Release-Gate

Abhaengigkeiten: KR-3901 bis KR-3908

Akzeptanz:

- Pflichtbenchmarks besitzen reproduzierbare Baselines und getrennte Build-/Laufzeitwerte
- Fastpaths und Inline-Caches bestehen die Differenz- und Invalidierungstests
- kein Optimierungspfad umgeht MMIO, Watchpoints, Ausnahmen oder Scheduler-Safepoints
- Codegroesse, Fallbackrate, Invalidierungen und Schedulerjitter liegen innerhalb der dokumentierten Budgets

### [ ] KR-4001 - Oeffentliche Installationsdokumentation

Abhaengigkeiten: KR-3502, KR-3706

### [ ] KR-4002 - Architektur- und Manifestreferenz

Abhaengigkeiten: KR-3411, KR-3505, KR-3506

### [ ] KR-4003 - Lizenz- und Rechtspruefung

Abhaengigkeiten: KR-3709, KR-3801

Umfang:

- Projektlizenz, Drittanbieterabhaengigkeiten und Referenzprovenienz gemeinsam pruefen
- direkte Flycast-Einbindung nur nach ausdruecklicher GPL-Kompatibilitaetsentscheidung zulassen
- dcrecomp-Code ohne nachgewiesene kompatible Freigabe von jeder Uebernahme ausschliessen
- Firmware-, Disc-, Font-, PVR- und Flashdaten aus Quellen, Tests und Paketen ausschliessen

Akzeptanz:

- Lizenzbericht nennt jede tatsaechlich gelinkte Abhaengigkeit und deren Pflichten
- Referenzvergleich und uebernommener Drittcode sind klar getrennt
- automatisierter Marker-Test erkennt verbotene Firmware- und Referenzdateien im Release-Staging

### [ ] KR-4004 - Kompatibilitaetsbericht

Abhaengigkeiten: KR-3806, KR-3807, KR-3808, KR-3909

### [ ] KR-4006 - Faehigkeits-, Firmwaremodus- und Datenaudit

Abhaengigkeiten: KR-3505, KR-3606, KR-3710, KR-3809, KR-3909, KR-4003

Umfang:

- Matrix fuer Direkteinstieg, HLE, optionales LLE, MMU, Fallback, SMC und Schedulerpraezision veroeffentlichen
- Pflicht-, optionale, experimentelle und nicht unterstuetzte Profile unterscheiden
- Release-Staging auf Firmwarebytes, extrahierte Assets, sensible Flashdaten, lokale Pfade und unredigierte Traces pruefen
- bekannte semantische und zeitliche Abweichungen pro Profil dokumentieren

Akzeptanz:

- jede oeffentliche Faehigkeitsbehauptung verweist auf einen automatisierten oder klar benannten lokalen Test
- optionales LLE wird nicht als Voraussetzung fuer Homebrew- oder Release-Tests dargestellt
- der Datenaudit laeuft vor Paket-Hash und Signierung
- ein absichtlich eingebrachtes synthetisches Geheimnis und ein Firmware-Marker werden erkannt

### [ ] KR-4005 - v0.40.0 Pre-Alpha-Release

Abhaengigkeiten: KR-4001 bis KR-4004, KR-4006

---

## v0.41.0 bis v0.44.0 - Desktop-GUI und Quellworkflow

### [ ] KR-4101 - GUI-Technologie und Architektur festlegen

Abhaengigkeiten: KR-3502, KR-3505, KR-3706

Umfang:

- Desktop-Toolkit, Packaging-Strategie und unterstuetzte Alpha-Plattformen festlegen
- GUI strikt ueber denselben Manifest-, Job- und Diagnosedienst wie CLI und Automatisierung aufbauen
- Verantwortlichkeiten zwischen Shell, Anwendungsdienst und Kernbibliotheken trennen
- Architektur gegen doppelte Semantik fuer Firmwareprofile, `.gdi`-Quellen und Buildjobs absichern

Akzeptanz:

- Architekturentscheidungen nennen Begruendung, Risiken und Migrationspfad
- kein GUI-Pfad erzeugt eigene Sonderlogik fuer Analyse-, Build- oder Firmwaremodi
- ein Minimalstart auf den Alpha-Zielplattformen oeffnet die App mit zentraler Konfiguration
- Packaging- und Runtime-Annahmen sind in CI oder dokumentierten lokalen Checks nachvollziehbar

### [ ] KR-4102 - Gemeinsamer GUI-Anwendungsdienst

Abhaengigkeiten: KR-4101

Umfang:

- GUI-faeigen Aufrufpfad fuer Analyse, Codegen, Build und Run ueber bestehende Kernkomponenten bereitstellen
- Jobzustand, Fortschritt, Fehler und Artefakte in ein stabiles Ereignismodell ueberfuehren
- Abbruch, Wiederholung und parallele Nicht-Konflikt-Jobs kontrolliert machen
- Konfiguration, zuletzt verwendete Projekte und Werkzeugeinstellungen persistieren

Akzeptanz:

- dieselben Jobs lassen sich aus GUI und CLI mit identischen Eingaben reproduzieren
- ein abgebrochener Job hinterlaesst keinen inkonsistenten Manifest- oder Cachezustand
- GUI-spezifische Adapter enthalten keine eigene Analyse- oder Quellsemantik
- Ereignisse koennen automatisiert fuer spaetere GUI-Tests beobachtet werden

### [ ] KR-4103 - GUI-Shell, Navigation und Einstellungen

Abhaengigkeiten: KR-4101, KR-4102

Umfang:

- Hauptfenster, Projektstart, Navigation und globale Einstellungen implementieren
- klare Statusflaechen fuer aktive Jobs, letzte Ergebnisse und Fehler schaffen
- Tastaturbedienung, Fokusfuehrung und persistente Layoutgrundlagen anlegen
- Logging und unerwartete Fehler in eine nutzerlesbare Recovery-Oberflaeche leiten

Akzeptanz:

- Projektwechsel, Neustart und Wiederherstellung fuehren nicht zu verlorenen Einstellungen oder haengenden Jobs
- Tastatur- und Mausbedienung decken die Kernnavigation gleichwertig ab
- unerwartete Fehler fuehren zu einer lesbaren Meldung statt zu stiller GUI-Beendigung
- Shell und Navigation bleiben bei langen Jobs responsiv

### [ ] KR-4201 - Projektworkflow fuer Anlegen, Oeffnen und Speichern

Abhaengigkeiten: KR-4102, KR-4103

Umfang:

- neue Projekte anlegen, bestehende oeffnen, speichern und kuerzlich verwendete Projekte wiederfinden
- Manifestdateien, Ausgabeordner und Werkzeugpfade ueber gefuehrte GUI-Dialoge verwalten
- ungespeicherte Aenderungen, Konflikte und Migrationsfaelle sichtbar behandeln
- Projektdatei als alleinige Quelle fuer den reproduzierbaren GUI-Workflow verwenden

Akzeptanz:

- ein Projekt kann ohne manuelle Dateiedits erstellt und spaeter identisch wieder geoeffnet werden
- Konfigurationsmigrationen sind versioniert und ruinierten keine bestehenden Projekte
- fehlende oder verschobene Projektressourcen erzeugen gezielte Recovery-Hinweise
- GUI und CLI lesen dasselbe gespeicherte Projektmodell

### [ ] KR-4202 - Quellenwahl fuer Raw, ELF und GDI

Abhaengigkeiten: KR-3006, KR-4102, KR-4201

Umfang:

- Dateiauswahl fuer Raw-, ELF- und `.gdi`-Quellen bereitstellen
- Quelltyp, Hash, Groesse, relativen Ursprung und bekannte Einschraenkungen sichtbar machen
- Mehrdatei-Quellen auf Projektverschiebungen und relative Pfadauflosung robust behandeln
- keine Dateiinhalte duplizieren oder in Projektdateien einbetten

Akzeptanz:

- `.gdi` und ihre Tracks bleiben nach Speichern, Verschieben und erneutem Oeffnen korrekt referenziert
- die GUI zeigt dieselben Validierungsfehler wie CLI und Diagnoseberichte
- ein ungueltiger Quellpfad blockiert den Jobstart mit nachvollziehbarer Meldung
- keine Quellbytes oder lokalen Geheimnisse landen in Manifest oder Logs

### [ ] KR-4203 - GDI-Inspektor und Quellvalidierung

Abhaengigkeiten: KR-3005, KR-3006, KR-4202

Umfang:

- Trackliste, Formate, Offsets, Dateigroessen und Daten-/Audiotrackrollen visualisieren
- Descriptor- und Trackfehler an konkreten Eintraegen markieren
- Read-only-Zusicherungen, relative Pfade und Quellprovenienz direkt in der GUI erklaeren
- Positiv- und Negativbeispiele fuer Support und Dokumentation exportierbar machen

Akzeptanz:

- jeder `.gdi`-Validierungsfehler ist in der GUI ohne CLI reproduzierbar sichtbar
- erfolgreiche Quellen zeigen alle Tracks in stabiler Reihenfolge mit nachvollziehbarer Provenienz
- keine Bearbeitungsfunktion schreibt in `.gdi` oder Trackdateien zurueck
- Ansichten lassen sich in automatisierten Tests verifizieren

### [ ] KR-4204 - Manifest-, Firmwareprofil- und Override-Editor

Abhaengigkeiten: KR-3505, KR-3506, KR-4201, KR-4202

Umfang:

- Einstiegspunkte, Segmente, Overrides, Schedulerprofil und Firmwaremodus ueber GUI editieren
- Pflicht-, optionale und lokale Firmwarepfade klar trennen
- datensparsame Hinweise fuer BIOS-, Flash- und Alias-bezogene Diagnosen bereitstellen
- ungueltige Kombinationen vor Jobstart validieren

Akzeptanz:

- alle GUI-Einstellungen serialisieren in dasselbe versionierte Manifestschema wie die CLI
- lokale Firmwareoptionen bleiben klar als optional und nicht releasepflichtig markiert
- ungueltige Segment-, Override- oder Profilkombinationen werden vor dem Start abgelehnt
- redigierte Diagnosen enthalten keine Firmwarebytes oder sensiblen Flashfelder

### [ ] KR-4301 - GUI-Orchestrierung fuer Analyse, Codegen, Build und Run

Abhaengigkeiten: KR-4102, KR-4204

Umfang:

- gefuehrte Startpfade fuer Analyse, Codegen, Build und optionalen Lauf bereitstellen
- Artefakte, Zielordner und Folgeaktionen nach Jobende sichtbar machen
- Jobabhaengigkeiten und notwendige Vorstufen automatisch aufloesen
- wiederholbare Startprofile fuer Alpha-relevante Workflows speichern

Akzeptanz:

- ein Nutzer kann den vollstaendigen Alpha-Hauptworkflow ohne CLI durchlaufen
- Folgejobs verwenden die erwarteten Artefakte und veraltete Ergebnisse werden erkannt
- ein fehlgeschlagener Zwischenschritt blockiert unsichere Folgeaktionen
- GUI und CLI erzeugen fuer identische Eingaben dieselben Kernartefakte

### [ ] KR-4302 - Fortschritt, Logs und Diagnostikansichten

Abhaengigkeiten: KR-3606, KR-3607, KR-3608, KR-4102, KR-4301

Umfang:

- Fortschritt, Warnungen, Fehler und strukturierte Diagnosen in der GUI anzeigen
- Dispatch-, Fallback-, Invalidierungs-, Firmware- und Schedulerereignisse filterbar machen
- Logs nach Relevanz gruppieren und exportieren, ohne sensible Quelldaten offenzulegen
- Fehlerpfade mit konkreten naechsten Schritten fuer Nutzer versehen

Akzeptanz:

- Diagnosen aus denselben JSON-Schemata wie CLI und Tests darstellen
- ein Fehler in `.gdi`, Firmwareprofil oder Buildschritt ist ohne Rohlogsuche auffindbar
- Export und Kopieren redigieren Hostpfade, Firmwarebytes und sensible Flashfelder
- grosse Logs lassen die GUI nicht einfrieren

### [ ] KR-4303 - Ergebnisansichten fuer Funktionen, Quellen und Provenienz

Abhaengigkeiten: KR-3305, KR-3602, KR-3608, KR-4202, KR-4301

Umfang:

- Funktionen, Segmente, Quellenzuordnung und generierte Artefakte durchsuchen und filtern
- Provenienz fuer Aliasnormalisierung, ROM-RAM-Kopien und Invalidierungen sichtbar machen
- Unterschiede zwischen direktem Einstieg, Homebrew-Pfad und optionalem Firmwarepfad erklaeren
- Ergebnisansichten fuer Supportfaelle reproduzierbar exportieren

Akzeptanz:

- zentrale Alpha-Fragen zu Quelle, Segment, Block und Provenienz lassen sich ohne CLI beantworten
- GUI-Ansichten referenzieren stabile Gastadressen und IDs statt fluechtiger Hostartefakte
- Exportformate bleiben reproduzierbar sortiert und datensparsam
- Ergebnisansichten stimmen mit den zugrunde liegenden Analyseberichten ueberein

### [ ] KR-4401 - GUI-End-to-End-Automatisierung

Abhaengigkeiten: KR-4203, KR-4204, KR-4301, KR-4302, KR-4303

Umfang:

- automatisierte GUI-Hauptpfade fuer neues Projekt, `.gdi` laden, Analyse und Build erstellen
- synthetische Positiv-, Negativ- und Recovery-Szenarien fuer `.gdi` und Firmwareoptionen integrieren
- Windows- und Linux-Laeufe fuer den Alpha-Scope absichern
- Screenshots, Logs und Artefaktpruefungen fuer fehlschlagende UI-Tests sammeln

Akzeptanz:

- Alpha-relevante GUI-Pfade laufen automatisiert auf allen Zielplattformen
- mindestens ein gezielter `.gdi`-Fehlerfall und ein Recovery-Fall sind im UI-Testkorpus vorhanden
- flackernde oder timingabhaengige UI-Tests werden vor Gate-Freigabe stabilisiert oder entfernt
- Testartefakte enthalten keine proprietaeren Quellbytes

### [ ] KR-4402 - GUI-Haertung, Accessibility und Packaging

Abhaengigkeiten: KR-4103, KR-4302, KR-4401

Umfang:

- DPI-Skalierung, Tastaturnavigation, Fokusindikatoren und lesbare Fehlerdarstellung nachziehen
- Crash-Recovery, Settings-Migration und sichere Standardpfade absichern
- verteilbare GUI-Pakete fuer den Alpha-Scope bauen und pruefen
- Installations- und Updatepfade fuer GUI und CLI gemeinsam dokumentieren

Akzeptanz:

- die GUI bleibt unter typischen Alpha-Fehlerfaellen bedienbar und stellt den Zustand wieder her
- Packaging und Start funktionieren auf den Zielplattformen mit dokumentierten Voraussetzungen
- Accessibility-Basis fuer Kernpfade ist vorhanden und getestet
- GUI-Dokumentation deckt Installation, Projektstart und `.gdi`-Workflow ab

### [ ] KR-4403 - v0.44.0 GUI-und-GDI-Release-Gate

Abhaengigkeiten: KR-4401, KR-4402

Akzeptanz:

- der Alpha-Hauptworkflow ist in der GUI ohne CLI-Zwang vollstaendig nutzbar
- `.gdi` ist als offizielle Quelle in GUI, CLI und Tests identisch validiert
- identische Projekte erzeugen ueber GUI und CLI dieselben Manifeste, Jobs und Ergebnisartefakte
- dokumentierte Blocker fuer GUI oder `.gdi` verhindern den Uebergang ins Alpha-Gate

---

## Spaetere Release-Gates

### [ ] KR-5000 - v0.50.0 Alpha-Gate

Abhaengigkeiten: KR-4005, KR-4403

Akzeptanz:

- der verteilbare Homebrew-Vertical-Slice laeuft ohne proprietaere Eingaben reproduzierbar
- alle Blockendtypen, Backend-/Fallback-Grenzen und Scheduler-Safepoints besitzen integrierte Tests
- unbekannte Opcodes, Ziele, BIOS-Aufrufe und MMIO-Zugriffe koennen im unterstuetzten Profil nicht still erfolgreich sein
- ausfuehrbarer RAM, Aliasdispatch und ROM-RAM-Handoff bestehen Invalidierungs- und Provenienztests
- die Desktop-GUI deckt Projektanlage, Quellenwahl, Analyse, Build und Diagnostik fuer Alpha vollstaendig ab
- `.gdi`-Dateien koennen als offizielle Quelle geladen, validiert und reproduzierbar verarbeitet werden
- Manifest, Runtime-ABI, Diagnoseschemata und Buildartefakte sind versioniert und reproduzierbar

### [ ] KR-7500 - v0.75.0 Beta-Gate

Abhaengigkeiten: KR-5000

Akzeptanz:

- ausgewaehlte, rechtmaessig lokal bereitgestellte Programme erreichen reproduzierbar interaktive Szenen
- Grafik, Audio, Eingabe, DMA, Timer und Interrupts laufen in einer gemeinsamen deterministischen Ereignisfolge
- Fallbackrate, Invalidierungen, Schedulerjitter und Performancebudgets werden pro Testprofil berichtet
- MMU-, FPSCR-, Store-Queue- und selbstmodifizierende Pfade besitzen titelunabhaengige Regressionen
- optionale lokale Firmwaretests bleiben von CI, Release und verteilbarem Pflichtkorpus getrennt

### [ ] KR-10000 - v1.0.0 Release-Gate

Abhaengigkeiten: KR-7500

Akzeptanz:

- CLI, Manifest, Runtime-ABI, Blockmetadaten und Replayformat besitzen dokumentierte Stabilitaetsvertraege
- Dispatch, Fallback, Scheduler, MMU-/Fastpath-Waechter und Codeinvalidierung sind als oeffentliche Architektur dokumentiert
- keine bekannte stille Fehlkompilierung besteht im als unterstuetzt ausgewiesenen Bereich
- jede Faehigkeitsbehauptung ist durch automatisierte oder ausdruecklich lokale, redigierte Tests gedeckt
- Releasepaket, Quellarchiv und Berichte enthalten keine BIOS-, Disc-, Spiel-, Asset- oder sensiblen Flashdaten
- Lizenz-, Referenzprovenienz-, Datenschutz- und Reproduzierbarkeitsaudits sind bestanden
