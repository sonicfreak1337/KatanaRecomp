# Analyzer-ABI

Der aktuelle oeffentliche Analyzervertrag ist Version `1`. Seine kanonische
Quelle ist `KATANA_ANALYZER_ABI_VERSION` in
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
