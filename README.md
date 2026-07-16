# KatanaRecomp

Aktueller interner Pre-Alpha-Meilenstein: `0.46.0`

KatanaRecomp ist ein unabhaengiges C++20-Framework fuer die statische
Rekompilierung von Sega-Dreamcast-SH-4-Code. Das Projekt ist kein Emulator,
kein ISO-Loader und kein Paket fuer kommerzielle Spieldaten.

BIOS-Dateien, Disc-Images, urheberrechtlich geschuetzte Assets und aus
kommerziellen Spielen erzeugter Code gehoeren nicht in dieses Repository.

## Status

Der aktuelle Pfad verarbeitet Raw-, ELF32-SH-, Projektmanifest- und validierte
GDI-Eingaben bis zu partitioniertem C++, einer zentralen Dreamcast-Runtime und
einem extern buildbaren Hostprojekt:

```text
Eingabe
  -> Executable Image
  -> SH-4-Decoder und Kontrollflussanalyse
  -> Katana-IR und Optimierung
  -> partitionierter C++-Codegen
  -> Runtime-/Plattformdienste
  -> natives Hostprojekt
```

Der aktuelle kumulative Debug-Vertrag umfasst **169 automatische Tests**.
Phase 10 ist
fuer den freigegebenen Windows-Workflow abgeschlossen: eine `.gdi` waehlen,
einen Ausgabeordner waehlen und den Analyse-/Buildzustand sichtbar verfolgen.
`sourcecode/` und `game.exe` entstehen nur bei vollstaendig bewiesenem
Kontrollfluss; unvollstaendige Analysen enden ehrlich als `partial`. Breitere
Kompatibilitaet und die Linux-GUI sind kein Bestandteil dieser internen
Windows-Freigabe.

Der genaue aktuelle Stand steht in [docs/STATUS.md](docs/STATUS.md), die
langfristige Planung in [ROADMAP.md](ROADMAP.md).

Die lokale, nicht versionierte Desktopanwendung wird direkt gestartet mit:

```powershell
.\KatanaRecomp-GUI.exe
```

`katana-file-dialog.exe` und der lokale, nicht versionierte Ordner
`runtime-sdk/` muessen daneben liegen. Das interne Paket ist relocatable und
verwendet keinen einkompilierten Entwicklerpfad.

Die GUI verlangt keine Projektdatei. Sie nimmt nur eine `.gdi` und einen
Ausgabeordner entgegen, zeigt Analyse, Codegen und Hostbuild sowie das
redigierte Buildlog live an und erzeugt bei vollstaendiger Analyse
`sourcecode/` und `game.exe`. Bei `partial` bleiben Analysebericht,
Ergebnisindex und Buildplan als Debuggrundlage erhalten; ein irrefuehrender
Hostbuild wird nicht erzeugt.

Der sichtbare interne Meilenstein `0.46.0` ist kein Produktrelease. CLI,
Fenstertitel, Jobberichte, Buildplan und Provenienz verwenden gemeinsam die
kanonische Werkzeugversion aus `VERSION`/CMake.

CLI und GUI verwenden intern dieselben Loader-, Analyse-, Codegen- und
Hostbuildkomponenten. Der entsprechende CLI-Pfad ist beispielsweise:

```powershell
.\build-current\katana-recomp.exe workflow build .\project.katana --output .\work-output
```

Architektur und interner Bedienpfad stehen in
[docs/PHASE10_GUI_ARCHITECTURE.md](docs/PHASE10_GUI_ARCHITECTURE.md) und
[docs/PHASE10_GUI_WORKFLOW.md](docs/PHASE10_GUI_WORKFLOW.md).
Der eigenstaendige Port nutzt den nativen Hostvideovertrag aus
[docs/HOST_VIDEO.md](docs/HOST_VIDEO.md).

## Umgesetzte Bereiche

- SH-4-Integer-, Systemregister-, Delay-Slot- und FPU-Grundsemantik
- Raw- und ELF32-SH-Loader, Symbole, Relocations und Manifest v1/v2
- rekursive Analyse, indirekte Zielbeweise, Overrides/Hints und Graphen
- Katana-IR, Verifier, Optimierungspipeline und C++-Backend
- zentrale CPU-/FPU-Runtime, Speicherbus, Exceptions und Interrupts
- RAM, VRAM, AICA-RAM, BIOS, Flash, MMIO, Watchpoints und Invalidierung
- Maple/Controller/VMU, PVR-Minimalpfad, AICA-HLE, GD-ROM/ISO9660/GDI
- Scheduler, TMU, RTC, DMA, Medienuhr und deterministische Systemreplays
- partitionierter/incrementeller Codegen und externer Port-Projektexport
- stabile CLI-/JSON-Vertraege, Diagnostik, Provenienz und Source Maps
- ASan, statische Analyse, Coverage, prozessisoliertes Fuzzing,
  Differentialtests und reproduzierbare Debugartefakte

## Noch offen

KatanaRecomp ist weiterhin Pre-Alpha. Insbesondere fehlen noch:

- vollstaendige SH-4- und FPU-Exception-Semantik
- vollstaendige Dreamcast-Hardwaremodelle und ARM7-LLE
- breitere Retail-Kompatibilitaet und Performanceoptimierung
- weitergehende Fortschritts-/Diagnoseansichten fuer Retail-Debugging
- Linux-Desktop-GUI
- native Alpha-Portintegration und Windows-/Linux-Alpha-CI

Diese Arbeiten sind in den Phasen 9 bis 13 der Roadmap aufgeteilt.

Die geplanten Staende v0.38.0 bis v0.49.0 sind interne Meilensteine ohne
Release-Commit, Tag oder Download. v0.50.0 Alpha wird der erste oeffentliche
Produktrelease.

## Voraussetzungen

- Visual Studio 2022 Build Tools mit MSVC x64
- CMake 3.25 oder neuer
- Ninja
- Git

Bis zum Alpha-Gate wird nur der Debug-Build verwendet. Das einzige lokale
Buildverzeichnis ist `build-current/`.

## Bauen und testen

In einer x64 Developer PowerShell:

```powershell
cmake --preset artifact-debug
cmake --build --preset artifact-debug --parallel
ctest --test-dir build-current --output-on-failure
```

Die kumulativen Gateprofile, Coverage und reproduzierbaren Artefakte sind in
[docs/DEBUG_GATE.md](docs/DEBUG_GATE.md), [docs/COVERAGE.md](docs/COVERAGE.md)
und [docs/REPRODUCIBLE_ARTIFACTS.md](docs/REPRODUCIBLE_ARTIFACTS.md)
dokumentiert.

## CLI-Beispiele

```powershell
# Version und ISA-Abdeckung
.\build-current\katana-recomp.exe --version
.\build-current\katana-recomp.exe isa-report
.\build-current\katana-recomp.exe isa-report --json

# Projekt analysieren und Graph exportieren
.\build-current\katana-recomp.exe analyze-json .\project.katana
.\build-current\katana-recomp.exe cfg-dot .\project.katana

# C++ aus einem Raw-/ELF-/Manifest-Eingang erzeugen
.\build-current\katana-recomp.exe emit-cpp .\program.bin 8C010000 .\generated.cpp 8C010000

# Extern buildbares Portprojekt aus einer lokalen GDI erzeugen
.\build-current\katana-recomp.exe port .\disc\game.gdi --output C:\ports\game --target-name game

# Die erzeugte Anwendung eigenstaendig mit der GDI starten
C:\ports\game\build\game.exe .\disc\game.gdi
```

`workflow` schreibt waehrend des Laufs versionierte Fortschrittsereignisse als
JSON Lines nach `stderr`; das abschliessende Jobergebnis bleibt als einzelnes
JSON-Dokument auf `stdout`. Gesamtprozente sind monoton. `step_total: null`
bedeutet einen tatsaechlich unbestimmten Einzelschritt und niemals null Prozent.
Die GUI verwendet exakt denselben Ereignisstrom fuer Fortschritt und Live-Log.

Die Windows-GUI besitzt ein dunkles, High-Contrast-kompatibles Design, native
Gesamt- und Schrittbalken, sichtbare kopierbare GDI-/Ausgabepfade und das
KatanaRecomp-App-Icon. Hauptinhalt und Live-Log lassen sich getrennt scrollen;
Layout, Mindestgroesse und Controls skalieren per Monitor von 100 bis 300
Prozent DPI. Das Icon bleibt bis zum KR-4902-Audit ein internes Asset.

Private, budgetierte Retail-Debuglaeufe verwenden den strikt externen Harness
aus [docs/PRIVATE_RETAIL_DEBUG.md](docs/PRIVATE_RETAIL_DEBUG.md). Er committed
weder GDI noch generierte Quellen, Binaries, Rohlogs, Pfade, Hashes oder
Captures und ersetzt keinen synthetischen Regressionstest oder Alpha-Nachweis.

Der Port-Export liest private Disc-Dateien nur lokal, schreibt ausschliesslich
verwaltete Dateien unter `generated/` neu und erhaelt handgeschriebenen Code
unter `src/`. Details: [docs/PORT_EXPORT.md](docs/PORT_EXPORT.md).
Die `game.exe` bettet keine Disc-Daten oder privaten Hostpfade ein; die GDI
bleibt eine explizite read-only Laufzeiteingabe. Ohne gueltige GDI endet der
Prozess mit einem Nichtnull-Exitcode und redigierter Diagnose.

Der generierte SH-4-Pfad erzwingt den privilegierten Modus fuer markierte
Systemregister- und Kontrollinstruktionen. Ein Zugriff aus dem User-Modus wird
vor jeder Teilwirkung als strukturierte Illegal-Instruction-Ausnahme an den
Runtime-Dispatcher uebergeben; er wird weder still ausgefuehrt noch als Host-
Exception verloren.

Ein Build gilt nur bei vollstaendiger Abdeckung aller committed ausfuehrbaren
Bytes sowie ohne unbekannte Instruktion, ungeloeste Kontrollflussstelle oder
erreichbare Abbruchkante als abgeschlossen. Eingaben werden vor dem Laden und
direkt danach kryptografisch verglichen. Wiederholte Fehler bewahren den letzten
erfolgreichen Stale-Stand; ueberlappende Ausgabeziele sind unter Windows und
Linux auch zwischen getrennten Prozessen gesperrt.

## Projektstruktur

```text
include/katana/   oeffentliche APIs
src/              Implementierungen fuer Analyse, Codegen, Runtime und CLI
tests/            Unit-, Integrations-, generierte und Plattformtests
tools/            Gate-, Qualitaets-, Release- und Diagnosewerkzeuge
docs/             Vertraege, Status, Tasks und Releaseberichte
```

## Workflow

- Roadmap und Tasks werden pro Arbeitspaket gepflegt; `STATUS.md` wird an Gates
  aktualisiert.
- Der vollstaendige lokale Regressionstest wird gesammelt am Phasenabschluss
  ausgefuehrt, nicht nach jedem einzelnen Task.
- Release-Builds und GitHub-CI kehren erst am Alpha-Gate `v0.50.0` zurueck.
- Zwischenstaende bis v0.49.0 werden intern geprueft, aber nicht als Releases
  veroeffentlicht.
- Mit Phase 11 beginnen autorisierte lokale, budgetierte Sonic-Adventure-
  Debuglaeufe. Private Ausgaben bleiben ausserhalb des Repositorys; jeder
  Befund wird als allgemeine synthetische oder frei verteilbare Regression
  abgesichert.
- Alpha ist erreicht, wenn der offizielle Port reproduzierbar bootet und bis
  in eine mit Hosteingabe kontrollierbare Spielszene laeuft.
- Entwicklung und Pushes erfolgen direkt auf `main`.

## Dokumentation

- [ROADMAP.md](ROADMAP.md) - Phasen und Release-Gates
- [docs/TASKS.md](docs/TASKS.md) - Tasks und Akzeptanzbedingungen
- [docs/STATUS.md](docs/STATUS.md) - kompakter aktueller Projektstand
- [CHANGELOG.md](CHANGELOG.md) - Versionshistorie
- [docs/CODEX_HANDOFF.md](docs/CODEX_HANDOFF.md) - Arbeitsregeln
- [docs/SONIC_ADVENTURE_ACCEPTANCE.md](docs/SONIC_ADVENTURE_ACCEPTANCE.md) - lokaler Alpha-Vertrag
- [docs/SH4_ALPHA_ISA.md](docs/SH4_ALPHA_ISA.md) - messbarer Alpha-ISA-Vertrag
- [docs/REFERENCE_PROVENANCE.md](docs/REFERENCE_PROVENANCE.md) - Referenz- und Lizenzprovenienz

## Rechtlicher Rahmen

KatanaRecomp wird unabhaengig entwickelt. Referenzprojekte duerfen zum
Verstaendnis beobachtbaren Verhaltens untersucht werden; fremder Code wird
nicht ohne ausdrueckliche, dokumentierte Lizenzentscheidung uebernommen.
