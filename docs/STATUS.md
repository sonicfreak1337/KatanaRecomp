# Projektstatus

Aktuelle interne Version: `v0.49.0`

Main-Ausgangsbasis dieser Implementierungs- und Produktrunde:

```text
8e5ab3145fb5fcafc056fd87025baf3497085342
Distinguish AOT template mismatches
```

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

## Aktueller Produktnachweis

`GameEntryHandoff` Schema 3 und Plattformzustandsvertrag 2 erfassen den
vollstaendigen Game-Entry-Zustand. Der reale NativeDisc-Capture entstand bei
Gastzyklus `415.233.270` und enthaelt 22 kanonische Geraete einschliesslich
Flash sowie fuenf ausstehende Ereignisse. Ein reales DirectBoot-Produkt hat
dieses `CompletePlatform`-Artefakt erfolgreich appliziert und anschliessend
Gastcode ausgefuehrt.

Der Apply-Pfad validiert alle gebundenen Payloads und Restoreplaene vor der
Mutation und prueft das Ergebnis durch semantischen Recapture. Noch offen sind
der streng globale `noexcept`-Commit ohne falliblen Schritt nach Commitbeginn
und normativ vergleichbare Digests pro Subsystem; das bleibt `KR-4967`.

Die historischen v24-Vergleichslaeufe:

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
Post-Entry-Budget und die Pflichtmeilensteinwertung bleiben in `KR-4966` offen.

Der aktuelle v28-Funktionslauf wurde auf der Main-Basis
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

## Abgeschlossene Sound-/AOT-Blocker und aktueller erster Blocker

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

Der erste aktive Produktblocker ist KR-4972:

```text
indirekter Call:       0x8C11088C
statisches Spielziel:  0x8C64784E
Dispatchlabel:         aot-template-mismatch
Materializergrund:     AotTemplateMismatch (14)
```

Fuer das unveraenderte Ziel im initialen Boot-Executable fehlt ein
generierter Block beziehungsweise passendes Runtime-AOT-Template. Der Zielcode
beginnt mit einem `BRA` auf den gemeinsamen Pfad `0x8C6478C2`; das deutet auf
einen Callback-/Shared-Tail-/Thunk-Vertrag, beweist aber weder die exakte
finale Funktionsgrenze noch die richtige allgemeine Modellierung. Interpreter,
JIT, Runtime-Decoder und Emulationsfallback sind keine zulaessige Reparatur.

## GameEntryHandoff-Stand

Belegt:

- `GameEntryHandoff` Schema 3
- Artefaktformat 2
- Plattformzustandsvertrag 2
- Runtime-ABI 63 und Portprojektvertrag 53
- Bindung an Contentidentitaet, Bootdatei, Konsolenprofil, Runtime-ABI und Descriptor
- CPU/MMU, RAM-Deltas, Scheduler sowie 22 Geraeteklassen einschliesslich Flash
- fuenf typisierte Ereignisse im realen NativeDisc-Capture
- vollstaendiger Capture und realer `CompletePlatform`-Apply im Direct-Produkt
- vollstaendige Vorvalidierung und semantischer Recapture nach Apply

Offen:

- deterministischer Doppel-Capture
- Artefakt-Inspect-/Verify-CLI
- normative NativeDisc-/DirectBoot-Paritaet
- streng globaler, nach Commitbeginn unfehlbarer `noexcept`-Commit
- per Subsystem normativ vergleichbare Digests

## GameProjectArtifact-Stand

`GameProjectArtifact` Format 1 ist ein besitzendes, versioniertes
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

`GameProjectFunctionBoundary::size` erreicht nun AnalysisOverride/-Seed,
Funktionskandidaten, CFG, IR und AOT. Diese oeffentliche Layoutaenderung
erhoeht Analyzer-ABI 2 auf 3.

## Maple-/VMU-Stand

Maple-/VMU-Zustand, MMIO, DMA und Ereignisrehydrierung sind in
`CompletePlatform` eingebunden. Die Save-Migration war im aktuellen realen
Lauf byteidentisch. Das beweist noch kein allgemeines, rollbackfreies
Produktprofil: Der Schutz einer nach dem Capture veraenderten Working Copy
und aktueller Nutzersaves bleibt in `KR-4970` offen.

## PVR-/Frame-Stand

Der alte NativeDiscBoot konnte ueber IP.BIN einen sichtbaren Direct-FB-Frame erzeugen. DirectBoot ueberspringt IP.BIN und soll deshalb nicht auf den Sega-Screen geprueft werden.

IP.BIN hinterlaesst jedoch relevante:

- PVR-/SPG-Register
- Framebufferbasis
- VRAM-Inhalt
- ASIC-/IRQ-Masken
- Schedulerereignisse

Der aktuelle DirectBoot appliziert den erfassten PVR-/SPG-/ASIC-Zustand.
v28 erhaelt nach dem Game Entry zwei gastbelegte Direct-Frames mit
`302.287` veraenderten Pixeln. Der Host-Presenter meldet weiterhin null
Frames, und alle 16 Fensteraufnahmen bleiben schwarz. Der technische
Framebufferfortschritt beweist daher noch keine normative Frameparitaet und
keinen sichtbaren Spielboot.

Der naechste visuelle DirectBoot-Meilenstein ist:

```text
FirstGameFramebufferWrite
oder
FirstTaFrame
danach FirstVisibleGameFrame
```

## Scheduler-/Gate-Stand

Das aktuelle Gate verwendet weiterhin 600 Millionen als absoluten finalen
Schedulerzyklus. Nach Restore bei `415.233.270` blieben dem DirectBoot deshalb
nur `184.766.730` Post-Entry-Zyklen. NativeDisc und DirectBoot erhalten damit
nicht dieselbe Gastarbeit.

v28 erreicht selbst dieses falsche absolute Maximum nicht: Der typisierte
AOT-Coveragefehler beendet den Lauf bei `553.990.562`, also nach
`138.757.292` Post-Entry-Zyklen. Der aktuelle Wrapper gibt trotz terminalem
Produktfehler Exitcode 0 weiter; auch diese falsche Gatewertung gehoert zum
KR-4966-Vertrag.

`KR-4966` muss eine Laufdauer ab Entry verwenden:

```text
target_cycle = restored_game_entry_cycle + requested_elapsed_guest_cycles
```

Zusaetzlich muss das externe Spielprojekt einen erforderlichen Meilenstein angeben.

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

Der aktuelle Handoff belegt damit keinen Performancegewinn.

v28 fuehrt bis zum funktionalen Fehler `10.079.932` Zentraldispatches aus und
damit `123.498` mehr als v26, bei `1.086.915` zusaetzlichen Gastzyklen. Die
extern gemessenen `5,275792 s` ergeben `26,3008 MHz` Post-Entry-Rate bis zum
Fehler gegen `5,746371 s` und `23,9578 MHz` bei v26, also provisorisch
`+9,78 %`. Dieser identisch restaurierte Fehler-zu-Fehler-Vergleich zeigt
eine kleine Laufzeitverbesserung,
ersetzt aber weder die v24-Baseline noch den relativen 600-Millionen-
Performancebenchmark.

Das Ziel ist:

```text
mindestens 200 MHz effektiv
mindestens 250 MHz unpaced als Reserve
```

Bereits vorhanden:

- statisches und dynamisches AOT-Tier
- validierte Ausfuehrungsdeskriptoren
- P1-/P2-Inline-Cache
- native interne Labels
- erste direkte native Calls
- konservative Registerlokalisierung
- `KATANA_STATIC_AOT_ESCAPE_STATS`

Offene Hauptpunkte:

- Function-AOT ist weiterhin stark an Single-Block-/Chainingvertraege gebunden
- direkte Calls committen zu oft pauschal Blockzeit
- Registerlokalisierung erfolgt nachtraeglich per C++-Stringersetzung
- Produktfastpaths verwenden `dynamic_cast`
- Blockmetadaten werden teilweise erneut per Map gesucht
- die Zentraldispatches muessen nach korrektem Post-Entry-Gate stark reduziert werden

Gastzyklen und Geraetelatenzen duerfen nicht kuenstlich reduziert werden.

## Build- und Workspace-Stand

```text
v28 warmer MSVC-Gateexport:       4,209083 s
  Analyse-/IR-Cache:              Hit
  Metadatencache:                 Hit
  AOT-Partitionscache:            42 / 42 Hits
v28 unveraenderter Hostbuild:     0,219272 s
v28 Produkt-EXE:             52.446.208 Bytes
v26 Produkt-EXE:             52.406.784 Bytes
historischer frischer Export:    169,3 s
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
`3.249.852.517` Bytes frei. Aktuell bleiben nur v28,
`.katana-port-work-e0e2126c4352`, Bootartefakt, Handoff, private GDI und
installierter Disc-Cache erhalten; die Entfernung ist nicht rueckgaengig.

Offen bleiben:

- Runtime-only-Rebuild plus Relink
- Hook-only-Build im externen Spielprojekt
- kalter Gesamtbuild
- schmalerer AOT-ABI-Header
- weniger generische Produkt-/Testlogik in der erzeugten `main.cpp`

## Aktiver kritischer Pfad

```text
KR-4965 ADXT-/mwSnd-Sound-Completion
  [abgeschlossen ueber engeren allgemeinen Blocker]

KR-4971 RuntimeOnly-AOT-Coverage fuer statisch identifizierbares Ziel
  [abgeschlossen]

KR-4972 Hashgebundene Shared-Callback-/Thunk-AOT-Coverage
  (zuerst)

Parallel offen:
KR-4966 korrektes relatives Post-Entry-Gate
KR-4967 strikter globaler noexcept-Commit und Subsystemdigests
KR-4970 allgemeines rollbackfreies Save-/VMU-Produktprofil
KR-4953 Doppel-Capture und Inspect-/Verify-CLI
KR-4962 normative NativeDisc-/DirectBoot-Paritaet

Danach:
KR-4955 -> KR-4956 -> KR-4957 -> KR-4958/KR-4959 -> KR-4960
KR-4954 -> KR-4961
KR-4964
```

## Geplante reale Produktlaeufe

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

### Lauf A3 - nach KR-4972

- denselben DirectBoot-Produktpfad verwenden
- Shared-Callback-/Thunk-Ziel ueber bewiesene externe Metadaten abdecken
- bis zum naechsten typisierten Produktresultat fortschreiten

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

- dass Sonic Adventure bootet
- dass NativeDisc und DirectBoot normativ paritaetisch sind
- dass der Sega-Screen im DirectBoot erwartet wird
- dass Maple der aktuelle Soundblocker ist
- dass AICA allein die Ursache ist
- dass der Completion-Writer oder der Flagwechsel direkt beobachtet wurde
- dass die zwei technischen Direct-Frames bereits sichtbar praesentiert wurden
- dass die neue Shared-Callback-/Thunk-AOT-Coverageluecke repariert ist
- dass die gemeldeten 119,64 MHz einen Performancegewinn darstellen
- dass die provisorischen 26,3008 MHz bis zum v28-Fehler eine
  600-Millionen-Performanceabnahme darstellen
- dass 36,8425 MHz spielbar oder echtzeitfaehig sind
- dass ein schwarzer Lauf mit Exitcode 0 ein erfolgreiches Produktgate ist
- dass eine einmal byteidentische Save-Migration ein allgemeines
  No-Rollback-Profil beweist

## Aktuelle Vertraege

```text
Runtime-ABI:                    63
Block-ABI:                       5
Analyzer-ABI:                    3
PlatformServices-ABI:           13
Backend-Interface-ABI:           8
Portprojektvertrag:             53
GameEntryHandoff-Schema:         3
GameEntryHandoff-Artefakt:       2
GameEntry-Plattformzustand:       2
Spielprojektvertrag:             2
GameProject-Artefaktformat:       1
Gastzyklusvertrag:                2
Native-AOT-Emissionsprofil:      8
Crash-Capsule-Vertrag:           1
Systemreplay-Schema:              8
Runtime-Probe-Schema:             5
Runtime-Probe-Device-Schema:      5
```

Historische Detailverlaeufe bleiben ueber Git-Historie, Changelog und Task-ID-Registry erhalten. Dieses Dokument ist ab jetzt die kompakte Wahrheit fuer den aktuellen `v0.49`-Produktbring-up.
