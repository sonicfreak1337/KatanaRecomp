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

## Einfache indirekte Calls und Spruenge

Eine indirekte Stelle gilt nur dann als `resolved`, wenn der beobachtete
Registerwert gerade ist und auf zwei committed Bytes eines ausfuehrbaren
Code-Segments zeigt. Der Grund `constant-register` dokumentiert den Beweis.
Unbekannte Werte und ungueltige Zielbereiche bleiben mit getrennten stabilen
Gruenden `unresolved`.

## Jump Tables

`analyze_jump_table` wertet eine bekannte, vier Byte ausgerichtete Tabelle mit
einer expliziten endlichen Eintragszahl aus. Absolute 32-Bit-Ziele werden nur
akzeptiert, wenn sie gerade sind und auf committed ausfuehrbaren Code zeigen.
Die Bereichsschranke liegt bei 4096 Eintraegen. Fehlende Eintraege, ungueltige
Ziele und unbeschraenkte Kandidaten bleiben als abgelehnt sichtbar; eine nur
teilweise gueltige Tabelle gilt nicht als aufgeloest.
