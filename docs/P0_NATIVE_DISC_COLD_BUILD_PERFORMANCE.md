# P0 NativeDisc-Kaltbuild: Architektur- und Produktplan

Status: Aktiver uebergeordneter Performancevertrag. Die Arbeitsbasis ist
`60638dd71d8a70d70a58aaecb3dbad9ec318bf62` plus der gemeinsame
KR-4985/KR-4986-Bugfix dieses Commits; der Produktgate
bleibt wegen unvollstaendiger D1-Evidenz offen. Der aktuelle enge
Produktblocker ist Candidate-Resolution; sein Detailplan steht in
[`P0_ROOT0_CANDIDATE_RESOLUTION_PERFORMANCE.md`](P0_ROOT0_CANDIDATE_RESOLUTION_PERFORMANCE.md).

KR-4974 bis KR-4980 sind quellseitig weitgehend umgesetzt. Der terminale
Sonic-v56-Diagnoselauf zeigt jedoch, dass der Port noch nicht produktiv
exportiert werden kann. KR-4982 und KR-4983 bleiben gestrichen. KR-4981 ist
bis KR-4993 gesperrt; KR-4992 bleibt ein bedingter Folgezweig nach einem
verfehlten Produktzeitgate.

Der gemeinsame Candidate-Resolution-Explosionsfix erfuellt KR-4985 und
KR-4986 source-seitig. Der einzige freigegebene D1-Lauf erreichte realen
Fortschritt auf zero-based Root 0, aber weder den historischen Root 1 noch
einen vollstaendigen schweren Root. Nach einem Supervisor-I/O-Fehler war die
temporaere JSONL bis `185,586 s` lesbar/gespuelt, aber ohne terminalen
Datensatz und ohne atomare Publikation;
D1/G1 ist daher fail-closed und unentschieden.

## Repositoryweiter Arbeitsvertrag

Jeder Task dieses Kaltbuildpfads folgt exakt:

```text
Task implementieren
  -> alle betroffenen Pfade reviewen
     und bestaetigte Fehler innerhalb dieses Reviews schliessen
  -> den reviewten Task direkt auf main committen und pushen
  -> naechster Task
```

Es gibt keine separate standardmaessige Test-, Verifikations-, Fix- oder
Integrationsphase. Neue Unit-Tests, Regressionstests, Fixtures,
Stresslaeufe, Testprojekte oder Testmatrizen werden nicht gebaut und nicht
als Abschlussbedingung verlangt.

Gefixt wird durch Reviews der betroffenen Pfade. Getestet wird mit dem realen
Sonic-Port an den festgelegten Diagnose- und Produktgates. Vorhandene Tests
duerfen nur repariert werden, wenn sie selbst konkret falsch oder gebrochen
sind; fehlende neue Tests sind kein Finding.

## Ziel

Ein vollstaendiger, wirklich kalter NativeDisc-Port darf auf einem normalen
Entwicklungsrechner kein stundenlanger Vorgang sein. Katana muss:

- die vorhandene CPU parallel und speichersicher nutzen;
- semantisch unveraenderte Arbeit wiederverwenden;
- nur tatsaechlich geaenderte Abhaengigkeiten neu analysieren;
- in jeder langen Phase belastbaren Fortschritt anzeigen;
- vollstaendigen statischen AOT-Code erzeugen;
- innerhalb eines realistischen Produktbudgets bis zum Hostprogramm gelangen.

Das aktuelle Produktziel fuer den frischen 24-Thread-Sonic-Port lautet:

```text
vollstaendiger Kaltport: hoechstens 8 Minuten
```

Dieses Ziel darf nicht durch weniger Funktionen, Bloecke, Resolutionen,
Guarded-AOT-Einstiege, Evidence, Inventory-Sinks oder Gastarbeit erreicht
werden.

## Unveraenderte Korrektheitsgrenzen

- kein Interpreter, JIT oder Emulationsfallback im normalen Produktpfad;
- fehlendes oder unvollstaendiges AOT endet fail-closed;
- Analysebudgets und Truncationflags bleiben autoritativ;
- Cachetreffer gelten nur bei vollstaendig gebundener semantischer Identitaet;
- korrupte, alte oder unvollstaendige Cacheartefakte sind sichere Misses;
- unvollstaendige Analyseepochen werden nie publiziert oder wiederverwendet;
- Stale-Ergebnisse publizieren weder Summary noch Evidence;
- keine Sonic-Adressen oder Retailwerte im generischen Source;
- keine kommerziellen Dateien oder ungeklaert daraus erzeugten
  spielgebundenen Artefakte im Repository oder verteilbaren Paket;
- Performance wird bei gleicher Gastarbeit und gleicher Produktabdeckung
  verglichen.

## Evidenzstand

### Historische Ausgangslage

Aeltere private Exporte belegten mehrfach sehr lange Candidate-Resolution-
Phasen und wiederholte Whole-Graph-Arbeit. Ein abgebrochener Lauf benoetigte
rund 3 Stunden 27 Minuten und erzeugte kein Portartefakt. Ein frueherer
vollstaendiger Kaltport benoetigte 711,2 Sekunden.

Diese Werte bleiben historische Vergleichsevidenz. Sie beschreiben nicht den
aktuellen Source.

### Terminaler v56-Befund

```text
funktionaler Source-Checkpoint:                  a521999
Runtime-ABI / Analyzer-ABI:                      87 / 31
Laufzeit:                                        1:28:24
Exitcode:                                        5
committed Resolution-Roots:                      1 / 1.191
contextual_return_evaluation_limited_functions:  1
Per-Function-Evaluationsbudget:                  65.536, ausgeschoepft
Context-Limit:                                   nicht erreicht
eindeutige Contexts:                             25.728
physische Auswertungen:                          27.872
Eviction-Recomputes:                             0
Retention:                                       incomplete-root
Portartefakt / game.exe / Screenshot:            keines / keine / keiner
```

Diese Rohwerte duerfen noch nicht arithmetisch gekoppelt werden: `65.536`
ist ein Per-Function-Budget, `25.728` Contexts und `27.872` physische
Auswertungen sind laufweite Aggregate. Bis KR-4985 einen gemeinsamen Root-,
Funktions- und Zaehlscope instrumentiert, bleiben daraus abgeleitete
Contextquoten, Cache-Reuse- und Requeuezahlen Hypothesen. Der aktuelle P0
liegt in Candidate-Resolution; seine dominante Kostenklasse entscheidet D1.

## Aktueller Datenfluss

```text
GDI und GameProject
  -> Disc-/Bootidentitaet
  -> Recursive/Local CFG
  -> Function- und Blockinventar
  -> Candidate-Contract- und Function-Value-Fixpunkt
  -> Candidate-Resolution / Guarded-AOT-Inventar
  -> IR-Lowering
  -> Partitionierung und C++-Emission
  -> Hostconfigure, Compile und Link
  -> Produktpaket und lokale Discinstallation
```

Jede Schicht besitzt einen eigenen Korrektheits- und Cachevertrag. Eine
Aenderung in einer spaeten Schicht darf nicht pauschal die gesamte fruehe
Analyse invalidieren, sofern deren semantische Komponentenidentitaet
unveraendert ist.

## Architekturvertraege

### Eingabe und Disc

- GDI, Tracks und GameProject werden einmal geoeffnet und identitaetsgebunden;
- vorhandene Track- und Chunkhashes werden wiederverwendet;
- ein gemeinsamer Streaming-Pass ist einem mehrfachen Vollscan vorzuziehen;
- private Discbytes bleiben ausserhalb von Repository und Paket;
- die Discgroesse allein ist nicht der aktuelle Candidate-Resolution-
  Hauptblocker.

### ProgramGraph, CFG und Function Value

- ProgramGraph, SCC-DAG, Callergraph und ABI-Vertraege bleiben ueber
  Candidate-/CFG-Runden erhalten;
- spaete Seeds invalidieren nur den nachweisbaren Dependency-Closure;
- nicht darstellbare Deltas fallen fail-closed auf die konservative
  Neuberechnung zurueck;
- Summaries und Guarded-AOT-Inventar bleiben vollstaendig und atomar;
- terminaler Budgetverlust wird in den Produktvertrag propagiert.

### Candidate-Resolution

- semantische Contextidentitaet und exakte Provenienz sind getrennt;
- Read-Lens-Projektion ist nur bei vollstaendig bewiesenen Vertraegen
  zulaessig;
- globale und kontextuelle Fallback-Summaries bleiben im Cache-Key;
- Context-, Evaluations-, Inventory- und Retentionbudgets bleiben
  fail-closed;
- unveraenderte semantische Versionen duerfen nicht unbegruendet erneut
  Budget verbrauchen;
- echte Breite-1-Ketten werden als kritischer Span ausgewiesen und nicht mit
  mehr Threads uebertuencht.

### Executor und Speicher

- ein gemeinsamer prozessweiter Executor verhindert verschachtelte
  Threadpool-Explosion;
- Root-, Context-, Latent-AOT- und weitere Analysearbeit teilen ein
  kontrolliertes Ressourcenbudget;
- kurzlebige und retained Speicheranteile werden getrennt betrachtet;
- RAM-Druck darf weder Paging-Sturm noch semantisches Truncation erzeugen;
- Workerzahl ersetzt keine Abhaengigkeitsbreite.

### Persistente Caches

Die Ebenen bleiben getrennt:

```text
Disc-/Imageidentitaet
ProgramGraph und ABI
Function-Value-/Resolution-Shards
IR
C++-Partitionen
Runtime-SDK und Hostobjekte
Whole Export
```

Ein Cache-Key bindet nur die semantisch relevante Komponentenidentitaet der
jeweiligen Ebene. Git-Commit, Targetname oder Ausgabepfad duerfen die teure
Analyse nicht ohne semantischen Grund pauschal invalidieren.

Positive Analysewiederverwendung ist nur bei vollstaendigem, atomarem und
fail-closed gebundenem Artefakt zulaessig. Ein grosser unstrukturierter
`analysis.bin`-Vertrauenssprung ist kein gueltiger Performancefix.

### Latent AOT

- exakte hashgebundene Module werden vor Heuristik behandelt;
- ein vollstaendiges Manifest darf einen ExactOnly-Pfad verwenden;
- positive und negative Modulresultate koennen semantisch gebunden
  wiederverwendet werden;
- bytegleiche Module duerfen mehrere exakte Disc-SourceBindings auf dasselbe
  native Template besitzen;
- Teilmodule und unbekannte Ausfuehrungsbereiche bleiben nicht ausfuehrbar.

### IR, Codegen und Hostbuild

- IR wird nur fuer geaenderte semantische Funktionsshards neu erzeugt;
- Partitionen sind content-adressiert;
- ein installiertes Runtime-SDK verhindert den kompletten Runtime-
  Sourcebuild fuer jeden neuen Ausgabeordner;
- Compiler-Cache und PCH duerfen vorhandene Hostarbeit wiederverwenden;
- Zielname und Packagingdaten gehoeren nicht in den Analysekey;
- statisches AOT bleibt der einzige normale Ausfuehrungspfad.

## Aktiver Taskpfad

```text
KR-4985/KR-4986 source-seitig abgeschlossen
  -> nach diesem Bugfix-Push origin/main synchronisieren
  -> Roadmap vollstaendig neu bewerten
```

Fuer jeden Implementierungstask gilt:

```text
implementieren -> betroffene Pfade reviewen und Findings schliessen -> main
```

D1 und D2 sind reale Sonic-Diagnoseexporte. KR-4981 ist der reale
Produkt- und Integrationstest. Die vorliegende D1-Evidenz ist nichtterminal;
KR-4987 bis KR-4990 bleiben inaktiv, und ein weiterer Folgepfad wird erst
nach Bugfix-Push, origin/main-Synchronisierung und Roadmap-Neubewertung
festgelegt. Es gibt keine begleitende neue Testmatrix.

## Produktmessvertrag KR-4981

Der erste volle Kaltport nach KR-4993 verwendet:

- denselben dokumentierten Host;
- normale 24-Thread-Konfiguration;
- definierten Kaltzustand;
- aktuelle private Originaldisc und aktuelles GameProject;
- vollstaendige Analyse- und AOT-Abdeckung;
- normale Produktkonfiguration ohne Interpreter oder Diagnosefallback.

Zu berichten sind:

| Bereich | Pflichtwerte |
|---|---|
| Eingabe | Disc-/Imagezeit und gelesene Bytes |
| CFG | Runden, Seeds und inkrementelle Wiederverwendung |
| Function Value | Epochen, Contexts, logische/physische Auswertungen, Requeues |
| Resolution | Roots, Head-of-Line, Budgets, Retention und Guarded-AOT |
| Executor | laufende, wartende, idle und speicherblockierte Arbeit |
| Speicher | Lease-Peak und echter Prozesspeak |
| Caches | Hits, Misses, Reuse und Invalidierungsgruende je Ebene |
| IR/Codegen | Funktionen, Bloecke, Partitionen und Zeit |
| Hostbuild | Configure, Compile und Link |
| Packaging | Publish- und Installationszeit |
| Produkt | Exitcode, Gastarbeit, erster Blocker, Frames und sichtbarer Screen |

Ein Erfolg verlangt:

- keinen Context-/Evaluationsbudgetverlust;
- kein `incomplete-root`;
- keine verworfene terminale Analyse-Epoche;
- keine reduzierte AOT-Abdeckung;
- einen vollstaendigen Port;
- normalen Sonic-Lauf und echten Screenshot;
- Zielzeit hoechstens acht Minuten.

Es gibt keinen zweiten Build nur fuer Timing und keine 1-/8-/12-/24-
Threadmatrix.

## Reviews statt neuer Tests

Fuer jeden Task werden die betroffenen Pfade end-to-end reviewt. Typische
Pruefpunkte sind:

- Vollstaendigkeit und Verdrahtung;
- Datenverlust und Teilmutation;
- Cache-Key und Invalidierung;
- Versionierung und ABI;
- AOT-Inventar und statische Emission;
- Register-, Speicher- und Reihenfolgesemantik;
- Stale, Cancellation, Budget und Rollback;
- Rechts- und Inhaltsgrenzen;
- vorhandene gebrochene Tests oder falsche Testzahlen.

Nicht Bestandteil des Reviews sind Forderungen nach neuen Unit-Tests,
Regressionen, Fixtures, Matrizen oder synthetischen Ersatzgates.

## Abschlussdefinition

Der NativeDisc-Kaltbuild-P0 ist erst geschlossen, wenn:

1. Candidate-Resolution ohne Context-/Evaluationslimit und ohne
   `incomplete-root` endet;
2. semantisch unnoetige Contexts und Wiederzulassungen geschlossen sind;
3. ProgramGraph, ABI, Summaries, IR und Partitionen semantisch inkrementell
   wiederverwendet werden;
4. Executor und Speicherhaushalt den real vorhandenen Parallelismus nutzen;
5. Cacheinvalidierung nur semantisch betroffene Ebenen trifft;
6. kein Performancepfad Analyse- oder AOT-Abdeckung reduziert;
7. KR-4993 alle bestaetigten Source-Findings geschlossen und Limit-, Stale-,
   Cancellation- sowie `IncompleteRoot`-Pfade fail-closed gehalten hat; und
8. KR-4981 einen vollstaendigen Sonic-Kaltport in hoechstens acht Minuten
   erzeugt oder einen engeren typisierten Produktblocker belegt.
