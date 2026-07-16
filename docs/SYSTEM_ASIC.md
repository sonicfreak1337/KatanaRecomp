# Dreamcast-System-ASIC

KR-4603 fuehrt die fuer den Alpha-Boot benoetigte Holly-System-ASIC-
Interruptmatrix als gemeinsames MMIO-Geraet ein. Primaerreferenzen sind die
freien KallistiOS-Dateien
[`dc/asic.h`](https://github.com/KallistiOS/KallistiOS/blob/master/kernel/arch/dreamcast/include/dc/asic.h)
und
[`hardware/asic.c`](https://github.com/KallistiOS/KallistiOS/blob/master/kernel/arch/dreamcast/hardware/asic.c).

Die drei Pending-/ACK-Baenke liegen bei `0x005F6900` bis `0x005F6908`.
Masken fuer IRL13, IRL11 und IRL9 liegen jeweils in drei Baenken bei
`0x005F6910..18`, `0x005F6920..28` und `0x005F6930..38`. Writes in eine
Pending-Bank quittieren gesetzte Bits; Masken bestimmen, welche externe
SH-4-Leitung der zentrale `PlatformInterruptRouter` setzt.

Der Alpha-Vertrag fuehrt folgende Quellen durch dieselbe Matrix:

- PVR Render/VBlank
- Maple-DMA
- GD-ROM-Befehl, DMA und DMA-Fehler
- AICA-/G2-DMA und AICA-Interrupt

Geraete melden Ereignisse mit einem Gastzyklus. Der `EventScheduler` ordnet
gleiche Zyklen stabil nach Einreihungs-ID; das ASIC protokolliert zusaetzlich
eine monotone Sequenz. Rueckwaerts laufende Gastzeit wird abgelehnt.

Nur 32-Bit-Zugriffe auf dokumentierte Register sind erlaubt. Reservierte
Offsets, Registerluecken und falsche Zugriffsbreiten werfen Fehler und koennen
nicht als erfolgreiche MMIO-No-ops weiterlaufen. Das Geraet wird an den
physischen sowie P1-/P2-Direktsegmenten abgebildet.
