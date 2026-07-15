# Plattform-Interruptintegration

KR-3104 verbindet die bereits sichtbaren Pending-Zustaende von TMU, RTC und
DMAC mit dem zentralen SH-4-Interrupt-Controller. Die Synchronisation geschieht
an einem expliziten Safepoint und verwendet keine Hostthreads oder Hostzeit.

## Quellen und Codes

- TMU0 bis TMU2: `0x400`, `0x420`, `0x440`
- RTC Periodic und Carry: `0x4A0`, `0x4C0`
- DMTE0 bis DMTE3: `0x640` bis `0x6A0`
- DMAE: `0x6C0`
- Dreamcast-externe IRL13-, IRL11- und IRL9-Leitungen: `0x320`, `0x360`, `0x3A0`

TMU-Kanaele besitzen getrennte Prioritaeten. RTC-Quellen teilen eine Prioritaet,
ebenso alle DMAC-Quellen; das entspricht den IPRA-/IPRC-Gruppen des SH-4. Diese
programmierbaren Level werden auf 0 bis 15 begrenzt. Die drei externen Leitungen
besitzen ihre festen Dreamcast-Level 13, 11 und 9. Bei gleichem Level bleibt die
bestehende stabile Quellordnung erhalten.

Der Router spiegelt levelartige Pending-Zustaende. Wird ein Interrupt angenommen,
waehrend das Geraeteflag gesetzt bleibt, erscheint er am naechsten Safepoint
erneut. Erst die Quittierung am urspruenglichen Geraet entfernt die Quelle. BL,
IMASK, SPC, INTEVT und der VBR+`0x600`-Eintritt bleiben Aufgabe des zentralen
Interrupt-/Exception-Pfads.

Externe Leitungen sind bewusst nur Pins. AICA-, PVR-, Maple- und GD-ROM-Ereignisse
werden nicht direkt unter Umgehung des noch fehlenden Dreamcast-System-ASIC an
den SH-4 verdrahtet. Frame- und Audio-Taktung folgt in KR-3105.

Primaerreferenz:

- Renesas, *SH7750, SH7750S, SH7750R Group User's Manual: Hardware*, Abschnitte
  5 und 19, Rev. 7.02, 2013:
  <https://www.renesas.com/en/document/mah/sh7750-sh7750s-sh7750r-group-users-manual-hardware>
