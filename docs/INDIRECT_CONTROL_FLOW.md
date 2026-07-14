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
Adressluecken setzen den lokalen Registerzustand zurueck, sodass Konstanten nicht
zwischen getrennt entdeckten Codebereichen weitergetragen werden.

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

Die Tabellenbasis muss vier Byte ausgerichtet sein. Die exklusive Endadresse wird
mit 64-Bit-Zwischenwerten berechnet, weshalb ein einzelner Eintrag bei
`0xFFFFFFFC` arithmetisch gueltig bleibt. Jeder Eintrag selbst muss committed
sein. Die Dispatch-Adresse muss als `JMP @Rn` oder `JSR @Rn` entdeckt worden
sein; Call-Tabellen erzeugen Funktionskandidaten, Jump-Tabellen nicht.

## Override-Datei Version 1

Die Textdatei beginnt mit `version = 1`. Wiederholbare Eintraege besitzen die
Formen `function = ADRESSE`, `jump = STELLE ZIEL` und
`jump_table = STELLE TABELLE ANZAHL`. Adressen sind hexadezimal, die Anzahl ist
dezimal. Der Parser sortiert alle Hinweise numerisch und weist doppelte Stellen,
unbekannte Felder sowie unbekannte Versionen zurueck. Overrides sind explizite
Nutzerhinweise und werden in Berichten als solche gekennzeichnet.

Function- und Jump-Ziele verwenden dieselbe committed-Code-Pruefung wie die
automatische Analyse. Fehler nennen Override-Datei, Zeile, Adresse und einen
stabilen Grund. Ein Override fuer eine noch nicht entdeckte Dispatch-Stelle wird
waehrend laufender Fixpunktiterationen zurueckgestellt und erst am Fixpunkt als
ungueltig gemeldet. Dadurch bleibt das Ergebnis unabhaengig von der Dateireihenfolge.

## Bericht

Der Bericht trennt `Aufgeloest` und `Ungeloest` in stabiler Reihenfolge. Sichere
Ziele nennen ihren Beweisgrund. Jede ungeloeste Stelle nennt ihren Ablehnungsgrund
und zeigt die passende `jump`- oder `jump_table`-Zeile fuer eine Override-Datei.
Eine teilweise gueltige Jump Table bleibt vollstaendig im ungeloesten Abschnitt.
Tabellenzeilen nennen fuer jedes Ziel, ob der Dispatch ein Jump oder Call ist.
Eine durch eine Tabelle erklaerte Dispatch-Stelle wird nicht zusaetzlich als
gewoehnlicher ungeloester Registersprung ausgegeben.
