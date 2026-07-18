# KatanaRecomp Task-Katalog

Dieses Dokument enthaelt nur aktive und zukuenftige Aufgaben. Abgeschlossene
Detailtasks bleiben in Git nachvollziehbar und werden nicht mehr auf mehreren
tausend Zeilen wiederholt.

## Regeln

- Eine Implementierung bearbeitet normalerweise genau eine Task-ID.
- Allgemeine Semantik und verteilbare Regressionen sind Pflicht.
- Private Retaildaten und daraus erzeugte Artefakte bleiben ausserhalb des Repos.
- Vor Abschluss von v0.47 darf eine private Sonic-`game.exe` gebaut, aber nicht
  gestartet werden.
- Deterministische Probes und interaktive Sitzungen sind getrennte Modi.
- Interaktive Sitzungen gelten nie als Gateevidenz.
- Gate-Vorbereitung stoppt immer fuer das Nutzerreview.
- Gate-Freigaben erzeugen nur dann einen oeffentlichen Release, wenn die Task das
  ausdruecklich verlangt.
- Eine Task-ID ist ab ihrem ersten Merge semantisch unveraenderlich.
- Entfallene oder ersetzte Tasks bleiben in `TASK_ID_REGISTRY.md` registriert.
- Ersatzarbeit erhaelt immer eine neue ID.

## Empfohlene Reihenfolge

```text
v0.47 Korrektheit:
KR-4611 bis KR-4617
  -> KR-4618

v0.47 Geschwindigkeit:
KR-4621 bis KR-4624
  -> KR-4625

v0.47 Retail-Build ohne Lauf:
KR-4715
  -> KR-4716 und KR-4717
  -> KR-4718
  -> KR-4719
  -> KR-4703
  -> KR-4704
  -> KR-4705

v0.48 Integration:
KR-4801, KR-4811, KR-4821 und KR-4824
  -> KR-4802
  -> KR-4803
  -> KR-4812, KR-4813 und KR-4814
  -> KR-4822
  -> KR-4823
  -> KR-4804
  -> KR-4805

v0.49 Alpha-Bring-up:
KR-4911
  -> KR-4912
  -> KR-4913
  -> KR-4914 und KR-4915
  -> KR-4916
  -> KR-4901, KR-4902 und KR-4903
  -> KR-4904
  -> KR-4905
  -> KR-4999
  -> KR-5000
```

Korrektheit blockiert Performance. Performance und Soundness blockieren neue
Retail-Codeentdeckung. GUI-, Controller- und Harnessarbeit darf nach dem
Core-Gate parallel laufen, muss aber vor dem ersten Sonic-Runtimelauf bestehen.

---

## v0.47.0 - Core-Stabilisierung und generische Retail-Runtime

## Stufe A: P0-Core-Korrektheit

### [x] KR-4611 - SH-4-Kontrollzustand, Delay Slots, RTE, SLEEP und Interrupts

Abhaengigkeiten: KR-4605
Prioritaet: P0

Umfang:

- effektive R0-R7-Bank aus `SR.MD && SR.RB` ableiten
- verzoegerte PR-Semantik fuer BSR, BSRF und JSR modellieren
- RTE als Fortsetzung bei SPC mit restauriertem SR behandeln
- SLEEP bis zu einem akzeptierten Interrupt anhalten
- normale Gast-Exceptions und Interrupts zum Handler dispatchen
- Cause, Eventcode, Vektor, TEA, SPC und Delay-Slot-Zustand gemeinsam ableiten

Akzeptanz:

- User-/Privileged- und RB0-/RB1-Uebergaenge sind bitgenau getestet
- STS/LDS PR im Call-Delay-Slot besitzen unabhaengige Referenzvektoren
- RTE, SLEEP, Maskierung, Interruptannahme und Handler-Rueckkehr laufen
  identisch im Referenz- und generierten Pfad
- keine bestehende korrekte CPU-Regression wird abgeschwaecht

### [x] KR-4612 - Store Queue und Cacheadressierung

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

### [x] KR-4613 - Einheitliche Gastwrites und Codeinvalidierung

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

### [x] KR-4614 - Kontexttreue, sounde Kontrollfluss- und Wertanalyse

Abhaengigkeiten: KR-4611
Prioritaet: P0

Umfang:

- Evidenzklassen `ProvenComplete`, `GuardedComplete`, `GuardedPartial`,
  `ForcedOverride`, `HintCandidate`, `RuntimeOnly` und `Unresolved` einfuehren
- Hint-Direktiven duerfen weder Beweise noch Exportvollstaendigkeit erzeugen
- Overrides als explizit erzwungene, runtimevalidierte Herkunft berichten
- Workitems nach Adresse, Delay-Slot-Owner und Beweisklasse unterscheiden
- Basic-Block-Fallthrough nur bei exakter architekturgemaesser Folgeadresse
  erzeugen
- Delay-Slot-Owner und Slot nur bei `PC + 2` und gegenseitiger Zuordnung paaren
- Vollstaendigkeit pro Site statt als `guarded`-Bit einzelner Kanten modellieren
- unbekannte Caller und unbekannte Pfade konservativ zusammenfuehren
- Zielmengen aller Callkontexte vereinigen
- endliche indirekte Callees nur bei vollstaendigen Summaries zusammenfassen
- Funktionsgrenzen, Shared Blocks und Tail Jumps mit Beweisklasse erhalten
- Decodekandidat und bewiesenen Instruktionsanfang trennen
- dynamische Herkunft ueber begrenzte CFG-/Def-Use-Slices statt eines linearen
  Acht-Befehle-Fensters bestimmen
- Provenienz typisieren und deterministisch erhalten

Akzeptanz:

- ein unbekannter Caller verhindert eine faelschlich vollstaendige Guardmenge
- mehrere Callkontexte vereinigen alle Ziele und Unsicherheiten
- Hintdateien koennen `unresolved` nicht reduzieren
- dieselbe Adresse kann als normale Instruktion und als Delay Slot getrennt
  analysiert werden
- Adressluecken erzeugen keinen Fallthrough
- eine einzelne bewiesene Kante entfernt keinen partiellen Runtime-Default
- kleine synthetische Programme werden gegen exhaustive Ausfuehrung verglichen
- dieselbe Eingabe erzeugt bytegleiche Berichte und Zielmengen

### [x] KR-4615 - Stabile und skalierbare Runtime-Blockregistry

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

### [x] KR-4616 - Einheitliches Gasttiming und Scheduler-/Geraeteintegration

Abhaengigkeiten: KR-4611, KR-4613
Prioritaet: P0

Umfang:

- zentralen versionierten Gastzyklusvertrag definieren
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

### [x] KR-4617 - Unabhaengige Cross-Engine- und CFG-Konformitaetstests

Abhaengigkeiten: KR-4611 bis KR-4616
Prioritaet: P0

Umfang:

- Referenzvektoren unabhaengig von der aktuellen Implementierung ableiten
- Decoder, IR, generierten C++-Pfad und Referenz-/Interpreterpfad vergleichen
- Registerbank, PR-Delay, RTE, SLEEP, Exceptions, SQ, Invalidierung und Timing
  als Pflichtkorpus aufnehmen
- exhaustive kleine CFGs mit Branches, Calls, Delay Slots und Joins erzeugen
- Hint-, Override-, Gap-, partielle Site- und unbekannte Callerfaelle aufnehmen
- Tests von exakten internen Fixpunktiterationszahlen entkoppeln

Akzeptanz:

- Debug und RelWithDebInfo liefern dieselben Gastzustaende
- jede P0-Korrektur besitzt Erfolgs-, Grenz- und Fehlerfall
- kein Test verwendet dieselbe Produktfunktion als Orakel
- Solverwechsel bleiben bei identischem Ergebnis erlaubt
- Terminierung wird ueber ein oberes Budget statt eine exakte Iterationszahl
  geprueft

### [x] KR-4618 - Core-Korrektheitsgate

Abhaengigkeiten: KR-4611 bis KR-4617
Prioritaet: P0

Akzeptanz:

- frischer Debug- und RelWithDebInfo-Build
- ASan/UBSan beziehungsweise MSVC-ASan, statische Analyse und Differentialtests
- vollstaendige bestehende Regression plus neues CFG-Konformitaetskorpus
- null bekannte P0-Semantik- oder Kontrollflusssoundnessfehler
- danach beginnt erst die Performance-Stufe

## Stufe B: P1-Performance und Build

### [x] KR-4621 - Speicher-, Dispatch- und Invalidierungs-Hotpaths

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

Umgesetzt mit abschaltbaren Memory-, Dispatch-, Invalidierungs- und
DMA-Referenzmodi, festen Diagnosekapazitaeten und instrumentierten
Vergleichsregressionen. Vertrag und Messpunkte stehen in `P1_HOTPATHS.md`;
frische Profilzeiten werden gesammelt im Gate KR-4625 erhoben.

### [ ] KR-4622 - Inkrementelle Kontrollflussanalyse, IR und Codegen

Abhaengigkeiten: KR-4618
Prioritaet: P1

Umfang:

- unveraenderliche Instruktionsarena und Block-Spans
- inkrementelle Seed-, CFG- und Def-Use-Worklists statt Ganzprogrammlaeufen
- SCC-basierte interprozedurale Summaries
- Wiederberechnung nur bei geaenderten Ingresswerten
- gemeinsame Instruktions-, Block-, Site-, Edge-, Funktions- und Segmentindizes
- internierte Evidence-IDs statt wachsender Herkunftsstrings
- generalisierte begrenzte Jump-Table-Slices mit Snapshotcache
- Bulk-Codegen und stabile Partitionen
- Codegencache auf SHA-256 und atomaren Publish umstellen

Akzeptanz:

- Resultate bleiben bytegleich zum sicheren Referenzmodus
- Hint-, Guard- und Vollstaendigkeitsstatus bleiben bei Cachehits erhalten
- 10k-, 50k- und 100k-Block-Fixtures besitzen feste Zeit-/Speicherbudgets
- neue Seeds analysieren nur die betroffene Deltafront erneut
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

### [ ] KR-4624 - Buildgraph, Cache und Testmatrix

Abhaengigkeiten: KR-4618
Prioritaet: P1

Umfang:

- MSVC, GCC und Clang in Debug und RelWithDebInfo pruefen
- Core-/CLI-Presets ohne Desktop-GUI als Standard
- Port- und Runtimepaketgrenzen fuer KR-4801 vorbereiten
- Tests nach Subsystem konsolidieren
- CMake-, Package- und ABI-Versionen aus einer kanonischen Quelle erzeugen
- Buildcache und Testshards vermessen

Akzeptanz:

- Releaseoptimierung wird dauerhaft regressionsgeprueft
- Test- und Portbuildzeiten besitzen dokumentierte Baselines
- keine Analyzerquelle wird unbeabsichtigt Runtimepflicht
- KR-4801 kann auf einem klaren Paketvertrag aufbauen

### [ ] KR-4625 - Performance-/Buildgate

Abhaengigkeiten: KR-4621 bis KR-4624
Prioritaet: P1

Akzeptanz:

- alle Korrektheits- und bestehenden Funktionstests bleiben gruen
- Debug- und RelWithDebInfo-Gastresultate sind identisch
- Speicher, Dispatch, Analyse, Codegen, Disc-I/O und Build halten Budgets
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

- `resolved + guarded_complete + guarded_partial + runtime_only + unresolved`
  stimmt mit `indirect_total` ueberein
- jede offene Stelle besitzt genau eine Klasse und eine Beweisherkunft
- partielle Kandidaten koennen Vollstaendigkeit nicht vortaeuschen
- jede Klasse besitzt eine synthetische positive oder negative Regression

### [ ] KR-4716 - ABI-erhaltene Callback-, Parameter- und Stackwerte

Abhaengigkeiten: KR-4715
Prioritaet: P0

Umfang:

- R8 bis R14 kontextsensitiv ueber direkte und bewachte Calls verfolgen
- endliche indirekte Calleemengen mit vollstaendigen Rueckgabesummaries vereinen
- R13-basierte Callbackpfade allgemein modellieren
- feste Stackspills, Reloads und Frame-Offsets in begrenzte Slices aufnehmen
- unbekannte Caller, Rekursion und Aliasverletzungen konservativ behandeln

Akzeptanz:

- direkte und bewachte Calls, mehrere Caller, R13, Stackspills und Rekursion
  sind synthetisch getestet
- nur vollstaendige endliche Mengen werden `guarded_complete`
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
- endliche VTable-Slots werden nur mit vollstaendiger Evidenz bewacht
- der dynamische Runtime-Default bleibt bei partieller Evidenz erhalten

### [ ] KR-4718 - Expliziter Runtime-only-Dispatch

Abhaengigkeiten: KR-4716, KR-4717
Prioritaet: P0

Umfang:

- `runtime-only` als eigene Kontrollflussklasse einfuehren
- nur klassifizierte echte Laufzeitquellen duerfen diesen Status erhalten
- Zieladresse, Instruktionsgrenze, Executable-Image und Blocktabelle validieren
- Blocktabellen-Misses sichtbar stoppen oder kontrolliert fallbacks ausfuehren
- Hits, Misses, Fallbacks und erste Fehler maschinenlesbar zaehlen

Akzeptanz:

- alle indirekten Sites besitzen genau eine Vollstaendigkeitsklasse
- `unresolved == 0` bleibt Voraussetzung fuer Export und Build
- Runtime-only-Misses erzeugen weder Erfolg noch Sonic-Checkpoint
- Hostadressen, No-ops und geratene Ziele sind ausgeschlossen

### [ ] KR-4719 - Privater Retail-Buildnachweis mit erzwungenem Build-only-Modus

Abhaengigkeiten: KR-4718
Prioritaet: P0

Umfang:

- private Sonic-GDI ueber den offiziellen Workflow vollstaendig analysieren
- Portprojekt, generierten Code und `game.exe` ausserhalb des Repos erzeugen
- Harnessconfig `version=2` und `execution_mode=build-only` verlangen
- Prozessstart im Build-only-Modus technisch verbieten
- Manifest, GDI und aktuelles Jobartefakt an dieselbe Identitaet binden
- Identitaet, Abdeckung und Buildresultat nur aggregiert berichten
- Bericht atomar und redigiert schreiben

Akzeptanz:

- `unresolved == 0`
- Analyse, Codegen und Hostbuild enden erfolgreich
- zwei Builds besitzen dieselben portablen Metadaten und generierten Quellen
- das gemeldete Executable stammt aus dem aktuellen Job
- `game_executable_started == false`
- kein Runtimeprozess kann aus diesem Modus erzeugt werden
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

- alle Core-, Kontrollfluss-, Performance- und v0.47-Regressionen bestehen
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
- die Freigabe startet v0.48-Integrationsarbeit
- es erfolgen noch kein Tag, Download oder oeffentlicher Release

---

## v0.48.0 - Port-, Harness-, Controller- und GUI-Integration

### [ ] KR-4801 - Versioniertes Runtime-SDK fuer externe Port-Projekte

Abhaengigkeiten: KR-4705, KR-4624
Prioritaet: P0

Umfang:

- minimales installierbares Runtime-SDK erzeugen
- CMake-Package und `find_package(KatanaRecomp CONFIG REQUIRED)` bereitstellen
- Runtime-, IO- und ABI-Abhaengigkeiten explizit versionieren
- Analyzerquellen aus dem externen Portbuild entfernen

Akzeptanz:

- sauberer Out-of-Tree-Port baut ohne KatanaRecomp-Quellbaum
- inkompatible Runtime-ABI wird vor dem Link sichtbar abgelehnt
- SDK-Inhalt ist reproduzierbar und enthaelt keine privaten Daten

### [ ] KR-4811 - Private Harnessmodi und technisch erzwungener No-run-Vertrag

Abhaengigkeiten: KR-4705
Prioritaet: P0

Umfang:

- Configschema 2 mit `build-only`, `runtime-probe` und `interactive`
- Stufen als getrennte Commands und Prozessrechte modellieren
- Build-only darf keinen Runtimeprozess erzeugen
- `ProcessStartInfo.ArgumentList` statt manueller Quotes
- klare Windows-Prozessbaumbehandlung
- getrennte globale, Build- und Runtimebudgets
- Executable ausschliesslich aus aktuellen Jobartefakten waehlen

Akzeptanz:

- negativer Test beweist, dass Build-only selbst bei vorhandener stale
  `game.exe` keinen Prozess startet
- Manifest, GDI, Jobidentitaet und Executablehash stimmen ueberein
- fehlende oder doppelte Artefakte werden sichtbar abgelehnt

### [ ] KR-4821 - Versionierte Jobtelemetrie und belastbarer Fortschritt

Abhaengigkeiten: KR-4705
Prioritaet: P1

Umfang:

- JobEvent-Vertrag mit stabiler Stage-ID und Vertragsversion
- Einheit, current, total, Rate, ETA und Cachehit getrennt berichten
- Analysezaehler fuer Bytes, Instruktionen, Funktionen und Kontrollflussklassen
- Codegenpartitionen und Compilerarbeitsmengen berichten
- unbekannte Totale ohne erfundenen Prozentwert darstellen
- Ereignisse als Deltas statt Vollsnapshots liefern

Akzeptanz:

- Fortschritt ist monoton und an reale Arbeitsmengen gebunden
- CLI, GUI und Automatisierung sehen dieselben Ereignisse
- Bericht und GUI koennen eine blockierende Siteklasse sichtbar nennen

### [ ] KR-4824 - Unveraenderliche Task-ID-Registry und Roadmaplinter

Abhaengigkeiten: KR-4705
Prioritaet: P1

Umfang:

- `docs/TASK_ID_REGISTRY.md` als kanonische Historie pflegen
- Task-ID, erster Titel, Status und Nachfolger erfassen
- semantische Wiederverwendung und doppelte aktive IDs pruefen
- ROADMAP, TASKS und Registry in CI vergleichen

Akzeptanz:

- KR-4801 bis KR-4805 und KR-4901 besitzen wieder ihre urspruengliche Bedeutung
- die versehentlich neu zugewiesenen Arbeiten besitzen neue IDs
- ein synthetischer Wiederverwendungsfehler laesst den Linter scheitern

### [ ] KR-4802 - Gemeinsamer CLI-/GUI-Portexport und Buildworkflow

Abhaengigkeiten: KR-4801, KR-4821
Prioritaet: P0

Umfang:

- CLI, GUI und Automatisierung verwenden denselben Portplan
- Export-, Configure-, Build- und Artifact-Schritte teilen einen Vertrag
- Buildprofile und Diagnosen sind identisch
- kein GUI-Sonderpfad dupliziert Analyse oder Codegen

Akzeptanz:

- identische Eingabe erzeugt bytegleiche Quellen und Metadaten
- GUI- und CLI-Jobresultate besitzen dieselben Rollen und Hashes

### [ ] KR-4803 - Out-of-Tree-game.exe-Integration

Abhaengigkeiten: KR-4802, KR-4811
Prioritaet: P0

Umfang:

- eigenstaendiges Portprojekt gegen das Runtime-SDK bauen
- Executable und Runtimeabhaengigkeiten aus Jobartefakten veroeffentlichen
- staging, atomaren Publish und stale-Artefaktschutz verwenden
- keine Entwicklerpfade einkompilieren

Akzeptanz:

- verlegtes Portprojekt baut und startet mit frei lizenzierter Quelle
- Harness kann exakt das aktuelle Executable identifizieren
- vorhandene alte Builds koennen nicht als aktueller Erfolg gelten

### [ ] KR-4812 - Strukturierte Runtimeevidenz, Budgets, Replay und Datenschutz

Abhaengigkeiten: KR-4803, KR-4811
Prioritaet: P0

Umfang:

- versionierten JSONL-Ereignisstrom mit Sequenz und Gastzyklus
- genau ein Abschlussmetrikobjekt
- stdout und stderr getrennt und geordnet erfassen
- Host- und Gastframe, Host- und Mapleinput sowie Host- und Gastaudio trennen
- Hosttimeout, Gastzyklus-, Scheduler- und Blockbudget getrennt berichten
- Checkpoints nur aus strukturierten Ereignissen ableiten
- private Berichte atomar, restriktiv und allowlist-redigiert schreiben
- zwei deterministische Probe-Laeufe und normalisierten Vergleich anbieten

Akzeptanz:

- Regex-Fremdtreffer und doppelte Metrikobjekte werden abgelehnt
- `SA_ANALYSIS_CONTINUES` wird nicht aus Zaehlerheuristiken erfunden
- interaktive Runs werden nie als deterministische Evidenz akzeptiert
- kein Bericht enthaelt Rohspeicher, Spielbytes, private Adressen oder Tracknamen

### [ ] KR-4813 - Content-addressed Harness- und Portbuildbeschleunigung

Abhaengigkeiten: KR-4803, KR-4812
Prioritaet: P1

Umfang:

- Eingabesnapshot einmal pro Harnesslauf erfassen und weiterreichen
- Analyse-, Codegen-, Configure- und Compilecache getrennt adressieren
- Tool-, SDK-, Manifest-, Input- und Profilhash einbeziehen
- CMake-/Ninja-Stand nur bei passendem Schluessel wiederverwenden
- Stufenzeiten, Cachehits und Peak-Speicher berichten
- finalen Source-Identity-Check niemals durch Cache ersetzen

Akzeptanz:

- warmer Lauf ist messbar schneller
- Cache an/aus liefert identische Artefakte und Gastergebnisse
- abgebrochene oder parallele Laeufe korrumpieren keinen Cache

### [ ] KR-4814 - Nativer Controller und gastzeitgebundene Maple-Eingabe

Abhaengigkeiten: KR-4803, KR-4616
Prioritaet: P0

Umfang:

- Gast in begrenzten Quanta/Safepoints statt einmal synchron bis zum Ende laufen
- Fenster-, Controller-, Scheduler-, Video- und Audioereignisse verschraenken
- Windows-Gamepadbackend ueber stabile Hostabstraktion bereitstellen
- Keyboardfallback erhalten
- Buttons, Trigger und Analogachsen mit Deadzones normalisieren
- Hotplug, Fokus und Controller-1-Auswahl behandeln
- Hostzustandsaenderungen mit Gastzyklus stempeln
- Maple `GetCondition` liest den letzten zum Transaktionszyklus sichtbaren Zustand
- Replayinput verwendet dieselbe Ereignisschnittstelle

Akzeptanz:

- frei lizenziertes Homebrew reagiert auf Buttons, Trigger und Analogachsen
- aktiv-niedrige Maple-Payloads sind bitgenau
- nur geaenderte Hostzustaende werden eingespeist
- Hotplug und Fokus erzeugen keine haengenden Tasten
- deterministisches Replay ist bytegleich
- keine Eingaberace zwischen Hostpoll und Maple-Read

### [ ] KR-4822 - GUI-Informationsarchitektur und responsives Layout

Abhaengigkeiten: KR-4802, KR-4821
Prioritaet: P1

Umfang:

- sichtbare Navigation fuer Projekt, Quelle, Analyse, Build, Diagnostik,
  Ergebnisse und Einstellungen
- obere Befehlsleiste und klare Start-/Abbruchaktion
- Stage-Timeline und Kennzahlenkarten
- Detailtabs fuer Diagnosen, Log und Artefakte
- responsive Grid-/Anchorstruktur statt fester Contenthoehe
- DPI, High Contrast, Tastatur und Screenreader erhalten

Akzeptanz:

- F6 beziehungsweise Navigation aendert eine sichtbar andere Seite
- 100, 150, 200 und 300 Prozent DPI funktionieren
- kleine Fenster bleiben benutzbar, grosse nutzen zusaetzlichen Raum
- kein relevanter Status existiert nur in einem Fliesstext

### [ ] KR-4823 - Diagnostik-, Ergebnis-, Log- und Workflow-QOL

Abhaengigkeiten: KR-4812, KR-4814, KR-4822
Prioritaet: P1

Umfang:

- eventgetriebene UI-Updates mit zusammengefasster Frequenz
- paginierte/virtuelle Logs, Diagnosen und Artefakte
- Logsuche, Severity-/Stagefilter und Autoscroll-Schalter
- Preflight, Drag-and-drop, letzte Projekte und Buildprofile
- Open Output, Copy Command und Rerun Failed Stage
- redigiertes Diagnosepaket und deterministischen Runvergleich
- Controllerstatus, Mappingtest und Eingabeanzeige
- Dateisystem- und Hasharbeit ausserhalb des UI-Threads
- Mutex nur fuer kurze Metadatenupdates halten

Akzeptanz:

- lange Logs blockieren weder Jobthread noch UI
- Snapshot kopiert keine unbeschraenkten Verlaufsmengen
- alle QOL-Aktionen besitzen Fehler- und Abbruchregressionen
- private Pfade bleiben standardmaessig verborgen

### [ ] KR-4804 - v0.48 Gate-Vorbereitung

Abhaengigkeiten: KR-4801 bis KR-4803, KR-4811 bis KR-4814, KR-4821 bis KR-4824
Prioritaet: P0

Akzeptanz:

- Runtime-SDK-, Port-, Harness-, Controller- und GUI-Regressionen bestehen
- CLI und GUI erzeugen denselben Portworkflow
- Build-only kann technisch keinen Prozess starten
- strukturierte Probe und Replay funktionieren mit frei lizenzierter Quelle
- Controller erreicht Maple end-to-end
- GUI zeigt reale Arbeitsmengen und Kontrollflussstatus
- Task-ID-Linter besteht
- danach fuer Nutzerreview stoppen

### [ ] KR-4805 - v0.48 interne Meilenstein-Freigabe

Abhaengigkeiten: KR-4804

Akzeptanz:

- die unveraenderte Gate-Vorbereitung ist freigegeben
- die Freigabe erlaubt den ersten privaten Sonic-Runtimelauf in v0.49
- es erfolgen noch kein oeffentlicher Release, Tag oder Download

---

## v0.49.0 - Sonic-Alpha-Bring-up und interner Release-Candidate

### [ ] KR-4911 - Runtimebeobachtung, Replay und Fehlerpakete

Abhaengigkeiten: KR-4805
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

### [ ] KR-4912 - Dynamische Codebereiche, Module und Overlays

Abhaengigkeiten: KR-4911
Prioritaet: P0

Umfang:

- nach dem Boot geladene ausfuehrbare Bereiche erkennen
- Discmodule, Overlays und ersetzten RAM-Code allgemein katalogisieren
- bekannte Module vorab rekompilieren oder kontrolliert materialisieren
- Codeinvalidierung, Aliase und Lebenszeit der Bloecke erhalten
- keine versteckte Analyzer-Abhaengigkeit in den Port einfuehren

Akzeptanz:

- synthetische Load-, Relocation-, Austausch- und Unload-Faelle bestehen
- unkompilierte ausfuehrbare Bytes koennen nicht still ausgefuehrt werden
- private Modulinhalte bleiben ausserhalb von Repository und Berichten

### [ ] KR-4913 - CPU-/Plattform-Bring-up bis SA_MAIN_ENTERED

Abhaengigkeiten: KR-4912
Prioritaet: P0

Umfang:

- private Sonic-`game.exe` erstmals im `runtime-probe`-Modus starten
- SH-4-, FPU-, MMU-/Cache-, BIOS-, GD-ROM-, DMA-, Interrupt- und Timingblocker
  titelunabhaengig beheben
- jeden Retailbefund als synthetische oder frei lizenzierte Regression sichern

Akzeptanz:

- zwei identische Probes erreichen `SA_MAIN_ENTERED`
- kein Fallback, Trap oder stiller Fehler wird als Erfolg ausgegeben
- deterministische Kernmetriken stimmen ueberein
- Probe und Bericht enthalten keine Spielbytes oder privaten Adressen

### [ ] KR-4914 - Private interaktive Runtime-Sitzung mit Controller

Abhaengigkeiten: KR-4913, KR-4814
Prioritaet: P0

Umfang:

- separaten `interactive`-Modus mit Fenster, Controller und kontrolliertem Ende
- Hosteventpump und Gastquanta dauerhaft verschraenken
- Pause, Fokus, Controllerhotplug und sauberen Shutdown behandeln
- interaktive Rohdiagnosen ausschliesslich privat speichern
- keine Screenshots, Spielbytes oder Speicherabbilder standardmaessig erfassen

Akzeptanz:

- Nutzer kann einen erreichten Gastpfad mit Controller untersuchen
- interaktive Sitzung wird in keinem Gate als deterministische Evidenz verwendet
- Crash, Close und Timeout hinterlassen keine Kindprozesse oder Schedulerreste

### [ ] KR-4915 - Gast-PVR-Pfad bis SA_FIRST_FRAME

Abhaengigkeiten: KR-4913
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
- `guest_pvr_frames` ist von `host_present_calls` getrennt
- Fehlerpfade bleiben sichtbar und reproduzierbar

### [ ] KR-4916 - Menue, Eingabe und spielbare Szene

Abhaengigkeiten: KR-4914, KR-4915
Prioritaet: P0

Umfang:

- Maple-Eingabe bis in das Gastprogramm verfolgen
- Menue, Discstreaming, Scheduler, DMA und PVR gemeinsam stabilisieren
- `SA_MENU_INTERACTIVE` und anschliessend `SA_ALPHA_PLAYABLE` erreichen
- bekannte Audioabweichungen messen und dokumentieren

Akzeptanz:

- zwei deterministische Probes erreichen dieselbe kontrollierbare Szene
- ein separater interaktiver Lauf ist tatsaechlich steuerbar
- `maple_get_condition_reads` und `guest_observed_input_changes` sind positiv
- Video und Eingabe funktionieren gemeinsam
- Disc-I/O und Scheduler machen weiter Fortschritt
- `silent_failures == 0`

### [ ] KR-4901 - Alpha-CI-Konfiguration fuer Windows und Linux

Abhaengigkeiten: KR-4916
Prioritaet: P0

Umfang:

- Windows Debug, RelWithDebInfo und Paketbuild in CI
- Linux Core/CLI Debug und RelWithDebInfo
- gemeinsame Regression, Fuzz-, Analyse- und deterministische Replayjobs
- private Retaildaten und private Harnessconfigs strikt ausschliessen

Akzeptanz:

- alle oeffentlichen CI-Jobs verwenden nur verteilbare Inputs
- Compiler- und Buildmatrix ist reproduzierbar
- keine Retailquelle, kein Hash und kein privater Bericht gelangt in CI-Artefakte

### [ ] KR-4902 - Reproduzierbare Pakete sowie Daten- und Lizenzaudit

Abhaengigkeiten: KR-4916
Prioritaet: P0

Umfang:

- Windows-Paket und Runtimeabhaengigkeiten
- reproduzierbare Paketmetadaten
- Datenschutz-, Lizenz-, Referenz- und Contentaudit
- First-Run-Diagnostik und bekannte Einschraenkungen

Akzeptanz:

- Paket startet auf einem sauberen Testsystem
- Pakete enthalten keine privaten oder proprietaeren Daten
- jede oeffentliche Faehigkeitsbehauptung besitzt einen Nachweis

### [ ] KR-4903 - Alpha-Checkpoint- und Gate-Automatisierung einfrieren

Abhaengigkeiten: KR-4916, KR-4812
Prioritaet: P0

Umfang:

- strukturierte Checkpoints und Metrikschemas versionieren
- deterministische Doppelprobes automatisieren
- interaktive Sitzungen strikt von Gateevidenz trennen
- Fehler-, Budget- und Redaktionsklassen einfrieren

Akzeptanz:

- Checkpointreihenfolge und Metriken sind schema-validiert
- doppelte, fehlende oder vertauschte Ereignisse scheitern
- Hostsmokes koennen keine Gastcheckpoints vortaeuschen

### [ ] KR-4904 - v0.49 Gate-Vorbereitung

Abhaengigkeiten: KR-4901 bis KR-4903, KR-4911 bis KR-4916
Prioritaet: P0

Akzeptanz:

- frische Debug- und RelWithDebInfo-/Release-Builds bestehen
- zwei private Probes erreichen `SA_ALPHA_PLAYABLE`
- separater interaktiver Lauf ist steuerbar
- Gatebericht, Pakete, Audits und bekannte Einschraenkungen liegen vor
- danach zwingend fuer Nutzerreview stoppen

### [ ] KR-4905 - v0.49 interne Kandidaten-Freigabe

Abhaengigkeiten: KR-4904

Akzeptanz:

- die unveraenderte Gate-Vorbereitung ist ausdruecklich freigegeben
- `KR_V049_ALPHA_CANDIDATE_READY` wird gesetzt
- noch kein oeffentlicher Release, Tag oder Download

---

## v0.50.0 Alpha - Oeffentliches Release

### [ ] KR-4999 - Alpha-Gate-Vorbereitung

Abhaengigkeiten: KR-4905
Prioritaet: P0

Akzeptanz:

- unveraenderte v0.49-Kandidatenbasis besteht frische Builds und Regression
- zwei private deterministische Laeufe erreichen `SA_ALPHA_PLAYABLE`
- eine getrennte interaktive Sitzung bestaetigt Controllerbedienung
- Pakete, Audits, Diagnosevertraege und bekannte Einschraenkungen liegen vor
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
