# KatanaRecomp Task-Katalog

Dieses Dokument enthaelt nur aktive und zukuenftige Aufgaben. Abgeschlossene
Detailtasks bleiben in Git nachvollziehbar und werden nicht mehr auf mehreren
tausend Zeilen wiederholt.

## Regeln

- Eine Implementierung bearbeitet normalerweise genau eine Task-ID.
- Allgemeine Semantik und verteilbare Regressionen sind Pflicht.
- Private Retaildaten und daraus erzeugte Artefakte bleiben ausserhalb des Repos.
- Vor Abschluss von v0.47 darf eine private Sonic-`game.exe` gebaut, aber nicht gestartet werden.
- Gate-Vorbereitung stoppt immer fuer das Nutzerreview.
- Gate-Freigaben erzeugen nur dann einen oeffentlichen Release, wenn die Task das
  ausdruecklich verlangt.

## Empfohlene Reihenfolge

```text
KR-4611 bis KR-4617
  -> KR-4618
  -> KR-4621 bis KR-4624
  -> KR-4625
  -> KR-4715
  -> KR-4716 und KR-4717
  -> KR-4718
  -> KR-4719
  -> KR-4703
  -> KR-4704
  -> KR-4705
  -> Alpha-Bring-up
```

Korrektheit blockiert Performance, Performance blockiert neue
Retail-Kontrollflussarbeit. Innerhalb einer Stufe duerfen unabhaengige Tasks
parallel entwickelt werden.

---

## v0.47.0 - Core-Stabilisierung und generische Retail-Runtime

## Stufe A: P0-Core-Korrektheit

### [ ] KR-4611 - SH-4-Kontrollzustand, Delay Slots, RTE, SLEEP und Interrupts

Abhaengigkeiten: KR-4605
Prioritaet: P0

Umfang:

- effektive R0-R7-Bank aus `SR.MD && SR.RB` ableiten
- verzoegerte PR-Semantik fuer BSR, BSRF und JSR modellieren
- RTE als Fortsetzung bei SPC mit restauriertem SR behandeln
- SLEEP bis zu einem akzeptierten Interrupt anhalten
- normale Gast-Exceptions und Interrupts zum Handler dispatchen statt den
  Hostlauf abzubrechen
- Cause, Eventcode, Vektor, TEA, SPC und Delay-Slot-Zustand tabellengetrieben
  zusammenhalten

Akzeptanz:

- User-/Privileged- und RB0-/RB1-Uebergaenge sind bitgenau getestet
- STS/LDS PR im Call-Delay-Slot besitzen unabhaengige Referenzvektoren
- RTE, SLEEP, Maskierung, Interruptannahme und Handler-Rueckkehr laufen
  identisch im Referenz- und generierten Pfad
- keine bestehende korrekte CPU-Regression wird abgeschwaecht

### [ ] KR-4612 - Store Queue und Cacheadressierung

Abhaengigkeiten: KR-4611
Prioritaet: P0

Umfang:

- SQ0/SQ1 aus Adressbit 5 waehlen
- P4-Lese-/Schreibfenster, QACR und PREF gemeinsam korrigieren
- Cachemaintenance und Operand-Cache-RAM-Zugriffsbreiten pruefen
- bisherige falsche Bit-25-Testannahmen als Bugregression sichern

Akzeptanz:

- `0xE0000000` und `0xE0000020` waehlen getrennte Queues
- Byte-, Word- und Longwordzugriffe sowie Queuegrenzen sind getestet
- RAM- und TA-Transfers besitzen identische Referenzbytes
- ICBI, MOVCA, OCBI, OCBP und OCBWB sind korrekt oder sichtbar nicht unterstuetzt

### [ ] KR-4613 - Einheitliche Gastwrites und Codeinvalidierung

Abhaengigkeiten: KR-4611, KR-4612
Prioritaet: P0

Umfang:

- CPU-, FPU-, DMA-, Store-Queue-, Copy- und Fallbackwrites ueber einen
  einheitlichen beobachtbaren Speichervertrag fuehren
- Byteidentitaet vor Generationserhoehung und Blockinvalidierung pruefen
- Aliasbloecke, eingehende Links und Dispatch-/Inline-Caches gemeinsam
  invalidieren
- generierte Stores duerfen den Tracker nicht umgehen

Akzeptanz:

- selbstmodifizierender synthetischer Code kann keinen stale Block ausfuehren
- identische Writes invalidieren nicht
- physische Aliase und ueberlappende Writes sind getestet
- sicherer Referenzpfad und optimierter Writepfad sind differenziell identisch

### [ ] KR-4614 - Sounde Kontrollfluss- und Wertanalyse

Abhaengigkeiten: KR-4611
Prioritaet: P0

Umfang:

- Vollstaendigkeit als Teil des abstrakten Wertes modellieren
- unbekannte Caller und unbekannte Pfade konservativ zusammenfuehren
- Zielmengen pro Instruktionssite vereinigen statt nach Adresse zu verwerfen
- CFG-, Join- und Delay-Slot-Kontexte durch einen echten Worklist-Fixpunkt
  tragen
- Provenienz typisieren und deterministisch erhalten

Akzeptanz:

- ein unbekannter Caller verhindert eine faelschlich vollstaendige Guardmenge
- mehrere Callkontexte vereinigen alle Ziele und Unsicherheiten
- kleine synthetische Programme werden gegen exhaustive Ausfuehrung verglichen
- dieselbe Eingabe erzeugt bytegleiche Berichte und Zielmengen

### [ ] KR-4615 - Stabile und skalierbare Runtime-Blockregistry

Abhaengigkeiten: KR-4613
Prioritaet: P0

Umfang:

- rohe Vektorzeiger durch stabile Block-IDs oder Handles ersetzen
- statische Bloecke bulk-registrieren und unveraenderlich indexieren
- dynamische Bloecke, Varianten und physische Aliase getrennt indexieren
- Erase, Invalidierung und Reaktivierung ohne Dangling Pointer implementieren

Akzeptanz:

- Registrierung und Lookup von 100.000 Bloecken bleiben in festen Budgets
- Lookup ist O(1) oder O(log N)
- Mutation bei wiederholtem Dispatch erzeugt keine ungueltigen Handles
- deterministische Blockidentitaeten bleiben erhalten

### [ ] KR-4616 - Einheitliches Gasttiming und Scheduler-/Geraeteintegration

Abhaengigkeiten: KR-4611, KR-4613
Prioritaet: P0

Umfang:

- einen zentralen versionierten Gastzyklusvertrag definieren
- Instruktionskosten, TMU, RTC, DMA, GD-ROM und PVR auf denselben Scheduler
  legen
- separate GD-ROM-Uhr entfernen
- SLEEP-Wakeup, Eventreihenfolge und Laufbudget vereinheitlichen
- Runtime-Metriken aus derselben Quelle ableiten

Akzeptanz:

- identische Laeufe liefern identische Ereignisreihenfolgen
- GD-ROM-Requests schliessen ohne manuellen zweiten Clock-Aufruf ab
- DMA-/Timer-/Interrupt-Reihenfolge ist gegen Referenzvektoren getestet
- `KATANA_GUEST_CYCLE_BUDGET` begrenzt tatsaechlich den Gastlauf

### [ ] KR-4617 - Unabhaengige Cross-Engine-Konformitaetstests

Abhaengigkeiten: KR-4611 bis KR-4616
Prioritaet: P0

Umfang:

- Referenzvektoren unabhaengig von der aktuellen Implementierung ableiten
- Decoder, IR, generierten C++-Pfad und Referenz-/Interpreterpfad vergleichen
- Registerbank, PR-Delay, RTE, SLEEP, Exceptions, SQ, Invalidierung und Timing
  als Pflichtkorpus aufnehmen
- falsche historische Erwartungen ausdruecklich markieren

Akzeptanz:

- Debug und RelWithDebInfo liefern dieselben Gastzustaende
- jede P0-Korrektur besitzt Erfolgs-, Grenz- und Fehlerfall
- kein Test verwendet dieselbe falsche Produktfunktion als Orakel

### [ ] KR-4618 - Core-Korrektheitsgate

Abhaengigkeiten: KR-4611 bis KR-4617
Prioritaet: P0

Akzeptanz:

- frischer Debug- und RelWithDebInfo-Build
- ASan/UBSan beziehungsweise MSVC-ASan, statische Analyse und Differentialtests
- vollstaendige bestehende Regression plus neues Konformitaetskorpus
- null bekannte P0-Semantikfehler
- danach beginnt erst die Performance-Stufe

## Stufe B: P1-Performance und Build

### [ ] KR-4621 - Speicher-, Dispatch- und Invalidierungs-Hotpaths

Abhaengigkeiten: KR-4618
Prioritaet: P1

Umfang:

- Seiten-/Regionsindex und direkte RAM-/VRAM-/AICA-Fastpaths
- native u16/u32-Zugriffe fuer lineare Speichergeraete
- Nullkostenpfad ohne Trace oder Watchpoint
- Page-to-Block-Invalidierungsindex und begrenzte Diagnosepuffer
- deterministische DMA-Batches

Akzeptanz:

- jeder Fastpath besitzt einen deaktivierbaren Referenzpfad
- Gastresultate bleiben bytegleich
- Memory-, Dispatch- und Invalidierungsbenchmarks verbessern sich messbar
- lange Laeufe besitzen keine ungebremst wachsenden Diagnosevektoren

### [ ] KR-4622 - Inkrementelle Analyse, IR und Codegen

Abhaengigkeiten: KR-4618
Prioritaet: P1

Umfang:

- inkrementelle CFG-/SCC-Worklists statt Ganzprogrammlaeufen
- immutable Arenen, Spans und Indizes statt grosser Kopien
- Block-, Site-, Edge- und Funktionsindizes gemeinsam verwenden
- Bulk-Codegen und stabile Partitionen
- Codegencache auf SHA-256 und atomaren Publish umstellen

Akzeptanz:

- Resultate bleiben bytegleich zum sicheren Referenzmodus
- 10k-, 50k- und 100k-Block-Fixtures besitzen feste Zeit-/Speicherbudgets
- abgebrochene Cachewrites koennen keinen gueltigen Hit vortaeuschen
- parallele Laeufe korrumpieren den Cache nicht

### [ ] KR-4623 - Disc-, GDI-, ISO- und GD-ROM-I/O

Abhaengigkeiten: KR-4616, KR-4618
Prioritaet: P1

Umfang:

- persistente read-only Dateihandles oder `pread`
- Track-/LBA-Index, Batchreads und begrenzten Sektorcache
- ISO-Verzeichnis- und Extentcache
- Provenienz-Hashes zwischen Analyse, Build und Runtime wiederverwenden
- GD-ROM-I/O ohne Host-Wall-Clock und doppelte Pufferketten

Akzeptanz:

- wiederholte Reads oeffnen Tracks nicht pro Zugriff neu
- Cache an/aus liefert identische Bytes und Ereignisse
- grosse sequenzielle und zufaellige Reads besitzen Benchmarks
- Pfad-, Identitaets- und Read-only-Vertraege bleiben unveraendert

### [ ] KR-4624 - Buildgraph, Runtime-SDK, Cache und Testmatrix

Abhaengigkeiten: KR-4618
Prioritaet: P1

Umfang:

- MSVC, GCC und Clang in Debug und RelWithDebInfo pruefen
- Core-/CLI-Presets ohne Desktop-GUI als Standard
- minimales installierbares Runtime-SDK und `find_package` fuer Ports
- Portprojekte duerfen nicht den gesamten Katana-Quellbaum bauen
- Tests nach Subsystem konsolidieren
- CMake-, Package- und ABI-Versionen aus einer kanonischen Quelle erzeugen

Akzeptanz:

- Portbuild linkt nur benoetigte Runtimeziele
- sauberer Out-of-Tree-Build funktioniert ohne Analyzerquellbaum
- Releaseoptimierung wird dauerhaft regressionsgeprueft
- Test- und Portbuildzeiten besitzen dokumentierte Baselines

### [ ] KR-4625 - Performance-/Buildgate

Abhaengigkeiten: KR-4621 bis KR-4624
Prioritaet: P1

Akzeptanz:

- alle Korrektheits- und bestehenden Funktionstests bleiben gruen
- Debug- und RelWithDebInfo-Gastresultate sind identisch
- Speicher, Dispatch, Analyse, Codegen, Disc-I/O und Portbuild halten Budgets
- keine Optimierung wird ohne gemessenen Nutzen aktiviert
- danach darf KR-4715 beginnen

## Stufe C: Retail-Kontrollfluss und Build

### [ ] KR-4715 - Ungeloeste Kontrollflussfront inventarisieren

Abhaengigkeiten: KR-4618, KR-4625
Prioritaet: P0

Umfang:

- jede `unresolved` Stelle genau einer stabilen Herkunftsklasse zuordnen
- Callback, Parameter, Stack, Objekt/VTable, Tabelle, unbeschraenkten Speicher
  und echten Laufzeitzeiger getrennt berichten
- aggregierte Text- und JSON-Summen deterministisch erzeugen
- private Adressen nur im externen lokalen Bericht zulassen

Akzeptanz:

- `resolved + guarded + unresolved == indirect_total`
- jede offene Stelle besitzt genau eine Klasse und eine Beweisherkunft
- Klassensumme und `unresolved_frontier` stimmen exakt ueberein
- jede Klasse besitzt eine synthetische positive oder negative Regression

### [ ] KR-4716 - ABI-erhaltene Callback-, Parameter- und Stackwerte

Abhaengigkeiten: KR-4715
Prioritaet: P0

Umfang:

- R8 bis R14 kontextsensitiv ueber direkte und guarded Calls verfolgen
- R13-basierte Callbackpfade allgemein modellieren
- feste Stackspills, Reloads und Frame-Offsets in begrenzte Slices aufnehmen
- unbekannte Caller, Rekursion und Aliasverletzungen konservativ behandeln

Akzeptanz:

- direkte und guarded Calls, mehrere Caller, R13, Stackspills und Rekursion sind
  synthetisch getestet
- nur vollstaendige endliche Mengen werden `guarded`
- die Analyse terminiert deterministisch unter festen Budgets

### [ ] KR-4717 - Objekt-, Feld- und VTable-Points-to

Abhaengigkeiten: KR-4715
Prioritaet: P0

Umfang:

- Objektzeiger durch Register, Stack und feste Feldoffsets verfolgen
- Konstruktor- und Initialisierungsstores fuer VTables und Callbacks erkennen
- mehrere konkrete Typen als endliche Kandidatenmenge erhalten
- unbekannte Stores, Aliaswechsel und externe Mutationen invalidieren
- beschreibbare VTables niemals statisch einfrieren

Akzeptanz:

- Konstruktorstore, Feldoffset, mehrere Typen, Alias und unbekannter Store sind
  synthetisch getestet
- endliche VTable-Slots werden nur mit vollstaendiger Evidenz `guarded`
- der dynamische Runtime-Default bleibt erhalten

### [ ] KR-4718 - Expliziter Runtime-only-Dispatch

Abhaengigkeiten: KR-4716, KR-4717
Prioritaet: P0

Umfang:

- `runtime-only` als vierte Kontrollflussklasse einfuehren
- nur klassifizierte echte Laufzeitquellen duerfen diesen Status erhalten
- Zieladresse, Ausrichtung, Executable-Image und Runtime-Blocktabelle validieren
- Blocktabellen-Misses sichtbar stoppen oder kontrolliert fallbacks ausfuehren
- Hits, Misses, Fallbacks und erste Fehler maschinenlesbar zaehlen

Akzeptanz:

- `resolved + guarded + runtime_only + unresolved == indirect_total`
- `unresolved == 0` bleibt Voraussetzung fuer Export und Build
- Runtime-only-Misses erzeugen weder Erfolg noch Sonic-Checkpoint
- Hostadressen, No-ops und geratene Ziele sind ausgeschlossen

### [ ] KR-4719 - Privater Retail-Buildnachweis ohne Ausfuehrung

Abhaengigkeiten: KR-4718
Prioritaet: P0

Umfang:

- die private Sonic-GDI ueber den offiziellen Workflow vollstaendig analysieren
- Portprojekt, generierten Code und `game.exe` ausserhalb des Repos erzeugen
- die erzeugte `game.exe` ausdruecklich nicht starten
- Identitaet, Abdeckung und Buildresultat nur aggregiert berichten

Akzeptanz:

- `unresolved == 0`
- Analyse, Codegen und Hostbuild enden erfolgreich
- zwei Builds besitzen dieselben portablen Metadaten und generierten Quellen
- `game_executable_started == false`
- kein Checkpoint hoeher als `SA_ANALYSIS_CONTINUES` wird ausgegeben

### [ ] KR-4703 - VMU-/Flash-Arbeitskopien und Host-Pacing

Abhaengigkeiten: KR-4616, KR-4625, KR-4719
Prioritaet: P1

Umfang:

- Saves und Flash nur als getrennte lokale Arbeitskopien verwalten
- atomisches Speichern und Recovery implementieren
- Gastzeit, Audio, Video und Host-Pacing verbinden
- Nutzerquellen und Portartefakte unveraendert halten

Akzeptanz:

- defekte oder geloeschte Arbeitskopien veraendern nie die Quelle
- Pause, Resume und Shutdown bleiben deterministisch
- Persistenz- und Pacingfehler sind reproduzierbar diagnostizierbar

### [ ] KR-4704 - v0.47 Gate-Vorbereitung

Abhaengigkeiten: KR-4703, KR-4715 bis KR-4719, KR-4625
Prioritaet: P0

Akzeptanz:

- alle Core-, Performance- und v0.47-Regressionen bestehen
- Debug und RelWithDebInfo liefern identische Gastresultate
- `unresolved == 0`
- eine frei lizenzierte Anwendung erreicht `KR_V047_NATIVE_HOST_READY`
- der private Sonic-Workflow wiederholt den Buildnachweis ohne Prozessstart
- keine privaten Retaildaten gelangen in Berichte oder Staging
- danach fuer Nutzerreview stoppen

### [ ] KR-4705 - v0.47 interne Freigabe

Abhaengigkeiten: KR-4704

Akzeptanz:

- die unveraenderte Gate-Vorbereitung ist ausdruecklich freigegeben
- die Freigabe startet die Alpha-Arbeit
- es erfolgen noch kein Tag, Download oder oeffentlicher Release

---

## v0.50.0 Alpha - Sonic Adventure Bring-up

### [ ] KR-4801 - Runtimebeobachtung, Replay und Fehlerpakete

Abhaengigkeiten: KR-4705
Prioritaet: P0

Umfang:

- Blockdispatch, Exceptions, Fallbacks, MMIO, GD-ROM, DMA, Interrupts, PVR und
  AICA in einem begrenzten Ereignisstrom erfassen
- ersten Fehler, letzten stabilen Checkpoint und relevante CPU-Zustaende sichern
- deterministische synthetische Replays und redigierte private Fehlerberichte
  erzeugen
- Rohspeicher und private Identitaetsdaten niemals committen

Akzeptanz:

- Haenger, Budgetende, Exception und Dispatch-Miss besitzen eindeutige Klassen
- identische synthetische Laeufe erzeugen identische Ereignisfolgen
- private Fehlerpakete sind redigiert und bleiben ausserhalb des Repos

### [ ] KR-4802 - Dynamische Codebereiche, Module und Overlays

Abhaengigkeiten: KR-4801
Prioritaet: P0

Umfang:

- nach dem Boot geladene ausfuehrbare Bereiche erkennen
- Discmodule, Overlays und ersetzten RAM-Code allgemein katalogisieren
- bekannte Module vorab rekompilieren oder ueber einen expliziten kontrollierten
  Runtimepfad materialisieren
- Codeinvalidierung, Aliase und Lebenszeit der Bloecke erhalten
- keine versteckte Analyzer-Abhaengigkeit in den Port einfuehren

Akzeptanz:

- synthetische Load-, Relocation-, Austausch- und Unload-Faelle bestehen
- unkompilierte ausfuehrbare Bytes koennen nicht still ausgefuehrt werden
- private Modulinhalte bleiben ausserhalb von Repository und Berichten

### [ ] KR-4803 - CPU-/Plattform-Bring-up bis SA_MAIN_ENTERED

Abhaengigkeiten: KR-4802
Prioritaet: P0

Umfang:

- die private Sonic-`game.exe` erstmals budgetiert starten
- SH-4-, FPU-, MMU-/Cache-, BIOS-, GD-ROM-, DMA-, Interrupt- und Timingblocker
  titelunabhaengig beheben
- jeden Retailbefund als synthetische oder frei lizenzierte Regression sichern

Akzeptanz:

- zwei identische Laeufe erreichen `SA_MAIN_ENTERED`
- kein Fallback, Trap oder stiller Fehler wird als Erfolg ausgegeben
- deterministische Kernmetriken stimmen ueberein

### [ ] KR-4804 - Gast-PVR-Pfad bis SA_FIRST_FRAME

Abhaengigkeiten: KR-4803
Prioritaet: P0

Umfang:

- echten Gast-PVR-/TA-Befehlsstrom verarbeiten
- benoetigte Polygon-, Textur-, Tiefen-, Blend-, Fog- und Listenpfade allgemein
  implementieren
- Hostframe ausschliesslich aus Gastzustand erzeugen
- Frame-, PVR-, DMA- und Interruptfortschritt diagnostizieren

Akzeptanz:

- `SA_FIRST_FRAME` wird aus einem Gastframe erreicht
- ein synthetisch vorgefuellter VRAM-Puffer gilt nicht als Nachweis
- Fehlerpfade bleiben sichtbar und reproduzierbar

### [ ] KR-4805 - Menue, Eingabe und spielbare Szene

Abhaengigkeiten: KR-4804
Prioritaet: P0

Umfang:

- Maple-Eingabe bis in das Gastprogramm verfolgen
- Menue, Discstreaming, Scheduler, DMA und PVR gemeinsam stabilisieren
- `SA_MENU_INTERACTIVE` und anschliessend `SA_ALPHA_PLAYABLE` erreichen
- bekannte Audioabweichungen messen und dokumentieren

Akzeptanz:

- zwei identische Laeufe erreichen dieselbe kontrollierbare Szene
- Video und Eingabe funktionieren gemeinsam
- Disc-I/O und Scheduler machen weiter Fortschritt
- `silent_failures == 0`

### [ ] KR-4901 - Alpha-Haertung, Paketierung, CI und Audit

Abhaengigkeiten: KR-4805
Prioritaet: P0

Umfang:

- Windows-Paket, Runtimeabhaengigkeiten und First-Run-Diagnostik fertigstellen
- Linux-Core/CLI-Build und gemeinsame Regression in CI absichern
- GUI-, CLI-, Manifest-, Runtime-ABI- und Berichtsvertraege versionieren
- reproduzierbare Pakete, Datenschutz-, Lizenz- und Referenzaudits ausfuehren
- bekannte Alpha-Einschraenkungen und Fehler dokumentieren

Akzeptanz:

- Windows-Paket startet auf einem sauberen Testsystem
- Linux Core und CLI bauen und testen reproduzierbar
- Pakete enthalten keine privaten oder proprietaeren Daten
- jede oeffentliche Faehigkeitsbehauptung besitzt einen Nachweis

### [ ] KR-4999 - Alpha-Gate-Vorbereitung

Abhaengigkeiten: KR-4901
Prioritaet: P0

Akzeptanz:

- frische Debug- und Release-Builds, vollstaendige Regression und CI bestehen
- zwei private Laeufe erreichen `SA_ALPHA_PLAYABLE`
- Gatebericht, Pakete, Audits und bekannte Einschraenkungen liegen vor
- danach zwingend fuer Nutzerreview stoppen

### [ ] KR-5000 - v0.50.0 Alpha-Release

Abhaengigkeiten: KR-4999

Akzeptanz:

- die unveraenderte KR-4999-Vorbereitung ist ausdruecklich freigegeben
- Version, Release-Commit, Tag und Downloads werden erst danach erzeugt
- Release und Repository enthalten keine Retaildaten

---

## v0.75.0 Beta - Breite Spielbarkeit

### [ ] KR-6001 - Sonic-Adventure-Abdeckung und Save-Kompatibilitaet

Abhaengigkeiten: KR-5000

Umfang:

- lange Spielabschnitte, Cutscenes, Bosse und Szenenwechsel abdecken
- Save, Laden, Neustart und VMU-Kompatibilitaet stabilisieren
- Storypfade und Sondermodi in einer Statusmatrix pflegen

Akzeptanz:

- mindestens eine Story laeuft von neuem Save bis zu den Credits
- Saves bleiben ueber Neustarts und neue Builds verwendbar
- bekannte Blocker sind reproduzierbar diagnostizierbar

### [ ] KR-6002 - PVR- und AICA-Genauigkeit

Abhaengigkeiten: KR-5000

Umfang:

- fehlende PVR-Textur-, Blend-, Fog-, Modifier- und Render-to-Texture-Pfade
  ergaenzen
- AICA-HLE erweitern oder notwendige ARM7-/LLE-Pfade einfuehren
- Streaming, ADPCM und CD-Audio nachvollziehbar behandeln

Akzeptanz:

- Bild- und Audioabweichungen besitzen Referenztests und bekannte Grenzen
- keine stille Audio- oder Renderauslassung im unterstuetzten Profil

### [ ] KR-6003 - Performance, Pacing und Langzeitstabilitaet

Abhaengigkeiten: KR-6001, KR-6002

Umfang:

- Codecache, Dispatch, Speicherpfade und Threading profilbasiert optimieren
- lange Sitzungen, Speicherverbrauch, Pause und Fokuswechsel testen
- LTO und PGO nur bei gemessenem Nutzen aktivieren

Akzeptanz:

- definierte Testprofile erreichen praktikable Performancebudgets
- Langzeitlaeufe besitzen keine bekannten Leaks oder stillen Deadlocks
- Optimierungen veraendern keine Gastsemantik

### [ ] KR-6004 - Mehrtitel-Kompatibilitaet und Debuggerwerkzeuge

Abhaengigkeiten: KR-6003

Umfang:

- mehrere rechtmaessig lokale Titel bis in interaktive Szenen bringen
- Kompatibilitaetsmatrix und bekannte Fehler pflegen
- Trace-, Breakpoint-, Speicher- und Dispatchdiagnostik fuer Entwickler anbieten

Akzeptanz:

- mehrere Titel erreichen reproduzierbar interaktive Szenen
- Abstuerze erzeugen verwertbare Berichte
- Titelsonderfaelle bleiben verboten oder werden klar als externe Patches
  ausserhalb des Kernprojekts behandelt

### [ ] KR-7499 - Beta-Gate-Vorbereitung

Abhaengigkeiten: KR-6004

Akzeptanz:

- frische Builds, Regression, CI, Kompatibilitaetsmatrix und Berichte bestehen
- danach fuer Nutzerreview stoppen

### [ ] KR-7500 - v0.75.0 Beta-Release

Abhaengigkeiten: KR-7499

Akzeptanz:

- die unveraenderte Gate-Vorbereitung ist ausdruecklich freigegeben
- Beta-Pakete, Release Notes und bekannte Fehler werden veroeffentlicht

---

## v1.0.0 Stable - Stabiles Framework

### [ ] KR-9001 - Oeffentliche Vertrage und Supportumfang einfrieren

Abhaengigkeiten: KR-7500

Umfang:

- CLI, Manifest, Runtime-ABI, SDK, Blockmetadaten und Replayformat stabilisieren
- unterstuetzten SH-4-, Plattform- und Spieleumfang explizit definieren
- Deprecation- und Kompatibilitaetsregeln dokumentieren

### [ ] KR-9002 - Plattformpakete, Installation und Migration

Abhaengigkeiten: KR-9001

Umfang:

- reproduzierbare Windows-Pakete und definierten Linux-Support liefern
- portable und installierte Nutzung testen
- Konfigurationen, Saves, Ports und Cacheformate migrieren

### [ ] KR-9003 - Langzeit-QA, Dokumentation und Wartung

Abhaengigkeiten: KR-9002

Umfang:

- Kompatibilitaetskorpus und Langzeitlaeufe automatisieren
- Sicherheits-, Datenschutz-, Lizenz- und Reproduzierbarkeitsaudits abschliessen
- Architektur-, Portierungs-, Debug- und Upgrade-Dokumentation vervollstaendigen

### [ ] KR-9999 - v1.0 Gate-Vorbereitung

Abhaengigkeiten: KR-9003

Akzeptanz:

- frische Builds, CI, Regression, Langzeitlaeufe, Pakete und Audits bestehen
- keine bekannte stille Fehlkompilierung im unterstuetzten Bereich
- danach fuer Nutzerreview stoppen

### [ ] KR-10000 - v1.0.0 Release

Abhaengigkeiten: KR-9999

Akzeptanz:

- die unveraenderte Gate-Vorbereitung ist ausdruecklich freigegeben
- stabile Pakete, Dokumentation und Supportmatrix werden veroeffentlicht
