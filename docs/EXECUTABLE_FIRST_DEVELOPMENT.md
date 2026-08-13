# Executable-First-Entwicklung

> v0.49.1: Dieses Dokument beschreibt die wiederverwendbare Artefakt- und
> Analysebasis sowie den historischen Dreamcast-Diagnosepfad. Ein
> Produktbinary verwendet ausschliesslich den nativen Vertrag aus
> `NATIVE_PORT_PRODUCT_CONTRACT.md`; RuntimeOnly, Handoff und Geraetestate sind
> kein Produktfallback.

Seit v0.49 ist die private `.gdi` nicht mehr der normale Eingang jeder
Bring-up-Iteration. Sie wird einmalig fuer Extraktion und spaeter fuer die
Nutzerinstallation verwendet. Analyse, Codegen und Warmbuild arbeiten danach
mit einem unveraenderlichen, hashgebundenen Boot-Executable-Artefakt.

## Einmalige lokale Extraktion

```powershell
katana-recomp extract-boot-executable .\eigene-disc\game.gdi `
  --output .\private\boot-artifact
```

Der Ausgabeordner muss ausserhalb des KatanaRecomp-Quellbaums liegen:

```text
private/boot-artifact/
  boot.katana-executable  Manifest und Identitaetsbindung
  boot.bin                private Boot-Executable-Bytes
  disc.katana-install     sektorfreier Installationsvertrag
```

Die Extraktion ist immutable. Ein bereits vorhandenes, abweichendes Artefakt
wird nicht ueberschrieben. Das Manifest wird zuletzt geschrieben und beim
Laden zusammen mit Bootbytes und Installationsrecipe erneut validiert.
`boot.bin` ist Retailinhalt und darf weder committet noch verteilt werden.

## Bring-up und Warmbuild

```powershell
$env:KATANA_PORT_BUILD_PROFILE = 'bringup'
$env:KATANA_HOST_BUILD_GENERATOR = 'Ninja'
$env:KATANA_PORT_CXX_COMPILER = 'msvc'
$env:KATANA_PORT_LINKER = 'default'
$env:KATANA_HOST_BUILD_JOBS = '8'
$env:KATANA_PORT_CODEGEN_JOBS = '8'

katana-recomp port-executable `
  .\private\boot-artifact\boot.katana-executable `
  --output .\private\ports\game-direct `
  --target-name GameDirect `
  --console-profile europe-pal `
  --game-project .\private\game-project.katana-game-project `
  --game-entry-handoff .\private\boot-artifact\game-entry.katana-handoff
```

Eine zusammen mit `runtime-sdk` installierte CLI findet
`KatanaRecomp::native_port_runtime` im gemeinsamen Installationspraefix
automatisch. `runtime` und `runtime_core` sind nicht installierbarer
Diagnosebestand.
Fuer ein anderes installiertes SDK kann `KATANA_RUNTIME_PREFIX` gesetzt
werden; `KATANA_RUNTIME_ROOT` waehlt stattdessen bewusst den lokalen
Quellbaum-Fallback.

Der Export liest und verifiziert die Bootbytes, kopiert sie aber nicht in das
Portprojekt. Das verteilbare Projekt enthaelt nur native AOT-Quellen,
Metadaten, den Installationsvertrag und die allgemeine Runtimebindung.
Partition-, Metadaten- und Hostbuildcaches bleiben in einem versteckten
lokalen Workspace und werden nicht publiziert.

`port-executable` besitzt zusaetzlich einen versions- und
identitaetsgebundenen Whole-Export-Cache. Bei einem verifizierten Treffer
werden Kontrollflussanalyse, IR-Lowering, Partitionsemission und
Metadatenerzeugung uebersprungen; Configure, reales Spieltarget, Packaging und
Publish laufen weiterhin. Der Schluessel bindet Artefakt, Zielname,
Diagnosemodus, Konsolenprofil, Spielprojektdefinition und exakte
Spielprojekt-Artefaktidentitaet, Game-Entry-Artefaktidentitaet sowie Tool-,
Runtime-, Backend-, Port-,
Partitions-, Metadaten- und AOT-Profilversionen. Vor Wiederverwendung werden
der erzeugte Quellbaum und die Installationsrecipe erneut geprueft.

Ein Treffer ist an
`KATANA_PORT_SUBPHASE whole-program-analysis-ir-cache-hit` und
`Analyse-/IR-Cache-Hit: ja` erkennbar. Dieser Whole-Export-Cache gilt fuer
`port-executable` beziehungsweise `probe-port-executable`; der GDI-basierte
NativeDiscBoot-Export behaelt seine Partitions- und Metadatencaches.
Portprojektvertrag 90 bindet den aktuellen Spielprojektvertrag 7,
GameProject-Artefaktformat 6, die `katana-game-project-v5`-Metadaten und den
getrennten Game-Entry-Vertrag in diesen Schluessel und invalidiert damit aeltere
Whole-Export-Treffer, statt sie mit einer inkompatiblen Runtimegrenze
wiederzuverwenden.

Der historisch gemessene v28-Gesamtexport mit unveraenderter Analyse dauerte
4,209083 Sekunden; Analyse/IR, Metadaten und alle 42 AOT-Partitionen trafen
den Cache. Der anschliessende unveraenderte Hostbuild dauerte 0,219272
Sekunden.

## RuntimeOnly-NativeDisc-Bring-up

Der vollstaendige NativeDisc-Produktport kann opt-in mit
`port --analysis-mode runtime-only --game-project <projekt>` betrieben werden;
der Default bleibt `platform`. RuntimeOnly setzt `GuestCallAbi::Unknown`,
umgeht die blockierende SuperHC-FunctionValue-/Candidate-Resolution und
erzeugt weiterhin nativen AOT-Code. Indirekte Aufrufe verwenden RuntimeOnly-
Dispatch ueber eine exakte statische Guest->Host-Tabelle; Stop-on-miss und
typed abort bleiben aktiv. Es gibt keinen Interpreter, JIT, Runtime-Decoder
oder geratenen Zielpfad. Der Whole-Export-Cache bindet den Modus.

RuntimeOnly-v25/v29 wurde in `112,571 s` exportiert: `6.546` Funktionen,
`147` Partitionen, Release-Hostbuild und `623/624` Compile-Cache-Hits. Der
gemeinsame Source-/Dokumentations-Checkpoint baut auf `5046c01` auf und
umfasst die vier Runtime-/Codegen-Aenderungen dieses Meilensteins. Der
beaufsichtigte Sonic-PAL-Lauf endete sauber ohne Fatal- oder Watchdogfehler,
zeigte SEGA, PAL-TV-Setting und 60-Hz-Testbild, erreichte aber noch keinen
Memory-Card-Screen oder Hauptmenue.

## Zwei Produktpfade

### DirectBootExecutable

DirectBoot startet die verifizierte Boot-Executable direkt nativ. Ein
produktiver Spieleinstieg darf dabei nicht aus einem allgemeinen
Post-BIOS-Zustand abgeleitet werden, sondern benoetigt einen
`GameEntryHandoff` Schema 3 des externen Spielprojekts. Die Bindung prueft
Content, Boot-Executable, Konsolenprofil, Runtime-ABI,
Plattformzustandsvertrag und Descriptoridentitaet vor dem ersten Gastblock.
Der private titelgebundene Artefaktprovider besitzt Descriptor und Payloads
nach dem Laden selbst und stellt nur vorvalidierte, hashgebundene Slices
bereit.

Der aktuelle KR-5000-Stand verwendet GameEntryHandoff-Artefaktformat 2,
Runtime-ABI 101, Block-ABI 5, Analyzer-ABI 40, PlatformServices-ABI 14,
Backend-Interface-ABI 21, Portprojektvertrag 90, Native-Port-Profilvertrag 13, Native-AOT-Profil 30,
Partitionsschema 7 und Plattformzustandsvertrag 2. Vorhandene private
CompletePlatform-Artefakte aus den ABI-63-/ABI-64-Runden sind historische
Evidenz und muessen fuer einen spaeteren DirectBoot-Produktlauf neu fuer den
dann aktuellen ABI erzeugt werden. NativeDisc benoetigt keinen
Game-Entry-Handoff.
`CompletePlatform` ist
absichtlich nicht teilbar: Der Validator verlangt immer den kanonischen Satz
aus 22 Geraeteklassen einschliesslich Flash sowie die exakt zugeordneten
Schedulerereignisse. Dazu gehoeren PVR, GD-ROM und G1, SH-4-DMAC, AICA,
Maple-Bus und Maple-DMA, System Bus und System ASIC, alle IRQ-Vertraege, MMU,
Cache, Store Queues, I/O-Ports, Holly-G2-/PVR-DMA, TMU, RTC-Clock und RTC,
SCIF sowie Flash gemeinsam. Eine vorhandene VMU- oder Flashdatei allein
ersetzt diesen Laufzeitzustand nicht.

Der vollstaendige Capture-/Apply-Pfad ist im realen Produktport belegt.
NativeDisc erfasst den Zustand am sauberen Game Entry; DirectBoot validiert
und restauriert CPU, Speicher, alle 22 Geraete und die typisierte
Scheduler-Timeline vor dem ersten Spielblock. Der vorhandene
`CpuMemoryDiagnostic` bleibt ein expliziter Teilpfad fuer Ursachenanalyse.
Das normale Produktgate verbietet weiterhin Capture und Diagnose-Apply; nur
`CompletePlatform` ist im Produktpfad zulaessig.

Der derzeitige Koordinator validiert den Gesamtzustand vorab und prueft ihn
nach dem Restore durch semantischen Recapture. Der Quellpfad bereitet alle
falliblen Speicher-, Geraete-, Scheduler-, IRQ- und CPU-Schritte vor dem
globalen Commit vor und veroeffentlicht CPU-PC/PR zuletzt. Das produktive
Handoffprofil uebernimmt gastseitigen Plattformzustand, setzt aber
Hostdiagnostik, PVR-/Audioevidenz und Produktmetriken am Game Entry auf eine
neue Baseline. Installierte VMU- und Flashdaten bleiben autoritativ; ein
Capture darf sie nicht zurueckrollen. `KR-4967` und `KR-4970` sind damit
quellseitig implementiert. Ihre ABI-passende Produktabnahme und normative
NativeDisc-/DirectBoot-Digests bleiben offen; sie folgen erst nach dem
Performance- und Gesamtpruefungsgate KR-4974 bis KR-4984.

### Privates Handoff binden

Das externe `GameProjectArtifact` Format 6 besitzt fuer Spielprojektvertrag 7
alle Strings und Arrays
seiner rein deklarativen Definition. Es serialisiert exakte Funktionsgrenzen,
Jump-/Callbacktabellen, Runtime-AOT-Templates, Symbole, Codeidentitaeten und
Bootkonfiguration mit Payload- und Gesamtartefakt-SHA-256. Native Overrides,
Mid-Function-Hooks und private Handoffprovider enthalten Prozesszeiger oder
separat besessene Daten und werden deshalb fail-closed nicht serialisiert.

Der Export bindet `--game-project` und `--game-entry-handoff` gemeinsam an
die Boot-Executable und nimmt beide exakten Artefaktidentitaeten in den
Whole-Export-Cache auf. Die vollstaendige Spielprojektdefinition steuert
Analyse, CFG, IR und AOT. Wenn keine nativen Hooks eine externe
Runtime-Registrierung erfordern, registriert der erzeugte Port lokal nur die
reduzierte Identitaets-, Boot- und Handoffdefinition. Die privaten Artefakte
werden nicht in das Portprojekt kopiert. Zum Lauf erhaelt der Produktport den
Handoffpfad explizit:

```powershell
$env:KATANA_GAME_ENTRY_HANDOFF_PRODUCT = `
  '.\private\boot-artifact\game-entry.katana-handoff'
.\private\ports\game-direct\GameDirect.exe
```

Die generierte Registrierung akzeptiert nur die beim Export erwartete
Executable-, Konsolen-, Descriptor- und Spielprojektidentitaet. Ein
abweichendes Artefakt oder eine konkurrierende Registrierung scheitert vor
Gastcode.

Gemeinsame BIOS-Dienste, Scheduler, Interrupts, PVR, AICA, Maple und GD-ROM
bleiben aktiv. DirectBoot trennt Bootstrapprobleme von Problemen im
eigentlichen Spielprogramm; er ist kein Interpreter und kein Emulator.

### NativeDiscBoot

```powershell
katana-recomp port .\eigene-disc\game.gdi `
  --output .\private\ports\game-disc `
  --target-name GameDisc `
  --console-profile europe-pal `
  --game-project .\private\game-projects\game.katana-game-project
```

Dieser Pfad kompiliert auch den disc-eigenen Bootstrap und bleibt das finale
Genauigkeits- und Kompatibilitaetsgate sowie die Game-Entry-Referenz. Er ist
bewusst nicht der schnelle Standard fuer jede Entwicklungsiteration. Das
optionale externe Projekt ist auf beiden Bootpfaden identisch
hashgebunden; ohne diese Bindung waeren AOT-Coverage- und Hookvergleiche
zwischen NativeDisc und DirectBoot nicht aussagekraeftig.

## Pflege privater Portexporte

Ein bestaetigter Nachfolgeexport ersetzt alte, unbrauchbare generierte
Portordner. Solche Altports werden regelmaessig gezielt geloescht, damit
Diagnose und Messungen nicht versehentlich gegen einen veralteten Vertrag
laufen. Der aktuelle DirectBoot-Port, die NativeDisc-Referenz, das
extrahierte Boot-Executable-Artefakt und installierte Nutzerdaten sind davon
getrennt und werden nicht als Portexport-Abfall behandelt.

## Historische Disc-Diagnoseinstallation

Die folgende `--install-disc`-Beschreibung ist nur der historische
Disc-Diagnosepfad. Ein nativer Produkt-Runner verlangt eine Executable und
einen privaten `ContentRoot`; beide Pfade werden vor dem Start validiert. Fuer
Native Ports gibt es keinen `--install-disc`-Produktflow.

## Historische Disc-Diagnoseinstallation

Die folgende `--install-disc`-Beschreibung ist nur der historische
Disc-Diagnosepfad. Ein nativer Produkt-Runner verlangt eine Executable und
einen privaten `ContentRoot`; beide Pfade werden vor dem Start validiert. Fuer
Native Ports gibt es keinen `--install-disc`-Produktflow.

## Nutzerinstallation bleibt discbasiert

Das verteilte Portbinary erwartet weiterhin die rechtmaessig vorhandene
Originaldisc des Nutzers:

```powershell
.\GameDirect.exe --install-disc D:\eigene-disc\game.gdi
.\GameDirect.exe
```

Der Installer prueft Geometrie, Tracks, Hashes, Contentidentitaet und
Bootdatei, bevor er atomar
`user-data/content/game.katana-disc` erzeugt. Dieser lokale Cache liefert
Runtime-Spieldateien; er aendert den CPU-Einstieg von DirectBoot nicht.
`user-data` bleibt beim Republizieren und bei Warmbuilds erhalten.

## Produkt-Gate

Bootfortschritt und Performance werden getrennt bewertet. Der aktuelle
Schalter setzt eine Gastzykluslaufdauer ab Game Entry und einen grosszuegigen
Host-Watchdog, statt nach einer festen Drei-Sekunden-Hostzeit automatisch
Bootkorrektheit zu behaupten:

```powershell
$env:KATANA_GUEST_CYCLE_BUDGET = '600000000'
$env:KATANA_PORT_FINAL_PROGRESS = '1'
$env:KATANA_GAME_ENTRY_HANDOFF_PRODUCT = `
  '.\private\boot-artifact\game-entry.katana-handoff'
.\GameDirect.exe
```

Der aktuelle `KR-4966`-Quellvertrag berechnet
`target_cycle = game_entry_cycle + requested_post_entry_cycles` und berichtet
Restore-, End- und tatsaechlich ausgefuehrte Post-Entry-Zyklen getrennt. Bei
angefordertem Produktbudget ist Exitcode 0 nur moeglich, wenn der geforderte
Meilenstein und das vollstaendige Budget erreicht wurden; ein vorzeitig
beendeter Bring-up-Lauf bleibt auch nach einem fruehen Meilenstein kein
erfolgreiches Gate. Eine inhaltliche Bildschirmklassifikation erfordert
weiterhin eine reale visuelle Aufnahme.

Die v24-`CompletePlatform`-Vergleichsbasis ergab:

- `NativeDiscBoot`: exakt 600.000.000 Gastzyklen in 6,3161 Sekunden,
  94,9954 effektive Gast-MHz, 17.080.114 zentrale Dispatches und ein
  sichtbarer IP.BIN-Frame;
- `DirectBootExecutable`: Restore bei 415.233.270 und Ende am absoluten
  Schedulermaximum 600.000.000; damit 184.766.730 Post-Entry-Zyklen in
  5,01505 Sekunden beziehungsweise 36,8425 MHz, 16.033.676 zentrale
  Dispatches und noch kein sichtbarer Frame.

Der terminal gemeldete Direct-Wert von 119,64 MHz verwendet den absoluten
Schedulerstand und ist nicht vergleichbar. Die 16.033.676 Dispatches ergeben
11,52 Zyklen je ausgefuehrtem Post-Entry-Dispatch.

Der historische v30-DirectBoot installiert weiterhin die private Originaldisc
ueber den Produktinstaller, startet danach aber executable-first mit dem
CompletePlatform-Handoff und dem externen Spielprojektartefakt. Die exakte
hashgebundene Funktionsgrenze gelangt durch Analyzer, CFG, IR und AOT; der
Produktlauf passiert damit den bisherigen KR-4971-Blocker. Er endet bei
Gastzyklus `553.990.562`, nach `138.757.292` Post-Entry-Zyklen und
`10.079.932` Zentraldispatches. Das sind `+1.086.915` Gastzyklen gegen v26.
Der historische v28-Fehler-zu-Fehler-Vergleich mass dieselbe
Post-Entry-Arbeit in 5,275792 Sekunden, also 26,3008 MHz gegen 23,9578 MHz
bei v26 und provisorisch `+9,78 %`. Der v30-Sichtlauf ist kein
Performancebenchmark; ein 600-Millionen-Gate liegt weiterhin nicht vor.

Der historische v28-/v30-Blocker KR-4972 war
die geprüfte private Callback-Kante. Das unveraenderte Ziel springt in einen
gemeinsamen Codepfad. Die damalige generische Analyse erkannte Ziel und
gemeinsamen Body ueber begrenzte Tail-Jump- und Runtime-Frame-Provenienz; der
damalige vollstaendige Spielprojektexport uebernahm den Seed aber noch nicht
in CFG, Source-Map und AOT. Der v30-Produktlauf reproduzierte deshalb den
`aot-template-mismatch` mit denselben Gastzyklen und Dispatches wie v28.
Zwei technische Direct-Frames sind vorhanden, die 15 neuen realen
Fensteraufnahmen bleiben jedoch schwarz.

Der historische NativeDisc-v32-Lauf verwendet erstmals dieselbe externe
Spielprojektdatei wie DirectBoot. Er zeigt ab 2,032 Sekunden sichtbar den
Sega-Lizenzscreen, praesentiert 127 Hostframes und endet nach 6,701 Sekunden
mit 11.080.283 Zentraldispatches und provisorisch 82,67 MHz exakt wie
DirectBoot-v30 bei Zyklus `553.990.562` an
die geprüfte private Callback-Kante. Damit ist der naechste Gast-/AOT-Blocker gleich,
waehrend die alte DirectBoot-Schwarzausgabe auf den inzwischen in
Runtime-ABI 64 entkoppelten Scanout-/Proofvertrag zurueckgefuehrt ist.
DirectBoot erwartet weiterhin keinen Sega-Screen, weil es IP.BIN
ueberspringt; seine naechste Abnahme ist ein echter Spiel-Framebufferwrite
oder TA-Frame nach einem frischen, ABI-passenden Handoff. Die historische
v32-Produkt-EXE war 53.677.056 Bytes gross.

Im damaligen Stand `b01586a` waren folgende P0-Umbauten im generischen
Quellpfad implementiert: Guarded-AOT-Einstiege und ihre Exportvollstaendigkeit,
Carrier-/Inventar-/Codepointer-Provenienz, atomarer und Save-erhaltender
CompletePlatform-Apply, relatives Produktgate, Static-AOT-Seitentabelle,
vorverlagerter P1/P2-Cache mit zielbezogener SMC-Revalidierung, direkte
Owner-Einstiege, endliche indirekte native Ziele, architektonische
Safepoints, livenessbasierte Registerlokalisierung und gebatchte direkte
Haupt-RAM-Writes mit zusammengefasster Codeinvalidierung. Der aktuelle
Source-Checkpoint `18f8537` enthaelt weitere Analyse-, Cache-, Fortschritts-
und Runtimeumbauten, ist aber ebenfalls keine Produkt-Erfolgsmeldung. Der
naechste private NativeDisc-Port bleibt bis zum Abschluss von KR-4974 bis
KR-4984 gesperrt.
