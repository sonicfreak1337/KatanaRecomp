# HLE-BIOS-ABI und dynamische Vektoren

KR-4602 modelliert die Dreamcast-BIOS-ABI titelunabhaengig als Runtimevertrag.
Primaerreferenz ist die freie KallistiOS-Implementierung
[`kernel/arch/dreamcast/hardware/syscalls.c`](https://github.com/KallistiOS/KallistiOS/blob/master/kernel/arch/dreamcast/hardware/syscalls.c).
Es werden keine Quellteile oder Firmwarebytes uebernommen.

Beim HLE-Boot werden sechs Zeiger in Dreamcast-RAM installiert:

| Slot | Dienstfamilie | Selektor |
| --- | --- | --- |
| `0x8C0000B0` | SYSINFO | `r7` |
| `0x8C0000B4` | ROMFONT | `r1` |
| `0x8C0000B8` | FLASH | `r7` |
| `0x8C0000BC` | MISC/GD-ROM | `r7`, Superselektor `r6` |
| `0x8C0000C0` | zweiter, undokumentierter GD-ROM-Vektor | `r7` |
| `0x8C0000E0` | SYSTEM | `r7` |

Jeder Slot zeigt auf einen generierten Vier-Byte-RAM-Stub und einen
gleichadressigen Runtimeblock. Der normale physische Aliasdispatch findet diese
Blocks auch ueber P1/P2-Aliase. `FirmwareHandoffMap` protokolliert Slot und
Handler als dynamische Laufzeitsymbole mit Gastzyklus und HLE-Provenienz.

Zustandsfreie Initialisierung sowie ROMFONT-Lock/Unlock koennen bereits
deterministisch abgeschlossen werden. Flash-, GD-ROM-, Fontdaten- und
Host-Lifecycle-Aufrufe sind bekannt, enden bis zur Anbindung ihrer
Plattformdienste aber sichtbar als `service-unavailable`. Unbekannte Vektoren,
Selektoren und Superselektoren werfen eine stabile `BiosAbiDispatchError`-
Diagnose mit Handler und Selektoren. Kein Aufruf wird titelbezogen umgebogen
oder als erfolgreicher No-op behandelt.

Der maschinenlesbare Vertrag besitzt Schema `katana-bios-abi`, Version 1.
Synthetische Tests pruefen reproduzierbare Vektorbytes, Runtimeblockdispatch,
P1/P2-Handoff, bekannte aufgeschobene Dienste und unbekannte Funktionen.
