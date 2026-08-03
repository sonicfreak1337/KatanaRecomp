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
- das Fehlen neuer Tests ist kein Reviewfinding;
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

funktionaler Source-Checkpoint:
  a521999 / Runtime-ABI 87 / Block-ABI 5 / Analyzer-ABI 31 /
  PlatformServices-ABI 13 / Backend-ABI 12 / Portprojektvertrag 75

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
kein Beweis fuer den aktuellen Source.

## Verbindliche Reihenfolge

```text
KR-4985 -> D1 nur nach ausdruecklicher Freigabe
  -> KR-4986
  -> KR-4987 bis KR-4990 nur bei ihrem positiven Messgate
  -> D2 nur nach ausdruecklicher Freigabe
  -> KR-4991 nur bei positivem G2
  -> KR-4993
  -> KR-4981 genau ein voller Sonic-Produktbuild und -lauf
     -> KR-4992 nur nach verfehltem Zeitgate und positivem Restkosten-/RAM-Gate
        -> KR-4993 erneut -> separat freigegebener KR-4981-Retry
```

Jeder Task in dieser Kette folgt einzeln:

```text
implementieren -> betroffene Pfade reviewen und Findings schliessen -> main
```

D1 und D2 sind begrenzte reale Sonic-Diagnoseexporte, keine Testmatrix.
KR-4982 und KR-4983 bleiben gestrichen.

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

Status: Der gemeinsame Executor ist quellseitig implementiert. v56 belegt
jedoch, dass der produktive Candidate-Resolution-Pfad wegen schmaler
Frontiers und zu vieler semantischer Contexts weiterhin fast seriell ist.

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
den aktuellen Hauptfehler nicht loest: Eviction-Recomputes bleiben null,
waehrend Contextmenge und logische Wiederzulassungen das Budget
erschoepfen.

### Abschlussbedingungen

- fehlende, alte oder beschaedigte Shards bleiben sichere Misses;
- lokale Aenderungen invalidieren nur semantisch gebundene Ebenen;
- Produktwirkung und Warmexportzeit werden erst im Sonic-Produktpfad
  bewertet.

---

## [ ] KR-4985 - Candidate-Resolution-Phasen- und
## Kardinalitaetstelemetrie

Prioritaet: P0 Performance-Diagnose

Abhaengigkeiten: KR-4974, Source-Checkpoint `a521999`

### Ziel

Den limitierenden Root und die limitierte Funktion identifizieren und die
Kosten von Snapshot, Cache-Key, physischer Auswertung, `apply_call()`, Merge,
Evidence und Commit getrennt sichtbar machen.

### Umfang

- Rootindex, Rootadresse, Funktionsadresse, Wellenindex und Frontierbreite;
- logische Evaluationen, physische Auswertungen, Cache-Reuse ohne physische
  Arbeit, Context- und Evaluationsbudgets;
- neue, verbreiterte und erneut zugelassene Lanes;
- Requeue-Ursachen getrennt nach Input-Widening, Summary-Aenderung,
  Forward-Edge-Insert/Widening, stale Dependency-Version,
  Evidence-Layout-Aenderung, Cache-Reuse und neuer semantischer Lane;
- Snapshot-, Key-, Auswertungs-, Merge-, Evidence- und Commitzeit;
- Bindingzahl, Hitposition, Equality-/Copy-/Mergearbeit und Stategroesse;
- aggregierte Full-State-, Projected-Lens- und Provenienz-Digests;
- keine Rohstates, Gastwerte oder per-Lane-Retaillogs.

### Review- und Abschlussvertrag

- Telemetrie darf die kanonische Semantik nicht veraendern;
- Drop-, Vollstaendigkeits- und Budgetpfade werden im Quellreview verfolgt;
- D1 wird nur nach ausdruecklicher Freigabe als realer Sonic-
  Diagnoseexport ausgefuehrt;
- D1 endet am ersten vollstaendigen schweren Root oder an den allgemeinen
  Lauf-/Stallgrenzen;
- das Messgate G1 entscheidet die bedingten Tasks KR-4987 bis KR-4990;
- keine neue Telemetrie-Testmatrix oder synthetische Ersatzabnahme.

---

## [ ] KR-4986 - Semantische Context-Lanes und exakte
## Provenienzabonnenten

Prioritaet: P0 Korrektheits-Enabler

Abhaengigkeit: KR-4985

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

## [ ] KR-4987 - Read-Lens-projizierte Context-Identitaet

Prioritaet: P0 Performance

Abhaengigkeiten: positives G1, KR-4986

Status: Nur bei positivem Messgate aktiv. Sonst bleibt FullState autoritativ.

### Ziel

Contexts vor der Lane-Erzeugung zusammenlegen, wenn der konkrete Callee
vollstaendig bewiesen denselben semantischen Eingang liest und sich die
Inputs nur in ungelesenem State oder Provenienz unterscheiden.

### Umfang

- SemanticContextKey aus Funktion, Lens, Contract-Fingerprint und
  projiziertem Ingress;
- vollstaendige Register-, Stack-, Memory-, Alias-, Output-, Inventory- und
  Fallback-Watcher;
- zwingender FullState-Fallback bei jeder Vertragsluecke;
- Rebucketing bei spaet erweitertem Read-Vertrag;
- stabile Content-Digests plus strukturelle Kollisionspruefung;
- Provenienz bleibt ausserhalb der semantischen Lane-Identitaet.

### Review- und Abschlussvertrag

- gelesene Unterschiede bleiben getrennt, ungelesene duerfen teilen;
- Completeness, Truncation und exakte Evidence bleiben erhalten;
- die Reduktion erfolgt vor Lane-Erzeugung und nicht erst durch einen
  spaeten Cachetreffer;
- die reale Wirkung wird mit D2 und spaeter Sonic bewertet, nicht mit einer
  neuen synthetischen Matrix.

---

## [ ] KR-4988 - Internierte AbstractStates und Function-Value-Summaries

Prioritaet: bedingtes P1 Performance

Abhaengigkeiten: positives KR-4988-Kostengate, KR-4986

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

Abhaengigkeiten: positives KR-4989-Kostengate, KR-4986

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

Abhaengigkeiten: positives Kosten-/Reusegate, KR-4986

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

## [ ] KR-4993 - Abschlussreview der Candidate-Resolution-Pfade

Prioritaet: P0, letztes Sourcegate vor KR-4981

Abhaengigkeiten: KR-4985, KR-4986 und alle durch G1/G2 aktivierten Tasks bis
KR-4991

### Ziel

Alle geaenderten Context-, Read-Lens-, Interning-, Binding-, Dependency-,
Cache-, Evidence-, Budget- und Schedulingpfade end-to-end reviewen und jedes
bestaetigte Finding vor dem Push schliessen.

### Umfang

- semantische Identitaet und exakte Provenienz;
- FullState-Fallback, Digests, Kollisionen und Cache-Key-Schema;
- globale und kontextuelle Fallback-Summaries;
- Invalidierung, Delta-Monotonie, Stale und Evidence-Publikation;
- Cancellation, Budgets, RAM, Retention und Progress;
- logische Zulassung, semantische Lane, physische Auswertung, Cache-Reuse,
  Requeue und Provenienzabonnent als getrennte Zaehldomaenen;
- D1-/D2-Befunde konsistent zusammenfassen.

### Abschlussbedingungen

- alle bestaetigten Findings geschlossen und erneut in den betroffenen
  Pfaden reviewt;
- keine reduzierte Analyse-, Resolution-, Guarded-AOT- oder
  Completenessabdeckung;
- keine `contextual_return_context_limited_functions`;
- keine `contextual_return_evaluation_limited_functions`;
- kein `resolution_root_logical_budget_exhausted`;
- kein `IncompleteRoot` im freizugebenden Candidate-Resolution-Endstand;
- jeder schwere Root ist terminal identifizierbar;
- kein neuer Test, keine Testmatrix und kein Produktlauf in KR-4993;
- nach dem Push auf `main` ist KR-4981 freigegeben.

---

## [ ] KR-4981 - Einmaliges 24-Thread-Sonic-Produktzeitgate

Prioritaet: P0 Produkt- und Performancegate

Abhaengigkeit: KR-4993

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
- danach KR-4993 erneut und ein Retry nur auf ausdrueckliche Freigabe.

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
