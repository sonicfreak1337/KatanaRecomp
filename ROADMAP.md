# KatanaRecomp Roadmap

Status: Pre-Alpha

Aktuelle Phase: `v0.49.0` - Sonic-Adventure-Produktbring-up, vollstaendiger Game-Entry-Handoff und 200-MHz-Hotpath

Erster oeffentlicher Release: `v0.50.0` Alpha

## Produktziel

KatanaRecomp ist ein statischer SH-4-Recompiler. KatanaRuntime ist die gemeinsam installierbare Dreamcast-Laufzeitbibliothek. Ein konkretes Spiel wird in einem getrennten, hashgebundenen Recomp-Projekt gebaut.

```text
KatanaRecomp
  -> analysiert SH-4
  -> erzeugt natives C++

KatanaRuntime
  -> stellt gemeinsame Dreamcast-Plattformvertraege bereit

SonicAdventureRecomp
  -> bindet generierten SA-Code, lokale Originaldaten, Handoffs, Hooks und Patches
  -> erzeugt die startbare Produkt-EXE
```

KatanaRecomp und KatanaRuntime bleiben im selben Repository, sind aber getrennte Build- und Installationsprodukte. Titeladressen, Titelhooks, private Symbole und Installationsprofile gehoeren langfristig in das externe Spielprojekt. Produktbefunde duerfen bis zu dieser Migration in der Bring-up-Dokumentation stehen, aber nie als titelbezogener Sonderfall in generischem Runtime- oder Recompilercode landen.

## Unverhandelbare Grenzen

- kein allgemeiner SH-4-Interpreter im normalen Produktport
- kein JIT
- kein Emulationsfallback
- keine stillen No-op-Stubs oder erfundenen Hardwareerfolge
- keine Sonic-spezifischen Adresshacks im generischen Katana-Kern
- keine Retail-, BIOS- oder Assetdaten im Repository oder verteilbaren Paket
- Flycast und XenonRecomp sind Referenzen, keine Codequellen
- das echte erzeugte Produkt ist die Bring-up-Abnahme
- keine neuen breiten Testmatrizen waehrend des Spiel-Bring-ups
- Produktlaeufe werden nach gleicher Gastarbeit verglichen, nicht nach fixer Hostzeit

## Verifizierter Ausgangsstand

Die Roadmap trennt ab jetzt drei voneinander unabhaengige Staende:

```text
letzte reale Produktevidenz: 60887f4 / ABI 74 / NativeDisc-v33
aktueller Source-Head main:   d3b87a1 / Analyzer-ABI 9 / Portvertrag 65
lokaler Arbeitsbaum:          nur Roadmap-/Statussynchronisierung
```

Der reale, erneut aus dem sauberen Main-Stand exportierte und mit der
privaten Originaldisc installierte ABI-74-NativeDisc-v33-Lauf praesentiert
den PAL-Sega-Screen. Er passiert den frueheren fehlenden AOT-Einstieg
`0x8C11088C -> 0x8C64784E` und stoppt erst bei Gesamtzyklus `573.987.074`
fail-closed an einer ungueltigen PVR-Background-Parameterdekodierung.
Ausgefuehrt wurden `158.753.804` Post-Entry-Zyklen in `6,87382 s`,
vorlaeufig `23,0954 MHz`, mit `16.376.023` zentralen Dispatches. Gegen den
vorherigen Lauf sind das `+19.996.512` Gastzyklen und `+2.763.834`
Dispatches. Das bleibt ein vorzeitig beendeter Bring-up-Lauf, kein
600-Millionen-Performancegate.

`d3b87a1` behebt die daraus allgemein abgeleitete PVR-Ursache: Background-
ISP/TSP-Parameter und ihre Vertices werden aus dem logischen
32-Bit-VRAM-Adressraum in das gemeinsame Backing projiziert; Texturdaten
bleiben im 64-Bit-Pfad. Zusaetzlich ist die Polaritaet von
`FPU_SHAD_SCALE.bit8` fuer Parameter-Selection gegen Intensity-Volume
korrigiert. Die kleine bestehende Rendererregression vergiftet den
numerisch gleichen Raw-Offset und passiert damit den zuvor fehlerhaften
Readpfad. Die reale Sonic-Abnahme dieses Source-Fixes steht noch aus.

`24d6132` schliesst vor dem naechsten Produktlauf die offenen
Reviewvertraege allgemein: Candidate-Calls nehmen am getrennten
Inventar-Rueckwaertsgraph teil, Codepointerprovenienz folgt nur dem
tatsaechlich uebergebenen Wert, und endliche bewachte Stackframe-Deltas
bleiben ausschliesslich im Inventarwalk erhalten. Die echte statische
PAL-Analyse nimmt dadurch `0x8C64784E` genau einmal als
`stored-code-address` auf und bindet ihn an den gemeinsamen Body
`0x8C6478C2`; `2.221` Guarded-AOT-Einstiege stehen `0` typisierten
Rejections gegenueber. Ein bekannter Inventarkandidat kann nicht mehr still
zwischen Analyse und Export verschwinden: jede Ablehnung besitzt einen
typisierten Grund, und der Produkt-Export bricht vor Codegen/Hostcompiler
fail-closed ab.

Die allgemeinen Source-Vertraege fuer `KR-4972`, `KR-4966`, `KR-4967` und
`KR-4970` sind weitgehend vorhanden: bewachte AOT-Einstiege mit
Exportvollstaendigkeitsinvariante, ein relatives Post-Entry-Gate, ein
vorbereiteter atomarer `CompletePlatform`-Commit sowie ein
nutzersave-autoritatives Product-Handoff-Profil. Die statische
`24d6132`-PAL-Analyse konvergiert ohne Budgetverlust und schliesst den
bekannten Guarded-Inventory-/Summaryblocker quellseitig. Produkt-Export,
Hostbuild und realer Lauf aus diesem Stand bleiben die noch offene Abnahme.

Der erste kalte ABI-73-Exportversuch benoetigte 419,5 Sekunden fuer die
vollstaendige Analyse und stoppte danach vor dem Hostcompiler an einem
faelschlich aus Datentabellen abgeleiteten bewachten AOT-Einstieg. Die
allgemeine Korrektur prueft `Stored`-/`Returned`-Inventarkandidaten nun mit
einem begrenzten lokalen CFG-Strukturvertrag, priorisiert echten
Delay-Slot-Kontext und erzeugt Callee-Metadaten aus dem finalen IR-CFG neu.
Die Strukturpruefung erfolgt vor der begrenzten globalen Inventaraufnahme;
ungueltige Kandidaten koennen das 1.024er-Budget daher nicht mehr
vergiften oder spaetere gueltige Ziele verdraengen. Der zugehoerige Cache
bleibt ueber den gesamten aeusseren Kontrollflussfixpunkt erhalten.
Der reale ABI-73-Sonic-PAL-NativeDisc-v33-Port wurde erzeugt und mit der
privaten Originaldisc installiert. Der kalte Export dauerte 711,2 Sekunden,
erzeugte 2.519 Funktionen und 63 Partitionen und traf keinen Analyse-/IR-
oder Codegencache. Der direkte unveraenderte Ninja-Warmbuild dauert
0,200236 Sekunden; ein identischer Voll-Warmexport traf den aeusseren Cache
jedoch erneut nicht und wurde nach 124 Sekunden beendet.

Der reale v33-Lauf praesentiert einen IP.BIN-Frame und erreicht Gesamtzyklus
`487.233.787`. Danach stoppt er korrekt ohne Interpreter an
`0x8C65EA06 -> 0x8C0101F2` mit `missing-aot`. Das entspricht 72.000.517
Post-Entry-Zyklen und 9.044.195 Post-Entry-Zentraldispatches, aber keinem
600-Millionen-Gate und keinem gueltigen MHz-Benchmark. Der generierte
Gatewrapper gibt fuer diese typisierte Fehlerstrecke faelschlich Exitcode 0
zurueck und die terminale `KATANA_BRINGUP_RUN`-Zusammenfassung fehlt. Der
Child-Exitvertrag und terminale Fehlerzusammenfassung sind auf `cb5fb47`
quellseitig und im vorhandenen kleinen Gatevertrag korrigiert; der reale
Sonic-Nachweis bleibt offen. Die separate v33-Sichtaufnahme wurde wegen des
angeforderten Rechnerneustarts vertagt.

Der anschliessende kalte NativeDisc-Exportversuch auf `7ecdefb` endete nach
`381,413 s` noch vor Hostcompiler und Portpaket:

```text
function_budget_exhausted:       1
candidate_inventory_truncated:   1
returned_table_scan_truncated:   0
admitted candidates:             29 / 1024
shape_budget_exceeded:           0
finale CFG-Iteration:            27
Seeds / Instruktionen:           2.023 / 215.623
kontextuelle Instruktionen:      229.443
indirekte Resolutions:           6.749
```

Die 65.536er-Grenze zaehlt Funktionsevaluationen, nicht entdeckte
Funktionen. Candidate-Call-Carrier waren als private Inventartransporte in
den semantischen Summary-Fixpunkt und dessen Rueckkanten geraten. Der
aggregierte Truncationwert kann ausserdem einen Shared-Multi-Owner-Regionwalk
nicht von den anderen Inventarlimits unterscheiden. `cb5fb47`
trennt Summary- und Inventarcallees, vereinigt wiederholte
Callargumentbeobachtungen und behandelt Multi-Owner-Shared-Tails als
begrenzte Inventarregion. Erst ein neuer Export kann diese Reparatur
abnehmen.

```text
Runtime-ABI:                    74
Block-ABI:                       5
Analyzer-ABI:                    9
PlatformServices-ABI:           13
Backend-Interface-ABI:          12
Portprojektvertrag:             65
Native-AOT-Emissionsprofil:     13
AOT-Partitionsschema:            5
```

### Reviewstatus vor dem naechsten Produktlauf

Auf `24d6132` sind die konkret reviewten Vertraege fuer
Carrieridentitaet, externe bedingte und normale Inventarnachfolger,
Codepointerwertprovenienz, Shape- und Raw-Inventarbudgets, zielbezogene
P1/P2-Code-Revalidierung, verschachtelte Owner-Exitframes,
Safepoint-Resume-Einstiege, Gateausfuehrung und exakte Exportartefakte
eingecheckt.
Zusaetzlich sind Guarded-AOT-Materialisierungsablehnungen nun ein
expliziter Analyse-/Exportvertrag und der konkrete Callbackpfad ueber
Candidate-Call, Candidate-Tail, bewachten Runtime-Stackframe und
Runtime-Objektstore ist allgemein inventarisierbar. Die fokussierten
Funktionswert-, Kontrollfluss- und Portexportvertraege sind gruen; der neue
Sonic-Produktproof aus diesem Stand ist der naechste Schritt.
Registerlokalisierung ist weiterhin nur teilweise am Architekturziel:
Liveness ist IR-basiert, die Ausdrucksumschreibung bleibt eine nun
lexikalisch begrenzte C++-Transformation.

Der Source-Commit schliesst die erst durch den
`7ecdefb`-Sonic-Export sichtbaren Punkte:

- Summary-Fixpunkt von Candidate-Carrier-Inventar trennen;
- wiederholte Callsite-/Callee-Beobachtungen vor Worklistupdates vereinigen;
- Shared-Multi-Owner-Tails ohne erfundene CFG-Kante inventarisieren;
- parameterabhaengige Candidate-Returns bei Bedarf in einem separat
  begrenzten Inventarwalk zum Caller zurueckfuehren;
- Owner-Domain-Wechsel auch fuer Fallthrough und Call-Continuation verfolgen
  oder explizit als unvollstaendig melden;
- rohe Stored-Kandidaten separat begrenzen und Evidenceklassen fair
  aufnehmen;
- alle Inventarbudgets in Analyse, Latent-AOT und Portmetadaten fail-closed
  transportieren;
- Whole-Export-Hits an ein exaktes generiertes Artefaktmanifest binden;
- typisierte Fehler in genau einer terminalen Bring-up-Zusammenfassung
  berichten.

Die direkt betroffenen Analyse-, Kontrollfluss-, Latent-AOT- und
Portexporttests sowie der ausgefuehrte Gatewrapper-Vertrag sind gruen. Der
manifestgebundene Whole-Export-Hit benoetigt wegen der absichtlichen
Dirty-Source-Sperre noch den nach dieser Dokumentationssynchronisierung
sauber neu konfigurierten CLI-Lauf. Keiner dieser Sourcepunkte ist damit
bereits als Sonic-Produktevidenz zu verstehen.

Die historische PAL-DirectBoot-Baseline ohne externes `GameEntryHandoff`
erreichte:

```text
Gastzyklen:           600.000.000
Hostzeit:             14,0113 s
effektive Gast-MHz:   42,8225
zentrale Dispatches:  52.329.316
GD-ROM-Kommandos:     70
AICA-Audiopuffer:     180
Frames:               0
sichtbarer Screen:    keiner
letzter PC:           0x8C65A624
first_problem:        none
```

Der Lauf bewies langen nativen Spielcodefortschritt ohne Missing-AOT oder
typisierten Geraeteabbruch. Er bewies keinen korrekten Spielzustand und keinen
erfolgreichen Handoff-DirectBoot.

### Historische CompletePlatform-v24-Vergleichsbasis

Der neue private NativeDisc-Capture wurde am exakten Game Entry bei
Gastzyklus `415.233.270` erzeugt. Sein Vertrag ist:

```text
GameEntryHandoff-Schema:       3
Artefaktformat:                2
Plattformzustandsvertrag:      2
Runtime-ABI:                   63
Portprojektvertrag:            53
kanonische Geraete:            22, einschliesslich Flash
portable Schedulerereignisse:  5
```

Der Handoff wurde im real erzeugten DirectBoot-Produktport validiert,
vollstaendig angewendet und vor dem ersten Spielblock semantisch
zurueckgelesen. Der Lauf meldete
`platform_state=complete`, `diagnostic=0` und `first_problem=none`.

Die damaligen absoluten 600-Millionen-Laeufe ergaben:

```text
NativeDiscBoot:
  Endzyklus:             600.000.000
  Hostzeit:              6,3161 s
  terminale Gast-MHz:    94,9954
  zentrale Dispatches:   17.080.114
  sichtbarer Meilenstein: IP.BIN-Frame
  finaler PC:            0x8C666D42
  first_problem:         none

DirectBoot CompletePlatform:
  Startzyklus:           415.233.270
  absolutes Endziel:     600.000.000
  post-entry Gastzyklen: 184.766.730
  Hostzeit:              5,01505 s
  vergleichbare Gast-MHz: 36,8425
  terminal ausgegeben:   119,64 MHz
  zentrale Dispatches:   16.033.676
  Gastzyklen/Dispatch:   11,52
  Frames:                0
  sichtbarer Screen:     keiner
  finaler PC:            0x8C666D42
  first_problem:         none
```

Die terminal ausgegebenen `119,64 MHz` teilen faelschlich den absoluten
Schedulerstand von 600 Millionen durch die reine DirectBoot-Hostzeit. Fuer
den Handoff-Lauf sind nur die `184.766.730` nach dem Entry ausgefuehrten
Gastzyklen vergleichbar; daraus folgen `36,8425 MHz`. Auch
`11,52` Gastzyklen pro Zentraldispatch entsprechen weiterhin dem alten
blockorientierten Verhaeltnis. Ein Geschwindigkeitsgewinn ist daher noch
nicht belegt. Der relative `KR-4966`-Quellvertrag ist inzwischen
implementiert; seine frische Produktabnahme steht noch aus.

Der warme Direct-Export mit unveraenderter Analyse dauerte `5,3 s`, ein
frischer Export `169,3 s`. Beim gezielten Entfernen regenerierbarer lokaler
Build-, Benchmark-, Scratch- und Testartefakte wurden `16.467.100.969` Bytes
freigegeben.

### DirectBoot-v26-Produktevidenz

Der aus `4cbab1e9c11320955fa8e18f66ae4b0e7e1cd0cb` erzeugte
MSVC-Produktport wurde ueber seinen echten Installer mit der privaten
PAL-Disc installiert (`3` Tracks, `521.461` Sektoren) und mit dem
CompletePlatform-Handoff ausgefuehrt. Der Lauf belegt den Abschluss der
konkreten Sound-DMA und verschiebt die funktionale Grenze:

```text
Restore-Zyklus:                    415.233.270
CompletePlatform-Apply:            22 Geraete, 5 Events
Endzyklus am typisierten Fehler:   552.903.647
Post-Entry-Zyklen:                 137.670.377
externe Walltime bis Fehler:       5,746371 s
warmer unveraenderter Build:       0,239825 s (Ninja no work)
zentrale Dispatches:               9.956.434
Post-Entry-Zyklen/Dispatch:        13,83
G2-Kanal 0:                        active=0, remaining=0
GD-ROM-Kommandos:                  72
AICA-Audiopuffer:                  165
PVR-Gastframes:                    2
PVR-Direct-Frames:                 2
veraenderte Direct-Pixel:          302.287
Hostframes / sichtbarer Screen:    0 / keiner
terminales Dispatchlabel:          byte-identity-mismatch (irrefuehrend)
Materializergrund:                 AotTemplateMismatch (14)
Callsite / Ziel:                   0x8C602B0A / 0x8C010F22
```

Die Walltime endet vor dem vorgesehenen Budget und ist deshalb kein
600-Millionen-Performancebenchmark. Sechzehn reale Fensteraufnahmen bis
`5,323 s` blieben schwarz. Die beiden technischen PVR-Direct-Frames sind
damit Fortschritt im Gast-/Framebufferpfad, aber noch kein sichtbarer
Hostframe.

Nach verifiziertem v26-Nachfolger wurden die ersetzten v23-, v24- und
v25-Ports samt ihren drei alten Exportworkspaces entfernt. Dadurch wurden
weitere `10.668.506.093` Bytes freigegeben; v26, sein installierter
Disc-Cache, der aktuelle Workspace und die private Originalquelle blieben
erhalten.

### Historische DirectBoot-v27-/v28-Produktevidenz

Der v27-MSVC-Gateport verwendet erstmals ein externes, serialisiertes
`GameProjectArtifact` Format 1 gemeinsam mit dem privaten
`GameEntryHandoff`. Das private, vollstaendig hashgebundene Sonic-Artefakt
traegt die Artefaktidentitaet
`sha256:9d4edff0270275f0b4931b733b2bd03ef330893f79d4728d6580adaf1107249f`
und seedet ausschliesslich die im v26-Produktlauf beobachtete exakte
Funktionsgrenze `0x8C010F22 + 0x18`. Diese Adresse liegt nur im externen
privaten Spielprojekt, nicht im generischen Katana-Kern.

Der Export erzeugte `1.946` Funktionen in `42` Partitionen. Der echte
Produktinstaller validierte erneut die private PAL-GDI (`3` Tracks,
`521.461` Sektoren); Repository und Portpaket enthalten weiterhin
`0` Retailsektoren. Nach zwei Reviewkorrekturen akzeptiert v28 sowohl die
reduzierte als auch die exakt passende vollstaendige externe
Runtime-Definition; eine exakte Funktionsgrenze auf einem Delay Slot wird
fail-closed abgewiesen. Der finale reale v28-CompletePlatform-Lauf ergab:

```text
Restore-Zyklus:                    415.233.270
CompletePlatform-Apply:            22 Geraete, 5 Events
Endzyklus am typisierten Fehler:   553.990.562
Post-Entry-Zyklen:                 138.757.292
Fortschritt gegen v26:             +1.086.915 Zyklen
externe Walltime bis Fehler:       5,275792 s
Post-Entry-Rate bis Fehler:         26,3008 MHz
zentrale Dispatches:               10.079.932
Dispatches gegen v26:              +123.498
G2-Kanaele:                        alle inaktiv
GD-ROM-Kommandos:                  72
AICA-Audiopuffer:                  165
PVR-Gastframes / Direct-Frames:    2 / 2
veraenderte Direct-Pixel:          302.287
Hostframes / sichtbarer Screen:    0 / keiner
terminales Dispatchlabel:          aot-template-mismatch
Materializergrund:                 AotTemplateMismatch (14)
Callsite / Ziel:                   0x8C11088C / 0x8C64784E
```

Das alte Ziel `0x8C010F22` wird damit statisch ausgefuehrt; KR-4971 ist
abgeschlossen. Der Lauf endet an einem neuen, engeren AOT-Coverageblocker und
erreicht deshalb weder 600 Millionen Gesamt- noch 600 Millionen
Post-Entry-Zyklen. Die tatsaechliche Post-Entry-Arbeit ergibt bis zum Fehler
`26,3008 MHz` gegen `23,9578 MHz` bei v26 und damit provisorisch `+9,78 %`
bei identischem Restore. Dieser Fehler-zu-Fehler-Vergleich ist keine
600-Millionen-Performanceabnahme.
Sechzehn reale v28-Fensteraufnahmen blieben schwarz; die technische PVR-Evidenz
blieb erhalten, ein sichtbarer Hostframe ist nicht belegt.

Der unveraenderte warme v28-Direct-Hostbuild dauerte `0,219272 s` gegen
`0,239825 s` bei v26. Der v27-Warmexport dauerte `4,263838 s`, v28 bestaetigt
`4,209083 s`; Analyse/IR und Metadaten trafen den Cache,
ebenso alle `42` AOT-Partitionen. Die v28-EXE ist `52.446.208` Bytes gross
gegen `52.406.784` Bytes bei v26 und besitzt SHA-256
`bdb20c5e8738cf4e5a2a21ed6f667384d44f87e3411506da27c0487f0f2cd7d8`.
Diese Buildwerte sind belastbar; wegen des vorzeitigen funktionalen Fehlers
sind sie kein aktueller MSVC-/clang-cl-Performancevergleich.

Nach dem nutzbaren v27-Nachfolger wurden v26 samt Workdir, die alten
v22-/v24-Referenzports und zwei zugehoerige Workdirs entfernt: sechs Targets,
exakt `10.166.434.310` Bytes. v27, sein
`.katana-port-work-b9a041bd0a2a`, Bootartefakt, Handoff, private GDI und
installierter Disc-Cache blieben erhalten. Die fruehere Aussage, v26 sei beim
v23-bis-v25-Cleanup bewahrt worden, beschreibt damit nur noch den damaligen
historischen Zwischenstand.

Nach der finalen v28-Abnahme wurden auch der ersetzte v27-Port und sein
Workspace gezielt entfernt. Das gab weitere `3.249.852.517` Bytes frei.
Erhalten bleiben ausschliesslich v28,
`.katana-port-work-e0e2126c4352`, Bootartefakt, Handoff, private GDI und
installierter Disc-Cache; die Entfernung ist nicht rueckgaengig.

### Historische DirectBoot-v30-Produktevidenz fuer KR-4972

Die allgemeine Funktionswertanalyse verfolgt Callback-Provenienz jetzt ueber
streng begrenzte Candidate-Tail-Jumps sowie ueber bewiesene
Runtime-Stackframes. Sie verwirft die Provenienz bei transformierender
Arithmetik, schliesst direkte `r15`-Prolog-Stores als Inventarsink aus und
haelt exakte Stackoffsets nur fuer vollstaendige, nicht aliasierende
Singletonwerte innerhalb eines kleinen Guardfensters. Der bestehende enge
Kontrollflusstest wurde fuer den konkret beobachteten Spill-/Reload- und
Runtime-Objektstore-Vertrag erweitert; keine neue Testsuite entstand.

Auf dem unveraenderten Sonic-Executable findet die generische Analyse damit
`0x8C64784E` als Funktion und `0x8C6478C2` als erreichbaren gemeinsamen Body:

```text
Analysezeit:                       15,901 s
Outer-Iterationen:                 30
finale Seeds / Funktionen:         1.715 / 1.756
analysierte Instruktionen:         154.092
Budgeterschoepfung:                nein
```

Der vollstaendige Export mit dem externen Spielprojekt transportiert diesen
Seed jedoch noch nicht in seine produktive CFG, Source-Map und AOT-Ausgabe.
Der frisch aus
`854141b8780626e24815c0bbbb60b5927635a1a6` erzeugte v30-MSVC-Port wurde
erneut ueber den Produktinstaller mit der privaten PAL-Disc installiert
(`3` Tracks, `521.461` Sektoren; weiterhin `0` Retailsektoren im
Repository). Sein realer Gate- und Sichtlauf ergab:

```text
Restore-Zyklus:                    415.233.270
CompletePlatform-Apply:            22 Geraete, 5 Events
Endzyklus am typisierten Fehler:   553.990.562
Post-Entry-Zyklen:                 138.757.292
zentrale Dispatches:               10.079.932
GD-ROM-Kommandos:                  72
AICA-Audiopuffer:                  165
PVR-Gastframes / Direct-Frames:    2 / 2
veraenderte Direct-Pixel:          302.287
Hostframes / sichtbarer Screen:    0 / keiner
reale Sichtaufnahmen:              15, alle schwarz
terminales Dispatchlabel:          aot-template-mismatch
Callsite / Ziel:                   0x8C11088C / 0x8C64784E
MSVC-Export:                       1.959 Funktionen / 42 Partitionen
Produkt-EXE:                       52.616.192 Bytes
Produkt-EXE SHA-256:               801f69727d1df3166b4ff29710856e327450f622e61fe2fd2fec76cc3a39d77e
```

Der Produktlauf ist damit metrisch an derselben funktionalen Grenze wie v28;
es gibt weder einen Boot- noch einen Dispatchfortschritt. Der frische Export
war ein kalter Export und ist kein gueltiger Warmbuildvergleich. An diesem
historischen Stand war KR-4972 nur teilweise umgesetzt: Die allgemeine
Analyseluecke war im Standalone-Lauf geschlossen, die Uebernahme durch die
Spielprojekt-/Exportintegration fehlte noch. Die folgenden Source-Vertraege
schliessen diese Luecke grundsaetzlich; der `7ecdefb`-Gesamtexport wird
jedoch vorher durch den neuen Summary-/Inventarbudgetfehler blockiert.

### Historische NativeDisc-v32-Sichtabnahme fuer KR-4973

Der saubere `906f185`-Kontrollport reproduzierte den Sega-Lizenzscreen und
grenzt die spaetere Schwarzregression auf das unter aktiver MMU
freigeschaltete Flag-Poll-Batching ein. Der allgemeine Fastpath akzeptiert
deshalb wieder nur `AddressTranslationMode::NoMmu`; unter AT=1 uebernimmt
vor jeder Mutation der normale statische AOT-Pfad.

Parallel ist die Hostpresentation nicht mehr vom einmaligen
`PvrGuestFrameProof` abhaengig. Ein gueltiger VBlank-Scanout wird bounded als
latest-wins Frame bereitgestellt, waehrend Proof, Marker und
Hardwareerfolgsmetriken getrennt bleiben. Runtime-ABI steigt auf 64.
`port <gdi>` kann nun dasselbe private, hashgebundene `--game-project` wie
DirectBoot konsumieren; die CLI legt weder Titeladressen noch Retailbytes in
den generischen Kern.

Der reale v32-MSVC-Gateport wurde frisch mit der privaten PAL-GDI installiert
und sichtbar ausgefuehrt:

```text
Gastzyklus am typisierten Fehler: 553.990.562
externe Produktzeit:              6,701 s
Rate bis Fehler:                  82,67 MHz (kein 600-Millionen-Gate)
zentrale Dispatches:              11.080.283
Hostframes:                       127
PVR Gast-/Direct-/Softwareframes: 2 / 2 / 1
hoechster sichtbarer Screen:      Sega ab 2,032 s
Callsite / Ziel:                  0x8C11088C / 0x8C64784E
MSVC-Export:                      2.051 Funktionen / 46 Partitionen
Produkt-EXE:                      53.677.056 Bytes
unveraenderter Ninja-Warmbuild:   0,203137 s
```

Damit treffen NativeDisc-v32 und DirectBoot-v30 am exakt gleichen
Schedulerzyklus und an derselben KR-4972-Kante ein. NativeDisc zeigt jedoch
in allen drei realen 640x480-Aufnahmen den Sega-Screen, DirectBoot-v30
meldete am gleichen Punkt null Hostframes und blieb in 15 Aufnahmen schwarz.
Der private Sega-Klassifikator liefert wegen des grauen PAL-Hintergrunds
weiterhin ein False-Negative; die Pixel sind manuell und durch stabile Blau-/
Nichtschwarzanteile belegt.

KR-4973 ist durch diese historische Abnahme abgeschlossen. Der
`KR-4972`-AOT-Vertrag ist im Source vorhanden, die aktuelle Gesamtanalyse
aber noch nicht exportfaehig. Ob der gemeinsame Blocker damit im realen
ABI-74-Produkt passiert wird, muss der frische Sonic-Lauf zeigen. Ein
Sega-Screen ist im DirectBoot wegen
uebersprungener IP.BIN weiterhin kein Pflichtmeilenstein.

`GameProjectArtifact` Format 1 besitzt eine Payload-SHA-256 und eine
Artefakt-SHA-256, kann durch die Runtime-API geschrieben und geladen werden
und bindet die vollstaendige deklarative Definition an den Export.
Native Callback-/Hookzeiger und der private Handoff-Provider werden bewusst
nicht serialisiert. Der Export konsumiert Funktions-, Tabellen-, Symbol-,
Template- und Codeidentitaetsmetadaten vollstaendig; der erzeugte Port bindet
zur Laufzeit nur den reduzierten Identitaets-, Boot- und Handoffvertrag, wenn
keine nativen Hooks eine vollstaendige externe Runtime-Registrierung
erfordern. `GameProjectFunctionBoundary::size` erreicht jetzt als exakte
Grenze Analyzer, CFG, IR und AOT; der Analyzer-ABI-Vertrag steigt deshalb von
2 auf 3.

## KR-4965-/KR-4971-Ergebnis und neuer erster Produktblocker

Der fruehere ADXT-/mwSnd-Sound-Completion-Poll war:

```text
Objekt:               0x8C8D3908
Completion-Flag:      0x8C8D3920
Poll-Callback:        0x8C666D42 (RTS; NOP)
Completion-Writer:    0x8C65A458
```

Der Waitpfad setzt `[object+24]` zunaechst auf `0` und pollt danach auf `1`. Der erwartete Writer schreibt `1` nach `[object+24]`. Saemtliche sechs statisch aufgeloesten Caller des Waitvertrags liegen in ADXT-/mwSnd-Soundpfaden.

Zwei allgemeine Holly-G2-Vertragsfehler bildeten die erste verlorene Kante:

```text
SB_G2APRO 0x4659404F
  -> Start- und Endbyte waren vertauscht
  -> zulaessiges Haupt-RAM wurde als Overrun abgewiesen

ADTSEL 5 + ADST 1
  -> CPU-initiierter Transfer mit externem AICA-Request-Level
  -> Runtime wartete auf einen Produkttrigger, den es nicht gab
  -> SB_FFST.bit0=0 wird jetzt beim Armieren als request-ready ausgewertet
```

v26 beendet G2-Kanal 0 vollstaendig und verlaesst `0x8C666D42`. Weder ein
Hostpatch am Completion-Flag noch eine Titeladresse im generischen
Runtimecode wurde eingefuehrt. Der Writer `0x8C65A458` beziehungsweise der
Flagwechsel auf `1` wurde nicht separat instrumentiert; KR-4965 ist gemaess
seiner ausdruecklichen Alternativabnahme abgeschlossen, weil ein neuer,
engerer allgemeiner Blocker belegt ist.

KR-4971 fuehrt das v26-Ziel ueber die exakte, hashgebundene externe
Funktionsgrenze statisch in Analyse und AOT. Die getrennte terminale Diagnose
meldet `AotTemplateMismatch` nun korrekt als `aot-template-mismatch`, statt
ihn mit einem echten Byteidentitaetsfehler zusammenzufassen. Der v28-Lauf
passiert das alte Ziel und belegt einen neuen, engeren allgemeinen Blocker.

Der **letzte historisch belegte Produktblocker** ist KR-4972: Ein indirekter Call
bei `0x8C11088C` erreicht das unveraenderte Boot-Executable-Ziel
`0x8C64784E`, fuer das im historischen real exportierten Port kein passendes
statisches AOT vorliegt. Der Zielcode beginnt mit einem `BRA` auf den
gemeinsamen Zielpfad `0x8C6478C2`. Die generische Analyse beweist diesen
Callback-/Shared-Tail-Pfad inzwischen aus Codepointer-Provenienz und
Runtime-Frame-Daten.

Der aktuelle `KR-4972`-Quellvertrag erhaelt bewachte Tail-/AOT-Einstiege bis
in CFG, IR und statisches AOT, behandelt Shared Bodies explizit und bricht
den Export ab, falls ein akzeptierter Einstieg weder emittiert noch begruendet
abgelehnt wurde. Die eingecheckte Summary-/Inventarkorrektur muss diesen
Vertrag jetzt im realen Export konvergent bestaetigen. Ob der historische Blocker im
realen Produkt damit passiert wird, ist bis zum frischen ABI-74-Sonic-Lauf
offen. Interpreter, JIT, Runtime-Dekodierung und Emulationsfallback bleiben
verboten.

## CompletePlatform-Quellstand und offene Produktabnahme

CompletePlatform-v24 transportiert und appliziert im realen Produktpfad
alle 22 kanonischen Geraetezustaende einschliesslich Flash sowie die fuenf
portablen Schedulerereignisse. Der fruehere
`game-entry-handoff-complete-platform-apply-unavailable`-Abbruch ist damit
nicht mehr der aktuelle Stand.

Die Handoff- und Gate-P0-Quellvertraege sind implementiert:

- KR-4966 verwendet ein relatives Post-Entry-Budget, berichtet die
  tatsaechliche Gastarbeit und erlaubt Exitcode 0 nur bei vollstaendigem Gate
  plus Pflichtmeilenstein.
- KR-4967 bereitet CPU, Speicher, Scheduler, IRQ und Geraete vor dem Commit
  vollstaendig vor; die Veroeffentlichung laeuft atomar und CPU-PC/PR werden
  zuletzt publiziert.
- KR-4970 trennt das Product-Handoff von verlustfreier Diagnose und behaelt
  installierte VMU-/Flash-Nutzerdaten als autoritative Working Copy.
- KR-4972 bindet bewachte AOT-Einstiege und Exportvollstaendigkeit allgemein;
  seine Summary-/Inventar-Konvergenzkorrektur ist auf `cb5fb47`
  eingecheckt.

Die reale ABI-74-Produktabnahme dieser vier Source-Vertraege bleibt offen.
Davon getrennte Langzeitpunkte bleiben:

- KR-4953 braucht weiterhin einen zweiten unabhaengigen Capture sowie
  Offline-Inspect/Verify.
- KR-4968 muss ausserdem einen bereits restaurierten aktiven
  Hardware-Request-G2-Kanal ohne rehydriertes Completionevent explizit
  nach dem passiven Apply abgleichen; die aktuelle Sonic-DMA wird erst nach
  dem Entry armiert und ist durch v26/v28 abgedeckt.
- KR-4962 bleibt bis zur belegten NativeDisc-/DirectBoot-Paritaet und einem
  ersten DirectBoot-Frame offen.

## Verbindlicher v0.49-Kritischer Pfad

```text
KR-4965 ADXT/mwSnd-Sound-Completion bis zum Writer schliessen
  [abgeschlossen ueber engeren allgemeinen Blocker]
  |
  +--> KR-4971 RuntimeOnly-AOT-Coverage fuer statisch identifizierbares
         Ziel herstellen [abgeschlossen; altes Ziel statisch emittiert]
         |
         +--> KR-4972 Hashgebundene Shared-Callback-/Thunk-AOT-Coverage
                herstellen [AOT-Vertrag und Analysefix eingecheckt,
                Produktproof offen]
                |
                +--> Summary-/Inventar-Konvergenz, Manifestcache und
                       terminalen Fehlerbericht [Source abgeschlossen]
                |
                +--> KR-4966 Post-Entry-Produktgate und erforderliche
                       Meilensteine [Source implementiert, Produktproof offen]
  |
  +--> KR-4967 Atomarer CompletePlatform-Capture-/Apply-Koordinator
         [Source implementiert, Produktproof offen]
         |
         +--> KR-4968 AICA-/G2-/DMAC-/Scheduler-/IRQ-Handoff
         +--> KR-4969 PVR-/SPG-/ASIC-Handoff fuer ersten Spiel-Frame
         +--> KR-4970 Produkt-sicherer Maple-/VMU-Handoff
                [Source implementiert, Produktproof offen]
                    |
                    +--> KR-4952 / KR-4953 abschliessen
                           [KR-4953 Double-Capture/Inspect/Verify offen]
                           -> KR-4962 Game-Entry-Paritaet und Produktboot
                              [Frame und Paritaet offen]

Nach dem ersten korrekten DirectBoot-Produktmeilenstein:

KR-4955 Explizite Funktionsgrenzen End-to-End
  -> KR-4956 Static-AOT-Dispatchflucht
  -> KR-4957 Direkte native Calls
  -> KR-4958 IR-basierte Registerlokalisierung
  -> KR-4959 Ereignisgetriebene Safepoints
  -> KR-4960 200-MHz-Produkt-Hotpath

Parallel nach stabiler Handoff-Grundlage:

KR-4954 Deklaratives Spielprojekt/Scaffold
  -> KR-4961 Externes SonicAdventureRecomp-Projekt

KR-4963 Inkrementeller Runtime-/Spielbuild

Alle Linien:
  -> KR-4964 v0.49 Produktabnahme
```

## Phase A - Sound-Completion vor weiterer Plattformbreite

Status: **abgeschlossen ueber die Alternativabnahme "engerer allgemeiner
Blocker"**.

Ziel war nicht, AICA pauschal auszubauen. NativeDiscBoot und
CompletePlatform-DirectBoot reproduzierten denselben ADXT-/mwSnd-Poll; daher
wurde zuerst dieser konkrete Completion-Vertrag geschlossen:

1. Ausloeser des ADXT-/mwSnd-Workers bestimmen.
2. Zugehoerigen G2-/AICA-/DMAC-Transfer und erwartete IRQ-/Schedulerkante identifizieren.
3. Beweisen, warum `0x8C65A458` nicht erreicht wird.
4. Allgemeine Ursache reparieren.
5. Danach einen einzigen normalen Produktlauf bis zum naechsten terminalen
   Ergebnis ausfuehren.

Akzeptanz:

- Completion-Flag wechselt durch den echten Gastwriter von `0` auf `1`, oder
- ein neuer, engerer allgemeiner Blocker ist belegt.
- Kein Hostpatch schreibt das Flag direkt.
- Keine private Adresse wird in generischen Produktcode eingebaut.

v26 und v28 belegen den Abschluss der G2-DMA und verlassen den alten Poll.
v28 passiert zusaetzlich die alte RuntimeOnly-AOT-Coverageluecke aus KR-4971
und endet am engeren Shared-Callback-/Thunk-Blocker aus KR-4972. Ein direkter
Writer-/Flagbeweis bleibt
eine diagnostische Zusatzinformation, ist aber nicht mehr der erste
Produktblocker.

## Phase B - CompletePlatform-Handoff

Der Handoff wird nicht als Reihe fallibler `restore_state()`-Aufrufe implementiert. Er braucht einen globalen Prepare-/Commit-Vertrag:

```text
capture
-> validate
-> prepare all allocations and event bindings
-> commit all noexcept
-> semantic digest
-> first guest block
```

Das historische CompletePlatform-v24 prevalidierte vor der ersten Mutation alle 22
Geraetenutzlasten und die Ereignisbijektion und hat einen realen
Produkt-Apply ohne `first_problem` erreicht. Im aktuellen Quellstand bereitet
`KR-4967` alle falliblen Subsystemzustaende vor Commitbeginn vor und
veroeffentlicht CPU-PC/PR zuletzt. Die frische ABI-74-Produktabnahme und
weitergehende normative per-Subsystem-Digests stehen noch aus.

Verbindliche Reihenfolge:

1. Host-/Media-Ausgabe noch nicht starten.
2. vorhandene Runtimeereignisse kontrolliert entfernen.
3. RAM/VRAM/AICA-RAM-Deltas vorbereiten.
4. Geraete passiv vorbereiten.
5. Schedulerzeit und typisierte Events vorbereiten.
6. Event-IDs neu erzeugen und den Geraeten zuordnen.
7. ASIC-/IRQ-Zustand konsolidieren.
8. CPU/MMU zuletzt committen.
9. per Subsystem Digests vergleichen.
10. erst dann Spielcode ausfuehren.

Produktdaten werden getrennt:

- deterministische Bootstrapdeltas
- bewiesene Baseline
- nutzereigene Daten wie VMU/Saves
- zeitabhaengige Ereignisse

Ein Produkt-Handoff darf keine alte VMU-Working-Copy ueber aktuelle Nutzerdaten schreiben.
Der dafuer allgemeingueltige, Save-erhaltende `KR-4970`-Quellvertrag ist
implementiert; sein realer ABI-74-Produktnachweis steht noch aus.

## Phase C - Produktgates

Boot- und Performancegates verwenden eine relative Laufdauer ab Game-Entry:

```text
target_cycle = restored_game_entry_cycle + requested_elapsed_guest_cycles
```

Die historischen v24-/v28-Ports verwendeten noch ein absolutes
Schedulermaximum von `600.000.000`. v24 fuehrte dadurch nach dem Restore nur
`184.766.730` Gastzyklen aus; v28 endete schon nach `138.757.292`
Post-Entry-Zyklen am AOT-Coveragefehler. Der aktuelle `KR-4966`-Quellvertrag
berichtet Startzyklus, Post-Entry-Gastzyklen, Hostzeit und daraus berechnete
effektive Gast-MHz und verweigert einem vorzeitig beendeten Budgetlauf
Exitcode 0. Die reale ABI-74-Abnahme ist noch ausstehend.

Das Spielprojekt definiert einen erforderlichen Meilenstein. Ein schwarzer Lauf darf nicht mit Exitcode 0 als vollstaendiger Produkterfolg gelten.

Empfohlene Resultate:

```text
0 = erforderlicher Produktmeilenstein erreicht
3 = Gastzyklusbudget erreicht, Meilenstein verfehlt
1 = typisierter Fehler
```

## Phase D - Xenon-artiger AOT-Hotpath

Der historische CompletePlatform-Direct-Lauf fuehrte `184.766.730`
Post-Entry-Gastzyklen mit `16.033.676` Zentraldispatches aus, also nur
`11,52` Gastzyklen je Dispatch. Dieser Wert stammt aus dem alten absoluten
Gate und belegt keinen Geschwindigkeitsgewinn.

Der eingecheckte Quellstand `cb5fb47` enthaelt weiterhin:

- direkt gebundene validierte Ausfuehrungs- und Fastpathdeskriptoren;
- direkte Owner-Einstiege ohne zweiten Owner-Switch fuer normale Entries;
- direkte native Ausfuehrung endlicher indirekter Ziele mit validiertem
  Fallback fuer unbekannte Ziele;
- IR-Liveness-basierte Auswahl fuer GPR-/Skalarregisterlokalisierung mit
  Release/Reload an Architekturgrenzen; die Ausdrucksumschreibung erfolgt
  noch als lexikalisch begrenzte C++-Transformation;
- blockweise direkte Haupt-RAM-Schreibbatches mit korrekter Invalidierungs-
  und Fallbackgrenze.

Die reale Wirkung auf Zentraldispatches und effektive Gast-MHz ist bis zum
frischen ABI-74-Produktlauf unbekannt. Danach bleiben als Performancearbeit
insbesondere:

- verbleibende Function-AOT-Escapes anhand
  `KATANA_STATIC_AOT_ESCAPE_STATS` reduzieren;
- konkrete FastRuntime-Kontexte statt `dynamic_cast` im Produkt-Hotpath;
- direkte Calls, Returns und Cycle-Commits ueber groessere bewiesene Regionen
  halten;
- mindestens 200 MHz, mit Zielreserve 250 MHz unpaced.

Gastzyklen oder Geraetelatenzen werden nicht kuenstlich reduziert.

## Boot-Testplanung

Das Endprodukt ist der Test. Es werden keine Tests pro Geraetefeld oder Hilfsfunktion angelegt.

### Produktlauf A - nach KR-4965 [ausgefuehrt]

Der gewoehnliche v26-DirectBoot wurde mit dem real installierten
PAL-Disc-Cache und CompletePlatform-Handoff ausgefuehrt. Er endete vor dem
falsch absoluten Budget bei Gastzyklus `552.903.647`.

Ergebnis:

- G2-Kanal 0 abgeschlossen und alter Sound-Poll verlassen
- engerer allgemeiner RuntimeOnly-AOT-Coverageblocker belegt
- zwei technische Direct-Frames, aber kein sichtbarer Hostframe
- keine Aussage ueber vollstaendigen Handoff oder Performancegate

### Produktlauf A2 - nach KR-4971 [ausgefuehrt]

Der finale v28-MSVC-Gateport wurde aus dem Boot-Executable-Artefakt, dem privaten
`GameProjectArtifact` und dem CompletePlatform-Handoff erzeugt, ueber den
echten Installer mit der PAL-GDI installiert und bis zum naechsten typisierten
Ergebnis ausgefuehrt.

Ergebnis:

- das alte statische Ziel `0x8C010F22` ist AOT-abgedeckt und wird passiert
- `+1.086.915` Gastzyklen Fortschritt gegen v26
- `5,275792 s` und `26,3008 MHz` Post-Entry-Rate bis zum Fehler gegen
  `5,746371 s` und `23,9578 MHz` bei v26 (`+9,78 %`)
- Sound-/G2- und technische PVR-Evidenz bleiben erhalten
- neuer erster Blocker `0x8C11088C -> 0x8C64784E` aus KR-4972
- 16 reale Fensteraufnahmen bleiben schwarz
- kein 600-Millionen-Performanceergebnis

### Historischer Produktlauf A3 - KR-4972-Analyserunde

Der v30-MSVC-Gateport wurde frisch aus der korrigierten generischen Analyse
erzeugt, ueber den Produktinstaller mit derselben PAL-GDI installiert und
real ausgefuehrt.

Ergebnis:

- generische Analyse erkennt `0x8C64784E` als Funktion und `0x8C6478C2` als
  erreichbaren gemeinsamen Body
- produktive CFG, Source-Map und AOT-Ausgabe enthalten den Seed noch nicht
- derselbe Fehler bei `553.990.562` Gastzyklen und `10.079.932`
  Zentraldispatches
- Sound-/G2- und technische PVR-Evidenz unveraendert
- 15 reale Fensteraufnahmen schwarz; `sega_seen=false`
- der damalige KR-4972-Exportvertrag blieb offen

### Produktlauf A4 - aktueller P0-Produktproof [ausstehend]

Nach Review, fokussiertem Build und Commit `cb5fb47` der Analyse-, Inventar-,
Terminalreport- und Cachekorrekturen wird genau ein frischer
ABI-74-MSVC-NativeDisc-Port erzeugt, mit der privaten Original-GDI
installiert und fuer exakt 600 Millionen Post-Entry-Gastzyklen ausgefuehrt.
Ein separater Sichtlauf klassifiziert den hoechsten realen Bildschirm. Erst
dieser Lauf entscheidet, ob sowohl der historische
`0x8C11088C -> 0x8C64784E`-Blocker als auch die v33-Grenze
`0x8C65EA06 -> 0x8C0101F2` passiert werden, welcher neue erste Fehler
vorliegt und welche effektive Gast-MHz der neue Hotpath erreicht.

### Produktlauf B - nach KR-4967 bis KR-4970

Ein realer Capture-/Apply-Lauf ist mit v24 belegt. Zur Abnahme fehlen
weiterhin ein zweiter unabhaengiger Capture, Offline-Inspect/Verify, die
normativen per-Subsystem-Digests und ein realer ABI-74-Nachweis des
implementierten Save-erhaltenden ProductHandoff.

Verbindlich bleiben zwei reale Laeufe:

1. NativeDisc bis zum Game-Entry und CompletePlatform-Capture
2. DirectBoot mit CompletePlatform-Apply bis unmittelbar vor den ersten Spielblock

Ziel:

- CPU-, Memory-, PVR-, Sound/DMA-, Maple-, IRQ- und Scheduler-Digests stimmen

### Produktlauf C - nach KR-4962

DirectBoot fuer 600 Millionen Gastzyklen **ab Game-Entry**.

Ziel:

- mindestens `FirstGameFramebufferWrite` oder `FirstTaFrame`
- bei Nichterreichen Exitcode 3 und konkreter naechster Blocker

### Produktlauf D - nach KR-4960

Derselbe Produktpfad und dieselbe Gastarbeit.

Ziel:

- mindestens 200 MHz effektiv
- sichtbarer Meilenstein bleibt erhalten

Kleine vorhandene Vertragstests duerfen angepasst werden, wenn eine schwer sichtbare Datenkorruption sonst nicht abgesichert werden kann. Neue breite Matrizen, neue Gateprojekte und Vollsuiten nach jeder Teilaufgabe sind nicht vorgesehen.

## Produktmeilensteine

### B0 - Game-Entry korrekt

- Haupt-Executable aus eigener GDI identifiziert und hashgebunden
- vollstaendiger Post-IP.BIN-Handoff mit globalem `noexcept`-Commit angewendet
- NativeDisc und DirectBoot besitzen gleiche normative Subsystemdigests

### B1 - Soundworker fortgeschritten

- ADXT-/mwSnd-Completion-Writer wird durch den Gast erreicht
- kein direktes Hostsetzen des Completion-Flags
- AICA-/G2-/DMA-/IRQ-/Schedulerkante ist typisiert und reproduzierbar

### B2 - Sichtbares Spielbild

- Haupt-Executable erzeugt Direct-FB- oder TA-Ausgabe
- aktiver Scanout liest den erzeugten Bereich
- Host praesentiert den Frame

### B3 - Echtzeit

- mindestens 200 MHz effektive Gastgeschwindigkeit
- Zielreserve mindestens 250 MHz unpaced
- billige Produktdiagnose benutzt denselben schnellen Pfad

### B4 - Titelbild und Eingabe

- Titelbild oder erster interaktiver Spielscreen
- Controller im real gestarteten Spiel
- mehrminuetiger stabiler Lauf

## Arbeitsregeln

- Eine Implementierungsrunde endet mit einem echten Produktlauf.
- Produktlaeufe folgen erst nach einem zusammenhaengenden Implementierungsblock.
- Keine neue breite Testsuite.
- Keine Controller-, GUI-, Paketierungs- oder Komfortarbeit vor B2.
- Kein Hardwareausbau auf Verdacht.
- Roadmap, Tasks und Status werden erst nach realer Produktevidenz als abgeschlossen markiert.
- Adressen aus dem SA-Bring-up duerfen dokumentiert werden, aber nie titelbezogene Sonderfaelle im generischen Code erzeugen.

## Nicht auf dem aktuellen P0-Pfad

- vollstaendige Dreamcast-Kompatibilitaet fuer weitere Titel
- umfassendes Replay jeder Gastinstruktion
- neue PVR-/AICA-Features ohne SA-Produktbefund
- GUI-Politur
- oeffentliche Releasepaketierung
- neue Konformitaetsmatrizen
- weitere Controller-Haertung

## v0.49 Definition of Done

`v0.49.0` ist erst abgeschlossen, wenn:

- Recompiler, Runtime und externes Spielprojekt getrennt gebaut werden koennen
- die Haupt-Executable aus Original-GDI lokal installiert wird
- DirectBoot einen vollstaendigen, produkt-sicheren Post-IP.BIN-Handoff nutzt
- Sound-Completion und erster sichtbarer Spiel-/TA-Frame erreicht werden
- NativeDisc und DirectBoot ab Game-Entry normativ uebereinstimmen
- der normale Produktport mindestens 200 MHz erreicht
- VMU/Saves durch den Handoff nicht zurueckgesetzt werden
- keine Sonic-Sonderfaelle oder Retailbytes im generischen Katana-Kern liegen
- keine umfangreiche Testsuite den Produktnachweis ersetzt
