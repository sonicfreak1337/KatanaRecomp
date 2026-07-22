# Indirekter Kontrollfluss

## Lokale Konstantenpropagation

`propagate_local_constants` fuehrt einen konservativen Registerzustand durch
eine bereits geordnete Instruktionsfolge. Der Transfer unterstuetzt
Immediate-Moves, Immediate-Additionen, Registerkopien, NOP sowie
`MOV.W/MOV.L @(disp,PC)` und `MOVA`. PC-relative Literale werden nur aus
committed Image-Bytes gelesen; `MOV.W` wird SH-4-konform vorzeichenerweitert
und `MOV.L` verwendet die vier Byte ausgerichtete PC-Basis. 32-Bit-Arithmetik
verwendet definiertes unsigned Wraparound.

Nicht eigens ausgewertete Instruktionen invalidieren nur die allgemeinen
Register, die ihre SH-4-Semantik tatsaechlich schreibt. Speicher-, Status-,
Akkumulator- und FPU-Effekte koennen damit ein unabhaengiges konstantes Ziel
nicht mehr zerstoeren. Pre-/Post-Adressupdates invalidieren die betroffenen
Adressregister; unbekannte Opcodes bleiben ein Voll-Clobber.

Kontrollflussgrenzen werden nach ihrem Delay Slot angewendet. Ein bedingter
Fallthrough darf lokale Werte weitertragen, direkte Sprungziele markieren aber
einen Join und beginnen konservativ ohne Registerbeweis. Dadurch kann weder ein
Delay-Slot-Wert ueber einen Call noch ein einseitiger Wert ueber einen
mehrdeutigen Join durchsickern.

## SH-C-Aufrufvertrag

`ExecutableImage` traegt die explizite Gast-ABI `Unknown` oder `SuperHC`.
Dreamcast-GDI-Images verwenden `SuperHC`; allgemeine Raw-/ELF-Images bleiben
ohne gesonderten Vertrag `Unknown`. Im SH-C-Modus werden R0 bis R7 an Calls
invalidiert und R8 bis R14 gemaess Renesas-Aufrufvertrag erhalten. Ein daraus
abgeleiteter Zielbeweis traegt die Herkunft `sh-c-abi-preserved-*`.

Die ABI-Regel ist damit eine titelunabhaengige Eingabeeigenschaft, kein
Sonic-spezifischer Patch. Grundlage ist die Renesas-Dokumentation der
garantierten Register R8 bis R14:
<https://www.renesas.com/en/document/apn/sh-compiler-application-note-2-compiler-use-guide-pragma-extension-guide>.

## Begrenzte Speicherwerte

Direkte, Displacement- und R0-indexierte Integerloads duerfen einen Wert nur
dann statisch uebernehmen, wenn die effektive Adresse vollstaendig konstant,
der gesamte Zugriff committed und das Segment lesbar sowie nicht beschreibbar
ist. Byte- und Wortloads werden vorzeichenerweitert. Beschreibbare Daten,
Zero-Fill, dynamische Basen und unbeschraenkte Indizes invalidieren nur das
Zielregister und erzeugen keinen geratenen Kontrollfluss.

## Relative SH-4-Sprungtabellen

Eine relative Tabelle wird automatisch nur fuer ein zusammenhaengendes,
strukturell eindeutiges Muster akzeptiert:

1. Ein unmittelbar geladenes positives Limit begrenzt den unsigned Index mit
   `CMP/HS`.
2. `BT` verlaesst den Tabellenpfad oder `BF` springt ueber einen direkten
   `BRA`-Fallback genau in den Tabellenpfad.
3. `SHLL`, Registerkopie und `MOVA` bilden den Zwei-Byte-Index und die
   PC-relative Tabellenbasis.
4. `MOV.W @(R0,Rm),Rn` liest einen signed 16-Bit-Offset; `BRAF Rn` verwendet
   `dispatch + 4` als Zielbasis.

Die vollstaendige Tabellenbreite muss in einem einzelnen committed, lesbaren
und nicht beschreibbaren Snapshot liegen. Alle Ziele muessen gerade und
ausfuehrbar sein; ein einziger ungueltiger Eintrag verwirft die gesamte
Zielmenge. Beschreibbare Pointer, Tabellen, VTables und Executable-RAM-Bereiche
bleiben dynamisch. Automatische Tabellen werden als aufgeloeste
Mehrfachstelle markiert, in den rekursiven Fixpunkt und `resolved_edges`
uebernommen und im JSON-Bericht mit Kodierung, Zielbasis und jedem Einzelbeweis
ausgegeben.

## Registerwertanalyse

Die Registerwertanalyse erweitert den lokalen Transfer um registerweise Add-,
Sub- und logische Operationen. Fuer jede indirekte `JMP`-/`JSR`-Stelle zeichnet
sie den verwendeten Registerindex und den davor beweisbaren Wert auf. Ein
fehlender Wert bleibt explizit unbekannt und wird nicht geraten.
Arithmetisch kombinierte Konstanten tragen eine verschachtelte Herkunft aus
Operation, Ziel- und Quellbeweis; dadurch wird eine PC-relative Basis weder als
reine Immediate-Konstante ausgegeben noch in umgekehrter Operandenlage
faelschlich zur alleinigen Herkunft. `XOR Rn,Rn` und `SUB Rn,Rn` besitzen den
eingangsunabhaengigen Nullbeweis `xor-self` beziehungsweise `sub-self`.
Adressluecken setzen den lokalen Registerzustand zurueck, sodass Konstanten nicht
zwischen getrennt entdeckten Codebereichen weitergetragen werden.

## Einfache indirekte Calls und Spruenge

Eine indirekte Stelle gilt nur dann als `resolved`, wenn der beobachtete
Registerwert gerade ist und auf zwei committed Bytes eines ausfuehrbaren
Code-Segments zeigt. Die Gruende `constant-register`, `pc-relative-literal`
und `pc-relative-address` dokumentieren den Beweis und seine Herkunft.
Unbekannte Werte und ungueltige Zielbereiche bleiben mit getrennten stabilen
Gruenden `unresolved`.

## Entry-Snapshot und bewachte Kandidaten

Dreamcast-GDI-Images tragen den expliziten
`EntryPointStraightLineQuiescent`-Anfangssnapshotvertrag. Nur der
zusammenhaengende gerade Pfad ab dem ausgezeichneten `initial_snapshot_entry`
darf ein beschreibbares PC-relatives Literal zeitlich beweisen. Das ist bei
Images mit mehreren Analyse-Eintritten insbesondere nicht zwingend der
numerisch erste Entry. Fehlt die explizite Markierung, ist nur bei genau einem
Entry ein kompatibler Fallback erlaubt. Ein CFG-Join, eine Adressluecke,
Kontrollfluss, ein unbekanntes Schreibziel oder ein ueberdeckender bekannter
Store beendet diesen Beweis. Allgemeine Raw-/ELF-Images bleiben bei
`ImmutableOnly`. Der vollstaendige Asynchronitaets- und Berichtsvertrag steht
in [INITIAL_SNAPSHOT_CONTRACT.md](INITIAL_SNAPSHOT_CONTRACT.md).

Ausserhalb dieses engen Vertrags darf ein committed lesbarer Wert aus
beschreibbarem Speicher lediglich einen Status `guarded` erzeugen. Das Ziel
wird rekursiv entdeckt und als partielle Kandidatenkante in Basic Blocks, IR,
CFG und Callgraph uebernommen. Der Block behaelt gleichzeitig seinen
indirekten Nachfolger; generierter Dispatch prueft den aktuellen Registerwert
und behaelt fuer jedes andere Laufzeitziel den dynamischen Default. Guarded ist
damit kein statischer Beweis und wird im Portbericht getrennt ausgewiesen. Es
blockiert den Export nicht: Der erzeugte Basic-Block-Dispatcher prueft die
Kandidaten zur Laufzeit und behaelt den dynamischen Default. Nur `unresolved`
ohne endlichen Kandidaten bleibt ein Vollstaendigkeitsblocker.

### Absolute Pointertabellen im Anfangssnapshot

Ein getrenntes, konservatives Muster erkennt einen PC-relativ geladenen
Tabellenzeiger, den nachfolgenden `MOV.L @(R0,Rm),Rn`-Load und dessen `JMP` oder
`JSR`. Zwischen Load und Dispatch sind nur semantisch unschaedliche `NOP`- und
R0-Indexanpassungen erlaubt. Der Literal- und Tabellenbereich muss committed,
lesbar und Teil der initialen Ladephase sein; beschreibbare Segmente sind nur
unter `EntryPointStraightLineQuiescent` zulaessig. Runtime-Memory ist immer
ausgeschlossen. Ein zusammenhaengender Lauf braucht mindestens zwei und
hoechstens 4096 ausgerichtete, committed ausfuehrbare Ziele. P0/P1/P2-Aliase
werden dabei auf dieselbe Imageherkunft normalisiert.

Diese Anfangswerte sind ausdruecklich kein Beweis fuer die spaetere
Tabellenbelegung. Die Site bleibt `RuntimeOnly`, die Kandidatenherkunft
`GuardedPartial`, und die Ziele erscheinen nur als `analysis_candidates`.
Insbesondere entstehen daraus keine `resolved_edges`, keine bewiesenen CFG-
Kanten und keine Funktionsseeds. Das IR verwendet akzeptierte Ziele lediglich
als Basic-Block-Leader innerhalb der bereits bewiesenen Funktion. Damit kann
der validierende Runtime-Dispatcher einen vorab kompilierten Zielblock betreten,
ohne am Blockende einen kuenstlichen Funktionsreturn zu erzeugen. Der aktuelle
Gastload bleibt massgeblich; jedes andere Ziel durchlaeuft weiterhin den
typisierten dynamischen Default.

### Relative BSRF-Handlerinseln im Anfangssnapshot

Ein ungeloester `BSRF` darf endliche AOT-Kandidaten liefern, wenn sein relatives
Zielregister innerhalb eines zusammenhaengenden begrenzten Rueckwaertsfensters
nachweislich aus einem Speicherwort stammt und danach nur durch geschlossene
relative Registertransformationen veraendert wird. Dispatch und Insel muessen
im selben initialen, ausfuehrbaren Snapshotsegment liegen; `RuntimeMemory` ist
ausgeschlossen. Beschreibbare Segmente setzen wie andere Snapshotkandidaten
`EntryPointStraightLineQuiescent` voraus.

Die Struktur muss eindeutig sein: Die Erkennung waehlt genau einen maximalen
Lauf aus mindestens vier Handlern mit demselben geraden Stride. Jeder Handler
enthaelt an der vorletzten Instruktionsposition ein `RTS`, an der letzten dessen
gueltigen Delay-Slot und davor keinen anderen Kontrolltransfer. Auf den letzten
Return-Handler folgt im selben Stride ein
obligatorischer terminaler Slot, der mit `BRA` beginnt, sonst nur `NOP`
enthaelt und auf gueltigen committed Code verzweigt. Gleich lange mehrdeutige,
nicht terminierte, nicht ausfuehrbare oder ueberlange Laeufe werden verworfen.

Auch diese Insel ist kein statischer Zielbeweis. Handler und terminaler Slot
erscheinen nur als `analysis_candidates`; die Site bleibt kandidaten-only
`RuntimeOnly`, und das live geladene relative BSRF-Ziel ist autoritativ. Es
entstehen keine `resolved_edges`, keine CFG-Kanten und keine Funktionsseeds.
Die Kandidaten werden lediglich bewacht analysiert und als Basic-Block-Leader
fuer einen spaeter validierten nativen Eintritt vorbereitet.

## Interprozedurale SH-C-Summaries

Nach dem lokalen Kontrollflussfixpunkt bildet die Analyse Basic Blocks und
Funktionen aus denselben bewiesenen Kanten. Fuer jede Funktion wird R0 an allen
erreichbaren `RTS`-Stellen zusammengefuehrt. Eine Summary ist nur vollstaendig,
wenn jeder Returnpfad einen bekannten Wert liefert und die vereinigte Menge
hoechstens acht Ziele besitzt. R8 bis R14 werden im `SuperHC`-Profil als
`abi-preserved-input` ausgewiesen; ohne explizite Gast-ABI entstehen keine
SH-C-Summaries.

Direkte Calls wenden den Callee-Return erst nach einem vorhandenen Delay Slot
an. Aendert sich eine Callee-Summary, werden nur ihre Caller erneut bewertet.
Rekursion konvergiert damit konservativ, ohne einen unbekannten Anfangswert als
Konstante auszugeben. Vollstaendige R0-Mengen duerfen nach derselben committed-
und executable-Pruefung wie lokale Ziele eine oder mehrere CFG-Kanten erzeugen.
Der Bericht nennt Register, Callsite, Callee, Returnstellen und den
Zusammenfuehrungsgrund.

Nicht bewiesene Stellen werden weiterhin sichtbar unterschieden:
`dynamic-return-value`, `dynamic-parameter`, `dynamic-stack-target`,
`dynamic-vtable-target` und `dynamic-unbounded-memory`. Diese Klassen sind
Diagnosen und keine Zielannahmen; veraenderliche VTables, Stackwerte und
unbeschraenkte Speicherloads werden nicht eingefroren.

Die funktionsweite Kandidatenanalyse traegt endliche Werte zusaetzlich durch
CFG-Joins und SH-C-erhaltene Register. Direkte sowie bereits bewachte indirekte
Calls liefern partielle Eingabekandidaten an ihre Callees; diese Herkunft bleibt
bewacht, weil unbekannte weitere Caller nicht ausgeschlossen sind. Direkte,
Displacement-, R0-indexierte und PC-relative Loads, Add/Sub, logische
Operationen, feste Shifts, Extensions und `MOVT`-Mengen bis acht Werte werden
ausgewertet. Ein unbekannter Pfad oder eine groessere Menge verwirft den
Kandidaten konservativ.

## Jump Tables

`analyze_jump_table` wertet eine bekannte, vier Byte ausgerichtete Tabelle mit
einer expliziten endlichen Eintragszahl aus. Die gesamte Absolute32-Tabelle
muss in einem einzelnen committed, lesbaren und nicht beschreibbaren Snapshot
liegen. Ziele werden nur akzeptiert, wenn sie gerade sind und auf committed
ausfuehrbaren Code zeigen.
Die Bereichsschranke liegt bei 4096 Eintraegen. Fehlende Eintraege, ungueltige
Ziele und unbeschraenkte Kandidaten bleiben als abgelehnt sichtbar; eine nur
teilweise gueltige Tabelle gilt nicht als aufgeloest.

Die Tabellenbasis muss vier Byte ausgerichtet sein. Die exklusive Endadresse wird
mit 64-Bit-Zwischenwerten berechnet, weshalb ein einzelner Eintrag bei
`0xFFFFFFFC` arithmetisch gueltig bleibt. Teilweise committed oder
beschreibbare Tabellen werden vor dem ersten Eintrag vollstaendig abgelehnt.
Die Dispatch-Adresse muss als `JMP @Rn` oder `JSR @Rn` entdeckt worden
sein; Call-Tabellen erzeugen Funktionskandidaten, Jump-Tabellen nicht.
Dieser vollstaendig bewiesene `Absolute32`-Vertrag ist von den oben
beschriebenen beschreibbaren Snapshotkandidaten getrennt. Nur die
nicht-beschreibbare, vollstaendige Tabelle erzeugt statische Kanten und bei
Calls Funktionskandidaten.

## Override-Datei Version 1

Die Textdatei beginnt mit `version = 1`. Wiederholbare Eintraege besitzen die
Formen `function = ADRESSE`, `jump = STELLE ZIEL` und
`jump_table = STELLE TABELLE ANZAHL`. Adressen sind hexadezimal, die Anzahl ist
dezimal. Der Parser sortiert alle Hinweise numerisch und weist doppelte Stellen,
unbekannte Felder sowie unbekannte Versionen zurueck. Overrides sind explizite
Nutzerhinweise und werden in Berichten als solche gekennzeichnet.
Dieselbe Dispatch-Adresse darf nicht zugleich als `jump` und `jump_table`
angegeben werden; eine Kollision wird mit Datei, beiden Zeilen und Adresse
abgelehnt, bevor sie die Analyse beeinflussen kann.

Function- und Jump-Ziele verwenden dieselbe committed-Code-Pruefung wie die
automatische Analyse. Fehler nennen Override-Datei, Zeile, Adresse und einen
stabilen Grund. Ein Override fuer eine noch nicht entdeckte Dispatch-Stelle wird
waehrend laufender Fixpunktiterationen zurueckgestellt und erst am Fixpunkt als
ungueltig gemeldet. Dadurch bleibt das Ergebnis unabhaengig von der Dateireihenfolge.

## Bericht

Der Bericht trennt `Aufgeloest` und `Ungeloest` in stabiler Reihenfolge. Sichere
Ziele nennen ihren Beweisgrund. Jede ungeloeste Stelle nennt ihren Ablehnungsgrund
und zeigt die passende `jump`- oder `jump_table`-Zeile fuer eine Override-Datei.

## Analyseanweisungen Version 2

Version 2 trennt zwingende Overrides von unverbindlichen Hints:

```text
schema = katana-analysis-directives
version = 2
mode = hint
function = 0x8C010000
jump = 0x8C010100 0x8C010200
```

`mode = override` behaelt die erzwingende v1-Semantik. `mode = hint` darf einen
bereits statisch bewiesenen, abweichenden Wert nicht ersetzen. Uebereinstimmende
Hints werden als `confirmed`, nutzbare Hinweise als `accepted`, Konflikte als
`rejected` und nicht mehr auffindbare Stellen als `stale` berichtet. Ungueltige
Hint-Adressen brechen die Analyse nicht ab, bleiben aber sichtbar diagnostiziert.

Die CLI-Option `--directives <Datei>` akzeptiert beide Modi; `--overrides`
bleibt als kompatibler Alias bestehen. Der Modus stammt immer aus der
versionierten Datei und wird nicht aus dem Optionsnamen geraten.
Eine teilweise gueltige Jump Table bleibt vollstaendig im ungeloesten Abschnitt.
Tabellenzeilen nennen fuer jedes Ziel, ob der Dispatch ein Jump oder Call ist.
Eine durch eine Tabelle erklaerte Dispatch-Stelle wird nicht zusaetzlich als
gewoehnlicher ungeloester Registersprung ausgegeben.
