# KatanaRecomp-v0.49-Architektur

## v0.49.1: native Produktarchitektur

Ab `v0.49.1` ist verbindlich klargestellt: KatanaRecomp erzeugt native PC-
Ports und keinen Emulator. Statisch rekompilierter SH-4-Code ruft an
validierten Spiel-/SDK-Grenzen native PC-Dienste auf. Grafik laeuft ueber die
Host-GPU, Audio/Movie ueber native Hostdienste und Disc/Eingabe/Save ueber
native Plattform-APIs.

ARM7-Interpreter, CPU-PVR-Softwarerasterizer und ein vollstaendiger emulierter
Dreamcast-Geraeteverbund sind nur noch historische Bring-up-/Diagnosepfade und
duerfen nicht in das Produktbinary gelinkt werden. Der vollstaendige
Vorrangvertrag steht in `NATIVE_PORT_PRODUCT_CONTRACT.md`; widersprechende
aeltere Abschnitte dieses Dokuments beschreiben nur den historischen Stand.

KatanaRecomp ist ein statischer Recompiler fuer Dreamcast-SH-4-Programme. Der
Produktpfad ist:

```text
Dreamcast-Programm
  -> SH-4-Analyse
  -> Katana-IR und Optimierung
  -> natives C++
  -> Hostcompiler
  -> natives Spielprojekt
```

Ein normaler Port enthaelt keinen allgemeinen SH-4-Interpreter, keinen JIT und
keinen Emulationsfallback. Nicht vorab kompilierter oder nicht mehr gueltiger
Code endet an einer typisierten Runtimegrenze. Der begrenzte
Diagnoseinterpreter ist nur Bestandteil eines ausdruecklich als
`diagnostic_partial` erzeugten Diagnoseports.

Der aktuelle KR-5003-Stand verwendet Runtime-ABI 95, Block-ABI 5,
Analyzer-ABI 36, PlatformServices-ABI 14, Backend-Interface-ABI 19,
Portprojektvertrag 82, Native-Port-Profilvertrag 7, Native-AOT-Profil 27 und
Partitionsschema 7. Die historischen Checkpoint- und Laufangaben dieses
Dokuments bleiben an ihre damaligen Vertraege gebunden. Der aktuelle native
Produktpfad ist durch KR-5000 physisch von den historischen Diagnosegeraeten
getrennt; der naechste aktive Task ist KR-5001.

## Drei Ebenen

### KatanaRecomp

Der Werkzeugkern ist titelunabhaengig und verantwortlich fuer:

- SH-4-Decoder und Kontrollflussanalyse;
- Funktions-, Block-, Jump-Table- und Callbackerkennung;
- Katana-IR, Optimierungen und statische C++-Codegeneration;
- partitionierte, reproduzierbare AOT-Quellen und Metadaten;
- stabile allgemeine Hook-, Patch- und Spielprojektvertraege.

Titeladressen, Discidentitaeten, private Symbole, Rendererpatches und
spielbezogene Installerlogik gehoeren nicht in diesen Kern.

### KatanaRuntime

Die installierbare Produktruntime stellt nur statisches SH-4-AOT, ordinary
guest memory, den unabhaengigen Native-Port-Kontext und native Hostgrenzen
bereit. `KatanaRecomp::native_port_runtime` bindet
`KatanaRecomp::aot_runtime`; weder Target enthaelt ARM7/AICA, PVR/TA, ASIC,
GD-ROM, Maple, Firmwareboot oder Interpreter.

`KatanaRecomp::runtime_core` und `KatanaRecomp::runtime` bleiben nur im
Buildbaum als historisches Diagnoseorakel. Sie werden nicht installiert und
sind kein Portprofil.

### Externes Spielprojekt

Ein separates Projekt darf versions- und hashgebunden enthalten:

- explizite Funktionsgrenzen, Jump Tables und Callbacktabellen;
- bekannte Runtimecode-Templates;
- schwache oder erforderliche native Funktionsoverrides;
- bedingte Mid-Function-Hooks mit Continue-, Jump-, Return- oder Abort-Aktion;
- titelbezogene Symbole und optionale Direct-Boot-Konfiguration;
- eine hashgebundene `GameEntryHandoffBinding` sowie den zugehoerigen privaten
  Runtimeprovider.

Die Schnittstelle validiert Identitaet, Sortierung, Adressbereiche und
Kontrolltransfervertraege fail-closed. Sie kopiert durch das Binden einer
Definition keine Titeldaten in KatanaRuntime.

Der oeffentliche C++-Vertrag liegt in
`katana/runtime/game_project.hpp`; der Export nimmt ihn ueber
`PortExportOptions::game_project` entgegen. Vollstaendige externe
Spielprojekte integrieren Definition, Callbackcode und Registrierung weiter
selbst in ihr Portbinary.

`GameProjectArtifact` Format 4 ist fuer Spielprojektvertrag 5 der besitzende,
binaere Transport fuer
rein deklarative Definitionen. Payload und Gesamtartefakt sind jeweils an
SHA-256 gebunden. Exakte Funktionsgrenzen, Jump-/Callbacktabellen,
Runtime-AOT-Templates, Symbole, Codeidentitaeten und Bootkonfiguration werden
serialisiert; native Overrides, Mid-Function-Hooks und private
Handoffprovider werden fail-closed abgewiesen. `port-executable
--game-project` kann mit `--game-entry-handoff` kombiniert werden. Die
vollstaendige Definition steuert Analyse und Export. Wenn keine nativen Hooks
eine externe Registrierung erfordern, traegt der erzeugte Port zur Laufzeit
nur die reduzierte Identitaets-, Boot- und Handoffdefinition. Dadurch
gelangen weder Titeladressen noch Payloadbytes in den generischen Kern.

## Game-Entry-Handoff

`GameEntryHandoff` Schema 3 trennt den Spieleinstieg vom allgemeinen
Post-BIOS-Zustand. Die Bindung umfasst Content- und
Boot-Executable-Identitaet, Konsolenprofil, Runtime-ABI,
Plattformzustandsvertrag und Descriptoridentitaet. Der Descriptor modelliert
den architektonischen CPU-Zustand einschliesslich physischer GPR-/FPU-Baenke,
MMU und Exceptionzustand, hashgesicherte RAM-Operationen, typisierte
Geraetezustaende und ausstehende Schedulerereignisse.

Der aktuelle Handoff-Quellvertrag besteht aus Handoff-Artefaktformat 2,
Runtime-ABI 95, Portprojektvertrag 82 und Plattformzustandsvertrag 2.
Vorhandene private
CompletePlatform-Artefakte aus den ABI-63-/ABI-64-Runden sind historische
Evidenz und muessen vor einem weiteren DirectBoot-Produktlauf fuer den dann
aktuellen ABI neu erfasst werden. NativeDisc benoetigt keinen
Game-Entry-Handoff. Private
titelgebundene Payloads werden durch den externen Spielprojektprovider
geliefert. Jede Slice ist an Offset, Groesse und SHA-256 gebunden, wird vor
der Freigabe vollstaendig validiert und anschliessend aus eigenem
unveraenderlichem Speicher gelesen. Lokale Pfade oder Payloadbytes werden
nicht Teil des generischen Spielprojektvertrags oder des erzeugten Portpakets.

`CompletePlatform` verlangt exakt 22 Geraeteinstanzen: PVR, GD-ROM, G1,
SH-4-DMAC, AICA, Maple, System Bus, System ASIC, Interruptcontroller,
Interruptrouter, Interruptregister, MMU, Cache, Store Queues, I/O-Ports,
Holly-G2-DMA, Holly-PVR-DMA, SH-4-TMU, SH-4-RTC-Clock, SH-4-RTC, SH-4-SCIF
und Flash. Dazu kommt eine bijektive typisierte Scheduler-Timeline.
Prozesslokale Event-IDs werden nicht serialisiert; die Zielruntime erzeugt
nach passiver Geraetewiederherstellung neue IDs. Hostseitige
`MediaVideo`-/`MediaAudio`-Ereignisse gehoeren nicht zum portablen
Dreamcast-Zustand und werden von der Zielsession neu aufgebaut.

Capture und Apply dieses vollstaendigen Vertrags sind im realen Produktport
belegt. `port-executable --game-entry-handoff` validiert das private Artefakt
bereits beim Export und bindet seine Identitaet in den Exportcache. Der
erzeugte Produktport nimmt den lokalen Pfad ueber
`KATANA_GAME_ENTRY_HANDOFF_PRODUCT` entgegen, prueft die exakte Bindung erneut
und registriert einen auf dieses Artefakt begrenzten Provider. Der weiterhin
vorhandene `CpuMemoryDiagnostic`-Teilpfad bleibt ausdrueckliche Diagnose und
ist kein Produkt- oder Bootnachweis.

Der aktuelle Koordinator validiert alle Payloads, Schedulerbeziehungen,
IRQ-/DMA-Quervertraege und eine abgetrennte CPU/MMU-Sicht vor der Mutation.
Alle falliblen Vorbereitungen werden vor dem globalen Commit abgeschlossen;
Speicher, Geraete und vorbereitete Schedulerdaten werden danach ohne
nachtraegliche Teilvalidierung veroeffentlicht, CPU-PC und PR zuletzt. Ein
vollstaendiger semantischer Recapture prueft den Zustand nach dem Restore.
Das produktive Handoffprofil trennt gastseitigen Geraetezustand von
Hostdiagnostik und setzt PVR-/Audio-/Produktevidenz am Game Entry auf eine
neue Baseline. Installierte VMU- und Flash-Nutzerdaten bleiben autoritativ und
werden nicht aus einem alten Capture zurueckgerollt. Diese P0-Vertraege aus
`KR-4967` und `KR-4970` sind im Quellpfad implementiert; normative
NativeDisc-/DirectBoot-Digests und die ABI-passende Produktabnahme bleiben
offen. Der naechste private Produktlauf folgt erst nach KR-4974 bis KR-4984.

### Belegter Produktstand

Die v24-Vergleichsgates endeten ohne erstes neues Geraete-, AOT- oder
Runtimeproblem jeweils bei Schedulerzyklus 600.000.000:

- `NativeDiscBoot`: 6,3161 Sekunden, 94,9954 effektive Gast-MHz,
  17.080.114 zentrale Dispatches und ein sichtbarer IP.BIN-Frame;
- `DirectBootExecutable`: Restore bei 415.233.270, anschliessend
  184.766.730 Post-Entry-Zyklen in 5,01505 Sekunden beziehungsweise
  36,8425 MHz, 16.033.676 zentrale Dispatches und noch kein sichtbarer Frame.

Der vom historischen Direct-Port gemeldete Wert 119,64 MHz verwendet
faelschlich den absoluten Schedulerstand als ausgefuehrte Arbeit. Der aktuelle
`KR-4966`-Quellvertrag berechnet das Ziel relativ ab Game Entry, berichtet
Restore-, End- und ausgefuehrte Post-Entry-Zyklen getrennt und liefert bei
unvollstaendigem Budget trotz bereits erreichtem Meilenstein keinen
erfolgreichen Produktgate-Exit. Die 16.033.676 Dispatches der historischen
Runde entsprechen 11,52 Zyklen pro Post-Entry-Dispatch und belegen noch
keinen Performancegewinn.

v26 korrigiert anschliessend `SB_G2APRO` und das reale AICA-Request-Level fuer
`ADTSEL=5`. v28 fuehrt die danach beobachtete exakte, hashgebundene
Funktionsgrenze aus dem externen Spielprojekt bis in Analyzer, CFG, IR und
AOT und passiert so den bisherigen KR-4971-Blocker. Der reale Lauf erreicht
Gastzyklus `553.990.562`, `138.757.292` Post-Entry-Zyklen und `10.079.932`
Zentraldispatches. Das sind `+1.086.915` Gastzyklen gegen v26. Die
tatsaechliche Post-Entry-Arbeit ergibt in 5,275792 Sekunden 26,3008 MHz
gegen 23,9578 MHz bei v26, also provisorisch `+9,78 %`, aber noch kein
600-Millionen-Gate.

Der historische v28-/v30-Blocker KR-4972 war
die historische private Callback-Kante. Das Ziel beginnt mit einem `BRA` auf den
gemeinsamen Pfad/Body. Die damalige generische Analyse gewann Ziel und
Body aus konkreter Codepointer-Provenienz ueber einen begrenzten
Tail-Jump-/Runtime-Frame-Pfad. Der damalige vollstaendige Export mit dem
externen Spielprojekt erhielt diesen Seed aber noch nicht in CFG, Source-Map
und AOT. Die terminale Diagnose meldete den Materializergrund
`AotTemplateMismatch` (14) bereits korrekt als `aot-template-mismatch`.
Der reale v30-Port endet deshalb mit denselben `553.990.562` Gastzyklen und
`10.079.932` Zentraldispatches wie v28. Sound-/G2- und technische
PVR-Evidenz bleiben erhalten, aber 15 neue reale Fensteraufnahmen bleiben
schwarz. Die frueheren warmen v28-Buildwerte bleiben der aktuelle
Warmbuildnachweis; der frische v30-Export war kalt.

KR-4973 trennt seit Runtime-ABI 64 den aktuellen VBlank-Scanout von der
Diagnoseproofqueue und sperrt das Flag-Poll-Batching unter aktiver MMU wieder
vor jeder Mutation. `port <gdi>` akzeptiert dieselbe externe
`--game-project`-Bindung wie DirectBoot. Der reale NativeDisc-v32-Lauf zeigt
dadurch in allen Aufnahmen den Sega-Lizenzscreen, praesentiert 127 Hostframes
und endet nach 6,701 Sekunden bei exakt demselben Zyklus `553.990.562` sowie
11.080.283 Zentraldispatches, provisorisch 82,67 MHz und derselben
KR-4972-Kante an der privaten Callback-Stelle wie DirectBoot-v30. Der sichtbare
Unterschied ist damit kein abweichender Gastfortschritt. DirectBoot selbst
braucht fuer einen spaeteren aktuellen Vertrag einen frischen ABI-passenden
Handoff; IP.BIN und
damit der Sega-Screen bleiben dort absichtlich uebersprungen. Diese
ABI-64-v32-Daten sind ausschliesslich historische Evidenz, einschliesslich der
53.677.056 Byte grossen Produkt-EXE und des sichtbaren Sega-Screens ab 2,032
Sekunden.

Seit diesem historischen Lauf ist die damalige KR-4972-Luecke im Quellpfad
geschlossen: Guarded-AOT-Einstiege bleiben als eigener, evidenzgebundener
Exportvertrag bis CFG, IR, Source-Map und statischer AOT-Ausgabe erhalten.
Kuenstliche Candidate-Carrier koennen reale Jump-Kanten nicht mehr anhand
bloss gleicher Callsite/Ziel-Paare entfernen, externe bedingte
Inventarnachfolger werden nicht mehr still als vollstaendig behandelt und
Adressprovenienz eines geladenen Objekts wird nicht als
Codepointerprovenienz des Inhalts vererbt. Der Export verlangt fuer jeden
akzeptierten Guarded-AOT-Einstieg einen statischen Block, ein natives Template
oder eine explizite Ablehnung. Ob der reale Sonic-Lauf damit
die private Callbackkante passiert, ist bis zur nach KR-4974 bis KR-4984 zulaessigen,
ABI-passenden Produktabnahme offen.

## Statischer und dynamischer AOT-Dispatch

Der Static AOT Fast Tier ist fuer nach dem Seal unveraenderliche native
Bloecke bestimmt. Eine kompakte zweistufige Tabelle bildet kanonische
Codepages und Halfword-Offsets direkt auf validierte AOT-Eintraege ab. Der
Caller fuehrt den bereits aufgeloesten Funktionszeiger aus; ein zweites
`RuntimeBlockTable::resolve` ist nicht erforderlich.

Der Dynamic AOT Tier bleibt fuer Runtimecode, Overlays, Module,
MMU-Varianten, Relocationen, Invalidierungen und Materialisierung
verantwortlich. Sein Ausfuehrungsdeskriptor traegt Blockhandle,
Funktionszeiger, virtuelle und physische Herkunft, Groesse, Variantenschluessel,
Endklasse, Runtime-Registrierung, optionalen Fastpath und die erforderlichen
Generationen.

Direkt abgebildete P1-/P2-Ziele pruefen zuerst den callsitegebundenen
Inline-Cache. Nur P0-/P3-/MMU-gemappte Ziele benoetigen immer die vollstaendige
Uebersetzung. Ein Cachetreffer bleibt an Adressraum-, MMU-, Runtime-, FPU-,
Code-, Modul-, Relocation- und Blockgeneration gebunden. Auch ein Treffer aus
der statischen Seitentabelle revalidiert nach globaler Codeinvalidierung die
zielbezogene Dispatchbarkeit; invalidierter statischer Code kann nicht allein
aufgrund eines neu aufgenommenen globalen Chain-Guards weiterlaufen.

## Function-Level-AOT

Der Produkt-Emitter gruppiert analysierte Gastfunktionen in native
C++-Funktionen. Interne Basic Blocks werden Labels, interne Kanten native
`goto`- beziehungsweise strukturierte Kontrollfluesse. Bewiesene direkte Calls
und eindeutige, live verglichene Callbackziele koennen andere AOT-Funktionen
direkt aufrufen. Ein statischer RuntimeBlock zeigt direkt auf den nativen
Owner-Einstieg des konkreten Blocks; der fruehere zusaetzliche
`dispatch_owner`-Wrapper entfaellt. Endliche indirekte Call-/Jump-Zielmengen
werden gegen das live geladene, unrelocatierte Gastziel verglichen und unter
denselben Timing-, Tiefen-, Code- und Architekturgenerationguards direkt
ausgefuehrt. Nur ein unbekanntes Ziel verlaesst diesen Pfad zum allgemeinen
validierenden Dispatcher.

Direkte Gastcalls verwenden einen threadlokalen Tiefenwaechter. Wird das
konservative Limit erreicht, bleibt `cpu.pc` auf dem bereits vorbereiteten
Gastziel und der Hoststack wickelt zum statischen Zentraldispatcher ab. Das ist
weder Interpretation noch Laufzeitdekodierung.

Registerlokalisierung und direkte RAM-Zugriffe sind konservative
Beweisoptimierungen. Der aktuelle Emitter verwendet IR-Use/Def und Liveness
fuer `r0` bis `r15` sowie T, PR, GBR, MACH, MACL und FPUL. Lokaler Zustand
wird an echten MMIO-, FPU-, Call-, Exception-, Hook-, SR-/Bankwechsel-,
Dispatch- und Safepointgrenzen abgegeben und nach einer weiterlaufenden
Grenze kontrolliert neu geladen; rohe C++-Fragmente werden nur
tokenbegrenzt umgeschrieben. Direkte P1-/P2-Haupt-RAM-Stores koennen in einem
fest begrenzten `DirectLinearWriteBatch` gesammelt werden. Vor Reads,
Terminals oder Architekturgrenzen wird der Batch abgeschlossen; bei
Guardmiss oder voller Batchkapazitaet faellt genau die betroffene Operation
auf den allgemeinen korrekten Speicherpfad zurueck. Codeinvalidierung,
Modul-/Blockinvalidierung und SMC-Beobachtung werden fuer die gesamte
beobachtete Schreibregion dedupliziert, nicht uebersprungen.

## Gastzeit und Diagnose

Reine native Regionen sammeln Gastzyklen. Ein Commit ist vor MMIO,
Schedulerereignissen, annehmbaren Interrupts, Exceptions, SR-/IMASK-Aenderung,
expliziten Safepoints oder dem begrenzten Cycle-Quantum erforderlich.
Interruptmetadaten werden ereignisgetrieben ueber Epoch, Pending-Level und
Pending-Maske aktualisiert.

Produkt-Performance verwendet feste Aggregatzaehler. Detaillierte
RuntimeOnly-Sitemetriken und ihre Map werden nur im Diagnoseprofil, bei
expliziten Diagnoseschaltern oder in der Runtimeprobe aktiviert. Der
vorreservierte Dispatchrecorder ist im normalen Produktmodus nicht an den
Dispatcher gebunden; Formatierung erfolgt terminal. Fastpathdeskriptoren
werden anhand stabiler Gastadressen direkt ausgewaehlt und nicht bei jedem
zentralen Dispatch linear gescannt. Wait-Loop-Rohwerte und vollstaendige
Dispatchereignisse sind ausdrueckliche lokale Opt-ins.

Die immer aktive Crash Capsule ist ein fester POD-Zustand mit einem
16-Ereignis-Ring fuer letzten Block, MMIO-Zugriff, Schedulerereignis und ersten
Fehler. Ihr Aufzeichnungspfad verwendet keine Strings, Maps, Heapallokationen
oder Locks; erst ein terminaler Fehler formatiert eine Zusammenfassung. Ein
automatisch durch Gastzyklus/PC/Fehler aktiviertes und begrenztes
Deep-Trace-Fenster bleibt offene Diagnosekonsolidierung.

## Sicherheits- und Eigentumsgrenzen

- Keine Sonic- oder sonstigen Titeladressen im generischen Kern.
- Keine Retailbytes oder lokalen Pfade im Repository oder Portpaket. Das
  Portpaket enthaelt nur die fuer die Installation erforderlichen
  Hash-/Contentidentitaeten; titelbezogene Identitaeten bleiben ausserhalb des
  generischen Kerns.
- Keine still erfolgreichen No-op-Stubs fuer unbekannte Hardware.
- Kein Flycast-, dcrecomp- oder sonstiger uebernommener Emulatorcode.
- Runtimecode wird nur nach Byteidentitaet, Herkunft und Generation aktiviert.
- DirectBoot und NativeDiscBoot verwenden dieselbe Dreamcast-Runtime; sie
  unterscheiden ausschliesslich die Bootstrapgrenze.
- Nur ein vollstaendig validierter `CompletePlatform`-Handoff darf den
  Produktpfad betreten; ein CPU-/RAM-Diagnosehandoff wird nicht als
  Plattform- oder Bootnachweis ausgegeben.
