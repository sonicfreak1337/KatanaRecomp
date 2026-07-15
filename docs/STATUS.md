# Projektstatus

Version: 0.32.0
Phase: Pre-Alpha
Naechster Meilenstein: v0.33.0 - Skalierbare Codeausgabe und Build-Cache

## Zusammenfassung

KatanaRecomp besitzt einen durchgaengigen Prototyp-Pfad von Raw- und ELF32-SH-Eingaben ueber ein segmentiertes Executable Image bis zu generiertem, gegen die zentrale Runtime-Bibliothek gelinktem und ausgefuehrtem C++.

## Fortschrittseinordnung

### Gesamtprojekt

- [x] Kernunterbau abgeschlossen: Phasen 1 bis 5 sind vollstaendig umgesetzt
- [~] Gesamtfortschritt nach gepflegten Roadmap-Tasks: 128 von 209 Tasks abgeschlossen = 61.2%
- [~] Fortschritt auf dem Weg von Dreamcast-Plattform bis Alpha: 34 von 112 Tasks abgeschlossen = 30.4%
- [ ] Alpha-Gate erreicht: nein

### Weg zum ersten echten Dreamcast-Test

Definition fuer diesen Status: ein BIOS-freier, frei verteilbarer Homebrew-Vertical-Slice gemaess Phase-6-Release-Gate, der Bild zeigt, Eingabe annimmt und Audio erzeugt.

- [x] Boot- und Homebrew-Einstieg vorhanden: v0.26.0 abgeschlossen
- [x] Dreamcast-Plattform-Implementierung: 29 von 29 Tasks und das kumulative Phase-6-Gate sind abgeschlossen
- [x] Eingabeweg fertig: Maple, Controller und deterministische Replays (`KR-2701` bis `KR-2703`) sind abgeschlossen
- [x] PVR-Minimalbildpfad fertig: Register, Framebuffer, Tile-Accelerator, erste Texturformate und Render-Backend (`KR-2801` bis `KR-2804`) sind abgeschlossen
- [x] AICA-Minimalaudiopfad fertig: Register, PCM/ADPCM, Mixer, Host-Audio sowie HLE-Timer und Interrupts (`KR-2901` bis `KR-2904`) sind vorhanden
- [x] Takt- und Ereignispfad implementiert: Scheduler, TMU, RTC, DMA, Plattform-Interruptintegration und gemeinsame Frame-/Audio-Taktung (`KR-3101` bis `KR-3105`) sind vorhanden
- [x] Disc-Pfad fertig: read-only Quellen, GD-ROM, ISO9660, Timing sowie GDI-Trackmodell und -Integration (`KR-3001` bis `KR-3006`) sind vorhanden

Praktische Einordnung:

- [x] Der frei verteilbare synthetische Vertical Slice bootet, verarbeitet Eingabe, rendert ein Bildprimitiv und erzeugt Audio in einem gemeinsamen Lauf
- [x] Der kumulative lokale Phase-6-Test erreicht `SA_PHASE6_MAIN_EXECUTION_STARTED`
- [x] Modulare Backend-, Block-ABI- und Plattformdienst-Schnittstelle von v0.32.0 abgeschlossen
- [~] Der naechste Engpass ist die skalierbare, deterministische und inkrementelle Codeausgabe von v0.33.0
- [ ] Von Alpha sind wir noch deutlich entfernt, weil nach Phase 6 auch Codegen-/Dispatch-Haertung, Tooling, Kompatibilitaet, GUI und `.gdi`-Workflow fehlen

### Phasenstatus

- [x] Phase 1 - SH-4 Integer-Kern: 31/31 Tasks = 100%
- [x] Phase 2 - Loader und Analyse: 21/21 Tasks = 100%
- [x] Phase 3 - Katana-IR: 14/14 Tasks = 100%
- [x] Phase 4 - Runtime-Grundlage: 18/18 Tasks = 100%
- [x] Phase 5 - SH-4 FPU: 10/10 Tasks = 100%
- [x] Phase 6 - Dreamcast-Plattform: 29/29 Tasks und Abschlussgate = 100%
- [~] Phase 7 - Codegen und Dispatch: 5/21 Tasks = 23.8%
- [ ] Phase 8 - Werkzeuge und Qualitaet: 0/25 Tasks = 0%
- [ ] Phase 9 - Kompatibilitaet und Leistung: 0/24 Tasks = 0%
- [ ] Phase 10 - Desktop-GUI und Quellworkflow: 0/13 Tasks = 0%
- [ ] Spaetere Release-Gates: 0/3 Tasks = 0%

## Teststatus

Letztes abgeschlossenes Release-Gate (`v0.32.0`):

```text
100% tests passed out of 127 (frischer lokaler Debug-Build)
```

Aktueller Release-Stand:

```text
127/127 Tests bestanden
modulare Backend-, Block-ABI- und Plattformdiensttests bestanden
kein Sonic-Adventure-Test am Zwischenrelease v0.32.0 erforderlich oder ausgefuehrt
```

Aktueller Entwicklungsstand (`v0.32.0`):

```text
127/127 Debug-Tests bestanden
katana-platform-services-tests bestanden
```

Abgeschlossenes kumulatives Phase-6-Gate:

```text
tracks_validated=3; iso9660_mounted=true; boot_file_loaded=true
executed_blocks=1; guest_cycles=16; scheduler_events=3
gdrom_completions=1; tmu_events=1; dma_events=1
interrupts_delivered=1; cache_invalidations=1; fallbacks=1
silent_failures=0
```

Phase-6-Nacharbeit: Scheduler-Advance und Reset sind reentrancy-geschuetzt;
callback-internes Stop, Stop/Start und Reset der Medienuhr sind gegen Doppel-
und Geisterereignisse regressionsgesichert. Das Release-Gate verwendet gemaess
aktueller Pre-Alpha-Regel nur Debug. Release-Build und CI kehren bei v0.50.0 zurueck.

Lokale Sonic-Adventure-Akzeptanzstrategie:

- [x] verbindliche kumulative Gates fuer Phase 6 bis Phase 10 und v0.50.0 sind in `docs/SONIC_ADVENTURE_ACCEPTANCE.md` definiert
- [x] einzelne Tasks, Commits und Zwischenreleases benoetigen keinen vollstaendigen Sonic-Adventure-Lauf
- [x] der bestehende v0.30.0-GDI-Smoke wurde beim Phase-6-Abschluss v0.31.0 kumulativ nach den neuen messbaren Kriterien revalidiert
- [x] das Phase-6-Gate v0.31.0 erreicht `SA_PHASE6_MAIN_EXECUTION_STARTED`
- [ ] kuenftige Phasen muessen die jeweils benoetigten allgemeinen Zaehler, Checkpoints und redigierten maschinenlesbaren Berichte bereitstellen, ohne spaetere Funktionen vorzuziehen

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
- [x] KR-2803 - Tile-Accelerator-Grundpfad
- [x] KR-2804 - Texturformate und Render-Backend
- [x] KR-2901 - AICA-Registerminimum
- [x] KR-2902 - PCM und ADPCM
- [x] KR-2903 - Mixer und Host-Audio
- [x] KR-2904 - ARM7-Strategie, Timer und Interrupts
- [x] KR-3001 - Disc- und Dateiquellen-Abstraktion
- [x] KR-3002 - GD-ROM-Kommandos
- [x] KR-3003 - ISO9660
- [x] KR-3004 - Asynchrone Reads und Timing
- [x] KR-3005 - GDI-Deskriptoren und Trackmodell
- [x] KR-3006 - GDI-Quellenintegration
- [x] KR-3101 - Event-Scheduler
- [x] KR-3102 - TMU und RTC
- [x] KR-3103 - DMA
- [x] KR-3104 - Plattform-Interruptintegration
- [x] KR-3105 - Frame- und Audio-Taktung
- [x] KR-3201 - Backend-Interface
- [x] KR-3202 - C++-Backend migrieren
- [x] KR-3203 - ABI-Faehigkeitspruefung
- [x] KR-3204 - Block-ABI und Zustandsuebergaben
- [x] KR-3205 - Plattformdienst-Schnittstelle
- [x] v0.32.0 Release-Gate - 127/127 Debug, kein Phasentest

## Naechster Arbeitsschritt

- [ ] KR-3301 - Translation-Unit-Partitionierung
- [ ] danach KR-3302 - Deterministische Dateinamen

## Aktuelle Einschraenkungen

- unvollstaendiger SH-4-Befehlssatz
- FPU-Grund- und Vektoroperationen sind vorhanden; vollstaendige FPSCR-Exception-Flags bleiben ausserhalb des aktuellen Konformanzvertrags
- Dreamcast-Speicherbereiche, MMIO, Ausnahmen, Interrupts, PVR, AICA, GD-ROM, Timer, DMA und Medienuhr sind als belastbares Plattformminimum vorhanden, aber noch keine vollstaendige Hardwareemulation
- BIOS-freier Homebrew-Boot, PREF-Beobachtung, protokolliertes Flash und ein integrierter synthetischer Bild-/Eingabe-/Audio-Vertical-Slice sind vorhanden
- AICA-Audio laeuft im dokumentierten HLE-Profil; ARM7-LLE ist nicht implementiert und wird sichtbar abgewiesen
- nur einfache konstante indirekte Ziele und bekannte begrenzte Jump Tables werden aufgeloest
