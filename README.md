# KatanaRecomp

Aktuelle Pre-Alpha-Version: `0.49.1`

`0.49.1` ist die native Produktarchitektur-Runde. `0.50.0` bleibt
ausdruecklich die erste Alpha und wird erst freigegeben, wenn Sonic ohne
emulatoraehnliche Produktzustaende ueber den rein nativen PC-Pfad das
Hauptmenue erreicht.

## Verbindlicher nativer Produktpfad

KatanaRecomp erzeugt native PC-Ports und keinen Emulator. Der ausgelieferte
Port besteht aus statisch rekompiliertem SH-4-Spielcode und nativen PC-
Diensten fuer GPU-Grafik, Audio/Movie, Dateien, Eingabe und Speicherstaende.
Ein ARM7-Interpreter, ein CPU-PVR-Softwarerasterizer oder ein vollstaendig
emulierter Dreamcast-Geraeteverbund sind kein Bestandteil des Produktpfads.

Der bisherige RuntimeOnly-Stand bis `001f3c2` bleibt historische Bring-up-
Evidenz fuer AOT-Abdeckung, Adressen, Kontrollfluss und den echten No-Skip-
Movielebenszyklus. Seine AICA-/ARM7- und CPU-PVR-Geraetepfade werden nicht
weiter als Produktarchitektur optimiert. Die gewonnenen Grenzen werden
stattdessen XenonRecomp-artig an der hoechsten belegten Spiel-/SDK-
Schnittstelle auf native Hostimplementierungen gebunden.

Der vollstaendige verbindliche Vertrag und die neue Taskreihenfolge stehen in
[`docs/NATIVE_PORT_PRODUCT_CONTRACT.md`](docs/NATIVE_PORT_PRODUCT_CONTRACT.md).

Aktueller Bring-up-Stand dieses Meilensteins: Runtime-ABI 90, Block-ABI 5,
PlatformServices-ABI 14,
Analyzer-ABI 34, Function-Analysis-Epoch-Schema 27, lokales
In-Process-Evaluation-Cache-Schema 13, Application-Contract 8,
Portprojektvertrag 76, Native-Port-Profilvertrag 1 sowie PVR-State-Contract 3.
Aktuelles Native-AOT-Emissionsprofil: `25`, AOT-Partitionsschema: `5`.

Der letzte funktionale, jetzt historische RuntimeOnly-Source-Stand ist der
Runtime-Performance-Checkpoint. Die
oeffentlichen AICA-/ARM7-Handoff-Layouts sind deshalb auf Runtime-ABI 90
versioniert; PlatformServices-ABI 14 und Backend-Interface-ABI 13 bleiben
aktuell.
Die inkompatible Erweiterung der oeffentlichen SDK-Layouts
`PortExportOptions` und `LatentAotDiscoveryOptions` hebt das Backend-
Interface-ABI auf `13`; bestehende generierte Ports muessen neu exportiert
werden.

Der neue opt-in-Modus `port --analysis-mode runtime-only` ist nur fuer den
vollstaendigen NativeDisc-Produktport mit `--game-project` zulaessig; der
Default bleibt `platform`. RuntimeOnly setzt fuer die Bootanalyse konservativ
`GuestCallAbi::Unknown`, ueberspringt die blockierende SuperHC-
FunctionValue-/Candidate-Resolution, erzeugt weiterhin nativen AOT-Code und
verwendet RuntimeOnly-Dispatch mit exakter statischer Guest->Host-Tabelle.
Stop-on-miss und typed abort bleiben aktiv; es gibt keinen Interpreter, JIT,
Runtime-Decoder oder geratenen Zielpfad. Der Whole-Export-Cache ist an den
Analysemodus gebunden.

Der aktuelle 70-s-No-Skip-RuntimeOnly-Produktlauf erreichte das erforderliche
Milestone `FirstVisibleGameFrame` ohne Start-Impuls, Movie-Skip,
Framebuffer-Hack oder kuenstlichen Moviepfad. Der erste Frame ist durch Digest
`16866779858248182758` bei Gastzyklus `622122619` belegt. Es gab `341`
Renderrequests, Rendercompletions und Rendererframes, `15.680`
YUV-Makrobloecke sowie `470` Audiopuffer mit `345.450` Audiobildern.

Die PVR-Fullevidenz endete nach vier bewiesenen Frames mit `1.228.800`
geaenderten Pixeln; danach wurde keine fortlaufende Vollbild-Evidenz behauptet.
Der Audiohash `8399287713367543391` blieb zwischen YUV-Lauf und Audio-Umbau
identisch. Es gab keine Fatal- oder Runtimefehler. Die Vergleichsreihe der
identischen Produktarbeit stieg von `23,7959 MHz` ueber `24,1885 MHz` und
`24,2825 MHz` auf `24,2926 MHz`, insgesamt `+0,4967 MHz` beziehungsweise
`+2,09 %`.

Der fruehere Checkpoint `e1d8ade` bindet einen echten AICA-ARM7TDMI-Kern,
Sound-/Main-Interrupts,
REG_L/REG_M, portable Fortsetzung und die Common-Monitorregister fuer MIDI-
Leerstand, Channel-Lifecycle und Current Address. Der zuvor bei null stehende
Sofdec-Audiotakt erreicht nun `0x2D0` und `0x890` bei der Einheit `0xAC44`
(`44.100`). Im No-Skip-Lauf gingen beide Readinesspfade auf `1`, der
Movie-Lifecycle auf Status `5`, und YUV-/PVR-/FB_R-Publikation wurde sichtbar.
Der Hostprozess nutzte dabei nur etwa `1,64` Kerne beziehungsweise `6,8 %`
der 24-Thread-Kapazitaet; der Performance-P0 liegt daher weiter beim
seriellen Runtime-/Dispatch-Overhead. `100 MHz` und der anschliessende
Identity-Miss `0x8C054008 -> 0x8C9000E8` sind offen; Memory-Card-Screen und
Hauptmenue wurden noch nicht erreicht. Der sichtbare Audio-/Videopfad darf
bei der weiteren Performancearbeit nicht regressieren.
Der Default-PlatformAbi-Pfad
bleibt erhalten; Ordinary-/Inventory-Stack-Alias-Capture und Lane-Fusion sind
spaetere, deferred PlatformAbi-Optimierungsbefunde und nicht Teil dieses
Bring-up-Meilensteins.

## Historischer RuntimeOnly-Bring-up-Stand

Der aktuelle Lauf belegt den natuerlichen Audio-/Videopfad bis zum ersten
sichtbaren Spielbild; der Bring-up ist damit sichtbar, aber noch kein
Hauptmenue-/Memory-Card-Gate. `PVR-State-Contract 3` fuehrt die Sentinel-
Semantik fuer abgeschlossene Fullevidenz in Snapshot, Persistenz und
generiertem Produktpfad. Das oeffentliche Runtime-Layout bleibt kompatibel;
Runtime-ABI 90 wird nicht angehoben.

Der zugehoerige Runtime-Performance-Stand haelt ARM7-RAM/Registerlocks ueber
einen `run_cycles`-Batch, nutzt direkte AICA-Sound-RAM-Spans und persistente
Scratchpuffer, committed den 32-Byte-Channel-2-DMA-Pfad fuer PVR-Geraete
wortweise und beobachtet PVR-YUV-Konfigurationswechsel einmal je Guest-Write.

Die aktuelle generische Source-Wiring umfasst eine Cross-Shard-
Codecopy-Abhaengigkeit in `control_flow_analysis.cpp`, einen togglebaren
direkten AOT-Bytecopy-Batch in `port_export.cpp` und ein begrenztes
Post-Root-Drain fuer haengenbleibende Host-Build-Helfer in `main.cpp`.
Candidate-Resolution und PlatformAbi-Optimierungen bleiben deferred; der
RuntimeOnly-Pfad bleibt statisches AOT mit Stop-on-miss und typed abort.

Der terminale v56-Stand und die folgenden Candidate-Resolution-Laeufe sind
historische PlatformAbi-Diagnostik. Ihre Aussagen ueber fehlende Artefakte
gelten fuer diese alten Versuche, nicht fuer den aktuellen RuntimeOnly-
Bring-up. Die historischen Zaehldomaenen sind nicht gemeinsam scoped und
werden nicht zu Requeue- oder Per-Context-Messwerten verrechnet.

KR-4985, KR-4986, KR-4993, KR-4987, KR-4994 und KR-4995 sind source-seitig
abgeschlossen. KR-4988 bis KR-4991 bleiben bis zu ihren Gates inaktiv. Der
Candidate-Resolution-P0 bleibt als historischer PlatformAbi-Folgepunkt
dokumentiert; er ist nicht der aktuelle RuntimeOnly-Buildblocker. Der
verbindliche Performanceplan steht in
[`docs/P0_ROOT0_CANDIDATE_RESOLUTION_PERFORMANCE.md`](docs/P0_ROOT0_CANDIDATE_RESOLUTION_PERFORMANCE.md),
der uebergeordnete Kaltbuildvertrag in
[`docs/P0_NATIVE_DISC_COLD_BUILD_PERFORMANCE.md`](docs/P0_NATIVE_DISC_COLD_BUILD_PERFORMANCE.md).

Der Candidate-Domain-Top-Fix behandelt abgeschnittene begrenzte Candidate-
Domains als kanonisches absorbierendes Top und ist ueber Merge, Normalisierung,
Vergleich, Keys, Persistenz, Consumer und ABI-Promotion konsistent. Der
einmalige Lauf `kr4981-20260809-020628-2bfd8af5` endete nach `343,627 s` bei
identischer Nichtkonvergenz, zuletzt Wave `48`, ohne Publikation oder
Portartefakt. Bei Wave `39` waren die 16 geprueften Kernzaehler identisch zum
Vorlauf; der Fix ist daher ein Korrektheits-/Persistenzfix, kein belegter
Konvergenzhebel. KR-4981 bleibt offen.

Der abgeschlossene Diagnose-Unterauftrag erreichte im Lauf
`kr4981-20260809-024141-c4ffdf15` das vollständige `attempts=1024`-Gate und
wurde nach `244,549 s` bei Wave `24` gezielt beendet. `uncategorized=0` für
alle Top-8-Funktionen; kein Fehler, Hänger, Portartefakt oder
KR-4981-Produktgate. Dominant war `0x8C10E44E` mit ausschließlich SavedEpoch-
pending-ABI-Skalaren und unvollständigem Callee-Set-Stackvertrag.
Der SavedEpoch-Lifecycle-Fix ist source-seitig abgeschlossen. Offen bleibt die
gemeinsame Ordinary-/Registermetadaten-/Alias-/Watcher-/Loss-/MemoryEpoch-
Lifecycle-Ursache.

Der SavedEpoch-Lifecycle-Fix konsumiert current-tracking Pending-ABI-Skalare nur
an bewiesenen normalen Call-/Tail-ABI-Gates; detached Epochs bleiben unangetastet.
`candidate_payload_lost` ist ein absorbierendes Epoch-Top, waehrend konkrete
Evidence und Nested-/Current-Aliasfakten erhalten bleiben. Der historische
SavedEpoch-Lifecycle-Stand lief mit Epoch-Schema `17` und Analyzer-ABI `33`.
Der Lauf
`kr4981-20260809-031826-0616113a` endete nach `369,171 s` fail-closed wegen
Nonconvergence bei Wave `76`, ohne Publikation oder `game.exe`. Der alte
SavedEpoch-Pending-Blocker ist beseitigt; der gemeinsame Ordinary-/Register-
Metadaten-/MemoryEpoch-Lifecycle bleibt offen. KR-4981 bleibt offen.

Der historische Candidate-Resolution-Source-Stand ist `49b0f72a9f49d60a4eb6e0481460cd57c5625735`.
Er erlaubt strukturelle Contextual-Hybrid-Projektion mit retained sticky loss;
die autoritative Hybridprojektion schliesst Contextual-MAY-Joins und Forward-
Edges erneut vollstaendig.
erkennt SavedEpoch-Slot-Pending-Top in allen Truncation-/Publication-Checks
fail-closed, trennt öffentliche Provenance-Replay-Capsule-/Keybyte-Limits vom
semantischen Evaluation-Limit und belastet bei echtem Evaluation-Cap nur den
Evaluation-Zähler. Analyzer-ABI `34`, Function-Analysis-Epoch-Schema `27` und
lokales In-Process-Evaluation-Cache-Schema `13` sind aktiv; der bestätigte Build
war erfolgreich, die EXE trug den Zeitstempel
Build-Exit `0` nach ca. `48 s`; `build-contextual-dirty/katana-recomp.exe`
trug LastWriteTime `09.08.2026 09:08:11 +02:00`. Tests wurden nicht ausgeführt.

Der erledigte Source-Unterauftrag integriert eine begrenzte 17-Source-
Provenienz-Live-in-Map für R0-R15 plus incoming stack, getrennte conditional /
unconditional SavedEpoch-Mutation und Alias-Capture-Verträge, per-flow
Register-/Stack-Taints und Return-Maps, duale Ordinary-/Provenance-Projektion,
current-/detached-Alias-Watcher sowie Persistenz-, Key-, Shard-, Contextual-,
Root- und Loss-Integration. Robuste R0-indexed-/Predecrement-Korrekturen sind
enthalten; RTS bindet R0-Provenienz als conditional alias-capture, raw
stack-derived Rückgaben und Storage-Loads gehen fail-closed in unresolved
SavedEpoch, und defensives Storage-Repair löscht semantische sowie
Inventory-R15-Koordinaten vorher. Der current mutation receiver umfasst den
detached watcher; eine blanket `stack_may_derived`-Lattice ist nicht enthalten.

Der historische PlatformAbi-Produktlauf `kr4981-20260809-091410-2766aaa6` endete nach ca.
`275 s` gesamt (Candidate ca. `221 s`) mit `nonconvergence` nach drei
Amplifikationssamples: `0/1274` Roots, HOL `0`, Wave `107`, `280` Contexts,
`970` Semantic-Lanes, `1.861` physische, `2.526` logische Requests,
Input-Widening `536`, Summary `22`, Forward `123`, stale Requeues `272`,
stale Discards `806`, Cache `589.178.706 B`; keine Budgets erschöpft, keine
Publikation und kein Artefakt bzw. `game.exe`. Der Supervisor schrieb wegen
`taskkill`-Zugriffsverweigerung keine Summary; der Kill-on-close-Job beendete
den Child trotzdem. Admission `1024/1024`, projected context/match jeweils
`0`. `0x8C641202` blieb bei `84/84` Attempts/Semantic Changes und `508`
Ordinary-Stack-Deltas trotz vollständigem Stackvertrag.

Der vorherige Produktlauf `kr4981-20260809-083308-4a3ff9be` endete nach
`286,387 s` (Candidate ca. `232,5 s`) nach drei zehnsekündigen
Amplifikationssamples mit `nonconvergence`/Wrapper-Exit `31`, ohne Crash:
`0/1274` Roots, HOL `0`, Wave `119`, kein Epoch-Publish/Discard und kein
Portartefakt oder `game.exe`. Final: `280` Contexts, `972` Lanes, `2.011`
physische, `2.814` logische, `203` Cache-Reuses, `2.790` Subscriber,
Provenienz `169.824`, Frontier `43` (max `250`), Cache `610.295.241 B`;
kein Budget war erschöpft. Das `attempts=1024`-Gate war `1024/1024` erfolgreich,
aber projected context/match jeweils `0`; der P0 bleibt intra-context semantic
widening mit Ordinary-Stack-/lokalen Stackkoordinaten.

Der vorherige Lauf `kr4981-20260809-041945-21b70ade` endete nach `425,924 s`
(Candidate ca. `341 s`) bei Wave `60`, `0/1194` Roots, `758` Semantic-Lanes,
`984` physischen und `1.398` logischen Auswertungen, `248` Input-, `102` stale-
Requeues und `347` stale Discards. Cache: ca. `501 MB`; Peak Root
`1.606.066.176 B`, Peak Job `1.814.822.912 B`; kein Portartefakt.

Der Vergleichslauf `kr4981-20260809-050420-3f47fd65` wurde nach `322,632 s`
(Candidate `237,116 s`) wegen belegter Nichtverbesserung beendet: Wave `39`,
`0/1194` Roots, `272` Contexts, `549` Lanes, `630` physische, `894` logische,
`181` Input-, `10` Summary-, `76` stale-Requeues, `226` Discards,
Provenienz `31.713`, Cache `455.638.275 B`, maximale physische Dauer
`42,359 s`, Peak Root `1.490.157.568 B`, Peak Job `1.672.388.608 B`; kein
`game.exe`. Das `attempts=1024`-Gate war gegenüber `9baea88` bitgleich:
`admission_success=999`, `projected_context_changed=0` und
`projected_match_changed=0`. Die Gateänderung ist korrekt, aber kein
Konvergenzhebel. Der historische PlatformAbi-P0 bleibt intra-context Ordinary-Stack: Die
vollstaendige autoritative Hybrid-Join-Closure ist beim vollstaendigen
Stackvertrag/Gate noch nicht wirksam. LocalStackCoordinate-/unvollstaendige
Stackvertraege bleiben sekundaer zu pruefen; keine Budget-/Thread-Erhoehung und
kein weiterer SavedEpoch-/Provenienzumbau.

```text
Runtime-ABI:                    90
Block-ABI:                       5
Analyzer-ABI:                   34
PlatformServices-ABI:           14
Backend-Interface-ABI:          13
Portprojektvertrag:             76
Native-Port-Profilvertrag:       1
Native-AOT-Emissionsprofil:     25
AOT-Partitionsschema:            5
```

KatanaRecomp ist ein unabhaengiges C++20-Framework fuer die statische
Rekompilierung von Dreamcast-SH-4-Programmen:

```text
Dreamcast-Programm
  -> SH-4-Analyse
  -> Katana-IR
  -> natives C++
  -> Hostcompiler
  -> natives Spielprojekt
```

Der normale Produktpfad enthaelt keinen allgemeinen SH-4-Interpreter, keinen
JIT und keinen Emulationsfallback. Nicht vorab gebundener Code sowie unbekannte
Hardwarewirkungen scheitern sichtbar und typisiert. Ein begrenzter
Diagnoseinterpreter ist nur in einem ausdruecklich als `diagnostic_partial`
exportierten Diagnoseport vorhanden.

BIOS-Dateien, Disc-Images, Boot-Executables, kommerzielle Assets und aus
Retailspielen erzeugte Quellen gehoeren nicht in dieses Repository oder in
verteilbare Portpakete.

## Architektur

v0.49 trennt drei Verantwortungsbereiche:

1. **KatanaRecomp** analysiert SH-4, erzeugt IR, optimiert und emittiert
   statische native C++-Quellen.
2. **KatanaRuntime** stellt die titelunabhaengigen Dreamcast-Grenzen fuer CPU,
   Speicher, MMU, Scheduler, Interrupts, BIOS, GD-ROM, PVR/TA, AICA, Maple,
   Video, Audio und Eingabe bereit.
3. **Externe Spielprojekte** duerfen hashgebundene Funktionsgrenzen,
   Jump-/Callbacktabellen, Runtimecode-Templates, native Overrides,
   Mid-Function-Hooks, Symbole und Direct-Boot-Konfiguration enthalten.

Titeladressen, Discidentitaeten, private Symbole und Spielpatches bleiben
ausserhalb des generischen Katana-Kerns.

Details: [v0.49-Architektur](docs/ARCHITECTURE_V049.md)

## AOT-Ausfuehrung

Der Static AOT Fast Tier bildet kanonische Codepages und Halfword-Offsets ueber
eine kompakte zweistufige Tabelle direkt auf validierte native
Funktionszeiger ab. Der Dispatcher fuehrt den bereits aufgeloesten
Ausfuehrungsdeskriptor aus; ein zweites Tabellenlookup ist nicht erforderlich.

Der Dynamic AOT Tier bleibt fuer Runtimecode, Overlays, Module, MMU-Varianten,
Relocationen, Materialisierung und Invalidierung verantwortlich. P1-/P2-Ziele
verwenden einen fruehen callsitegebundenen Inline-Cache mit vollstaendigen
Generationguards.

Function-Level-AOT fasst analysierte Gastfunktionen zu nativen Funktionen mit
internen Labels zusammen. Bewiesene Calls koennen direkt zwischen AOT-Funktionen
wechseln; ein Host-Stackwaechter wickelt tiefe Gastrekursion sicher zum
statischen Dispatcher ab. Die planbasierte Registerlokalisierung haelt
ausgewaehlte GPRs sowie T, PR, GBR, MACH, MACL und FPUL ueber native
Funktionsregionen lokal und gibt sie an Architekturgrenzen explizit frei
beziehungsweise laedt sie danach neu. FPU-Registerarrays bleiben bewusst
ausserhalb dieses Vertrags. Direkte Haupt-RAM-Zugriffe und lokalisierte
Register bleiben konservativ an Watchpoint-, Trace-, MMIO-, Exception-,
Interrupt-, SR-/Bank- und Invalidierungsgrenzen gebunden.

## Executable-First-Entwicklung

Die private `.gdi` wird fuer Bring-up nicht mehr bei jeder Iteration analysiert.
Sie dient einmalig zur Extraktion:

```powershell
.\build-current\katana-recomp.exe extract-boot-executable `
  D:\eigene-disc\game.gdi `
  --output D:\private\game-boot
```

Anschliessend arbeiten Analyse, Codegen und Warmbuild mit dem unveraenderlichen
Executable-Artefakt:

```powershell
$env:KATANA_PORT_BUILD_PROFILE = 'bringup'
$env:KATANA_HOST_BUILD_GENERATOR = 'Ninja'
$env:KATANA_PORT_CXX_COMPILER = 'msvc'
$env:KATANA_HOST_BUILD_JOBS = '8'
$env:KATANA_PORT_CODEGEN_JOBS = '8'

.\build-current\katana-recomp.exe port-executable `
  D:\private\game-boot\boot.katana-executable `
  --output D:\private\ports\game-direct `
  --target-name GameDirect `
  --console-profile europe-pal `
  --game-project D:\private\game-project.katana-game-project `
  --game-entry-handoff D:\private\game-boot\game-entry.katana-handoff
```

Das Artefakt enthaelt lokal `boot.bin`; diese Datei ist Retailinhalt und darf
nicht verteilt werden. Der erzeugte Port enthaelt nur AOT-Code, Metadaten und
Hash-/Installationsvertraege.

Der vollstaendige Discpfad bleibt erhalten:

```powershell
.\build-current\katana-recomp.exe port `
  D:\eigene-disc\game.gdi `
  --output D:\private\ports\game-disc `
  --target-name GameDisc `
  --console-profile europe-pal
```

`DirectBootExecutable` ist der executable-first Entwicklungspfad. Ein
bewiesener Spieleinstieg benoetigt dabei einen titel- und
Executable-identitaetsgebundenen `GameEntryHandoff` aus dem externen
Spielprojekt. Der aktuelle Handoff-Vertrag verwendet Schema 3,
Handoff-Artefaktformat 2 und Plattformzustandsvertrag 2; der dokumentierte
Source-Checkpoint `18f8537` verwendet Runtime-ABI 85 und
Portprojektvertrag 75. Davon getrennt verwendet `GameProject` Vertrag 5 und
Artefaktformat 4. `CompletePlatform` erfasst und restauriert den kanonischen
Satz aus 22 Dreamcast-Geraeten einschliesslich Flash sowie die exakte
typisierte Scheduler-Timeline. Capture und Apply sind nur im historischen
Produktport belegt. Der historische PlatformAbi-D-Lauf war der freigegebene KR-4981-
Produktversuch und bestand das globale Produktgate nicht; ein weiterer Lauf
ist nicht automatisch freigegeben.

`GameProjectArtifact` Format 4 transportiert fuer Spielprojektvertrag 5 die deklarativen,
hashgebundenen Spielprojektdaten ueber die CLI. Dazu gehoeren exakte
Funktionsgrenzen, Jump-/Callbacktabellen, Runtime-AOT-Templates, Symbole,
Codeidentitaeten und Direct-Boot-Konfiguration. Native Hookzeiger und private
Handoff-Payloads werden nicht serialisiert. Die vollstaendige Definition
steuert Analyse und AOT; ohne native Hooks bindet der erzeugte Port zur
Laufzeit nur Identitaet, Bootkonfiguration und Handoff.

Fuer den schmalen artifact-only Entwicklungsweg bindet
`port-executable --game-project ... --game-entry-handoff ...` beide privaten
Artefakte bereits beim Export an Executable-, Konsolen-, Projekt- und
Descriptoridentitaet. Die Artefakte werden nicht in den Port kopiert. Der
erzeugte Produktport laedt das Handoff lokal ueber
`KATANA_GAME_ENTRY_HANDOFF_PRODUCT`, prueft dieselbe Bindung erneut und wendet
den vollstaendigen Plattformzustand vor dem ersten Spielblock an.
`NativeDiscBoot` kompiliert weiterhin den disc-eigenen Bootstrap und bleibt
Referenz- und finales Genauigkeitsgate sowie Grundlage der
Nutzerinstallation. Beide Pfade verwenden dieselbe Dreamcast-Runtime; keiner
interpretiert SH-4.

Vollstaendiger Vertrag:
[Executable-First-Entwicklung](docs/EXECUTABLE_FIRST_DEVELOPMENT.md)

## Nutzerinstallation

Eine verteilte Port-EXE enthaelt keine Discsektoren. Jeder Nutzer installiert
seine eigene passende Originaldisc einmalig:

```powershell
.\GameDirect.exe --install-disc D:\eigene-disc\game.gdi
.\GameDirect.exe
```

Der Installer validiert Descriptor, Tracks, Geometrie, Hashes,
Contentidentitaet und Bootdatei, bevor er atomar
`user-data/content/game.katana-disc` anlegt. Dieser lokale Cache bleibt bei
Warmbuilds und Republishing erhalten und darf nicht weitergegeben werden.

## Bauen

Voraussetzungen:

- CMake 3.25 oder neuer;
- C++20-Compiler;
- Ninja oder ein anderer von CMake unterstuetzter Generator.

Windows/MSVC:

```powershell
cmake --preset msvc-relwithdebinfo
cmake --build --preset msvc-relwithdebinfo --target katana-recomp
```

Das installierbare CMake-Ziel lautet `KatanaRecomp::runtime_core`. Ein direkt
konfiguriertes Portprojekt kann es per `find_package` verwenden. Die
installierte CLI erkennt das Runtimepaket im gemeinsamen Installationspraefix
automatisch; `KATANA_RUNTIME_PREFIX` waehlt ein anderes installiertes Paket.
`KATANA_RUNTIME_BUILD_TARGETS` kann den
`KatanaRuntimeBuildTargets.cmake`-Export eines lokalen Buildtrees direkt
binden. Single-Config-Baeume muessen als `RelWithDebInfo`, `Release` oder
`MinSizeRel` konfiguriert sein; bei Multi-Config-Baeumen baut die CLI eine
vorhandene optimierte Konfiguration.
`KATANA_RUNTIME_ROOT` bleibt der explizite Quellbaum-Fallback und wird
`EXCLUDE_FROM_ALL` eingebunden. Generierte AOT-TUs verwenden die schmale
`katana/runtime/aot_runtime_abi.hpp` sowie eine PCH.

Profile und Toolchainauswahl:
[Portbuildprofile](docs/PORT_BUILD_PROFILES.md)

## Produkt-Gate

Bootkorrektheit und Performance sind getrennte Ergebnisse. Ein Produktlauf
wird nicht mehr anhand eines festen Drei-Sekunden-Hostlimits bewertet. Das
Budget bezeichnet im aktuellen Quellvertrag die ab Game-Entry auszufuehrende
Gastarbeit:

```powershell
$env:KATANA_GUEST_CYCLE_BUDGET = '600000000'
$env:KATANA_PORT_FINAL_PROGRESS = '1'
$env:KATANA_GAME_ENTRY_HANDOFF_PRODUCT = 'D:\private\game-boot\game-entry.katana-handoff'
.\GameDirect.exe
```

`KR-4966` berechnet daraus
`target_cycle = restored_game_entry_cycle + requested_post_entry_cycles` und
berichtet Restore-, Final- und ausgefuehrte Post-Entry-Zyklen getrennt. Bei
angefordertem Produktbudget ist Exitcode 0 nur mit vollstaendiger Gastarbeit,
erreichtem Pflichtmeilenstein und echtem `KATANA_PRODUCT_GATE` zulaessig. Die
Zusammenfassung nennt Gastzyklen, Hostzeit, Dispatches, technische
Framemarker und das erste neue AOT-, Runtime- oder Geraeteproblem; ein
sichtbarer Bildschirm wird separat anhand einer realen Ausgabeaufnahme
klassifiziert. Dieser Vertrag ist im Source-Checkpoint `18f8537`
implementiert, aber noch nicht mit einem aktuellen Sonic-Port abgenommen.
Der historische PlatformAbi-D-Lauf war der freigegebene KR-4981-Produktversuch; er bestand
das globale Produktgate nicht. Ein weiterer vollstaendiger privater
Produktlauf ist nicht automatisch freigegeben.

Die **historische v24-`CompletePlatform`-Vergleichsbasis** endete in beiden Pfaden bei
Schedulerzyklus 600.000.000 ohne erstes neues AOT-, Runtime- oder
Geraeteproblem:

- `NativeDiscBoot`: 6,3161 Sekunden, 94,9954 effektive Gast-MHz,
  17.080.114 zentrale Dispatches und ein sichtbarer IP.BIN-Frame;
- `DirectBootExecutable`: Restore bei 415.233.270, danach 184.766.730
  Post-Entry-Zyklen in 5,01505 Sekunden, also 36,8425 MHz ueber die
  tatsaechlich ausgefuehrte Gastarbeit, 16.033.676 zentrale Dispatches und
  noch kein sichtbarer Frame.

Der Direct-Port meldete aus dem absoluten Zaehler 119,64 MHz; dieser Wert ist
kein gueltiger Performancevergleich. Seine 16.033.676
Dispatches entsprechen 11,52 Post-Entry-Gastzyklen pro Zentraldispatch und
belegen noch keinen Hotpathgewinn.

Der **historische v30-DirectBoot** verwendet weiterhin das externe,
hashgebundene `GameProjectArtifact`. Die darin privat beschriebene exakte
Funktionsgrenze wird durch Analyzer, CFG, IR und AOT transportiert; dadurch
passiert der Produktlauf den bisherigen Blocker aus KR-4971. Der echte Lauf
endet bei Gastzyklus `553.990.562`, also nach `138.757.292`
Post-Entry-Zyklen und `10.079.932` Zentraldispatches. Gegen v26 sind das
`+1.086.915` Gastzyklen. Der historische v28-Fehler-zu-Fehler-Vergleich mass
dieselben `138.757.292` Post-Entry-Zyklen in 5,275792 Sekunden, also
26,3008 MHz gegen 23,9578 MHz bei v26 und provisorisch `+9,78 %`. Der
v30-Sichtlauf ist kein kontrollierter Performancebenchmark; wegen des
vorzeitigen Fehlers gibt es weiterhin keine 600-Millionen-Abnahme.

Sein erster Blocker war KR-4972:
`0x8C11088C -> 0x8C64784E`. Das unveraenderte Ziel beginnt mit einem Sprung
auf einen gemeinsamen Codepfad. Die generische Analyse verfolgt den
Callback jetzt ueber begrenzte Tail-Jump- und Runtime-Frame-Pfade, erkennt
`0x8C64784E` als Funktion und erreicht `0x8C6478C2` als gemeinsamen Body.
Der aktuelle Quellstand transportiert solche bewachten AOT-Einstiege durch
CFG, Source-Map und AOT und erzwingt ihre Exportvollstaendigkeit. Der
aktuelle Produktnachweis steht weiterhin aus; der aktuelle D-Lauf bestand das
KR-4981-Gate nicht und erzeugte kein Produktartefakt.
Die terminale Diagnose unterscheidet diesen Fall jetzt korrekt als
`aot-template-mismatch` von echten Byteidentitaetsfehlern.

Sound-/G2- und technische PVR-Evidenz bleiben erhalten: alle G2-Kanaele sind
inaktiv, zwei Direct-Frames enthalten 302.287 geaenderte Pixel. Der
v30-Sichtlauf bestaetigt mit 15 realen Aufnahmen erneut einen vollstaendig
schwarzen Hostscreen; der Host-Presenter meldet null Frames. Der frische
v30-Gateexport erzeugt 1.959 Funktionen in 42 Partitionen und eine
52.616.192 Byte grosse MSVC-EXE. Sein kalter Export ist nicht mit dem
frueheren warmen v28-Export von 4,209083 Sekunden vergleichbar. Das
200-MHz-Ziel, das relative Gate und ein sichtbarer
DirectBoot-Spielbildnachweis bleiben offen.

Der **historische Runtime-ABI-64-/NativeDisc-v32-Pfad** behebt zwei allgemeine
Sichtluecken: Flag-Poll-Batching ist unter aktiver MMU wieder fail-closed,
und ein gueltiger PVR-VBlank-Scanout wird unabhaengig von einem einmaligen
Diagnoseproof praesentiert. `port <gdi>` akzeptiert nun ausserdem dieselbe
hashgebundene Option `--game-project` wie `port-executable`.

Der kanonische, frisch exportierte und ueber die private Original-GDI
installierte v32-MSVC-Port zeigt ab 2,032 Sekunden den Sega-Lizenzscreen und
praesentiert 127 Hostframes. Er endet bei
Gastzyklus 553.990.562 und damit exakt wie DirectBoot-v30 an
`0x8C11088C -> 0x8C64784E`; NativeDisc ist sichtbar, DirectBoot-v30 blieb am
gleichen Punkt schwarz. Der Lauf dauerte 6,701 Sekunden, fuehrte
11.080.283 Zentraldispatches aus, erreichte 82,67 MHz bis zum Fehler und
erzeugte eine 53.677.056 Byte grosse EXE. Er erreichte wegen KR-4972 nicht
das 600-Millionen-Performancegate. Diese v32-Zahlen bleiben historische
Vergleichsevidenz und sind kein Nachweis fuer den aktuellen
Source-Checkpoint.

## Diagnose

- **Produkt-Performance:** feste Aggregatzaehler; RuntimeOnly-Sitedetails und
  deren Map bleiben im normalen Produktlauf deaktiviert.
- **Crash Capsule:** ein fester allokationsfreier POD-Ring haelt letzten Block,
  MMIO, Schedulerereignis und ersten Fehler; Strings entstehen erst terminal.
- **Fehlerdiagnose:** der bestehende begrenzte Dispatchrecorder wird nur bei
  expliziter tiefer Diagnose an den Dispatcher gebunden.
- **Tiefe Diagnose:** Wait-Loop-Rohwerttrace und vollstaendige
  Dispatchereignisse sind explizite lokale Opt-ins.

Ein automatisch durch Gastzyklus/PC/Fehler begrenztes Triggerfenster ist in
v0.49 noch kein abgeschlossener oeffentlicher Runtimevertrag.

Private Pfade, Identitaeten, Gastbytes und Titeladressen gehoeren nicht in
oeffentliche Berichte.

## Repository

```text
include/   oeffentliche Analyzer-, Codegen- und Runtimevertraege
src/       Implementierungen
tests/     bestehende enge Vertrags- und Regressionstests
docs/      Architektur-, Runtime-, Build- und Sicherheitsdokumentation
cmake/     Paket- und ABI-Versionierung
```

Wichtige Dokumente:

- [Roadmap](ROADMAP.md)
- [Verbindlicher Native-Port-Produktvertrag](docs/NATIVE_PORT_PRODUCT_CONTRACT.md)
- [v0.49-Architektur](docs/ARCHITECTURE_V049.md)
- [Executable-First-Entwicklung](docs/EXECUTABLE_FIRST_DEVELOPMENT.md)
- [Portbuildprofile](docs/PORT_BUILD_PROFILES.md)
- [Portexport und Originaldisc](docs/PORT_EXPORT.md)
- [Runtime-Vertrauensvertrag](docs/PORT_RUNTIME_TRUST_CONTRACT.md)
- [Runtime](docs/RUNTIME.md)
- [Indirect Control Flow](docs/INDIRECT_CONTROL_FLOW.md)
- [Sonic-Acceptancevertrag](docs/SONIC_ADVENTURE_ACCEPTANCE.md)
- [v0.49.1-Releasehinweise](docs/releases/v0.49.1.md)
- [v0.49.0-Releasehinweise](docs/releases/v0.49.0.md) (historisch)

## Rechtlicher Rahmen

KatanaRecomp wird unabhaengig entwickelt. Es enthaelt keinen Flycast-,
dcrecomp- oder sonstigen uebernommenen Emulatorcode. Externe Referenzen muessen
den Provenienz- und Lizenzregeln in
[REFERENCE_PROVENANCE.md](docs/REFERENCE_PROVENANCE.md) entsprechen.

Dreamcast und zugehoerige Marken sind Eigentum ihrer jeweiligen Rechteinhaber.
Dieses Projekt liefert keine Spiele, Firmware oder urheberrechtlich
geschuetzten Assets.
