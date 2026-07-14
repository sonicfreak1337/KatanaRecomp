# Katana-IR

Katana-IR beschreibt die dekodierten SH-4-Operationen unabhaengig vom C++-Backend.
Die Metadaten einer Instruktion werden beim Lowering aus Operation und Operanden
abgeleitet und sind deshalb deterministisch.

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

`MAC.W` und `MAC.L` besitzen kein pauschales 64-Bit-Ergebnis. Ihre Akkumulator-
Metadaten nennen stattdessen getrennt, welche Teile von MACH/MACL bei geloeschtem
oder gesetztem S-Bit geschrieben werden. Bei gesetztem S schreibt `MAC.W` nur MACL,
waehrend `MAC.L` weiterhin MACH und MACL schreibt.

## Statusregistereffekte

`StatusRegisterEffects` trennt gelesene und geschriebene SR-Bestandteile. Die
Bitmaske unterscheidet T, S, Q und M. `Full` markiert einen Zugriff auf das gesamte
Statusregister, beispielsweise fuer `STC SR`, `LDC SR`, `TRAPA` und `RTE`.

Eine `Full`-Maske beantwortet Abfragen nach T, S, Q und M ebenfalls positiv.

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

Bei `MOV.B/W/L @Rm+,Rn` werden Operation und konkrete Register gemeinsam
ausgewertet. Falls `Rm == Rn` ist, findet gemaess SH-4-Semantik kein
Post-Increment-Registerupdate statt; andernfalls wird genau ein Update ausgewiesen.

## Delay Slots

`DelaySlotRelation` ersetzt unabhaengige Wahrheitswerte durch drei eindeutige Rollen:
`None`, `Owner` und `Slot`. Owner und Slot nennen jeweils die Adresse ihres
Gegenparts. Der C++-Codegenerator akzeptiert eine Delay-Slot-Instruktion nur dann
als Teil des Kontrolltransfers, wenn beide Rollen und beide Adressen zueinander
passen.

Der Verifier gleicht die Owner-Rolle zusaetzlich mit den Decoder-Metadaten des
Originalopcodes ab. Ein Paar muss exakt aus Owner-Adresse und Owner-Adresse plus
zwei bestehen. Nicht verzoegerte Owner, verzoegerte Opcodes ohne Owner, verwaiste
Slots und Kontrollflussopcodes im Slot sind ungueltig.

Damit ist die Ausfuehrungsreihenfolge nicht mehr aus zwei lose gekoppelten Markern
zu erraten. Fehlende, verwaiste oder widerspruechliche Beziehungen koennen vor dem
Codegenerator vom IR-Verifier abgelehnt werden.

## Verifikation

`verify_function` prueft jede Funktion unabhaengig und liefert deterministisch nach
Adresse und Meldung sortierte Diagnosen. Geprueft werden mindestens:

- vorhandene und eindeutige Bloecke und Instruktionsadressen
- ein zum Funktionseintritt passender Startblock
- ausgerichtete Instruktionen und Register aus R0 bis R15
- kanonische Operandbreiten sowie Status-, Speicher- und Akkumulatoreffekte
- vollstaendige direkte Kontrollflussziele und vorhandene Blocknachfolger
- gegenseitig konsistente Delay-Slot-Beziehungen und Blockterminale

`require_valid_function` verdichtet die erste Diagnose zu einem Fehler mit Funktions-
und Instruktionsadresse. `emit_cpp_program` ruft diese Pruefung fuer jede Funktion
auf, bevor irgendein C++-Text erzeugt wird. Tests, die IR absichtlich von Hand
aufbauen, muessen daher dieselben expliziten Metadaten wie das normale Lowering
setzen.

## Deterministische Ausgabe

`emit_ir_text` und `emit_ir_json` verifizieren jede Funktion vor der Ausgabe und
sortieren Funktionen, Bloecke, Nachfolger und Aufruflisten nach Adresse. Doppelte
Funktionseinstiege werden abgelehnt. Instruktionen erscheinen in Adressreihenfolge.

Beide Formate enthalten Operandbreiten, konkrete Operanden, Status-, Speicher- und
Akkumulatoreffekte, Delay-Slot-Beziehungen, Privilegstatus und Kontrollflussdaten.
Die JSON-Ausgabe verwendet das Schema `katana-ir-v2`, hexadezimale Adressen als
Strings und feste englische Feld- und Enum-Namen. `katana-recomp ir` gibt das
Textformat aus; `katana-recomp ir-json` liefert JSON ohne dateipfadabhaengige
Kopfzeilen.
