# ABI-Wertanalyse fuer Callbacks, Parameter und Stack

Stand: KR-4716

Die SH-C-Funktionswertanalyse verfolgt R0 sowie die ABI-erhaltenen Register
R8 bis R14 getrennt je Funktionskontext. Ein Kontext wird erst dann als
geschlossen behandelt, wenn alle statisch bekannten direkten Callstellen und
alle vollstaendig bewiesenen indirekten Callstellen beobachtet wurden.
Entry-Points, unbekannte Caller und partielle indirekte Calleemengen erzeugen
dagegen einen unbekannten Ingress und koennen keine Zielvollstaendigkeit
herstellen.

## Endlichkeit und Guards

Jeder abstrakte Registerwert traegt unabhaengig:

- eine begrenzte, sortierte Kandidatenmenge;
- die Information, ob diese Menge vollstaendig ist;
- die Information, ob ihre Gueltigkeit zur Laufzeit bewacht werden muss;
- Callstellen und Callees als typisierte Evidenz.

Mehrere bekannte Caller werden vereinigt. Ein bewachter, aber vollstaendiger
Ingress bleibt `guarded_complete`; ein unbekannter oder unvollstaendiger
Ingress bleibt `guarded_partial`, `runtime_only` oder `unresolved`. Endliche
indirekte Calleemengen duerfen ihre R0-Summaries nur vereinigen, wenn die
Calleemenge und jede verwendete Return-Summary vollstaendig sind.

R13 wird als allgemeiner Callbacktraeger behandelt. Direkte R13-Dispatches
und einfache Registerkopien behalten die Herkunftsklasse `callback`.

## Begrenztes Stackmodell

R15 startet pro Funktionskontext bei dem symbolischen Offset null. Einfache
Registerkopien, Immediate-Additionen, Pre-Decrement und Post-Increment halten
symbolische Stack- beziehungsweise Framepointeroffsets bis zu einem Abstand
von 4096 Bytes. Longword-Stores und -Loads ueber feste Offsets erhalten einen
bekannten Spillwert. Ueberlappende Teilstores invalidieren den betroffenen
Slot.

Ein Store ueber eine nicht als Stackalias bewiesene Adresse, ein entkommener
Stackalias in caller-saved Registern oder ein nicht begrenzbarer Offset leert
die betroffenen Stackfakten konservativ. Framepointer, die durch `mov r15,Rn`
entstehen, werden in der Herkunftsklassifikation als `stack` und nicht als
Objektbasis behandelt.

## Terminierung und Bericht

Der globale Summary-/Ingress-Fixpunkt besitzt ein hartes Budget von 65536
Funktionsauswertungen. Bei Erschoepfung werden alle betroffenen Summaries und
Ingresswerte auf unbekannt herabgestuft; ein teilweise berechnetes Ergebnis
kann dadurch keinen Vollstaendigkeitsbeweis liefern.

Der Detailbericht gibt `function_iteration_budget`,
`function_budget_exhausted` und fuer jede Registersummary `guarded` aus.
Tests vergleichen Ergebnisse und die Budgetobergrenze, nicht eine konkrete
interne Iterationszahl.

## Gate-Regressionen

KR-4704 setzt die gesammelten synthetischen Faelle um und fuehrt sie aus:

- direkte und vollstaendig bewachte Calls;
- mehrere Caller mit verschiedenen R13- beziehungsweise Parameterwerten;
- endliche indirekte Calleemengen mit vereinigten R0-Summaries;
- Spill und Reload ueber festen Framepointeroffset;
- unbekannte Caller, unbekannte Stackaliase und Rekursion als Negativfaelle;
- Terminierung innerhalb des veroeffentlichten Budgets.
