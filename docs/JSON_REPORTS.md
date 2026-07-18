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

`katana-recomp analyze-json <manifest> [overrides]` erzeugt den lokalen
Detailbericht `katana-control-flow-v3`. Version 3 fuehrt disjunkte
Vollstaendigkeitszustaende und typisierte Herkunftsklassen ein. Der
Anwendungsworkflow erzeugt daneben `katana-control-flow-frontier-v1` ohne
Gastadressen, Symbole oder Hostpfade. `katana-recomp ir-json ...` behaelt
`katana-ir-v2`. Historische Phase-6-Berichte verwenden
`katana-phase6-gate-v1` und behalten ihre Messfelder auf der obersten Ebene.

Die Funktionswertanalyse berichtet in der Kontrollfluss-Summary
`function_iteration_budget` und `function_budget_exhausted`. Jede
Registersummary trennt `complete` von `guarded`; nur `complete=true` darf eine
vollstaendige endliche Zielmenge begruenden.

Funktionssummaries tragen zusaetzlich `memory_complete` und die sortierte Liste
`memory_values`. Jeder Eintrag nennt Adresse, `complete`, `guarded` und seine
endliche Wertemenge. Diese lokalen Detailfelder duerfen Gastadressen enthalten
und werden deshalb nicht in den adressfreien Frontierbericht uebernommen.

Berichte enthalten keine Hostzeit als Determinismusquelle. Absolute lokale
Pfade, Firmwarebytes und Flash-Rohdaten sind keine portablen Berichtfelder;
spaetere Diagnosebefehle muessen solche Inhalte standardmaessig redigieren.

`katana-alpha-isa`/`alpha-isa` Vertragsversion 1 wird mit
`katana-recomp isa-report --json` erzeugt. Der Bericht zaehlt den gesamten
16-Bit-Opcoderaum, ordnet jede decodierte Instruktionsart einer Familie zu und
meldet Decoder, IR, Backend und Runtime getrennt als `supported`, `restricted`
oder `rejected`. Semantikvertrag, konkrete Einschraenkung und
Testanforderung sind Pflichtfelder; eine reine Decoderzaehlung ist keine
Faehigkeitsbehauptung.

## Anwendungsjob und Buildplan

`katana-application-job` Version 5 unterscheidet die Endzustaende `completed`,
`partial`, `failed` und `cancelled`. `partial` ist kein erfolgreicher Build:
Analyseartefakte bleiben nutzbar, Codegen und Hostkompilierung werden jedoch
unterdrueckt. Das Feld `analysis` enthaelt committed ausfuehrbare Bytes,
analysierte und nicht analysierte ausfuehrbare Bytes, Instruktions-/
Funktionszahlen, vollstaendige und partielle Guards, reine Laufzeit- und
ungeloeste Kontrollflussstellen, unbekannte Instruktionen, erreichbare
Abbruchkanten und `control_flow_complete`. Vollstaendig bedeutet exakt: null
unbekannte Instruktionen, null partielle, reine Laufzeit- und ungeloeste
Kontrollflussstellen, null nicht analysierte committed ausfuehrbare Bytes und
null erreichbare Abbruchkanten. Es gibt keine heuristische Prozentgrenze.

`failure_category` trennt `none`, `input-output`, `processing`,
`code-generation`, `build` und `internal`. Die Workflow-CLI bildet diese
Kategorien auf ihre bestehenden stabilen Exitcodes ab. `partial` und
`cancelled` sind keine versteckten Exceptions; ihr Feld bleibt `none`, der
Prozessstatus ist dennoch ungleich null, solange der Job nicht `completed` ist.

`katana-build-plan` Version 5 spiegelt denselben Zustand und dieselben Metriken.
Bei `status=partial` ist `host_compilation=false`; nur `status=built` darf eine
veroeffentlichte `game.exe` behaupten. Beide Berichte tragen `tool_version` aus
derselben CMake-Definition wie CLI, GUI und Portprovenienz.

## Live-Jobereignisse

`katana-job-event` Version 1 ist der gemeinsame geordnete Observerstrom von CLI
und GUI. `sequence` beginnt je Job bei null. `overall_percent` ist monoton;
`stage` und `step_status` benennen den aktiven Einzelschritt. `step_current` und
`step_total` sind entweder gemeinsam gesetzt oder gemeinsam `null`. Ein
unbekannter Umfang bleibt dadurch unbestimmt. `timestamp_ms` und `elapsed_ms`
geben Ereigniszeit und Joblaufzeit an. `log_chunk` enthaelt ausschliesslich neu
beobachtete, bereits redigierte Hostausgabe; Diagnosen stehen typisiert in
`diagnostic`. Fehler und Abbruch verwenden den aktiven Schritt statt eines
informationsarmen generischen `failed`-Schritts.
