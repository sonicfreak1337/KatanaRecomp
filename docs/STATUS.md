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

Die aktuellen realen Laeufe:

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

Beide Pfade enden am selben ADXT-/mwSnd-Wait. Der funktionierende
CompletePlatform-Apply hat den ersten Produktblocker daher nicht verschoben:
`KR-4965` bleibt zuerst.

## Aktueller erster Blocker: ADXT-/mwSnd-Sound-Completion

Der konkrete produktive Waitvertrag ist jetzt eingegrenzt:

```text
Objekt:               0x8C8D3908
Completion-Flag:      0x8C8D3920
Poll-Callback:        0x8C666D42
Completion-Writer:    0x8C65A458
```

Der Waitpfad setzt `[object+24]` zunaechst auf `0` und prueft danach fortlaufend auf `1`. Der Poll-Callback besteht nur aus `RTS; NOP`. Der erwartete Completion-Pfad schreibt tatsaechlich `1` nach `[object+24]`. Alle sechs statisch aufgeloesten Caller dieses Waitvertrags liegen in ADXT-/mwSnd-Soundpfaden.

Die wahrscheinlichste Fehlerzone ist:

```text
ADXT/mwSnd-Worker
  -> G2/AICA/DMAC
  -> Scheduler/IRQ
  -> Completion-Writer
```

Noch nicht bewiesen ist:

- ob AICA selbst falschen Zustand liefert,
- ob ein G2-/DMAC-Transfer nicht abschliesst,
- ob der Gastworker nicht geplant oder fortgesetzt wird,
- ob ein IRQ fehlt oder falsch quittiert wird,
- ob der Scheduler eine Completion nicht zustellt.

Maple/VMU ist fuer diesen konkreten Poll nicht belegt und nicht der erste Reparaturfokus.

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

Der aktuelle DirectBoot appliziert den erfassten PVR-/SPG-/ASIC-Zustand,
erreicht aber weiterhin keinen Frame. Capture und Apply allein beweisen daher
noch keine normative Frameparitaet und keinen sichtbaren Spielboot.

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
KR-4965 ADXT-/mwSnd-Sound-Completion (zuerst)

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

### Lauf A - nach KR-4965

Gewoehnlicher DirectBoot mit gleicher post-entry Gastarbeit.

Erwartung:

- Completion-Writer erreicht oder engerer Blocker belegt.

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
