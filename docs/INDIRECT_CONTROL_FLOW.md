# Indirekter Kontrollfluss

## Lokale Konstantenpropagation

`propagate_local_constants` fuehrt einen konservativen Registerzustand durch
eine bereits geordnete Instruktionsfolge. Version 1 des Transfers unterstuetzt
Immediate-Moves, Immediate-Additionen, Registerkopien und NOP. 32-Bit-
Arithmetik verwendet definiertes unsigned Wraparound.

Sobald eine noch nicht modellierte Instruktion erreicht wird, werden alle
bekannten Registerwerte verworfen. Diese konservative Schranke verhindert,
dass spaetere indirekte Ziele aus veralteten Annahmen abgeleitet werden.
