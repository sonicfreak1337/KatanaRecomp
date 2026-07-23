# KatanaRecomp Task-Katalog

Dieses Dokument enthaelt nur aktive und zukuenftige Aufgaben. Abgeschlossene
Detailtasks bleiben in Git nachvollziehbar und werden nicht mehr auf mehreren
tausend Zeilen wiederholt.

## Regeln

- Eine Implementierung bearbeitet normalerweise genau eine Task-ID.
- Allgemeine Semantik und verteilbare Regressionen sind Pflicht.
- Private Retaildaten und daraus erzeugte Artefakte bleiben ausserhalb des Repos.
- Private Retaillaeufe sind fuer v0.48 nur als budgetierte lokale Diagnose
  erlaubt; Quell-GDIs werden dabei nie geloescht oder veraendert.
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

v0.48 Native Disc Boot und erster echter Gastframe:
KR-4831 und KR-4841
  -> KR-4843 -> KR-4844 -> KR-4845 -> KR-4846 -> KR-4847
KR-4841 -> KR-4842 -> KR-4911 -> KR-4912
KR-4843 und KR-4912 -> KR-4848 -> KR-4913
KR-4847 und KR-4913 -> KR-4849 -> KR-4915 -> KR-4850
KR-4850 -> KR-4851
KR-4851 -> KR-4852
  -> KR-4853
  -> Nutzerreview
  -> KR-4854

v0.49 Controller-, Port-, Harness-, GUI-Integration und Alpha-Candidate:
KR-4854
  -> KR-4814
  -> KR-4914
KR-4854
  -> KR-4801, KR-4811, KR-4821 und KR-4824
  -> KR-4802
  -> KR-4803
  -> KR-4812 und KR-4813
  -> KR-4822 und KR-4823
KR-4812, KR-4823 und KR-4914
  -> KR-4916
  -> KR-4901, KR-4902 und KR-4903
  -> KR-4904
  -> KR-4905
  -> KR-4999
  -> KR-5000
```

Korrektheit blockiert Performance. Waehrend KR-4841 bis KR-4851 werden
grundsaetzlich nur betroffene Targets und kleine Regressionen ausgefuehrt. Der
First-Frame-/KR-4848-Zwischenblock erhielt zusaetzlich ein vollstaendiges
x64-Auditgate mit 178/178 CTest-Eintraegen; die finale konsolidierte Suite und
der private Retaillauf bleiben Teil von KR-4852. Jeder Prozess besitzt ein
hartes Limit von 15 Minuten. Der moderne Hostcontrollervertrag und die private
interaktive Sitzung (`KR-4814`/`KR-4914`) gehoeren gemeinsam mit GUI, Harness
und Portintegration zu v0.49. Sie beginnen erst nach der internen
v0.48-Freigabe aus `KR-4854`. Der erste echte Gastframe ist durch `KR-4850`
belegt; BootExecutable und Spielboot bleiben das aktuelle P0.

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

### [x] KR-4622 - Inkrementelle Kontrollflussanalyse, IR und Codegen

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

Umgesetzt mit immutable Instruktionsarena und Blockspans, gemeinsamen
Analyseindizes, Baseline-/Delta-Worklists, SCC-geordneter Summarypropagation,
internierter Evidenztabelle, begrenztem Jump-Table-Snapshotcache, stabilen
IR-Hashpartitionen sowie SHA-256-/atomarem Codegencache-Publish. Details und
Gate-Messwerte stehen in `P1_INCREMENTAL_ANALYSIS.md`.

### [x] KR-4623 - Disc-, GDI-, ISO- und GD-ROM-I/O

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

Umgesetzt mit persistenten read-only Trackhandles, Tracknummer-/LBA-Index,
Trackbatches, abschaltbarem begrenztem Sektorcache, ISO-Verzeichnis-/Extentcache
und zwischen Runtime/Portexport wiederverwendeten SHA-256-Provenienzen. Der
GD-ROM-Pfad bleibt ausschliesslich gastzyklusgesteuert. Details und Messpunkte
stehen in `P1_DISC_IO.md`.

### [x] KR-4624 - Buildgraph, Cache und Testmatrix

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

Umgesetzt mit GUI-freiem Core-/CLI-Standard, expliziten MSVC-, GCC- und
Clang-Presets fuer Debug/RelWithDebInfo, einer dauerhaften CI-Matrix,
Compiler-Cachevertrag und gelabelten Testshards. Projekt-, Package- und
ABI-Versionen werden aus `VERSION` und `cmake/KatanaVersions.cmake` generiert.
`runtime-sdk` und `analyzer-sdk` besitzen getrennte Exportziele; ein
Out-of-Tree-Vertragstest stellt sicher, dass die Runtime keine Analyzerquelle
benoetigt. Baseline und Bedienung stehen in `P1_BUILD_GRAPH.md`; die frischen
Messwerte folgen gesammelt im Gate KR-4625.

### [x] KR-4625 - Performance-/Buildgate

Abhaengigkeiten: KR-4621 bis KR-4624
Prioritaet: P1

Akzeptanz:

- alle Korrektheits- und bestehenden Funktionstests bleiben gruen
- Debug- und RelWithDebInfo-Gastresultate sind identisch
- Speicher, Dispatch, Analyse, Codegen, Disc-I/O und Build halten Budgets
- keine Optimierung wird ohne gemessenen Nutzen aktiviert
- danach darf KR-4715 beginnen

Das frische Gate bestand 168 Quality-Debug- und 167 RelWithDebInfo-Tests mit
identischem Inventar aus 167 gemeinsamen
Core-Regressionen. MSVC-ASan, statische Analyse, exakte Referenzvektoren,
Format-, Qualitaetsvertrags- und Referenz-/Lizenzaudit sind gruen. Die
instrumentierten Hotpath-, Deltaanalyse-, 10k/50k/100k-Codegen-, Disc-I/O-
und Paket-/Buildvertraege halten ihre Budgets. Der maschinenlesbare Bericht
wird lokal und durch den Windows-CI-Buildgate als Artifact erzeugt.

Die Gate-Nacharbeit erhaelt Write-only-MMIO im gebuendelten Schreibpfad,
fuehrt das erzeugte Ninja-Projekt mit Runtime-Includes und Buildvertrag frisch
aus und beschraenkt Buildwiederholungen auf klassifizierte
`LNK1104`-/`LNK1168`-Ausgabesperren. Tatsaechliche Versuche, Exitcodes und
Retrygruende werden im JSON-Bericht gespeichert.

## Stufe C: Retail-Kontrollfluss und Build

### [x] KR-4715 - Ungeloeste Kontrollflussfront inventarisieren

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

Umgesetzt mit fuenf disjunkten Berichtszustaenden, sieben typisierten
Herkunftsklassen und getrennten lokalen Detail- beziehungsweise adressfreien
Aggregatberichten. Anwendungsjob, Buildplan und Portmetadaten fuehren
vollstaendige, partielle, Runtime-only- und ungeloeste Stellen getrennt;
partielle Kandidaten blockieren Anwendungs-Vollstaendigkeit. Vertrag und
spaetere Gate-Regressionen stehen in `CONTROL_FLOW_FRONTIER.md`.

Die Review-Nacharbeit bildet einen validierten `HintCandidate` mit erhaltenem
dynamischem Default in der Berichtstaxonomie als `guarded_partial` ab. Der
interne `Unresolved`-Status und die schwache Hint-Evidenz bleiben bestehen;
Hints ohne validiertes Ziel bleiben `unresolved`.

### [x] KR-4716 - ABI-erhaltene Callback-, Parameter- und Stackwerte

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

Umgesetzt mit geschlossenen Caller-Ingressmengen, getrennten
Vollstaendigkeits-/Guardbits und der Vereinigung vollstaendiger indirekter
Callee-/R0-Summaries. R13-Callbacks und R8 bis R14 werden kontextsensitiv
propagiert. Ein auf 4096 Bytes begrenztes symbolisches Stackmodell erhaelt
Longword-Spills ueber feste SP-/Framepointeroffsets; unbekannte Caller,
Aliase und Budgeterschoepfung stufen Ergebnisse konservativ herab. Vertrag
und Gatefaelle stehen in `ABI_VALUE_ANALYSIS.md`.

### [x] KR-4717 - Objekt-, Feld- und VTable-Points-to

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

Umgesetzt mit einer auf 256 Longword-Fakten begrenzten Objektfeldtabelle im
kontextsensitiven Funktionszustand und vollstaendigen Memory-Return-Summaries.
Dominante Initialisierungsstores, Register-/Stackzeiger und feste
Feld-/VTable-Slotloads werden weitergeleitet.
Unbekannte oder ueberlappende Stores, Aliasverletzungen, Calls und sonstige
Speicherseiteneffekte invalidieren konservativ. Beschreibbare statische
VTables bleiben ohne dominierenden Store partiell. Vertrag und Gatefaelle
stehen in `OBJECT_POINTS_TO.md`.

### [x] KR-4718 - Expliziter Runtime-only-Dispatch

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
- Runtime-only-Misses erzeugen weder Erfolg noch nachfolgenden Runtimecheckpoint
- Hostadressen, No-ops und geratene Ziele sind ausgeschlossen

Umgesetzt mit einer expliziten IR-Zielklasse, getrennten bewachten,
Runtime-only- und ungeloesten Codegenpfaden sowie einem validierenden
Blocktabellendispatch. Nur ausgerichtete aktive Blockanfaenge mit gueltiger
Generation und Backendfunktion werden angenommen; Misses brechen sichtbar ab.
Gesamt- und Runtime-only-Zaehler erfassen Hits, Misses, kontrollierte Fallbacks
und den ersten Fehler. Runtime-ABI 12, Backend-Interface-ABI 2 und
Portprojektvertrag 4 versionieren die Erweiterung. Vertrag und Gatefaelle
stehen in `RUNTIME_ONLY_DISPATCH.md`.

### [x] KR-4719 - Privater Retail-Buildnachweis mit erzwungenem Build-only-Modus

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
- kein Checkpoint hoeher als `KR_RETAIL_ANALYSIS_CONTINUES` wird ausgegeben

Umgesetzt mit Configversion 2 und dem einzigen zugelassenen Modus
`build-only`. Der rollenbewusste Prozessstarter weist Runtimeprozesse vor
`Process.Start` ab. Zwei vorher nicht vorhandene Jobziele durchlaufen den
offiziellen Buildworkflow; Manifest-GDI, Jobresultat, Resultindex,
Portmetadaten und aktuelles Executable werden intern an dieselbe Identitaet
gebunden. Portable Metadaten und generierte Quellen muessen bytegleich sein.
Der atomare `katana-private-retail-build`-Bericht enthaelt nur Aggregate und
weist Prozessstart sowie maximale Checkpointstufe explizit aus. Vertrag,
Selbsttest und die bei KR-4704 auszufuehrende private Gatewiederholung stehen
in `PRIVATE_RETAIL_DEBUG.md`.

### [x] KR-4703 - VMU-/Flash-Arbeitskopien und Host-Pacing

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

Umgesetzt mit versionierten, SHA-256-gebundenen Primaer-/Recoverycontainern,
read-only Quellen, projektgebundenem Nutzerdatenpfad sowie persistenten
Flash-/VMU-Geraeten. Hostruntimevertrag 2 verankert einen ganzzahligen Pacer an
Video-Gastzyklen und speichert beide Arbeitskopien beim geordneten Shutdown.
Runtime-ABI 13 und Portprojektvertrag 5 versionieren die Integration. Vertrag
und die bei KR-4704 umzusetzenden Regressionen stehen in
`MUTABLE_STORAGE_AND_PACING.md`.

### [x] KR-4704 - v0.47 Gate-Vorbereitung

Abhaengigkeiten: KR-4703, KR-4715 bis KR-4719, KR-4625
Prioritaet: P0

Akzeptanz:

- alle Core-, Kontrollfluss-, Performance- und v0.47-Regressionen bestehen
- Debug und RelWithDebInfo liefern identische Gastresultate
- `unresolved == 0`
- eine frei lizenzierte Anwendung erreicht `KR_V047_NATIVE_HOST_READY`
- der private Sonic-Workflow wiederholt den Buildnachweis ohne Prozessstart
- keine privaten Retaildaten gelangen in Berichte oder Staging
- die aktuelle GUI wird erfolgreich gebaut, verifiziert und als neue
  Root-Version bereitgestellt
- danach fuer Nutzerreview stoppen

Abschluss:

- Decoder, IR, Backend und Runtime decken die zuvor unbekannten OCBP-/OCBWB-
  Instruktionen mit einem expliziten kohaerenten Cachevertrag ab
- Kontrollflussberichte enthalten Definitionen, Caller-, Owner- und
  Kontextprovenienz sowie getrennte `analysis_candidates`
- private Aggregation: 55.202 Instruktionen, 813 Funktionen,
  `guarded_partial=0`, `unresolved=0`, `unknown_instructions=0`,
  `runtime_only=1.826`, `reachable_abort_edges=0`
- aktuelles Root-GUI-Paket, Dialoghelfer und Runtime-SDK sind gebaut und der
  relocatable synthetische GDI-Portbuild ist verifiziert
- der neue Gatevertrag blockiert unbekannte Speicherbytes nicht allein wegen
  einer ausfuehrbaren Segmentberechtigung; sie bleiben unbekannt und besitzen
  keinen impliziten Dispatchstatus
- alle direkten, indirekten, Return-, Exception- und Interrupttransfers laufen
  durch die zentrale Ziel- und Generationsvalidierung
- der private doppelte Build-only-Nachweis baut zwei frische Hostartefakte mit
  identischen portablen Metadaten und Quellen
- `uncovered_control_targets=0`, `dispatch_paths_without_validation=0` und alle
  bisherigen Kontrollflussblocker sind null
- das aktuelle Hostartefakt ist an Jobidentitaet und neu ermittelten Hash
  gebunden; kein Runtimeprozess wurde gestartet

Umgesetzt als allgemeiner Vertrag mit adressfreiem und lokalem Inventar,
`mixed`-Bootsegmenten, getrennten Precompile-Mengen, versioniertem
Modul-/Overlaykatalog, Byteidentitaetspruefung, budgetierter optionaler
Materialisierung und begrenzten Siteprofilen. Der private reine Analyselauf
klassifiziert 110.404 Bytes als initial erforderlich und statisch vorkompiliert.
16.554 Bytes sind bewiesene
Literalpools. MOVA ist dabei eine Adressreferenz, kein Literalbeweis. 408.019
Padding-Kandidaten und 6.200.319 `unknown_executable`-Bytes bleiben unbekannter
Speicher, blockieren den sicheren Export aber nicht ohne erreichbaren
Kontrolltransfer.
Rollen, Beweisklassen, Dateiprovenienz, Referenzen, Laufzeitwrites,
Relocationen, potenzielle Kontrollflussziele und Klassifikationsgruende stehen
im lokalen Bericht. Der adressfreie Bericht gruppiert 1.603 dominant
unbekannte Seiten in 14 Ursachenklassen nach Quelle, Ladephase,
Schreibbarkeit, Evidenz, Entropie, Decodedichte und Naehe zu bewiesenem Code.
Decode-Dichte allein kann niemals Code beweisen. Vertrag:
`EXECUTABLE_INVENTORY_AND_MODULES.md`.

### [x] KR-4705 - v0.47 interne Freigabe

Abhaengigkeiten: KR-4704

Akzeptanz:

- die unveraenderte Gate-Vorbereitung ist ausdruecklich freigegeben
- die Freigabe startet v0.48-Integrationsarbeit
- es erfolgen noch kein Tag, Download oder oeffentlicher Release

Abgeschlossen: 2026-07-19. Die interne Freigabe startet die v0.48-Entwicklung;
es wurde weder ein Tag noch ein oeffentlicher Release erzeugt.

---

## v0.48.0 - Native Disc Boot und erster echter Gastframe

### [x] KR-4831 - Generischer Originaldisc-Installer ohne Retaildaten im Portpaket

Abhaengigkeiten: KR-4705
Prioritaet: P0

Umfang:

- verteilbaren AOT-Port ohne Raw-, Audio- oder sonstige Retailsektoren erzeugen
- versionierte, spielagnostische Recipe aus Hashes und Trackgeometrie ausgeben
- eigene Original-GDI beim Nutzer vollstaendig und read-only validieren
- lokalen Disc-Cache atomar ausschliesslich unter `user-data/content/` erzeugen
- Repository-, CI-, Release- und Paket-Audits auf `*.katana-disc` beibehalten
- mindestens zwei unterschiedliche PAL-Spiele nur privat als E2E-Fixtures nutzen

Akzeptanz:

- Exportpaket enthaelt null Retailsektoren und keine privaten Quellpfade
- falscher Descriptor, Trackhash, Tracktyp, LBA, Offset oder Sektorformat scheitert
- installierter Cache ist zur validierten Quelle sektorweise identisch
- Original-GDI und Tracks bleiben byteidentisch und werden nie geloescht
- dieselbe Installer-/Runtimeimplementierung gilt ohne Titelspezialfall fuer alle Spiele

Abgeschlossen: 2026-07-20. Portprojekte enthalten nur die versionierte
Installations-Recipe; die vollstaendige Disc bleibt lokal ausserhalb des
Repositorys und wird beim Nutzer read-only validiert. Der Cache entsteht
atomar unter `user-data/content/`, die Quell-GDI und ihre Tracks bleiben
unveraendert erhalten. Synthetische Negativtests und private PAL-Nachweise fuer
mehrere Spiele verwenden denselben titelunabhaengigen Installervertrag.

### [x] KR-4841 - Clean-Room-Referenz- und Nicht-Emulationsvertrag

Abhaengigkeiten: KR-4831
Prioritaet: P0

- Referenzcommits fuer Flycast und XenonRecomp pinnen; dcrecomp ausschliesslich
  als nicht uebernehmbare Warn-/Architekturreferenz dokumentieren
- Flycast nur als Verhaltensvergleich und XenonRecomp nur als
  AOT-Werkzeugklassenvergleich nutzen
- keine Code-, Tabellen- oder Konstantenuebernahme und kein Linking
- bounded Interpreter nur als explizite Bring-up-Diagnose zulassen; der normale
  Produktpfad darf weder Interpreter noch JIT enthalten
- `IP.BIN` und BootExecutable statisch in nativen PC-Code rekompilieren;
  Dreamcast-Komponenten nur als titelunabhaengige typisierte Plattformgrenzen

Abgeschlossen als Architektur- und Provenienzvertrag: 2026-07-22. Flycast
bleibt ein gepinnter reiner
Verhaltensvergleich, XenonRecomp das gepinnte Vorbild fuer die statische
Executable-zu-C++-/Hostcompiler-Werkzeugklasse. Die nicht reproduzierbar
versionierte dcrecomp-Kopie mit enthaltenen GPL-Flycast-Teilen ist
ausdruecklich keine Codequelle. Der Produktpfad rekompiliert `IP.BIN` und
BootExecutable AOT. Das Gate verbietet die Interpretergrenze im normalen
Portlauf; der normale Export emittiert oder linkt sie inzwischen nicht mehr.
Nur `diagnostic_partial` behaelt den begrenzten Diagnoseinterpreter. Die noch
offenen strukturierten Disc-Ladetransaktionen und latenten nativen Module
bleiben ausdruecklich Teil von `KR-4848`.

### [ ] KR-4842 - Seiteneffektfreie Bootdiagnostik und Wait-Loop-Klassifikation

Abhaengigkeiten: KR-4841
Prioritaet: P0

- MMU-bewusste, nicht mutierende Peeks nur fuer lineare Speicherregionen
- MMIO ausschliesslich ueber strukturierte Geraetesnapshots diagnostizieren
- Backedges und Pollingloops samt Wertwechsel und Writer-Provenienz klassifizieren
- Diagnose darf Gastzustand und Ereignisreihenfolge nicht veraendern

Teilstand 2026-07-23: `Memory::peek_u32` erzwingt die lineare Geraetegrenze
selbst. Selbst ein absichtlich in die Whitelist aufgenommenes
`MmioMemoryDevice` wird vor dessen Readhandler abgelehnt; die Regression
belegt null Handleraufrufe. Produktprobes erlauben nur die linearen Backings
von Haupt-RAM, VRAM und AICA-RAM. Flash bleibt ohne eigenen
Side-Effect-Free-Peek-Vertrag ausgeschlossen. Wait-Loop-Erkennung,
Wertwechselfolge, Writer-Provenienz und die Diagnose-an/aus-Invarianz halten
`KR-4842` weiter offen.

### [x] KR-4843 - Alias-korrekter nativer Disc-Systembootstrap

Abhaengigkeiten: KR-4831, KR-4841
Prioritaet: P0

- Bootstrap physisch aus `0x0C008000` binden und bei `0xAC008300` betreten
- virtuellen P2-PC fuer PC-relative Loads und MOVA erhalten
- IP.BIN und Bootdatei als getrennte AOT-Segmente abbilden
- Direct-Boot als expliziten Bypass sowie P1/P2- und Cachevertraege regressionssichern

Abgeschlossen: 2026-07-21. IP.BIN und Bootdatei sind getrennte, physisch
gebundene Segmente; der native HLE-Pfad betritt den P2-Bootstrap bei
`0xAC008300`. Alias-, PC-relativer und Direct-Boot-Vertrag sind fokussiert
regressionsgesichert.

### [x] KR-4844 - Gastzeit, Interruptreihenfolge und vollstaendiger AOT-Chaining-Guard

Abhaengigkeiten: KR-4843
Prioritaet: P0

- pending Interrupts vor dem Block pruefen, Gastwirkung ausfuehren und erst danach
  tatsaechlich retirierte Instruktionen und Schedulerzeit verbuchen
- Faults und Delay Slots nur bis zur tatsaechlichen Ausfuehrungsgrenze zaehlen
- Chaining gegen Codegeneration, FPSCR, Watchpoints und BlockVariantKey absichern
- heissen Uebergang ohne Strings oder `unordered_map` halten; Referenzpfad bewahren

Abgeschlossen: 2026-07-21. Retirierte Instruktionen bestimmen die Gastzeit;
Faults und Delay-Slots werden nur bis zur echten Ausfuehrungsgrenze gezaehlt.
Der native Chainingpfad prueft alle zustandsabhaengigen Generationen und endet
spaetestens am begrenzten Scheduler-Safepoint.

### [x] KR-4845 - BIOS-Lifecycle, HLE-Bridges, Flash, Sysinfo und Region

Abhaengigkeiten: KR-4843, KR-4844
Prioritaet: P0

- SYSTEM `-1`, `1` und `3` als nicht zurueckkehrende Lifecyclegrenzen modellieren
- GD2-Alias `0x8C0010F0` und validierte HLE-Handlerbytes erhalten
- Flash-Readzaehler und read-only Factorypartition erzwingen
- SYSINFO_ICON schreibt 704 Bytes oder meldet ServiceUnavailable
- Disc-Areasymbole vom Konsolenprofil trennen; kein JUE-zu-Europa-Automatismus

Abgeschlossen: 2026-07-22. Menue-/Resetaufrufe sind typisierte, nicht
zurueckkehrende Lifecyclegrenzen; GD2-Alias und HLE-Stubintegritaet sind
gesichert. Factory-Flash bleibt read-only, SYSINFO_ICON behauptet keinen
Scheinerfolg und das Konsolenprofil ist von Disc-Areasymbolen getrennt.

### [x] KR-4846 - GD-ROM-BIOS-Requestqueue, Status und TOC

Abhaengigkeiten: KR-4845
Prioritaet: P0

- Idle-, Queued-, Processing-, Complete-, Streaming- und Error-Zustaende abbilden
- Rueckgaben `-1` bis `4`, Vierwortstatus und uebertragene Bytezahl implementieren
- REQ_CMD, GET_CMD_STAT, EXEC_SERVER, Abort, Callbacks und Transferstatus abdecken
- unbekannte Kommandos kontrolliert ablehnen
- BIOS-TOC als 102 Gastwoerter fuer LOW/HIGH getrennt vom Paket-TOC erzeugen

Abgeschlossen: 2026-07-22. Requestqueue, oeffentliche Zustandsklassen,
Vierwortstatus, Bytezaehler, Abort, Transferstatus und kontrollierte
Fehlerabschluesse sind implementiert. BIOS- und Paket-TOC besitzen getrennte,
regressionsgesicherte Ausgabeformate. Die physische Transferintegration wird
in `KR-4847` weitergefuehrt.

### [ ] KR-4847 - GD-ROM-MMIO, PIO, G1-DMA und Disc-Streaming

Abhaengigkeiten: KR-4846
Prioritaet: P0

- gemeinsamen ATA-/SPI-Kommandozustand fuer BIOS, MMIO, PIO und DMA verwenden
- IRQ-Quittierung, Alternate Status, DRQ, BSY, READY, CHECK und Sense modellieren
- mehrphasige PIO-Daten und gastzeitgebundene G1-DMA-Teilschritte implementieren
- konfigurierte GDSTAR/GDLEN von Livezaehlern GDSTARD/GDLEND trennen
- Abort, Illegal Address, Overrun und Timeout sichtbar behandeln

Teilstand 2026-07-22: Taskfile-Offsets und Command-IRQ-Quittierung,
Command-28-/37-PIO-/DMA-Streaming, gastzeitgebundene 2048-Byte-G1-Chunks,
Livezaehler, Fortschritt/Gesamtstream-Rest, Callback-Handoffs und Abort ohne
spaete Ereignisse sind synthetisch belegt. BIOS und Taskfile besitzen nun einen
gemeinsamen Laufwerksbesitzer. Dreamcast-SPI 11 bis 14 verwenden einen
vollstaendigen 32-Byte-Modepuffer, persistenten CHECK/Sense und phasengetrenntes
PIO-DataIn/DataOut; `REQ_STAT` liefert den bereichsgeprueften 10-Byte-
Laufwerks-/Track-/FAD-Status. `CD_READ` mit `Features.Bit0` fuehrt stattdessen einen
eigenen `DmaIn`-/DMARQ-DMACK-Pfad ohne PIO-DRQ und Zwischen-IRQ aus; partielle
Transfers bleiben `BSY`, erst das letzte DMA-Byte erzeugt den finalen
Status-IRQ. Unbekannte SET-FEATURES-Kombinationen sowie InvalidCommand,
InvalidField, OutOfRange und NoMedia enden kontrolliert. Offen bleiben der
eigenstaendige Dreiwortvertrag der EX-Kommandos 38/39 sowie noch nicht belegte
Timeout-/Overrun-Grenzen. Asynchrone BIOS-Completions verwenden nicht das
quittierbare Taskfile-IRQ-Latch, sodass sequenzielle BIOS-Reads ihre Flanke
behalten; persistenter Sense-Payload und ATA-`ERR` des aktuellen Kommandos sind
getrennt. Der fokussierte GD-ROM-Test besteht nach diesem Integrationsfix 1/1;
asynchrone Read- und TOC-Gastpuffer werden vor dem ersten Write MMU-bewusst als
vollstaendig linear schreibbarer Bereich validiert und bei ungueltigen,
ueberlaufenden oder MMIO-Zielen atomar als `InvalidField` abgelehnt. Die
kombinierte fokussierte Validierung des aktuellen Blocks besteht mit 12
Buildjobs 12/12; EX 38/39 und unabhaengig belegte Timeout-/Overrun-Grenzen
bleiben offen. Die oeffentlich belegten BIOS-Kommandos 29 bis 31 sind nun
ebenfalls geschlossen: `NOP`, `REQ_MODE` und `SET_MODE` verwenden die
gemeinsame asynchrone Requestqueue. Mode-Read und -Write teilen den persistenten
Zustand mit der Paketoberflaeche; Status, Endianness, einmalige Completion und
atomare Ablehnung ungueltiger Zielbereiche sind regressionsgesichert.
Eine nicht mehr darstellbare Schedulerfrist wird inzwischen bereits bei der
Admission geschlossen: PACKET endet mit `READY|ERR`, ABRT-Sense, finalem IRQ
und freigegebenem Owner; BIOS-Read und -Streaming liefern einmalig
`Aborted`, null Bytes und keinen Teilwrite. Regressionen am
`UINT64_MAX-999`-Rand belegen ausserdem, dass ein Folgerequest angenommen
wird. EX 38/39 sowie unabhaengige laufende G1-Overrun-/Timeoutgrenzen halten
`KR-4847` weiter offen.

### [ ] KR-4848 - Runtimecode, Disc-Module, Overlays und latentes AOT

Abhaengigkeiten: KR-4843, KR-4912
Prioritaet: P0

- bekannte Discdateien und Module beim Export analysieren und latentes AOT erzeugen
- Aktivierung nur bei exakter Content- und Byteidentitaet erlauben
- Runtimewrites bytegenau erfassen und Overlays atomar invalidieren
- im normalen Produktpfad fehlendes latentes AOT typisiert abbrechen; unbekannte
  RAM-Bytes nicht ausfuehren
- Interpreteranteil und Herkunft nur im expliziten Diagnosemodus berichten

Teilstand 2026-07-23: Der Modulkatalog verwaltet aktive Byte-Extents ueber
kanonische physische Herkunft. Partielle CPU-/FPU-/Store-Queue-Writes
invalidieren nur ihr Fenster; Copy/DMA loeschen veraltete Runtimeprovenienz und
P0/P1/P2-Aliase bleiben konsistent. Ein kanonischer 4-KiB-Seitenindex weist
Writes ohne aktive Extentueberlappung vor dem Modulkatalogscan ab und misst
Fast-Rejects getrennt von notwendigen Scans. Auch der Ersatz derselben Modul-
ID wird vorvalidiert und atomar angewandt; ein ungueltiger Ersatz laesst laut
Negativregression Katalog, Bloecke, Tracker, Provenienz und Metriken
unveraendert. BIOS-/GD-Reloads werden mit den tatsaechlichen Copy-/DMA-Writes
korreliert: Byteidentische Reloads erhalten gebundene native AOT-Bloecke;
bereits bewiesene Modulabdeckung wird nicht dupliziert und ein frischer
bytegleicher Bereich behaelt seinen ersten Provenienznachweis. Geaenderte Bytes
invalidieren durch den bereits beobachteten Gastwrite exakt einmal. MMU-PIO
publiziert nur die committed physische Range und lehnt nichtlineare TLB-Spannen
vor dem ersten Write ab; `0x0C` bis `0x0F` invalidieren dasselbe Haupt-RAM-
Backing auch ueber eine Spiegelgrenze. Der normale Produktport emittiert und linkt keinen
Interpreter mehr und endet bei fehlendem AOT-Ziel typisiert. Nur
`diagnostic_partial` enthaelt den ausgewiesenen begrenzten
Diagnoseinterpreter. Beschreibbare absolute Pointertabellen aus einem
`EntryPointStraightLineQuiescent`-Anfangssnapshot liefern jetzt endliche
`analysis_candidates`, bleiben am Dispatch aber `RuntimeOnly` mit
`GuardedPartial`-Herkunft. Sie erzeugen weder bewiesene CFG-Kanten noch
Funktionsseeds; ihre akzeptierten Ziele werden nur als Basic-Block-Leader fuer
validierten nativen Eintritt vorbereitet. P1-/P2-Aliase dispatchen dabei
denselben kompilierten Block. Der Snapshotcache ist an das jeweilige Image
gebunden; eine P2-Tabellenadresse wird vor der Analyse auf ihre physische
P1-Herkunft aufgeloest. Offen bleiben strukturierte Disc-
Ladetransaktionen und vorab erzeugte latente native Module; der Task wird
deshalb nicht abgehakt.

Der fuer den privaten Nachweis notwendige Exportpfad baut CFG-, Kanten- und
Writer-Slice-Indizes nicht mehr pro Funktion beziehungsweise Site neu auf.
Single-Block-Partitionen emittieren nur lokale Deklarationen und der Writer
uebernimmt dieselbe konfigurierte Parallelitaet wie der Codegen. Der CLI-
Hostbuild nutzt dynamisch die Host-CPU, akzeptiert
`KATANA_HOST_BUILD_JOBS` und kann unter Windows in einem getrennten Ninja-
Buildverzeichnis laufen; der primaere Rechner verwendet zwoelf Jobs. Windows
hasht die Eingabeprovenienz ueber BCrypt in grossen Chunks.

Der zuletzt installierte private PAL-Port verliess den frueheren `BSRF`-Stopp.
Nach Schliessen der unmittelbar danach belegten GD-ROM-Modekommandos erreichte
der Gast 345.568.225 Zyklen und spaetere PVR-Registerwrites. Der danach
fehlende native Inneneinstieg `0x8C654F5C` gehoert zu einer begrenzten
`MOV.W`-/`BRAF`-Relative16-Tabelle, nicht zu einer weiteren `BSRF`-Insel. Der
statische Audit weist 87 Eintraege, 76 eindeutige Kandidaten und 73 im
vorherigen Port fehlende Ziele nach. Diese Ziele werden nun nur als native
Blockleader vorbereitet; der live geladene Dispatch bleibt `RuntimeOnly`.
Lokale AOT-Blockketten reichen die tatsaechliche Terminatorquelle, Callsite,
Transferart und Siteklasse exakt an den externen Dispatch weiter. Runtime-ABI
37, Backend-Interface-ABI 3 und Portprojektvertrag 23 versionieren diesen
damaligen Vertrag. Der damalige Produktlauf verlaesst `0x8C654F5C` und meldet in 761.011
Dispatchereignissen keinen Fehler. Sein Haupthotspot
`0x8C6658D0 -> 0x8C65247E` ist ein endlicher 4-Byte-Kopier-/
Initialisierungsloop (`r6=4`, Ziel `r14`, Quelle Stack, `r14+=4`) und kein
fehlender Zielblock. Strukturierte Disc-Ladetransaktionen und vorab erzeugte
latente Module bleiben weiterhin offen; der Task wird nicht abgehakt.

Der allgemeine Projektschreiber shardet statische Dispatchregistries nach
maximal 512 Bloecken. Jeder Owner besitzt pro Shard genau einen Wrapper; ein
balancierter Router haelt die zentrale Datei klein. Beim PAL-Port misst
`runtime-dispatch.cpp` 34.879 Byte/607 Zeilen statt zuvor 36.703.886 Byte/
525.996 Zeilen; 43 Shards bleiben bei maximal 393.454 Byte. Eine synthetische
513-Block-Regression erzwingt zwei Shards und entfernt veraltete Sharddateien;
der vollstaendige Ninja-/MSVC-Link besteht in 15 Sekunden. Die sechs
fokussierten Regressionstargets bestehen 6/6.

Der optimierte 12-Job-PAL-Export dauerte 140,5 Sekunden und erzeugte 1.856
Funktionen, 37 Codepartitionen und 43 Shards bei null Retailsektoren im
Portpaket. Die unveraenderte Original-GDI wurde lokal mit drei Tracks und
521.461 Sektoren installiert; die Quelle blieb erhalten, der ersetzte
`gdrom-mode-fix`-Port wurde danach geloescht. Ein 30-Sekunden-Lauf blieb ueber
312.939.023 Zyklen und 1.000.000 Rootdispatches stabil. Bei 100 Millionen
Zyklen laufen `IP.BIN`-AOT und 48.471 native Runtime-only-Treffer ohne Fehler,
Fallback oder Materialisierung; GD-ROM, TA und PVR sind noch null. Bei 320
Millionen Zyklen erreicht der Gast Spielecode, zwei GD-ROM-Kommandos und einen
spaeten PVR-Registerwrite, weiterhin ohne TA-, Render- oder Framebeweis.

Der aktuelle kumulative Schnittstellenstand verwendet Runtime-ABI 39,
Block-ABI 3, Backend-Interface-ABI 3, Portprojektvertrag 24 und
Host-Video-Vertrag 2. Source-relativierte native AOT-Templates und ihr
adressierter Binder sind vorhanden; strukturierte Disc-Ladetransaktionen, der
allgemeine native Materializer und die Registry latenter Module halten
`KR-4848` weiterhin offen. Das fokussierte Gate besteht 11/11, der
vollstaendige x64-Build mit zwoelf parallelen Jobs ist gruen und das
CTest-Zwischengate besteht 178/178 Eintraege in rund 4:04 Minuten.

### [ ] KR-4849 - TA-Eingang und PVR-Kommandopfad

Abhaengigkeiten: KR-4847, KR-4848, KR-4913
Prioritaet: P0

- Store Queue, Channel 2 und PVR-DMA in denselben TA-FIFO fuehren
- normalisierte TA-Paketdiagnostik sowie allgemeine beobachtete Pakettypen abdecken
- List Init, Continue, Completion und STARTRENDER implementieren
- RenderDone nur nach erfolgreicher Verarbeitung melden; Unsupported sichtbar halten

Teilstand 2026-07-22: Der produktive Store-Queue-Pfad fuehrt TA-Pakete in den
gemeinsamen FIFO und schreibt den sichtbaren TA-Positionszeiger fort. Der
SH-4-DMAC-Channel-2-Pfad akzeptiert den realen externen Memory-to-Device-
Request `RS=2` und verlangt 32-Byte-Einheiten, inkrementierende Quelle, festes
Ziel, Burstmodus, `DE` sowie `DMAOR.DME+DDT`; die vier physischen Area-3-RAM-
Spiegel sind abgedeckt. Eine Runtime-End-to-End-
Regression fuehrt Haupt-RAM bis TA-Object-List/EOL und trennt Channel-2-
Completion von PVR-DMA; falsche Richtung und Cycle-Steal scheitern sichtbar.
Fuer Direct-Texture-Ziele `0x11`/`0x13` bleibt die Zielprogression bei
mehrteiligen Transfers als generische P1-Luecke offen. Der
Hintergrundpfad dekodiert Tagadresse, Offset, Skip, Shadowstride, Tiefe und
Render-to-Texture allgemein. Der feste Overscan-Quad beruecksichtigt HScale;
D uebernimmt die Attribute von C und bei Texturierung X/U von B, waehrend die
horizontale Texturerweiterung X und U gemeinsam anpasst. Der zusammenhaengende
Nachweis aller drei TA-Eingaenge und ihrer Completion-/Fehlerreihenfolge bleibt
offen. Der aktuelle private PAL-Lauf erreichte weiterhin keinen TA-Transfer;
SCANINT1 ist dabei kein belegter Fehler. Der weitere Audit prueft allgemeine
PVR-Register-, Timing-, DMA- und Completionvertraege statt Titeladressen.

### [x] KR-4850 - Erster scanoutgebundener Gastframe

Abhaengigkeiten: KR-4915
Prioritaet: P0

- aktiven Scanout und entweder eine validierte TA-Rendergeneration oder einen
  sichtbar geaenderten Direct-Framebuffer-Gastwrite belegen
- Write-Framebuffer, aktiven Read-Framebuffer und ungeblankten Scanout verbinden
- `KR_FIRST_GUEST_FRAME` hostunabhaengig von `KR_FIRST_PRESENTED_FRAME` trennen
- Testframe, vorgefuellter VRAM oder blosser RenderDone-Zaehler gelten nicht

Historischer Teilstand 2026-07-22: Rendergenerationen erfassen final gepackte Pixelwerte und
geaenderte Bytemasken. Erst der echte Scheduler-VBlank-In revalidiert die
aktuellen VRAM-Bytes und friert den exakten Frame ein; ein spaeterer Render
zaehlt erst am folgenden VBlank. PAL/Interlace prueft das aktive SPG-Feld gegen
`FB_R_SOF1/2`, waehrend `SCALER_CTL.Interlace`/`Field Select` auf
`FB_W_SOF1/2` rendern. Offscreen-/RTT-Evidenz kann erst durch einen passenden
Bufferflip sichtbar werden. 256 Generationen, 64 MiB, Range-Fast-Reject und
2.097.152 Pixelpruefungen pro VBlank begrenzen den Nachweis; Scanout-
Skalierungsbomben werden vor der Allokation abgewiesen. Der gemeinsame Proof-
Pump trennt Gastbeweis und Host-Present. Scheduler-VBlank, Proof, Pump und
FakeVideo sind pixelgenau synthetisch verbunden; die vier fokussierten Targets
bestehen 4/4 in 0,66 Sekunden mit 12 Buildjobs. Zu diesem Zeitpunkt blieb der
Task bis zum ersten entsprechenden privaten Retail-Gastframe offen. Die frische Probe verliess die
SCANINT1-Wartestelle sowie die folgenden PR-, `BSRF`- und GD-ROM-Modegrenzen.
Bei 345.568.225 Gastzyklen sind spaetere PVR-Registerwrites beobachtet, aber
weiterhin kein TA-Transfer, Renderrequest oder echter Gastframe; der damalige
Lauf endete typisiert am naechsten fehlenden nativen Inneneinstieg. Dieser ist
inzwischen als Kandidat einer `MOV.W`-/`BRAF`-Relative16-Tabelle vorbereitet
und im frischen privaten Lauf erfolgreich passiert. Der kontrollierte
320-Millionen-Zyklen-Snapshot erreicht zwei GD-ROM-Kommandos und einen spaeten
PVR-Registerwrite, aber weiterhin keinen TA-Transfer, Renderrequest oder echten
Gastframe. Der fuehrende Dispatchhotspot ist ein endlicher 4-Byte-Kopier-/
Initialisierungsloop und kein neuer fehlender Zielblock.

Abschluss 2026-07-23: Der recompilierte Sonic-Adventure-PAL-Discbootstrap
`IP.BIN` beschreibt den sichtbaren Direct-Framebuffer ueber die gemeinsame
logische 32-Bit-VRAM-Abbildung. Backing-Byte-adressierte Dirty-Evidenz plus das
vorherige Scanout-Abbild belegen einen tatsaechlich geaenderten sichtbaren
Pixel. Der budgetierte Lauf erreicht nach 50 Millionen Gastzyklen in 5,3
Sekunden `KR_FIRST_GUEST_FRAME` und danach `KR_FIRST_PRESENTED_FRAME`; TA
bleibt null und der Budget-Exit ist erwartet. BootExecutable, Spielboot,
`KR-4848` und der produktive TA-Pfad bleiben offen.
Nach dem Vollgate wurde der Vertrag-24-Port unter Runtime-ABI 39 und Block-ABI
3 frisch neu exportiert und gebaut: 1.860 Funktionen, 37 Codepartitionen und
null Retailsektoren. Die lokale read-only Originaldisc-Installation umfasst
drei Tracks und 521.461 Sektoren. Der abschliessende 50-Millionen-Lauf
reproduziert beide Marker mit `frames=2`, `pvr_guest_frames=2`,
`pvr_direct_frames=2` und 302.287 geaenderten Direct-FB-Pixeln; TA,
Rendergeneration und Materializer bleiben null.

### [ ] KR-4851 - Boot- und Frame-Hotpath

Abhaengigkeiten: KR-4844, KR-4848, KR-4850
Prioritaet: P0

- numerische Blockhandles, Generationstoken und direkte P1/P2-RAM-Fastpaths nutzen
- Scheduler bis zur naechsten Ereignisgrenze buendeln
- statische Call-/Sprungtabellen und latente Module ohne erneuten Codegen aktivieren
- Fastpath an/aus muss bytegleiche Gastresultate liefern und die Baseline halten

Historischer Teilstand 2026-07-22: Ein budgetierter 50-Millionen-Zyklen-Lauf mit nativem
P1-/P2-Blockchaining endete nach 6,0 Sekunden. Das ist ein Hotpathnachweis, kein
Frame-Gate. `KR-4850` ist inzwischen erfuellt; der Task bleibt fuer den
vollstaendigen Boot-/Frame-Hotpath und den Fastpath-/Referenzvergleich offen.

### [ ] KR-4852 - Konsolidierte v0.48-Validierung

Abhaengigkeiten: KR-4831, KR-4841 bis KR-4851, KR-4911, KR-4912, KR-4913
und KR-4915
Prioritaet: P0

Erst nach vollstaendiger Implementierung der Kernrunde werden einmal gebuendelt
betroffene Targets, fokussierte Regressionen, Gesamtbestand, Sanitizer, frischer
Portexport, lokale Originaldisc-Installation und privater Bootlauf ausgefuehrt.
Danach werden nur konkrete neue Blocker gezielt geprueft. Jeder Einzelprozess
endet spaetestens nach 15 Minuten samt Prozessbaum.

### [ ] KR-4853 - v0.48 Boot-Gate-Vorbereitung

Abhaengigkeiten: KR-4852
Prioritaet: P0

- STATUS, README, ROADMAP, TASKS und CHANGELOG abgleichen
- ABI-/Formatversionen aus dem finalen Diff bestimmen
- nur belegte Testzahlen und keine Retaildaten dokumentieren
- vor Freigabe zwingend fuer Nutzerreview stoppen

### [ ] KR-4854 - v0.48 interne Freigabe

Abhaengigkeiten: KR-4853 und ausdrueckliche Nutzerfreigabe
Prioritaet: P0

- zwei abschliessende private Reproduktionslaeufe ausfuehren
- `v0.48.0` erst nach Nutzerfreigabe intern freigeben und Artefakte abgleichen
- keinen Tag erzeugen; Tags beginnen erst mit der Alpha
- genau einen aktuellen Build und ein aktuelles Backup behalten
- keine Releaseartefakte mit Retaildaten erzeugen

Die bestehenden Bring-up-IDs `KR-4911`, `KR-4912`, `KR-4913` und `KR-4915`
sind dem nativen Boot- und Frame-Meilenstein v0.48 zugeordnet.

### [ ] KR-4911 - Runtimebeobachtung, Replay und Fehlerpakete

Abhaengigkeiten: KR-4831, KR-4842
Meilenstein: v0.48
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
Meilenstein: v0.48
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

### [ ] KR-4913 - CPU-/Plattform-Bring-up bis `KR_GUEST_PROGRAM_ENTERED`

Abhaengigkeiten: KR-4848, KR-4912
Meilenstein: v0.48
Prioritaet: P0

Umfang:

- eine private Retail-Testbench erstmals im `runtime-probe`-Modus starten;
  Sonic Adventure ist dabei eine lokale Testquelle, kein Produktprofil
- SH-4-, FPU-, MMU-/Cache-, BIOS-, GD-ROM-, DMA-, Interrupt- und Timingblocker
  titelunabhaengig beheben
- jeden Retailbefund als synthetische oder frei lizenzierte Regression sichern

Akzeptanz:

- zwei identische Probes erreichen `KR_GUEST_PROGRAM_ENTERED`
- kein Fallback, Trap oder stiller Fehler wird als Erfolg ausgegeben
- deterministische Kernmetriken stimmen ueberein
- Probe und Bericht enthalten keine Spielbytes oder privaten Adressen

### [x] KR-4915 - Gast-PVR-Pfad bis `KR_FIRST_GUEST_FRAME`

Abhaengigkeiten: KR-4849, KR-4913
Meilenstein: v0.48
Prioritaet: P0

Umfang:

- echten Gast-PVR-Pfad ueber TA oder den programmierten Direct-Framebuffer
  verarbeiten
- benoetigte Polygon-, Textur-, Tiefen-, Blend-, Fog- und Listenpfade allgemein
  implementieren
- Hostframe ausschliesslich aus Gastzustand erzeugen
- Frame-, PVR-, DMA- und Interruptfortschritt diagnostizieren

Akzeptanz:

- `KR_FIRST_GUEST_FRAME` wird aus einem Gastframe erreicht
- ein synthetisch vorgefuellter VRAM-Puffer gilt nicht als Nachweis
- `guest_pvr_frames` ist von `host_present_calls` getrennt
- Fehlerpfade bleiben sichtbar und reproduzierbar

Abschluss 2026-07-23: Der legitime Direct-Framebuffer-Pfad des recompilierten
`IP.BIN` erreicht innerhalb von 50 Millionen Gastzyklen in 5,3 Sekunden
hostunabhaengig `KR_FIRST_GUEST_FRAME` und danach
`KR_FIRST_PRESENTED_FRAME`. Der TA-Zaehler bleibt null; damit ist der
Gast-PVR-/Scanoutnachweis erbracht, nicht aber der noch offene produktive
TA-Vertrag aus `KR-4849`. Ebenso bleiben BootExecutable, Spielboot und
`KR-4848` offen.

---

## v0.49.0 - Controller-, Port-, Harness-, GUI-Integration und Alpha-Candidate

### [ ] KR-4814 - Nativer Controller und gastzeitgebundene Maple-Eingabe

Abhaengigkeiten: KR-4854, KR-4616
Meilenstein: v0.49, Implementierung erst nach dem ersten echten Gastframe
Prioritaet: P1 nach KR-4850 (Frame bleibt P0)

Umfang:

- Gast in begrenzten Quanta/Safepoints statt einmal synchron bis zum Ende laufen
- Fenster-, Controller-, Scheduler-, Video- und Audioereignisse verschraenken
- Windows-Gamepadbackend ueber stabile Hostabstraktion bereitstellen
- aktuelle Xbox-Controller, DualSense/DualShock und uebliche Standardgamepads
  ueber denselben geraeteagnostischen Vertrag abdecken
- Keyboardfallback erhalten
- Buttons, Trigger und Analogachsen mit Deadzones normalisieren
- Hotplug, Fokus und Controller-1-Auswahl behandeln
- Hostzustandsaenderungen mit Gastzyklus stempeln
- Maple `GetCondition` liest den letzten zum Transaktionszyklus sichtbaren Zustand
- Replayinput verwendet dieselbe Ereignisschnittstelle

Akzeptanz:

- frei lizenziertes Homebrew reagiert auf Buttons, Trigger und Analogachsen
- Xbox-, DualSense-/DualShock- und Standardcontrollerprofile bestehen denselben
  normalisierten Eingabevertrag ohne titelbezogene Sonderbehandlung
- aktiv-niedrige Maple-Payloads sind bitgenau
- nur geaenderte Hostzustaende werden eingespeist
- Hotplug und Fokus erzeugen keine haengenden Tasten
- deterministisches Replay ist bytegleich
- keine Eingaberace zwischen Hostpoll und Maple-Read

### [ ] KR-4914 - Private interaktive Runtime-Sitzung mit Controller

Abhaengigkeiten: KR-4854, KR-4814
Meilenstein: v0.49
Prioritaet: P1 nach KR-4850 (Frame bleibt P0)

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

Teilstand 2026-07-22: Der CLI-Portexport meldet innerhalb von
`analysis-codegen` datenschutzneutrale Subphasen fuer Disc-Load, Bootimage,
Kontrollflussanalyse, Lowering, Optimierung, Provenienz, Validierung,
Partitionscodegen, Metadaten, Recipe und Writer. Zaehler, Rate, ETA und der
gemeinsame GUI-/JobEvent-Vertrag bleiben offen; der Task ist nicht abgeschlossen.

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

### [ ] KR-4803 - Out-of-Tree-`game.exe`-Integration

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
- `KR_RETAIL_ANALYSIS_CONTINUES` wird nicht aus Zaehlerheuristiken erfunden
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

### [retired] KR-4804 - v0.48 Gate-Vorbereitung: Tests und Build

`superseded_by KR-4853`. Die ID bleibt ausschliesslich als Historieneintrag
erhalten und ist keine aktive Aufgabe.

### [retired] KR-4805 - v0.48 interne Meilenstein-Freigabe

`superseded_by KR-4854`. Die ID bleibt ausschliesslich als Historieneintrag
erhalten und ist keine aktive Aufgabe.

### [ ] KR-4916 - Menue, Eingabe und spielbare Szene

Abhaengigkeiten: KR-4914, KR-4915
Meilenstein: v0.49
Prioritaet: P0

Umfang:

- Maple-Eingabe bis in das Gastprogramm verfolgen
- Gastinput, Discstreaming, Scheduler, DMA und PVR gemeinsam stabilisieren
- `KR_GUEST_INPUT_INTERACTIVE` und anschliessend
  `KR_CONTROLLED_RETAIL_SCENE` erreichen
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

### [ ] KR-4904 - v0.49 Gate-Vorbereitung: Tests und Build

Abhaengigkeiten: KR-4901 bis KR-4903, KR-4911 bis KR-4916
Prioritaet: P0

Akzeptanz:

- frische Debug- und RelWithDebInfo-/Release-Builds bestehen
- private Probes erreichen denselben generischen Runtimecheckpoint; die
  oeffentliche Gateevidenz bleibt synthetisch oder frei lizenziert
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
- private deterministische Testbench-Laeufe erreichen denselben generischen
  Runtimecheckpoint; sie definieren keinen titelbezogenen Produktvertrag
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

## v0.75.0 Beta - Breite Frameworkkompatibilitaet

### [ ] KR-6001 - Langzeit-Retailabdeckung und Save-Kompatibilitaet

Abhaengigkeiten: KR-5000

Umfang:

- lange Lauf-, Streaming-, Modul- und Szenenwechselprofile abdecken
- Save, Laden, Neustart und VMU-Kompatibilitaet stabilisieren
- Last-, Save- und Modulszenarien in einer adressfreien Statusmatrix pflegen

Akzeptanz:

- mindestens ein privates Retail-Testprofil laeuft ueber eine definierte lange
  Sitzung ohne titelbezogene Frameworkausnahme
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
