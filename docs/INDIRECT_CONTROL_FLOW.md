# Indirekter Kontrollfluss

## Lokale Konstantenpropagation

`propagate_local_constants` fuehrt einen konservativen Registerzustand durch
eine bereits geordnete Instruktionsfolge. Version 1 des Transfers unterstuetzt
Immediate-Moves, Immediate-Additionen, Registerkopien und NOP. 32-Bit-
Arithmetik verwendet definiertes unsigned Wraparound.

Sobald eine noch nicht modellierte Instruktion erreicht wird, werden alle
bekannten Registerwerte verworfen. Diese konservative Schranke verhindert,
dass spaetere indirekte Ziele aus veralteten Annahmen abgeleitet werden.

## Registerwertanalyse

Die Registerwertanalyse erweitert den lokalen Transfer um registerweise Add-,
Sub- und logische Operationen. Fuer jede indirekte `JMP`-/`JSR`-Stelle zeichnet
sie den verwendeten Registerindex und den davor beweisbaren Wert auf. Ein
fehlender Wert bleibt explizit unbekannt und wird nicht geraten.
