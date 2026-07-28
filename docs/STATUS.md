# Projektstatus

Aktuelle interne Version: `v0.49.0`

Aktuelle Basis:

```text
3714e26e117c609980b2b9be6ea0b85c2484c2dd
Refocus v0.49 plan on sound completion and product handoff
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

Der aktuelle v26-Funktionslauf wurde aus
`4cbab1e9c11320955fa8e18f66ae4b0e7e1cd0cb` erzeugt und ueber den echten
Produktinstaller mit der privaten PAL-Disc installiert:

| Metrik | DirectBoot-v26 |
|---|---:|
| Entry-/Restore-Zyklus | 415.233.270 |
| CompletePlatform-Apply | 22 Geraete, 5 Events |
| Endzyklus am typisierten Fehler | 552.903.647 |
| Post-Entry-Zyklen | 137.670.377 |
| externe Walltime bis Fehler | 5,746371 s |
| warmer unveraenderter Build | 0,239825 s (`ninja: no work to do`) |
| Zentraldispatches | 9.956.434 |
| Post-Entry-Zyklen pro Dispatch | 13,83 |
| G2-Kanal 0 | `active=0`, `remaining=0` |
| GD-ROM-Kommandos | 72 |
| AICA-Audiopuffer | 165 |
| PVR-Gast-/Direct-Frames | 2 / 2 |
| veraenderte Direct-Pixel | 302.287 |
| Hostframe / sichtbarer Screen | 0 / keiner |
| terminales Dispatchlabel | `byte-identity-mismatch` (irrefuehrend) |
| interner Materializergrund | `AotTemplateMismatch` (14) |
| Callsite / Ziel | `0x8C602B0A` / `0x8C010F22` |

Die Walltime endet vor dem vorgesehenen Budget und ist kein
600-Millionen-Performancebenchmark. Sechzehn reale Fensteraufnahmen bis
`5,323 s` blieben schwarz.

## Abgeschlossener Soundblocker und aktueller erster Blocker

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

Beide Ursachen sind generisch repariert. v26 beendet G2-Kanal 0 und
verlaesst `0x8C666D42`, ohne einen Hostpatch am Completion-Flag oder eine
Titeladresse im Kern. Der Writer `0x8C65A458` beziehungsweise der konkrete
Flagwechsel wurde nicht separat instrumentiert. KR-4965 ist gemaess seiner
Alternativabnahme abgeschlossen, weil ein neuer, engerer allgemeiner
Blocker belegt ist.

Der erste aktive Produktblocker ist KR-4971:

```text
indirekter Call:       0x8C602B0A
statisches Spielziel:  0x8C010F22
Dispatchlabel:         byte-identity-mismatch (irrefuehrend)
Materializergrund:     AotTemplateMismatch (14)
Materializer:          2 Anfragen, 1 Erfolg, 1 Miss
RuntimeOnly-Anteil:    282.818 ppm
```

Fuer das unveraenderte Ziel im initialen Boot-Executable fehlt ein
generierter Block beziehungsweise passendes Runtime-AOT-Template. Die
terminale Diagnose kollabiert `AotTemplateMismatch` derzeit falsch zu einem
Byteidentitaetsfehler; ein Gastbytewechsel ist nicht belegt. Die Reparatur
seedet das Ziel ueber hash-/bytegebundene Metadaten des externen
Spielprojekts statisch in Analyse und AOT. Interpreter, JIT, Runtime-Decoder
und Emulationsfallback sind keine zulaessige Reparatur.

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
v26 erzeugt erstmals nach dem Game Entry zwei gastbelegte Direct-Frames mit
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

v26 erreicht selbst dieses falsche absolute Maximum nicht: Der typisierte
RuntimeOnly-AOT-Fehler beendet den Lauf bei `552.903.647`, also nach
`137.670.377` Post-Entry-Zyklen. Der aktuelle Wrapper gibt trotz terminalem
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

v26 fuehrt bis zum funktionalen Fehler `9.956.434` Zentraldispatches aus,
entsprechend `13,83` Post-Entry-Zyklen je Dispatch. Die extern gemessenen
`5,746371 s` bis zum vorzeitigen Fehler ergeben keinen vergleichbaren
600-Millionen-Benchmark und ersetzen die v24-Baseline nicht.

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

- `GameProjectFunctionBoundary::size` erreicht den Analyzer nicht
- Function-AOT ist weiterhin stark an Single-Block-/Chainingvertraege gebunden
- direkte Calls committen zu oft pauschal Blockzeit
- Registerlokalisierung erfolgt nachtraeglich per C++-Stringersetzung
- Produktfastpaths verwenden `dynamic_cast`
- Blockmetadaten werden teilweise erneut per Map gesucht
- die Zentraldispatches muessen nach korrektem Post-Entry-Gate stark reduziert werden

Gastzyklen und Geraetelatenzen duerfen nicht kuenstlich reduziert werden.

## Build- und Workspace-Stand

```text
Warmer Export:           5,3 s
Frischer Export:       169,3 s
Produkt-EXE:      52.404.736 Bytes
```

Beim konservativen Cleanup wurden `16.467.100.969` Bytes eindeutig
regenerierbarer Build-, Publish- und Testartefakte entfernt. Retailquellen,
aktuelle Referenzen und Nutzerdaten blieben erhalten.

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

### Lauf A2 - nach KR-4971

- derselbe DirectBoot-Produktpfad
- hash-/bytegebundenes statisches AOT fuer das identifizierte Ziel
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
- dass die RuntimeOnly-AOT-Coverageluecke repariert ist
- dass die gemeldeten 119,64 MHz einen Performancegewinn darstellen
- dass 36,8425 MHz spielbar oder echtzeitfaehig sind
- dass ein schwarzer Lauf mit Exitcode 0 ein erfolgreiches Produktgate ist
- dass eine einmal byteidentische Save-Migration ein allgemeines
  No-Rollback-Profil beweist

## Aktuelle Vertraege

```text
Runtime-ABI:                    63
Block-ABI:                       5
Analyzer-ABI:                    2
PlatformServices-ABI:           13
Backend-Interface-ABI:           8
Portprojektvertrag:             53
GameEntryHandoff-Schema:         3
GameEntryHandoff-Artefakt:       2
GameEntry-Plattformzustand:       2
Spielprojektvertrag:             2
Gastzyklusvertrag:                2
Native-AOT-Emissionsprofil:      8
Crash-Capsule-Vertrag:           1
Systemreplay-Schema:              8
Runtime-Probe-Schema:             5
Runtime-Probe-Device-Schema:      5
```

Historische Detailverlaeufe bleiben ueber Git-Historie, Changelog und Task-ID-Registry erhalten. Dieses Dokument ist ab jetzt die kompakte Wahrheit fuer den aktuellen `v0.49`-Produktbring-up.
