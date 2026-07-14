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

## ELF32-SH-Loader

`load_elf32_sh` akzeptiert ausfuehrbare ELF32-Dateien fuer `EM_SH` (42) in Little-Endian-Kodierung. Ladbare `PT_LOAD`-Program-Header werden als Segmente uebernommen; `p_vaddr`, `p_offset`, `p_filesz`, `p_memsz` und `PF_R/PF_W/PF_X` bleiben erhalten. `p_memsz - p_filesz` beschreibt den nicht in der Datei gespeicherten Zero-Fill-Bereich.

Der Loader prueft ELF-Identifikation, Klasse, Byte-Reihenfolge, Version, Typ, Maschine, Headergroessen, Tabellenbereiche, Segmentbereiche und Groessenrelationen. Fehler nennen Quelldatei, Dateioffset und Ursache.

Grundlage sind die System-V-ABI-Strukturen `Elf32_Ehdr` und `Elf32_Phdr`, `PT_LOAD` sowie die standardisierte Maschinenkennung `EM_SH`.
