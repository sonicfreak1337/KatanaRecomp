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

Sourcebasis dieses Arbeitsstands:
  dd3ff7eccec5c3f0c6308ee44c315fb2f6bf55fa
  plus reviewtes KR-4994-Source-Delta in diesem Task
  Analyzer-ABI 33, Function-Analysis-Epoch-Schema 15

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

### Aktueller D-Lauf

Der einmalige D-Lauf dauerte `460,6 s`; Candidate Resolution lief von
`00:37:17` bis `00:42:43` (ca. `325,8 s`). Summary `product-exit` bedeutet hier
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
Kosten je Churn-Schritt bzw. den Durchsatz, entfernt den semantischen
Lane-Treiber aber nicht. Candidate-Resolution und KR-4981 bleiben offen.

## Aktueller kritischer Pfad

```text
KR-4985/KR-4986/KR-4993/KR-4987/KR-4994 source-seitig abgeschlossen
  -> D-Lauf beendet nach belegter Nichtverbesserung; Candidate-Resolution offen
```

KR-4992 bleibt ein optionaler Folgezweig nach einem verfehlten KR-4981 und
positivem Restkosten-/RAM-Gate. KR-4982 und KR-4983 bleiben gestrichen.

D1 und D2 sind reale Sonic-Diagnoseexporte, keine Testmatrix. D1/G1 bleibt
wegen der historischen, nichtterminalen Root-0-Evidenz unentschieden; D2/G2
wurde nicht ausgefuehrt. D9 ist beendet und Root 0 konvergierte fail-closed,
ohne Portartefakt oder Produkterfolg. KR-4988 bis KR-4991 bleiben inaktiv;
KR-4994 ist source-seitig abgeschlossen; der semantische Lane-Treiber bleibt
als P0-Produktblocker offen. KR-4981 bleibt das globale Produktgate und ist
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
Produkterfolg. KR-4994 ist source-seitig abgeschlossen; der semantische
Lane-Treiber bleibt offen. KR-4981 bleibt das globale Produktgate und ist
nicht bestanden.
```

Ein zweiter D1-Lauf, D2/G2 und eine Aktivierung von KR-4987 bis KR-4991 gehoeren
nicht zu diesem Dokumentationspass.
