# KatanaRecomp

Aktuelle Pre-Alpha-Entwicklungsphase: `0.48.0`
Abgeschlossener interner Meilenstein: `0.47.0`

KatanaRecomp ist ein unabhaengiges C++20-Framework fuer die statische
Rekompilierung von Sega-Dreamcast-SH-4-Code. Das Projekt ist kein Emulator,
kein ISO-Loader und kein Paket fuer kommerzielle Spieldaten.

Das Architekturziel entspricht der Werkzeugklasse von XenonRecomp: Gastcode
wird vorausanalysiert, in C++ beziehungsweise nativen Hostcode uebersetzt und
mit einer getrennten Plattformruntime gebaut. Das Freigabegate fuer einen
normalen Portlauf verbietet interpretierte oder JIT-kompilierte SH-4-
Instruktionen und eine virtuelle Dreamcast aus einem Disc-Image. Im Disc-Pfad
werden konkret `IP.BIN` und das BootExecutable als getrennte SH-4-AOT-Segmente
zu nativem PC-Code; Dreamcast-Geraete erscheinen nur als titelunabhaengige,
typisierte Plattformgrenzen und nicht als Emulator-, Discplayer- oder
Titelhackschicht.

BIOS-Dateien, Disc-Images, urheberrechtlich geschuetzte Assets und aus
kommerziellen Spielen erzeugter Code gehoeren nicht in dieses Repository.

## Status

v0.48 fuehrt den von der Originaldisc lokal installierten Systembootstrap und
die Bootdatei als getrennte native AOT-Segmente aus. BIOS-Requestqueue,
Vierwortstatus, LOW/HIGH-TOC sowie gastzeitgebundene GD-ROM-PIO-/G1-DMA-
Streamingtransfers sind implementiert. Der reale SH-4-DMAC-Channel-2-Vertrag
fuehrt `RS=2`, 32-Byte-Bursts und `DMAOR.DDT` aus allen vier Area-3-RAM-Spiegeln
bis zum TA-FIFO; die Regression enthaelt eine vollstaendige Liste samt EOL.
Byteidentische BIOS-/GD-Reloads erhalten bereits gebundene native AOT-Bloecke;
geaenderte Bytes invalidieren diese genau einmal. MMU-PIO bindet den Load an die
tatsaechlich geschriebene physische Range und lehnt nichtlineare TLB-Spannen vor
dem ersten Write ab.

G1-DMA prueft den geschuetzten und beschreibbaren Zielbereich atomar. Ein
Fehler meldet Phase, Adresse, uebertragenes Praefix und Residue, stoppt alle
Folgeereignisse und wird vom GD-ROM-Vertrag als stabiler CHECK-/Sense- und
Requestfehler uebernommen. Store-Queue-`PREF` verwendet bei deaktivierter MMU
QACR und bei aktiver MMU die UTLB; Ausrichtung, `SQMD`, ASID/SV,
Schreibberechtigung, Dirty-Bit, Miss und Multiple-Hit werden vor jeder
TA-/Speicherwirkung geprueft. Der native Emitter fuehrt dabei auch
adressabhaengige `PREF`-Fehler ueber den SH-4-Exceptionpfad und laesst sie nicht
als Hostexception aus der Port-EXE entkommen.

Der normale Produktport emittiert oder linkt keinen SH-4-Interpreter. Ein
nicht vorab gebundenes AOT-Ziel endet stattdessen als typisierter Fehler. Nur
ein expliziter `diagnostic_partial`-Export enthaelt den begrenzten
Diagnoseinterpreter und weist ihn im Manifest aus. Strukturierte Disc-
Ladetransaktionen und vorab erzeugte latente AOT-Module bleiben in KR-4848
offen. P1-/P2-Aliase dispatchen denselben nativen Code; absolute Pointerwerte
aus beschreibbaren Anfangssnapshottabellen bleiben validierte Runtime-
Kandidaten und werden weder zu erfundenen CFG-Kanten noch zu Funktionsseeds.
Der statische PAL-Audit findet in den begrenzten `MOV.W`-/`BRAF`-Relative16-
Tabellen 87 Eintraege, 76 eindeutige Kandidaten und 73 Ziele, die im vorherigen
Port noch fehlten. Diese Ziele werden als native Basic-Block-Leader
vorbereitet; der live geladene Dispatch bleibt `RuntimeOnly` und erzeugt keine
feste CFG-Kante. Snapshotcache und P2-Aliasaufloesung sind gegen imagefremde
Beweise abgesichert. Lokale AOT-Blockketten tragen die exakte tatsaechliche
Terminatorquelle und Siteklasse bis zum externen Dispatch weiter. Der aktuelle
kumulative Stand verwendet Runtime-ABI 38, Backend-Interface-ABI 3 und
Portprojektvertrag 23.

Der frische optimierte ABI-38-PAL-Port wurde mit zwoelf Jobs in 140,9 Sekunden
exportiert; der inkrementelle Reexport dauerte 29,2 Sekunden. Er umfasst 1.856
Funktionen und 37 Codepartitionen, aber weiterhin null Retailsektoren im
Portpaket. Die lokale Discinstallation war erfolgreich, und die unveraenderte
Original-GDI blieb erhalten.

Der aktuelle Produktlauf erreicht 345.609.251 Gastzyklen und einen echten
Runtimehandler am architektonischen SH-4-Interruptvektor `VBR + 0x600`.
Frame-, TA- und Gast-PVR-Zaehler bleiben null. Eine getrennte, begrenzte
Diagnose weist ein 56-Byte-Copy-plus-Patch-Codetemplate ueber die beobachteten
Gastwrites nach und fuehrt daraus 19 bytebewiesene Runtimeinstruktionen aus.
Der folgende noch nicht statisch gebundene AOT-Einstieg stoppt kontrolliert;
es wird weder interpretiertes SH-4 in den Produktpfad aufgenommen noch eine
private Adresse als Spielsonderfall festgeschrieben. `KR-4848` bleibt fuer die
allgemeine native Bindung solchen Runtimecodes offen. Der aktuelle P0 bleibt
damit der erste scanoutgebundene, vom Gast erzeugte Frame. Moderner
Hostcontrollersupport fuer Xbox-, DualSense- und vergleichbare Geraete gehoert
zu v0.49 und beginnt auf der freigegebenen Framebasis.

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

Der konfigurierte Testbestand umfasst derzeit 181 Tests; ein vollstaendiges
181er Gate wurde in diesem Zwischenblock nicht ausgefuehrt. Das fokussierte
Kern-Gate besteht 9/9, nach der source-relativen Fallthroughkorrektur der
Portexporttest zusaetzlich 1/1. Der generische C++-Fix setzt bei
nachfolgerlosen Bloecken in allen Backendmodi den PC auf die echte
Folgeinstruktion; eine kompilierte Regression deckt Einzelblock, lokales
Chaining und normalen Backendpfad ab. Die Produktinvariante vergleicht mit der
tatsaechlichen Terminatorquelle statt dem Wrapper-Einstieg.

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

Der sichtbare interne Meilenstein `0.47.0` ist kein Produktrelease. CLI,
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
Audio, Eingabe, Fokus und Shutdown folgen dem Vertrag in
[docs/HOST_RUNTIME.md](docs/HOST_RUNTIME.md).

## Umgesetzte Bereiche

- SH-4-Integer-, Systemregister-, Delay-Slot- und FPU-Grundsemantik
- Raw- und ELF32-SH-Loader, Symbole, Relocations und Manifest v1/v2
- rekursive Analyse, indirekte Zielbeweise, Overrides/Hints und Graphen
- registerweise SH-4-Wertanalyse mit explizitem SH-C-Callvertrag fuer GDI-Images
- vollstaendig validierte absolute und begrenzte relative SH-4-Sprungtabellen
- Katana-IR, Verifier, Optimierungspipeline und C++-Backend
- zentrale CPU-/FPU-Runtime, Speicherbus, Exceptions und Interrupts
- RAM, VRAM, AICA-RAM, BIOS, Flash, MMIO, Watchpoints und Invalidierung
- Maple/Controller/VMU, PVR-Minimalpfad, AICA-HLE, GD-ROM/ISO9660/GDI
- allgemeiner Disc-Hardwareauditor fuer SH-4-, MMIO-, DMA- und TA-Zugriffe
- gastzeitgebundener PVR-Scanout mit PAL/NTSC, Interlace, Blank/Border und
  echtem Render-vor-Present-Vertrag
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
- GPU-beschleunigter PVR-Rasterizer; die aktuelle Windows-Presentation ist
  funktional, der allgemeine PVR-Renderer arbeitet aber noch auf der CPU
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

Der CLI-Portbuild nutzt standardmaessig die vom Host gemeldete Parallelitaet.
`KATANA_HOST_BUILD_JOBS` setzt die Hostbuild-Workerzahl explizit;
`KATANA_PORT_CODEGEN_JOBS` dient als Fallback. Auf Windows kann
`KATANA_HOST_BUILD_GENERATOR=Ninja` zusammen mit
`KATANA_HOST_BUILD_MAKE_PROGRAM` einen getrennten parallelen `build-ninja`-
Build erzwingen. Auf dem primaeren Entwicklungsrechner werden dafuer zwoelf
Jobs verwendet; auf anderen Rechnern bleibt die Wahl dynamisch.

Statische Dispatchregistries werden in maximal 512 Bloecke grosse Shards
geteilt. Ein balancierter Router und hoechstens ein Wrapper pro Owner und Shard
halten die zentrale Datei klein. Beim aktuellen PAL-Port misst
`runtime-dispatch.cpp` 34.879 Byte beziehungsweise 607 Zeilen statt zuvor
36.703.886 Byte beziehungsweise 525.996 Zeilen; der groesste der 43 Shards
misst 393.454 Byte. Eine 513-Block-Regression prueft die Zwei-Shard-Grenze und
entfernt veraltete Shards; ein vollstaendiges synthetisches Ninja-/MSVC-Projekt
kompiliert und linkt in 15 Sekunden. Die fokussierte Suite besteht 6/6.

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
.\build-current\katana-recomp.exe port .\disc\game.gdi --output C:\ports\game --target-name game --console-profile europe-pal

# Einmalig die eigene unveraenderte Originaldisc pruefen und lokalen Content installieren
C:\ports\game\game.exe --install-disc .\disc\game.gdi

# Danach aus dem lokalen, nicht verteilbaren Nutzercache starten
C:\ports\game\game.exe
```

Der Portexport verwendet standardmaessig die vom Host gemeldeten CPU-Threads
fuer partitionierten C++-Codegen und Projektausgabe. Mit
`KATANA_PORT_CODEGEN_JOBS` kann die Workerzahl reproduzierbar begrenzt werden,
beispielsweise auf `12`; Analyseindizes werden einmal aufgebaut und von allen
Funktionen wiederverwendet. Unter Windows verwendet die Provenienzerfassung
den nativen BCrypt-SHA-256-Pfad mit grossen Eingabechunks.
Die CLI meldet innerhalb der groben `analysis-codegen`-Phase stabile
`KATANA_PORT_SUBPHASE`-Marker. Sie enthalten nur den Stufennamen und weder
Discidentitaeten noch private Pfade oder Inhalte.

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
Die `game.exe` bettet keine Disc-Daten oder privaten Hostpfade ein. Der Export
legt stattdessen nur eine spielagnostische `game.katana-install`-Recipe mit
Hashes, Bootbindung und Trackgeometrie unter `content/` an. Sie enthaelt keine
Retailsektoren, Tracknamen oder Hostpfade. Jeder Nutzer stellt beim einmaligen
`--install-disc`-Schritt die eigene Original-GDI bereit; erst dann entsteht
unter `user-data/content/` ein lokaler, nicht verteilbarer Disc-Cache. Quelle
und Tracks bleiben read-only und werden niemals veraendert oder geloescht.
Fehlende oder abweichende Originaldaten enden vor Gastcode mit einem
Nichtnull-Exitcode und redigierter Diagnose.

Recipe 2 und Disc-Pack 2 binden diesen lokalen Cache an die tatsaechlich
gelesenen Raw-Chunks: Die Content-Root wird beim Installieren neu hergeleitet
und beim Oeffnen aus Tracktabelle und Chunkindex rekonstruiert. Eine waehrend
des Installierens veraenderte Quelle kann daher kein gueltiges Paket unter
einer zuvor erfassten Identitaet erzeugen.

Der aktuelle private PAL-Bring-up ist echte native AOT-Ausfuehrung und keine
Konsolenemulation: Discbytes dienen lokal als Nutzerdatenquelle, waehrend der
SH-4-Code in statische Hostfunktionen rekompiliert wird. Im HLE-GDI-Pfad gilt
das sowohl fuer den 16-Sektoren-Systembootstrap der Disc als auch fuer die
Bootdatei; beide bleiben getrennte, quellgebundene Segmente und der native
Einstieg beginnt im Bootstrapcode ueber den P2-Alias `0xAC008300` (physisch
`0x0C008300`). Noch nicht gebundene
Hardwareaktionen, insbesondere DMA-Starts, brechen sichtbar ab; die Runtime
meldet sie nicht als erfolgreich und erzeugt daraus keinen kuenstlichen Frame.
Bereits gebundene Einheiten wie Maple verarbeiten dagegen echte Gast-
Deskriptoren und schreiben ihre Antworten gastzeitgebunden per DMA zurueck.
AICA/G2- und PVR-DMA nutzen fuer lineare Gastregionen einen validierten
Bulk-Kopierpfad; Beobachter- und MMIO-Situationen behalten den exakten
referenziellen Einzelzugriffspfad.
Der AICA-HLE-Produktpfad liest ebenfalls die echten Gast-Slotregister und das
gemeinsame Sound-RAM und erzeugt pro Media-Clock-Tick PCM16-Stereo; ein
synthetischer Stummpuffer gilt nicht mehr als Audioerfolg.

Der HLE-BIOS-Pfad initialisiert den unteren BIOS-Arbeits-RAM definiert mit
`0xFF`, installiert die dynamischen Vektoren sowie den direkten GD2-Alias und
behandelt `SYSTEM 1` als nicht zurueckkehrenden BIOS-Menue-Lifecycle. Ein
Neustart ist eine explizite Hostentscheidung und keine versteckte Semantik des
BIOS-Aufrufs. Das Konsolenprofil wird beim Portexport explizit gewaehlt; offene
Disc-Areasymbole wie `JUE` bestimmen weder PAL/NTSC noch die Flashregion. Der
Systembootstrap kann ueber `CCR.ORA/OIX` das allgemein modellierte SH-4-On-
Chip-RAM verwenden.

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
- Sonic Adventure bleibt dabei ausschliesslich private Retail-Testbench. Ein
  spaeterer Sonic-Port samt Installer und Enhancements ist ein eigenstaendiges
  Produkt und kein Bestandteil von KatanaRecomp.
- Framework-Alpha ist erreicht, wenn die allgemeinen Build-, Runtime-, SDK-,
  Diagnose- und Datenschutzvertraege reproduzierbar bestehen.
- Entwicklung und Pushes erfolgen direkt auf `main`.

## Dokumentation

- [ROADMAP.md](ROADMAP.md) - Phasen und Release-Gates
- [docs/TASKS.md](docs/TASKS.md) - Tasks und Akzeptanzbedingungen
- [docs/STATUS.md](docs/STATUS.md) - kompakter aktueller Projektstand
- [CHANGELOG.md](CHANGELOG.md) - Versionshistorie
- [docs/CODEX_HANDOFF.md](docs/CODEX_HANDOFF.md) - Arbeitsregeln
- [docs/SONIC_ADVENTURE_ACCEPTANCE.md](docs/SONIC_ADVENTURE_ACCEPTANCE.md) - private Retail-Testbench
- [docs/SH4_ALPHA_ISA.md](docs/SH4_ALPHA_ISA.md) - messbarer Alpha-ISA-Vertrag
- [docs/REFERENCE_PROVENANCE.md](docs/REFERENCE_PROVENANCE.md) - Referenz- und Lizenzprovenienz

## Rechtlicher Rahmen

KatanaRecomp wird unabhaengig entwickelt. Referenzprojekte duerfen zum
Verstaendnis beobachtbaren Verhaltens untersucht werden; fremder Code wird
nicht ohne ausdrueckliche, dokumentierte Lizenzentscheidung uebernommen.
