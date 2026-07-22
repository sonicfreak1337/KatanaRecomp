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

`FLASHROM_READ` liefert bei Erfolg die kopierte Bytezahl. Schreiben liefert
die programmierte Bytezahl und behaelt die physische 1-nach-0-Semantik;
Loeschen arbeitet partitionsweise. Die Factory-Partition bei `0x0001A000`
ist fuer beide mutierenden Dienste schreibgeschuetzt. Vor jedem HLE-Aufruf
werden ausserdem die installierten `RTS/NOP`-Stubbytes im Gastspeicher
validiert. Eine Mutation bleibt dadurch auch ueber einen bereits aufgeloesten
Runtimehandle nicht ausfuehrbar.

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

Der GD-ROM-Dienst modelliert genau einen sichtbaren Request als `Queued`,
`Processing`, `Streaming`, `Complete`, `Error` oder `Aborted`. `REQ_CMD`
(Selektor 0) liefert eine positive Request-ID oder 0, wenn kein weiterer
Request angenommen werden kann. `EXEC_SERVER` (Selektor 2) startet den
eingereihten Request. `GET_CMD_STAT` (Selektor 1) liefert vor der Verarbeitung
1, nach Erfolg einmalig 2, fuer einen bereiten Stream 3, bei Fehler `-1` und
nach Abholung beziehungsweise Abbruch 0. Der optionale Statuspuffer umfasst
vier Gastwoerter: Fehler 1, Fehler 2, uebertragene Bytezahl und Warte-/ATA-
Zustand. Unbekannte Kommandos werden kontrolliert mit `InvalidCommand`
abgeschlossen und niemals als stiller Erfolg behandelt.

Die BIOS-Kommandos 28 und 37 bilden den DMA- beziehungsweise PIO-Stream als
denselben Requestzustandsautomaten ab. Nach `EXEC_SERVER` wird die
Discbereitschaft erst nach der geplanten Gastzeit sichtbar; erst dann meldet
`GET_CMD_STAT` den Zustand 3. Selektor 6 startet einen ausgerichteten DMA-
Teiltransfer, Selektor 7 schreibt den Fortschritts-/Restwert und liefert 1
waehrend des Transfers beziehungsweise 0 nach Abschluss. Konkret steht bei
Rueckgabe 1 der bereits uebertragene Zaehler des aktiven Teiltransfers im
Ausgabewort; bei Rueckgabe 0 steht dort der noch verbleibende Gesamtstream.
Der G1-Pfad bewegt
die Daten in gastzeitgebundenen, hoechstens 2048 Byte grossen Chunks, fuehrt
Liveadresse und Residue nach jedem Chunk fort und erzeugt genau einen finalen
GD-ROM-DMA-Interrupt. Selektoren 12 und 13 bilden denselben Vertrag fuer einen
PIO-Teiltransfer mit derselben Fortschritt-/Gesamtstream-Bedeutung ab; der
Datentransfer findet dort an der expliziten BIOS-Grenze statt.

Die EX-Kommandos 38 und 39 sind keine Aliase fuer 28 und 37: Ihr drittes
Parameterwort waehlt einen abweichenden Paketvertrag. Bis dessen Bedeutung
vollstaendig eigenstaendig spezifiziert und getestet ist, werden diese
Kommandos kontrolliert abgelehnt statt mit erfundenen Normalstreamwerten
ausgefuehrt.

Selektor 5 ist ausdruecklich keine DMA-Callbackregistrierung. Er akzeptiert
Callbackadresse und Argument nur als einmaligen Handoff, nachdem ein DMA-
Teiltransfer seinen Interrupt wirklich erreicht hat; ein vorzeitiger oder
wiederholter Aufruf schlaegt sichtbar fehl. Selektor 11 installiert dagegen
einen persistenten PIO-Callback. Ein faelliger Callback verlaesst den BIOS-
Block als typisierter Gast-`Call`, behaelt `PR` bei und uebergibt das
registrierte Argument in `r4`. Abbruch und Reset entfernen ausstehende
Schedulerereignisse, G1-Transfers und Callback-Handoffs, sodass keine spaeten
Gastschreibzugriffe oder Interrupts entstehen.

Das BIOS-TOC besitzt unabhaengig vom Paketkommando exakt 102 Gastwoerter und
trennt LOW/HIGH. Ein begrenztes Ereignislog zeichnet Zyklus, Aufrufstelle,
Selektoren, Argumente, Zustandswechsel und Vierwortstatus ohne Pfade oder
Discidentitaeten auf.

`GDSTAR/GDLEN` bleiben programmierte G1-Register. `GDSTARD/GDLEND` zeigen den
separaten Liveadress- und Transferzaehler. `SYSTEM 0` restauriert den aus der
geladenen Bootgroesse berechneten BIOS-Livewert, ohne die programmierten
Register zu veraendern.

BIOS und ATA-/SPI-Taskfile teilen einen expliziten Laufwerksbesitzer und
koennen keine widerspruechlichen Kommandos gleichzeitig beginnen. Der
Taskfilepfad implementiert Dreamcast `REQ_MODE`, `SET_MODE`, `REQ_ERROR` und
`GET_TOC`. `REQ_STAT` liefert aus einem exakt 10 Byte grossen Puffer den
Laufwerks-, Discformat-, Track- und aktuellen FAD-Status; Offset und
Allocation-Length muessen vollstaendig darin liegen. `REQ_MODE` adressiert
einen 32-Byte-Hardwareinfopuffer nur an
geraden, vollstaendig enthaltenen Bereichen; die unabhaengig bekannten Bytes
0 bis 9 sind definiert, die nicht uebernommenen Identitaetsfelder 10 bis 31
bleiben kontrolliert unavailable. `SET_MODE` darf nur den beschreibbaren
Bereich 0 bis 9 aendern.

PIO-DataIn/DataOut verwendet den programmierten Host-ByteCount als
Phasengrenze (`0` bedeutet 64 KiB), aktualisiert den sichtbaren Restzaehler und
signalisiert jede neue Datenphase sowie den finalen Status ueber Command-IRQ.
Bleibt ein Phasen-IRQ bei bereits hohem Signal pending, wird er erst nach der
normalen Statusquittierung erneut als Low-zu-High-Flanke zugestellt; Alternate
Status quittiert ihn nicht. `CD_READ` mit `Features.Bit0` verwendet dagegen
den getrennten `DmaIn`-/DMARQ-DMACK-Vertrag: kein PIO-DRQ, kein Zwischen-IRQ,
`BSY` waehrend partieller G1-DMA und genau ein finaler Status-IRQ nach dem
letzten DMA-Byte. Der belegte ATA-SET-FEATURES-Modus ist auf Features `0x13`
mit Sector Count `0x22` begrenzt; andere Kombinationen enden mit ABRT und
INVALID FIELD.

Sense/CHECK bleibt bis zur Datenphase von `REQ_ERROR` persistent. Die
sichtbaren ASC-Abbildungen unterscheiden InvalidCommand `0x20`, OutOfRange
`0x21`, InvalidField `0x24` und NoMedia `0x3A`. Der persistente Sense-Payload
ist vom ATA-`ERR`-Bit des aktuell abgeschlossenen Taskfilekommandos getrennt:
Ein erfolgreiches Folgekommando endet ohne `ERR`, ohne den noch abrufbaren
Sense vorzeitig zu verlieren. Asynchrone BIOS-Completions signalisieren die
gemeinsame ASIC-Grenze direkt und verwenden nicht das durch Command-Status
quittierbare Taskfile-IRQ-Latch; deshalb bleibt jede sequenzielle BIOS-
Completion als eigene Flanke sichtbar. Waehrend eines BIOS-Reads
beziehungsweise -Teiltransfers meldet das vierte Statuswort den Wartezustand 4
und das Taskfile bleibt sichtbar `BSY`.

Alle hier beschriebenen GD-ROM-BIOS-Ausgaben mit Gastziel werden vor dem ersten
Byte MMU-bewusst ueber die gesamte Laenge vorvalidiert. Virtuelle Seiten
muessen auf einen
zusammenhaengenden, vollstaendig schreibbaren linearen Speicherbereich
abbilden; Adressueberlauf, Luecken und MMIO werden kontrolliert als
`InvalidField` mit Sense `0x24` beendet. Insbesondere asynchrone Reads und der
408-Byte-BIOS-TOC koennen dadurch weder Teilwrites noch Host-Exceptions
erzeugen.

Der maschinenlesbare Vertrag besitzt Schema `katana-bios-abi`, Version 9.
Synthetische Tests pruefen reproduzierbare Vektorbytes, Runtimeblockdispatch,
P1/P2-Handoff, Stubintegritaet, Factory-Schreibschutz, den direkten GD2-Alias,
Lifecycle-Evidenz, GD-ROM-Zustaende,
Vierwortstatus, LOW/HIGH-TOC, gastzeitgebundene DMA-Chunks, DMA-IRQ-Handoff,
persistente PIO-Callbacks, getrennte PIO-/`DmaIn`-Phasen, den 32-Byte-
Modepuffer, Sense-/IRQ-Grenzen, Abbruch ohne spaete Ereignisse, getrennte G1-
Zaehler, bekannte aufgeschobene Dienste und unbekannte Funktionen.

Die Installation gehoert zum produktiven Bootzustand: `boot_homebrew()` gibt
im HLE-Modus die befuellte `RuntimeBlockTable` und `FirmwareHandoffMap` als
lebenszyklusgebundene Ressourcen zurueck. Der GDI-/Portpfad waehlt HLE aus
`execution.firmware`, initialisiert dieselben Ressourcen in
`DreamcastRuntimeState` und bettet die Moduswahl in die erzeugte `game.exe` ein.
Der Vertical-Slice ruft keine nachtraegliche Testinstallation mehr auf.
