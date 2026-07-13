# SH-4 T-Bit-Semantik fuer Integer-Arithmetik

Dieses Dokument beschreibt die von KatanaRecomp garantierte T-Bit-Semantik der Carry-, Borrow- und Overflow-Instruktionen.

## Grundregel

Das SH-4-T-Bit besitzt je nach Instruktion eine andere Bedeutung. Es darf nicht allgemein als Carry-Flag oder allgemein als Overflow-Flag behandelt werden.

## Instruktionen

| Instruktion | T als Eingabe | T als Ausgabe |
|---|---|---|
| `ADDC Rm,Rn` | Carry-In, Wert 0 oder 1 | unsigned Carry-Out |
| `SUBC Rm,Rn` | Borrow-In, Wert 0 oder 1 | unsigned Borrow-Out |
| `NEGC Rm,Rn` | Borrow-In, Wert 0 oder 1 | unsigned Borrow-Out |
| `ADDV Rm,Rn` | ignoriert | signed Overflow |
| `SUBV Rm,Rn` | ignoriert | signed Overflow |

## ADDC

Operation:

```text
Rn = Rn + Rm + T
```

Das Ergebnis wird auf 32 Bit gekuerzt.

T wird gesetzt, wenn das unsigned Zwischenergebnis groesser als `0xFFFFFFFF` ist.

Grenzfall:

```text
0xFFFFFFFF + 0 + 1 = 0x00000000, T = 1
```

## SUBC

Operation:

```text
Rn = Rn - Rm - T
```

Das Ergebnis wird auf 32 Bit gekuerzt.

T wird gesetzt, wenn der unsigned Minuend kleiner als `Rm + Borrow-In` ist.

Grenzfall:

```text
0 - 0 - 1 = 0xFFFFFFFF, T = 1
```

## NEGC

Operation:

```text
Rn = 0 - Rm - T
```

T wird gesetzt, wenn fuer die Negation ein Borrow entsteht. Das ist der Fall, wenn `Rm + Borrow-In` ungleich null ist.

Grenzfaelle:

```text
0 - 1 - 0 = 0xFFFFFFFF, T = 1
0 - 0 - 0 = 0x00000000, T = 0
```

## ADDV

Operation:

```text
Rn = Rn + Rm
```

T wird nur bei signed 32-Bit-Overflow gesetzt.

Grenzfall:

```text
0x7FFFFFFF + 1 = 0x80000000, T = 1
```

Ein unsigned Carry ohne signed Overflow ist fuer `ADDV` nicht relevant.

## SUBV

Operation:

```text
Rn = Rn - Rm
```

T wird nur bei signed 32-Bit-Overflow gesetzt.

Grenzfall:

```text
0x80000000 - 1 = 0x7FFFFFFF, T = 1
```

Ein unsigned Borrow ohne signed Overflow ist fuer `SUBV` nicht relevant.

## Implementierungsregeln

- Alle Registerergebnisse verwenden wohldefiniertes unsigned 32-Bit-Wraparound.
- Signed Overflow wird ohne signed C++-Overflow erkannt.
- `ADDC`, `SUBC` und `NEGC` lesen das alte T-Bit, bevor sie das neue T-Bit schreiben.
- Quellregister werden nicht veraendert.
- Die Semantik wird durch Decoder-, IR-, Codegen- und End-to-End-Tests abgesichert.