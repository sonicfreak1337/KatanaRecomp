# Port-Projektexport

Der generische Exportpfad erzeugt aus einer validierten GDI ein getrenntes,
reproduzierbares Portprojekt, ein lokales Contentpaket und das Hosttarget im
Debugprofil:

```powershell
katana-recomp port .\disc\game.gdi --output .\port --target-name game
```

Die GDI wird read-only validiert und in einen portablen, pfadfreien
`game.katana-disc` gepackt. Der Pack erhaelt die kanonische Tracktabelle, den
logischen LBA-Raum, Raw-Sektoren, Audiosektoren, Sessions, Chunkindex und
SHA-256-Integritaetswerte. Danach folgen Executable Image,
Kontrollflussanalyse, Katana-IR, Optimierung und deterministische
Translation-Unit-Partitionierung. Die Original-GDI und ihre Trackdateien sind
nur Eingaben und werden weder kopiert noch veraendert.

Der gemeinsame GUI-/Workflow-Build exportiert nur bei vollstaendig abgedecktem
Kontrollfluss. Ungeloeste oder nur partiell bewachte indirekte Ziele sowie
unbekannte Instruktionen liefern
`partial`, einen Buildplan mit `host_compilation=false` und keinen Hostbuild.
Kontrollierte Teilanalysen erfolgen ueber `analyze-json`; sie sind keine
Kompatibilitaetsaussage.

Im Anwendungsworkflow uebernimmt der Export das bereits validierte Executable
Image, Analyseergebnis, optimierte IR, Eingabeprovenienz und die portable
Projektidentitaet. Dadurch koennen eine zweite GDI-Analyse und voneinander
abweichende Identitaets-/Codegen-Snapshots nicht entstehen.

## Layout

```text
port/
  game.exe                    veroeffentlichtes Hostprogramm
  content/
    game.katana-disc          vollstaendige read-only Discquelle
    game.katana-disc.json     Identitaet, Tracks und Artefaktbindung
  runtime/
    runtime-dependencies.json Runtimevertrag (derzeit statisch gelinkt)
  user-data/                  Flash, VMU und weitere veraenderliche Daten
  .gitignore                  ignoriert den getrennten Hostbuild
  CMakeLists.txt              einmalig angelegter Nutzer-Bootstrap
  src/main.cpp                einmalig angelegte Integrationsschicht
  generated/
    .katana-generated-artifacts
    CMakeLists.txt
    build.ninja
    compile_commands.json
    katana-port.cmake
    code/runtime-dispatch.cpp
    code/unit-*.cpp
    include/katana_port.hpp
    metadata/port-project.json
    metadata/provenance.json
    metadata/source-map.json
    metadata/cfg.json
    metadata/cfg.dot
    metadata/callgraph.json
    metadata/callgraph.dot
```

Nur Dateien im Katana-Artefaktmanifest unter `generated/` werden bei einer
erneuten Generierung ersetzt oder als veraltet entfernt. Unbekannte Dateien und
insbesondere `src/` bleiben erhalten. Symbolische Links in verwalteten Pfaden
werden abgelehnt.

## Hostbuild und Runtimevertrag

Der CLI-Aufruf erzeugt zuerst ein Stagingpaket, verifiziert Disc-Pack und
Hostbuild und veroeffentlicht den Ausgabeordner danach atomar. Ein
fehlgeschlagener Export ersetzt keinen letzten erfolgreichen Stand.

Der lokale Debugbuild bindet KatanaRecomp ueber den
expliziten CMake-Parameter
`KATANA_RUNTIME_ROOT` ein. Portprojekt-Vertragsversion 7 umfasst den
eigenstaendigen Runtime-/GDI-Einstieg, die Runtime-only-Dispatchmetriken sowie
projektgebundene Flash-/VMU-Arbeitskopien und Host-Pacing. Die generierten
Quellen pruefen Runtime-ABI 14 und PlatformServices-ABI 5 beim
Kompilieren; portable Dateien enthalten keinen absoluten lokalen Quellpfad. Der
Build liegt getrennt unter `port/build/`. Konfigurations- oder Buildfehler enden
mit dem stabilen CLI-Exitcode `7` (`build-failure`). Die folgenden Befehle zeigen
den entsprechenden manuellen Wiederholungsweg:

```powershell
cmake -S .\port -B .\port\build -G Ninja `
  -DCMAKE_BUILD_TYPE=Debug `
  -DKATANA_RUNTIME_ROOT=<KatanaRecomp-Quellbaum>
cmake --build .\port\build --target game
# Der offizielle `katana-recomp port`-Job veroeffentlicht danach atomar:
.\port\game.exe
```

Das Hostprogramm oeffnet standardmaessig
`content/game.katana-disc` relativ zum eigenen Speicherort. Mit
`--content <Pack>` kann ein anderer Pack explizit gewaehlt werden.
`--gdi-debug <Quelle.gdi>` ist der einzige direkte GDI-Pfad und ausschliesslich
fuer lokale Entwicklung vorgesehen. Die Runtime liest Bootmetadaten und
ISO9660-Bootdatei ueber dieselbe `DiscSource`-Grenze, initialisiert
Dreamcast-Hauptspeicher, VRAM, AICA-RAM, Flash, CPU und Scheduler und waehlt
den Programmeinstieg ueber die generische Blocktabelle. Der erste indirekte
Dispatch wird strukturiert diagnostiziert; ein fehlendes Ziel oder ein
Speicherfehler kann nicht als erfolgreicher Prozess enden.

Der Standardadapter oeffnet vor dem Runtime-Start lokale Arbeitskopien unter
`user-data/`, bindet Flash und VMU ein, taktet den Host am
Videoereignis und speichert beim geordneten Shutdown. `KATANA_USER_DATA_ROOT`
kann die lokale Wurzel festlegen. Weder GDI noch exportierte Quellen werden
veraendert. Details stehen in
[`MUTABLE_STORAGE_AND_PACING.md`](MUTABLE_STORAGE_AND_PACING.md).

Eine explizit als `runtime_only` klassifizierte Stelle darf exportiert werden.
Ihr Ziel muss zur Laufzeit ein ausgerichteter exakter Anfang eines aktiven,
generationsgueltigen Blocks im Executable Image sein. Ein Miss beendet den Lauf
und kann weder Erfolg noch einen nachfolgenden Checkpoint erzeugen. Details
stehen in [`RUNTIME_ONLY_DISPATCH.md`](RUNTIME_ONLY_DISPATCH.md).

`PackedDiscSource` prueft Tabellenintegritaet beim Oeffnen und jeden Raw-Chunk
vor der ersten Nutzung. Ein begrenzter Chunkcache erlaubt zufaelligen
Sektorzugriff ohne vollstaendiges Laden. Fehlende Chunks, LBA-Luecken,
Audiotracks in der Datensicht und unbekannte Sektormodi werden explizit
abgelehnt. Der vollstaendige Pack wird vor der Veroeffentlichung gelesen und
verifiziert. EXE, Pack, Packmanifest und Runtimevertrag tragen dieselbe
Jobgeneration und werden nur gemeinsam aus dem Job-Staging veroeffentlicht.

Nach erfolgreichem Export darf der gesamte Portordner verschoben und die
urspruengliche GDI samt Tracks entfernt werden. Der Standardstart benoetigt
keinen Zugriff auf den urspruenglichen Quell- oder Exportpfad.

Ein externes Portprojekt darf die allgemeinen DiscSource-, Host- und
Runtime-SDK-Grenzen integrieren. Titelbezogene Installer-, Patch- und
Enhancementlogik sowie ein konkretes Produktlayout gehoeren nicht in
KatanaRecomp.

Metadaten, Contentmanifest und Provenienz enthalten nur Rollen, relative
Pfade, Format-/ABI-/Partitionsdaten, Groessen, Generationen und SHA-256-Werte.
Absolute GDI-, Track-, Build- und Hostpfade sowie urspruengliche Tracknamen
werden nicht serialisiert.
