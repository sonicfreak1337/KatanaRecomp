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
  --console-profile europe-pal
```

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
Diagnosemodus, Konsolenprofil sowie Tool-, Runtime-, Backend-, Port-,
Partitions-, Metadaten- und AOT-Profilversionen. Vor Wiederverwendung werden
der erzeugte Quellbaum und die Installationsrecipe erneut geprueft.

Ein Treffer ist an
`KATANA_PORT_SUBPHASE whole-program-analysis-ir-cache-hit` und
`Analyse-/IR-Cache-Hit: ja` erkennbar. Dieser Whole-Export-Cache gilt fuer
`port-executable` beziehungsweise `probe-port-executable`; der GDI-basierte
NativeDiscBoot-Export behaelt seine Partitions- und Metadatencaches.

## Zwei Produktpfade

### DirectBootExecutable

DirectBoot erzeugt einen clean-room definierten Post-BIOS-Zustand und startet
die verifizierte Boot-Executable direkt nativ. Gemeinsame BIOS-Dienste,
Scheduler, Interrupts, PVR, AICA, Maple und GD-ROM bleiben aktiv. DirectBoot
trennt Bootstrapprobleme von Problemen im eigentlichen Spielprogramm; er ist
kein Interpreter und kein Emulator.

### NativeDiscBoot

```powershell
katana-recomp port .\eigene-disc\game.gdi `
  --output .\private\ports\game-disc `
  --target-name GameDisc `
  --console-profile europe-pal
```

Dieser Pfad kompiliert auch den disc-eigenen Bootstrap und bleibt das finale
Genauigkeits- und Kompatibilitaetsgate. Er ist bewusst nicht der schnelle
Standard fuer jede Entwicklungsiteration.

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

Bootfortschritt und Performance werden getrennt bewertet. Das normale Gate
verwendet mindestens 600.000.000 Gastzyklen und einen grosszuegigen
Host-Watchdog, statt nach einer festen Drei-Sekunden-Hostzeit automatisch
Bootkorrektheit zu behaupten:

```powershell
$env:KATANA_GUEST_CYCLE_BUDGET = '600000000'
$env:KATANA_PORT_FINAL_PROGRESS = '1'
$env:KATANA_PORT_IGNORE_FOCUS = '1'
.\GameDirect.exe
```

Die terminale Zusammenfassung nennt erreichte Gastzyklen, Hostzeit, effektive
Gast-MHz, zentralen Dispatchcount, sichtbare technische Framemarker und das
erste AOT-, Runtime- oder Geraeteproblem. Eine inhaltliche
Bildschirmklassifikation erfordert weiterhin eine reale visuelle Aufnahme.
