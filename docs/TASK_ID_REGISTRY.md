# KatanaRecomp Task-ID-Registry

## Verbindliche Regeln

1. Eine Task-ID ist ab ihrem ersten Merge semantisch unveraenderlich.
2. Ein Titel darf praezisiert, aber nicht durch eine andere Aufgabe ersetzt werden.
3. Entfaellt eine Aufgabe, bleibt ihre ID als `retired` registriert.
4. Wird eine Aufgabe ersetzt oder aufgeteilt, erhaelt jede neue Arbeit eine neue ID.
5. `superseded_by` ist eine einseitige Historienreferenz, kein Alias.
6. ROADMAP und TASKS muessen fuer aktive IDs denselben Titel und dieselbe
   Grundbedeutung verwenden.
7. Gate- und Release-IDs werden nie fuer Implementierungsarbeit wiederverwendet.

## Wiederhergestellte historische IDs

| ID | Erster stabiler Titel | Status |
|---|---|---|
| KR-4801 | Versioniertes Runtime-SDK fuer externe Port-Projekte | aktiv in v0.49 |
| KR-4802 | Gemeinsamer CLI-/GUI-Portexport und Buildworkflow | aktiv in v0.49 |
| KR-4803 | Out-of-Tree-`game.exe`-Integration | aktiv in v0.49 |
| KR-4804 | v0.48 Gate-Vorbereitung: Tests und Build | retired, superseded_by KR-4853 |
| KR-4805 | v0.48 interne Meilenstein-Freigabe | retired, superseded_by KR-4854 |
| KR-4901 | Alpha-CI-Konfiguration fuer Windows und Linux | aktiv in v0.49 |
| KR-4902 | Reproduzierbare Pakete sowie Daten- und Lizenzaudit | aktiv in v0.49 |
| KR-4903 | Alpha-Checkpoint- und Gate-Automatisierung einfrieren | aktiv in v0.49 |
| KR-4904 | v0.49 Gate-Vorbereitung: Tests und Build | aktiv in v0.49 |
| KR-4905 | v0.49 interne Kandidaten-Freigabe | aktiv in v0.49 |

## Migration der versehentlich wiederverwendeten IDs

Die Roadmapverdichtung ab Commit
`9e8257b5fbac8003fae445477bb7a40af67ca34b` verwendete mehrere bestehende IDs
fuer neue Arbeit. Diese Zuordnung gilt als korrigierter Planungsfehler.

| Zwischenzeitliche Bedeutung | Faelschliche ID | Neue ID |
|---|---:|---:|
| Runtimebeobachtung, Replay und Fehlerpakete | KR-4801 | KR-4911 |
| Dynamische Codebereiche, Module und Overlays | KR-4802 | KR-4912 |
| CPU-/Plattform-Bring-up bis `KR_GUEST_PROGRAM_ENTERED` | KR-4803 | KR-4913 |
| Gast-PVR-Pfad bis `KR_FIRST_GUEST_FRAME` | KR-4804 | KR-4915 |
| Menue, Eingabe und spielbare Szene | KR-4805 | KR-4916 |
| Alpha-Haertung, Paketierung, CI und Audit | KR-4901 | KR-4901, KR-4902 und KR-4903 nach urspruenglichem Scope |

## Neue IDs dieser Planung

| ID | Titel |
|---|---|
| KR-4811 | Private Harnessmodi und technisch erzwungener No-run-Vertrag |
| KR-4812 | Strukturierte Runtimeevidenz, Budgets, Replay und Datenschutz |
| KR-4813 | Content-addressed Harness- und Portbuildbeschleunigung |
| KR-4814 | Nativer Controller und gastzeitgebundene Maple-Eingabe |
| KR-4821 | Versionierte Jobtelemetrie und belastbarer Fortschritt |
| KR-4822 | GUI-Informationsarchitektur und responsives Layout |
| KR-4823 | Diagnostik-, Ergebnis-, Log- und Workflow-QOL |
| KR-4824 | Unveraenderliche Task-ID-Registry und Roadmaplinter |
| KR-4831 | Generischer Originaldisc-Installer ohne Retaildaten im Portpaket |
| KR-4841 | Clean-Room-Referenz- und Nicht-Emulationsvertrag |
| KR-4842 | Seiteneffektfreie Bootdiagnostik und Wait-Loop-Klassifikation |
| KR-4843 | Alias-korrekter nativer Disc-Systembootstrap |
| KR-4844 | Gastzeit, Interruptreihenfolge und vollstaendiger AOT-Chaining-Guard |
| KR-4845 | BIOS-Lifecycle, HLE-Bridges, Flash, Sysinfo und Region |
| KR-4846 | GD-ROM-BIOS-Requestqueue, Status und TOC |
| KR-4847 | GD-ROM-MMIO, PIO, G1-DMA und Disc-Streaming |
| KR-4848 | Runtimecode, Disc-Module, Overlays und latentes AOT |
| KR-4849 | TA-Eingang und PVR-Kommandopfad |
| KR-4850 | Erster scanoutgebundener Gastframe |
| KR-4851 | Boot- und Frame-Hotpath |
| KR-4852 | Konsolidierte v0.48-Validierung |
| KR-4853 | v0.48 Boot-Gate-Vorbereitung |
| KR-4854 | v0.48 interne Freigabe |
| KR-4911 | Runtimebeobachtung, Replay und Fehlerpakete |
| KR-4912 | Dynamische Codebereiche, Module und Overlays |
| KR-4913 | CPU-/Plattform-Bring-up bis `KR_GUEST_PROGRAM_ENTERED` |
| KR-4914 | Private interaktive Runtime-Sitzung mit Controller |
| KR-4915 | Gast-PVR-Pfad bis `KR_FIRST_GUEST_FRAME` |
| KR-4916 | Menue, Eingabe und spielbare Szene |

## Aktuelle Meilensteinzuordnung

- v0.48: `KR-4814`, `KR-4831`, `KR-4841` bis `KR-4854` sowie `KR-4911`,
  `KR-4912`, `KR-4913`, `KR-4914` und `KR-4915`; Controllerarbeit
  `KR-4814`/`KR-4914` beginnt nach dem durch `KR-4850` belegten Gastframe und
  endet vor dem einzigen finalen Vollgate in `KR-4852`
- v0.49: `KR-4801` bis `KR-4803`, `KR-4811` bis `KR-4813`, `KR-4821`
  bis `KR-4824`, `KR-4901` bis `KR-4905` und `KR-4916`
- `KR-4804` und `KR-4805` sind retired; die Nachfolger sind `KR-4853` und
  `KR-4854`.

## Geplanter Lintervertrag

Der spaetere Roadmaplinter muss mindestens pruefen:

- doppelte aktive IDs
- unterschiedliche aktive Titel derselben ID
- unbekannte IDs ohne Registryeintrag
- Wiederverwendung von Gate-/Release-IDs
- fehlende `superseded_by`-Ziele
- zyklische Migrationen
- aktive Task ohne ROADMAP- oder TASKS-Eintrag
