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

Der aktuelle Source-Stand ist der Runtime-Performance-Checkpoint; Runtime-ABI
90, PlatformServices-ABI 14, Analyzer-ABI 34, Backend-Interface-ABI 13,
PVR-State-Contract 3, Function-Analysis-Epoch-Schema 27,
lokales In-Process-Evaluation-Cache-Schema 13. Der opt-in Modus
`port --analysis-mode runtime-only` ist nur mit `--game-project` fuer den
vollstaendigen NativeDisc-Produktport zulaessig; der Default bleibt `platform`.
Der aktuelle No-Skip-Lauf erreicht `FirstVisibleGameFrame`; `341`
Renderrequests/-completions/-frames, `15.680` YUV-Makrobloecke und `470`
Audiopuffer belegen den natuerlichen Pfad. Die Vergleichsreihe reicht bis
`24,2926 MHz`; `100 MHz`, Identity-Miss, Memory-Card-Screen und Hauptmenue
bleiben offen.

Historisch erzeugte v56 kein Portartefakt und
meldete `1/1191` committed Roots. Die einmalige D1-Nachauswertung lieferte
Root-0-Transportevidenz bis `185,370 s`, aber keinen vollstaendigen Root und
keinen erreichten Root 1; D1/G1 bleibt unentschieden. KR-4987 ist source-seitig
abgeschlossen; D9 ist beendet und Root 0 konvergierte fail-closed ohne
Portartefakt oder Produkterfolg. D2/G2 ist abgeschlossen und negativ; kein
positiver Schedulerhebel ist belegt. KR-4988 bis
KR-4991 bleiben inaktiv. KR-4994 und KR-4995 sind source-seitig abgeschlossen; die fehlende
Wirksamkeit der autoritativen Hybrid-Join-Closure beim vollstaendigen
Stackvertrag/Gate bleibt als historischer PlatformAbi-Befund dokumentiert.
KR-4981 bleibt das globale sichtbare Produktgate und ist nach dem
RuntimeOnly-Build-/Export-Gate noch offen.

Der fruehere D-Lauf dauerte `460,6 s` gesamt, Candidate Resolution ca.
`325,8 s`; der identifizierte Kindprozess wurde nach belegter
Nichtverbesserung manuell beendet. Ergebnis: `0/1194` committed Roots, HOL `0`,
Wave `103`, `1.044` Semantic-Lanes, `1.029` contextual physical evaluations,
`2.430` contextual logical requests, `1.359` Input-Widening-, `29` Summary-
und `733` stale-Dependency-Requeues, `1.359` stale snapshot discards,
`518.425.788 B` Cache-Payload und `0/0` publizierte/verwarfene Epochen. Kein
Portartefakt; KR-4981 ist nicht bestanden.

Der Lauf nach dem Candidate-Domain-Top-Fix (`kr4981-20260809-020628-2bfd8af5`)
endete nach `343,627 s` bei belegter identischer Nichtkonvergenz. Die letzte
Bewegung war Wave `48`, Peak Root `1.450.078.208 B`, Peak Job `1.618.132.992 B`;
bei Wave `39` waren die 16 geprueften Kernzaehler exakt wie im Vorlauf. Es gab
keine kanonische Publikation und kein Portartefakt; KR-4981 bleibt offen.

Der abgeschlossene Hot-Callee-Diagnoseunterauftrag erreichte im Lauf
`kr4981-20260809-024141-c4ffdf15` das vollständige `attempts=1024`-Gate und
wurde nach `244,549 s` bei Wave `24` gezielt beendet. `uncategorized=0` für
alle Top-8-Funktionen. `0x8C10E44E` isolierte SavedEpoch-pending-ABI-Skalare
als dominante semantische Änderungsdomäne; der Callee-Set-Stackvertrag war
unvollständig. Der Unterauftrag ist abgeschlossen, KR-4981 bleibt offen; der
SavedEpoch-Lifecycle-Unterauftrag ist abgeschlossen; der neue
Ordinary-/Registermetadaten-/MemoryEpoch-Analysepunkt bleibt offen.

Der Lauf `kr4981-20260809-031826-0616113a` endete nach `369,171 s` mit
`nonconvergence`/Exitcode `31` bei Wave `76`; D1024 und D2048 meldeten
`uncategorized=0`. Der SavedEpoch-Pending-Blocker ist beseitigt; der naechste
Root-Analysepunkt ist die gemeinsame Ordinary-/Registermetadaten-/Alias- und
MemoryEpoch-Lifecycle-Ursache. KR-4981 bleibt fail-closed offen.

Der historische Lauf `kr4981-20260809-083308-4a3ff9be` endete nach `286,387 s`
(Candidate ca. `232,5 s`) mit `nonconvergence`/Exit `31`: `0/1274` Roots,
Wave `119`, `280` Contexts, `972` Semantic-Lanes, `2.011` physische,
`2.814` logische, `203` Cache-Reuses, `2.790` Subscriber, Provenienz
`169.824`, stale Discards `922`, Frontier `43` (max `250`) und Cache
`610.295.241 B`; keine Publikation oder Artefakterzeugung. Admission war
`1024/1024`, projected context/match jeweils `0`. Die Rohwerte sind wegen des
frueheren Endes bei Wave `107` statt `119` nicht direkt vergleichbar; eine
materielle Produkt-/Performanceverbesserung ist nicht belegt. Der offene P0
liegt bei intra-context Ordinary-Stack und lokalen Stackkoordinaten. KR-4981
bleibt offen.

Der aktuelle Lauf `kr4981-20260809-091410-2766aaa6` endete nach ca. `275 s`
gesamt (Candidate ca. `221 s`) mit `nonconvergence` nach drei
Amplifikationssamples: `0/1274` Roots, HOL `0`, Wave `107`, `280` Contexts,
`970` Semantic-Lanes, `1.861` physische, `2.526` logische Requests,
Input-Widening `536`, Summary `22`, Forward `123`, stale Requeues `272`,
stale Discards `806`, Cache `589.178.706 B`; keine Budgets erschöpft, keine
Publikation und kein Artefakt. `0x8C641202` blieb bei `84/84` Attempts/Semantic
Changes und `508` Ordinary-Stack-Deltas trotz vollständigem Stackvertrag.
Der Supervisor schrieb wegen `taskkill`-Zugriffsverweigerung keine Summary;
der Kill-on-close-Job beendete den Child trotzdem.

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
| KR-4968 | AICA-/G2-/DMAC-/Scheduler-/IRQ-Handoff fuer Soundfortschritt | quellseitig implementiert; echter ARM7-/Sound-/Main-Interrupt- und Common-Monitorpfad unter Runtime-ABI 90 laeuft, vollstaendige Produktabnahme bleibt KR-4981 |
| KR-4969 | PVR-/SPG-/ASIC-Handoff fuer den ersten Spiel-Frame | quellseitig implementiert, Produktabnahme offen; Video-/ISP-/TSP-RenderDone-Fanout und resetfeste TA-Metrik unter Runtime-ABI 89 vorhanden, sichtbare Abnahme weiter offen |
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
| KR-4981 | Einmaliges 24-Thread-Sonic-Produktzeitgate | P0 globales Produktgate; RuntimeOnly erreicht `FirstVisibleGameFrame` und den natuerlichen Audio-/Videopfad bis `24,2926 MHz`; mindestens `100 MHz`, Memory-Card-Screen/Hauptmenue und der nachgelagerte Identity-Miss `0x8C054008 -> 0x8C9000E8` bleiben offen |
| KR-4982 | GPU-Offload-Entscheidungsgate und repraesentativer Prototyp | vorerst gestrichen; nur mit neuer ausdruecklicher Nutzerfreigabe |
| KR-4983 | Deterministische capability-gated GPU-Beschleunigung | vorerst gestrichen; nur mit neuer ausdruecklicher Nutzerfreigabe |
| KR-4984 | Unabhaengige Gesamtpruefung und P0/P1-Schliessung vor NativeDisc-Produktlauf | historisches Sourcegate vor v56, dreifach re-reviewed; Candidate-Resolution-P0 folgt im gegateten Kernpfad bis KR-4991 und KR-4993 |
| KR-4985 | Candidate-Resolution-Phasen- und Kardinalitaetstelemetrie | [x] source-seitig abgeschlossen; D1-Telemetrie explizit opt-in, Produktgate unentschieden |
| KR-4986 | Semantische Context-Lanes und exakte Provenienzabonnenten | [x] source-seitig abgeschlossen; Full-State-paritaetische Semantik-/Provenienztrennung |
| KR-4987 | Read-Lens-projizierte Context-Identitaet | [x] source-seitig abgeschlossen; vollstaendige Key-Bytes, strikter FullState-Fallback sowie exakte Provenienz/Restore; D9 beendet fail-closed, kein Erfolg behauptet |
| KR-4988 | Internierte AbstractStates und Function-Value-Summaries | bedingt geplant P1; nur bei positivem Zehn-Prozent-Kostengate, sonst gemessener Skip |
| KR-4989 | Indexierte exakte Context-Bindings | bedingt geplant P1; nur bei positivem Zehn-Prozent-Bindinggate, Indexlookup mit unveraendertem Join-Fallback |
| KR-4990 | Inkrementelle Contextual-Dependency-Views | bedingt geplant P1; nur bei positivem Zehn-Prozent-Kosten- und 50-Prozent-Reusegate, sonst Full-Rebuild |
| KR-4991 | Versionierte monotone Context-Worklist | bedingt geplant P0; D2 entscheidet vor Taskbeginn, Umbau nur bei positivem Barrier-Messgate G2 |
| KR-4992 | Begrenzte Spekulation spaeterer Resolution-Roots | optionales P1 erst nach verfehltem KR-4981 und positivem Restkosten-/RAM-Gate; danach Retry nur auf ausdrueckliche Freigabe |
| KR-4993 | Abschlussreview der Candidate-Resolution-Pfade | [x] source-seitig abgeschlossen; vollstaendiger Endreview wiederverwendet, Analyzer-ABI-Finding unter dem aktuellen Analyzer-ABI 34 geschlossen; globale Produktabnahme bleibt KR-4981 |
| KR-4994 | Begrenzter identitaetserhaltender unresolved Stack-/Context-Candidate-Carrier | [x] source-seitig abgeschlossen; bounded-merge/Pending-Carrier plus kanonisches absorbierendes Top fuer abgeschnittene Candidate-Domains ueber Merge, Normalisierung, Key/Persistenz, ABI-Promotion und Harvest; der Hybrid-Join-Befund bleibt historisch auf dem PlatformAbi-Pfad |
| KR-4995 | AICA-ARM7-Ausfuehrung und Sound-Interrupt-Lifecycle | [x] in `e1d8ade` source-seitig abgeschlossen; Runtime-ABI 90/AICA-Handoff 2, vorhandener Test und No-Skip-Sonic-Lauf belegen echten ARM7, fortschreitenden Sofdec-Audiotakt, Player-Status 5 und sichtbare Movie-Bildpublikation |

## Aktuelle Meilensteinzuordnung

- `KR-4965` ist ueber den belegten engeren allgemeinen Blocker abgeschlossen.
- `KR-4971` ist durch den v28-Produktlauf abgeschlossen.
- `KR-4972` ist quellseitig implementiert; Guarded-AOT-Einstieg und
  Exportvollstaendigkeit sind vorhanden. Ob der historische Missing-AOT-
  Grenzpunkt dadurch real passiert wird, entscheidet erst der nach KR-4994
  und Sol-Review zulaessige ABI-passende Sonic-Lauf.
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
