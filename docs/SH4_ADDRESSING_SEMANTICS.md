# SH-4 Adressierungssemantik

Dieses Dokument beschreibt die von KatanaRecomp implementierten SH-4-Adressierungsarten. KR-1401 fuehrt Pre-Decrement-Stores und Post-Increment-Loads fuer Byte, Word und Long ein.

## Pre-Decrement-Stores

Unterstuetzte Instruktionen:

```text
MOV.B Rm,@-Rn
MOV.W Rm,@-Rn
MOV.L Rm,@-Rn
```

Die effektive Adresse wird vor dem Speicherzugriff um die Operandengroesse verringert:

```text
Byte: address = Rn - 1
Word: address = Rn - 2
Long: address = Rn - 4
```

Danach wird der alte Wert aus Rm an die neue Adresse geschrieben und Rn auf diese Adresse gesetzt.

```text
value = Rm
address = Rn - width
memory[address] = low_width_bits(value)
Rn = address
```

Die Registerarithmetik verwendet `std::uint32_t` und besitzt daher definiertes Modulo-2-hoch-32-Verhalten.

### Identisches Quell- und Zielregister

Wenn Rm und Rn dasselbe Register sind, wird der Wert des Registers vor der Verringerung gespeichert.

Beispiel:

```text
R13 = 0x00000210
MOV.B R13,@-R13

Speicher[0x0000020F] = 0x10
R13 = 0x0000020F
```

KatanaRecomp sichert deshalb den Quellwert, bevor das Adressregister aktualisiert wird.

## Post-Increment-Loads

Unterstuetzte Instruktionen:

```text
MOV.B @Rm+,Rn
MOV.W @Rm+,Rn
MOV.L @Rm+,Rn
```

Der Speicherwert wird zuerst von der alten Adresse in Rm gelesen:

```text
address = Rm
Rn = memory[address]
```

Byte- und Word-Loads werden wie bei den direkten SH-4-Loads vorzeichenerweitert. Long-Loads uebernehmen alle 32 Bit unveraendert.

Wenn Rm und Rn verschieden sind, wird Rm anschliessend fortgeschaltet:

```text
Byte: Rm = address + 1
Word: Rm = address + 2
Long: Rm = address + 4
```

### Identisches Quell- und Zielregister

Wenn Rm und Rn dasselbe Register sind, findet kein Post-Increment statt.

```text
MOV.B @R14+,R14
```

R14 enthaelt danach ausschliesslich den geladenen und vorzeichenerweiterten Wert. Die urspruengliche Adresse wird nicht mehr benoetigt und darf nicht nachtraeglich erhoeht werden.

Diese Regel ist fuer alle drei Breiten identisch.

## Register-Displacements

KR-1402 unterstuetzt die sechs registerrelativen Displacement-Formen:

```text
MOV.B R0,@(disp,Rn)
MOV.W R0,@(disp,Rn)
MOV.L Rm,@(disp,Rn)
MOV.B @(disp,Rm),R0
MOV.W @(disp,Rm),R0
MOV.L @(disp,Rm),Rn
```

Das Opcodefeld enthaelt ein unsigned 4-Bit-Displacement. KatanaRecomp skaliert es bereits im Decoder auf ein Byte-Displacement:

| Breite | Skalierung | Byte-Bereich |
|---|---:|---:|
| Byte | 1 | 0 bis 15 |
| Word | 2 | 0 bis 30 |
| Long | 4 | 0 bis 60 |

Die effektive Adresse wird danach ohne weitere Opcodekenntnis gebildet:

```text
address = base_register + scaled_displacement
```

Die Addition verwendet `std::uint32_t` und besitzt definiertes Modulo-2-hoch-32-Verhalten. Die Basisregister bleiben unveraendert. Byte- und Word-Loads werden auf 32 Bit vorzeichenerweitert; Long-Loads uebernehmen alle Bits unveraendert.

Bei Byte- und Word-Stores ist R0 fest als Quellregister kodiert. Bei Byte- und Word-Loads ist R0 fest als Zielregister kodiert. Die Long-Formen erlauben unabhaengige allgemeine Quell-, Basis- und Zielregister.

Wenn die berechnete Adresse ausserhalb des begrenzten Runtime-Speichers liegt, schlaegt der Zugriff sichtbar fehl. Ein fehlgeschlagener Load veraendert das Zielregister nicht.

Referenzgrundlage ist das [Renesas SH-4A Software Manual](https://www.renesas.com/en/document/mas/sh-4a-software-manual), insbesondere die MOV-Strukturdatenformen und ihre Displacement-Skalierung.

## R0-indexierte Adressierung

KR-1403 unterstuetzt die sechs R0-indexierten Formen:

```text
MOV.B Rm,@(R0,Rn)
MOV.W Rm,@(R0,Rn)
MOV.L Rm,@(R0,Rn)
MOV.B @(R0,Rm),Rn
MOV.W @(R0,Rm),Rn
MOV.L @(R0,Rm),Rn
```

Die effektive Adresse ist fuer alle Breiten identisch:

```text
address = R0 + base_register
```

Die Addition verwendet `std::uint32_t` und damit definiertes Modulo-2-hoch-32-Verhalten. Anders als bei Post-Increment werden weder R0 noch das Basisregister fortgeschaltet. Byte- und Word-Loads werden auf 32 Bit vorzeichenerweitert.

R0 kann zugleich Index und Datenregister eines Stores oder Index und Zielregister eines Loads sein. Die effektive Adresse und der zu speichernde Wert werden aus dem alten Registerzustand gelesen; ein Load schreibt sein Ziel erst nach einem erfolgreichen Speicherzugriff.

Ungueltige effektive Adressen schlagen sichtbar fehl, ohne die beteiligten Register vorzeitig zu veraendern.

## GBR-relative Adressierung

KR-1404 unterstuetzt die sechs GBR-relativen Formen:

```text
MOV.B R0,@(disp,GBR)
MOV.W R0,@(disp,GBR)
MOV.L R0,@(disp,GBR)
MOV.B @(disp,GBR),R0
MOV.W @(disp,GBR),R0
MOV.L @(disp,GBR),R0
```

Das Opcodefeld enthaelt ein unsigned 8-Bit-Displacement. Die Skalierung erfolgt bereits im Decoder:

| Breite | Skalierung | Byte-Bereich |
|---|---:|---:|
| Byte | 1 | 0 bis 255 |
| Word | 2 | 0 bis 510 |
| Long | 4 | 0 bis 1020 |

Die effektive Adresse lautet:

```text
address = GBR + scaled_displacement
```

GBR ist ein explizites `std::uint32_t`-Feld des generierten CPU-Zustands und bleibt bei allen Zugriffen unveraendert. Die Adressaddition besitzt definiertes Modulo-2-hoch-32-Verhalten. Stores lesen immer R0; Loads schreiben immer R0. Byte- und Word-Loads werden vorzeichenerweitert.

Ungueltige Runtime-Adressen schlagen vor einer Registeraenderung fehl.

## Speicherreihenfolge

KatanaRecomp modelliert die sichtbare Reihenfolge explizit:

### Pre-Decrement

1. alten Quellwert sichern
2. neue Adresse berechnen
3. Speicher schreiben
4. Adressregister aktualisieren

### Post-Increment

1. alte Adresse sichern
2. Speicher lesen
3. Zielregister schreiben
4. Adressregister nur bei unterschiedlichen Registern erhoehen

Dadurch bleiben auch identische Registerpaare deterministisch.

## Getestete Faelle

- Byte-, Word- und Long-Pre-Decrement
- Byte-, Word- und Long-Post-Increment
- Little-Endian-Speicherwerte
- Vorzeichenerweiterung bei Byte- und Word-Loads
- unveraenderte Quellregister bei verschiedenen Pre-Decrement-Registern
- identische Register bei allen drei Pre-Decrement-Breiten
- identische Register bei allen drei Post-Increment-Breiten
- kein Post-Increment bei identischen Registern
- Pre-Decrement-Wraparound auf eine ungueltige Runtime-Adresse
- ungueltige Post-Increment-Leseadresse
- keine vorzeitigen Registeraenderungen bei fehlgeschlagenen Zugriffen
- alle sechs Register-Displacement-Formen
- Displacements null und maximal
- Skalierung mit 1, 2 und 4
- unveraenderte Basisregister
- 32-Bit-Wraparound der effektiven Displacement-Adresse
- ungueltige Displacement-Adresse ohne vorzeitige Registeraenderung
- alle sechs R0-indexierten Byte-, Word- und Long-Formen
- R0 zugleich als Index und Store-Quelle
- R0 zugleich als Index und Load-Ziel
- 32-Bit-Wraparound der R0-indexierten Adresse
- ungueltige R0-indexierte Adresse ohne Registeraenderung
- alle sechs GBR-relativen Byte-, Word- und Long-Formen
- unsigned 8-Bit-Displacements null und maximal
- GBR-Skalierung mit 1, 2 und 4
- unveraendertes GBR
- 32-Bit-Wraparound der GBR-relativen Adresse
- ungueltige GBR-Adresse ohne Registeraenderung
- unveraenderte Status- und MAC-Register
- direkte generierte Funktionsaufrufe
- kompletter BSR-Aufrufspfad
