# Port-Projektexport und Originaldisc-Installation

## v0.49: Executable-First als Bring-up-Standard

Der Entwicklungsworkflow trennt die einmalige private Discverarbeitung von
wiederholter Analyse und Codegeneration:

```powershell
katana-recomp extract-boot-executable .\disc\game.gdi `
  --output .\private\game-boot

katana-recomp port-executable `
  .\private\game-boot\boot.katana-executable `
  --output .\private\game-port `
  --target-name game `
  --console-profile europe-pal `
  --game-project .\private\game-project.katana-game-project `
  --game-entry-handoff .\private\game-boot\game-entry.katana-handoff
```

`boot.katana-executable` bindet Projekt-, Content- und Bootbyteidentitaet. Die
danebenliegende `boot.bin` bleibt private Retailquelle ausserhalb des
Repositorys. Der Export validiert sie, kopiert sie aber weder in generierte
Quellen noch in das Portpaket. Stabile AOT-Partitionen, Metadaten und der
Hostbuild werden im privaten Workspace gecacht; `user-data` bleibt beim
Republishing erhalten. Bei unveraendertem, verifiziertem
`port-executable`-Schluessel kann der Whole-Export-Cache auch Analyse,
IR-Lowering und Emission ueberspringen. Hostconfigure, Spieltarget, Packaging
und Publish laufen weiterhin.

Dieser DirectBootExecutable-Pfad ist der schnelle Bring-up-Standard.
NativeDiscBoot ueber die `.gdi` bleibt das finale Genauigkeits- und
Kompatibilitaetsgate. Die Nutzerinstallation bleibt fuer beide Portarten
unveraendert discbasiert.

Buildprofil, Compiler und Linker werden ausserhalb des verteilbaren Projekts
gewaehlt:

```text
KATANA_PORT_BUILD_PROFILE=bringup|gate
KATANA_PORT_CXX_COMPILER=msvc|clang-cl
KATANA_PORT_LINKER=default|msvc|lld
KATANA_HOST_BUILD_GENERATOR=Ninja
KATANA_HOST_BUILD_JOBS=<N>
KATANA_PORT_CODEGEN_JOBS=<N>
KATANA_RUNTIME_PREFIX=<installiertes Runtime-SDK, optional>
KATANA_RUNTIME_BUILD_TARGETS=<optimierter lokaler Buildtree-Export, optional>
KATANA_RUNTIME_ROOT=<Quellbaum-Fallback, optional>
```

Weitere Details stehen in
[`EXECUTABLE_FIRST_DEVELOPMENT.md`](EXECUTABLE_FIRST_DEVELOPMENT.md) und
[`PORT_BUILD_PROFILES.md`](PORT_BUILD_PROFILES.md).

## RuntimeOnly-NativeDisc-Bring-up

Der `port`-Aufruf akzeptiert den opt-in Modus
`--analysis-mode runtime-only` nur zusammen mit `--game-project` fuer den
vollstaendigen NativeDisc-Produktport. Der Default bleibt `platform`.
RuntimeOnly setzt fuer die Bootanalyse konservativ `GuestCallAbi::Unknown` und
umgeht damit die blockierende SuperHC-FunctionValue-/Candidate-Resolution;
Analyse und Codegen erzeugen weiterhin nativen AOT-Code. Indirekte Aufrufe
verwenden RuntimeOnly-Dispatch ueber eine exakte statische Guest->Host-Tabelle.
Stop-on-miss und typed abort bleiben aktiv; Interpreter, JIT, Runtime-Decoder
und geratenen Ziele sind ausgeschlossen.

Der Whole-Export-Cache bindet den Analysemodus und verwendet keinen Eintrag
des anderen Modus. Der erste vollstaendige Hostbuild kompilierte 83
Translation Units und linkte `game.exe`; der publizierende Sonic-PAL-
RuntimeOnly-Lauf war nach `19,077 s` erfolgreich mit `1.631` nativen
Funktionen, `41` Partitionen, `3` latenten AOT-Modulen, `3.967` RuntimeOnly-
Stellen und `0` unresolved. Das verteilbare Paket enthaelt keine
Retailsektoren. Der Build-/Export-Gate ist bestanden; der beaufsichtigte
Start bis mindestens zum Memory-Card-Screen bleibt das naechste
Produktgate.

RuntimeOnly-v16 erreichte im realen Sonic-PAL-Lauf nach etwa `4,014 s`
erstmals den echten SEGA-Screen sowie echte Guest- und Presented-Frames bei
etwa `9,16 MHz`. Das aktuelle Native-AOT-Emissionsprofil ist `25` bei
AOT-Partitionsschema `5`. Der Lauf lief bis etwa `27 s` weiter und stoppte danach korrekt fail-closed am
generischen Fehler `missing-aot`. Candidate-Resolution und PlatformAbi-
Optimierungen bleiben deferred; Stop-on-miss und typed abort bleiben aktiv.

## Opt-in Portbuild-Telemetrie

`port` und `port-executable` akzeptieren
`--telemetry-jsonl <datei>`. Ohne diese Option startet weder ein Writerthread
noch eine Ressourcenabtastung. Mit der Option sammelt Katana ein versioniertes
Manifest sowie Fortschritts-, Prozessbaumressourcen- und Terminalrecords:

```powershell
katana-recomp port .\disc\game.gdi `
  --output .\port `
  --target-name game `
  --telemetry-jsonl .\measurements\cold-build.jsonl
```

Die Zieldatei muss ausserhalb von Quell-, Ausgabe-, Runtime-, Workspace-,
Publishjournal- und anderen geschuetzten Baeumen liegen und darf weder GDI,
Track, Spielprojekt, Handoff, Runtimepayload noch Publishlock aliasieren.
Reservierte Windows-Geraetenamen werden abgelehnt. Katana schreibt in eine
exklusiv erzeugte temporaere Nachbardatei und ersetzt das Ziel erst nach dem
terminalen Flush atomar. Eine explizit angeforderte, unvollstaendige oder
nicht publizierbare Telemetrie laesst den CLI-Lauf fail-closed scheitern.

Die Datei ist deshalb ein terminales Messartefakt und keine Live-Tail-
Schnittstelle. Laufender Fortschritt bleibt ueber die bestehende
Konsolenausgabe sichtbar. Ein gesetztes `telemetry_complete=false`, verlorene
Beobachtungen oder eine als unvollstaendig qualifizierte Prozessbaumabfrage
duerfen nicht als Performancebeweis verwendet werden.

Der generische Exportpfad uebersetzt das validierte Dreamcast-Bootprogramm in
statischen nativen AOT-Code. Ein verteilbares Portpaket enthaelt keine Raw-,
Audio- oder sonstigen kommerziellen Discsektoren:

```powershell
katana-recomp port .\disc\game.gdi --output .\port --target-name game --console-profile europe-pal
```

Die GDI und alle Tracks werden read-only geoeffnet. Ohne optionales externes
`GameProjectDefinition` bleiben Analyse, Katana-IR, Optimierung und
C++-Partitionierung spielagnostisch. Ein externes Spielprojekt darf seine
hashgebundenen Titeladressen, Symbole und Hooks ueber
`PortExportOptions::game_project` einbringen; diese Daten werden dadurch nicht
Teil des generischen Katana-Kerns. Die Originaldateien werden weder veraendert
noch geloescht.

Fuer den executable-first CLI-Pfad transportiert
`--game-project <datei.katana-game-project>` ein `GameProjectArtifact`
Format 1. Es bindet Identitaet, exakte Funktionsgrenzen, Jump- und
Callbacktabellen, Runtime-AOT-Templates, Symbole, Codeidentitaeten und
optionale Bootkonfiguration durch Payload- und Gesamtartefakt-SHA-256. Native
Hooks und private Handoffprovider werden nicht serialisiert. Die
vollstaendige Definition steuert Analyse und AOT; ohne native Hooks bindet der
generierte Produktport nur eine reduzierte Runtimeidentitaet samt
Bootkonfiguration und optionalem `--game-entry-handoff`. Beide
Artefaktidentitaeten sind Teil des Whole-Export-Schluessels.

`--console-profile` waehlt die nachgebildete Konsolenkonfiguration explizit:
`japan-ntsc`, `north-america-ntsc`, `europe-pal` oder `vga`. Ohne Option gilt
der dokumentierte Japan-NTSC-Default. Disc-Areasymbole sind nur
Kompatibilitaetsmetadaten und werden nicht als Konsolenregion interpretiert.

## Verteilbares Layout

```text
port/
  game.exe
  content/
    game.katana-install       Hash-, Boot- und Trackgeometrie-Recipe
    game.katana-install.json  Bindung von Recipe und AOT-Executable
  runtime/
    runtime-dependencies.json
  user-data/
    content/                  anfangs leer; lokaler Retailcache nach Installation
  generated/                 deterministische AOT-Quellen und Metadaten
  INSTALL_ORIGINAL_DISC.txt
  .gitignore
```

`game.katana-install` enthaelt Recipe-Version 2, Jobgeneration,
Descriptor-SHA-256, Boot-SHA-256, Contentidentitaet und pro Track Nummer, LBA,
Typ, Sektorgroesse, Offset, Sektoranzahl und SHA-256. Sie enthaelt keine
Discbytes, Tracknamen, absoluten Pfade oder privaten Hostinformationen.

## Installation und Start

Jeder Nutzer stellt einmalig die eigene rechtmaessig vorhandene Originaldisc
bereit:

```powershell
.\game.exe --install-disc D:\eigene-disc\game.gdi
.\game.exe
```

Der Installer validiert Descriptor, vollstaendige Trackliste, LBAs, Typen,
Sektorformate, Offsets, Groessen, Track-SHA-256, Contentidentitaet und
Bootdatei. Erst danach wird atomar
`user-data/content/game.katana-disc` erzeugt. Dieser Cache erhaelt den
vollstaendigen logischen LBA-Raum einschliesslich Raw- und Audiosektoren und
darf nicht verteilt werden. Er ist durch `.gitignore` geschuetzt und wird von
Repository-, CI-, Release- und Paketaudits als Retailinhalt abgelehnt.

Disc-Pack-Format 2 berechnet seine Content-Root nicht aus vorab uebernommenen
Dateimetadaten, sondern aus kanonischer Trackgeometrie und den SHA-256-Werten
der tatsaechlich geschriebenen Raw-Chunks. Der Installer vergleicht diese
waehrend des Schreibens neu hergeleitete Root mit der Recipe; bei einer
zwischenzeitlich veraenderten Quelle wird nur das Staging verworfen und kein
Cache publiziert. Beim Oeffnen rekonstruiert die Runtime Trackintegritaet und
Content-Root erneut aus dem Chunkindex, bevor Gastcode laufen darf.

Der normale Start verwendet ausschliesslich diesen lokalen Cache. Ein fehlender
oder manipulierter Cache, eine ersetzte Recipe oder eine nicht exakt passende
Originaldisc scheitert vor Gastcode. `--content <cache>` waehlt fuer lokale
Diagnosen einen anderen Pack; `--gdi-debug <disc.gdi>` bleibt ein expliziter
Entwicklungsmodus.

## Recompilation statt Emulation

Der Programmcode aus der validierten Bootdatei wird analysiert, in Katana-IR ueberfuehrt,
optimiert, in native C++-Translation-Units partitioniert und als x64-Executable
kompiliert. Dreamcast-Plattformdienste werden ueber die versionierte native
Runtime bereitgestellt. Der lokal installierte Disc-Cache ist nur die
unveraenderliche Datenquelle fuer Disczugriffe; er ersetzt weder die statische
Codeuebersetzung noch fuehrt er SH-4-Code durch Emulation aus.

## Publish-, Identitaets- und Quellschutzvertrag

Export und Hostbuild laufen in einem Stagingverzeichnis. Erst wenn AOT-EXE,
Recipe, Installationsmanifest und Runtimevertrag dieselbe Jobgeneration tragen,
wird der Portordner atomar veroeffentlicht. Ein fehlgeschlagener Job ersetzt
keinen letzten erfolgreichen Stand.

Nur Katana-Artefakte unter `generated/` werden bei erneutem Codegen ersetzt.
Unbekannte und handgeschriebene Dateien bleiben erhalten; symbolische Links in
verwalteten Pfaden werden abgelehnt. Generische Provenienzmetadaten enthalten
nur Rollen, relative Pfade, Format-/ABI-Daten, Groessen, Generationen und
SHA-256-Werte. Ein bewusst gebundenes externes Spielprojekt erhaelt zusaetzlich
`metadata/game-project.json` mit seinen eigenen Adressen, Symbolen und
Identitaetsbindungen.

Der Vertrag gilt fuer alle unterstuetzten Dreamcast-GDI-Titel. Private Spiele
dienen ausschliesslich als lokale End-to-End-Fixtures; verteilbare und CI-Tests
verwenden synthetische oder frei lizenzierte Inhalte.

Bewusste Loadergrenze: WinCE-Layouts mit getrenntem erstem Bootsektor und
gescrambelte Nicht-GD-ROM-Bootdateien werden derzeit mit den stabilen Fehlern
`unsupported-dreamcast-wince-boot-layout` beziehungsweise
`unsupported-scrambled-non-gdrom-boot-layout` abgelehnt. Der Export behauptet
fuer diese Layouts keine native AOT-Faehigkeit und descrambelt keine Daten mit
uebernommenem Emulatorcode.
