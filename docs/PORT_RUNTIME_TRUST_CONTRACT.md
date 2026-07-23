# Port-Runtime-Vertrauensvertrag

Der mit KR-4508 eingefuehrte Portprojektvertrag Version 3 trennt
Analyseerfolg, Eingabeidentitaet und tatsaechliche Gastausfuehrung. Der
aktuelle kumulative Stand verwendet Portprojektvertrag 27, Runtime-ABI 43,
PlatformServices-ABI 10, Block-ABI 3 und Backend-Interface-ABI 3. Keine der
Vertrauensaussagen wird aus der blossen Erzeugung oder dem Start eines
Hostprozesses abgeleitet.

## Eingabeidentitaet

Das erzeugte Programm enthaelt SHA-256-Erwartungswerte fuer den
GDI-Descriptor, jeden referenzierten Track und die bei der Analyse extrahierte
Bootdatei. Beim Start wird dieselbe GDI erneut geladen und vollstaendig
gehasht. Jede Abweichung oder ein Ladefehler endet vor der ersten
Gastinstruktion stabil mit `source-identity-mismatch`.

Absolute lokale Quellpfade gehoeren nicht zum Vertrag und werden nicht in den
Port geschrieben. Laufzeitdiagnosen redigieren nur belastbare kanonische
absolute Pfade; relative Komponenten wie `.` oder `..` sind keine globalen
Ersetzungsmuster.

## Ausfuehrungsnachweis

Jeder IR-Basic-Block wird mit seiner echten Startadresse, Bytegroesse und
Abschlussart in der Runtime-Blocktabelle registriert. Ein Wrapper fuehrt genau
einen Block aus und liefert `Fallthrough`, statischen oder dynamischen Sprung,
Call, Return, Exception oder Interrupt-Safepoint an den zentralen Dispatcher
zurueck.

`KR_GUEST_PROGRAM_ENTERED` und `silent_failures=0` sind nur erlaubt, nachdem mindestens
ein Block jenseits des initialen Entry-Blocks einen Gast-Checkpoint erreicht
hat. Ein sofortiges `TRAPA`, eine Exception, ein kontrollierter Fallback, ein
erschoepftes Schedulerbudget oder fehlender Fortschritt ist ein sichtbarer
Fehler. Die synthetische Trap-Fixture prueft explizit, dass beide
Erfolgsmeldungen ausbleiben.

## Plattformdienste

PlatformServices-ABI 4 fuehrte diese Bindung an die vorhandenen
Dreamcast-Runtimekomponenten ein; der aktuelle kumulative Vertrag steht auf
PlatformServices-ABI 10:

- Interrupts werden ueber den Interrupt-Router angenommen.
- DMA wird ueber den SH-4-DMAC geplant und bleibt bis zur Schedulerausfuehrung
  asynchron.
- `PREF` uebertraegt echte Store-Queue-Inhalte unter Beruecksichtigung von
  QACR0/QACR1 und invalidiert ueberdeckten generierten Code. Seine exakte
  Instruktionsherkunft bleibt bis zur Store Queue erhalten.
- Gastschreibzugriffe und registrierte Codebereiche laufen durch den
  Executable-Code-Tracker; der Port bindet seine lokale Blocktabelle an dessen
  Gueltigkeitszustand.

Bewachte Kontrollflusskandidaten sind analysierbar und kompilierbar, aber kein
statischer Erreichbarkeitsbeweis. Der Bericht weist `resolved`, `guarded` und
`unresolved` getrennt aus; der Port behaelt fuer Guarded-Stellen den
dynamischen Default. Nur Stellen ohne endliche Kandidaten blockieren die
Vollstaendigkeit.

## Seiteneffektfreie Produktdiagnostik

Portprojektvertrag 26 und Runtime-ABI 42 binden freie Produktprobes an die
aktuelle Gast-MMU und danach ausschliesslich an echte lineare Haupt-RAM-,
VRAM- oder AICA-RAM-Backings. Eine Probe auf Flash oder MMIO wird vor jedem
Geraetehandler abgelehnt. Erfolgreiche und abgelehnte Peeks veraendern weder
CPU-/Exceptionzustand noch Observer, Watchpoints, MMIO-Tracking oder
Speicherzaehler.

Das aktivierte Last-MMIO-Tracking speichert waehrend der Gastausfuehrung einen
allokationsfreien POD aus Operation, Adresse, Breite, Wert und Regionsbasis.
Erst der terminale Reporter loest die Region auf und materialisiert den
owning String. PVR- und Systembusfortschritt wird ueber strukturierte
`snapshot()`-Schnittstellen gelesen. Sie pumpen keine Completion, starten oder
beenden keinen Channel-2-Transfer und bewegen weder Scheduler noch
Interruptbeobachter. Diagnose an oder aus darf damit keine andere
Gastentscheidung erzeugen.

Runtime-ABI 42 stellt zusaetzlich einen POD-Zugriffssink fuer den bereits
ausgefuehrten Gastzugriff bereit. AOT und begrenzter Diagnoseinterpreter
liefern Quell- und Laufzeit-PC; Store-Queue, PVR-Render und PVR-YUV tragen
getrennte Writer-Urspruenge. VRAM32 wird auf das gemeinsame lineare Backing
projiziert. Beobachtete Readwerte und MMIO-Handler werden nicht erneut
abgefragt. Nur fuer die No-op-Klassifikation eines Wrapperwrites darf der
aktivierte Trace vor dem Write dessen seiteneffektfreies lineares Backing
vergleichen. Produkt-`GuestWriteObserver` und Scanout-Evidenz bleiben
konservativ und bei Trace aus/an identisch.

`RuntimeWaitLoopTrace` v1 verdichtet Wertlaeufe und zugehoerige Writer mit
festen Kapazitaeten. Die Deskriptoren entstehen generisch und deterministisch
aus dem Hardwareaudit; der Port enthaelt weder Titelhardcoding noch private
Quell- oder Retaildaten. Ein vorab sortierter Read-Site-Index ersetzt lineare
Deskriptorscans; auch MMIO-Werte stammen aus dem bereits ausgefuehrten
Zugriff, nicht aus einem zweiten Handleraufruf. Bytegenaue lineare
Backing-Ueberschneidungen tragen den beweisenden Linktyp
`exact-backing-bytes`; physische MMIO-Ueberschneidungen werden nur als
`physical-range-candidate` gekennzeichnet. Ein Backing-Index verwirft
unbeteiligte lineare Writes ohne Location-Vollscan. Der aktive Trace bestimmt
skalare und Range-Wrapperaenderungen bytegenau und verwirft No-op-Writer,
ohne die konservative Produktevidenz umzuschreiben.
`guest_memory_access_change_tracking_limit` begrenzt die Aenderungsmap auf
1 MiB; bis 256 Byte verwendet sie Inline-Speicher. Ein groesserer Range oder
eine fehlgeschlagene Diagnoseallokation darf den Gastwrite nicht abbrechen:
Der Sink erhaelt ein ungueltiges Event, `invalid_access_events` steigt und
`complete` wird falsch. Der PVR-Renderer prueft die Trace-Aktivitaet einmal
pro Render statt pro Pixel.

Strukturell ungueltige
Access-Events, darunter Projektionen mit mehr als vier Byteoffsets, erhoehen
`invalid_access_events` und erzwingen `complete:false`; sie sind keine bloss
ignorierten gueltigen Events. Allokationsfehler beim Locationaufbau erhoehen
`dropped_locations`, erzwingen ebenfalls `complete:false` und verlassen den
`noexcept`-Callback kontrolliert. Writer-JSON bindet `instruction_valid`;
seine PCs gelten nur bei gesetztem Marker als Herkunftsnachweis.

Ausschliesslich `KATANA_PORT_WAIT_LOOP_TRACE=1` aktiviert den Rohwerttrace,
unabhaengig vom breiten Diagnoseschalter. Bei leerer Deskriptorliste entstehen
weder Recorder noch Sink. Sonst warnt der Port einmalig auf `stderr`, dass die
Ausgabe nur lokal verwendet, wegen roher Gastwerte geprueft und nicht
ungeprueft geteilt werden darf. Das JSON bindet
`contains_raw_guest_values:true`, `writer_scope:"since-previous-sample"` und
ungueltige skalare Range-Werte als `scalar_value_valid:false` mit
`value:null`; `invalid_access_events` ist Teil desselben Zaehlerobjekts. Der
RAII-Besitzer entfernt den Sink vor der terminalen
JSON-Ausgabe; ohne Trace-Opt-in bleibt der Fastpath ohne Recorder oder
Projektion.
Runtime-ABI 43 und Portprojektvertrag 27 binden zusaetzlich
`katana.runtime-probe` Version 1 mit Profil `deterministic-v1`,
Device-Schema 1 und Hashvertrag `fnv1a64-le-v1`. Das Profil erfasst CPU,
Scheduler, Replay, den vollstaendigen linearen Gast- und Persistenzspeicher
sowie exakt 35 produktive Geraeteinstanzen mit 867 kanonischen Feldern.
Grosse Bytebereiche werden laengengebunden und domain-separiert gehasht; rohe
Werte, Hashes und private Pfade gehoeren nicht in den aggregierten Bericht.
Die begrenzte Store-Queue-Transferfolge und ihr Dropzaehler werden in
kanonischer Reihenfolge einbezogen.

Der private A/B-Runner erzeugt zwei frische Runtimewurzeln, verwendet dieselbe
EXE und denselben lokal installierten Disc-Pack und setzt ausschliesslich
`KATANA_RUNTIME_PROBE=deterministic-v1`, ein positives Gastzyklusbudget sowie
`KATANA_PORT_DIAGNOSTICS=0` beziehungsweise `1`. Andere Diagnose- und
Tracevariablen sind verboten. Beide Prozesse laufen in einem begrenzten
Kill-on-close-Job; der Vergleich akzeptiert genau eine terminale
`KATANA_RUNTIME_PROBE`-Zeile je Lauf.

Der Abschlussnachweis vom 23.07.2026 lief zweimal bis exakt 100.000
Gastzyklen und endete jeweils `complete`/`budget-reached`. Systemreplay v3 war
vollstaendig angebunden und versiegelt, ohne Drops. Alle normativen Felder
waren identisch, EXE und Disc-Pack unveraendert und beide
Wait-Loop-Rohtracezaehler null. Der aggregierte Bericht
`katana-private-runtime-probe-ab` Version 1 meldete `status=success`; damit ist
`KR-4842` abgeschlossen.

## Konsistenzgrenzen

`BRAF` und `BSRF` berechnen relative Ziele in Analyse und Backend modulo
2 hoch 32. Der ISA-Bericht misst pro Instruktion Decoder-, IR-, Backend- und
Runtimeunterstuetzung; der oeffentliche Status ist ausschliesslich deren
Schnittmenge. Eine absichtlich abgelehnte Schicht muss deshalb den
Gesamtstatus ablehnen.

Der reproduzierbare Nachweis besteht aus den CTests fuer Wertanalyse,
ISA-Abdeckung, Portexport, Port-CLI, Application und PlatformServices sowie dem
vollstaendigen CTest-Zwischengate der x64-Debug-Desktop-GUI-off-Konfiguration
mit ASan, konfiguriertem MSVC-Coverage-Backend und `/analyze /WX`. Es besteht
183/183 Eintraege in 312,97 Sekunden, darunter 181 regulaere Passes und zwei
erwartete Regex-Erfolge. Ein Coverage-Bericht wurde in diesem direkten CTest-
Lauf nicht erhoben; Desktop-GUI- und Harness-Tests sind nicht Teil der 183.
Alle Fixtures sind synthetisch; Spielinhalte und private Adressen sind nicht
Bestandteil des Repository. Das Zwischengate ist kein Abschluss von `KR-4852`,
`KR-4853` oder `KR-4854`.

Der abschliessende KR-4842-Nachweis umfasst 6/6 fokussierte Tests in
6,40 Sekunden, den generierten Port-CLI-Pfad 1/1 in 156,11 Sekunden und den
erfolgreichen privaten A/B-Produktlauf. Es wurde keine neue Vollsuite und kein
`KR-4852` ausgefuehrt.
