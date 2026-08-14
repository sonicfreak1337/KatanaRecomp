# Analyzer-ABI

Der aktuelle oeffentliche Analyzervertrag ist Version `48`. Der aktuelle
Source-Stand verwendet Runtime-ABI 107, Block-ABI 5, PlatformServices-ABI 14,
Backend-Interface-ABI 23, Portprojektvertrag 97 und Native-Port-
Profilvertrag 20. GameProject-Vertrag 8/Artefaktformat 6 und Analysis-
Directives-Version 4, Native-AOT-Emissionsprofil 36 und Port-Metadata-Cache-
Schema 8 gehoeren zum aktuellen Exportvertrag; sie ersetzen nicht den
Analyzer-ABI-Zaehler.
Der RuntimeOnly-Bring-up-Meilenstein verwendet diesen unveraenderten
Analyzer-ABI-Vertrag; der historische Candidate-Resolution-Checkpoint
`49b0f72a9f49d60a4eb6e0481460cd57c5625735` bleibt als ABI-34-Referenz erhalten.
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
