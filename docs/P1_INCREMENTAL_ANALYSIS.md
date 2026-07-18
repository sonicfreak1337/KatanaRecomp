# Inkrementelle Analyse und Codegen

Task: `KR-4622`

## Instruktions- und Indexmodell

`InstructionArena` sortiert und validiert dekodierte Instruktionen einmal und
gibt danach nur konstante Spans aus. `InstructionSpan` beschreibt Basic Blocks
durch Arenaoffset und Anzahl, ohne Instruktionskopien anzulegen.
`AnalysisIndex` stellt gemeinsame O(1)-Indizes fuer Instruktionen,
Blockeinstiege, Kontrollfluss-Sites/-Kanten und Funktionen sowie einen
geordneten Segmentindex bereit.

Wiederkehrende Begruendungen werden in `EvidenceInterner` als stabile
32-Bit-IDs gespeichert. Der Kontrollflussbefund baut eine gemeinsame Tabelle
aus Diagnose-, Resolution- und Jump-Table-Gruenden; typisierte
`ControlFlowEvidence`- und `AnalysisEvidenceOrigin`-Werte bleiben unveraendert.

## Deltafront und Funktionssummaries

`RecursiveAnalysisOptions::baseline` uebernimmt bereits verarbeitete
Kontextschluessel aus Adresse, Ingress, Delay-Slot-Owner und Evidenz. Neue Seeds
stellen deshalb nur neue oder staerkere Kontexte in die Decodeworklist ein.
Instruktionen, Funktionskandidaten und Diagnosen werden deterministisch mit dem
Baselinebefund vereinigt. Der Kontrollfluss-Fixpunkt nutzt diese Baseline ab
dem zweiten Durchlauf.

Die interprozedurale Wertanalyse zerlegt den direkten Callgraph mit Tarjan in
stark zusammenhaengende Komponenten. Summaries und Callee-Ingresswerte werden
weiterhin monoton vereinigt; nur bei geaenderter Summary werden Caller und nur
bei geaendertem Ingress werden Callees erneut eingereiht. SCC-Anzahl,
Summaryauswertungen und unveraenderte Ingress-Skips stehen im JSON-Bericht.

## Jump Tables

Absolute32- und SignedRelative16-Tabellen teilen einen begrenzten
Snapshotcache. Der Schluessel bindet Encoding, Dispatch, Tabellenbereich,
Zielbasis, Grenze und SHA-256 der vollstaendig committed read-only Bytes. Ein
Cachehit gibt den gesamten `JumpTableAnalysis`-Befund einschliesslich
Akzeptanz-, Evidenz- und Fehlerstatus zurueck. Der Cache ist auf 128 Snapshots
begrenzt und meldet Hits, Misses und Evictions.

## Bulk-Codegen und Cachepublish

Translation-Unit-Partitionen bleiben nach Gastadresse stabil und besitzen
einen SHA-256 ueber ihre kanonische IR. `changed_translation_unit_partitions`
liefert nur Partitionen mit geaendertem Inhalt oder geaenderten Grenzen. Die
10k-, 50k- und 100k-Fixtures besitzen feste 30-/60-/120-Sekundenbudgets und
begrenzen die Partitionsanzahl.

Codegencache-Schema 3 verwendet einen 256-Bit-SHA-256-Schluessel ueber alle
laengenpraefigierten Eingabefelder. Artefakte werden vollstaendig in einem
atomar angelegten Stagingverzeichnis geschrieben und erst danach ueber einen
atomaren Hardlink publiziert. Ein existierender gleicher Inhalt ist ein Hit;
abweichender Inhalt unter demselben Schluessel ist ein sichtbarer Fehler.
Abbruch- und Konkurrenzpfade koennen dadurch weder Teildateien publizieren
noch einen gueltigen Treffer ersetzen.

## Gate-Messwerte

KR-4625 berichtet fuer Debug und RelWithDebInfo mindestens:

- verarbeitete Delta-Workitems und wiederverwendete Kontexte
- SCC-Zahl, Summaryauswertungen und unveraenderte Ingress-Skips
- Jump-Table-Cachehits und -misses
- stabile und geaenderte Codegenpartitionen fuer 10k/50k/100k
- Codegencache-Hits sowie seriellen und parallelen Publish
