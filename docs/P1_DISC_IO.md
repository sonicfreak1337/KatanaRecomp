# P1-Disc-I/O-Vertrag

Task: `KR-4623`

## Dateihandles und Provenienz

`FileDiscSource` oeffnet jede Trackdatei genau einmal read-only und behaelt den
Hoststream fuer seine Lebensdauer. Positionsreads werden unter einem
quellenlokalen Mutex ausgefuehrt; parallele Quellen blockieren einander nicht.
Zaehler fuer Opens, Reads und gelesene Bytes machen Reopen-Regressionen
sichtbar.

Der GDI-Parser erfasst SHA-256 und Groesse des Descriptors sowie jeder
Trackdatei einmal. Analyse, Portbuild und Runtime uebernehmen diese
`InputProvenance`-Werte, statt dieselben Dateien erneut zu hashen. Die portable
Discidentitaet `gdi-sha256:<64 hex>` bindet Tracklayout und Trackhashes, aber
keine lokalen Pfade.

## Track-/LBA-Index und Batches

Tracknummern besitzen einen Hashindex. Logische LBAs werden per `upper_bound`
im sortierten Trackintervallindex aufgeloest. Ein zusammenhaengender Read
innerhalb eines Tracks wird als ein Hostread ausgefuehrt und danach sektorweise
in die 2048-Byte-Datensicht dekodiert. Bereiche ueber Trackgrenzen werden in
deterministische Trackbatches zerlegt; Luecken und Audiotracks bleiben sichtbare
Fehler.

Der GDI-Sektorcache haelt hoechstens 256 dekodierte Sektoren. Er ist mit
`DiscCacheMode::DisabledReference` abschaltbar. Cache an/aus liefert dieselben
Bytes; Hits, Misses, Evictions, Tracklookups, Hostreads und dekodierte Sektoren
werden getrennt gezaehlt.

## ISO9660

ISO-Verzeichnisrecords werden nach `(LBA, Groesse)` gecacht, aufgeloeste
Pfade als Extents nach kanonischem ISO-Pfad. Der Verzeichniscache ist auf 256,
der Extentcache auf 4096 Eintraege begrenzt. `DisabledReference` umgeht beide
Caches vollstaendig. Pfadnormalisierung, Both-Endian-Pruefung,
Directory-/Dateityp und Extentbereich bleiben unveraendert.

## GD-ROM

`GdRomDrive` liest direkt in den einzigen Antwortvektor des Requests; die
GDI-Schicht verwendet nur den fuer Raw-Sektor-Konvertierung notwendigen
Batchpuffer. `GdRomAsyncReader` besitzt weiterhin keine Host-Wall-Clock. Jede
Completion wird ausschliesslich aus `command_latency`, `cycles_per_sector` und
dem zentralen `EventScheduler` bestimmt.

## Gate-Messwerte

KR-4625 misst grosse sequenzielle und deterministisch zufaellige Reads in
Debug und RelWithDebInfo. Der Bericht enthaelt mindestens:

- persistente Opens, Hostreadanzahl und gelesene Bytes
- Track-/LBA-Lookups und Sektoren pro Hostread
- GDI-Sektorcache- und ISO-Verzeichnis-/Extentcache-Hits
- identische SHA-256 fuer Cache-an/Cache-aus-Ausgaben
- identische GD-ROM-Completionzyklen und Ereignisreihenfolge
