# Projektstatus

Interner Entwicklungsmeilenstein: `0.44.0`

Phase: Pre-Alpha

Naechster Roadmap-Task: Phase 11 / KR-4510

Naechstes Phasengate: `v0.46.0` - Abschluss Phase 11

Erster oeffentlicher Produktrelease: `v0.50.0` Alpha

## Fortschritt

- 197 von 236 gepflegten Roadmap-Tasks abgeschlossen: 83,5 %
- Phase-9-Reviewkorrekturen sind implementiert; das eigenstaendige
  achtteilige Homebrew-Korpus und Linux-Evidenz bleiben offen
- Phase 10: 13/13 Tasks im freigegebenen Windows-GDI-Workflow
- Phase 11: 7/16 Tasks
- Phase 12: 0/10 Tasks
- Phase 13: 0/5 Tasks vor dem Alpha-Gate
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
| 9 | Kompatibilitaet und Leistung | Reviewkorrekturen offen |
| 10 | Desktop-GUI und Quellworkflow | 13/13 (Windows-GDI-Scope) |
| 11 | Bootanalyse und Retail-Systemdienste | 7/16 |
| 12 | Interaktive Retail-Runtime und Portintegration | 0/10 |
| 13 | Spielbarer Alpha-Kandidat | 0/5 |

Die Einzelaufgaben und Abhaengigkeiten werden ausschliesslich in
[`docs/TASKS.md`](TASKS.md) gepflegt. `STATUS.md` dupliziert diese Liste nicht.

## Aktueller technischer Stand

Der durchgaengige Pfad verarbeitet Raw-, ELF32-SH-, Projektmanifest- und
validierte GDI-Eingaben ueber Executable Image, Decoder, Kontrollflussanalyse,
Katana-IR und partitionierten C++-Codegen bis zu einem extern buildbaren
Hostprojekt.

Der korrigierte Anwendungsjob-Vertrag Version 4 gibt Codegen und Hostbuild nur
bei vollstaendiger committed Executable-Byteabdeckung und ohne erreichbare
Analyse-Abbruchkante frei. Vorher/nachher-Provenienzpruefung, dauerhafter letzter
Erfolgsstand bei wiederholten Fehlern und echte Windows-/Linux-Prozesslocks
sichern Eingabe und Ausgabe desselben Jobs.

CLI und GUI beobachten denselben sequenzierten hierarchischen Ereignisstrom.
Gesamtfortschritt ist monoton, Einzelschritte besitzen nur bei bekannter Menge
Zaehler, Hostausgabe erreicht die Oberflaeche inkrementell und redigiert. Fehler
und Abbruch benennen den aktiven Schritt.

Die erzeugte Hostanwendung ist kein Platzhalter mehr. `game.exe <disc.gdi>`
laedt die Bootdatei read-only aus der GDI, initialisiert den definierten
Dreamcast-Speicher- und CPU-Zustand, Scheduler und Plattformdienste und ruft
den generierten Einstieg ueber die Runtime-Blocktabelle auf. Der verteilbare
synthetische Nachweis erreicht dabei einen diagnostizierten indirekten
Dispatch ohne KatanaRecomp-CLI. Das ist noch kein Sonic-Boot- oder
Spielbarkeitsnachweis.

Die Windows-GUI reduziert den Nutzerworkflow bewusst auf `.gdi` und
Ausgabeordner. Der komplette Recompile-Lauf bleibt responsiv sichtbar.
Vollstaendige Analysen erzeugen `sourcecode/`, `game.exe` und ein redigiertes
`recompile.log`; unvollstaendige Analysen enden als `partial`, behalten ihre
Metriken und erzeugen bewusst keinen irrefuehrenden Hostbuild.
Projektmanifeste sind dabei nur interne Adapterdetails. Der native Dateidialog
laeuft isoliert, damit fehlerhafte Explorer-Shell-Erweiterungen den
ASan-instrumentierten Hauptprozess nicht beenden. Linux-CLI/Core-Builds sind
ohne X11 moeglich; die Linux-Desktop-GUI ist nicht Teil dieser Freigabe.

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
- synthetische SH-4-Probes und ein Runtime-Vertical-Slice fuer PVR, AICA,
  Maple, Scheduler, DMA, Interrupt sowie Firmware-Handoff; das eigenstaendige
  Homebrew-Korpus bleibt offen
- deterministische Ausfuehrungsprofile, bewachte Speicher-/Dispatch-Fastpaths,
  Inline-Caches, Benchmarks und maschinenlesbare Faehigkeitsmatrix
- versionierter Alpha-ISA-Vertrag mit getrennten Decoder-, IR-, Backend- und
  Runtimezustaenden sowie expliziten Semantikgrenzen und Testanforderungen

## Historisches internes Phasengate: Phase 9 (durch Review invalidiert)

Gate-Commit: `1d7fd7517f160c2327c23dcb9d051a83ccc68ad3`

```text
156/156 Tests bestanden
frischer MSVC-Debug-Build in 81.713 ms
KR_PHASE9_HOMEBREW_HOST_FRAME: 2 Frames, 2 Audio-Puffer, 2 Maple-Transaktionen
silent_failures=0, fallback_count=0, invalidations=1, scheduler_jitter=1
reproduzierbares Basisartefakt und Phase-9-Datenaudit bestanden
```

Der Bericht war ein Windows/MSVC-Lauf und enthielt noch unzureichend abgeleitete
Metriken; KR-3707, KR-3801 bis KR-3806, KR-3808 und KR-4006 wurden wieder
geoeffnet. Es wurden weder Versionierung noch Release-Commit, Tag oder
Veroeffentlichung ausgefuehrt.

## Phase-10-Abschluss nach Nutzerfreigabe

Gate-Commit: `9e9cdb2587b83e4d2da143f94bc2b04be76d151a`

```text
160/160 Tests bestanden
frischer MSVC-Debug-Build in 86.294 ms
GUI-Modellautomatisierung erreicht; keine native Control-E2E-Automatisierung
GUI und CLI: identische Projektidentitaet und 8 identische Kernartefakte
GDI-Modellpfade abgedeckt; native Tastatur-/DPI-/Dialogpfade nicht abgedeckt
Phase-9-Checkpoint, Coverage, Basisartefakt, GUI-Paket und Datenaudit bestanden
```

Der Reviewumfang wurde auf den gewuenschten Windows-GDI-Workflow korrigiert.
Native Fenstererzeugung, Resize, Tastaturnavigation, Hintergrundjob,
Abbruchschutz und GDI-zu-`game.exe` sind automatisiert; Modell- und
Anwendungsdiensttests bleiben getrennt benannt. Die Nutzerfreigabe fuer
KR-4403 wurde am 16.07.2026 erteilt. Es wurden kein Tag und keine
Veroeffentlichung ausgefuehrt.

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

- Phase 11 beginnt mit lokalen, read-only und budgetierten Retail-Debuglaeufen.
- Private GDI-, Ausgabe-, Quell-, Binary-, Pfad-, Hash- und Capture-Daten
  bleiben ausserhalb von Repository, verteilbaren Tests und Gateberichten.
- Pre-Alpha-Gates verwenden weiterhin synthetische Fixtures und frei
  lizenzierte Homebrew-Programme als oeffentlichen Nachweis.
- Jeder Retail-Befund wird auf eine allgemeine, verteilbare Regression reduziert.
- Ein autorisierter read-only Nachlauf nach KR-4502 meldete aggregiert
  `unknown_instructions=0` und weiterhin `unresolved_control_flow=350`; der
  Zustand bleibt korrekt `partial`. Keine Retail-Adresse, kein Pfad, Hash oder
  Artefakt ist Bestandteil des Repositorynachweises.
- Das Alpha-Gate verlangt reproduzierbar:
  `GDI -> Port-Projekt -> game.exe -> SA_ALPHA_PLAYABLE`.
- Alpha erfordert Boot, Video, Eingabe und eine kontrollierbare Spielszene.

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
