# Deterministische Systemereignis-Replays

`SystemReplayLog` zeichnet Plattformereignisse ausschliesslich gegen logische
Gastzyklen auf. Host-Wall-Clock, Threadzeit, Dateizeitstempel und
Praesentationszeit sind weder Eingabe noch Berichtsfeld.

Ein Schedulerreset eroeffnet eine neue `time_epoch`. Monoton geordnet wird das
Paar aus Resetgeneration und Gastzyklus; innerhalb einer Epoche darf die
Gastzeit nicht rueckwaerts laufen, waehrend eine neue Epoche wieder bei Zyklus
null beginnen darf.

Das v2-Format unterscheidet:

- CPU-Safepoints
- MMIO-Lese- und Schreibzugriffe
- DMA
- Interrupts
- Timer
- Schedulercallbacks
- Video- und Audioereignisse
- externe Eingaben und andere Hostereignisse

Safepoints koennen direkt beim `SchedulerSafepoints`-Objekt angebunden werden.
`system_replay_mmio_observer()` verwendet den bestehenden Speicherobserver.
DMA-, Interrupt-, Timer-, Scheduler- und Medienpfade liefern typisierte
`SystemReplayEvent`-Eintraege mit stabilem Ereigniscode, Gastzyklus und den
erforderlichen numerischen Parametern.

## Begrenzung und portable Ereigniscodes

`SystemReplayLog` verwendet standardmaessig eine feste Kapazitaet von 4096
Ereignissen. Eine lokale Konfiguration darf sie verkleinern oder bis hoechstens
65536 Ereignisse anheben. Null und groessere Werte werden vor einer
Speicherreservierung abgelehnt. Portable Ereigniscodes sind auf 64 Zeichen und
die bereits festgelegte ASCII-Zeichenmenge begrenzt. Vor der Speicherung wird
der Code in eine eigene kurze Zeichenfolge normalisiert; eine vom Aufrufer
uebernommene uebergrosse `std::string`-Kapazitaet kann deshalb den RAM-Vertrag
nicht umgehen.

Erreicht `record()` die Kapazitaet, markiert es genau einen Drop und bricht den
Aufruf sichtbar ab. `try_record()` liefert dafuer `false`, ohne denselben Drop
ein zweites Mal zu zaehlen. Jede unvollstaendige Aufzeichnung bleibt
unversiegelbar. Nach erfolgreichem `seal()` kann auch ein spaeter
Best-effort-Drophinweis den versiegelten Zustand nicht mehr veraendern.

## Externe Injektionen

`ExternalInput` und `HostEvent` werden von `record()` abgelehnt, solange sie
nicht ueber `inject()` ausdruecklich als injiziert markiert wurden. Umgekehrt
darf kein deterministisches internes Ereignis das Injektionsbit tragen. Damit
kann ein Replay keine nichtdeterministische Hostquelle still als interne
Wahrheit behandeln.

## Abschluss und Verifikation

Vor dem Replay wird die Aufzeichnung mit einem Gastzustandshash versiegelt.
`hash_replay_guest_state()` hasht alle expliziten SH-4-Register- und
Steuerfelder, Schedulerzyklus und einen vom Plattformverbund gelieferten
Subsystemhash. Speicher, MMIO, DMA, Timer, Interrupt- und Medienzustand werden
ueber diesen Subsystemhash gebunden, ohne rohe Speicher- oder Firmwarebytes im
Bericht abzulegen.

`DeterministicSystemReplay` vergleicht jedes beobachtete Ereignis sofort mit
dem naechsten erwarteten Eintrag. Ein fehlendes, zusaetzliches, anders
sortiertes oder inhaltlich abweichendes Ereignis wirft
`SystemReplayMismatch` am ersten betroffenen Index. Nach dem letzten Ereignis
muss auch der Gastzustandshash exakt uebereinstimmen.

Best-effort-Hooks koennen die Gastausfuehrung bei einem Diagnosefehler nicht
stoppen. Jeder solche Verlust erhoeht jedoch vor der Versiegelung
`dropped_events`; eine unvollstaendige Aufzeichnung darf weder versiegelt noch
abgespielt werden.

Der Ereignishash ist eine portable FNV-1a-Pruefsumme ueber explizit kodierte
Felder. Er dient der Reproduzierbarkeitskontrolle und nicht als kryptografische
Integritaets- oder Sicherheitsgarantie.

## Redigierter JSON-Vertrag

Der v2-JSON-Bericht ist standardmaessig redigiert. `code`, `address`, `value`,
`detail` und `auxiliary` werden als `null` ausgegeben. Auch `event_hash` und
`final_guest_state_hash` bleiben `null`, weil diese Felder sonst weiterhin
private Gastidentitaeten, Adressen, Werte oder daraus abgeleitete exakte
Fingerabdruecke veroeffentlichen wuerden. `capacity`, `serialize_values`,
`codes_redacted`, `addresses_redacted`, `values_redacted`,
`numeric_payloads_redacted` und `hashes_redacted` machen den aktiven Vertrag
explizit. Zahlen- und Hexfelder verwenden unabhaengig von der globalen
Host-Locale immer die klassische portable Schreibweise.

Die interne Aufzeichnung, `event_hash()` und `DeterministicSystemReplay`
behalten trotzdem alle Werte und vergleichen sie exakt. Nur ein ausdrueckliches
lokales `SystemReplayConfig::serialize_values=true` schreibt Codes, Adressen,
Werte, numerische Payloads sowie Ereignis- und Endzustandshash in JSON. Dieses
Opt-in ist fuer lokale Differentialdiagnostik bestimmt und darf nicht fuer
verteilbare oder private redigierte Fehlerpakete aktiviert werden.
