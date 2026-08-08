# KatanaRecomp Task-Katalog

Dieses Dokument enthaelt die aktiven `v0.49`-Produktaufgaben. Historische
Aufgaben und fruehere Detailstaende bleiben in Git und in
`TASK_ID_REGISTRY.md` nachvollziehbar.

## Repositoryweiter Taskvertrag

Fuer jeden Task gilt ohne zusaetzliche Zwischenstufe:

```text
Task implementieren
  -> alle betroffenen Pfade reviewen
     und bestaetigte Fehler innerhalb des Reviews schliessen
  -> den reviewten Task direkt auf main committen und pushen
  -> naechster Task
```

Verbindlich ist dabei:

- `AGENTS.md` gilt fuer jeden Task und jeden automatisierten Bearbeiter;
- ein Task wird direkt auf `main` bearbeitet und gepusht;
- Branches oder Pull Requests entstehen nur auf ausdrueckliche
  Nutzeranweisung;
- die Reviewstufe umfasst Implementierung, Aufrufer, Verbraucher, Datenfluss,
  Verdrahtung, Fehlerpfade, ABI-, Cache-, Versions-, AOT-, Runtime- und
  Produktvertraege der Aenderung;
- bestaetigte Fehler im Taskscope werden vor dem Push geschlossen;
- es gibt keine separate standardmaessige Test-, Verifikations-, Fix- oder
  Integrationsrunde zwischen Review und Push;
- erst der Push des reviewten Tasks gibt den naechsten Task frei;
- der Push ist die Freigabe; der naechste ungegatete Task benoetigt keine
  weitere Nutzeranweisung;
- ein Review darf ausserhalb des Taskscopes liegende Beobachtungen notieren,
  daraus aber nicht eigenmaechtig neue Tasks oder Scope ableiten.

## Sonic ist der Test

Oberste Prioritaet ist ein lauffaehiger Sonic-Adventure-PAL-Produktport. Der
reale Sonic-Port ist der massgebliche Produkt- und Integrationstest:

```text
Export -> Installation aus der Originaldisc -> normaler Lauf ->
sichtbarer Fortschritt
```

Daher gilt projektweit:

- keine neuen Unit-Tests, Regressionstests, Testmatrizen, synthetischen
  Fixtures, Stresslaeufe, Testprojekte, Ersatzgates oder
  Konformitaetssuiten;
- das Fehlen neuer Tests ist kein Review-Befund;
- Reviews verlangen keine neue Testabdeckung als Abschlussbedingung;
- vorhandene Tests duerfen auf gebrochene Erwartungen, falsche Testzahlen
  oder widerspruechliche Semantik geprueft und bei Bedarf repariert werden,
  ihr Bestand wird aber nicht erweitert;
- ein Task startet keinen eigenen Testbuild und keine Matrix als Pushgate;
- Sonic-Laeufe erfolgen an den unten festgelegten Produktgates oder auf eine
  ausdrueckliche Nutzeranweisung, nicht nach jedem Task;
- mehrere zusammenhaengende, reviewte Tasks duerfen vor dem naechsten
  Produktlauf auf `main` landen;
- DirectBoot besitzt keinen Sega-Screen als Pflichtmeilenstein, weil dieses
  Bild zu IP.BIN gehoert;
- kein Interpreter, JIT oder Emulationsfallback im normalen Produktpfad;
- keine Sonic-Adressen, Titelhooks, Retailbytes oder kommerziellen Inhalte im
  generischen Katana-Code.

## Lauf- und Ressourcenvertrag

- Kein Prozess und keine einzelne Phase laeuft laenger als 15 Minuten, ausser
  der Nutzer hebt die Grenze fuer genau einen benannten Lauf auf.
- Jeder potenziell lange Prozess besitzt spaetestens alle zehn Sekunden einen
  belastbaren Fortschrittsindikator.
- Liveness ohne kanonischen Fortschritt ist kein Erfolg. Stalls und
  Nichtkonvergenz werden nach dem Vertrag in `AGENTS.md` beendet.
- Das Per-Function-Budget von `65.536` Contextual-Return-Evaluationen wird
  nicht als Performancefix erhoeht. Der aktuelle P0 muss durch weniger
  notwendige Arbeit geschlossen werden.

## Getrennte Evidenzstaende

```text
letzte reale Produktevidenz:
  historische NativeDisc-/DirectBoot-Ports mit aelteren ABI-Vertraegen

Sourcebasis dieses Arbeitsstands:
  dd3ff7eccec5c3f0c6308ee44c315fb2f6bf55fa
  plus reviewtes KR-4994-Source-Delta in diesem Task
  Analyzer-ABI 33, Function-Analysis-Epoch-Schema 15

aktueller Diagnosebefund:
  Sonic-v56 endete nach 1:28:24 mit Exitcode 5
  1/1191 Roots committed
  65.536 Contextual-Return-Evaluationen einer Funktion ausgeschoepft
  Context-Limit nicht erreicht
  25.728 eindeutige Contexts
  27.872 physische Auswertungen
  0 Eviction-Recomputes
  Epoch-Retention: incomplete-root
  kein Portartefakt, keine game.exe, kein Screenshot
```

Der v56-Lauf ist Diagnoseevidenz, kein Produktnachweis. Historische Ports sind
kein Beweis fuer den aktuellen Source. Die `65.536` Evaluationen sind ein
Per-Function-Budget, `25.728` Contexts und `27.872` physische Auswertungen
sind laufweite Aggregate. Bis KR-4985 dieselben Root-, Funktions- und
Zaehlscope-Dimensionen ausgibt, duerfen daraus weder `37.664` vermeintlich
physiklose Evaluationen noch `2,547` logische Evaluationen je Context als
Messwert abgeleitet werden.

## Verbindliche Reihenfolge

```text
KR-4985, KR-4986, KR-4993, KR-4987 und KR-4994: source-seitig abgeschlossen
  -> D-Lauf beendet nach belegter Nichtverbesserung; Candidate-Resolution offen
```

Jeder Task in dieser Kette folgt einzeln:

```text
implementieren -> betroffene Pfade reviewen und Findings schliessen -> main
```

D1 und D2 sind begrenzte reale Sonic-Diagnoseexporte, keine Testmatrix. Der
einzige freigegebene D1-Lauf war nichtterminal; D1/G1 bleibt fail-closed und
unentschieden. D2/G2 wurde nicht ausgefuehrt. D9 ist beendet und Root 0
konvergierte fail-closed ohne Portartefakt oder Produkterfolg. KR-4987 bis
KR-4991 bleiben inaktiv; KR-4994 ist source-seitig abgeschlossen. KR-4981
bleibt das globale Produktgate und ist nicht bestanden. KR-4982 und KR-4983
bleiben gestrichen.

---

## [ ] KR-4972 - Hashgebundene Shared-Callback-/Thunk-AOT-Coverage

Prioritaet: P0 Boot

Status: Der allgemeine Guarded-AOT-Entry- und
Exportvollstaendigkeitsvertrag ist quellseitig vorhanden. Ein aktueller
Produktnachweis fehlt, weil Candidate-Resolution vor dem Portexport endet.

### Noch offen

- Candidate-Resolution ohne Context-/Evaluationslimit und ohne
  `incomplete-root` abschliessen;
- danach die bekannten historischen indirekten Ziele durch validiertes
  statisches AOT passieren oder einen engeren typisierten Blocker belegen;
- keine titelbezogene Adresse als generischen Fix verwenden;
- kein Interpreter, JIT, Runtime-Dekoder oder Emulationsfallback.

### Abschluss

Der Task bleibt bis zum erfolgreichen KR-4981-Produktlauf offen. Die
Quellpfade werden in den Tasks KR-4985 bis KR-4993 geschlossen und reviewt.

---

## [ ] KR-4979 - Priorisierter Analyseexecutor und Speicherhaushalt

Prioritaet: P0 Performance

Status: Der gemeinsame Executor ist quellseitig implementiert. Die historische
v56-Ausgabe besass noch keinen gemeinsamen Scope fuer Contextidentitaet,
Wiederzulassung, Per-Context-Kosten und kritischen Span; die Source-Trennung
ist durch KR-4985/KR-4986 abgeschlossen, waehrend D1/G1 unentschieden bleibt.

### Abschlussbedingungen

- kein Root endet am Contextual-Evaluationslimit oder an `incomplete-root`;
- reale unabhaengige Arbeit nutzt den Executor, echte Breite-1-Ketten werden
  als kritischer Span und nicht als Executorfehler berichtet;
- RAM-Druck erzeugt kein semantisches Truncation;
- Produktwirkung wird in KR-4981 am Sonic-Port gemessen.

---

## [ ] KR-4980 - Schichtweiser persistenter NativeDisc-Buildcache

Prioritaet: P0 Performance

Status: Quellseitig implementiert. v56 belegt, dass weitere Cachekapazitaet
ohne belegte Eviction-Recomputes keine begruendete Hauptloesung ist. Die
historische v56-Ausgabe besass noch keinen gemeinsamen Scope fuer die
Budgetzaehldomaene; KR-4985 ist abgeschlossen, D1/G1 bleibt unentschieden.

### Abschlussbedingungen

- fehlende, alte oder beschaedigte Shards bleiben sichere Misses;
- lokale Aenderungen invalidieren nur semantisch gebundene Ebenen;
- Produktwirkung und Warmexportzeit werden erst im Sonic-Produktpfad
  bewertet.

---

## [x] KR-4985 - Candidate-Resolution-Phasen- und Kardinalitaetstelemetrie

Prioritaet: P0 Performance-Diagnose

Abhaengigkeiten: KR-4974, funktionaler Source-Checkpoint
`0ae993f8f59db1fc866ce5e77874015b610a8bd5`

Status: Source-seitig abgeschlossen durch den gemeinsamen Candidate-Resolution-
Explosionsfix. D1-Telemetrie ist explizit opt-in produktiv; die begrenzte
Produktevidenz blieb wegen vorzeitigem Supervisor-Abbruch unvollstaendig und
entscheidet weder D1/G1 positiv noch negativ.

### Ziel

Den limitierenden Root und die limitierte Funktion identifizieren und die
Kosten von Snapshot, Cache-Key, physischer Auswertung, `apply_call()`, Merge,
Evidence und Commit getrennt sichtbar machen.

### Umfang

- Rootindex, Rootadresse, Funktionsadresse, Wellenindex und Frontierbreite;
- logische Evaluationen, physische Auswertungen, Cache-Reuse ohne physische
  Arbeit, Context- und Evaluationsbudgets;
- neue, verbreiterte und erneut zugelassene Lanes;
- Requeue-Ursachen getrennt nach initial root, neuer exakter Lane,
  Input-Widening, Summary-Aenderung, Forward-Edge-Insert/Widening und stale
  Dependency-Version;
- Snapshot-, Key-, Auswertungs-, Merge-, Evidence- und Commitzeit;
- Bindingzahl, Hitposition, Equality-/Copy-/Mergearbeit und Stategroesse;
- aggregierte Full-State-, Projected-Lens- und Provenienz-Digests;
- keine Rohstates, Gastwerte oder per-Lane-Retaillogs.
- D1-sichere Resultatannahme: Dependency-/Snapshot-Version wird vor
  `item.error` und vor jeder terminalen Publikation geprueft; ein veraltetes
  Batchresultat wird verworfen und gezielt neu eingeplant, statt den Root
  mit seinem inzwischen gegenstandslosen Fehler zu beenden;
- Cancellation-, Fehler- und Stale-Reihenfolge in allen Resultatpfaden.

### Review- und Abschlussvertrag

- Telemetrie darf die kanonische Semantik nicht veraendern;
- Drop-, Vollstaendigkeits- und Budgetpfade werden im Quellreview verfolgt;
- stale oder gecancelte Resultate koennen weder Summary/Evidence publizieren
  noch ueber `item.error` einen aktuellen Root terminal beenden;
- D1 wurde einmal freigegeben, erreichte aber weder den historisch
  limitierenden Root noch einen vollstaendigen schweren Root; D1/G1 bleibt
  daher unentschieden;
- KR-4987 bis KR-4991 werden durch diesen unvollstaendigen Lauf nicht aktiviert;
- keine neue Telemetrie-Testmatrix oder synthetische Ersatzabnahme.

---

## [x] KR-4986 - Semantische Context-Lanes und exakte Provenienzabonnenten

Prioritaet: P0 Korrektheits-Enabler

Abhaengigkeit: KR-4985

Status: Source-seitig abgeschlossen durch den gemeinsamen Explosionsfix. Die
Full-State-Semantik bleibt autoritativ; exakte Provenienz wird getrennt
replayed. Keine Read-Lens-Aktivierung und keine reduzierte Analyseabdeckung
wurden daraus abgeleitet.

### Ziel

Physische Fixpunktsemantik von exakten Contributions, Callsites, Callees und
Evidence-Tokens trennen, zunaechst mit unveraenderter Full-State-Identitaet.

### Umfang

- semantische Lane plus kanonische Menge exakter Provenienzabonnenten;
- stabiler Full-State-Key aus Funktion, Evaluation-Lens, Contract und
  Ingress-Content;
- Summary- und Dependency-Versionen dienen der Stale-Validierung, nicht der
  Erzeugung immer neuer Lane-Identitaeten;
- getrenntes Accounting fuer logische Requests, semantisch eindeutige Lanes,
  physische Auswertungen und Provenienzlinks;
- exaktes Replay isolierter, forwarded und rootkorrelierter Evidence.

### Review- und Abschlussvertrag

- normalisierte Summaries, Resolutions, Guarded Inventory, Evidence und
  Budgetflags bleiben unveraendert;
- unvollstaendige Rootarbeit wird nie als wiederverwendbare Epoche
  publiziert;
- keine Read-Lens-Projektion vor belegter Full-State-Paritaet im Review;
- keine neue Worker-, Cache- oder Differentialtestmatrix.

---

## [x] KR-4987 - Read-Lens-projizierte Context-Identitaet

Prioritaet: P0 Performance

Abhaengigkeit: KR-4986; source-seitige Aktivierung durch den freigegebenen
KR-4987-Fixpass

Status: Source-seitig abgeschlossen am funktionalen Source-Checkpoint
`594f0191b321bd2f470d0aa07100e82f3eea956f` plus dieser KR-4987-Aenderung.
Der gezielte `katana-recomp`-Build war laut Sol-Review in `42,4 s` erfolgreich.
D9 ist beendet und wird nur als fail-closed, nicht erfolgreicher Lauf
dokumentiert; kein Produkt- oder G1-Erfolg wird behauptet.

D9-Beobachtung: `20,331 s`, Root 0 bis Wave `184`, Frontier `0` (maximal
`216`), `288` admitted contexts, `2.724` admitted evaluations/Semantic-Lanes,
`4.349` logical requests, `3.739` physical evaluations, `2.497` input-
widening und `932` stale-dependency requeues, `1.740` stale snapshot
discards, `939` semantic und `2.377` provenance-only widenings. Der Lauf
endete fail-closed am unvollstaendigen Root; kein Portartefakt und kein
Produkt-, G1- oder Limit-Erfolg wird behauptet.

Aktueller D-Lauf: `460,6 s` gesamt, Candidate Resolution ca. `325,8 s`,
manuelles Beenden des identifizierten Kindprozesses nach belegter
Nichtverbesserung, `0/1194` committed Roots, HOL `0`, Wave `103`, `272`
Contexts, `1.044` Semantic-Lanes, `1.029` contextual physical evaluations,
`2.430` contextual logical requests, `1.359` Input-Widening-, `29` Summary-
und `733` stale-Dependency-Requeues, `1.359` stale snapshot discards,
`518.425.788 B` Cache-Payload, `3.964` physische Auswertungen gesamt und
`0/0` publizierte/verwarfene Epochen. Context-/Evaluation-/Composite-Budgets
blieben unverbraucht; kein Portartefakt oder `game.exe`.
Attempts `1024`, `2048` und `4096` hatten bitgenau gleiche relevante
Admission-/Stack-Diagnostik wie der vorherige Fehlerlauf; der neue Durchsatz
verbessert Kosten je Churn-Schritt, beseitigt den semantischen Lane-Treiber
aber nicht. KR-4981 bleibt offen.

### Ziel

Contexts vor der Lane-Erzeugung zusammenlegen, wenn der konkrete Callee
vollstaendig bewiesen denselben semantischen Eingang liest und sich die
Inputs nur in ungelesenem State oder Provenienz unterscheiden.

### Umfang

- SemanticContextKey aus Funktion, Lens, Contract-Fingerprint und
  projiziertem Ingress;
- vollstaendige Key-Bytes fuer kollisionssichere SemanticLane-Identitaet;
- vollstaendige Register-, Stack-, Memory-, Alias-, Output-, Inventory- und
  Fallback-Watcher;
- zwingender FullState-Fallback bei jeder Vertragsluecke;
- FullState bei Truncation und Read-Lens-Fallback;
- Rebucketing bei spaet erweitertem Read-Vertrag;
- stabile Content-Digests plus strukturelle Kollisionspruefung;
- Provenienz bleibt ausserhalb der semantischen Lane-Identitaet.

### Review- und Abschlussvertrag

- gelesene Unterschiede bleiben getrennt, ungelesene duerfen teilen;
- Completeness, Truncation und exakte Evidence bleiben erhalten;
- die Reduktion erfolgt vor Lane-Erzeugung und nicht erst durch einen
  spaeten Cachetreffer;
- exakte Provenienz und Restore sowie Discovery -> Freeze -> Publish bleiben
  unveraendert;
- D9 ist beendet und ersetzt keine Produktabnahme, D2/G2 oder KR-4981.

---

## [x] KR-4994 - Begrenzter identitaetserhaltender unresolved Stack-/Context-Candidate-Carrier

Prioritaet: P0 Candidate-Resolution-Korrektheit

Status: Source-seitig abgeschlossen am reviewten Delta auf Basis
`dd3ff7eccec5c3f0c6308ee44c315fb2f6bf55fa`. Der begrenzte Pending-Carrier
bewahrt identitaetsgebundene Payloads ueber Merge, Key/Cache, Lifetime,
ABI-/Summary-Propagation, Stack-may-load, Candidate-Recompute und
contextual/forwarded/stable Harvest sowie Export-Gate. Der D-Lauf belegt
weiterhin den semantischen Lane-Treiber als offenen Produktblocker.

Vertrag:

- einen strikt begrenzten, monotonen und identitaetsgebundenen unresolved
  Stack-/Context-Candidate-Carrier einfuehren;
- ihn vollstaendig in Merge, Key/Cache, Lifetime, ABI-/Summary-Propagation,
  Stack-may-load, Candidate-Recompute sowie contextual/forwarded/stable
  Harvest und Export-Gate integrieren;
- keinen Scheduler- oder Budgetumbau, keinen Fallback, keine Coverage-
  Reduktion und keinen Sonic-spezifischen Hack einfuehren;
- ein neuer Produktlauf bleibt bis zur naechsten ausdruecklichen Freigabe
  nach diesem Review gesperrt.

---

## [ ] KR-4988 - Internierte AbstractStates und Function-Value-Summaries

Prioritaet: bedingtes P1 Performance

Abhaengigkeiten: positives, durch D1 ermitteltes G1-Kostengate, KR-4986

### Ziel

Tiefe Equality-, Copy- und Keyarbeit an unveraenderlichen Snapshots durch
kanonische Identitaeten senken.

### Umfang

- StateId/SummaryId nur fuer immutable Snapshots;
- Content-Hash mit struktureller Kollisionspruefung;
- prozesslokale Identitaet getrennt von persistentem Content-Digest;
- gebundene Lebenszeit, RAM-Haushalt und Retentiontelemetrie.

### Review- und Abschlussvertrag

- mutable Fixpunktzustande werden nicht in-place interniert;
- persistente Keys verwenden nie nur prozesslokale IDs;
- kein falscher Equality-Fastpath und keine ungebundene Retention;
- D2 und Sonic bewerten die Wirkung, keine neue Interning-Testmatrix.

---

## [ ] KR-4989 - Indexierte exakte Context-Bindings

Prioritaet: bedingtes P1 Performance

Abhaengigkeiten: positives, durch D1 ermitteltes G1-Bindinggate, KR-4986

### Ziel

Exakte Binding-Treffer in grossen Familien direkt finden, ohne den
konservativen Join-/Subsumption-Fallback zu veraendern.

### Umfang

- StateId oder kollisionsgepruefter Fingerprint auf kanonische
  Binding-Indizes;
- vollstaendige Equality-Pruefung nach Indexlookup;
- atomare Indexpflege bei Insert, Widening und Generationwechsel;
- linearer Fallback bei Miss und fuer kleine Familien.

### Review- und Abschlussvertrag

- Kollision, Widening, Invalidation und Full-Miss bleiben korrekt;
- Merge-/Subsumptionssemantik bleibt unveraendert;
- D2 und Sonic bewerten die Wirkung, keine neue Binding-Matrix.

---

## [ ] KR-4990 - Inkrementelle Contextual-Dependency-Views

Prioritaet: bedingtes P1 Performance

Abhaengigkeiten: positives, durch D1 ermitteltes G1-Kosten-/Reusegate,
KR-4986

### Ziel

Unveraenderte Adjazenz, Bindings, Summary-Versionen und Evidence-Layouts
nicht bei jeder Reevaluation vollstaendig neu aufbauen.

### Umfang

- generationierte immutable Views nach Funktion/Lane, Edge, Binding und
  Summary-Aggregat;
- nur geaenderte View-Shards ersetzen;
- kanonischer inkrementeller Digest;
- Full-Rebuild bei unbekannter Invalidierung oder hoher Aenderungsrate;
- Referenzen nur in schreibfreier Phase, owning Materialisierung vor
  Besitzuebergang.

### Review- und Abschlussvertrag

- Add/Widen, Summary-only-Aenderung, stale Entfernung und Zyklen
  invalidieren exakt;
- globale und kontextuelle Fallbacks bleiben gebunden;
- D2 und Sonic bewerten die Wirkung, keine neue View-Differentialmatrix.

---

## [ ] KR-4991 - Versionierte monotone Context-Worklist

Prioritaet: bedingtes P0 Performance/Scheduling

Abhaengigkeiten: KR-4986, alle aktivierten KR-4987 bis KR-4990, D2 und
positives G2

### Ziel

Kausal freigesetzte unabhaengige Contextarbeit ohne globale
Jacobi-Wellenbarriere starten, ohne stale oder nichtkanonische Ergebnisse zu
publizieren.

### Umfang

- immutable Worker-Snapshot plus Dependency-Versionen;
- private semantische Deltas und versionierter kanonischer Commit;
- sofortiges Enqueue neu aktivierter Lanes;
- jede Wiederzulassung besitzt genau einen typisierten kausalen Grund;
- unveraenderte Ingress-, Summary-, Edge-, Evidence- und
  Dependency-Versionen erzeugen keinen neuen semantischen Budgetverbrauch;
- stale Ergebnisse werden verworfen und gezielt neu geplant;
- begrenztes Retry-/Ressourcenbudget mit konservativem Jacobi-/Seriell-
  Fallback;
- Jacobi bleibt bis KR-4993 der Rollbackpfad.

### Review- und Abschlussvertrag

- stale Summary oder Evidence kann nie committed werden;
- semantische Budgets bleiben deterministisch und fail-closed;
- echte Breite-1-Ketten werden nicht als parallelisierbar ausgegeben;
- kein Root scheitert allein durch Wiederzulassung unveraenderter Versionen
  am 65.536er-Budget;
- D2 und Sonic bewerten Schedulingwirkung, keine neue Thread- oder
  Worklistmatrix.

---

## [x] KR-4993 - Abschlussreview der Candidate-Resolution-Pfade

Prioritaet: P0, letztes Sourcegate vor KR-4981

Abhaengigkeiten: KR-4985, KR-4986 und alle durch G1/G2 aktivierten Tasks bis
KR-4991

Status: Source-seitig abgeschlossen am funktionalen Source-Checkpoint
`594f0191b321bd2f470d0aa07100e82f3eea956f` mit diesem Abschlusscommit.
Der vollstaendige Sol-Endreview
des unmittelbar vorherigen Explosionsbug-Diffs wurde wiederverwendet; alle
bestaetigten Findings sind geschlossen; das bisher offene Analyzer-ABI-
Finding wurde im vorherigen Fixcommit durch ABI 32 geschlossen. Nicht aktivierte
KR-4988 bis KR-4991 wurden nicht als geaendert oder reviewpflichtig behauptet.

### Ziel

Der vollstaendige Endreview der aktivierten/geaenderten Context-, Cache-,
Evidence- und Budgetpfade sowie die Pruefung der unveraendert konservativen
FullState-, Binding-, Dependency- und Scheduling-Fallbackgrenzen ist
abgeschlossen; alle vorher bestaetigten Findings wurden vor `0ae993f`
geschlossen; das Analyzer-ABI-Finding wurde im vorherigen Fixcommit mit ABI 32
geschlossen.

### Umfang

- semantische Identitaet und exakte Provenienz;
- FullState-Fallback, Digests, Kollisionen und Cache-Key-Schema;
- globale und kontextuelle Fallback-Summaries;
- Invalidierung, Delta-Monotonie, Stale und Evidence-Publikation;
- Cancellation, Budgets, RAM, Retention und Progress;
- logische Zulassung, semantische Lane, physische Auswertung, Cache-Reuse,
  Requeue und Provenienzabonnent als getrennte Zaehldomaenen;
- D1-/D2-Befunde konsistent zusammenfassen.

### Quellseitige Freigabebedingungen

- alle bestaetigten Findings geschlossen und erneut in den betroffenen
  Pfaden reviewt;
- keine reduzierte Analyse-, Resolution-, Guarded-AOT- oder
  Completenessabdeckung;
- Context-, Evaluations- und logische Rootbudgets bleiben autoritativ,
  fail-closed und terminal typisiert;
- `IncompleteRoot` kann weder publiziert noch als wiederverwendbare Epoche
  behalten werden;
- Stale-, Cancellation- und Fehlerreihenfolge kann weder alte Resultate
  publizieren noch aktuelle Arbeit durch einen veralteten Fehler beenden;
- jeder schwere Root ist terminal identifizierbar;
- kein neuer Test, keine Testmatrix und kein Produktlauf in KR-4993;
- KR-4981 bleibt nach KR-4994 und Sol-Review ein genau einmaliges globales
  Produktgate.

D1 und D2 sind begrenzte Diagnoseexporte und decken nicht zwingend alle
`1.191` Roots ab. Die globale Abwesenheit von
`contextual_return_context_limited_functions`,
`contextual_return_evaluation_limited_functions`,
`resolution_root_logical_budget_exhausted` und `IncompleteRoot` ist deshalb
keine beweisbare KR-4993-Bedingung. Sie wird erst in KR-4981 am
vollstaendigen Produktport abgenommen.

---

## [ ] KR-4981 - Einmaliges 24-Thread-Sonic-Produktzeitgate

Prioritaet: P0 Produkt- und Performancegate

Abhaengigkeit: KR-4994 plus Sol-Review; KR-4981 bleibt das globale Produktgate
und darf danach genau einmal erneut laufen.

### Umfang

- genau ein frischer privater NativeDisc-Sonic-Kaltport;
- normale 24-Thread-Konfiguration auf der aktuellen Maschine;
- Phasenzeit, CPU, RAM, Cache, Codegen, Hostbuild, Packaging und Gesamtzeit;
- Installation aus der privaten Originaldisc;
- normaler Produktlauf und echter Fensterscreenshot;
- bekannte historische AOT-Grenzen passieren oder engeren typisierten
  Blocker belegen.

### Abschlussbedingungen

- vollstaendiger Port in hoechstens acht Minuten als Ziel;
- kein Context-/Evaluationslimit, kein `incomplete-root` und keine verworfene
  Analyse-Epoche;
- keine reduzierte Funktions-, Block-, Resolution- oder AOT-Abdeckung;
- sichtbarer Fortschritt und terminaler Produktbericht;
- keine 1-/8-/12-/24-Threadmatrix und kein zweiter Build nur fuer Timing.

---

## [ ] KR-4992 - Begrenzte Spekulation spaeterer Resolution-Roots

Prioritaet: bedingtes P1

Abhaengigkeiten: verfehlter KR-4981-Lauf, akzeptable Head-of-Line-Zeit und
positives Restkosten-/RAM-Gate

### Ziel

Erst nach stabilem Candidate-Resolution-Pfad einen kleinen isolierten Anteil
sonst ungenutzter Kerne fuer verwerfbare spaetere Rootarbeit einsetzen.

### Grenzen

- harte Worker-, RAM- und Cache-Reserve fuer den kanonischen Root;
- keine Publikation spaeterer Roots vor ihrer kanonischen Reihenfolge;
- versionierte verwerfbare Resultate;
- keine relevante Verlangsamung des Head-of-Line-Roots;
- bei Cacheverdraengung oder hohem Throwaway-Anteil Pfad deaktivieren;
- danach ein Retry nur auf ausdrueckliche Freigabe.

---

## Weitere offene v0.49-Aufgaben

| ID | Kurzstatus |
|---|---|
| KR-4952 / KR-4953 | frischer ABI-passender CompletePlatform-Handoff, zweiter Capture und normative Paritaet offen |
| KR-4954 / KR-4961 | externes deklaratives Spielprojekt und wiederverwendbares Scaffold offen |
| KR-4955 bis KR-4960 | Funktionsgrenzen, direkte native Calls, Registerlokalisierung, Safepoints und 200-MHz-Hotpath produktseitig abzunehmen |
| KR-4962 | NativeDisc-/DirectBoot-Paritaet am Game Entry offen |
| KR-4963 | inkrementeller Runtime-/Spielbuild und Compilervergleich offen |
| KR-4964 | v0.49-Produktabnahme bis sichtbarem Spielbild und Echtzeit offen |
| KR-4966 bis KR-4970 | relatives Gate, atomarer Handoff sowie AICA/PVR/Maple-Vertraege quellseitig vorhanden, aktueller Produktnachweis offen |

Auch diese Aufgaben folgen dem repositoryweiten Dreischritt und erzeugen
keine neuen Tests oder Testmatrizen.

## Geplante Produktlaeufe

### Lauf A - KR-4981

- Candidate-Resolution muss vollstaendig abschliessen;
- frischer NativeDisc-Port;
- Originaldisc installieren;
- normaler Lauf mit relativer Gastarbeit;
- realer Screenshot;
- AOT-, Runtime-, Geraete- und Performancebefund dokumentieren.

### Lauf B - DirectBoot-Paritaet

- frischen ABI-passenden CompletePlatform-Zustand erfassen;
- DirectBoot ProductHandoff anwenden;
- Subsystemdigests vergleichen;
- normaler Sonic-Lauf bis zum naechsten Produktmeilenstein.

### Lauf C - sichtbares Spielbild und Echtzeit

- mindestens FirstGameFramebufferWrite oder FirstTaFrame;
- danach mindestens 200 MHz bei gleicher Gastarbeit;
- Controller und stabiler mehrminuetiger Lauf folgen erst nach sichtbarem
  Spielfortschritt.

Zwischen diesen Produktgates werden keine neuen Tests, Vollsuiten oder
Matrizen gebaut. Gefixt wird durch Reviews, getestet wird mit Sonic.
