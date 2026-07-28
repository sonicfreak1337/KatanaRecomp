# KatanaRecomp Task-ID-Registry

## Verbindliche Regeln

1. Eine Task-ID ist ab ihrem ersten Merge semantisch unveraenderlich.
2. Ein Titel darf praezisiert, aber nicht durch eine andere Aufgabe ersetzt werden.
3. Entfaellt eine Aufgabe, bleibt ihre ID als `retired` registriert.
4. Wird eine Aufgabe ersetzt oder aufgeteilt, erhaelt jede neue Arbeit eine neue ID.
5. `superseded_by` ist eine einseitige Historienreferenz, kein Alias.
6. ROADMAP und TASKS muessen fuer aktive IDs denselben Titel und dieselbe Grundbedeutung verwenden.
7. Gate- und Release-IDs werden nie fuer Implementierungsarbeit wiederverwendet.

## Historische und bestehende IDs

| ID | Erster stabiler Titel | Status |
|---|---|---|
| KR-4801 | Versioniertes Runtime-SDK fuer externe Port-Projekte | aktiv in v0.49 |
| KR-4802 | Gemeinsamer CLI-/GUI-Portexport und Buildworkflow | aktiv in v0.49 |
| KR-4803 | Out-of-Tree-`game.exe`-Integration | aktiv in v0.49 |
| KR-4804 | v0.48 Gate-Vorbereitung: Tests und Build | retired, superseded_by KR-4853 |
| KR-4805 | v0.48 interne Meilenstein-Freigabe | retired, superseded_by KR-4854 |
| KR-4811 | Private Harnessmodi und technisch erzwungener No-run-Vertrag | historisch |
| KR-4812 | Strukturierte Runtimeevidenz, Budgets, Replay und Datenschutz | historisch |
| KR-4813 | Content-addressed Harness- und Portbuildbeschleunigung | historisch |
| KR-4814 | Nativer Controller und gastzeitgebundene Maple-Eingabe | abgeschlossen |
| KR-4821 | Versionierte Jobtelemetrie und belastbarer Fortschritt | historisch |
| KR-4822 | GUI-Informationsarchitektur und responsives Layout | spaeter |
| KR-4823 | Diagnostik-, Ergebnis-, Log- und Workflow-QOL | spaeter |
| KR-4824 | Unveraenderliche Task-ID-Registry und Roadmaplinter | aktiv in v0.49 |
| KR-4831 | Generischer Originaldisc-Installer ohne Retaildaten im Portpaket | abgeschlossen |
| KR-4841 | Clean-Room-Referenz- und Nicht-Emulationsvertrag | abgeschlossen |
| KR-4842 | Seiteneffektfreie Bootdiagnostik und Wait-Loop-Klassifikation | abgeschlossen |
| KR-4843 | Alias-korrekter nativer Disc-Systembootstrap | abgeschlossen |
| KR-4844 | Gastzeit, Interruptreihenfolge und vollstaendiger AOT-Chaining-Guard | abgeschlossen |
| KR-4845 | BIOS-Lifecycle, HLE-Bridges, Flash, Sysinfo und Region | abgeschlossen |
| KR-4846 | GD-ROM-BIOS-Requestqueue, Status und TOC | abgeschlossen |
| KR-4847 | GD-ROM-MMIO, PIO, G1-DMA und Disc-Streaming | offen ausserhalb des aktuellen DirectBoot-P0 |
| KR-4848 | Runtimecode, Disc-Module, Overlays und latentes AOT | abgeschlossen |
| KR-4849 | TA-Eingang und PVR-Kommandopfad | offen, produktgetrieben |
| KR-4850 | Erster scanoutgebundener Gastframe | historisch durch IP.BIN-Direct-FB belegt |
| KR-4851 | Boot- und Frame-Hotpath | superseded in v0.49 by KR-4956 bis KR-4960 |
| KR-4852 | Konsolidierte v0.48-Validierung | offen, nicht aktueller P0 |
| KR-4853 | v0.48 Boot-Gate-Vorbereitung | offen, nicht aktueller P0 |
| KR-4854 | v0.48 interne Freigabe | offen, nicht aktueller P0 |
| KR-4901 | Alpha-CI-Konfiguration fuer Windows und Linux | spaeter |
| KR-4902 | Reproduzierbare Pakete sowie Daten- und Lizenzaudit | spaeter |
| KR-4903 | Alpha-Checkpoint- und Gate-Automatisierung einfrieren | spaeter |
| KR-4904 | v0.49 Gate-Vorbereitung: Tests und Build | spaeter |
| KR-4905 | v0.49 interne Kandidaten-Freigabe | spaeter |
| KR-4911 | Runtimebeobachtung, Replay und Fehlerpakete | abgeschlossen |
| KR-4912 | Dynamische Codebereiche, Module und Overlays | abgeschlossen |
| KR-4913 | CPU-/Plattform-Bring-up bis `KR_GUEST_PROGRAM_ENTERED` | abgeschlossen im historischen NativeDiscBoot |
| KR-4914 | Private interaktive Runtime-Sitzung mit Controller | offen nach sichtbarem Spielboot |
| KR-4915 | Gast-PVR-Pfad bis `KR_FIRST_GUEST_FRAME` | historisch durch IP.BIN-Direct-FB belegt |
| KR-4916 | Menue, Eingabe und spielbare Szene | offen |

## v0.49 Sonic-Adventure-Produktaufgaben

| ID | Titel | Status |
|---|---|---|
| KR-4951 | Produktgate nach Gastzyklen und getrennte visuelle Meilensteine | abgeschlossen, Folgearbeit KR-4966 |
| KR-4952 | Post-IP.BIN-Spielhandoff fuer DirectBootExecutable | aktiv P0; reales CompletePlatform-Capture/-Apply belegt, strikt atomarer Commit, Save-Profil und normative Digests offen |
| KR-4953 | Privates Game-Entry-Handoff-Artefakt aus Original-GDI | aktiv P0; 22-Geraete-/5-Event-Artefakt erfasst und verwendet, Doppel-Capture, Inspect/Verify und allgemeiner Save-Schutz offen |
| KR-4954 | Deklaratives externes Spielprojekt und CLI-Scaffold | geplant P1 |
| KR-4955 | Explizite Funktionsgrenzen und Tabellenhinweise End-to-End | geplant P0 nach stabilem DirectBoot |
| KR-4956 | Static-AOT-Dispatchflucht inventarisieren und schliessen | aktiv P0 Performance |
| KR-4957 | Direkte native Calls ueber sichere Timinggrenzen | geplant P0 Performance |
| KR-4958 | IR-basierte Registerlokalisierung und RAM-Regionen | geplant P1 Performance |
| KR-4959 | Ereignisgetriebene Scheduler-/IRQ-Safepoints | geplant P1 Performance und Korrektheit |
| KR-4960 | 200-MHz-Produkt-Hotpath | geplant P0 Performance-Gate |
| KR-4961 | Externes SonicAdventureRecomp-Bring-up-Projekt | geplant P1 |
| KR-4962 | NativeDiscBoot-/DirectBoot-Paritaet am Game-Entry | aktiv P0; v26 beendet die konkrete G2-DMA und erzeugt zwei technische Direct-Frames, normative Digests, sichtbarer Frame, NativeDisc-Vergleich und relatives 600-Millionen-Gate offen |
| KR-4963 | Inkrementeller Runtime-/Spielbuild und Compiler-A/B | aktiv P1; warmer Direct-v24-Build 5,3 s, erster frischer Build 169,3 s, Runtime-/Hook-Schleifen und aktuelles Compiler-A/B offen |
| KR-4964 | v0.49 Produktabnahme bis sichtbarem Spielbild | Gate |
| KR-4965 | ADXT/mwSnd-Sound-Completion bis zum Writer schliessen | abgeschlossen ueber Alternativabnahme; allgemeine G2-Ursache repariert, alter Poll verlassen und engerer Blocker belegt |
| KR-4966 | Post-Entry-Produktgate und erforderliche Meilensteine | aktiv P0; absolutes 600-Millionen-Limit laesst nach Restore nur 184.766.730 Post-Entry-Zyklen |
| KR-4967 | Atomarer CompletePlatform-Capture-/Apply-Koordinator | aktiv P0, teilweise umgesetzt; Vorvalidierung, passive Restoreplaene, semantischer Recapture und Produkt-Apply belegt, globaler noexcept-Commit und Subsystemdigests offen |
| KR-4968 | AICA-/G2-/DMAC-/Scheduler-/IRQ-Handoff fuer Soundfortschritt | aktiv P0; Adapter und konkrete DirectBoot-G2-Completion belegt, aktive Restore-Reconciliation und exakte Paritaet offen |
| KR-4969 | PVR-/SPG-/ASIC-Handoff fuer den ersten Spiel-Frame | aktiv P0; zwei technische Direct-Frames mit 302.287 geaenderten Pixeln belegt, sichtbarer Hostframe und exakte Paritaet offen |
| KR-4970 | Produkt-sicherer Maple-/VMU-Handoff und Event-Rehydration | aktiv P0; Game-Entry-Adapter, Produktlauf und einmal identische migrierte Saves belegt, allgemeines No-Rollback-Profil offen |
| KR-4971 | RuntimeOnly-AOT-Coverage fuer statisch identifizierbares Ziel herstellen | aktiv P0, erster Produktblocker; v26 fehlt bei `0x8C602B0A -> 0x8C010F22` ein AOT-Eintrag, interner Fehler `AotTemplateMismatch` (14) |

## Aktuelle Meilensteinzuordnung

- `KR-4965` ist ueber den belegten engeren allgemeinen Blocker abgeschlossen.
- `KR-4971` ist der erste aktive Produktblocker.
- `KR-4966` korrigiert den im realen v24-Schedulerrestore belegten absoluten Mess- und Meilensteinfehler.
- `KR-4967` bis `KR-4970` vervollstaendigen den strikt atomaren, digestgeprueften und Save-erhaltenden Vertrag aus `KR-4952` und `KR-4953`.
- `KR-4962` liefert die normative Game-Entry-Paritaet sowie den Sound-/Frame-Nachweis mit relativem 600-Millionen-Budget.
- `KR-4955` bis `KR-4960` folgen nach einem stabilen Game-Entry-Produktmeilenstein.
- `KR-4954` und `KR-4961` erzeugen danach das externe Spielprojekt.
- `KR-4963` laeuft parallel, sobald die Handoff-Grundlage stabil ist.
- `KR-4964` ist das abschliessende v0.49-Produktgate.
- `KR-4847`, `KR-4849`, `KR-4914` und `KR-4916` werden nur aktiv, wenn ein echter SA-Produktlauf sie als naechsten Blocker belegt.
- `KR-4901` bis `KR-4905` beginnen erst nach `KR-4964`.

## Geplanter Lintervertrag

Der Roadmaplinter muss mindestens pruefen:

- doppelte aktive IDs
- unterschiedliche aktive Titel derselben ID
- unbekannte IDs ohne Registryeintrag
- Wiederverwendung von Gate-/Release-IDs
- fehlende `superseded_by`-Ziele
- zyklische Migrationen
- aktive Task ohne ROADMAP- oder TASKS-Eintrag
