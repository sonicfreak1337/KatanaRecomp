# Executable Images

## Modell

`katana::io::ExecutableImage` beschreibt eine geladene Programmeingabe unabhaengig vom Dateiformat. Ein Image besitzt eine Quelldatei, sortierte Segmente und bekannte Einstiegspunkte.

Jedes `ImageSegment` speichert:

- stabilen Namen
- virtuelle 32-Bit-Basisadresse
- 64-Bit-Dateioffset
- Speicher- und Dateigroesse
- committed Dateibytes
- Klassifikation als `Code`, `Data` oder `Unknown`
- Read-, Write- und Execute-Berechtigungen

`memory_size` darf groesser als die Zahl der Dateibytes sein. Dieser hintere Bereich repraesentiert beispielsweise zero-initialized Speicher, besitzt aber bewusst keinen Dateioffset innerhalb von `bytes`.

## Invarianten

- Segmente sind nicht leer und besitzen einen Namen.
- Dateidaten passen vollstaendig in die Speichergroesse.
- Virtuelle Bereiche ueberschreiten den 32-Bit-Adressraum nicht.
- Dateioffset plus Dateigroesse laeuft nicht ueber.
- Segmente eines Images ueberlappen sich nicht.
- Segment- und Einstiegspunktreihenfolge ist deterministisch.

KR-1601 fuehrt nur das neutrale Modell ein. Raw- und ELF-Loader folgen in KR-1602 und KR-1603; die bestehenden flachen Analyse-APIs bleiben bis zu ihrer geplanten Migration kompatibel.
