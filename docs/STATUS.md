# Projektstatus

Aktuelle interne Version: `v0.49.1`

`v0.50.0` bleibt die erste Alpha und wird erst freigegeben, wenn der rein
native Sonic-Port ohne ARM7, CPU-PVR oder andere emulatoraehnliche
Produktzustaende das Hauptmenue erreicht.

## Repositoryweiter Arbeitsvertrag

Fuer jeden Task gilt projektweit:

```text
Task implementieren
  -> alle betroffenen Pfade reviewen
     und bestaetigte Fehler innerhalb dieses Reviews schliessen
  -> den reviewten Task direkt auf main committen und pushen
  -> naechster Task
```

Es gibt keine separate standardmaessige Test-, Verifikations-, Fix- oder
Integrationsphase. Neue Unit-Tests, Regressionstests, Fixtures,
Stresslaeufe, Testprojekte oder Testmatrizen werden nicht gebaut und nicht
als Abschlussbedingung gefordert.

Der reale Sonic-Adventure-PAL-Port ist der massgebliche Produkt- und
Integrationstest. Reviews duerfen fehlende neue Tests nicht als Finding
melden. Vorhandene Tests werden nur geprueft oder repariert, wenn sie selbst
konkret gebrochen, widerspruechlich oder zahlenmaessig falsch sind.

## Aktueller Bring-up-Stand

Der aktive P0 ist ab `v0.49.1` der native Sonic-Port und nicht die
Beschleunigung der historischen Dreamcast-Geraetemodelle. Verbindlich sind
statisches SH-4-AOT sowie native PC-GPU-, Audio-/Movie-, Datei-, Eingabe- und
Savepfade. ARM7-Interpreter und CPU-PVR sind aus dem Produktprofil zu
entfernen; fehlende Hooks enden fail-closed statt auf Emulation
zurueckzufallen.

Aktive Reihenfolge: `KR-5000` Produktlinkgrenze, `KR-5001` Hookkarte,
`KR-5002` nativer Audio-/Moviepfad, `KR-5003` nativer GPU-Pfad, `KR-5004`
native Plattformdienste und `KR-5005` No-Skip-Lauf bis Hauptmenue. Der
vollstaendige Vertrag steht in `NATIVE_PORT_PRODUCT_CONTRACT.md`.

Aktueller KR-5005-Produktstand: Der v111/v30-Lauf hat die vollstaendige
zweiteilige Introsequenz erstmals innerhalb des echten nativen Spielablaufs
ohne Eingabe oder Skip abgeschlossen. Sonic Team lieferte `200/200` Video,
`294.016` Audioframes und `200` nichtschwarze Frames; das Opening lieferte
`3.257/3.257` Video, `4.709.760` Audioframes und `3.254` nichtschwarze
Frames. Der Sequenzabschluss gab den originalen Erfolgsstatus an den
rekompilierten Titelcode zurueck. Der naechste typisierte Endpunkt ist ein
noch nicht gebundener statischer Entry eines nach dem Intro materialisierten
PRS-Overlays. Ab jetzt darf nur in Diagnoselaeufen ein echter
Start-Controllerimpuls das bereits vollstaendig verifizierte Intro skippen;
Memory-Card-Screen und Hauptmenue bleiben unbewiesen.

Der aktuelle Export umfasst `5.918` Funktionen, `167` Partitionen, `4.430`
rohe und `2.509` guarded Callback-Kandidaten. Gegenueber v29 sind das `+815`
Funktionen und `+18` Partitionen. Die Closure steht bei `759` Gaps statt
`803`; `865` Sites sind durch Hooks ersetzt, `20` durch native CPU-Control-
Semantik und `53` als Progress-Waits gebunden. Analysebudgets und Inventare
blieben vollstaendig.

Der aktuelle PRS-Entry-Table-Zwischenstand erkennt ausschliesslich
identitaetsgebundene, nullterminierte Prefix-Tabellen mit `3..64` eindeutigen
direkten Main-RAM-Entries und validiert Runtimeextent, Decode, fruehen
Kontrollfluss, vollstaendige CFG und Relocation vor der RuntimeOnly-Zulassung.
Die abgeleitete Evidenz ist von privaten exakten Hints getrennt; Stop-on-miss
bleibt verpflichtend. Zwei Kandidaten (`9`/`3` Entries) wechselten von
inventory-truncated zu zugelassen, vier Stack-/Inventory-Kandidaten bleiben
offen. Der gruen beendete Voll-Export (`294,9 s`) ergibt `36/36`
Module/Quellen, `6.171` Blockidentitaeten, `200` Funktionsidentitaeten,
`3.406` externe Pointer, `440` Transfers, `168` Partitionen und `5.773`
Funktionen. `1.423` bekannte Hardware-Sites und `1.425` Gaps (`1.373`
hook-missing, `51` progress-wait, `1` root-ownership) sind erweiterte
Closure-Sichtbarkeit, keine Regression. Der Release-Build mit `24` Jobs
endete in `51,4 s`; der 100-s-Produktsmoke erreichte Film 0 `200/200`, aber
noch keine dynamische Overlay-Bestaetigung.

Der Windows-Native-Port unterstuetzt nun XInput und WinMM-DualSense,
DualShock sowie generisches HID ueber Plattformvertrag `2`. Physische Sony-
Controller werden bei neuer Belegung priorisiert; Slots bleiben ueber Hotplug
stabil, Achsen sind backenduebergreifend konsistent und XInput-Vibration
bleibt dem tatsaechlichen XInput-Geraet zugeordnet. Der Runtime-/CLI-Build ist
sauber. Der private Export endete nach `567,512 s` mit Exit `0`, `5.774`
Funktionen und `168` Partitionen; die Closure bleibt bei `1.423` Sites,
`1.425` Gaps und `865` ersetzten Sites. Der physische DualSense-Menutest
steht ohne anwesenden Nutzer noch aus.

`KR-5000` ist als physische Source-, Link- und Installgrenze abgeschlossen.
Das installierte Produkt-SDK exportiert nur `KatanaRecomp::aot_runtime`,
`KatanaRecomp::native_port_runtime` und die explizite native
Produktheader-Allowlist; der historische Dreamcast-Gerätepfad ist
nur ein internes, nicht installierbares Diagnoseorakel und kein Exportprofil.
Profilvertrag `14`, Portprojektvertrag `91` und der Post-Link-Audit sperren
ARM7/SkyEmu, AICA, PVR/TA, ASIC, GD-ROM, Maple und Interpreterbestandteile.

`KR-5000` ist abgeschlossen: NativePortDefinition, NativePortArtifact,
NativePortContent, NativePortRuntime und Bootstrap sowie read-only
Content-Mappings, Hook-/Hardware-Closure, direkter nativer Dispatch und
Linkaudit sind implementiert. Ein privater Adapter wird erreicht,
statisch rekompilierter Spielcode startet, und der erste unaufgeloeste
Plattformzugriff endet typisiert als `UnresolvedHardwareAccess` ohne
Emulator-/Interpreter-/Runtimefallback. Der generierte Runner verlangt
Executable plus privaten ContentRoot und validiert beide Pfade; der
Bring-up-Schalter gilt nur bei unvollstaendiger Closure.

`KR-5001` ist source-seitig abgeschlossen: Die deterministische
`metadata/native-hook-requirements.json`-Karte und Hardware-Closure Schema
`v2` verlangen exakte Function-/Instruction-Replacement-Proofs. Bekannte
Hardware- und unbekannte Instruktionsstellen bleiben hookpflichtig;
range-gepruefte Native-Memory-Zugriffe enden ausserhalb typisiert, und
`MemoryAccessError`/Native-Dispatch tragen `GuestInstructionOrigin` auch ohne
Tracesink. `KR-5002` ist source-seitig abgeschlossen: Native Audio-/Movie-
Dienste verwenden WinMM PCM und einen in-process LGPL-Shared-FFmpeg/libav-
Provider ohne Dreamcast-Geraetefallback. Bytequellen sind hash-/handlegebunden,
reparse-sicher und waehrend Decode exklusiv gesperrt; Timestamps, EOS und
bounded Queues bleiben strikt. Headerloser Sofdec-PS-Inhalt wird nur ueber ein
bounded virtuelles Praefix fuer den Demuxer erkannt; `NativePortMovieSession`
reicht von `Ready` bis `Stopped`. Der relevante 24-Worker-Inkrementalbuild war
in etwa `4,5 s` erfolgreich. `KR-5003` ist source- und produktseitig
abgeschlossen: Der native hardware-only-D3D11-Pfad nutzt keine WARP/REF/GDI-
oder CPU-Rasterizer und keine PVR/TA-/historische Geraeteruntime. Native
Vertices, Texturen und Drawstate laufen ueber GPU-Offscreen-Renderflaeche und
Swapchain; Standard ist 1920x1080, Render-/Outputaufloesung sowie Game-, UI-
und Kamera-Viewports/Aspect-Policies sind getrennt. Die sichtbare native SFD-
Abnahme lief `Ready` -> `Playing` -> `Completed` -> `Stopped` mit 200
dekodierten und 200 GPU-praesentierten Videoframes, 294.016 Audioframes,
114.688.000 GPU-Uploadbytes und `hardware_accelerated=true`, ohne PVR/Scanout/
Gastframebuffer. `KR-5004` ist source- und produktseitig abgeschlossen:
Native Plattformdienste binden exakt identitaetsgebundene read-only Content-
Ranges, XInput fuer vier Gamepads und atomare projekt-/slot-/schema-gebundene
Saves mit Backup-Recovery. Read-only-/Writable-Roots, sichere IDs, User-Data-
Save-Root und Digest-Domaenen bleiben fail-closed. Der vollstaendige originale
SFD-Opening-Stream lief ohne Skip bis EOS und endete `Completed` mit 3.257
dekodierten und 3.257 GPU-praesentierten Videoframes, 4.709.760 Audioframes,
3.257 GPU-Presents und `hardware=1`. Aktiv ist jetzt KR-5005.
Der Texture-/Font-Foundation-Unterauftrag innerhalb von KR-5005 ist
source-seitig abgeschlossen. Sieben Layouts werden dekodiert; `588/588`
PVM-Archive und `16.725/16.725` Texturen sind abgedeckt, darunter `12.704`
mipmapped Texturen, `73.817` untere Mip-Level und `668.876.160` RGBA-Bytes.
SmallVQ umfasst `427` kompakte Streams und `52` Compact-Streams mit
Full-Footprint-Trailer; die Trailer bleiben hinter dem kompakten
Codebook-/Index-Stream, `0` Faelle sind ambig. Headerlose identity-bound
SDK-Fontoberflaechen belegen ARGB1555. Dieser Unterauftrag schliesst KR-5005
nicht insgesamt; einen Sonic-Produktlauf gab es dafuer nicht.
Der aktuelle P0-Lifecycle-/Datenintegritaetsfix kopiert Savebaum und
`katana-content-root.txt` transaktional; der Altport bleibt bis zum
Publikationscommit autoritativ, und Binding-Kollisionen bereinigen nur die
Teilkopie. RuntimeImages und Loaded-AOT werden vor Replacement gemeinsam
validiert und scoped deaktiviert; partielle Bereiche, Live-PC/PR, aktive
Bloecke und Immutable-Ranges bleiben fail-closed. Es gab keinen Sonic-
Produktlauf und keine Produktabnahme.
Der KR-5005-Zwischenfix transportiert die verifizierte FFmpeg-Deploymentclosure
bei Parent-Projekten ueber globale CMake-Properties und validiert Quellen sowie
sichere Dateinamen fail-closed. Die frische Konfiguration erzeugte korrekte
absolute Pfade fuer Lizenz, Notice, Buildkonfiguration und Redistribution-
Source; das Produktgate bleibt offen.
Der anschliessende Linkaudit-Fix maskiert ausschliesslich das vollständige
Composite-Fragment `nativeportplatformservices`, damit `NativePortPlatformServices`
nicht als falscher Teilstringtreffer abgelehnt wird; ein eigenstaendiges
`platformservices` bleibt verboten. Der Audit-Nachweis war Exit `0` ohne
Legacy-Geraete-/Interpreter-Symbole.

Das installierte Runtime-SDK wurde in einem frischen Prefix von einem
externen Consumer konfiguriert, gelinkt und gestartet. Die installierbare
Closure umfasst genau die fuenf benoetigten FFmpeg-DLLs sowie LGPL-/Notice-
Dateien und enthaelt keine absoluten Worktree- oder Dependency-Cache-Pfade.
Der private Provider decodierte den gebundenen Sofdec-/ADX-Inhalt vollstaendig
bis EOS; private Dateinamen, Pfade und Digests bleiben ausserhalb des
Repositorys.
Oeffentliche FFmpeg-Pakete benoetigen `FFmpeg-Corresponding-Source.zip`;
DLL, Lizenz, Notice, Buildkonfiguration und Source werden einzeln in
`runtime-dependencies.json` Schema `v3` gebunden. Ohne vollstaendige Source
bleibt `redistribution_ready=false`; die exakte 2-GB-Quellclosure liegt nicht
im Repository.

Der KR-5000-Reviewstand wurde mit `katana-recomp`, `katana_analyzer_sdk` und
`katana_native_port_runtime` in einem inkrementellen 24-Worker-Build in
`14,2 s` bestaetigt. In diesem Checkpoint gab es keine Tests und keinen neuen
Sonic-Export oder -Lauf.

Ein vorheriger KR-5005-Source-Snapshot fuehrte identity-bound Bootstrap-
Materialisierung, echte Post-Bootstrap-AOT-Roots und resumierbare Continuations
durch Analyse, CFG, Optimierung und Export. Die Provider-/Draw-IR-Grenze bleibt
backendneutral; D3D11 ist zunaechst das Windows-Backend. Steam-Deck-/Linux-
Unterstuetzung ist spaeter geplant, aktuell nicht priorisiert und kein Gate.
Der KR-5005-Produktnachweis bleibt offen. Die fruehere Aliasgrenze ist
behoben; Bootstrap und Linkaudit sind keine offenen Produktblocker. Ein
frueherer nativer Produktbeleg vervollstaendigte Film `id=0` mit `200`
dekodierten, `200` praesentierten und `200` intern als nichtschwarz
klassifizierten Frames sowie `294.016` Audioframes. Der aktuelle v74-
Direktlauf bleibt im real sichtbaren Fenster jedoch vollstaendig schwarz.
Damit sind die internen Frame-/Drawzaehler kein hinreichender Sichtbeweis und
die native Compose-/Swapchain-Grenze bleibt als Grafikregression offen. Der
schwarze/stale-Overlay-Uebergang ist geschlossen. Danach werden sechs
dynamische Oberflaechen, Stage-Content, Texturen und das erste NINJA-Modell
nativ verarbeitet. Homogenes GPU-Clipping, perspektivische Interpolation und
reziproke Depth-/Fog-Semantik passieren die frueheren Texture- und Mixed-Clip-
Stops ohne TA-/QACR-Reentry. Die danach beobachtete Registrar-/Objektfeld-
Callback-Kette ist im v74-Export generisch statisch gebunden und wird im
Direktlauf passiert. Der naechste typisierte Ausfuehrungsendpunkt ist eine
offene Host-Timing-Unterfunktion; unabhaengig davon muss der vollstaendig
schwarze sichtbare Output zuerst geschlossen werden. Film `id=1`/Opening und
Hauptmenue bleiben offen. Im frueheren Presented-by-SEGA-Pfad hatten Frames
1--189 native Draws; Frame 190 und 191 wiederholen bei geschlossenem GPU-Frame
das letzte abgeschlossene Bild. Der generische Present-or-Repeat-Vertrag ist
bestaetigt; der synthetische Schwarz-Clear ist geschlossen.

Der validierte v59-Export untersuchte `1.094` Dateien mit `198.135.759`
encodierten Bytes, dekodierte `849/849` PRS-Dateien strikt und erzeugte
`3.965` Funktionen in `127` Partitionen. `488` rohe und `395` guarded
Callback-Kandidaten sowie `39` Latent-AOT-Kandidaten wurden ohne Truncation
oder Budgeterschoepfung erfasst. Die Hardware-Closure-Gaps stiegen von `257`
auf `304`, weil mehr echte erreichbare Funktionen analysiert wurden; dies ist
keine Hardware-Regression. Der aktuelle Hardware-Closure-Stand umfasst `850`
Sites, `47` geschlossen, `803` offen und `129` Owner; ein neuer
9-Slot-/8-Unique-Callbackvektor fuehrte zu `96` weiteren Exportfunktionen.
Framepacing blieb bei 60/60 mit deaktiviertem Catch-up.

Der warme v72-Export erzeugte `5.103` Funktionen in `149` Partitionen und
`203` Host-TUs in `24,356 s`: `149/149` Codegen-Treffer, warmer Analyse-/IR-/
Metadatencache und `200/203` Hostobjekt-Treffer. Der identische kalte v71-
Export brauchte `422,637 s`; der warme Pfad ist damit etwa `17,4x` schneller.
Die Closure bleibt bei `47/850` geschlossen, weil die negative
Replacement-Reachability weiterhin nicht vollstaendig bewiesen ist.

Der v74-Export umfasst `5.316` Funktionen, `158` Partitionen und `213` Host-
TUs. Das positive Inventar stieg gegenueber v73 von `953/508` auf `2.029`
rohe und `618` guarded Callback-Kandidaten; `326.461/4.194.304` Shape-
Arbeitseinheiten blieben ohne Truncation oder Budgetende. Gegenueber v73 sind
`111` Funktionen und `2` Partitionen hinzugekommen, gegenueber v72 `213` und
`9`. `42` latente Module und `849/849` PRS-Decodes blieben vollstaendig. Die
Hardware-Closure bleibt bei `850` Sites, `47` geschlossen, `803` offen und
`129` Owner. Der Callback-AOT-P0 ist passiert; sichtbare Grafik und danach
Host-Timing bleiben die naechsten Foundationgrenzen.

Der reviewte v87-Export umfasst `5.217` Funktionen und `155` Partitionen.
`42` latente Module liefern `3.828` Blockidentitaeten, `107`
Funktionsidentitaeten, `4.222` externe Codepointer und `290` Cross-Image-
Transfers. Zwei beschreibbare relative Switchtabellen erhalten bounded
`guarded-owner-extent`-Evidenz, ohne als vollstaendiger CFG oder
Laufzeitzielsatz zu gelten. Actionable Whole-Function-Kandidaten stiegen
`116 -> 118`, fehlende exakte Grenzen sanken `16 -> 14`; der private
Disassembly-Abgleich ergab keinen weiteren exakten Import. Die groessere
Auditclosure umfasst `909` Sites in `136` Ownern (`50` geschlossen, `859`
offen). Der warme Export dauerte `117,044 s`, traf `155/155` Partitionen und
baute den Host in `2,485 s`.

Der aktuelle Produktlauf bestaetigt das SEGA-Bild ohne die zuvor sichtbaren
horizontalen Naehte, schliesst Film `id=0` ab, laedt Stage-Overlay sowie
Settings-/Camera-Assets und erreicht den ersten umfangreichen 3D-Frame. Der
Stillstand liegt dort innerhalb einer wiederholten Modell-/Polygon-
Submission. Der aktive Foundation-P0 ist die vollstaendige Grafik-/Transfer-
Ownerfamilie dieses Pfads samt Callbackkanten und Seiteneffekten, nicht ein
einzelner Stall-PC.

Der folgende RuntimeOnly-Stand ist historische Bring-up-Evidenz. Seine AOT-
Abdeckung, Adresskarte und Lebenszyklusbefunde werden wiederverwendet; seine
AICA-/ARM7- und CPU-PVR-Ausfuehrung ist keine Produktarchitektur mehr.

Funktionaler Source-Stand: aktueller KR-5005-Architekturreview-Checkpoint.
Aktuell gelten Runtime-ABI `106`,
PlatformServices-ABI `14`, Analyzer-ABI `48`, Function-Analysis-Epoch-Schema
`28`, lokales In-Process-Evaluation-Cache-Schema `13`, Backend-Interface-ABI
`23`, PVR-State-Contract `3`, Portprojektvertrag `97` und Native-Port-
Profilvertrag `20`. Der aktuelle GameProject-Vertrag ist `8` mit Artefaktformat
`6`; er transportiert die unabhaengige Native-Port-Definition ausdruecklich
nicht. Der SDK-Reviewabschluss trennt `port_export.cpp` als
nicht installierte Tooling-Object-Closure vom Analyzer-SDK und schliesst
`port_export.hpp` sowie `native_port_artifact.hpp` aus der Analyzer-
Headerinstallation aus.
Die unabhaengige `PortExportOptions::native_port_definition`-Grenze ist durch
Backend-Interface-ABI `23` versioniert; bestehende generierte Ports muessen
neu exportiert werden.
Aktuelles Native-AOT-Emissionsprofil: `36`, AOT-Partitionsschema: `7`,
Port-Metadata-Cache-Schema: `8`,
NativePort-Artifact-Format: `9`, NativePortDefinition `10`, Analysis Directives `4`, Hookkarte `v4`,
Hardware-Closure `v5` und GameProject-Metadaten `katana-game-project-v5`.

Der historische Modus `port --analysis-mode runtime-only` war nur mit
`--game-project` zulaessig und bleibt jetzt ausschliesslich internes
Diagnoseorakel. RuntimeOnly setzt fuer die Bootanalyse `GuestCallAbi::Unknown`,
umgeht damit die blockierende SuperHC-FunctionValue-/Candidate-Resolution,
erzeugt weiterhin nativen AOT-Code und nutzt RuntimeOnly-Dispatch mit einer
exakten statischen Guest->Host-Tabelle. Stop-on-miss und typed abort bleiben
aktiv; Interpreter, JIT, Runtime-Decoder und geratene Ziele sind ausgeschlossen.
Der Whole-Export-Cache ist modegebunden.

## Historischer RuntimeOnly-Geraetestand

Der letzte identische 70-s-No-Skip-Lauf erreichte `FirstVisibleGameFrame`
mit First-Frame-Digest `16866779858248182758` bei Zyklus `622122619`.
Er brachte `341` Renderrequests/-completions/-frames, `15.680`
YUV-Makrobloecke sowie `470` Audiopuffer mit `345.450` Audiobildern. Die
PVR-Fullevidenz endete nach vier bewiesenen Frames mit `1.228.800`
geaenderten Pixeln; der Audiohash `8399287713367543391` blieb zwischen
YUV-Lauf und Audio-Umbau identisch.

Die identische Vergleichsreihe stieg von `23,7959 MHz` ueber `24,1885 MHz`
und `24,2825 MHz` auf `24,2926 MHz` (`+0,4967 MHz`, `+2,09 %`). Der
Hostprozess nutzte nur etwa `1,64` Kerne beziehungsweise `6,8 %` der
24-Thread-Kapazitaet. `100 MHz`, der post-filmische Identity-Miss
der private Identity-Miss und das Memory-Card-/Hauptmenue-Gate bleiben offen.

Der Default-PlatformAbi-Pfad bleibt erhalten. Ordinary-/Inventory-Stack-
Alias-Capture und Lane-Fusion bleiben deferred PlatformAbi-Optimierungsbefunde
und sind in diesem Bring-up nicht implementiert. Die folgenden alten Candidate-
Resolution- und v56-Werte sind historische PlatformAbi-Diagnostik; damalige
Aussagen ueber fehlende Artefakte gelten nicht fuer den aktuellen RuntimeOnly-
Bring-up.

```text
historische v56-Produktevidenz:
  Exitcode 5, 1/1191 Resolution-Roots committed
  65.536 Contextual-Return-Evaluationen ausgeschoepft
  25.728 eindeutige Contexts, 27.872 physische Auswertungen
  Epoch-Retention: incomplete-root, kein Portartefakt aus diesem alten Lauf

aktueller Dokumentationsstand:
  Source-Tasks KR-4985/KR-4986/KR-4993/KR-4987/KR-4994/KR-4995 abgeschlossen;
  RuntimeOnly-Build-/Export- und sichtbares Movie-Gate bestanden;
  KR-4981 bis Memory-Card-Screen/Hauptmenue offen
```

Source-, Diagnose- und Produktevidenz duerfen nicht als derselbe Fortschritt
ausgegeben werden. Die aktuellen Dokumentationscommits veraendern keine
Recompiler-, Runtime- oder Produktsemantik.

## Historischer RuntimeOnly- und Candidate-Stand

Der alte RuntimeOnly-P0 ist kein aktiver Implementierungstask: Sein
Build-/Export-Gate war bestanden; die neue Abnahme laeuft nur ueber KR-5001
bis KR-5005.
Der Candidate-Resolution-P0 darunter beschreibt weiterhin den konservativen
PlatformAbi-Pfad und bleibt fuer diesen Bring-up zurueckgestellt.

Der terminale v56-Befund meldet null Eviction-Recomputes und liefert damit
keinen Beleg fuer Cache-Eviction als verbleibende Hauptursache. Ursache der
Explosion war, dass das Per-Function-Budget vor MultiRoot-, Cache- und
semantischer Deduplizierung pro exaktem Provenienzrequest belastet wurde.
Der Fix identifiziert Full-State-Semantic-Lanes kollisionssicher, trennt
Provenienzabonnenten und belastet das Budget nur bei neuer semantischer Lane.

Der aktuelle Zweikanal-Sourcefix vergleicht fuer die logische Contextual-
Lane den oeffentlichen Call-/State-Effekt ohne Evidence-Wachstum; die
alpha-normalisierte Evidence-Mitgliedschaft bleibt in begrenzten privaten
Provenienz-Replaykapseln fuer die physische Auswertung erhalten. Evidence-
Stale darf dadurch keine neue semantische Lane oder ein neues logisches
Budgetereignis erzeugen; Cap-/Replayfehler bleiben fail-closed.

Das `65.536`-Limit ist ein Per-Function-Budget; `25.728` Contexts und
`27.872` physische Auswertungen sind historische laufweite Aggregate und
werden nicht miteinander verrechnet.

Der Source-Fix ist fuer KR-4985/KR-4986 abgeschlossen. KR-4987 ist
source-seitig abgeschlossen: Die Read-Lens-projizierte Contextual-
SemanticLane-Identitaet verwendet vollstaendige Key-Bytes, bleibt bei
Vertragsluecke/Truncation/Fallback strikt FullState und erhaelt exakte
Provenienz/Restore sowie Discovery -> Freeze -> Publish. Nach dem Prozessende
war die temporaere JSONL bis Sequence `2266` bei `185,586 s` lesbar/gespuelt
(`2.267` Records, `10,8 MB`), aber ohne terminalen Datensatz und ohne atomare
Publikation; daraus folgt kein terminaler Produktabschluss. Es gab `348`
Candidate-Resolution-Records
von `9,371` bis `185,370 s`, zunaechst marker-only und danach ausschliesslich
fuer den zero-based Root 0. Root 1 wurde sicher nicht erreicht.

Der letzte belastbare nichtterminale D1-Snapshot bei `185,370 s` meldete
`running`, `0/1191` abgeschlossene Roots, Root 0, Wave `1.019`, Frontier `0`
bei maximal `223`, `288` zugelassene Contexts, `6.724` Evaluationen bzw.
logische Admissions, `15.170` logische Requests, `6.724` Semantic-Lanes,
`6.725` physische Auswertungen, `5.846` Cache-Reuses, `15.157` exakte
Subscriber und `226.886` Provenienzverknuepfungen. Requeues waren `1` initial
root, `287` neue exakte Lane, `8.248` Input-Widening, `177` Summary-
Aenderung, `405` Forward-Edge und `6.052` stale Dependency; stale Discards
lagen bei `12.643`. Semantic Widenings lagen bei `10.412`, provenance-only
Widenings bei `2.201`.

Die D1-Kosten meldeten Snapshot `15.170 / 2,950 s`, Key `15.160 / 5,124 s`,
inklusive Cache-Request `12.571 / 162,453 s`, inklusive Apply `63.742 /
17,790 s`, darin Binding-Merge `41.124 / 1,519 s`, Evidence `15.157 /
2,492 s`, serielle Commit-Operationen `1.018 / 0,000506 s` und
Publish-Operationen `1.018 / 0,008050 s`. Diese Operationszaehler sind keine
committed Resolution-Roots. Bindingzahl und Hitposition waren maximal jeweils
`1`; Full-State-Lanes und Projected-Physical-Keys jeweils `6.724`, Alpha-
Fallbacks `0`. Alle Context-/Evaluation-/Compositebudget-, IncompleteRoot-,
Retention-, Projected-/Classification- und allgemeinen Telemetrie-Degraded- /
Drop-Flags waren false; `telemetry_complete` war im letzten nichtterminalen
Progressdatensatz true.

D1/G1 ist damit strikt fail-closed und unentschieden: Der Transport und der
Root-0-Fortschritt sind valide nichtterminale Evidenz, aber der Supervisor-
Fehler, das fehlende terminale Atomic-Rename, `0/1191` abgeschlossene Roots
und der nicht erreichte historische Root 1 erlauben keine Entscheidung ueber
Candidate-Resolution-Gesamtzeit, Limitfreiheit, terminale
IncompleteRoot-/Retentionwerte, Coverage oder G1.

### D9-Produktbeobachtung

Der einmalige ueberwachte Sonic-Lauf dauerte `20,331 s` und endete beim ersten
fail-closed Telemetrie-/Publikationssignal. Root 0 erreichte Wave `184` und
Frontier `0` bei maximal `216`; der Prozessbaum ist sauber beendet, es gibt
kein Portartefakt und keinen Produkterfolg. Context-/Evaluation-/Composite-
Budgets blieben unverbraucht.

```text
contexts admitted:                         288
evaluations admitted / Semantic-Lanes:     2.724 / 2.724
logical requests / physical evaluations:   4.349 / 3.739
input-widening / stale-dependency requeues: 2.497 / 932
stale snapshot discards:                   1.740
semantic / provenance-only widenings:      939 / 2.377
final / maximum frontier:                  0 / 216
analysis epochs published / discarded:     0 / 1
retention:                                 incomplete-root
```

Der Root blieb fail-closed unvollstaendig: `local_fixpoint=0`,
`pending_regions=0`, alle Context-/Evaluation-/Budgetlimits `0`,
`candidate_values_truncated=1`, `abi_stack_base_unresolved=1`; alle anderen
Candidate-/Stack-/Table-Truncationflags blieben `0`. Der generische
telemetry-degraded-/Exit-34-Befund war die erwartete Folge des verworfenen
unvollstaendigen Roots, kein Haenger und kein separater Publikationsfehler.
64 Candidate-Truncation-Diagnosen waren ausschliesslich
`carrier=state`, `coordinate/domain=identity`, `values=0`; das terminale
Kandidatenbit stammt aus `inventory_stack_callback_loss_identity_truncated`.
6 Contextual-Value-Overflows erreichten jeweils `merged_values=9`. 462 Stack-Loss-
Diagnosen verteilten sich auf 189 forwarded-call, 158 candidate-store,
113 fixpoint-call und 2 forwarded-tail; tail-store-identity-loss blieb `0`.
Der naechste echte Engpass ist damit Stack-/Storage-Identitaetsverlust.

Eine Erhoehung des 65.536er-Budgets, mehr Cache oder mehr Threads ist kein
Fix. Die Arbeit muss semantisch reduziert und kausal korrekt eingeplant
werden, ohne Analyse-, Evidence- oder AOT-Abdeckung zu verlieren.

Der terminale Lauf meldet `1/1191`, daher ist Root 0 nicht mehr als
endgueltig gescheitert belegt. Bis Rootindex, Rootadresse und limitierte
Funktion terminal ausgegeben werden, gilt der Befund allgemein fuer die
ersten schweren Candidate-Resolution-Roots.

### Frueherer Vergleichslauf

Der korrekte VsDevCmd-Incremental-Build von `katana-recomp --parallel 12`
war in `42,8 s` erfolgreich; es blieben nur bekannte getenv-/Shadowing-
Warnungen. Run-ID: `kr4981-20260809-012851-0b360903`. Der fruehere Lauf
dauerte `460,6 s`; Candidate Resolution lief von
`00:37:17` bis `00:42:43` (ca. `325,8 s`). Dieser fruehere Lauf ist
Vergleichsevidenz; Summary `product-exit` bedeutet hier
nur, dass der exakt identifizierte Kindprozess nach belegter Nichtverbesserung
manuell beendet wurde. Es gab keine kanonische Publikation, `0/1194`
committed Roots, HOL `0`, Wave `103`, `272` zugelassene Contexts, `1.044`
Semantic-Lanes, `1.029` contextual physical evaluations und `2.430`
contextual logical requests.

Context-/Evaluation-/Composite-Budgets blieben unverbraucht.

Weitere D-Laufwerte: `1.359` Input-Widening-, `29` Summary- und `733`
stale-Dependency-Requeues, `1.359` stale snapshot discards,
`518.425.788 B` Cache-Payload, `3.964` physische Auswertungen gesamt sowie
`0/0` publizierte/verwarfene Analyseepochen. Ein Portartefakt oder `game.exe`
entstand nicht.

Bei Attempts `1024`, `2048` und `4096` waren die relevanten Admission-/Stack-
Diagnosezaehler bitgenau identisch zum vorherigen Fehlerlauf. Bei vergleichbarer
Gesamtzeit (~`459,6 s`) erreichte der neue Lauf jedoch Wave `103` statt `67`,
`1.044` statt `722` Semantic-Lanes, `1.029` statt `713` contextual physical
evaluations, `733` statt `839` stale requeues und `518.425.788` statt
`444.266.838 B` Cache-Payload. Der Pending-Carrier verbessert damit offenbar
Kosten je Churn-Schritt bzw. den Durchsatz, belegt aber keinen
Konvergenzhebel. Candidate-Resolution und KR-4981 bleiben offen; historisch
wurde hier Inventory-Provenance-Live-in/Spill-through als P0 vermutet.

### Lauf nach Candidate-Domain-Top-Fix

Der Candidate-Domain-Top-Fix macht abgeschnittene begrenzte Candidate-Domains
zum kanonischen absorbierenden Top mit leerem endlichem Praefix und haelt Merge,
Normalisierung, Vergleich, Keys, Persistenz, Consumer und ABI-Promotion
konsistent. Der historische Candidate-Domain-Top-Lauf lief unter
Function-Analysis-Epoch-Schema `18` und Analyzer-ABI `33`; der aktuelle
Source-Checkpoint ist separat oben ausgewiesen.
Der korrekte VsDevCmd-Incremental-Compile+Link war erfolgreich.

Der einmalige Lauf `kr4981-20260809-020628-2bfd8af5` wurde nach `343,627 s`
durch manuellen Abbruch bei belegter identischer Nichtkonvergenz beendet. Die
Voranalyse bis Candidate-Start dauerte etwa `146 s` einschliesslich des
Gesamtstarts; letzte Bewegung war Wave `48`. Peak Root: `1.450.078.208 B`,
Peak Job: `1.618.132.992 B`; keine kanonische Publikation und kein
Portartefakt. Bei Wave `39` waren alle 16 geprueften Kernzaehler exakt wie im
Vorlauf: Frontier `177`, Contexts `272`, Semantic-Lanes `606`, physische
Auswertungen `645`, exakte Subscriber `870`, Provenienz `21.355`,
Input-Widening `263`, Summary `10`, Forward `123`, stale `95`, stale Discards
`299`, semantische Widenings `553` und provenance-only `382` sowie die
weiteren geprueften Kernzaehler. Der Fix ist damit als Korrektheits-/Persistenz-
fix belegt, nicht als Konvergenzhebel; KR-4981 bleibt offen.

### [x] Abgeschlossener Hot-Callee-Diagnoseunterauftrag

Der Lauf `kr4981-20260809-024141-c4ffdf15` erreichte das erste vollständige
`attempts=1024`-Diagnosegate und wurde nach `244,549 s` bei Wave `24` gezielt
beendet. Der erwartbare Supervisorstatus `product-exit -1` entstand durch
diesen Stop, nicht durch Fehler oder Hänger. Peak Root WS war
`1.260.388.352 B`, Peak Job WS `1.387.151.360 B`; keine Publikation und kein
`game.exe`. `uncategorized=0` für alle Top-8-Funktionen.

Der dominante Befund ist eine Hot-Callee: `20` echte semantische Änderungen und
`40` Stack-Widenings, ausschließlich SavedEpoch-pending-ABI-Skalare
(`reg_epoch_pending=92`, `stack_epoch_pending=80`, `tail_epoch_pending=20`,
`state_stack_epoch_pending=20`, `state_memory_epoch_pending=20`), bei
unvollständigem Callee-Set-Stackvertrag am Owner-/Site-/Target-Feld
(`target=0`). Ordinary/direct-code/direct-PC/contextual,
Callback-Loss-, Topologie-, Top-Domain-, Map-/Tail-Topologie- und
Metadatenänderungen waren dort `0`. Eine zweite Hot-Callee zeigte `28` Änderungen,
ebenfalls Callee-Set-incomplete, jedoch gemischte Domänen. Eine weitere Hot-Callee
zeigte `48` Änderungen, darunter
`reg_epoch_pending=180`, bei vollständigem Stackvertrag.

Der Diagnose-Unterauftrag ist damit abgeschlossen; KR-4981 und das
Sonic-Produktgate bleiben ausdrücklich offen. Der SavedEpoch-Lifecycle-Fix ist
source-seitig abgeschlossen. Offen bleibt die gemeinsame Ordinary-/
Registermetadaten-/Alias-/Watcher-/Loss-/MemoryEpoch-Lifecycle-Ursache, ohne
Alias-/Current-Tracking oder fail-closed Restore zu verlieren. Die Callee-Set-
incomplete-Ursache an den dynamischen
Sites bleibt nach diesem Fix weiter zu prüfen.

### [x] Abgeschlossener SavedEpoch-Lifecycle-Unterauftrag

Current-tracking SavedEpoch-Pending-ABI-Skalare werden rekursiv nur an
bewiesenen normalen Call-/Tail-ABI-Gates konsumiert; detached Epochs bleiben
unangetastet. `candidate_payload_lost` ist physisch und semantisch ein
absorbierendes Epoch-Top ueber Normalize, Merge, Equality, Key, Subsumption,
Evidence, Restore und Persistenz. Konkrete Evidence sowie Nested-/Current-
Aliasfakten bleiben, finite Payload/Slots verschwinden; detached Top erhaelt
keine fremde Tail-Evidence. Der historische SavedEpoch-Lifecycle-Stand lief
unter Epoch-Schema `17` und Analyzer-ABI `33`.

Der Lauf `kr4981-20260809-031826-0616113a` endete nach `369,171 s` mit Status
`nonconvergence`, Exitcode `31`, durch drei zehnsekundige
Null-Publikations-Amplifikationssamples. Das war kein Crash oder Haenger:
Wave `76`, `0` committed/ready/completed Roots, `272` Contexts, Semantic-Lanes
`846 -> 863 -> 886`, physische Auswertungen `1.135 -> 1.164 -> 1.213`, Frontier
`101 -> 88 -> 131`, stale Discards `395 -> 396 -> 415`. Peak Root WS:
`1.663.037.440 B`, Peak Job WS: `1.895.583.744 B`; keine Publikation und kein
`game.exe`. D1024 und D2048: `uncategorized=0`. Der alte SavedEpoch-Pending-
Blocker ist beseitigt; der naechste Root-Analysepunkt ist die gemeinsame
Ordinary-/Registermetadaten-/Alias-/Watcher-/Loss- und MemoryEpoch-Lifecycle-
Ursache, nicht ein weiterer SavedEpoch-Pending-Patch. KR-4981 bleibt
fail-closed offen.

## Historischer Candidate-Resolution-Source- und Laufstand

Der historische Candidate-Resolution-Source-Stand ist `49b0f72a9f49d60a4eb6e0481460cd57c5625735`.
Er erlaubt strukturelle Contextual-Hybrid-Projektion mit retained sticky loss;
die autoritative Hybridprojektion schliesst Contextual-MAY-Joins und Forward-
Edges erneut vollstaendig.
erkennt SavedEpoch-Slot-Pending-Top in sämtlichen Truncation-/Publication-
Checks fail-closed und transportiert Provenance-Replay-Capsule-/Keybyte-Limits
öffentlich getrennt vom semantischen Evaluation-Limit. Ein echter Evaluation-
Cap belastet wieder nur den Evaluation-Zähler. Im historischen Stand waren
Analyzer-ABI `34`, Function-Analysis-Epoch-Schema `27` und lokales In-Process-
Evaluation-Cache-Schema `13` aktiv; der bestätigte Build war
Build-Exit `0` nach ca. `48 s`; `build-contextual-dirty/katana-recomp.exe`
trug LastWriteTime `09.08.2026 09:08:11 +02:00`. Tests wurden nicht ausgeführt.

Der historische PlatformAbi-Produktlauf `kr4981-20260809-091410-2766aaa6` endete nach ca.
`275 s` gesamt (Candidate ca. `221 s`) mit `nonconvergence` nach drei
Amplifikationssamples: `0/1274` Roots, HOL `0`, Wave `107`, `280` Contexts,
`970` Semantic-Lanes, `1.861` physische, `2.526` logische Requests,
Input-Widening `536`, Summary `22`, Forward `123`, stale Requeues `272`,
stale Discards `806`, Cache `589.178.706 B`; keine Budgets erschöpft, keine
Publikation und kein Artefakt bzw. `game.exe`. Der Supervisor schrieb wegen
`taskkill`-Zugriffsverweigerung keine Summary; der Kill-on-close-Job beendete
den Child trotzdem. Admission `1024/1024`, projected context/match jeweils
`0`; der sauberste Ordinary-Stack-Treiber blieb bei `84/84` Attempts/Semantic Changes und `508`
Ordinary-Stack-Deltas trotz vollständigem Stackvertrag.

Der vorherige Produktlauf `kr4981-20260809-083308-4a3ff9be` endete nach
`286,387 s` (Candidate ca. `232,5 s`) nach drei zehnsekündigen
Amplifikationssamples mit `nonconvergence`/Exit `31`, ohne Crash. `0/1274`
Roots, Wave `119`, kein Epoch-Publish/Discard, kein Portartefakt oder
`game.exe`; final `280` Contexts, `972` Lanes, `2.011` physische, `2.814`
logische, `203` Cache-Reuses, `2.790` Subscriber, Provenienz `169.824`,
stale Discards `922`, Frontier `43` (max `250`), Cache `610.295.241 B`.
Admission `1024/1024`, projected context/match jeweils `0`. Der P0 liegt nun
bei intra-context Ordinary-Stack und lokalen Stackkoordinaten.

Der vorherige Lauf `kr4981-20260809-041945-21b70ade` endete nach `425,924 s`
(Candidate ca. `341 s`) bei Wave `60`, `0/1194` Roots, `758` Semantic-Lanes,
`984` physischen und `1.398` logischen Auswertungen, `248` Input-, `102` stale-
Requeues und `347` Discards. Cache ca. `501 MB`, Peak Root
`1.606.066.176 B`, Peak Job `1.814.822.912 B`; kein Portartefakt.

Der Vergleichslauf `kr4981-20260809-050420-3f47fd65` wurde nach `322,632 s`
(Candidate `237,116 s`) wegen belegter Nichtverbesserung beendet: Wave `39`,
`0/1194` Roots, `272` Contexts, `549` Lanes, `630` physische, `894` logische,
`181` Input-, `10` Summary-, `76` stale-Requeues, `226` Discards,
Provenienz `31.713`, Cache `455.638.275 B`, maximale physische Dauer
`42,359 s`, Peak Root `1.490.157.568 B`, Peak Job `1.672.388.608 B`; kein
`game.exe`. Das `attempts=1024`-Gate war gegenüber `9baea88` bitgleich:
`admission_success=999`, `projected_context_changed=0`,
`projected_match_changed=0`. Die Gateänderung ist korrekt, aber kein
Konvergenzhebel. Der offene P0 bleibt intra-context Ordinary-Stack. Die
vollstaendige autoritative Hybrid-Join-Closure ist beim vollstaendigen
Stackvertrag/Gate noch nicht wirksam; LocalStackCoordinate-/unvollstaendige
Stackvertraege bleiben sekundaer zu pruefen. Keine Budget-/Thread-Erhoehung
und kein weiterer SavedEpoch-/Provenienzumbau.


## Aktueller kritischer Pfad

```text
KR-5000  native Produktgrenze und Linkisolation
  -> KR-5001  statische Spiel-/SDK-Hookkarte
  -> KR-5002  nativer Audio-/Moviepfad
  -> KR-5003  nativer GPU-Pfad
  -> KR-5004  native Disc-/Eingabe-/Save-Dienste
  -> KR-5005  No-Skip-Sonic rein nativ bis Hauptmenue
  -> v0.50.0 Alpha
```

Der fruehere RuntimeOnly-Pfad und KR-4981 bleiben historische Evidenz.
KR-4982 und KR-4983 bleiben als optionale GPU-Offload-Aufgaben des alten
Pfads gestrichen; KR-5003 ist der neue native GPU-Produktpfad.

D1 und D2 sind reale Sonic-Diagnoseexporte, keine Testmatrix. D1/G1 bleibt
wegen der historischen, nichtterminalen Root-0-Evidenz unentschieden; D2/G2
ist abgeschlossen und negativ, ohne positiven Schedulerhebel. D9 ist beendet
und Root 0 konvergierte fail-closed, ohne Portartefakt oder Produkterfolg.
Dieser D9-Befund ist historische PlatformAbi-Diagnostik. KR-4988 bis KR-4991
bleiben inaktiv; KR-4994 und KR-4995 sind source-seitig abgeschlossen.
KR-4981 ist kein aktives Produktgate mehr und wird durch KR-5005 abgeloest.

## Quellseitig vorhandene Hauptvertraege

Der aktuelle funktionale Source enthaelt unter anderem:

- statische Guarded-AOT-Einstiege und fail-closed
  Exportvollstaendigkeitsvertraege;
- getrennte semantische und inventorybezogene Analysepfade;
- inkrementelle ProgramGraph-, SCC-, ABI-, Summary- und Candidate-
  Strukturen;
- gemeinsame Analyseexecutor- und Speicherhaushaltsvertraege;
- schichtweise Analyse-, IR-, Codegen- und Hostbuildcaches;
- exakte Latent-AOT-Hints und Multi-Extent-SourceBindings;
- baseline- und bildinhaltsgebundene sichtbare Frameklassifikation;
- relatives Post-Entry-Produktgate und typisierte Fehlerausgaenge;
- vorbereiteten atomaren CompletePlatform-Apply;
- save-erhaltendes ProductHandoff-Profil;
- statische native Produktmaterialisierung ohne Interpreter oder JIT.

Diese Sourcevertraege sind fuer den aktuellen Stand nicht produktseitig
abgenommen, weil v56 kein Portartefakt erzeugte.

## Offene Produktabnahmen

- `native-port`-Link ohne ARM7-, CPU-PVR- oder Diagnoseinterpreter-Symbole;
- vollstaendige statische Hookbindung ohne Emulationsfallback;
- nativer Audio-/Moviepfad und nativer GPU-Pfad;
- native Disc-, Eingabe- und Save-Dienste;
- korrektes Opening, 60-Hz-PAL-Pfad, Memory-Card-Screen und Hauptmenue;
- reales Framepacing, stabile Audioausgabe und brauchbare Eingabelatenz;
- inkrementeller Portbuild ohne historischen Vollreexport;
- externes Spielprojekt ohne Retaildaten oder Sonic-Sonderfaelle im
  Katana-Kern.

## Test- und Reviewstatus

Projektweit gilt ab jetzt:

- Gefixt wird mit Reviews der vollstaendigen betroffenen Pfade.
- Getestet wird mit Sonic an den geplanten Produktgates.
- Keine neuen Tests, Testmatrizen, synthetischen Fixtures oder Ersatzgates.
- Fehlende neue Tests werden in Reviews nicht beanstandet.
- Vorhandene Tests und Testzahlen werden nur bei konkretem Fehler repariert.

Historische Angaben zu frueher ausgefuehrten Tests bleiben historische
Evidenz und erzeugen keine neue Pflicht fuer den aktuellen Arbeitsablauf.

## Naechster Schritt

```text
KR-5000: `native-port`-Produktprofil und Linkisolation durchsetzen.
Danach KR-5001: hoechste belegte SH-4-Spiel-/SDK-Grenzen aus der privaten
Adresskarte binden. Keine weitere ARM7- oder CPU-PVR-Optimierung.
```

Ein zweiter D1-Lauf gehoert nicht zu diesem Dokumentationspass. D2/G2 ist
abgeschlossen und negativ; KR-4988 bis KR-4991 bleiben inaktiv.
