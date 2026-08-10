# Analyzer-ABI

Der aktuelle oeffentliche Analyzervertrag ist Version `34`. Der aktuelle
Source-Stand verwendet Runtime-ABI 89, Block-ABI 5, PlatformServices-ABI 14,
Backend-Interface-ABI 13 und Portprojektvertrag 75.
Der RuntimeOnly-Bring-up-Meilenstein verwendet diesen unveraenderten
Analyzer-ABI-Vertrag; der historische Candidate-Resolution-Checkpoint
`49b0f72a9f49d60a4eb6e0481460cd57c5625735` bleibt als ABI-34-Referenz erhalten.
Version 34 ist die Kompatibilitaetsgrenze fuer die aktuell exportierten
Layouts, Signaturen und Analyseergebnisse. Analyzer-ABI 11 band historisch
die engere Provenienz fuer 32-Bit-PC-relative
Code-Literale: Erst eine echte Call- oder Tail-ABI-Grenze darf sie zu einem
bewachten Codepointerargument machen. Normale Objektfeldloads und bedingte
Owner-Uebergaenge erhalten diesen Beweis nicht. Damit invalidieren Analyse-,
IR-, Codegen- und Whole-Export-Caches den alten Bestand, der solche direkt vor
einem Tail-Registrar geladenen Callbacks noch verlor.

Analyzer-ABI 34 bindet die typisierten Executor-/RAM-Fortschrittsfelder,
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
