# Verbindlicher Native-Port-Produktvertrag

Status: verbindliche Architekturentscheidung ab 11. August 2026.

Dieses Dokument hat fuer den Produktpfad Vorrang vor aelteren Bring-up-,
RuntimeOnly-, AICA-, PVR-, Performance- und Handoff-Beschreibungen. Alte
Messungen bleiben historische Evidenz, definieren aber nicht mehr die
Zielarchitektur.

## Entscheidung

KatanaRecomp baut native PC-Ports. Das ausgelieferte Spiel ist kein Emulator
und enthaelt keinen emulierten Dreamcast als Laufzeitumgebung.

```text
statisch analysierter SH-4-Spielcode
  -> statisch erzeugtes natives C++
  -> validierte native Spiel-/SDK-Hooks
  -> native PC-Grafik, -Audio, -Dateien, -Eingabe und -Speicherstaende
  -> natives Spielbinary
```

Der SH-4-Spielcode wird vor dem Build statisch rekompiliert und danach direkt
auf der Host-CPU ausgefuehrt. Plattform- und Middlewaregrenzen werden an der
hoechsten sicher identifizierten Schnittstelle auf native Hostdienste
abgebildet. Ein allgemeiner Dreamcast-Geraeteapparat ist kein Bestandteil des
Produktpfads.

## Im Produktpfad verboten

- interpretierte Gast-CPUs, insbesondere ein AICA-ARM7-Interpreter;
- JIT, Runtime-Dekodierung oder ein Interpreterfallback fuer SH-4-Code;
- ein CPU-PVR-Softwarerasterizer oder ein emulierter PVR als Produktrenderer;
- zyklusweises Ausfuehren von Gast-Geraetefirmware;
- ein vollstaendiger emulierter ASIC-/AICA-/PVR-/GD-ROM-/Maple-Geraeteverbund;
- erfundene Completion-Flags, automatische Bildwechsel, Movie-Skips,
  vorgerenderte Ersatzframes oder titelbezogene Adresshacks im Katana-Kern;
- ein stiller Rueckfall auf historische Geraetemodelle, wenn ein nativer Hook
  fehlt.

Fehlt eine erforderliche native Bindung, endet der Produktbuild oder der
Produktlauf typisiert und fail-closed. Er wird nicht durch Emulation gerettet.

## Native Zielschichten

### CPU

- SH-4-Spielcode wird statisch in C++ beziehungsweise nativen Hostcode
  ueberfuehrt.
- Statisch bekannte Kanten werden direkt verkettet; zentrale Dispatchgrenzen
  bleiben nur an wirklich dynamischem Kontrollfluss und nativen Hookgrenzen.
- Die Adress-/Funktionskarte des privaten Spielprojekts dient der statischen
  Bindung, nicht einer Laufzeitinterpretation.

### Grafik

- Grafikarbeit wird an der hoechsten belegten NINJA-/Kamui-/Spielrenderer-
  Grenze abgefangen und in eine native GPU-API uebersetzt.
- Transformation, Rasterisierung, Texturierung, Blending und Present laufen
  auf der PC-GPU.
- Ein CPU-PVR darf nur noch in einem getrennten Diagnose-/Referenzprofil
  existieren und darf nicht in ein Produktbinary gelinkt werden.
- Das Interpretieren von TA-Paketen ist PVR-HLE und deshalb ebenfalls kein
  zulaessiger Produktfallback. TA bleibt ausschliesslich Diagnoseevidenz.

### Audio und Movie

- mwSnd-/CRI-/ADXT-/Sofdec-Aufrufe werden vor dem AICA-Kommandoring auf eine
  native Audio-/Movie-Implementierung abgebildet.
- Decoding, Mixing, Pufferung und Ausgabe verwenden native Hostbibliotheken
  und das PC-Audiogeraet. Normale CPU-Decodierung oder -Mischung eines PC-
  Ports ist erlaubt; AICA-Instruktions- oder Geraeteemulation ist es nicht.
- Das Opening bleibt vollstaendig und ungeskippt. Status, Callbacks und
  Lebenszyklus werden aus echter nativer Verarbeitung erzeugt, nicht direkt
  gesetzt.

### Dateien, Eingabe und Speicherstaende

- Discdateien werden aus der lokalen Originalinstallation ueber native
  Dateisystemzugriffe bereitgestellt.
- Maple-Eingabe wird auf native Controller-APIs abgebildet.
- VMU-/Flash-Speicherstaende werden ueber eine native, atomare und
  versionsgebundene Speicherdatei umgesetzt.
- Retaildaten, private Adressen und titelgebundene Hooktabellen bleiben im
  externen privaten Spielprojekt und werden nicht in KatanaRecomp eingecheckt.

## Erlaubte Kompatibilitaetsbruecken

Ein nativer Port braucht weiterhin klar definierte Daten- und Kontrollgrenzen.
Erlaubt sind deshalb kleine, explizite Adapter fuer:

- ABI-, Pointer-, Speicherlayout- und Endian-Konvertierung;
- Rueckrufe in statisch rekompilierten Spielcode;
- Hostzeit, Framepacing und asynchrone Completion;
- die minimale fuer den gebundenen Spielcode sichtbare Statusprojektion;
- Validierung, Identitaet, Save-Autoritaet und typisierte Fehler.

Diese Adapter duerfen keine Gastinstruktionen interpretieren, keinen
Dreamcast-Chip softwareseitig nachbauen und keinen allgemeinen
Emulationsfallback bilden. Die Grenze lautet: Daten uebersetzen und native
Dienste aufrufen, nicht Konsolenhardware ausfuehren.

## Historischer RuntimeOnly-Stand

Der Checkpoint `001f3c2` und der sichtbare No-Skip-Sonic-Lauf bei
`24,2926 MHz` bleiben wertvolle Bring-up-Evidenz. Sie haben unter anderem
belegt:

- die statische SH-4-AOT-Abdeckung bis in den Moviepfad;
- reale Kontrollfluss-, Funktions- und Adressbindungen;
- den erwarteten No-Skip-Audio-/Videolebenszyklus;
- relevante Render-, Audio-, YUV-, Callback- und Post-Movie-Grenzen;
- den nachgelagerten privaten Identity-Miss.

Der dabei verwendete ARM7-/AICA- und CPU-PVR-Pfad ist ab jetzt nur historische
Referenz und Diagnoseevidenz. Er wird weder zum Produktpfad erklaert noch
weiter auf Produktperformance optimiert. Seine Messwerte sind keine
Abnahmebasis fuer den nativen Port.

## Umschaltbarkeit und Linkgrenze

Soweit ohne Architekturaufweitung moeglich, bleiben alte Geraetemodelle hinter
einem expliziten Buildprofil als Diagnosewerkzeug erhalten. Dabei gilt:

- `native-port` ist das Produktprofil und der einzige Releasepfad;
- ein historisches Geraeteprofil ist opt-in, nicht installierbar und nicht
  verteilbar;
- Produktartefakte duerfen keine ARM7-Interpreter-, SkyEmu-,
  PVR-Softwarerasterizer- oder Diagnoseinterpreter-Symbole enthalten;
- native Hooks duerfen nicht zur Laufzeit auf historische Geraetemodelle
  zurueckfallen.

KR-5000 bindet diese Grenze an Runtime-ABI `103`, Analyzer-ABI `40`, Backend-
Interface-ABI `21`, Portprojektvertrag `91` und Native-Port-Profilvertrag `14`.
Der aktuelle Export nutzt Native-AOT-Emissionsprofil `33` und
Port-Metadata-Cache-Schema `4`.
Der aktuelle GameProject-Vertrag steht auf `7`/Artefaktformat `6` und
enthaelt weiterhin keine Native-Port-Definition. Das installierte Produkt-SDK
exportiert nur `aot_runtime`, `native_port_runtime` und die explizite native
Produktheader-Allowlist; der
historische Geraetepfad bleibt ein nicht installierbares Buildbaum-Orakel und
ist kein Exportprofil. Das fertige Produktbinary wird per Linkmap auf
Legacy-Runtime-, ARM7-/SkyEmu-, CPU-PVR-/TA- und Interpreterbestandteile
auditiert.

Der SDK-Linkabschluss trennt `port_export.cpp` als nicht installierte
Tooling-Object-Closure vom Analyzer-SDK und schliesst `port_export.hpp` sowie
`native_port_artifact.hpp` aus der Analyzer-Headerinstallation aus.
NativePortDefinition, NativePortArtifact, NativePortContent, NativePortRuntime
und Bootstrap sind implementiert. Read-only Content-Mappings, Hook- und
Hardware-Closure, direkter nativer Dispatch sowie Linkaudit bleiben an die
externe private Contentidentitaet gebunden. Der private Adapter wird erreicht;
statisch rekompilierter Spielcode startet. Der erste nicht aufgeloeste
Plattformzugriff endet typisiert als `UnresolvedHardwareAccess`, ohne
Emulator-, Interpreter- oder Runtimefallback.

Der unabhaengige `NativePortDefinition`-/`NativePortContext`-Vertrag bindet
Originalimage, Bootstrap, direkte Hooksymbole und Hardwareaufloesungen an
SHA-256-Identitaeten. Die Hardware-Closure akzeptiert jede erreichbare
Hardwarestelle nur nach vollstaendig ersetzendem Required-Hook. Eine
deklarierte Native-Memory-Range ist fuer einen unvollstaendig aufgeloesten
effektiven Adresssatz noch kein Beweis und bleibt gesperrt, bis Analyse und
verifizierte Imagematerialisierung die komplette EA-/Zugriffs-/Breitenmenge
gemeinsam binden. Separat materialisierte Module bleiben bis zu ihrem eigenen
Audit fail-closed. Der generierte Native-Produkt-Runner verlangt eine
Executable und einen privaten ContentRoot, validiert beide Pfade und verwendet
den expliziten Bring-up-Schalter nur bei unvollstaendiger Closure. Einen
historischen Guest-Cycle-Budgetpfad oder einen stillen Fallback gibt es nicht.

Der Titelbootstrap laeuft genau einmal nach der verifizierten
Imagematerialisierung und vor jedem rekompilierten Spieleintritt. Er darf das
identity-bound initiale RAM-Abbild und nativen Titelzustand fertigstellen;
weder AOT-Bruecken noch Gastcode sind in dieser Phase verfuegbar. Nach
erfolgreicher Rueckkehr werden Kontextpointer und Stopzustand validiert, erst
dann werden Immutable-Write-Guard und AOT-Bruecken aktiviert. Spaetere
Code-/Read-only-Writes bleiben dadurch weiterhin typisierte Produktfehler.

KR-5001 erzeugt automatisch `metadata/native-hook-requirements.json` als
deterministische Hookanforderungskarte. Function-/Instruction-Replacement ist
nur bei exaktem Grenz-, Eigentuemerschafts-, Entry-, Resume-, Seed-, Guarded-,
Kontext- und CFG-Proof zulaessig; bekannte Hardware- und unbekannte
Instruktionsstellen bleiben hookpflichtig. Die Hardware-Closure ist Schema
`v2`. Gewoehnliche dynamische Speicherzugriffe laufen ausschliesslich ueber
range-geprueften Native Memory und enden ausserhalb typisiert. Der
`GuestInstructionOrigin` bleibt in `MemoryAccessError`, Emitter und
Native-Dispatch auch ohne Tracesink erhalten. Ein moeglicher Diagnosebypass
ohne `NativePortDefinition` ist geschlossen.

KR-5002 bindet native Audio-/Movie-Dienste ohne Dreamcast-Geraetefallback.
PCM-Ausgabe verwendet WinMM; Bytequellen sind hash- und handlegebunden,
reparse-sicher und waehrend des Decodes exklusiv gesperrt. Timestamps, EOS und
bounded Queues bleiben strikt. Der in-process FFmpeg/libav-Provider ist als
LGPL-Shared-Komponente eingebunden, verwendet kein `ffmpeg.exe` und prueft
Header-, ABI- und Lizenzvertrag der installierbaren Fuenf-DLL-Closure. Ein
User-Override wird nicht veraendert; der automatische Cache wird auf diese
Closure begrenzt. Headerloser Sofdec-PS-Inhalt wird nur ueber ein virtuelles,
bounded Praefix fuer den Demuxer erkannt; die Originalquelle bleibt
unveraendert. Der oeffentliche `NativePortMovieSession`-Lebenszyklus reicht
von `Ready` bis `Stopped`.

KR-5003 bindet den nativen GPU-Pfad ausschliesslich an hardwarebeschleunigtes
D3D11. WARP, REF, GDI, CPU-Rasterizer, PVR/TA und historische Geraeteruntime
sind im Produktpfad unzulaessig. Native Vertices, Texturen und Drawstate nutzen
eine GPU-Offscreen-Renderflaeche und Swapchain. Standard ist 1920x1080;
Render-/Outputaufloesung sowie Game-, UI- und Kamera-Viewports und Aspect-
Policies sind getrennte Vertraege. Der native SFD-Lifecycle wurde von `Ready`
ueber `Playing` und `Completed` bis `Stopped` mit 200 dekodierten und 200
GPU-praesentierten Videoframes, 294.016 Audioframes und 114.688.000
GPU-Uploadbytes abgenommen; `hardware_accelerated=true` und kein
PVR/Scanout/Gastframebuffer sind Teil der Evidenz. Oeffentliche Enums,
Budgets, Lebensdauern und Ownerthread-Regeln bleiben fail-closed; Titel werden
vor Besitzkopie auf 1024 Bytes begrenzt, endliche Vertices vollstaendig
geprueft und Resize im offenen Frame an das Renderziel zurueckgebunden.

Die Provider- und Draw-IR-Grenze bleibt backendneutral: native Bildansichten,
Draw-Pakete, Viewports und Renderkonfiguration werden nicht an D3D11-Typen
gekoppelt. D3D11 ist zunaechst das Windows-Backend; Steam-Deck-/Linux-
Unterstuetzung bleibt eine spaetere, nicht aktuelle Prioritaet und ist kein
gegenwaertiges Produkt- oder Alpha-Gate.

Die native Texture-/Font-Foundation ist als begrenzter Unterauftrag innerhalb
von KR-5005 abgeschlossen, nicht als Abschluss des No-Skip-Gates. Der Decoder
deckt SquareTwiddled, SquareTwiddledMipmaps, VQ, VQ-Mipmaps, Rectangle,
SmallVQ und SmallVQ-Mipmaps ab. SmallVQ unterscheidet `427` kompakte Streams
von `52` Compact-Streams mit reserviertem Full-Footprint-Trailer; die
semantische Codebook-Groesse und der Index-Offset bleiben kompakt, und `0`
Faelle sind ambig. Headerlose identity-bound SDK-Fontoberflaechen verwenden
belegtes ARGB1555. D3D11 materialisiert vollstaendige Mip-Subresources
transaktional und exceptionsicher. Es gab keinen Sonic-Produktlauf fuer
diesen Unterauftrag.

KR-5004 bindet native Datei-, Eingabe- und Save-Dienste ohne GD-ROM-, Maple-
oder VMU-Geraetevertrag. `NativePortPlatformServices` liest ausschliesslich
exakt SHA-256-identitaetsgebundene read-only Dateien/Ranges, stellt XInput fuer
vier Gamepads mit Analog/Button/Vibration bereit und speichert atomare,
projekt-/slot-/schema-gebundene Records mit Backup-Recovery und exklusiver
Instanzsperre. Read-only- und Writable-Roots duerfen nicht ueberlappen; IDs,
Windows-Geraetenamen, der kanonische User-Data-Save-Root und die
laengenpraefixierten Digest-Domaenen werden fail-closed validiert. Ein inkompatibles
Primary ist autoritativ, waehrend der Recovery-Store einen guten Backupbestand
bewahrt. Der native Linkaudit enthaelt nur native Plattform-/Runtime-/Media-/
Grafik-TUs und keine historischen Geraetesymbole.

Der vollstaendige originale SFD-Opening-Stream wurde ohne Skip ueber den
nativen FFmpeg-, GPU- und Audiopfad bis EOS abgenommen: `Ready` -> `Playing` ->
`Completed` -> `Stopped`, 3.257 dekodierte und 3.257 GPU-praesentierte
Videoframes, 4.709.760 Audioframes, 3.257 GPU-Presents und `hardware=1`.
Der Codec-Provider-Vertrag steht auf `2`, der Plattformdienstevertrag auf `1`;
PTS-Reorder bleibt begrenzt und der
interleaved Video-Tail ist auf 64 Frames begrenzt.

Der native Produktpfad folgt visuell standardmaessig 1:1 der Dreamcast-
Referenz. Originale Modelle, Texturen, Beleuchtung, Fog, Blend-/Farbsemantik,
Animationen und Framing sind verbindlich; SADX/Steam ist keine visuelle Ground
Truth. 1080p ist die Standardausgabe. 4K, 21:9 und Filter sind spaetere
optionale, togglebare Modi und duerfen den Dreamcast-Fidelity-Modus nicht
veraendern.

Die KR-5002-FFmpeg-Distribution bleibt strikt: Public Packages benoetigen
`FFmpeg-Corresponding-Source.zip` und fuehren DLL, Lizenz, Notice,
Buildkonfiguration und Source einzeln in `runtime-dependencies.json` Schema
`v3`. Ohne vollstaendige Source bleibt `redistribution_ready=false`; die
exakte 2-GB-Quellclosure liegt nicht im Repository, und DEVELOPMENT-ONLY-
Builds sind nicht redistributierbar.

Ein vorheriger KR-5005-Source-Snapshot fuehrte identity-bound Bootstrap-
Materialisierung, echte Post-Bootstrap-AOT-Roots und resumierbare Continuations
durch Analyse, CFG, Optimierung und Export. Post-Bootstrap-Bytes werden vor
Analyse, IR und AOT materialisiert; jedes emittierte Instruktionswort wird
gegen die gebundene Post-Ansicht validiert. Exakte Non-Root-Boundaries,
edge-only JumpTables, image_id-gebundene Metadaten, aktive Overlays,
CallbackTable-Roots, sichere Replacement-Reachability und begrenzte
Callback-Reentrys sind Teil dieser Closure. Der Nested-AOT-Fehlertransport
erhaelt den typisierten Hook-Abbruch bis zum Produktlauf; Linkmap-/PE-Importaudit
und historische CpuState-Bindings bleiben fail-closed. Die Bootstrap-Zeitgrenze
verwendet eine frische monotone Host-Epoche; Acceptance darf erst nach
erfolgreichem Bootstrap und nativer Frame-Presentation bezeugt werden. Der
aktuelle Lauf vervollstaendigte Film `id=0` mit `200` dekodierten, `200`
praesentierten und `200` sichtbar nichtschwarzen Frames sowie `294.016`
Audioframes. Der schwarze/stale-Overlay-Uebergang ist geschlossen; Film
`id=1`/Opening und Hauptmenue bleiben offen. Der aktive P0 ist die Verifikation
der generischen Present-or-Repeat-Grenze fuer den diagnostisch isolierten
Schwarz-Clear bei Frame 190, nicht Bootstrap, AOT oder Linkaudit. Der
typisierte Laufblocker ist der Modell-/Texturpfad mit `0x53414704`. Im
Presented-by-SEGA-Pfad haben Frames `1--189` native Draws; Frame `190` und
`191` wiederholen das letzte abgeschlossene Bild. Der generische
Present-or-Repeat-Vertrag ist bestaetigt; der synthetische Schwarz-Clear ist
geschlossen. Die Hardware-Closure steht aktuell bei `850` Sites (`47`
geschlossen, `803` offen, `129` Owner); ein neuer 9-Slot-/8-Unique-
Callbackvektor fuehrte zu `96` weiteren Exportfunktionen.

Der v59-Export dekodierte `849/849` PRS-Dateien strikt und erzeugte `3.965`
Funktionen in `127` Partitionen. Transformierte Quellidentitaet, decoded
Modulidentitaet, Cache und Source-Map bleiben dabei getrennt gebunden; Loaded-
AOT-Rebase installiert eine Zuordnung erst nach exakter Codeblock-Closure.
Bring-up-Probes sind begrenzt, aber kein Ersatz fuer
vollstaendige Hook-/Hardware-Closure.

## Verbindliche Taskreihenfolge

1. `KR-5000`: Native Produktgrenze und Linkisolation durchsetzen. [x]
2. `KR-5001`: private SH-4-Spiel-/SDK-Grenzen und native Hookbindung
   vollstaendig ableiten. [x]
3. `KR-5002`: Audio-/Moviepfad nativ anbinden und ARM7/AICA aus dem
   Produktprofil entfernen. [x]
4. `KR-5003`: Grafikpfad an eine native GPU-API anbinden und CPU-PVR aus dem
   Produktprofil entfernen. [x]
5. `KR-5004`: Disc, Eingabe, Save und verbleibende Plattformdienste nativ
   anbinden. [x]
6. `KR-5005`: echter No-Skip-Sonic-Lauf bis Hauptmenue mit nativer Bild-,
   Ton- und Eingabekette; erst dieses Gate gibt `v0.50.0 Alpha` frei.

Der erste Implementierungsschritt ist die hoechste verifizierbare Hookgrenze,
nicht ein weiterer Umbau der alten Geraeteemulation. XenonRecomp ist das
Architekturvorbild: statische Recompilation plus portprojektspezifische native
Implementierungen; nicht Flycast oder ein anderer Emulator.

## Produktabnahme

Ein Meilenstein gilt nur, wenn derselbe native Produktpfad alle Punkte erfuellt:

- kein ARM7-Interpreter und kein CPU-PVR im gelinkten Produkt;
- statisch rekompilierter SH-4-Code mit fail-closed nativen Bindungen;
- Opening ohne Skip, Ersatzbild, erzwungenen Status oder privaten Bildhack;
- korrektes Bild ueber die PC-GPU und korrekter Ton ueber das PC-Audiogeraet;
- 60-Hz-PAL-Auswahl und korrekter weiterer Spielzustand;
- Memory-Card-Screen beziehungsweise Hauptmenue mit nativer Eingabe;
- inkrementeller Portbuild fuer normale Iterationen statt historischem
  Vollreexport.

`v0.49.1` bleibt Pre-Alpha, bis dieser rein native Pfad das Hauptmenue
erreicht. Genau dieser Nachweis gibt `v0.50.0 Alpha` frei. Ein Hauptmenue, das
noch ARM7, CPU-PVR oder andere emulierte Dreamcast-Geraete verwendet, erfuellt
das Gate nicht.

Dreamcast-MHz sind ab diesem Architekturwechsel kein Produktgate mehr. Sie
waren eine Kennzahl des emulationsnahen RuntimeOnly-Pfads. Fuer den nativen
Port werden reale Ladezeit, Framezeit/Framerate, Audio-Stabilitaet, CPU-/GPU-
Zeit, Hostauslastung und Eingabelatenz gemessen. Gastzyklen duerfen intern
weiter die Zeitbasis des rekompilierten Spielcodes bilden, sind aber keine
Behauptung, eine Dreamcast-CPU oder -Konsole zu emulieren.

200 MHz nachhaltiger AOT-Durchsatz und 250 MHz unpaced Reserve bleiben
nachgelagerte Optimierungsziele, soweit diese interne Messung fuer den
rekompilierten CPU-Pfad noch sinnvoll ist. Sie blockieren nicht die
Versionsgrenze: `v0.50.0 Alpha` wird durch das native Hauptmenue freigegeben.
