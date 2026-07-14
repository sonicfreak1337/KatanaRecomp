# Projektmanifest Version 1

Ein KatanaRecomp-Projektmanifest beschreibt Eingabedatei, Dateiformat und
Adresslayout unabhaengig vom aktuellen Arbeitsverzeichnis. Es ist eine
UTF-8-Textdatei mit genau einem `KEY = VALUE` pro Zeile. Leerzeilen und Zeilen,
die nach optionalem Leerraum mit `#` beginnen, werden ignoriert.

## Pflichtfelder

- `version = 1`
- `format = raw` oder `format = elf32-sh`
- `input = PFAD`
- `base_address = HEX` fuer `raw`

## Optionale Felder

- `entry_point = HEX`
- `map = PFAD`
- `segment_name = NAME` fuer `raw`
- `segment_kind = code`, `data` oder `unknown` fuer `raw`
- `permissions = rwx`, wobei `-` ein fehlendes Recht markiert, fuer `raw`

Relative `input`- und `map`-Pfade werden relativ zum Manifestverzeichnis
aufgeloest. Adressen sind hexadezimal und duerfen mit `0x` beginnen.
Unbekannte oder doppelte Felder, nicht unterstuetzte Versionen und Raw-Felder
in ELF-Projekten sind Fehler mit Datei- und Zeilenangabe.

## Raw-Beispiel

```text
version = 1
format = raw
input = program.bin
base_address = 0x8C010000
entry_point = 0x8C010000
segment_name = .text
segment_kind = code
permissions = r-x
map = program.map
```

## ELF32-SH-Beispiel

```text
version = 1
format = elf32-sh
input = program.elf
map = additional-symbols.map
```

ELF-Segmente, Berechtigungen, Einstiegspunkt, Symbole und Relocations stammen
aus der ELF-Datei. Ein optionaler `entry_point` fuegt einen weiteren bekannten
Einstiegspunkt hinzu; eine Map-Datei ergaenzt die Symbole.
