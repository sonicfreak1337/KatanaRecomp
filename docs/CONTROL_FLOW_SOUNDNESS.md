# Kontexttreue Kontrollfluss- und Wertanalyse

KR-4614 ersetzt implizite Bool-Vertraege durch typisierte Evidenz und macht
Kontext, Vollstaendigkeit und Runtime-Defaults zu expliziten Bestandteilen der
Analyse.

## Evidenzklassen

Jede indirekte Kontrollfluss-Site, ihre CFG-Kanten, Funktionskandidaten und
Jump Tables tragen eine `ControlFlowEvidence`:

- `ProvenComplete`: statisch bewiesene, vollstaendige Zielmenge
- `GuardedComplete`: vollstaendige Zielmenge unter einem expliziten Runtimeguard
- `GuardedPartial`: nuetzliche Kandidaten, aber nicht vollstaendig
- `ForcedOverride`: erzwungene Direktive mit erhaltenem Runtime-Default
- `HintCandidate`: unverbindlicher Hint ohne Beweiswirkung
- `RuntimeOnly`: absichtlich dynamischer Stack-, Parameter-, Return- oder
  VTable-Pfad
- `Unresolved`: weder Beweis noch belastbare dynamische Klassifikation

Nur `ProvenComplete` und `GuardedComplete` duerfen den dynamischen Nachfolger
einer Site entfernen. Die Vollstaendigkeit wird einmal pro Site ueber alle
Ziele und Kontexte vereinigt; eine einzelne unguarded Kante kann sie nicht
mehr vortaeuschen.

Die Klassifikation ist von der typisierten Herkunft getrennt. Deterministisch
sortierte `AnalysisEvidenceOrigin`-Werte unterscheiden lokale Werte,
Entry-Snapshots, Funktionssummaries, Jump Tables, Overrides, Hints und reine
Runtimeklassifikation; menschenlesbare `reason`-Texte sind nur Diagnose und
keine Beweislogik mehr.

Hints bleiben im bisherigen Statusmodell `Unresolved`, koennen aber als
Kandidaten rekursiv decodiert und berichtet werden. Sie reduzieren weder die
ungeloeste Front noch erzeugen sie harte Funktionsgrenzen oder exportierbare
Beweise. Overrides werden als `ForcedOverride` berichtet und behalten den
dynamischen Dispatch fuer eine Runtimevalidierung. Statisch staerkere Evidenz
verdraengt Hint-Evidenz deterministisch.

Der JSON-Kontrollflussbericht verwendet deshalb Schema
`katana-control-flow-v2` und fuehrt Evidenz fuer Funktionen, indirekte Sites
und Jump Tables explizit auf.

## Instruktions- und Delay-Slot-Kontext

Die rekursive Worklist ist nicht mehr nur durch die Adresse bestimmt. Ihr
Schluessel enthaelt:

- Instruktionsadresse,
- eingehende Kontrollflussadresse,
- optionalen Delay-Slot-Owner,
- Evidenzklasse.

`RecursiveAnalysisResult::contextual_instructions` erhaelt diese Varianten
getrennt. Dieselbe Adresse kann damit als normale Instruktion und als Slot
verschiedener Owner untersucht werden, ohne dass ein globales Delay-Slot-Bit
einen Kontext unterdrueckt. Kontrollfluss im Slot wird als
`control-flow-in-delay-slot` diagnostiziert. Fehlende und unbekannte Slots
erzeugen sichtbare Ownerdiagnosen.

Owner und Slot werden in der Block-, Funktions- und Graphbildung nur gepaart,
wenn der Slot exakt bei `Owner + 2` liegt, als Slot markiert ist und der Owner
tatsaechlich einen Delay Slot besitzt. Ein fremder naechster Eintrag wird nie
in den Block gezogen.

## CFG und Funktionsgrenzen

Ein Fallthrough wird nur erzeugt, wenn ein Block exakt an der
architekturgemaessen Folgeadresse beginnt:

- normale Instruktion: `PC + 2`
- Kontrollinstruktion mit Delay Slot: `PC + 4`

Eine Adressluecke und der naechste sortierte Block sind kein Kontrollfluss.
Guarded-, Hint- und Override-Callziele bleiben Kandidaten und werden nicht zu
harten Funktionsgrenzen. `FunctionInfo` erhaelt getrennte Shared-Block- und
Tail-Jump-Mengen; nur bewiesene Funktionskandidaten werden in IR und Callgraph
als Entries verwendet.

`validate_decode_candidate` prueft, ob Bytes legal decodiert werden duerfen.
`proven_instruction_boundary` prueft getrennt, ob die Adresse durch einen
bewiesenen Kontext als Instruktionsanfang bestaetigt wurde. Eine ansonsten
gueltige Codeadresse wird ohne diesen zweiten Nachweis zu `GuardedPartial`
herabgestuft.

## Interprozedurale Werte

Kandidateneingaenge besitzen fuer jedes Register einen eigenen
Vollstaendigkeitszustand. Ein unbekannter relevanter Caller taintet den Eingang
und kann durch einen anderen bekannten Caller nicht wieder in eine endliche
vollstaendige Menge verwandelt werden. Platzhalterauswertungen noch nicht
beobachteter Callees zaehlen dabei nicht als echte unbekannte Caller.

Alle Auswertungen derselben indirekten Site werden konservativ vereinigt:

- Vereinigungsmenge aller Ziele,
- `complete` nur wenn jeder Kontext vollstaendig ist,
- `guarded` sobald ein Kontext dynamisch oder unvollstaendig ist,
- Vereinigungsmenge aller Callsite- und Callee-Evidenz.

Bei einem endlichen indirekten Call wird R0 nur dann aus Callee-Summaries
vereinigt, wenn jeder Kandidat eine vollstaendige, nichtleere Summary besitzt
und das gemeinsame Wertbudget nicht ueberschritten wird. Eine fehlende Summary
laesst R0 unbekannt.

Die dynamischen Herkunftsklassen verwenden einen auf 64 Instruktionen
begrenzten CFG-Backward-Slice. Er folgt Vorgaengern ueber Blockgrenzen, vereinigt
Joins konservativ und akzeptiert einen Writer nur, wenn jeder relevante Pfad
denselben belastbaren Def erreicht. Stack-, Parameter-, Return-, Speicher- und
VTable-Klassen beruhen damit nicht mehr auf einem linearen Acht-Befehle-Fenster.

## Spaetere Testanforderungen

KR-4617 muss synthetisch und ohne private Spieldaten pruefen:

- Hints veraendern Zielkandidaten und Diagnostik, aber nie `unresolved`, harte
  Funktionsentries oder Exportvollstaendigkeit.
- Forced Overrides behalten bei abweichendem Runtimeziel den dynamischen
  Default.
- dieselbe Adresse wird als normale Instruktion und als Delay Slot getrennt
  erfasst; widerspruechliche Slots und Slot-Kontrollfluss sind sichtbar.
- Adressluecken erzeugen weder normale noch Call-/Branch-Fallthroughs.
- eine einzelne bewiesene oder unguarded Kante entfernt keinen partiellen
  Site-Default.
- bekannte und unbekannte Mehrcaller-Kontexte bleiben unvollstaendig; mehrere
  bekannte Kontexte vereinigen alle Zielwerte.
- endliche indirekte Callees vereinigen R0 nur bei vollstaendigen Summaries.
- Decodekandidaten ohne bewiesene Instruktionsgrenze bleiben partiell.
- CFG-Slices behandeln Joins, Schleifen, Stack-, Parameter-, Return- und
  VTable-Herkunft konservativ.
- kleine Programme werden gegen exhaustive Ausfuehrung verglichen; Berichte
  und Zielmengen sind bei identischer Eingabe bytegleich.

KR-4618 fuehrt die Regressionen im frischen Core-Gate-Build aus. Dieser
Implementierungstask zieht gemaess Handoff weder Konfiguration noch Build oder
Tests vor.
