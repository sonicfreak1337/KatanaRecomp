# Objekt-, Feld- und VTable-Points-to

Stand: KR-4717

Die Funktionswertanalyse fuehrt neben Register- und Stackwerten eine begrenzte
Memory-Forwarding-Tabelle fuer dominante Objektinitialisierungen. Sie ist kein
allgemeiner Heap-Solver: Nur vollstaendige endliche Adressmengen und exakte
Longword-Stores koennen einen Feldwert erzeugen.

## Verfolgte Pfade

Registerkopien und feste Additionen erhalten konkrete Objektzeiger. Diese
Zeiger koennen ueber das begrenzte Stackmodell aus KR-4716 gespillt und wieder
geladen werden. Direkte, Pre-Decrement-, Displacement- und R0-indizierte
Longword-Stores schreiben einen abstrakten Feldwert, sofern ihre gesamte
Adressmenge bekannt ist. Longword-Loads ueber dieselben Adressen verwenden den
dominierenden Store vor dem statischen Image.

Dadurch werden insbesondere diese Muster abgedeckt:

- Konstruktor- oder Initialisierungsstore eines VTable-Zeigers;
- Callbackfelder an festem Objektoffset;
- VTable-Load gefolgt von einem festen Slot-Load;
- CFG-Joins mit mehreren vollstaendigen konkreten Typwerten;
- Objektzeiger, die zuvor ueber Register oder einen festen Stackslot liefen.

Die Tabelle ist auf 256 Longword-Fakten und jede Kandidatenmenge auf acht
Werte begrenzt. Eine Ueberschreitung stuft die Fakten auf unbekannt herab.

## Guards und Invalidierung

Ein weitergeleiteter Laufzeitspeicherwert ist immer bewacht. Vollstaendig ist
er nur, wenn Adressmenge und gespeicherter Wert vollstaendig sind und alle
eingehenden Kontexte denselben Feldfakt besitzen. Mehrere Werte bleiben eine
vollstaendige endliche Menge und koennen `guarded_complete` erzeugen.

Teilbreite oder ueberlappende Stores invalidieren den betroffenen Longword-
Fakt. Ein unbekannter Basiszeiger, eine partielle Adressmenge, R0-/GBR-
Seiteneffekte, atomare Byteoperationen, FPU-Stores, PREF, TRAPA oder ein
unbekannter Opcode invalidieren konservativ. Calls loeschen Objektfakten im
Caller; der Zustand vor dem Call darf einem vollstaendig bekannten Callee als
Ingress dienen. Ein Callee mit vollstaendigen Returnpfaden gibt die auf allen
Returns gemeinsamen Feldfakten als Memory-Summary zurueck. Damit bleiben auch
Konstruktorstores nach einem direkten oder vollstaendig bewachten Call
sichtbar. Unvollstaendige oder rekursive Summaries stellen keine Fakten wieder
her.

Statische Loads aus einem beschreibbaren Segment bleiben wie bisher
`guarded_partial`: Der Imageinhalt kann extern mutieren und wird niemals durch
seine momentane Kandidatenmenge eingefroren. Nur ein im analysierten Pfad
dominierender Store darf einen vollstaendigen, weiterhin bewachten Feldwert
begruenden.

## Herkunft

Loads ueber R4 bis R14 mit festem Feldpfad und zweistufige VTable-/Slotloads
werden als `object-vtable` klassifiziert. Ein von R15 abgeleiteter R14-
Framepointer behaelt Vorrang und bleibt `stack`; ein unbeschraenkter
Speicherzeiger bleibt `unbounded-memory`.

## Gate-Regressionen

KR-4704 setzt die gesammelten synthetischen Faelle um und fuehrt sie aus:

- Konstruktorstore, direkter Callbackfeldoffset und VTable-Slot;
- Objektzeiger ueber Register und festen Stackspill;
- CFG-Join mit mehreren konkreten Typen als endliche Zielmenge;
- ueberlappender Teilstore, Aliaswechsel und unbekannter Store als
  Negativfaelle;
- Mutation ueber Call, PREF und nicht modellierte Stores;
- beschreibbare VTable bleibt `guarded_partial`, ein dominierender
  Initialisierungsstore darf `guarded_complete` werden;
- Kandidaten-, Feld- und Fixpunktbudgets werden nicht ueberschritten.
