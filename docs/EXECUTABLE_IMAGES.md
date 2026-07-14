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

## Raw-Binary-Loader

`load_raw_binary` bildet eine vollstaendige Datei auf genau ein Segment ab. Basisadresse, Name, Klassifikation, Berechtigungen und optionaler Einstiegspunkt werden ueber `RawBinaryLoadOptions` festgelegt. Das Segment beginnt immer bei Dateioffset null; Datei- und Speichergroesse sind identisch.

Leere Dateien, nicht lesbare Eingaben und Abbildungen ausserhalb des 32-Bit-Adressraums schlagen sichtbar mit Pfad, Offset oder Ursache fehl.

KR-1601 fuehrt das neutrale Modell ein, KR-1602 den Raw-Loader. Der ELF-Loader folgt in KR-1603; die bestehenden flachen Analyse-APIs bleiben bis zu ihrer geplanten Migration kompatibel.
