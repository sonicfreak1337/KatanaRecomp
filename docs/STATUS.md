# Projektstatus

Version: 0.21.0
Phase: Pre-Alpha
Aktiver Meilenstein: v0.22.0 - Dreamcast-Speicherbus

## Zusammenfassung

KatanaRecomp besitzt einen durchgaengigen Prototyp-Pfad von Raw- und ELF32-SH-Eingaben ueber ein segmentiertes Executable Image bis zu generiertem, gegen die zentrale Runtime-Bibliothek gelinktem und ausgefuehrtem C++.

## Teststatus

```text
100% tests passed out of 87
```

## Fertiggestellte Roadmap-Tasks

- [x] KR-1101 - SUB, NEG und NOT
- [x] KR-1102 - AND, OR und XOR
- [x] KR-1103 - CMP-Varianten
- [x] KR-1104 - Carry und Overflow
- [x] KR-1105 - Extend, Swap und XTRCT
- [x] KR-1106 - DT und MOVT
- [x] KR-1107 - v0.11.0 Release-Gate
- [x] KR-1201 - Ein-Bit-Shifts
- [x] KR-1202 - Feste Mehrfach-Shifts
- [x] KR-1203 - Rotationen
- [x] KR-1204 - Dynamische Shifts
- [x] KR-1205 - v0.12.0 Release-Gate
- [x] KR-1301 - Einfache Multiplikation
- [x] KR-1302 - Doppelte Multiplikation
- [x] KR-1303 - MAC-Instruktionen
- [x] KR-1304 - Division
- [x] KR-1305 - v0.13.0 Release-Gate
- [x] KR-1401 - Pre-Decrement und Post-Increment
- [x] KR-1402 - Register-Displacements
- [x] KR-1403 - R0-indexierte Adressierung
- [x] KR-1404 - GBR-relative Adressierung
- [x] KR-1405 - PC-relative Loads und MOVA
- [x] KR-1406 - Systemregistertransfers
- [x] KR-1407 - Privilegierte Kontrollinstruktionen
- [x] KR-1408 - v0.14.0 Release-Gate
- [x] KR-1501 - Zentrale Instruktionsmetadaten
- [x] KR-1502 - Decoder-Kollisionspruefung
- [x] KR-1503 - ISA-Abdeckungsbericht
- [x] KR-1504 - Spezifikations-Testvektoren
- [x] KR-1505 - Decoder-Fuzzer
- [x] KR-1506 - v0.15.0 Release-Gate
- [x] KR-1601 - Image- und Segmentmodell
- [x] KR-1602 - Raw-Binary-Loader
- [x] KR-1603 - ELF32-SH-Loader
- [x] KR-1604 - Symbole und Map-Dateien
- [x] KR-1605 - Relocations
- [x] KR-1606 - Projektmanifest Version 1
- [x] KR-1607 - v0.16.0 Release-Gate
- [x] KR-1701 - Worklist ab Einstiegspunkten
- [x] KR-1702 - Code-Daten-Klassifikation
- [x] KR-1703 - Herkunft und Konfidenz von Funktionen
- [x] KR-1704 - Nicht erreichbare Bereiche
- [x] KR-1705 - Ueberlappende Bereiche
- [x] KR-1706 - Analysebericht
- [x] KR-1707 - v0.17.0 Release-Gate
- [x] KR-1801 - Lokale Konstantenpropagation
- [x] KR-1802 - Registerwertanalyse
- [x] KR-1803 - Einfache indirekte Calls und Jumps
- [x] KR-1804 - Jump Tables
- [x] KR-1805 - Override-Datei
- [x] KR-1806 - Bericht ungeloester Kontrollflussstellen
- [x] KR-1807 - v0.18.0 Release-Gate
- [x] KR-1901 - Explizite Operandbreiten
- [x] KR-1902 - Explizite Statusregistereffekte
- [x] KR-1903 - Speicher-Seiteneffekte
- [x] KR-1904 - Delay-Slot-Normalisierung
- [x] KR-1905 - IR-Verifier
- [x] KR-1906 - Deterministische Text- und JSON-Ausgabe
- [x] KR-1907 - v0.19.0 Release-Gate
- [x] KR-2001 - Constant Folding
- [x] KR-2002 - Copy Propagation
- [x] KR-2003 - Dead-Code-Elimination
- [x] KR-2004 - CFG-Simplifizierung
- [x] KR-2005 - Load-Store-Vereinfachung
- [x] KR-2006 - Pass-Pipeline und Debug-Schalter
- [x] KR-2007 - v0.20.0 Release-Gate
- [x] KR-2101 - Runtime aus generiertem Code auslagern
- [x] KR-2102 - Vollstaendigen CPU-Zustand zentralisieren
- [x] KR-2103 - Deterministischen Reset-Zustand definieren
- [x] KR-2104 - v0.21.0 Release-Gate
- [x] KR-2201 - Regionbasierter Bus
- [x] KR-2202 - RAM und Spiegelungen
- [x] KR-2203 - VRAM und AICA-RAM-Abstraktionen
- [x] KR-2204 - BIOS- und Flash-Abstraktionen

## Naechster Roadmap-Task

- [ ] KR-2205 - MMIO-Handler

## Aktuelle Einschraenkungen

- unvollstaendiger SH-4-Befehlssatz
- keine FPU
- Dreamcast-Haupt-RAM, VRAM, AICA-RAM, BIOS und Flash samt direkten Spiegelungen sind vorhanden; MMIO fehlt noch
- keine Plattformhardware
- nur einfache konstante indirekte Ziele und bekannte begrenzte Jump Tables werden aufgeloest
