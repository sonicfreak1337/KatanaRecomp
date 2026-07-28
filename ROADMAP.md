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

Roadmapbasis dieser Runde:

```text
8e5ab3145fb5fcafc056fd87025baf3497085342
Distinguish AOT template mismatches
```

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

### CompletePlatform-v24-Vergleichsbasis

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

Die aktuellen absoluten 600-Millionen-Laeufe ergaben:

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
nicht belegt, und KR-4966 bleibt offen.

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

### DirectBoot-v27-/v28-Produktevidenz

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

### DirectBoot-v30-Produktevidenz fuer KR-4972

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
war ein kalter Export und ist kein gueltiger Warmbuildvergleich. KR-4972 ist
teilweise umgesetzt: Die allgemeine Analyseluecke ist im standalone
Lauf auf demselben Produktinput geschlossen, die Uebernahme dieser Erkenntnis durch die
Spielprojekt-/Exportintegration bleibt der aktive Blocker.

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

Der **erste aktive Produktblocker** bleibt KR-4972: Ein indirekter Call
bei `0x8C11088C` erreicht das unveraenderte Boot-Executable-Ziel
`0x8C64784E`, fuer das im real exportierten Port weiterhin kein passendes
statisches AOT vorliegt. Der Zielcode beginnt mit einem `BRA` auf den
gemeinsamen Zielpfad `0x8C6478C2`. Die generische Analyse beweist diesen
Callback-/Shared-Tail-Pfad inzwischen aus Codepointer-Provenienz und
Runtime-Frame-Daten; der vollstaendige Spielprojektexport verwirft
beziehungsweise uebernimmt das Ergebnis aber noch nicht.

KR-4972 muss deshalb nicht mit einer geratenen Sonic-Grenze umgangen werden,
sondern den allgemein erkannten Zielvertrag durch die
Spielprojekt-/Exportintegration bis in CFG, IR und statisches AOT erhalten.
Interpreter, JIT, Runtime-Dekodierung und Emulationsfallback bleiben
verboten.

## Verbleibende CompletePlatform-Luecken

CompletePlatform-v24 transportiert und appliziert im realen Produktpfad
alle 22 kanonischen Geraetezustaende einschliesslich Flash sowie die fuenf
portablen Schedulerereignisse. Der fruehere
`game-entry-handoff-complete-platform-apply-unavailable`-Abbruch ist damit
nicht mehr der aktuelle Stand.

Die Produktabnahme bleibt dennoch offen:

- KR-4966 muss das absolute 600-Millionen-Gate durch ein relatives
  Post-Entry-Gate ersetzen.
- KR-4967 braucht weiterhin einen strikt globalen Prepare-Vertrag mit
  garantiert `noexcept` ausfuehrbarem Commit und normative Digests je
  Subsystem. Die aktuelle Apply-Implementierung prevalidiert alle
  Nutzlasten, garantiert aber noch keinen solchen globalen Commitvertrag.
- KR-4970 muss einen allgemeinen, Save-erhaltenden ProductHandoff
  sicherstellen, der neuere VMU-/Flash-Working-Copies niemals mit einem
  aelteren Capture ueberschreibt.
- KR-4953 braucht weiterhin einen zweiten unabhaengigen Capture sowie
  Offline-Inspect/Verify.
- KR-4968 muss ausserdem einen bereits restaurierten aktiven
  Hardware-Request-G2-Kanal ohne rehydriertes Completionevent explizit
  nach dem passiven Apply abgleichen; die aktuelle Sonic-DMA wird erst nach
  dem Entry armiert und ist durch v26/v28 abgedeckt.
- KR-4962 bleibt bis zur belegten NativeDisc-/DirectBoot-Paritaet und einem
  ersten DirectBoot-Frame offen.
- KR-4972 muss den neuen hashgebundenen Shared-Callback-/Thunk-AOT-
  Coverageblocker im vollstaendigen Export schliessen, bevor ein relatives
  Langzeitgate aussagekraeftig laufen kann. Die generische Analyse erkennt
  das Ziel bereits; der v30-Produktport enthaelt es noch nicht.

## Verbindlicher v0.49-Kritischer Pfad

```text
KR-4965 ADXT/mwSnd-Sound-Completion bis zum Writer schliessen
  [abgeschlossen ueber engeren allgemeinen Blocker]
  |
  +--> KR-4971 RuntimeOnly-AOT-Coverage fuer statisch identifizierbares
         Ziel herstellen [abgeschlossen; altes Ziel statisch emittiert]
         |
         +--> KR-4972 Hashgebundene Shared-Callback-/Thunk-AOT-Coverage
                herstellen [FIRST, Analyse teilweise; Produktexport offen]
                |
                +--> KR-4966 Post-Entry-Produktgate und erforderliche
                       Meilensteine [offen]
  |
  +--> KR-4967 Atomarer CompletePlatform-Capture-/Apply-Koordinator
         [teilweise: Produkt-Apply belegt; noexcept-Commit/Digests offen]
         |
         +--> KR-4968 AICA-/G2-/DMAC-/Scheduler-/IRQ-Handoff
         +--> KR-4969 PVR-/SPG-/ASIC-Handoff fuer ersten Spiel-Frame
         +--> KR-4970 Produkt-sicherer Maple-/VMU-Handoff [offen]
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

CompletePlatform-v24 prevalidiert vor der ersten Mutation alle 22
Geraetenutzlasten und die Ereignisbijektion und hat einen realen
Produkt-Apply ohne `first_problem` erreicht. Das ist ein belastbarer
Zwischenstand, aber noch nicht die Erfuellung von KR-4967: Der Commit ist
noch nicht als global strikt `noexcept` garantiert, und normative
per-Subsystem-Digests fehlen.

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
Der dafuer allgemeingueltige, Save-erhaltende Vertrag ist KR-4970 und
bleibt trotz des erfolgreichen privaten v24-Captures offen.

## Phase C - Produktgates

Boot- und Performancegates verwenden eine relative Laufdauer ab Game-Entry:

```text
target_cycle = restored_game_entry_cycle + requested_elapsed_guest_cycles
```

Der generierte DirectBoot-Gatepfad verwendet weiterhin ein absolutes
Schedulermaximum von `600.000.000`. v24 fuehrte dadurch nach dem Restore nur
`184.766.730` Gastzyklen aus. v28 erreicht dieses falsche Maximum wegen des
neuen typisierten AOT-Coveragefehlers bereits nicht und endet nach
`138.757.292` Post-Entry-Zyklen. KR-4966 muss mindestens Startzyklus,
post-entry Gastzyklen, Hostzeit und daraus berechnete effektive Gast-MHz
berichten; ein vorzeitiger typisierter Fehler darf nicht als Gateerfolg
erscheinen.

Das Spielprojekt definiert einen erforderlichen Meilenstein. Ein schwarzer Lauf darf nicht mit Exitcode 0 als vollstaendiger Produkterfolg gelten.

Empfohlene Resultate:

```text
0 = erforderlicher Produktmeilenstein erreicht
3 = Gastzyklusbudget erreicht, Meilenstein verfehlt
1 = typisierter Fehler
```

## Phase D - Xenon-artiger AOT-Hotpath

Der aktuelle Produktpfad bleibt zu blockorientiert. CompletePlatform-Direct
fuehrte `184.766.730` post-entry Gastzyklen mit `16.033.676`
Zentraldispatches aus, also nur `11,52` Gastzyklen je Dispatch. Die
niedrigere absolute Dispatchzahl gegenueber der historischen Baseline
entstand durch die geringere Gastarbeit und belegt keinen
Geschwindigkeitsgewinn.

Die Performancearbeit erfolgt erst nach einem stabilen Bootmeilenstein und verwendet die bereits vorhandenen `KATANA_STATIC_AOT_ESCAPE_STATS`.

Schwerpunkte:

- die jetzt end-to-end transportierten expliziten Funktionsgroessen auf
  weitere bewiesene externe Grenzen anwenden
- `NativeEntrySafe`, `DirectCallEligible`, `CompletionDeferrable` und `RequiresSafepointBeforeEntry` trennen
- bekannte Calls und Returns innerhalb nativer Funktionsregionen halten
- IR-/Liveness-basierte Registerlokalisierung statt C++-Stringersetzung
- konkrete FastRuntime-Kontexte statt `dynamic_cast` im Produkt-Hotpath
- Block-/Fastpathmetadaten direkt im validierten Deskriptor mitfuehren
- mindestens 200 MHz, Zielreserve 250 MHz unpaced

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

### Produktlauf A3 - KR-4972-Analyserunde [ausgefuehrt, Abnahme offen]

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
- KR-4972 bleibt bis zur Exportintegration offen

### Produktlauf B - nach KR-4967 bis KR-4970

Ein realer Capture-/Apply-Lauf ist mit v24 belegt. Zur Abnahme fehlen
weiterhin ein zweiter unabhaengiger Capture, Offline-Inspect/Verify, die
normativen per-Subsystem-Digests und der allgemeine Save-erhaltende
ProductHandoff.

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
