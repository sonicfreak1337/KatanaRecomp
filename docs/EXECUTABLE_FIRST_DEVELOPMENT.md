# Executable-First-Entwicklung

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
`KatanaRecomp::runtime_core` im gemeinsamen Installationspraefix automatisch.
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
Portprojektvertrag 53 bindet den erweiterten Spielprojekt- und
Game-Entry-Vertrag samt `katana-game-project-v2`-Metadaten in diesen
Schluessel und invalidiert damit aeltere
Whole-Export-Treffer, statt sie mit einer inkompatiblen Runtimegrenze
wiederzuverwenden.

Der aktuell gemessene v28-Gesamtexport mit unveraenderter Analyse dauerte
4,209083 Sekunden; Analyse/IR, Metadaten und alle 42 AOT-Partitionen trafen
den Cache. Der anschliessende unveraenderte Hostbuild dauerte 0,219272
Sekunden.

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

Der aktuelle Gesamtvertrag verwendet Artefaktformat 2, Runtime-ABI 63,
Portprojektvertrag 53 und Plattformzustandsvertrag 2. `CompletePlatform` ist
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
nach dem Restore durch semantischen Recapture. `KR-4967` bleibt trotzdem
offen: CPU/RAM werden noch vor einer Folge potentiell fallibler passiver
Geraeterestores committed. Der geforderte global vorbereitete
`noexcept`-Commit, normative Subsystemdigests und das allgemeine
save-erhaltende `ProductHandoff` aus `KR-4970` sind noch nicht abgeschlossen.

### Privates Handoff binden

Das externe `GameProjectArtifact` Format 1 besitzt alle Strings und Arrays
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
  --console-profile europe-pal
```

Dieser Pfad kompiliert auch den disc-eigenen Bootstrap und bleibt das finale
Genauigkeits- und Kompatibilitaetsgate sowie die Game-Entry-Referenz. Er ist
bewusst nicht der schnelle Standard fuer jede Entwicklungsiteration.

## Pflege privater Portexporte

Ein bestaetigter Nachfolgeexport ersetzt alte, unbrauchbare generierte
Portordner. Solche Altports werden regelmaessig gezielt geloescht, damit
Diagnose und Messungen nicht versehentlich gegen einen veralteten Vertrag
laufen. Der aktuelle DirectBoot-Port, die NativeDisc-Referenz, das
extrahierte Boot-Executable-Artefakt und installierte Nutzerdaten sind davon
getrennt und werden nicht als Portexport-Abfall behandelt.

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
Schalter setzt ein absolutes Schedulermaximum und einen grosszuegigen
Host-Watchdog, statt nach einer festen Drei-Sekunden-Hostzeit automatisch
Bootkorrektheit zu behaupten:

```powershell
$env:KATANA_GUEST_CYCLE_BUDGET = '600000000'
$env:KATANA_PORT_FINAL_PROGRESS = '1'
$env:KATANA_GAME_ENTRY_HANDOFF_PRODUCT = `
  '.\private\boot-artifact\game-entry.katana-handoff'
.\GameDirect.exe
```

Nach einem Schedulerrestore ist dieses Maximum noch keine Laufdauer.
`KR-4966` muss den Zielwert auf
`restored_game_entry_cycle + requested_elapsed_guest_cycles` umstellen. Bis
dahin ist die terminal aus dem absoluten Zaehler berechnete MHz-Zahl eines
Handoff-Laufs kein gueltiger Performancevergleich. Eine inhaltliche
Bildschirmklassifikation erfordert weiterhin eine reale visuelle Aufnahme.

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

Der aktuelle v28-DirectBoot installiert weiterhin die private Originaldisc
ueber den Produktinstaller, startet danach aber executable-first mit dem
CompletePlatform-Handoff und dem externen Spielprojektartefakt. Die exakte
hashgebundene Funktionsgrenze gelangt durch Analyzer, CFG, IR und AOT; der
Produktlauf passiert damit den bisherigen KR-4971-Blocker. Er endet bei
Gastzyklus `553.990.562`, nach `138.757.292` Post-Entry-Zyklen und
`10.079.932` Zentraldispatches. Das sind `+1.086.915` Gastzyklen gegen v26.
Die tatsaechliche Post-Entry-Arbeit ergibt in 5,275792 Sekunden 26,3008 MHz
bis zum Fehler gegen 23,9578 MHz bei v26, also provisorisch `+9,78 %`,
nicht aber ein 600-Millionen-Gate.

Der neue erste Blocker KR-4972 ist
`0x8C11088C -> 0x8C64784E`. Das unveraenderte Ziel springt in einen
gemeinsamen Codepfad; die richtige Callback-/Shared-Tail-/Thunk-Modellierung
und exakte Grenze muessen bewiesen werden. Der Fehler wird korrekt als
`aot-template-mismatch` klassifiziert. Zwei technische Direct-Frames sind
vorhanden, die sechzehn realen Fensteraufnahmen bleiben jedoch schwarz.
