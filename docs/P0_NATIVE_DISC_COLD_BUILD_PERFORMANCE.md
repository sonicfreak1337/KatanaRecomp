# P0 NativeDisc-Kaltbuild: Architektur- und Produktplan

Status: historischer RuntimeOnly-/PlatformAbi-Performancevertrag. Ab v0.49.1
hat `NATIVE_PORT_PRODUCT_CONTRACT.md` Vorrang; aktive Sourcewerte sind
Runtime-ABI 95, Analyzer-ABI 36, Portprojektvertrag 82 und Native-Port-
Profilvertrag 7. Das aktuelle Native-AOT-Emissionsprofil ist 27; das AOT-
Partitionsschema ist 7. Die folgenden Angaben beschreiben das interne
Diagnoseorakel und sind keine Produktarchitektur. Der historische opt-in
Modus `port --analysis-mode runtime-only` war nur mit `--game-project`
zulaessig und ist jetzt kein Produkt-/Releaseprofil.
RuntimeOnly setzt `GuestCallAbi::Unknown`, umgeht die blockierende SuperHC-
FunctionValue-/Candidate-Resolution, erzeugt weiterhin nativen AOT-Code und
nutzt RuntimeOnly-Dispatch ueber eine exakte statische Guest->Host-Tabelle.
Der Whole-Export-Cache ist modegebunden; kein Interpreter, JIT, Runtime-
Decoder oder geratener Zielpfad wird verwendet.

Der historische No-Skip-RuntimeOnly-Lauf erreichte `FirstVisibleGameFrame`; ein
privates Portartefakt oder dessen Hash gehoert nicht in diesen Plan. Der
bereinigte Runtime-/Codegen-Checkpoint entfernt Sonic-spezifische
`SA_PRIVATE_*`-Dumps und Diagnose-Stacktraces; allgemeine Fixes bleiben.

Die folgenden Candidate-Resolution- und D1/D9-Befunde sind historische
PlatformAbi-Diagnostik. Ihr alter Status ohne Portartefakt gilt nicht fuer den
aktuellen RuntimeOnly-Bring-up. Der PlatformAbi-Default bleibt erhalten;
Ordinary-/Inventory-Stack-Alias-Capture und Lane-Fusion sind deferred.
Der historische Candidate-Detailplan steht in
[`P0_ROOT0_CANDIDATE_RESOLUTION_PERFORMANCE.md`](P0_ROOT0_CANDIDATE_RESOLUTION_PERFORMANCE.md).

Der letzte identische 70-s-No-Skip-Lauf erreichte `FirstVisibleGameFrame`.
`341` Renderrequests/-completions/-frames, `15.680` YUV-Makrobloecke und
`470` Audiopuffer mit `345.450` Audiobildern belegen den natuerlichen
Audio-/Videopfad. Die PVR-Fullevidenz endete nach vier bewiesenen Frames mit
`1.228.800` geaenderten Pixeln; der Audiohash `8399287713367543391` blieb
zwischen YUV-Lauf und Audio-Umbau identisch.

Die identische Vergleichsreihe stieg von `23,7959 MHz` ueber `24,1885 MHz`
und `24,2825 MHz` auf `24,2926 MHz` (`+0,4967 MHz`, `+2,09 %`). Der aktuelle
Runtime-P0 ist weiterhin der serielle Runtime-/Dispatch-Overhead bis
mindestens `100 MHz`; danach bleibt der Identity-Miss
der private Identity-Miss offen.

KR-4974 bis KR-4980 sind quellseitig weitgehend umgesetzt. Der terminale
Sonic-v56-Diagnoselauf zeigt jedoch, dass der Port noch nicht produktiv
exportiert werden kann. KR-4982 und KR-4983 bleiben gestrichen. KR-4993 und
KR-4994 und KR-4995 sind source-seitig abgeschlossen; der historische P0 ist die fehlende
Wirksamkeit der autoritativen Hybrid-Join-Closure beim vollstaendigen
Stackvertrag/Gate. Der historische D-Lauf bestand das
globale KR-4981-Produktgate nicht. Ein weiterer Lauf ist nicht automatisch
freigegeben. KR-4992 bleibt ein bedingter Folgezweig nach einem verfehlten
Produktzeitgate.

Der gemeinsame Candidate-Resolution-Explosionsfix erfuellt KR-4985 und
KR-4986 source-seitig. Der einzige freigegebene D1-Lauf erreichte realen
Fortschritt auf zero-based Root 0, aber weder den historischen Root 1 noch
einen vollstaendigen schweren Root. Nach einem Supervisor-I/O-Fehler war die
temporaere JSONL bis `185,586 s` lesbar/gespuelt, aber ohne terminalen
Datensatz und ohne atomare Publikation;
D1/G1 ist daher fail-closed und unentschieden. Der anschliessende D9-Lauf
endete nach `20,331 s` beim ersten fail-closed Signal: Root 0 erreichte Wave
`184`, Frontier `0` (maximal `216`), Budgets blieben unverbraucht, Epochs
published/discarded `0/1`, Retention `incomplete-root`; kein Portartefakt und
kein Produkterfolg. `2.724` admitted evaluations/Semantic-Lanes, `4.349`
logical requests, `3.739` physical evaluations, `2.497` input-widening,
`932` stale-dependency requeues, `1.740` stale discards, `939` semantic und
`2.377` provenance-only widenings gehoeren zum beendeten D9-Lauf.

Der korrekte VsDevCmd-Incremental-Build von `katana-recomp --parallel 12`
war in `42,8 s` erfolgreich; es blieben nur bekannte getenv-/Shadowing-
Warnungen. Der fruehere D-Lauf dauerte `460,6 s` gesamt, Candidate Resolution ca.
`325,8 s`; der identifizierte Kindprozess wurde nach belegter
Nichtverbesserung manuell beendet. Es gab `0/1194` committed Roots, HOL `0`,
Wave `103`, `272` Contexts, `1.044` Semantic-Lanes, `1.029` contextual
physical evaluations, `2.430` contextual logical requests, `1.359` Input-
Widening-, `29` Summary- und `733` stale-Dependency-Requeues, `1.359` stale
snapshot discards, `518.425.788 B` Cache-Payload, `3.964` physische
Auswertungen gesamt und `0/0` publizierte/verwarfene Epochen. Kein
Portartefakt. Die relevante Admission-/Stack-Diagnostik war bei Attempts
`1024`, `2048` und `4096` bitgenau unveraendert. Die Rohwerte sind wegen der
unterschiedlichen Endpunkte nicht direkt vergleichbar und belegen keine
materielle Produkt-/Performanceverbesserung. Dies ist ein historischer
Inventory-Provenance-Live-in/Spill-through-Befund, nicht der aktuelle P0.

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
KR-4985/KR-4986/KR-4993/KR-4987/KR-4994/KR-4995 source-seitig abgeschlossen
  -> RuntimeOnly-Build-/Export-Gate bestanden
  -> No-Skip-Sonic-Audio-/Videopfad bis FirstVisibleGameFrame; 24,2926 MHz
  -> Ziel mindestens 100 MHz ohne Sicht-/Audioregression
  -> post-filmischen privaten Identity-Miss schliessen
```

Fuer jeden Implementierungstask gilt:

```text
implementieren -> betroffene Pfade reviewen und Findings schliessen -> main
```

D1 und D2 sind reale Sonic-Diagnoseexporte. KR-4981 ist der reale
Produkt- und Integrationstest. Die vorliegende D1-/D9-Evidenz ist nichtterminal;
D1/G1 bleibt historisch unentschieden, D2/G2 ist abgeschlossen und negativ:
kein positiver Schedulerhebel. D9 ist
beendet und Root 0 konvergierte fail-closed ohne Erfolgsaussage; KR-4988 bis
KR-4991 bleiben inaktiv. KR-4994 und KR-4995 sind source-seitig abgeschlossen;
die fehlende Wirksamkeit der autoritativen Hybrid-Join-Closure bleibt ein
historischer PlatformAbi-Befund. Der aktuelle RuntimeOnly-P0 ist der
serielle Runtime-/Dispatch-Overhead des sichtbaren Audio-/Videopfads:
`24,2926 MHz` im letzten Vergleichslauf, Ziel mindestens `100 MHz` ohne
Regression. Danach folgt der post-filmische Identity-Miss
der private Identity-Miss. KR-4981 bleibt das globale Produktgate und ist
bis Hauptmenue nicht bestanden.
Es gibt keine begleitende neue Testmatrix.

### Lauf nach Candidate-Domain-Top-Fix

Der Candidate-Domain-Top-Fix behandelt abgeschnittene begrenzte Candidate-
Domains als kanonisches absorbierendes Top mit leerem endlichem Praefix und
fuehrt Merge, Normalisierung, Vergleich, Keys, Persistenz, Consumer und
ABI-Promotion konsistent fort. Der historische Candidate-Domain-Top-Lauf lief
unter Function-Analysis-Epoch-Schema `18` und Analyzer-ABI `33`. Der aktuelle
Source-Checkpoint ist separat oben ausgewiesen. Der korrekte
VsDevCmd-Incremental-Compile+Link war erfolgreich.

Der einmalige Lauf `kr4981-20260809-020628-2bfd8af5` endete nach `343,627 s`
durch manuellen Abbruch bei belegter identischer Nichtkonvergenz. Die Voranalyse
bis Candidate-Start dauerte etwa `146 s` einschliesslich des Gesamtstarts;
letzte Bewegung war Wave `48`. Peak Root lag bei `1.450.078.208 B`, Peak Job
bei `1.618.132.992 B`; es gab keine kanonische Publikation und kein
Portartefakt. Bei Wave `39` waren die 16 geprueften Kernzaehler exakt wie im
Vorlauf. Der Fix ist damit als Korrektheits-/Persistenzfix, nicht als
Konvergenzhebel, belegt; KR-4981 bleibt offen.

### [x] Abgeschlossener Hot-Callee-Diagnoseunterauftrag

Der Lauf `kr4981-20260809-024141-c4ffdf15` erreichte das vollständige
`attempts=1024`-Diagnosegate und wurde nach `244,549 s` bei Wave `24` gezielt
beendet. Der erwartbare `product-exit -1`-Status entstand durch diesen Stop;
es gab keinen Fehler, Hänger, keine Publikation und kein `game.exe`. Peak Root
WS: `1.260.388.352 B`, Peak Job WS: `1.387.151.360 B`. `uncategorized=0`
galt für alle Top-8-Funktionen. Die dominante Hot-Callee isolierte `20` semantische
Änderungen und `40` Stack-Widenings ausschließlich auf SavedEpoch-pending-ABI-
Skalare; der Callee-Set-Stackvertrag war unvollständig. Der SavedEpoch-
Lifecycle-Fix ist source-seitig abgeschlossen; offen bleibt die gemeinsame
Ordinary-/Registermetadaten-/Alias-/Watcher-/Loss-/MemoryEpoch-Lifecycle-
Ursache, ohne Alias-/Current-Tracking oder fail-closed Restore zu verlieren.
KR-4981 bleibt offen.

### [x] Abgeschlossener SavedEpoch-Lifecycle-Unterauftrag

Current-tracking Pending-ABI-Skalare werden nur an bewiesenen normalen
Call-/Tail-ABI-Gates konsumiert; detached Epochs bleiben unangetastet.
`candidate_payload_lost` ist ein absorbierendes Epoch-Top ueber Normalize,
Merge, Equality, Key, Subsumption, Evidence, Restore und Persistenz. Konkrete
Evidence und Nested-/Current-Aliasfakten bleiben erhalten, finite Payload/Slots
verschwinden; detached Top uebernimmt keine fremde Tail-Evidence. Der
historische SavedEpoch-Lifecycle-Stand lief unter Epoch-Schema `17` und
Analyzer-ABI `33`.

Der Lauf `kr4981-20260809-031826-0616113a` endete nach `369,171 s` mit
`nonconvergence`/Exitcode `31` bei Wave `76`; `uncategorized=0` in D1024 und
D2048, keine Publikation und kein `game.exe`. Der alte SavedEpoch-Pending-
Blocker ist beseitigt; die gemeinsame Ordinary-/Registermetadaten-/Alias- und
MemoryEpoch-Lifecycle-Ursache bleibt der naechste Analysepunkt. KR-4981 bleibt
fail-closed offen.

Der historische Candidate-Resolution-Source-Stand ist `49b0f72a9f49d60a4eb6e0481460cd57c5625735` und
der damalige ABI-passende Stand: strukturelle Contextual-Hybrid-Projektion;
die autoritative Hybridprojektion schliesst Contextual-MAY-Joins und Forward-
Edges erneut vollstaendig.
mit retained sticky loss, fail-closed SavedEpoch-Slot-Pending-Top in allen
Truncation-/Publication-Checks und öffentlich getrennte Provenance-Replay-
Capsule-/Keybyte-Limits neben dem semantischen Evaluation-Limit. Ein echter
Evaluation-Cap belastet nur den Evaluation-Zähler; der damalige Source-Stand
verwendete Analyzer-ABI `34`, Epoch-Schema `27` und lokales In-Process-
Evaluation-Cache-Schema `13`;
der bestätigte Incremental-Build endete mit Exit `0` nach ca. `48 s`, die EXE
trug LastWriteTime `09.08.2026 09:08:11 +02:00`.

Der historische PlatformAbi-Produktlauf `kr4981-20260809-091410-2766aaa6` endete nach ca.
`275 s` gesamt (Candidate ca. `221 s`) mit `nonconvergence` nach drei
Amplifikationssamples: `0/1274` Roots, HOL `0`, Wave `107`, `280` Contexts,
`970` Semantic-Lanes, `1.861` physische, `2.526` logische Requests,
Input-Widening `536`, Summary `22`, Forward `123`, stale Requeues `272`,
stale Discards `806`, Cache `589.178.706 B`; keine Budgets erschöpft, keine
Publikation und kein Artefakt bzw. `game.exe`. Der Supervisor schrieb wegen
`taskkill`-Zugriffsverweigerung keine Summary; der Kill-on-close-Job beendete
den Child trotzdem. Admission `1024/1024`, projected context/match jeweils
`0`; der sauberste Ordinary-Stack-Treiber blieb bei `84/84` Attempts/Semantic Changes und `508`
Ordinary-Stack-Deltas trotz vollständigem Stackvertrag. Der historische P0 ist
die fehlende Wirksamkeit der autoritativen Hybrid-Join-Closure beim
vollstaendigen Stackvertrag/Gate.

Der vorherige Produktlauf `kr4981-20260809-083308-4a3ff9be` endete nach
`286,387 s` (Candidate ca. `232,5 s`) mit `nonconvergence`/Exit `31`: `0/1274`
Roots, Wave `119`, keine Publikation, `280` Contexts, `972` Lanes, `2.011`
physische, `2.814` logische, `203` Cache-Reuses, `2.790` Subscriber,
Provenienz `169.824`, stale Discards `922`, Frontier `43` (max `250`), Cache
`610.295.241 B`; kein `game.exe`. Admission `1024/1024`, projected context/
match jeweils `0`; P0 sind Ordinary-Stack und lokale Stackkoordinaten.

Der vorherige Lauf `kr4981-20260809-041945-21b70ade` endete nach `425,924 s` bei Wave
`60`, `0/1194` Roots, `758` Lanes, `984` physischen und `1.398` logischen
Auswertungen, `248` Input-, `102` stale-Requeues und `347` Discards; Peak Root
`1.606.066.176 B`, Peak Job `1.814.822.912 B`, kein Portartefakt.
Der Vergleichslauf `kr4981-20260809-050420-3f47fd65` endete nach `322,632 s` bei Wave
`39`, `0/1194` Roots, `272` Contexts, `549` Lanes, `630` physischen und
`894` logischen Auswertungen; `181` Input-, `10` Summary-, `76` stale-
Requeues, `226` Discards, Provenienz `31.713`, kein `game.exe`. Das
`attempts=1024`-Gate blieb gegenüber `9baea88` bitgleich; die Gateänderung ist
korrekt, aber kein Konvergenzhebel. Historisch wurde Inventory-Provenance-
Live-in/Spill-through als P0-Folgepunkt vermutet; der historische P0 ist die
fehlende Wirksamkeit der autoritativen Hybrid-Join-Closure beim vollständigen
Stackvertrag/Gate.

## Produktmessvertrag KR-4981

Der RuntimeOnly-Build-/Export-Gate ist bestanden. Das globale KR-4981-
Produktgate bleibt fuer den beaufsichtigten Start bis mindestens zum
Memory-Card-Screen offen; ein weiterer Produktlauf ist nicht automatisch
freigegeben.

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
7. KR-4993 alle bestaetigten Source-Findings geschlossen, das Analyzer-ABI-
   Finding unter dem aktuellen Analyzer-ABI 36 mit dem SDK-Linkabschluss geschlossen und Limit-, Stale-, Cancellation- sowie
   `IncompleteRoot`-Pfade fail-closed gehalten hat; und
8. KR-4981 einen vollstaendigen Sonic-Kaltport in hoechstens acht Minuten
   erzeugt oder einen engeren typisierten Produktblocker belegt.
