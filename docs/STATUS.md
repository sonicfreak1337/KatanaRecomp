# Projektstatus

Version: 0.37.0
Phase: Pre-Alpha
Naechster Meilenstein: v0.38.0 - rechtlich sauberes Homebrew-Testkorpus

## Zusammenfassung

KatanaRecomp besitzt einen durchgaengigen Prototyp-Pfad von Raw- und ELF32-SH-Eingaben ueber ein segmentiertes Executable Image bis zu generiertem, gegen die zentrale Runtime-Bibliothek gelinktem und ausgefuehrtem C++.

## Fortschrittseinordnung

### Gesamtprojekt

- [x] Kernunterbau abgeschlossen: Phasen 1 bis 5 sind vollstaendig umgesetzt
- [~] Gesamtfortschritt nach gepflegten Roadmap-Tasks: 170 von 238 Tasks abgeschlossen = 71.4%
- [~] Fortschritt auf dem Weg von Dreamcast-Plattform bis einschliesslich Alpha-Gate: 76 von 140 Tasks abgeschlossen = 54.3%
- [ ] Alpha-Gate erreicht: nein; Alpha verlangt eine aus der lokalen
  Sonic-Adventure-GDI erzeugte und gestartete `game.exe`, die reproduzierbar
  `SA_ALPHA_BOOTED` erreicht

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
- [x] Der verteilbare kumulative Phase-6-Test erreicht `KR_PHASE6_PLATFORM_INTEGRATED`; die fruehere lokale GDI-Blockprobe gilt nur als historische Quellen-/Bootblockdiagnose
- [x] Modulare Backend-, Block-ABI- und Plattformdienst-Schnittstelle von v0.32.0 abgeschlossen
- [x] Skalierbare, deterministische und inkrementelle Codeausgabe von v0.33.0 abgeschlossen
- [x] Indirekter Dispatch, Fallback und Codeinvalidierung von v0.34.0 sind abgeschlossen
- [x] Versioniertes Projektmanifest und offizieller `.gdi`-zu-Port-Projektexport mit getrenntem generiertem und handgeschriebenem Code sowie ausfuehrbarem Host-Target sind abgeschlossen
- [x] Diagnostik, reproduzierbare Debug-Gates, prozessisoliertes Fuzzing und Referenz-/Lizenzprovenienz von Phase 8 sind abgeschlossen
- [ ] Die Luecke zwischen v0.44 und Alpha wird durch v0.45 bis v0.49 fuer
  ISA-Abdeckung, Retail-Bootdienste, native Hostruntime, Portintegration und
  Alpha-CI geschlossen

### Phasenstatus

- [x] Phase 1 - SH-4 Integer-Kern: 31/31 Tasks = 100%
- [x] Phase 2 - Loader und Analyse: 21/21 Tasks = 100%
- [x] Phase 3 - Katana-IR: 14/14 Tasks = 100%
- [x] Phase 4 - Runtime-Grundlage: 18/18 Tasks = 100%
- [x] Phase 5 - SH-4 FPU: 10/10 Tasks = 100%
- [x] Phase 6 - Dreamcast-Plattform: 29/29 Tasks und Abschlussgate = 100%
- [x] Phase 7 - Codegen und Dispatch: 21/21 Tasks = 100%
- [x] Phase 8 - Werkzeuge und Qualitaet: 26/26 Tasks = 100%
- [ ] Phase 9 - Kompatibilitaet und Leistung: 0/24 Tasks = 0%
- [ ] Phase 10 - Desktop-GUI und Quellworkflow: 0/13 Tasks = 0%
- [ ] Phase 11 - Alpha-Integration und Haertung: 0/25 Tasks = 0%
- [ ] Spaetere Gate-Vorbereitungen und Release-Gates: 0/6 Tasks = 0%

## Aktueller Arbeits- und Gate-Workflow

- Implementierungs-Tasks werden zuerst inhaltlich abgearbeitet; ihre
  Erfolgs-, Grenz- und Fehlerfaelle werden als Testanforderungen gesammelt.
- Erst der letzte Gate-Vorbereitungstask setzt diese Tests um und fuehrt genau
  einen frischen Build in `build-current/` sowie die vollstaendige Regression
  aus.
- Danach wird vor jedem Phasen-Release-Gate zwingend fuer das Nutzerreview
  gestoppt. Versionierung, Release-Commit, Tag und Veroeffentlichung beginnen
  erst nach ausdruecklicher Freigabe.
- Review-Aenderungen erfordern eine vollstaendige Wiederholung der
  Gate-Vorbereitung.

## Teststatus

Letztes abgeschlossenes Release-Gate (`v0.37.0`):

```text
100% tests passed out of 151 (lokaler Debug-/ASan-/Coverage-Build)
```

Aktueller Release-Stand:

```text
151/151 Tests bestanden
Manifest, Port-Hostbuild, Diagnostik, Replay, echter generierter Differentialpfad, prozessisoliertes Fuzzing und reproduzierbares Artefakt bestanden
kein Sonic-Adventure-Test vor der Alpha-Gate-Vorbereitung KR-4999 ausgefuehrt
```

Letzter Entwicklungsnachweis vor Einfuehrung der gebuendelten
Gate-Vorbereitung (nach `KR-3409`, Basis `v0.33.0`):

```text
141/141 Debug-Tests bestanden
katana-interpreter-boundary-tests bestanden
```

Aktuelle, nach dem Review vollstaendig wiederholte v0.34-Gate-Vorbereitung
nach `KR-3410`:

```text
frischer Debug-Build in build-current: erfolgreich
142/142 Tests bestanden
katana-codegen-project-tests isoliert erneut bestanden
Scheduler-Reset-/RTCCLK-/R64CNT-/RTCEN-/CF-CIE-Regressionspfade bestanden
dynamischer Fallback: idempotente Wiederholung und Reaktivierung bestanden
generiertes PREF: SQ0->RAM und SQ1->TA ueber Plattformdienste bestanden
DMAC-NMI/AE-Verwerfen und DME-Pause bestanden
BSR/JSR/RTS-Delay-Slot-PR-Semantik bestanden
Codegen-Pfadcontainment und selektive Bereinigung bestanden
KR-3411 abgeschlossen; Release-Commit und exakter Tag v0.34.0 erstellt
Sonic-Test gemaess Strategie vor KR-4999 nicht ausgefuehrt
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
und Geisterereignisse regressionsgesichert. Dieser historische Gate-Bericht
behauptet keine Sonic-Adventure-Ausfuehrung.

Lokale Sonic-Adventure-Alpha-Akzeptanzstrategie:

- [x] Sonic Adventure wird vor v0.50.0 nicht ausgefuehrt; alle Pre-Alpha-Gates
  verwenden synthetische Fixtures und frei lizenzierte Homebrew-Programme
- [x] eine lokale GDI darf vor Alpha read-only validiert, analysiert,
  rekompiliert und bis `game.exe` gebaut werden, aber nicht gestartet werden
- [x] KR-4999 ist der erste Sonic-Ausfuehrungstest und muss den lokalen Pfad
  GDI -> Port-Projekt -> `game.exe` -> `SA_ALPHA_BOOTED` reproduzieren
- [x] ein Frame, Menue oder interaktives Gameplay ist kein Alpha-Kriterium,
  sondern gehoert zum Beta-Gate
- [x] Assetextraktion und eine Installation ohne spaetere GDI-Nutzung gehoeren
  in das titelbezogene Folgeprojekt, nicht in den allgemeinen Compilervertrag

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
- [x] KR-3301 - Translation-Unit-Partitionierung
- [x] KR-3302 - Deterministische Dateinamen
- [x] KR-3303 - Inkrementeller Codegen-Cache
- [x] KR-3304 - Parallele Ausgabe und Buildintegration
- [x] KR-3305 - Deterministische Blockmetadaten
- [x] v0.33.0 Release-Gate - 132/132 Debug, kein Phasentest

## Naechster Arbeitsschritt

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
- [x] Verpflichtender Nutzerreview-Stopp und Freigabe vor KR-3411
- [x] KR-3411 - v0.34.0 Release-Gate, 142/142 Debug, kein Sonic-Test vor KR-4999
- [ ] KR-3501 - Manifest-Schema und Versionierung

## Aktuelle Einschraenkungen

- unvollstaendiger SH-4-Befehlssatz
- FPU-Grund- und Vektoroperationen sind vorhanden; vollstaendige FPSCR-Exception-Flags bleiben ausserhalb des aktuellen Konformanzvertrags
- Dreamcast-Speicherbereiche, MMIO, Ausnahmen, Interrupts, PVR, AICA, GD-ROM, Timer, DMA und Medienuhr sind als belastbares Plattformminimum vorhanden, aber noch keine vollstaendige Hardwareemulation
- BIOS-freier Homebrew-Boot, PREF-Beobachtung, protokolliertes Flash und ein integrierter synthetischer Bild-/Eingabe-/Audio-Vertical-Slice sind vorhanden
- AICA-Audio laeuft im dokumentierten HLE-Profil; ARM7-LLE ist nicht implementiert und wird sichtbar abgewiesen
- nur einfache konstante indirekte Ziele und bekannte begrenzte Jump Tables werden aufgeloest
