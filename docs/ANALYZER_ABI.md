# Analyzer-ABI

Der aktuelle oeffentliche Analyzervertrag ist Version `66`. Der aktuelle
Source-Stand verwendet Runtime-ABI 122, Block-ABI 5, PlatformServices-ABI 14,
Backend-Interface-ABI 24, Portprojektvertrag 103 und Native-Port-
Profilvertrag 24. GameProject-Vertrag 9/Artefaktformat 6 und Analysis-
Directives-Version 5, Native-AOT-Emissionsprofil 40, AOT-Partitionsschema 9
und Port-Metadata-Cache-
Schema 12 gehoeren zum aktuellen Exportvertrag; sie ersetzen nicht den
Analyzer-ABI-Zaehler.
Der bestehende Function-Value-Sanitylauf ist mit `463/463` Checks gruen, und
der erste vollstaendige `analyze-port`-Lauf samt identitaetsgebundenem Resume
ist abgeschlossen. Der historische Candidate-Resolution-Checkpoint
`49b0f72a9f49d60a4eb6e0481460cd57c5625735` bleibt ausschliesslich als
ABI-34-Referenz erhalten.

Der aktuelle Native-Port-Vertrag verwendet
`NativePortDefinition` `12`, `NativePortArtifact` `14` und
Hardware-Closure `v8`. Seine fail-closed Beweisschicht bildet
`OwnerSemanticSummary` auf der Analyzer-Seite und den
`NativeProviderSemanticContract` (Runtime-Typ
`NativePortProviderSemanticContract`) auf der Provider-Seite. Das Owner-
Summary bindet eine exakte, identity-bound Funktion mit geschlossener
CFG/SCC-, Guard- und Effekt-Sicht; die Ergebnisprojektion muss ebenfalls
vollstaendig sein. Der Providervertrag bindet Hookadresse und Symbol an
Owner-Semantikidentitaet und getrennte Provider-Implementierungsidentitaet
sowie dieselben geordneten Guards, Effekte und Resultate. Jede fehlende,
unbekannte, truncierte oder nicht identische Evidenz bleibt ein Gap und darf
keinen Replacement-Hook schliessen. Der alte Dreamcast-Geraetepfad bleibt
dabei ausschliesslich internes Offline-Orakel, nie Produktlink oder Runtime.

Runtime-ABI 122 erweitert CrashCapsule v3 um feste Direct-RAM-Fenster um
GPR, PC, PR, GBR und VBR. Die Fenster werden nur im kontrollierten
Produktfehlerpfad und ohne MMIO-/Geraetezugriff gelesen; der normale
Produktpfad bleibt ohne zusaetzliche Arbeit. Runtime-ABI 121 fuehrte die
CrashCapsule-v3-Bindung im `NativePortContext` und den festen, pfadfreien
Diagnosetextpuffer ein. Damit bleiben vollstaendige, adressgebundene
Contract-Details erhalten, ohne Heapallokation, unbeschraenkte Ausgabe oder
Hostpfade in den Crashpfad aufzunehmen. Der Analyzervertrag bleibt 66.

Analyzer-ABI 66 und Runtime-ABI 120 korrigieren den oeffentlichen Store-
Queue-PREF-Effektvertrag: SH-4 `PREF` commitet eine vollstaendige 32-Byte-
Queuezeile. Runtimevalidator und Analyzer-Aequivalenz binden dieselbe
oeffentliche Breitenkonstante; der zuvor akzeptierte, aber zum Hardwareaudit
inkompatible 4-Byte-Providervertrag wird fail-closed abgelehnt. Provider-
Semantikvertrag und Semantic-Identity-Domaene stehen deshalb auf `v3`, das
Native-Port-Artefaktformat auf 14. Cache- und andere Prefetch-Ressourcen
bleiben unveraendert ausserhalb dieses engen Vertrags.
`inspect-native-provider-semantics <Artefakt> --format agent-json` laedt ein
privates Native-Port-Artefakt read-only, gibt fuer jeden Providervertrag die
deklarierte und kanonisch berechnete Semantikidentitaet aus und endet bei
einer Abweichung fail-closed.

Analyzer-ABI 65 und Runtime-ABI 119 banden latente AOT-Owner erstmals an
dieselbe Beweiskette wie statische Image-Owner. Das Native-Disc-
Analyseartefakt Schema 8 traegt dazu nur einen bounded, gegen die aktuelle
Disc- und Modulidentitaet revalidierten Ledger exakter PC-relativer s16/u32-
Literale; Retailbytes selbst werden nicht serialisiert. Ein Hook aus einer
latenten AOT-Quelle muss `NativePortHookCodeSource::LatentAotModule`, die
exakte Modulidentitaet, eine eindeutige Funktionsidentitaet und die komplette
Hookgrenze binden. Statische Image-Hooks behalten ihren bisherigen
Containmentvertrag. Als erster enger Verbraucher erkennt das Owner-Summary
einen exakt vierblockigen SH-4-SPG_STATUS-Wait auf dieselbe immutable
Statusadresse und denselben Maskenwert. Nur die beiden belegten Set-/Clear-
SCCs werden als stabile Wait-Phasen repraesentiert; andere Schleifen,
Hardwarezugriffe und Literale bleiben unveraendert fail-closed.

Historisch fuehrten Analyzer-ABI 64 und Runtime-ABI 118 im oeffentlichen Provider-
Effektvertrag um die append-only Operation `Prefetch`. Owner-Effekte muessen
Kind, Operation und Queue-Ressource exakt korrelieren; ein als Read
degradierter Prefetch oder ein Prefetch ohne Hardware-Referenz ist nicht
representierbar. Provider-Semantikvertrag und Semantic-Identity-Domaene
stehen deshalb seitdem auf `v2`; das damalige Native-Port-Artefaktformat war
12. Alte
Vertraege und Artefakte werden fail-closed abgelehnt und koennen keine
Hardware-Closure publizieren. Der weiterhin gueltige Effektvertrag
repraesentiert nur einen
auditierten SH-4-Store-Queue-PREF; Cache- und andere Prefetch-Ressourcen
bleiben bis zu einer eigenen Klassifikation offen.

Historisch trennte Analyzer-ABI 63 die IR-Implementierungsidentitaet in
unoptimiertes
Analysis-/Lowering-IR und Produktoptimierung. Boot-/CFA-/FVA-Caches binden nur
den ersten Vertrag. Der monolithische Native-Disc-Checkpoint bleibt als
Schema 7 ausdruecklich an die Produktoptimierung gebunden, weil er weiterhin
optimiertes Primaer- und Latent-IR enthaelt. Eine reine Optimizeraenderung darf
daher Analysezustand wiederverwenden, aber niemals veraltetes Produkt-IR als
aktuellen Checkpoint akzeptieren. Runtime-Frontier-Bindings tragen diese
Identitaet ab Binding-Version 3 ebenfalls explizit.

ABI 63 ersetzt ausserdem die ueberladene boolesche Storage-Authority in
`FunctionValueSummary` durch die getrennten Zustaende `Provisional`,
`Committed` und `TerminalTop`. Nur `Committed` darf positive Cell-/Epoch-/
Loss-Fakten publizieren; `TerminalTop` ist autoritativ und dauerhaft
fail-closed, ohne provisorische Fakten durch Cache-, Summary- oder
Contextual-Replay in Caller zu tragen. Persistente Function-Value-Epochen
binden diesen Vertrag mit Schema 38 und Evaluation-Lens-Schema 8.

Analyzer-ABI 62 erweitert den oeffentlichen Boot-Analyseparser um einen
bounded Diagnosegrund. Damit bleibt nach einem abgelehnten Checkpoint nicht
nur `Miss` oder `Corrupt`, sondern auch die exakte Codecphase sichtbar.

Analyzer-ABI 61 bindet den persistenten Cache-Schreibvertrag von
`LatentAotDiscoveryOptions`. Autoritative Agentanalysen duerfen weiterhin
exakt identitaetsgebundene Cacheeintraege lesen, publizieren aber keine
Epoch-, IR- oder Root-Seeds, bevor die vollstaendige Kandidatengeneration das
aeussere Session-Authority-Gate passiert hat. Ein verworfener Kandidat kann
dadurch keinen spaeteren Resume-Lauf mit nichtautoritativen Fakten speisen.
Das transaktionale Agent-Session-Ledger-Schema 5 bindet dazu den stabilen
semantischen Eingabevertrag getrennt von der wechselnden
Analyzer-Implementierungsidentitaet.

Analyzer-ABI 60 trennt die policyseitige Deaktivierung des
interprozeduralen Function-Value-Fixpunkts von der realen Guest-Call-ABI des
analysierten Images. RuntimeOnly darf FVA auslassen, ohne dadurch den
bewiesenen SH-C-Vertrag fuer die nonvolatilen Register r8 bis r14 aus der
lokalen CFA zu loeschen. Eine identitaetsgebundene statische Pointerkette darf
einen intervenierenden Call nur dann als exakten Transfer ueberqueren, wenn
der Wert auf jedem solchen Abschnitt in einem nonvolatilen Register liegt und
das Image ausdruecklich `SuperHC` bindet. Unbekannte ABI bleibt fail-closed.
Die neue FVA-Policy ist Teil von Session- und Boot-Cache-Identitaet; ein
policyfremder Cache oder Resume-Stand kann daher keine Closure publizieren.

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
