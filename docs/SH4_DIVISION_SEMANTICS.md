# SH-4 Divisionssemantik

Dieses Dokument beschreibt die von KatanaRecomp implementierte Semantik von `DIV0U`, `DIV0S` und `DIV1`.

## CPU-Zustand

KR-1304 erweitert den generierten CPU-Zustand um die booleschen Statusbits Q und M. Zusammen mit T steuern sie den iterativen Divisionsalgorithmus des SH-4.

```text
Q: aktueller Quotienten- und Zwischenschrittzustand
M: Vorzeichen beziehungsweise Modus des Divisors
T: Eingangsbit fuer den Shift und Ergebnisvergleich Q gleich M
```

Die Instruktionen veraendern S, MACH und MACL nicht.

## DIV0U

`DIV0U` initialisiert eine vorzeichenlose Division:

```text
Q = 0
M = 0
T = 0
```

Allgemeine Register bleiben unveraendert.

## DIV0S Rm,Rn

`DIV0S` initialisiert eine vorzeichenbehaftete Division anhand der Vorzeichenbits:

```text
Q = Rn[31]
M = Rm[31]
T = M XOR Q
```

Rm und Rn bleiben unveraendert.

## DIV1 Rm,Rn

`DIV1` ist genau ein Hardware-Divisionsschritt und keine vollstaendige Division.

Zuerst werden der alte Q-Zustand und das Vorzeichenbit von Rn erfasst. Danach wird Rn links verschoben und das alte T-Bit als niederwertigstes Bit eingefuegt:

```text
old_Q = Q
Q = Rn[31]
Rn = (Rn << 1) | T
shifted = Rn
```

Anschliessend wird abhaengig von `old_Q` und M addiert oder subtrahiert:

```text
old_Q=0, M=0: Rn = shifted - Rm
old_Q=0, M=1: Rn = shifted + Rm
old_Q=1, M=0: Rn = shifted + Rm
old_Q=1, M=1: Rn = shifted - Rm
```

Carry beziehungsweise Borrow werden mit vorzeichenlosen 32-Bit-Vergleichen bestimmt. Q wird danach gemaess der SH-4-Zustandstabelle mit dem Carry- oder Borrow-Ergebnis kombiniert.

Zum Abschluss gilt:

```text
T = (Q == M)
```

M bleibt waehrend `DIV1` unveraendert. Rm bleibt ebenfalls unveraendert.

## Definierte C++-Semantik

Alle Registerrechnungen verwenden `std::uint32_t`. Linksverschiebung, Addition und Subtraktion besitzen dadurch definiertes Modulo-2-hoch-32-Verhalten.

Boolesches XOR wird explizit mit Ungleichheit ausgedrueckt:

```text
a XOR b entspricht a != b
NOT a XOR b entspricht (!a) != b
```

Dadurch ist die Operatorrangfolge sichtbar und nicht von schwer lesbaren Ausdruecken wie `!Q ^ carry` abhaengig.

## Getestete Referenzvektoren

Die End-to-End-Tests decken ab:

- `DIV0U` mit zuvor gesetzten Q-, M- und T-Bits
- alle Vorzeichenkombinationen von `DIV0S`
- alle vier Kombinationen aus altem Q und M fuer `DIV1`
- gesetztes und geloeschtes Eingangs-T
- gesetztes und geloeschtes Vorzeichenbit von Rn
- Additions-Carry
- Subtraktions-Borrow und Nicht-Borrow
- unveraendertes M
- unveraenderter Divisor Rm
- unveraendertes S, MACH und MACL
- kompletter generierter Aufrufspfad
