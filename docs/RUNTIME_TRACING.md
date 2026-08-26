# Runtime-Tracing und Watchpoints

Der Standard-Bring-up aus
[`NATIVE_BRINGUP_WORKFLOW.md`](NATIVE_BRINGUP_WORKFLOW.md) verwendet keine
dauerhafte Vollspur. Er schreibt vorallokierte, begrenzte Events oder Rolling
Digests und aktiviert detailliertes Tracing erst fuer ein kurzes
First-Divergence-Intervall. Ein Trace, der Frames auf Sekunden verlaengert,
ist ausschliesslich ein gezieltes Offline-/Diagnosewerkzeug.

Traceereignisse sind `Observed`. Sie duerfen einen statischen Befund
widerlegen oder einen Promotion-Auftrag erzeugen, aber niemals Blocktabelle,
Allowlist, Dispatchklasse oder Closure veraendern.

`RuntimeTraceRecorder` sammelt IR-, Block-, Speicher-, Watchpoint-, Exception-
und Schedulerereignisse in einer gemeinsamen, streng monotonen Reihenfolge.
Jedes Ereignis besitzt Sequenznummer, logischen Gastzyklus, Ursprung und PC;
optionale Adresse, Breite und ein portabler Ereigniscode ergaenzen den Kontext.
Host- oder Presentationzeit ist kein Bestandteil des Formats.

Die Kapazitaet ist fest und darf nicht null sein. Nach Erreichen der Grenze
bleiben die ersten vollstaendigen Ereignisse erhalten; weitere Ereignisse
erhoehen sichtbar `total_events` und `dropped_events`. Ein rueckwaerts laufender
Gastzyklus oder Zaehlerueberlauf wird abgelehnt.

Bestehende `Memory`-Tracehandler und Watchpoints koennen ueber
`memory_observer()` an denselben Recorder gebunden werden. Ein globaler Handler
verwendet die Speicherarten, ein Watchpoint die Art `watchpoint`. Die Provider
fuer PC und Gastzyklus werden erst beim Treffer abgefragt.

Speicherwerte sind standardmaessig redigiert und erscheinen als `null`. Nur die
explizite lokale Option `capture_memory_values` nimmt Rohwerte auf. Regionsnamen
und freie Callbacktexte werden nicht serialisiert. Das versionierte JSON ist
dadurch standardmaessig fuer portable Crash- und Replaydiagnostik geeignet.
