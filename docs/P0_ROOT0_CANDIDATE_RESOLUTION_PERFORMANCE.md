# P0 Candidate-Resolution: Aufgaben- und Messplan

Status: Source-seitiger KR-4985/KR-4986/KR-4987/KR-4994-Fix abgeschlossen;
Produkt-D1 bleibt unentschieden. Der funktionale Source-Checkpoint ist
`SavedEpoch-Slot-Identity-Fix`; Analyzer-ABI 33,
Function-Analysis-Epoch-Schema 18. Der terminale Sonic-v56-
Diagnoselauf belegt eine echte Contextual-State-Explosion und keine fertige
Produktartefakterzeugung.

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

## Frueherer Vergleichslauf

Der korrekte VsDevCmd-Incremental-Build von `katana-recomp --parallel 12`
war in `42,8 s` erfolgreich; es blieben nur bekannte getenv-/Shadowing-
Warnungen. Run-ID: `kr4981-20260809-012851-0b360903`. Der fruehere Lauf
dauerte `460,6 s`; Candidate Resolution lief ca.
`325,8 s`. Summary `product-exit` entstand nur durch das manuelle Beenden
des exakt identifizierten Kindprozesses nach belegter Nichtverbesserung. Es
gab keine kanonische Publikation, `0/1194` committed Roots, HOL `0`, Wave
`103`, `272` Contexts, `1.044` Semantic-Lanes, `1.029` contextual physical
evaluations und `2.430` contextual logical requests. Requeues: `1.359`
Input-Widening, `29` Summary, `733` stale Dependency; stale snapshot discards
`1.359`. Cache-Payload: `518.425.788 B`; physische Auswertungen gesamt:
`3.964`; publizierte/verwarfene Epochen: `0/0`. Kein Portartefakt oder
`game.exe` entstand; Context-/Evaluation-/Composite-Budgets blieben
unverbraucht.

Bei Attempts `1024`, `2048` und `4096` war die relevante Admission-/Stack-
Diagnostik bitgenau identisch zum vorherigen Fehlerlauf. Bei gleicher
Gesamtzeit (~`459,6 s`) stiegen Wave von `67` auf `103`, Semantic-Lanes von
`722` auf `1.044`, contextual physical evaluations von `713` auf `1.029` und
Cache-Payload von `444.266.838 B` auf `518.425.788 B`; stale requeues sanken
von `839` auf `733`. Der bounded-merge/Pending-Carrier-Fix verbessert damit
offenbar Kosten je Churn-Schritt bzw. Durchsatz, entfernt den semantischen
Lane-Treiber aber nicht. Candidate-Resolution bleibt offen und KR-4981 ist
nicht bestanden.

Die historischen Rohwerte besitzen keine belegte gemeinsame Zaehldomaene:
`65.536` ist ein Per-Function-Budget, `25.728` Contexts und `27.872`
physische Auswertungen sind Laufaggregate. Quotienten und Differenzen daraus
bleiben unzulaessig; die neuen Root-0-Werte sind dagegen klar als
nichtterminale D1-Domane gekennzeichnet.

## Lauf nach Candidate-Domain-Top-Fix

Der Candidate-Domain-Top-Fix macht abgeschnittene begrenzte Candidate-Domains
zum kanonischen absorbierenden Top mit leerem endlichem Praefix. Merge,
Normalisierung, Vergleich, Keys, Persistenz, Consumer und ABI-Promotion
behandeln diese Top-Domains konsistent; der aktuelle funktionale
Source-Checkpoint verwendet Function-Analysis-Epoch-Schema `18`, Analyzer-ABI
`33` bleibt ohne oeffentliche Structlayout-Aenderung unveraendert.

Der einmalige Lauf `kr4981-20260809-020628-2bfd8af5` endete nach `343,627 s`
durch manuellen Abbruch bei belegter identischer Nichtkonvergenz. Die Voranalyse
bis Candidate-Start dauerte etwa `146 s` einschliesslich des Gesamtstarts;
letzte Bewegung war Wave `48`. Peak Root: `1.450.078.208 B`, Peak Job:
`1.618.132.992 B`; keine kanonische Publikation und kein Portartefakt. Bei
Wave `39` waren alle 16 geprueften Kernzaehler exakt wie im Vorlauf:
Frontier `177`, Contexts `272`, Semantic-Lanes `606`, physische Auswertungen
`645`, exakte Subscriber `870`, Provenienz `21.355`, Input-Widening `263`,
Summary `10`, Forward `123`, stale `95`, stale Discards `299`, semantische
Widenings `553` und provenance-only `382` sowie die weiteren geprueften
Kernzaehler. Der Fix ist ein Korrektheits-/Persistenzfix, kein belegter
Konvergenzhebel; KR-4981 bleibt offen.

## [x] Abgeschlossener Hot-Callee-Diagnoseunterauftrag

Der Lauf `kr4981-20260809-024141-c4ffdf15` erreichte das erste vollständige
`attempts=1024`-Diagnosegate und wurde nach `244,549 s` bei Wave `24` gezielt
beendet. `uncategorized=0` für alle Top-8-Funktionen; kein Fehler, Hänger,
Portartefakt oder `game.exe`, keine Veröffentlichung. Peak Root WS:
`1.260.388.352 B`, Peak Job WS: `1.387.151.360 B`; der erwartbare
Supervisorstatus `product-exit -1` entstand durch den gezielten Stop.

`0x8C10E44E` ist der dominante isolierte Befund: `20` echte semantische
Änderungen und `40` Stack-Widenings, ausschließlich SavedEpoch-pending-ABI-
Skalare (`reg_epoch_pending=92`, `stack_epoch_pending=80`,
`tail_epoch_pending=20`, `state_stack_epoch_pending=20`,
`state_memory_epoch_pending=20`), bei erstem und terminalem Top-Frame mit
Callee-Set-incomplete (`owner=0x8C10E44E`, `site=0x8C10E486`, `target=0`).
Ordinary/direct-code/direct-PC/contextual, Callback-Loss-, Topologie-,
Top-Domain-, Map-/Tail-Topologie- und Metadatenänderungen waren dort `0`.
`0x8C09859C` zeigte `28` Änderungen und ebenfalls Callee-Set-incomplete an
`0x8C0985B0`, jedoch gemischte Domänen. `0x8C64E55E` zeigte `48` Änderungen,
darunter `reg_epoch_pending=180`, bei vollständigem Stackvertrag.

Der Diagnose-Unterauftrag ist abgeschlossen; KR-4981 und das globale
Sonic-Produktgate bleiben offen. Der SavedEpoch-Lifecycle-Fix ist source-seitig
abgeschlossen. Offen bleibt die gemeinsame Lifecycle-Ursache in Ordinary-,
Registermetadaten-, Alias-/Watcher-/Loss- und MemoryEpoch-Domänen; Alias-/Current-Tracking und
fail-closed Restore bleiben erhalten. Die Callee-Set-incomplete-Gründe an den
dynamischen Sites werden danach weiter geprüft.

Cache-Eviction ist mit null Recomputes nicht als Hauptursache belegt. Der
## [x] Abgeschlossener SavedEpoch-Lifecycle-Unterauftrag

Current-tracking SavedEpoch-Pending-ABI-Skalare werden nur an bewiesenen
normalen Call-/Tail-ABI-Gates konsumiert; detached Epochs bleiben unangetastet.
`candidate_payload_lost` ist physisch und semantisch ein absorbierendes
Epoch-Top ueber Normalize, Merge, Equality, Key, Subsumption, Evidence,
Restore und Persistenz. Konkrete Evidence und Nested-/Current-Aliasfakten
bleiben erhalten, finite Payload/Slots verschwinden; detached Top uebernimmt
keine fremde Tail-Evidence. Epoch-Schema `18`, Analyzer-ABI `33`, keine
oeffentliche Layoutaenderung.

Der Lauf `kr4981-20260809-031826-0616113a` endete nach `369,171 s` mit Status
`nonconvergence`/Exitcode `31` durch drei zehnsekundige
Null-Publikations-Amplifikationssamples. Wave `76`, `0` committed/ready/
completed Roots, `272` Contexts; Semantic-Lanes `846 -> 863 -> 886`, physische
Auswertungen `1.135 -> 1.164 -> 1.213`, Frontier `101 -> 88 -> 131`, stale
Discards `395 -> 396 -> 415`. Peak Root WS: `1.663.037.440 B`, Peak Job WS:
`1.895.583.744 B`; keine Publikation und kein `game.exe`. D1024 und D2048
hatten `uncategorized=0`. Der alte SavedEpoch-Pending-Blocker ist beseitigt;
der naechste Root-Analysepunkt ist die gemeinsame Ordinary-/Registermetadaten-
/Alias-/Watcher-/Loss- und MemoryEpoch-Lifecycle-Ursache, nicht ein weiterer
SavedEpoch-Pending-Patch. KR-4981 bleibt fail-closed offen.

Replay-Cap (`maximum_forwarded_evidence_tokens`) und semantisches
`contextual_return_evaluation_budget` sind getrennte Zaehldomaenen. Beide
koennen numerisch `65.536` betragen, duerfen aber weder addiert noch dividiert
oder als derselbe oeffentliche Zaehler interpretiert werden. Ob der bestehende
Produkt-/JSONL-Diagnosevertrag diese Trennung in jedem Verbraucher sichtbar
genug ausweist, bleibt ein offener Diagnosevertragsbefund; dieser Lauf
behauptet keine Loesung.

Der D2048-Top-8-Befund war: `0x8C10D19C` sem `36` mit reg ordinary `94`
(`mask B870`), reg metadata `4` (`mask 3860`) und state-memory Epoch-Topologie
`12`; `0x8C606E60` sem `36` mit reg ordinary `12` (`mask C010`), Alias-Flags
`16` und state-memory Topologie `2`; `0x8C09859C` sem `34` mit `48` Stack-
Events, incomplete Top-Chain, reg ordinary `8` (`mask 0F00`), Alias-Flags
`10` und Stack-Key-Topologie `6`; `0x8C6648BC` sem `32` mit reg ordinary `80`
(`mask FE31`), metadata `16` (`mask 1E00`), Alias `8` und state-memory
Topologie `6`; `0x8C098A82` sem `28` mit reg ordinary `8` (`mask 0011`) und
Alias `14`; `0x8C64E55E` sem `28`/Events `144` mit Alias `24`;
`0x8C10C99C` sem `26` mit reg ordinary `40` (`mask 800F`), metadata `2`
(`mask 0004`) und state-memory Topologie `12`; `0x8C604440` sem `22` mit reg
ordinary `14` (`mask 0070`) und metadata `12` (`mask 0050`). Kein Eintrag
zeigt einen Stack-/Tail-/Memory-Ordinary-Payload als neuen Haupttreiber.

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

### D2 - abgeschlossene Produktdiagnose vor KR-4991

Der aktuelle D2-Lauf ist abgeschlossen und erzeugte keine neue Testsuite,
Matrix oder synthetischen Gatevertrag. Er zeigte bei bitgleicher relevanter
Admission-/Stack-Diagnostik in den Vergleichsversuchen zwar hoeheren
Durchsatz, aber weiterhin einen nicht konvergierenden semantischen
Stack-Zyklus. D2 belegt damit keinen positiven G2-Schedulerhebel; KR-4991
bleibt inaktiv und KR-4981 ist nicht bestanden.

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

### KR-4994 - Begrenzter identitaetserhaltender unresolved Stack-/Context-Candidate-Carrier [x]

Source-seitig abgeschlossen. Der bounded-merge/Pending-Carrier-Fix bewahrt
Stack-/Storage-Identitaet in einem strikt begrenzten, monotonen und
identitaetsgebundenen Carrier und integriert ihn in Merge, Key/Cache, Lifetime,
ABI-/Summary-Propagation, Stack-may-load, Candidate-Recompute,
contextual/forwarded/stable Harvest und Export-Gate, ohne Scheduler-/Budget-
umbau, Fallback, Coverage-Reduktion oder Sonic-Hack. Der Zweikanalpfad haelt
Evidence-Stale in privaten Replaykapseln und trennt es vom logischen
Semantic-Lane-Key. Der D-Lauf zeigt jedoch, dass der semantische Lane-Treiber
als Produktblocker offen bleibt.

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
sind geschlossen; das Analyzer-ABI-Finding ist unter dem aktuellen Analyzer-ABI
33 geschlossen. Nicht aktivierte KR-4988 bis KR-4991 wurden
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
- KR-4981 bleibt das globale Produktgate; der aktuelle D-Lauf bestand es nicht
  und ein weiterer Lauf ist nicht automatisch freigegeben.

Die globale Abwesenheit der Limitmetriken und von `IncompleteRoot` ist erst
im vollstaendigen KR-4981-Port beweisbar. D1 und D2 sind begrenzte
Diagnoseexporte und ersetzen diesen Produktnachweis nicht. Der aktuelle
KR-4981-Versuch ist abgeschlossen und nicht bestanden; ein weiterer Lauf
benoetigt eine ausdrueckliche Freigabe.

### KR-4981 - Sonic-Produktgate

Der aktuelle D-Lauf war der freigegebene 24-Thread-NativeDisc-Sonic-Versuch;
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
Cacheverdraengung oder hohem Throwaway-Anteil deaktiviert. Ein weiterer
KR-4981-Lauf folgt nicht automatisch.

## Abhaengigkeitskette

```text
KR-4985/KR-4986/KR-4993/KR-4987/KR-4994 source-seitig abgeschlossen
  -> D-Lauf beendet nach belegter Nichtverbesserung; Candidate-Resolution offen
```

Die einzige D1-Evidenz ist ein unvollstaendiger, nichtterminaler Root-0-Lauf;
D1/G1 bleibt historisch unentschieden. D2/G2 ist abgeschlossen und negativ:
kein positiver Schedulerhebel. D9 ist
beendet und Root 0 konvergierte fail-closed ohne Erfolgsaussage; KR-4988 bis
KR-4991 bleiben inaktiv. KR-4994 ist source-seitig abgeschlossen; der
semantische Lane-Treiber bleibt der offene P0-Produktblocker. KR-4981 bleibt
das globale Produktgate und ist nicht bestanden.
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
7. KR-4993 ist mit dem im vorherigen Fixcommit geschlossenen Analyzer-ABI-
   Finding source-seitig abgeschlossen; und
8. KR-4981 einen vollstaendigen Sonic-Port erzeugt oder einen engeren
   typisierten Produktblocker belegt.
