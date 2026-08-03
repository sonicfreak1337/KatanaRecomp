# P0 Candidate-Resolution: Aufgaben- und Messplan

Status: Aktiver Plan. Der funktionale Ausgangsstand ist `a521999`; der
terminale Sonic-v56-Diagnoselauf belegt eine echte Contextual-State-
Explosion und keine fertige Produktartefakterzeugung.

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
Source-Checkpoint:                              a521999
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

Daraus folgen:

```text
physische Auswertungen je eindeutigem Context:    1,083
logische Evaluationen je eindeutigem Context:     2,547
logische Evaluationen ohne neue physische Arbeit: 37.664
Anteil physischer Erstberechnungen:                rund 92,3 Prozent
```

Cache-Churn, Eviction-Recomputes und die fruehere unnoetige
Deep-Copy-Verstaerkung sind nicht mehr die Hauptursache. Der offene P0 ist:

```text
echte semantische Contextmenge
  x
Kosten je Context
  x
ueberwiegend serieller kritischer Scheduling-Span
```

Der terminale Stand darf nicht mehr pauschal als „Root 0 scheiterte“
bezeichnet werden. Bei `1/1191` ist Root 0 wahrscheinlich committed worden.
Bis Rootindex, Rootadresse und limitierte Funktion terminal ausgegeben
werden, gilt der Befund fuer die ersten schweren Candidate-Resolution-Roots
allgemein.

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

## Diagnosegates

### D1 in KR-4985

D1 wird nur nach ausdruecklicher Freigabe ausgefuehrt. Der reale Sonic-
Diagnoseexport endet:

- nach dem ersten vollstaendigen schweren Candidate-Resolution-Root; oder
- frueher an den repositoryweiten Stall-, Nichtkonvergenz- oder
  15-Minuten-Grenzen.

D1 identifiziert mindestens:

- Rootindex und Rootadresse;
- limitierte Funktionsadresse;
- Context- und Evaluationsbudgets;
- logische und physische Auswertungen;
- Requeue-Ursachen;
- Frontierbreite;
- Snapshot-, Key-, Apply-/Merge-, Evidence- und Commitkosten;
- Full-State-, Projected-Lens- und Provenienz-Cardinalitaet.

### D2 zu Beginn von KR-4991

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

### KR-4985 - Phasen- und Kardinalitaetstelemetrie

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

Abschluss:

```text
implementieren -> betroffene Pfade reviewen und Findings schliessen -> main
```

Danach darf D1 separat freigegeben werden. Keine neue Testinfrastruktur.

### KR-4986 - Semantische Lanes und Provenienzabonnenten

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

### KR-4987 - Read-Lens-Context-Identitaet

Nur bei positivem G1.

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

Die Wirkung wird in D2 und spaeter im Sonic-Produktgate gemessen, nicht in
einer neuen synthetischen Matrix.

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

### KR-4993 - Abschlussreview

KR-4993 implementiert keine neue Testinfrastruktur und startet keinen
Produktlauf. Es reviewt alle aktivierten Candidate-Resolution-Pfade end-to-
end und schliesst jedes bestaetigte Finding vor dem Push auf `main`.

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

Freigabebedingungen:

- keine bestaetigten offenen Findings;
- keine reduzierte Analyse- oder AOT-Abdeckung;
- keine `contextual_return_context_limited_functions`;
- keine `contextual_return_evaluation_limited_functions`;
- kein `resolution_root_logical_budget_exhausted`;
- kein `IncompleteRoot`;
- jeder schwere Root terminal identifizierbar;
- Push auf `main`, danach KR-4981.

### KR-4981 - Sonic-Produktgate

Nach KR-4993 wird genau ein frischer 24-Thread-NativeDisc-Sonic-Port gebaut,
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
Cacheverdraengung oder hohem Throwaway-Anteil deaktiviert. Danach folgen
erneut KR-4993 und ein separat freigegebener KR-4981-Retry.

## Abhaengigkeitskette

```text
KR-4985 -> D1/G1
  -> KR-4986
  -> positiv gegatete KR-4987..KR-4990
  -> D2/G2
  -> KR-4991 nur bei positivem G2
  -> KR-4993
  -> KR-4981
  -> optional KR-4992 -> KR-4993 -> freigegebener Retry
```

Jeder Implementierungstask endet mit Review und Push auf `main`. D1, D2 und
KR-4981 sind die einzigen in diesem Plan vorgesehenen neuen Sonic-Laeufe.
Keine neue Testmatrix ersetzt sie.

## Abschlussdefinition

Der Candidate-Resolution-P0 ist erst geschlossen, wenn:

1. limitierter Root und Funktion sowie alle dominanten Kosten erklaert sind;
2. semantische Contextarbeit und exakte Provenienz getrennt sind;
3. jede aktivierte Projektion bei Vertragsluecken FullState verwendet;
4. die belegten dominanten Per-Context-Kosten geschlossen oder begruendet
   unveraendert gelassen wurden;
5. keine unveraenderte semantische Version unbegruendet erneut Budget
   verbraucht;
6. keine stale oder nichtkanonische Publikation moeglich ist;
7. KR-4993 ohne offenes Finding endet; und
8. KR-4981 einen vollstaendigen Sonic-Port erzeugt oder einen engeren
   typisierten Produktblocker belegt.
