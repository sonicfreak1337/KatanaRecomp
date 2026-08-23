# Kontrollierte Crashberichte

Der additive Runtimebaustein `CrashCapsule` v2 wurde mit Runtime-ABI `116`
eingefuehrt und ist im aktuellen Runtime-ABI `120` enthalten. Er ergaenzt den
portablen v1-Bericht um begrenzte PC/PR-,
Modul-/Generations-, Materialisierungs-, Wait- und Dispatchdaten. Die
Produkt-Catch-Verdrahtung verwendet dafuer feste, sanitizte Tokens. Freie
Hosttexte, Pfade, Retailbytes und Heap-/iostream-Nutzung bleiben im Crashpfad
ausgeschlossen.

Der versionierte Bericht `katana-crash-report` beschreibt einen kontrollierten
Runtime-Abbruch ohne freien Hostfehlertext. Sein `stop_code` und alle Herkunfts-
beziehungsweise Aktionsfelder sind portable Tokens; Pfade, Hostzeiger und
Speicherinhalte koennen deshalb nicht versehentlich ueber diese Felder
serialisiert werden.

Erfasst werden:

- virtueller PC und kanonische physische Adresse
- allgemeine und banked Register sowie PR, SR, FPSCR und Exceptionregister
- Trap-, Exception- und Delay-Slot-Zustand mit tatsaechlichem Fault-PC,
  physischer Herkunft und gelatchtem Owner-PC
- getrennte Zaehler fuer versuchte und abgeschlossene Gastinstruktionen sowie
  gesamte und noch nicht an den Scheduler uebergebene Gastzyklen
- Blockadresse, Endtyp, Provenienz und alle Blockvariantengenerationen
- logischer Schedulerzyklus und Anzahl ausstehender Ereignisse
- letzter Dispatch mit Callsite, Ziel, PR, Herkunft und Aktion

Die physische Blockadresse wird beim Capture kanonisiert. Blockadresse,
Blockvariante und Blockprovenienz muessen gemeinsam vorliegen. Ein Dispatch mit
Adresse verlangt Herkunft und Aktion. Bei einer Exception im Delay Slot wird
ein fehlender Owner bevorzugt aus der zur aktuellen Exceptiongeneration
gelatchten Fault-Metadaten und nur fuer aeltere Zustaende aus dem architektonischen
SPC uebernommen.

Der Bericht enthaelt bewusst keinen Runtime-Speicherdump und keine freie
Exceptionnachricht. Symbol- und Source-Map-Werkzeuge koennen die numerischen
Gastadressen nachtraeglich anreichern, ohne den Crashvertrag zu veraendern.

## Produkt-CrashCapsule v2

Der generierte Native-Port besitzt daneben einen allocation-, formatierungs-
und lockfreien Crashpfad. `CrashCapsule` v2 erweitert den unveraenderten
v1-Grundvertrag additiv um bereits vorhandene, begrenzte Runtimefakten:

- Hostexception- und Contractcode sowie sanitizte Typ-Tokens;
- Gast-PC und PR, aktive Callsite und aktiver Entry;
- Runtime-/Source-Modulidentitaet samt Generation und Relocation;
- letzter Materialisierungs-/Provideruebergang;
- Wait-, Scheduler- und Schlafzustand;
- bis zu 16 bereits aufgezeichnete Block-, MMIO-, Scheduler- und
  Fehlerereignisse in chronologischer Reihenfolge.

Alle Tokens besitzen feste Puffer und akzeptieren nur pfadfreie, portable
Zeichen. Truncation und ungueltige Zeichen werden markiert. Freie
Exceptiontexte, Hostpfade, Retailbytes, Dumps und erfundene Thread-/Task-IDs
werden nicht uebernommen.

Die v2-Serializerbausteine sind `noexcept`, besitzen feste
Ausgabebudgets und traversieren im Crashpfad keine Ownershipgraphen. Der
eigentliche SEH- und aeussere Catch-Pfad besitzt keinen Capture-
Funktionszeiger: Er liest ausschliesslich die vorab aufgezeichnete feste
CrashCapsule-POD-Struktur. Exception-Adresse und sonstige Hostpointer werden
fail-closed nicht serialisiert; bei fehlendem PC bleibt der Wert null.
Die reichere Runtime-Aufzeichnung erfolgt nur vor dem eigentlichen Handler
ueber den normalen, kontrollierten Pfad; der Handler selbst verwendet den
bereits vorliegenden Snapshot.
Der generierte Windows-Produktcode installiert fuer seine Lebensdauer einen
Unhandled-Exception-Filter, kettet den vorherigen Filter und stellt ihn beim
Verlassen wieder her. SEH und C++-Fehler teilen eine atomare Exactly-once-
Ausgabe; die fertige Zeile wird direkt als begrenzter Bytepuffer geschrieben,
nicht ueber `iostream` oder dynamische JSON-Serializer.
Die aeusseren Lifecycle- und Runtime-Probe-Catches emittieren vor ihrer
Terminalzusammenfassung nur feste Tokens. Lifecycle verwendet
callsite/return_address aus dem POD-Evidence; beim Probe-Abbruch werden beide
Werte als null uebergeben und intern auf last_pc zurueckgefuehrt.
Die bestehende v1-Zeile wird davor weiterhin ebenfalls bounded ausgegeben;
Auswerter koennen daher schrittweise auf die additive v2-Zeile wechseln.

Ein Runtime-Frontier-Record ist davon getrennt: Er ist ein streng
identitaetsgebundener Beobachtungshinweis fuer den naechsten statischen
Analyselauf und niemals selbst Closure-Evidenz.
