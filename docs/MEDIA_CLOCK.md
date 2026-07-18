# Deterministische Frame- und Audio-Taktung

KR-3105 fuehrt Video- und Audiofristen auf der gemeinsamen Gastzyklusuhr
zusammen. `DreamcastMediaClock` liefert typisierte Video- und Audioereignisse an
austauschbare Callbacks; konkrete PVR- und AICA-Backends bleiben davon getrennt.

## Vertrag

- Frame- und Audioereignisse werden ausschliesslich als Scheduler-Ereignisse
  erzeugt. Hostzeit, Sleeps und Threads beeinflussen die Semantik nicht.
- Nicht ganzzahlige Verhaeltnisse verwenden einen rationalen Restakkumulator.
  Dadurch erreicht etwa eine 10-Hz-Gastzyklusuhr bei 3 Hz exakt die Fristen
  3, 6, 10 statt dauerhaft gerundeter Drift.
- Audioereignisse enthalten Pufferindex, ersten Sampleframe und Frameanzahl.
  Videoereignisse enthalten Frameindex und Gastzyklus.
- Gleichzeitige Fristen folgen der stabilen Scheduler-ID-Reihenfolge. Das
  gemeinsame Ereignisbudget kann einen Lauf sichtbar und fortsetzbar stoppen.
- Stop entfernt beide ausstehenden Ereignisse. Ein Neustart ankert die Kadenz
  am aktuellen Gastzyklus; Reset loescht zusaetzlich alle Medienzaehler.
- Callbackfehler propagieren und stoppen beide Kadenzen, statt einen halbaktiven
  Medienpfad zu hinterlassen.
- Jeder Start erhaelt eine neue Laufgeneration. Alte Handler planen nach einem
  callback-internen `stop()`, `start()` oder `reset()` nichts mehr nach und
  koennen keine Event-ID eines neuen Laufs ueberschreiben.

KR-4703 koppelt den Video-Callback zusaetzlich an `HostPacer`. Diese Kopplung
wartet den Host bis zu einer aus Gastzyklen berechneten Deadline, veraendert
aber weder Schedulerreihenfolge noch Audio-/Videozaehler oder Gastresultate.
Pause/Resume verankert nur die Hostabbildung neu. Details stehen in
[`MUTABLE_STORAGE_AND_PACING.md`](MUTABLE_STORAGE_AND_PACING.md).

Die Regression treibt Recording-PVR- und Recording-Audio-Backends ueber diese
Callbacks. Die historische v0.31.0-GDI-Blockprobe prueft nur Quelle,
Bootblock und Plattformereignisse und gilt nicht als Sonic-Ausfuehrung. Der
erste vollstaendige lokale Sonic-Adventure-Lauf gehoert ausschliesslich in die
Alpha-Gate-Vorbereitung KR-4999.
