# Eingabeprovenienz und Buildidentitaet

KatanaRecomp erfasst externe Eingaben als Rolle, Bytegroesse und SHA-256. Der
lokale absolute Pfad bleibt getrennt in `InputProvenance::local_path` und geht
weder in die portable Buildidentitaet noch in den standardmaessigen JSON-Bericht
ein. Gleiche Bytes an verschiedenen Orten besitzen deshalb dieselbe portable
Identitaet; bereits ein geaendertes Byte aendert den Hash.

Der versionierte Bericht `katana-build-provenance` enthaelt:

- Werkzeugversion sowie Manifest- und optionale Analyseanweisungshashes
- IR-Version, Runtime-ABI, Backendname und Backend-ABI
- fuer jede externe Eingaberolle ausschliesslich Groesse und SHA-256

Die portable Buildidentitaet ist der SHA-256 dieses deterministisch sortierten
Berichts. Sie reicht zur Wiederholung der Werkzeugkonfiguration und zur sicheren
Cacheentscheidung, enthaelt aber weder absolute Pfade noch Firmwarestrings,
Flashdaten oder rekonstruierbare Eingabebytes.

Der Codegen-Cachevertrag ist deshalb Version 2. Sein Schluessel bindet nun auch
die Werkzeugversion ein; Eingabe-, IR-, Konfigurations-, Manifest- und
Anweisungshash sowie Runtime-/Backend-/Optimierungs-ABIs bleiben getrennte
Invalidierungskomponenten.
