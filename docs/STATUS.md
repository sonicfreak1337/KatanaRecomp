# Projektstatus

Aktuelle interne Version: `v0.49.0`

## Repositoryweiter Arbeitsvertrag

Fuer jeden Task gilt projektweit:

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
als Abschlussbedingung gefordert.

Der reale Sonic-Adventure-PAL-Port ist der massgebliche Produkt- und
Integrationstest. Reviews duerfen fehlende neue Tests nicht als Finding
melden. Vorhandene Tests werden nur geprueft oder repariert, wenn sie selbst
konkret gebrochen, widerspruechlich oder zahlenmaessig falsch sind.

## Evidenztrennung

```text
letzte reale Produktevidenz:
  historische NativeDisc-/DirectBoot-Ports mit aelteren ABI-Vertraegen

Funktionaler Source-Checkpoint dieses Arbeitsstands:
  099ae2cb2dfe7699f90338e9df0bad24a7888823
  Analyzer-ABI 34, Function-Analysis-Epoch-Schema 19

aktueller realer Diagnosebefund:
  Sonic-v56 terminal nach 1:28:24 mit Exitcode 5
  1/1191 Resolution-Roots committed
  65.536 Contextual-Return-Evaluationen einer Funktion ausgeschoepft
  Context-Limit nicht erreicht
  25.728 eindeutige Contexts
  27.872 physische Auswertungen
  0 Eviction-Recomputes
  Epoch-Retention: incomplete-root
  kein Portartefakt, keine game.exe, kein Screenshot

aktueller Dokumentationsstand:
  Source-Tasks KR-4985/KR-4986/KR-4993/KR-4987/KR-4994 abgeschlossen; Produktgate offen
```

Source-, Diagnose- und Produktevidenz duerfen nicht als derselbe Fortschritt
ausgegeben werden. Die aktuellen Dokumentationscommits veraendern keine
Recompiler-, Runtime- oder Produktsemantik.

## Aktueller P0

Der terminale v56-Befund meldet null Eviction-Recomputes und liefert damit
keinen Beleg fuer Cache-Eviction als verbleibende Hauptursache. Ursache der
Explosion war, dass das Per-Function-Budget vor MultiRoot-, Cache- und
semantischer Deduplizierung pro exaktem Provenienzrequest belastet wurde.
Der Fix identifiziert Full-State-Semantic-Lanes kollisionssicher, trennt
Provenienzabonnenten und belastet das Budget nur bei neuer semantischer Lane.

Der aktuelle Zweikanal-Sourcefix vergleicht fuer die logische Contextual-
Lane den oeffentlichen Call-/State-Effekt ohne Evidence-Wachstum; die
alpha-normalisierte Evidence-Mitgliedschaft bleibt in begrenzten privaten
Provenienz-Replaykapseln fuer die physische Auswertung erhalten. Evidence-
Stale darf dadurch keine neue semantische Lane oder ein neues logisches
Budgetereignis erzeugen; Cap-/Replayfehler bleiben fail-closed.

Das `65.536`-Limit ist ein Per-Function-Budget; `25.728` Contexts und
`27.872` physische Auswertungen sind historische laufweite Aggregate und
werden nicht miteinander verrechnet.

Der Source-Fix ist fuer KR-4985/KR-4986 abgeschlossen. KR-4987 ist
source-seitig abgeschlossen: Die Read-Lens-projizierte Contextual-
SemanticLane-Identitaet verwendet vollstaendige Key-Bytes, bleibt bei
Vertragsluecke/Truncation/Fallback strikt FullState und erhaelt exakte
Provenienz/Restore sowie Discovery -> Freeze -> Publish. Nach dem Prozessende
war die temporaere JSONL bis Sequence `2266` bei `185,586 s` lesbar/gespuelt
(`2.267` Records, `10,8 MB`), aber ohne terminalen Datensatz und ohne atomare
Publikation; daraus folgt kein terminaler Produktabschluss. Es gab `348`
Candidate-Resolution-Records
von `9,371` bis `185,370 s`, zunaechst marker-only und danach ausschliesslich
fuer den zero-based Root 0. Root 1 wurde sicher nicht erreicht.

Der letzte belastbare nichtterminale D1-Snapshot bei `185,370 s` meldete
`running`, `0/1191` abgeschlossene Roots, Root 0, Wave `1.019`, Frontier `0`
bei maximal `223`, `288` zugelassene Contexts, `6.724` Evaluationen bzw.
logische Admissions, `15.170` logische Requests, `6.724` Semantic-Lanes,
`6.725` physische Auswertungen, `5.846` Cache-Reuses, `15.157` exakte
Subscriber und `226.886` Provenienzverknuepfungen. Requeues waren `1` initial
root, `287` neue exakte Lane, `8.248` Input-Widening, `177` Summary-
Aenderung, `405` Forward-Edge und `6.052` stale Dependency; stale Discards
lagen bei `12.643`. Semantic Widenings lagen bei `10.412`, provenance-only
Widenings bei `2.201`.

Die D1-Kosten meldeten Snapshot `15.170 / 2,950 s`, Key `15.160 / 5,124 s`,
inklusive Cache-Request `12.571 / 162,453 s`, inklusive Apply `63.742 /
17,790 s`, darin Binding-Merge `41.124 / 1,519 s`, Evidence `15.157 /
2,492 s`, serielle Commit-Operationen `1.018 / 0,000506 s` und
Publish-Operationen `1.018 / 0,008050 s`. Diese Operationszaehler sind keine
committed Resolution-Roots. Bindingzahl und Hitposition waren maximal jeweils
`1`; Full-State-Lanes und Projected-Physical-Keys jeweils `6.724`, Alpha-
Fallbacks `0`. Alle Context-/Evaluation-/Compositebudget-, IncompleteRoot-,
Retention-, Projected-/Classification- und allgemeinen Telemetrie-Degraded- /
Drop-Flags waren false; `telemetry_complete` war im letzten nichtterminalen
Progressdatensatz true.

D1/G1 ist damit strikt fail-closed und unentschieden: Der Transport und der
Root-0-Fortschritt sind valide nichtterminale Evidenz, aber der Supervisor-
Fehler, das fehlende terminale Atomic-Rename, `0/1191` abgeschlossene Roots
und der nicht erreichte historische Root 1 erlauben keine Entscheidung ueber
Candidate-Resolution-Gesamtzeit, Limitfreiheit, terminale
IncompleteRoot-/Retentionwerte, Coverage oder G1.

### D9-Produktbeobachtung

Der einmalige ueberwachte Sonic-Lauf dauerte `20,331 s` und endete beim ersten
fail-closed Telemetrie-/Publikationssignal. Root 0 erreichte Wave `184` und
Frontier `0` bei maximal `216`; der Prozessbaum ist sauber beendet, es gibt
kein Portartefakt und keinen Produkterfolg. Context-/Evaluation-/Composite-
Budgets blieben unverbraucht.

```text
contexts admitted:                         288
evaluations admitted / Semantic-Lanes:     2.724 / 2.724
logical requests / physical evaluations:   4.349 / 3.739
input-widening / stale-dependency requeues: 2.497 / 932
stale snapshot discards:                   1.740
semantic / provenance-only widenings:      939 / 2.377
final / maximum frontier:                  0 / 216
analysis epochs published / discarded:     0 / 1
retention:                                 incomplete-root
```

Der Root blieb fail-closed unvollstaendig: `local_fixpoint=0`,
`pending_regions=0`, alle Context-/Evaluation-/Budgetlimits `0`,
`candidate_values_truncated=1`, `abi_stack_base_unresolved=1`; alle anderen
Candidate-/Stack-/Table-Truncationflags blieben `0`. Der generische
telemetry-degraded-/Exit-34-Befund war die erwartete Folge des verworfenen
unvollstaendigen Roots, kein Haenger und kein separater Publikationsfehler.
64 Candidate-Truncation-Diagnosen waren ausschliesslich
`carrier=state`, `coordinate/domain=identity`, `values=0`; das terminale
Kandidatenbit stammt aus `inventory_stack_callback_loss_identity_truncated`.
6 Contextual-Value-Overflows erreichten jeweils `merged_values=9`. 462 Stack-Loss-
Diagnosen verteilten sich auf 189 forwarded-call, 158 candidate-store,
113 fixpoint-call und 2 forwarded-tail; tail-store-identity-loss blieb `0`.
Der naechste echte Engpass ist damit Stack-/Storage-Identitaetsverlust.

Eine Erhoehung des 65.536er-Budgets, mehr Cache oder mehr Threads ist kein
Fix. Die Arbeit muss semantisch reduziert und kausal korrekt eingeplant
werden, ohne Analyse-, Evidence- oder AOT-Abdeckung zu verlieren.

Der terminale Lauf meldet `1/1191`, daher ist Root 0 nicht mehr als
endgueltig gescheitert belegt. Bis Rootindex, Rootadresse und limitierte
Funktion terminal ausgegeben werden, gilt der Befund allgemein fuer die
ersten schweren Candidate-Resolution-Roots.

### Frueherer Vergleichslauf

Der korrekte VsDevCmd-Incremental-Build von `katana-recomp --parallel 12`
war in `42,8 s` erfolgreich; es blieben nur bekannte getenv-/Shadowing-
Warnungen. Run-ID: `kr4981-20260809-012851-0b360903`. Der fruehere Lauf
dauerte `460,6 s`; Candidate Resolution lief von
`00:37:17` bis `00:42:43` (ca. `325,8 s`). Dieser fruehere Lauf ist
Vergleichsevidenz; Summary `product-exit` bedeutet hier
nur, dass der exakt identifizierte Kindprozess nach belegter Nichtverbesserung
manuell beendet wurde. Es gab keine kanonische Publikation, `0/1194`
committed Roots, HOL `0`, Wave `103`, `272` zugelassene Contexts, `1.044`
Semantic-Lanes, `1.029` contextual physical evaluations und `2.430`
contextual logical requests.

Context-/Evaluation-/Composite-Budgets blieben unverbraucht.

Weitere D-Laufwerte: `1.359` Input-Widening-, `29` Summary- und `733`
stale-Dependency-Requeues, `1.359` stale snapshot discards,
`518.425.788 B` Cache-Payload, `3.964` physische Auswertungen gesamt sowie
`0/0` publizierte/verwarfene Analyseepochen. Ein Portartefakt oder `game.exe`
entstand nicht.

Bei Attempts `1024`, `2048` und `4096` waren die relevanten Admission-/Stack-
Diagnosezaehler bitgenau identisch zum vorherigen Fehlerlauf. Bei vergleichbarer
Gesamtzeit (~`459,6 s`) erreichte der neue Lauf jedoch Wave `103` statt `67`,
`1.044` statt `722` Semantic-Lanes, `1.029` statt `713` contextual physical
evaluations, `733` statt `839` stale requeues und `518.425.788` statt
`444.266.838 B` Cache-Payload. Der Pending-Carrier verbessert damit offenbar
Kosten je Churn-Schritt bzw. den Durchsatz, belegt aber keinen
Konvergenzhebel. Candidate-Resolution und KR-4981 bleiben offen; der aktuelle
P0 ist Inventory-Provenance-Live-in/Spill-through.

### Lauf nach Candidate-Domain-Top-Fix

Der Candidate-Domain-Top-Fix macht abgeschnittene begrenzte Candidate-Domains
zum kanonischen absorbierenden Top mit leerem endlichem Praefix und haelt Merge,
Normalisierung, Vergleich, Keys, Persistenz, Consumer und ABI-Promotion
konsistent. Der historische Candidate-Domain-Top-Lauf lief unter
Function-Analysis-Epoch-Schema `18` und Analyzer-ABI `33`; der aktuelle
Source-Checkpoint ist separat oben ausgewiesen.
Der korrekte VsDevCmd-Incremental-Compile+Link war erfolgreich.

Der einmalige Lauf `kr4981-20260809-020628-2bfd8af5` wurde nach `343,627 s`
durch manuellen Abbruch bei belegter identischer Nichtkonvergenz beendet. Die
Voranalyse bis Candidate-Start dauerte etwa `146 s` einschliesslich des
Gesamtstarts; letzte Bewegung war Wave `48`. Peak Root: `1.450.078.208 B`,
Peak Job: `1.618.132.992 B`; keine kanonische Publikation und kein
Portartefakt. Bei Wave `39` waren alle 16 geprueften Kernzaehler exakt wie im
Vorlauf: Frontier `177`, Contexts `272`, Semantic-Lanes `606`, physische
Auswertungen `645`, exakte Subscriber `870`, Provenienz `21.355`,
Input-Widening `263`, Summary `10`, Forward `123`, stale `95`, stale Discards
`299`, semantische Widenings `553` und provenance-only `382` sowie die
weiteren geprueften Kernzaehler. Der Fix ist damit als Korrektheits-/Persistenz-
fix belegt, nicht als Konvergenzhebel; KR-4981 bleibt offen.

### [x] Abgeschlossener Hot-Callee-Diagnoseunterauftrag

Der Lauf `kr4981-20260809-024141-c4ffdf15` erreichte das erste vollständige
`attempts=1024`-Diagnosegate und wurde nach `244,549 s` bei Wave `24` gezielt
beendet. Der erwartbare Supervisorstatus `product-exit -1` entstand durch
diesen Stop, nicht durch Fehler oder Hänger. Peak Root WS war
`1.260.388.352 B`, Peak Job WS `1.387.151.360 B`; keine Publikation und kein
`game.exe`. `uncategorized=0` für alle Top-8-Funktionen.

Der dominante Befund ist `0x8C10E44E`: `20` echte semantische Änderungen und
`40` Stack-Widenings, ausschließlich SavedEpoch-pending-ABI-Skalare
(`reg_epoch_pending=92`, `stack_epoch_pending=80`, `tail_epoch_pending=20`,
`state_stack_epoch_pending=20`, `state_memory_epoch_pending=20`), bei
unvollständigem Callee-Set-Stackvertrag (`owner=0x8C10E44E`,
`site=0x8C10E486`, `target=0`). Ordinary/direct-code/direct-PC/contextual,
Callback-Loss-, Topologie-, Top-Domain-, Map-/Tail-Topologie- und
Metadatenänderungen waren dort `0`. `0x8C09859C` zeigte `28` Änderungen,
ebenfalls Callee-Set-incomplete an `0x8C0985B0`, jedoch gemischte Domänen.
`0x8C64E55E` zeigte `48` Änderungen, darunter
`reg_epoch_pending=180`, bei vollständigem Stackvertrag.

Der Diagnose-Unterauftrag ist damit abgeschlossen; KR-4981 und das
Sonic-Produktgate bleiben ausdrücklich offen. Der SavedEpoch-Lifecycle-Fix ist
source-seitig abgeschlossen. Offen bleibt die gemeinsame Ordinary-/
Registermetadaten-/Alias-/Watcher-/Loss-/MemoryEpoch-Lifecycle-Ursache, ohne
Alias-/Current-Tracking oder fail-closed Restore zu verlieren. Die Callee-Set-
incomplete-Ursache an den dynamischen
Sites bleibt nach diesem Fix weiter zu prüfen.

### [x] Abgeschlossener SavedEpoch-Lifecycle-Unterauftrag

Current-tracking SavedEpoch-Pending-ABI-Skalare werden rekursiv nur an
bewiesenen normalen Call-/Tail-ABI-Gates konsumiert; detached Epochs bleiben
unangetastet. `candidate_payload_lost` ist physisch und semantisch ein
absorbierendes Epoch-Top ueber Normalize, Merge, Equality, Key, Subsumption,
Evidence, Restore und Persistenz. Konkrete Evidence sowie Nested-/Current-
Aliasfakten bleiben, finite Payload/Slots verschwinden; detached Top erhaelt
keine fremde Tail-Evidence. Der historische SavedEpoch-Lifecycle-Stand lief
unter Epoch-Schema `17` und Analyzer-ABI `33`.

Der Lauf `kr4981-20260809-031826-0616113a` endete nach `369,171 s` mit Status
`nonconvergence`, Exitcode `31`, durch drei zehnsekundige
Null-Publikations-Amplifikationssamples. Das war kein Crash oder Haenger:
Wave `76`, `0` committed/ready/completed Roots, `272` Contexts, Semantic-Lanes
`846 -> 863 -> 886`, physische Auswertungen `1.135 -> 1.164 -> 1.213`, Frontier
`101 -> 88 -> 131`, stale Discards `395 -> 396 -> 415`. Peak Root WS:
`1.663.037.440 B`, Peak Job WS: `1.895.583.744 B`; keine Publikation und kein
`game.exe`. D1024 und D2048: `uncategorized=0`. Der alte SavedEpoch-Pending-
Blocker ist beseitigt; der naechste Root-Analysepunkt ist die gemeinsame
Ordinary-/Registermetadaten-/Alias-/Watcher-/Loss- und MemoryEpoch-Lifecycle-
Ursache, nicht ein weiterer SavedEpoch-Pending-Patch. KR-4981 bleibt
fail-closed offen.

## Aktueller Source- und Laufstand

Der funktionale Source-Commit `099ae2cb2dfe7699f90338e9df0bad24a7888823`
erlaubt strukturelle Contextual-Hybrid-Projektion mit retained sticky loss,
erkennt SavedEpoch-Slot-Pending-Top in sämtlichen Truncation-/Publication-
Checks fail-closed und transportiert Provenance-Replay-Capsule-/Keybyte-Limits
öffentlich getrennt vom semantischen Evaluation-Limit. Ein echter Evaluation-
Cap belastet wieder nur den Evaluation-Zähler. Analyzer-ABI `34` und
Function-Analysis-Epoch-Schema `19` sind aktiv; der bestätigte Build war
erfolgreich, die EXE trug den Zeitstempel `05:00:45`, Tests wurden nicht
ausgeführt.

Der vorherige Lauf `kr4981-20260809-041945-21b70ade` endete nach `425,924 s`
(Candidate ca. `341 s`) bei Wave `60`, `0/1194` Roots, `758` Semantic-Lanes,
`984` physischen und `1.398` logischen Auswertungen, `248` Input-, `102` stale-
Requeues und `347` Discards. Cache ca. `501 MB`, Peak Root
`1.606.066.176 B`, Peak Job `1.814.822.912 B`; kein Portartefakt.

Der aktuelle Lauf `kr4981-20260809-050420-3f47fd65` wurde nach `322,632 s`
(Candidate `237,116 s`) wegen belegter Nichtverbesserung beendet: Wave `39`,
`0/1194` Roots, `272` Contexts, `549` Lanes, `630` physische, `894` logische,
`181` Input-, `10` Summary-, `76` stale-Requeues, `226` Discards,
Provenienz `31.713`, Cache `455.638.275 B`, maximale physische Dauer
`42,359 s`, Peak Root `1.490.157.568 B`, Peak Job `1.672.388.608 B`; kein
`game.exe`. Das `attempts=1024`-Gate war gegenüber `9baea88` bitgleich:
`admission_success=999`, `projected_context_changed=0`,
`projected_match_changed=0`. Die Gateänderung ist korrekt, aber kein
Konvergenzhebel. Der offene P0 ist Inventory-Provenance-Live-in/Spill-through
(unter anderem r12/SavedEpoch), nicht SavedEpoch-Pending und nicht Budgetarbeit.


## Aktueller kritischer Pfad

```text
KR-4985/KR-4986/KR-4993/KR-4987/KR-4994 source-seitig abgeschlossen
  -> D-Lauf beendet nach belegter Nichtverbesserung; Candidate-Resolution offen
```

KR-4992 bleibt ein optionaler Folgezweig nach einem verfehlten KR-4981 und
positivem Restkosten-/RAM-Gate. KR-4982 und KR-4983 bleiben gestrichen.

D1 und D2 sind reale Sonic-Diagnoseexporte, keine Testmatrix. D1/G1 bleibt
wegen der historischen, nichtterminalen Root-0-Evidenz unentschieden; D2/G2
ist abgeschlossen und negativ, ohne positiven Schedulerhebel. D9 ist beendet
und Root 0 konvergierte fail-closed,
ohne Portartefakt oder Produkterfolg. KR-4988 bis KR-4991 bleiben inaktiv;
KR-4994 ist source-seitig abgeschlossen; der aktuelle P0-Folgepunkt ist
Inventory-Provenance-Live-in/Spill-through. KR-4981 bleibt das globale Produktgate und ist
nicht bestanden.

## Quellseitig vorhandene Hauptvertraege

Der aktuelle funktionale Source enthaelt unter anderem:

- statische Guarded-AOT-Einstiege und fail-closed
  Exportvollstaendigkeitsvertraege;
- getrennte semantische und inventorybezogene Analysepfade;
- inkrementelle ProgramGraph-, SCC-, ABI-, Summary- und Candidate-
  Strukturen;
- gemeinsame Analyseexecutor- und Speicherhaushaltsvertraege;
- schichtweise Analyse-, IR-, Codegen- und Hostbuildcaches;
- exakte Latent-AOT-Hints und Multi-Extent-SourceBindings;
- baseline- und bildinhaltsgebundene sichtbare Frameklassifikation;
- relatives Post-Entry-Produktgate und typisierte Fehlerausgaenge;
- vorbereiteten atomaren CompletePlatform-Apply;
- save-erhaltendes ProductHandoff-Profil;
- statische native Produktmaterialisierung ohne Interpreter oder JIT.

Diese Sourcevertraege sind fuer den aktuellen Stand nicht produktseitig
abgenommen, weil v56 kein Portartefakt erzeugte.

## Offene Produktabnahmen

- Candidate-Resolution ohne Context-/Evaluationslimit und ohne
  `incomplete-root`;
- vollstaendiger aktueller NativeDisc-Port;
- bekannter historischer Missing-AOT-Pfad durch statisches AOT passiert oder
  engerer typisierter Blocker;
- korrekter terminaler Produktbericht und Child-Exitcode;
- frischer ABI-passender CompletePlatform-Capture und ProductHandoff;
- NativeDisc-/DirectBoot-Paritaet am Game Entry;
- sichtbarer Spielframe statt technischer Hilfsmetrik;
- vollstaendiger Kaltport in hoechstens acht Minuten;
- mindestens 200 MHz im normalen Produktpfad;
- externes Spielprojekt ohne Retaildaten oder Sonic-Sonderfaelle im
  Katana-Kern.

## Test- und Reviewstatus

Projektweit gilt ab jetzt:

- Gefixt wird mit Reviews der vollstaendigen betroffenen Pfade.
- Getestet wird mit Sonic an den geplanten Produktgates.
- Keine neuen Tests, Testmatrizen, synthetischen Fixtures oder Ersatzgates.
- Fehlende neue Tests werden in Reviews nicht beanstandet.
- Vorhandene Tests und Testzahlen werden nur bei konkretem Fehler repariert.

Historische Angaben zu frueher ausgefuehrten Tests bleiben historische
Evidenz und erzeugen keine neue Pflicht fuer den aktuellen Arbeitsablauf.

## Naechster Schritt

```text
D9 ist beendet und fail-closed; Root 0 konvergierte ohne Portartefakt und
Produkterfolg. KR-4994 ist source-seitig abgeschlossen; der aktuelle
P0-Folgepunkt ist Inventory-Provenance-Live-in/Spill-through. KR-4981 bleibt
das globale Produktgate und ist
nicht bestanden.
```

Ein zweiter D1-Lauf gehoert nicht zu diesem Dokumentationspass. D2/G2 ist
abgeschlossen und negativ; KR-4987 bis KR-4991 bleiben inaktiv.
