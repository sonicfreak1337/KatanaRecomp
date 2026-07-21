# Executable-Inventar, Module und Blockmaterialisierung

Dieser KR-4704-Vertrag trennt den Inhalt geladener Bytes von ihren
Laufzeitberechtigungen und von der Frage, wann ein Block kompiliert sein muss.
Er gilt allgemein fuer Raw-, ELF-, Disc-, Modul- und Overlayquellen.

## Byteinventar

Jedes committed Byte eines ausfuehrbaren Segments gehoert disjunkt zu genau
einer Klasse:

- `proven_reachable_code`
- `runtime_discovered_code`
- `unreachable_decodable_code`
- `embedded_data`
- `literal_pool`
- `jump_table`
- `pointer_table`
- `padding`
- `overlay_candidate`
- `module_candidate`
- `compressed_or_encoded`
- `unknown_executable`

Der Bericht `katana-executable-byte-inventory` besitzt eine adressfreie Form
fuer Aggregate und eine lokale Detailform mit Segment, lokalem Quellnamen,
Adresse, Dateioffset, Ladephase, Schreibbarkeit und Bereichen. Jeder lokale
Bereich nennt ausserdem Rolle, Beweisklasse, statische Referenzen,
Laufzeitwrites, Relocationen, potenzielle Kontrollflussziele und den
Klassifikationsgrund. Private
Quellnamen und Adressen duerfen die lokale Form nicht verlassen.

`padding` ist zunaechst nur eine diagnostische Kandidatenklasse. Gleichfoermige
Null- oder FF-Bereiche bleiben im blockierenden unbekannten Satz. Nur ein auf
32 Byte ausgerichteter, bis zum Dateiende reichender Fillbereich eines
quellgebundenen Mixed-Bootsegments darf als bewiesen gelten, wenn weder
Kontrollflussziel, Referenz, Relocation, Modulbeschreibung noch analysierte
Instruktion hineinragt. Innenliegendes Fill bleibt Kandidat. Die vorhandene
Codegeneration sorgt bei spaeteren Writes fuer Generationserhoehung; das
Padding selbst registriert keinen ausführbaren Block. Ebenso werden
komprimierte oder codierte Bytes nicht heuristisch aus dem Gate entfernt.

## Mixed-Segment-Rollen und Seiteninventar

Die unveraenderte Speicherabbildung besitzt intern Teilrollen:
`proven_code`, `reachable_code`, `runtime_materializable_code`,
`literal_pool`, `jump_table`, `pointer_table`, `read_only_data`,
`writable_data`, `padding`, `compressed_or_encoded`, `module_payload` und
`unknown`. Jede Rolle traegt `proven`, `guarded`, `candidate` oder `unknown`
als getrennte Beweisklasse. Eine gueltige SH-4-Decodierung allein ist keine
Codeevidenz. Ein noch nicht analysiertes Codeziel braucht zusaetzlich einen
Entry-, Funktions-, Kanten-, Tabellen- oder Relocationbeleg.

Der Seitenbericht verwendet 4-KiB-Bereiche. Die lokale Form enthaelt Adresse,
Entropie, Decodedichte, Entfernung zu bewiesenem Code sowie Referenz-,
Relocation- und Zielzaehler. Die adressfreie Form gruppiert diese Seiten nach
Segmentquelle, Ladephase, Schreibbarkeit, Rolle, Beweis, Referenz-/Relocation-
und Zielevidenz sowie Entropie-, Decodedichte- und Naehebucket. Diese Merkmale
priorisieren unbekannte Klassen; sie stufen Bytes nicht heuristisch aus dem
Gate heraus.

## Loadervertrag

Runtimeberechtigungen und Inhaltstyp sind getrennt. Ein rohes Disc-Bootabbild
ist ein beschreibbares, ausfuehrbares `mixed`-Segment und nicht pauschal Code.
Bewiesene Ziele duerfen darin dekodiert werden; lineare Volldisassemblierung
wird daraus nicht abgeleitet. Dateibytes, `memory_size` und Zero-Fill werden
getrennt berichtet. Raw- und ELF-Segmente tragen Quelltyp und Ladephase; ELF
uebernimmt Code-/Datenrechte weiterhin ausschliesslich aus `PT_LOAD`.

## Vorabkompilierung

Das Inventar wird getrennt auf folgende Mengen abgebildet:

- `initially_reachable`
- `statically_discoverable`
- `loadable_module`
- `runtime_materializable`
- `never_executed_data`
- `unknown`

Der Bericht nennt `unknown_executable_bytes`, `unproven_padding_bytes`,
`incomplete_initial_required_code_bytes` und
`uncovered_runtime_materializable_bytes` weiterhin getrennt. Unbekannte
Speicherbytes blockieren nicht allein aufgrund einer ausfuehrbaren
Segmentberechtigung. Gateblocker sind unvollstaendige initial erforderliche
Bereiche, unabgedeckte Kontrollflussziele, unvollstaendig gebundene
materialisierbare Bereiche oder Dispatchpfade ohne zentrale Validierung. Nur bewiesene Objektdaten,
Literalpools, Jump-/Pointertabellen und vollstaendig bewiesenes Padding zaehlen
als `never_executed_data`. Module und Overlays duerfen nur mit dem
nachfolgenden sicheren Runtimevertrag aufgeschoben werden.

## Module und Overlays

Ein ausfuehrbares Modul besitzt eine stabile Modul- und Quellenidentitaet,
Gastbereich, unveraenderte Quellbytes, Relocationen, Art, Schreibbarkeit,
Generation und aktiven Lebenszustand. Ueberlappende oder ungebundene Quellen
werden abgelehnt. Ersatz und Unload entfernen ueberlappende Runtimebloecke und
invalidieren den Code-Tracker. Ein unbekanntes ausfuehrbares Ziel bleibt ein
sichtbarer Dispatchfehler. Eine engere Ausnahme gilt fuer nachweislich
geaenderte Gastbytes im Dreamcast-Haupt-RAM: Der Katalog merkt deren kanonische
physische Byteposition, ohne den gesamten RAM als Modul oder Code zu
deklarieren. Erst der tatsaechliche Kontrolltransfer darf daraus einen neuen,
begrenzten Modulsnapshot erzeugen.

Der unterstuetzte Relocationtyp `module_base32` interpretiert ein
32-Bit-Little-Endian-Quellwort als `Quellwort + Gastmodulbasis + Addend` mit
32-Bit-Ueberlauf. Die Byteidentitaet wird gegen dieses kanonisch relokierte
Abbild geprueft, nicht gegen die unveraenderten Quellbytes. Unbekannte oder
ueberlappende Relocationfelder werden bereits beim Publizieren beziehungsweise
Aktualisieren abgelehnt. Eine Aenderung von Tabelle, Typ oder Addend invalidiert
alte Bloecke und erhoeht die Relocationgeneration.

## Demand-driven-Materialisierung

Der optionale Pfad ist standardmaessig deaktiviert. Im aktivierten Modus gilt:

1. Zielausrichtung, committed Bytes, Ausfuehrungsrecht und aktive
   Modulherkunft pruefen. Fehlt nur die Modulherkunft, duerfen mindestens zwei
   tatsaechlich geschriebene Zielbytes einen bytegenauen, hoechstens 128 Byte
   grossen Runtime-Write-Snapshot erzeugen; unbeschriebenes RAM bleibt
   `unknown-source`.
2. Gastzyklus-, Block-, Byte-, Instruktions-, Seed-, Zeit-, Speicher-, Lauf-
   und Wiederholungsbudgets pruefen.
3. Einen unveraenderlichen Bytesnapshot bilden und den Referenzcallback fuer
   Decode, begrenzte Analyse, IR-Pruefung und Codegen aufrufen.
4. Bereich, Variante, Backendfunktion und Provenienz validieren.
5. Bytes, Modul-, Quellen-, Relocation- und Codegeneration erneut pruefen.
6. Code-Tracker und Runtime-Blocktabelle registrieren und den Handle nochmals
   validieren.
7. Erst danach dispatchen.

Jeder spaetere ueberlappende Gastwrite deaktiviert einen solchen Snapshot. Der
Code-Tracker entfernt den physischen Runtimeblock auch dann, wenn der Write
ueber einen anderen P0/P1/P2-Alias erfolgt. Ein erneuter Kontrolltransfer muss
die aktuellen Bytes unter einer neuen Modulidentitaet materialisieren.

Ein Interpreter kann den expliziten Referenzcallback stellen, wird aber nicht
automatisch oder dauerhaft aktiviert. Deaktivierung, unbekannte Quelle,
Byteabweichung, Budgetende und ungueltiger Block sind typisierte Misses.

## Runtime-only-Profil

Pro Site werden Aufrufe, Hits, Misses, begrenzte verschiedene Ziele,
Materialisierungen und Invalidierungen erfasst. Die Stabilitaet ist
`never-hit`, `monomorphic`, `small-polymorphic` oder `dynamic`. Der adressfreie
Bericht enthaelt nur Aggregate und den Dispatchanteil in ppm. Die lokale Form
enthaelt Callsite und Ziele und bleibt private Diagnoseevidenz. Eine spaetere
Spezialisierung ist nur aufgrund wiederholbarer Profile zulaessig; das Profil
selbst ist kein statischer Vollstaendigkeitsbeweis.

## Aktueller privater Messstand

Der reine Analyselauf startete kein Gastprogramm. Von 6.735.296 committed
ausfuehrbaren Bytes sind 110.404 `initially_reachable`. 16.554 Bytes sind
bewiesene Literalpools; MOVA-Ziele werden korrekt als Adressreferenzen und
nicht als Literale behandelt. 408.019 Bytes sind nur Padding-Kandidaten und
bleiben unbekannt. Weitere 6.200.319 Bytes sind `unknown_executable`. Damit
bleiben 6.608.338 unbekannte Speicherbytes, die weder als statisch analysierter
Code noch als implizit dispatchbar gelten.

Die 1.603 Seiten mit dominant unbekannter Rolle zerfallen adressfrei in 14
Ursachengruppen. Die groessten Seitenspannen ohne Referenz, Relocation oder
Kontrollflussziel sind: 545 Seiten mit niedriger Entropie und gemischter
Decodedichte, 366 Seiten mit mittlerer Entropie und gemischter Decodedichte,
300 Seiten mit mittlerer Entropie und hoher Decodedichte sowie 187 Seiten mit
niedriger Entropie und geringer Decodedichte. Hohe Decodedichte bleibt dabei
ausdruecklich nur Diagnose, nicht Codebeweis.

Der private doppelte Build-only-Nachweis besteht mit 110.404 initial
erforderlichen und statisch vorkompilierten Bytes, 1.826 Runtime-only-Sites,
null unbekannten Instruktionen, null ungeloesten oder partiell bewachten Sites,
null erreichbaren Abbruchkanten, null unabgedeckten Kontrollflusszielen und
null Dispatchpfaden ohne Validierung. Beide frischen Hostbuilds erzeugen
identische portable Metadaten und Quellen. Das aktuelle Hostartefakt wird an
Jobidentitaet und neu ermittelten Hash gebunden; ein Runtimeprozess wird im
Build-only-Modus technisch vor dem Start abgewiesen.
