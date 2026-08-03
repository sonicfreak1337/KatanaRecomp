# P0 Root 0 / Candidate-Resolution: Aufgaben- und Messplan

Status: Nur geplant. Dieser Stand aendert keine Analyse-, Cache-, Context-,
Executor- oder Produktsemantik und fuehrt keinen Build, Test oder privaten
Produktlauf aus.

Reviewbasis:

```text
Repository:                    sonicfreak1337/KatanaRecomp
Branch:                        main
Reviewter HEAD:                a52199996898e2191cdf2f1a5808a7da2b355873
Vergleichscommit:              d3f02208954cee72e8cd00cfdb6205d6f9fa0435
Produkttelemetrie:             v55 / v56
```

Die unabhaengige Pruefung bestaetigt fuer `a521999` keinen neuen P0/P1-
Soundnessfehler. Insbesondere bleibt Cache-Key-Schema 10 mit der globalen
Fallback-Summary korrekt. Der offene P0 ist Performance: Root 0 besitzt einen
nahezu seriellen kritischen Span, waehrend die Kosten je physischer
Auswertung mit Context-Adjazenz, Bindings und AbstractState-Groesse wachsen.

Dieser Plan ergaenzt
[`P0_NATIVE_DISC_COLD_BUILD_PERFORMANCE.md`](P0_NATIVE_DISC_COLD_BUILD_PERFORMANCE.md).
Er ersetzt weder die dortigen Korrektheitsinvarianten noch das finale
`KR-4981`-Produktzeitgate.

## Gemessener Ausgangspunkt

Die fuer v56 berichteten Aggregatwerte lauten:

| Messwert | Wert |
|---|---:|
| verstrichene Walltime am unvollstaendigen `0/1191`-Snapshot | 401 s |
| aufsummierte physische Auswertungszeit | 430,1 s |
| eindeutige Contexts | 9.135 |
| physische Auswertungen | 11.279 |
| Eviction-Recomputes | 0 |
| mittlere effektive Kerne | 1,073 |
| Auswertungen je eindeutigem Context | 1,235 |
| mittlere physische Auswertungszeit | 38,1 ms |

Die `401 s` sind ein Zwischenstand bei weiterhin `0/1191`, keine
abgeschlossene Root-0-Walltime. Sie belegen zusammen mit der aufsummierten
Workerzeit die geringe Parallelitaet, aber noch weder die vollstaendige
Root-0-Groesse noch den spaeten Gesamtverlauf.

Die spaeten beobachteten Auswertungen lagen bei ungefaehr `150 ms`. Damit
sind zwei verschiedene Probleme zu trennen:

1. Die physische Arbeit ist fast seriell. Spaetere Roots bleiben bis zum
   kanonischen Commit von Root 0 gesperrt; innerhalb von Root 0 verarbeitet
   die Jacobi-Welle nur den bereits sichtbaren Frontier und veroeffentlicht
   Folgearbeit erst nach einer globalen Merge-/Evidence-/Commitbarriere.
2. Eine einzelne Auswertung wird teurer. Snapshot, Cache-Key, `apply_call()`,
   Equality/Copy/Merge und Provenienzauswahl skalieren weiterhin mit dem
   angewachsenen Contextual-Graphen.

`1,235` Auswertungen je Context und null Eviction-Recomputes widerlegen einen
weiteren grossen Evaluation-Cache als naechsten Haupthebel. Die noch zu
messende strukturelle Hypothese lautet, dass viele Full-State-Contexts fuer
den konkreten Callee denselben vollstaendig bewiesenen Read-Lens-Zustand
besitzen und nur in irrelevanten Daten oder Provenienz variieren.

## Ziel

Der Aufgabenblock muss beide Multiplikatoren senken:

```text
Anzahl semantisch eindeutiger Contexts
  x
Kosten je Context
  x
verbleibender kritischer Scheduling-Span
```

Der Erfolg wird nicht an einer schoenen CPU-Auslastung gemessen. Normativ
sind weniger Root-0-Walltime, ein vollstaendiger kanonischer Ergebnisdigest,
unveraenderte Soundness- und Budgetflags und schliesslich das bestehende
`KR-4981`-Ziel eines vollstaendigen kalten 24-Thread-Ports in hoechstens acht
Minuten.

## Nicht im Umfang

- kein groesserer oder zusaetzlicher Evaluation-Cache;
- keine Erhoehung der Threadzahl als Performancefix;
- kein vollstaendiges Freigeben aller spaeteren Roots;
- kein Revert der Dependency-Views, Evidence-Referenzen, State-
  Wiederverwendung oder Merge-Fastpaths aus `a521999`;
- kein GPU-Offload und keine Reaktivierung von KR-4982/KR-4983;
- keine neue breite Testsuite, Threadmatrix oder synthetische Ersatzabnahme;
- keine titelbezogene Adresse, kein Retailwert und kein privater Pfad in
  Source, Tests, Telemetrie oder Dokumentation.

## Normative Invarianten

Jeder folgende Task erhaelt diese Invarianten:

1. **Fail-closed Read-Lens.** Nur vollstaendig bewiesene Register-, Stack-
   und Memory-Reads sowie alle output-, alias-, inventory-, evidence- und
   fallbackrelevanten Watcher duerfen projizieren. Jede Luecke waehlt
   `FullState`.
2. **Semantik und Provenienz bleiben getrennt.** Eine physische semantische
   Lane darf mehrere exakte Contributions, Callsites, Callees und Evidence-
   Tokens bedienen. Diese Abonnenten duerfen weder verschmelzen noch verloren
   gehen.
3. **Digest ist kein Beweis.** Content-Digests und Fingerprints dienen als
   Index. Gleichheit verlangt kanonische Identitaet oder strukturelle
   Kollisionspruefung.
4. **Persistente Identitaet bleibt stabil.** Prozesslokale `StateId`,
   `SummaryId` oder `DependencyViewId` duerfen nie allein in persistenten
   oder kanonischen Keys stehen. Dort bleibt ein schema- und contentgebundener
   Digest erforderlich.
5. **Stale Ergebnisse publizieren nichts ungeprueft.** Summary, Evidence und
   kanonische Ausgabe duerfen nur nach Versionsvalidierung committed werden.
6. **Monotonie und Budgets bleiben autoritativ.** Widening, Truncation,
   Cancellation, Memory-Leases und alle vorhandenen Context-/Inventory-
   Budgets bleiben seriell und parallel gleich sowie fail-closed.
7. **Determinismus bleibt beobachtbar.** Normalisierte Summaries,
   Resolutions, Guarded-AOT-Inventar, Evidence und Fehlerflags bleiben fuer
   einen und 24 Worker sowie ueber Wiederholungen identisch. Nur Zeit- und
   Performancezaehler duerfen abweichen.
8. **Cache-Key-Schema 10 bleibt korrekt.** Die globale Fallback-Summary und
   alle relevanten contextual Summaries bleiben bis zu einer explizit
   begruendeten Schemaabloesung vollstaendig gebunden.

## Mess- und Ausfuehrungsvertrag

Dieser Plan autorisiert noch keine Implementierung, keinen Build und keinen
Lauf. Jeder Task benoetigt eine neue ausdrueckliche Nutzeranweisung. Bei
spaeterer Implementierungsfreigabe gilt pro Task unveraendert:

```text
implementieren
  -> nur P0/P1 der betroffenen Pfade reviewen
  -> bestaetigte P0/P1 gebuendelt schliessen
  -> fokussiert verifizieren
  -> einzeln committen und pushen
  -> naechster Task
```

Vor dem finalen Produktgate sind genau zwei begrenzte reale Root-0-
Diagnoseexporte vorgesehen. Jeder benoetigt am erreichten Task erneut eine
ausdrueckliche Nutzerfreigabe:

- **D1 in KR-4985:** Telemetrie-Baseline und Entscheidung von G1;
- **D2 zu Beginn von KR-4991:** nach KR-4986 und allen durch G1 aktivierten
  Tasks KR-4987 bis KR-4990; Nachmessung der verbleibenden Kosten und
  Entscheidung von G2.

Beide enden nach einem vollstaendigen Root 0 oder frueher an den
repositoryweiten Stall-, Nichtkonvergenz- oder 15-Minuten-Grenzen. Sie sind
keine Produktnachweise und warten nicht mehrere Stunden auf `1191/1191`.
Zwischen D1, D2 und KR-4993 gibt es keinen weiteren privaten Sonic- oder
vollstaendigen Produktlauf. Der erste vollstaendige Produktmesspunkt bleibt
KR-4981 nach KR-4993.

Vorher/Nachher-Werte muessen denselben Host, Compiler, Buildmodus,
24-Thread-Haushalt, Eingabeumfang und definierten Kaltzustand verwenden.
Wallzeit einer Welle und aufsummierte Workerzeit werden getrennt berichtet.

## Messgate G1

KR-4985 entscheidet anhand des realen Root-0-Profils, welche Hypothesen
tatsaechlich tragen:

- **Read-Lens-Projektion ist positiv**, wenn die Zahl verschiedener
  `ProjectedLensDigest` gegenueber `FullStateDigest` um mindestens 30 Prozent
  sinkt oder ein zeitgewichtetes Modell mindestens 20 Prozent Root-0-Gewinn
  belegt. Eine Reduktion um Faktor zwei oder mehr ist ein starkes Signal.
- Jeder Wert unterhalb dieses positiven Gates gilt als negativ oder
  unzureichend belegt und aktiviert KR-4987 nicht. Weniger als zehn Prozent
  Potenzial oder ueberwiegender `FullState`-Fallback sind ein besonders
  starkes Negativsignal.
- **KR-4988 ist positiv**, wenn Equality-, Copy-, Merge- oder Keyarbeit
  mindestens zehn Prozent der gemessenen Root-0-Arbeit ausmacht oder ein
  zeitgewichtetes Modell mindestens zehn Prozent Root-0-Walltimegewinn durch
  Interning belegt.
- **KR-4989 ist positiv**, wenn Binding-Scan und zugehoerige Equality-
  beziehungsweise Mergearbeit dieselbe Zehn-Prozent-Schwelle erreichen und
  zugleich wachsende Binding-p95-Werte oder spaete Exact-Hit-Indizes
  vorliegen.
- **KR-4990 ist positiv**, wenn Snapshot-/Key-Aufbau dieselbe Zehn-Prozent-
  Schwelle erreicht und im Median mindestens 50 Prozent der Edge-, Binding-
  und Evidence-View-Eintraege zwischen Reevaluations unveraendert bleiben.
  Andernfalls bleibt der Full-Rebuild autoritativ.

`Root-0-Arbeit` meint fuer diese Gates entweder gemessene Phasenwalltime ohne
Doppelzaehlung oder, bei ueberlappender Workerarbeit, ein explizit
zeitgewichtetes Modell. Alle nicht positiv belegten P1-Pfade werden als
begruendeter Skip geschlossen; sie blockieren die Folgetasks nicht.

Die Schwellen sind Stop/Go-Grenzen fuer riskante Umbauten, keine
Produktabnahme. Eine semantische Abweichung stoppt jeden Performancepfad
unabhaengig vom gemessenen Gewinn.

## Messgate G2

Die versionierte Worklist aus KR-4991 wird erst nach D2 aktiviert, wenn die
Telemetrie nach den Context- und Kostenumbauten noch freisetzbare Arbeit
hinter der Jacobi-Barriere zeigt. Eine echte Breite-1-Kausalkette ist ein
negatives Gate: Ein anderer Scheduler kann sie nicht parallelisieren.

Ein positives Gate verlangt mindestens eines der folgenden Signale:

- mindestens zehn Prozent der D2-Walltime entfallen auf Barriere oder
  serielle Veroeffentlichung, obwohl dependency-gueltige Folgearbeit bereit
  waere; oder
- ein zeitgewichtetes D2-Modell belegt mindestens zehn Prozent Root-0-
  Walltimepotenzial durch sofortiges Einreihen bereits freigesetzter Arbeit.

Neu aktivierte Lanes, `idle-with-ready-work` und noch laufende unabhaengige
Jobs muessen dieses Potenzial kausal belegen. Jeder andere Befund ist negativ
oder unzureichend; KR-4991 behaelt dann Jacobi und dokumentiert den Skip.

## Aufgaben

### KR-4985 - Root-0-Phasen- und Kardinalitaetstelemetrie

Prioritaet: P0 Performance-Diagnose

Abhaengigkeiten: a521999, KR-4974

Ziel: Jede wachsende Kostenklasse und die moegliche Context-Kollapsrate im
realen Root-0-Pfad eindeutig messen, ohne Semantik oder Scheduling zu
veraendern.

Umfang:

- pro Contextual-Welle `wave_index`, `pending_before`,
  `stale_entries_removed`, `selected_width`, `new_lanes`, `widened_lanes`
  und `requeued_lanes` erfassen;
- `snapshot_prepare_ns`, `cache_key_build_ns`, `physical_evaluation_ns`,
  `merge_ns`, `evidence_publish_ns` und `lane_commit_ns` ausweisen; bei
  ueberlappender Arbeit jeweils Wellenwallzeit und aufsummierte Workerzeit
  explizit getrennt halten;
- `outgoing_edges`, `edge_lanes`, `called_dependencies`,
  `contextual_bindings`, `global_bindings`, `state_memory_values`,
  `state_stack_values`, `state_alias_entries` und `cache_key_bytes` erfassen;
- `apply_call()` aggregiert mit `calls`, `bindings_scanned`,
  `exact_state_hits`, `exact_hit_average_index`, `merge_attempts`,
  `merge_changes`, `full_binding_misses`, `state_equality_ns`,
  `state_copy_ns` und `state_merge_ns` erfassen;
- pro Root/Funktion/Lens ausschliesslich aggregierte Anzahlen verschiedener
  `FullStateDigest`, `ProjectedLensDigest` und `ProvenanceDigest` sowie
  `FullState`-Fallbacks ausgeben;
- Hotpathzaehler threadlokal sammeln und begrenzt je Welle oder Root mergen;
  keine rohen States, Gastwerte oder ungebundenen per-Lane-Logs erzeugen.

Akzeptanz:

- bestehender Progress-/JSONL-Pfad transportiert die neuen Felder mit
  expliziter Vollstaendigkeit und Dropdiagnose;
- Zaehlerinvarianten sind in den vorhandenen fokussierten Progress- und
  Function-Value-Vertraegen abgedeckt;
- ein spaeter ausdruecklich freigegebener realer Root-0-Messlauf kann die
  spaeten ungefaehr 150 ms eindeutig Snapshot, Key, Apply/Merge oder
  Kernauswertung zuordnen;
- Telemetrie an/aus veraendert keine kanonische Semantik; das angestrebte
  Laufzeitbudget fuer die Beobachtung betraegt hoechstens fuenf Prozent.

Codeanker: `FunctionValueAnalysisProgress`,
`emit_progress_snapshot_locked()`, `capture_contextual_snapshot()`,
`make_function_evaluation_cache_key()`, `apply_call()`,
`StructuredControlFlowProgress`, `ProgressCounterSnapshot` und der bestehende
Portbuild-JSONL-Recorder.

### KR-4986 - Semantische Context-Lanes und exakte Provenienzabonnenten

Prioritaet: P0 Korrektheits-Enabler

Abhaengigkeit: KR-4985

Ziel: Physische Fixpunktarbeit und exakte Contribution-/Evidence-Provenienz
zunaechst mit unveraenderter Full-State-Identitaet sauber trennen.

Umfang:

- `ContextualLane` in eine semantische Lane und eine kanonische Menge exakter
  Provenienzabonnenten teilen;
- als erstes Backend den bisherigen Full-State-Key verwenden;
- stabile Lane-Identitaet aus Funktion, Evaluation-Lens, Contract-Fingerprint
  und Ingress-Content bilden;
- Summary-/Dependency-Versionen fuer Snapshot- und Stale-Validierung statt
  als Quelle immer neuer Lane-Identitaeten verwenden;
- Contributions, Callsites, Callees, isolierte/forwarded Korrelation und
  Evidence-Tokens separat und exakt zurueckspielen.

Akzeptanz:

- Altpfad und neuer Full-State-Pfad liefern bytegleich normalisierte
  Summaries, Resolutions, Guarded Inventory, Evidence und Budgetflags;
- Accounting trennt semantisch eindeutige Lanes, Reuse und
  Provenienzabonnenten ohne Doppelzaehlung;
- ein und 24 Worker sowie Wiederholungen bleiben deterministisch;
- keine Read-Lens-Projektion wird aktiviert, bevor diese Paritaet und die
  exakte Evidence-Wiederherstellung belegt sind.

Codeanker: `ContextualLane`, `contextual_entry_families`,
`EvidenceProvenanceLens`, `make_multi_root_provenance_lens()` und
`MultiRootEvaluationCoordinator`.

### KR-4987 - Read-Lens-projizierte Context-Identitaet

Prioritaet: P0 Performance

Abhaengigkeiten: KR-4985 mit positivem G1, KR-4986

Ziel: Semantisch gleiche Callee-Eingaenge einmal auswerten, auch wenn ihr
voller State oder ihre Provenienz in fuer den Callee irrelevanten Daten
abweicht.

Umfang:

- `SemanticContextKey` aus Funktion, Evaluation-Lens, versioniertem
  Contract-Fingerprint und projiziertem Ingress-Content bilden;
- vollstaendig bewiesene Register-, Stack- und Memory-Reads samt allen
  relevanten Watchern abbilden;
- bei jeder unvollstaendigen Register-, Stack-, Memory-, Alias-, Inventory-
  oder Fallbackdomaene `FullState` verwenden;
- exakte Subscriber-Inputs behalten und bei erweitertem Read-Vertrag neu
  projizieren, rebucketen und betroffene Lanes neu auswerten;
- Digestkollisionen strukturell pruefen; persistente Keys nur mit stabilem
  Content-Digest bilden.

Akzeptanz:

- Unterschiede nur in ungelesenem State oder Provenienz teilen eine
  physische Lane; gelesene Unterschiede bleiben getrennt;
- unvollstaendiger oder spaet erweiterter Read-Vertrag faellt sicher auf
  FullState zurueck beziehungsweise rekeyt alle betroffenen Abonnenten;
- exakte Evidence und alle Completeness-/Truncationflags bleiben erhalten;
- der fokussierte Vergleich behaelt den kanonischen Ergebnisdigest; D2 weist
  Contextzahl und reale Gesamtwirkung gemeinsam mit den weiteren aktivierten
  Kostenpfaden aus.

Codeanker: `FunctionEvaluationProjection`, `project_evaluation_ingress()`,
`make_function_evaluation_projection()`, `ForwardedRegisterReadMap`,
`AbiStackArgumentReadMap` und die Memory-Read-Vertraege der
`FunctionValueSummary`.

### KR-4988 - Internierte AbstractStates und Function-Value-Summaries

Prioritaet: P1 Performance

Abhaengigkeiten: KR-4985 mit positivem KR-4988-Gate, KR-4986

Status: Bedingt geplant. Unterhalb der in G1 definierten Zehn-Prozent-
Schwelle wird der Task als gemessener Skip geschlossen.

Ziel: Wiederholte tiefe Equality-, Copy- und Keyarbeit an unveraenderlichen
Evaluationgrenzen durch kanonische Identitaeten und strukturelle
Wiederverwendung ersetzen.

Umfang:

- nur immutable Snapshots an klaren Besitzgrenzen als `StateId` und
  `SummaryId` internieren; mutable Fixpunktzustande nicht in-place internieren;
- Content-Hash mit struktureller Kollisionspruefung verwenden;
- prozesslokale O(1)-Equality-Fastpaths von stabilen persistenten
  Content-Digests trennen;
- Lebenszeit, RAM-Budget, Dedup-Hits, retained/reclaimed Bytes und
  Kollisionspfad sichtbar machen.

Akzeptanz:

- Interning an/aus liefert dieselbe normalisierte Semantik;
- kanonische Gleichheit ist O(1), nichtkanonische oder kollidierende Werte
  werden strukturell geprueft;
- keine ungebundene Retention und keine relevante Working-Set-Regression;
- Equality-/Copyzeit oder Cache-Key-Bytes sinken in der fokussierten
  Verifikation; D2 weist die reale Gesamtwirkung gemeinsam aus.

Codeanker: `AbstractState`, `FunctionValueSummary`,
`FunctionValueAnalysisSession::Impl`, `EvaluationKeyEncoder`,
`canonical_abstract_state_key()` und `FunctionEvaluationCache`.

### KR-4989 - Indexierte exakte Context-Bindings

Prioritaet: P1 Performance

Abhaengigkeiten: KR-4985 mit positivem KR-4989-Gate, KR-4986; KR-4988 nur,
wenn dessen eigenes Gate positiv war

Status: Bedingt geplant. Bei uebersprungenem KR-4988 darf ein stabiler,
kollisionsgepruefter State-Fingerprint den Index tragen.

Ziel: Vorhandene exakte Binding-Treffer nicht mehr linear am Anfang, in der
Mitte oder am Ende grosser Binding-Familien suchen.

Umfang:

- je Callsite-/Summary-Binding-Familie `StateId` oder stabilen
  State-Fingerprint auf kanonisch sortierte Binding-Indizes abbilden;
- Kandidaten nach dem Indexlookup vollstaendig auf Gleichheit pruefen;
- bei Nichttreffer den bestehenden linearen Join-/Subsumptionpfad
  unveraendert verwenden;
- Index bei Insert, Widening und Generationwechsel atomar konsistent halten;
- kleine Familien unter einer telemetriebegruendeten Schwelle ungeindext
  lassen, wenn Indexpflege teurer ist.

Akzeptanz:

- Exact-Hits an jeder Position, Fingerprintkollision, Widening,
  Invalidierung und Full-Miss bleiben korrekt;
- Merge- und Subsumptionssemantik ist unveraendert;
- `bindings_scanned`, Exact-Hit-Index und Equality-/Mergezeit sinken in den
  in KR-4985 belegten grossen Familien; D2 weist die reale Gesamtwirkung aus.

Codeanker: `ContextualSummaryBindings`,
`ContextualSummaryBindingViews` und der Binding-Scan in `apply_call()`.

### KR-4990 - Inkrementelle Contextual-Dependency-Views

Prioritaet: P1 Performance

Abhaengigkeiten: KR-4985 mit positivem KR-4990-Gate, KR-4986; KR-4988 und
KR-4989 nur, wenn ihre eigenen Gates positiv waren

Status: Bedingt geplant. Bei weniger als 50 Prozent median unveraenderten
View-Eintraegen oder unterhalb der Zehn-Prozent-Kostenschwelle bleibt der
Full-Rebuild erhalten.

Ziel: Unveraenderte Context-Adjazenz, Bindings, Summary-Versionen und
Evidence-Layouts nicht fuer jede Reevaluation vollstaendig neu aufbauen und
serialisieren.

Umfang:

- einen unveraenderlichen `ContextualFunctionDependencyView` nach Funktion,
  Caller-/Lane-Scope, lokaler Edge-Generation, Binding-Generation und
  Summary-Generation-Aggregat halten;
- nur geaenderte Edge-/Binding-Shards ersetzen;
- Dependency-Digest inkrementell, aber ordnungsstabil und kanonisch
  aktualisieren;
- Evidence-Layout nur bei struktureller Aenderung neu bilden;
- Full-Rebuild als fail-closed Fallback bei unbekannter Invalidierung oder
  hoher Aenderungsrate erhalten;
- Referenzen nur innerhalb der schreibfreien Phase halten und vor dem
  Besitzuebergang materialisieren.

Akzeptanz:

- ein vorhandener Debug-Oracle-Vergleich kann inkrementellen und voll
  aufgebauten View fuer Bindings, Versionen, Digest und Evidence-Layout
  gleichsetzen;
- Edge-Add/Widen, Summary-only-Aenderung, stale Entfernung und Zyklen
  invalidieren exakt die betroffenen Shards;
- globale und alle relevanten contextual Fallbacks bleiben im Key;
- Snapshot-/Key-Zeit und Key-Bytes sinken in der fokussierten Verifikation
  ohne falschen Cachehit; D2 weist die reale Gesamtwirkung aus.

Codeanker: `capture_contextual_snapshot()`, `contextual_callees`,
`contextual_callers`, `ContextualDependencyVersion` und die vorhandenen
immutable Resolution-Dependency-Shards der `FunctionAnalysisEpoch` als
Strukturvorbild.

### KR-4991 - Versionierte monotone Context-Worklist

Prioritaet: P0 Performance/Scheduling

Abhaengigkeiten: KR-4986; alle in G1 aktivierten Tasks KR-4987 bis KR-4990;
separat freigegebener D2-Diagnoseexport und positives G2

Status: Bedingt geplant. KR-4991 beginnt mit D2. Bei negativem oder
unzureichendem G2 bleibt Jacobi autoritativ und der Task endet als
dokumentierter Skip ohne Schedulerumbau.

Ziel: Neu freigesetzte unabhaengige Contextarbeit ohne globale
Jacobi-Wellenbarriere sofort ausfuehrbar machen, ohne die kanonische
Veroeffentlichung oder Soundness aufzugeben.

Umfang:

- Worker lesen unveraenderliche Lane-Snapshots samt Dependency-Versionen und
  liefern ausschliesslich private semantische Deltas;
- der Commit validiert Versionen, merged aktuelle Deltas in stabiler
  ID-Reihenfolge und reiht neue Lanes sofort ein;
- stale Summary-/Evidence-Ergebnisse verwerfen und gezielt neu planen;
- stale physische Versuche nur gegen ein separates, begrenztes Retry- und
  Ressourcenbudget zaehlen; die vorhandenen semantischen Context-/Inventory-
  Budgets zaehlen ausschliesslich akzeptierte logische beziehungsweise
  kanonische Arbeit deterministisch;
- bei erschoepftem Retrybudget asynchrone Retries abbrechen und die Lane auf
  den konservativen Jacobi-/seriellen Pfad zurueckstellen, ohne dadurch
  semantische Truncation- oder Completenessflags zu setzen;
- Evidence separat kanonisch normalisieren; Cancellation, Memory-Leases und
  terminale Fehlerreihenfolge erhalten;
- den Jacobi-Pfad bis zur Abschlusspruefung als begrenzten Referenz- und
  Rollbackpfad behalten.

Akzeptanz:

- Widening, SCC-Zyklus, kuenstlich verzoegerter Worker, stale Ergebnis,
  Cancellation und Budgetende liefern dieselbe normalisierte Semantik wie
  Jacobi;
- keine stale Summary oder Evidence kann committed werden;
- ein, zwei und 24 Worker sowie Wiederholungen bleiben deterministisch;
- fokussierte deterministische Scheduling-Traces belegen weniger
  Barrier-Walltime und `idle-with-ready-work`; die reale Produktwirkung wird
  erst in KR-4981 gemessen und eine echte Breite-1-Kette erzeugt keine
  falsche Parallelitaetsbehauptung;
- Retrybudget-Erschoepfung liefert per Jacobi-/Seriell-Fallback dieselbe
  Semantik und dieselben semantischen Budgetflags wie der Referenzpfad.

Codeanker: `select_contextual_batch()`, die Prepare-/Evaluate-/Commitphasen
in `harvest_contextual_candidate_returns()`, vorhandene Lane-Versionen und
`ParallelWorkExecutor::submit_once()`.

### KR-4992 - Begrenzte Spekulation spaeterer Resolution-Roots

Prioritaet: bedingtes P1

Abhaengigkeiten: ein verfehlter erster KR-4981-Lauf nach KR-4993, akzeptable
Root-0-Zeit und positives Restkosten-/RAM-Gate

Ziel: Erst nach Schliessung des Root-0-Pfads ungenutzte Kerne begrenzt fuer
spaetere Roots verwenden und dadurch ausschliesslich die verbleibende
Gesamtzeit senken.

Aktivierungsgate nach dem ersten KR-4981-Lauf:

- Root-0-Walltime und Soundness sind akzeptabel, der vollstaendige Port
  verfehlt aber weiterhin das Acht-Minuten-Ziel;
- spaetere Rootarbeit liegt nach Root 0 auf dem belegten Restpfad und idle
  Ressourcen stehen waehrenddessen zur Verfuegung;
- RAM-, Cache- und Eviction-Telemetrie belegt eine sichere getrennte Reserve.

Umfang:

- harte Root-0-Worker-, RAM- und Cache-Reserve;
- kleine konfigurierbare Spekulationsquote, beispielsweise anfangs 20
  reservierte und hoechstens vier spekulative Worker;
- keine kanonische Publikation spaeterer Roots vor Root 0;
- Resultate versionieren, bei Stale verwerfen und Throwaway-/Evictionkosten
  getrennt ausweisen.

Akzeptanz:

- Root-0-Walltime verschlechtert sich nicht relevant; Richtwert hoechstens
  drei Prozent;
- Gesamtzeit sinkt, Speichergrenzen bleiben eingehalten und
  Eviction-Recomputes bleiben null;
- Rootreihenfolge und kanonischer Output sind identisch;
- bei Root-0-Verlangsamung, Cacheverdraengung oder hohem Throwaway-Anteil
  wird die Spekulation deaktiviert und der negative Befund dokumentiert.

KR-4992 blockiert weder den ersten KR-4993-Abschluss noch den ersten
KR-4981-Lauf. Wird er nach einem verfehlten KR-4981 aktiviert, folgt vor
einem separat autorisierten KR-4981-Wiederholungslauf erneut KR-4993.

Codeanker: `canonical_root_committed`,
`initial_root_admission_available_locked()`, `ResolutionDispatchState` und
der vorhandene globale RAM-/Ready-Haushalt.

### KR-4993 - Unabhaengige Root-0-P0/P1-Abschlusspruefung

Prioritaet: P0, letzter Gate-Vorbereitungstask

Abhaengigkeiten: KR-4985, KR-4986 und alle durch G1/G2 tatsaechlich
aktivierten Tasks bis KR-4991; begruendete negative Gates blockieren nicht

Ziel: Den gesamten geaenderten Context-, Cache-, Dependency-, Evidence- und
Schedulingpfad unabhaengig reviewen, alle bestaetigten P0/P1 schliessen und
erst danach KR-4981 freigeben.

Umfang:

- End-to-End-Review von Context-Identitaet, Read-Lens-Vollstaendigkeit,
  Provenienzabonnenten, State-/Summary-Interning und Binding-Index;
- Review inkrementeller Dependency-Invalidierung, stabiler Digests,
  Cache-Key-Schema 10, globaler Fallback-Summary und Referenzlebenszeiten;
- Review Delta-Monotonie, stale Validierung, kanonischer Evidence-
  Veroeffentlichung, Cancellation, Budgets, RAM und Progress;
- Vergleich gegen den konservativen Full-State-/Full-View-/Jacobi-
  Referenzpfad und erneute Pruefung jedes Finding-Fixes;
- die bereits freigegebenen D1-/D2-Befunde konsistent zusammenfassen, ohne
  einen weiteren privaten Sonic- oder Produktlauf zu starten.

Akzeptanz:

- alle bestaetigten P0/P1 sind geschlossen und re-reviewed;
- normalisierte Analyse- und Evidence-Ergebnisse stimmen in fokussierten,
  deterministischen Differentialvertraegen fuer Referenz- und neuen Pfad,
  einen und 24 Worker sowie kalten und warmen Cachezustand ueberein; diese
  Matrix ist kein privater Sonic- oder vollstaendiger Produktlauf;
- keine reduzierte Funktion, Resolution, Guarded-AOT-Evidenz oder
  Completenessdiagnose;
- D1/D2 berichten Root-0-Contextzahl, Kostenaufteilung, verstrichene Walltime,
  effektive Kerne, RAM und stale/throwaway Work vollstaendig und ehrlich,
  auch wenn die allgemeinen Grenzen vor einer Root-0-Completion greifen;
- erst danach ist genau das bestehende einmalige KR-4981-Produktzeitgate
  zulaessig.

## Abhaengigkeitskette

```text
D1 in KR-4985: Telemetrie und G1
  -> KR-4986 semantische Lane / exakte Provenienz
       -> KR-4987 Read-Lens-Projektion nur bei positivem G1
       -> KR-4988 Interning nur bei positivem Kostengate
       -> KR-4989 Exact-Binding-Index nur bei positivem Kostengate
       -> KR-4990 inkrementelle Views nur bei positivem Kosten-/Reusegate

alle aktivierten Tasks bis KR-4990
  -> D2 zu Beginn KR-4991: Nachmessung und G2
       -> KR-4991 monotone Worklist nur bei positivem G2
            -> KR-4993 unabhaengige P0/P1-Abschlusspruefung
                 -> KR-4981 erster vollstaendiger 24-Thread-Sonic-Produktport
                      -> bei Erfolg: Gate geschlossen
                      -> bei verfehltem Acht-Minuten-Ziel und positivem
                         Restkosten-/RAM-Gate: KR-4992
                           -> KR-4993 erneut
                           -> separat autorisierter KR-4981-Wiederholungslauf
```

Ein negatives G1 oder G2 wird als begruendetes Ergebnis dokumentiert. Es darf
keinen unnoetigen Architekturumbau erzwingen und blockiert KR-4993 nicht,
sofern der konservative Pfad erhalten und der offene P0 durch die
ausgefuehrten Schritte geschlossen ist. KR-4992 ist ein eigener, nur nach
einem verfehlten KR-4981 aktivierbarer Folgezweig.

## Abschlussdefinition

Der Root-0-Block ist erst geschlossen, wenn:

1. die v56-Grenzkosten eindeutig phasen- und kardinalitaetsbezogen erklaert
   sind;
2. semantische Contextarbeit und exakte Provenienz getrennt sind;
3. jede aktivierte Read-Lens-Projektion bei Vertragsluecken FullState nutzt;
4. die gemessenen dominanten Per-Context-Kosten beseitigt oder begruendet
   unveraendert gelassen wurden;
5. eine aktivierte Worklist keine stale oder nichtkanonische Publikation
   zulaesst;
6. KR-4993 ohne offenen P0/P1 endet; und
7. der danach einmalig ausgefuehrte KR-4981-Port das bestehende Acht-Minuten-
   Ziel ohne weniger Analyseabdeckung bestaetigt oder einen engeren
   typisierten Produktblocker belegt.
