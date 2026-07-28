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
3714e26e117c609980b2b9be6ea0b85c2484c2dd
Refocus v0.49 plan on sound completion and product handoff
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

### CompletePlatform-v24-Produktevidenz

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

## Neu belegter erster Produktblocker

Die derzeitige konkrete Laufgrenze ist ein ADXT-/mwSnd-Sound-Completion-Poll:

```text
Objekt:               0x8C8D3908
Completion-Flag:      0x8C8D3920
Poll-Callback:        0x8C666D42 (RTS; NOP)
Completion-Writer:    0x8C65A458
```

Der Waitpfad setzt `[object+24]` zunaechst auf `0` und pollt danach auf `1`. Der erwartete Writer schreibt `1` nach `[object+24]`. Saemtliche sechs statisch aufgeloesten Caller des Waitvertrags liegen in ADXT-/mwSnd-Soundpfaden.

Der gleiche Stillstand ist sowohl im NativeDiscBoot als auch im
CompletePlatform-DirectBoot reproduziert. Beide aktuellen Produktlaeufe
enden bei `0x8C666D42`. Damit bleibt KR-4965 der **erste aktive
Produktblocker**; der vollstaendige Plattform-Handoff hat ihn nicht
aufgeloest.

Die wahrscheinlichste Fehlerzone ist deshalb:

```text
ADXT/mwSnd-Worker
  -> G2/AICA/DMAC
  -> Scheduler/IRQ-Fortschritt
  -> Completion-Writer
```

Noch nicht bewiesen ist, ob AICA selbst falsch arbeitet, der Gastworker nicht fortschreitet, ein DMA-/IRQ-Ereignis fehlt oder der Scheduler die Completion nicht zustellt. Maple/VMU ist fuer diesen konkreten Poll nicht belegt und darf nicht als erste Reparatur behandelt werden.

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
- KR-4962 bleibt bis zur belegten NativeDisc-/DirectBoot-Paritaet und einem
  ersten DirectBoot-Frame offen.

## Verbindlicher v0.49-Kritischer Pfad

```text
KR-4965 ADXT/mwSnd-Sound-Completion bis zum Writer schliessen [FIRST, offen]
  |
  +--> KR-4966 Post-Entry-Produktgate und erforderliche Meilensteine [offen]
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

Ziel ist nicht, AICA pauschal auszubauen. NativeDiscBoot und
CompletePlatform-DirectBoot reproduzieren denselben ADXT-/mwSnd-Poll; daher
wird zuerst dieser konkrete Completion-Vertrag geschlossen:

1. Ausloeser des ADXT-/mwSnd-Workers bestimmen.
2. Zugehoerigen G2-/AICA-/DMAC-Transfer und erwartete IRQ-/Schedulerkante identifizieren.
3. Beweisen, warum `0x8C65A458` nicht erreicht wird.
4. Allgemeine Ursache reparieren.
5. Erst danach einen einzigen normalen Produktlauf bis zum gleichen post-entry Gastzyklusziel ausfuehren.

Akzeptanz:

- Completion-Flag wechselt durch den echten Gastwriter von `0` auf `1`, oder
- ein neuer, engerer allgemeiner Blocker ist belegt.
- Kein Hostpatch schreibt das Flag direkt.
- Keine private Adresse wird in generischen Produktcode eingebaut.

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

Der aktuelle generierte DirectBoot-Gatepfad stoppt noch beim absoluten
Schedulerstand `600.000.000`. Er fuehrt dadurch nach dem Restore nur
`184.766.730` Gastzyklen aus und darf seine terminal ausgegebenen
`119,64 MHz` nicht als vergleichbaren Leistungswert verwenden. KR-4966
muss mindestens Startzyklus, post-entry Gastzyklen, Hostzeit und daraus
berechnete effektive Gast-MHz berichten.

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

- explizite Funktionsgroessen wirklich an den Analyzer weitergeben
- `NativeEntrySafe`, `DirectCallEligible`, `CompletionDeferrable` und `RequiresSafepointBeforeEntry` trennen
- bekannte Calls und Returns innerhalb nativer Funktionsregionen halten
- IR-/Liveness-basierte Registerlokalisierung statt C++-Stringersetzung
- konkrete FastRuntime-Kontexte statt `dynamic_cast` im Produkt-Hotpath
- Block-/Fastpathmetadaten direkt im validierten Deskriptor mitfuehren
- mindestens 200 MHz, Zielreserve 250 MHz unpaced

Gastzyklen oder Geraetelatenzen werden nicht kuenstlich reduziert.

## Boot-Testplanung

Das Endprodukt ist der Test. Es werden keine Tests pro Geraetefeld oder Hilfsfunktion angelegt.

### Produktlauf A - nach KR-4965

Gewoehnlicher DirectBoot, gleiche post-entry Gastarbeit wie die Baseline.

Ziel:

- Sound-Completion-Writer erreicht oder engerer Blocker belegt
- keine Aussage ueber vollstaendigen Handoff

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
