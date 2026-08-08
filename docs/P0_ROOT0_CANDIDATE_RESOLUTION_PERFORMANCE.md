# P0 Candidate-Resolution: Aufgaben- und Messplan

Status: Source-seitiger KR-4985/KR-4986/KR-4987-Fix abgeschlossen; Produkt-D1 bleibt
unentschieden. Die Arbeitsbasis ist
`594f0191b321bd2f470d0aa07100e82f3eea956f` plus KR-4987-Sourceaenderung in
diesem Task; Analyzer-ABI 32. Der terminale Sonic-v56-Diagnoselauf belegt eine
echte Contextual-State-Explosion und keine fertige Produktartefakterzeugung.

Dieses Dokument ergaenzt
[`P0_NATIVE_DISC_COLD_BUILD_PERFORMANCE.md`](P0_NATIVE_DISC_COLD_BUILD_PERFORMANCE.md)
und unterliegt dem repositoryweiten Vertrag in `../AGENTS.md`.

## Repositoryweiter Arbeitsablauf

Jeder Task dieses Plans folgt exakt:

```text
Task implementieren
  -> alle betroffenen Pfade reviewen
     und bestaetigte Fehler innerhalb dieses Reviews schliessen
  -> den reviewten Task direkt auf main committen und pushen
  -> naechster Task
```

Es gibt keine separate standardmaessige Test-, Verifikations-, Fix- oder
Integrationsphase. Neue Unit-Tests, Regressionstests, Fixtures,
Stresslaeufe, Testprojekte oder Matrizen werden nicht gebaut und nicht als
Abschlussbedingung verlangt. Gefixt wird durch Reviews der betroffenen Pfade;
getestet wird mit Sonic an den ausdruecklich vorgesehenen Diagnose- und
Produktgates.

D1 und D2 sind reale, begrenzte Sonic-Diagnoseexporte und keine
Testmatrizen. Sie benoetigen jeweils eine ausdrueckliche Nutzerfreigabe.

## Terminaler v56-Ausgangspunkt

```text
Historischer Source-Checkpoint:                 a521999
Laufzeit:                                       1:28:24
Exitcode:                                       5
5-Stunden-Hardlimit erreicht:                   nein
kanonisch committed Roots:                      1 / 1.191
contextual_return_evaluation_limited_functions: 1
Per-Function-Evaluationsbudget:                 65.536, ausgeschoepft
Context-Limit:                                  nicht erreicht
eindeutige Contexts:                            25.728
physische Auswertungen:                         27.872
Cache-Eviction-Recomputes:                      0
Resolution-Epoch-Retention:                     incomplete-root
Portartefakt / game.exe / Screenshot:           keines / keine / keiner
```

Der gemeinsame Fix behandelt die historische Ursache: Das Per-Function-
Budget wurde vor MultiRoot-, Cache- und semantischer Deduplizierung pro
exaktem Provenienzrequest belastet. Kollisionssichere Full-State-
Semantic-Lanes tragen die semantische Arbeit, exakte Contributions und
Evidence bleiben private Provenienzabonnenten, und das Budget wird nur bei
erstmaliger Lane-Zulassung belastet. Die produktive D1-Telemetrie ist nur bei
expliziter Detailtelemetrie aktiv.

Der einmalige freigegebene D1-Lauf lieferte bei `185,370 s` valide,
nichtterminale Root-0-Evidenz: Status `running`, `0/1191` abgeschlossene
Roots, Wave `1.019`, Frontier `0` (maximal `223`), `288` zugelassene Contexts,
`6.724` Evaluationen bzw. logische Admissions, `15.170` logische Requests,
`6.724` Semantic-Lanes, `6.725` physische Auswertungen, `5.846` Cache-Reuses,
`15.157` exakte Subscriber und `226.886` Provenienzverknuepfungen.
Requeues: `1` initial root, `287` neue exakte Lane, `8.248` Input-Widening,
`177` Summary-Aenderung, `405` Forward-Edge, `6.052` stale Dependency;
stale Discards `12.643`. Semantic Widenings: `10.412`, provenance-only
Widenings: `2.201`. Full-State-Lanes und Projected-Physical-Keys lagen jeweils
bei `6.724`, Alpha-Fallbacks bei `0`; alle Budget-, IncompleteRoot-,
Retention-, Degraded- und Drop-Flags waren false; `telemetry_complete` war im
letzten nichtterminalen Progressdatensatz true.

Die aggregierten D1-Kosten waren Snapshot `15.170 / 2,950 s`, Key `15.160 /
5,124 s`, inklusiver Cache-Request `12.571 / 162,453 s`, inklusives Apply
`63.742 / 17,790 s`, darin Binding-Merge `41.124 / 1,519 s`, Evidence
`15.157 / 2,492 s`, serielle Commit-Operationen `1.018 / 0,000506 s` und
Publish-Operationen `1.018 / 0,008050 s`. Diese Operationszaehler sind keine
committed Resolution-Roots. Nach einem privaten Supervisor-I/O-Fehler wurde
die temporaere JSONL bis `185,586 s` lesbar/gespuelt, aber ohne terminalen
Datensatz und ohne atomare Publikation; Root 1 wurde nicht erreicht. D1/G1 ist deshalb
fail-closed und unentschieden.

Der D9-Lauf dauerte `20,331 s` und endete am ersten fail-closed
Telemetrie-/Publikationssignal. Root 0 erreichte Wave `184`, Frontier `0`
(maximal `216`), bei `288` admitted contexts, `2.724` admitted evaluations/
Semantic-Lanes, `4.349` logical requests und `3.739` physical evaluations.
Input-widening/stale-dependency requeues: `2.497`/`932`; stale snapshot
discards `1.740`; semantic/provenance-only widenings `939`/`2.377`.
Budgets blieben unverbraucht; `local_fixpoint=0`, `pending_regions=0`,
`candidate_values_truncated=1`, `abi_stack_base_unresolved=1`, die übrigen
Candidate-/Stack-/Table-Loss-Flags `0`. Epochs: published `0`, discarded `1`,
Retention `incomplete-root`. 64 Candidate-Truncations waren ausschließlich
state/identity mit `values=0`; das terminale Kandidatenbit stammt aus
`inventory_stack_callback_loss_identity_truncated`. 6 Contextual-Value-
Overflows hatten jeweils `merged_values=9`. 462 Stack-Loss-Diagnosen: 189
candidate-store, 113 fixpoint-call, 2 forwarded-tail, 0 tail-store-identity-loss.
Das ist kein Haenger und kein Produkterfolg; der nächste Engpass ist
Stack-/Storage-Identitaetsverlust.

Die historischen Rohwerte besitzen keine belegte gemeinsame Zaehldomaene:
`65.536` ist ein Per-Function-Budget, `25.728` Contexts und `27.872`
physische Auswertungen sind Laufaggregate. Quotienten und Differenzen daraus
bleiben unzulaessig; die neuen Root-0-Werte sind dagegen klar als
nichtterminale D1-Domane gekennzeichnet.

Cache-Eviction ist mit null Recomputes nicht als Hauptursache belegt. Der
Produktblocker bleibt bis zu einem vollstaendigen Produktgate offen; als
Kostenmodell gilt weiterhin:

```text
echte semantische Contextmenge
  x
Kosten je Context
  x
ueberwiegend serieller kritischer Scheduling-Span
```

Der terminale Stand darf nicht mehr pauschal als „Root 0 scheiterte“
bezeichnet werden. `1/1191` identifiziert ohne terminal ausgegebenen
Rootindex, Rootadresse und limitierte Funktion keinen konkreten Root als
Fehlerursache. Bis KR-4985 diese Identitaet ausgibt, gilt der Befund fuer den
Candidate-Resolution-Pfad allgemein.

## Nicht im Umfang

- keine Erhoehung des 65.536er-Evaluationsbudgets als Performancefix;
- kein groesserer Evaluation-Cache als vermeintliche Hauptloesung;
- keine Erhoehung der Threadzahl als Ersatz fuer algorithmische Arbeit;
- keine vollstaendige Freigabe spaeterer Roots vor stabilem Head-of-Line-
  Pfad;
- kein Revert der Dependency-Views, Evidence-Referenzen,
  State-Wiederverwendung oder Merge-Fastpaths aus `a521999`;
- kein GPU-Offload und keine Reaktivierung von KR-4982/KR-4983;
- keine neue Test-, Thread-, Cache-, Worker- oder Differentialmatrix;
- keine synthetische Ersatzabnahme;
- keine titelbezogenen Adressen, Retailwerte oder privaten Pfade in Source,
  Telemetrie oder Dokumentation.

## Normative Invarianten

1. **Fail-closed Read-Lens.** Nur vollstaendig bewiesene Register-, Stack-
   und Memory-Reads sowie alle Output-, Alias-, Inventory-, Evidence- und
   Fallback-Watcher duerfen projizieren. Jede Luecke verwendet `FullState`.
2. **Semantik und Provenienz bleiben getrennt.** Eine semantische Lane darf
   mehrere exakte Contributions, Callsites, Callees und Evidence-Tokens
   bedienen. Diese Abonnenten duerfen weder verschmelzen noch verloren gehen.
3. **Digest ist nur ein Index.** Gleichheit verlangt kanonische Identitaet
   oder strukturelle Kollisionspruefung.
4. **Persistente Identitaet bleibt stabil.** Prozesslokale IDs stehen nie
   allein in persistenten oder kanonischen Keys.
5. **Stale Ergebnisse publizieren nichts.** Summary, Evidence und
   kanonische Ausgabe werden nur nach Versionsvalidierung committed.
6. **Monotonie und Budgets bleiben autoritativ.** Widening, Truncation,
   Cancellation, Memory-Leases und Inventory-/Contextbudgets bleiben
   fail-closed.
7. **Cache-Key-Schema bleibt vollstaendig.** Globale Fallback-Summary und
   alle relevanten kontextuellen Summaries bleiben gebunden.
8. **Keine Performance durch weniger Produktarbeit.** Funktionen, Bloecke,
   Resolutionen, Guarded-AOT-Einstiege, Evidence und Inventory-Sinks duerfen
   nicht reduziert werden, um eine bessere Zeit zu melden.
9. **Unvollstaendige Roots werden nie behalten.** `IncompleteRoot` kann kein
   wiederverwendbares Analyseartefakt werden.
10. **AOT-only bleibt verbindlich.** Kein Interpreter, JIT, Runtime-Dekoder
    oder Emulationsfallback uebernimmt fehlende Analysearbeit.
11. **Fehler folgen der Version.** Ein Batchresultat wird vor `item.error`,
    Cancellationwirkung und terminaler Publikation auf Stale-Versionen
    geprueft. Ein veralteter Fehler darf keinen aktuellen Root beenden.

## Diagnosegates

### D1 in KR-4985

D1 wurde einmal nach ausdruecklicher Freigabe ausgefuehrt. Ein weiterer Lauf
ist in diesem Bugfix nicht vorgesehen. Der Diagnoseexport sollte enden:

- nach dem ersten vollstaendigen schweren Candidate-Resolution-Root; oder
- frueher an den repositoryweiten Stall-, Nichtkonvergenz- oder
  15-Minuten-Grenzen.

D1 identifiziert mindestens:

- Rootindex und Rootadresse;
- limitierte Funktionsadresse;
- Context- und Evaluationsbudgets;
- logische und physische Auswertungen;
- Requeue-Ursachen: initial root, neue exakte Lane, Input-Widening,
  Summary-Aenderung, Forward-Edge-Insert/Widening und stale Dependency;
- Frontierbreite;
- Snapshot-, Key-, Apply-/Merge-, Evidence- und Commitkosten;
- Full-State-, Projected-Lens- und Provenienz-Cardinalitaet.

Die Resultatannahmepfade sind source-seitig so geschlossen, dass
Stale-/Cancellationpruefung vor Fehlerbehandlung und Publikation erfolgt.
Die vorliegende D1-Evidenz ist trotzdem nichtterminal und kann G1,
Limitfreiheit, Coverage oder terminale Retention nicht entscheiden.

### D2 vor der Entscheidung ueber KR-4991

D2 folgt erst nach KR-4986 und allen durch G1 aktivierten Tasks KR-4987 bis
KR-4990. Es misst denselben realen Sonic-Pfad und entscheidet, ob hinter der
Jacobi-Barriere tatsaechlich dependency-gueltige Arbeit wartet.

D1 und D2 sind Produktdiagnose. Sie erzeugen keine neue Testsuite, keine
Matrix und keinen allgemeinen synthetischen Gatevertrag.

## Messgate G1

G1 entscheidet nur anhand der D1-Produktdiagnose:

- **KR-4987 positiv:** `ProjectedLensDigest` reduziert die Zahl
  semantischer Contexts gegenueber `FullStateDigest` um mindestens 30
  Prozent oder ein zeitgewichtetes Modell belegt mindestens 20 Prozent
  Candidate-Resolution-Gewinn.
- **KR-4988 positiv:** Equality-, Copy-, Merge- oder Keyarbeit belegt
  mindestens zehn Prozent der gemessenen Arbeit beziehungsweise des
  realistischen Walltimepotenzials.
- **KR-4989 positiv:** Binding-Scan samt Equality-/Mergearbeit erreicht
  dieselbe Zehn-Prozent-Schwelle und besitzt wachsende Binding-p95-Werte oder
  spaete Exact-Hit-Indizes.
- **KR-4990 positiv:** Snapshot-/Key-Aufbau erreicht dieselbe
  Zehn-Prozent-Schwelle und im Median bleiben mindestens 50 Prozent der
  Edge-, Binding- und Evidence-View-Eintraege zwischen Reevaluations
  unveraendert.

Ein negatives Gate dokumentiert den konservativen Pfad und aktiviert keinen
unnuetzen Umbau.

## Messgate G2

KR-4991 wird nur aktiviert, wenn D2 mindestens eines belegt:

- mindestens zehn Prozent der D2-Walltime entfallen auf Barriere oder
  serielle Veroeffentlichung, obwohl dependency-gueltige Folgearbeit bereit
  ist; oder
- ein zeitgewichtetes Modell belegt mindestens zehn Prozent realisierbares
  Candidate-Resolution-Walltimepotenzial durch sofortiges Einreihen bereits
  freigesetzter Arbeit.

Eine echte Breite-1-Kausalkette ist ein negatives G2. Ein anderer Scheduler
kann keine Parallelitaet erfinden, obwohl Menschen das bei Threadzahlen gern
versuchen.

## Aufgaben

### KR-4985 - Phasen- und Kardinalitaetstelemetrie [x]

Ziel:

- limitierenden Root und Funktion identifizieren;
- logische Zulassung, semantisch eindeutige Lane, physische Auswertung,
  Cache-Reuse und Requeue trennen;
- steigende Per-Context-Kosten zuordnen;
- G1 belastbar entscheiden.

Betroffene Pfade fuer das Review:

- `FunctionValueAnalysisProgress`;
- Progress-/JSONL-Transport und Dropdiagnose;
- `capture_contextual_snapshot()`;
- `make_function_evaluation_cache_key()`;
- `apply_call()` und `merge_state()`;
- Root-, Budget-, Retention- und Terminalpfade.
- Batchresultatannahme, Dependency-/Snapshot-Versionen, `item.error`,
  Cancellation und gezielte Stale-Neuplanung.

Status und Abschluss:

```text
implementieren -> betroffene Pfade reviewen und Findings schliessen -> main
```

Source-seitig abgeschlossen; D1/G1 bleibt wegen des unvollstaendigen Laufs
unentschieden. Keine neue Testinfrastruktur. Ein stale oder gecanceltes
Resultat publiziert weder Summary/Evidence noch beendet es ueber seinen
veralteten Fehler einen aktuellen Root.

### KR-4986 - Semantische Lanes und Provenienzabonnenten [x]

Ziel:

- physische Fixpunktsemantik von exakter Contribution-/Evidence-Provenienz
  trennen;
- zunaechst die bestehende Full-State-Identitaet verwenden;
- Versionen fuer Stale-Validierung statt als immer neue Lane-Identitaet
  behandeln.

Betroffene Pfade fuer das Review:

- `ContextualLane` und Lane-Familien;
- Multi-Root-Koordination;
- Forwarded-/isolierte Korrelation;
- Evidence-Linsen und Tokenlebenszeiten;
- Budget- und Epochretention.

Abschluss:

- gleiche kanonische Semantik und Evidence;
- unvollstaendige Rootarbeit bleibt unveroeffentlicht;
- keine Read-Lens-Aktivierung vor abgeschlossener Full-State-Pruefung;
- direkt auf `main`, ohne neue Tests oder Matrix.

### KR-4987 - Read-Lens-Context-Identitaet [x]

Source-seitig abgeschlossen. Die Read-Lens-projizierte Contextual-
SemanticLane-Identitaet verwendet vollstaendige Key-Bytes fuer
Kollisionssicherheit; jede Vertragsluecke, Truncation oder Fallbackbedingung
bleibt strikt FullState. Exakte Provenienz/Restore und Discovery -> Freeze ->
Publish bleiben unveraendert. Der D9-Lauf ist beendet und fail-closed; Root 0
konvergierte ohne Portartefakt und ohne Produkterfolg.

Ziel:

- semantisch gleiche Callee-Eingaenge vor Lane-Erzeugung zusammenlegen;
- ungelesenen State und Provenienz aus der semantischen Identitaet entfernen;
- bei jeder Vertragsluecke `FullState` verwenden.

Reviewschwerpunkte:

- Register-, Stack-, Memory-, Alias-, Output-, Inventory- und
  Fallback-Read-Vertraege;
- spaete Vertragserweiterung und Rebucketing;
- Digestkollisionen;
- exakte Provenienzabonnenten;
- Completeness und Truncation.

Die Produktwirkung bleibt nach dem beendeten fail-closed D9-Lauf und der
globalen Produktabnahme offen; D9 erzeugte kein Portartefakt und keinen
Produk­terfolg. KR-4994 ist der naechste Implementierungstask nach Sol-Review.

### KR-4994 - Begrenzter identitaetserhaltender unresolved Stack-/Context-Candidate-Carrier

Offener P0-Folgetask. Der D9-Lauf belegt Stack-/Storage-Identitaetsverlust als
naechsten Engpass; der boolsche State-Carrier blockiert korrekt fail-closed.
Der Carrier muss strikt begrenzt, monoton und identitaetsgebunden sein und in
Merge, Key/Cache, Lifetime, ABI-/Summary-Propagation, Stack-may-load,
Candidate-Recompute, contextual/forwarded/stable Harvest und Export-Gate
integriert werden. Kein Scheduler-/Budgetumbau, kein Fallback, keine
Coverage-Reduktion und kein Sonic-Hack. Erst nach Sol-Review genau ein neuer
Produktlauf.

### KR-4988 - State-/Summary-Interning

Nur bei positivem Kostengate.

Ziel:

- tiefe Equality-, Copy- und Keyarbeit fuer immutable Snapshots reduzieren;
- prozesslokale Identitaet von persistentem Content-Digest trennen.

Reviewschwerpunkte:

- keine In-place-Internierung mutabler Fixpunktzustande;
- strukturelle Kollisionspruefung;
- Lebenszeit, Retention und RAM;
- Cache-Key- und Persistenzvertraege.

Die Wirkung wird in D2 und Sonic bewertet. Keine Interning-Testmatrix.

### KR-4989 - Indexierte exakte Bindings

Nur bei positivem Kostengate.

Ziel:

- exakte Binding-Treffer direkt finden;
- den bestehenden linearen Join-/Subsumption-Fallback unveraendert
  erhalten.

Reviewschwerpunkte:

- Indexkollisionen;
- Insert, Widening und Generationwechsel;
- vollstaendige Equality nach Lookup;
- Miss- und Fallbacksemantik.

Die Wirkung wird in D2 und Sonic bewertet. Keine Binding-Matrix.

### KR-4990 - Inkrementelle Dependency-Views

Nur bei positivem Kosten-/Reusegate.

Ziel:

- unveraenderte Adjazenz, Bindings, Versionen und Evidence-Layouts behalten;
- nur geaenderte Shards ersetzen;
- Full-Rebuild als fail-closed Fallback erhalten.

Reviewschwerpunkte:

- Edge-Add/Widen;
- Summary-only-Aenderung;
- stale Entfernung und Zyklen;
- globale und kontextuelle Fallback-Summaries;
- Referenzlebenszeiten und owning Materialisierung.

Die Wirkung wird in D2 und Sonic bewertet. Keine View-Differentialmatrix.

### KR-4991 - Versionierte monotone Worklist

Nur bei positivem G2.

Ziel:

- kausal freigesetzte unabhaengige Arbeit sofort einreihen;
- unveraenderte semantische Versionen nicht erneut gegen das Budget
  zulassen;
- stale Ergebnisse vor Publikation verwerfen.

Reviewschwerpunkte:

- immutable Worker-Snapshots;
- Dependency-Versionen;
- kanonische Deltapublikation;
- Retry-/Ressourcenbudget;
- Jacobi-/Seriell-Fallback;
- Cancellation, Fehlerreihenfolge und Evidence;
- echte Breite-1-Ketten.

D2 und Sonic bewerten die Wirkung. Keine Worker- oder Threadmatrix.

### KR-4993 - Abschlussreview [x]

KR-4993 implementiert keine neue Testinfrastruktur und startet keinen
Produktlauf. Der vollstaendige Sol-Endreview des unmittelbar vorherigen
Explosionsbug-Diffs wurde wiederverwendet; alle bestaetigten Source-Findings
sind geschlossen; das bisher offene Analyzer-ABI-Finding wird in diesem
Commit mit ABI 32 geschlossen. Nicht aktivierte KR-4988 bis KR-4991 wurden
nicht als geaendert oder reviewpflichtig behauptet.

Pflichtumfang:

- Contextidentitaet und Provenienz;
- FullState-Fallback und Read-Lens;
- State-/Summary-Interning und Binding-Indizes;
- Dependency-Invalidierung und Cache-Key-Schema;
- globale Fallback-Summary;
- Stale, Evidence und kanonische Publikation;
- Cancellation, RAM, Budgets und Retention;
- getrennte Zaehldomaenen;
- D1-/D2-Ergebnisse.

Quellseitige Freigabebedingungen:

- keine bestaetigten offenen Findings;
- keine reduzierte Analyse- oder AOT-Abdeckung;
- alle Context-, Evaluations- und Rootbudgetpfade bleiben fail-closed und
  terminal typisiert;
- `IncompleteRoot` bleibt unpublizierbar und nicht wiederverwendbar;
- kein stale oder gecanceltes Resultat kann publizieren oder einen aktuellen
  Root ueber einen veralteten Fehler beenden;
- jeder schwere Root terminal identifizierbar;
- KR-4981 bleibt nach KR-4994 und Sol-Review ein genau einmaliges globales
  Produktgate.

Die globale Abwesenheit der Limitmetriken und von `IncompleteRoot` ist erst
im vollstaendigen KR-4981-Port beweisbar. D1 und D2 sind begrenzte
Diagnoseexporte und ersetzen diesen Produktnachweis nicht. KR-4981 darf erst
nach KR-4994 und Sol-Review genau einmal erneut laufen.

### KR-4981 - Sonic-Produktgate

Nach KR-4994 und Sol-Review wird fuer KR-4981 genau ein frischer
24-Thread-NativeDisc-Sonic-Port gebaut,
installiert und normal ausgefuehrt.

Ziele:

- vollstaendiger Kaltport in hoechstens acht Minuten;
- kein Context-/Evaluationslimit und keine verworfene Epoche;
- keine reduzierte Funktions-, Block-, Resolution- oder AOT-Abdeckung;
- sichtbarer Fortschritt und echter Screenshot;
- bekannter AOT-Pfad passiert oder engerer typisierter Blocker;
- kein zweiter Build nur fuer Timing und keine Threadmatrix.

### KR-4992 - Begrenzte spaetere Root-Spekulation

Nur nach einem verfehlten KR-4981 und positivem Restkosten-/RAM-Gate.

Der Pfad erhaelt eine harte Reserve fuer den kanonischen Root, publiziert
nichts vor der kanonischen Reihenfolge und wird bei Verlangsamung,
Cacheverdraengung oder hohem Throwaway-Anteil deaktiviert. Ein KR-4981-Retry
folgt erst nach KR-4994 und Sol-Review und wird genau einmal freigegeben.

## Abhaengigkeitskette

```text
KR-4985/KR-4986/KR-4993/KR-4987 source-seitig abgeschlossen
  -> D9 beendet fail-closed; Root 0 konvergiert, kein Portartefakt und kein Erfolg
  -> KR-4994 naechster Implementierungstask nach Sol-Review
```

Die einzige D1-Evidenz ist ein unvollstaendiger, nichtterminaler Root-0-Lauf;
D1/G1 bleibt historisch unentschieden. D2/G2 wurde nicht ausgefuehrt. D9 ist
beendet und Root 0 konvergierte fail-closed ohne Erfolgsaussage; KR-4988 bis
KR-4991 bleiben inaktiv. KR-4994 ist ein offener P0-Folgetask und naechster
Implementierungstask nach Sol-Review. KR-4981 bleibt das globale Produktgate;
ein Retry ist erst nach KR-4994 plus Sol-Review genau einmal zulaessig.
Keine neue Testmatrix ersetzt reale Produktgates.

## Abschlussdefinition

Die Source-Aufgaben KR-4985, KR-4986, KR-4993 und KR-4987 sind durch den
gemeinsamen Explosionsfix bzw. den abgeschlossenen Endreview abgeschlossen.
Der Candidate-Resolution-P0 als Produktgate ist
wegen des fehlenden vollstaendigen Roots, des nicht erreichten historischen
Root 1 und der ausstehenden terminalen Produktevidenz noch nicht geschlossen.

Der Candidate-Resolution-P0 ist erst geschlossen, wenn:

1. limitierter Root und Funktion sowie alle dominanten Kosten erklaert sind;
2. semantische Contextarbeit und exakte Provenienz getrennt sind;
3. jede aktivierte Projektion bei Vertragsluecken FullState verwendet;
4. die belegten dominanten Per-Context-Kosten geschlossen oder begruendet
   unveraendert gelassen wurden;
5. keine unveraenderte semantische Version unbegruendet erneut Budget
   verbraucht;
6. keine stale oder nichtkanonische Publikation moeglich ist;
7. KR-4993 ist mit dem in diesem Commit geschlossenen Analyzer-ABI-Finding
   source-seitig abgeschlossen; und
8. KR-4981 einen vollstaendigen Sonic-Port erzeugt oder einen engeren
   typisierten Produktblocker belegt.
