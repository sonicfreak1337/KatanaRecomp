# P1-Hotpathvertrag

Task: `KR-4621`

## Speicher

`Memory` verwendet standardmaessig einen zweistufigen 64-KiB-Seitenindex. Eine
eindeutig belegte Seite fuehrt direkt zum Regionsrecord; Seiten mit mehreren
kleinen MMIO-Regionen fallen auf die geordnete Referenzsuche zurueck. Der Modus
`MemoryLookupMode::Reference` deaktiviert den Index fuer bytegleiche
Vergleichslaeufe.

Lineare Speichergeraete lesen und schreiben 16- und 32-Bit-Werte mit genau
einer Bereichspruefung und einem ausrichtungsneutralen `memcpy`. Little Endian
bleibt auch auf Big-Endian-Hosts explizit. RAM-, VRAM- und AICA-Mappings nutzen
denselben gecachten linearen Geraetepfad. Ohne Tracehandler und Watchpoints wird
kein `MemoryAccessEvent`, Regionsstring oder Observervektor erzeugt.

`MemoryPerformanceCounters` trennt indizierte Treffer, Referenzprobes sowie
beobachtete und unbeobachtete Zugriffe. `memory_bus_tests` prueft Index und
Referenzmodus gegen dieselben Gastbytes.

## Dispatch und Invalidierung

Die Runtime-Blocktabelle besitzt zusaetzlich zu den geordneten
Referenzindizes Hashindizes fuer exakte virtuelle Blockziele. Der aktive Modus
ist `RuntimeBlockLookupMode::Direct`; `ReferenceTree` bleibt fuer
Differenztests erhalten. Physische Bereichsinvalidierung bleibt seitenindiziert
und untersucht nur Kandidaten der beruehrten Seiten.

`ExecutableCodeTracker` verwaltet stabile Blockindizes nach Identitaet und
einen Page-to-Block-Index. `CodeInvalidationLookupMode::ReferenceScan`
deaktiviert den Index. Ergebnislisten werden weiterhin sortiert und
dedupliziert, sodass beide Modi dieselben invalidierten Bloecke und
eingehenden Links liefern.

Invalidierungsprovenienz ist standardmaessig auf 1024 Ereignisse begrenzt.
Dispatchdiagnostik aggregiert Duplikate und speichert hoechstens 1024
unterschiedliche Ereignisse. Beide Pfade melden verworfene Detailereignisse
separat, ohne den monotonen Gesamtzaehler zu verlieren. Die Dreamcast-
Store-Queue-Provenienz behaelt ebenfalls nur die letzten 1024 Transfers.

## DMA

`DmaExecutionMode::SingleUnitReference` behaelt die gastzyklusgenauen
Einzelereignisse. `DeterministicBatch` fasst automatische Transfers bis zur
konfigurierten Obergrenze oder bis vor das naechste fremde Schedulerereignis
zusammen. Externe Requests und Round-Robin-Prioritaet bleiben im Einzelmodus;
Transfer-Ende und Interrupt werden erst nach der letzten Einheit gesetzt.

Der Dreamcast-Bootpfad aktiviert deterministische Batches. Der DMA-Test
vergleicht einen 64-Longword-Transfer byteweise mit dem Referenzmodus und
verlangt weniger Schedulercallbacks.

## Messpunkte

Die Gate-Messung verwendet die bestehenden Runtime-, Phase-9- und
100.000-Block-Fixtures. Folgende Zaehler sind Teil des Berichts:

- Regionsindextreffer gegen Referenzprobes
- direkte Dispatchprobes gegen Referenzbaumprobes
- Page-to-Block-Kandidaten gegen Ganzscan-Kandidaten
- DMA-Schedulercallbacks und abgeschlossene Batches
- belegte und verworfene Diagnose-/Provenienzslots

Absolute Debug- und RelWithDebInfo-Zeiten werden ausschliesslich aus den
frischen KR-4625-Builds berichtet.
