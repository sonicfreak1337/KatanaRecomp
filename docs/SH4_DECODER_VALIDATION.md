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
