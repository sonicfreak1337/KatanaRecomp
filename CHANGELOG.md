# Changelog

## [Unreleased]

### Geaendert

- Der Software-PVR fuehrt die TSP-Fogmodi jetzt im echten Fragmentpfad aus:
  Look-up-Table-Fog dekodiert `FOG_DENSITY` und interpoliert die 128
  Koeffizienten, Per-Vertex-Fog verwendet das interpolierte Offset-Alpha und
  Tabellenmodus 2 ersetzt Base-RGB/-Alpha vor der Texturoperation. Die
  getrennten Tabellen-/Vertex-Fogfarben sowie die aktivierbare RGB-
  Farbklammer aus `FOG_CLAMP_MIN/MAX` wirken vor dem Alpha-Blending. Direkte
  Renderregressionen decken alle vier Fogmodi und Color Clamp ab.
- MSVC-Hostbuilds serialisieren parallele Programmdatenbank-Schreibzugriffe jetzt mit `/FS` fuer
  Runtime und generierte AOT-Units. Dadurch koennen grosse Multi-Unit-Ports unter Visual-Studio-
  Generatoren nicht mehr sporadisch mit `C1041` an einer gemeinsam genutzten Ziel-PDB scheitern;
  `/bigobj` bleibt fuer die grossen generierten Uebersetzungseinheiten aktiv.
- Der SH4-Direktsegment-Vertrag normalisiert P0/P1/P2-Codealiase jetzt durchgaengig fuer
  Imagevalidierung, rekursive Einstiegspunkte, Funktionssymbole, Analyse-Seeds, absolute und
  relative Sprungtabellen sowie den Runtime-only-Dispatcher. Diagnosen behalten das angeforderte
  Aliasziel, waehrend nativer Code unter seiner kanonischen Imageadresse ausgefuehrt wird. Der
  Metadaten-Vollstaendigkeitstest erwartet wieder alle 159 normalen SH4-Regeln statt des alten
  Standes vor den drei Cache-/TLB-Ergaenzungen.
- Die zusammenhaengenden Holly-G1- und G2-DMA-Steuerbloecke bei
  `0x005F7400` und `0x005F7800` sowie der echte Channel-2-/PVR-DMA-Pfad ueber
  den Systembusblock ab `0x005F6800` besitzen geschlossene
  Register-, Masken-, Schutzschluessel- und Aliasvertraege. AICA/G2- und
  PVR-DMA kopieren reale Gastbytes ueber einen neuen linearen
  `Memory::copy_bytes`-Fastpath, aktualisieren Zaehler und Status nach
  deterministischer Gastzeit und melden die passende ASIC-Quelle. Der noch
  ungebundene GD-ROM-DMA-Start bleibt explizit gesperrt. PAL-Bootwerte,
  reservierte Nullregister, falsche Breiten und echte Transfers sind in
  Release und unter AddressSanitizer getestet.
- Die Maple-Laufzeit besitzt jetzt einen echten MMIO-/DMA-Produktpfad fuer den
  Registerblock `0x005F6C00..0x005F6CFC`. Kommandotabellen werden begrenzt und
  schutzfenstergebunden geparst, Frames an den vorhandenen Controller-/VMU-Bus
  zugestellt und Antworten erst nach deterministischer Gastzeit per DMA in den
  Gastspeicher geschrieben; genau eine Completion erreicht danach das
  System-ASIC. Controller und VMU liefern vollstaendige 28-Wort-Device-Info-
  Frames. Unbekannte Muster, widerspruechliche Laengen, ungueltige Adressen,
  falsche Zugriffsbreiten und reservierte Register besitzen Negativtests; der
  Pfad besteht zusaetzlich unter MSVC AddressSanitizer.
- Der spielagnostische Portexport erzeugt fuer verteilbare Pakete keinen
  vollstaendigen Disc-Pack mehr. `game.katana-install` enthaelt ausschliesslich
  versionierte Hash-, Boot- und Trackgeometrie; `game.exe --install-disc
  <eigene.gdi>` prueft die eigene Originaldisc vollstaendig und erzeugt den
  Retailcache atomar nur unter `user-data/content/`. Die Recipe enthaelt weder
  Tracknamen noch Hostpfade, und der Exportbaum besitzt null Retailsektoren.
  Der relocatable GUI-Paketcheck installiert seine synthetische Originaldisc
  ebenfalls erst nach dem Publish und auditiert die Distribution vorher auf
  einen leeren Retaildatenbestand.
- Der SH-4-ISA-Audit deckt die zuvor fehlenden offiziellen SH7750-Befehle
  `LDTLB`, `OCBI` und `MOVCA.L` nun durch Decoder, IR und nativen C++-Backendpfad
  ab. LDTLB kopiert PTEH/PTEL/PTEA in den per MMUCR.URC gewaehlten UTLB-Eintrag;
  Cacheinvalidierung und Allocate-Store bleiben als sichtbare eigene Semantik
  erhalten. Runtime-ABI 15 versioniert den erweiterten CPU-Zustand.
- Private AOT-Fortschrittsdiagnosen melden neben PC und Schedulerstand nun
  Exceptionursache, EXPEVT, INTEVT, TEA, SPC, SSR und VBR. Ein Sprung auf den
  allgemeinen SH-4-Vektor kann damit auf die ausloesende CPU-/Busursache
  zurueckgefuehrt werden.
- Der Portexport kennzeichnet vollstaendige `*.katana-disc`-Dateien als
  ausschliesslich lokalen Retail-Content, ignoriert sie in Root- und erzeugten
  Port-Repositories standardmaessig und erzeugt einen deutlichen Rechte- und
  Paketierungshinweis. CI-, Core-Gate- und Artefaktaudits weisen solche Packs
  in Repositorys und verteilbaren Paketen explizit zurueck. Verteilbare Ports
  muessen ein Patch-/Installerverfahren mit der Originaldisc des Nutzers
  verwenden; GDI und Tracks bleiben autoritative, unveraenderte Quellen.
- Der native Portscanout wird aus `FB_R_CTRL`, `FB_R_SIZE` und `FB_R_SOF1`
  dekodiert und unterstuetzt 16-, 24- und 32-Bit-PVR-Framebuffer statt einer
  fest verdrahteten 640x480-RGB565-Flaeche. MSVC-AOT-Units verwenden `/bigobj`;
  rekursive, bekannte und noch funktionslose Analysebloecke werden als
  bewachte native AOT-Komponenten materialisiert.
- PlatformServices-ABI 6 fuehrt einen Host-Lifecycle-Safepoint ein. Der
  generierte native Dispatch pollt Hostereignisse unabhaengig von der
  pausierbaren Media-Clock, haelt Gastcode bei Pause an und verlaesst ihn bei
  Shutdown kontrolliert. Synthetische Produktpfadtests decken KeyDown/KeyUp,
  Fokusverlust/-gewinn sowie Close im laufenden und pausierten Zustand ab.

### Korrigiert

- Texturkoordinaten werden im Software-PVR jetzt perspektivisch ueber die
  interpolierte reziproke W-Tiefe statt affin im Bildschirmraum berechnet.
  TSP-Mipmap-Bias, Supersampling und Primaer-/Sekundaer-Akkumulationsauswahl
  werden als Materialzustand dekodiert. Solange der echte mehrstufige
  Trilinear-/Akkumulationspfad und Vierfach-Supersampling noch fehlen, werden
  diese Modi explizit abgewiesen und nicht mehr still als einfaches Bilinear
  ausgegeben.
- Der TA-Decoder folgt bei HOLLY2-Vertices jetzt den tatsaechlichen
  32-/64-Byte-Parameterformaten. Untexturierte Packed-Color-Vertices lesen die
  Base Color aus Offset `0x18`; texturierte Floating-Color-Vertices uebernehmen
  Base- und Offset-ARGB aus ihrem zweiten Parameter. Intensity Mode 1 dekodiert
  die Float-Face-Colors aus kurzen oder erweiterten Polygonheadern, waehrend
  Mode 2 diese Face-Colors spezifikationsgemaess wiederverwendet, aber weiterhin
  die Intensitaet jedes Vertices liest. Mode 2 ohne vorherige Mode-1-Face-Color
  sowie abgeschnittene 64-Byte-Parameter werden sichtbar abgewiesen.
- Dreamcast-Discimages tragen jetzt einen expliziten SH-4-Direktsegmentvertrag.
  P0-, P1- und P2-Funktionszeiger werden bei der Codevalidierung auf denselben
  committed P1-Block normalisiert, auch wenn der Alias erst durch mehrere
  beschreibbare PC-relative Literale und Bitoperationen entsteht. Solche
  Snapshotwerte bleiben Runtime-only statt zu statischen Kanten zu werden,
  seeden aber den kanonischen nativen AOT-Block. Der Dispatcher bewahrt das
  angeforderte Aliasziel fuer Diagnose und Metriken und setzt fuer die
  Backendfunktion den passenden kompilierten Ausfuehrungs-PC.
- Der direkte Dreamcast-Disc-Handoff setzt `VBR` beim Einstieg in die native
  Bootdatei jetzt auf die reale Haupt-RAM-Vektorbasis `0x8C000000`, statt den
  SH-4-Resetwert `0` durchzureichen. Titel koennen damit ihre allgemeinen
  Exception-Handler bei `VBR + 0x100` installieren, ohne faelschlich auf die
  physische Adresse `0x00000100` zu schreiben. Der Vertrag ist spielagnostisch
  und besitzt eine synthetische Produktpfadregression.
- Der fruehe Dreamcast-Holly-Systembus bildet den geschlossenen
  Steuerregisterblock `0x005F6800..0x005F68AC` jetzt an allen direkten
  SH-4-Segmentaliasen mit dokumentierten Resetwerten, Bitmasken und
  Lese-/Schreibrechten ab. Channel-2- und Sort-DMA-Start bleiben sichtbar
  gesperrt, solange kein echter Transferpfad gebunden ist, statt Erfolg zu
  simulieren. Die benachbarten PVR-/G2-DMA-Triggermasken bis `0x005F6954`
  sind ebenfalls abgedeckt; unbekannte, reservierte und falsch breite
  Zugriffe besitzen Negativtests. Damit passiert die allgemeine PAL-
  Initialisierung den zuvor ungemappten Schreibzugriff auf `0xA05F6800`.
- Der SH-4-INTC-Block bildet ICR, IPRA, IPRB, IPRC und das auf Dreamcast
  read-only gelesene IPRD an P4- und Area-7-Adressen mit 16-Bit-Breitenvertrag
  ab. IPRA routet TMU-/RTC-Prioritaeten und IPRC die DMAC-Prioritaet direkt in
  den nativen Interruptcontroller; reservierte Offsets, falsche Breiten und
  IPRD-Schreibversuche besitzen Negativtests. Damit loest die PAL-Initialisierung
  an `0xFFD00000` keine kuenstliche Adressfehlerausnahme mehr aus.
- Private AOT-Diagnoselaeufe wenden `KATANA_PORT_BLOCK_LIMIT` jetzt auf die
  gesamte verschachtelte Dispatchsitzung statt erneut pro Dispatchkette an und
  koennen mit `KATANA_PORT_PROGRESS_INTERVAL` belastbare Block-/PC-/Gastzyklus-
  Zwischenstaende ausgeben. `KATANA_PORT_IGNORE_FOCUS=1` verhindert fuer
  explizite unbeaufsichtigte Diagnosen eine dauerhafte Fokus-Pause, ohne den
  normalen Fokusvertrag zu veraendern.
- Die HLE-BIOS-ABI fuehrt FLASHROM_INFO, READ, WRITE und DELETE nun mit dem
  dokumentierten SH-4-Registervertrag, vollstaendigen Bereichspruefungen,
  echter Flash-Programmiersemantik und sektorweisem Loeschen aus. Neue lokale
  Arbeitskopien erhalten aus den Disc-Areasymbolen ein minimales gueltiges
  Japan-/Nordamerika-/Europa-Flashprofil; PAL-Ports starten damit ohne ein
  proprietaeres Flashabbild, waehrend optionale Originalquellen unveraendert
  bleiben.
- SYSINFO_INIT, SYSINFO_ID und der validierte SYSINFO_ICON-Rueckgabevertrag
  sind vollstaendig an das regionsabhaengige Arbeits-Flash gebunden. Der
  24-Byte-Systemblock bei `0x8C000068` wird deterministisch aufgebaut, statt
  dass Titel nach erfolgreichem Init weiterhin uninitialisierte Daten sehen.
- Eigenstaendige Disc-Ports installieren die titelunabhaengigen Dreamcast-
  BIOS-ABI-Vektoren nun vor dem ersten Gastblock und dispatchen HLE-Handler im
  selben RuntimeBlockTable wie den nativen AOT-Code. BIOS-Wrapper springen
  dadurch nicht mehr ueber einen nullinitialisierten Vektor. Die generierten
  BIOS-RAM-Stubs werden zugleich als feste statische Bootstrap-Bloecke im
  ExecutableCodeTracker registriert und weder vom AOT-Tracker noch von der
  Demand-Materializer-Schranke faelschlich als veraltet abgewiesen;
  Portprojektvertrag 8 versioniert den geaenderten Dispatch-Handoff.
- Die Disc-Runtime modelliert die SH-4-I/O-Ports PCTRA/PDTRA, PCTRB/PDTRB und
  GPIOIC nun mit registergenauen Zugriffsbreiten, getrennten Eingangs- und
  Ausgangsbits sowie einem deterministischen Dreamcast-Composite-Kabelprofil.
  Frueher brach bereits die generische Kabelabfrage bei `PDTRA` als
  ungemappter P4-Zugriff ab.
- Der direkte Disc-Handoff startet den SH-4 nun wie vom Bootcode erwartet im
  privilegierten MD-Modus mit maskierten Interrupts. Privilegierte
  Systemregisterzugriffe waehrend der fruehen Retail-Initialisierung loesen
  dadurch keine kuenstliche Illegal-Instruction mehr aus.
- Die generierte statische AOT-Blockregistrierung konstruiert nicht mehr
  tausende `RuntimeBlock`-Stringtemporaries in einer einzigen MSVC-Funktion.
  Ein kompakter Helper beseitigt den dadurch erzeugten gigantischen
  Exception-Unwind-Frame und den Stack Overflow vor dem ersten Gastblock.
- Der Packed-Disc-Parser validiert unbekannte Payload-Enums sowie alle
  erlaubten Kombinationen aus Tracktyp, Sektorgroesse, Payloadart und Offset,
  bevor der Lesepfad auf Modusbytes zugreift. Negativtests manipulieren die
  Metadaten semantisch und berechnen den Metadatenhash neu, sodass zu kleine
  Sektoren und falsche Offsets nicht durch eine blosse Hashabweichung scheitern.
- Generierte EXEs pruefen Packed-Disc-`content_identity`, Projektgeneration
  und Bootdatei-SHA gegen den AOT-Buildvertrag. Ein neu aufgebauter Pack mit
  lediglich kopierter Jobgeneration wird vor Gastcode abgelehnt.
- Fokusverlust leert den Controllerzustand, ohne den Host-Eventpump zu
  verlieren. Close beendet die native Gastdispatchschleife und verhindert,
  dass nach bereits erfolgtem Host-Shutdown noch Audiopuffer eingereicht
  werden.

## [0.47.0] - 2026-07-19

### Geaendert

- KR-4703: Flash und VMU verwenden projektgebundene, atomisch gespeicherte
  Primaer-/Recovery-Arbeitskopien; Nutzerquellen bleiben read-only und werden
  vor Save erneut identitaetsgeprueft. Der generierte Port koppelt Video-
  Gastzyklen an einen ganzzahligen, diagnostizierbaren Hostpacer und speichert
  beim geordneten Shutdown genau einmal. Runtime-ABI 13,
  Hostruntimevertrag 2 und Portprojektvertrag 5 versionieren die Integration.
- KR-4719: Der private Retail-Harness verlangt Configversion 2 und
  `execution_mode=build-only`. Runtimeprozesse werden vor `Process.Start`
  technisch abgewiesen. Zwei frische offizielle Buildjobs muessen Identitaet,
  Kontrollflussabdeckung, aktuelles Executable sowie bytegleiche portable
  Metadaten und Quellen nachweisen; der atomare Bericht enthaelt nur Aggregate
  und kann keinen Checkpoint ueber `KR_RETAIL_ANALYSIS_CONTINUES` ausgeben.
- KR-4718: Kontrollflussklassen werden bis in IR, Codeausgabe und Portadapter
  getragen. Echte Runtime-only-Quellen dispatchen nur auf validierte aktive
  Blockanfaenge; Misses stoppen sichtbar. Gesamt- und Runtime-only-Zaehler fuer
  Hits, Misses, Fallbacks und den ersten Fehler sind maschinenlesbar.
  Runtime-ABI 12, Backend-Interface-ABI 2 und Portprojektvertrag 4
  versionieren den Vertrag.
- KR-4717: Eine begrenzte Objektfeldanalyse leitet dominante Konstruktor- und
  Initialisierungsstores ueber Register, Stack, feste Offsets und
  vollstaendige Memory-Return-Summaries zu Callback- und VTable-Slots weiter.
  Mehrere vollstaendige Typen bleiben endliche bewachte Mengen; unbekannte
  Stores, Aliase, Calls und externe Mutation invalidieren konservativ,
  beschreibbare VTables bleiben partiell.
- KR-4716: Geschlossene direkte und vollstaendig bewachte Callermengen
  propagieren R8 bis R14 kontextsensitiv. Endliche indirekte Calleemengen
  vereinigen nur vollstaendige Return-Summaries. R13-Callbacks sowie begrenzte
  Stack-/Framepointer-Spills bleiben erhalten; unbekannte Caller, Aliase,
  Rekursion und Budgeterschoepfung werden konservativ herabgestuft.
- KR-4625-Nacharbeit: Gebuendelte Writes lesen Write-only-MMIO nicht vorab und
  invalidieren nichtlineare Geraete pessimistisch. Das erzeugte Ninja-Projekt
  baut Runtime-Includes und den generierten Buildvertrag mit der von CMake
  erkannten Hosttoolchain wirklich. Das Gate wiederholt nur klassifizierte
  Windows-Linkerausgabesperren und berichtet alle Versuche samt Exitcode und
  Grund.
- KR-4715: Kontrollflussberichte unterscheiden `resolved`,
  `guarded_complete`, `guarded_partial`, `runtime_only` und `unresolved` sowie
  sieben typisierte Herkunftsklassen. Ein adressfreier Aggregatbericht ist von
  der lokalen Detailanalyse getrennt; partielle Kandidaten blockieren die
  Anwendungs-Vollstaendigkeit.
- KR-4624: Core/CLI ist der GUI-freie Standardbuild. MSVC, GCC und Clang
  besitzen Debug-/RelWithDebInfo-Presets und eine dauerhafte CI-Matrix mit
  Compiler-Cachemessung; Tests sind in stabile Subsystemshards gelabelt.
  Projekt-, Package- und ABI-Versionen werden kanonisch generiert. Getrennte
  Runtime-/Analyzer-CMake-Pakete und ein Out-of-Tree-Consumer sichern die
  Runtimegrenze ohne Analyzerquellen ab.
- KR-4623: GDI-Tracks verwenden persistente read-only Handles, indizierte
  Track-/LBA-Aufloesung, gebuendelte Hostreads und einen abschaltbaren
  256-Sektorcache. ISO9660 besitzt begrenzte Verzeichnis- und Extentcaches.
  Descriptor-/Track-SHA-256 werden zwischen Runtime und Portexport
  wiederverwendet; die pfadunabhaengige Discidentitaet ist SHA-256-basiert.
  GD-ROM-Completions bleiben ausschliesslich an Gastzyklen gebunden.
- KR-4622: Kontrollfluss-Seeds arbeiten ab dem zweiten Fixpunkt auf einer
  wiederverwendeten Kontextbaseline; Funktionssummaries werden SCC-geordnet
  und nur bei geaendertem Ingress propagiert. Immutable Instruktionsarenen,
  Blockspans, gemeinsame Analyseindizes und internierte Evidenzgruende
  reduzieren Kopien und Suchlaeufe. Jump-Table-Snapshots sind begrenzt und
  SHA-256-gebunden; stabile IR-Partitionen sowie Codegencache-Schema 3
  verwenden SHA-256 und atomaren konkurrenzsicheren Publish.
- KR-4621: Memory, exakter Dispatch und Codeinvalidierung besitzen
  abschaltbare Regions-, Hash- und Page-to-Block-Indizes. Lineare u16/u32-
  Zugriffe und der unbeobachtete Speicherpfad vermeiden Byte-/Diagnosearbeit;
  automatische DMA-Transfers koennen deterministisch gebuendelt werden.
  Invalidierungs-, Dispatch- und Store-Queue-Provenienz ist fest begrenzt und
  behaelt monotone Drop-/Gesamtzaehler.
- Die internen v0.48- und v0.49-Meilensteine wurden wiederhergestellt. Damit
  behalten KR-4801 bis KR-4805 und KR-4901 ihre historischen Bedeutungen; die
  zwischenzeitlich damit bezeichneten Alpha-Bring-up-Aufgaben wurden auf
  KR-4911 bis KR-4916 migriert. Eine Task-ID-Registry verbietet kuenftige
  semantische Wiederverwendung.
- Die Roadmap erfasst neue P0-Kontrollflussarbeit fuer Hint-Vertrauen,
  Delay-Slot-Kontexte, exakte Fallthroughs, Site-Vollstaendigkeit und
  interprozedurale Zielmengen sowie P1-Arbeit fuer inkrementelle SCC-Fixpunkte.
- Vor dem Alpha-Bring-up werden private Harnessmodi, strukturierte Metriken,
  native Controller-/Maple-Eingabe und eine informative, eventgetriebene
  Desktop-GUI umgesetzt.
- Eine private Sonic-`game.exe` darf in v0.47 vollstaendig gebaut, aber nicht
  gestartet werden. Der erste Runtimelauf und `KR_GUEST_PROGRAM_ENTERED` gehoeren zur
  Alpha-Entwicklung; der oeffentliche Alpha-Release verlangt weiterhin
  `KR_CONTROLLED_RETAIL_SCENE`.
- Zuvor nicht ausreichend geplante Arbeit fuer Runtime-Replay, dynamische
  Codebereiche und Overlays, echten Gast-PVR-Output, Langzeitspielbarkeit,
  Mehrtitel-Kompatibilitaet und stabile v1.0-Vertraege ist nun explizit
  dokumentiert.
### Korrigiert

- KR-4715-Nacharbeit: Validierte `HintCandidate`-Ziele werden in Detail- und
  Aggregatberichten als `guarded_partial` gezaehlt, waehrend der interne
  `Unresolved`-Status, die schwache Evidenz und der dynamische Default erhalten
  bleiben. Kandidatenlose Hints bleiben `unresolved`.
- KR-4625: Gebuendelte Speicherwrites pruefen den gesamten Zielbereich und die
  vorhandenen Bytes vor dem Commit; Regions- und Schreibschutzfehler koennen
  kein nicht invalidiertes Schreibpraefix mehr hinterlassen. Ein frisches
  Quality-Debug-/RelWithDebInfo-Gate besteht 168/168 beziehungsweise 167/167
  Tests mit 167 gemeinsamen Core-Regressionen. GitHub Actions fuehrt dasselbe
  Windows-Buildgate aus und veroeffentlicht den maschinenlesbaren JSON-Bericht
  sowie CTest-/JUnit-Protokolle als Artifacts.
- KR-4618: Das frische Core-Korrektheitsgate besteht mit 171 Quality-Debug-
  Tests unter MSVC-ASan und statischer Analyse sowie 170 RelWithDebInfo-Tests.
  Beide Profile teilen exakt 170 Core-Regressionen; der zusaetzliche
  Debug-Profiltest prueft die MSVC-ASan-Runtime. Format-, Qualitaetsvertrags-
  und Referenz-/Lizenzaudit sind erfolgreich, und es wurden keine privaten
  Retaildaten verwendet.
- KR-4617: Unabhaengige SH-4-, Store-Queue-, Gastwrite-, Registry-, Timing-
  und CFG-Referenzvektoren decken die P0-Vertraege mit Erfolgs-, Grenz- und
  Fehlerfaellen ab. Hints, Overrides, Luecken, partielle Sites, Delay-Slot-
  Kontexte und unbekannte Caller behalten konservative Runtime-Defaults;
  Fixpunkte werden gegen ein oberes Terminierungsbudget statt gegen interne
  Iterationszahlen geprueft. Die frische Debug-/RelWithDebInfo-Ausfuehrung
  bleibt dem Core-Gate KR-4618 vorbehalten.
- KR-4616: Gastzeitvertrag 1 vereinheitlicht Instruktionskosten, TMU, RTC, DMA,
  GD-ROM, PVR, SLEEP und Laufbudget auf dem zentralen Scheduler. GD-ROM besitzt
  keine manuell fortgeschaltete Uhr mehr, PVR-Abschluesse sind terminiert und
  `KATANA_GUEST_CYCLE_BUDGET` wird von der Runtime tatsaechlich durchgesetzt.
  Runtime-ABI 11 und PlatformServices-ABI 5 versionieren den Vertrag.
- KR-4615: Die Runtime-Blockregistry verwendet stabile, generationsgesicherte
  Handles statt Adressen verschiebbarer Vektorelemente. Statische Bloecke
  werden bulk-registriert und versiegelt; statische und dynamische virtuelle,
  physische und Aliasindizes sind getrennt. Logarithmische Lookups,
  seitenindizierte physische Invalidierung sowie sichere dynamische
  Reaktivierung bilden Runtime-ABI 10.
- KR-4614: Kontrollfluss- und Wertanalyse verwenden typisierte Evidenz und
  Site-Vollstaendigkeit. Hints bleiben ungeloest, Overrides behalten den
  Runtime-Default, Workitems unterscheiden Delay-Slot-Owner und eingehenden
  Kontext, und CFG-Fallthroughs verlangen die exakte Folgeadresse. Unbekannte
  Caller, abweichende Callkontexte und endliche indirekte Callee-Summaries
  werden konservativ vereinigt; dynamische Herkunft folgt einem begrenzten
  CFG-Backward-Slice. Der JSON-Bericht ist als `katana-control-flow-v2`
  versioniert.
- KR-4613: CPU-, FPU-, DMA-, Store-Queue-, Copy- und Fallbackwrites verwenden
  eine gemeinsame beobachtbare `Memory`-Commitgrenze. Lineares RAM wird vor
  einer Generationsaenderung byteweise verglichen. Geaenderte physische
  Bereiche invalidieren Aliase, Links und Blocktabelleneintraege; gebundene
  Dispatch- und Inline-Cache-Lookups koennen stale Bloecke nicht mehr liefern.
  Runtime-ABI 9 und PlatformServices-ABI 4 versionieren den neuen Vertrag.
- KR-4612: Store Queues verwenden Adressbit 5 statt Bit 25. Schreib- und
  Longword-Lesefenster, QACR und PREF teilen eine zentrale SQ0/SQ1-Auswahl und
  pruefen Breite, Ausrichtung sowie Queuegrenzen. Operand-Cache-RAM behandelt
  Byte, Word und Longword little-endian; ICBI richtet auf die 32-Byte-Codezeile
  aus. Nicht modellierte OCBI-/OCBP-/OCBWB-Zustaende schlagen sichtbar fehl.
- KR-4611: Die sichtbare R0-R7-Bank folgt jetzt `SR.MD && SR.RB`. BSR, BSRF
  und JSR aktualisieren PR vor dem Delay Slot, ohne einen dort geschriebenen
  Wert nachtraeglich zu ueberschreiben. Block-ABI 2 unterscheidet RTE und SLEEP
  von normalen Returns und Fallthroughs; der Portdispatcher setzt Gast-
  Exceptions und Interrupts am Handler fort, kehrt ueber SPC zurueck und
  haelt SLEEP bis zu einem angenommenen Interrupt an. Ursache, Eventcode und
  Vektor stammen aus einem gemeinsamen Exceptionvertrag.
- Der erzeugte GDI-Port meldet `KR_GUEST_PROGRAM_ENTERED` und `silent_failures=0` nur
  nach einem echten Gast-Checkpoint. Sofortige Traps, Exceptions oder
  Fallbacks scheitern vorher sichtbar. Descriptor, alle Tracks und die
  extrahierte Bootdatei werden per SHA-256 vor der ersten Gastinstruktion an
  die analysierte Eingabe gebunden.
- Der Portadapter registriert und dispatcht echte IR-Basic-Blocks mit exakter
  Groesse und Abschlussart. Interrupt-, DMA-, Store-Queue- und
  Codeinvalidierungsdienste verwenden die bestehenden Runtimekomponenten;
  `guarded` Kandidaten bleiben dynamisch dispatchbar und blockieren den Export
  nicht mehr.
- `BRAF`/`BSRF` verwenden in Analyse und Codegen dasselbe SH-4-konforme
  32-Bit-Wraparound. Quellpfade werden nur noch ueber belastbare absolute
  Varianten redigiert. ISA-Status ist die gemessene Schnittmenge aus Decoder,
  IR-Lowering, C++-Backend und Runtimegrenze statt einer festen Konstanten.
- KR-4711/KR-4712: Reproduzierbare Analyse-Regressionen sichern jetzt
  SH-C-Call-Clobber und R8-bis-R14-Erhalt ueber direkte/indirekte Calls samt
  Delay Slot, ABI-lose Raw-/ELF-Eingaben, Pre-/Post-Updates, CFG-Joins,
  immutable und beschreibbare Loads sowie absolute und relative Tabellen mit
  `BT`/`BF`, negativen Offsets, Ueberlauf und ungueltigen Mehrfachzielen ab.
- Absolute32- und Relative16-Tabellen benoetigen einen einzigen vollstaendig
  committed, lesbaren und nicht beschreibbaren Snapshot fuer die gesamte
  Tabellenbreite. Die zuvor vorhandene RWX-Ausnahme wurde entfernt;
  PC-relative Literale im Zero-Fill werden ohne leere Offset-Dereferenz
  konservativ verworfen.
- Registerarithmetik kombiniert die Herkunft von Ziel und Quelle jetzt fuer
  `ADD`, `SUB`, `AND`, `OR` und `XOR` deterministisch. Mehrstufige Beweisketten
  bleiben erhalten; Self-XOR und Self-SUB werden mit eigener Herkunft sicher zu
  null gefaltet.

### Hinzugefuegt

- KR-4702: Die versionierte Hostruntime verbindet WinMM-PCM16-Audio,
  reproduzierbare Audiohashes, explizit sequenzierte Win32-Fokus-/Tastatur-/
  Close-Ereignisse und injizierbare Maple-Controllerzustaende. Pause und
  Fokusverlust stoppen Media-Clock und Audio; Shutdown sowie Fehlerpfade
  hinterlassen null Schedulerereignisse. Die erzeugte `game.exe` verwendet
  den Vertrag produktiv und meldet Audio-, Frame- und Eingabemetriken.
- KR-4714: Ein enger GDI-Entry-Snapshotvertrag beweist beschreibbare Literale
  nur vor Join, Kontrollfluss oder moeglicher Ueberdeckung. Alle anderen
  committed Speicherwerte bleiben dynamisch und liefern hoechstens bewachte
  Kandidatenkanten mit erhaltenem Runtime-Default. Funktionsweite endliche
  Werte laufen durch CFG-Joins, SH-C-Calls, direkte und bewachte indirekte
  Parameter sowie Logik-, Shift- und Indextransfers. IR und Graphen markieren
  partielle Kandidaten getrennt von statischen Beweisen. Die adressfreie
  read-only Probe erschloss 55.104 Instruktionen, 813 Funktionen und 1.826
  indirekte Stellen, davon 1.708 bewacht und 117 ohne endliches Ziel; private
  Inhalte blieben ausserhalb des Repository und die Hostanwendung blieb aus.

- KR-4713: Ein titelunabhaengiger SH-C-Callfixpunkt fasst vollstaendige
  konstante oder endliche R0-Rueckgabemengen zusammen und speist sie mit
  Callsite-, Callee-, Register- und Returnevidenz als Einzel- oder
  Mehrfachkanten in den CFG-Fixpunkt. Rekursive, unbekannte und
  widerspruechliche Returns bleiben ebenso offen wie Parameter-, Stack-,
  VTable- und unbeschraenkte Speicherziele. Ein neues synthetisches CTest-
  Programm sichert alle positiven und negativen Klassen; private Inhalte oder
  Retailadressen wurden nicht uebernommen und die Hostanwendung blieb aus.
- Der Analysehotpath vermeidet doppelt geplante rekursive Adressen, ersetzt
  lineare Block-, Kanten- und Site-Suchen durch Indizes und fuehrt teure
  Funktionssummaries erst nach dem stabilen lokalen Fixpunkt aus. Sortierte
  Endergebnisse erhalten den deterministischen Berichtvertrag.

- KR-4712: Die Analyse propagiert Speicherwerte nur aus committed read-only
  Segmenten und erkennt begrenzte SH-4-`CMP/HS`-/`MOVA`-/`MOV.W`-/`BRAF`-
  Tabellen mit `BT`- oder `BF`-/Fallback-Guard. Vorzeichenbehaftete 16-Bit-
  Eintraege werden vollstaendig validiert, als Mehrfachkanten in den Fixpunkt
  gespeist und mit Kodierung, Basis und Einzelevidenz berichtet. Die
  synthetischen Regressionen sind der reproduzierbare Abschlussnachweis; die
  Hostanwendung wurde nicht gestartet und es wurden keine privaten Inhalte
  uebernommen.

- KR-4711: Die Wertanalyse invalidiert unbekannte SH-4-Effekte registerweise
  statt als pauschalen Voll-Clobber, behandelt Call-Delay-Slots und bedingte
  CFG-Fallthroughs konservativ und bindet den dokumentierten SH-C-Vertrag fuer
  R8 bis R14 explizit an Dreamcast-GDI-Images. ABI-lose Images sowie R0 bis R7
  bleiben Call-Clobber; ABI-erhaltene Beweise tragen eine eigene Herkunft. Ein
  redigierter read-only Retailvergleich erweiterte die Entdeckung auf 40.599
  Instruktionen, 575 Funktionen und 1.288 indirekte Stellen, davon 1.017
  bewiesen und 271 weiterhin dynamisch. Es wurde keine Hostanwendung gestartet.

- KR-4701: Ein versionierter nativer Hostvideovertrag stellt externen Ports
  Win32-Fenster, Resize, seitenverhaeltnistreues RGBA-Present, kontrolliertes
  Schliessen und weitergereichte GDI-Fehler ohne CLI-Laufzeithuelle bereit.
  Die erzeugte `game.exe` praesentiert produktiv einen VRAM-Frame und meldet
  `frames=1`. Ein selbst erzeugter rechtefreier Farbrahmen prueft den nativen
  Vertrag; Port-CLI und relocatiertes Runtime-SDK pruefen den Produktpfad.
- KR-4506: Die allgemeine Wertanalyse loest PC-relative SH-4-Wort-/Langwort-
  Literale und `MOVA` samt Herkunft auf und speist beweisbare indirekte Ziele
  in den rekursiven CFG-Fixpunkt ein.
- KR-4507: Anwendungsjobs und Buildplaene tragen versionierte
  Analyseabdeckungsmetriken und unterscheiden `completed`, `partial` und
  `failed`. Unvollstaendiger Kontrollfluss erzeugt weder Codegen noch
  Hostbuild; CLI, GUI, Jobbericht und Buildplan verwenden dieselbe kanonische
  Werkzeugversion. Ein gemeinsamer read-only Eingabesnapshot verhindert
  gemischte Provenienz, der Portexport uebernimmt das bereits gepruefte
  Analyse-/IR-Ergebnis, und typisierte Fehlerkategorien erreichen die stabilen
  CLI-Exitcodes. Das interne GUI-Paket liefert ein relocatables Runtime-SDK mit
  und beweist den verschobenen synthetischen GDI-Hostbuild ohne einkompilierten
  Entwicklerpfad. Jobs erzeugen Ergebnisse in einem kurzen, identitaetsgebundenen
  Stagingordner und veroeffentlichen sie erst nach Erfolg; Fehler und Abbrueche
  entfernen Teilresultate, behalten vorhandene Ergebnisse als explizit veraltet
  und beenden unter Windows auch gestartete Hostbuild-Kindprozesse. Prozessweite
  Ausgabe-Locks verhindern ueberlappende Schreibziele mehrerer Instanzen. Die
  korrigierte Vertragsversion 3 verlangt ausserdem vollstaendige Abdeckung aller
  committed ausfuehrbaren Bytes und null erreichbare Abbruchkanten. Ein vor dem
  Laden erfasster und direkt danach erneut gepruefter Eingabesnapshot bindet das
  geladene Image an seine Provenienz. Wiederholte Fehler mit derselben Job-ID
  behalten den letzten erfolgreichen Stale-Stand; Linux verwendet dafuer und
  fuer ueberlappende Ziele eine prozessweite `flock`-Registry.
- KR-4508: Das Portprojekt besitzt mit Vertragsversion 2 einen eigenstaendigen
  GDI-Runtime-Einstieg. `game.exe <disc.gdi>` liest Bootmetadaten und
  ISO9660-Bootdatei erst zur Laufzeit, initialisiert Dreamcast-RAM, VRAM,
  AICA-RAM, Flash, CPU, Stack, Scheduler und Plattformdienste und erreicht den
  generierten Einstieg ueber die generische Blocktabelle samt diagnostiziertem
  indirektem Dispatch. Fehlende Quellen und `--run-generated` ohne Bootimage
  enden nichtnull mit redigierter Diagnose; GDI-Daten und private Hostpfade
  werden weiterhin nicht eingebettet. Der relocatierte Pakettest fuehrt die
  erzeugte `game.exe` ohne KatanaRecomp-CLI bis zum Runtime-Marker aus.
- KR-4501: Der SH-4-Alpha-ISA-Vertrag trennt Decoderzaehlung von behaupteter
  Semantik. Sieben Familien melden Decoder, IR, Backend und Runtime einzeln als
  `supported`, `restricted` oder `rejected`; jede Behauptung besitzt einen
  benannten Semantikvertrag und eine Testanforderung. System-/Privileg-,
  Cache-/Store-Queue- und FPU-Semantik bleiben mit konkreten Runtime-Grenzen
  eingeschraenkt, unbekannte oder nicht implementierte Encodings werden
  abgelehnt. `isa-report --json` exportiert Vertragsversion 1 reproduzierbar.
- KR-4502: `BRAF`/`BSRF` verwenden durch Decoder, Wertanalyse, IR und Backend
  die exakte vor dem Delay Slot gelesene Zielbildung `PC+4+Rm`; `BSRF` setzt
  PR wie ein Aufruf. `CLRMAC`, `TAS.B` sowie `TST.B`/`AND.B`/`XOR.B`/`OR.B`
  auf `@(R0,GBR)` besitzen explizite Akkumulator-, T-Bit-, Speicher- und
  Fehlersemantik. Eine neue generierte Regression fuehrt Null-/Nichtnull- und
  Unmapped-Grenzen aus. Ein autorisierter privater Retail-Nachlauf fiel von
  vier unbekannten Instruktionen auf null, bleibt wegen dynamischem
  Kontrollfluss aber ehrlich `partial`; dieser Lauf ist kein Bootnachweis.
- KR-4503: Der generierte Backendpfad prueft jede als privilegiert markierte
  Instruktion vor ihrer ersten Teilwirkung gegen `SR.MD`. Privilegierte
  STC/LDC-Systemregistertransfers, `RTE` und `SLEEP` erzeugen im User-Modus
  eine strukturierte Illegal-Instruction-Ausnahme mit stabilem Gast-PC und
  Delay-Slot-Kontext; Supervisor-Ausfuehrung, SR-/FPSCR-Maskierung und
  Registerbankwechsel bleiben durch gemeinsame End-to-End-Tests abgesichert.
- KR-4509: Der gemeinsame Anwendungsjob-Vertrag Version 4 liefert geordnete
  `katana-job-event`-Ereignisse fuer Validierung, Hashing, Bootimage, Analyse,
  IR, Codegen, Hostkonfiguration, Kompilierung und Abschluss. Ereignisse tragen
  monotone Gesamtprozente, Schrittstatus, optionale Zaehler, Zeitstempel,
  Laufzeit und redigierte inkrementelle Log-Chunks. CLI und GUI konsumieren
  denselben Observerstrom; unbekannte Schrittgroessen bleiben ohne erfundene
  Prozentzahl. Fehler und Abbruch behalten den aktiven Schrittnamen. Die GUI
  liest das Buildlog nicht mehr periodisch von der Datei neu ein.
- KR-4510: Die native Windows-GUI zeigt gewaehlte GDI und Ausgabe dauerhaft in
  kopierbaren Feldern, Gesamt- und Einzelschritt in echten Progressbars sowie
  Aktion und Laufzeit an. Ein DPI-skaliertes vertikales Hauptlayout und das
  unabhaengige Mausrad-/Tastatur-Log bleiben bei kleinen Fenstern bedienbar.
  Dark Theme, High-Contrast-Fallback und ein eingebettetes Mehrgroessen-Icon
  stammen aus dem geprueften internen Logoableitat. Ein nativer Control-Test
  prueft Icon, Fokus, Scrollbereich, Theme-Fallback und 100 bis 300 Prozent
  Layoutskalierung; Read-only-EDIT-Benachrichtigungen koennen keinen rekursiven
  Refresh oder Callback-Abbruch mehr ausloesen.
- KR-4511: Ein privater Retail-Debugharness erzwingt externe Konfigurations-,
  Eingabe-, Ausgabe- und Berichtspfade, Hostzeitbudget, vorbereitetes
  Gastzyklusbudget, monotone `SA_...`-Checkpoints und allgemeine Fehlerklassen.
  Der redigierte Bericht enthaelt keine Pfade, Hashes, Spieldaten oder Rohlogs;
  Gitignore und Paketpfad schliessen Retailartefakte technisch aus. Ein
  verteilbarer Selbsttest prueft Checkpointordnung, partielle Analyse und
  Datenredaktion ohne Retaildaten. Der autorisierte budgetierte Nachlauf
  erreichte nur `KR_RETAIL_ANALYSIS_CONTINUES` und endete reproduzierbar als
  `analysis-incomplete`; dies ist kein Bootnachweis.
- KR-4504: Das kumulative v0.45-Vorbereitungsgate besteht 164/164 Tests unter
  MSVC Debug, AddressSanitizer und Coverage im einzigen frischen
  `build-current`. Der synthetische Homebrew-Hostlauf meldet zwei Frames und
  null stille Fehler; Format-, Qualitaets-, Referenz-, Lizenz- und Datenaudits
  sowie ISA-, privater Harness-Selbsttest und nativer QoL-Vertrag bestehen.
  Ein seit KR-4502 veralteter Tabellenumfang im Metadaten-Vertragstest wurde
  von 146 auf die tatsaechlichen 154 Regeln korrigiert. Der verteilbare Marker
  lautet `KR_V045_BOOT_ANALYSIS_READY`; er ist kein Retail-Bootnachweis. Vor
  Vor KR-4505 blieb das ausdrueckliche Nutzerreview offen, ohne Release, Tag
  oder Veroeffentlichung.
- KR-4505: Der Nutzer hat die unveraenderte v0.45-Gate-Vorbereitung am
  16.07.2026 ausdruecklich freigegeben. Der interne Pre-Alpha-Meilenstein ist
  damit `0.45.0`, und die Arbeit an v0.46 darf beginnen. Diese Freigabe aendert
  nicht die kanonische Produktversion und erzeugt weder Release-Commit noch
  Tag, Download oder Paket.
- KR-4601: Ein versionierter Alpha-Firmwarevertrag trennt den verfuegbaren
  Direct-Retail-Einstieg, den bis KR-4602 nur vertraglich definierten HLE-Pfad
  und optionales, nicht implementiertes LLE. Jeder Anwendungsjob validiert den
  Modus vor Snapshot, Loader, Speicherabbildung und CPU-Reset. BIOS- und
  Flashquellen sind read-only, werden nie paketiert und muessen ebenso wie
  veraenderliche Flash-Arbeitskopien ausserhalb des Port-Ausgabeordners liegen.
  Ein synthetischer Vertragstest prueft Statusmatrix, Testanforderungen,
  Pfadisolation sowie konkrete HLE-/LLE-Ablehnungen ohne Firmwarebytes.
- KR-4602: Die HLE-BIOS-ABI installiert SYSINFO-, ROMFONT-, FLASH-, MISC/GD-ROM-,
  GDROM2- und SYSTEM-Vektoren reproduzierbar als Zeiger und Runtimeblocks im
  Gast-RAM. Der gemeinsame physische Dispatch und die Firmware-Handoff-Provenienz
  erhalten P1/P2-Aliase und Gastzyklen. Nachweisbar zustandsfreie Aufrufe werden
  abgeschlossen; bekannte noch nicht angebundene Hardwaredienste enden als
  `service-unavailable`, unbekannte Selektoren als stabile harte Diagnose. Der
  Vertrag ist titelunabhaengig und enthaelt keine BIOS- oder Retailbytes.
- KR-4603: Ein gemeinsames Dreamcast-System-ASIC-MMIO-Geraet bildet drei
  Pending-/ACK-Baenke und je drei IRL13-/IRL11-/IRL9-Maskenbaenke ab. PVR-,
  Maple-, GD-ROM-, DMA- und AICA-Ereignisse werden ueber den EventScheduler in
  stabiler Gastzeit-/Einreihungsreihenfolge zugestellt und ausschliesslich ueber
  den zentralen externen Interruptrouter signalisiert. ACK, Maskierung,
  physische/P1/P2-Aliase sowie unbekannte, reservierte und falsch breite MMIO-
  Zugriffe besitzen synthetische Erfolgs- und Fehlerregressionen.
- KR-4604: Das kumulative v0.46-Vorbereitungsgate besteht 168/168 Tests in
  genau einem frischen MSVC-x64-Debug-Build mit AddressSanitizer, statischer
  Analyse und Coverage. Der gemeinsame synthetische HLE-/BIOS-ABI-/ASIC-
  Vertical-Slice erreicht `KR_V046_RETAIL_BOOT_SERVICES_READY`; Homebrew und
  Audits melden keine stillen Fehler oder privaten Daten. Nach spaeteren
  Reviewkorrekturen wurde der alte Nachweis verworfen und das Gate auf Commit
  `e7b19ba` vollstaendig frisch und erfolgreich wiederholt.
- KR-4605: Der Nutzer hat vor dem korrigierten Gate ausdruecklich festgelegt,
  dass ein erfolgreicher Lauf die interne v0.46-Freigabe erteilt. Der interne
  Pre-Alpha-Meilenstein ist damit `0.46.0`, und Phase 12 darf beginnen. Es
  wurden weder Release-Commit noch Tag, Download oder Paket erzeugt.
- Der neue inkrementelle Entwicklungszyklus behaelt `build-current/`, setzt teure Gate-Instrumentierung beim Debugprofil sicher zurueck und baut sowie testet mit begrenzter Parallelitaet. Das frische Abschluss-Gate bleibt erhalten; die vier deterministischen Fuzzziele laufen mit unveraenderter Fallzahl und denselben abgeleiteten Seeds als parallele CTest-Eintraege.
- Der Entwicklungsrunner normalisiert auch eine bereits aktive x86-Developer-PowerShell auf die fuer den Projektbuild erforderliche native x64-MSVC-Umgebung.
- KR-3801 bis KR-3808: Ein intern provenance-gebundenes, vollstaendig
  synthetisches Homebrew-Korpus verbindet CPU-, Konsole-, Maple-, 2D-PVR-,
  AICA-, Firmware-Handoff-, DMA-, Interrupt- und Replayszenarien in einem
  deterministischen Zwei-Frame-Bericht ohne BIOS, Flash oder Disc-Image.
- KR-3901 bis KR-3907: Versionierte exakte und gesampelte Ausfuehrungsprofile,
  deterministische Hot-Block/-Edge-Berichte, bewachte lineare RAM-Fastpaths,
  generationengesicherte monomorphe Callsites, eine begrenzte
  Inliningstrategie und getrennte Phase-9-Benchmarks bilden die
  Performancebasis.
- KR-4002: Eine interne Architektur-/Manifestreferenz und maschinenlesbare
  Faehigkeitsmatrix trennen erforderliche, unterstuetzte, experimentelle und
  nicht verfuegbare Profile samt automatisiertem Nachweis und Einschraenkung.
- KR-4006: Das kumulative Phase-9-Gate besteht 156/156 Tests in einem frischen
  MSVC-Debug-Build. Der deterministische Zwei-Frame-Hostlauf meldet keine
  stillen Fehler und keinen Fallback; Reproduzierbarkeit, Budgets sowie der
  Audit auf Firmwarebytes, Geheimnisse, lokale Pfade und Rohtraces bestehen.
  Der Gate-Bericht wechselt ohne Versionierung oder Release zu Phase 10.
- KR-4101 bis KR-4103: Eine native Win32-/X11-Desktopshell, ein portables
  GUI-Modell und der gemeinsame C++-Anwendungsdienst stellen Navigation,
  Einstellungen, Recovery, beobachtbare Jobs, Abbruch und identische
  CLI-/GUI-Aufrufe ohne duplizierte Kernsemantik bereit.
- KR-4201 bis KR-4204: Das bestehende Projektmanifest kann Raw, ELF32-SH und
  GDI deterministisch speichern. GDI-Tracks bleiben read-only und werden mit
  Rollen, Sektorformaten, Groessen, Descriptorzeilen und Hashprovenienz
  inspiziert; Firmwareprofile und Analyse-Overrides verwenden denselben
  Parservertrag wie die CLI.
- KR-4301 bis KR-4303: Analyse-, Codegen-, Build- und Run-Preflight-Jobs
  liefern redigierte Fortschritts-, Diagnose-, Funktions-, Segment-, Quellen-
  und Provenienzberichte mit stabilen Gastadressen und relativen Artefakten.
- KR-4401: Synthetische GDI-Positiv-, Fehler- und Recovery-Pfade sowie der
  GUI-/CLI-Artefaktvergleich erreichen automatisiert
  `KR_PHASE10_GUI_END_TO_END`.
- KR-4402: Das kumulative Phase-10-Gate besteht 160/160 Tests in einem
  frischen MSVC-Debug-Build. GUI und CLI erzeugen dieselbe Projektidentitaet
  und acht byteidentische Kernartefakte; Phase-9-Regression, Coverage,
  reproduzierbares Basisartefakt, standalone GUI-Paket und Datenaudit
  bestehen. Vor KR-4403 bleibt der ausdrueckliche Nutzerreview offen.
- Das fuer Phase 10 bereitgestellte App-Logo ist mit Abmessungen, Transparenz,
  SHA-256 und Herkunft als interner GUI-Asset-Eingang eingebunden. Eine
  oeffentliche Weitergabe bleibt bis zum vollstaendigen KR-4902-Audit gesperrt.

### Geaendert

- v0.38.0 bis v0.49.0 sind interne Entwicklungsmeilensteine ohne
  Release-Commit, Tag, Download oder Veroeffentlichung; v0.50.0 Alpha wird der
  erste oeffentliche Produktrelease. Phase 9 besitzt nur noch ein kumulatives
  Abschlussgate, waehrend oeffentliche Dokumentation, Packaging sowie der
  vollstaendige Daten- und Lizenzaudit im Alpha-Kandidaten gebuendelt werden.

### Behoben

- KR-4604 wurde nach den Reviewkorrekturen auf Commit `e7b19ba` erneut mit
  genau einem frischen Build ausgefuehrt: 168/168 Tests, ASan, statische
  Analyse, Coverage, null stille Homebrew-Fehler und
  `KR_V046_RETAIL_BOOT_SERVICES_READY`.
- Reviewkorrektur: Produktiver HLE-Boot installiert und besitzt die sechs
  BIOS-ABI-Vektoren samt Runtimeblocktabelle und Firmware-Handoff selbst. Der
  GDI-Portexport uebernimmt `execution.firmware` in die erzeugte `game.exe`.
  Der produktive Runtimezustand besitzt System-ASIC, Scheduler, Router, PVR,
  Maple, GD-ROM und AICA gemeinsam; reale Geraeteaktionen signalisieren ihre
  ASIC-Ereignisse ueber Callbacks. Der bisherige v0.46-Gatelauf ist wegen
  dieser und weiterer Reviewkorrekturen invalidiert; KR-4604 ist wieder offen.
- Reviewkorrektur: Der private Retail-Harness trennt Prozessstart von Exitcode
  und Timeout, uebernimmt `silent_failures` ausschliesslich aus genau einem
  validierten Runtimemarker und prueft SA-Checkpoints als lueckenlose,
  eindeutige Zustandsmaschine. Konfiguration und alle privaten Pfade werden
  inklusive Repositorywurzel, nicht vorhandener Kinder, Symlinks und Windows-
  Junctions/Reparse Points lexikalisch sowie physisch vom Repository getrennt;
  breitere Gitignore-Muster sperren auch frei benannte Retailkonfigurationen.
- Reviewkorrektur: Terminale Jobereignisse werden erst nach erfolgreichem
  Schreiben, atomarem Rename und Stale-Cleanup gesendet; Publikationsfehler
  bleiben strukturierte `input-output`-Fehler. Die Live-Logposition folgt nach
  atomarer Redaktion der neuen Dateigroesse und verliert keine fruehe
  Kompilierausgabe. Linux wiederholt `waitpid` bei `EINTR`, wertet bei anderen
  Fehlern keinen alten Status aus und eskaliert einen ignorierten `SIGTERM`
  nach fuenf Sekunden zu `SIGKILL`.

- Lokale MSVC-AddressSanitizer-Builds kopieren ihre passende Laufzeit-DLL in
  `build-current`, sodass CLI und GUI auch ausserhalb einer Developer-
  PowerShell ohne `clang_rt.asan_dynamic-x86_64.dll`-Systemfehler starten.
- Mehrteiliger Port-Codegen verwendet einen partitionsuebergreifend identischen
  Programmeinstieg, validiert externe Callziele als IR-Fragmente und fuehrt den
  erzeugten Hostbuild im Regressionstest tatsaechlich aus.
- Differentialtests kompilieren die Spezialkorpora ueber den echten
  C++-Emitter; Manifestprofile erreichen alle Analyseberichte und nicht
  anwendbare Runtimeprofile enden kontrolliert als Codegen-Capability-Fehler.
- Der isolierte Fuzz-Reducer startet Kindprozesse shellfrei und minimiert nur
  bei gleicher Ausnahme-, Prozessstatus- oder Signalsignatur. CLI-I/O-Fehler
  verwenden typisierte Ausnahmen statt uebersetzbarer Meldungsfragmente.
- Release-Provenienz wird gegen den aufgeloesten Tag-Commit geprueft, ohne einen
  selbstbezueglichen ZIP-Hash im Tag zu behaupten. Der Referenzaudit lehnt
  unbekannte Binaerdateien, Git-LFS-Zeiger und generierten Quellcode ausserhalb
  der Testfixtures ab.
- Performancepfade umgehen weder aktive Watchpoints und MMIO noch MMU-, Alias-,
  Berechtigungs-, Adressraum- oder Codegenerationswaechter. Direkte RAM-Writes
  behalten die bestehende Codeinvalidierung; stale Inline-Caches fallen auf
  den generischen Dispatch zurueck.

## [0.37.0] - 2026-07-16

### Hinzugefuegt

- KR-3501: Projektmanifeste besitzen ein namespaced v2-Schema mit festem `katana-project`-Identifier und getrennten Projekt-, Eingabe-, Image- und Segmentfeldern. Der v1-Vertrag bleibt kompatibel lesbar; unbekannte Versionen, Schemas und versionsfremde Felder scheitern vor dem Loaderzugriff.
- KR-3502: Die CLI veroeffentlicht stabile Exitcodes fuer Nutzung, Eingabe, I/O, Verarbeitung, Codegen, Hostbuild und interne Fehler. Zentrale Diagnosen tragen eine feste Fehlerklasse; `--help`, `-h` und `--version` sind stabile erfolgreiche Basiskommandos.
- KR-3503: Ein gemeinsamer versionierter JSON-Reportkopf und zentrale kontrollzeichenfeste String-Escapierung vereinheitlichen IR-, Kontrollfluss- und Gate-Berichte. `analyze-json` liefert Funktionen, indirekte Ziele, Jump Tables und Diagnosen deterministisch und ohne Hostzeitfelder.
- KR-3504: Versionierte v2-Analyseanweisungen trennen erzwingende Overrides von unverbindlichen Hints. Statische Beweise werden nie durch abweichende Hints ersetzt; angenommene, bestaetigte, abgelehnte und stale Hinweise erscheinen deterministisch im Kontrollflussbericht. v1-Overrides bleiben kompatibel.
- KR-3505: Das v2-Projektmanifest beschreibt explizite Direct-/HLE-/LLE-Firmwaremodi, Fallback-, Scheduler-, MMU- und Fastpathprofile, Backend-Faehigkeiten, Alias- und kanonische Speicherbereiche, WX-Segmente, erwartete Einstiege sowie dynamische BIOS-Vektoren. Sichere Direct-/Abort-Defaults und Voranalysevalidierung verhindern stillen Fallback, unvollstaendiges LLE und widerspruechliche Aliase.
- KR-3506: Externe Eingaben erhalten portable SHA-256-/Groessenprovenienz mit strikt getrenntem lokalem Pfad. Ein deterministischer redigierter Buildbericht bindet Manifest, Analyseanweisungen, Werkzeug, IR, Runtime und Backend; der Codegen-Cache-v2-Schluessel bezieht nun auch die Werkzeugversion ein.
- KR-3507: `katana-recomp port <quelle.gdi> --output <ordner> --target-name <name>` fuehrt die validierte Disc-Bootdatei ueber Analyse, IR, Optimierung und deterministische Partitionierung in ein extern buildbares CMake-/Ninja-Portprojekt. Nur manifestierte Dateien unter `generated/` werden ersetzt; `src/`, Nutzerdateien und private Disc-Inhalte bleiben getrennt, waehrend redigierte Metadaten und eine austauschbare DiscSource-Hostgrenze reproduzierbare Folgeprojekte ermoeglichen.
- KR-3601: Ein gemeinsamer deterministischer Symbolindex priorisiert ELF- und Map-Namen nach Binding, Art, Groesse und Name. Text- und Kontrollfluss-JSON-Berichte ergaenzen exakte oder groessenbegrenzte `name+offset`-Informationen, ohne die numerische Gastadresse zu ersetzen oder groessenlose Symbole ueber ihren exakten Ort hinaus zu raten.
- KR-3602: Stabile `katana-guest`-Marker im generierten C++ bilden jede emittierte SH-4-Adresse auf portables Eingabesegment/-offset und relative Generated-Datei/-zeile ab. Die versionierte JSON-Source-Map erhaelt Mehrfachpositionen deterministisch und lehnt absolute Pfade, ausbrechende Komponenten sowie unportable Segmentangaben ab.
- KR-3603: Versionierte kontrollierte Crashberichte erfassen virtuelle und kanonische Adresse, Register-, Exception- und Delay-Slot-Zustand, Blockvariante, Schedulerstand sowie letzten Dispatch. Strikte portable Diagnosecodes und gekoppelte Kontextvalidierung verhindern freie Hostmeldungen, Pfade, Hostzeiger oder Speicherdumps im Bericht.
- KR-3604: Ein kapazitaetsbegrenzter Runtime-Trace vereint IR-, Block-, Speicher-, Watchpoint-, Exception- und Schedulerereignisse unter monotoner Gastzeit. Drops bleiben gezaehlt, Memory- und Watchpointobserver verwenden bestehende Runtime-Hooks, und Speicherwerte bleiben ohne ausdrueckliches lokales Opt-in redigiert.
- KR-3605: Deterministische CFG- und Callgraph-Exporte stehen als versioniertes JSON und Graphviz-DOT ueber stabile CLI-Kommandos sowie im Portprojekt bereit. Direkte, bedingte, aufgeloeste indirekte und explizit unaufgeloeste Kanten behalten Gastadressen und optionale Symbole, ohne Hostpfade oder Eingabebytes zu serialisieren.
- KR-3606: `firmware-diagnose` prueft lokale BIOS-/Flashgroessen und optionale SHA-256-Werte read-only. Konservative Bereichsklassen sowie Flashpartitionen, Header, Blockgenerationen und CRCs erscheinen maschinenlesbar; logische IDs und Region bleiben standardmaessig redigiert, waehrend Firmwarebytes und Assets nie extrahiert werden.
- KR-3607: Eine ausnahmesichere Dispatchdiagnostik erfasst Callsite, virtuelle und kanonische Ziele, PR, Blockende, Alias- und Beweisherkunft sowie Fallbackgrund, Aktion, Gastinstruktionen, Austritt und stabile Fehlerklasse. Identische Ereignisse erhoehen nur ihren Zaehler; Beobachtungsfehler koennen die Gastentscheidung nicht beeinflussen.
- KR-3608: Ein gemeinsamer Runtime-Provenienzbericht verbindet strukturierte Blockherkunft, kanonische Aliasgruppen, ROM-RAM-Codekopien, zeitabhaengige Firmwarevektoren und eine best-effort Invalidierungshistorie. CPU-, DMA- und Copy-Writes behalten virtuelle/physische Adresse, Seitengeneration, invalidierte Bloecke und geloeste Links, ohne Firmware- oder Codebytes auszugeben.
- KR-3609: Versionierte Systemreplays ordnen CPU-Safepoints, MMIO, DMA, Interrupts, Timer, Schedulercallbacks, Medienereignisse und explizite externe Injektionen ausschliesslich nach Gastzeit. Ereignis- und Gastzustandshash sichern deterministische Wiederholung; fehlende, zusaetzliche oder anders sortierte Ereignisse scheitern am ersten Abweichungsindex.
- KR-3701: Portable CMake-Presets und `tools/gates/run-debug-gate.ps1` definieren einen frischen lokalen Debug-Build mit vollstaendiger Regression in `build-current/`. Der Profilvertrag haelt Release-Build und Windows-/Linux-CI bis KR-4999 deaktiviert; der zuvor bei jedem Push laufende Debug-/Release-Workflow wurde aus dem aktiven GitHub-Pfad entfernt.
- KR-3702: Das frische `sanitizer-debug`-Profil aktiviert AddressSanitizer auf MSVC/GCC/Clang und zusaetzlich UndefinedBehaviorSanitizer ohne Recovery auf GCC/Clang. Es verwendet dasselbe CTest-Korpus und ausschliesslich `build-current/`; ausgefuehrt wird es gesammelt in KR-3709 statt pro Task.
- KR-3703: `katana-fuzz` mutiert synthetische Decoder-, ELF32-SH-Loader- und IR-Eingaben mit reproduzierbaren Seeds, festen Groessen-/Iterationsgrenzen und fallgenauer Fehlerdiagnose. Der Loader besitzt dafuer einen dateilosen Byte-Span-Einstieg; `fuzz-debug` kombiniert den portablen Kurzlauf mit dem Sanitizerprofil fuer KR-3709.
- KR-3704: `coverage-debug` instrumentiert GCC/Clang ueber gcov-kompatible Flags und nutzt unter MSVC das native dynamische Visual-Studio-Coveragewerkzeug. Ein abgesicherter Runner sammelt das vollstaendige CTest-Korpus in einen Cobertura-Bericht ausschliesslich unter `build-current/coverage/`.
- KR-3705: Ein repositoryweiter `.clang-format`-/`.clang-tidy`-Vertrag und ein read-only Formatpruefer decken Include-, Quell-, Test- und Toolcode ab. `quality-debug` fuehrt MSVC-Analyse beziehungsweise `clang-tidy` mit Fehlereskalation im selben kumulativen Debug-Build aus.
- KR-3706: `artifact-debug` entfernt lokale Quellpfade aus Debuginformationen und aktiviert reproduzierbare Compiler-/Linkoptionen. Ein strikt begrenzter Paketierer erzeugt sortierte, zeitnormalisierte `0.37.0-dev`-ZIPs samt SHA-256-Manifest zweimal und akzeptiert sie nur bei Bytegleichheit; private oder generierte Eingaben sind nicht paketierbar.
- KR-3707: Ein Differential-Harness speist dieselben synthetischen Mikrogramme in IR-Referenz, generiertes C++ und kontrollierten Interpreter-Fallback. Kanonische Checkpoints vergleichen CPU, Speicher, Ausnahme, MMIO und Scheduler am ersten abweichenden Gast-PC; stabile JSON-Gegenbeispiele enthalten Seed, Korpus und Zustandspfad.
- KR-3708: Das neue `katana-fuzz --target runtime` kombiniert synthetische Multi-Segment-Images, validierte Aliasgruppen, MMU-/Watchpointvarianten, exakten und kanonischen Dispatch, ROM-RAM-Codekopien sowie CPU-/DMA-/Copy-Invalidierung. Aliasverkettungen und -zyklen werden bereits im Manifestvertrag abgelehnt; Seitengenerationen verhindern stale Blocklookups.
- KR-3709/KR-3710: Das kumulative lokale Debug-Gate besteht 151/151 Tests mit AddressSanitizer, MSVC-Analyse, Coverage, festem prozessisoliertem Fuzz-Kurzlauf, Format-/Provenienzaudits und reproduzierbarem Artefakt. Release- und CI-Builds bleiben wie beschlossen bis zum Alpha-Gate deaktiviert.

### Behoben

- Partitionsuebergreifende direkte Calls verwenden einen global validierten, extern gelinkten Symbolvertrag; Namespacewahl ist eine Emitteroption statt breiter Textersetzung.
- Port-Zielpfade werden ueber kanonische Elternpfade gegen den Repository-Root geprueft, auch wenn eine Symlink-Komponente auf den Quellbaum zeigt.
- Geladene v2-Projekte behalten das validierte Ausfuehrungsprofil; MMIO-Replays fuehren die Scheduler-Resetepoche mit.
- Der Runtime-Fuzzer fuehrt echte Backend-, DMA-, CPU- und ROM-RAM-Copy-Pfade aus, vergleicht Callsite-Cache und generischen Dispatch und reduziert Sanitizer-/Prozesscrashes in Kindprozessen. Minimierte Hex-Inputs sind direkt wiedergebbar.
- Leere IR-Bloecke werden strukturiert abgelehnt, ohne im zweiten Verifierdurchlauf dereferenziert zu werden.

## [0.34.0] - 2026-07-16

### Behoben

- KR-3101-Nacharbeit: Scheduler-Resets recyceln keine Ereignis-IDs mehr und benachrichtigen registrierte Laufzeitzeitgeber. Aktive TMU-/RTC-Quellen werden auf Zyklus null neu verankert, ohne spaeter ueber veraltete Handles fremde Ereignisse zu loeschen oder einzufrieren.
- KR-3102-Nacharbeit: TMU-TPSC `110` verwendet eine gemeinsame rationale 16,384-kHz-RTCCLK-Domaene statt eines 64-Hz-Ersatztakts. R64CNT besitzt sieben wirksame Bits; RTCEN und Kalender-START sowie CF und CIE sind getrennt, Divider-Reset rephasiert periodische Ereignisse und RTC-getaktete TMU-Kanaele, und TMU-Pending bleibt aus `UNF && UNIE` abgeleitet.
- KR-3409-Nacharbeit: Wiederholte dynamische Interpreter-Fallbacks registrieren identische gueltige Bloecke idempotent und reaktivieren invalidierte Bloecke derselben Adresse, Groesse und Provenienz. Abweichende Geometrie bleibt ein sichtbarer Fehler; Block-, Link- und Invalidierungszaehler werden nicht dupliziert.
- KR-3407-Nacharbeit: Schleifen-Safepoints ziehen bei erschoepftem Ereignisbudget nur die tatsaechlich erreichte Zyklusdifferenz ab. Wiederholte Stopps erreichen den exakten Zielzyklus; ein Budgetstopp ohne Zyklusfortschritt bricht sichtbar statt endlos ab.
- KR-3408-Nacharbeit: Die Watchpointgeneration ist nun Bestandteil des echten Laufzeit-`BlockVariantKey`; ein integrierter Tabellenlookup kann eine vor der Watchpointaenderung registrierte Variante nicht wiederverwenden.
- KR-3304-Nacharbeit: Ein versioniertes Katana-Artefaktmanifest entfernt bei wiederverwendeten Ausgabeordnern ausschliesslich zuvor erzeugte, nun veraltete Units, Metadaten, Symbole und Konstantdateien. Fremde Nutzerdateien bleiben erhalten; fehlgeschlagene Bereinigung nennt den betroffenen Pfad und bricht sichtbar ab.
- KR-3304-Nacharbeit: Artefaktpfade werden vor Schreiben und selektiver Bereinigung komponentenweise auf symbolische Links und kanonisches Verlassen des Ausgabeziels geprueft. Vorhandene Nutzerdateien bleiben unangetastet; Symlink-Ausbrueche werden sichtbar abgelehnt.
- KR-3101/3102-Nacharbeit: Ein Scheduler-Reset bei vor der TMU erzeugter RTC storniert alte TMU-Ereignisse vor der Neuverankerung und kann deshalb keine doppelten Unterlaeufe erzeugen.
- KR-3103-Nacharbeit: Nach NMI oder DMA-Adressfehler werden bereits angenommene externe DMAC-Anforderungen verworfen; `DME=0` pausiert sie dagegen weiterhin ohne Datenverlust.
- Delay-Slot-Nacharbeit: `BSR` und `JSR` schreiben `PR` erst nach einem erfolgreichen Delay Slot, waehrend `RTS` sein Ruecksprungziel vor dem Slot festhaelt. Ausfuehrbare Speicher- und FPU-Ausnahmeregressionen sichern Owner-PC, Registerzustand und verschachtelte Aufrufe.

### Hinzugefuegt

- Die Roadmap schliesst die bisherige Luecke zwischen v0.44 und v0.50 mit den
  Alpha-Integrationsstufen v0.45 bis v0.49 fuer ISA-Abdeckung, Retail-Boot,
  native Hostruntime, Portintegration und Alpha-CI.
- Der lokale Alpha-Vertrag umfasst den offiziellen Pfad von einer
  Nutzer-GDI ueber ein externes Port-Projekt zu `game.exe`; Alpha gilt als
  erreicht, wenn die Anwendung startet und reproduzierbar `KR_RETAIL_BOOTED`
  erreicht. Ein erster Frame oder Gameplay bleibt Beta-Scope.
- KR-3401: Eine deterministisch sortierte Laufzeit-Blocktabelle verbindet virtuelle Diagnoseadressen, kanonische physische Herkunft, Blockgrenzen, Endtypen und Backendfunktionen. Statische sowie dynamische Eintraege teilen den Lookup; MMU-, FPSCR-, Adressraum- und Runtime-Varianten bleiben explizit, waehrend Ueberlappungsfehler beide Provenienzen nennen.
- KR-3402: Der generische indirekte Dispatch trennt Calls, Tail-Jumps und Returns, kanonisiert P1-/P2-Ziele ueber den Adressraumvertrag und protokolliert Callsite, Ziel, PR, Quellblock sowie Lookupart. Unbekannte Ziele brechen sichtbar ab; Calls setzen PR, Returns verwenden PR und Jumps bewahren ihn.
- KR-3403: Kontrollierte Fallbackrichtlinien fuer Abbruch, reine Diagnose, Interpreter und expliziten Nutzerhook klassifizieren unbekannte Opcodes, ungeloesten Kontrollfluss sowie dynamischen Code getrennt. Jede Nutzung wird stabil gezaehlt; CPU, Speicher, Ausnahme- und Schedulerkontext werden an einer typisierten Grenze synchronisiert, und Fortsetzung ist nur an der vereinbarten Blockgrenze erlaubt.
- KR-3404: Ein hostunabhaengiger Tracker registriert ausfuehrbare RAM-Seiten, vereinheitlicht CPU-, DMA- und Copy-Writes ueber kanonische physische Adressen, erhoeht Seitengenerationen, invalidiert alle ueberdeckenden Blockvarianten und loest eingehende Links. Bytegleiche Writes koennen nur nach explizitem Nachweis ausgenommen werden; Hotspots bleiben messbar.
- KR-3405: Ein Firmware-Handoff-Modell verbindet gleichzeitig gemappte ROM-, RAM-, Flash- und MMIO-Segmente, vereinigt physisch identische P1-/P2-ROM-Urspruenge und verfolgt verifizierte ROM-nach-RAM-Codekopien. Veraenderte Kopien verlieren ihren statischen Beweis; dynamisch installierte BIOS-ABI-Vektoren bleiben als Laufzeitsymbole sichtbar.
- KR-3406: Der kanonische Blockdispatcher implementiert Fallthrough, statische, bedingte und dynamische Spruenge, Calls, Returns, Exceptions sowie Interrupt-Safepoints als getrennte Endklassen. Direkte Nachfolger und Fallthrough bleiben getrennt; virtuelle Diagnose- und physische Lookupadresse werden bewahrt, und generische Relink-/Unlink-Hooks loesen alle eingehenden Direktlinks stabil.
- KR-3407: Explizite Scheduler-Safepoints verbrauchen reproduzierbare Gastzyklen an Blockenden, Schleifenrueckkanten sowie vor und nach Delay Slots. Ein festes Schleifenquantum verhindert Eventverhungerung; Backend und Fallback teilen denselben Budgetpfad, waehrend Ereignisse, Interruptzustellung, Jitter und Budgetstopps maschinenlesbar bleiben.
- KR-3408: Ein expliziter Instruktions- und Datenuebersetzungsvertrag trennt No-MMU-Fastpaths von TLB-gestuetzter Ausfuehrung. MMUCR, LDTLB-/TLB-, Adressraum- und Watchpointgenerationen sowie FPSCR PR/SZ/FR/RM werden als Blockwaechter erfasst; Rechtefehler erzeugen strukturierte SH-4-Ausnahmen und aktive MMU-Bloecke enden konservativ an Seitengrenzen.
- KR-3409: Eine praezise Interpretergrenze erlaubt Eintritt nur am synchronisierten Gast-PC und Austritt nur an der vereinbarten Blockgrenze. Delay-Slot-Owner, Speicherfehler, Watchpoints, Schedulerbudget und strukturierte Ausnahmen werden mit generiertem Code geteilt; dynamischer Code nutzt dieselbe Provenienz- und Invalidierungsschicht, und Manifeste koennen jeden stabil gezaehlten Grund verbieten.
- KR-3410: Zwei getrennte 32-Byte-Store-Queues modellieren ihre P4-Schreibfenster, QACR0/QACR1-Zielbildung, Queueauswahl und exakt ausgerichtete `PREF`-Transfers zu RAM oder Tile Accelerator. `OCBI`, `OCBP`, `OCBWB`, `ICBI` und `MOVCA.L` besitzen explizite Profileffekte; Codewartung und `MOVCA.L` invalidieren ausfuehrbares RAM, waehrend nicht aktiviertes Operand-Cache-RAM vor LLE-Ausfuehrung sichtbar abgelehnt wird.
- Generiertes `PREF` erreicht ueber die versionierten Plattformdienste die echten SH-4-Store-Queues. Der kompilierte End-to-End-Pfad prueft SQ0 nach RAM und SQ1 zum Tile-Accelerator-Sink; normales `PREF` bleibt nebenwirkungsfrei.

### Geaendert

- Die v0.34-Gate-Vorbereitung wurde nach allen Scheduler-, Timer-, DMAC-, Delay-Slot-, Store-Queue- und Pfadsicherheitskorrekturen vollstaendig wiederholt: ein neu erzeugter lokaler Debug-Build besteht 142/142 Tests. Die Vorbereitung ist vom Nutzer fuer KR-3411 freigegeben; Versionierung und Tag erfolgen erst im getrennten Release-Commit.
- Implementierungs-Tasks werden vor einem Phasen-Gate zuerst ohne
  routinemaessige Builds und Testlaeufe abgearbeitet. Der letzte
  Gate-Vorbereitungstask setzt die gesammelten Tests um und erstellt den
  frischen Build samt vollstaendiger Regression.
- Vor jedem Phasen-Release-Gate gilt ein verpflichtender Nutzerreview-Stopp.
  Versionierung, Release-Commit, Tag und Veroeffentlichung beginnen erst nach
  ausdruecklicher Freigabe; Review-Aenderungen wiederholen die
  Gate-Vorbereitung.
- Sonic Adventure wird vor der Alpha-Gate-Vorbereitung KR-4999 nicht
  ausgefuehrt. Fruehere lokale GDI-Proben bleiben historische Quellen- und
  Bootblockdiagnosen, nicht Sonic-Ausfuehrungsnachweise.

## [0.33.0] - 2026-07-16

### Hinzugefuegt

- KR-3301: Funktionen werden unabhaengig von ihrer Eingabereihenfolge deterministisch nach Gastadresse in groessen- und instruktionsbegrenzte Translation-Unit-Partitionen aufgeteilt.
- KR-3302: Translation Units erhalten portable Namen aus Partitionsindex, Gastadressbereich und Hash der kanonischen IR-Serialisierung; Hostpfade und Eingabereihenfolge bleiben ohne Einfluss.
- KR-3303: Ein inhaltsadressierter Codegen-Cache verwendet versionierte, komponentenweise getrennte Schluessel, bewahrt bytegleiche Treffer ohne Neuschreiben und sperrt Pfadausbrueche ueber Artefaktnamen.
- KR-3304: Eine begrenzt parallele Projektausgabe nutzt Cachetreffer, schreibt Artefakte in stabiler Reihenfolge und erzeugt bytegleiche CMake-, Ninja- und Compile-Commands-Integration.
- KR-3305: Versionierte Blockmetadaten erfassen virtuelle und physische Gastadressen, Segment, Bytebereich, Provenienz, Opcodes, Zyklen, Endtyp, Nachfolger und Zustandswaechter; Code, Konstanten, Symbole und Runtime-Metadaten bleiben getrennt.

### Behoben

- KR-3301-Nacharbeit: Eine einzelne Funktion oberhalb des konfigurierten Instruktionslimits wird sichtbar mit `length_error` abgelehnt, statt eine angeblich begrenzte Uebergroessenpartition zu erzeugen.

### Geaendert

- Das v0.33.0-Gate besteht mit 132/132 Tests in einem frischen lokalen Debug-Build. Nach aktueller Gate-Strategie bleibt Sonic Adventure bis zur Alpha-Gate-Vorbereitung KR-4999 unausgefuehrt.

## [0.32.0] - 2026-07-16

### Hinzugefuegt

- KR-3201: Eine IR-basierte Backend-Schnittstelle validiert Programme und Einstiegspunkte zentral und trennt Deklarationen, Funktionen sowie Metadaten in deterministisch zusammensetzbare Emissionsabschnitte.
- KR-3202: Der bestehende C++-Emitter ist als Referenzbackend hinter die modulare Schnittstelle migriert; die bisherige `emit_cpp_program`-API bleibt als kompatibler, getesteter Einstieg erhalten.
- KR-3203: Backend-Interface-ABI, Runtime-ABI und explizite Faehigkeitsmasken werden vor der Emission geprueft; Fehler nennen Backend sowie den abweichenden Vertrag, bevor Code erzeugt wird.
- KR-3204: Eine versionierte Block-ABI trennt virtuelle und kanonische physische Gastadressen, typisiert alle Blockendklassen und synchronisiert CPU-, Delay-Slot-, Ausnahme- sowie Schedulerzustand an Backend- und Fallbackgrenzen.
- KR-3205: Versionierte Plattformdienste kapseln Speicher, Scheduler, Interrupts, DMA und kontrollierten Fallback mit expliziten MMU-, Watchpoint-, RAM- und Firmwarefaehigkeiten; generierter Code kennt keine konkreten Dreamcast-Subsystemtypen.

### Behoben

- Phase-6-Gate-Nacharbeit: Ein unveraenderter CPU-Zustand gilt nicht mehr als Blockausfuehrung, die dokumentierte CCR-Invalidierung ist verpflichtend, nur Calls im kopierten Einstiegsblock blockieren die Probe, und GDI-Tracks koennen weder die Descriptorgrenze noch die vollstaendige Hash- und Cleanup-Pruefung umgehen.

### Geaendert

- Das v0.32.0-Gate besteht mit 127/127 Tests in einem frischen lokalen Debug-Build. Nach aktueller Gate-Strategie bleibt Sonic Adventure bis zur Alpha-Gate-Vorbereitung KR-4999 unausgefuehrt.

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
- Das v0.31.0-Gate besteht mit 122/122 Tests im frischen Debug-Build. Zwei identische lokale GDI-Blockproben erreichen bytegleich und ohne Quellaenderung den Checkpoint `KR_PHASE6_MAIN_EXECUTION_STARTED`; er gilt nur als Quellen-/Bootblockdiagnose.

### Behoben

- KR-3101-Nacharbeit: Rekursive `advance_to()`-/`advance_by()`-Aufrufe und `reset()` aus Scheduler-Callbacks werden sichtbar abgewiesen. Die Gastzyklusuhr kann dadurch nicht mehr durch ein verschachteltes Advance zurueckspringen; verschachtelte Planung und Cancellation bleiben erlaubt.
- KR-3105-Nacharbeit: Laufgenerationen verhindern doppelte oder nicht mehr abbrechbare Video-/Audioereignisse, wenn ein Callback die Medienuhr stoppt, neu startet oder zuruecksetzt.

## [0.30.0] - 2026-07-15

### Hinzugefuegt

- Die zentrale lokale Akzeptanzstrategie definiert verteilbare synthetische/Homebrew-Checkpoints bis v0.49.0 und den ersten Sonic-Adventure-Ausfuehrungstest in der Alpha-Gate-Vorbereitung KR-4999. Der v0.30.0-GDI-Smoke wird nur als Quellen-/Bootblockprobe revalidiert.

- KR-3001: Eine gemeinsame read-only DiscSource-Abstraktion stellt bereichsgepruefte Speicher- und Hostdateiquellen mit expliziter semantischer Identitaet bereit; Hostpfade sind weder Identitaet noch Schreibziel.
- KR-3002: Ein GD-ROM-Laufwerksmodell verarbeitet Ready-, Status-, Kapazitaets- und Sektorlesekommandos mit Big-Endian-Kapazitaetsantworten und expliziten No-Media-, Feld-, Befehls- und Bereichsfehlern.
- KR-3003: Ein read-only ISO9660-Pfad validiert den Primary Volume Descriptor und Both-Endian-Directory-Records, normalisiert Versionssuffixe und liest Dateien case-insensitive auch aus Unterverzeichnissen.
- KR-3004: Asynchrone GD-ROM-Requests verwenden ausschliesslich explizite Gastzyklen, Basislatenz und Sektorkosten; Fertigstellungen sind stabil geordnet und Zeitrueckspruenge sowie Zyklusueberlaeufe sichtbar.
- KR-3005: Der `.gdi`-Parser liest quoted relative Trackpfade, Typ, Sektorformat, Offset und Reihenfolge in ein Modell mit Zeilenprovenienz und weist fehlende Dateien, Trackkonflikte sowie unplausible Dateigroessen read-only ab.
- KR-3006: GDI-Mehrdateiquellen liefern pfadunabhaengige Inhaltsidentitaet, Raw-Audiotracks und normalisierte 2048-Byte-Datensektoren ueber dieselbe DiscSource-, GD-ROM- und ISO9660-Kette; der Read-only-Vertrag ist dokumentiert und getestet.

### Geaendert

- Alte versionierte `.katana_backup_*`-Migrationssnapshots wurden aus dem aktuellen Repository-Baum entfernt. Die Arbeitsregeln erlauben nur noch genau ein unversioniertes Quellbackup des neuesten committed Stands und schliessen Build-, Referenz- sowie private Spieldaten aus.
- Die vollstaendige Regression umfasst 114 Tests und besteht in frischen lokalen Debug- und Release-Builds. Der damalige lokale GDI-Nachweis wird heute nur als Quellen-/Bootblockprobe gewertet; der erste Sonic-Adventure-Ausfuehrungstest ist KR-4999.

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
