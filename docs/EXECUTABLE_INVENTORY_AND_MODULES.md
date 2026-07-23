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

Der aktive Lebenszustand ist bytegenau als sortierte physische Extents
modelliert. Partielle Overlays stanzen nur ihr Fenster aus; unbetroffene
Extents derselben Quellenidentitaet bleiben aktiv und koennen weiter gebunden
werden. Geladene Ersatzbereiche werden vor jeder Katalog-, Block- oder
Provenienzmutation vollstaendig validiert, damit ein abgewiesener Load keinen
alten gueltigen Zustand teilweise zerstoert. P0/P1/P2-Aliase werden vor
Ueberlappungspruefung kanonisiert, waehrend Bereiche ueber nichtlineare
Aliasgrenzen als ungueltig gelten.

Das gilt auch fuer einen Ersatz mit derselben Modul-ID: Erst nach vollstaendiger
Vorvalidierung werden alter Index-, Katalog-, Block-, Tracker- und
Provenienzzustand atomar ersetzt. Die Negativregression prueft, dass ein
ungueltiger Ersatz auch Metriken und Generationen unveraendert laesst.

Ein kanonischer 4-KiB-Seitenindex zaehlt aktive Extents pro physischer Seite.
Writes auf Seiten ohne aktive Extentherkunft werden damit vor dem linearen
Modulkatalogscan verworfen, waehrend die bytegenaue Runtimeprovenienz weiterhin
aktualisiert wird. Publish, Ersatz, Teilpatch, Unload und geladene Bereiche
halten den Index konsistent; getrennte Metriken zaehlen Fast-Rejects und
tatsaechlich erforderliche Katalogscans.

BIOS-/GD-Ladevorgaenge besitzen zusaetzlich einen `ExecutableLoadWriteTracker`.
Er fasst angrenzende unmittelbare Copy-/DMA-Writes ueber kanonische physische
Aliase zusammen und ordnet sie exakt dem veroeffentlichten Ladebereich zu. Bei
byteidentischen Bytes bleibt der vorhandene native AOT-Bestand unangetastet.
Eine bereits bewiesene bytegleiche Modulabdeckung wird nicht dupliziert; ein
frischer bereits bytegleicher Ladebereich erhaelt jedoch seinen notwendigen
ersten Provenienznachweis. Bei geaenderten Bytes hat der Write-Observer die
betroffenen Bloecke bereits einmal exakt invalidiert, sodass die
Modulveroeffentlichung keine zweite Generation erzeugt. MMU-PIO verwendet die
tatsaechlich committed physische Range; nichtlineare TLB-Spannen werden vor
jedem Partialwrite abgelehnt. Die vier Area-3-Haupt-RAM-Spiegel teilen sich fuer
Codewrites dasselbe 16-MiB-Backing und werden auch an der Wrapgrenze korrekt
geteilt. Fehlt eine passende Beobachtung, bleibt die bisherige konservative
Invalidierung bestehen.

Der unterstuetzte Relocationtyp `module_base32` interpretiert ein
32-Bit-Little-Endian-Quellwort als `Quellwort + Gastmodulbasis + Addend` mit
32-Bit-Ueberlauf. Die Byteidentitaet wird gegen dieses kanonisch relokierte
Abbild geprueft, nicht gegen die unveraenderten Quellbytes. Unbekannte oder
ueberlappende Relocationfelder werden bereits beim Publizieren beziehungsweise
Aktualisieren abgelehnt. Eine Aenderung von Tabelle, Typ oder Addend invalidiert
alte Bloecke und erhoeht die Relocationgeneration.

`KR-4912` bindet diese Operationen an monotone Modulinkarnationen. Eine neue
Load-, Relocation-, Replace- oder Unload-Transaktion kann eine fruehere
Inkarnation nie unbemerkt wiederbeleben. `ObservedByteIdentical` prueft alle
ueberlappten aktiven Extents vor einer Mutation. Die bereits identisch
abgedeckte Vereinigungsmenge bleibt unangetastet; nur wirklich disjunkte neue
Fenster werden veroeffentlicht. Damit sind auch Supersets ueber mehrere Extents
atomar, waehrend eine widerspruechliche Modul-ID oder ein einzelnes
abweichendes Byte den gesamten Load ohne Teilzustand ablehnt.

Byteidentische CPU-, FPU-, Store-Queue- und Fallback-Writes stellen die
Writer-Provenienz her, ohne native Bloecke zu invalidieren. Byteidentische
Copy-/DMA-Writes erhalten den vorhandenen Provenienznachweis. Ein interner,
bereits bewiesener Runtime-Write-Snapshot darf ausschliesslich an seinem
zusammenhaengenden Tail ueber weitere geschriebene, gemappte und noch
unbeanspruchte Bytes wachsen. So kann insbesondere ein Block an der bisherigen
128-Byte-Grenze seinen tatsaechlich geschriebenen Delay Slot aufnehmen, ohne
ID, Generation oder bereits gebundene Prefixbloecke auszutauschen.

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

Materializer-Origins enthalten deshalb nicht nur Modul-, Quellen-,
Relocation- und Codegeneration, sondern auch die konkrete Blockidentitaet.
Vor Lookup und terminaler Statusausgabe werden inaktive oder nicht mehr zur
aktuellen Modulinkarnation passende Origins abgeglichen. Der Abgleich
deaktiviert eine noch gebundene Runtime-Tabellenidentitaet, retired den
zugehoerigen Trackerblock und entfernt die veraltete Origin. Dieselbe
Bereinigung gilt fuer Replace, Teilpatch, Unload und eine nachtraeglich
fehlgeschlagene Bytevalidierung. P0-/P1-/P2-Ziele werden dabei nach
MMU-Uebersetzung gegen dieselbe physische Herkunft und denselben
Generationsvertrag geprueft.

Gleiche AOT-Validierungsbelege teilen einen unveraenderlichen Bytesnapshot,
wenn physischer Pruefbereich, Quelle, Inkarnation, Relocation und Template
uebereinstimmen. Nur ein einzigartiger Snapshot zaehlt gegen
`max_memory_bytes`. `retained_validation_bytes`,
`peak_retained_validation_bytes` und `reclaimed_validation_bytes` machen
Belegung und Freigabe sichtbar; die letzte Origin gibt den Proof frei.
Byteabweichung, Budgetende oder ein veralteter Lifecycle koennen damit weder
einen alten Block weiterverwenden noch unbegrenzt Beweisspeicher halten.

Das Produkt-Gate erlaubt einen Interpreter nur in einem expliziten Bring-up-
Diagnoseprofil. Der normale Export emittiert und linkt
`runtime-sh4-interpreter` nicht, deaktiviert die interpretiert gestuetzte
Demand-Materialisierung und endet bei fehlendem AOT typisiert. Nur
`diagnostic_partial` aktiviert den begrenzten Interpreter und weist dies im
Manifest als `diagnostic-interpreter` aus. Deaktivierung, unbekannte Quelle,
Byteabweichung, Budgetende und ungueltiger Block bleiben typisierte Misses.
`KR-4848` ist mit strukturierten Disc-Ladetransaktionen, dem allgemeinen
nativen Materializer und vorab erzeugten latenten nativen Modulen
abgeschlossen. Der aktuelle kumulative Vertrag verwendet Runtime-ABI 47,
Block-ABI 3, Backend-Interface-ABI 3, PlatformServices-ABI 10 und
Portprojektvertrag 31. Systemreplay-Schema 5 und Runtime-Probe-Schema 2
skalieren die Produktbeobachtung, ohne den interpreterfreien
Materialisierungsvertrag zu aendern.

Eine erfolgreiche Materialisierung markiert den Dispatch explizit und erzeugt
ihr Replay-Ereignis unabhaengig vom Diagnose-Sampling. Oeffentliche Probe-,
Fault- und Materialisierungsberichte serialisieren weder Modul-/
Quellidentitaeten noch Gastbytes; die lokale Zieladresse bleibt einem
ausdruecklichen Detailmodus vorbehalten. Die fokussierten Load-, Relocation-,
Replace-, Unload-, Alias-, Proof-, Replay- und Redaktionsregressionen sind
gruen. Fuer `KR-4912` lief weder ein privater Retaillauf noch eine Vollsuite
oder `KR-4852`.

Die optionale Runtimebeobachtung aendert diesen Materialisierungsvertrag nicht.
Ein POD-Zugriffssink versieht bereits ausgefuehrte AOT-Zugriffe und Zugriffe
des begrenzten Diagnoseinterpreters mit Quell- und Laufzeit-PC. Store-Queue-
`PREF`, PVR-Render und PVR-YUV tragen getrennte Writer-Urspruenge; die
VRAM32-Sicht projiziert auf das gemeinsame lineare Backing. Der Sink fragt
beobachtete Readwerte und MMIO-Handler nicht erneut ab. Nur der aktive Trace
vergleicht fuer Wrapperwrite-No-ops vor dem Write das seiteneffektfreie
lineare Backing; Produktobserver und Scanout-Evidenz bleiben konservativ sowie
bei Trace aus/an identisch.

`RuntimeWaitLoopTrace` v1 verwendet ausschliesslich generische,
deterministisch aus dem Hardwareaudit abgeleitete Deskriptoren. Ein vorab
sortierter Read-Site-Index vermeidet lineare Deskriptorscans; MMIO-Werte
werden aus dem bereits ausgefuehrten Zugriff uebernommen. Lineare bytegenaue
Writerlinks tragen `exact-backing-bytes`; physische MMIO-Ueberschneidungen
bleiben `physical-range-candidate`. Backing-indizierte Locations vermeiden
Vollscans fuer unbeteiligte lineare Writes. Der aktive Trace bestimmt skalare
und Range-Wrapperaenderungen bytegenau und verwirft No-op-Writer. Nur
`KATANA_PORT_WAIT_LOOP_TRACE=1` aktiviert den Rohwerttrace, unabhaengig von
`KATANA_PORT_DIAGNOSTICS`. Bei leerer Deskriptorliste entstehen weder Recorder
noch Sink. Sonst warnt der Port einmalig auf `stderr`, dass die rohen
Gastwerte nur lokal bestimmt und nicht ungeprueft teilbar sind. Das JSON
deklariert `contains_raw_guest_values:true`,
`writer_scope:"since-previous-sample"` und ungueltige skalare Range-Werte mit
`scalar_value_valid:false` und `value:null`. Strukturell ungueltige
Access-Events erhoehen `invalid_access_events` und erzwingen
`complete:false`; sie sind keine bloss irrelevanten gueltigen Events. RAII
entfernt den Sink vor der terminalen Ausgabe. Ohne Trace-Opt-in bleibt der
Ausfuehrungs- und Materialisierungshotpath ohne Recorderprojektion. Die Registervarianten von
`PREF`, `OCBI`, `OCBP`, `OCBWB` und `TAS.B` sind im begrenzten Interpreter
geschlossen; `FMOV`-Doppelwortzugriffe laufen low nach high. Keine dieser
Diagnosefaehigkeiten macht unbekannte Bytes zu Code oder einen
Interpreterblock zu Produkt-AOT.

Der Abschlussnachweis fuer `KR-4842` umfasst 6/6 fokussierte Tests in
6,40 Sekunden, den Port-CLI-Pfad 1/1 in 156,11 Sekunden und einen erfolgreichen
Diagnose=0/1-A/B-Produktlauf bis jeweils 100.000 Gastzyklen. Systemreplay v3
war vollstaendig und versiegelt, die normativen Felder waren identisch und
EXE sowie Pack blieben unveraendert. Es lief keine Vollsuite und kein
`KR-4852`; `KR-4842` ist abgeschlossen.

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
