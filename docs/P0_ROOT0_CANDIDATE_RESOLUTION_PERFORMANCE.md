# P0 Candidate-Resolution: Aufgaben- und Messplan

Status: historischer PlatformAbi-Analyseplan; der native Produktvertrag hat
Vorrang. Aktuell gelten Runtime-ABI 93, Analyzer-ABI 36,
Portprojektvertrag 80 und Native-Port-Profilvertrag 5. Der source-seitige
KR-4985/KR-4986/KR-4987/KR-4994-Fix ist abgeschlossen;
die folgenden D1/D2/Candidate-Resolution-Werte sind historische PlatformAbi-
Diagnostik. Der damalige RuntimeOnly-Bring-up verwendete Analyzer-ABI 34,
Function-Analysis-Epoch-Schema 27 und lokales In-Process-Evaluation-Cache-
Schema 13. Native-AOT-Emissionsprofil 27 und AOT-Partitionsschema 7 sind
aktuell. Der historische Modus `port --analysis-mode runtime-only` war nur mit
`--game-project` zulaessig und ist jetzt ausschliesslich internes
Diagnoseorakel, kein Produktprofil. RuntimeOnly setzt `GuestCallAbi::Unknown`,
umgeht die blockierende SuperHC-FunctionValue-/Candidate-Resolution, erzeugt
weiterhin nativen AOT-Code und nutzt RuntimeOnly-Dispatch ueber eine exakte
statische Guest->Host-Tabelle. Stop-on-miss und typed abort bleiben aktiv;
kein Interpreter, JIT, Runtime-Decoder oder geratener Zielpfad.

Das historische RuntimeOnly-Build-/Export-Gate war bestanden. Der letzte saubere
RuntimeOnly-Diagnoselauf erreichte `FirstVisibleGameFrame` ohne Skip; Candidate-
Resolution bleibt im RuntimeOnly-Bring-up deferred und ist nicht der aktuelle
Produktblocker. Die identische Vergleichsreihe stieg bis `24,2926 MHz`, aber
`100 MHz` und Memory-Card-Screen/Hauptmenue bleiben offen.

Der aktuelle Runtime-P0 liegt im seriellen Runtime-/Dispatch-Overhead bis
mindestens `100 MHz`, nicht in fehlender Movie-Decodierung. Die fruehere
StartRender-/Frame-Finalize-Diagnose ist verworfen; StartRender wurde
beobachtet. Dieses Dokument bewahrt die Candidate-Resolution-Diagnostik und
deren historische Gates, behauptet aber keinen RuntimeOnly-Candidate-
Resolution-Produktblocker.

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
von `839` auf `733`. Diese historische Gegenüberstellung belegt wegen der
unterschiedlichen Rohwerte und Endpunkte keine materielle Produkt-/Performance-
verbesserung und keinen Konvergenzhebel. Candidate-Resolution bleibt offen
und KR-4981 ist nicht bestanden; historisch wurde hier Inventory-Provenance-
Live-in/Spill-through als P0 vermutet.

Die historischen Rohwerte besitzen keine belegte gemeinsame Zaehldomaene:
`65.536` ist ein Per-Function-Budget, `25.728` Contexts und `27.872`
physische Auswertungen sind Laufaggregate. Quotienten und Differenzen daraus
bleiben unzulaessig; die neuen Root-0-Werte sind dagegen klar als
nichtterminale D1-Domane gekennzeichnet.

## Lauf nach Candidate-Domain-Top-Fix

Der Candidate-Domain-Top-Fix macht abgeschnittene begrenzte Candidate-Domains
zum kanonischen absorbierenden Top mit leerem endlichem Praefix. Merge,
Normalisierung, Vergleich, Keys, Persistenz, Consumer und ABI-Promotion
behandeln diese Top-Domains konsistent; der historische Candidate-Domain-Top-
Lauf lief unter Function-Analysis-Epoch-Schema `18` und Analyzer-ABI `33`. Der
aktuelle Source-Checkpoint ist separat oben ausgewiesen.

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

Die dominante Hot-Callee ist der isolierte Befund: `20` echte semantische
Änderungen und `40` Stack-Widenings, ausschließlich SavedEpoch-pending-ABI-
Skalare (`reg_epoch_pending=92`, `stack_epoch_pending=80`,
`tail_epoch_pending=20`, `state_stack_epoch_pending=20`,
`state_memory_epoch_pending=20`), bei erstem und terminalem Top-Frame mit
Callee-Set-incomplete am Owner-/Site-/Target-Feld des unvollständigen Vertrags (`target=0`).
Ordinary/direct-code/direct-PC/contextual, Callback-Loss-, Topologie-,
Top-Domain-, Map-/Tail-Topologie- und Metadatenänderungen waren dort `0`.
Eine zweite Hot-Callee zeigte `28` Änderungen und ebenfalls Callee-Set-incomplete,
jedoch gemischte Domänen. Eine weitere Hot-Callee zeigte `48` Änderungen,
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
keine fremde Tail-Evidence. Der historische SavedEpoch-Lifecycle-Stand lief
unter Epoch-Schema `17` und Analyzer-ABI `33`.

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

## Aktueller Source- und Laufstand

Der historische Candidate-Resolution-Source-Stand ist `49b0f72a9f49d60a4eb6e0481460cd57c5625735`.
Er erlaubt strukturelle Contextual-Hybrid-Projektion mit retained sticky loss,
erkennt SavedEpoch-Slot-Pending-Top in allen Truncation-/Publication-Checks
fail-closed und trennt Provenance-Replay-Capsule-/Keybyte-Limits öffentlich
vom semantischen Evaluation-Limit. Ein echter Evaluation-Cap belastet nur den
Evaluation-Zähler; der damalige Source-Stand verwendete Analyzer-ABI `34`,
Epoch-Schema `27` und lokales In-Process-Evaluation-Cache-Schema `13`.

Der erledigte Source-Unterauftrag umfasst eine begrenzte 17-Source-
Provenienz-Live-in-Map für R0-R15 plus incoming stack, getrennte conditional /
unconditional SavedEpoch-Mutation und Alias-Capture-Verträge, per-flow
Register-/Stack-Taints und Return-Maps, duale Ordinary-/Provenance-Projektion,
current-/detached-Alias-Watcher sowie Persistenz, Keys, Shards, Contextual-,
Root- und Loss-Integration. R0-indexed-/Predecrement-Korrekturen sind
enthalten. RTS bindet R0-Provenienz als conditional alias-capture, raw
stack-derived Rückgaben und Storage-Loads werden fail-closed in unresolved
SavedEpoch überführt; defensives Storage-Repair löscht semantische und
Inventory-R15-Koordinaten vorher. Der current mutation receiver umfasst den
detached watcher; eine blanket `stack_may_derived`-Lattice ist nicht enthalten.

## Aktueller Produktlauf

Der historische PlatformAbi-Produktlauf `kr4981-20260809-091410-2766aaa6` endete nach ca.
`275 s` gesamt (Candidate Resolution ca. `221 s`) nach drei
Amplifikationssamples mit `nonconvergence`: `0/1274` Roots, HOL `0`, Wave
`107`, `280` Contexts, `970` Semantic-Lanes, `1.861` physische und `2.526`
logische Requests, Input-Widening `536`, Summary `22`, Forward `123`, stale
Requeues `272`, stale Discards `806`, Cache-Payload `589.178.706 B`; keine
Budgets erschöpft, keine Publikation und kein Artefakt bzw. `game.exe`.
Der Supervisor-Abbruch meldete bei `taskkill` Zugriff verweigert und schrieb
deshalb keine Summary-Datei; der Kill-on-close-Job beendete den Child trotzdem,
danach lief kein `katana-recomp`-Prozess mehr. Die Evidenz stammt aus
Supervisor-, stdout- und stderr-Logs; der Supervisorlog liegt unter
der Supervisorlog im privaten Diagnosebereich,
stdout/stderr tragen dasselbe Präfix.

Admission war `1024/1024`, `projected_context_changed=0` und
`projected_match_changed=0`. Der entscheidende Hotspot bleibt der sauberste Ordinary-Stack-Treiber
mit `84/84` Attempts/Semantic Changes und `508` Ordinary-Stack-Deltas trotz
vollstaendigem Stackvertrag. Die neue autoritative Hybridprojektion schliesst
Contextual-MAY-Joins und Forward-Edges erneut vollstaendig. Die vollstaendige
Hybrid-Join-Closure ist damit ein Korrektheitsfix ohne materielle Produkt-/
Performanceverbesserung;
beim vollstaendigen Stackvertrag/Gate ist die Closure noch nicht wirksam. Der naechste
Analysepunkt ist diese fehlende
Wirksamkeit; LocalStackCoordinate-/unvollstaendige Stackvertraege bleiben
sekundaer zu pruefen. Keine Budget-/Thread-Erhoehung und kein weiterer
SavedEpoch-/Provenienzumbau.

Der Gegenlauf auf Source-Commit `49cee39a93df1fae28a97d955a2d742132409dd1`
(`kr4981-20260809-083308-4a3ff9be`) lief `286,387 s` gesamt (Candidate ca.
`232,5 s`) bis Wave `119` mit `972` Lanes, `2.011` physischen, `2.814`
logischen Requests, `606` Input- und `360` stale-Requeues, `922` stale
Discards sowie Cache-Payload `610.295.241 B`. Diese Rohwerte sind wegen des
frueheren Endes des neuen Laufs bei Wave `107` nicht direkt vergleichbar;
normalisiert ist keine materielle Produkt-/Performance- oder
Konvergenzverbesserung belegt. Physical evaluations/Wave lagen leicht hoeher,
Cache-Payload/Wave eher hoeher, stale requeues/Wave etwas niedriger.

Der vorherige Lauf `kr4981-20260809-083308-4a3ff9be` endete nach `286,387 s`
(Candidate ca. `232,5 s`) nach drei zehnsekündigen Amplifikationssamples mit
Status `nonconvergence` und Wrapper-Exit `31`; kein Crash. Es gab `0/1274`
ready/completed/committed Roots, HOL Root `0`, Wave `119`, keinen Epoch-Publish
oder -Discard und kein Portartefakt bzw. `game.exe`. Peak Root WS:
`1.670.086.656 B`, Peak Job WS: `1.890.910.208 B`.

Final: `280` Contexts, `972` Semantic-Lanes, `2.011` physische Evaluationen,
`2.814` logische Requests, `972` Admissions, `203` Cache-Reuses, `2.790`
exakte Subscriber, Provenienz `169.824`; Requeues input-widening `606`,
summary-change `17`, forward-edge `128`, stale-dependency `360`, stale
snapshot discards `922`, semantic lane widenings `1.269`, provenance-only
widenings `1.711`, Frontier `43` (max `250`), Cache retained
`610.295.241 B`, globale Evaluationen/Cacheeinträge `5.157`. Kein Budget war
erschöpft; kein Fallback/FullState wurde verwendet.

Die Samples bewegten sich von Wave `93` (`280/960/1.640/678`, Cache
`539.097.507 B`) über Wave `103` (`280/961/1.810/756`, `574.167.905 B`) und
Wave `105` (`280/963/1.838/775`, `577.509.522 B`) zu Wave `119`.
Admission war `1024/1024` erfolgreich, aber
`projected_context_changed=0` und `projected_match_changed=0`: Inputs waren
upstream bereits kanonisch; der späte Projektionspunkt reduziert nichts.
Der Blocker ist intra-context semantic widening, nicht wachsende Contextzahl.

Failure-Signale: Context-Stackvertrag invalid `629`, Memory invalid `755`,
Candidate truncation `900`, ABI stack-base unresolved `852`, unresolved
SavedEpoch alias sources `625`, tracks-current `625`, current watcher `291`;
Top-Gründe local-stack-coordinate `594`, callee-top `110`, callee-set-incomplete
`14`. Der sauberste Ordinary-Stack-Treiber: `84/84`
Attempts/semantic changes, vollständiger Stack-Read-Vertrag und `508`
Stack-Ordinary-Widenings ohne Epoch-Topologie-Widening. Weitere incomplete
local-stack-coordinate-Sites sind mehrere weitere geprüfte lokale Stackkoordinaten.

Gegenüber dem vorherigen Lauf (`322,632 s`, Candidate `237,116 s`, Wave `39`,
`272` Contexts, `549` Lanes, `630` physische, `894` logische, `226` stale
Discards, Cache ca. `455,6 MB`) sind die Rohwerte wegen des frueheren Endes
nicht direkt vergleichbar. Eine materielle Produkt-/Performanceverbesserung ist
nicht belegt: weiterhin `0` Publikationen und fortgesetzte Amplifikation.

Der vorherige Lauf `kr4981-20260809-041945-21b70ade` endete nach `425,924 s`
(Candidate ca. `341 s`) bei Wave `60`, `0/1194` Roots, `758` Lanes, `984`
physischen und `1.398` logischen Auswertungen, `248` Input-, `102` stale-
Requeues und `347` Discards. Cache ca. `501 MB`, Peak Root
`1.606.066.176 B`, Peak Job `1.814.822.912 B`; kein Portartefakt.

Der Vergleichslauf `kr4981-20260809-050420-3f47fd65` endete nach `322,632 s`
(Candidate `237,116 s`) wegen belegter Nichtverbesserung: Wave `39`, `0/1194`
Roots, `272` Contexts, `549` Lanes, `630` physische, `894` logische
Auswertungen, `181` Input-, `10` Summary-, `76` stale-Requeues, `226`
Discards, Provenienz `31.713`, Cache `455.638.275 B`, maximale physische Dauer
`42,359 s`, Peak Root `1.490.157.568 B`, Peak Job `1.672.388.608 B`; kein
`game.exe`. Das `attempts=1024`-Gate war gegenüber `9baea88` bitgleich:
`admission_success=999`, `projected_context_changed=0`,
`projected_match_changed=0`. Die Gateänderung ist korrekt, aber kein
Konvergenzhebel. Historisch wurde dabei Inventory-Provenance-Live-in/
Spill-through als P0-Folgepunkt vermutet; der aktuelle P0 ist nun die fehlende
Wirksamkeit der autoritativen Hybrid-Join-Closure beim vollständigen
Stackvertrag/Gate. Dies ist ein historischer PlatformAbi-Befund.

Der D2048-Top-8-Befund war: Top-1 sem `36` mit reg ordinary `94`
(`mask B870`), reg metadata `4` (`mask 3860`) und state-memory Epoch-Topologie
`12`; Top-2 sem `36` mit reg ordinary `12` (`mask C010`), Alias-Flags
`16` und state-memory Topologie `2`; Top-3 sem `34` mit `48` Stack-
Events, incomplete Top-Chain, reg ordinary `8` (`mask 0F00`), Alias-Flags
`10` und Stack-Key-Topologie `6`; Top-4 sem `32` mit reg ordinary `80`
(`mask FE31`), metadata `16` (`mask 1E00`), Alias `8` und state-memory
Topologie `6`; Top-5 sem `28` mit reg ordinary `8` (`mask 0011`) und
Alias `14`; Top-6 sem `28`/Events `144` mit Alias `24`;
Top-7 sem `26` mit reg ordinary `40` (`mask 800F`), metadata `2`
(`mask 0004`) und state-memory Topologie `12`; Top-8 sem `22` mit reg
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
  20-Minuten-Grenzen.

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
Admission-/Stack-Diagnostik in den Vergleichsversuchen unterschiedliche Rohwerte,
aber weiterhin einen nicht konvergierenden semantischen
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
Produk­terfolg. KR-4994 ist source-seitig abgeschlossen; der aktuelle P0 ist die
fehlende Wirksamkeit der autoritativen Hybrid-Join-Closure beim vollständigen
Stackvertrag/Gate.

### KR-4994 - Begrenzter identitaetserhaltender unresolved Stack-/Context-Candidate-Carrier [x]

Source-seitig abgeschlossen. Der bounded-merge/Pending-Carrier-Fix bewahrt
Stack-/Storage-Identitaet in einem strikt begrenzten, monotonen und
identitaetsgebundenen Carrier und integriert ihn in Merge, Key/Cache, Lifetime,
ABI-/Summary-Propagation, Stack-may-load, Candidate-Recompute,
contextual/forwarded/stable Harvest und Export-Gate, ohne Scheduler-/Budget-
umbau, Fallback, Coverage-Reduktion oder Sonic-Hack. Der Zweikanalpfad haelt
Evidence-Stale in privaten Replaykapseln und trennt es vom logischen
Semantic-Lane-Key. Der historische offene P0 ist Inventory-Provenance-Live-in/
Spill-through.

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
36 mit dem SDK-Linkabschluss geschlossen. Nicht aktivierte KR-4988 bis KR-4991 wurden
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
- KR-4981 bleibt das globale Produktgate; der RuntimeOnly-Build-/Export-Gate
  ist bestanden, der beaufsichtigte Start bis mindestens Memory-Card-Screen
  bleibt offen, und ein weiterer Lauf ist nicht automatisch freigegeben.

Die globale Abwesenheit der Limitmetriken und von `IncompleteRoot` ist erst
im vollstaendigen KR-4981-Port beweisbar. D1 und D2 sind begrenzte
Diagnoseexporte und ersetzen diesen Produktnachweis nicht. Der historische
PlatformAbi-KR-4981-Versuch ist abgeschlossen und nicht bestanden; der
RuntimeOnly-Build-/Export-Gate ist davon getrennt. Ein weiterer Lauf benoetigt
eine ausdrueckliche Freigabe.

### KR-4981 - Sonic-Produktgate

Der historische D-Lauf war der freigegebene 24-Thread-NativeDisc-Sonic-Versuch;
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
  -> RuntimeOnly-Build-/Export-Gate bestanden
  -> beaufsichtigter Start bis mindestens Memory-Card-Screen offen;
     aktuell blockiert durch Gast-Presentation/Framebuffer-/PVR-Scanout
```

Die einzige D1-Evidenz ist ein unvollstaendiger, nichtterminaler Root-0-Lauf;
D1/G1 bleibt historisch unentschieden. D2/G2 ist abgeschlossen und negativ:
kein positiver Schedulerhebel. D9 ist
beendet und Root 0 konvergierte fail-closed ohne Erfolgsaussage; KR-4988 bis
KR-4991 bleiben inaktiv. KR-4994 ist source-seitig abgeschlossen; der historische
P0 ist die fehlende Wirksamkeit der autoritativen Hybrid-
Join-Closure beim vollständigen Stackvertrag/Gate. KR-4981 bleibt
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
