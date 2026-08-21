# Analyzer-ABI

Der aktuelle oeffentliche Analyzervertrag ist Version `59`. Der aktuelle
Source-Stand verwendet Runtime-ABI 116, Block-ABI 5, PlatformServices-ABI 14,
Backend-Interface-ABI 24, Portprojektvertrag 103 und Native-Port-
Profilvertrag 23. GameProject-Vertrag 9/Artefaktformat 6 und Analysis-
Directives-Version 5, Native-AOT-Emissionsprofil 40, AOT-Partitionsschema 9
und Port-Metadata-Cache-
Schema 12 gehoeren zum aktuellen Exportvertrag; sie ersetzen nicht den
Analyzer-ABI-Zaehler.
Der bestehende Function-Value-Sanitylauf ist mit `463/463` Checks gruen, und
der erste vollstaendige `analyze-port`-Lauf samt identitaetsgebundenem Resume
ist abgeschlossen. Der historische Candidate-Resolution-Checkpoint
`49b0f72a9f49d60a4eb6e0481460cd57c5625735` bleibt ausschliesslich als
ABI-34-Referenz erhalten.

Analyzer-ABI 59 bindet die inkompatible, wertgebundene Pending-ABI-Scalar-
Lane in `FunctionRegisterValueSummary`. Endliche Pending-Werte und ihr
Truncationzustand bleiben bis zu einem bewiesenen ABI-/Callback-Gate getrennt
von Callbackprovenienz; Objekte aus ABI 58 besitzen dieses oeffentliche Layout
nicht und duerfen weder gelinkt noch aus einem Analysecache wiederverwendet
werden.

Analyzer-ABI 58 bindet zusaetzlich getrennte current-, detached- und
Memory-Callback-Loss-Diagnosen sowie autoritative Storage-Summaries. ABI 57
band bereits die transaktionale Materialization-World,
den inzwischen als Schema 4 publizierten Resume-Ledger und die streng
identitaetsgebundene Frontier-
Importgrenze. Beobachtete Runtime-Frontiers bleiben `ObservedHint` und duerfen
weder CFG-Kanten noch AOT-Roots oder Hardware-Closure erzeugen. Positive
Produktwiederverwendung bleibt bis zum vollstaendigen Callback-/Target-/
Hardware-Owner-Beweis deaktiviert.

Analyzer-ABI 56 trennt persistente 32-Bit-Argumentstores von direkten
Callbackregistraren. Latente Module duerfen einen solchen Primary-Image-Sink
nur mit einem per SH-4-Datenfluss bewiesenen lokalen Descriptoranfang plus
`Index * Stride` und einem unabhaengig bewiesenen Callback-Feld-Walker
kombinieren. Die bounded Tabelle erlaubt Nullfelder, verlangt mindestens zwei
lokale Entry-Shapes und einen expliziten Terminator vor 256 Records. Alle
gewonnenen Entries bleiben guarded; keine indirekte Zielmenge wird dadurch
vollstaendig. Analyzer-ABI 56 und Boot-Analysecache-Schema 9 binden den neuen
Analysevertrag, ohne unveraenderte AOT-Emissionen oder Hostobjekte kalt zu
invalidieren.

Ein exakter Latent-AOT-Entry-Hinweis bindet bei transformierten `.PRS`-
Quellen den kodierten Disc-Extent, aber die Modulidentitaet und den Entryoffset
an die strikt dekodierte Executable-Sicht. Im kombinierten Heuristikmodus
erweitert diese Runtime-Frontier-Evidenz die bereits validierte Rootmenge des
Moduls; sie darf sie nicht ersetzen oder den PRS-Transform umgehen.

Analyzer-ABI 53 publiziert die kanonischen Callback- und Record-Field-Sinks
der finalen RuntimeOnly-CFA als Teil von `ControlFlowAnalysisResult`. Der
Cross-Image-Exporter darf diese Fixpunktansicht direkt wiederverwenden und
muss das vollstaendige Primary Image nicht erneut analysieren. Ein typisierter
Materialisierungsstatus trennt die kanonische Sicht von ABI-Modi, die weiterhin
den bounded Fallback benoetigen. Boot-Analysecache-Schema 5 serialisiert die
neuen Ergebnisfelder; alte Cacheartefakte werden deterministisch verworfen.

Analyzer-ABI 52 band die bounded Cross-Image-Record-Callback-Analyse.
Statische Primary-Image-Walker publizieren Funktions-, Call-, Load-, Feld-
und Call-/Jump-Evidenz; Latent-AOT-Module verfolgen exakte Rueckgaben direkter
externer Record-Konstruktoren durch CFG-Joins und erkennen lokale
Codepointerstores in diesen Feldern. Die resultierenden Entries bleiben
guarded und werden erneut gegen Modulbytes, Entry-Shape, Early-CF, CFG und
Relocation validiert; die Analyse behauptet keine Vollstaendigkeit der
indirekten Aufrufkante. Feld-Sinks sind Teil von Analyzer-, Kandidatenepoch-
und Persistent-Cacheidentitaet. Portprojektvertrag 101, Latent-AOT-Cache 6,
Analyseimplementierung v20, Native-AOT-Profil 40 und Metadata-Cache 12
invalidieren aeltere Artefakte deterministisch.

Version 41 ist die Kompatibilitaetsgrenze fuer die aktuell exportierten
Layouts, Signaturen und Analyseergebnisse. Analyzer-ABI 11 band historisch
die engere Provenienz fuer 32-Bit-PC-relative
Code-Literale: Erst eine echte Call- oder Tail-ABI-Grenze darf sie zu einem
bewachten Codepointerargument machen. Normale Objektfeldloads und bedingte
Owner-Uebergaenge erhalten diesen Beweis nicht. Damit invalidieren Analyse-,
IR-, Codegen- und Whole-Export-Caches den alten Bestand, der solche direkt vor
einem Tail-Registrar geladenen Callbacks noch verlor.

Der vorherige Analyzer-ABI-35-Stand erweiterte `DreamcastHardwareAudit` um die exakten erreichbaren
Instruktionsadressen nicht vollstaendig aufgeloester Speicherzugriffe. Das
Native-Port-Hardwaregate darf dadurch nicht mehr einen blossen Summenzaehler
als Vollstaendigkeitsbeweis verwenden. Das JSON-Schema steigt passend auf
`katana.hardware-audit.v5`.

Analyzer-ABI 36 schliesst zusaetzlich den inkompatiblen Analyzer-SDK-Linkbruch:
`port_export.cpp` liegt in einer separaten, nicht installierten Tooling-
Object-Closure; `port_export.hpp` und `native_port_artifact.hpp` sind aus der
Analyzer-SDK-Headerinstallation ausgeschlossen. Damit gelangen Codegen-
Tooling und das private Native-Port-Artefakt nicht mehr als Analyzer-SDK-
Oberflaeche in installierte Consumer.

Analyzer-ABI 39 trennte exakte Funktionsgrenzen von Analyse-Roots und band
edge-only Jump-Table-Metadaten samt ihrer Cacheidentitaet. Identity-bound
GameProject-Grenzen begrenzen damit Recursive-, Function-Value- und IR-
Analyse, ohne unerreichbaren Code als Root wiederzubeleben. Die gebundene
Post-Bootstrap-Ansicht wird vor Analyse/IR/AOT materialisiert; emittierte
Instruktionswoerter werden gegen sie validiert und aktive image_id-Overlays,
CallbackTable-Roots sowie Replacement-Reachability bleiben Teil der
identitaetsgebundenen Metadaten.

Analyzer-ABI 40 bindet den begrenzten Static-Callback-Inventarpfad, die
kompakte SH-4-Switch-Erkennung und die fruehe Exportgrenze. Transformierte
Latent-AOT-Quellen tragen neben ihrer encoded Source-Identitaet eine getrennte
decoded Modulidentitaet; PRS-Quell-/Decoded-Caps, transformgebundene Cachekeys,
Telemetry und Source-Maps werden damit deterministisch versioniert. Der
Loaded-AOT-Rebase installiert gemischte Module erst nach exakter
Codeblock-Closure. Diese Aenderungen invalidieren den vorherigen
Analyser-/Metadata-Cachebestand.

Analyzer-ABI 41 trennt deskriptive Funktionskenntnis von Analyse-Roots. Die
neue Analysis-Directives-v4-Form `function_entry_hint` kann einen bereits aus
dem erreichbaren CFG oder einer semantischen Callback-Kette gewonnenen Entry
identitaetsgebunden bestaetigen, erzeugt selbst aber weder Root noch Kante.
Externe erreichbare CFG-Bloecke erhalten fuer die begrenzte Callbackanalyse
lokale Owner, wenn noch keine Funktionsgrenze existiert; Registrar-/Objektfeld-
Codepointer bleiben dadurch bis zur spaeteren indirekten Aufrufgrenze
erhalten. Hints, externe Entries und die resultierende Analyseclosure sind in
Cache, IR und AOT-Vertrag gebunden.

Analyzer-ABI 44 bindet die bounded, stride-basierte statische
Callback-Inventarisierung. Der Stride muss aus der SH-4-Indexarithmetik
folgen, ist auf `256` Bytes begrenzt und verlangt mindestens zwei
stride-konsistente, shape-validierte Codeeintraege vor einem Nicht-Code-
Terminator. PC-relative Literale liefern allein keine Descriptor-Tabellen-
Evidenz mehr: erst ihr statischer Speicher-Dereferenz legitimiert die
Inventarisierung. Das verhindert Formatstring-/Ressourcen-Scheinevidenz;
alle Ergebnisse bleiben guarded und erweitern weder unbewiesen CFG noch
Laufzeitziele. Die zugehoerigen Hook-Proofs protokollieren die eingehenden
Instruktionsquellen, und externe Fortschritts-Waits werden als
identitaetsgebundene Provideranforderung ausgegeben.

Analyzer-ABI 48 bindet die getrennte Provenienz einer vollstaendig
begrenzten, transformgebundenen PRS-Prefix-Entry-Tabelle. Nur `3..64`
eindeutige direkte Main-RAM-Ziele mit Nullterminator, passendem Runtimeextent
sowie Decode-/Early-CF-Pruefung koennen nach vollstaendiger CFG- und
Relocation-Closure RuntimeOnly-Roots bilden. Externe exakte Hints bleiben
eigenstaendig; nicht inventarisierte Ziele bleiben typisierte Stop-on-miss-
Faelle. Die pfadfreie Kandidatendiagnostik exponiert nur Groessen-, Entry- und
terminale Vollstaendigkeitsdimensionen, nicht Quellnamen, Adressen oder Bytes.
Die Zielobergrenze zaehlt ausschliesslich Nicht-Null-Ziele; der Terminator wird
in einer zusaetzlichen begrenzten Zelle gelesen. Die statisch aus den
Tabellenwerten abgeleitete Page-Basis beweist nur relative Offsets. Eine
Produktabbildung entsteht erst, wenn der Loaded-AOT-Binder am erreichten
Runtimebereich die exakte materialisierte Modul- beziehungsweise
Codeidentitaet unabhaengig bestaetigt.

Analyzer-ABI 49 bindet exakte erreichte Non-Root-Funktionsgrenzen bereits in
der CFA als getrennte Owner, ohne unerreichbare Metadaten zu Roots zu machen.
Eine inferred Funktionsausdehnung darf keine unabhaengige bekannte
Function-Entry-Grenze mehr verschlucken. Mehrere unabhaengige Gruende fuer
einen vollstaendigen persistenten Analyse-Bypass werden vor der FVA einmalig
zusammengefuehrt; der strikte direkte Producervertrag bleibt unveraendert.
Damit besitzen native Ganzfunktionshooks belastbare Ownergrenzen, waehrend
Cache, IR und AOT denselben erreichten CFG behalten. Analysis-Directives `5`,
Native-AOT-Profil `37` und Metadata-Cache `9` invalidieren den alten Bestand.

Im historischen ABI-41-Stand wurde die davon getrennte oeffentliche
Codegen-Grenze `PortExportOptions::native_port_definition` durch
Backend-Interface-ABI `22` und Portprojektvertrag `93` versioniert.
Analyzer-ABI `41` band den damaligen SDK-Vertrag einschliesslich des
Hardwareaudit-Layouts; die Zaehler ersetzen einander nicht.

Analyzer-ABI 34 band die typisierten Executor-/RAM-Fortschrittsfelder,
das Eviction-Ledger und den ganz-oder-gar-nicht-Vertrag persistierter
Resolution-/Presentation-Roots. Ein unvollstaendiger Root verwirft die
optionale Epoch typisiert als `incomplete-root`; ein spaeteres TerminalFull
muss ihn exakt neu berechnen.

Der konkrete ABI-34-Grund ist die oeffentliche Erweiterung von
`GuardedCodeInventoryWalkDiagnostics`: Provenance-Replay-Capsule-Budget und
Keybyte-Budget sowie deren Limitzaehler sind nun getrennt vom semantischen
Contextual-Return-Evaluation-Limit exponiert. Diese Layoutfelder duerfen nicht
als alter Evaluation-Zaehler interpretiert werden. Die historischen ABI-33-
Vertraege bleiben an ihre damaligen Source-/Laufstaende gebunden.

Seine kanonische Quelle ist `KATANA_ANALYZER_ABI_VERSION` in
`cmake/KatanaVersions.cmake`. CMake uebernimmt den Wert in:

- `katana::build_contract::analyzer_abi_version`;
- `katana::analysis::abi_version`;
- die Packagevariable `KatanaRecomp_ANALYZER_ABI_VERSION`;
- die transitive Compile- und Kompatibilitaetseigenschaft von
  `KatanaRecomp::analyzer`.

Der Vertrag umfasst die oeffentlichen C++-Layouts und Signaturen der
Analyzerbibliothek. Dazu gehoeren insbesondere
`FunctionRegisterValueSummary`, `ReturnedCodeAddressTableCandidate`,
`GuardedCodeInventory` und `FunctionValueAnalysisResult`. Eine inkompatible
Layout-, Signatur-, Enum- oder Besitzmodell-Aenderung erhoeht den
Analyzer-ABI-Zaehler unabhaengig von Runtime-, Block- und Backend-ABI.

## Technische Erzwingung

Oeffentliche Layoutheader verlangen die vom Analyzer-Target exportierte
Definition `KATANA_ANALYZER_ABI_VERSION` und vergleichen sie per
`static_assert` mit dem generierten Buildvertrag. Ein Wechsel des Targetwerts
veraendert den Compilebefehl und erzwingt damit den Neubau abhaengiger
Translation Units.

Jede Translation Unit mit einem oeffentlichen Analyzerlayout bindet
zusaetzlich eine ABI-spezifische Linkspezialisierung. Das Analyzerarchiv
definiert ausschliesslich die aktuelle Spezialisierung. Ein bereits mit
Analyzer-ABI N gebautes Objekt kann deshalb nicht gegen ein Archiv mit
Analyzer-ABI N+1 gelinkt werden.

Der installierte Packagevertrag prueft beide Grenzen mit einem
Out-of-Tree-Consumer:

- aktueller Zaehler: Configure, Compile und Link muessen gelingen;
- falscher erwarteter Zaehler: Configure muss scheitern;
- absichtlich fremde Linkspezialisierung: Link muss scheitern.

Das `runtime-sdk` enthaelt weiterhin keine Analyzerheader. Analyzerverbraucher
installieren zusaetzlich `analyzer-sdk` und linken den exportierten Target
`KatanaRecomp::analyzer`; rohe, unvertragliche Archiv- und Headerkombinationen
sind keine unterstuetzte SDK-Grenze.
