# KatanaRecomp

**Aktuelle Version: 0.49.3**

KatanaRecomp ist ein C++20-Framework fuer die statische Rekompilierung von
Dreamcast-SH-4-Programmen in native PC-Ports. Das Projekt ist Pre-Alpha:
Einzelne native Grafik-, Audio-, Eingabe-, Speicher- und Plattformpfade sind
vorhanden, die vollstaendige Spielabdeckung ist noch kein Releaseversprechen.

## Produktgrenzen

Der Produktpfad ist strikt nativ und fail-closed:

- SH-4-Code wird analysiert und als natives C++ erzeugt.
- Grafik, Audio/Movie, Eingabe, Dateien, Saves und Plattformdienste laufen
  ueber native PC-Implementierungen.
- Unbewiesener Code, unbekannte Ziele und unvollstaendige Hardwaresemantik
  bleiben sichtbar offen oder enden typisiert.
- Es gibt keinen Emulator, keinen SH-4-Interpreter, keinen JIT, kein Runtime-
  Decoding, keine Kandidatenpromotion und keine geratenen Call-Ziele.
- PVR-/TA-/ASIC-/MMIO-Replay und historische Dreamcast-Geraetepfade gehoeren
  nicht in Produktlink oder Produktruntime. Sie koennen ausschliesslich als
  internes Offline-Orakel fuer Analyse und Vergleich dienen.

## Architektur

Der Port ist in drei Schichten geteilt:

1. **KatanaRecomp** analysiert SH-4-Instruktionen, Kontrollfluss und
   Datenprovenienz, erzeugt die Katana-IR und emittiert statische native
   Quellen.
2. **KatanaRuntime** stellt titelunabhaengige native PC-Dienste und die
   verifizierten AOT-/Runtime-Grenzen bereit.
3. **Externe Spielprojekte** liefern private, identitaetsgebundene
   Funktionsgrenzen, Overlays, Jump-/Callbacktabellen, Symbole, Manifeste,
   Provider und AOT-Konfiguration. Titeladressen und Retaildaten bleiben
   ausserhalb des generischen Kerns.

Weitere Architekturhintergruende stehen in
[`docs/ARCHITECTURE_V049.md`](docs/ARCHITECTURE_V049.md).

## Entwicklungsworkflow

Wir unterscheiden einen grossen, seltenen Strict-Product-Lauf und einen
kleinen, schnellen Native-Bring-up-Loop. Beide verwenden dieselbe belegte
Provenienz; der kleine Loop erzeugt keinen neuen Analysezustand neben dem
stabilen AOT-Pack.

### Strict-Product-Loop

Diesen Lauf starten wir nur, wenn sich Analysegrundlagen oder AOT-Abdeckung
geaendert haben: Function Boundaries, neue Overlays, FunctionMap,
AOT-wirksame Manifestdaten, Analyzer, Codegen oder statische
Provider-/AOT-Daten.

```text
strikte Analyse
  -> vollstaendige CFG-/Owner-/Hardware-Closure
  -> AOT-Code, Overlays und native Allowlist
  -> stabiler sonic.aotpack (oder entsprechendes Projekt-Pack)
  -> vollstaendiger Export und Produktgate
```

### Native-Bring-up-Loop

Wenn das Pack stabil ist, bleibt die Analyse unangetastet:

```text
gleiches AOT-Pack laden
  -> Replay reproduzieren
  -> erste Divergenz oder erster Crash
  -> genaues Runtime-/Adapter-/Manifest-Problem erfassen
  -> inkrementell bauen
  -> dasselbe Pack erneut laden und replayen
```

Der Bring-up-Dispatcher darf ausschliesslich aktive, vorab kompilierte,
identitaets- und generationgebundene Ziele aus der Allowlist ausfuehren. Ein
Miss ist `UnknownCompiledTarget`; es gibt keinen Interpreter-, JIT-,
Materializer- oder Guessing-Fallback. Die vollstaendige Regelung steht in
[`docs/NATIVE_BRINGUP_WORKFLOW.md`](docs/NATIVE_BRINGUP_WORKFLOW.md).

### Evidence statt impliziter Freigaben

Runtimebeobachtungen, Logs und Disassembly sind wertvolle Evidenz, aber kein
automatischer Abschlussbeweis. Sie werden einem konkreten Owner, Callsite,
Target, Pack, Modul, Generation und einer Fortsetzung zugeordnet. Die
Evidenzklassen sind:

```text
Observed -> Candidate -> Proven
                    \-> RuntimeContract -> Strict Product
```

`Observed` und `Candidate` duerfen Strict nicht schliessen. `Proven` darf
eine statische Closure schliessen. `RuntimeContract` darf nur ueber einen
validierten, vorab kompilierten aktiven RuntimeOnly-Block in den Produktlauf
einfliessen. Unaufgeloeste oder widerspruechliche Evidenz bleibt offen.
Ein explizit authorierter `Candidate` darf ausschliesslich im nicht
releasefaehigen Bring-up laufen, nachdem Katana das konkrete Source-/Target-
Paar samt Bytes, Owner, Pack und Generation erneut exakt validiert hat; sein
offener Proof bleibt dabei unveraendert sichtbar.

Die erste Divergenz ist wichtiger als der terminale Crash. Diagnostik bleibt
kompakt, gebunden und standardmaessig still; tiefe Telemetrie ist ein
expliziter Offline-/Diagnosemodus und gehoert nicht in den Frame-Hotpath.

## Bauen

Voraussetzungen sind CMake 3.25+, ein C++20-Compiler und ein unter Windows
unterstuetzter Generator (typischerweise MSVC/Ninja).

Der kanonische Entwicklungsbuild lautet:

```powershell
.\tools\build-katana-cli.ps1
```

Alternativ kann ein Standard-CMake-Preset verwendet werden:

```powershell
cmake --preset msvc-relwithdebinfo
cmake --build --preset msvc-relwithdebinfo --target katana-recomp
```

Die installierbaren nativen Ziele sind `KatanaRecomp::aot_runtime` und
`KatanaRecomp::native_port_runtime`. Interne Diagnose- und historische
Geraeteflaechen sind kein Teil des oeffentlichen Produkt-SDK.

## CLI-Schnellstart

Die Beispiele verwenden Platzhalter und enthalten keine titel- oder
adressspezifischen Daten:

```powershell
$cli = '.\build-contextual-dirty\katana-recomp.exe'

& $cli disasm .\input\program.bin 0x8C010000
& $cli analyze .\input\project.toml
```

Fuer executable-first Bring-up wird ein einmalig extrahiertes, unveraender-
liches Executable-Artefakt mit einem privaten Spielprojekt und einem
identitaetsgebundenen Entry-Handoff exportiert:

```powershell
& $cli extract-boot-executable .\input\game.gdi `
  --output .\work\boot

& $cli port-executable .\work\boot\boot.katana-executable `
  --output .\work\port `
  --target-name GamePort `
  --game-project .\private\game.katana-game-project `
  --game-entry-handoff .\work\boot\game-entry.katana-handoff
```

Die vollstaendige CLI- und Handoff-Dokumentation steht in
[`docs/EXECUTABLE_FIRST_DEVELOPMENT.md`](docs/EXECUTABLE_FIRST_DEVELOPMENT.md)
und [`docs/PORT_EXPORT.md`](docs/PORT_EXPORT.md).

## Wichtige Dokumente

- [Native-Bring-up-Workflow](docs/NATIVE_BRINGUP_WORKFLOW.md)
- [Native-Port-Produktvertrag](docs/NATIVE_PORT_PRODUCT_CONTRACT.md)
- [Analyse-Workflow](docs/AGENT_ANALYSIS_WORKFLOW.md)
- [RuntimeOnly-Dispatch](docs/RUNTIME_ONLY_DISPATCH.md)
- [Port-Build-Profile](docs/PORT_BUILD_PROFILES.md)
- [Reproduzierbare Artefakte](docs/REPRODUCIBLE_ARTIFACTS.md)
- [System-Replay und Differenzdiagnose](docs/SYSTEM_REPLAY.md)
- [Dispatch-Diagnostik](docs/DISPATCH_DIAGNOSTICS.md)
- [Roadmap](ROADMAP.md)
- [Aktueller Stand](docs/CURRENT_STATE.md)
- [Historisches Statusledger](docs/STATUS.md)
- [Tasks](docs/TASKS.md)
- [Lizenz-/Referenzprovenienz](docs/REFERENCE_PROVENANCE.md)

Historische ABI-Staende, Run-Tagebuecher und alte Produktversuche gehoeren in
die jeweiligen Status- und Release-Dokumente, nicht in diese Einstiegsseite.

## Repository-Layout

```text
include/   oeffentliche C++-Schnittstellen
src/       Analyzer-, Codegen- und Runtime-Implementierungen
tests/     fokussierte Vertrags- und Regressionstests
tools/     reproduzierbare Build-, Analyse- und Gate-Skripte
docs/      Architektur-, Workflow-, Runtime- und Sicherheitsdokumente
cmake/     Build-, Paket- und ABI-Konfiguration
```

Private Titelprojekte, Disassemblies, Spielpatches und erzeugte Retail-
Artefakte liegen ausserhalb dieses oeffentlichen Repositorys.

## Status und rechtlicher Rahmen

KatanaRecomp ist Pre-Alpha und nicht als fertiger Dreamcast-Port veroeffent-
licht. Der aktuelle Entwicklungsstatus und bekannte Gates stehen in
[`docs/CURRENT_STATE.md`](docs/CURRENT_STATE.md) und
[`ROADMAP.md`](ROADMAP.md).

Dieses Repository enthaelt keine BIOS-Dateien, Disc-Images, Boot-Executables,
Spiele, Firmware oder kommerziellen Assets. Dreamcast und zugehoerige Marken
bleiben Eigentum ihrer jeweiligen Rechteinhaber. Externe Referenzen muessen
die Regeln in [`docs/REFERENCE_PROVENANCE.md`](docs/REFERENCE_PROVENANCE.md)
einhalten.
