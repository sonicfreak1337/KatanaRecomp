# Port-Projektexport und Originaldisc-Installation

Der generische Exportpfad uebersetzt das validierte Dreamcast-Bootprogramm in
statischen nativen AOT-Code. Ein verteilbares Portpaket enthaelt keine Raw-,
Audio- oder sonstigen kommerziellen Discsektoren:

```powershell
katana-recomp port .\disc\game.gdi --output .\port --target-name game
```

Die GDI und alle Tracks werden read-only geoeffnet. Analyse, Katana-IR,
Optimierung und C++-Partitionierung bleiben spielagnostisch; es gibt keine
Titeladressen, Dateinamen oder Sonderfaelle. Die Originaldateien werden weder
veraendert noch geloescht.

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

Der Programmcode aus `1ST_READ.BIN` wird analysiert, in Katana-IR ueberfuehrt,
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
verwalteten Pfaden werden abgelehnt. Metadaten enthalten nur Rollen, relative
Pfade, Format-/ABI-Daten, Groessen, Generationen und SHA-256-Werte.

Der Vertrag gilt fuer alle unterstuetzten Dreamcast-GDI-Titel. Private Spiele
dienen ausschliesslich als lokale End-to-End-Fixtures; verteilbare und CI-Tests
verwenden synthetische oder frei lizenzierte Inhalte.
