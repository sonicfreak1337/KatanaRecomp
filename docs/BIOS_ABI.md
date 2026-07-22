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
| `0x8C0000E0` | SYSTEM | `r4` |

Jeder Slot zeigt auf einen generierten Vier-Byte-RAM-Stub und einen
gleichadressigen Runtimeblock. Der normale physische Aliasdispatch findet diese
Blocks auch ueber P1/P2-Aliase. `FirmwareHandoffMap` protokolliert Slot und
Handler als dynamische Laufzeitsymbole mit Gastzyklus und HLE-Provenienz.
Zusaetzlich existiert bei `0x8C0010F0` ein gleichwertiger direkter Aliasblock
fuer GDROM2. Dieser bekannte ABI-Einstieg wird von Software ohne Umweg ueber
den Slot `0x8C0000C0` aufgerufen.

Anders als die uebrigen C-artigen Vektoren uebergibt SYSTEM seinen
Funktionscode als erstes Argument in `r4`. Die Rueckkehrfunktionen 0
(Normalinitialisierung) und 2 (Discpruefung samt erneutem Laden der sieben
Bootstrapsektoren) sind an PVR-/ASIC- beziehungsweise GD-ROM-Zustand gebunden.
`SYSTEM -1`, `SYSTEM 1` und `SYSTEM 3` sind typisierte, nicht zurueckkehrende
Lifecycle-Ausgaenge fuer Reset, BIOS-Menue und CD-Menue. Insbesondere startet
`SYSTEM 1` weder den Bootstrap noch die Bootdatei intern neu. Der Host kann
danach explizit einen neuen Lauf anbieten. Die Grenze bewahrt Gastzyklus,
Callsite, `PR`, `r0..r15` und den letzten GD-ROM-Requeststatus als strukturierte
Evidenz.

Zustandsfreie Initialisierung sowie ROMFONT-Lock/Unlock koennen bereits
deterministisch abgeschlossen werden. Flash- und GD-ROM-Dienste sind an ihre
konkreten Plattformdienste gebunden. Fontdaten und `SYSINFO_ICON` enden bis zu
einer echten Pufferausgabe sichtbar als `service-unavailable`. Unbekannte
Vektoren, Selektoren und Superselektoren werfen eine stabile
`BiosAbiDispatchError`-Diagnose mit Handler, Selektoren, Ruecksprungadresse und
einem vollstaendigen SH-4-Registersnapshot. Kein Aufruf wird titelbezogen
umgebogen oder als erfolgreicher No-op behandelt.

Vor dem ersten nativen Gastblock stellt der Disc-Boot den BIOS-Handoffzustand
her. Dazu gehoeren die dokumentierten SH-4-Kontrollregister, `DMAOR=0x8201`,
die AICA-Interruptlevel und fuer das explizite Profil `europe-pal` das PAL-
Broadcast-Bit. Die Disc-Areasymbole waehlen keine Konsolenregion.
HLE-Stubs liegen ausserhalb von `VBR+0x100`, `VBR+0x400` und `VBR+0x600`, damit
echte Exceptions nie als BIOS-Aufruf fehlgeroutet werden. Diese Werte sind
plattformweit und werden weder aus einem Spiel noch aus proprietaeren
Firmwarebytes uebernommen.

Der untere 64-KiB-BIOS-Arbeitsbereich beginnt definiert mit `0xFF`. Danach
werden Vektoren und HLE-Stubs sowie im oberen 32-KiB-Fenster der Discbootstrap
installiert. Bootstrap- und Bootdateibytes unterliegen der normalen
Codeinvalidierung; ein Lifecycle-Aufruf mutiert oder restauriert sie nicht.

Der GD-ROM-Dienst modelliert Requests als `Queued`, `Processing`, `Complete`,
`Streaming` oder `Error`. `GET_CMD_STAT` liefert vor der Verarbeitung 1, nach
Erfolg einmalig 2, fuer Streaming 3, bei Fehler `-1` und nach Abholung 0. Der
optionale Statuspuffer umfasst vier Woerter: Fehler 1, Fehler 2, uebertragene
Bytezahl und Warte-/ATA-Zustand. Das BIOS-TOC besitzt unabhaengig vom
Paketkommando exakt 102 Gastwoerter und trennt LOW/HIGH. Unbekannte Kommandos
werden nicht als Erfolg abgeschlossen. Ein begrenztes Ereignislog zeichnet
Zyklus, Aufrufstelle, Selektoren, Argumente, Zustandswechsel und Vierwortstatus
ohne Pfade oder Discidentitaeten auf.

`GDSTAR/GDLEN` bleiben programmierte G1-Register. `GDSTARD/GDLEND` zeigen den
separaten Liveadress- und Transferzaehler. `SYSTEM 0` restauriert den aus der
geladenen Bootgroesse berechneten BIOS-Livewert, ohne die programmierten
Register zu veraendern.

Der maschinenlesbare Vertrag besitzt Schema `katana-bios-abi`, Version 6.
Synthetische Tests pruefen reproduzierbare Vektorbytes, Runtimeblockdispatch,
P1/P2-Handoff, den direkten GD2-Alias, Lifecycle-Evidenz, GD-ROM-Zustaende,
Vierwortstatus, LOW/HIGH-TOC, getrennte G1-Zaehler, bekannte aufgeschobene
Dienste und unbekannte Funktionen.

Die Installation gehoert zum produktiven Bootzustand: `boot_homebrew()` gibt
im HLE-Modus die befuellte `RuntimeBlockTable` und `FirmwareHandoffMap` als
lebenszyklusgebundene Ressourcen zurueck. Der GDI-/Portpfad waehlt HLE aus
`execution.firmware`, initialisiert dieselben Ressourcen in
`DreamcastRuntimeState` und bettet die Moduswahl in die erzeugte `game.exe` ein.
Der Vertical-Slice ruft keine nachtraegliche Testinstallation mehr auf.
