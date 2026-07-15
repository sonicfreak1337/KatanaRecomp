# Changelog

## [Unreleased]

### Hinzugefuegt

- KR-3201: Eine IR-basierte Backend-Schnittstelle validiert Programme und Einstiegspunkte zentral und trennt Deklarationen, Funktionen sowie Metadaten in deterministisch zusammensetzbare Emissionsabschnitte.
- KR-3202: Der bestehende C++-Emitter ist als Referenzbackend hinter die modulare Schnittstelle migriert; die bisherige `emit_cpp_program`-API bleibt als kompatibler, getesteter Einstieg erhalten.
- KR-3203: Backend-Interface-ABI, Runtime-ABI und explizite Faehigkeitsmasken werden vor der Emission geprueft; Fehler nennen Backend sowie den abweichenden Vertrag, bevor Code erzeugt wird.
- KR-3204: Eine versionierte Block-ABI trennt virtuelle und kanonische physische Gastadressen, typisiert alle Blockendklassen und synchronisiert CPU-, Delay-Slot-, Ausnahme- sowie Schedulerzustand an Backend- und Fallbackgrenzen.

### Behoben

- Phase-6-Gate-Nacharbeit: Ein unveraenderter CPU-Zustand gilt nicht mehr als Blockausfuehrung, die dokumentierte CCR-Invalidierung ist verpflichtend, nur Calls im kopierten Einstiegsblock blockieren die Probe, und GDI-Tracks koennen weder die Descriptorgrenze noch die vollstaendige Hash- und Cleanup-Pruefung umgehen.

## [0.31.0] - 2026-07-16

### Hinzugefuegt

- KR-3101: Ein zentraler, hostzeitfreier Event-Scheduler ordnet Callbacks deterministisch nach 64-Bit-Gastzyklus und Ereignis-ID, unterstuetzt Cancellation und verschachtelte Planung und stoppt mit sichtbarem Status an einem expliziten Ereignisbudget.
- KR-3102: Drei SH-4-TMU-Kanaele liefern gastzyklusgenauen Countdown, Pck-/RTC-Teiler, Auto-Reload und sichtbare UNF-/UNIE-Zustaende; eine hostzeitfreie 256-Hz-RTC zaehlt Kalender, Schaltjahre, Carry- und Periodic-Ereignisse deterministisch.
- KR-3103: Vier schedulergetaktete SH-4-DMAC-Kanaele bilden SAR/DAR/DMATCR/CHCR/DMAOR, Transfergroessen, Adressmodi, Prioritaeten sowie sichtbare TE-/IE-/AE-/NMIF-Zustaende ueber das 32-Bit-MMIO-Registerfenster ab.
- KR-3104: Ein zentraler Plattformrouter spiegelt TMU-, RTC-, DMTE-, DMAE- und drei feste Dreamcast-IRL-Quellen an deterministischen CPU-Safepoints mit offiziellen INTEVT-Codes, gruppierten Prioritaeten und levelartiger Quittierungssemantik.
- KR-3105: Eine gemeinsame Medienuhr erzeugt driftfreie rationale Video- und Audiokadenzen auf Gastzyklen, typisierte Backend-Callbacks, stabile Gleichzyklusreihenfolge sowie sichtbaren Budget-, Stop-, Reset- und Fehlerzustand.
- Das lokale Phase-6-Gate erzeugt aus dem allgemein ermittelten Disc-Bootblock eine temporaere, wirklich kompilierte Einblock-Probe, fuehrt sie mit festem Gastzyklusbudget aus und schreibt einen redigierten, deterministischen JSON-Bericht. Scheduler, echter asynchroner GDI-Read, TMU, DMA, CCR-Invalidierung und Abschlussinterrupt werden messbar erfasst.
- Ein minimales SH-4-CCR-Modell stellt den fuer den Disc-Boot beobachteten 32-Bit-Registerzugriff bereit, weist reservierte Bits und falsche Breiten sichtbar ab und zaehlt die selbstloeschende Instruktionscache-Invalidierung.
- Ein frei verteilbarer synthetischer Homebrew-Vertical-Slice bootet ueber den normalen Plattformpfad und weist Eingabe, ein farbiges PVR-Primitiv sowie nicht-stummes AICA-Audio in einem gemeinsamen gastzyklusbegrenzten Lauf nach.

### Geaendert

- Lokale Buildausgaben werden auf genau ein ignoriertes Multi-Config-Verzeichnis `build-current/` begrenzt; 14 alte task-, versions- und konfigurationsspezifische Buildbaeume wurden entfernt. Bis Alpha liegen darin nur Debug-Artefakte; die spaetere Release-Konfiguration nutzt keinen zweiten Buildbaum.
- Mehrsitzige GDI-Quellen verwenden den letzten Datentrack als primaere Disc-Sitzung. ISO9660 kann die Position des Primary Volume Descriptors und die LBA-Basis der Extents getrennt abbilden, sodass sowohl relative synthetische als auch absolute Dreamcast-GD-Extents ohne Trackumbau funktionieren.
- Bis einschliesslich v0.44.0 verwenden lokale Pre-Alpha-Gates genau einen frischen Debug-Build. Regulare Release-Builds sowie verpflichtende Windows-/Linux-CI kehren erst beim Alpha-Gate v0.50.0 zurueck.
- Das v0.31.0-Gate besteht mit 122/122 Tests im frischen Debug-Build. Zwei identische lokale Sonic-Adventure-Laeufe erreichen bytegleich und ohne Quellaenderung `SA_PHASE6_MAIN_EXECUTION_STARTED`.

### Behoben

- KR-3101-Nacharbeit: Rekursive `advance_to()`-/`advance_by()`-Aufrufe und `reset()` aus Scheduler-Callbacks werden sichtbar abgewiesen. Die Gastzyklusuhr kann dadurch nicht mehr durch ein verschachteltes Advance zurueckspringen; verschachtelte Planung und Cancellation bleiben erlaubt.
- KR-3105-Nacharbeit: Laufgenerationen verhindern doppelte oder nicht mehr abbrechbare Video-/Audioereignisse, wenn ein Callback die Medienuhr stoppt, neu startet oder zuruecksetzt.

## [0.30.0] - 2026-07-15

### Hinzugefuegt

- Eine zentrale lokale Sonic-Adventure-Akzeptanzstrategie definiert genau ein kumulatives, messbares End-to-End-Gate je abgeschlossener Phase von v0.31.0 bis v0.44.0 sowie das Alpha-Gate v0.50.0. Der v0.30.0-GDI-Smoke wird erst bei v0.31.0 revalidiert; einzelne Tasks und Zwischenreleases behalten ihre bestehenden kleineren Tests.

- KR-3001: Eine gemeinsame read-only DiscSource-Abstraktion stellt bereichsgepruefte Speicher- und Hostdateiquellen mit expliziter semantischer Identitaet bereit; Hostpfade sind weder Identitaet noch Schreibziel.
- KR-3002: Ein GD-ROM-Laufwerksmodell verarbeitet Ready-, Status-, Kapazitaets- und Sektorlesekommandos mit Big-Endian-Kapazitaetsantworten und expliziten No-Media-, Feld-, Befehls- und Bereichsfehlern.
- KR-3003: Ein read-only ISO9660-Pfad validiert den Primary Volume Descriptor und Both-Endian-Directory-Records, normalisiert Versionssuffixe und liest Dateien case-insensitive auch aus Unterverzeichnissen.
- KR-3004: Asynchrone GD-ROM-Requests verwenden ausschliesslich explizite Gastzyklen, Basislatenz und Sektorkosten; Fertigstellungen sind stabil geordnet und Zeitrueckspruenge sowie Zyklusueberlaeufe sichtbar.
- KR-3005: Der `.gdi`-Parser liest quoted relative Trackpfade, Typ, Sektorformat, Offset und Reihenfolge in ein Modell mit Zeilenprovenienz und weist fehlende Dateien, Trackkonflikte sowie unplausible Dateigroessen read-only ab.
- KR-3006: GDI-Mehrdateiquellen liefern pfadunabhaengige Inhaltsidentitaet, Raw-Audiotracks und normalisierte 2048-Byte-Datensektoren ueber dieselbe DiscSource-, GD-ROM- und ISO9660-Kette; der Read-only-Vertrag ist dokumentiert und getestet.

### Geaendert

- Alte versionierte `.katana_backup_*`-Migrationssnapshots wurden aus dem aktuellen Repository-Baum entfernt. Die Arbeitsregeln erlauben nur noch genau ein unversioniertes Quellbackup des neuesten committed Stands und schliessen Build-, Referenz- sowie private Spieldaten aus.
- Die vollstaendige Regression umfasst 114 Tests und besteht in frischen lokalen Debug- und Release-Builds. CI bleibt bis zum Alpha-Gate optional; der vollstaendige lokale Sonic-Adventure-Test ist erst beim kumulativen Phase-6-Gate v0.31.0 verpflichtend.

### Behoben

- PVR-Framebuffer-Geometrie, VRAM-Endadresse und RGBA-Allokationsgroesse verwenden nun explizit gepruefte `size_t`-Multiplikationen und -Additionen; kuenstlich grosse Eingaben koennen die Stride- oder Grenzpruefung nicht mehr per Integerueberlauf umgehen.
- Die im README sichtbare Testsuitenzahl stimmt wieder mit dem v0.29.0-Gate von 108 Tests ueberein.

## [0.29.0] - 2026-07-15

### Hinzugefuegt

- KR-2901: Ein 32-KiB-AICA-Registerfenster unterstuetzt little-endian Byte-, Halfword- und Word-Zugriffe ueber alle direkten SH-4-Segmentaliase, deterministischen Reset und sichtbare Schreibereignisse.
- KR-2902: Ein zustandsbehafteter Sampledecoder verarbeitet signed PCM8, little-endian PCM16 und AICA-ADPCM mit korrekter Low-/High-Nibblefolge, Predictor-/Step-Clamping, Streaming-Fortsetzung und Reset.
- KR-2903: Ein ganzzahliger Stereo-Mixer kombiniert Mono-Voices mit Gain, Pan, Nullauffuellung und 16-Bit-Saettigung; eine Host-Audio-Abstraktion uebergibt validierte Stereo-Frames an ein deterministisches Recording-Backend.
- KR-2904: Das v0.29-Profil implementiert einen ausdruecklichen HLE-Audiovertrag ohne vorgetaeuschte ARM7-Ausfuehrung, drei deterministische AICA-Timer und maskierbare, quittierbare Interrupts; nicht implementiertes ARM7-LLE scheitert sichtbar.

### Geaendert

- Die vollstaendige Regression umfasst 108 Tests und besteht in frischen lokalen Debug- und Release-Builds. CI bleibt gemaess Projektentscheidung bis zum Alpha-Gate optional.

## [0.28.0] - 2026-07-15

### Hinzugefuegt

- KR-2801: Ein busfaehiges PVR-Registerminimum bildet ID, Revision, Framebuffer-, Video- und Rendersteuerregister ueber alle direkten Segmente ab. Breitenfehler, read-only-Register, Softreset und Renderanforderungen sind sichtbar.
- KR-2802: Ein Framebuffer-Pfad wandelt RGB565, ARGB1555 und RGB888 mit Stride- und VRAM-Grenzpruefung deterministisch in RGBA-Frames um und zaehlt praesentierte Frames.
- KR-2803: Ein strukturierter Tile-Accelerator-Grundpfad sammelt Triangle-Strips in Opaque-, Punch-through- und Translucent-Listen, erzwingt Listenreihenfolge und Framegrenzen und verwirft keine Primitive still.
- KR-2804: RGB565-, ARGB1555- und ARGB4444-Texturen werden begrenzt in RGBA dekodiert; eine Render-Backend-Abstraktion mit deterministischem Recording-Backend uebergibt Frames und Texturen ohne Host-Grafikabhaengigkeit.

### Geaendert

- Die vollstaendige Regression umfasst 104 Tests und besteht in frischen lokalen Debug- und Release-Builds. CI bleibt gemaess Projektentscheidung bis zum Alpha-Gate optional.

## [0.27.0] - 2026-07-15

### Hinzugefuegt

- KR-2701: Ein deterministischer Maple-Bus adressiert vier Ports mit jeweils sechs Units, transportiert explizite Request-/Response-Frames und protokolliert erfolgreiche Transaktionen mit stabilen Sequenznummern.
- KR-2702: Ein Controllergeraet kodiert Tasten aktiv-low, Trigger und zwei Analogachsen in Maple-Condition-Frames. Das Host-Input-Interface ist austauschbar; endliche Replays liefern framegenaue Tests und fallen bei Erschoepfung nicht still auf Live-Eingabe zurueck.
- KR-2703: Ein minimales VMU-Geraet bietet 256 Bloecke zu 512 Byte, Little-Endian-Blocktransfers, Schreibschutz und eine Copy-on-write-Arbeitskopie, ohne das synthetische Quellabbild zu veraendern.

### Geaendert

- Die vollstaendige Regression umfasst 100 Tests und besteht in frischen lokalen Debug- und Release-Builds. CI bleibt gemaess Projektentscheidung bis zum Alpha-Gate optional.

### Behoben

- KR-2607: `FCNVDS` fuehrt nun auch das konvertierte Single-Precision-Ergebnis durch die zentrale `FPSCR.DN`-Behandlung. Positive und negative subnormale Ergebnisse werden bei `DN=1` auf vorzeichenbehaftete Null gespult und sind regressionsgesichert.

## [0.26.0] - 2026-07-15

### Hinzugefuegt

- KR-2601/2602: Eine Dreamcast-Plattformkonfiguration laedt Raw- und ELF-`ExecutableImage`-Segmente samt BSS in Hauptspeicher, validiert den Einstieg und setzt PC, Stack, VBR, SR und FPSCR deterministisch.
- KR-2603: Der Boot-Handoff liefert reproduzierbare Eintraege fuer Firmwareart, Einstieg, Segmente und Bereitschaft.
- KR-2604: Eine Architekturentscheidung macht BIOS-freien Homebrew-Direktboot zum Standard. HLE- und LLE-Modi werden vor Teilwirkungen sichtbar abgewiesen; proprietaere Firmware bleibt eine rein lokale, optionale Zukunftseingabe.
- KR-2605: `PREF @Rn` besitzt das exakte Opcode-Muster fuer alle Register, IR- und Codegen-Unterstuetzung sowie einen kompilierten Runtime-Pfad, der normale und Store-Queue-Adressen unterscheidet.
- KR-2606: Ein zustandsbehaftetes 128-KiB-Flashgeraet erzwingt Unlock-, Programmier- und Sektorloeschsequenzen, 1-zu-0-Bituebergaenge, Schreibschutz und Reset. Aenderungen liegen ausschliesslich in einer Arbeitskopie.

### Geaendert

- Die Runtime-ABI steigt auf Version 8 und nimmt deterministische PREF-Beobachtung in den CPU-Zustand auf.
- Die vollstaendige Regression umfasst 97 Tests. CI bleibt gemaess Projektentscheidung bis zum Alpha-Gate optional.

## [0.25.0] - 2026-07-15

### Hinzugefuegt

- KR-2501: `FSCA` und `FSRRA` laufen durch Decoder, IR, Runtime und kompilierten generierten C++-Code. Die vier Quadrantenanker sind exakt; weitere Werte werden mit einer dokumentierten Single-Precision-Toleranz geprueft.
- KR-2502: `FIPR` bildet alle vier FV-Ansichten ab und bleibt bei ueberlappenden Vektoren deterministisch. `FTRV` liest die 4x4-XMTRX aus der XF-Hintergrundbank und schreibt ein zuvor gesichertes FV-Ergebnis.
- KR-2503: Die Grafikoperationen verwenden die zentrale NaN-Kanonisierung, DN-Behandlung und Host-Rundungsumgebung; reservierte RM-Werte und PR=1 werden vor Teilwirkungen abgewiesen.
- KR-2504: Numerische Runtime-Vektoren und ein kompilierter generierter End-to-End-Pfad pruefen Winkel, reziproke Quadratwurzel, Skalarprodukt, Matrixtransformation, Registerueberlappung und Bankzugriffe.

### Geaendert

- Die Instruktionsmetadaten umfassen 145 normale Regeln und der ISA-Bericht 149 implementierte Instruktionsarten.
- CI bleibt gemaess Projektentscheidung bis zum Alpha-Gate optional; v0.25.0 wird durch frische lokale Debug- und Release-Builds freigegeben.

## [0.24.0] - 2026-07-15

### Hinzugefuegt

- KR-2307: Generierter C++-Code wird fuer Speicheradressfehler in den Delay Slots von `BRA`, `BSR`, `JMP`, `JSR`, `RTS` und `RTE` kompiliert und tatsaechlich ausgefuehrt. Die Regression prueft Owner-`SPC`, vorbereitete Registereffekte, restaurierten RTE-Status samt Registerbank, illegale Slot-Instruktionen und die Propagation durch verschachtelte generierte Aufrufe.
- Interrupttests decken gleiche Prioritaetslevel mit Quell-ID-Tie-Break, Level 0, Clamping auf Level 15, Prioritaetsupdates und `trap_pending` ohne gesetztes `BL` ab.
- KR-2401: `FPSCR.FR` schaltet die sichtbaren FR-/XF-Rohbitbaenke zentral und reversibel um; `FMOV` uebertraegt 32-Bit-Einzelwerte oder 64-Bit-Registerpaare gemaess `SZ`.
- KR-2402: Decoder, Katana-IR, Runtime und C++-Emitter fuehren Single-Precision-`FADD`, `FSUB`, `FMUL`, `FDIV`, `FMAC`, `FABS`, `FNEG`, `FSQRT`, Konstanten und FPUL-Transfers aus.
- KR-2403: `FCMP/EQ`, `FCMP/GT`, `FLOAT`, `FTRC`, `FCNVDS` und `FCNVSD` besitzen ausgefuehrte Runtime- und generierte End-to-End-Regressionen.
- KR-2404: Zentrale `FPSCR`-Masken und Zugriffe modellieren `PR`, `SZ`, `FR`, `DN` und `RM`; Round-to-Nearest und Round-to-Zero laufen in einer strikt gesetzten Host-Rundungsumgebung.
- KR-2405: Gepaarte FR-Register bilden Double Precision ab. Arithmetik, Vergleiche, Quadratwurzel, Konvertierungen und Little-Endian-64-Bit-Transfers sind abgedeckt.
- KR-2406: FPU-Disable-Ausnahmen sichern `EXPEVT`, Slotkontext und Owner-`SPC`; unzulaessige Moduskombinationen werden vor Teilwirkungen abgewiesen. Der generierte Pfad wird kompiliert und einschliesslich eines BRA-Delay-Slot-Fehlers ausgefuehrt.
- Die Release-Regression deckt FTRC-Grenzen und -Saettigung, vorzeichenbehaftete Null bei Division und Quadratwurzel, Quiet-/Signaling-NaNs, Infinity-Vergleiche, Double- und Konvertierungsrundung, vollstaendig ueberlappendes FMAC sowie 64-Bit-FMOV an Regionsgrenzen ab.
- `FPSCR.DN` spuelt denormalisierte Single- und Double-Operanden und -Ergebnisse auf vorzeichenbehaftete Null; `FABS`, `FNEG` und Rohbittransfers bleiben davon ausgenommen. Reservierte RM-Werte werden vor einer Rechenwirkung deterministisch abgewiesen.

### Geaendert

- Konsistente `Unknown`-IR darf den strukturierten Illegal-Instruction-Pfad des Codegenerators erreichen; widerspruechliche IR-Metadaten werden weiterhin abgelehnt.
- Die Runtime-ABI steigt auf Version 7 und zentralisiert `FPSCR`-Schreibzugriffe sowie FR-/XF-Bankwechsel.
- Die vollstaendige Regression umfasst nun 95 Tests und besteht in frischen lokalen Debug- und Release-Builds. CI bleibt gemaess Projektentscheidung bis zum Alpha-Gate optional.

## [0.23.0] - 2026-07-15

### Hinzugefuegt

- KR-2301: Relevante SH-4-Statusregisterfelder besitzen zentrale Masken und strukturierte Zugriffe fuer `IMASK`, `BL`, `MD`, `RB` und `FD`; Interruptmasken werden deterministisch auf vier Bit begrenzt.
- KR-2302: Ein gemeinsamer Exception-Eintritt sichert `SR`, `PC` und `R15` in `SSR`, `SPC` und `SGR`, setzt `EXPEVT` oder `INTEVT`, wechselt in den privilegierten geblockten Registerbankmodus und springt ueber `VBR` zum passenden Vektor. `TEA`, Ursache und Delay-Slot-Kontext bleiben maschinenlesbar erhalten.
- KR-2303: Ein testbarer Interrupt-Controller verwaltet Quellen deterministisch, aktualisiert doppelte Anforderungen, priorisiert nach Level und Quell-ID und akzeptiert nur Interrupts oberhalb von `IMASK` bei geloeschtem `BL`.
- KR-2304: Generiertes `TRAPA` und `RTE` verwenden den zentralen Runtime-Exception-Pfad. Trap- und Rueckkehrzustand, Registerbankwechsel und RTE-Delay-Slot-Reihenfolge werden nicht mehr separat im Emitter dupliziert.
- KR-2305: Generierte Speicherzugriffe ueberfuehren Ausrichtungs-, Bus- und Adressfehler in den CPU-Exception-Zustand. Fehler im Delay Slot sichern den Owner-PC; Exceptions propagieren durch generierte Funktionsaufrufe, statt als generische C++-Exceptions zu verschwinden.

### Geaendert

- Die Runtime-ABI steigt auf Version 6. `CpuState` enthaelt jetzt `TEA`, die letzte strukturierte Exception-Ursache und den Delay-Slot-Status; generierter Code bindet den zentralen Exception-Pfad ein und prueft ABI 6.
- Die vollstaendige Regression umfasst 91 Tests in Debug und Release. Historisch abgeschlossene Roadmap-Aufgaben fuer v0.11, v0.19 und v0.20 sind nun auch in `ROADMAP.md` explizit abgehakt.

## [0.22.0] - 2026-07-14

### Hinzugefuegt

- KR-2201: `katana::runtime::Memory` dekodiert Adressen jetzt ueber benannte, nicht ueberlappende 32-Bit-Regionen und delegiert Bytezugriffe an registrierbare `MemoryDevice`-Instanzen. `LinearMemoryDevice`, Read-only-Regionen, Little-Endian-Mehrbytezugriffe und auslesbare Regionsmetadaten bilden die Grundlage fuer die folgenden Dreamcast-Speicherbereiche.
- KR-2202: `map_dreamcast_main_ram` registriert ein gemeinsames, nullinitialisiertes 16-MiB-Backing in allen direkten Area-3-, U0/P0-, P1-, P2- und derzeitigen P3-No-MMU-Aliasfenstern. Schreibzugriffe durch jedes Fenster bleiben bytegenau sichtbar; P4 wird ausdruecklich nicht als RAM abgebildet.
- KR-2203: `map_dreamcast_vram` registriert ein gemeinsames 8-MiB-Backing ueber lineare 64-Bit- und bankinterleavte 32-Bit-Zugriffsfenster; `map_dreamcast_aica_ram` bildet 2 MiB Sound-RAM in allen direkten Spiegelungen ab. Beide Konfigurationen pruefen Kollisionen vor der ersten Registrierung.
- KR-2204: `map_dreamcast_bios` bildet ein optional bereitgestelltes 2-MiB-BIOS read-only ab; `map_dreamcast_flash` registriert ein separates beschreibbares 128-KiB-Flash-Backing. Fehlende Firmware wird deterministisch mit `0xFF` initialisiert, falsche Abbildgroessen und Kollisionen werden vor der ersten Registrierung abgelehnt.
- KR-2205: `MmioMemoryDevice` leitet 8-, 16- und 32-Bit-Zugriffe als genau einen breitenbewussten Callback weiter. Bestehende bytebasierte Speichergeraete behalten einen zentralen Little-Endian-Fallback; fehlende Lese- oder Schreibhandler und ueberlaufende Zugriffe schlagen sichtbar fehl.
- KR-2206: Der Speicherbus prueft standardmaessig die natuerliche Ausrichtung von Halfword- und Word-Zugriffen, meldet Adressfehler als strukturierte `MemoryAccessError`-Gruende und bietet optionale globale Traces sowie adress- und zugriffsgefilterte Watchpoints.
- KR-2207: Das v0.22-Release-Gate prueft die gesamte 8-MiB-Offsetabbildung des bankinterleavten 32-Bit-VRAM-Pfads, fuehrt frische Debug- und Release-Regressionen aus und etabliert GitHub Actions fuer Linux/GCC und Windows/MSVC.

### Geaendert

- Die Runtime-ABI steigt auf Version 5. Version 3 fuehrte den regionbasierten Bus ein, Version 4 breitenbewusste MMIO-Zugriffe und Version 5 strukturierte Zugriffsfehler, konfigurierbare Ausrichtung sowie Trace- und Watchpoint-Zustand in `Memory`. Normale Busse sind standardmaessig strikt ausgerichtet; der historische lineare 1-MiB-Speicher in `CpuState` bleibt bis zur SH-4-Adressfehlerphase explizit permissiv.

## [0.21.0] - 2026-07-14

### Hinzugefuegt

- KR-2101: Speicher, CPU-Zustand, Statusregisterzugriff und ungeloeste Kontrollflusspfade wurden aus dem generierten C++ in die eigene statische Bibliothek `KatanaRecomp::runtime` mit ABI-Version 1 ausgelagert. Generierte Programme pruefen die ABI beim Kompilieren und werden in allen End-to-End-Tests explizit gegen die Runtime gelinkt.
- KR-2102: Der zentrale CPU-Zustand besitzt jetzt benannte Registeranzahlen, getrennte `FR`-/`XF`-Rohbitbaenke und `INTEVT`. Die Layoutaenderung wird als Runtime-ABI Version 2 ausgewiesen und durch einen eigenen CPU-Zustands-Test abgesichert.
- KR-2103: `reset_cpu` stellt alle CPU-Register und Runtime-Flags deterministisch wieder her, akzeptiert optionale Startwerte fuer PC, R15, VBR, SR und FPSCR und bewahrt den Runtime-Speicher.

## [0.20.0] - 2026-07-14

### Hinzugefuegt

- KR-2001: Ein konservativer Constant-Folding-Pass faltet beweisbare status- und speicherneutrale 32-Bit-Integerausdruecke mit definiertem Wraparound.

- KR-2002: Copy Propagation ersetzt lokale Registerkopien nur bis zum naechsten Schreibzugriff auf Quelle oder Ziel und verwirft Aliase bei unbekannten Effekten.

- KR-2003: Dead-Code-Elimination entfernt innerhalb eines Blocks reine Registerdefinitionen, die vor jeder Nutzung eindeutig ueberschrieben werden.

- KR-2004: CFG-Simplifizierung entfernt unerreichbare Funktionsbloecke und kanonisiert Nachfolgerlisten deterministisch.

- KR-2005: Unmittelbar aufeinanderfolgende, adressgleiche 32-Bit-Stores und -Loads reichen den Registerwert weiter, ohne den sichtbaren Speicher-Read zu entfernen.

- KR-2006: Eine deterministische Pipeline fuehrt alle sicheren IR-Paesse in fester Reihenfolge aus, erlaubt Einzelschalter und Vorher-/Nachher-Dumps und kann per `--no-opt` vollstaendig deaktiviert werden.

## [0.19.0] - 2026-07-14

### Hinzugefuegt

- KR-1901: Jede Katana-IR-Instruktion traegt explizite Breiten fuer semantisches Ergebnis und Eingaben, kodierte Immediate- und Displacement-Felder, Speichertransfers und effektive Adressen.

- KR-1902: Statusregister-Lese- und Schreibeffekte fuer T, S, Q, M und vollstaendige SR-Transfers sind pro IR-Instruktion explizit modelliert.

- KR-1903: Speicherzugriffsart, Transferbreite, Zugriffszahl sowie Pre-Decrement- und Post-Increment-Registerupdates sind als IR-Seiteneffekte sichtbar.

- KR-1904: Delay Slots sind in der Katana-IR als normalisierte Owner/Slot-Beziehung mit gegenseitigen Instruktionsadressen dargestellt.

- KR-1905: Ein pro Funktion nutzbarer IR-Verifier prueft Struktur, Metadaten, Register, Kontrollflussziele und Delay-Slot-Beziehungen; ungueltige IR wird vor dem C++-Codegenerator abgelehnt.

- KR-1906: Katana-IR besitzt eine vollstaendige, deterministisch sortierte Textausgabe und eine maschinenlesbare JSON-Ausgabe ueber den neuen CLI-Befehl `ir-json`.

### Behoben

- Operandbreiten fuer DIV0S und RTE, S-abhaengige MACH/MACL-Wirkungen, Full-SR-Abfragen, registerabhaengiges Post-Increment sowie die opcodebasierte Delay-Slot-Verifikation bilden die SH-4-Semantik nun widerspruchsfrei ab.

- Sicher aufgeloeste indirekte Spruenge, Calls, Overrides und vollstaendig validierte Jump Tables werden bis zum deterministischen Fixpunkt in die rekursive Codeentdeckung zurueckgefuehrt. Eine gemeinsame committed-Code-Pruefung lehnt Zero-Fill- und Segmentgrenzen einheitlich ab; unbekannte Opcodes beenden ihren Analysepfad mit sichtbarer Diagnose.

- `jump` und `jump_table` sind fuer dieselbe Dispatch-Adresse nun gegenseitig ausgeschlossen; die Parserdiagnose nennt Datei, beide betroffenen Zeilen und die Adresse.

## [0.18.0] - 2026-07-14

### Hinzugefuegt

- KR-1801: Eine konservative lokale Konstantenpropagation verfolgt Immediate-Werte, Additionen und Registerkopien mit definiertem 32-Bit-Wraparound und verwirft Annahmen bei unmodellierten Effekten.

- KR-1802: Eine adressbezogene Registerwertanalyse erweitert sichere arithmetische und logische Transfers und zeichnet beweisbare oder explizit unbekannte Registerwerte an indirekten `JMP`-/`JSR`-Stellen auf.

- KR-1803: Einfache indirekte Calls und Spruenge werden nur bei beweisbaren konstanten Registerwerten in committed ausfuehrbaren Code aufgeloest; unbekannte oder ungueltige Ziele bleiben mit getrennten Gruenden sichtbar.

- KR-1804: Eine beschraenkte Jump-Table-Analyse validiert bekannte absolute 32-Bit-Tabellen vollstaendig und weist fehlende, ungerade oder nicht ausfuehrbare Ziele explizit zurueck.

- KR-1805: Eine strikt versionierte Override-Datei nimmt deterministisch sortierte Funktions-, Sprung- und Jump-Table-Hinweise auf und diagnostiziert unbekannte oder doppelte Angaben.

- KR-1806: Der Kontrollflussbericht trennt sichere und ungeloeste Ziele, nennt stabile Gruende und liefert passende Override-Hinweise fuer jede offene Stelle.

## [0.17.0] - 2026-07-14

### Behoben

- ELF32-SH-Einstiegspunkte werden nur noch uebernommen, wenn sie zwei Byte ausgerichtet sind und innerhalb committed Daten eines ausfuehrbaren Code-Segments liegen; Zero-Fill- und Segmentgrenzen werden diagnostisch abgelehnt.

### Hinzugefuegt

- KR-1701: Eine deterministische Worklist verfolgt bekannten Code ab den Einstiegspunkten eines Executable Images ueber direkte Spruenge, Calls, Fallthrough und Delay Slots, ohne nicht erreichbare Segmentbytes linear zu dekodieren.

- KR-1702: Rekursive Analyseergebnisse klassifizieren normalisierte Adressbereiche als erreichbaren Code, deklarierte Daten oder unbekannte Bytes; nicht erreichter Code und Zero-Fill bleiben sichtbar unbekannt.

- KR-1703: Funktionskandidaten tragen zusammengefuehrte Herkunftsevidenz aus Image-Einstiegspunkten, direkten Calls und Funktionssymbolen sowie eine deterministische Konfidenzstufe.

- KR-1704: Nicht durch die Worklist erreichte committed Bytes aus ausfuehrbaren Code-Segmenten werden separat als unerreichbare Codebereiche ausgewiesen, ohne Zero-Fill oder Datensegmente einzubeziehen.

- KR-1705: Mehrdeutige Adressen, die zugleich Funktionskandidat und Delay Slot sind, werden als deterministische Analysekonflikte ausgewiesen statt doppelt oder stillschweigend interpretiert.

- KR-1706: Ein deterministischer rekursiver Analysebericht erklaert Funktionsherkunft und Konfidenz sowie Code-/Datenbereiche, unerreichbaren Code und Analysekonflikte mit stabilen Adressen und Gruenden.

### Geaendert

- KR-1707: Basic-Block-, Funktions-, IR- und Codegen-Pfade verwenden die rekursive Worklist; `disasm` bleibt als linearer Diagnosemodus erhalten und `analyze <Manifest>` gibt den begruendeten Bericht aus. Version und Release-Dokumentation wurden auf v0.17.0 aktualisiert.

## [0.16.0] - 2026-07-14

### Hinzugefuegt

- KR-1601: Ein neutrales Executable-Image- und Segmentmodell bildet virtuelle Adressen, Dateioffsets, Speicher- und Dateigroessen, Code-/Datenklassifikation, Berechtigungen und Einstiegspunkte mit validierten 32-Bit-Adressbereichen ab.

- KR-1602: Ein konfigurierbarer Raw-Binary-Loader bildet Dateien mit Basisadresse, Segmentklasse, Berechtigungen und optionalem Einstiegspunkt auf das Executable-Image-Modell ab und meldet Pfad-, Offset- und Adressraumfehler sichtbar.

- KR-1603: Ein validierender Little-Endian-ELF32-SH-Loader uebernimmt `PT_LOAD`-Segmente, Einstiegspunkt, Datei-/Speichergroessen und `PF_R/PF_W/PF_X` in das neutrale Image-Modell und meldet Loaderfehler mit Datei, Offset und Ursache.

- KR-1604: Executable Images speichern deterministisch sortierte Funktions- und Objektsymbole; ELF32-`SHT_SYMTAB`/`SHT_DYNSYM` sowie optionale Katana-Map-Dateien werden mit Bindung, Groesse und diagnostischen Datei-/Zeilenfehlern geladen.

- KR-1605: ELF32-SH-`SHT_REL`-Eintraege werden im Image-Modell sichtbar; `R_SH_DIR32` und `R_SH_REL32` werden mit implizitem Addend angewendet, waehrend unbekannte Typen unveraendert als nicht unterstuetzt erhalten bleiben.

- KR-1606: Ein strikt versioniertes Projektmanifest v1 waehlt Raw- oder ELF32-SH-Eingaben, beschreibt Raw-Adresslayout und Berechtigungen, loest relative Eingabe- und Map-Pfade am Manifest auf und weist unbekannte oder doppelte Felder diagnostisch zurueck.

### Geaendert

- KR-1607: Der normale CLI-Analyse- und Codegen-Pfad laedt Raw-Binaries als Executable Images; der Analyzer verarbeitet nur ausfuehrbare Code-Segmente. Version, Roadmap, Status und Release-Dokumentation wurden auf v0.16.0 und den naechsten Phase-2-Meilenstein aktualisiert.

## [0.15.0] - 2026-07-14

### Hinzugefuegt

- KR-1501: Zentrale, unveraenderliche Instruktionsmetadaten fuer Opcode-Masken, Operandenformate, Kontrollfluss, Delay Slots und Privilegstatus eingefuehrt; Systemregisterkodierungen verwenden dieselbe Quelle im Decoder und in Tests.

- KR-1502: Eine vollstaendige paarweise Decoder-Kollisionspruefung erkennt auch Teilmengen ueber unterschiedlich breite Opcode-Masken und sichert die aktuelle Regeltabelle als mehrdeutigkeitsfrei ab.

- KR-1503: `katana-recomp isa-report` zaehlt deterministisch den gesamten 16-Bit-Opcode-Raum und listet jede implementierte Instruktionsart mit Regel-, Opcode- und Privileginformationen auf.

- KR-1504: Manuell aus dem offiziellen Renesas-SH-4-Handbuch abgeleitete, von der Metadatentabelle unabhaengige Decodervektoren pruefen Format-, Grenz-, Privileg- und Unknown-Faelle.

- KR-1505: Ein reproduzierbarer korpusbasierter Decoder-Mutationsfuzzer prueft Determinismus, eindeutige Metadatenzuordnung, Unknown-Verhalten, Operandenbereiche und Disassembly mit festem Seed.

### Geaendert

- KR-1506: Alle normalen Decoderbedingungen beziehen Opcode-Masken und Muster aus der zentralen Metadatenquelle; Version, Roadmap, Status und Release-Dokumentation wurden auf v0.15.0 und den Beginn von Phase 2 aktualisiert.

## [0.14.0] - 2026-07-14

### Hinzugefuegt

- KR-1401: Byte-, Word- und Long-Formen von `MOV Rm,@-Rn` und `MOV @Rm+,Rn` mit korrekter Registerreihenfolge, Vorzeichenerweiterung und identischen Registerpaaren implementiert.

- KR-1402: Register-Displacement-Formen von `MOV` mit unsigned 4-Bit-Displacement, breitenabhaengiger Skalierung, R0-Sonderformen und definiertem Adress-Wraparound implementiert.

- KR-1403: Byte-, Word- und Long-Formen der R0-indexierten `MOV`-Adressierung mit definiertem 32-Bit-Wraparound und R0-Ueberlappungsfaellen implementiert.

- KR-1404: GBR-relative Byte-, Word- und Long-Formen von `MOV` mit explizitem GBR im generierten CPU-Zustand, 8-Bit-Displacement und breitenabhaengiger Skalierung implementiert.

- KR-1405: PC-relative `MOV.W`- und `MOV.L`-Loads sowie `MOVA` mit korrekter PC-Ausrichtung, Displacement-Skalierung und Vorzeichenerweiterung implementiert.

- KR-1406: Direkte und speicherbasierte `STS`, `LDS`, `STC` und `LDC` fuer System-, Kontroll- und Bankregister mit SR/FPSCR-Maskierung und expliziter Privilegmarkierung implementiert.

- KR-1407: `TRAPA`, `RTE` und `SLEEP` mit sichtbarem Trap- und Schlafzustand, SH-4-Ausnahmeregistern, Registerbankwechsel und korrekter RTE-Delay-Slot-Reihenfolge implementiert.

### Geaendert

- KR-1408: Die End-to-End-Tests fuer KR-1402 bis KR-1405 verwenden committed Binaer-Fixtures und den vollstaendigen Pfad ueber Binary Reader und `katana-recomp emit-cpp`; synthetische KR-1405-Grenzfaelle sind separat gekennzeichnet.

## [0.13.0] - 2026-07-14

### Hinzugefuegt

- KR-1304: `DIV0U`, `DIV0S` und `DIV1` mit expliziten Q-, M- und T-Bits sowie bitgenauen Carry-/Borrow-Referenzvektoren implementiert.

- KR-1303: `MAC.W`, `MAC.L`, `SETS` und `CLRS` mit Post-Inkrement, identischen Adressregistern sowie 32- und 48-Bit-Saettigung implementiert.

- KR-1302: `DMULS.L` und `DMULU.L` sowie das generierte CPU-Register `MACH` mit portabler signed und unsigned 64-Bit-Produktsemantik implementiert.

- KR-1301: `MUL.L`, `MULS.W` und `MULU.W` sowie das generierte CPU-Register `MACL` mit portabler 16- und 32-Bit-Produktsemantik implementiert.

## [0.12.0] - 2026-07-13

### Hinzugefuegt





- KR-1204: `SHAD` und `SHLD` mit positiven, negativen und grossen Shiftzaehlern sowie dokumentierten Sonderfaellen fuer negative Vielfache von 32 implementiert.

- KR-1203: `ROTL`, `ROTR`, `ROTCL` und `ROTCR` mit bitgenauer Register- und T-Bit-Semantik implementiert.

- KR-1202: `SHLL2`, `SHLL8`, `SHLL16`, `SHLR2`, `SHLR8` und `SHLR16` mit plattformunabhaengiger 32-Bit-Semantik und unveraendertem T-Bit implementiert.
- KR-1201: `SHLL`, `SHLR`, `SHAL` und `SHAR` mit bitgenauer T-Bit-Semantik durch Decoder, Katana-IR, C++-Codegenerator und End-to-End-Tests implementiert.

## [0.11.0] - 2026-07-13

### Hinzugefuegt


- KR-1106: `DT Rn` und `MOVT Rn` mit 32-Bit-Wraparound, T-Bit-Semantik und End-to-End-Tests implementiert.

- KR-1105: `EXTU.B`, `EXTU.W`, `EXTS.B`, `EXTS.W`, `SWAP.B`, `SWAP.W` und `XTRCT` mit bitgenauen End-to-End-Tests implementiert.

- KR-1104: `ADDC`, `SUBC`, `NEGC`, `ADDV` und `SUBV` mit dokumentierter Carry-, Borrow- und Overflow-Semantik implementiert.

- KR-1103: `CMP/HS`, `CMP/GE`, `CMP/HI`, `CMP/GT`, `CMP/PZ`, `CMP/PL` und `CMP/STR` durch Decoder, Katana-IR, C++-Codegenerator und End-to-End-Tests implementiert.

- KR-1102: Register- und Immediate-Formen von `AND`, `OR` und `XOR` durch Decoder, Katana-IR, C++-Codegenerator und End-to-End-Tests implementiert.

- KR-1101: `SUB Rm,Rn`, `NEG Rm,Rn` und `NOT Rm,Rn` durch Decoder, Katana-IR, C++-Codegenerator und End-to-End-Tests implementiert.
