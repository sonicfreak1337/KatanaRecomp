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

### [ ] KR-1106 - DT und MOVT

Abhaengigkeiten: KR-1103

Umfang:

- `DT Rn`
- `MOVT Rn`
- Schleifentest mit `BF`

Akzeptanz:

- `DT` setzt T nur bei Ergebnis null
- End-to-End-Schleife terminiert mit erwartetem Registerzustand

### [ ] KR-1107 - v0.11 Release-Gate

Abhaengigkeiten: KR-1101 bis KR-1106

Umfang:

- Abdeckungsdokument aktualisieren
- CHANGELOG aktualisieren
- Version auf 0.11.0 setzen
- kompletter Regressionstest
- Git-Tag vorbereiten

---

## v0.12.0 - Shifts und Rotationen

### [ ] KR-1201 - Ein-Bit-Shifts

Abhaengigkeiten: KR-1107

Umfang:

- `SHLL`, `SHLR`
- `SHAL`, `SHAR`

Akzeptanz:

- herausgeschobenes Bit landet korrekt in T
- arithmetischer und logischer Shift sind getrennt getestet

### [ ] KR-1202 - Feste Mehrfach-Shifts

Abhaengigkeiten: KR-1201

Umfang:

- Shift um 2, 8 und 16 Bit
- links und rechts

### [ ] KR-1203 - Rotationen

Abhaengigkeiten: KR-1201

Umfang:

- `ROTL`, `ROTR`
- `ROTCL`, `ROTCR`

Akzeptanz:

- Rotationen mit und ohne T-Bit
- bitgenaue Testvektoren

### [ ] KR-1204 - Dynamische Shifts

Abhaengigkeiten: KR-1201

Umfang:

- `SHAD`
- `SHLD`

Akzeptanz:

- positive, negative und Grenz-Shiftwerte
- Verhalten bei grossen Shiftzaehlern dokumentiert

### [ ] KR-1205 - v0.12 Release-Gate

Abhaengigkeiten: KR-1201 bis KR-1204

---

## v0.13.0 - Multiplikation, Division und MAC

### [ ] KR-1301 - Einfache Multiplikation

Abhaengigkeiten: KR-1205

Umfang:

- `MUL.L`
- `MULS.W`
- `MULU.W`

### [ ] KR-1302 - Doppelte Multiplikation

Abhaengigkeiten: KR-1301

Umfang:

- `DMULS.L`
- `DMULU.L`
- `MACH`, `MACL`

### [ ] KR-1303 - MAC-Instruktionen

Abhaengigkeiten: KR-1302

Umfang:

- `MAC.W`
- `MAC.L`
- Speicheradressierung und Registerfortschaltung

### [ ] KR-1304 - Division

Abhaengigkeiten: KR-1104

Umfang:

- `DIV0U`
- `DIV0S`
- `DIV1`
- Q-, M- und T-Bit

### [ ] KR-1305 - v0.13 Release-Gate

Abhaengigkeiten: KR-1301 bis KR-1304

---

## v0.14.0 - Adressierung und Systemregister

### [ ] KR-1401 - Pre-Decrement und Post-Increment

Abhaengigkeiten: KR-1305

Umfang:

- Byte-, Word- und Long-Formen
- Sonderfall identisches Quell- und Zielregister

### [ ] KR-1402 - Register-Displacements

Abhaengigkeiten: KR-1401

### [ ] KR-1403 - R0-indexierte Adressierung

Abhaengigkeiten: KR-1402

### [ ] KR-1404 - GBR-relative Adressierung

Abhaengigkeiten: KR-1402

### [ ] KR-1405 - PC-relative Loads und MOVA

Abhaengigkeiten: KR-1402

Akzeptanz:

- PC-Ausrichtung und Displacement-Skalierung sind getestet

### [ ] KR-1406 - Systemregistertransfers

Abhaengigkeiten: KR-1401

Umfang:

- `STS`, `LDS`
- `STC`, `LDC`
- direkte und Speicherformen

### [ ] KR-1407 - Privilegierte Kontrollinstruktionen

Abhaengigkeiten: KR-1406

Umfang:

- `TRAPA`
- `RTE`
- `SLEEP`
- vorerst klarer Runtime-Vertrag, auch wenn Plattformlogik spaeter folgt

### [ ] KR-1408 - v0.14 Release-Gate

Abhaengigkeiten: KR-1401 bis KR-1407

---

## v0.15.0 - Decoder-Haertung

### [ ] KR-1501 - Zentrale Instruktionsmetadaten

Abhaengigkeiten: KR-1408

### [ ] KR-1502 - Decoder-Kollisionspruefung

Abhaengigkeiten: KR-1501

### [ ] KR-1503 - ISA-Abdeckungsbericht

Abhaengigkeiten: KR-1501

### [ ] KR-1504 - Spezifikations-Testvektoren

Abhaengigkeiten: KR-1501

### [ ] KR-1505 - Decoder-Fuzzer

Abhaengigkeiten: KR-1502

### [ ] KR-1506 - v0.15 Release-Gate

Abhaengigkeiten: KR-1501 bis KR-1505

---

## v0.16.0 - Executable Images und Loader

### [ ] KR-1601 - Image- und Segmentmodell

Abhaengigkeiten: KR-1506

### [ ] KR-1602 - Raw-Binary-Loader

Abhaengigkeiten: KR-1601

### [ ] KR-1603 - ELF32-SH-Loader

Abhaengigkeiten: KR-1601

### [ ] KR-1604 - Symbole und Map-Dateien

Abhaengigkeiten: KR-1603

### [ ] KR-1605 - Relocations

Abhaengigkeiten: KR-1603

### [ ] KR-1606 - Projektmanifest Version 1

Abhaengigkeiten: KR-1602, KR-1603

### [ ] KR-1607 - v0.16 Release-Gate

Abhaengigkeiten: KR-1601 bis KR-1606

---

## v0.17.0 - Rekursive Analyse

### [ ] KR-1701 - Worklist ab Einstiegspunkten

Abhaengigkeiten: KR-1607

### [ ] KR-1702 - Code-Daten-Klassifikation

Abhaengigkeiten: KR-1701

### [ ] KR-1703 - Herkunft und Konfidenz von Funktionen

Abhaengigkeiten: KR-1701

### [ ] KR-1704 - Nicht erreichbare Bereiche

Abhaengigkeiten: KR-1702

### [ ] KR-1705 - Ueberlappende Bereiche

Abhaengigkeiten: KR-1702

### [ ] KR-1706 - Analysebericht

Abhaengigkeiten: KR-1701 bis KR-1705

### [ ] KR-1707 - v0.17 Release-Gate

Abhaengigkeiten: KR-1701 bis KR-1706

---

## v0.18.0 - Indirekter Kontrollfluss

### [ ] KR-1801 - Lokale Konstantenpropagation

Abhaengigkeiten: KR-1707

### [ ] KR-1802 - Registerwertanalyse

Abhaengigkeiten: KR-1801

### [ ] KR-1803 - Einfache indirekte Calls und Jumps

Abhaengigkeiten: KR-1802

### [ ] KR-1804 - Jump Tables

Abhaengigkeiten: KR-1802

### [ ] KR-1805 - Override-Datei

Abhaengigkeiten: KR-1606

### [ ] KR-1806 - Bericht ungeloester Kontrollflussstellen

Abhaengigkeiten: KR-1803, KR-1804

### [ ] KR-1807 - v0.18 Release-Gate

Abhaengigkeiten: KR-1801 bis KR-1806

---

## v0.19.0 - IR Version 2

### [ ] KR-1901 - Explizite Operandbreiten

Abhaengigkeiten: KR-1807

### [ ] KR-1902 - Explizite Statusregistereffekte

Abhaengigkeiten: KR-1901

### [ ] KR-1903 - Speicher-Seiteneffekte

Abhaengigkeiten: KR-1901

### [ ] KR-1904 - Delay-Slot-Normalisierung

Abhaengigkeiten: KR-1901

### [ ] KR-1905 - IR-Verifier

Abhaengigkeiten: KR-1901 bis KR-1904

### [ ] KR-1906 - Deterministische Text- und JSON-Ausgabe

Abhaengigkeiten: KR-1905

### [ ] KR-1907 - v0.19 Release-Gate

Abhaengigkeiten: KR-1901 bis KR-1906

---

## v0.20.0 - IR-Optimierungen

### [ ] KR-2001 - Constant Folding

Abhaengigkeiten: KR-1907

### [ ] KR-2002 - Copy Propagation

Abhaengigkeiten: KR-2001

### [ ] KR-2003 - Dead-Code-Elimination

Abhaengigkeiten: KR-2002

### [ ] KR-2004 - CFG-Simplifizierung

Abhaengigkeiten: KR-2003

### [ ] KR-2005 - Load-Store-Vereinfachung

Abhaengigkeiten: KR-1903, KR-2001

### [ ] KR-2006 - Pass-Pipeline und Debug-Schalter

Abhaengigkeiten: KR-2001 bis KR-2005

### [ ] KR-2007 - v0.20 Release-Gate

Abhaengigkeiten: KR-2001 bis KR-2006

---

## v0.21.0 - Runtime und CPU-Zustand

### [ ] KR-2101 - Runtime aus generiertem Code auslagern

Abhaengigkeiten: KR-2007

### [ ] KR-2102 - Vollstaendiger Integer-CPU-Zustand

Abhaengigkeiten: KR-2101

### [ ] KR-2103 - Banked Register

Abhaengigkeiten: KR-2102

### [ ] KR-2104 - FPU-Zustand vorbereiten

Abhaengigkeiten: KR-2102

### [ ] KR-2105 - Versionierte Runtime-ABI

Abhaengigkeiten: KR-2101 bis KR-2104

### [ ] KR-2106 - Deterministischer Reset

Abhaengigkeiten: KR-2102

### [ ] KR-2107 - v0.21 Release-Gate

Abhaengigkeiten: KR-2101 bis KR-2106

---

## v0.22.0 - Speicherbus

### [ ] KR-2201 - Regionbasierter Bus

Abhaengigkeiten: KR-2107

### [ ] KR-2202 - RAM und Spiegelungen

Abhaengigkeiten: KR-2201

### [ ] KR-2203 - VRAM und AICA-RAM-Abstraktionen

Abhaengigkeiten: KR-2201

### [ ] KR-2204 - BIOS- und Flash-Abstraktionen

Abhaengigkeiten: KR-2201

### [ ] KR-2205 - MMIO-Handler

Abhaengigkeiten: KR-2201

### [ ] KR-2206 - Ausrichtung, Fehler und Watchpoints

Abhaengigkeiten: KR-2201

### [ ] KR-2207 - v0.22 Release-Gate

Abhaengigkeiten: KR-2201 bis KR-2206

---

## v0.23.0 - Ausnahmen und Interrupts

### [ ] KR-2301 - SR-Felder und Interruptmasken

Abhaengigkeiten: KR-2107

### [ ] KR-2302 - Exception-Eintritt

Abhaengigkeiten: KR-2301, KR-2207

### [ ] KR-2303 - Interrupt-Controller

Abhaengigkeiten: KR-2301

### [ ] KR-2304 - TRAPA und RTE

Abhaengigkeiten: KR-2302

### [ ] KR-2305 - Delay-Slot-Ausnahmen

Abhaengigkeiten: KR-2302

### [ ] KR-2306 - v0.23 Release-Gate

Abhaengigkeiten: KR-2301 bis KR-2305

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

Abhaengigkeiten: KR-2107

### [ ] KR-3202 - C++-Backend migrieren

Abhaengigkeiten: KR-3201

### [ ] KR-3203 - ABI-Faehigkeitspruefung

Abhaengigkeiten: KR-3201, KR-2105

### [ ] KR-3301 - Translation-Unit-Partitionierung

Abhaengigkeiten: KR-3202

### [ ] KR-3302 - Deterministische Dateinamen

Abhaengigkeiten: KR-3301

### [ ] KR-3303 - Inkrementeller Codegen-Cache

Abhaengigkeiten: KR-3302

### [ ] KR-3304 - Parallele Ausgabe und Buildintegration

Abhaengigkeiten: KR-3303

### [ ] KR-3401 - Laufzeit-Funktionstabelle

Abhaengigkeiten: KR-1807, KR-2105

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

Abhaengigkeiten: KR-2105

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