# SH-4 TMU und RTC

KR-3102 bindet Timer Unit und Echtzeituhr an den zentralen, hostzeitfreien
Event-Scheduler. Grundlage ist das offizielle Renesas-Hardwarehandbuch fuer
SH7750/SH7750S/SH7750R:

https://www.renesas.com/en/document/mah/sh7750-sh7750s-sh7750r-group-users-manual-hardware

Die drei TMU-Kanaele besitzen 32-Bit-Konstanten und -Counter, interne Pck-Teiler,
RTC-Takt, Auto-Reload sowie sichtbare UNF-/UNIE-Zustaende. Reservierte und nicht
deterministisch bereitgestellte externe Takte scheitern sichtbar.

Die RTC verwendet eine 256-Hz-Unterteilung der konfigurierten Gastzyklen pro
Sekunde und niemals Hostzeit. Kalenderuebertraege, gregorianische Schaltjahre,
die sieben periodischen Raten sowie Carry-Zustaende sind deterministisch.

Register-MMIO und die Verbindung der sichtbaren Interruptzustaende mit dem
zentralen Interrupt-Controller werden in KR-3104 gemeinsam abgeschlossen, damit
keine zweite Interruptlogik entsteht.
