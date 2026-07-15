# Lokale Sonic-Adventure-Akzeptanzstrategie

Dieses Dokument definiert Sonic Adventure ab Phase 6 als massgeblichen realen,
lokalen End-to-End-Akzeptanztest. Es ersetzt keine Unit-, Integrations-,
Regression-, Fuzzing-, Plattform- oder Homebrew-Tests. Diese bleiben der
oeffentlich verteilbare Nachweis, dienen der Fehlerlokalisierung und laufen
weiterhin fuer einzelne Tasks und in der oeffentlichen CI.

## Grundregeln

- Verwendet wird ausschliesslich der bereits bekannte, vom Nutzer lokal
  bereitgestellte GDI-Dump.
- Spieldaten, extrahierte Dateien, Screenshots, Audioinhalte, Dump-Hashes und
  lokale Pfade duerfen weder committed noch in Release-Artefakte aufgenommen
  werden.
- Fest codierte Sonic-Adventure-Adressen, Remaps, Patches, titelbezogene
  Runtime-Sonderfaelle und willkuerliche spielspezifische Zaehlerstaende sind
  unzulaessig.
- Der Test ist fuer lokale Phasen-Releases verpflichtend, fuer oeffentliche CI
  jedoch optional. Fehlt der lokale Dump, wird er sauber als uebersprungen
  ausgewiesen.
- Der vollstaendige Test laeuft nicht nach einzelnen Tasks, Commits oder
  Zwischenreleases. Diese erhalten weiterhin passende kleinere Tests.
- Pro vollstaendiger Roadmap-Phase existiert genau ein kumulatives lokales
  Sonic-Adventure-Release-Gate.
- Jedes Gate prueft sein neues Phasenziel, alle vorherigen Checkpoints und die
  Abwesenheit von Regressionen im bis dahin erreichten End-to-End-Pfad.
- Gates verwenden konkrete Ereignisse, Zaehler, Artefakte und endliche
  Gastzyklusbudgets. Formulierungen wie "laeuft weiter" oder "haengt nicht"
  sind ohne messbaren Endzustand kein Akzeptanzkriterium.
- Zukuenftige Gates werden vorab nur dokumentiert. Ihre Funktionen werden erst
  in der vorgesehenen Phase implementiert.

## Verbindliche Testzeitpunkte

Der vollstaendige lokale Test wird ausschliesslich zu diesen Zeitpunkten
ausgefuehrt:

| Zeitpunkt | Kumulativer Checkpoint |
| --- | --- |
| Phase 6, v0.31.0 | `SA_PHASE6_MAIN_EXECUTION_STARTED` |
| Phase 7, v0.34.0 | `SA_PHASE7_GENERATED_RUNTIME_ACTIVE` |
| Phase 8, v0.37.0 | `SA_PHASE8_REPRODUCIBLE_DIAGNOSTIC_RUN` |
| Phase 9, v0.40.0 | `SA_PHASE9_FIRST_GAME_FRAME` |
| Phase 10, v0.44.0 | `SA_PHASE10_GUI_END_TO_END` |
| Alpha, v0.50.0 | `SA_ALPHA_FIRST_REPRODUCIBLE_FRAME` |

Der bisherige v0.30.0-GDI-Smoke-Test wird nicht separat wiederholt. Seine
GDI-, Track- und ISO9660-Kriterien werden beim Phase-6-Release v0.31.0 als Teil
des kumulativen Tests erneut geprueft.

## Maschinenlesbarer Bericht

Die Implementierung muss schrittweise einen redigierten, maschinenlesbaren
Ergebnisbericht bereitstellen. Das vorgesehene Ausgangsschema ist:

```json
{
  "checkpoint": "CHECKPOINT_NAME",
  "gdi_loaded": false,
  "tracks_validated": 0,
  "iso9660_mounted": false,
  "boot_metadata_read": false,
  "boot_file_loaded": false,
  "main_executable_entered": false,
  "executed_blocks": 0,
  "guest_cycles": 0,
  "scheduler_events": 0,
  "gdrom_completions": 0,
  "dma_events": 0,
  "interrupts_delivered": 0,
  "cache_invalidations": 0,
  "indirect_dispatches": 0,
  "fallbacks": 0,
  "silent_failures": 0,
  "pvr_frames": 0,
  "audio_sample_frames": 0,
  "maple_transactions": 0
}
```

Felder duerfen an die tatsaechliche Architektur angepasst und versioniert
werden. Schwellenwerte werden erst festgeschrieben, wenn sie empirisch bestimmt
und technisch begruendet sind. Berichte duerfen keine unredigierten Hostpfade,
Spieldaten oder Dump-Hashes enthalten.

## Phase 6: Dreamcast-Plattformminimum

Das Gate wird mit v0.31.0 ausgefuehrt und umfasst v0.30.0 sowie v0.31.0.

GDI- und Dateisystemkriterien:

- Der GDI-Deskriptor wird ueber die normale `DiscSource`-/GDI-Abstraktion
  geladen; alle referenzierten Tracks werden gefunden und relative Pfade korrekt
  aufgeloest.
- Trackgroessen, Sektorformate und Trackgrenzen werden validiert.
- ISO9660 und Boot-Metadaten werden gelesen, der Bootdateiname wird ermittelt
  und die Bootdatei vollstaendig mit plausibler, von null verschiedener Groesse
  geladen.
- Wiederholte Reads derselben Bereiche liefern identische Daten; kein Read
  ueberschreitet Track-, Datei- oder Discgrenzen.
- Ungueltige Descriptor- oder Trackdaten erzeugen strukturierte Fehler.
- Der Dump bleibt unveraendert und Git-Diff sowie Berichte enthalten keine
  proprietaeren Daten oder lokalen Pfade.

Der Zwischencheckpoint lautet `SA_PHASE6_GDI_MOUNTED`.

Scheduling-, Timer- und DMA-Kriterien:

- Die Bootdatei wird in den Gastadressraum geladen und ihr Programmeinstieg
  allgemein ermittelt und gesetzt.
- Mindestens ein Block innerhalb des geladenen Hauptprogramms wird ausgefuehrt;
  `executed_blocks > 0` und `guest_cycles > 0`.
- Der Scheduler verarbeitet mindestens ein Ereignis und mindestens ein
  asynchroner GD-ROM-Read wird abgeschlossen. Das zugehoerige Abschlussereignis
  oder ein Interrupt wird verarbeitet.
- TMU-, DMA- und Interruptverarbeitung werden im Bericht erfasst.
- Ein festes Gastzyklusbudget verhindert unbegrenzte Laeufe.
- Unbekannte Opcodes, MMIO-Zugriffe und Interruptzustaende werden nicht still
  ignoriert; `silent_failures == 0`.
- Zwei identische Laeufe erreichen denselben Checkpoint, letzten Gast-PC und
  dieselben deterministischen Scheduler-Kernzustaende.

Der Checkpoint `SA_PHASE6_MAIN_EXECUTION_STARTED` wird nur daraus abgeleitet,
dass der Gast-PC im allgemein bestimmten Bereich des geladenen Hauptprogramms
liegt und dort Code ausgefuehrt wurde. Eine fest codierte Spieladresse ist
untersagt. Das Gate laeuft je einmal in einem frischen Debug- und Release-Build;
beide muessen dieselben Checkpoints und strukturellen Ergebnisse liefern.

Die v0.31.0-Implementierung nutzt ausschliesslich den bereits vorhandenen
Phase-5-C++-Emitter: Aus dem allgemein analysierten Einstieg wird lokal genau
ein Block erzeugt, tatsaechlich kompiliert und einmal innerhalb eines
64-Gastzyklusbudgets ausgefuehrt. Die danach erwartete noch nicht vorhandene
Phase-7-Zielauflosung endet als gezaehlter kontrollierter Fallback. Sie gilt
nicht als `indirect_dispatches` und zieht weder Blocktabelle noch modularen
Backend-Dispatch vor. Temporaere Quelle, Objekt und Programm werden nach dem
Gate geloescht.

Unter Windows fuehrt `tools/run_phase6_gate.ps1` zwei identische Laeufe der
gewaehlten Debug- oder Release-Konfiguration aus, vergleicht die redigierten
Berichte byteweise und prueft die Disc-Dateien vor und nach dem Lauf. Der
vollstaendige Phasenabschluss verlangt weiterhin je eine frische Debug- und
Release-Konfiguration; das Skript ist kein Gate pro Einzeltask.

## Phase 7: Codegenerator und Runtime-Dispatch

Das Gate wird mit v0.34.0 ausgefuehrt und umfasst alle Phase-6-Kriterien.

- Analyse, IR-Erzeugung und Codegenerierung ueber die modulare
  Backend-Schnittstelle laufen vollstaendig durch.
- Der erzeugte Code wird deterministisch auf mehrere Translation Units verteilt;
  identische Laeufe liefern dieselben Dateinamen und Blockmetadaten, ein
  unveraenderter zweiter Lauf messbare Cache-Treffer.
- Das Hostprojekt kompiliert, die generierte Anwendung startet und prueft die
  Runtime-ABI-Version.
- Mindestens ein generierter Block wird ausgefuehrt;
  `indirect_dispatches > 0`, und mindestens ein indirektes Ziel wird generisch
  ueber die Blocktabelle aufgeloest.
- Unbekannte Ziele werden diagnostiziert oder kontrolliert an den Fallback
  uebergeben; `silent_failures == 0`.
- Vollstaendiger CPU-Zustand wird an Backend- und Fallbackgrenzen uebertragen,
  Scheduler-Safepoints werden erreicht und identische Laeufe liefern dieselben
  deterministischen Dispatch-Kernmetriken.

`SA_PHASE7_GENERATED_RUNTIME_ACTIVE` gilt erst, wenn der Lauf aus dem
generierten Hostprogramm stammt und mindestens ein indirekter Kontrollfluss
generisch aufgeloest wurde.

## Phase 8: Werkzeuge und Qualitaet

Das Gate wird mit v0.37.0 ausgefuehrt und umfasst alle Phase-7-Kriterien.

- Ein versioniertes Katana-Projektmanifest beschreibt nur Referenzen und
  Metadaten, niemals Spieldaten.
- Analyse, Codegen, Build und Run laufen ueber die stabile CLI mit dokumentierten
  Exitcodes und JSON-Berichten nach einem versionierten Schema.
- Berichte enthalten keine Spieldaten oder unredigierten lokalen Pfade.
- Ein kontrollierter Abbruch erzeugt einen Bericht mit Gast-PC, kanonischer
  Blockadresse, Blockendtyp, Delay-Slot-, Ausnahme- und Schedulerzustand sowie
  letztem Dispatchvorgang.
- Dispatch-, Fallback-, DMA-, Interrupt- und Schedulerereignisse sind
  nachvollziehbar; begrenzte Laeufe sind deterministisch protokollierbar.
- Identische Laeufe erreichen denselben Checkpoint und erzeugen bytegleiche
  Manifeste und Blockmetadaten. Hostzeit ist keine versteckte Wahrheitsquelle.
- Sanitizer-, Fuzzing- und Differenztests bleiben vom lokalen Dump unabhaengig.

`SA_PHASE8_REPRODUCIBLE_DIAGNOSTIC_RUN` gilt erst, wenn ein begrenzt beendeter
oder fehlgeschlagener Lauf reproduzierbar und maschinenlesbar diagnostiziert
werden kann.

## Phase 9: Kompatibilitaet und Leistung

Das Gate wird mit v0.40.0 ausgefuehrt und umfasst alle Phase-8-Kriterien. Das
Homebrew-Testkorpus bleibt der oeffentlich verteilbare Pflichtnachweis.

- Nach Eintritt in das Hauptprogramm erreicht mindestens ein Spiel-Frame den
  vollstaendigen PVR-Pfad; `pvr_frames >= 1`.
- Breite, Hoehe, Stride sowie Framebuffer- und VRAM-Grenzen sind plausibel. Ein
  lokales Capture wird erzeugt, aber niemals committed.
- `audio_sample_frames > 0`, sofern der erreichte Pfad bereits Audio ausgibt;
  `maple_transactions > 0`, sofern er den Controller initialisiert.
- Grafik-, Audio-, Maple-, GD-ROM- und Schedulerereignisse stammen aus demselben
  zusammenhaengenden, gastzyklusbegrenzten Lauf. Mindestens zwei Frameintervalle
  werden verarbeitet und `silent_failures == 0`.
- Der Performancebericht trennt Analyse-, Codegen-, Host-Kompilier-, Start- und
  Laufzeit und erfasst Dispatches, Fallbacks, Schedulerjitter,
  Codeinvalidierungen und erzeugte Codegroesse.
- Aktivierte und deaktivierte Fastpaths liefern denselben beobachtbaren
  Gastzustand. Das Pre-Alpha-Paket enthaelt keine Spieldaten, Captures oder
  lokalen Pfade.

`SA_PHASE9_FIRST_GAME_FRAME` beweist nur den technisch vollstaendigen PVR-Pfad.
Visuelle und akustische Korrektheit werden zunaechst lokal manuell bewertet;
plattformuebergreifende Screenshot-Hashes sind kein alleiniger Nachweis.

## Phase 10: Desktop-GUI und Alpha-Workflow

Das Gate wird mit v0.44.0 ausgefuehrt und umfasst alle Phase-9-Kriterien.

- Ein Katana-Projekt kann in der GUI angelegt, gespeichert und wieder geoeffnet
  werden; der lokale GDI-Dump kann als Quelle gewaehlt und mit validierten
  Tracks, Groessen und Sektorformaten angezeigt werden.
- Analyse, Codegen, Build und Run lassen sich starten, Jobs abbrechen und
  Fortschritt, Warnungen sowie Fehler anzeigen.
- GUI und CLI verwenden dieselben Anwendungsdienste und erzeugen dieselben
  Manifeste, Analyse- und Codegen-Artefakte sowie denselben Checkpoint.
- Ein absichtlich ungueltiger Trackpfad erzeugt in GUI und CLI dieselbe
  Fehlerklasse. Exportierte Berichte sind redigiert.
- Der GUI-Workflow ist fuer die im Alpha-Scope unterstuetzten Windows- und
  Linux-Konfigurationen automatisiert testbar.

`SA_PHASE10_GUI_END_TO_END` prueft nur den Workflow. CPU-, Runtime- und
Plattformkorrektheit verbleiben in den Headless-Integrationstests; die GUI darf
keine Dreamcast-Testlogik duplizieren.

## Alpha-Gate v0.50.0

Das Alpha-Gate wiederholt den vollstaendigen kumulativen Test und verlangt alle
vorherigen Checkpoints. Messbar nachzuweisen sind:

1. GDI, Tracks, ISO9660 und Bootdatei werden ueber den offiziellen
   Quellenworkflow verarbeitet.
2. Das Hauptprogramm wird geladen, analysiert und in IR sowie Hostcode
   ueberfuehrt; das Hostprojekt baut und startet.
3. Hauptprogrammcode wird ausgefuehrt; Scheduler, GD-ROM, DMA und Interrupts
   treiben den gastzyklusbegrenzten Lauf voran.
4. Mindestens ein indirektes Ziel wird generisch aufgeloest und
   `silent_failures == 0`.
5. Mindestens ein Spiel-Frame erreicht das Host-Backend und wird nur lokal als
   Capture erzeugt; Audio- und Maple-Zustand werden erfasst.
6. CLI und GUI erreichen denselben Checkpoint. Zwei identische Laeufe erreichen
   denselben Checkpoint und dieselben deterministischen Kernmetriken.
7. Ein begrenzt beendeter Fehllauf erzeugt einen verwertbaren, redigierten
   Diagnosebericht.
8. Repository und Releasepaket enthalten keine Spieldaten, Captures oder lokalen
   Dump-Pfade.

Der Alpha-Checkpoint lautet `SA_ALPHA_FIRST_REPRODUCIBLE_FRAME`. Interaktives
Sonic-Adventure-Gameplay ist kein Alpha-Pflichtkriterium, sondern gehoert zum
spaeteren Beta-Gate.
