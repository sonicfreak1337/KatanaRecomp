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

Aktueller Dokumentations-HEAD:

```text
69f3d122613338672d0e74d8a775c772d090746d
Record fresh DirectBoot PAL product run
```

Aktueller Code-HEAD:

```text
31c5575bd4e89c2bc85c20a14f4d1745fbbc987f
Bind lossless Maple state to game handoff
```

Der gewoehnliche PAL-DirectBoot ohne externes `GameEntryHandoff` erreicht:

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

Der Lauf beweist langen nativen Spielcodefortschritt ohne Missing-AOT oder typisierten Geraeteabbruch. Er beweist keinen korrekten Spielzustand und keinen erfolgreichen Handoff-DirectBoot.

## Neu belegter erster Produktblocker

Die derzeitige konkrete Laufgrenze ist ein ADXT-/mwSnd-Sound-Completion-Poll:

```text
Objekt:               0x8C8D3908
Completion-Flag:      0x8C8D3920
Poll-Callback:        0x8C666D42 (RTS; NOP)
Completion-Writer:    0x8C65A458
```

Der Waitpfad setzt `[object+24]` zunaechst auf `0` und pollt danach auf `1`. Der erwartete Writer schreibt `1` nach `[object+24]`. Saemtliche sechs statisch aufgeloesten Caller des Waitvertrags liegen in ADXT-/mwSnd-Soundpfaden.

Die wahrscheinlichste Fehlerzone ist deshalb:

```text
ADXT/mwSnd-Worker
  -> G2/AICA/DMAC
  -> Scheduler/IRQ-Fortschritt
  -> Completion-Writer
```

Noch nicht bewiesen ist, ob AICA selbst falsch arbeitet, der Gastworker nicht fortschreitet, ein DMA-/IRQ-Ereignis fehlt oder der Scheduler die Completion nicht zustellt. Maple/VMU ist fuer diesen konkreten Poll nicht belegt und darf nicht als erste Reparatur behandelt werden.

## Weiterhin realer DirectBoot-Mangel

IP.BIN hinterlaesst unter anderem PVR-/SPG-, VRAM-, AICA-, G2-/DMA-, ASIC-/IRQ-, Maple- und Schedulerzustand. Der aktuelle gewoehnliche DirectBoot startet diese Komponenten frisch. Der vorhandene produktive `CompletePlatform`-Pfad validiert und staged den Handoff, bricht aber absichtlich mit `game-entry-handoff-complete-platform-apply-unavailable` ab.

Der vollstaendige DirectBoot braucht daher weiterhin:

- atomaren, geraeteuebergreifenden Capture-/Apply-Koordinator
- typisierte Schedulerereignisse mit frischen lokalen Event-IDs
- PVR-/SPG-/VRAM-Zustand fuer den ersten Spiel-Frame
- AICA-/G2-/DMAC-/IRQ-Zustand fuer den Sound-Completion-Pfad
- produkt-sicheren Maple-/VMU-Zustand ohne Savegame-Rollback
- per Subsystem vergleichbare NativeDisc-/DirectBoot-Digests
- relative Gastzyklusdauer ab Game-Entry statt absolutes Schedulermaximum

## Verbindlicher v0.49-Kritischer Pfad

```text
KR-4965 ADXT/mwSnd-Sound-Completion bis zum Writer schliessen
  |
  +--> KR-4966 Post-Entry-Produktgate und erforderliche Meilensteine
  |
  +--> KR-4967 Atomarer CompletePlatform-Capture-/Apply-Koordinator
         |
         +--> KR-4968 AICA-/G2-/DMAC-/Scheduler-/IRQ-Handoff
         +--> KR-4969 PVR-/SPG-/ASIC-Handoff fuer ersten Spiel-Frame
         +--> KR-4970 Produkt-sicherer Maple-/VMU-Handoff
                    |
                    +--> KR-4952 / KR-4953 abschliessen
                           -> KR-4962 Game-Entry-Paritaet und Produktboot

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

Ziel ist nicht, AICA pauschal auszubauen. Zuerst wird der konkrete Completion-Vertrag geschlossen:

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

## Phase C - Produktgates

Boot- und Performancegates verwenden eine relative Laufdauer ab Game-Entry:

```text
target_cycle = restored_game_entry_cycle + requested_elapsed_guest_cycles
```

Das Spielprojekt definiert einen erforderlichen Meilenstein. Ein schwarzer Lauf darf nicht mit Exitcode 0 als vollstaendiger Produkterfolg gelten.

Empfohlene Resultate:

```text
0 = erforderlicher Produktmeilenstein erreicht
3 = Gastzyklusbudget erreicht, Meilenstein verfehlt
1 = typisierter Fehler
```

## Phase D - Xenon-artiger AOT-Hotpath

Der aktuelle Produktpfad bleibt zu blockorientiert. 52.329.316 Zentraldispatches fuer 600 Millionen Gastzyklen sind etwa ein Dispatch je 11,47 Gastzyklen.

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

Zwei reale Laeufe:

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
- vollständiger Post-IP.BIN-Handoff atomar angewendet
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
