# Projektstatus

Abgeschlossener interner Meilenstein: `v0.47.0`
Phase: `v0.48.0` - Integration
Naechster Roadmap-Task: `KR-4811`
Naechstes Gate: `v0.48.0` - Integration
Weitere interne Gates: `v0.48.0` Integration und `v0.49.0` Alpha-Candidate
Erster oeffentlicher Release: `v0.50.0` Alpha

## Aktiver P0: Sonic-Adventure-PAL bis zum ersten echten Gastframe

Der allgemeine Disc-Hardwareauditor erfasst fuer den aktuellen privaten
PAL-Build 55.504 erreichbare SH-4-Instruktionen in 815 Funktionen. Es bleiben
keine unbekannten SH-4-Instruktionen und keine harte statische Hardwareluecke.
Der SCIF-Produktpfad, AICA-ARM-Reset, PVR-Blank/Border-Scanout und die komplette
Store-Queue-/Channel-2-DMAC-/TA-/ASIC-Kette sind allgemein implementiert.

Der generierte Port praesentiert erst nach einem erfolgreich gerenderten
PVR-Frame und meldet `KR_FIRST_GUEST_FRAME` nicht fuer blosses VBlank oder
erzwungenes Blank. Eine optional aktivierte, leichtgewichtige Diagnostik
erfasst letzten MMIO-Zugriff, aktive Interruptquelle, alle relevanten
DMA-Zustaende, GD-ROM, TA/PVR, AICA und den Demand-Materializer. Der emittierte
Produktcode wurde mit einer synthetischen High-Density-GDI bis zur nativen EXE
gebaut. Der aktuelle ABI-30-/Portvertrag-17-PAL-Lauf fuehrt den rekompilierten
Disc-Systembootstrap stabil aus: 150 Millionen Gastzyklen beziehungsweise
41.808.508 beobachtete Bloecke laufen ohne Exception bis in dessen belegte
10.000-Iterationen-Warteschleife. `FB_R_CTRL` ist programmiert; TA, GD-ROM und
ein echter PVR-Frame wurden bis zu diesem Punkt noch nicht beobachtet. Ein
5-Millionen-Zyklen-Lauf sank durch gecachtes Hostpolling und das Auslassen von
Codeinvalidierung fuer reine OCRAM-Stackwrites zunaechst von rund 5,8 auf
4,265 Sekunden. Invalidierungsgesichertes lokales P1-/P2-Blockchaining senkt
denselben Lauf weiter auf 2,523 Sekunden; Gastzustand, Zykluszahl und 1.001.521
beobachtete Bloecke bleiben identisch. Ein erster Gastframe wird weiterhin
ausdruecklich nicht behauptet. Die zusammenhaengende
ASan-/Artifact-Gesamtvalidierung ist mit 180 von
180 Tests bestanden; der anschliessende fokussierte Produktpfadlauf ist mit 28
von 28 Tests gruen. Der danach frisch exportierte und lokal aus der
unveraenderten PAL-Disc installierte Port erreichte 17.092.443 beobachtete
Bloecke beziehungsweise 50.829.325 Gastzyklen. Der naechste allgemeine Blocker
ist damit belegt: Der Gast erreicht seinen zur Laufzeit installierten
Exception-Handler bei `VBR+0x100`, dessen Haupt-RAM-Bytes der bisher nur fuer
vorab katalogisierte Quellen offene Materializer noch als `unknown-source`
abwies. TA/PVR blieben bis dahin inaktiv; ein erster Gastframe wird weiterhin
ausdruecklich nicht behauptet.

Der allgemeine Fix verfolgt geaenderte Haupt-RAM-Bytes kanonisch und autorisiert
erst beim echten Kontrolltransfer einen bytegenauen, auf 128 Bytes begrenzten
Runtime-Code-Snapshot. Unbeschriebenes RAM bleibt gesperrt; jeder
ueberlappende Write entwertet Snapshot und Runtimeblock. Die gezielte
Materializer-Regression besteht unter AddressSanitizer. Der erneute private
PAL-Nachweis materialisierte den Handler nachweislich als Interpreterblock und
erreichte dessen Zugriff auf `EXPEVT` bei `0xFF000024`. Damit wurde die naechste
allgemeine Luecke sichtbar: `TRA`, `EXPEVT` und `INTEVT` existierten nur als
interner CPU-Zustand, aber noch nicht im produktiven P4-Bus. Alle drei
32-Bit-R/W-Register sind nun samt Area-7-Alias, reservierten Bitmasken und
Hardwareauditorabdeckung implementiert. Der folgende private Nachweis las
`EXPEVT=0x100` korrekt und materialisierte sieben Handlerbloecke. Als Ursache
der Exception ist nun die bisher absichtliche Ablehnung von `DMAOR.DDT`
belegt. Der allgemeine DMAC-Vertrag akzeptiert das R/W-Modusbit jetzt und
bildet begrenzte DDT-Requestqueues sowie den TR-only-Wiederholpfad ab. TA/PVR
blieben im folgenden 100-Millionen-Zyklen-Lauf zwar ohne Exception, doch ein
PVR-Softreset setzte faelschlich den gesamten Registerblock zurueck und liess
`SPG_STATUS` dauerhaft auf Scanline null stehen. TA-, Core- und
SDRAM-Interface-Reset sind nun getrennt; SPG-/Framebufferzustand und aktives
Scanouttiming bleiben erhalten. Der frische private PAL-Port verlaesst diese
Warteschleife, programmiert `FB_R_CTRL`, `FB_R_SIZE` und `VO_CONTROL` und
erreicht nach 51.288.624 Gastzyklen neuen Runtimecode ohne Exception. Vier
dynamische Interpreterbloecke wurden korrekt materialisiert; ein spaeterer
Sprung auf eine innere Instruktion desselben Snapshots wurde jedoch als
ueberlappendes zweites Modul abgewiesen. Der allgemeine Materializer verwendet
solche geraden Inneneinstiege nun nur fuer interpretergestuetzte Bloecke und
nur nach erneuter Byte-, Generation-, Varianten- und Herkunftspruefung. Native
Inneneinstiege bleiben gesperrt; Rewrite- und Aliasinvalidierung sind durch
Regressionsfaelle belegt. Der erneute PAL-Nachweis deckte ausserdem einen
ueberbrueckenden, erst in umgekehrter Reihenfolge entdeckten Nachbarblock auf.
Der generierte Demand-Interpreter materialisiert deshalb genau eine Instruktion
beziehungsweise einen Kontrolltransfer samt untrennbarem Delay-Slot. Die
gezielten Export-, Interpreter- und Modulregressionen sind gruen; der private
Produktlauf erreicht damit rund 51,3 Millionen Gastzyklen ohne Block-Overlap.
Der dort folgende Address-Error ist auf `0x00710000` eingegrenzt: Der Auditor
hatte das AICA-RTC-Fenster faelschlich als implementiert bewertet, obwohl die
Runtime nur die AICA-Register bis `0x00707FFF` mappte. Die drei RTC-Register
sind nun samt Gastzeit, Write-Enable, Breiten, Aliasen und Reset allgemein
implementiert; auch der Auditorvertrag besitzt positive und negative
Registertests. Im ersten ABI-22-Lauf ist der RTC-Address-Error nachweislich
verschwunden. Der folgende `INTEVT=0x3A0`-Fallback belegte stattdessen eine
vertauschte System-ASIC-Zuordnung: Das vom Gast aktivierte Level-6-Maskenfenster
wurde als Level 2 zugestellt. Die drei Leitungen sind nun allgemein auf
Level 2/4/6 und `INTEVT` `0x3A0/0x360/0x320` korrigiert. Der nachfolgende Lauf
isolierte den gueltigen SYSTEM-1-Rueckweg nach abgeschlossener
Gastinitialisierung. Der Vergleich mit dem freien Reios-Handoff zeigte, dass
der native Direktstart nur PC/SP/VBR/SR, nicht aber den uebrigen BIOS-CPU- und
Geraetezustand setzte. Dieser allgemeine Handoff umfasst jetzt `T`, `FPSCR`,
GBR/SSR/SPC/SGR/DBR/PR, DMAOR, AICA-Masken und PAL-Portzustand. Ein echter
Address-Error deckte anschliessend auf, dass der alte SYSINFO-HLE-Stub mit
`VBR+0x100` kollidierte. Alle HLE-Stubs liegen nun ausserhalb der
SH-4-Exceptionvektoren. Der neue Dispatch-Hotspotbericht wies zudem 2.097.152
Aufrufe desselben gueltigen
Vier-Byte-Kopierpfads als Bootzeit-Hotspot statt als Dispatchfehler nach.

Der anschliessende PAL-Lauf erreichte reproduzierbar `SYSTEM 1`. Ein erster
Shortcut setzte nur den CPU-Handoff und sprang direkt zur vorhandenen
Bootdatei; der erneute Audit verwarf diesen unvollstaendigen Vertrag. Der
allgemeine Runtimepfad liest jetzt Bootstrap und Bootdatei erneut aus der
gebundenen Discquelle, rekonstruiert den definierten BIOS-RAM-Zustand samt
Vektoren und setzt DMAOR, AICA-Masken, Cache sowie PAL-Port erneut. Der Einstieg
erfolgt wieder ueber den Systembootstrap bei `0x8C008300`. Der direkte
GD2-BIOS-Einstieg `0x8C0010F0` ist als zusaetzlicher Aliasblock vorhanden.
Synthetische Regressionen veraendern Bootbytes und Geraetezustand vor SYSTEM 1
und beweisen deren Wiederherstellung.

Der erweiterte Kontrollflussaudit belegt den anschliessenden Soft-Reboot als
absichtlichen Fatalpfad des fruehen Gastcodes. Ein unabhaengiger
Flycast-/SH-4-Abgleich deckte dabei einen echten allgemeinen Portfehler auf:
Pinmodus 3 wurde wie GPIO-Ausgang behandelt und machte aus dem PAL-Composite-
Wert `0x0300` faelschlich `0x0304`. Die korrigierte 2-Bit-Modusdekodierung ist
regressionsgetestet. Der private A/B-Lauf erreicht danach weiterhin keinen
GD-ROM-/TA-/PVR-Zugriff; der vorgelagerte Statuspfad bleibt daher aktiver P0.

Speicherproben vor dem Fatalpfad belegen die Ursache inzwischen enger: Das
gesamte Disc-Systemfenster ab `0x8C008000` war null, weil der bisherige
Direct-Boot nur die Bootdatei nach `0x8C010000` lud. Der PAL-SYSINFO-Wert war
dagegen korrekt. Der gegen Flycast-Reios und die lokale BIOS-Zerlegung
abgeglichene, aber eigenstaendig implementierte Produktvertrag liest deshalb
die ersten 16 Datensektoren als separates Bootstrapsegment, nimmt dessen
Einstieg bei `0x8C008300` in Analyse, IR und nativen Codegen auf und laedt
Bootstrap sowie Hauptprogramm vor dem Gaststart. Die sieben Sektoren nach
`0x8C008100` bleiben davon getrennt die Semantik des BIOS-Discpruefaufrufs.
Sechs fokussierte Boot-, Manifest-, GUI- und Portexportregressionen sind gruen;
der erste frische PAL-Nachweis erreichte den nativen Bootstrap und belegte
`CCR.ORA` sowie einen Stack im SH-4-On-Chip-RAM. Das zuvor fehlende allgemeine
OCRAM-Modell bildet nun das 8-KiB-Backing, ORA/OIX-Indexierung, Reset,
MMU-Ausnahme und Hardwareaudit ab. Der danach frisch exportierte ABI-30-Port
erreicht mit der unveraenderten PAL-Disc 5.000.000 Gastzyklen und 1.001.521
Gastbloecke ohne Exception oder Materializerfehler. Der letzte beobachtete
MMIO-Zugriff liegt im aktiven OCRAM; der fruehere Abbruch nach 12 Gastzyklen
ist damit beseitigt. TA/PVR und ein echter Gastframe bleiben fuer den laengeren
Folgelauf weiterhin offen.

Runtime-ABI 30, BIOS-ABI 5 und Portprojektvertrag 18 bilden den kumulativen
v0.48-Stand ab.
PlatformServices-ABI 8 versioniert das invalidierungsgesicherte lokale
Blockchaining.

Die zusammenhaengende zweite Bootkorrekturrunde ist implementiert. Sie umfasst
RTC-Schreiblatch, vollstaendige MMU-Miss-/Multiple-Hit-Semantik, `MMUCR.SV`,
physisch gebundene AOT-/Demand-Varianten, bytegenaue Runtime-Codepromotion,
pixelgebundenen Gastframe-Nachweis sowie zustandsfuehrende Holly-DMA-Fehler.
Separater PVR-DMA ist jetzt vom Channel-2-Ereignis getrennt und an Kanal 0 des
SH-4-DMAC samt Residue gebunden; AICA triggert G2 nur noch ueber einen echten
expliziten Request. Die zehn fokussierten ASan-Regressionen sind gruen. Ein
frischer Export und die einmalige lokale Installation aus der unveraenderten
PAL-GDI sind erfolgt. Der erste budgetierte Produktlauf erreichte 51,3 Mio.
Gastzyklen und 17,29 Mio. Dispatchbloecke mit 367 erfolgreichen dynamischen
Materialisierungen, aber weiterhin ohne TA-/PVR-Aktivitaet oder Gastframe. Er
legte eine reihenfolgeabhaengige Ueberlappung zweier Runtime-Code-Snapshots
offen. Der allgemeine Katalog begrenzt ein neu entdecktes Fenster nun am
Beginn jedes bereits aktiven Nachbarmoduls; die fokussierte Regression fuer
rueckwaerts entdeckte Codebereiche ist gruen. Der erneute PAL-Nachweis
beseitigt die Runtime-Code-Ueberlappung und beendet sich sauber nach 51,3 Mio.
Gastzyklen mit 367 erfolgreichen dynamischen Materialisierungen. Er deckt den
naechsten allgemeinen Kontrollflussfehler auf: Der Root-Dispatcher behandelte
das erste `RTS` eines runtime-materialisierten Aufrufs wie das Ende des
gesamten Portprogramms. Programmroot und verschachtelte Host-Aufrufgrenzen sind
nun getrennt; der Root folgt dem gelatchten Gast-Returnziel und endet nur an
seinem expliziten Einstiegssentinel. Der folgende private Lauf erreicht 582
erfolgreiche Runtime-Materialisierungen und endet nun sichtbar statt
faelschlich erfolgreich. Dabei wird der BIOS-SYSTEM-Vektor aufgerufen. Dessen
Funktionscode wurde bisher irrig aus dem fuer Cacheadressen verwendeten `r7`
gelesen; KallistiOS, die lokale BIOS-Analyse und Flycast-Reios belegen fuer
diesen Vektor uebereinstimmend das erste SH-4-C-Argument `r4`. BIOS-ABI 2
korrigiert den Selektor und bindet die zurueckkehrenden Funktionen 0 und 2 an
ASIC/PVR beziehungsweise den lokalen GD-ROM-Bootstrap. Der erneute private
Nachweis steht aus; ein Gastframe wird bis dahin weiterhin nicht behauptet.

Der iterative Portworkflow verwendet jetzt sicheres inkrementelles Staging:
Vorhandene Buildobjekte werden fuer denselben Ausgabeport wiederverwendet,
waehrend `user-data` und `*.katana-disc` niemals in Staging oder Paket gelangen.
Nach erfolgreicher atomarer Publikation wird das lokale `user-data` direkt vom
alten in den neuen Port verschoben; ein Fehler rollt auf den exakten alten Port
samt Disc-Cache, Flash und VMU zurueck. Der private PAL-Rebuild hat diesen Pfad
mit erhaltenem 1.227.063.528-Byte-Pack und ohne liegengebliebene Stage-/Stale-
Verzeichnisse bestaetigt.
Bytegleiche Quellen behalten ihre Zeitstempel; AOT-Codegen und MSVC-Kompilierung
arbeiten parallel. Die Workerzahl folgt der Host-CPU und kann mit
`KATANA_PORT_CODEGEN_JOBS` begrenzt werden. Eine zusaetzliche identische Regeneration erhoehte den
synthetischen Gesamt-Porttest nur um rund fuenf Sekunden. `KR-4813` bleibt fuer
den weitergehenden content-addressed Analyse-/IR-Cache offen.

## KR-4831 technisch abgeschlossen

Der verteilbare Portexport schreibt keinen vollstaendigen Retail-Disc-Pack
mehr. Er veroeffentlicht eine spielagnostische, pfadfreie
`game.katana-install`-Recipe mit Descriptor-, Boot-, Content- und
Track-SHA-256 sowie der kanonischen Trackgeometrie. Der generierte native
AOT-Port verlangt einmalig `game.exe --install-disc <eigene.gdi>`, validiert
die Originaldisc vollstaendig und erzeugt den lokalen Pack atomar nur unter
`user-data/content/`. Synthetische Differenz-, Negativ- und CLI-End-to-End-
Tests sind gruen; Sonic Adventure PAL und Sonic Shuffle PAL bleiben private,
read-only Kompatibilitaetsfixtures und erhalten keine Sonderlogik.

Der abschliessende Identitaetsblock verwendet jetzt Disc-Pack-Format 2 und
Recipe 2. Die Content-Root entsteht aus den wirklich gelesenen Raw-Chunks und
wird sowohl beim Schreiben gegen die Recipe als auch beim Oeffnen gegen
Tracktabelle und Chunkindex geprueft; abweichendes Staging wird nicht
veroeffentlicht. Der damals eingefuehrte AICA-Vertrag ist im aktuellen
  Runtime-ABI 30 und Portprojektvertrag 18 enthalten. Das kumulative v0.48-
Debug-Gate ist mit 180 von 180 Tests bestanden; der anschliessende private
PAL-Nachweis ist erfolgt: Der vollstaendige neue Port entstand mit 12 Workern
in 126,8 Sekunden, ein inkrementeller Runtime-Neubau in 77,8 Sekunden. Die
Originaldisc installiert 521.461 Sektoren ausschliesslich lokal. Der echte
PVR-DMA-Steuerblock ab `0x005F7C00` und alle vier SH-4-Cachearray-Aperturen
sind nun geschlossen; der Vergleichslauf erreicht 17.516.050 native
AOT-Bloecke in 50 Mio. Gastzyklen ohne Exception. Ein Gastframe steht weiterhin
aus.

Die danach isolierte permanente Wartestelle las `SPG_STATUS` bei
`0xA05F810C`, waehrend der HLE-Handoff `SPG_LOAD=0` hinterlassen hatte. Der
Syncgenerator leitet Scanline, Field und Blank nun aus Gastzeit und
dokumentierten Registerfeldern ab. Der Firmware-Handoff waehlt fuer Europa das
PAL-Interlace-Profil und fuer Japan/Nordamerika NTSC-Interlace; zusaetzlich
sind PAL/NTSC non-interlaced und VGA geschlossen getestet. Der frische private
PAL-Port verlaesst `0x8C6044DE` nachweislich und erreicht bei 10 Mio.
Gastzyklen `0x8C0100E2` ohne Exception. TA-/PVR-Aktivitaet und der erste
Gastframe stehen weiterhin aus.

Der anschliessende P0-Runtimeblock korrigiert den zweiten SH-4-Ausfuehrungskern
und seine AOT-Grenze: Pre-Decrement-Fault-Rollback, SHAD/SHLD bei negativen
32er-Vielfachen, PR-Sichtbarkeit und -Rollback im Delay-Slot sowie die
Instruktionszaehlung sind durch eine eigene Regression belegt. Jeder native
AOT-Block kehrt fuer den naechsten Instruction Fetch in den zentralen
Dispatcher zurueck. MMU-Varianten werden nur bei identischer physischer
Codeherkunft an vorhandenen AOT-Code gebunden; echte Remaps verwenden keinen
stale Block. URC/URB und Multiple-Hit sind geschlossen getestet.

GD-ROM-Paketkommandos und G1-DMA schliessen nun erst nach Gastzeit ab. Bytes
werden vor der G1-Completion nicht sichtbar, normale Paketfehler bleiben im
ATA-Vertrag, und ein fehlgeschlagener PVR-Render erzeugt kein `RenderDone`.
Direct- und HLE-Firmwaremodus sind getrennt. Der C++-Emitter routet jeden
Gastzugriff direkt ueber die MMU-Helfer statt durch globale Textersetzung.
Die fokussierten P0-Regressionen und der vollstaendige 180er-Debug-Gate sind
gruen. Ein neuer privater PAL-Lauf ist noch nicht erfolgt, daher bleibt der
erste echte Gastframe offen.

## Privater PAL-Runtime-Bring-up

Ein frischer privater Hostbuild hat eine allgemeine MSVC-Parallelitaetsluecke offengelegt:
mehrere AOT-Uebersetzungseinheiten schrieben gleichzeitig dieselbe Ziel-PDB und konnten mit
`C1041` abbrechen. Runtime und generierte AOT-Bibliothek verwenden deshalb nun explizit `/FS`;
der erneute Produktbuild ist erfolgreich. Er veroeffentlichte 896 Funktionen in 14
AOT-Partitionen und drei Installer-Recipe-Tracks bei weiterhin null Retailsektoren im
verteilbaren Paket. Der lokale Disc-Installations- und Laufzeitnachweis steht noch aus.

Sonic Adventure PAL erreicht reproduzierbar mehr als 5,1 Millionen native
AOT-Bloecke, weiterhin ohne Gastframe. Die erste belegte Ausnahme bei
`0xFFD00000` wurde durch die vollstaendige SH-4-INTC-Registerfamilie behoben.
Der anschliessende Halt bei `0xA05F6800` fuehrte zu einem allgemeinen Audit
des Holly-Systembus-Steuerblocks: `0x005F6800..0x005F68AC`, seine direkten
Segmentaliase und die angrenzenden PVR-/G2-DMA-Triggermasken sind nun mit
geschlossenen Breiten-, Masken- und Zugriffsvertraegen umgesetzt. Channel 2
fuehrt Channel-2-TA-DMA ueber den realen Systembusblock und SH-4-DMAC aus;
ASIC-Triggermasken starten den davon getrennten PVR-DMA-Controller bei
`0x005F7C00` und G2-DMA gastzeitgebunden. Kein Pfad taeuscht Transfererfolg vor.

Die folgende Probe erreichte denselben Blockstand und identifizierte
`SB_MDSTAR` bei `0xA05F6C04` als naechste allgemeine Luecke. Der vollstaendige
Maple-Steuerblock ist inzwischen an einen echten, gastzeitgebundenen DMA-Pfad
angeschlossen: begrenzte Kommandotabellen, Schutzfenster, Controller-/VMU-
Transaktionen, DMA-Antwortwrites und genau eine System-ASIC-Completion sind
synthetisch sowie unter AddressSanitizer geprueft. Der bestehende High-Level-
Bus bleibt fuer direkte Plattformtests verfuegbar, waehrend Gastzugriffe den
MMIO-Pfad verwenden.

Der vollstaendig ausgelesene Rest derselben PAL-Initialisierungstabelle belegt
anschliessend G2-DMA, PVR-DMA und einen auf Null gesetzten GD-DMA-Start. Die
G1-/G2-/PVR-DMA-Registerfamilien und alle Direktsegmente sind nun geschlossen
gemappt. AICA/G2 und PVR verwenden echte, gastzeitgebundene DMA-Kopien mit
Zaehlern und ASIC-Completion; lineare Speicherregionen werden dabei ohne
Byte-fuer-Byte-Dispatch kopiert. GD-ROM-DMA darf initialisiert und deaktiviert
werden, verweigert einen aktiven Start aber weiterhin sichtbar, bis Discquelle,
Laenge und Completion an denselben Vertrag gebunden sind.

Der daraufhin frisch exportierte und aus der unveraenderten Originaldisc lokal
installierte Port erreichte 5.112.299 native Bloecke. Die naechste belegte
Ausnahme war ein Schreibzugriff auf `0x00000100`: Der Gast installiert dort
nicht etwa Low-Memory-Code, sondern kopiert seinen Handler nach `VBR + 0x100`.
Der direkte Runtime-Handoff hatte faelschlich den SH-4-Resetwert `VBR=0`
beibehalten. Der allgemeine Dreamcast-Spielhandoff verwendet nun die reale
Haupt-RAM-Vektorbasis `0x8C000000`; eine synthetische Produktpfadregression
haelt den Vertrag fest. Der folgende private Lauf bestaetigte den Fix und
erreichte 5.112.471 native Bloecke ohne SH-4-Exception. Er stoppte erst an
einem Runtime-only-Call auf einen P2-Codealias: Die unveraenderten Zielbytes
liegen im initialen Bootimage und beginnen an einer gueltigen
Instruktionsgrenze, waren der Analyse aber nur unter ihrem P1-Namen bekannt.
Der allgemeine Image-, Analyse- und Dispatchvertrag normalisiert deshalb nun
P0/P1/P2-Codealiase, kompiliert beschreibbare Snapshotkandidaten unter ihrer
kanonischen Imageadresse vor und bewahrt das angeforderte Aliasziel getrennt
vom nativen Ausfuehrungs-PC. Der erneute private Portbuild ist erfolgreich und
enthaelt den zuvor fehlenden Block `0x8C5FBFAC` nativ; Installation und Lauf
ueber die alte Grenze folgen als naechster Nachweis.

Der angrenzende Analyseaudit bindet auch explizite Einstiegspunkte,
Funktionssymbole, zusaetzliche Seeds und absolute wie relative Sprungtabellen
an denselben kanonischen Aliasvertrag. Der Vollstaendigkeitstest deckt wieder
alle 159 normalen SH-4-Metadatenregeln ab; seine alte 156er-Schranke war nach
den drei bereits implementierten Cache-/TLB-Befehlen selbst veraltet.

Der anschliessende PVR-Audit hat weitere spielagnostische Produktluecken
geschlossen: HOLLY2-Packed-Color liest bei untexturierten Vertices die Base
Color aus `0x18`; 64-Byte-Floatparameter erhalten Base- und Offsetfarbe; beide
Intensity-Modi besitzen korrekte Face-Color- und Vertexvertraege. Der
Software-Rasterizer fuehrt Tabellen-, Per-Vertex- und Tabellenmodus-2-Fog
sowie RGB-Color-Clamp nun wirklich aus, statt die TSP-Bits nur zu speichern.
Texturkoordinaten sind jetzt perspektivisch ueber die reziproke W-Tiefe.
Vierfach-Supersampling und Sekundaer-Akkumulation laufen im Fragmentpfad.
Trilinear-Pass A/B bleibt dagegen bis zu einer echten D-basierten
Mipmap-Levelwahl sichtbar abgewiesen; der vorherige einzelne, nur gewichtete
Bilinear-Sample wird nicht mehr faelschlich als Trilinearbild ausgegeben.
Der SA-gestuetzte Gesamtcheck hat danach eine allgemeine TA-Registerluecke
belegt: Das Spiel programmiert `TA_NEXT_OPB_INIT`, waehrend die Runtime bei
`TA_LIST_INIT` irrtuemlich `TA_OL_BASE` als Arbeitszeiger verwendete und
`TA_LIST_CONT` keinen Parserpfad besass. Beide Befehle folgen nun dem
dokumentierten HOLLY-Vertrag. Fortsetzungen erhalten abgeschlossene Primitive,
setzen den transienten Parserzustand kontrolliert zurueck und werden waehrend
offener oder unvollstaendiger Parameter abgewiesen. Adressmasken und read-only
Arbeitszeiger sind in Register- und FIFO-Regressionen geschlossen.
Diese Fortschritte sind synthetisch getestet; ein neuer privater PAL-Lauf ist
noch nicht erfolgt, daher bleibt der erste echte Gastframe offen.

Die PAL-GDI und alle Trackquellen bleiben unveraendert; veraltete
Portausgaben werden nur nach erfolgreichem Ersatz entfernt.

## KR-4704 technisch bestanden

Die aktuelle Kontrollflussfront umfasst in der privaten Build-only-Analyse
55.202 Instruktionen und 813 Funktionen. Die drei zuvor unbekannten
Cacheinstruktionen sind als allgemeine SH-4-Decoder-, IR-, Backend- und
Runtimepfade umgesetzt. OCBP und OCBWB verwenden einen expliziten kohaerenten
Operand-Cache-Vertrag: Der aktuelle Referenzspeicher besitzt keine verborgene
Dirty-Line und meldet deshalb weder erfundenen Write-back noch Speicherwrite.

Die sieben vollstaendig ungeloesten Sites und die 1.708 zuvor partiell
bewachten Sites wurden nach Herkunft und Vollstaendigkeit neu klassifiziert.
Beschreibbare Tabellen, zusammengefuehrte unvollstaendige Kontexte und echte
Laufzeitzeiger verwenden den validierenden Runtime-only-Dispatcher. Fruehere
Kandidaten bleiben nur als `analysis_candidates` fuer die monotone Analyse
erhalten und werden nicht als vollstaendige Laufzeitzielmenge exportiert. Der
Dispatcher akzeptiert ausschliesslich registrierte, ausgerichtete und fuer die
aktive Codegeneration gueltige Bloecke; ein Miss bricht strukturiert ab.

Der aktuelle private Aggregatstand ist `resolved=1`,
`guarded_complete=0`, `guarded_partial=0`, `runtime_only=1.826`,
`unresolved=0`, `unknown_instructions=0` und
`reachable_abort_edges=0`, `uncovered_control_targets=0` und
`dispatch_paths_without_validation=0`. Der neue Gatevertrag verlangt nicht
mehr die statische Klassifikation aller Bytes mit ausfuehrbarer
Speicherberechtigung. Unbekannter Speicher bleibt unbekannt und nicht
dispatchbar, bis ein validierter Kontrolltransfer ihn erreicht. Der private
doppelte Build-only-Nachweis ist erfolgreich; beide frischen Hostbuilds
besitzen identische portable Metadaten und generierte Quellen. Das aktuelle
Executable wurde als damaliges Gateartefakt neu gehasht, aber nicht gestartet.

Persistenz und Host-Pacing besitzen nun eigene Regressionen. Windows liest
Nutzerdatenpfade ohne unsicheren `getenv`-Pfad, Firmwarefehler werden in der
vertraglich richtigen Reihenfolge diagnostiziert, und der generierte Port
bindet persistente Flash-/VMU-Arbeitskopien sowie den ganzzahligen Hostpacer
ein. Gate und Paketierung erkennen die Visual-Studio-/Ninja-Werkzeuge robust,
vermeiden doppelte `PATH`-/`Path`-Eintraege, behandeln ASan im Releasepaket als
optional und testen den relocatable Port mit getrenntem Nutzerdatenpfad.

Der neue v0.47-Gate-Runner verbindet frische Debug-/RelWithDebInfo-Builds,
GUI-Paket, Harness-Selbsttest, privaten doppelten Build-only-Nachweis,
Datenaudit und atomische Root-GUI-Verteilung. Der Rootstand enthaelt eine
aktuelle verifizierte `KatanaRecomp-GUI.exe`, den Dialoghelfer und das
zugehoerige Runtime-SDK. Die fokussierten Decoder-, Analyse-, IR-, Backend-,
Runtime-, Portexport- und GUI-Regressionen sowie der relocatable synthetische
GDI-Paketbuild bestehen. KR-4704 und die anschliessende interne Freigabe
KR-4705 sind abgeschlossen; die aktive Arbeit liegt in v0.48.

Das neue exakte Inventar klassifiziert die zuvor pauschal betrachteten Bytes:
`proven_reachable_code`, `runtime_discovered_code`,
`unreachable_decodable_code`, `embedded_data`, `literal_pool`, `jump_table`,
`pointer_table`, `padding`, `overlay_candidate`, `module_candidate`,
`compressed_or_encoded` und `unknown_executable`. Oeffentliche Aggregate sind
adressfrei; lokale Details werden zusaetzlich nach Segment, Discdatei,
Ladephase und Schreibbarkeit gruppiert. 4-KiB-Seiten werden nach Rolle,
Beweisklasse, Referenz-, Relocation- und Zielevidenz, Entropie, Decodedichte
und Abstand zu bewiesenem Code gruppiert. Eine gueltige SH-4-Decodierung allein
ist kein Codebeweis. Anschliessend werden reale Ladegroesse,
Reservierung, Zero-Fill, Code-/Datensegmente, Boot-/Nachladerollen,
Berechtigungen, Alignment und Padding im Loadervertrag geprueft.

Reachability wird von der Pflicht zur Vorabkompilierung getrennt. Nur
`initially_reachable` und `statically_discoverable` gehoeren ohne weiteren
Vertrag zwingend in den statischen Bestand; `loadable_module` und
`runtime_materializable` duerfen das Gate nur mit sicherer Herkunfts-,
Byteidentitaets-, Lebenszeit-, Invalidierungs- und Materialisierungskette
verlassen. Neue Module und Overlays erhalten synthetische Fixtures. Die
Demand-driven-Materialisierung validiert Ziel und Herkunft, findet oder baut
einen budgetierten Block, registriert ihn und dispatcht erst danach. Sie bleibt
deterministisch und abschaltbar; ein Interpreter ist nur Referenzpfad.
Runtime-only-Sites werden nach Aufrufen, Zielvielfalt, Stabilitaet, Misses,
Materialisierungen und Invalidierungen profiliert, damit nachweislich mono-
oder klein polymorphe Sites spaeter sicher spezialisiert werden koennen.

Die allgemeine Grundlage ist nun umgesetzt. Der GDI-Loader beschreibt die
rohe Bootdatei als `mixed` statt pauschal als Code; ihre 6.735.296 Dateibytes
sind exakt committed, ohne reservierten Zero-Fill. Der private reine
Analyselauf fand 110.404 initial erreichbare Instruktionsbytes und 16.554
bewiesene Literalpoolbytes. MOVA liefert nur eine Adressreferenz und wird nicht
mehr faelschlich als Literalpool gerechnet. 408.019 gleichfoermige Bytes
bleiben als unbewiesene Padding-Kandidaten unbekannt; 6.200.319 Bytes bleiben
`unknown_executable`. Insgesamt bleiben 6.608.338 unbekannte Speicherbytes,
die ohne validierten Kontrolltransfer weder statisch analysiert noch
dispatchbar sind.
Bewiesenes Padding ist nur fuer streng quellgebundenes, ausgerichtetes
Dateiend-Fill ohne Referenz-, Relocation-, Modul- oder Kontrollflussevidenz
zulaessig; im privaten Abbild erfuellt aktuell kein Kandidat diesen Vertrag.
Module, Overlays, Lebenszeit, Relocationen, Byteidentitaet,
Invalidierung, budgetierte Materialisierung und Runtime-only-Siteprofile sind
synthetisch abgesichert. Runtime-ABI 14, Portprojektvertrag 6 und
Anwendungsvertrag 7 versionieren die Erweiterung. Die 1.603 dominant
unbekannten Seiten sind adressfrei in 14 Ursachengruppen zerlegt; die groessten
Gruppen besitzen keine Referenz, Relocation oder Kontrollflussevidenz und
werden nach Entropie und Decodedichte statt einzeln nach Adresse priorisiert.
Der Gesamtvertrag steht in
[`EXECUTABLE_INVENTORY_AND_MODULES.md`](EXECUTABLE_INVENTORY_AND_MODULES.md).

## KR-4703 umgesetzt

Flash und VMU verwenden getrennte lokale Arbeitskopien in einem versionierten,
SHA-256-gebundenen Primaer-/Recoverycontainer. Nutzerquellen werden nur gelesen
und vor jedem atomischen Save erneut verifiziert. Defekte Primaerkopien werden
nur aus einer vollstaendig validierten Recovery wiederhergestellt; keine
Fehlerbehandlung beschreibt oder ersetzt die Quelle. Die portable
Projektidentitaet trennt die Nutzerdaten verschiedener Ports.

Der generierte Port bindet persistentes Command-Flash und eine VMU ein. Ein
ganzzahliger Hostpacer wartet an Video-Gastzyklen auf monotone Hostdeadlines,
ohne Scheduler-, Audio-, Video- oder Eingabesemantik aus Hostzeit abzuleiten.
Pause und Resume ankern neu; Shutdown stoppt Ausgabe und Scheduler und speichert
beide Abbilder genau einmal. Pacing- und Persistenzstatus sind strukturiert und
ohne lokale Pfade oder Nutzdaten diagnostizierbar. Runtime-ABI 13,
Hostruntimevertrag 2 und Portprojektvertrag 5 versionieren die Erweiterung.

Vertrag und gesammelte Regressionen stehen in
[`MUTABLE_STORAGE_AND_PACING.md`](MUTABLE_STORAGE_AND_PACING.md). Die
Regressionen sind umgesetzt und fokussiert ausgefuehrt; ihre Wiederholung in
beiden frischen Gateprofilen bleibt Bestandteil von KR-4704.

## KR-4621 umgesetzt

Der Speicherbus verwendet einen abschaltbaren 64-KiB-Regionsindex, native
lineare 16-/32-Bit-Zugriffe und einen ereignisfreien Pfad ohne Trace oder
Watchpoint. Exakter virtueller Dispatch besitzt einen direkten Hashindex; die
Codeinvalidierung untersucht ueber einen Page-to-Block-Index nur beruehrte
Kandidaten. Geordnete beziehungsweise lineare Referenzmodi bleiben fuer
Differenzlaeufe erhalten.

Invalidierungs-, Dispatch- und Store-Queue-Diagnostik sind fest begrenzt und
melden verworfene Details ueber Aggregatzaehler. Automatische DMA-Transfers
koennen deterministisch bis vor das naechste fremde Schedulerereignis
gebuendelt werden; Einzeltransfer-, externe Request- und Round-Robin-Pfade
bleiben als Referenz erhalten. Vertrag und Gate-Messpunkte stehen in
[`P1_HOTPATHS.md`](P1_HOTPATHS.md).

## KR-4622 umgesetzt

Kontrollfluss-Fixpunkte uebernehmen bekannte Kontextschluessel und dekodieren
bei neuen Seeds nur die Deltafront. Die Funktionswertanalyse ordnet ihre
monotone Worklist nach Callgraph-SCCs und reiht Caller beziehungsweise Callees
nur bei geaenderter Summary oder geaendertem Ingress erneut ein. Immutable
Instruktionsarenen, Blockspans, gemeinsame Adressindizes und internierte
Evidence-IDs bilden die gemeinsame Analysegrundlage.

Jump-Table-Snapshots sind SHA-256-gebunden und begrenzt. Codegenpartitionen
tragen einen kanonischen IR-Hash und koennen als Delta ermittelt werden; der
Codegencache verwendet Schema 3 mit SHA-256-Schluesseln und atomarem,
konkurrenzsicherem Publish. Vertrag und Budgets stehen in
[`P1_INCREMENTAL_ANALYSIS.md`](P1_INCREMENTAL_ANALYSIS.md).

## KR-4623 umgesetzt

Trackdateien werden einmal read-only geoeffnet und danach ueber persistente
Handles gelesen. GDI besitzt Tracknummer- und LBA-Indizes, gebuendelte
Trackreads sowie einen abschaltbaren, auf 256 Sektoren begrenzten Cache.
ISO9660 cacht Verzeichnisse und Extents mit festen Obergrenzen; Cache-an und
Referenzmodus behalten identische Bytes und Pfadfehler.

Descriptor- und Track-SHA-256 werden beim Oeffnen erfasst und vom Portexport
wiederverwendet. Die portable GDI-Identitaet ist jetzt SHA-256-basiert.
GD-ROM-Completions bleiben ohne Hostuhr auf dem zentralen Gastscheduler. Der
Vertrag steht in [`P1_DISC_IO.md`](P1_DISC_IO.md).

## KR-4624 umgesetzt

Core und CLI sind der GUI-freie Standardbuild. MSVC, GCC und Clang besitzen
Debug-/RelWithDebInfo-Presets und werden in derselben sechsfachen CI-Matrix
regressionsgeprueft. Ein optionaler Compilerlauncher bindet ccache oder sccache
ein; alle Tests tragen stabile Subsystemlabels fuer getrennte Shards.

Projekt-, Paket- und ABI-Versionen entstehen aus kanonischen CMake-Werten. Das
installierbare `KatanaRecomp::runtime`-Paket bleibt von Analyzerquellen frei;
`KatanaRecomp::analyzer` ist ein getrennter Zusatz. Ein echter Out-of-Tree-
Consumer prueft diese Grenze. Vertrag und Ausgangsbaseline stehen in
[`P1_BUILD_GRAPH.md`](P1_BUILD_GRAPH.md).

## KR-4625 umgesetzt

Das Performance-/Buildgate erstellt Quality-Debug und RelWithDebInfo jeweils
frisch mit fester Linkparallelitaet. Quality-Debug bestand 168 von 168 Tests
mit MSVC-ASan und statischer Analyse; RelWithDebInfo bestand 167 von 167 Tests.
Beide Profile besitzen dasselbe Inventar aus 167 Core-Regressionen. Format-,
Qualitaetsvertrags- und Referenz-/Lizenzaudit sind erfolgreich, alle
instrumentierten Performancevertraege halten ihre Budgets und private
Retaildaten wurden nicht verwendet.

`Memory::write_bytes()` prueft gebuendelte Zielbereiche und vorhandene Bytes
vollstaendig vor dem Commit. Ein Fehler an einer Regions- oder
Schreibschutzgrenze veraendert kein Praefix; ein spaeter Geraetefehler meldet
jeden bereits geschriebenen Bereich noch vor dem Weiterwerfen zur
Codeinvalidierung. Write-only-MMIO wird dabei nicht vorgelesen und gilt
pessimistisch als geaendert. Das erzeugte Ninja-Projekt konfiguriert Runtime-
Includes, Buildvertrag und Hosttoolchain wirklich und wird in einer frischen
Regression gebaut. Das Gate wiederholt ausschliesslich erkannte Windows-
Linkerausgabesperren und protokolliert Versuch, Exitcode und Grund. Der lokale
JSON-Bericht wird durch einen identischen
Windows-CI-Buildgate als GitHub-Actions-Artifact unabhaengig nachvollziehbar.
Damit ist Stufe B abgeschlossen und KR-4715 beginnt.

## KR-4715 umgesetzt

Indirekte Stellen werden als `resolved`, `guarded_complete`,
`guarded_partial`, `runtime_only` oder `unresolved` disjunkt gezaehlt. Jede
offene Stelle traegt genau eine typisierte Callback-, Parameter-, Stack-,
Objekt/VTable-, Tabellen-, unbeschraenkte Speicher- oder Laufzeitzeigerklasse
und eine maschinenlesbare Evidenzherkunft. Partielle Kandidaten bleiben im
dynamischen Default und koennen Anwendungs-Vollstaendigkeit nicht herstellen.

Der lokale Bericht `katana-control-flow-v3` behaelt Adressen und Einzeldetails;
`katana-control-flow-frontier-v1` enthaelt ausschliesslich adressfreie
Aggregatzaehler. Anwendungsworkflow, Buildplan und Portmetadaten geben die
Klassen getrennt aus. Der Vertrag steht in
[`CONTROL_FLOW_FRONTIER.md`](CONTROL_FLOW_FRONTIER.md). KR-4716 ist der naechste
Task.

Die Review-Nacharbeit zaehlt validierte Hint-Kandidaten mit erhaltenem
dynamischem Default als `guarded_partial`, ohne ihren internen
`Unresolved`-Status oder die schwache `HintCandidate`-Evidenz aufzuwerten.

## KR-4716 umgesetzt

Die SH-C-Funktionswertanalyse vereinigt R8 bis R14 erst nach Beobachtung aller
bekannten direkten beziehungsweise vollstaendig bewachten Callstellen.
Unbekannte Caller und partielle indirekte Calleemengen bleiben unbekannter
Ingress. Endliche indirekte Calleemengen duerfen vollstaendige R0-Summaries
vereinigen; Guard und Vollstaendigkeit bleiben dabei getrennte Fakten.

R13-Callbacks behalten ihre eigene Herkunft. Feste Longword-Spills und
Reloads ueber symbolische SP-/Framepointeroffsets werden innerhalb eines
4096-Byte-Fensters erhalten. Unbekannte oder entkommene Aliase invalidieren
Stackfakten konservativ. Ein hartes Fixpunktbudget verhindert unbestimmte
Analysezeiten und stuft bei Erschoepfung alle betroffenen Beweise herab. Der
Vertrag steht in [`ABI_VALUE_ANALYSIS.md`](ABI_VALUE_ANALYSIS.md). KR-4717 ist
der naechste Task.

## KR-4717 umgesetzt

Eine begrenzte abstrakte Objektfeldtabelle verfolgt dominante Longword-
Initialisierungsstores und Loads ueber Register, feste Stackspills sowie
konstante Feld- und VTable-Slotoffsets. Vollstaendige Memory-Return-Summaries
erhalten Konstruktorwirkungen ueber bekannte Calls. Mehrere vollstaendige
Typwerte bleiben eine endliche bewachte Menge. Framepointer- und Objektpfade
besitzen getrennte Herkunftsklassen.

Unbekannte, partielle oder ueberlappende Stores sowie Calls und nicht
modellierte Speicherseiteneffekte invalidieren Objektfakten konservativ.
Beschreibbare statische VTables werden niemals allein aus dem Imageinhalt
eingefroren. Der Vertrag steht in
[`OBJECT_POINTS_TO.md`](OBJECT_POINTS_TO.md). KR-4718 ist der naechste Task.

## KR-4718 umgesetzt

Die Berichtstaxonomie wird bis in die IR- und Codeausgabe getragen. Nur als
echte Laufzeitquelle klassifizierte Stellen erhalten `runtime_only`; Hints,
Overrides und unbekannte Quellen koennen diesen Pfad nicht auswaehlen.
`guarded_complete` und `guarded_partial` behalten ihren bewachten Default,
waehrend `unresolved` im generierten Port sichtbar und ohne Tabellendispatch
abbricht.

Der Runtime-only-Dispatcher akzeptiert ausschliesslich ausgerichtete exakte
Instruktionsanfaenge aktiver, generationsgueltiger Bloecke mit Backendfunktion.
Virtuelle und kanonische physische Aliase werden gegen denselben registrierten
Executable-Image-/Blocktabellenvertrag geprueft. Misses stoppen den Lauf; es
gibt weder No-op noch geratenes Ziel. Begrenzte Zaehler erfassen Hits, Misses,
kontrollierte Fallbacks und den ersten Fehler maschinenlesbar. Runtime-ABI 12,
Backend-Interface-ABI 2 und Portprojektvertrag 4 versionieren die Erweiterung.
Der Vertrag und die bei KR-4704 auszufuehrenden Regressionen stehen in
[`RUNTIME_ONLY_DISPATCH.md`](RUNTIME_ONLY_DISPATCH.md). KR-4719 ist der
naechste Task.

## KR-4719 umgesetzt

Der private Harness verlangt Configversion 2 und ausschliesslich
`execution_mode=build-only`. Sein einziger Prozessstarter unterscheidet Tool-
und Runtimeprozesse und verwirft jede Runtimefunktion vor `Process.Start`.
Frische zufaellig benannte Jobziele koennen eine vorhandene stale `game.exe`
nicht als Erfolg wiederverwenden.

Zwei offizielle Buildjobs muessen vollstaendige Kontrollflussabdeckung, die
Analyse-/Codegen-/Hostbuild-Checkpoints und genau ein hashverifiziertes
aktuelles Executable liefern. Manifest-GDI, Jobresultat, Resultindex und
Portmetadaten werden intern an dieselbe Projektidentitaet gebunden; portable
Metadaten und generierte Quellen muessen zwischen beiden Jobs bytegleich sein.
Der atomare Bericht gibt nur Aggregate, Boolvertraege und allgemeine
Fehlerklassen aus. Er kann hoechstens `KR_RETAIL_ANALYSIS_CONTINUES` melden und setzt
`game_executable_started=false` sowie `runtime_processes_started=0`.

Der ausfuehrbare Vertrag steht in
[`PRIVATE_RETAIL_DEBUG.md`](PRIVATE_RETAIL_DEBUG.md). Der echte private
Doppelnachweis wird gemaess Handoff erst mit der frischen Gate-CLI in KR-4704
ausgefuehrt. KR-4703 ist der naechste Implementierungstask.

## Historischer Reviewbefund vor KR-4611 bis KR-4618

Der folgende Befund dokumentiert ausschliesslich den damaligen Ausgangspunkt.
Die Kontrollflussrisiken wurden mit KR-4611 bis KR-4614, die Harness- und
Gatepunkte mit KR-4616 bis KR-4618 abgearbeitet. Der aktuelle Stand steht in
den jeweiligen Umsetzungsabschnitten weiter unten.

Die damalige Kontrollflusspruefung hatte folgende P0-Soundnessrisiken gefunden:

- Hint-Direktiven werden als `Resolved` und damit als Beweis behandelt
- Delay-Slot-Kontext wird global nur nach Adresse gespeichert
- Basic Blocks koennen Fallthrough ueber Adressluecken erzeugen
- Site-Vollstaendigkeit wird auf ein Kantenbit reduziert
- unbekannte Caller und abweichende Callkontexte koennen Zielmengen verkleinern
- Kontextaufloesungen werden nur nach Instruktionsadresse dedupliziert

Der damalige private Harness startete nach erfolgreichem Build automatisch
`game.exe`.
Das widerspricht dem aktuellen v0.47-Build-only-Vertrag. Metriken werden noch
aus Textregexen gelesen, stdout und stderr verlieren ihre Reihenfolge und
Hostsmokes sind nicht ausreichend von Gastfortschritt getrennt.

Die Controllergrundlage existiert in Maple und Hostruntime. Der generierte Port
pollt Eingaben jedoch erst nach dem synchronen Gastlauf. Controllerunterstuetzung
braucht deshalb vor allem eine echte verschraenkte Runtime-Schleife, nicht noch
eine weitere Taste in einem Enum.

Die GUI besitzt intern mehrere Seiten und strukturierte Jobereignisse, zeigt
unter Windows aber fast nur eine Fliesstextzusammenfassung, zwei Balken und ein
Logfeld. Layout und Aktualisierung sind hart kodiert beziehungsweise
pollingbasiert.

Der vollstaendige Befund steht in
[`CONTROL_FLOW_HARNESS_GUI_REVIEW_2026-07-18.md`](CONTROL_FLOW_HARNESS_GUI_REVIEW_2026-07-18.md).

## Task-ID-Korrektur

Die historischen Bedeutungen von `KR-4801` bis `KR-4805` und `KR-4901` werden
wiederhergestellt. Die zwischenzeitlich mit diesen IDs bezeichneten
Alpha-Aufgaben erhalten `KR-4911` bis `KR-4916`.

Die kanonische Historie steht in
[`TASK_ID_REGISTRY.md`](TASK_ID_REGISTRY.md).

## KR-4611 umgesetzt

Der SH-4-Kontrollzustand verwendet fuer R0 bis R7 jetzt die effektive
Bankauswahl `SR.MD && SR.RB`. BSR, BSRF und JSR schreiben PR vor ihrem Delay
Slot; ein PR-Schreibzugriff des Slots bleibt dadurch sichtbar. RTE und SLEEP
besitzen getrennte Blockendtypen in Block-ABI 2. Der Portdispatcher setzt nach
normalen Gast-Exceptions und Interrupts am jeweiligen Handlervektor fort,
behandelt RTE als dynamische Fortsetzung bei SPC und fuehrt waehrend SLEEP
keinen weiteren Block vor der Annahme eines Interrupts aus.

Exceptionursache, Gast-Eventcode und Vektor werden aus einer gemeinsamen
Metadatentabelle abgeleitet. Die spaeter bei KR-4617 und KR-4618 umzusetzenden
und auszufuehrenden Testanforderungen stehen in
[`SH4_CONTROL_STATE.md`](SH4_CONTROL_STATE.md).

## KR-4612 umgesetzt

SQ0 und SQ1 werden jetzt durch Adressbit 5 gewaehlt. Das Schreibfenster
`0xE0000000` bis `0xE3FFFFFF`, das getrennte Longword-Lesefenster ab
`0xFF001000`, QACR0/QACR1 und PREF verwenden denselben Queuevertrag. P4- und
QACR-Zugriffe pruefen Fenster, Breite, Ausrichtung und Queuegrenzen zentral.

Operand-Cache-RAM unterstuetzt explizite Byte-, Word- und
Longword-Little-Endian-Zugriffe mit Ausrichtungs- und Bereichspruefung. ICBI
invalidiert die ausgerichtete 32-Byte-Codezeile. OCBI bleibt ohne
Cachetagmodell sichtbar nicht unterstuetzt. OCBP und OCBWB durchlaufen einen
expliziten kohaerenten Runtimevertrag, der im aktuellen direkten Speichermodell
korrekt keine verborgene Dirty-Line und keinen Write-back meldet. Vertrag und
gesammelte Gate-Testanforderungen stehen in
[`STORE_QUEUE_CACHE.md`](STORE_QUEUE_CACHE.md).

## KR-4613 umgesetzt

`Memory` ist jetzt die gemeinsame beobachtbare Commitgrenze fuer CPU-, FPU-,
DMA-, Store-Queue-, Copy- und Fallbackwrites. Lineares RAM wird vor der
Invalidierung auf Byteidentitaet geprueft; gebuendelte Writes melden einen
gemeinsamen Bereich. Generierte Stores koennen den Tracker dadurch nicht mehr
umgehen.

Geaenderte physische Bereiche invalidieren Trackerbloecke, Aliase und
eingehende Links und entfernen ueberlappende Eintraege der zentralen
Runtime-Blocktabelle. Zusaetzlich verweigern trackergebundene Tabellen stale
virtuelle, physische und Alias-Lookups, sodass Direktdispatch und Inline-Cache
kein invalidiertes Ziel ausfuehren koennen. Vertrag und die bei KR-4617/KR-4618
nachzuholenden Tests stehen in [`GUEST_WRITES.md`](GUEST_WRITES.md).

## KR-4614 umgesetzt

Kontrollfluss-Sites, CFG-Kanten, Funktionskandidaten und Jump Tables verwenden
die sieben typisierten Evidenzklassen von `ProvenComplete` bis `Unresolved`.
Hints bleiben unverbindliche Kandidaten und koennen die ungeloeste Front oder
den Export nicht verkleinern. Forced Overrides behalten den Runtime-Default.

Die rekursive Worklist unterscheidet Adresse, eingehenden Kontext,
Delay-Slot-Owner und Evidenz. Basic Blocks erzeugen Fallthrough nur an der
exakten Folgeadresse und paaren Owner/Slot nur gegenseitig bei `PC + 2`.
Unbekannte Caller tainten Kandidateneingaenge; Zielmengen aller Callkontexte
und vollstaendige Summaries endlicher indirekter Callees werden konservativ
vereinigt. Dynamische Herkunft verwendet einen begrenzten CFG-Backward-Slice
statt eines linearen Acht-Instruktions-Fensters.

Der genaue Vertrag und die bei KR-4617/KR-4618 nachzuholenden Regressionen
stehen in [`CONTROL_FLOW_SOUNDNESS.md`](CONTROL_FLOW_SOUNDNESS.md).

## KR-4615 umgesetzt

Die Runtime-Blocktabelle gibt keine Adressen verschiebbarer Vektorelemente
mehr heraus. Dispatch, Inline-Cache und generierter Port verwenden stabile
`RuntimeBlockHandle` aus Record-ID und Generation und loesen sie unmittelbar
vor dem Zugriff erneut auf. Erase und physische Invalidierung markieren
Records stale; eine dynamische Reaktivierung derselben Identitaet behaelt die
ID und liefert eine neue Generation.

Statische Bloecke werden vom Port sortiert in einem Bulk registriert und
danach versiegelt. Statische und dynamische virtuelle, physische und
Aliasindizes bleiben getrennt; aktive virtuelle Bereiche und physische Seiten
bilden die Mutationsindizes. Exakte Lookups sind logarithmisch und physische
Invalidierungen untersuchen nur beruehrte Seiten. Runtime-ABI 10 versioniert
die Handle- und Bulk-Registry-Schnittstelle.

Der genaue Vertrag und die bei KR-4617/KR-4618 nachzuholenden Last-, Alias-
und Mutationsregressionen stehen in
[`RUNTIME_BLOCK_REGISTRY.md`](RUNTIME_BLOCK_REGISTRY.md).

## KR-4616 umgesetzt

Gastzeitvertrag 1 verwendet den `EventScheduler` als einzige monotone
64-Bit-Zyklusuhr. Generierte Bloecke verbrauchen relative Instruktionskosten
ueber PlatformServices-ABI 5; Fallback-Safepoints, Blockaustritte und
Runtimemetriken lesen denselben Schedulerstand. Runtime-ABI 11 versioniert
diese Schnittstelle.

TMU, RTC und DMA verwenden ihre Hardwarefristen weiterhin auf diesem
Scheduler. GD-ROM besitzt keine eigene fortzuschaltende Uhr mehr: `submit`
plant seine Completion direkt. PVR-Renderstarts erhalten ebenfalls eine
Schedulerfrist, bevor der System-ASIC-Abschluss sichtbar wird. Resets
reaktivieren laufende Timer-/DMA-Fristen deterministisch und entfernen stale
GD-ROM-/PVR-Completions.

`KATANA_GUEST_CYCLE_BUDGET` wird von `game.exe` als positive 64-Bit-Zahl
validiert und direkt im Scheduler durchgesetzt. SLEEP prueft zuerst bereits
annehmbare Interrupts, springt andernfalls zum naechsten Ereignis und kann
weder ohne Wakeupquelle noch ueber das Gastbudget hinaus still weiterlaufen.
Der genaue Vertrag und die bei KR-4617/KR-4618 nachzuholenden
Reihenfolge-, Budget- und Cross-Engine-Regressionen stehen in
[`GUEST_TIMING.md`](GUEST_TIMING.md).

## KR-4617 umgesetzt

Die synthetische Core-Suite enthaelt nun unabhaengige Referenzvektoren fuer
alle P0-Vertraege: die Vier-Zustaende-Registerbankmatrix, PR vor Call-Delay
Slots, RTE-/Exceptionmetadaten, SQ-Bit-5-Adressierung, alle Gastwritequellen,
Aliasinvalidierung, generationsgesicherte Blockhandles und den gemeinsamen
Gastzeitvertrag. Erfolgs-, Grenz- und sichtbare Fehlerfaelle sind getrennt.

CFG-Regressionen pruefen Hint- und Forced-Override-Evidenz, erhaltene
Runtime-Defaults, Adressluecken, partielle Sites, normale und Delay-Slot-
Kontexte, unbekannte Caller sowie exhaustive kleine Conditional-Graphen.
Fixpunktterminierung wird nur noch gegen ein oberes Budget geprueft; interne
Iterationszahlen sind kein Testvertrag mehr.

Die Tests passen zu Runtime-ABI 11, PlatformServices-ABI 5 und dem stabilen
Blockhandle-API. Gemaess Gate-Arbeitsmodell wurden bei KR-4617 weder
Konfiguration noch Build oder Tests gestartet. Die Debug-/RelWithDebInfo-
Ausfuehrung und der Konfigurationsvergleich folgen gesammelt in KR-4618.

## KR-4618 umgesetzt

Das Core-Korrektheitsgate erstellt `build-current` fuer Quality-Debug und
RelWithDebInfo jeweils frisch. Quality-Debug bestand 171 von 171 Tests mit
MSVC-ASan und statischer Analyse; RelWithDebInfo bestand 170 von 170 Tests.
Beide Profile besitzen dasselbe Inventar aus 170 Core-Regressionen. Der
zusaetzliche Debug-Test prueft gezielt die ausgelieferte MSVC-ASan-Runtime.

Format-, Qualitaetsvertrags- und Referenz-/Lizenzaudit sind erfolgreich. Die
exakten Referenzvektoren bestanden in beiden Konfigurationen; private
Retaildaten wurden nicht verwendet. Der maschinenlesbare Gatebericht wurde
unter `build-current/reports/core-correctness-gate.json` erzeugt. Damit ist die
P0-Core-Korrektheitsstufe abgeschlossen und KR-4621 beginnt die Performance-
und Buildstufe.

## Naechste Reihenfolge

```text
v0.47:
KR-4611 bis KR-4618
-> KR-4621 bis KR-4624 -> KR-4625
-> KR-4715 -> KR-4716 und KR-4717 -> KR-4718
-> KR-4719 -> KR-4703 -> KR-4704 -> KR-4705

v0.48:
Runtime-SDK, gemeinsamer Export, Harness, Controller und GUI
-> KR-4804 -> KR-4805

v0.49:
KR-4911 -> KR-4912 -> KR-4913
-> KR-4914 und KR-4915 -> KR-4916
-> KR-4901 bis KR-4903 -> KR-4904 -> KR-4905

v0.50:
KR-4999 -> KR-5000
```

## Private Laufzeitgrenze

Sonic Adventure PAL darf fuer den v0.48-Bring-up lokal gebaut und gestartet
werden. Deterministische Diagnoseproben und interaktive Sitzungen bleiben von
Release-Gateevidenz getrennt; private Retaildaten, BIOSdaten und lokale
Installationspacks duerfen weder committed noch verteilt werden.

## Bestehende Funktionssicherung

- keine bestehende korrekte Regression wird entfernt oder abgeschwaecht
- falsche historische Erwartungen werden durch unabhaengige Vektoren ersetzt
- jeder Fastpath behaelt einen deaktivierbaren Referenzpfad
- Debug und RelWithDebInfo muessen dieselben Gastresultate liefern
- Performance darf keine Beweis-, Guard- oder Runtime-Default-Semantik aendern
- Retailbefunde werden nur als allgemeine, verteilbare Regression umgesetzt
