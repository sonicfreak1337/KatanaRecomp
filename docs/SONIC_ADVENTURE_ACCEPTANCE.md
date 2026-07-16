# Lokale Sonic-Adventure-Alpha-Akzeptanzstrategie

Dieses Dokument definiert den ersten erlaubten und verpflichtenden lokalen
Sonic-Adventure-Ausfuehrungstest. Sonic Adventure ist vor v0.50.0 kein
Phasentest. Alpha ist erreicht, wenn der offizielle Workflow aus der lokal
bereitgestellten GDI ein externes Port-Projekt und `game.exe` erzeugt,
`game.exe` tatsaechlich startet und reproduzierbar `SA_ALPHA_BOOTED` erreicht.

Unit-, Integrations-, Regressions-, Fuzzing-, Plattform- und Homebrew-Tests
bleiben der oeffentlich verteilbare Nachweis und die Grundlage aller
Pre-Alpha-Gates.

Die Versionsstaende v0.38.0 bis v0.49.0 sind interne Meilensteine ohne
Release-Commit, Tag, Download oder Veroeffentlichung. v0.50.0 Alpha ist der
erste oeffentliche Produktrelease.

## Abgrenzung des Port-Artefakts

KatanaRecomp erzeugt Code, Metadaten, Builddateien und ein ausfuehrbares
Host-Target. Es verteilt oder installiert keine Sonic-Adventure-Assets. Fuer
die lokale Alpha-Pruefung darf `game.exe` die vom Nutzer bereitgestellte GDI
ueber die austauschbare `DiscSource` lesen.

Eine spaetere Installation, die benoetigte Assets einmalig aus der Nutzer-GDI
in ein eigenes Datenverzeichnis extrahiert und danach ohne GDI auskommt, gehoert
in das titelbezogene Folgeprojekt wie `Sonic Adventure Recompiled`. Dessen
Assetlayout und Installer sind nicht Teil des allgemeinen KatanaRecomp-
Compilervertrags. Der generierte Spielcode darf deshalb nicht von einem festen
Assetlayout abhaengen.

## Grundregeln

- Verwendet wird ausschliesslich eine rechtmaessig vom Nutzer lokal
  bereitgestellte GDI.
- Vor KR-4999 darf diese GDI read-only validiert, statisch analysiert,
  rekompiliert und bis zu einem lokalen Hostbuild verarbeitet werden; ihr
  Programm beziehungsweise ihre `game.exe` wird nicht gestartet.
- Spieldaten, extrahierte Dateien, Screenshots, Audioinhalte, Dump-Hashes und
  lokale Pfade duerfen weder committed noch in KatanaRecomp-Release-Artefakte
  aufgenommen werden.
- Fest codierte Sonic-Adventure-Adressen, Remaps, Patches, titelbezogene
  Runtime-Sonderfaelle und willkuerliche spielspezifische Zaehlerstaende sind
  unzulaessig.
- Alle Laeufe besitzen endliche Gastzyklus- und Hostzeitbudgets. Formulierungen
  wie "laeuft weiter" oder "haengt nicht" sind kein Akzeptanzkriterium.
- Oeffentliche CI benoetigt niemals proprietaere Daten. Der lokale Sonic-Test
  laeuft nur auf einem ausdruecklich autorisierten Rechner.
- Der lokale Test wird nicht nach einzelnen Implementierungs-Tasks ausgefuehrt.
  Er ist ausschliesslich Bestandteil von KR-4999.

## Pre-Alpha-Checkpoints

Die folgenden Gates werden ausschliesslich mit synthetischen Fixtures und frei
lizenzierten Homebrew-Programmen ausgefuehrt:

| Interner Meilenstein | Verteilbarer technischer Checkpoint |
| --- | --- |
| Phase 6, v0.31.0 | `KR_PHASE6_PLATFORM_INTEGRATED` |
| Phase 7, v0.34.0 | `KR_PHASE7_GENERATED_RUNTIME_ACTIVE` |
| Phase 8, v0.37.0 | `KR_PHASE8_REPRODUCIBLE_TOOLCHAIN` |
| Phase 9, v0.40.0 | `KR_PHASE9_HOMEBREW_HOST_FRAME` |
| Phase 10, v0.44.0 | `KR_PHASE10_GUI_END_TO_END` |
| v0.45.0 | `KR_V045_ISA_ALPHA_PROFILE_READY` |
| v0.46.0 | `KR_V046_RETAIL_BOOT_SERVICES_READY` |
| v0.47.0 | `KR_V047_NATIVE_HOST_READY` |
| v0.48.0 | `KR_V048_PORT_WORKFLOW_READY` |
| v0.49.0 | `KR_V049_ALPHA_CANDIDATE_READY` |
| Erster oeffentlicher Release, Alpha v0.50.0 | `SA_ALPHA_BOOTED` |

Keiner der `KR_...`-Checkpoints behauptet eine Sonic-Adventure-Ausfuehrung.
Die fruehere lokale Phase-6-GDI-Blockprobe bleibt lediglich als historische
Quellen-, Track-, ISO9660- und Bootblockdiagnose erhalten. Sie wird nicht als
Sonic-Ausfuehrungs- oder Alpha-Nachweis gewertet.

## Maschinenlesbarer Alpha-Bericht

KR-4999 erzeugt einen redigierten, versionierten Bericht nach diesem
Mindestvertrag:

```json
{
  "checkpoint": "SA_ALPHA_BOOTED",
  "gdi_loaded": false,
  "tracks_validated": 0,
  "iso9660_mounted": false,
  "boot_file_loaded": false,
  "port_project_generated": false,
  "host_build_succeeded": false,
  "game_executable_started": false,
  "main_executable_entered": false,
  "executed_blocks": 0,
  "guest_cycles": 0,
  "scheduler_events": 0,
  "gdrom_completions": 0,
  "dma_events": 0,
  "interrupts_delivered": 0,
  "indirect_dispatches": 0,
  "fallbacks": 0,
  "silent_failures": 0,
  "booted": false
}
```

Felder und Schwellenwerte duerfen mit einer Schema-Version erweitert werden.
Berichte duerfen keine unredigierten Hostpfade, Spieldaten, Disc- oder
Dateihashes enthalten.

## Alpha-Gate v0.50.0

KR-4999 muss nachweisen:

1. Die lokale GDI wird read-only ueber den offiziellen Quellenworkflow geladen;
   Tracks, ISO9660 und Bootdatei werden reproduzierbar validiert.
2. Analyse, IR, deterministischer Codegen und Hostbuild erzeugen ausserhalb des
   KatanaRecomp-Quellbaums ein Port-Projekt mit `game.exe`.
3. `game.exe` wird direkt als eigenstaendige Hostanwendung gestartet, nicht nur
   als interner KatanaRecomp-Test oder CLI-Simulator.
4. Das Hauptprogramm wird betreten. Scheduler, GD-ROM, DMA, Interrupts und
   generischer Dispatch zeigen innerhalb des festen Budgets messbaren,
   deterministischen Fortschritt.
5. Zwei identische Laeufe erreichen `SA_ALPHA_BOOTED` mit denselben
   deterministischen Kernmetriken und `silent_failures == 0`.
6. Ein budgetbedingt oder kontrolliert beendeter Fehllauf erzeugt einen
   verwertbaren, redigierten Diagnosebericht.
7. Die verteilbaren Debug-/Release-Builds, die vollstaendige Regression und die
   erforderliche Windows-/Linux-CI bestehen ohne proprietaere Eingaben.
8. Repository, Release-Staging und Berichte enthalten keine Spiel-, Asset-,
   Capture-, Hash- oder lokalen Pfaddaten.

Ein erster Frame, Hauptmenue, spielbare Szene, vollstaendige Grafik- oder
Audiokorrektheit und spielbare Performance sind keine Alpha-Pflichtkriterien.
Sie gehoeren zum Beta-Gate. Visuelle Ausgabe darf lokal als zusaetzlicher,
nicht zu committender Diagnosehinweis dienen.

## Gate-Workflow und Review-Stop

KR-4999 ist der letzte Alpha-Gate-Vorbereitungstask. Erst dort werden die
gesammelten Alpha-Tests vervollstaendigt, frische Builds und Regression
ausgefuehrt und Sonic Adventure erstmals gestartet. Nach dem fertigen
Gate-Bericht wird zwingend fuer das Nutzerreview gestoppt.

KR-5000 darf erst nach ausdruecklicher Nutzerfreigabe beginnen. Ohne diese
Freigabe erfolgen keine Versionsaenderung, kein Release-Commit, kein Tag und
keine Veroeffentlichung. Verlangt das Review Aenderungen, wird KR-4999 nach den
Korrekturen vollstaendig wiederholt.
