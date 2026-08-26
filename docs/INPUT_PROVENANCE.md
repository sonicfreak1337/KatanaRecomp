# Eingabeprovenienz und Buildidentitaet

Die Zuordnung der Eingaben zu den beiden Entwicklungsloops ist in
[`NATIVE_BRINGUP_WORKFLOW.md`](NATIVE_BRINGUP_WORKFLOW.md) festgelegt. Die
Identitaet einer AOT-wirksamen Eingabe gehoert zum statischen Pack; eine
Runtime-, Adapter- oder reine Diagnostikaenderung darf dieses Pack nicht
still veraendern. Ein Bring-up-Replay bindet deshalb Pack-, Runtime-, Adapter-,
Manifest- und Replayidentitaet getrennt und verwendet bei unveraenderten
statischen Eingaben denselben Pack.

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

Der Anwendungsjob erfasst GDI-Descriptor, Tracks und optionale Symbol-,
Analyseanweisungs-, BIOS- und Flashdateien als einen gemeinsamen Snapshot.
Projektidentitaet, Portprovenienz und Build verwenden exakt diesen Snapshot;
der Portexport analysiert die Eingabe nicht ein zweites Mal. Vor einem
erfolgreichen Abschluss werden die Rollen erneut read-only geprueft. Eine
waehrend des Jobs veraenderte wirksame Eingabe fuehrt zu einem sichtbaren
Verarbeitungsfehler statt zu gemischten Artefakten.

Eingabeprovenienz ist eine Identitaets- und Invalidierungsgrundlage, kein
Runtime-Beweis. Ein Log, Witness oder beobachtetes Ziel aendert weder Hash noch
Packstatus und wird nicht automatisch von `Observed`/`Candidate` nach `Proven`
promotet. Eine Aenderung an Disc-/Imagebytes, FunctionMap, Funktionsgrenzen,
Overlays oder AOT-wirksamen Patches erzwingt einen neuen Strict-Product-Lauf;
der kleine Bring-up-Loop darf nur die unveraenderten statischen Artefakte
wiederverwenden.
