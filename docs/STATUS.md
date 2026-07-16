# Projektstatus

Interner Entwicklungsmeilenstein: `0.40.0`

Phase: Pre-Alpha

Naechster Roadmap-Task: Nutzerreview von `KR-4402`; danach `KR-4403`

Naechstes Phasengate: `v0.44.0` - Abschluss Phase 10

Erster oeffentlicher Produktrelease: `v0.50.0` Alpha

## Fortschritt

- 198 von 231 gepflegten Roadmap-Tasks abgeschlossen: 85,7 %
- 104 von 133 Tasks auf dem Weg vom Dreamcast-Plattformstart bis zum Alpha-Gate abgeschlossen: 78,2 %
- Phasen 1 bis 9 abgeschlossen
- Phase 9: 16/17 Tasks; das optionale lokale Firmware-LLE-Smoke-Profil wurde nicht angefordert
- Phase 10: 12/13 Tasks; Gate-Vorbereitung bestanden, Freigabereview offen
- Phase 11: 0/25 Tasks
- Alpha-Gate noch nicht erreicht

| Phase | Inhalt | Stand |
|---|---|---:|
| 1 | SH-4 Integer-Kern | 31/31 |
| 2 | Loader und Analyse | 21/21 |
| 3 | Katana-IR | 14/14 |
| 4 | Runtime-Grundlage | 18/18 |
| 5 | SH-4 FPU | 10/10 |
| 6 | Dreamcast-Plattform | 29/29 |
| 7 | Codegen und Dispatch | 21/21 |
| 8 | Werkzeuge und Qualitaet | 26/26 |
| 9 | Kompatibilitaet und Leistung | 16/17 (Pflichtumfang abgeschlossen) |
| 10 | Desktop-GUI und Quellworkflow | 12/13 (Review vor Freigabe) |
| 11 | Alpha-Integration und Haertung | 0/25 |

Die Einzelaufgaben und Abhaengigkeiten werden ausschliesslich in
[`docs/TASKS.md`](TASKS.md) gepflegt. `STATUS.md` dupliziert diese Liste nicht.

## Aktueller technischer Stand

Der durchgaengige Pfad verarbeitet Raw-, ELF32-SH-, Projektmanifest- und
validierte GDI-Eingaben ueber Executable Image, Decoder, Kontrollflussanalyse,
Katana-IR und partitionierten C++-Codegen bis zu einem extern buildbaren
Hostprojekt.

Abgeschlossen sind unter anderem:

- SH-4-Integer-, Systemregister-, Delay-Slot- und FPU-Grundsemantik
- regionbasierter Dreamcast-Speicherbus mit RAM, VRAM, AICA-RAM, BIOS, Flash,
  MMIO, Ausrichtungsfehlern, Tracing und Watchpoints
- Exceptions, Interrupts, Scheduler, TMU, RTC, DMA und Medienuhr
- Maple, Controller, VMU, PVR-Minimalpfad, AICA-HLE und GD-ROM/ISO9660/GDI
- modulare Runtime-/Backend-/Block-ABI, indirekter Dispatch, kontrollierter
  Fallback, MMU-/Zustandswaechter und Codeinvalidierung
- Manifest v2, stabile CLI/JSON-Berichte, Analyseanweisungen, Provenienz,
  Diagnostik, Systemreplays und reproduzierbarer Port-Projektexport
- lokale Debugqualitaet mit statischer Analyse, ASan, Coverage, Fuzzing,
  Differentialtests sowie Referenz- und Lizenzaudits
- synthetisches Homebrew-Pflichtkorpus mit zusammenhaengendem PVR-, AICA-,
  Maple-, Scheduler-, DMA-, Interrupt- und Firmware-Handoff-Nachweis
- deterministische Ausfuehrungsprofile, bewachte Speicher-/Dispatch-Fastpaths,
  Inline-Caches, Benchmarks und maschinenlesbare Faehigkeitsmatrix

## Letztes internes Phasengate: Phase 9

Gate-Commit: `1d7fd7517f160c2327c23dcb9d051a83ccc68ad3`

```text
156/156 Tests bestanden
frischer MSVC-Debug-Build in 81.713 ms
KR_PHASE9_HOMEBREW_HOST_FRAME: 2 Frames, 2 Audio-Puffer, 2 Maple-Transaktionen
silent_failures=0, fallback_count=0, invalidations=1, scheduler_jitter=1
reproduzierbares Basisartefakt und Phase-9-Datenaudit bestanden
```

Der Gate-Bericht nennt `next_phase: 10`. Es wurden weder Versionierung noch
Release-Commit, Tag oder Veroeffentlichung ausgefuehrt.

## Aktueller Review-Checkpoint: Phase 10 / KR-4402

Gate-Commit: `024990b0c531287bd5de7c6450d3b2e1247e10f4`

```text
159/159 Tests bestanden
frischer MSVC-Debug-Build in 82.133 ms
KR_PHASE10_GUI_END_TO_END erreicht
GUI und CLI: identische Projektidentitaet und 8 identische Kernartefakte
GDI-Positiv-, Fehler- und Recovery-Pfade sowie Tastatur/DPI-Recovery abgedeckt
Phase-9-Checkpoint, Coverage, Basisartefakt, GUI-Paket und Datenaudit bestanden
```

`KR-4403` wurde nicht begonnen. Der unveraenderte Gate-Bericht verlangt vor
der internen Meilenstein-Freigabe das ausdrueckliche Nutzerreview. Es wurden
keine Versionierung, kein Release-Commit, kein Tag und keine Veroeffentlichung
ausgefuehrt.

## Letztes Release-Gate: v0.37.0

Release-Commit und Tag: `3febb53` / `v0.37.0`

```text
100% tests passed out of 151
MSVC Debug + AddressSanitizer + statische Analyse + Coverage
Format-, Qualitaets-, Referenz- und Lizenzaudit bestanden
prozessisolierter Fuzz-Kurzlauf bestanden
reproduzierbares KatanaRecomp-0.37.0-dev.zip erzeugt
```

Die fruehere Zuordnung eines konkreten ZIP-Hashs zum nachtraeglich erzeugten
Tag-Commit wurde zurueckgezogen, weil das interne `source_commit` des damaligen
ZIPs nicht gegen den Tag verifiziert wurde. Tag-Provenienz und externer
Artefakthash werden kuenftig ohne Selbstbezug getrennt geprueft und publiziert.

Der feste Fuzzlauf verwendet Seed `0x3703` und jeweils 256 Iterationen fuer
Decoder, Loader, IR und Runtime. Kandidaten laufen in Kindprozessen, sodass
auch Sanitizer- und Prozessabbrueche reduziert werden koennen. Minimierte
Hex-Eingaben sind direkt wiedergebbar.

Release-Builds und GitHub-CI bleiben gemaess Pre-Alpha-Workflow bis zum
Alpha-Gate `v0.50.0` deaktiviert. Der fruehere CI-Badge wurde deshalb entfernt.

## Sonic-Adventure-Strategie

- Vor `KR-4999` wird Sonic Adventure nicht ausgefuehrt.
- Pre-Alpha-Gates verwenden synthetische Fixtures und frei lizenzierte
  Homebrew-Programme.
- Eine lokale GDI darf read-only validiert, analysiert, rekompiliert und bis
  `game.exe` gebaut, aber vor `KR-4999` nicht gestartet werden.
- Das Alpha-Gate verlangt erstmals reproduzierbar:
  `GDI -> Port-Projekt -> game.exe -> SA_ALPHA_BOOTED`.
- Ein erster Frame oder Gameplay gehoert nicht zum Alpha-Kriterium.

Der vollstaendige Vertrag steht in
[`docs/SONIC_ADVENTURE_ACCEPTANCE.md`](SONIC_ADVENTURE_ACCEPTANCE.md).

## Arbeits- und Gate-Workflow

- Implementierungs-Tasks erhalten passende Unit-, Integrations- und
  Regressionstests; der vollstaendige Phasenlauf erfolgt gesammelt am Gate.
- Bis zum Alpha-Gate wird ausschliesslich der Debug-Build verwendet.
- Auf der Festplatte bleibt genau ein Buildverzeichnis: `build-current/`.
- Es bleibt genau ein Backup der neuesten Version erhalten.
- Roadmap, Tasks und Changelog werden bei jedem Task gepflegt; `STATUS.md` wird
  an jedem Gate aktualisiert.
- Nach der Gate-Vorbereitung wird vor Versionierung und Phasenwechsel fuer das
  Nutzerreview gestoppt.
- Commits und Pushes gehen direkt auf `main`.

## Aktuelle Grenzen

- Der SH-4-Befehlssatz und die Dreamcast-Hardwaremodelle sind noch nicht
  vollstaendig.
- FPU-Exception-Flags, weitere MMIO-/Interruptquellen und tiefere
  Cache-/MMU-Semantik bleiben auszubauen.
- PVR und AICA bilden belastbare Minimal-/HLE-Pfade, keine vollstaendige
  Hardwareemulation; ARM7-LLE ist nicht implementiert.
- Indirekte Zielanalyse bleibt konservativ auf beweisbare Ziele und begrenzte
  Jump Tables beschraenkt.
- Retail-Kompatibilitaet, Desktop-GUI, native Portintegration und Alpha-CI
  werden in den Phasen 10 und 11 weitergefuehrt.

## Massgebliche Dokumente

- [`ROADMAP.md`](../ROADMAP.md): Phasen, Versionen und Gates
- [`docs/TASKS.md`](TASKS.md): Taskstatus und Akzeptanzbedingungen
- [`CHANGELOG.md`](../CHANGELOG.md): veroeffentlichte und unveroeffentlichte Aenderungen
- [`docs/releases/v0.37.0.md`](releases/v0.37.0.md): letzter Releasebericht
- [`docs/CODEX_HANDOFF.md`](CODEX_HANDOFF.md): verbindliche Arbeitsregeln
