# SH-4 Multiplikationssemantik

Dieses Dokument beschreibt die von KatanaRecomp implementierte Semantik der einfachen SH-4-Multiplikationsinstruktionen.

## CPU-Zustand

KR-1301 erweitert den generierten CPU-Zustand um das 32-Bit-Register `MACL`.

```text
MACL: unteres Multiplikations- und Akkumulatorregister
```

Die Instruktionen dieses Tasks veraendern weder die allgemeinen Quellregister noch das T-Bit.

## MUL.L Rm,Rn

`MUL.L` multipliziert die beiden vollstaendigen 32-Bit-Registerwerte. Nur die unteren 32 Bit des 64-Bit-Produkts werden nach MACL geschrieben.

```text
product = uint64(Rn) * uint64(Rm)
MACL = product & 0xFFFFFFFF
```

Fuer die unteren 32 Produktbits ist die signed oder unsigned Interpretation der Operanden identisch. KatanaRecomp berechnet deshalb ein unsigned 64-Bit-Zwischenergebnis und schneidet es definiert auf 32 Bit ab.

## MULS.W Rm,Rn

`MULS.W` verwendet nur die unteren 16 Bit beider Register und interpretiert sie als vorzeichenbehaftete Zweierkomplementwerte.

```text
source = sign_extend_16(Rm)
destination = sign_extend_16(Rn)
MACL = uint32(source * destination)
```

Die Vorzeichenerweiterung wird mit expliziten 32-Bit-Operationen ausgefuehrt. Dadurch haengt das Ergebnis nicht von implementation-defined Konvertierungen zwischen unsigned und signed 16-Bit-Typen ab.

Der Produktbereich passt vollstaendig in ein vorzeichenbehaftetes 32-Bit-Ergebnis.

## MULU.W Rm,Rn

`MULU.W` verwendet nur die unteren 16 Bit beider Register und interpretiert sie als vorzeichenlose Werte.

```text
MACL = (Rn & 0xFFFF) * (Rm & 0xFFFF)
```

Das groesste Ergebnis ist:

```text
65535 * 65535 = 0xFFFE0001
```

Es passt vollstaendig in MACL.

## Doppelte 32-Bit-Multiplikation

KR-1302 erweitert den generierten CPU-Zustand um das 32-Bit-Register `MACH`. Zusammen bilden die beiden Register das 64-Bit-Ergebnis:

```text
MACH:MACL
obere 32 Bit : untere 32 Bit
```

### DMULU.L Rm,Rn

`DMULU.L` multipliziert beide Register als vorzeichenlose 32-Bit-Werte.

```text
product = uint64(Rn) * uint64(Rm)
MACH = product >> 32
MACL = product & 0xFFFFFFFF
```

### DMULS.L Rm,Rn

`DMULS.L` interpretiert beide Register als vorzeichenbehaftete 32-Bit-Zweierkomplementwerte. Die 32-Bit-Vorzeichenerweiterung erfolgt explizit nach 64 Bit.

```text
source = sign_extend_32(Rm)
destination = sign_extend_32(Rn)
signed_product = source * destination
product_bits = uint64(signed_product)
MACH = product_bits >> 32
MACL = product_bits & 0xFFFFFFFF
```

Das signed 32-mal-32-Bit-Produkt passt vollstaendig in einen vorzeichenbehafteten 64-Bit-Wert. Die anschliessende Umwandlung nach `uint64_t` ist fuer negative Ergebnisse als Modulo-2-hoch-64-Konvertierung definiert. Dadurch bleiben die exakten Zweierkomplementbits fuer MACH und MACL erhalten.

`DMULS.L` und `DMULU.L` veraendern weder die allgemeinen Quellregister noch das T-Bit.
## Multiply-Accumulate

KR-1303 implementiert `MAC.W` und `MAC.L` einschliesslich Speicherzugriff, Post-Inkrement und S-Bit-Saettigung.

Der generierte CPU-Zustand besitzt dafuer ein boolesches `S`-Bit. `SETS` setzt dieses Bit, `CLRS` loescht es. Die MAC-Instruktionen veraendern T und S nicht.

### Adressierung und Registerfortschaltung

```text
MAC.W @Rm+,@Rn+:
    signed_word_m = memory16[Rm]
    signed_word_n = memory16[Rn]
    Rm += 2
    Rn += 2

MAC.L @Rm+,@Rn+:
    signed_long_m = memory32[Rm]
    signed_long_n = memory32[Rn]
    Rm += 4
    Rn += 4
```

Wenn Rm und Rn dasselbe Register bezeichnen, werden zwei aufeinanderfolgende Speicherwerte gelesen. Das Register wird zweimal fortgeschaltet:

```text
MAC.W gleiches Register: insgesamt +4
MAC.L gleiches Register: insgesamt +8
```

### S gleich 0

Ohne Saettigung wird das signed Produkt modulo 2 hoch 64 zu `MACH:MACL` addiert.

```text
accumulator = uint64(MACH:MACL)
result = accumulator + uint64(signed_product)
MACH = result[63:32]
MACL = result[31:0]
```

Die Addition wird mit unsigned 64-Bit-Operationen ausgefuehrt. Wraparound ist damit in C++ definiert.

### MAC.W mit S gleich 1

`MAC.W` saettigt das Ergebnis auf einen signed 32-Bit-Wert in MACL:

```text
Minimum: 0x80000000
Maximum: 0x7FFFFFFF
```

MACH bleibt in diesem Modus unveraendert.

### MAC.L mit S gleich 1

`MAC.L` saettigt auf einen signed 48-Bit-Wert:

```text
Minimum: MACH=0x00008000, MACL=0x00000000
Maximum: MACH=0x00007FFF, MACL=0xFFFFFFFF
```

Im Saettigungsmodus werden nur die unteren 16 Bit von MACH als oberer Teil des 48-Bit-Akkumulators verwendet. KatanaRecomp setzt die oberen 16 Bit von MACH nach der Operation kanonisch auf null.

### Getestete MAC-Grenzfaelle

- positive und negative signed Produkte
- 64-Bit-Wraparound ohne Saettigung
- positive und negative 32-Bit-Saettigung von MAC.W
- positive und negative 48-Bit-Saettigung von MAC.L
- negative nicht gesaettigte 48-Bit-Ergebnisse
- verschiedene Adressregister
- identische Adressregister
- unveraendertes T- und S-Bit
- `SETS` und `CLRS`
- kompletter generierter Aufrufspfad
## Getestete Grenzfaelle

- `DMULU.L` mit `0xFFFFFFFF * 0xFFFFFFFF`
- gesetztes Bit 63 im unsigned Produkt
- gemischtes 64-Bit-Produkt mit nichttrivialen High- und Low-Haelften
- `DMULS.L` mit `-1`, `INT32_MIN` und `INT32_MAX`
- negative und positive signed 64-Bit-Produkte
- korrekte Aufteilung nach MACH und MACL

- 32-Bit-Wraparound bei MUL.L
- negative und positive 16-Bit-Operanden bei MULS.W
- `-32768`, `-2`, `-1` und `32767`
- Maximalwerte `65535 * 65535` bei MULU.W
- ignorierte obere 16 Registerbits bei Word-Multiplikationen
- unveraenderte Quellregister
- unveraendertes T-Bit
- direkter Funktionsaufruf und kompletter generierter Aufrufspfad
