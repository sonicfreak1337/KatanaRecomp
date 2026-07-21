# Block-, Alias- und Invalidierungsprovenienz

`serialize_runtime_provenance_json()` verbindet den Zustand des
`ExecutableCodeTracker` mit dem `FirmwareHandoffMap` in einem deterministischen,
versionierten Bericht. Der Bericht enthaelt keine Code-, Firmware- oder
Speicherbytes.

## Blockherkunft

Jede ausfuehrbare Blockregistrierung besitzt eine strukturierte Herkunft:

- `image-segment`
- `rom-ram-copy`
- `fallback-decode`
- `runtime-write`

Identitaet, kanonische physische Adresse, Groesse, portable Provenienz,
Gueltigkeit und eingehende Links bleiben sichtbar. Eine reaktivierte Identitaet
darf Adresse, Groesse, Provenienz oder Herkunft nicht wechseln.

## Aliase, Kopien und Laufzeitsymbole

Firmwareabbildungen werden nach kanonischem physischem Bereich gruppiert. Jeder
virtuelle Alias bleibt mit Name und Segmentart einzeln aufgefuehrt. Verifizierte
ROM-RAM-Codekopien verbinden Quell- und Zielbereich, Groesse und
Aenderungszustand, jedoch niemals den kopierten Inhalt.

Dynamisch installierte BIOS-ABI-Symbole enthalten virtuelle und physische
Adresse, portable Provenienz sowie den Gastzyklus ihrer Installation. Dadurch
bleibt sichtbar, dass ein Vektor Laufzeitzustand und kein statisches ROMsymbol
ist.

## Invalidierungen

Jede beobachtete CPU-, DMA- oder Kopierschreiboperation erhaelt eine monotone
Sequenz, virtuelle und kanonische Adresse, Groesse sowie die betroffenen Seiten
mit ihrer neuen Generation. Invalidierte Bloecke und geloeste eingehende Links
werden gemeinsam festgehalten. Nachweislich bytegleiche Schreibvorgaenge
bleiben als Ereignis sichtbar, erhoehen aber keine Generation.

Fuer Demand-Materialisierung fuehrt der Dreamcast-Produktpfad zusaetzlich eine
begrenzte Bytebelegung fuer tatsaechlich geaenderte Haupt-RAM-Writes. Sie ist
kein Codebeweis und verleiht weder einer Seite noch dem RAM Ausfuehrungsrecht.
Erst ein realer, ausgerichteter Kontrolltransfer auf zwei belegte Bytes darf
einen maximal 128 Byte grossen Snapshot autorisieren. Dessen Byteidentitaet
wird vor Registrierung und vor jedem Dispatch erneut geprueft; ein
ueberlappender Write deaktiviert ihn aliasunabhaengig.

Die Historie wird best-effort nach der eigentlichen Invalidierung angelegt. Ein
Speichermangel kann deshalb nur `dropped_invalidation_events` erhoehen und weder
Seitengeneration, Blockgueltigkeit noch Linkaufloesung veraendern. Nicht portable
Labels mit Pfad- oder Kontrollzeichen werden im Bericht als `redacted`
ausgegeben.
