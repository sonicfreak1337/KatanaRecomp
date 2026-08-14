# KatanaRecomp Roadmap

Status: Pre-Alpha

Aktuelle Phase: `v0.49.1` - Sonic-Adventure-Native-Port,
statisches SH-4-AOT und native PC-Plattformdienste

Erster oeffentlicher Release: `v0.50.0` Alpha

## Aktueller Produktmeilenstein

Der v111/v30-Lauf schliesst die echte zweiteilige Introsequenz im nativen
Spielablauf ohne Controllerinput oder Skip. Sonic Team erreichte `200/200`
Videoframes und `294.016` Audioframes; das Opening erreichte `3.257/3.257`
Videoframes und `4.709.760` Audioframes. Der Sequenzabschluss kehrte mit dem
originalen Erfolgsstatus in statisch rekompilierten Titelcode zurueck.

Der aktuelle Export umfasst `5.773` Funktionen in `168` Partitionen. Seine
generische, identitaetsgebundene PRS-Prefix-Entry-Table-Erkennung akzeptiert
nur begrenzte, nullterminierte `3..64`-Entry-Tabellen nach Runtimeextent-,
Decode-, Early-CF-, CFG- und Relocation-Proof; sie bleibt getrennt von
privaten exakten Hints und RuntimeOnly behaelt Stop-on-miss. Die Hardware-
Closure zeigt `1.425` Gaps (`1.373` hook-missing, `51` progress-wait, `1`
root-ownership) bei `1.423` bekannten Sites. Das ist neu sichtbare
Closure-Arbeit, keine Regression. Der 100-s-Produktsmoke erreichte Film 0
`200/200`, nicht aber die dynamische Overlay-Bestaetigung. Danach folgen
Memory-Card-Pfad und Hauptmenue; ein Start-Controllerimpuls ist erst seit dem
vollstaendigen No-Skip-Beleg fuer kuerzere Diagnoselaeufe erlaubt.

Der Windows-Native-Port hat jetzt XInput- sowie WinMM-Controllerabdeckung
fuer DualSense, DualShock und generisches HID. Physische Sony-Controller
haben bei neuer Belegung Prioritaet; stabile Slots, Hotplug und konsistente
Achsen sind Teil des Plattformvertrags `2`, XInput-Vibration bleibt direkt
gebunden. Der Runtime-/CLI-Build ist sauber; die physische DualSense-
Menuabnahme bleibt offen, bis ein Nutzer vor Ort testet.

## Produktziel

KatanaRecomp ist ein statischer SH-4-Recompiler fuer native PC-Ports. Ein
konkretes Spiel wird in einem getrennten, hashgebundenen Recomp-Projekt
gebaut. Die Produktruntime stellt native Hostdienste bereit; sie ist kein
Dreamcast-Emulator.

```text
KatanaRecomp
  -> analysiert SH-4 statisch
  -> erzeugt natives C++/Hostprogramm

Katana Native Runtime
  -> bindet validierte Spiel-/SDK-Grenzen an native PC-APIs
  -> nutzt Host-CPU, GPU, Audio, Dateisystem, Eingabe und Speicher

SonicAdventureRecomp
  -> bindet generierten Spielcode und lokal installierte Originaldaten
  -> erzeugt die startbare Produkt-EXE
```

KatanaRecomp und KatanaRuntime bleiben im selben Repository, sind aber
getrennte Build- und Installationsprodukte. Titeladressen, Titelhooks,
private Symbole und Installationsprofile gehoeren langfristig in das externe
Spielprojekt. Produktbefunde duerfen im Bring-up dokumentiert werden, aber
nie als Sonic-Sonderfall in generischem Runtime- oder Recompilercode landen.

## Projektweiter Arbeitsvertrag

Fuer jeden Task und jeden Projektbereich gilt ab sofort exakt:

```text
Task implementieren
  -> alle durch den Task betroffenen Pfade reviewen
     und bestaetigte Fehler innerhalb dieses Reviews schliessen
  -> den reviewten Task direkt auf main committen und pushen
  -> naechster Task
```

Die Reviewstufe ist die Fehlerfindungs- und Fixstufe. Sie umfasst den
implementierten Pfad, Aufrufer, Verbraucher, Datenfluss, Verdrahtung,
Fehlerpfade, ABI-, Cache-, Versions-, AOT- und Runtimevertraege sowie alle
unmittelbar betroffenen Schichten. Bestaetigte P0-, P1- und andere fuer den
Task relevante Fehler werden vor dem Push geschlossen.

Es gibt keine zusaetzliche standardmaessige Test-, Verifikations-,
Integrations- oder Fixrunde zwischen Review und Push. Tasks werden direkt auf
`main` bearbeitet und veroeffentlicht. Branches, Pull Requests oder
parallele Integrationszweige entstehen nur auf eine neue ausdrueckliche
Nutzeranweisung.

## Sonic ist der Test

Der reale Sonic-Adventure-PAL-Port ist der projektweit massgebliche Produkt-
und Integrationstest:

```text
realer Export
  -> Installation aus der lokalen Originaldisc
  -> normaler Produktlauf
  -> sichtbarer Boot- und Spielfortschritt
```

Daraus folgen verbindlich:

- keine neuen Unit-Tests, Regressionstests, Testmatrizen, synthetischen
  Fixtures, Stresslaeufe, Testprojekte, Ersatzgates oder
  Konformitaetssuiten als Bestandteil eines Tasks;
- Reviews melden das Fehlen neuer Tests nicht als Finding und verlangen keine
  neue Testabdeckung als Abschlussbedingung;
- vorhandene Tests duerfen auf gebrochene Erwartungen, widerspruechliche
  Semantik oder falsche Testzahlen geprueft und bei Bedarf repariert werden,
  ihr Bestand wird fuer neue Tasks aber nicht erweitert;
- ein Task besitzt keinen eigenen Testbuild als Pushgate;
- Sonic-Laeufe erfolgen an den in dieser Roadmap festgelegten Produktgates
  oder nach ausdruecklicher Nutzeranweisung, nicht nach jedem einzelnen Task;
- mehrere zusammenhaengende, reviewte Tasks duerfen vor dem naechsten
  Sonic-Produktlauf auf `main` landen;
- Performance wird am echten End-to-End-Port gemessen, nicht an einer
  synthetischen Matrix oder einer schoenen CPU-Auslastungszahl.

## Unverhandelbare Produktgrenzen

- kein allgemeiner SH-4-Interpreter im normalen Produktport;
- kein JIT;
- kein Emulationsfallback;
- kein ARM7-Interpreter oder zyklusweise Geraetefirmware im Produkt;
- kein CPU-PVR-Softwarerasterizer oder vollstaendiger emulierter Dreamcast-
  Geraeteverbund im Produkt;
- Grafik ueber eine native GPU-API, Audio/Movie ueber native Hostdienste und
  Disc/Eingabe/Save ueber native PC-Plattformdienste;
- Der native Port folgt standardmaessig visuell 1:1 der Dreamcast-Referenz:
  originale Modelle, Texturen, Beleuchtung, Fog, Blend-/Farbsemantik,
  Animationen und Framing sind massgeblich. SADX/Steam ist keine visuelle
  Ground Truth. 1080p ist die Standardausgabe; 4K, 21:9 und Filter sind
  spaetere optionale, togglebare Modi und veraendern den Fidelity-Modus nicht;
- keine stillen No-op-Stubs oder erfundenen Hardwareerfolge;
- keine Sonic-spezifischen Adresshacks im generischen Katana-Kern;
- keine Retail-, BIOS- oder Assetdaten im Repository oder verteilbaren Paket;
- kein aus kommerziellen Dateien kopierter oder ungebunden verteilter Code;
- Flycast und XenonRecomp sind Referenzen, keine Codequellen;
- das echte erzeugte Produkt bleibt die Boot-, Integrations- und
  Performanceabnahme;
- Produktlaeufe werden nach gleicher Gastarbeit verglichen, nicht nach einer
  beliebigen Hostzeit.

Der vollstaendige native Produktvertrag in
[`docs/NATIVE_PORT_PRODUCT_CONTRACT.md`](docs/NATIVE_PORT_PRODUCT_CONTRACT.md)
hat Vorrang vor allen aelteren RuntimeOnly-, AICA-, PVR- und
Performancebeschreibungen.

## Aktueller v0.49.1-Native-Portpfad

Die neue verbindliche Reihenfolge lautet:

```text
KR-5000  Native Produktgrenze und Linkisolation
  -> KR-5001  Statische Spiel-/SDK-Hookkarte
  -> KR-5002  Nativer Audio-/Moviepfad
  -> KR-5003  Nativer GPU-Pfad
  -> KR-5004  Native Disc-, Eingabe- und Save-Dienste
  -> KR-5005  Nativer No-Skip-Sonic-Produktlauf
                rein nativ bis Hauptmenue; v0.50.0 Alpha
```

Die private Adress- und Funktionskarte aus dem Bring-up wird weiterverwendet.
Der erste aktive Implementierungspunkt ist die hoechste belegte SH-4-Spiel-/
SDK-Grenze vor AICA-Kommandoring und PVR/TA-Geraeteprotokoll. Fehlende native
Bindungen enden fail-closed; sie fallen nicht auf Geraeteemulation zurueck.

`KR-5000` ist als physische Source-, Link- und Installgrenze abgeschlossen:
Das Produkt-SDK exportiert nur `aot_runtime` und `native_port_runtime`; der
historische Geraeteverbund ist ein nicht installierbares Buildbaum-Orakel und
kein Portprofil. Portprojektvertrag `91`, Native-Port-Profilvertrag `14` und
der erweiterte Linkmap-Audit sperren jede Rueckkante. NativePortDefinition,
NativePortArtifact, NativePortContent, NativePortRuntime und Bootstrap sowie
read-only Content-Mappings, Hook-/Hardware-Closure und direkter nativer
Dispatch sind implementiert. Ein privater Adapter wird erreicht, statisch
rekompilierter Spielcode startet, und der erste unaufgeloeste Plattformzugriff
endet typisiert als `UnresolvedHardwareAccess` ohne Emulator-/Interpreter- oder
Runtimefallback. Der generierte Runner verlangt Executable plus privaten
ContentRoot und validiert beide Pfade; der Bring-up-Schalter gilt nur bei
unvollstaendiger Closure. KR-5001 und seine Hookkarten-/Closure-Verbindung
sind source-seitig abgeschlossen; KR-5002 ist abgeschlossen.

`KR-5001` ist source-seitig abgeschlossen. Die deterministische
`metadata/native-hook-requirements.json`-Karte verlangt fuer Function- und
Instruction-Replacement exakte Grenzen, Eigentumer und echte Funktionsentry-
Grenzen; interne Resume-Labels bilden keinen Replacement-Beweis. Bekannte Hardware- und unbekannte
Instruktionsstellen bleiben hookpflichtig; dynamische Speicherzugriffe laufen
nur ueber range-geprueften Native Memory und enden ausserhalb typisiert.
`MemoryAccessError`, Emitter und Native-Dispatch transportieren
`GuestInstructionOrigin` auch ohne Tracesink. Die Hardware-Closure steht auf
Schema `v2`; ein Diagnosebypass ohne `NativePortDefinition` ist geschlossen.
`KR-5002` ist source-seitig abgeschlossen. WinMM PCM und
ein in-process LGPL-Shared-FFmpeg/libav-Provider arbeiten ohne Dreamcast-
Geraetefallback. Hash-/handlegebundene, reparse-sichere und waehrend Decode
exklusiv gesperrte Bytequellen, strikte Timestamps/EOS und bounded Queues
bilden den Dienstvertrag. Headerloser Sofdec-PS-Inhalt wird nur ueber ein
bounded virtuelles Praefix fuer den Demuxer erkannt; `Ready` bis `Stopped` ist
der oeffentliche `NativePortMovieSession`-Lebenszyklus. Der relevante
24-Worker-Inkrementalbuild war in etwa `4,5 s` erfolgreich. `KR-5003` ist
source- und produktseitig abgeschlossen: Der native GPU-Pfad ist hardware-only
D3D11 ohne WARP/REF/GDI/CPU-Rasterizer, PVR/TA oder historische Geraeteruntime.
Native Vertices, Texturen und Drawstate nutzen GPU-Offscreen-Renderflaeche und
Swapchain; Standard ist 1920x1080, Render-/Outputaufloesung sowie Game-, UI-
und Kamera-Viewports/Aspect-Policies sind getrennt. Die native SFD-Abnahme
durchlief `Ready` -> `Playing` -> `Completed` -> `Stopped` mit 200 dekodierten
und 200 GPU-praesentierten Videoframes, 294.016 dekodierten Audioframes,
114.688.000 GPU-Uploadbytes und `hardware_accelerated=true`; PVR/Scanout/
Gastframebuffer waren nicht beteiligt. Der inkrementelle 24-Worker-Targetbuild
war in `4,9 s` erfolgreich. `KR-5004` ist source- und produktseitig
abgeschlossen: Native Plattformdienste binden exakt identitaetsgebundene
read-only Content-Ranges, XInput fuer vier Gamepads und atomare projekt-,
Slot- und schema-gebundene Saves mit Backup-Recovery. Read-only-/Writable-
Roots, sichere IDs, User-Data-Save-Root und Digest-Domaenen bleiben
fail-closed; der Linkaudit enthaelt keine historischen Geraetesymbole. Der
vollstaendige originale SFD-Opening-Stream lief ohne Skip bis EOS und endete
`Completed` mit 3.257 dekodierten und 3.257 GPU-praesentierten Videoframes,
4.709.760 Audioframes und 3.257 GPU-Presents.
Der Texture-/Font-Foundation-Unterauftrag innerhalb von KR-5005 ist
source-seitig abgeschlossen: Sieben Layouts einschliesslich Mipmap- und
SmallVQ-Varianten werden dekodiert; `588/588` PVM-Archive und
`16.725/16.725` Texturen sind abgedeckt, darunter `12.704` mipmapped
Texturen, `73.817` untere Mip-Level und `668.876.160` RGBA-Bytes. SmallVQ
umfasst `427` kompakte Streams und `52` Compact-Streams mit Full-Footprint-
Trailer; die `52` Trailer sind reserviert, ohne den kompakten Index-Offset zu
verschieben, und `0` Faelle sind ambig. Headerlose identity-bound
SDK-Fontoberflaechen belegen ARGB1555. Dieser Unterauftrag schliesst KR-5005
nicht insgesamt. Die anschliessende Grafik-Foundation ist fuer den aktuell
erreichten Produktpfad source-seitig weitgehend implementiert, produktseitig
aber wieder offen: NINJA-Modellpunkte und native
Drawstates werden backendneutral uebergeben; D3D11 uebernimmt homogenes
Clipping, perspektivische Interpolation und reziproke Depth-/Fog-Semantik.
Der reale Lauf passiert die frueheren Texture- und Mixed-Clip-Stops ohne
TA-/QACR-Reentry. Die danach beobachtete Registrar-/Objektfeld-Callback-Kette
ist jetzt generisch statisch inventarisiert und als AOT-Closure gebunden.
KR-5005 bleibt offen, weil der Lauf anschliessend eine noch nicht vollstaendig
ersetzte Host-Timing-Unterfamilie erreicht. Zuvor ist jedoch die aktuelle
vollstaendige Schwarzbildregression zu schliessen: Audio und Titelablauf
laufen, interne Draw-/Presentzaehler steigen, aber im Fenster erscheint vom
ersten SEGA-Bild an kein Bild. Steam-Deck-/Linux-Unterstuetzung
ist spaeter geplant, aktuell nicht priorisiert und kein gegenwaertiges Gate.

KR-5005 verwendet ab Portprojektvertrag `91` einen echten schnellen
Bring-up-Hostbuild: nur die grossen generierten AOT-TUs laufen mit `/Od /Ob0`
und einem gemessenen Vierer-Ninja-Pool; eine gemeinsame MSVC-PDB ist
ausgeschlossen, und 4.096 Dispatch-Eintraege pro Shard vermeiden Hunderte
triviale Compiler-/Linkobjekte. Runtime, Titeladapter und Bootstrap bleiben
optimiert. Der finale `gate`-Build bleibt davon getrennt voll optimiert.
Der reale Sonic-Mikrovergleich sank fuer vier identische TUs von `16,542 s`
mit `/O1` auf `4,560 s` mit `/Od`. Der anschliessende kalte Vollport schrieb
233 Host-TUs, beendete Export und Packaging in `408,278 s` bei `337,205 s`
Hostbuildzeit und bestand den Post-Link-Audit. Die damalige offene Post-
Overlay-Callback-/Function-Pointer-Kante ist im v74-Stand geschlossen; die
vollstaendige sichtbare Produktabnahme bleibt wegen der nachfolgenden nativen
Host-Timing-Grenze aus.

Der erste Lauf dieses Binaries lokalisierte zudem eine generische
Bootstrapluecke: identity-bound Titel-RAM musste vor den Laufzeit-
Immutable-Guards materialisiert werden, AOT-Bruecken durften dabei aber noch
nicht aktiv sein. Portprojektvertrag `91` schliesst diese Reihenfolge
fail-closed; nach erfolgreichem Bootstrap beginnt erst der ueberwachte
statische AOT-Lauf.

Der vorherige Source-Snapshot erweiterte diesen KR-5005-Pfad um echte
Post-Bootstrap-AOT-Roots und resumierbare Continuations sowie die
Dispatchability-Weitergabe durch CFG und Optimierung. Die Closure- und
Nested-AOT-Fehlertransport-Pruefungen sind abgeschlossen; die spaetere
Registrar-/Objektfeld-Callback-Kante ist ebenfalls geschlossen. Das
Produktgate bleibt am nachfolgenden Host-Timing-Vertrag offen. Provider- und
Draw-IR bleiben backendneutral; D3D11 ist zunaechst
das Windows-Backend, waehrend Steam-Deck-/Linux-Unterstuetzung spaeter und
nicht als aktuelle Prioritaet vorgesehen ist.

Ein frueherer Produktbeleg vervollstaendigte Film `id=0` mit `200`
dekodierten und `200` praesentierten Videoframes, `294.016` Audioframes und
`200` intern als nichtschwarz klassifizierten Frames. Der aktuelle v74-
Direktlauf bleibt real vollstaendig schwarz; die gemeinsame Offscreen-/
Compose-/Swapchain-Grenze ist deshalb offen. Der fruehere schwarze/stale-
Overlay-Uebergang war
geschlossen. Der Lauf materialisiert danach Stage-Content, sechs dynamische
Oberflaechen, Texturen und ein natives Modell, passiert die anschliessende
gespeicherte Callback-Kette und endet erst an einer offenen Host-Timing-
Unterfunktion. Film `id=1`/Opening und Hauptmenue bleiben offen. Im
Presented-by-SEGA-Pfad haben Frames `1--189`
native Draws; Frame `190` und `191` wiederholen bei geschlossenem GPU-Frame
das letzte abgeschlossene Bild. Der Present-or-Repeat-Vertrag ist bestaetigt;
der synthetische Schwarz-Clear ist geschlossen.

Der validierte v59-Export untersuchte `1.094` Dateien, dekodierte `849/849`
PRS-Dateien strikt und erzeugte `3.965` Funktionen in `127` Partitionen.
`488` rohe und `395` guarded Callback-Kandidaten sowie `39` Latent-AOT-
Kandidaten wurden ohne Truncation oder Budgeterschoepfung erfasst. Die
Closure-Gaps stiegen von `257` auf `304`, weil der Export mehr echte
erreichbare Funktionen einbezog; dies ist keine Hardware-Regression. Der
aktuelle Hardware-Closure-Stand umfasst `850` Sites, `47` geschlossen, `803`
offen und `129` Owner. Ein neuer 9-Slot-/8-Unique-Callbackvektor fuehrte zu
`96` weiteren Exportfunktionen.

Der warme v72-Export erzeugte `5.103` Funktionen in `149` Partitionen und
`203` Host-TUs in `24,356 s`. Analyse, IR und Metadaten waren Cachetreffer,
Codegen erreichte `149/149` Treffer und der Hostbuild `200/203` Objekttreffer.
Gegenueber dem identischen kalten v71-Export mit `422,637 s` ist das etwa
`17,4x` schneller. Die Closurezahl bleibt bis zum vollstaendigen statischen
Replacement-Reachability-Beweis unveraendert.

Der v74-Export bindet externe erreichbare CFG-Bloecke als lokale
Analyseowner, verfolgt statische Codepointer durch Registrar und Objektfeld
und verwendet GameProject-Funktionswissen nur noch als Non-Root-Hint. Er
erzeugte `5.316` Funktionen in `158` Partitionen und `213` Host-TUs.
Gegenueber v73 sind das `+111` Funktionen/`+2` Partitionen, gegenueber v72
`+213`/`+9`. Das Inventar stieg auf `2.029` rohe und `618` guarded Callback-
Kandidaten; `326.461/4.194.304` Shape-Arbeitseinheiten blieben ohne
Truncation oder Budgetende. Der direkte Produktlauf passiert den frueheren
Callback-Endpunkt. Die Hardware-Closure bleibt bei `850` Sites, `47`
geschlossen, `803` offen und `129` Owner, da kein Hardwareprovider vorzeitig
als ersetzt gilt.

Der reviewte v87-Export bindet latente Module ueber `3.828`
Blockidentitaeten, `107` Funktionsidentitaeten, `4.222` externe Codepointer
und `290` Cross-Image-Transfers. Zwei beschreibbare relative Switchtabellen
erhalten bounded `guarded-owner-extent`-Evidenz, ohne als vollstaendiger CFG
oder Laufzeitzielsatz zu gelten. Actionable Whole-Function-Kandidaten stiegen
`116 -> 118`, fehlende exakte Grenzen sanken `16 -> 14`; der private
Disassembly-Abgleich lieferte keinen weiteren exakten Import. Die erweiterte
Auditclosure umfasst `909` Sites in `136` Ownern (`50` geschlossen, `859`
offen). Der warme Export dauerte `117,044 s` mit `155/155` Codegentreffern
und `2,485 s` Hostbuild.

Der aktuelle Produktlauf bestaetigt das SEGA-Bild ohne die zuvor sichtbaren
horizontalen Naehte, schliesst Film `id=0` ab, laedt Stage-Overlay sowie
Settings-/Camera-Assets und erreicht den ersten umfangreichen 3D-Frame. Der
Stillstand liegt dort innerhalb einer wiederholten Modell-/Polygon-
Submission. Als naechstes wird deshalb die gesamte zugehoerige Grafik-/
Transfer-Ownerfamilie samt Callbackkanten und Seiteneffekten geschlossen;
ein einzelner Stall-PC ist kein Providervertrag.

## Historischer RuntimeOnly-Bring-up

Der historische CLI-Modus `port --analysis-mode runtime-only` war nur mit
`--game-project` freigegeben. Er ist jetzt internes Diagnoseorakel und kein
Produkt-/Releaseprofil. Fuer seine Bootanalyse setzt RuntimeOnly konservativ
`GuestCallAbi::Unknown`, ueberspringt die blockierende SuperHC-
FunctionValue-/Candidate-Resolution und erzeugt weiterhin nativen AOT-Code.
RuntimeOnly-Dispatch verwendet eine exakte statische Guest->Host-Tabelle;
Stop-on-miss und typed abort bleiben aktiv, ohne Interpreter, JIT,
Runtime-Decoder oder geratenen Zielpfad. Der Whole-Export-Cache ist
modegebunden.

Der letzte historische RuntimeOnly-Lauf erreichte ohne Skip, Start-Impuls oder
kuenstlichen Moviepfad das Milestone `FirstVisibleGameFrame`. Die begrenzte
Bild- und Audiopublikation war nachweisbar; private Frame-, Audio- und
Ausfuehrungszaehler sind nicht Teil der Roadmap. Das Memory-Card-Gate und das
Hauptmenue bleiben offen. Der Default-PlatformAbi-Pfad bleibt unveraendert.

## Historischer RuntimeOnly-Geraetestand

Der vorherige bereinigte Runtime-/Codegen-Checkpoint hob Runtime-ABI `87` auf
`88` und PlatformServices-ABI `13` auf `14`. `efc531b` hebt Runtime-ABI wegen
der vollstaendigen PVR-Completion- und TA-Metrikvertraege weiter auf `89`;
`e1d8ade` hebt ihn wegen der oeffentlichen AICA-/ARM7-Fortsetzung weiter auf
`90`; Backend-Interface-ABI `13` und PVR-State-Contract `3` gehoeren zu
diesem historischen Geraetestand.

Historische Diagnosevergleiche und begrenzte PVR-/Audioevidenz bleiben von
den aktuellen Produktgates getrennt. Private Frame-, Audio- und
Ausfuehrungsidentitaeten werden nicht in der Roadmap geführt. Serielle
Runtime-/Dispatch-Kosten und der post-filmische private Identity-Miss sind
historische Diagnosebefunde, aber keine aktiven Produktgates des nativen
Pfads.

## Historischer Candidate-Evidenzstand

Die folgenden Staende bleiben getrennt:

```text
letzte reale Produktevidenz:
  historische NativeDisc-/DirectBoot-Ports mit aelteren ABI-Vertraegen

aktueller funktionaler Source-Stand:
  aktueller Native-Port-Architekturreview-Checkpoint
  Runtime-ABI 106, PlatformServices-ABI 14, Backend-Interface-ABI 23,
  PVR-State-Contract 3, Portprojektvertrag 97, Native-Port-Profilvertrag 20
  Analyzer-ABI 48
  Function-Analysis-Epoch-Schema 28
  lokales In-Process-Evaluation-Cache-Schema 13
  Native-AOT-Emissionsprofil 36, AOT-Partitionsschema 7,
  Port-Metadata-Cache-Schema 8

Der SDK-Reviewabschluss hält `port_export.cpp` in einer separaten, nicht
installierten Tooling-Object-Closure und schliesst `port_export.hpp` sowie
`native_port_artifact.hpp` aus der Analyzer-SDK-Headerinstallation aus.

frueherer Vergleichslauf:
  D-Lauf, 460,6 s gesamt; Candidate Resolution ca. 325,8 s
  manuelles Beenden des identifizierten Kindprozesses nach belegter Nichtverbesserung
  0/1194 Resolution-Roots committed, HOL 0, Wave 103
  272 Contexts, 1.044 Semantic-Lanes, 1.029 contextual physical evaluations
  2.430 contextual logical requests, 1.359 Input-Widening-, 29 Summary- und
  733 stale-Dependency-Requeues, 1.359 stale snapshot discards
  518.425.788 B Cache-Payload, 3.964 physische Auswertungen gesamt
  0/0 publizierte/verwarfene Epochen, kein Portartefakt, keine game.exe
```

Der aktuelle Lauf nach dem Candidate-Domain-Top-Fix (`kr4981-20260809-020628-2bfd8af5`)
endete nach `343,627 s` durch manuellen Abbruch bei belegter identischer
Nichtkonvergenz. Die Voranalyse bis zum Candidate-Start dauerte etwa `146 s`
einschliesslich des Gesamtstarts; die letzte Bewegung war Wave `48`. Peak Root
lag bei `1.450.078.208 B`, Peak Job bei `1.618.132.992 B`; es gab keine
kanonische Publikation und kein Portartefakt. Bei Wave `39` waren die 16
geprueften Kernzaehler exakt identisch zum Vorlauf (darunter Frontier `177`,
Contexts `272`, Semantic-Lanes `606`, physische Auswertungen `645`, exakte
Subscriber `870`, Provenienz `21.355`, Input-Widening `263`, Summary `10`,
Forward `123`, stale `95`, stale Discards `299`, semantische Widenings `553`
und provenance-only `382`). Der Top-Fix ist damit ein Korrektheits- und
Persistenzfix, kein belegter Konvergenzhebel; KR-4981 bleibt offen.

Der abgeschlossene Diagnose-Unterauftrag im Lauf
`kr4981-20260809-024141-c4ffdf15` erreichte das vollständige
`attempts=1024`-Gate und wurde nach `244,549 s` bei Wave `24` gezielt beendet;
der daraus erwartbare Supervisorstatus `product-exit -1` ist kein Fehler- oder
Hängerbefund. Peak Root lag bei `1.260.388.352 B`, Peak Job bei
`1.387.151.360 B`; es gab keine Publikation und kein `game.exe`.
`uncategorized=0` galt für alle Top-8-Funktionen. Bei der dominanten Hot-Callee waren alle
`20` semantischen Änderungen und `40` Stack-Widenings ausschließlich
SavedEpoch-pending-ABI-Skalare; der Stackvertrag war am Callee-Set unvollständig.
Eine zweite geprüfte Hot-Callee zeigte `28` Änderungen mit gemischten Domänen und ebenfalls
unvollständigem Callee-Set; eine vollständige-stackvertragliche Hot-Callee zeigte `48` Änderungen bei vollständigem
Stackvertrag, darunter `reg_epoch_pending=180`. Der Diagnose-Unterauftrag ist
damit abgeschlossen, KR-4981 und das globale Sonic-Produktgate bleiben offen.
Der SavedEpoch-Lifecycle-Fix ist source-seitig abgeschlossen. Offen bleibt die
gemeinsame Ordinary-/Registermetadaten-/Alias-/Watcher-/Loss-/MemoryEpoch-
Lifecycle-Ursache; Alias-/Current-Tracking und fail-closed Restore bleiben
erhalten.

Der SavedEpoch-Lifecycle-Unterauftrag ist source-seitig abgeschlossen. Current-
tracking SavedEpoch-Pending-ABI-Skalare werden nur an bewiesenen normalen
Call-/Tail-ABI-Gates konsumiert; detached Epochs bleiben unangetastet.
`candidate_payload_lost` ist ein physisch und semantisch absorbierendes
Epoch-Top ueber Normalize, Merge, Equality, Key, Subsumption, Evidence,
Restore und Persistenz. Konkrete Evidence sowie Nested-/Current-Aliasfakten
bleiben erhalten, finite Payload/Slots verschwinden; detached Top uebernimmt
keine fremde Tail-Evidence. Der historische SavedEpoch-Lifecycle-Stand lief
mit Epoch-Schema `17` und Analyzer-ABI `33`.

Der Lauf `kr4981-20260809-031826-0616113a` endete nach `369,171 s`
(`6:09`) mit Status `nonconvergence`, Exitcode `31`, nach drei zehnsekundigen
Null-Publikations-Amplifikationssamples. Wave `76`, `0` committed/ready/
completed Roots, `272` Contexts; Semantic-Lanes `846 -> 863 -> 886`,
physische Auswertungen `1.135 -> 1.164 -> 1.213`, Frontier `101 -> 88 -> 131`
und stale Discards `395 -> 396 -> 415`. Peak Root lag bei `1.663.037.440 B`,
Peak Job bei `1.895.583.744 B`; keine Publikation und kein `game.exe`.
D1024 und D2048 hatten `uncategorized=0`. Der alte SavedEpoch-Pending-Blocker
ist damit zielgenau beseitigt; der naechste Root-Analysepunkt ist die gemeinsame
Ordinary-/Registermetadaten-/Alias-/Watcher-/Loss- und MemoryEpoch-Lifecycle-
Ursache, nicht ein weiterer SavedEpoch-Pending-Patch. KR-4981 bleibt fail-closed
offen.

Der historische Candidate-Resolution-Source-Stand ist `49b0f72a9f49d60a4eb6e0481460cd57c5625735`.
Er behaelt retained sticky loss in der strukturellen Contextual-Hybrid-Projektion;
die autoritative Hybridprojektion schliesst Contextual-MAY-Joins und Forward-
Edges erneut vollstaendig.
erkennt SavedEpoch-Slot-Pending-Top in allen Truncation-/Publication-Checks
fail-closed und trennt Provenance-Replay-Capsule-/Keybyte-Limits vom
semantischen Evaluation-Limit. Der echte Evaluation-Cap erhoeht nur den
Evaluation-Zaehler; im historischen Stand waren Analyzer-ABI `34`,
Epoch-Schema `27` und lokales In-Process-Evaluation-Cache-Schema `13` aktiv.

Der historische PlatformAbi-Produktlauf `kr4981-20260809-091410-2766aaa6` endete nach ca.
`275 s` gesamt (Candidate ca. `221 s`) mit `nonconvergence` nach drei
Amplifikationssamples: `0/1274` Roots, HOL `0`, Wave `107`, `280` Contexts,
`970` Semantic-Lanes, `1.861` physische, `2.526` logische Requests,
Input-Widening `536`, Summary `22`, Forward `123`, stale Requeues `272`,
stale Discards `806`, Cache `589.178.706 B`; keine Budgets erschöpft, keine
Publikation und kein Artefakt. Admission `1024/1024`, projected context/match
jeweils `0`; der sauberste Ordinary-Stack-Treiber blieb bei `84/84` Attempts/Semantic Changes und
`508` Ordinary-Stack-Deltas trotz vollständigem Stackvertrag. Der Supervisor
schrieb wegen `taskkill`-Zugriffsverweigerung keine Summary; der Kill-on-close-
Job beendete den Child trotzdem.

Der vorherige Produktlauf `kr4981-20260809-083308-4a3ff9be` endete nach
`286,387 s` (Candidate ca. `232,5 s`) mit `nonconvergence`/Exit `31` nach
drei Amplifikationssamples: `0/1274` Roots, Wave `119`, keine Publikation,
`280` Contexts, `972` Lanes, `2.011` physische, `2.814` logische, `203`
Cache-Reuses, `2.790` Subscriber, Provenienz `169.824`, stale Discards `922`,
Frontier `43` (max `250`), Cache `610.295.241 B`; kein Artefakt. Admission
`1024/1024`, projected context/match jeweils `0`. Der P0 liegt intra-context
bei Ordinary-Stack und lokalen Stackkoordinaten.

Der vorherige Lauf `kr4981-20260809-041945-21b70ade` endete nach `425,924 s`
(Candidate ca. `341 s`) bei Wave `60`, `0/1194` Roots, `758` Lanes, `984`
physischen und `1.398` logischen Auswertungen, `248` Input-, `102` stale-
Requeues und `347` Discards; Peak Root `1.606.066.176 B`, Peak Job
`1.814.822.912 B`, kein Artefakt.

Der Vergleichslauf `kr4981-20260809-050420-3f47fd65` wurde nach `322,632 s`
(Candidate `237,116 s`) wegen Nichtverbesserung beendet: Wave `39`, `0/1194`,
`272` Contexts, `549` Lanes, `630` physische, `894` logische Auswertungen,
`181` Input-, `10` Summary-, `76` stale-Requeues, `226` Discards,
Provenienz `31.713`, Cache `455.638.275 B`, maximale physische Dauer
`42,359 s`; kein `game.exe`. Gegenüber `9baea88` blieb das `attempts=1024`-
Gate bitgleich (`admission_success=999`, projected changed/match jeweils `0`);
die Gateänderung ist korrekt, aber kein Konvergenzhebel. Der offene P0 ist
Historisch wurde Inventory-Provenance-Live-in/Spill-through als P0 vermutet;
der historische PlatformAbi-P0 ist die fehlende Wirksamkeit der autoritativen Hybrid-Join-
Closure beim vollstaendigen Stackvertrag/Gate. KR-4981 bleibt offen.

Historische Ports belegen keinen aktuellen Sourcezustand. Der v56-Lauf ist
Diagnoseevidenz und kein Produktnachweis, weil kein Produkt entstand.

Der gemeinsame Candidate-Resolution-Explosionsfix erfuellt KR-4985 und
KR-4986 source-seitig: Full-State-Semantic-Lanes sind kollisionssicher,
exakte Provenienzabonnenten werden privat replayt, und das Budget wird nur bei
neuer semantischer Lane belastet. Die D1-Telemetrie ist explizit opt-in und
bleibt ohne Detailtelemetrie vollstaendig aus dem Progress-Hotpath.

Der einzige freigegebene D1-Lauf lieferte valide nichtterminale Root-0-
Transport- und Fortschrittsevidenz, erreichte aber weder den historisch
limitierenden Root 1 noch einen vollstaendigen schweren Root. Nach einem
privaten Supervisor-I/O-Fehler war die temporaere JSONL bis `185,586 s`
lesbar/gespuelt, aber ohne terminalen Datensatz und ohne atomare Publikation.
D1/G1 ist daher strikt fail-closed und unentschieden; KR-4988 bis KR-4991
bleiben inaktiv.

## Historischer RuntimeOnly-P0 bis FirstVisibleGameFrame

Der No-Skip-Sicht- und Audiopfad erreichte historisch
`FirstVisibleGameFrame`. Die aktive Produktreihenfolge ist deshalb: den
vollstaendigen Sicht-/Audiopfad bis zum Memory-Card-Screen und Hauptmenue
absichern, danach den post-filmischen AOT-Identity-Blocker schliessen.
KR-4981 bleibt historisch und bis zu diesen sichtbaren Produktgates offen.
ARM7-Ausfuehrung, AICA-Interrupt-/Monitor-Lifecycle, Sofdec-Audiotakt und
Movie-Bildpublikation laufen. Der nachgelagerte funktionale Blocker ist der
nachgelagerte private Identity-Miss (`byte-identity-mismatch`).

Der Candidate-Resolution-P0 bleibt als historische PlatformAbi-Diagnostik
dokumentiert. Null Eviction-Recomputes liefern keinen Beleg fuer Cache-Eviction
als Hauptursache; der gemeinsame Source-Fix trennt weiterhin semantische
Full-State-Lanes von exakter Provenienz.

```text
echte semantische Contextmenge
  x
Kosten je Context
  x
ueberwiegend serieller kritischer Scheduling-Span
```

Die v56-Zahlen wurden in unterschiedlichen Zaehldomaenen ausgegeben. Das
Per-Function-Budget von `65.536` darf daher weder von den laufweiten `27.872`
physischen Auswertungen subtrahiert noch durch die laufweiten `25.728`
Contexts dividiert werden. Diese Werte bleiben historische Evidenz und
werden nicht als gemeinsamer Root behauptet.

Eine blosse Erhoehung des 65.536er-Budgets, mehr Cache oder mehr Threads ist
kein Fix. Der historische D-Lauf zeigt gegenueber dem vorherigen Fehlerlauf bei
gleicher Gesamtzeit (~459,6 s) unterschiedliche Rohwerte (`wave 103` statt
`67`, `1.044` statt `722` Lanes, `1.029` statt `713` contextual physical
evaluations, `733` statt `839` stale requeues und `518.425.788` statt
`444.266.838` B Cache-Payload); diese historische Gegenüberstellung belegt
keine materielle Produkt-/Performanceverbesserung und keinen Konvergenzhebel.
Bei
Attempts `1024`, `2048` und `4096` waren die relevanten
Admission-/Stack-Diagnosezaehler bitgenau identisch. Candidate-Resolution
bleibt deshalb offen; KR-4981 ist nicht bestanden.

Der Detailvertrag steht in
[`docs/P0_ROOT0_CANDIDATE_RESOLUTION_PERFORMANCE.md`](docs/P0_ROOT0_CANDIDATE_RESOLUTION_PERFORMANCE.md),
der uebergeordnete Kaltbuildvertrag in
[`docs/P0_NATIVE_DISC_COLD_BUILD_PERFORMANCE.md`](docs/P0_NATIVE_DISC_COLD_BUILD_PERFORMANCE.md).

## Historischer RuntimeOnly-Taskpfad

| ID | Aufgabe | Ergebnis |
|---|---|---|
| KR-4985 | Candidate-Resolution-Phasen- und Kardinalitaetstelemetrie | [x] source-seitig abgeschlossen; produktive D1-Telemetrie explizit opt-in, Produktgate wegen unvollstaendigem Lauf unentschieden |
| KR-4986 | Semantische Context-Lanes und exakte Provenienzabonnenten | [x] source-seitig abgeschlossen; Full-State-Semantik und exakte Contribution-/Evidence-Provenienz getrennt |
| KR-4987 | Read-Lens-projizierte Context-Identitaet | [x] source-seitig abgeschlossen; vollstaendige Key-Bytes, konservativer FullState-Fallback und exakte Provenienz/Restore; D9 beendet fail-closed, kein Erfolg behauptet |
| KR-4988 | Internierte AbstractStates und Summaries | nur bei positivem Kostengate werden unveraenderliche States/Summaries kanonisch wiederverwendet |
| KR-4989 | Indexierte exakte Context-Bindings | nur bei positivem Kostengate vermeiden exakte Treffer den linearen Scan |
| KR-4990 | Inkrementelle Contextual-Dependency-Views | nur bei positivem Kosten-/Reusegate werden unveraenderte View-Shards behalten |
| KR-4991 | Versionierte monotone Context-Worklist | nur bei positivem G2 startet kausal freigesetzte Arbeit ohne globale Jacobi-Barriere |
| KR-4993 | Abschlussreview der Candidate-Resolution-Pfade | [x] vollstaendiger Source-Endreview wiederverwendet; das Analyzer-ABI-Finding ist mit dem SDK-Linkabschluss unter dem aktuellen Analyzer-ABI 36 geschlossen, Produktlimits bleiben KR-4981 vorbehalten |
| KR-4981 | Einmaliges Sonic-Produktzeitgate | historisches RuntimeOnly-Gate; `FirstVisibleGameFrame` historisch erreicht, durch das native Alpha-Gate KR-5005 abgeloest |
| KR-4992 | Begrenzte Spekulation spaeterer Roots | nur nach einem verfehlten KR-4981 und positivem Restkosten-/RAM-Gate |
| KR-4994 | Begrenzter identitaetserhaltender unresolved Stack-/Context-Candidate-Carrier | [x] source-seitig abgeschlossen; begrenzter Pending-Carrier plus kanonisches absorbierendes Top fuer abgeschnittene Candidate-Domains ueber Merge/Normalisierung/Key/Persistenz/ABI-Promotion und Harvest; der Hybrid-Join-Befund bleibt historisch auf dem PlatformAbi-Pfad |
| KR-4995 | AICA-ARM7-Ausfuehrung und Sound-Interrupt-Lifecycle | [x] in `e1d8ade` source-seitig abgeschlossen und mit Sonic produktseitig durch fortschreitenden Sofdec-Audiotakt, Readiness 1, Player-Status 5 und sichtbare Movie-Bildpublikation belegt |

Die Reihenfolge ist normativ:

```text
KR-4985/KR-4986/KR-4993/KR-4987/KR-4994/KR-4995 source-seitig abgeschlossen
  -> RuntimeOnly-Build-/Export-Gate bestanden
  -> No-Skip-Sonic-Audio-/Videopfad historisch bis FirstVisibleGameFrame
  -> weitere RuntimeOnly-Performancemarken bleiben historische Diagnostik
  -> post-filmischen privaten Identity-Miss schliessen
  -> beaufsichtigter Start bis mindestens Memory-Card-Screen/Hauptmenue
  -> PlatformAbi-Candidate-Resolution bleibt deferred und ist kein RuntimeOnly-
     Buildblocker
```

D1 und D2 sind reale, begrenzte Sonic-Diagnoseexporte, keine neue Testmatrix.
D1/G1 bleibt historisch unentschieden; D2/G2 ist abgeschlossen und negativ:
kein positiver Schedulerhebel. D9 ist historisch beendet und Root 0
konvergierte fail-closed ohne Portartefakt oder Produkterfolg. KR-4988 bis
KR-4991 bleiben inaktiv. KR-4994 und KR-4995 sind source-seitig abgeschlossen; die
PlatformAbi-Optimierungsbefunde bleiben deferred. KR-4981 ist historische
RuntimeOnly-Evidenz und durch KR-5005 abgeloest. KR-4982 und KR-4983 bleiben
als alte optionale Offload-Aufgaben gestrichen.

## v0.49.1-Kritischer Pfad

1. **[x] Native Produktlinkgrenze**
   - eigenes `native-port`-Produktprofil;
   - kein ARM7-Interpreter, CPU-PVR, Diagnoseinterpreter oder vollstaendiges
     Geraetemodell im Produktlink;
   - fehlende native Bindungen enden typisiert und fail-closed.

2. **[ ] Statische Spiel-/SDK-Hookkarte**
   - private Bring-up-Adressen werden zu Funktions- und Datengrenzen
     zusammengefuehrt;
   - Audio/Movie vor dem AICA-Kommandoring, Grafik vor dem PVR-
     Geraeteprotokoll und Plattformdienste an ihren Bibliotheksgrenzen;
   - Titelbindung bleibt im externen privaten Spielprojekt.

3. **Native Hostimplementierungen**
   - Audio/Movie ueber native Decoder-, Mixer- und Ausgabedienste;
   - Grafik ueber eine native GPU-API;
   - Disc, Eingabe und Save ueber native PC-Plattformdienste;
   - keine Skips, erfundenen Statuswerte oder Ersatzframes.

4. **Externes Spielprojekt und inkrementeller Build**
   - generierter Titelcode und Originaldaten bleiben lokal;
   - Hook-/Runtimeaenderungen vermeiden unnoetigen SH-4-Neuexport;
   - der historische Exporter ist keine produktive Buildoption.

5. **Xenon-artiger nativer Hotpath**
   - statisches AOT und direkte native Calls ueber bewiesene Grenzen;
   - native GPU-/Audiowarteschlangen statt emulierter Geraeteschritte;
   - reale Framezeit, Audio-Stabilitaet und Hostauslastung sind die primaeren
     Produktmetriken.

## Produktmeilensteine

### B0 - Game Entry korrekt

- Haupt-Executable aus der eigenen GDI identifiziert und hashgebunden;
- statisch rekompilierter Einstieg ohne Interpreter/JIT;
- native Hooktabelle ist identitaetsgebunden und vollstaendig.

### B1 - Nativer Audio-/Moviepfad

- mwSnd-/CRI-/ADXT-/Sofdec-Grenzen rufen native Hostdienste;
- Opening laeuft ohne Skip und mit echtem Ton;
- kein ARM7-Interpreter oder AICA-Firmwarepfad ist gelinkt.

### B2 - Nativer GPU-Pfad

- echte Spielrenderarbeit wird von der PC-GPU ausgefuehrt und praesentiert;
- kein CPU-PVR-Softwarerasterizer ist gelinkt;
- ein uniformer Clear- oder Fehlerframe gilt nicht als Spielfortschritt.

### B3 - Native Echtzeit

- stabiles 60-Hz-Framepacing ohne Audio-Unterlaeufe;
- reale Framezeit, CPU-/GPU-Zeit, Hostauslastung und Eingabelatenz sind
  innerhalb des Produktbudgets;
- billige Produktdiagnose verwendet denselben nativen Pfad.

### B4 - Titelbild und Eingabe

- Titelbild oder erster interaktiver Spielscreen;
- Controller im real gestarteten Spiel;
- mehrminuetiger stabiler Lauf.

## Arbeitsregeln

- Jeder Task folgt dem Dreischritt Implementierung, Review der betroffenen
  Pfade mit unmittelbarer Findingschliessung, Push auf `main`.
- Fehlende neue Tests sind kein Finding.
- Keine neue breite oder schmale Testsuite, keine Matrix und kein
  synthetisches Ersatzgate.
- Vorhandene Tests werden nur repariert, wenn sie selbst konkret falsch oder
  gebrochen sind.
- Sonic-Produktlaeufe folgen an den dokumentierten Gates oder nach
  ausdruecklicher Nutzeranweisung.
- Keine Controller-, GUI-, Paketierungs- oder Komfortarbeit vor B2.
- Kein Hardwareausbau auf Verdacht.
- Adressen aus dem Bring-up duerfen dokumentiert werden, aber nie
  titelbezogene Sonderfaelle im generischen Code erzeugen.

## Nicht auf dem aktuellen P0-Pfad

- vollstaendige Dreamcast-Kompatibilitaet fuer weitere Titel;
- umfassendes Replay jeder Gastinstruktion;
- neue PVR-/AICA-Features ohne Sonic-Produktbefund;
- GPU-Offload der Analyse;
- GUI-Politur;
- oeffentliche Releasepaketierung;
- neue Test-, Konformitaets- oder Threadmatrizen;
- weitere Controller-Haertung.

## v0.49.1 Definition of Done

`v0.49.1` ist abgeschlossen und gibt `v0.50.0 Alpha` erst frei, wenn:

- Recompiler, Runtime und externes Spielprojekt getrennt gebaut werden
  koennen;
- die Haupt-Executable aus der Original-GDI lokal installiert wird;
- das Produktbinary weder ARM7-Interpreter noch CPU-PVR oder einen
  vollstaendigen Dreamcast-Geraeteverbund enthaelt;
- Audio/Movie, GPU-Grafik, Disc, Eingabe und Save nativ gebunden sind;
- Opening und Memory-Card-Pfad ohne Skip oder Ersatzpfad durchlaufen;
- Sonic ueber diesen rein nativen Pfad das Hauptmenue erreicht;
- VMU/Saves nativ und atomar erhalten bleiben;
- normale Iterationen inkrementell ohne historischen Vollreexport bauen;
- keine Sonic-Sonderfaelle oder Retailbytes im generischen Katana-Kern
  liegen;
- keine neue Testsuite oder Testmatrix den Sonic-Produktnachweis ersetzt.

Dreamcast-MHz sind kein Versionsgate mehr. Reale Ladezeit, Framezeit,
Audio-Stabilitaet, Hostauslastung und Eingabelatenz bewerten den nativen Port.
