# Privater Retail-Debugharness

`tools/phase11/run-private-retail-debug.ps1` fuehrt einen lokalen Retail-Lauf
mit festem Hostzeit- und vorbereitetem Gastzyklusbudget aus. Konfiguration,
GDI, Manifest, Ausgabe und Bericht muessen ausserhalb des Repositorys liegen.
Der Harness lehnt Repositoryziele ab und gibt nur den redigierten Vertrag
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
`KATANA_GUEST_CYCLE_BUDGET` wird an eine gestartete `game.exe` uebergeben; bis
die Runtime den Verbrauch selbst meldet, weist der Bericht nur das konfigurierte
Budget aus und behauptet keine verbrauchten Gastzyklen.

Checkpoints werden niemals aus Dateinamen oder titelbezogenen Adressen
abgeleitet. `SA_ANALYSIS_CONTINUES` folgt aus nachweisbar fortgesetzter Analyse.
Hoehere Checkpoints werden nur aus gleichnamigen Runtime-Markern uebernommen.
Partielle Analyse, I/O, Codegen, Hostbuild, Runtime-Exit und Hostzeitbudget
besitzen allgemeine Fehlerklassen. Rohes stdout/stderr bleibt fluessig und wird
nicht in den redigierten Bericht kopiert.

Der verteilbare Selbsttest benoetigt keine Retaildaten:

```powershell
.\tools\phase11\run-private-retail-debug.ps1 -SelfTest
```

`.gitignore` sperrt `private/`, `*.retail-debug.json`,
`*.retail-debug.log` und `.katana-retail-debug/`. Pakete beziehen keine dieser
Pfade ein.
