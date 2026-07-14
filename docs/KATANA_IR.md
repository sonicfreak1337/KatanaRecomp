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

## Statusregistereffekte

`StatusRegisterEffects` trennt gelesene und geschriebene SR-Bestandteile. Die
Bitmaske unterscheidet T, S, Q und M. `Full` markiert einen Zugriff auf das gesamte
Statusregister, beispielsweise fuer `STC SR`, `LDC SR`, `TRAPA` und `RTE`.

Die Effekte beschreiben die bereits implementierte Semantik explizit. So liest und
schreibt `ADDC` das T-Bit, ein Vergleich schreibt T ohne es zu lesen und `DIV1`
liest T, Q und M, schreibt aber nur T und Q. Statusneutrale Instruktionen tragen
leere Masken. Bei den generischen Spezialregistertransfers wird der Effekt anhand
des konkreten Spezialregisteroperanden bestimmt.

## Speicher-Seiteneffekte

`MemoryEffects` kennzeichnet Reads und Writes, die Transferbreite und die Anzahl
der Zugriffe. Zusaetzlich beschreibt `AddressUpdateKind`, ob Adressregister vor dem
Zugriff dekrementiert oder danach inkrementiert werden und wie viele Registerupdates
stattfinden.

Ein normaler Load ist damit ein einzelner Read ohne Registerupdate. Ein
Pre-Decrement-Store schreibt nach vorheriger Adressaenderung. `MAC.W` und `MAC.L`
besitzen jeweils zwei Reads und zwei Post-Increment-Updates. Die Transferbreite muss
mit `OperandWidths.memory` uebereinstimmen; diese Beziehung wird vom IR-Verifier
geprueft.

## Delay Slots

`DelaySlotRelation` ersetzt unabhaengige Wahrheitswerte durch drei eindeutige Rollen:
`None`, `Owner` und `Slot`. Owner und Slot nennen jeweils die Adresse ihres
Gegenparts. Der C++-Codegenerator akzeptiert eine Delay-Slot-Instruktion nur dann
als Teil des Kontrolltransfers, wenn beide Rollen und beide Adressen zueinander
passen.

Damit ist die Ausfuehrungsreihenfolge nicht mehr aus zwei lose gekoppelten Markern
zu erraten. Fehlende, verwaiste oder widerspruechliche Beziehungen koennen vor dem
Codegenerator vom IR-Verifier abgelehnt werden.
