# Lokale Sonic-Adventure-Debug- und Alpha-Akzeptanzstrategie

Dieses Dokument trennt zwei Zwecke strikt:

1. Ab Phase 11 darf Sonic Adventure lokal als privater, budgetierter Debuginput
   verwendet werden, um allgemeine Compiler-, Runtime- und Plattformfehler zu
   finden.
2. Erst KR-4999 wertet einen solchen Lauf als Alpha-Gate-Nachweis.

Alpha ist erreicht, wenn der offizielle Workflow aus der lokal bereitgestellten
GDI ein externes Port-Projekt und `game.exe` erzeugt und diese Anwendung
reproduzierbar bis in eine mit Hosteingabe kontrollierbare Spielszene gelangt.
Eine nur startende EXE, ein betretenes Hauptprogramm oder ein einzelner Frame
reichen nicht aus.

## Datenschutz- und Repositorygrenze

- Verwendet wird ausschliesslich eine rechtmaessig vom Nutzer lokal
  bereitgestellte GDI.
- GDI, Tracks, extrahierte Dateien, generierte Retail-Quellen, `game.exe`, Logs,
  Screenshots, Audio, Hashes und lokale Pfade bleiben ausserhalb des
  KatanaRecomp-Repositorys und aller Releasepakete.
- Der aktuell autorisierte private Arbeitsbereich liegt ausserhalb des Repos
  unter `private/`; sein konkreter Hostpfad wird nicht in Berichte uebernommen.
- Oeffentliche CI und verteilbare Tests benoetigen niemals proprietaere Daten.
- Fest codierte Sonic-Adressen, Remaps, Patches, titelbezogene Runtime-
  Sonderfaelle oder willkuerliche Checkpointzaehler sind unzulaessig.

## Retail-getriebener Debugzyklus

Jeder lokale Lauf besitzt ein Hostzeit- und Gastzyklusbudget und endet mit
einem Checkpoint oder einer benannten Fehlerklasse. Ein Haengen, stiller Erfolg
oder bloss erfolgreicher Hostbuild ist kein Ergebnis.

Fuer jeden neuen Blocker gilt:

1. letzten stabilen allgemeinen Checkpoint und Fehlerklasse redigiert erfassen;
2. Ursache ohne titelbezogene Annahme bestimmen;
3. minimales synthetisches oder frei verteilbares Reproduktionsprogramm bauen;
4. allgemeine Implementierung korrigieren;
5. Reproduktion als Repository-Regression aufnehmen;
6. privaten Lauf wiederholen, ohne private Artefakte zu committen.

Der private Lauf ist damit ein Kompatibilitaets-Orakel, nicht der oeffentliche
Testbestand.

Der konkrete lokale Aufruf, Konfigurationsvertrag, Budgetpfad und Selbsttest
sind in [`PRIVATE_RETAIL_DEBUG.md`](PRIVATE_RETAIL_DEBUG.md) beschrieben.

## Entwicklungscheckpoints

Die Checkpoints sind monoton und titelunabhaengig instrumentiert:

| Checkpoint | Bedeutung |
| --- | --- |
| `SA_ANALYSIS_CONTINUES` | Rekursive Analyse passiert den ersten indirekten Bootsprung und entdeckt weiteren erreichbaren Code. |
| `SA_MAIN_ENTERED` | Das Hauptprogramm laeuft in der eigenstaendigen `game.exe` mit initialisiertem Speicher und Runtime-Dispatch. |
| `SA_FIRST_FRAME` | Ein aus Gastzustand erzeugter Frame wird im Hostfenster praesentiert. |
| `SA_MENU_INTERACTIVE` | Eine Menue- oder aequivalente Auswahl reagiert deterministisch auf Hosteingabe. |
| `SA_ALPHA_PLAYABLE` | Mindestens eine Spielszene ist sichtbar und ueber Hosteingabe kontrollierbar; Disc-I/O und Scheduler machen weiter Fortschritt. |

Zwischenstaende duerfen lokal beobachtet werden. Nur `SA_ALPHA_PLAYABLE` ist
der finale Alpha-Checkpoint.

Der erste KR-4511-Harnesslauf erreichte innerhalb seines Budgets aggregiert
`SA_ANALYSIS_CONTINUES`, blieb mit 350 ungeloesten Kontrollflussstellen
`analysis-incomplete` und startete keine Hostanwendung. Dieser allgemeine
Blocker ist weder Boot- noch Spielbarkeitsnachweis; private Identitaetsdaten
sind nicht Bestandteil dieser Dokumentation.

## Maschinenlesbarer privater Laufbericht

Der lokale Harness darf ausserhalb des Repositorys einen Bericht mit mindestens
folgenden redigierten Feldern erzeugen:

```json
{
  "schema": "katana-private-retail-debug",
  "version": 1,
  "checkpoint": "SA_ANALYSIS_CONTINUES",
  "analysis": {
    "boot_bytes": 0,
    "instructions": 0,
    "functions": 0,
    "unresolved_control_flow": 0,
    "coverage_complete": false
  },
  "runtime": {
    "game_executable_started": false,
    "main_executable_entered": false,
    "frames_presented": 0,
    "input_events_consumed": 0,
    "guest_cycles": 0,
    "scheduler_events": 0,
    "gdrom_completions": 0,
    "dma_events": 0,
    "interrupts_delivered": 0,
    "indirect_dispatches": 0,
    "fallbacks": 0,
    "silent_failures": null
  },
  "failure_class": null,
  "budget_exhausted": false
}
```

Berichte duerfen keine Disc-/Dateihashes, Spieldaten, Screenshots oder
unredigierten Hostpfade enthalten. Gateberichte enthalten nur aggregierte,
freigegebene Metriken.
`silent_failures: null` bedeutet, dass die Runtime keinen validierten
Metrikenmarker geliefert hat, nicht dass null Fehler bewiesen wurden.

## Verteilbare Nachweise

Alle allgemeinen Faehigkeiten bleiben durch synthetische Fixtures und frei
lizenzierte Homebrew-Programme abgesichert. Die internen Meilensteine sind:

| Meilenstein | Verteilbarer Checkpoint |
| --- | --- |
| Phase 10, v0.44.0 | `KR_PHASE10_GUI_END_TO_END` |
| Phase 11, v0.45.0 | `KR_V045_BOOT_ANALYSIS_READY` |
| Phase 11, v0.46.0 | `KR_V046_RETAIL_BOOT_SERVICES_READY` |
| Phase 12, v0.47.0 | `KR_V047_NATIVE_HOST_READY` |
| Phase 12, v0.48.0 | `KR_V048_PORT_WORKFLOW_READY` |
| Phase 13, v0.49.0 | `KR_V049_ALPHA_CANDIDATE_READY` |
| Alpha v0.50.0 | `SA_ALPHA_PLAYABLE` |

Kein `KR_...`-Checkpoint behauptet fuer sich Sonic-Kompatibilitaet.
`KR_V045_BOOT_ANALYSIS_READY` belegt ausschliesslich die synthetische/Homebrew-
Regression der Bootanalyse. Der Checkpoint behauptet weder einen Sonic-Boot
noch erzeugte Frames, Audio, Eingabe oder Spielbarkeit.
`KR_V046_RETAIL_BOOT_SERVICES_READY` belegt ausschliesslich den synthetischen
HLE-BIOS-ABI-/System-ASIC-Vertical-Slice, keinen Sonic-Boot.

## Alpha-Gate v0.50.0

KR-4999 muss nachweisen:

1. Die GDI wird read-only ueber den offiziellen Workflow verarbeitet.
2. Analyse und Codegen melden keine unvollstaendige Abdeckung als Erfolg.
3. Ein externes Port-Projekt baut eine eigenstaendige `game.exe` ohne
   eingebettete private Daten oder absolute private Pfade.
4. `game.exe` initialisiert Bootimage, CPU, Speicher, Scheduler,
   Plattformdienste und generischen Dispatch und startet ohne CLI-Huelle.
5. Boot, Hauptmenue beziehungsweise aequivalente Auswahl und mindestens eine
   Spielszene werden erreicht.
6. Video und Hosteingabe funktionieren zusammen; Disc-I/O, Scheduler und
   Interruptzustellung machen messbaren Fortschritt.
7. Zwei identische Laeufe erreichen `SA_ALPHA_PLAYABLE` mit denselben
   deterministischen Kernmetriken und `silent_failures == 0`.
8. Fehler- und Budgetpfade liefern redigierte, verwertbare Diagnosen.
9. Debug-/Release-Builds, vollstaendige Regression und Windows-/Linux-CI
   bestehen ohne proprietaere Eingaben.
10. Repository, Staging und Pakete enthalten keine privaten Retail-Artefakte.

Audio darf zum Alpha bekannte und klar dokumentierte Genauigkeitsprobleme
besitzen. Vollstaendige Audio-/Grafikkorrektheit, mehrere Retail-Titel und
Release-Performance bleiben Beta-Ziele. Ein kompletter Spieldurchlauf ist kein
Alpha-Kriterium.

## Review und Freigabe

KR-4999 ist die Alpha-Gate-Vorbereitung und stoppt nach dem redigierten Bericht
zwingend fuer das Nutzerreview. KR-5000 darf erst nach ausdruecklicher Freigabe
beginnen. Ohne Freigabe erfolgen keine Versionsaenderung, kein Tag, kein
Download und keine Veroeffentlichung.
