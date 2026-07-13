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
- unveraenderte Status- und MAC-Register
- direkte generierte Funktionsaufrufe
- kompletter BSR-Aufrufspfad
