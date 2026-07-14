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

## Symbole und Map-Dateien

Images speichern benannte Funktions-, Objekt- oder unbekannte Symbole mit 32-Bit-Adresse, optionaler Groesse und Local-/Global-/Weak-Bindung. ELF-Loader lesen definierte Eintraege aus `SHT_SYMTAB` und `SHT_DYNSYM` samt verknuepfter `SHT_STRTAB`.

Optionale Katana-Map-Dateien verwenden pro Zeile das deterministische Format:

```text
ADDRESS KIND NAME [SIZE]
```

Adressen und Groessen sind hexadezimal. `KIND` ist `FUNC`, `OBJECT` oder `UNKNOWN`; die Kurzformen `F`, `O` und `U` sind ebenfalls erlaubt. Leerzeilen und mit `#` beginnende Kommentare werden ignoriert. Parserfehler nennen Datei und Zeile, doppelte Namen sind ungueltig.

## Relocations

Das Image-Modell speichert Relocation-Adresse, rohen ABI-Typ, normalisierte Art,
Symbol, impliziten Addend und ein optionales Anwendungsergebnis. Der ELF32-SH-
Loader liest `SHT_REL` und unterstuetzt zunaechst die beiden 32-Bit-Typen:

- `R_SH_DIR32` (1): `S + A`
- `R_SH_REL32` (2): `S + A - P`

`S` ist die Symboladresse, `A` der Little-Endian-Addend am Ziel und `P` die
Relocation-Adresse. Ergebnisse werden mit 32-Bit-Wraparound in die geladenen
Segmentdaten geschrieben. Unbekannte Typen bleiben als `Unsupported` im Image
sichtbar und veraendern keine Bytes. Ungeloeste Symbole, ungueltige Tabellen und
Ziele ausserhalb committed Segmentdaten schlagen mit Datei, Offset und Ursache fehl.

## Projektmanifest

`parse_project_manifest` liest das strikt versionierte Manifest-v1-Schema;
`load_project_manifest` waehlt Raw- oder ELF32-SH-Loader, wendet das deklarierte
Adresslayout an und laedt optional eine Symbol-Map. Relative Eingabe- und
Map-Pfade beziehen sich immer auf das Manifestverzeichnis. Das vollstaendige
Format steht in `docs/PROJECT_MANIFEST.md`.
