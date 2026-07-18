# Privater Retail-Debugharness

`tools/phase11/run-private-retail-debug.ps1` fuehrt einen lokalen Retail-Lauf
mit festem Hostzeit- und vorbereitetem Gastzyklusbudget aus. Konfiguration,
GDI, Manifest, Ausgabe und Bericht muessen ausserhalb des Repositorys liegen.
Dies gilt auch fuer die Konfigurationsdatei selbst und fuer den exakten
Repositorywurzelpfad. Der Harness vergleicht sowohl lexikalische als auch
physisch aufgeloeste Pfade; Symlinks, Windows-Junctions/Reparse Points und noch
nicht vorhandene Kinder bestehender Eltern koennen die Grenze nicht umgehen.
Der Harness gibt nur den redigierten Vertrag
`katana-private-retail-debug` Version 1 aus.

Beispiel fuer eine ausschliesslich lokale, nicht versionierte Konfiguration:

```json
{
  "schema": "katana-private-retail-debug-config",
  "version": 1,
  "manifest_path": "<privates Manifest ausserhalb des Repositorys>",
  "gdi_path": "<private GDI ausserhalb des Repositorys>",
  "output_root": "<privates Ausgabeziel ausserhalb des Repositorys>",
  "report_path": "<privater Bericht ausserhalb des Repositorys>",
  "host_timeout_seconds": 300,
  "guest_cycle_budget": 10000000
}
```

Aufruf in PowerShell:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
  .\tools\phase11\run-private-retail-debug.ps1 `
  -Config <private-config.json>
```

Der Hostprozess und seine Kinder werden bei Zeitueberschreitung beendet.
`KATANA_GUEST_CYCLE_BUDGET` wird an eine gestartete `game.exe` uebergeben und
von Gastzeitvertrag 1 direkt im zentralen Scheduler durchgesetzt. Ungueltige,
leere oder nullwertige Angaben werden abgewiesen. Die Runtime meldet den
tatsaechlich erreichten Schedulerzyklus als `guest_cycles`; der Harness darf
diesen Verbrauch weiterhin nicht aus dem konfigurierten Budget ableiten.

Checkpoints werden niemals aus Dateinamen oder titelbezogenen Adressen
abgeleitet. `SA_ANALYSIS_CONTINUES` folgt aus nachweisbar fortgesetzter Analyse.
Hoehere Checkpoints werden nur aus gleichnamigen Runtime-Markern uebernommen.
Sie bilden eine strikte Zustandsmaschine: jeder Vorgaenger muss genau einmal
und in Reihenfolge vorkommen. Fehlende, doppelte oder vertauschte Marker sind
Harnessfehler.
Partielle Analyse, I/O, Codegen, Hostbuild, Runtime-Exit und Hostzeitbudget
besitzen allgemeine Fehlerklassen. Rohes stdout/stderr bleibt fluessig und wird
nicht in den redigierten Bericht kopiert.

`game_executable_started` erfasst den erfolgreichen Prozessstart unabhaengig
von Exitcode oder Timeout. `silent_failures` ist `null`, solange die Runtime
keinen einzelnen gueltigen `KATANA_RUNTIME_METRICS ... silent_failures=N`-
Marker liefert; der Harness setzt diesen Wert niemals selbst auf null. Ein
gemeldeter Wert groesser null erzeugt die Fehlerklasse `silent-failures`.

Der verteilbare Selbsttest benoetigt keine Retaildaten:

```powershell
.\tools\phase11\run-private-retail-debug.ps1 -SelfTest
```

`.gitignore` sperrt `private/`, Retail-Debugberichte/-logs, frei benannte
`*retail-config*.json`-/`*retail-debug-config*.json`-Konfigurationen und
`.katana-retail-debug/`. Pakete beziehen keine dieser Pfade ein.
