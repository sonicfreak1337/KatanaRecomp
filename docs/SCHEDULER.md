# Deterministischer Event-Scheduler

KR-3101 fuehrt eine zentrale Gastzyklusuhr fuer die Dreamcast-Plattform ein.
Der Scheduler verwendet weder Hostzeit noch Threads. Alle Fristen sind absolute
64-Bit-Gastzyklen.

## Vertrag

- Ereignisse laufen aufsteigend nach Gastzyklus und bei gleicher Frist stabil
  nach ihrer monotonen Ereignis-ID.
- Callbacks sehen den exakten Ereigniszyklus als aktuellen Schedulerzustand und
  duerfen weitere Ereignisse fuer denselben oder einen spaeteren Zyklus planen.
- Rekursive `advance_to()`-/`advance_by()`-Aufrufe und `reset()` aus einem
  Callback werden mit `std::logic_error` abgewiesen. Dadurch bleibt die
  Gastzyklusuhr auch bei Reentrancy-Versuchen streng monoton.
- Verschachteltes Planen und Abbrechen von Ereignissen bleibt ausdruecklich
  erlaubt und wird noch im laufenden Advance deterministisch beruecksichtigt.
- Ereignisse in der Vergangenheit, rueckwaertslaufende Zeit und
  Zielzyklusueberlauf sind strukturierte Fehler.
- Cancellation entfernt nur noch ausstehende Ereignisse und meldet sichtbar, ob
  eine ID tatsaechlich vorhanden war.
- Jeder Lauf besitzt ein explizites Ereignisbudget. Reicht es nicht aus, stoppt
  der Scheduler am letzten verarbeiteten Ereignis und meldet
  `EventBudgetExhausted`, statt unkontrolliert weiterzulaufen.
- Callback-Ausnahmen werden nicht verschluckt. Das betroffene Ereignis gilt als
  verarbeitet, die Uhr bleibt an seiner Frist stehen und der Fehler propagiert.
- `reset()` stellt Uhr, IDs, Zaehler und Ereignismenge ausserhalb eines laufenden
  Advances deterministisch zurueck.

TMU, RTC, DMA, Interruptintegration sowie Frame- und Audio-Taktung werden in den
folgenden KR-3102- bis KR-3105-Tasks auf diese Zeitbasis gesetzt. KR-3101 zieht
diese Geraetesemantik nicht vor.
