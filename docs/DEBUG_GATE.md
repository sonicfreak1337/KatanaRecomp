# Lokales Debug-Gate

Bis zum Alpha-Gate wird KatanaRecomp ausschliesslich ueber einen frischen
lokalen Debug-Build geprueft. Der reproduzierbare Befehl lautet:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass `
  -File .\tools\gates\run-debug-gate.ps1
```

Das Skript akzeptiert nur `build-current/` als Buildverzeichnis, entfernt
diesen zuvor kontrolliert und fuehrt danach die CMake-Presets `debug-gate` fuer
Konfiguration, Build und vollstaendige CTest-Regression aus. Jeder fehlerhafte
Teilprozess beendet das Gate sichtbar; nur der vollstaendige Erfolg schreibt
`KR_DEBUG_GATE_SUCCESS` auf die Konsole.

Die zugrunde liegenden Presets sind hostneutral. Eine spaetere Alpha-CI kann
dieselbe Folge verwenden:

```text
cmake --preset debug-gate --fresh
cmake --build --preset debug-gate --parallel
ctest --preset debug-gate
```

## Aktivierungsvertrag

`tools/quality/profiles.json` ist die maschinenlesbare Liste der
Qualitaetsprofile und ihrer verantwortlichen Tasks. KR-3701 aktiviert nur das
lokale Debug-Gate. Sanitizer, Fuzzing, Coverage, Formatierung, statische
Analyse, Artefakt- und Lizenzaudit werden durch ihre jeweiligen Phase-8-Tasks
aktiviert.

Ein regulaerer Release-Build und Windows-/Linux-CI bleiben bis KR-4999
deaktiviert. Deshalb liegt vor dem Alpha-Gate kein aktiver Workflow unter
`.github/workflows/`. Ein vorhandener Workflow im Repository darf bis dahin
weder Push-/PR-Checks noch einen Release-Build starten.

Das Gate wird gemaess der Roadmap nicht nach jedem Task ausgefuehrt. KR-3709
ruft es genau einmal nach Umsetzung aller gesammelten Phase-8-Regressionen auf
und stoppt danach fuer das Nutzerreview vor KR-3710.
