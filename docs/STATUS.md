# Projektstatus

Aktuelle interne Version: `v0.49.0`

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

Der juengste normale PAL-DirectBoot wurde aus `31c5575` mit MSVC erzeugt. Er verwendete kein externes `GameEntryHandoff`.

```text
Funktionen:            1.939
Partitionen:           42
Discinstallation:      3 Tracks / 521.461 Sektoren
Gastzyklen:            600.000.000
Hostzeit:              14,0113 s
effektive Gast-MHz:    42,8225
Zentraldispatches:     52.329.316
GD-ROM-Kommandos:      70
AICA-Audiopuffer:      180
Frames:                0
sichtbarer Screen:     keiner
letzter PC:            0x8C65A624
Exitcode:              0
first_problem:         none
```

Der Lauf beweist:

- Spielcode wird langfristig nativ ausgefuehrt.
- Es entsteht kein Missing-AOT.
- GD-ROM und AICA machen messbaren Fortschritt.
- Es liegt kein typisierter Runtime-, Dispatch- oder PVR-Abbruch vor.

Der Lauf beweist nicht:

- einen korrekten Post-IP.BIN-Ausgangszustand,
- einen erfolgreichen `CompletePlatform`-Handoff,
- einen sichtbaren Spiel-Frame,
- Echtzeitgeschwindigkeit.

`visible_screen=none` darf kuenftig nicht mehr als vollstaendiger Produkterfolg gelten.

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

Vorhanden:

- Schema 2
- Bindung an Contentidentitaet, Bootdatei, Konsolenprofil, Runtime-ABI und Descriptor
- CPU-State einschliesslich GPR-/FPU-Baenken, Spezialregistern, UTLB und Exceptionfeldern
- Main-RAM-/VRAM-/AICA-RAM-Deltas
- privates Artefaktformat mit Hash- und Groessenpruefung
- diagnostischer CPU/RAM-Apply
- kanonische Pflichtmenge von 21 Geraeteklassen
- Maple-Snapshot, Codec, passiver Restore und typisierte Event-Rehydration

Nicht vorhanden:

- vollstaendiger Geraete-Capture
- globaler atomarer Prepare-/Commit-Koordinator
- produktiver `CompletePlatform`-Apply
- PVR-/SPG-/ASIC-Adapter
- AICA-/G2-/DMAC-/Sound-Adapter
- produkt-sicheres Maple-/VMU-Profil
- konsistente Scheduler-/IRQ-Rehydration
- per Subsystem vergleichbare Entry-Digests

Der produktive Handoffpfad staged die vollstaendige Bindung, beendet sich danach aber absichtlich mit:

```text
game-entry-handoff-complete-platform-apply-unavailable
```

Ein Handoff-DirectBoot kann deshalb im aktuellen Stand definitionsgemaess nicht starten.

## Maple-/VMU-Stand

Der neue Maple-Zustandsvertrag kann diagnostisch erfassen:

- Bustopologie
- Controller-Framezaehler
- VMU-Quelle und Working Copy
- Schreibschutz und Dirty-Zustand
- Maple-MMIO
- aktive DMA und ausstehende Antworten
- frische Scheduler-Event-ID bei Rehydration

Offen:

- Einbindung in `GameEntryHandoff`
- global atomarer Restore
- Trennung `DiagnosticLossless` / `ProductHandoff`
- Schutz aktueller Nutzersaves

Ein dauerhaftes Produkt-Handoff darf keine alte VMU-Working-Copy aus dem Capture ueber aktuelle Spielstaende schreiben.

## PVR-/Frame-Stand

Der alte NativeDiscBoot konnte ueber IP.BIN einen sichtbaren Direct-FB-Frame erzeugen. DirectBoot ueberspringt IP.BIN und soll deshalb nicht auf den Sega-Screen geprueft werden.

IP.BIN hinterlaesst jedoch relevante:

- PVR-/SPG-Register
- Framebufferbasis
- VRAM-Inhalt
- ASIC-/IRQ-Masken
- Schedulerereignisse

Der aktuelle DirectBoot startet diese Komponenten frisch. Ein fehlender sichtbarer Spiel-Frame ist deshalb weiterhin mit dem unvollstaendigen Handoff vereinbar.

Der naechste visuelle DirectBoot-Meilenstein ist:

```text
FirstGameFramebufferWrite
oder
FirstTaFrame
danach FirstVisibleGameFrame
```

## Scheduler-/Gate-Stand

Das aktuelle Gate verwendet 600 Millionen Gastzyklen als absolutes Schedulermaximum.

Der NativeDisc-Game-Entry wurde bei Gastzyklus `415.233.270` erfasst. Nach einem echten Schedulerrestore blieben dadurch nur rund 184,8 Millionen Zyklen post-entry. NativeDisc und DirectBoot waeren damit nicht vergleichbar.

Der neue Vertrag muss eine Laufdauer ab Entry verwenden:

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

Aktuell:

```text
42,8225 MHz
52.329.316 Zentraldispatches
etwa 11,47 Gastzyklen pro Zentraldispatch
```

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
- 52 Millionen Zentraldispatches muessen stark reduziert werden

Gastzyklen und Geraetelatenzen duerfen nicht kuenstlich reduziert werden.

## Build-Stand

```text
Warmer Export MSVC:      2,258 s
Warmer Export clang-cl:  2,297 s
Erster MSVC-Umbau:       etwa 199,5 s
```

Der Warmexport ist brauchbar. Offen bleiben:

- Runtime-only-Rebuild plus Relink
- Hook-only-Build im externen Spielprojekt
- kalter Gesamtbuild
- schmalerer AOT-ABI-Header
- weniger generische Produkt-/Testlogik in der erzeugten `main.cpp`

## Aktiver kritischer Pfad

```text
KR-4965 Sound-Completion
  -> KR-4967 CompletePlatform-Koordinator
       -> KR-4968 Sound-/DMA-/IRQ-Handoff
       -> KR-4969 PVR-/Frame-Handoff
       -> KR-4970 Maple-/VMU-Product-Handoff
            -> KR-4952 / KR-4953
            -> KR-4962 Game-Entry-Paritaet und Produktboot

Parallel:
KR-4966 relatives Produktgate

Nach korrektem Produktboot:
KR-4955 -> KR-4956 -> KR-4957 -> KR-4958/KR-4959 -> KR-4960

Spielprojekt:
KR-4954 -> KR-4961

Final:
KR-4964
```

## Geplante reale Produktlaeufe

### Lauf A - nach Sound-Completion-Implementierung

Gewoehnlicher DirectBoot mit gleicher post-entry Gastarbeit.

Erwartung:

- Completion-Writer erreicht oder engerer Blocker belegt.

### Lauf B - nach CompletePlatform-Adaptern

- NativeDisc Capture am Game-Entry
- DirectBoot Apply bis vor ersten Spielblock
- CPU-, Memory-, PVR-, Sound/DMA-, Maple-, IRQ- und Scheduler-Digests vergleichen

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
- dass DirectBoot korrekt initialisiert ist
- dass der Sega-Screen im DirectBoot erwartet wird
- dass Maple der aktuelle Soundblocker ist
- dass AICA allein die Ursache ist
- dass 42,82 MHz spielbar oder echtzeitfaehig sind
- dass ein schwarzer Lauf mit Exitcode 0 ein erfolgreiches Produktgate ist

## Aktuelle Vertraege

```text
Runtime-ABI:                    62
Block-ABI:                       5
Analyzer-ABI:                    2
PlatformServices-ABI:           13
Backend-Interface-ABI:           8
Portprojektvertrag:             52
GameEntryHandoff-Schema:         2
GameEntryHandoff-Artefakt:       2
Spielprojektvertrag:             2
Native-AOT-Emissionsprofil:      8
Crash-Capsule-Vertrag:           1
```

Historische Detailverlaeufe bleiben ueber Git-Historie, Changelog und Task-ID-Registry erhalten. Dieses Dokument ist ab jetzt die kompakte Wahrheit fuer den aktuellen `v0.49`-Produktbring-up.
