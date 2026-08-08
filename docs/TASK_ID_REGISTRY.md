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

Der historische Quellstatus fuer KR-4951 bis KR-4973 in dieser Tabelle
bezieht sich auf `b01586a` mit Runtime-ABI 73, Block-ABI 5, Analyzer-ABI 6,
PlatformServices-ABI 13, Backend-Interface-ABI 12, Portprojektvertrag 62,
Native-AOT-Profil 11 und Partitionsschema 5. KR-4974 bis KR-4984 bilden den
ersten Kaltbuild-P0-Block vom 31. Juli 2026. `quellseitig implementiert,
Produktabnahme offen` ist ausdruecklich kein Produkt-Erfolg: Der frische
Sonic-PAL-NativeDisc-Lauf ueber 600 Millionen Post-Entry-Zyklen und der
Sichtnachweis stehen noch aus.

Der aktuelle Source-Checkpoint ist `a521999` mit Runtime-ABI 87,
Analyzer-ABI 31 und Portprojektvertrag 75. Der v56-Diagnosebefund erzeugte
kein Portartefakt und meldete `1/1191` committed Roots; er identifiziert
weder Root 0 noch einen anderen konkreten Root als terminale Fehlerursache.
Das Per-Function-Budget und die laufweiten Context-/Auswertungsaggregate
duerfen bis KR-4985 nicht arithmetisch zu einem gemeinsamen Messwert
verbunden werden. Der gegatete Kernpfad KR-4985 bis KR-4991 und KR-4993 ist
der aktive Folgeblock; KR-4992 ist nur ein optionaler Fehlgate-Zweig nach
KR-4981. Jeder Task wird nach Review direkt auf `main` gepusht, und dieser
Push gibt den naechsten ungegateten Task ohne weitere Nutzeranweisung frei.
Nur D1 und D2 benoetigen eine ausdrueckliche Lauf-Freigabe; bedingte Tasks
benoetigen ihr dokumentiertes positives G1-/G2-Messgate.

| ID | Titel | Status |
|---|---|---|
| KR-4951 | Produktgate nach Gastzyklen und getrennte visuelle Meilensteine | abgeschlossen, Folgearbeit KR-4966 |
| KR-4952 | Post-IP.BIN-Spielhandoff fuer DirectBootExecutable | quellseitig implementiert, Produktabnahme offen; atomarer CompletePlatform-Commit, Product-vs-Diagnostic-Evidenzbaseline und Save-Autoritaet vorhanden, frischer ABI-passender Handoff und normative Paritaet nach KR-4993 offen |
| KR-4953 | Privates Game-Entry-Handoff-Artefakt aus Original-GDI | quellseitig implementiert, Produktabnahme offen; 22-Geraete-/5-Event-Vertrag vorhanden, DirectBoot benoetigt fuer den spaeteren Lauf einen frischen ABI-passenden Capture |
| KR-4954 | Deklaratives externes Spielprojekt und CLI-Scaffold | aktiv P1; binaeres `GameProjectArtifact` Format 4 fuer Spielprojektvertrag 5 mit SHA-256, kombinierte `--game-project`-/Handoff-CLI und privater externer CMake-Generator belegt, Textdescriptor und wiederverwendbares Scaffold offen |
| KR-4955 | Explizite Funktionsgrenzen und Tabellenhinweise End-to-End | quellseitig implementiert, Produktabnahme offen; Guarded-AOT-Einstiege, Carrier-/Inventar-/Codepointer-Provenienz und Exportvollstaendigkeit reichen bis CFG/IR/AOT |
| KR-4956 | Static-AOT-Dispatchflucht inventarisieren und schliessen | quellseitig implementiert, Produktabnahme offen; Static-AOT-Seitentabelle, direkt gebundene Fastpaths und zielbezogene Revalidierung nach Codeinvalidierung vorhanden |
| KR-4957 | Direkte native Calls ueber sichere Timinggrenzen | quellseitig implementiert, Produktabnahme offen; direkte Owner-Einstiege, bekannte Calls und endliche indirekte Ziele umgehen den Zentraldispatcher unter Guards |
| KR-4958 | IR-basierte Registerlokalisierung und RAM-Regionen | quellseitig implementiert, Produktabnahme offen; IR-Liveness, GPR-/Skalar-Lokalisierung und `DirectLinearWriteBatch` mit deduplizierter SMC-Invalidierung vorhanden |
| KR-4959 | Ereignisgetriebene Scheduler-/IRQ-Safepoints | quellseitig implementiert, Produktabnahme offen; architektonische Grenzepochs, IRQ-Guards und regionsweise Cycle-/State-Commits vorhanden |
| KR-4960 | 200-MHz-Produkt-Hotpath | geplant P0 Performance-Gate |
| KR-4961 | Externes SonicAdventureRecomp-Bring-up-Projekt | aktiv P1; privates CMake-Projekt baut gegen das installierte Runtimepaket und erzeugt das hashgebundene v28-Artefakt, wiederverwendbares Scaffold und normaler Hook-/Port-Buildworkflow offen |
| KR-4962 | NativeDiscBoot-/DirectBoot-Paritaet am Game-Entry | quellseitige P0-Vertraege implementiert, Produktabnahme offen; zuerst gegateter Kernpfad bis KR-4991 und KR-4993, danach erster ABI-passender NativeDisc-Lauf, DirectBoot spaeter mit neuem ABI-passenden Handoff |
| KR-4963 | Inkrementeller Runtime-/Spielbuild und Compiler-A/B | aktiv P1; v28 warmer MSVC-Gateexport 4,209083 s, unveraenderter Hostbuild 0,219272 s, Runtime-/Hook-Schleifen und aktuelles Compiler-A/B offen |
| KR-4964 | v0.49 Produktabnahme bis sichtbarem Spielbild | Gate |
| KR-4965 | ADXT/mwSnd-Sound-Completion bis zum Writer schliessen | abgeschlossen ueber Alternativabnahme; allgemeine G2-Ursache repariert, alter Poll verlassen und engerer Blocker belegt |
| KR-4966 | Post-Entry-Produktgate und erforderliche Meilensteine | quellseitig implementiert, Produktabnahme offen; relatives Post-Entry-Ziel, getrennte Arbeitsmetriken und Gate-Exit nur bei vollem Budget plus Meilenstein |
| KR-4967 | Atomarer CompletePlatform-Capture-/Apply-Koordinator | quellseitig implementiert, Produktabnahme offen; alle falliblen Vorbereitungen vor Commit, CPU-PC/PR zuletzt, frischer ABI-passender Handoff offen |
| KR-4968 | AICA-/G2-/DMAC-/Scheduler-/IRQ-Handoff fuer Soundfortschritt | quellseitig implementiert, Produktabnahme offen; Adapter, Event-/IRQ-Vertrag und historische G2-Completion vorhanden, ABI-passende Paritaet offen |
| KR-4969 | PVR-/SPG-/ASIC-Handoff fuer den ersten Spiel-Frame | quellseitig implementiert, Produktabnahme offen; gastseitiger Zustand und neue Post-Entry-Evidenzbaseline vorhanden, sichtbare ABI-passende Abnahme offen |
| KR-4970 | Produkt-sicherer Maple-/VMU-Handoff und Event-Rehydration | quellseitig implementiert, Produktabnahme offen; installierte VMU-/Flashdaten bleiben autoritativ, Hostdiagnostik wird nicht als Gastzustand restauriert |
| KR-4971 | RuntimeOnly-AOT-Coverage fuer statisch identifizierbares Ziel herstellen | abgeschlossen; v28 emittiert und passiert die externe hashgebundene Grenze `0x8C010F22 + 0x18`, getrennte AOT-Template-Diagnose belegt |
| KR-4972 | Hashgebundene Shared-Callback-/Thunk-AOT-Coverage herstellen | quellseitig implementiert, Produktabnahme offen; Guarded-AOT-Entry und Exportinvariante erhalten Ziel/Shared-Body, reale Carrier-Kanten und Codepointerprovenienz ohne erfundene feste CFG-Kante |
| KR-4973 | NativeDisc-Sichtregression und proof-unabhaengige PVR-Ausgabe | abgeschlossen durch historische ABI-64-v32-Evidenz; Sega ab 2,032 s, 127 Hostframes, Fehler bei `553.990.562` / `11.080.283` an `0x8C11088C -> 0x8C64784E` |
| KR-4974 | Reproduzierbare Kaltbuild-Telemetrie und Miss-Reason-Ledger | abgeschlossen; JSONL-, Prozessbaum-, Phasen-, Workset- und Cache-Miss-Telemetrie produktiv verdrahtet |
| KR-4975 | Semantische FunctionEvaluation-Key-Projektion und Cachelinsen | abgeschlossen; versionierte fail-closed Register-/Stackprojektion, kanonische Ausgaenge, Set-Interning und Lens-Telemetrie produktiv verdrahtet |
| KR-4976 | Persistente FunctionValue-Programm-/SCC-Session | abgeschlossen; immutable Graphshards, persistente SCC-/ABI-/Summary-Epoch und gerichtete Invalidierung produktiv verdrahtet |
| KR-4977 | Gemeinsamer Multi-Root-Guarded-Inventory-Fixpunkt | abgeschlossen in `4d17526` |
| KR-4978 | Inkrementeller CFG-/Seed-/Candidate-Contract-Fixpunkt | abgeschlossen und re-reviewed |
| KR-4979 | Priorisierter Analyseexecutor und begrenzter Speicherhaushalt | implementiert und P0/P1-re-reviewed; v56 belegt offene Candidate-Resolution-Produktakzeptanz, Schliessung ueber gegateten Kernpfad bis KR-4991 und KR-4993 |
| KR-4980 | Schichtweiser persistenter NativeDisc-Buildcache | quellseitig implementiert und P0/P1-re-reviewed in `3c018be`; Produktmessung offen |
| KR-4981 | Einmaliges 24-Thread-Sonic-Produktzeitgate | P0 offen als erster voller Produktmesspunkt nach KR-4993; je reviewtem Sourcekandidaten hoechstens ein Lauf, keine Vorab-Buildmatrix |
| KR-4982 | GPU-Offload-Entscheidungsgate und repraesentativer Prototyp | vorerst gestrichen; nur mit neuer ausdruecklicher Nutzerfreigabe |
| KR-4983 | Deterministische capability-gated GPU-Beschleunigung | vorerst gestrichen; nur mit neuer ausdruecklicher Nutzerfreigabe |
| KR-4984 | Unabhaengige Gesamtpruefung und P0/P1-Schliessung vor NativeDisc-Produktlauf | historisches Sourcegate vor v56, dreifach re-reviewed; Candidate-Resolution-P0 folgt im gegateten Kernpfad bis KR-4991 und KR-4993 |
| KR-4985 | Candidate-Resolution-Phasen- und Kardinalitaetstelemetrie | geplant P0; Telemetrie plus D1-sichere Stale-/Cancellation-/Fehlerreihenfolge, keine Schedulingstrategieaenderung |
| KR-4986 | Semantische Context-Lanes und exakte Provenienzabonnenten | geplant P0; Full-State-paritaetische Semantik-/Provenienztrennung |
| KR-4987 | Read-Lens-projizierte Context-Identitaet | bedingt geplant P0; nur bei positivem Messgate G1, sonst FullState |
| KR-4988 | Internierte AbstractStates und Function-Value-Summaries | bedingt geplant P1; nur bei positivem Zehn-Prozent-Kostengate, sonst gemessener Skip |
| KR-4989 | Indexierte exakte Context-Bindings | bedingt geplant P1; nur bei positivem Zehn-Prozent-Bindinggate, Indexlookup mit unveraendertem Join-Fallback |
| KR-4990 | Inkrementelle Contextual-Dependency-Views | bedingt geplant P1; nur bei positivem Zehn-Prozent-Kosten- und 50-Prozent-Reusegate, sonst Full-Rebuild |
| KR-4991 | Versionierte monotone Context-Worklist | bedingt geplant P0; D2 entscheidet vor Taskbeginn, Umbau nur bei positivem Barrier-Messgate G2 |
| KR-4992 | Begrenzte Spekulation spaeterer Resolution-Roots | optionales P1 erst nach verfehltem KR-4981 und positivem Restkosten-/RAM-Gate; danach KR-4993 vor Retry |
| KR-4993 | Abschlussreview der Candidate-Resolution-Pfade | geplant P0 als quellseitiger Gate-Vorbereitungstask vor der globalen Produktabnahme in KR-4981 |

## Aktuelle Meilensteinzuordnung

- `KR-4965` ist ueber den belegten engeren allgemeinen Blocker abgeschlossen.
- `KR-4971` ist durch den v28-Produktlauf abgeschlossen.
- `KR-4972` ist quellseitig implementiert; Guarded-AOT-Einstieg und
  Exportvollstaendigkeit sind vorhanden. Ob der historische Missing-AOT-
  Grenzpunkt dadurch real passiert wird, entscheidet erst der nach KR-4993
  zulaessige ABI-passende Sonic-Lauf.
- `KR-4973` ist durch den sichtbaren v32-NativeDisc-Produktlauf abgeschlossen;
  seine ABI-64-Messwerte bleiben historische Evidenz. DirectBoot benoetigt
  fuer einen spaeteren aktuellen ABI-Pfad einen frischen
  CompletePlatform-Capture und bleibt in KR-4969/KR-4972 offen.
- `KR-4966` ist quellseitig implementiert; die Produktabnahme des relativen
  600-Millionen-Gates bleibt offen.
- `KR-4967` bis `KR-4970` sind quellseitig atomar und Save-erhaltend
  implementiert; ABI-passende Produktabnahme und normative Paritaetsdigests
  bleiben offen.
- `KR-4962` liefert die normative Game-Entry-Paritaet sowie den Sound-/Frame-Nachweis mit relativem 600-Millionen-Budget.
- `KR-4955` bis `KR-4959` besitzen den aktuellen P0-Quellpfad fuer
  Guarded-AOT, Static-Tier, direkte Owner-/Callziele, Registerlokalisierung,
  RAM-Batching/SMC und Safepoints. Ihre Produktwirkung sowie das
  200-MHz-Gate aus `KR-4960` bleiben offen.
- `KR-4954` und `KR-4961` entwickeln das jetzt erstmals produktiv verwendete
  externe Spielprojekt bis zum vollstaendigen Scaffold weiter.
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
