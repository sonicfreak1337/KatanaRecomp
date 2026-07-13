# SH-4-System- und Kontrollsemantik

Dieses Dokument beschreibt die in v0.14.0 modellierten Systemregistertransfers und privilegierten Kontrollpfade.

## Systemregistertransfers

KR-1406 unterstuetzt direkte und speicherbasierte Formen von `STS`, `LDS`, `STC` und `LDC`.

Systemregister:

- MACH, MACL und PR
- FPUL und FPSCR

Kontrollregister:

- SR, GBR, VBR, SSR, SPC, SGR und DBR
- R0_BANK bis R7_BANK

SGR besitzt entsprechend der SH-4-ISA nur STC-Storeformen. Alle anderen aufgefuehrten Register besitzen die jeweils definierten Lade- und Storeformen.

## Direkte Formen

```text
STS system,Rn
LDS Rm,system
STC control,Rn
LDC Rm,control
```

Direkte Stores lesen das Spezialregister und schreiben den Wert nach Rn. Direkte Loads sichern zuerst den allgemeinen Quellregisterwert und schreiben ihn danach in das Spezialregister. Dadurch bleibt auch ein SR-Bankwechsel deterministisch.

## Speicherformen

```text
STS.L system,@-Rn
LDS.L @Rm+,system
STC.L control,@-Rn
LDC.L @Rm+,control
```

Alle Speicherformen uebertragen exakt 32 Bit in Little Endian. Pre-Decrement berechnet `Rn - 4`, fuehrt den Store aus und aktualisiert Rn erst nach einem erfolgreichen Zugriff. Post-Increment liest zuerst, aktualisiert das alte aktive Rm danach um vier und schreibt zuletzt das Spezialregister. Das ist insbesondere fuer `LDC.L @Rm+,SR` wichtig, weil SR.RB die aktive Registerbank wechseln kann.

Fehlgeschlagene Speicherzugriffe veraendern weder das Adressregister noch das Ziel-Spezialregister.

## SR und Registerbanken

LDC maskiert SR mit `0x700083F3`. T, S, Q und M bleiben mit den bereits explizit modellierten booleschen CPU-Feldern synchron. Aendert sich SR.RB, tauscht die Runtime R0 bis R7 mit R0_BANK bis R7_BANK. STC liest stets den daraus zusammengesetzten aktuellen SR-Wert.

## FPSCR

LDS maskiert FPSCR mit `0x003FFFFF`. FPUL wird als unveraendertes 32-Bit-Kommunikationsregister uebertragen. Die eigentliche FPU-Semantik folgt in einem spaeteren Meilenstein.

## Privilegstatus

Alle laut SH-4-ISA privilegierten STC/LDC-Formen tragen diese Eigenschaft explizit in Decoder und Katana-IR. GBR-Transfers sind nicht privilegiert. Da Ausnahmen und Betriebsmodi noch kein vollstaendiges Runtime-Subsystem besitzen, fuehrt der generierte Code eine markierte privilegierte Instruktion derzeit aus, statt eine Illegal-Instruction-Ausnahme zu dispatchen.

## Getestete Faelle

- alle 78 unterstuetzten Opcodecodierungen
- alle direkten lesbaren und schreibbaren Spezialregister
- SR- und FPSCR-Maskierung
- Synchronisierung von T, S, Q und M
- SR.RB-Bankwechsel
- Little-Endian-STS.L/LDS.L
- Pre-Decrement und Post-Increment
- unveraenderter Zustand bei ungueltigen Speicheradressen
- korrekte Privilegmarkierung mit GBR-Ausnahme
