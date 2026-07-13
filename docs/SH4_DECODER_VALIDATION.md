# SH-4-Decoder-Validierung

## Primaerquelle

Die unabhaengigen Vektoren in `tests/decoder/sh4_specification_vectors.cpp` wurden manuell gegen das offizielle Renesas-Handbuch "SH-4 Software Manual" abgeleitet:

https://www.renesas.com/en/document/mas/sh-4-software-manual

Die Vektoren sind absichtlich nicht aus `instruction_metadata.cpp` generiert. Sie pruefen feste 16-Bit-Kodierungen und erwartete Operanden fuer Register-, Immediate-, Sprung-, Speicher-, PC/GBR-relative und Systemregisterformate.

## Gepruefte Grenzen

- Immediate-Grenzen `-128`, `-1` und `127`
- 12-Bit-Sprungdisplacements `4094` und `-4096`
- negatives 8-Bit-Sprungdisplacement
- niedrigste und hoechste allgemeine Registernummern
- maximale 4- und 8-Bit-Speicherdisplacements mit Skalierung
- banked, privilegierte und nicht privilegierte Systemregistertransfers
- reservierte beziehungsweise noch nicht implementierte Kodierungen bleiben `Unknown`

Die Tests validieren Decoderergebnisse. Sie beanspruchen keine vollstaendige SH-4-Konformitaet fuer noch nicht implementierte Instruktionen.

## Reproduzierbarer Mutationsfuzzer

`katana-decoder-fuzzer [Seed] [Iterationen]` beginnt mit jeder implementierten Opcode-Regel sowie bekannten Unknown-Faellen. Pro Iteration wird ein einzelnes Bit gekippt, ein kompletter Zufallswert eingesetzt oder ein Zufallsmuster mit dem Korpus kombiniert.

Der CTest-Lauf verwendet den festen Seed `0x00C0FFEE` und 200.000 Iterationen. Ein Fehler meldet den konkreten Opcode; derselbe Lauf kann damit exakt wiederholt werden. Geprueft werden:

- bitidentische Decoderergebnisse bei Wiederholung
- genau eine Metadatenregel je bekanntem Opcode
- keine Metadatenregel je unbekanntem Opcode
- Uebereinstimmung von Metadaten- und Decoder-Kind
- Registeroperanden nur im Bereich R0 bis R15
- nicht leere Disassembly fuer bekannte und unbekannte Werte
