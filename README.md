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

Aktueller Architekturstand dieses Meilensteins: Runtime-ABI 107, Block-ABI 5,
PlatformServices-ABI 14,
Analyzer-ABI 48, Function-Analysis-Epoch-Schema 28, lokales
In-Process-Evaluation-Cache-Schema 13, Application-Contract 8,
Portprojektvertrag 97, Native-Port-Profilvertrag 20 sowie PVR-State-Contract 3.
Aktuelles Native-AOT-Emissionsprofil: `36`, AOT-Partitionsschema: `7`.
Port-Metadata-Cache-Schema: `8`.
Der aktuelle GameProject-Vertrag ist `8` mit Artefaktformat `6`; der native
Port-Definitionsvertrag ist `10` und bleibt davon getrennt; das
NativePortArtifact-Format steht auf `9`. Die aktuellen
Analyse-Direktiven stehen auf `4`, das Hardware-Closure-Schema auf `v6`, die
Hookanforderungskarte auf `v5` und die exportierten GameProject-Metadaten auf
`katana-game-project-v5`.

Der Windows-Native-Port bindet XInput sowie die explizit identifizierten
WinMM-Layouts von DualSense und DualShock an Plattformvertrag `3`. Unbekannte
HID-Buttonordnungen werden nicht als semantische Controllerbelegung geraten.
Ein Sony-HID und sein XInput-Kompatibilitaetsendpoint werden nach eindeutiger
aktiver Korrelation zu genau einem Titelcontroller zusammengefuehrt; ein bis
dahin mehrdeutiger XInput-Endpunkt belegt keinen zweiten Slot. Die native
Sony-Eingabe bleibt sichtbar, Vibration laeuft ausschliesslich ueber den
wirklich gebundenen XInput-Endpunkt. Backendbereitschaft und Hotplug bleiben
fail-closed. Der Runtime-/CLI-Build ist sauber; der physische DualSense-
Menutest steht aus, bis ein Nutzer am System ist.

Der aktuelle v111/v30-Produktbeleg bindet die statisch hergeleitete
MOVIE.BIN-Sequenz als nativen, identitaetsgebundenen Multi-Clip-Vertrag. Ein
sichtbarer Lauf ohne Controllerinput oder Skip vervollstaendigte zuerst den
Sonic-Team-Film mit `200/200` Videoframes, `294.016` Audioframes und `200`
nichtschwarzen Frames und danach das originale Opening mit `3.257/3.257`
Videoframes, `4.709.760` Audioframes und `3.254` nichtschwarzen Frames. Die
Sequenz endete regulaer mit dem originalen Erfolgsstatus; der Kontaktbogen
belegt beide Filmphasen bis zum Ende. Das getestete `game.exe` hat SHA-256
`616f57fb414d25fc72fb199b5a0aa653b33c93a6ab611ff7fa325ba3f69df358`.
Erst nach diesem No-Skip-Nachweis darf ein echter Start-Controllerimpuls das
Intro in spaeteren Diagnoselaeufen abkuerzen.

Der zugehoerige Export umfasst `5.918` Funktionen in `167` Partitionen,
`4.430` rohe und `2.509` guarded Callback-Kandidaten sowie `400.972` von
`4.194.304` Entry-Shape-Arbeitseinheiten ohne Truncation oder Budgetende.
Gegenueber v29 sind das `+815` Funktionen und `+18` Partitionen. Die
Hardware-Closure verbesserte sich von `803` auf `759` Gaps; `865` Sites sind
durch vollstaendige Hooks ersetzt, `20` durch native CPU-Control-Semantik und
`53` als native Progress-Waits gebunden. Nach dem vollstaendigen Intro endet
der Port jetzt fail-closed an einem noch ungebundenen Entry eines neu
materialisierten PRS-Overlays. Opening und native Movie-Sequenz sind damit
geschlossen; Memory-Card-Pfad und Hauptmenue bleiben KR-5005.

Der aktuelle Export erweitert die generische, identitaetsgebundene PRS-
Prefix-Entry-Table-Erkennung: genau `3..64` eindeutige direkte Main-RAM-Ziele
mit Nullterminator, passendem Runtimeextent sowie Decode-/Early-CF-Pruefung
werden erst nach vollstaendiger CFG- und Relocation-Closure als RuntimeOnly-
Rootmenge zugelassen. Abgeleitete Tabellenprovenienz bleibt von privaten
exakten Hints getrennt; nicht inventarisierte Ziele enden weiterhin typisiert
per Stop-on-miss. Zwei zuvor inventory-truncated Kandidaten mit `9` und `3`
Entries sind nun zugelassen, vier Stack-/Inventory-Kandidaten bleiben
konservativ offen. `36/36` Module/Quellen ergeben `6.171` Block- und `200`
Funktionsidentitaeten, `3.406` externe Pointer, `440` Transfers, `168`
Partitionen und `5.773` Funktionen. Die sichtbare Hardware-Closure hat
`1.423` bekannte Sites und `1.425` Gaps (`1.373` hook-missing, `51`
progress-wait, `1` root-ownership); das ist erweiterte statische Sichtbarkeit,
keine Hardware-Regression. Release-Build (`24` Jobs) und Voll-Export waren
in `51,4 s` beziehungsweise `294,9 s` erfolgreich. Ein 100-s-Produktsmoke
schloss Film 0 (`200/200`) vor seinem Diagnose-Watchdog ab; ein neuer Miss
trat nicht auf, die dynamische Overlay-Bestaetigung steht noch aus.

Der Architekturreview ist in der Source- und SDK-Grenze umgesetzt: Das
installierte Produktpaket exportiert nur `KatanaRecomp::aot_runtime`,
`KatanaRecomp::native_port_runtime` und eine explizite Allowlist nativer
AOT-Header; Runtime-, PlatformServices-, Firmware-, Interpreter-, ARM7-,
AICA-, PVR-, ASIC-, GD-ROM- und Maple-Oberflaechen bleiben ausserhalb des
Produkt-SDK und im Buildbaum als internes Diagnoseorakel.
Der SDK-Reviewabschluss trennt `port_export.cpp` als nicht installierte
Tooling-Object-Closure vom Analyzer-SDK und schliesst
`port_export.hpp` sowie `native_port_artifact.hpp` aus der Analyzer-Header-
Installation aus. `NativePortDefinition`, `NativePortArtifact`,
`NativePortContent`, `NativePortRuntime`, `NativePortContext`, Bootstrap,
direkte Linkersymbole, read-only Content-Mappings, Hook-/Hardware-Closure,
direkter nativer Dispatch und Linkaudit sind implementiert. Ein privater
Sonic-Adapter wird erreicht, statisch rekompilierter Spielcode startet; der
erste unaufgeloeste Plattformzugriff endet typisiert als
`UnresolvedHardwareAccess` ohne Emulator-/Interpreter-/Runtimefallback. Der
generierte Runner verlangt Executable plus privaten ContentRoot und validiert
beide Pfade; der explizite Bring-up-Schalter gilt nur bei unvollstaendiger
Closure. Einen historischen Zyklusbudgetpfad gibt es nicht. Das
identitaetsgebundene Image-/Hookmanifest bleibt Teil des Produktvertrags.
Der historische Dreamcast-Launcher wird niemals als nativer Fallback gelinkt.

KR-5001 ist source-seitig abgeschlossen: Die deterministische
`metadata/native-hook-requirements.json`-Karte und Hardware-Closure Schema
`v5` und Hookanforderungskarte `v4` verlangen exakte Function-/Instruction-
Replacement-Proofs an echten
Funktionsentry-Grenzen. Bekannte
Hardware- und unbekannte Instruktionsstellen bleiben hookpflichtig;
range-gepruefte Native-Memory-Zugriffe enden ausserhalb typisiert. Der
`GuestInstructionOrigin` bleibt in `MemoryAccessError`, Emitter und
Native-Dispatch auch ohne Tracesink erhalten. KR-5002 ist source-seitig
abgeschlossen: Native Audio-/Movie-Dienste laufen ohne Dreamcast-
Geraetefallback ueber WinMM PCM und einen in-process LGPL-Shared-
FFmpeg/libav-Provider. Hash-/handlegebundene, reparse-sichere und waehrend
des Decodes gesperrte Bytequellen, strikte Timestamps/EOS und bounded Queues
bleiben aktiv. Headerloser Sofdec-PS-Inhalt wird nur fuer den Demuxer ueber
ein bounded virtuelles Praefix erkannt; der oeffentliche
`NativePortMovieSession`-Lebenszyklus reicht von `Ready` bis `Stopped`. KR-5003
ist source- und produktseitig abgeschlossen: Der native GPU-Pfad verwendet
ausschliesslich hardwarebeschleunigtes D3D11, ohne WARP/REF/GDI/CPU-
Rasterizer, PVR/TA oder historische Geraeteruntime. Native Vertices, Texturen
und Drawstate laufen ueber eine GPU-Offscreen-Renderflaeche und Swapchain.
Standard ist 1920x1080; Render-/Outputaufloesung sowie Game-, UI- und Kamera-
Viewports/Aspect-Policies sind getrennt. Eine sichtbare native SFD-Abnahme
erreichte Ready, Playing, Completed und Stopped mit 200 dekodierten und 200
GPU-praesentierten Videoframes, 294.016 dekodierten Audioframes,
114.688.000 GPU-Uploadbytes und `hardware_accelerated=true`; PVR/Scanout/
Gastframebuffer waren nicht beteiligt. Das installierte Runtime-SDK wurde
von einem externen Consumer gebaut, gelinkt und gestartet; der naechste aktive
KR-5004 ist source- und produktseitig abgeschlossen: Der neue
`NativePortPlatformServices`-Vertrag bindet exakt identitaetsgebundene
read-only Content-Ranges, XInput fuer vier Gamepads sowie atomare, projekt-,
Slot- und schema-gebundene Saves mit Backup-Recovery und exklusiver
Instanzsperre. Read-only- und Writable-Roots ueberlappen nicht; Pfad-/ID-
Validierung, User-Data-Save-Root und Digest-Domaenen bleiben fail-closed.
Der vollstaendige native SFD-Opening-Stream lief ohne Skip bis EOS und endete
sauber mit `Completed`: 3.257 dekodierte und 3.257 GPU-praesentierte
Videoframes, 4.709.760 Audioframes, 3.257 GPU-Presents und `hardware=1`.
Der Texture-/Font-Foundation-Unterauftrag innerhalb von KR-5005 ist
source-seitig abgeschlossen: Sieben Texture-Layouts einschliesslich Mipmap-
und SmallVQ-Varianten werden dekodiert. Im vollstaendigen Korpus wurden
`849` PRS-Dateien, `588/588` PVM-Archive und `16.725/16.725` Texturen
verarbeitet; `12.704` waren mipmapped, mit `73.817` unteren Mip-Leveln und
`668.876.160` dekodierten RGBA-Bytes. Die headerlose identity-bound
SDK-Font-Grenze belegt ARGB1555. Dies ist ein erledigter Foundation-
Unterauftrag innerhalb von KR-5005, nicht die No-Skip-Abnahme.
Der aktuelle P0-Lifecycle-/Datenintegritaetsstand kopiert den lokalen
Savebaum und die `katana-content-root.txt`-Bindung transaktional, laesst den
Altport bis zum Publikationscommit autoritativ und bereinigt nach einer
Binding-Kollision nur die Teilkopie. RuntimeImages und Loaded-AOT werden vor
Executable-Replacement gemeinsam validiert und ueber einen scoped,
kanonischen Retirement-Pfad vollstaendig deaktiviert; Live-PC/PR,
Active-Block, partielle Bereiche und Immutable-Ranges bleiben fail-closed.
Dies ist kein Sonic-Produktlauf und keine No-Skip-Abnahme.
KR-5005 bleibt das offene native Produktgate. Die Grafik-Foundation ist fuer
den aktuell erreichten Pfad source-seitig weitgehend implementiert, aber
produktseitig wieder offen: Texturen,
dynamische Oberflaechen, NINJA-Modellpunkte, vollstaendige native Drawstates,
homogenes GPU-Clipping, perspektivische Interpolation und reziproke Depth-/Fog-
Koordinaten laufen ueber die backendneutrale Draw-IR und das D3D11-Backend.
Der reale Lauf passiert die frueheren typisierten Texture- und Mixed-Clip-
Stops ohne TA-/QACR-Reentry oder Software-PVR. Die anschliessende statische
Callback-Luecke ist generisch geschlossen: externe, bereits erreichbare CFG-
Bloecke werden als lokale Analyseowner behandelt, gespeicherte Codepointer
ueber Registrar und Objektfeld verfolgt und nur durch Non-Root-Funktionshints
oder einen eigenstaendigen Entry-Shape-Beweis als AOT-Ziele zugelassen. Der
Produktlauf passiert diese Kette, Film `id=0`, den Stage-Overlay-Ladevorgang
und die ersten nativen Modell-Draws. Das SEGA-Bild ist nach der gemeinsamen
Vertex-/Pixel-Constant-Buffer-Bindung sichtbar und ohne die zuvor beobachteten
horizontalen Naehte. Der aktuelle Lauf bleibt danach im ersten umfangreichen
3D-Frame innerhalb der Modell-/Polygon-Submission stehen. Der aktive
Foundation-P0 ist deshalb jetzt die vollstaendige Grafik-/Transfer-
Ownerfamilie dieses Drawpfads, nicht ein einzelner Stall-PC. Vollstaendige
spielweite Grafikabdeckung bleibt bis zu Opening, Menue und Gameplay
unbewiesen. Steam-Deck-/Linux-Unterstuetzung bleibt eine spaetere,
nicht aktuelle Prioritaet und ist kein gegenwaertiges Produktgate.

Ein frueherer KR-5005-Zwischenstand trug die generischen Native-Architektur-
Reviewfixes: Post-Bootstrap-Bytes werden vor Analyse, IR und AOT materialisiert
und jedes emittierte Instruktionswort wird gegen die gebundene Post-Ansicht
validiert. Exakte Non-Root-Grenzen, edge-only JumpTables, image_id-gebundene
Metadaten, aktive Overlayauswahl, CallbackTable-Roots und sichere
Replacement-Reachability sperren unbewiesene Negativbeweise; deklarativ
begrenzte Callback-Reentrys sowie der Nested-AOT-Fehlertransport erhalten
typisierte Hook-Aborts bis zum Produktlauf. Linkmap-Owner-/PE-Importaudit und
die Ablehnung historischer CpuState-Bindings bleiben fail-closed.

Der zugehoerige inkrementelle Release-Build von `katana-recomp` mit 24 Jobs war in
`18,8 s` erfolgreich. Ein frischer Export nach Vertragsinvalidierung umfasste
`1.812` Funktionen und `44` Partitionen mit `44` Codegen-Hits und `0` Misses;
der Folgeexport nach dem Nested-AOT-Fehlerfix erreichte erneut `44/44` Hits.
Der native Produktlauf erreichte den ersten untexturierten Draw und danach den
Sprite-Texture-Pfad, endete aber erwartungsgemaess typisiert mit
`0x53414704`. Diese fruehere Evidenz ist durch die nachfolgende
Lazy-AOT-Aliasnormalisierung ueberholt; es gibt keine Emulations- oder
Interpreterfallbacks.

Ein frueherer Produktbeleg vervollstaendigte Film `id=0` mit `200`
dekodierten und `200` praesentierten Videoframes, `294.016` Audioframes und
`200` intern als nichtschwarz klassifizierten Frames. Der aktuelle v74-
Direktlauf bleibt im real sichtbaren Fenster vollstaendig schwarz, waehrend
Audio und Titelablauf weiterlaufen. Das Checkpoint-Runtime-Image wurde vor
dem ersten Stage-Overlay genau einmal deaktiviert; anschliessend wurden die
Overlay-, Settings- und Camera-Assets identitaetsgebunden geladen. Der
schwarze/stale-Overlay-Uebergang ist geschlossen. Danach werden sechs
dynamische Titeloberflaechen, Stage-Content, Texturen und das erste native
Modell materialisiert und gezeichnet; die frueheren Texture- und Mixed-Clip-
Stops sowie die anschliessende gespeicherte Callback-Kette werden passiert.
Der Lauf endet nun typisiert an einer noch offenen Host-Timing-Unterfunktion.
Film `id=1`/Opening und Hauptmenue bleiben offen. Im
Presented-by-SEGA-Pfad haben die Frames 1--189 native Draws;
Frame 190 und Frame 191 wiederholen ohne neuen offenen GPU-Frame das letzte
abgeschlossene Bild. Es gibt keinen synthetischen Schwarz-Clear mehr.

Seit `cf15c229` bleibt der PRS-Transform auf den statisch rekompilierten
Originalcode begrenzt; atomar retired werden nur ueberlappende Runtime-
Images vor dem Transform. Der generische Lazy-AOT-Aliasfehler ist behoben:
Nach `bind_entry` wird der Entry-Source in die gebundene P1-Sicht
normalisiert, sodass P0/P1/P2-Ziele den Latent-AOT-Owner erreichen.
Der warme v72-Export umfasst `5.103` Funktionen, `149` Partitionen und `203`
Host-TUs. Er endete mit `149/149` Codegen-Treffern, warmem Analyse-/IR-/
Metadatencache und `200/203` Hostobjekttreffern in `24,356 s`; der identische
kalte v71-Export brauchte `422,637 s`. Das sind `398,281 s` beziehungsweise
`94,2 %` weniger und etwa `17,4x` schneller. Der Hardware-Closure-Stand
umfasst weiter `850` Sites, davon `47` geschlossen, `803` offen und `129`
Owner. Die Grafik kann nachgelagerte TA-/QACR-/PVR-Sites erst dann statisch
entladen, wenn die Replacement-Reachability vollstaendig bewiesen ist; aktuell
steht dieser Beleg bewusst auf `false`.

Der v74-Export nach der generischen Callback-Owner-/Registrar-Erweiterung
umfasst `5.316` Funktionen in `158` Partitionen und `213` Host-TUs. Gegenueber
v73 sind das `+111` Funktionen und `+2` Partitionen, gegenueber v72 `+213`
Funktionen und `+9` Partitionen. Das positive statische Inventar stieg von
v73 `953/508` auf `2.029` rohe und `618` guarded Callback-Kandidaten;
gegenueber v72 sind das `+1.378/+110`. Die Shape-Pruefung stieg auf `326.461`
Arbeitseinheiten, blieb aber weit unter ihrem Budget von `4.194.304`; es gab
keine Truncation oder Budgeterschoepfung. `42` latente Module und `849/849`
PRS-Decodes blieben vollstaendig. Die Hardware-Closure blieb erwartungsgemaess
bei `850/47/803/129`, weil dieser Schritt AOT-Abdeckung erweiterte, aber noch
keinen Hardwareprovider ersetzte. Der direkte `game.exe`-Lauf passierte den
alten Callback-Endpunkt und erreichte die naechste Host-Timing-Grenze.

Der v94-Zwischenstand verfeinert die begrenzte statische Callback-
Inventarisierung: Eine per SH-4-Indexarithmetik nachgewiesene, variable
Record-Stride wird bis `256` Bytes durchsucht (der relevante Titelpfad hat
`16` Bytes), verlangt mindestens zwei stride-konsistente Codeeintraege und
einen Nicht-Code-Terminator und bleibt vollstaendig `guarded`. Ein blosses
PC-relatives Literal wird dagegen erst nach einem echten statischen
Speicher-Dereferenz als Descriptor-Tabellenquelle zugelassen; so bleiben
Formatstrings und Ressourcen ohne irrefuehrende Callback-Evidenz. Der Export
liefert `5.196` Funktionen in `153` Partitionen, `1.960` rohe und `612`
guarded Callback-Kandidaten, `140` Codegen-Treffer und `13` Misses in
`171,7 s`; weder Truncation noch Budget- oder ungelöste-CF-Terminierung trat
auf. Die Hook-Proofs tragen nun die eingehenden Instruktionsquellen und
externe Fortschritts-Waits als explizite Provideranforderung. Der direkte
Produktlauf passiert den frueheren `0x8C01E6A6`-Miss, erreicht Frame `198`
und laedt `STG00`/3D; der neue deterministische Stop ist
`loaded-aot-entry-identity-missing` bei Runtime `0x8C900D76`, Quelle
`0x8805A000+0xD76`. Damit bleibt der fehlende Block eine
identitaetsgebundene Loaded-AOT-Abdeckung, kein stillschweigend ausgefuehrter
Fallback.

Der reviewte v87-Export erweitert diese statische Grundlage auf `5.217`
Funktionen in `155` Partitionen. `42` latente Module liefern `3.828`
Blockidentitaeten, `107` ganze Funktionsidentitaeten, `4.222` externe
Codepointer und `290` identitaetsgebundene Cross-Image-Transfers. Zwei
beschreibbare relative Switchtabellen werden jetzt als positive, bounded
`guarded-owner-extent`-Evidenz behandelt: ihre `850`- und `992`-Byte-Owner
werden zusammengefuehrt, ohne die Tabellenwerte als vollstaendigen CFG oder
Laufzeitziele auszugeben. Actionable Whole-Function-Kandidaten stiegen
`116 -> 118`, fehlende Grenzen sanken `16 -> 14`; der Disassembly-Abgleich
fand unter den verbleibenden Grenzen keinen weiteren exakten Import. Die
Hardwarekarte umfasst durch die groessere echte Auditclosure nun `909` Sites
in `136` Ownern (`50` geschlossen, `859` offen). Der warme v87-Lauf endete in
`117,044 s` mit `155/155` Codegen-Treffern und `2,485 s` Hostbuild; gegenueber
v83d (`432,1 s`) ist das etwa `3,69x` schneller. Mehr offene Sites sind hier
erweiterte Sichtbarkeit, keine Hardware-Regression.

Der validierte v59-Export untersuchte `1.094` Dateien mit `198.135.759`
encodierten Bytes, dekodierte `849/849` PRS-Dateien strikt und erzeugte
`3.965` Funktionen in `127` Partitionen. Er nahm `39` Latent-AOT-Kandidaten
auf; `488` rohe und `395` guarded Callback-Kandidaten wurden ohne Truncation
oder Budgeterschoepfung erfasst. Die Hardware-Closure wuchs von `257` auf
`304`, weil mehr echte erreichbare Funktionen analysiert wurden; das ist keine
Hardware-Regression. Framepacing blieb bei 60/60 mit deaktiviertem Catch-up.

Der KR-5005-Zwischenfix schliesst die CMake-Deploymentgrenze fuer FFmpeg im
Parent-Projekt: Die verifizierte Closure wird unabhaengig vom Caller-Scope
transportiert, absolute Quellen und sichere Dateinamen werden fail-closed
geprueft. Der frische Konfigurationsnachweis erzeugte korrekte absolute
Pfade fuer Lizenz, Notice, Buildkonfiguration und Redistribution-Source;
KR-5005 selbst bleibt als Produktgate offen.
Der anschliessende Linkaudit-Zwischenfix maskiert ausschliesslich den
vollstaendigen erlaubten Composite-Identifier `nativeportplatformservices`;
ein eigenstaendiges oder anderes `platformservices` bleibt verboten.

Der letzte funktionale, jetzt historische RuntimeOnly-Source-Stand ist der
Runtime-Performance-Checkpoint. Die
oeffentlichen AICA-/ARM7-Handoff-Layouts sind deshalb auf Runtime-ABI 90
versioniert; PlatformServices-ABI 14 und Backend-Interface-ABI 13 gehoeren zu
diesem historischen Checkpoint. Die damalige Erweiterung von
`PortExportOptions` und `LatentAotDiscoveryOptions` fuehrte Backend-
Interface-ABI `13` ein; der jetzt unabhaengige
`PortExportOptions::native_port_definition`-Vertrag wurde inzwischen auf
Backend-Interface-ABI `23` weiterentwickelt.

Der historische opt-in-Modus `port --analysis-mode runtime-only` war nur mit
`--game-project` zugelassen. Ab v0.49.1 ist er ausschliesslich internes
Diagnoseorakel und kein Produkt-/Releaseprofil. Fuer seine Bootanalyse setzt er
`GuestCallAbi::Unknown`, ueberspringt die blockierende SuperHC-
FunctionValue-/Candidate-Resolution, erzeugt weiterhin nativen AOT-Code und
verwendet RuntimeOnly-Dispatch mit exakter statischer Guest->Host-Tabelle.
Stop-on-miss und typed abort bleiben aktiv; es gibt keinen Interpreter, JIT,
Runtime-Decoder oder geratenen Zielpfad. Der Whole-Export-Cache ist an den
Analysemodus gebunden.

Ein historischer No-Skip-RuntimeOnly-Diagnoselauf erreichte das damalige
Milestone `FirstVisibleGameFrame` ohne Start-Impuls, Movie-Skip,
Framebuffer-Hack oder kuenstlichen Moviepfad. Die begrenzte Bild- und
Audiopublikation war dort nachweisbar; private Frame-, Audio- und
Ausfuehrungszaehler sind bewusst nicht Teil dieser Produktdokumentation.
Dieser historische Befund ersetzt weder eine fortlaufende Vollbild-Evidenz
noch das noch offene Memory-Card-/Hauptmenue-Gate.

Der fruehere Checkpoint `e1d8ade` bindet einen echten AICA-ARM7TDMI-Kern,
Sound-/Main-Interrupts,
REG_L/REG_M, portable Fortsetzung und die Common-Monitorregister fuer MIDI-
Leerstand, Channel-Lifecycle und Current Address. Der zuvor bei null stehende
Sofdec-Audiotakt erreicht nun `0x2D0` und `0x890` bei der Einheit `0xAC44`
(`44.100`). Im No-Skip-Lauf gingen beide Readinesspfade auf `1`, der
Movie-Lifecycle auf Status `5`, und YUV-/PVR-/FB_R-Publikation wurde sichtbar.
Der Hostprozess nutzte dabei nur etwa `1,64` Kerne beziehungsweise `6,8 %`
der 24-Thread-Kapazitaet; der damalige Performanceblocker lag daher beim
seriellen Runtime-/Dispatch-Overhead. `100 MHz`, der anschliessende
der nachgelagerte private Identity-Miss und das nicht erreichte Hauptmenue
sind historische Evidenz, aber keine aktiven Produktgates mehr.
Der Default-PlatformAbi-Pfad
bleibt erhalten; Ordinary-/Inventory-Stack-Alias-Capture und Lane-Fusion sind
spaetere, deferred PlatformAbi-Optimierungsbefunde und nicht Teil dieses
Bring-up-Meilensteins.

## Historischer RuntimeOnly-Bring-up-Stand

Der letzte historische Lauf belegt den natuerlichen Audio-/Videopfad bis zum ersten
sichtbaren Spielbild; der Bring-up ist damit sichtbar, aber noch kein
Hauptmenue-/Memory-Card-Gate. `PVR-State-Contract 3` fuehrt die Sentinel-
Semantik fuer abgeschlossene Fullevidenz in Snapshot, Persistenz und
generiertem Produktpfad. Das oeffentliche Runtime-Layout bleibt kompatibel;
Runtime-ABI 90 wird nicht angehoben.

Der zugehoerige Runtime-Performance-Stand haelt ARM7-RAM/Registerlocks ueber
einen `run_cycles`-Batch, nutzt direkte AICA-Sound-RAM-Spans und persistente
Scratchpuffer, committed den 32-Byte-Channel-2-DMA-Pfad fuer PVR-Geraete
wortweise und beobachtet PVR-YUV-Konfigurationswechsel einmal je Guest-Write.

Die erhaltene historische Source-Wiring umfasst eine Cross-Shard-
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
dokumentiert; er ist kein aktiver Native-Port-Buildblocker. Der
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
KR-4981-Produktgate. Dominant war eine Hot-Callee mit ausschließlich SavedEpoch-
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
Evaluation-Zähler. Im historischen Stand waren Analyzer-ABI `34`,
Function-Analysis-Epoch-Schema `27` und lokales In-Process-Evaluation-Cache-
Schema `13` aktiv; der bestätigte Build
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
`0`. Der sauberste Ordinary-Stack-Treiber blieb bei `84/84` Attempts/Semantic Changes und `508`
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
Runtime-ABI:                    102
Block-ABI:                       5
Analyzer-ABI:                   39
PlatformServices-ABI:           14
Backend-Interface-ABI:          21
Portprojektvertrag:             89
Native-Port-Profilvertrag:      13
Native-AOT-Emissionsprofil:     29
AOT-Partitionsschema:            7
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
Handoff-Artefaktformat 2 und Plattformzustandsvertrag 2; der aktuelle
KR-5005-Stand verwendet Runtime-ABI 107, Analyzer-ABI 48, Portprojektvertrag 97 und
Native-Port-Profilvertrag 20. Davon getrennt verwendet `GameProject` Vertrag 8 und
Artefaktformat 6. `CompletePlatform` erfasst und restauriert den kanonischen
Satz aus 22 Dreamcast-Geraeten einschliesslich Flash sowie die exakte
typisierte Scheduler-Timeline. Capture und Apply sind nur im historischen
Produktport belegt. Der historische PlatformAbi-D-Lauf war der freigegebene KR-4981-
Produktversuch und bestand das globale Produktgate nicht; ein weiterer Lauf
ist nicht automatisch freigegeben.

`GameProjectArtifact` Format 6 transportiert fuer Spielprojektvertrag 7 die deklarativen,
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

## Historischer Disc-Diagnosepfad

Die folgende `--install-disc`-Beschreibung gehoert ausschliesslich zum
historischen Disc-Diagnosepfad. Ein nativer Produkt-Runner erwartet stattdessen
eine Executable und einen privaten `ContentRoot`, validiert beide Pfade und
besitzt keinen `--install-disc`-Produktflow.

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

Die installierbaren Produktziele lauten
`KatanaRecomp::native_port_runtime` und dessen transitive AOT-Basis
`KatanaRecomp::aot_runtime`. Ein direkt konfiguriertes natives Portprojekt
kann sie per `find_package` verwenden; `KatanaRecomp::runtime` und
`KatanaRecomp::runtime_core` bleiben ausschliesslich im internen Diagnose-
Buildbaum. Die installierte CLI erkennt das Runtimepaket im gemeinsamen
Installationspraefix automatisch; `KATANA_RUNTIME_PREFIX` waehlt ein anderes
installiertes Paket.
`KATANA_RUNTIME_BUILD_TARGETS` kann den
`KatanaRuntimeBuildTargets.cmake`-Export eines lokalen Buildtrees direkt
binden. Single-Config-Baeume muessen als `RelWithDebInfo`, `Release` oder
`MinSizeRel` konfiguriert sein; bei Multi-Config-Baeumen baut die CLI eine
vorhandene optimierte Konfiguration.
`KATANA_RUNTIME_ROOT` bleibt der explizite Quellbaum-Fallback und wird
`EXCLUDE_FROM_ALL` eingebunden. Generierte AOT-TUs verwenden die native
Produktheader-Allowlist sowie eine PCH; der historische
`aot_runtime_abi.hpp`-Vertrag wird nicht als Produkt-SDK installiert.
Der normale Portexport verwendet fuer Bring-up standardmaessig den schnellen
Release-Hostbuild: nur generierte AOT-TUs werden mit `/Od /Ob0` und einem
eigenen, auf MSVC standardmaessig vier Worker breiten Ninja-Pool kompiliert.
Runtime, Titeladapter und Bootstrap bleiben optimiert; der separate
`gate`-Build erzeugt das voll optimierte Produkt. Eine gemeinsame `/Zi`-/`/FS`-
PDB ist im AOT-Pfad ausgeschlossen.

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
klassifiziert. Dieser Vertrag bleibt als historische Diagnosegrenze
dokumentiert; KR-5000 trennt den nativen Produktpfad physisch von diesem
Geraeteverbund.
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
passiert der Produktlauf den damaligen Blocker aus KR-4971. Der historische
Sichtlauf ist kein kontrollierter Performancebenchmark und belegt keine
vollstaendige Langlaufabnahme.

Sein erster Blocker war KR-4972:
die geprüfte private Callback-Kante. Das unveraenderte Ziel beginnt mit einem Sprung
auf einen gemeinsamen Codepfad. Die generische Analyse verfolgt den
Callback jetzt ueber begrenzte Tail-Jump- und Runtime-Frame-Pfade, erkennt
die private Callback-Kante als Funktion und erreicht den gemeinsamen Body.
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
installierte v32-MSVC-Port erreichte historisch einen sichtbaren Lizenzscreen,
beendete jedoch am damaligen verifizierten Callback-Gate. Diese Beobachtung
bleibt historische Vergleichsevidenz und ist kein Nachweis fuer den aktuellen
Source-Checkpoint oder ein Langlauf-Performancegate.

## Diagnose

- **Produkt-Performance:** feste Aggregatzaehler; RuntimeOnly-Sitedetails und
  deren Map bleiben im normalen Produktlauf deaktiviert.
- **Crash Capsule:** ein fester allokationsfreier POD-Ring haelt letzten Block,
  MMIO, Schedulerereignis und ersten Fehler; Strings entstehen erst terminal.
- **Fehlerdiagnose:** der bestehende begrenzte Dispatchrecorder wird nur bei
  expliziter tiefer Diagnose an den Dispatcher gebunden.
- **Tiefe Diagnose:** Wait-Loop-Rohwerttrace und vollstaendige
  Dispatchereignisse sind explizite lokale Opt-ins.

Ein automatisch durch Ausfuehrungsfortschritt/PC/Fehler begrenztes
Triggerfenster ist in
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
