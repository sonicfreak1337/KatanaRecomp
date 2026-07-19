# Privater Retail-Build-only-Harness

Stand: KR-4719

`tools/phase11/run-private-retail-debug.ps1` verwendet fuer v0.47
ausschliesslich den Build-only-Vertrag. Der historische Dateiname bleibt fuer
lokale Aufrufe und den verteilbaren Selbsttest stabil; ein Retail-Runtimelauf
ist in diesem Modus technisch ausgeschlossen.

Konfiguration, GDI, Manifest, beide Jobausgaben und Bericht muessen ausserhalb
des Repositorys liegen. Dies gilt auch fuer die Konfigurationsdatei selbst und
den exakten Repositorywurzelpfad. Der Harness vergleicht lexikalische und
physisch aufgeloeste Pfade. Windows-Junctions/Reparse Points sowie noch nicht
vorhandene Kinder bestehender Eltern koennen die Grenze nicht umgehen.

## Konfiguration Version 2

Der Harness akzeptiert genau die folgenden Felder. Fehlende und unbekannte
Felder werden abgelehnt; `execution_mode` muss `build-only` sein.

```json
{
  "schema": "katana-private-retail-debug-config",
  "version": 2,
  "execution_mode": "build-only",
  "manifest_path": "<privates Manifest ausserhalb des Repositorys>",
  "gdi_path": "<private GDI ausserhalb des Repositorys>",
  "output_root": "<privates Ausgabeziel ausserhalb des Repositorys>",
  "report_path": "<privater Bericht ausserhalb des Repositorys>",
  "host_timeout_seconds": 1800
}
```

Aufruf in PowerShell:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
  .\tools\phase11\run-private-retail-debug.ps1 `
  -Config <private-config.json>
```

Der konfigurierte GDI-Pfad muss physisch mit `input.path` des Manifests
uebereinstimmen und das Manifest muss `input.format = gdi` verwenden. Der
offizielle `workflow build`-Pfad erfasst Descriptor, Tracks, Manifestprofil und
optionale Analysedirektiven in einer portablen Projektidentitaet. Private
Pfade, Hashes und Rohlogs werden nicht in den Harnessbericht uebernommen.

## Erzwungener No-run-Vertrag

Alle Kindprozesse muessen den einzigen rollenbewussten Prozessstarter
durchlaufen. `Assert-BuildOnlyProcessAllowed` verwirft die Rolle `runtime`,
bevor `Process.Start` erreicht wird. Der aktuelle Harness besitzt keinen
Aufruf, der `game.exe` oder ein anderes Runtimeartefakt an diesen Starter
uebergibt. Zwei zufaellig benannte, vorher nicht vorhandene Jobverzeichnisse
verhindern ausserdem, dass eine stale `game.exe` als aktuelles Ergebnis
ausgewaehlt wird.

Der Selbsttest legt eine stale synthetische `game.exe` an und bestaetigt, dass
die Runtimeprozessrolle vor dem Start abgewiesen wird. Der Bericht muss immer
`game_executable_started=false` und `runtime_processes_started=0` enthalten.
Ein Checkpoint hoeher als `KR_RETAIL_ANALYSIS_CONTINUES` kann nicht entstehen, weil
der Harness weder Runtimeausgabe einliest noch einen Runtimeprozess erzeugt.

## Zwei frische Buildnachweise

Ein Harnesslauf fuehrt zwei vollstaendige offizielle Buildjobs in getrennten
frischen Verzeichnissen aus. Jeder Job muss:

- `state=completed`, die Checkpoints `analysis-complete`, `codegen-complete`
  und `host-build-complete` jeweils genau einmal sowie keinen Runcheckpoint
  melden;
- `control_flow_complete=true`, `guarded_partial_control_flow=0`,
  `unresolved_control_flow=0` und keine unbekannte Instruktion besitzen;
- genau ein `host-executable`-Artefakt melden, dessen SHA-256 intern mit der
  Datei im aktuellen Jobverzeichnis uebereinstimmt;
- dieselbe Projektidentitaet in Jobresultat, Resultindex und
  Portprojektmetadaten tragen;
- einen Buildplan mit `status=built`, `host_compilation=true` und
  `native_execution=false` besitzen.

Anschliessend muessen beide Jobs dieselbe interne Projektidentitaet sowie
bytegleiche relative Inventare unter `generated/code`, `generated/include` und
`generated/metadata` besitzen. Die dafuer gelesenen Hashes bleiben intern und
werden nicht berichtet.

## Atomarer redigierter Bericht

`katana-private-retail-build` Version 1 enthaelt ausschliesslich aggregierte
Abdeckungszahlen, Boolwerte fuer Identitaets- und Reproduzierbarkeitsvertrag,
Buildanzahl, No-run-Zustand, den maximalen Checkpoint und eine allgemeine
Fehlerklasse. Gastadressen, Eingabeidentitaeten, SHA-256-Werte, Tracknamen,
private Pfade und stdout/stderr sind verboten.

Der Bericht wird zuerst neben seinem Ziel in eine eindeutige Temporaerdatei
geschrieben und danach auf demselben Dateisystem atomar verschoben oder
ersetzt. Auch ein Fehlerbericht durchlaeuft dieselbe Allowlist- und
Redaktionspruefung.

Der verteilbare Selbsttest benoetigt keine Retaildaten und startet keinen
Build:

```powershell
.\tools\phase11\run-private-retail-debug.ps1 -SelfTest
```

`.gitignore` sperrt `private/`, Retail-Berichte/-logs, frei benannte
`*retail-config*.json`-/`*retail-debug-config*.json`-Konfigurationen und
`.katana-retail-debug/`. Pakete beziehen keine dieser Pfade ein.

## Gateausfuehrung

Gemaess Handoff wird der echte private Doppelnachweis erst in KR-4704 mit der
frisch gebauten CLI ausgefuehrt. Bis dahin dokumentiert KR-4719 den
ausfuehrbaren Vertrag und seine synthetischen Gateanforderungen, aber keine
privaten Messwerte.
