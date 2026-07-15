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

Die Regression treibt Recording-PVR- und Recording-Audio-Backends ueber diese
Callbacks. Ein vollstaendiger Sonic-Adventure-Lauf gehoert weiterhin ausschliesslich
zum v0.31.0-Phase-Gate und wird erst nach der vereinbarten Review-/Fixrunde
ausgefuehrt.
