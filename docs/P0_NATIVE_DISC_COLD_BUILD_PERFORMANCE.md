# P0 NativeDisc-Kaltbuild: Architektur- und Aufgabenplan

Status: verbindlicher P0-Fahrplan; KR-4974 abgeschlossen, KR-4975 bis KR-4984
nicht abgeschlossen

Analysebasis: P0-Planstand `ffd45ae`, Source-Checkpoint `18f8537` vom
31. Juli 2026 und der abgebrochene private v24-Export. Der Source-Checkpoint
enthaelt die bis dahin vorliegenden Analyse-, Cache-, Executor-,
Fortschritts- und Runtimeumbauten, schliesst KR-4974 bis KR-4984 aber nicht
ab. Alle Aussagen ueber den Source muessen vor dem Produktlauf gegen den dann
aktuellen Head neu geprueft werden.

## Ziel

Ein vollstaendiger, wirklich kalter NativeDisc-Port darf auf einem normalen
Entwicklungsrechner kein stundenlanger Vorgang sein. Katana muss die vorhandene
CPU parallel und speichersicher nutzen, semantisch unveraenderte Arbeit
wiederverwenden und in jeder langen Phase belastbaren Fortschritt anzeigen.

Der Performanceumbau darf dabei weder Analyseabdeckung entfernen noch einen
Interpreter-, JIT- oder Emulationsfallback einfuehren. Jede beschleunigte,
inkrementelle, persistierte oder optional GPU-gestuetzte Ausfuehrung muss
dieselben kanonischen Analyse-, IR- und Produktartefakte wie der konservative
CPU-Referenzpfad liefern. Alle bestehenden fail-closed Vollstaendigkeits- und
Budgetvertraege bleiben normativ.

Nach der Implementierung folgt zwingend eine unabhaengige Gesamtpruefung aller
betroffenen Pfade. Alle P0- und P1-Funde werden geschlossen und die betroffenen
Gates erneut ausgefuehrt. Erst danach darf genau ein neuer realer
NativeDisc-Sonic-Port gebaut, installiert und sichtbar ausgefuehrt werden.

## KR-4974-Abschlussstand

Der Beobachtungs- und Reproduzierbarkeitspfad ist implementiert:

- versionierte Manifest-, Progress-, Resource- und Terminalrecords in einer
  opt-in JSONL-Ausgabe;
- begrenzte asynchrone Aufnahme, explizite Drop-/Completenessfelder,
  geordneter Flush und atomare terminale Veroeffentlichung;
- pfad- und identitaetsbasierter Schutz der Telemetrieausgabe vor Quell-,
  GDI-Track-, Ausgabe-, Workspace-, Publishlock- und Geraetenamen-Aliasen;
- Prozessbaumressourcen ueber Windows Job Objects beziehungsweise
  qualifizierte POSIX-Prozessgruppenaufnahme mit finaler `wait4`-Evidenz;
- genaue FunctionEvaluation-Cachelookups mit Ready-Hit,
  In-Flight-Coalesce, Miss, Eviction, Groesse und genau einem primaeren
  Missgrund;
- Heartbeats sowie getrennte Zaehler fuer Worksetwachstum, Head-of-Line,
  geplant, gestartet, ready, committed und aktive Worker;
- eine deterministische retailfreie NativeDisc-Stressfixture mit kleinem
  `smoke`- und gatefaehigem `reference`-Profil.

Die modellierten Roots, Requests, Module, IR- und Partitionsdaten laufen durch
reale Katana-Pfade. Ein waehrend der Abnahme gefundener zehnminuetiger
Windows-MSPDB-Nachlauf nach beendetem CMake-Configure ist durch einen privaten
Endpoint und eine einsekuendige Helper-Shutdownfrist geschlossen; der
Supervisor wartet weiterhin fail-closed auf Job-Leere. Die eigentlichen
8-/12-/24-Thread-Gates folgen in KR-4981. GPU-Offload bleibt ein eigenes
beweispflichtiges Entscheidungsgate in KR-4982. Die Abschluss-Gesamtpruefung
KR-4984 bleibt unveraendert Pflicht.

## Gemessene Ausgangslage

Die folgenden Zahlen stammen aus den privaten Exportlogs
`v23-final-export.stdout.log` und `v24-real-export.stdout.log`. Private
Originaldaten oder titelbezogene Adressen werden weder in Caches noch in
oeffentliche Testartefakte uebernommen.

| Messung | v23, Pass 15 | v24, Pass 15 | v24, Pass 22 | v24, Pass 24 beim Abbruch |
|---|---:|---:|---:|---:|
| Resolution-Roots | 1.328 | 1.192 | 1.416 | 1.426 |
| Candidate Resolution | 100,56 min | 89,72 min | 110,71 min | nach 3,56 min beendet |
| kanonisch publizierte Roots | 1.328 | 1.192 | 1.416 | 7 |
| Evaluation-Hits | 6.352 | 6.801 | 7.646 | 503 |
| Evaluation-Misses | 78.331 | 77.745 | 82.440 | 27.244 |
| Hitquote | 7,50 % | 8,04 % | 8,49 % | 1,81 % partiell |

Die v24-Pass-15-Zeit sank gegenueber v23 nur um etwa 10,8 Prozent, waehrend
die Zahl der Resolution-Roots um etwa 10,2 Prozent sank. Das ist nahezu nur
weniger Arbeit, keine grundlegende Verbesserung des Durchsatzes.

Der v24-Gesamtlauf wurde am 31. Juli 2026 gegen 09:12 Uhr Europe/Berlin
nach etwa `3 h 27 min` auf ausdruecklichen Nutzerwunsch beendet. Er zeigte:

- etwa 15 belegte CPU-Kerne im Langzeitmittel auf einem 24-Thread-Host,
  abgeleitet aus `Get-Process.CPU / Prozesswalltime`;
- etwa `11,24 GiB` beobachteten Working-Set-Peak aus wiederholten
  `Get-Process`-Snapshots;
- zwei abgeschlossene, jeweils weit ueber 15 Minuten lange
  Candidate-Resolution-Phasen;
- nach weiteren Seeds eine dritte vollstaendige Function-Value-Neuberechnung;
- beim Abbruch `7 / 1.426` kanonisch publizierte Roots in Pass 24.

Cache-/Rootwerte stammen aus den versionierten `KATANA_PROGRESS`-Zeilen des
Logs. Die Parent-Scopes 13, 15 und 17 und ihre
Candidate-Resolution-Child-Scopes 14, 16 und 18 spiegeln jeweils dieselben
FunctionEvaluation-Zaehler; sie duerfen nicht doppelt addiert werden. Die
Pass-24-Hitquote ist wegen des Abbruchs nur ein Fruehwert und nicht direkt
mit den abgeschlossenen Passes vergleichbar. Die Prozessbeobachtung ist keine
abgeschlossene Benchmarkmessung. Sie reicht aber aus, um die Behauptung zu
widerlegen, Katana nutze nur einen Kern. Das Problem ist primaer mehrfach
ausgefuehrte und zu grob geschnittene Arbeit, nicht bloss eine fehlende
Threadzahl.

Der Abbruch erfolgte vor Abschluss der Analyse. Das angeforderte
Portverzeichnis wurde nicht publiziert; es gibt keine `game.exe`, keine
Discinstallation, keinen Sonic-Prozess und keinen neuen Screenshot. Der Lauf
ist ausschliesslich Performance-Diagnoseevidenz.
Der lokale Iterationsname v24 und `v24-real-export.stdout.log` bezeichnen
nicht den wesentlich aelteren historischen CompletePlatform-v24-
Produktport.

Die Discgroesse ist ebenfalls nicht der aktuelle Hauptblocker. Im v24-Log
brauchte das Laden und Hashen der GDI etwa 5,4 Sekunden; die anschliessende
Packed-Disc-Identitaet verwendete die bereits vorhandenen Chunkhashes wieder.

## Verifizierter aktueller Datenfluss

### Aeusserer Kontrollflussfixpunkt

`analyze_control_flow()` in
`src/analysis/control_flow_analysis.cpp` behaelt bereits eine
`FunctionValueAnalysisSession` ueber die aeusseren CFG-Runden. Pro Runde
werden jedoch Funktionen, Bloecke, Seedmengen und die davon abhaengige
Function-Value-Programmstruktur erneut aufgebaut.

Der innere Candidate-Contract-Fixpunkt ist auf 64 Runden begrenzt. Die
vorliegenden Logs belegen nicht, dass dieses Maximum ausgeschoepft wurde;
der Architekturvertrag muss dennoch verhindern, dass jede zulaessige Runde
das komplette Programm ohne inkrementelle Wiederverwendung neu berechnet.

Der aktuelle Pfad ist vereinfacht:

```text
Seeds
  -> Recursive/Local CFG
  -> Function- und Blockinventar
  -> Candidate-Contract-Fixpunkt
     -> vollstaendige Function-Value-Analyse
     -> Resolutionen koennen neue Seeds liefern
     -> Boundary-Normalisierung
     -> Guarded Inventory kann weitere Seeds liefern
  -> neue Seeds?
     ja: aeussere CFG-Runde komplett erneut
     nein: Ergebnis publizieren
```

Im v24-Lauf erweiterte Pass 15 die Seedmenge von 1.327 auf 1.382. Weitere
Runden wuchsen bis Pass 22 auf 1.547 Seeds. Pass 22 begann danach erneut eine
vollstaendige Function-Value-Analyse mit 1.586 Funktionen, 22.394 Bloecken
und 1.416 Resolution-Roots. Nach deren Abschluss erweiterte die Summary-
Expansion die Seedmenge von 1.547 auf 1.554; rekursive Nacharbeit erreichte
1.557 Seeds und startete Pass 24 mit 1.597 Funktionen, 22.431 Bloecken und
1.426 Resolution-Roots erneut. Der Lauf wurde in diesem dritten Vollansatz
beendet.

### Function-Value-Auswertung

`src/analysis/function_value_analysis.cpp` baut pro Aufruf unter anderem neu:

- Block- und Funktionsindizes;
- Funktionszuordnung und Callerlisten;
- SCCs und SCC-Abhaengigkeiten;
- ABI-Register-, Stack-, Return- und Persistent-Store-Vertraege;
- Summary- und Candidate-Inputs;
- Resolution-Root- und Inventory-Sink-Mengen.

Der aktuelle `FunctionEvaluationCache` ist ein exakter In-Memory-Cache. Sein
Schluessel bindet nicht nur Funktion und Bloecke, sondern auch den ganzen
Eingangszustand, Resolutionmodus, isolierte Callsites, Summary-
Abhaengigkeiten, ABI-Vertraege, Tail-Ingress und Inventory-Sinks. Eine kleine
irrelevante Aenderung kann deshalb einen vollstaendigen Miss erzeugen.

Die Session behaelt Evaluationen, aber nicht das immutable Funktionsprogramm,
den SCC-DAG oder inkrementelle Summary-/Dependency-Zustaende. Die aktuelle
Standardgrenze von 16.384 Cacheeintraegen beziehungsweise 1 GiB kann bei
einem Lauf mit mehr als 80.000 Evaluationen ausserdem Verdraengung und
erneute Arbeit verursachen. Der aktuelle Fortschrittsvertrag weist
Evictions noch nicht aus; genau deshalb ist das Ledger aus KR-4974 vor einer
quantitativen Zuschreibung Pflicht.

Der aktuelle Cache zaehlt sowohl einen bereits fertigen Treffer als auch das
Warten auf eine identische laufende Evaluation als `hit`. Die oben
angegebenen Hitquoten koennen diese beiden Wirkungen deshalb noch nicht
trennen.

### Root-lokale Guarded-Inventory-Arbeit

Die finale Resolution wird nach Rootfunktionen parallelisiert. Jede
Rootauswertung besitzt aber eigene `ForwardedStoreContext`-Vektoren,
Workqueues und Indizes. Gleiche nachgelagerte Kontexte koennen dadurch fuer
viele Roots erneut ausgewertet werden.

Der globale kontextuelle Candidate-Return-Koordinator vereinigt bereits einen
Teil der Ownerarbeit. Der normale forwarded Inventory-Walk bleibt dennoch
root-lokal. Das ist der wichtigste Kandidat fuer die lange Root-17-Tailphase.

Die Ergebnisveroeffentlichung ist absichtlich kanonisch nach Rootindex
geordnet. Der aktuelle Fortschrittswert `completed=16` beschreibt deshalb
nur den publizierbaren Praefix. Er sagt nicht, wie viele spaetere Slots
bereits gerechnet wurden und erzeugt ohne Zusatzmetriken einen falschen
Stillstandseindruck.

### Parallel-Executor

`include/katana/analysis/parallel_work.hpp` besitzt bereits einen festen,
prozessweiten Workerpool. Verschachtelte Batches helfen derselben Queue; es
entstehen keine voneinander unabhaengigen 12-mal-12-Threadpools mehr.

Offen bleiben:

- nur eine FIFO-Queue ohne Kosten- oder Kritischer-Pfad-Prioritaet;
- Rootjobs sind zu gross und nur begrenzt teilbar;
- kein globaler Dependency-DAG fuer Summary-, Context- und Inventoryarbeit;
- keine explizite RAM-/Cache-Drucksteuerung;
- keine getrennte Anzeige fuer geplant, aktiv, fertig aber blockiert und
  kanonisch publiziert.

### Vorhandene persistente Caches

Folgende Schichten existieren bereits und duerfen nicht neu erfunden werden:

- Whole-Export-Cache mit manifestiertem Dateibaum;
- Product-Boot-Analysis-Cache fuer einen vollstaendig erfolgreichen
  Analyse-/IR-Treffer;
- positiver und negativer Latent-AOT-Modulcache;
- Partition-Codegen- und Metadatencaches;
- inkrementeller Hostbuild und Runtime-Buildprofile;
- komponentenbezogene Implementierungsidentitaeten fuer Analyse, IR,
  Codegen und Orchestrierung.

Die fehlende Schicht liegt dazwischen: wiederverwendbare
Funktionsprogramm-, SCC-, Summary-, Candidate-, Inventory- und IR-Shards nach
einem Whole-Analysis-Miss.

## Ursachenrangfolge

### P0-A: Mehrfachaufbau des gesamten Function-Value-Programms

Jede aeussere Seedrunde und jeder relevante Candidate-Contract-Neustart kann
Graphen, SCCs, ABI-Vertraege und Fixpunktzustand erneut aufbauen. Spaete,
monoton neu entdeckte Seeds machen bereits abgeschlossene, unveraenderte
SCC-Arbeit dadurch wertlos.

### P0-B: Root-lokale Wiederholung gleicher Guarded-Inventory-Kontexte

Viele Roots laufen durch identische nachgelagerte Funktions- und
Storekontexte. Der aktuelle Cache kann nur identische vollstaendige
Evaluationen treffen; Rootprovenienz und zu breite Eingangszustaende
verhindern die Zusammenfuehrung semantisch gleicher Arbeit.

### P0-C: Zu breite, nicht erklaerbare Evaluation-Schluessel

Eine Hitquote von etwa acht Prozent ist kein Korrektheitsproblem. Sie zeigt
aber, dass der Cache kaum semantische Uebereinstimmung erkennt. Ohne
Miss-Reason-Ledger ist nicht belegbar, ob Ingress, Summaryversion,
Inventory-Linse, Isolation, Verdraengung oder fehlende Exact-Replay-Daten den
Miss verursacht.

### P0-D: Grobe Rootjobs und kanonischer Head-of-Line-Block

Der FIFO-Executor kann keine lange Roottransitive Arbeit zerlegen oder den
kritischen publizierbaren Praefix priorisieren. Freie Worker und fertige
spaetere Roots helfen nicht, wenn Root 17 den kanonischen Fold blockiert.

### P0-E: Speicherverstaerkung

Die aktuelle Callee-Projektion konstruiert den Stackzustand bereits direkt
und darf nicht auf die fruehere Komplettkopie zurueckfallen. Register und
`memory_values` werden dabei weiterhin vollstaendig kopiert; auch
`AbstractState`, Callargumente, Inventory-Transfers, Evidence-Sets, Maps und
Vektoren werden an anderen Stellen vielfach konservativ dupliziert.
Root-lokale Kontexte und ein grosser Evaluationcache konkurrieren um RAM.
Ein schnellerer Pfad, der auf 16-GiB-Rechnern paginiert, ist kein
erfolgreicher Performancefix.

### P0-F: Zu grobe persistente Wiederverwendung

Der vollstaendige Boot-Analysis-Cache ist wertvoll, trifft aber nur bei
kompletter Identitaet. Nach einer kleinen Analyzer- oder Seedveraenderung
fehlt eine sichere Zwischenebene, die unveraenderte Programmarenen, SCCs und
IR-Shards weiterverwendet.

## Zielarchitektur

### Immutable FunctionProgramGraph

Eine `FunctionValueAnalysisSession` erhaelt ein versioniertes,
content-addressed `FunctionProgramGraph`:

```text
FunctionProgramGraph
  - immutable Block-/Instruction-Arena
  - FunctionFingerprint je Funktion
  - Block- und Ownerindizes
  - direkte und indirekte Dependency-Indizes
  - Caller-/Callee-Graph
  - SCC-DAG und topologische Ordnung
  - ABI-Read-/Write-/Persistent-Store-Vertraege
  - Inventory-Sink- und Tail-Ingress-Indizes
```

Eine neue Seedmenge erweitert dieses Programm monoton. Unveraenderte
Funktionsfingerprints behalten ihre IDs und abgeleiteten Vertraege. Nur neu
dekodierte oder in ihrer Grenze veraenderte Funktionen bauen ihre lokalen
Strukturen neu.

### Semantische Cachelinsen

Statt den kompletten `AbstractState` und alle globalen Maps in jeden
Evaluation-Key zu serialisieren, erhaelt jede Auswertungsart eine explizite
Linse:

```text
EvaluationLens
  - Summary
  - CandidateContract
  - GuardedInventory
  - ContextualReturn
  - IsolatedObservation
```

Die Linse projiziert nur die durch bewiesene ABI-Read-Sets, benoetigte
Stackslots, relevante Memoryfacts, Tail-Ingress und Inventory-Sinks
beobachtbaren Eingaben. Die Projektion ist selbst versioniert und wird gegen
eine konservative Vollzustandsauswertung geprueft.

Ist ein Read-Set, Stackbezug oder Inventory-Sink unvollstaendig,
mehrdeutig oder budgetbedingt abgeschnitten, darf die Linse nichts
weglassen. Der Eintrag verwendet dann den konservativen Vollzustand oder ist
nicht cachebar; ein enger Key darf nie aus unvollstaendiger Abhaengigkeits-
evidenz entstehen.

Jeder Lookup erhaelt zunaechst genau einen stabilen Ausgang:

- `ReadyHit`;
- `InFlightCoalesced`;
- `Miss`.

Jeder echte `Miss` erhaelt danach genau einen primaeren Grund:

- `Cold`;
- `Evicted`;
- `OversizeOrNoExactReplay`;
- `FunctionShapeChanged`;
- `ProjectedIngressChanged`;
- `SummaryDependencyChanged`;
- `AbiContractChanged`;
- `ResolutionLensChanged`;
- `InventorySinkChanged`;
- `IsolationPartitionChanged`;
- `ContextualSummaryChanged`;
- `TailIngressChanged`.

Die Summe der primaeren Missgruende muss der Zahl der echten Misses
entsprechen. Aendern sich mehrere Keykomponenten, bestimmt eine versionierte
Prioritaetsreihenfolge den primaeren Grund; eine optionale Secondary-Bitmaske
bewahrt die weiteren Ursachen. `ReadyHit` und `InFlightCoalesced` werden
separat ausgewiesen. Fuer Vergleiche mit alten Logs darf zusaetzlich ihre
Summe als historischer `cache_hits`-Wert ausgegeben werden.

### Inkrementelle SCC-Session

Eine `AnalysisEpoch` bindet:

- ProgramGraph-Identitaet;
- Seed- und Overrideidentitaet;
- Summary-, Candidate- und Inventory-Schema;
- Analyzer-Implementierungsidentitaet;
- options- und ABI-relevante Vertraege.

Jede SCC besitzt Eingangs-, Ergebnis- und Dependency-Versionen. Eine
Aenderung invalidiert nur:

1. direkt veraenderte Funktionen;
2. deren SCC;
3. abhaengige Caller-SCCs;
4. betroffene Candidate-/Inventory-Sinks;
5. explizit korrelierte isolierte Partitionen.

Der Rest bleibt gueltig. Die Veroeffentlichung einer Epoch ist atomar. Bei
Budgetfehler, Exception, Cachekorruption oder Invariantenbruch wird die neue
Epoch verworfen; die alte Epoch wird niemals teilweise ueberschrieben.

### Gemeinsamer Multi-Root-Fixpunkt

Guarded-Inventory-Arbeit wird nach einem kanonischen `ContextKey` global
geteilt:

```text
ContextKey
  - Zielfunktion
  - Transferart
  - Isolation-/Korrelationspartition
  - projizierter Eingangszustand
  - relevante Dependency-Versionen
```

Ein identischer Context wird einmal ausgewertet. Zu ihm gehoert eine
internierte Menge von Rootprovenienzen. Neue Rootquellen erweitern diese
Menge monoton und fuehren nur dann zu neuer semantischer Arbeit, wenn die
Provenienz selbst im Ergebnis beobachtbar ist.

Physische Wiederverwendung und logische Budgets sind getrennt. Ein geteilter
Context darf die bestehenden per-Root-/per-Isolations-Zaehler fuer
Contextzahl, Reevaluation und Root-Callsites weder mehrfach belasten noch
verbergen. Seriell/cachelos und dedupliziert muessen bei identischen
logischen Fakten dieselben FIFO-Referenzdiagnosen, Caps und
Truncationergebnisse erzeugen. Ein zusaetzliches globales Ressourcenbudget
begrenzt nur Scheduler und RAM; ein Root darf keinen anderen Root still
abschneiden.

Korrelationen duerfen nicht erfunden werden:

- isolierte Callsites bleiben in getrennten Partitionen;
- alternative Rootquellen werden nicht als gleichzeitig wahr angenommen;
- ein Join ueber mehrere Owner wird als guarded/incomplete markiert, wenn die
  bestehende Semantik das verlangt;
- Evidence-Sets werden intern und kanonisch sortiert, nicht abgeschnitten.

### Inkrementeller Seed-/Candidate-Contract-Fixpunkt

Neue Resolution- und Inventory-Seeds werden als monotone Fakten mit Ursache
publiziert. Sie erweitern den ProgramGraph und markieren nur betroffene
SCCs dirty. Der gesamte Function-Value-Aufruf wird nicht erneut gestartet,
solange die restliche Programmarena unveraendert ist.

Die finale Inventory-Materialisierung erfolgt erst nach stabilem relationalem
Zustand. Seed-erzeugende Fakten bleiben jedoch waehrend des Fixpunkts
sichtbar, damit kein valider AOT-Kandidat bis zur Abschlussrunde verloren
geht.

### Priorisierter, teilbarer Executor

Der globale Pool bleibt erhalten. Seine Queue wird durch typisierte
Workitems ersetzt:

```text
AnalysisWorkItem
  - Phase und Dependency-Epoch
  - SCC/Context/Root
  - geschaetzte Kosten und Fanout
  - kritischer kanonischer Praefix
  - Speicherbudget
  - teilbare Fortsetzung
```

Prioritaet erhalten:

1. Arbeit, die neue Seeds oder SCC-Invalidierungen freigibt;
2. Arbeit auf dem kritischen kanonischen Publikationspfad;
3. kleine unblockende SCC-/Contextjobs;
4. unabhaengige Durchsatzarbeit.

Lange Roottransitive Walks werden an sicheren Contextgrenzen teilbar. Ein
Workitem publiziert Ergebnisse weiter transaktional und kanonisch; die
Ausfuehrungsreihenfolge darf das Ergebnis nicht beeinflussen.

### Speicherhaushalt

Der Analyzer erhaelt ein gemeinsames Budget fuer:

- Evaluationcache;
- internierte States/Evidence-Sets;
- aktive Contextworklists;
- persistente ProgramGraph-/SCC-Shards;
- optionalen GPU-Staging- und VRAM-Bedarf.

Unter Druck werden zuerst reproduzierbar erneut berechenbare Caches
verdraengt oder auf atomare persistente Shards ausgelagert. Semantische
Fakten werden nie wegen RAM-Druck abgeschnitten. Wenn selbst die
Referenzauswertung nicht innerhalb des konfigurierten Sicherheitsbudgets
moeglich ist, endet der Export typisiert und fail-closed.

### Schichtweiser persistenter Cache

Die Cachehierarchie wird um folgende content-addressed Schichten erweitert:

```text
Disc-/Image-Identitaet
  -> FunctionProgramGraph-Shards
  -> ABI-/Dependency-Vertraege
  -> SCC-Summary-/Candidate-Zustaende
  -> Guarded-Inventory-Kontexte und -Ergebnisse
  -> Lowered-/Optimized-IR-Shards
  -> vorhandener Partition-Codegen-Cache
  -> vorhandener Hostobjekt-/Runtime-Buildpfad
```

Jeder Shard bindet ausschliesslich seine semantisch relevanten
Komponenten-IDs. Der Git-Commit bleibt Provenienz, ist aber nicht pauschal
der Invalidator aller Ebenen. Fehlende, fremde, alte, beschaedigte oder
unvollstaendig geschriebene Shards sind sichere Misses und werden atomar neu
erzeugt.

## Normative Korrektheitsinvarianten

Alle Aufgaben dieses Plans muessen folgende Invarianten gemeinsam erfuellen:

1. Cache und Scheduling sind reine Optimierungen. Mit deaktiviertem Cache,
   erzwungener Verdraengung oder beschaedigten Shards entsteht dasselbe
   kanonische Ergebnis.
2. Die Analyse erfindet keine CFG-Kante, keinen Runtimewert und keinen
   Codepointer. Unbekannte Livewerte bleiben runtime-autoritativ.
3. Root-, Callsite- und Ownerkorrelationen werden nicht durch einen
   Performancejoin verschaerft.
4. Alle Budget- und Verlustdiagnosen bleiben fail-closed, insbesondere
   Candidate-, Raw-Inventory-, Tabellen-, Shape-, Forwarding-, Context-,
   Stackbasis- und lokale Fixpunktverluste.
5. Seriell sowie mit 8, 12 und 24 Threads entstehen bytegleiche kanonische
   Analyse-JSONs, IR, Source-Maps, generierte TUs und Produktartefakte;
   ausgenommen sind ausdruecklich nichtkanonische Timing- und
   Provenienzfelder.
6. Ein Performancegate darf nicht durch weniger Funktionen, Bloecke,
   Guarded-AOT-Einstiege, Resolutionen oder Inventory-Sinks erreicht werden.
7. Cachekeys binden Sourceinhalt, relevante ABI-/Schema-/Optionswerte und
   komponentenbezogene Implementierungsidentitaeten.
8. Persistente Ergebnisse werden erst nach vollstaendiger
   Hash-/Strukturpruefung atomar sichtbar.
9. RAM-Druck bewirkt Eviction, Spill oder typisierten Abbruch, niemals
   stilles Truncation.
10. GPU-Beschleunigung ist niemals Korrektheitsvoraussetzung. Jeder
    CPU-only-Host bleibt voll unterstuetzt.

## GPU-Offload: belastbare Machbarkeitsgrenze

### Aktueller Sourcebefund

Katana besitzt derzeit keine Compute-Abstraktion fuer den Analyzer. Der
einzige GPU-Backendpfad ist der Win32-D3D11-Presenter in
`src/runtime/host_video_d3d11.cpp`. Er erstellt ein privates, als
`D3D11_CREATE_DEVICE_SINGLETHREADED` markiertes Presentationsgeraet sowie
Vertex- und Pixelshader. Es existieren keine Compute-Shader, Dispatches oder
portable CUDA-, OpenCL-, Vulkan-, Metal- oder wgpu-Analysepfade.

Das Runtime-Presentationsgeraet darf nicht an den CLI-Analyzer gekoppelt
werden. Ein optionaler Analyse-Compute-Pfad benoetigt ein eigenes
capability-gated Backend, eigene Lebensdauer und CPU-Fallback.

### Ungeeignete GPU-Arbeit

Folgende aktuelle Hotpathklassen bleiben auf der CPU:

- SCC-/Tarjan- und Dependency-Graphsteuerung;
- pointer-, map- und set-lastige `AbstractState`-Worklists;
- datenabhaengige SH-4-Transferfunktionen mit hoher Verzweigung;
- Rootprovenienz-, Isolation- und Evidence-Korrelation;
- kanonischer finaler Fold und transaktionale Cachepublikation;
- kleine oder haeufig synchronisierte Batches.

Diese Arbeit besitzt wenig gleichfoermige Arithmetik, viele variable
Allokationen und hohe Synchronisationskosten. Eine direkte Portierung auf
die GPU wuerde den eigentlichen Architekturfehler nur in ein neues Backend
verschieben.

### Nur nach CPU-Flachlegung prototypfaehige Kerne

Folgende Kerne duerfen nach Messung einen begrenzten Prototyp erhalten:

1. Hashing vieler bereits gepackter, unabhaengiger
   `ProjectedIngress`-/Cachekey-Puffer;
2. breite Bitset-/Lattice-Joins fuer internierte, fest dimensionierte
   Candidate- und Evidence-Sets;
3. Transfer gleicher Opcode-/Blockformen ueber sehr grosse homogene
   Contextbatches;
4. zusammenhaengende Shape-/Decode-Validierung grosser Kandidatenarrays.

Disc-SHA darf nur prototypisch vermessen werden. Der gemessene
Gesamtanteil von rund 5,4 Sekunden ist zu klein fuer ein P0-Zeitproblem, und
die PCIe-/UMA-Transferkosten koennen den Gewinn leicht uebersteigen. Der
kanonische Track-SHA-Vertrag darf nicht durch einen anderen Hashbaum ersetzt
werden.

Fuer jeden Kandidaten muss zuerst eine CPU-SIMD-/Threadpool-Referenz
existieren. Erst wenn Profiling grosse, homogene Batches und ausreichende
arithmetische Intensitaet belegt, wird ein D3D11-Compute-Prototyp auf Windows
gebaut. Eine spaetere portable Backendentscheidung folgt aus Messdaten, nicht
aus der vorhandenen Presenter-API.

### GPU-Entscheidungsgate

Ein GPU-Kern darf nur in den Produktpfad, wenn alle folgenden Kriterien
erfuellt sind:

- mindestens zweifacher Phasendurchsatz gegen die optimierte CPU-Referenz,
  einschliesslich Geraeteerzeugung, Shadercompile, H2D und D2H;
- mindestens 15 Prozent schnellere mediane End-to-End-Kaltport-Walltime auf
  zwei repraesentativen diskreten GPUs;
- keine End-to-End-Regression auf einer repraesentativen iGPU;
- keine Regression und keine neue Abhaengigkeit auf CPU-only- oder
  nicht unterstuetzten Hosts;
- hoechstens 1 GiB zusaetzlicher Analyzer-VRAM-Bedarf;
- Host-RAM bleibt innerhalb der Threadklassen-Gates;
- bytegleiche CPU-/GPU-Ergebnisse bei normalen, zufaellig permutierten und
  fehlerinjizierten Ausfuehrungen;
- jeder GPU-Wait ist zeitlich gebunden;
- Device-Lost, Timeout, Shader-/Treiberfehler sowie nachweisbare Struktur-,
  Bounds-, Digest- oder unvollstaendige Rueckgaben verwerfen das gesamte
  GPU-Batch und berechnen es auf der CPU neu;
- unbekannte oder nicht vermessene Devices bleiben auf CPU, bis ein
  zeitlich gebundener Crossover-Test den konkreten Batchtyp als schneller
  belegt.

Beliebige stille Rechenfehler koennen ohne CPU-Gegenrechnung nicht allgemein
erkannt werden. Deshalb pruefen Differenz-/Fehlerinjektionstests jede
GPU-Ausgabe gegen CPU; der Produktpfad verspricht nur die explizit
validierbaren API-, Struktur-, Bounds- und Digestfehler und erfindet keine
unbelegbare Vollerkennung.

Erreicht ein Prototyp diese Schwellen nicht, wird er nicht integriert. Das
negative, reproduzierbare Messergebnis schliesst das P0-Entscheidungsgate
ordnungsgemaess; ein langsamer GPU-Pfad bleibt nicht als Wartungslast liegen.

## Fortschritts- und Telemetrievertrag

Jede Phase, die laenger als zehn Sekunden dauern kann, meldet versioniert:

- aktuelle Phase/Subphase und stabile Work-ID;
- bekannte, dynamisch hinzugekommene und abgeschlossene Gesamtarbeit;
- geplant, queued, aktiv, fertig-aber-nicht-publizierbar und kanonisch
  publiziert;
- aktive Worker und konfigurierte Threadzahl;
- CPU-Zeit, Walltime und effektive CPU-Auslastung;
- aktuelle und maximale RSS/Working Set/Private Bytes;
- Cachehits, Misses, In-Flight-Coalesces, Evictions, Bytes und Missgruende;
- SCC-/Root-/Context-Fanout und Laufzeitquantile;
- Seedzugang nach Quelle und dadurch invalidierte SCCs;
- bei GPU-Nutzung Device/Backend, Batchgroesse, H2D/D2H, Kernelzeit,
  VRAM-Peak und CPU-Fallbacks;
- belastbare ETA nur bei stabilem Nenner, sonst explizit `growing-workset`.

Der kanonische Praefix bleibt eine wichtige Korrektheitsmetrik, wird aber nie
mehr allein als `completed` dargestellt.

## Reproduzierbarer Kalt-/Warmvertrag

Ein Performancewert ist nur gueltig, wenn sein Cache- und Buildzustand
explizit ist.

Jeder Gatebericht bindet ein Referenzhostmanifest mit CPU-Modell,
physischen/logischen Kernen, SMT-Status, RAM-Kapazitaet, SSD/Dateisystem,
OS-Build, Energieprofil, Compiler-/Linker-/CMake-/Generatorversion,
Katana-Buildprofil und allen Job-/Cacheoptionen. Die 8-/12-/24-
Konfiguration bezeichnet die explizite Analyzer- und Buildjobgrenze auf
diesem Host, nicht drei unbeschriebene Rechner.

`cold-port` bedeutet:

- Katana-CLI, Compiler und das stabile Runtime-SDK sind bereits gebaut; ihr
  eigener Toolchainbootstrap ist nicht Teil eines Spielportexports;
- neuer task-lokaler Ausgabe-, Workspace- und Cache-Root;
- keine Whole-Export-, Boot-Analysis-, Function/SCC-, Latent-AOT-, Codegen-
  oder spielbezogenen Compilerobjekte aus einem frueheren Lauf;
- das stabile vorgebaute Runtime-SDK darf und soll wiederverwendet werden,
  sofern seine exakte Komponentenidentitaet passt;
- OS-Dateicache wird nicht kuenstlich global geleert, aber sein Einfluss
  wird durch drei Wiederholungen und getrennte Disc-I/O-Zeit sichtbar;
- Walltime umfasst Analyse, IR, Codegen, Hostcompile, Link und Packaging
  samt allen Childprozessen.

CPU-Zeit und Speicher werden fuer den ganzen Prozessbaum erfasst. Das
Speichergate verwendet den maximalen gleichzeitig beobachteten
Process-Tree-Stand fuer Private Bytes/Commit; Working Set, Hard/Pagefaults
und I/O werden daneben berichtet. Ein einzelner Elternprozesswert darf
Compiler- und Linkerchildren nicht verstecken.

`warm-identical` bedeutet:

- identische Source-, Disc-, Optionen-, ABI-/Schema- und
  Komponentenidentitaeten;
- derselbe sichere Cache-Root;
- unveraenderter Dateibaum und unveraenderte Runtime-/Toolchainbindung.

Die retailfreie Stressform wird deterministisch lokal erzeugt und nicht als
grosser Blob eingecheckt. Sie bildet neben Funktions-, Block-, Root- und
logischen Evaluationrequests/-kontexten auch spaete Seedwellen,
Modul-/Chunkstruktur, Partitionen und generierte Host-TUs ab. Die rund 84.000
Requests sind eine logische Vergleichslast; die Zahl physisch ausgefuehrter
Evaluationen soll durch sichere Deduplication gerade sinken. Ein schneller
Analyse-Microbenchmark allein gilt nicht als voller Kaltport.

## Aufgaben

### KR-4974 - Reproduzierbare Kaltbuild-Telemetrie und Miss-Reason-Ledger

Typ: P0 Implementierung

Ziel: Jede lange Phase und jeder Evaluationmiss wird so gemessen, dass
Architekturarbeit nach Zeit, CPU, RAM, Fanout und Ursache priorisiert werden
kann.

Umfang:

- versioniertes JSONL- und menschenlesbares Fortschrittsformat;
- echte Phasen-/Subphasentimer inklusive CFG-Runden und
  Candidate-Contract-Iterationen;
- geplante/aktive/ready/committed-Metriken;
- Miss-Reason-Ledger und Cache-Evictions;
- Referenzhost-/Toolchain-/Buildprofilmanifest;
- Prozessbaum-CPU, Private/Commit/Working Set, Pagefaults und I/O sowie
  optional GPU-/Transfermetriken;
- oeffentlich reproduzierbare synthetische Stressform.

Akzeptanz:

- `lookups = ready_hits + in_flight_coalesces + misses`, und die primaeren
  Missgruende summieren sich exakt zu `misses`;
- keine Phase kann zehn Sekunden ohne Fortschritts- oder Heartbeatdatensatz
  laufen;
- Messung erklaert Head-of-Line-Stalls und wachsende Worksets;
- deaktivierte Telemetrie veraendert kein kanonisches Ergebnis.

### KR-4975 - Semantische FunctionEvaluation-Key-Projektion und Cachelinsen

Typ: P0 Implementierung

Abhaengigkeit: KR-4974

Ziel: Nur tatsaechlich beobachtbare Eingaben invalidieren eine Evaluation.

Umfang:

- versionierte `EvaluationLens` je Auswertungsart;
- Projektion ueber bewiesene ABI-Read-Sets und benoetigte
  Stack-/Memoryfacts;
- komponentisierte Keydigests und erklaerbare Missgruende;
- internierte kanonische Candidate-/Evidence-Sets;
- In-Flight-Coalescing fuer identische projizierte Keys.

Akzeptanz:

- Vollzustands- und Linsenauswertung sind in fokussierten Differenztests
  identisch;
- irrelevante Register-/Memory-Aenderungen erzeugen keinen Miss;
- relevante Aenderungen koennen keinen falschen Hit erzeugen;
- unvollstaendige Read-/Stack-/Sink-Evidenz verwendet Vollzustand oder
  deaktiviert den Cache;
- Hitquote und vermiedene Evaluationzeit werden getrennt ausgewiesen.

### KR-4976 - Persistente FunctionValue-Programm-/SCC-Session

Typ: P0 Implementierung

Abhaengigkeiten: KR-4974, KR-4975

Ziel: ProgramGraph, SCC-DAG, ABI-Vertraege und stabile Summaryzustande leben
ueber Candidate- und aeussere CFG-Runden hinweg.

Umfang:

- immutable, content-addressed ProgramGraph-Arena;
- stabile Function-/Block-/SCC-Fingerprints;
- versionierte Dependency- und Summaryzustande;
- transactional `AnalysisEpoch`;
- bestehende direkte Callee-Stackprojektion erhalten und Register-/Memory-
  Fakten nur bei vollstaendiger Read-Evidenz weiter verengen.

Akzeptanz:

- unveraenderte Funktionen bauen Graph-/ABI-Daten nicht erneut;
- eine neue Funktion invalidiert nur ihren nachweisbaren
  Dependency-Closure;
- unvollstaendige Register-/Memory-Read-Evidenz behaelt die konservativen
  Fakten;
- Abbruch oder Exception publiziert keine halbe Epoch;
- Peak-RAM sinkt oder bleibt innerhalb der Gates.

### KR-4977 - Gemeinsamer Multi-Root-Guarded-Inventory-Fixpunkt

Typ: P0 Implementierung

Abhaengigkeiten: KR-4975, KR-4976

Ziel: Semantisch gleiche forwarded/contextual Inventory-Kontexte werden
global einmal ausgewertet, ohne Rootkorrelation zu erfinden.

Umfang:

- globaler `ContextKey` und internierte Rootprovenienz;
- gemeinsame Contextworklist ueber alle Roots;
- explizite Isolation-/Korrelationspartitionen;
- einheitlicher forwarded und contextual Return Coordinator;
- partielle, sichere Continuations fuer lange Contextketten.

Akzeptanz:

- identischer Context wird pro Dependency-Version einmal ausgewertet;
- Root-/Callsite-Korrelation entspricht dem CPU-Referenzpfad;
- physische Deduplication und logische per-Root-/per-Isolations-
  Budgetzaehlung sind getrennt;
- alle bisherigen Forwarding-/Contextbudgets und FIFO-Diagnosen bleiben
  seriell/parallel identisch, sichtbar und fail-closed;
- Root-17-artige Tailarbeit wird durch Context- und Fanoutmetriken erklaert
  und materiell reduziert.

### KR-4978 - Inkrementeller CFG-/Seed-/Candidate-Contract-Fixpunkt

Typ: P0 Implementierung

Abhaengigkeiten: KR-4976, KR-4977

Ziel: Spaete monotone Seeds berechnen nur betroffene SCCs und
Inventory-Sinks neu.

Umfang:

- typisierte monotone Seedfakten mit Ursache;
- Dirty-SCC-/Caller-/Sink-Invalidierung;
- inkrementelle Funktionsgrenzen und ProgramGraph-Erweiterung;
- finale Inventory-Materialisierung nach relationaler Stabilitaet;
- kein kompletter Function-Value-Neustart bei lokalem Seedzuwachs.

Akzeptanz:

- Pass-15-zu-Pass-22-artiger Seedzuwachs zeigt die exakt invalidierten SCCs;
- unveraenderte SCCs werden nicht erneut ausgewertet;
- Seed-, Funktions-, Block-, AOT- und Resolutionmengen bleiben exakt;
- jeder nicht darstellbare inkrementelle Zustand faellt auf eine
  konservative CPU-Neuberechnung zurueck.

### KR-4979 - Priorisierter Analyseexecutor und begrenzter Speicherhaushalt

Typ: P0 Implementierung

Abhaengigkeiten: KR-4974, KR-4977, KR-4978

Ziel: Alle konfigurierten Threads arbeiten auf teilbaren, unblockenden Jobs,
ohne den Host durch RAM-Druck unbrauchbar zu machen.

Umfang:

- typisierte Workitems mit Kosten-, Fanout- und Critical-Prefix-Prioritaet;
- teilbare SCC-/Context-/Root-Continuations;
- ein globaler Executor fuer alle Analysephasen;
- gemeinsames RAM-/Cache-/Contextbudget;
- sichere Eviction/Spill-/Recompute-Regeln;
- faire Nested-Work-Unterstuetzung ohne Threadpoolvermehrung.

Akzeptanz:

- schwere Analysefenster nutzen mindestens 75 Prozent der konfigurierten
  Kerne, ausgenommen kurze I/O- und kanonische Endtails;
- fertig-aber-blockiert und echter Leerlauf sind getrennt sichtbar;
- kein Paging-Sturm, OOM oder semantisches Truncation unter dem 16-GiB-Gate;
- 1-, 8-, 12- und 24-Thread-Ergebnisse sind kanonisch identisch.

### KR-4980 - Schichtweiser persistenter NativeDisc-Buildcache

Typ: P0 Implementierung

Abhaengigkeiten: KR-4975, KR-4976, KR-4978

Ziel: Ein Whole-Analysis-Miss verwirft keine semantisch unveraenderten
Programm-, SCC-, Inventory- oder IR-Ergebnisse.

Umfang:

- atomare ProgramGraph-, ABI-, SCC-, Inventory- und IR-Shards;
- positive und negative Ergebnisse mit typisiertem Grund;
- komponentenbezogene Implementierungsidentitaeten;
- Cacheinspektion, Groessenlimit, LRU/Spill und Korruptionsdiagnose;
- bestehende Whole-Export-, Latent-AOT-, Codegen- und Hostbuildcaches
  weiterverwenden.

Akzeptanz:

- alter oder beschaedigter Cache ist ein sicherer Miss;
- kleine lokale Analyzerveraenderung invalidiert nur gebundene Schichten;
- kein vertrauenswuerdiger Cache ist von einem absoluten Ausgabeordner
  abhaengig;
- identischer Warmexport erreicht das Warmgate.

### KR-4981 - 8-/12-/24-Thread-Kaltbuild-Performancegate

Typ: P0 Gate-Vorbereitung

Abhaengigkeiten: KR-4974 bis KR-4980 sowie KR-4982; KR-4983 nur bei positivem
GPU-Entscheidungsgate

Ziel: Die neue Architektur wird vor dem unabhaengigen Schlussreview mit
reproduzierbarer, retailfreier oeffentlicher Last gegen harte Zeit-, RAM- und
Korrektheitsgrenzen abgenommen. Dieser Task baut noch keinen privaten
Sonic-Port.

Akzeptanz:

| Konfiguration | Analyse und Codegen | voller kalter Port | Process-Tree Private/Commit-Peak |
|---|---:|---:|---:|
| 24 Threads | hoechstens 6 min | hoechstens 8 min | hoechstens 12 GiB |
| 12 Threads | hoechstens 9 min | hoechstens 11 min | hoechstens 10 GiB |
| 8 Threads | hoechstens 12 min | hoechstens 15 min | hoechstens 8 GiB |

Zusaetzlich:

- unveraenderter exakter Warmexport hoechstens 30 Sekunden;
- drei Wiederholungen je oeffentlicher Threadklasse, Median und Maximum
  dokumentiert;
- kein Einzelprozess und keine Phase laenger als 15 Minuten;
- etwa 1.600 Funktionen, 22.400 Bloecke, 1.400 Roots, 84.000 logische
  Evaluationrequests/-kontexte und spaete Seeds sind in einer retailfreien
  Stressform abbildbar;
- die Stressform prueft gleiche Funktions-/Block-/AOT-Vollstaendigkeit
  zwischen Referenz- und Performancepfad.

### KR-4982 - GPU-Offload-Entscheidungsgate und repraesentativer Prototyp

Typ: P0 Entscheidungs- und Benchmarktask

Abhaengigkeiten: KR-4974, KR-4975, KR-4977, KR-4979

Ziel: Fuer jeden plausiblen Batchkern wird gegen den bereits optimierten
CPU-Pfad mit End-to-End-Kosten entschieden, ob GPU-Offload Katana wirklich
beschleunigt. Kernelinventar und Batchprofiling duerfen nach KR-4974
beginnen; Prototypbenchmark und Gateabschluss warten auf KR-4975, KR-4977
und KR-4979.

Umfang:

- CPU-Profil, Batchgroesse, Bytes und arithmetische Intensitaet erfassen;
- optimierte CPU-SIMD-/Threadpool-Referenz;
- begrenzter D3D11-Compute-Prototyp fuer hoechstens die belegten Kandidaten;
- Setup-, Compile-, H2D-, Kernel-, D2H-, RAM- und VRAM-Messung;
- gebundener D3D11-Wait/Timeout sowie Device-Lost- und strukturell
  validierbare Fehler-Fallbacktests;
- per-Device-/Batch-Crossoverprofil; nicht vermessene oder langsamere
  Geraete fallen automatisch auf CPU;
- schriftliche Integrate-/Reject-Entscheidung je Kernel.

Akzeptanz: Es gelten ausnahmslos die oben definierten GPU-Schwellen.

### KR-4983 - Deterministische capability-gated GPU-Beschleunigung

Typ: bedingte P0 Implementierung

Abhaengigkeit: positives KR-4982-Gate

Ziel: Nur ein nachweislich schneller GPU-Kern wird als optionale
Beschleunigung integriert.

Umfang:

- separates Analyse-Compute-Backend, keine Kopplung an den Runtime-Presenter;
- Capability-/Treiber-/Speicher- und per-Device-Crossoverpruefung;
- komplette CPU-Referenz und automatische Batch-Neuberechnung bei
  API-/Timeout-/Device-Lost-/Struktur-/Digestfehler;
- deterministische kanonische Ein-/Ausgabe und atomare Uebernahme;
- Telemetrie und Abschaltoption.

Akzeptanz:

- alle KR-4982-Schwellen bleiben im integrierten End-to-End-Pfad erfuellt;
- GPU an/aus, unsupported, Device-Lost und erkannte API-/Struktur-/
  Digestfehler ergeben bytegleiche kanonische Artefakte;
- faellt KR-4982 negativ aus, wird KR-4983 als nicht erforderlich
  dokumentiert und es bleibt kein inaktiver Produktbackend-Rest.

### KR-4984 - Unabhaengige Gesamtpruefung und P0/P1-Schliessung vor NativeDisc-Produktlauf

Typ: letzter P0 Gate-Vorbereitungstask

Abhaengigkeiten: KR-4981, KR-4982 und gegebenenfalls KR-4983

Ziel: Ein unabhaengiger Reviewer verfolgt alle geaenderten Daten- und
Fehlerpfade end-to-end. Der reale NativeDisc-Lauf bleibt gesperrt, bis jeder
P0/P1 geschlossen und nachgeprueft ist.

Pflichtumfang:

- Decoder-/CFG-/Seed-/Boundary-Pfad;
- Function-Value-Programm, Summary, SCC, ABI-State und Candidate-Contract;
- forwarded/contextual Guarded Inventory, Rootprovenienz und Isolation;
- Cachelinsen, persistente Shards, Korruption, Eviction und Migration;
- Executor, Scheduling, Fortschritt, Abbruch und RAM-Haushalt;
- exakte Latent-AOT-Hints, bytegleiche Multi-Extent-Sourcebindings und
  Template-/Cacheidentitaeten;
- IR, Codegen, Whole-Export, Hostbuild und Packaging;
- optionaler GPU-Compute-Pfad und CPU-Fallback;
- Runtime-CPU-Last, Runtime-Parallelwork sowie D3D11-/CPU-
  Ausgabepresentation als getrennte, aber durch gemeinsame Buildprofile
  beruehrte Pfade;
- sichtbarer-Frame-Gate, Entry-Baseline, veraenderte Pixel-/Kachelverteilung
  und echter Screenshot als einziges Produktbildbeweismittel;
- alle fail-closed Export- und Runtimevollstaendigkeitsdiagnosen.

Akzeptanz:

- Runtime-Vorher/Nachher verwendet dieselbe Post-Entry-Gastarbeit, Eingabe
  und sichtbaren Meilenstein; paced und unpaced werden getrennt;
- berichtet werden Gesamt-CPU-Zeit, CPU-Zeit pro Gastzyklus, effektive
  Kernauslastung, Thread-/Hotspotanteile, Busy-Wait/Framepacing,
  Hostframes, Working Set und Pagefaults;
- berichtet werden aktives Presenterbackend und jeder Fallback,
  Uploadbytes/-anzahl, Upload-/Present-/Blockzeit sowie, soweit die
  Plattform sie belastbar liefert, GPU-Zeit, Auslastung und VRAM;
- kein ungebundener Busy-Spin; wartende Runtimeworker blockieren oder
  schlafen an einem architektonisch korrekten Ereignis;
- die bestaetigte Runtime-CPU-Hotspotarbeit pro gleicher Gastarbeit sinkt
  gegen die vor dem Umbau erfasste Baseline mindestens 20 Prozent, ohne
  sichtbaren Meilenstein, Hostframes oder Gastfortschritt zu reduzieren;
- Runtime-Multicore oder GPU wird nur fuer einen gemessenen Hotspot
  aktiviert und muss CPU-Zeit oder Walltime end-to-end verbessern; reine
  Kern-/GPU-Auslastung ist kein Erfolgskriterium;
- Review liefert ein pfadbezogenes Finding-Ledger;
- jeder P0/P1 ist behoben, erneut reviewed und sein betroffenes
  Performance-/Korrektheitsgate wiederholt;
- keine unverdrahtete Option, Telemetrie, Cacheidentitaet, Fallback- oder
  Packaginggrenze bleibt offen;
- erst danach wird genau ein frischer NativeDisc-Port gebaut, installiert
  und mit echtem Screenshot bis ueber die bekannte Sega-/Schwarzgrenze
  geprueft;
- der abschliessende private Kaltport bestaetigt auf dem 24-Thread-
  Referenzhost ebenfalls das 8-Minuten-Gesamtziel; bei Verfehlung bleibt der
  P0 offen und der betroffene Implementierungs-/Reviewzyklus wird vor einem
  weiteren Produktversuch wiederholt;
- Buildgesamtzeit, Phasenzeiten, CPU/RAM/GPU-Metriken und das reale sichtbare
  Ergebnis werden berichtet, danach wird der abgeschlossene Stand committed
  und gepusht.

## Abhaengigkeitskette

```text
KR-4974 Telemetrie
  +--> KR-4975 semantische Cachelinsen
  |      +--> KR-4976 persistente Programm-/SCC-Session
  |             +--> KR-4977 globaler Multi-Root-Fixpunkt
  |                    +--> KR-4978 inkrementeller Seed-/CFG-Fixpunkt
  |                           +--> KR-4979 Executor/RAM-Haushalt
  |                           +--> KR-4980 persistenter Schichtcache
  |
  +--> KR-4982 GPU-Entscheidungsgate
           +--> KR-4983 nur bei positivem Gate

KR-4974 bis KR-4980 + GPU-Entscheidung
  -> KR-4981 8/12/24-Thread-Performancegate
  -> KR-4984 unabhaengige Gesamtpruefung und P0/P1-Schliessung
  -> genau ein frischer realer NativeDisc-Sonic-Lauf
```

KR-4975 bis KR-4980 sind keine voneinander isolierten Mikrooptimierungen.
Ihre Datenmodelle werden vor Implementierungsbeginn gemeinsam festgelegt,
damit ProgramGraph, Fingerprints, Dependency-Versionen, ContextKeys und
persistente Shards denselben kanonischen Vertrag verwenden.

## Migrations- und Rollbackvertrag

- Neue persistente Cacheformate verwenden neue Schemata und Verzeichnisse.
  Alte Caches gelten als Miss und werden nicht als Beweis migriert.
- CPU-Vollanalyse, inkrementelle Session, Multi-Root-Fixpunkt und optionaler
  GPU-Pfad besitzen waehrend der Einfuehrung getrennte Featuregates.
- Der Referenzpfad bleibt fuer fokussierte Differenztests und Shadow-Runs
  verfuegbar, bis KR-4984 geschlossen ist.
- Eine neue Analyse-Epoch wird nur atomar publiziert. Mismatch, Exception
  oder Fehlerdiagnose verwirft sie vollstaendig.
- Jeder Aufgabencommit ist einzeln rueckrollbar und veraendert weder private
  Disc-/Portdaten noch Nutzersaves.
- Ein Performance-Rollback darf keine neu bewiesene Korrektheitsluecke wieder
  oeffnen. In diesem Fall wird auf den langsameren sicheren Pfad
  zurueckgefallen.

## Nicht im Umfang

- Sonic-spezifische Adressen oder Sonderfaelle im generischen Analyzer;
- weniger Analyse-, AOT- oder Runtimeabdeckung fuer bessere Zahlen;
- Interpreter, JIT oder dynamischer Emulationsfallback;
- GPU-Nutzung als Selbstzweck;
- Kopplung des CLI-Analyzers an das Runtime-Presentationsgeraet;
- neue breite Kompatibilitaets- oder Hardwaretestmatrix;
- Runtime-Renderingumbauten ohne belegten Runtimebefund;
- Schoenrechnen ueber Warmcaches bei einem als kalt deklarierten Gate.

## Abschlussdefinition

Dieser P0-Block ist erst abgeschlossen, wenn:

1. KR-4974 bis KR-4980 implementiert und die harten Gates aus KR-4981
   bestanden sind;
2. KR-4982 eine belastbare GPU-Entscheidung liefert und KR-4983 nur bei
   positivem Ergebnis vollstaendig integriert ist;
3. alle kanonischen Ergebnisse und fail-closed Diagnosen gegen den
   Referenzpfad identisch sind;
4. KR-4984 alle betroffenen Pfade unabhaengig reviewed und alle P0/P1
   geschlossen hat;
5. erst danach ein frischer echter NativeDisc-Sonic-Lauf gebaut und mit
   echten Screenshots ausgewertet wurde;
6. Buildzeit, Phasezeiten, Ressourcen, Cachewirkung und sichtbares Ergebnis
   ehrlich berichtet und der abgeschlossene Stand auf `main` gepusht wurde.
