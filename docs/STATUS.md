# Projektstatus

Version: 0.27.0
Phase: Pre-Alpha
Naechster Meilenstein: v0.28.0 - PVR Minimum Viable Video

## Zusammenfassung

KatanaRecomp besitzt einen durchgaengigen Prototyp-Pfad von Raw- und ELF32-SH-Eingaben ueber ein segmentiertes Executable Image bis zu generiertem, gegen die zentrale Runtime-Bibliothek gelinktem und ausgefuehrtem C++.

## Fortschrittseinordnung

### Gesamtprojekt

- [x] Kernunterbau abgeschlossen: Phasen 1 bis 5 sind vollstaendig umgesetzt
- [~] Gesamtfortschritt nach gepflegten Roadmap-Tasks: 106 von 209 Tasks abgeschlossen = 50.7%
- [~] Fortschritt auf dem Weg von Dreamcast-Plattform bis Alpha: 12 von 112 Tasks abgeschlossen = 10.7%
- [ ] Alpha-Gate erreicht: nein

### Weg zum ersten echten Dreamcast-Test

Definition fuer diesen Status: ein BIOS-freier, frei verteilbarer Homebrew-Vertical-Slice gemaess Phase-6-Release-Gate, der Bild zeigt, Eingabe annimmt und Audio erzeugt.

- [x] Boot- und Homebrew-Einstieg vorhanden: v0.26.0 abgeschlossen
- [~] Dreamcast-Plattformphase insgesamt: 12 von 29 Tasks abgeschlossen = 41.4%
- [ ] Eingabeweg fertig: Maple und Controller (`KR-2701`, `KR-2702`) fehlen
- [ ] Bildpfad fertig: PVR/Framebuffer/Render-Backend (`KR-2801` bis `KR-2804`) fehlen
- [ ] Audiopfad fertig: AICA/Mixer/Host-Audio (`KR-2901` bis `KR-2903`) fehlen
- [ ] Takt- und Ereignispfad fertig: Scheduler, Timer, DMA und Interruptintegration (`KR-3101` bis `KR-3105`) fehlen
- [ ] Optional fuer Disc-Quellen, aber nicht noetig fuer den ersten BIOS-freien Vertical Slice: `.gdi`-Pfad (`KR-3001` bis `KR-3006`) fehlt

Praktische Einordnung:

- [~] Von einem echten Dreamcast-Smoke-Test sind wir funktional noch vier Plattform-Meilensteine entfernt: `v0.27.0` bis `v0.31.0`
- [~] Der CPU-, IR-, Runtime- und FPU-Unterbau steht; der Engpass ist jetzt fast vollstaendig Plattformintegration
- [ ] Von Alpha sind wir noch deutlich entfernt, weil nach Phase 6 auch Codegen-/Dispatch-Haertung, Tooling, Kompatibilitaet, GUI und `.gdi`-Workflow fehlen

### Phasenstatus

- [x] Phase 1 - SH-4 Integer-Kern: 31/31 Tasks = 100%
- [x] Phase 2 - Loader und Analyse: 21/21 Tasks = 100%
- [x] Phase 3 - Katana-IR: 14/14 Tasks = 100%
- [x] Phase 4 - Runtime-Grundlage: 18/18 Tasks = 100%
- [x] Phase 5 - SH-4 FPU: 10/10 Tasks = 100%
- [~] Phase 6 - Dreamcast-Plattform: 12/29 Tasks = 41.4%
- [ ] Phase 7 - Codegen und Dispatch: 0/21 Tasks = 0%
- [ ] Phase 8 - Werkzeuge und Qualitaet: 0/25 Tasks = 0%
- [ ] Phase 9 - Kompatibilitaet und Leistung: 0/24 Tasks = 0%
- [ ] Phase 10 - Desktop-GUI und Quellworkflow: 0/13 Tasks = 0%
- [ ] Spaetere Release-Gates: 0/3 Tasks = 0%

## Teststatus

```text
100% tests passed out of 100 (lokal, Debug und Release)
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
- [x] KR-2205 - MMIO-Handler
- [x] KR-2206 - Ausrichtung, Fehler und Watchpoints
- [x] KR-2207 - v0.22 Release-Gate
- [x] KR-2301 - SR-Felder und Interruptmasken
- [x] KR-2302 - Exception-Eintritt
- [x] KR-2303 - Interrupt-Controller
- [x] KR-2304 - TRAPA und RTE
- [x] KR-2305 - Delay-Slot-Ausnahmen
- [x] KR-2306 - v0.23 Release-Gate
- [x] KR-2307 - Ausfuehrbare Delay-Slot- und Interrupt-Review-Regressionen
- [x] KR-2401 - FR- und XF-Baenke
- [x] KR-2402 - Single-Precision-Arithmetik
- [x] KR-2403 - Vergleiche und Konvertierungen
- [x] KR-2404 - FPSCR-Modi
- [x] KR-2405 - Double-Precision
- [x] KR-2406 - v0.24.0 Release-Gate
- [x] KR-2501 - FSCA und FSRRA
- [x] KR-2502 - FIPR und FTRV
- [x] KR-2503 - NaN, Rundung und Sonderwerte
- [x] KR-2504 - FPU-Konformitaetssuite
- [x] KR-2601 - Plattformkonfiguration und Bootzustand
- [x] KR-2602 - Homebrew-Raw- und ELF-Start
- [x] KR-2603 - Minimales Plattformlogging
- [x] KR-2604 - Firmware-Betriebsart und BIOS-ABI
- [x] KR-2605 - PREF und bootrelevante Cacheeffekte
- [x] KR-2606 - Zustandsbehaftetes Flash-Geraetemodell
- [x] KR-2607 - FCNVDS-DN-Review-Regression
- [x] KR-2701 - Maple-Bus
- [x] KR-2702 - Controller und deterministische Host-Eingabe
- [x] KR-2703 - VMU-Minimum
- [x] KR-2801 - PVR-Registerminimum
- [x] KR-2802 - Framebuffer-Ausgabe

## Naechster Roadmap-Task

- [ ] KR-2701 - Maple-Bus

## Aktuelle Einschraenkungen

- unvollstaendiger SH-4-Befehlssatz
- FPU-Grund- und Vektoroperationen sind vorhanden; vollstaendige FPSCR-Exception-Flags bleiben ausserhalb des aktuellen Konformanzvertrags
- Dreamcast-Speicherbereiche, MMIO-Handler, strukturierte SH-4-Ausnahmen, Interruptprioritaeten und Watchpoints sind vorhanden; konkrete Plattformregister fehlen noch
- BIOS-freier Homebrew-Boot, PREF-Beobachtung und protokolliertes Flash sind vorhanden; weitere Plattformhardware folgt ab v0.27
- nur einfache konstante indirekte Ziele und bekannte begrenzte Jump Tables werden aufgeloest
