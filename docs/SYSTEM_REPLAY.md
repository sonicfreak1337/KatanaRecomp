# Deterministische Systemereignis-Replays

`SystemReplayLog` zeichnet Plattformereignisse ausschliesslich gegen logische
Gastzyklen auf. Host-Wall-Clock, Threadzeit, Dateizeitstempel und
Praesentationszeit sind weder Eingabe noch Berichtsfeld.

Ein Schedulerreset eroeffnet eine neue `time_epoch`. Monoton geordnet wird das
Paar aus Resetgeneration und Gastzyklus; innerhalb einer Epoche darf die
Gastzeit nicht rueckwaerts laufen, waehrend eine neue Epoche wieder bei Zyklus
null beginnen darf.

Das v1-Format unterscheidet:

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
stoppen. Jeder solche Verlust erhoeht jedoch `dropped_events`; eine
unvollstaendige Aufzeichnung darf weder versiegelt noch abgespielt werden.

Der Ereignishash ist eine portable FNV-1a-Pruefsumme ueber explizit kodierte
Felder. Er dient der Reproduzierbarkeitskontrolle und nicht als kryptografische
Integritaets- oder Sicherheitsgarantie.
