# SH-4 Shift-Semantik

Dieses Dokument beschreibt die von KatanaRecomp implementierte Semantik der SH-4-Shift-Instruktionen.

## Ein-Bit-Shifts

- `SHLL` und `SHAL` verschieben um ein Bit nach links.
- Das alte Bit 31 wird nach T uebernommen.
- `SHLR` verschiebt logisch nach rechts.
- `SHAR` verschiebt arithmetisch nach rechts und erhaelt das Vorzeichenbit.
- Das alte Bit 0 wird bei den Rechtsshifts nach T uebernommen.

## Feste Mehrfach-Shifts

- `SHLL2`, `SHLL8` und `SHLL16` verschieben logisch nach links.
- `SHLR2`, `SHLR8` und `SHLR16` verschieben logisch nach rechts.
- Diese Instruktionen veraendern T nicht.

## Rotationen

- `ROTL` und `ROTR` rotieren innerhalb des 32-Bit-Registers.
- Das herausrotierte Bit wird nach T uebernommen.
- `ROTCL` und `ROTCR` verwenden das alte T-Bit als zusaetzliches Eingangsbit.
- Das aus dem Register herausrotierte Bit wird zum neuen T-Bit.

## Dynamische Shifts

`SHAD Rm,Rn` und `SHLD Rm,Rn` verwenden den Wert aus Rm als Shiftzaehler und veraendern T nicht.

### Nichtnegative Zaehler

Wenn Bit 31 von Rm null ist:

```text
amount = Rm & 31
Rn = Rn << amount
```

SHAD und SHLD besitzen fuer nichtnegative Zaehler dieselbe Linksshift-Semantik. Nur die unteren fuenf Bits bestimmen die Distanz. Ein Wert von 32 oder 64 wirkt daher wie ein Linksshift um null Bit.

### Negative Zaehler

Wenn Bit 31 von Rm gesetzt ist:

```text
amount = ((~Rm) & 31) + 1
```

Damit liegt die Rechtsshiftdistanz zwischen 1 und 32.

`SHLD` verschiebt logisch nach rechts. Bei `amount == 32` wird Rn auf null gesetzt.

`SHAD` verschiebt arithmetisch nach rechts. Bei `amount == 32` wird Rn vollstaendig mit dem vorherigen Vorzeichenbit gefuellt:

```text
negativer Ausgangswert -> 0xFFFFFFFF
nichtnegativer Ausgangswert -> 0x00000000
```

KatanaRecomp verwendet fuer die arithmetische Rechtsschiebung ausschliesslich definierte unsigned Bitoperationen. Das Ergebnis haengt daher nicht vom Verhalten eines C++-Compilers bei signed Rechtsshifts ab.

## Getestete Grenzfaelle

- positive Zaehler
- negative Zaehler
- Zaehler groesser als 31
- negative Vielfache von 32
- positive und negative Ausgangswerte fuer SHAD
- unveraendertes T-Bit
- unveraenderte Quellregister
