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

LDS maskiert FPSCR mit `0x003FFFFF`. FPUL wird als unveraendertes
32-Bit-Kommunikationsregister uebertragen. Seit v0.24.0 wechselt `FPSCR.FR`
zentral die sichtbaren FR-/XF-Baenke; `PR`, `SZ` und `RM` steuern Praezision,
Transferbreite und Rundung. Bei gesetztem `SR.FD` nehmen auch direkte und
speicherbasierte FPUL-/FPSCR-Transfers vor einer Teilwirkung die strukturierte
FPU-Disable-Ausnahme.

## Privilegstatus

Alle laut SH-4-ISA privilegierten STC/LDC-Formen tragen diese Eigenschaft
explizit in Decoder und Katana-IR. GBR-Transfers sind nicht privilegiert. Seit
KR-4503 prueft der generierte Code `SR.MD` vor der ersten Teilwirkung. Im
User-Modus wird eine strukturierte Illegal-Instruction-Ausnahme mit dem
verursachenden Gast-PC dispatcht; Register und Speicher bleiben unveraendert.

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
- erfolgreiche Supervisor-Ausfuehrung und Ablehnung im User-Modus

## Privilegierte Kontrollpfade

KR-1407 modelliert `TRAPA`, `RTE` und `SLEEP` als terminale Runtime-Pfade.

### TRAPA

`TRAPA #imm` fuehrt folgende sichtbare Zustandsaenderungen aus:

1. `TRA = imm << 2`
2. aktuelles SR nach SSR sichern
3. Instruktions-PC plus zwei nach SPC sichern
4. R15 nach SGR sichern
5. `EXPEVT = 0x160`
6. SR.MD, SR.BL und SR.RB setzen
7. `PC = VBR + 0x100`
8. `trap_pending = true`

Der SR.RB-Wechsel aktiviert dabei die alternative Registerbank. Der generierte Funktionsaufruf kehrt anschließend zum Runtime-Dispatcher zurueck, weil der VBR-Inhalt erst zur Laufzeit bekannt ist.

### RTE

`RTE` ist privilegiert und besitzt einen Delay Slot. SPC wird vor dem Delay Slot als Ruecksprungziel gesichert. SSR wird bereits vor Ausfuehrung des Delay Slots nach SR uebernommen; dadurch sieht der Delay Slot die wiederhergestellten Statusbits und die wiederhergestellte Registerbank. Danach werden PC auf das gesicherte SPC gesetzt und `trap_pending` geloescht.

### SLEEP

`SLEEP` setzt `sleeping = true`, setzt PC auf die Folgeinstruktion und kehrt zum Runtime-Dispatcher zurueck. Alle anderen CPU-Felder bleiben erhalten. Ein spaeteres Interrupt- und Scheduler-Subsystem ist fuer das Aufwecken verantwortlich; KR-1407 simuliert keinen Plattforminterrupt.

Die Privilegmarkierung ist in Decoder, IR und CLI sichtbar. `RTE` und `SLEEP`
pruefen seit KR-4503 vor dem Delay Slot beziehungsweise vor dem Setzen des
Schlafzustands `SR.MD`. Eine User-Modus-Ausfuehrung nimmt ohne Teilwirkung die
Illegal-Instruction-Ausnahme; im Delay-Slot-Kontext wird der Owner-PC gesichert.
