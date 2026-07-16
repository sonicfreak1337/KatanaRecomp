# JSON-Berichte

KatanaRecomp-Berichte tragen neben ihrem fachlichen `schema` drei gemeinsame
Felder:

- `report_version`: Version des gemeinsamen Berichtvertrags, aktuell `1`
- `report_type`: stabiler Typname wie `ir`, `control-flow` oder `phase6-gate`
- `status`: maschinenlesbarer Abschlussstatus, bei erfolgreichen Berichten
  `success`

Fachfelder bleiben im jeweiligen Schema definiert. Neue optionale Felder
duerfen hinzugefuegt werden; eine inkompatible Bedeutungs- oder Typaenderung
braucht eine neue fachliche Schema-Kennung. Listen, deren Reihenfolge keine
Gastsemantik traegt, werden vor der Ausgabe nach Gastadresse und Typ sortiert.

`katana-recomp analyze-json <manifest> [overrides]` erzeugt
`katana-control-flow-v1`. `katana-recomp ir-json ...` behaelt
`katana-ir-v2`. Historische Phase-6-Berichte verwenden
`katana-phase6-gate-v1` und behalten ihre Messfelder auf der obersten Ebene.

Berichte enthalten keine Hostzeit als Determinismusquelle. Absolute lokale
Pfade, Firmwarebytes und Flash-Rohdaten sind keine portablen Berichtfelder;
spaetere Diagnosebefehle muessen solche Inhalte standardmaessig redigieren.

## Anwendungsjob und Buildplan

`katana-application-job` Version 2 unterscheidet die Endzustaende `completed`,
`partial`, `failed` und `cancelled`. `partial` ist kein erfolgreicher Build:
Analyseartefakte bleiben nutzbar, Codegen und Hostkompilierung werden jedoch
unterdrueckt. Das Feld `analysis` enthaelt committed ausfuehrbare Bytes,
analysierte Instruktionsbytes, Instruktions-/Funktionszahlen, ungeloeste
Kontrollflussstellen, unbekannte Instruktionen und
`control_flow_complete`.

`failure_category` trennt `none`, `input-output`, `processing`,
`code-generation`, `build` und `internal`. Die Workflow-CLI bildet diese
Kategorien auf ihre bestehenden stabilen Exitcodes ab. `partial` und
`cancelled` sind keine versteckten Exceptions; ihr Feld bleibt `none`, der
Prozessstatus ist dennoch ungleich null, solange der Job nicht `completed` ist.

`katana-build-plan` Version 2 spiegelt denselben Zustand und dieselben Metriken.
Bei `status=partial` ist `host_compilation=false`; nur `status=built` darf eine
veroeffentlichte `game.exe` behaupten. Beide Berichte tragen `tool_version` aus
derselben CMake-Definition wie CLI, GUI und Portprovenienz.
