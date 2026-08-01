# Projektstatus

Aktuelle interne Version: `v0.49.0`

## Evidenztrennung

```text
letzte reale Produktevidenz: historischer ABI-77/78-NativeDisc-Stand
Source-Checkpoint:           18f8537
verbindlicher P0-Plan:       ffd45ae / KR-4974 bis KR-4984
aktueller Source:            Runtime-ABI 85 / Analyzer-ABI 23 /
                             Portprojektvertrag 75
letzter Exportversuch:       NativeDisc-v24 nach ca. 3 h 27 min abgebrochen
aktuelles Portartefakt:      keines
aktueller Sonic-Lauf:        keiner
aktueller Screenshot:        keiner
offene Produktabnahme:       Performancegate, Gesamtpruefung und erst danach
                             genau ein neuer NativeDisc-Sonic-Lauf
```

## Aktueller P0-Stand vom 31. Juli 2026

Source-Checkpoint `18f8537` enthaelt umfangreiche Umbauten, ist aber
ausdruecklich kein vollstaendiger P0-Abschluss. Quellseitig vorhanden sind:

- komponentenbezogene Analyse-, IR-, Codegen- und Orchestrierungsidentitaeten;
- ein gebundener positiver Boot-Analysecache sowie positiver und negativer
  Latent-AOT-Modulcache;
- ExactOnly-Discovery fuer vollstaendig manifestierte Module;
- getrennte native Templates und mehrere exakte SourceBindings fuer
  bytegleiche Disc-Extents;
- ein baseline-, Pixel-, Kachel-, Innenkachel-, Farb- und
  Luminanzklassen-gebundener Sichtbarkeitsklassifikator;
- strukturierte Fortschrittsereignisse und Heartbeats in langen
  Exportpfaden;
- ein gemeinsamer Runtime-Executor fuer beweisbar unabhaengige AICA- und
  PVR-Arbeit, eine aggregierte Prozess-CPU-Grenze und ein bevorzugter
  D3D11-Flip-Presenter mit gebundenem GDI-Fallback;
- weitere Function-Value-/Guarded-Inventory-Korrekturen und
  Rootfilterung.

Der fokussierte `katana-function-value-analysis-tests`-Lauf und der
Release-Build von `katana-recomp` sind gruen. Unabhaengige Teilreviews der
zuletzt korrigierten Contextual-/Rootfilter- und Runtime-Parallelpfade
meldeten keine bestaetigten P0/P1-Funde. Das ist **keine** abschliessende
Gesamtpruefung des gesamten Arbeitsbaums. Insbesondere sind Kaltbuildzeit,
Runtime-CPU-Entlastung, D3D11-Nutzung und ein moeglicher Analyse-GPU-Offload
noch nicht end-to-end abgenommen.

Der verbindliche Umsetzungs-, Mess-, GPU- und Abschlussvertrag steht in
[`P0_NATIVE_DISC_COLD_BUILD_PERFORMANCE.md`](P0_NATIVE_DISC_COLD_BUILD_PERFORMANCE.md).
KR-4974 und KR-4975 sind abgeschlossen; KR-4976 bis KR-4984 bleiben offen. Ein GPU-Compute-Pfad gilt nur nach
belegtem End-to-End-Gewinn; der vorhandene D3D11-Presenter ist
hardwarebeschleunigte Ausgabe, aber kein Beleg fuer GPU-beschleunigte
Analyse.

### KR-4974-Abschlussstand

KR-4974 ist implementiert. Vorhanden sind:

- versionierte opt-in JSONL-Datensaetze fuer Manifest, Fortschritt,
  Prozessbaumressourcen und terminalen Abschluss;
- eine begrenzte asynchrone Aufzeichnung mit expliziten Drop- und
  Vollstaendigkeitsfeldern, geordnetem Flush und atomarer terminaler
  Veroeffentlichung;
- fail-closed Pfadschutz gegen Quell-, GDI-Track-, Ausgabe-, Workspace-,
  Publishlock- und Windows-Geraetenamen-Aliase;
- Windows-Job-Object-Ressourcen und eine ehrlich qualifizierte
  POSIX-Prozessgruppenaufnahme, deren finale `wait4`-Werte auch bereits
  beendete Kinder fuer CPU, Faults und Peak-RSS erhalten;
- FunctionEvaluation-Lookups, Ready-Hits, In-Flight-Coalesces, Misses,
  Evictions, Eintraege, Bytes und genau einen primaeren Grund je Miss;
- getrennte Werte fuer geplant, gestartet, ready, kanonisch committed,
  dynamisch hinzugekommene Arbeit, Head-of-Line-Zeit und aktive Worker;
- eine deterministische retailfreie NativeDisc-Stressfixture in den Profilen
  `smoke` und `reference`.

Der kombinierte Build der betroffenen Targets, die engen Progress- und
Hostprozessvertraege sowie der reale Komponentenpfad der Fixture waren gruen.
Die anschliessende breite CLI-Matrix wurde nicht wiederholt: Sie hatte nach
einem in zwei Sekunden abgeschlossenen CMake-Configure einen zehnminuetigen
MSPDB-Helper-Stall offengelegt. Der Windows-Produktpfad isoliert MSPDB jetzt
pro Hostkommando und setzt dessen natuerliche Shutdownfrist auf eine Sekunde,
ohne die Job-Leere als Prozessbaumbeweis aufzuweichen. Diese letzte Aenderung
baut in `katana-recomp`; ihre massgebliche Abnahme erfolgt gemaess `AGENTS.md`
am spaeteren realen Produktpfad. Offen bleiben KR-4976 bis KR-4980, die
8-/12-/24-Thread-Gates KR-4981, das GPU-Gate KR-4982, gegebenenfalls KR-4983,
die Gesamtpruefung KR-4984 und erst danach der einzelne neue Sonic-Lauf.

### Abgebrochener NativeDisc-v24-Export

`v24` bezeichnet hier den lokalen Iterationsnamen und das Log
`v24-real-export.stdout.log`, nicht den wesentlich aelteren historischen
CompletePlatform-v24-Produktport.

Der Export verwendete den Source-Checkpoint-Arbeitsstand mit installiertem
Runtime-ABI-85-/Analyzer-ABI-23-SDK und 24 Analysejobs. Er wurde am
31. Juli 2026 nach etwa `3 h 27 min` auf ausdruecklichen Nutzerwunsch
beendet. `stderr` blieb leer, doch der Abbruch erfolgte mitten in der dritten
Function-Value-Neuberechnung. Es existiert weder das angeforderte
Portverzeichnis noch eine `game.exe`.

| Function-Value-Lauf | Candidate-Scope | Roots | Zustand | Candidate-Zeit | Hits | Misses | Hitquote |
|---|---:|---:|---|---:|---:|---:|---:|
| Pass 15 | 14, Parent 13 | 1.192 | abgeschlossen | 89,72 min | 6.801 | 77.745 | 8,04 % |
| Pass 22 | 16, Parent 15 | 1.416 | abgeschlossen | 110,71 min | 7.646 | 82.440 | 8,49 % |
| Pass 24 | 18, Parent 17 | 1.426 | bei 7 Roots abgebrochen | 3,56 min bis Abbruch | 503 | 27.244 | 1,81 % partiell |

Die Parent-Scopes und ihre Candidate-Resolution-Child-Scopes melden
dieselben FunctionEvaluation-Zaehler; sie werden nicht addiert. Die
Pass-24-Quote ist ein unvollstaendiger Fruehwert und nicht direkt mit den
abgeschlossenen Laeufen vergleichbar.

Pass 15 erweiterte die Seedmenge von `1.327` auf `1.382`. Weitere rekursive
Runden fuehrten zu `1.547` Seeds und Pass 22. Dessen Summary-Expansion
erhoehte die Menge auf `1.554`; rekursive Nacharbeit erreichte `1.557` und
startete Pass 24 als dritte volle Function-Value-Neuberechnung. Der live
beobachtete Working-Set-Peak betrug etwa `11,24 GiB`. Diese Messung ist eine
Prozessbeobachtung, keine abgeschlossene Benchmark.

Der v24-Versuch lieferte damit:

```text
Portexport:                      nein
Hostbuild:                       nein
Discinstallation:               nein
Sonic-Prozessstart:              nein
neuer echter Screenshot:        nein
Fortschritt ueber SEGA->leer:    nicht geprueft
Performance-P0:                 offen
```

Der vorherige ABI-78-NativeDisc-Lauf erreichte noch das vollstaendige
relative Produktgate:

```text
Startzyklus:                     415.233.270
Ziel-/Endzyklus:               1.015.233.270
Post-Entry-Gastzyklen:           600.000.000 / 600.000.000
Hostzeit:                            20,3550 s
effektive Gastgeschwindigkeit:       29,4768 MHz
Zentraldispatches:               66.212.631
Gastzyklen je Zentraldispatch:          9,06
PVR Requests / Completions:           112 / 112
Host-Presents:                         121
native Materialisierungen:             56
Interpreter-Materialisierungen:         0
erstes Problem:                       none
```

Diese historische ABI-78-Fensteraufnahme zeigt zuerst den
Sega-Lizenzscreen der PAL-Disc und danach einen
Sonic-eigenen Speicherkartenhinweis: `Memory card not ready. The game
cannot be saved. To save game files, insert a memory card into the
controller.` Massgeblich ist `KATANA_PRODUCT_GATE` mit exakt vollstaendiger
Gastarbeit.

Der anschliessende Runtime-only-Relink von `b064ee8` passiert die
vertauschte Maple-DMA-Schutzfensterdekodierung und erreicht einen neuen,
typisierten AOT-Blocker:

```text
Gesamtzyklus:                    557.991.327
Post-Entry-Gastzyklen:           142.758.057 / 600.000.000
Hostzeit:                              4,98722 s
effektive Abbruchrate:                28,6248 MHz
Post-Entry-Zentraldispatches:     14.408.160
erster Fehler:                    missing-aot / runtime-only tail-jump
Callsite -> Ziel:                 0x8C647B38 -> 0x8C010F0E
hoechster sichtbarer Screen:      Sega-Lizenzscreen; danach weiss
PAL-50-/60-Hz-Auswahl:            nicht erreicht
```

`a9cf938` schliesst die zugrunde liegende allgemeine Analyseluecke.
Ein decode-valides 32-Bit-PC-relatives Literal besitzt nun eine eigene
Inventarprovenienz und darf nur an einer echten Call-/Tail-ABI-Grenze zum
Codepointerargument werden. Gewoehnliche Objektfeldloads und bedingte
Owner-Uebergaenge erhalten diesen Beweis nicht. Das Callbackziel wird
dadurch als `StoredCodeAddress/GuardedPartial` fuer statisches AOT
inventarisiert; der indirekte Live-Tail bleibt `RuntimeOnly` und wird nicht
als feste CFG-Kante erfunden. Analyzer-ABI 11 invalidiert den alten
Analyse-/Exportbestand. Beide betroffenen bestehenden Regressionstargets
und `katana-recomp` sind gruen; zwei unabhaengige Finalreviews melden keine
P0/P1-Funde. Ein frischer Analyzer-ABI-11-Sonic-Export und Produktlauf
stehen noch aus.

Die Diagnose belegt dort ein vom Gast aufgebautes Runtime-Image.
`db9e721` bindet dessen externe, private Byteidentitaet allgemein an
statische Source-Bloecke und FixedAddress-Runtimeziele. Der erste v36-
Produkt-Export fand genau einen 132-Byte-Block unter 224 Imagebloecken und
stoppte vor dem Hostcompiler, weil das bisherige 128-Byte-Limit nur fuer
anonyme Runtimewrites gedacht war.

`4983bd1` behebt diese Vertragsvermischung ohne Sonic-Adresse im Kern:
FixedAddress-Ziele werden vorab auf exakten Quellblock, Groesse und
Identitaet geprueft, exakt gesnapshotet und gehasht; erst danach werden
Runtimewrite-Modul und Kontrolltransfer exakt fuer diesen Block
veroeffentlicht. Hashfehler mutieren den Modulkatalog nicht,
Fixed-Range-Loecher fallen weder in dynamischen Code noch in den
Diagnoseinterpreter, und normale Runtimewrites bleiben bei 128 Byte.
Vorcompilierte AOT-Bloecke umgehen nur irrelevante Laufzeitanalysebudgets;
Block-, Byte- und Proofbudgets bleiben aktiv. Runtime-ABI 77 und
Portprojektvertrag 67 binden den neuen Vertrag. Der bestehende
Native-AOT-Test materialisiert den konkreten 132-Byte-Fall einschliesslich
Hashfehler-vor-Mutation; der Portexporttest erzeugt denselben
Groessenvertrag. Beide Tests und der Finalreview sind gruen.
NativeDisc-v37 exportiert `2.902` Funktionen in `69` Partitionen und
materialisiert das gebundene Runtime-Image im echten Produkt ohne Fehler.
Der naechste offene Schritt ist die allgemeine Maple-/VMU-Erkennung hinter
dem sichtbaren Spielhinweis; Sonic-Adressen bleiben aus dem Kern.

`2f2d3b4` behebt die konkrete Ursache allgemein. Der Maple-Hauptperipherie-
Sender bildet Port und angeschlossene Subunits ab (`0x21` fuer Controller A
mit VMU A1), statt den Requestempfaenger `0x20` als Antwortsender zu
duplizieren. Die VMU stellt Media-Info, Blockread, vierphasiges Blockwrite
und Sync mit einem atomaren 512-Byte-Commit bereit. Ausstehende
Schreibphasen sind serialisierter gastseitiger Geraetezustand und werden im
Product-Handoff wiederhergestellt; installierte Savebytes, Dirtyzustand und
Hostschreibschutz bleiben weiterhin Zielautoritaet. Eine erstmalig
quelllose VMU wird deterministisch mit dem Standardlayout fuer 256 Bloecke
und 200 Datenbloecke formatiert, ohne vorhandene oder importierte
Arbeitskopien zu ersetzen. Runtime-ABI 78 und Maple-State-Vertrag 2 binden
die Aenderung. Die drei fokussierten Maple-/VMU-/Persistenztests,
`katana-recomp`, `diff --check` und der abschliessende kombinierte Review
sind sauber.

Die anschliessende terminale ABI-78-Diagnose belegt den naechsten
allgemeinen Blocker. Sonic startete `23` Maple-DMAs; alle endeten vor dem
ersten Deskriptor mit `MapleDmaError::ProtectedRange`, weshalb die
Transaktionshistorie leer blieb. Das programmierte
`SB_MDAPRO=0x404F` beschreibt die physische RAM-Spanne
`0x0C000000..0x0CFFFFFF`. Katana interpretierte die beiden
Siebenbit-Seitenfelder im Livepfad und im Handoff-Validator in umgekehrter
Reihenfolge. `b064ee8` vereinigt beide Pfade in einem fail-closed Helper:
Bits 8..14 sind die erste, Bits 0..6 die letzte 1-MiB-Seite. Der vorhandene
Maple-MMIO-Test deckt Sonics asymmetrisches Fenster im Live- und
Restorepfad sowie das invertierte Fenster ab. Fokustest, Runtime-Build,
`diff --check` und unabhaengiger Finalreview sind sauber; die reale
Produktabnahme des Fixes steht noch aus.

Der davor liegende reale ABI-73-NativeDisc-v33-Lauf erreicht sichtbar den
Sega-Lizenzscreen der PAL-Disc und passiert das zuvor fehlende statische Ziel
`0x8C11088C -> 0x8C64784E`. Er stoppt danach fail-closed bei Gesamtzyklus
`573.987.074` beziehungsweise `158.753.804` Post-Entry-Zyklen an der
PVR-Meldung `PVR-Hintergrundvertex ist kleiner als sein ISP/TSP-Format`.
Bis dorthin vergehen `6,87382 s`, entsprechend vorlaeufig `23,0954 MHz`,
bei `16.376.023` zentralen Dispatches. Das sind `+19.996.512` Gastzyklen
gegen den vorherigen Produktlauf. Das 600-Millionen-Gate ist nicht
vollstaendig und diese Rate daher nur eine Abbruchmessung.

`d3b87a1` korrigiert den allgemeinen Hardwarevertrag hinter diesem neuen
Blocker. `ISP_BACKGND_T` und `PARAM_BASE` adressieren Background-Parameter
im logischen 32-Bit-VRAM-Bereich; der Renderer hatte stattdessen den
numerisch gleichen Raw-/64-Bit-Backingoffset dekodiert. Parameter und
Vertices werden nun korrekt projiziert, Texturdaten bewusst nicht.
`FPU_SHAD_SCALE.bit8` wird ausserdem korrekt als Intensity-Volume
interpretiert; nur Parameter-Selection verwendet `3 + 2*SKIP`. Der
fokussierte bestehende Renderertest passiert die neue Poison-Regression
und die Shadow-Stride-Pruefung, bevor er spaeter an einer unabhaengigen
bestehenden Direct-C888-Pruefung endet. Die Sonic-Produktabnahme steht aus.

`20228a1` bindet auch die beiden Area-4-Direct-Texture-Pfade an ihre echten
Systembusregister. Path 0 (`0x11`/`0x118`) folgt `SB_LMMODE0`, Path 1
(`0x13`/`0x138`) folgt `SB_LMMODE1`; der Wert 0 waehlt Raw-/64-Bit-VRAM,
der Wert 1 die projizierte 32-Bit-Sicht. Speicherwrites, Channel 2 und
Renderer-Dirty-Evidenz teilen nun denselben live restaurierten Vertrag,
waehrend Gastreads aus Area 4 fail-closed abgelehnt werden.
Runtime-ABI 75 versioniert die geaenderte oeffentliche Signatur, das Enum
und das Rendererlayout. `katana-pvr-ta-tests`,
`katana-system-asic-tests` und der vollstaendige inkrementelle
MSVC-`katana-recomp`-Build sind gruen. Der Renderer-Sammeltest passiert die
neuen LMMODE-Pruefungen und endet erst am nachweislich bereits auf
`60887f4` vorhandenen Direct-C888-Assert.

`24d6132` schliesst die dazugehoerige allgemeine Analyseluecke: Der
Inventargraph transportiert Candidate-Calls getrennt vom semantischen CFG,
der Codepointermarker folgt nur dem uebergebenen Wert, und ein endlicher
bewachter Runtime-Stackframe-Delta darf ausschliesslich im Guarded-Inventory
Spill/Reload verbinden. Die echte PAL-Analyse liefert danach:

```text
Guarded-AOT-Einstiege:       2.221
typisierte Rejections:           0
rohe Stored-Kandidaten:        242 / 4.096
aufgenommene Kandidaten:       200 / 1.024
Inventory-Truncation:         false
Ziel 0x8C64784E:              aufgenommen
gemeinsamer Body:             0x8C6478C2
```

Akzeptierte Kandidaten koennen nicht mehr still aus der
Materialisierung verschwinden. Analyse und JSON berichten jede Ablehnung
mit Typ und Provenienz; der normale Produkt-Export stoppt damit vor
Codegen/Hostcompiler. Runtime-ABI bleibt 74, Analyzer-ABI ist 9,
Portprojektvertrag 65 und Native-AOT-Profil 13.

Der erste kalte ABI-73-Exportversuch lief 419,5 Sekunden und brach noch vor
dem Hostcompiler in der IR-Validierung ab. Die allgemeine Ursache war ein
bewachter Inventarkandidat in einem als `Mixed` markierten Bereich, dessen
Datentabelle zufaellig als gueltiger SH-4-Einstieg dekodierbar war. Der
naechste Quellstand schuetzt solche `Stored`-/`Returned`-Einstiege deshalb
durch einen begrenzten lokalen CFG-Strukturcheck, behandelt den
Ausfuehrungskontext eines Delay Slots vor den Rohopcode-Eigenschaften und
baut direkte Callee-Metadaten ausschliesslich aus den finalen
Blockterminatoren neu auf. Das ist ein generischer Analyse- und
IR-Vertrag; es wurden keine Sonic-Adressen oder Retailbytes eingetragen.
Der Strukturcheck liegt vor der globalen 1.024er-Inventaraufnahme, damit
abgelehnte Dateneintraege das Budget nicht belegen und spaetere gueltige
Einstiege nicht verdraengen koennen. Sein Ergebnis-Cache lebt ueber den
gesamten aeusseren Kontrollflussfixpunkt und dekodiert identische Kandidaten
nicht in jeder Iteration erneut.
Der reale ABI-73-Sonic-PAL-NativeDisc-v33-Export auf `1629268` war
erfolgreich:

```text
kalter Gesamtexport:              711,2 s
Funktionen / Partitionen:         2.519 / 63
Analyse-/IR-/Codegencache:        Miss / Miss / 0 Hits
Produkt-EXE:                      109.217.792 Bytes
Produkt-EXE SHA-256:              428A36CF3BDB1640B8C6225B771A7AE9B7E8E9EDB08E7C0851F464458145DBC7
unveraenderter Ninja-Warmbuild:   0,200236 s
Originaldisc-Installation:        3 Tracks / 521.461 Sektoren
Retailsektoren im Portpaket:      0
```

Ein zweiter identischer Voll-Export traf den aeusseren Analysecache nicht
und wurde nach 124 Sekunden beendet. Der direkte Host-Warmbuild ist schnell,
der persistente Voll-Exportcache war in dieser Produktevidenz defekt.

Der echte NativeDisc-Produktlauf praesentierte acht Hostframes und meldete
`KR_FIRST_IP_BIN_VISIBLE_FRAME`. Er stoppte fail-closed bei Gesamtzyklus
`487.233.787`, also 72.000.517 Zyklen nach dem bekannten Game Entry
`415.233.270`, bei `0x8C65EA06 -> 0x8C0101F2` mit
`missing-aot / guarded-fallback`. Nach Entry wurden 9.044.195 zentrale
Dispatches und 22 AICA-Puffer ausgefuehrt; es gab keine
Interpreter-Materialisierung. Das 600-Millionen-Post-Entry-Gate wurde nicht
erreicht, daher wird keine Gast-MHz-Zahl abgeleitet.

Der historische Gatewrapper liefert auf dieser typisierten Fehlerstrecke
faelschlich Exitcode 0 und die normale `KATANA_BRINGUP_RUN`-Zusammenfassung
fehlt. `cb5fb47` propagiert 0/1/3, liefert beim Watchdog 124, reicht
stdout/stderr weiter und erzeugt genau eine terminale Fehlerzusammenfassung.
Der vorhandene kleine CLI-Vertrag fuehrt diese Pfade einschliesslich eines
vorzeitig beendeten 600-Millionen-Laufs aus; der reale Sonic-Nachweis bleibt
offen. Die separate
v33-Sichtaufnahme wurde wegen des angeforderten Rechnerneustarts vertagt.

Der nachfolgende kalte Sonic-NativeDisc-Exportversuch auf `7ecdefb` endete
nach `381,413 s` noch vor Hostcompiler und Portpaket:

```text
function_budget_exhausted=1
candidate_inventory_truncated=1
returned_table_scan_truncated=0
candidates=29/1024
shape_budget_exceeded=0
CFG-Fixpunkt: i27 / s2023 / n215623 / c229443 / r6749
```

Die 65.536er-Grenze steht fuer ausgefuehrte Funktionsanalysen, nicht fuer
65.536 entdeckte Funktionen. Candidate-Call-Carrier waren entgegen ihrem
privaten Inventarvertrag in den semantischen Summary-Fixpunkt aufgenommen
worden. Wiederholte beziehungsweise mehrere Owner-Kontexte derselben
physischen Callsite konnten denselben Inputslot abwechselnd ersetzen und
Rueckkanten erneut einreihen. `cb5fb47` trennt deshalb
Summary- von Inventarcallees und vereinigt wiederholte Callargumente vor dem
interprozeduralen Update. Er behandelt ausserdem Multi-Owner-Shared-Tails als
begrenzte Inventarregionen, begrenzt rohe Stored-Kandidaten und verschaerft
den manifestgebundenen Whole-Export-Cache. Ein erfolgreicher Export oder
Sonic-Lauf aus diesem Stand existiert noch nicht.

## Zielarchitektur

KatanaRecomp, KatanaRuntime und das spaetere externe Spielprojekt sind getrennte Produkte:

```text
KatanaRecomp
  -> SH-4-Analyse, IR, Optimierung und C++-Codegen

KatanaRuntime
  -> gemeinsame Dreamcast-Laufzeitbibliothek

SonicAdventureRecomp
  -> titelgebundener Code, Handoff, Hooks, Installer und Produkt-EXE
```

KatanaRecomp und KatanaRuntime bleiben im selben Repository. Das konkrete Spielprojekt wird extern aufgebaut. Generischer Katana-Code darf keine Sonic-spezifischen Adresshacks oder Retailbytes enthalten.

## Historischer Reviewstatus vor dem aktuellen P0-Umbau

Auf `4983bd1` ohne offene P0/P1/P2 reviewt:

- vollstaendiger manifestgebundener Whole-Export-Cache;
- evidencepriorisiertes, roh begrenztes AOT-Inventar;
- externe identitaetsgebundene RuntimeImages ohne Retailbytes im Kern;
- separate binder-only Sourceblocktabelle;
- zielgenaue FixedAddress-Probe und exakte Materialisierung ueber 128 Byte;
- Snapshot und SHA vor Modulmutation sowie exakte Autorisierung;
- terminale Fixed-Range-Loecher und kein Diagnosefallback;
- Runtime-/Port-ABI- und Cacheinvalidierung.

Eingecheckt bis `24d6132`, neuer Sonic-Produktproof offen:

- reale CFG-Kanten bleiben trotz Candidate-Carrier mit gleicher Callsite und
  gleichem Ziel erhalten;
- externe bedingte und normale Inventarnachfolger werden als bewachte
  Regionen verfolgt oder als trunciert markiert;
- Codepointerwert- und Objektadressprovenienz sind getrennt;
- Shapevalidierung besitzt typisierte Ergebnisse und ein globales
  Arbeitsbudget;
- der P1/P2-Chain-Cache revalidiert die zielbezogene Codegeneration;
- verschachtelte native Owner-Entries sichern den Exitstatus pro Aufruf;
- Gateerfolg verlangt Pflichtmeilenstein und vollstaendige angeforderte
  Post-Entry-Arbeit, der PowerShell-Wrapper propagiert den Child-Exitcode;
- Safepoint-Resume-Einstiege und lexikalisch sichere
  Registerersetzung sind vorhanden;
- Candidate-Carrier aus dem Summary-Fixpunkt entfernen und wiederholte
  Callargumente vereinigen;
- Multi-Owner-Shared-Tail-Ingress ohne erfundene CFG-Kante;
- parameterabhaengige Candidate-Returns werden in einem separat begrenzten
  kontextuellen Inventarwalk zurueckgefuehrt;
- Fallthrough und Call-Continuation ueber Owner-Domaingrenzen werden als
  eigene bewachte Inventarregionen verfolgt;
- faires Returned-/Stored-Inventar mit separatem Raw-Stored-Budget;
- vollstaendige Budgetmetadaten und Latent-AOT-Fail-Closed-Vertrag;
- exaktes Artefaktmanifest und exaktes `src/main.cpp`-Dateiset fuer
  Whole-Export-Hits und Miss-Reparatur;
- terminale Bring-up-Zusammenfassung fuer typisierte Fehler;
- ausfuehrender Gatewrapper-Vertrag fuer 0/1/3/124, Leerzeichenpfad,
  Budgetisolation, vorzeitiges Gateende und typisierte Fehler.
- Candidate-Call-Carrier nehmen nur am separaten Inventar-Rueckwaertsgraph
  teil; `requires_code_pointer`-Tails bleiben echte Inventarsenken;
- endliche, aber runtime-autoritative Stackframe-Deltas verbinden im
  Guarded-Inventory Spill/Reload, ohne eine feste CFG-Kante zu erzeugen;
- jeder nicht materialisierbare Guarded-AOT-Einstieg besitzt einen
  typisierten Grund und blockiert den Produkt-Export fail-closed.

Architektonisch weiter offen bleiben strukturierte Registeroperandemission,
die gemeinsame reale Lokalisierungs-/RAM-Batch-/Invalidierungskette,
DirectBoot-Paritaet, ein sichtbarer Spielscreen, 200 MHz und der
MSVC-/clang-cl-Produktvergleich.

## Historischer Produktnachweis und aktueller Proof-Status

`GameEntryHandoff` Schema 3 und Plattformzustandsvertrag 2 erfassen den
vollstaendigen Game-Entry-Zustand. Der reale NativeDisc-Capture entstand bei
Gastzyklus `415.233.270` und enthaelt 22 kanonische Geraete einschliesslich
Flash sowie fuenf ausstehende Ereignisse. Ein reales DirectBoot-Produkt hat
dieses `CompletePlatform`-Artefakt erfolgreich appliziert und anschliessend
Gastcode ausgefuehrt.

Der Apply-Pfad validiert alle gebundenen Payloads und Restoreplaene vor der
Mutation und prueft das Ergebnis durch semantischen Recapture. Das beschreibt
den historischen v24-Befund. Der aktuelle `KR-4967`-Quellvertrag bereitet alle
falliblen Subsystemzustaende vor Commitbeginn vor, committet atomar und
publiziert CPU-PC/PR zuletzt. Seine reale ABI-passende Abnahme nach KR-4974
bis KR-4984 sowie weitergehende normative Digests pro Subsystem stehen noch
aus.

Die explizit historischen v24-Vergleichslaeufe:

| Metrik | NativeDisc | DirectBoot-Artefakt |
|---|---:|---:|
| Entry-/Restore-Zyklus | 415.233.270 | 415.233.270 |
| finaler Gesamtzyklus | 600.000.000 | 600.000.000 |
| Hostzeit | 6,3161 s | 5,01505 s |
| gemeldete Rate | 94,9954 MHz | 119,64 MHz, nicht vergleichbar |
| tatsaechlich ausgefuehrte Post-Entry-Zyklen | nicht separat gemessen | 184.766.730 |
| vergleichbare DirectBoot-Rate | - | 36,8425 MHz |
| Zentraldispatches | 17.080.114 | 16.033.676 |
| Post-Entry-Zyklen pro Dispatch | - | 11,52 |
| letzter PC | `0x8C666D42` | `0x8C666D42` |
| GD-ROM-Kommandos | 72 | 72 |
| AICA-Audiopuffer | 180 | 179 |
| Frame | IP.BIN-Frame | 0 |
| `first_problem` | `none` | `none` |

Die gemeldeten 119,64 MHz des DirectBoot-Laufs sind kein gueltiger
Leistungsvergleich: Der Bericht teilt den finalen Gesamtzyklus durch die
Hostzeit, obwohl der Lauf erst bei Zyklus 415.233.270 restauriert wurde.
Tatsaechlich wurden nur 184.766.730 Zyklen ausgefuehrt. Die vergleichbare Rate
betraegt 36,8425 MHz und belegt keinen Performancegewinn. Das relative
Post-Entry-Budget und die Pflichtmeilensteinwertung sind im aktuellen
`KR-4966`-Quellvertrag implementiert; die reale ABI-passende Abnahme nach
KR-4974 bis KR-4984 steht noch aus.

Der historische v28-Funktionslauf wurde auf der Main-Basis
`8e5ab3145fb5fcafc056fd87025baf3497085342` mit dem neuen externen
`GameProjectArtifact` erzeugt und ueber den echten Produktinstaller mit der
privaten PAL-Disc installiert:

| Metrik | DirectBoot-v28 |
|---|---:|
| Entry-/Restore-Zyklus | 415.233.270 |
| CompletePlatform-Apply | 22 Geraete, 5 Events |
| Endzyklus am typisierten Fehler | 553.990.562 |
| Post-Entry-Zyklen | 138.757.292 |
| Fortschritt gegen v26 | +1.086.915 Zyklen |
| externe Walltime bis Fehler | 5,275792 s |
| Post-Entry-Rate bis Fehler | 26,3008 MHz |
| v26-Vergleich bis Fehler | 5,746371 s / 23,9578 MHz |
| warmer unveraenderter Hostbuild | 0,219272 s |
| vollstaendiger warmer Export | 4,209083 s |
| Exportcache | 42 Partitionshits, Analyse/IR + Metadaten Hit |
| MSVC-Gateexport | 1.946 Funktionen / 42 Partitionen |
| Produkt-EXE | 52.446.208 Bytes |
| Produkt-EXE SHA-256 | `bdb20c5e8738cf4e5a2a21ed6f667384d44f87e3411506da27c0487f0f2cd7d8` |
| v26-Produkt-EXE | 52.406.784 Bytes |
| Zentraldispatches | 10.079.932 |
| Dispatches gegen v26 | +123.498 |
| G2-Kanaele | alle inaktiv |
| GD-ROM-Kommandos | 72 |
| AICA-Audiopuffer | 165 |
| PVR-Gast-/Direct-Frames | 2 / 2 |
| veraenderte Direct-Pixel | 302.287 |
| Hostframe / sichtbarer Screen | 0 / keiner |
| terminales Dispatchlabel | `aot-template-mismatch` |
| interner Materializergrund | `AotTemplateMismatch` (14) |
| Callsite / Ziel | `0x8C11088C` / `0x8C64784E` |

Die Walltime endet vor dem vorgesehenen Budget und ist kein
600-Millionen-Performancebenchmark. Aus der tatsaechlichen Post-Entry-Arbeit
folgen `26,3008 MHz` gegen `23,9578 MHz` bei v26, also provisorisch
`+9,78 %` bei identischem Restore, aber keine Gateabnahme. Sechzehn reale
Fensteraufnahmen blieben schwarz.

### Historischer v30-KR-4972-Lauf

Die generische Analyse gewinnt den Callback `0x8C64784E` jetzt ueber einen
begrenzten Candidate-Tail-Jump und einen bewiesenen Runtime-Stackframe
zurueck. `0x8C6478C2` ist darin als gemeinsamer Body erreichbar. Der
vollstaendige Export mit dem externen Spielprojekt uebernimmt diesen Seed
noch nicht in produktive CFG, Source-Map und AOT.

Der frische v30-MSVC-Port wurde aus der oben genannten Main-Basis exportiert,
mit der privaten Originaldisc installiert und real ausgefuehrt:

| Metrik | DirectBoot-v30 |
|---|---:|
| Entry-/Restore-Zyklus | 415.233.270 |
| CompletePlatform-Apply | 22 Geraete, 5 Events |
| Endzyklus am typisierten Fehler | 553.990.562 |
| Post-Entry-Zyklen | 138.757.292 |
| Zentraldispatches | 10.079.932 |
| retired Gastinstruktionen | 92.554.138 |
| GD-ROM-Kommandos | 72 |
| AICA-Audiopuffer | 165 |
| PVR-Gast-/Direct-Frames | 2 / 2 |
| veraenderte Direct-Pixel | 302.287 |
| Hostframe / sichtbarer Screen | 0 / keiner |
| Sichtlauf | 15 Aufnahmen, alle schwarz; `sega_seen=false` |
| terminales Dispatchlabel | `aot-template-mismatch` |
| Callsite / Ziel | `0x8C11088C` / `0x8C64784E` |
| MSVC-Gateexport | 1.959 Funktionen / 42 Partitionen |
| Produkt-EXE | 52.616.192 Bytes |
| Produkt-EXE SHA-256 | `801f69727d1df3166b4ff29710856e327450f622e61fe2fd2fec76cc3a39d77e` |

Der Lauf endet vor dem 600-Millionen-Budget und liefert keinen neuen
Performancewert. Sein Funktions-, Dispatch-, Geraete- und Sichtresultat ist
gegen v28 unveraendert. Der v30-Export war kalt und darf nicht als
Warmbuildvergleich gegen v28 gewertet werden.

### Historischer NativeDisc-v32-KR-4973-Lauf

Der generische Flag-Poll-Fastpath ist unter aktiver MMU wieder fail-closed.
Ein VBlank-Scanout besitzt ausserdem eine von Diagnoseproofs unabhaengige,
auf einen Frame begrenzte Hostqueue. Die externe Spielprojektmetadatei kann
jetzt sowohl an `port <gdi>` als auch an `port-executable` gebunden werden.
Dieser historische Quellvertrag verwendete Runtime-ABI 64.

Der frische v32-MSVC-NativeDisc-Port wurde mit derselben privaten
hashgebundenen Spielprojektdatei wie DirectBoot exportiert, mit der
Originaldisc installiert und real sichtbar ausgefuehrt:

| Metrik | NativeDisc-v32 |
|---|---:|
| Gastzyklus am typisierten Fehler | 553.990.562 |
| externe Produktzeit | 6,701 s |
| Rate bis Fehler | 82,67 MHz |
| Zentraldispatches | 11.080.283 |
| GD-ROM-Kommandos | 72 |
| AICA-Audiopuffer | 166 |
| PVR Gast-/Direct-/Softwareframes | 2 / 2 / 1 |
| Hostframes | 127 |
| hoechster sichtbarer Screen | Sega-Lizenzscreen ab 2,032 s |
| Callsite / Ziel | `0x8C11088C` / `0x8C64784E` |
| MSVC-Gateexport | 2.051 Funktionen / 46 Partitionen |
| Produkt-EXE | 53.677.056 Bytes |
| Produkt-EXE SHA-256 | `888028348cc6caa5510c2cf4dfa5ce5055d63fa8e8927b86c3815f5a75f520bf` |
| unveraenderter Ninja-Warmbuild | 0,203137 s |

NativeDisc-v32 und DirectBoot-v30 enden damit exakt am selben Zyklus, an
derselben Callsite und am selben Ziel. Das Bild unterscheidet sich klar:
NativeDisc zeigt in allen drei 640x480-Aufnahmen Sega; DirectBoot-v30 blieb
in 15 Aufnahmen schwarz und praesentierte null Frames. Der private
Sega-Klassifikator meldet wegen des grauen PAL-Hintergrunds irrtuemlich
`sega_seen=false`; die Aufnahmen besitzen stabil
`blue_ratio=0,0609375` und `non_black_ratio=0,9830208`.

Der v32-Lauf endete vor dem 600-Millionen-Budget am damaligen
KR-4972-Blocker und ist kein Performancegate. Er schliesst KR-4973 als
historischen sichtbaren Bootfortschritt ab. Der aktuelle
`KR-4972`-AOT-Vertrag ist vorhanden; der anschliessende v24-Iterationslauf
auf Basis des Checkpoints `18f8537` wurde jedoch nach rund 3 h 27 min ohne
Portartefakt abgebrochen. Erst der nach KR-4974 bis KR-4984 zulaessige
ABI-passende Lauf kann zeigen, ob dieser Blocker passiert wird.

## Abgeschlossene Sound-/AOT-Blocker und historische Produktgrenzen

Der fruehere produktive Waitvertrag war:

```text
Objekt:               0x8C8D3908
Completion-Flag:      0x8C8D3920
Poll-Callback:        0x8C666D42
Completion-Writer:    0x8C65A458
```

Der Waitpfad setzt `[object+24]` zunaechst auf `0` und prueft danach
fortlaufend auf `1`. Der Poll-Callback besteht nur aus `RTS; NOP`. Der
erwartete Completion-Pfad schreibt `1` nach `[object+24]`. Alle sechs
statisch aufgeloesten Caller dieses Waitvertrags liegen in
ADXT-/mwSnd-Soundpfaden.

Die erste verlorene Kante bestand aus zwei allgemeinen Holly-G2-Fehlern:

```text
SB_G2APRO 0x4659404F
  -> Start- und Endbyte waren vertauscht
  -> Haupt-RAM wurde faelschlich als Overrun abgewiesen

ADTSEL 5 + ADST 1
  -> CPU-initiierter Transfer mit externem AICA-Request-Level
  -> SB_FFST.bit0=0 wurde beim Armieren nicht als request-ready ausgewertet
```

Beide Ursachen sind generisch repariert. v26 und v28 beenden G2-Kanal 0 und
verlassen `0x8C666D42`, ohne einen Hostpatch am Completion-Flag oder eine
Titeladresse im Kern. Der Writer `0x8C65A458` beziehungsweise der konkrete
Flagwechsel wurde nicht separat instrumentiert. KR-4965 ist gemaess seiner
Alternativabnahme abgeschlossen, weil ein neuer, engerer allgemeiner
Blocker belegt ist.

KR-4971 ist durch v28 abgeschlossen. Das private externe, an die vollstaendige
Boot- und Contentidentitaet gebundene Spielprojektartefakt seedet die exakt
beobachtete Grenze `0x8C010F22 + 0x18` in Analyzer, CFG, IR und AOT. Der
Produktlauf passiert dieses Ziel. Die generischen Katana-Quellen enthalten
keine Sonic-Adresse.

Die historische v28-/v30-/v32-Produktgrenze war KR-4972:

```text
indirekter Call:       0x8C11088C
statisches Spielziel:  0x8C64784E
Dispatchlabel:         aot-template-mismatch
Materializergrund:     AotTemplateMismatch (14)
```

Fuer das unveraenderte Ziel im initialen Boot-Executable fehlte im
historisch exportierten Port ein generierter Block beziehungsweise
passendes Runtime-AOT-Template. Die generische Analyse beweist den
Callback-/Shared-Tail-Pfad inzwischen aus konkreter Codepointer-Provenienz
und erreicht den gemeinsamen Body `0x8C6478C2`. Der aktuelle Source-Vertrag
erhaelt bewachte AOT-Einstiege bis in CFG, Source-Map und AOT, behandelt
Shared Bodies explizit und erzwingt die Exportvollstaendigkeit. Erst der nach
KR-4974 bis KR-4984 zulaessige ABI-passende Produktlauf kann belegen, ob die
historische Grenze damit passiert wird. Interpreter, JIT, Runtime-Decoder,
Emulationsfallback und eine geratene Sonic-Grenze sind keine zulaessige
Reparatur.

Die zeitlich letzte reale Produktgrenze stammt dagegen aus
NativeDisc-v33/ABI 73:

```text
finaler Gesamtzyklus: 487.233.787
Post-Entry-Zyklen:    72.000.517
Callsite / Ziel:      0x8C65EA06 / 0x8C0101F2
Fehler:               missing-aot / guarded-fallback
```

Ob diese scheinbar fruehere AOT-Luecke durch den breiteren aktuellen
Inventarvertrag geschlossen wird, ist ebenfalls erst im nach KR-4974 bis
KR-4984 zulaessigen ABI-passenden Produktport belegbar.

## GameEntryHandoff-Stand

Historisch belegt:

- `GameEntryHandoff` Schema 3
- Artefaktformat 2
- Plattformzustandsvertrag 2
- Runtime-ABI 63 und Portprojektvertrag 53 im damaligen Capture/Port
- Bindung an Contentidentitaet, Bootdatei, Konsolenprofil, Runtime-ABI und Descriptor
- CPU/MMU, RAM-Deltas, Scheduler sowie 22 Geraeteklassen einschliesslich Flash
- fuenf typisierte Ereignisse im realen NativeDisc-Capture
- vollstaendiger Capture und realer `CompletePlatform`-Apply im Direct-Produkt
- vollstaendige Vorvalidierung und semantischer Recapture nach Apply

Fuer den aktuellen Source-Checkpoint `18f8537` mit Runtime-ABI 85 offen:

- deterministischer Doppel-Capture
- Artefakt-Inspect-/Verify-CLI
- normative NativeDisc-/DirectBoot-Paritaet
- realer ABI-passender Nachweis des implementierten atomaren
  Prepare-/Commitpfads nach KR-4974 bis KR-4984
- per Subsystem normativ vergleichbare Digests

## GameProjectArtifact-Stand

`GameProjectArtifact` Format 4 ist ein besitzendes, versioniertes
Binaerartefakt fuer deklarative externe Spielprojektdaten. `write()` und
`load()` binden sowohl die Payload als auch das gesamte Artefakt ueber
SHA-256. Serialisiert werden Identitaet, exakte Funktionsgrenzen,
Jump-/Callbacktabellen, Runtime-AOT-Templates, Symbole, Codeidentitaeten und
optionale Direct-Boot-Konfiguration. Prozesslokale native Callback- und
Hookzeiger sowie der private `GameEntryHandoff`-Provider werden fail-closed
nicht serialisiert.

`port-executable --game-project` kann mit `--game-entry-handoff` kombiniert
werden. Die vollstaendige Definition steuert Analyse, CFG, IR, AOT und
Exportmetadaten. Wenn keine nativen Hooks eine externe Registrierung
erfordern, bindet der erzeugte Produktport zur Laufzeit nur die reduzierte
Identitaets-, Boot- und Handoffdefinition; reine Analysemetadaten werden nicht
nochmals in den Runtime-Hotpath getragen.

Der private v27-/v28-Befund:

```text
Artefaktidentitaet:
  sha256:9d4edff0270275f0b4931b733b2bd03ef330893f79d4728d6580adaf1107249f
exakte beobachtete Grenze:
  0x8C010F22 + 0x18
Titeladressen im generischen Kern:
  0
```

`GameProjectFunctionBoundary::size` erreicht AnalysisOverride/-Seed,
Funktionskandidaten, CFG, IR und AOT. Der aktuelle Spielprojektvertrag ist 5,
das Artefaktformat 4 und der Analyzer-ABI 23; die darunter beschriebenen
v27-/v28-Artefakte bleiben historische Format-1-Evidenz.

## Maple-/VMU-Stand

Maple-/VMU-Zustand, MMIO, DMA und Ereignisrehydrierung sind in
`CompletePlatform` eingebunden. Die Save-Migration war im damaligen realen
Lauf byteidentisch. Das beweist noch kein allgemeines, rollbackfreies
Produktprofil. Der aktuelle `KR-4970`-Source-Vertrag behandelt installierte
VMU-/Flash-Working-Copies als autoritativ und trennt Product-Handoff von
verlustfreier Diagnose. `2f2d3b4` ergaenzt die echte Maple-
Subunitadressierung, den FT1-Speichervertrag, serialisierte Schreibphasen
und ein formatiertes quellloses Standardmedium. Sein realer ABI-78-Nachweis
steht noch aus.

## PVR-/Frame-Stand

Der alte NativeDiscBoot konnte ueber IP.BIN einen sichtbaren Direct-FB-Frame erzeugen. DirectBoot ueberspringt IP.BIN und soll deshalb nicht auf den Sega-Screen geprueft werden.

IP.BIN hinterlaesst jedoch relevante:

- PVR-/SPG-Register
- Framebufferbasis
- VRAM-Inhalt
- ASIC-/IRQ-Masken
- Schedulerereignisse

Der aktuelle DirectBoot appliziert den erfassten PVR-/SPG-/ASIC-Zustand.
Der historische v28-Lauf erhaelt nach dem Game Entry zwei gastbelegte Direct-Frames mit
`302.287` veraenderten Pixeln. Der Host-Presenter meldet weiterhin null
Frames, und alle 16 Fensteraufnahmen bleiben schwarz. Der technische
Framebufferfortschritt beweist daher noch keine normative Frameparitaet und
keinen sichtbaren Spielboot.

KR-4973 hat die zugrunde liegende Runtimekopplung inzwischen allgemein
entfernt: Ein aktueller Scanout kann auch ohne neuen Proof praesentiert
werden. Der sichtbare historische NativeDisc-v32-Lauf belegt diesen ABI-64-Pfad mit 127
Hostframes. DirectBoot-v30 enthaelt noch den alten ABI-63-Code und ist daher
kein Gegenbeweis fuer die aktuellen Source-Vertraege. Ein neuer DirectBoot-
Nachweis braucht einen passend gebundenen Handoff und einen eigenen realen
Sichtlauf. Ein Sega-Screen bleibt dort wegen
uebersprungener IP.BIN ausdruecklich ungueltig als Pflichtmeilenstein.

Der naechste visuelle DirectBoot-Meilenstein ist:

```text
FirstGameFramebufferWrite
oder
FirstTaFrame
danach FirstVisibleGameFrame
```

## Scheduler-/Gate-Stand

Das historische Gate verwendete 600 Millionen als absoluten finalen
Schedulerzyklus. Nach Restore bei `415.233.270` blieben dem DirectBoot deshalb
nur `184.766.730` Post-Entry-Zyklen. NativeDisc und DirectBoot erhielten damit
nicht dieselbe Gastarbeit.

Der historische v28-Lauf erreicht selbst dieses falsche absolute Maximum
nicht: Der typisierte
AOT-Coveragefehler beendet den Lauf bei `553.990.562`, also nach
`138.757.292` Post-Entry-Zyklen. Der damalige Wrapper konnte trotz
unvollstaendigem Budget Exitcode 0 liefern.

Der aktuelle `KR-4966`-Quellvertrag verwendet eine Laufdauer ab Entry:

```text
target_cycle = restored_game_entry_cycle + requested_elapsed_guest_cycles
```

Das externe Spielprojekt gibt einen erforderlichen Meilenstein an. Bei
angefordertem Produktbudget ist Exitcode 0 nur zulaessig, wenn Meilenstein,
vollstaendige Post-Entry-Arbeit und `KATANA_PRODUCT_GATE` gemeinsam erfuellt
sind. Der ABI-passende Produktnachweis nach KR-4974 bis KR-4984 steht noch
aus.

Empfohlene Exitcodes:

```text
0 = Meilenstein erreicht
3 = Gastzyklusbudget erreicht, Meilenstein verfehlt
1 = typisierter Fehler
```

## Performance-Stand

Der DirectBoot-Bericht nennt 119,64 MHz, diese Zahl verwendet jedoch den
restaurierten Gesamtzyklus als geleistete Arbeit. Fuer die tatsaechlich
ausgefuehrten `184.766.730` Post-Entry-Zyklen gelten:

```text
36,8425 MHz
16.033.676 Zentraldispatches
11,52 Gastzyklen pro Zentraldispatch
```

Der historische Handoff belegt damit keinen Performancegewinn.

Der historische v28-Lauf fuehrt bis zum funktionalen Fehler `10.079.932`
Zentraldispatches aus und
damit `123.498` mehr als v26, bei `1.086.915` zusaetzlichen Gastzyklen. Die
extern gemessenen `5,275792 s` ergeben `26,3008 MHz` Post-Entry-Rate bis zum
Fehler gegen `5,746371 s` und `23,9578 MHz` bei v26, also provisorisch
`+9,78 %`. Dieser identisch restaurierte Fehler-zu-Fehler-Vergleich zeigt
eine kleine Laufzeitverbesserung,
ersetzt aber weder die v24-Baseline noch den relativen 600-Millionen-
Performancebenchmark.

Der ABI-77-v37-Lauf ist nun der erste korrekte relative
600-Millionen-Produktbenchmark:

```text
600.000.000 Post-Entry-Gastzyklen
20,2117 s Hostzeit
29,6858 MHz
66.212.631 Zentraldispatches
9,06 Gastzyklen pro Zentraldispatch
```

Die funktionale Abdeckung reicht damit erstmals bis zu einem sichtbaren
Sonic-Spielhinweis. Performance bleibt dennoch ein harter Produktblocker:
Funktion-/Return-/Partition-Escapes erzeugen weiterhin mehr als 66
Millionen zentrale Dispatches.

Das Ziel ist:

```text
mindestens 200 MHz effektiv
mindestens 250 MHz unpaced als Reserve
```

Bereits vorhanden:

- statisches und dynamisches AOT-Tier
- direkt gebundene validierte Ausfuehrungs- und Fastpathdeskriptoren
- P1-/P2-Inline-Cache
- native interne Labels und direkte Owner-Entries
- direkte native Calls sowie endliche indirekte Zielausfuehrung
- IR-Liveness-basierte Auswahl ausgewaehlter GPRs sowie
  T/PR/GBR/MACH/MACL/FPUL mit Release/Reload an Architekturgrenzen; die
  Registerausdruecke werden noch durch eine lexikalisch begrenzte
  C++-Transformation umgeschrieben
- blockweise direkte Haupt-RAM-Schreibbatches
- `KATANA_STATIC_AOT_ESCAPE_STATS`

Offene Hauptpunkte:

- Function-AOT ist weiterhin stark an Single-Block-/Chainingvertraege gebunden
- direkte Calls committen zu oft pauschal Blockzeit
- strukturierte Operandemission statt nachtraeglicher C++-Umschreibung
- FPU-Registerarrays bleiben bewusst ausserhalb der aktuellen Lokalisierung
- Produktfastpaths verwenden `dynamic_cast`
- die Zentraldispatches muessen nach korrektem Post-Entry-Gate stark reduziert werden

Gastzyklen und Geraetelatenzen duerfen nicht kuenstlich reduziert werden.

## Historischer Build- und Workspace-Stand

```text
v28 warmer MSVC-Gateexport:       4,209083 s
  Analyse-/IR-Cache:              Hit
  Metadatencache:                 Hit
  AOT-Partitionscache:            42 / 42 Hits
v28 unveraenderter Hostbuild:     0,219272 s
v28 Produkt-EXE:             52.446.208 Bytes
v26 Produkt-EXE:             52.406.784 Bytes
historischer frischer Export:    169,3 s
v33 kalter ABI-73-Export:        711,2 s
v33 unveraenderter Hostbuild:    0,200236 s
v33 identischer Voll-Warmexport: Cachemiss, nach 124 s beendet
v37 kalter Export plus Build:    381 s
v37 Funktionen / Partitionen:    2.902 / 69
v37 Produkt-EXE:                 156.044.800 Bytes
```

Beim konservativen Cleanup wurden `16.467.100.969` Bytes eindeutig
regenerierbarer Build-, Publish- und Testartefakte entfernt. Retailquellen,
aktuelle Referenzen und Nutzerdaten blieben erhalten.

Nach dem nutzbaren v27-Nachfolger wurden ausserdem v26 samt Workdir, die alten
v22-/v24-Referenzports und ihre zwei zugehoerigen Workdirs entfernt. Die sechs
Targets gaben exakt `10.166.434.310` Bytes frei. v27, sein
`.katana-port-work-b9a041bd0a2a`, Bootartefakt, Handoff, private GDI und der
installierte Disc-Cache bleiben erhalten.

Nach der finalen v28-Abnahme wurden der ersetzte v27-Port und
`.katana-port-work-b9a041bd0a2a` entfernt. Das gab weitere
`3.249.852.517` Bytes frei. Zu diesem historischen Zeitpunkt blieben nur v28,
`.katana-port-work-e0e2126c4352`, Bootartefakt, Handoff, private GDI und
installierter Disc-Cache erhalten; die Entfernung ist nicht rueckgaengig.

Nach dem erfolgreichen NativeDisc-v37-Gate wurden der ersetzte v33-Port,
zwei gescheiterte Exportworkspaces, SDK v27 bis v36, alte
Spielprojekt-Builds und alte beziehungsweise doppelte private Artefakte
entfernt. Das gab `11.207.660.888` weitere Bytes frei. Erhalten und
verifiziert sind v37-Port samt installiertem Disc-Cache, v37-SDK, aktueller
Exportworkspace, v37-Spielprojektartefakt, kanonischer
64-KiB-Runtimecapture und private Originaldisc. Die entfernten generierbaren
Artefakte sind nur durch Neuaufbau beziehungsweise Neuinstallation
wiederherstellbar.

Offen bleiben:

- Runtime-only-Rebuild plus Relink
- Hook-only-Build im externen Spielprojekt
- kalter Gesamtbuild des ABI-77-Stands weiter unter 381 s reduzieren
- erfolgreicher manifestgebundener Whole-Export-Warmhit
- schmalerer AOT-ABI-Header
- weniger generische Produkt-/Testlogik in der erzeugten `main.cpp`

## Aktiver kritischer Pfad

```text
KR-4974 Telemetrie und Miss-Reason-Ledger
  -> KR-4975 semantische Cachelinsen
  -> KR-4976 persistente Programm-/SCC-Session
  -> KR-4977 gemeinsamer Multi-Root-Inventory-Fixpunkt
  -> KR-4978 inkrementeller CFG-/Seed-Fixpunkt
  -> KR-4979 priorisierter Executor und RAM-Grenzen
  -> KR-4980 persistente Buildshards
  -> KR-4981 8-/12-/24-Thread-Kaltbuildgate
  -> KR-4982 GPU-Entscheidungsgate
     -> KR-4983 nur bei positivem GPU-Beweis
  -> KR-4984 unabhaengige Gesamtpruefung, P0/P1-Schliessung und Re-Review
  -> genau ein frischer privater NativeDisc-Sonic-Lauf
```

Vor KR-4984 wird kein weiterer privater Sonic-Port gebaut. Jeder P0-Task
wird allgemein ueber den gesamten betroffenen Strang umgesetzt, fokussiert
verifiziert, einzeln committed und gepusht. Die alten KR-496x-Aufgaben
bleiben als nachgelagerte Produktarchitekturarbeit erhalten, steuern aber
nicht den naechsten Lauf.

## Historische und gesperrte reale Produktlaeufe

Die Laeufe A bis A4 sind historische Evidenz. Der naechste reale Lauf ist
bis zum Abschluss von KR-4984 gesperrt und ersetzt keinen der P0-
Performance- oder Reviewnachweise.

### Lauf A - nach KR-4965 [ausgefuehrt]

Der v26-DirectBoot wurde mit dem real installierten PAL-Disc-Cache und
CompletePlatform-Handoff ausgefuehrt.

Ergebnis:

- G2-Kanal 0 abgeschlossen und alter Sound-Poll verlassen
- engerer RuntimeOnly-AOT-Blocker bei `0x8C010F22`
- zwei technische Direct-Frames, aber kein sichtbarer Hostframe

### Lauf A2 - nach KR-4971 [ausgefuehrt]

- privates hashgebundenes `GameProjectArtifact` gemeinsam mit Handoff
- altes statisches Ziel passiert
- `+1.086.915` Gastzyklen gegen v26
- neuer typisierter Produktblocker aus KR-4972
- 16 reale Fensteraufnahmen schwarz

### Historischer Lauf A3 - KR-4972-Analyserunde [ausgefuehrt]

- derselbe DirectBoot-Produktpfad und dieselbe private Discinstallation
- generischer Analyzer erkennt `0x8C64784E` und den Body `0x8C6478C2`
- vollstaendiger Produktexport enthaelt den Seed noch nicht
- identischer typisierter Fehler bei `553.990.562` Gastzyklen
- 15 reale Fensteraufnahmen schwarz

### Lauf A4 - ABI-77-NativeDisc-v37 [ausgefuehrt]

- Finalreview ohne P0/P1/P2 und Source `d645d20`
- genau ein frischer MSVC-NativeDisc-Port
- private PAL-GDI ueber den Produktinstaller installiert
- exakt `600.000.000` Post-Entry-Gastzyklen ausgefuehrt
- `20,2117 s`, `29,6858 MHz`, `66.212.631` Zentraldispatches
- Sega-Lizenzscreen der PAL-Disc und danach sichtbarer
  Sonic-Speicherkartenhinweis
- alle bekannten AOT-/PVR-Grenzen passiert, `first_problem=none`
- `56` native und `0` Interpreter-Materialisierungen

### Lauf B - nach KR-4966 und KR-4967

- NativeDisc Capture am Game-Entry
- DirectBoot Apply bis vor ersten Spielblock
- gleiche Post-Entry-Gastarbeit ausfuehren
- CPU-, Memory-, PVR-, Sound/DMA-, Maple-, IRQ- und Scheduler-Digests normativ vergleichen

### Lauf C - nach Game-Entry-Paritaet

- 600 Millionen Gastzyklen ab Entry
- mindestens `FirstGameFramebufferWrite` oder `FirstTaFrame`
- Exitcode 3 bei verfehltem Meilenstein

### Lauf D - nach Performanceblock

- derselbe Produktpfad und dieselbe Gastarbeit
- mindestens 200 MHz
- sichtbarer Meilenstein bleibt erhalten

Zwischen diesen Laeufen sind keine Vollsuiten vorgesehen. Ein kleiner bestehender Vertragstest darf nur angepasst werden, wenn eine schwer sichtbare Datenkorruption sonst nicht abgesichert werden kann.

## Nicht behauptet

Der aktuelle Stand behauptet nicht:

- dass der abgebrochene v24-Export ein Portartefakt erzeugt hat
- dass aus v24 ein Sonic-Prozess gestartet oder ein Screenshot aufgenommen
  wurde
- dass der aktuelle Kaltbuild produktiv nutzbar oder das 8-Minuten-Ziel
  erreicht ist
- dass Runtime-CPU-Last, D3D11-Presenter oder Analyse-GPU-Offload bereits
  end-to-end gemessen und abgenommen sind
- dass die Teilreviews eine unabhaengige Gesamtpruefung des aktuellen
  Arbeitsbaums ersetzen
- dass Sonic Adventure bereits ueber den Speicherkartenhinweis hinaus bootet
- dass NativeDisc und DirectBoot normativ paritaetisch sind
- dass der Sega-Screen im DirectBoot erwartet wird
- dass Maple der aktuelle Soundblocker ist
- dass AICA allein die Ursache ist
- dass der Completion-Writer oder der Flagwechsel direkt beobachtet wurde
- dass eine VMU oder ein Controller im aktuellen Produktlauf erkannt wird
- dass die gemeldeten 119,64 MHz einen Performancegewinn darstellen
- dass die provisorischen 26,3008 MHz bis zum v28-Fehler eine
  600-Millionen-Performanceabnahme darstellen
- dass 36,8425 MHz spielbar oder echtzeitfaehig sind
- dass ein schwarzer Lauf mit Exitcode 0 ein erfolgreiches Produktgate ist
- dass eine einmal byteidentische Save-Migration ein allgemeines
  No-Rollback-Profil beweist

## Aktuelle Source-Vertraege

Diese Werte stammen aus Source-Checkpoint `18f8537`. Sie sind Quellstand,
kein Produktnachweis.

```text
Runtime-ABI:                    85
Block-ABI:                       5
Analyzer-ABI:                   23
PlatformServices-ABI:           13
Backend-Interface-ABI:          12
Portprojektvertrag:             75
GameEntryHandoff-Schema:         3
GameEntryHandoff-Artefakt:       2
GameEntry-Plattformzustand:       2
Spielprojektvertrag:             5
GameProject-Artefaktformat:       4
Gastzyklusvertrag:                2
Native-AOT-Emissionsprofil:     13
AOT-Partitionsschema:            5
Crash-Capsule-Vertrag:           1
Systemreplay-Schema:              8
Runtime-Probe-Schema:             5
Runtime-Probe-Device-Schema:      6
```

Historische Detailverlaeufe bleiben ueber Git-Historie, Changelog und Task-ID-Registry erhalten. Dieses Dokument ist ab jetzt die kompakte Wahrheit fuer den aktuellen `v0.49`-Produktbring-up.
