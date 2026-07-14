# Katana-IR

Katana-IR beschreibt die dekodierten SH-4-Operationen unabhaengig vom C++-Backend.
Die Metadaten einer Instruktion werden beim Lowering aus der Operation abgeleitet und
sind deshalb deterministisch.

## Operandbreiten

`OperandWidths` trennt sechs Arten von Breiten:

- `result`: Breite des semantisch erzeugten Werts
- `input`: Breite der nicht unmittelbar kodierten Eingangswerte
- `immediate`: Breite des kodierten Immediate-Felds
- `displacement`: Breite des kodierten Displacement-Felds
- `memory`: Breite des Speichertransfers
- `address`: Breite der effektiven Adresse oder des Kontrollflussziels

Mehrere Registereingaben einer aktuellen SH-4-Operation besitzen dieselbe `input`-
Breite. `None` bezeichnet einen nicht vorhandenen Operanden und darf nicht als Breite
null interpretiert werden. Die Namen `i1`, `i4`, `i8`, `i12`, `i16`, `i32` und `i64`
sind die stabilen Textnamen der Breiten.

Ein Byte-Load besitzt beispielsweise `result=i32`, `memory=i8` und `address=i32`.
Das Basisregister wird dabei nicht als Daten-Input missverstanden. Ein Vergleich
besitzt ein boolesches `result=i1` und 32-Bit-Eingaben; die Bindung dieses Ergebnisses
an Statusregister wird getrennt modelliert.
