# Kontrollierte Crashberichte

Der additive Runtimebaustein `CrashCapsule` wurde mit Runtime-ABI `116`
eingefuehrt und liegt im aktuellen Runtime-ABI als v3 vor. Er ergaenzt den
portablen v1-Bericht um begrenzte PC/PR-/Register-,
Modul-/Generations-, Materialisierungs-, Wait- und Dispatchdaten. Die
Produkt-Catch-Verdrahtung verwendet dafuer feste, sanitizte Tokens. Ein
kontrollierter Contract- oder Exceptiontext darf nur ueber den festen,
pfadfreien v3-Diagnosepuffer laufen. Hostpfade und Heap-/iostream-Nutzung
bleiben im Crashpfad ausgeschlossen; Gastbytes erscheinen ausschliesslich in
den expliziten festen Direct-RAM-Fenstern des Runtime-ABI-122-Vertrags.

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

Der portable v1-Bericht enthaelt bewusst keinen Runtime-Speicherdump und keine
freie Exceptionnachricht. Symbol- und Source-Map-Werkzeuge koennen die
numerischen Gastadressen nachtraeglich anreichern, ohne den v1-Crashvertrag zu
veraendern. Die additive Produkt-CrashCapsule v3 kann davon getrennt die unten
beschriebenen festen Direct-RAM-Fenster tragen.

## Produkt-CrashCapsule v3

Der generierte Native-Port besitzt daneben einen allocation-, formatierungs-
und lockfreien Crashpfad. `CrashCapsule` v3 erweitert den unveraenderten
v1-Grundvertrag additiv um bereits vorhandene, begrenzte Runtimefakten:

- Hostexception- und Contractcode sowie sanitizte Typ-Tokens;
- Gast-PC, PR, GPR/SR/GBR/VBR/MACH/MACL/FPUL/FPSCR, aktive Callsite und
  aktiver Entry;
- Runtime-/Source-Modulidentitaet samt Generation und Relocation;
- letzter Materialisierungs-/Provideruebergang;
- Wait-, Scheduler- und Schlafzustand;
- hoechstens 20 feste Direct-RAM-Fenster mit je 16 Woertern um GPR, PC, PR,
  GBR und VBR;
- bis zu 64 bereits aufgezeichnete Block-, Hardware-, Scheduler- und
  Fehlerereignisse in chronologischer Reihenfolge.

Jedes RAM-Fenster bindet Gastfokus, Gastbasis, kanonische physische Basis,
eine Quellenmaske und eine Wortgueltigkeitsmaske. Quellenbits 0 bis 15 stehen
fuer r0 bis r15, Bit 16 fuer PC, Bit 17 fuer PR, Bit 18 fuer GBR und Bit 19
fuer VBR. Nur `try_read_direct_linear_u32` darf Woerter aufnehmen; ungemappte,
MMIO- und Geraetebereiche bleiben ungueltig. Die kanonische Adressabbildung
bewahrt den SH-4-On-Chip-RAM-Aperturbereich und verhindert dessen Alias auf
gewoehnliches RAM. Die Erfassung erfolgt ausschliesslich nach einem bereits
kontrollierten Produktfehler und fuegt dem normalen Produktpfad keine Reads
hinzu. Es gibt weder einen variablen noch einen unbeschraenkten Speicherdump.

Alle Tokens besitzen feste Puffer und akzeptieren nur pfadfreie, portable
Zeichen. Der v3-Contract-Detailpuffer besitzt 511 Nutzbytes und akzeptiert
zusaetzlich die fuer bounded Adress-/Werttupel benoetigte portable
Interpunktion. Anfuehrungszeichen, Pfadtrenner und Steuerzeichen werden
fail-closed abgelehnt; Truncation und ungueltige Zeichen werden markiert, der
Hash bindet weiterhin den vollstaendigen Eingabetext. Freie Hostpfade,
unbeschraenkte Dumps und erfundene Thread-/Task-IDs werden nicht uebernommen;
die explizit markierten festen RAM-Fenster bleiben der einzige begrenzte
Speicherinhalt der v3-Zeile.

Die v3-Serializerbausteine sind `noexcept`, besitzen feste
Ausgabebudgets und traversieren im Crashpfad keine Ownershipgraphen. Der
eigentliche SEH- und aeussere Catch-Pfad liest ausschliesslich den gebundenen
CPU-Zeiger und die vorab aufgezeichnete feste CrashCapsule-POD-Struktur.
Exception-Adresse und sonstige Hostpointer werden
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
Auswerter koennen daher schrittweise auf die additive v3-Zeile wechseln.

Ein Runtime-Frontier-Record ist davon getrennt: Er ist ein streng
identitaetsgebundener Beobachtungshinweis fuer den naechsten statischen
Analyselauf und niemals selbst Closure-Evidenz.
