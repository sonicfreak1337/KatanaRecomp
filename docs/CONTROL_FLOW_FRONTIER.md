# Kontrollflussfront und Datenschutz

Stand: KR-4717

Jede indirekte Kontrollflussstelle besitzt genau einen der fuenf disjunkten
Berichtszustaende:

- `resolved`: statisch bewiesene und vollstaendige Zielmenge;
- `guarded_complete`: vollstaendige Zielmenge unter explizitem Guard;
- `guarded_partial`: endliche Kandidaten mit erhaltenem dynamischem Default;
- `runtime_only`: belastbar als rein dynamische Herkunft klassifiziert;
- `unresolved`: weder vollstaendiger Beweis noch Runtime-only-Vertrag.

Die Summe dieser Zustaende ist immer `indirect_total`. `guarded` bleibt nur als
abgeleiteter Kompatibilitaetszaehler fuer `guarded_complete + guarded_partial`
erhalten. `unresolved_frontier` umfasst `guarded_partial + runtime_only +
unresolved`; partielle Kandidaten koennen damit keinen vollstaendigen Build
mehr vortaeuschen.

## Herkunftsklassen

Jede Stelle der offenen Front besitzt genau eine typisierte Klasse:

- `callback`
- `parameter`
- `stack`
- `object-vtable`
- `table`
- `unbounded-memory`
- `runtime-pointer`

Die Klasse ist von der Beweisstaerke getrennt. `evidence` und die sortierten
`evidence_origins` bleiben die maschinenlesbare Herkunft; `reason` ist nur eine
menschenlesbare Diagnose. Fehlt fuer einen offenen Alt- oder Testdatensatz eine
explizite Klasse, wird er konservativ als `runtime-pointer` gezaehlt.

Ein validierter `HintCandidate` mit mindestens einem Kandidaten wird im Bericht
als `guarded_partial` gezaehlt. Sein interner `Unresolved`-Status, die schwache
Hint-Evidenz und der dynamische Default bleiben unveraendert. Ein Hint ohne
validierten Kandidaten bleibt `unresolved`.

KR-4716 kann vollstaendige bewachte Zielmengen aus geschlossenen Caller-
Kontexten, R13-Callbacks, festen Stackspills und vollstaendigen indirekten
Callee-Summaries ableiten. Unbekannte Caller oder Aliase bleiben Teil der
offenen Front. Der genaue Vertrag steht in `ABI_VALUE_ANALYSIS.md`.

KR-4717 ergaenzt dominante Objektfeld- und VTable-Initialisierungen. Nur
vollstaendige endliche Adress-/Wertmengen koennen `guarded_complete` werden;
beschreibbare statische Tabellen und invalidierte Objektfakten bleiben
`guarded_partial` oder dynamisch. Der Vertrag steht in `OBJECT_POINTS_TO.md`.

## Berichte und Datenschutz

`katana-control-flow-v3` ist der lokale Detailbericht. Er ist ausdruecklich mit
`privacy=local-detailed` markiert und darf Gastadressen, Symbole und einzelne
Evidenzketten enthalten. Er gehoert nicht in oeffentliche CI-Artefakte, wenn er
aus privaten Eingaben stammt.

`katana-control-flow-frontier-v1` enthaelt nur aggregierte Funktions-,
Instruktions-, Status- und Klassenzaehler. Er ist mit `privacy=aggregate`
markiert, enthaelt keine Gastadresse, keinen Symbolnamen und keinen lokalen
Pfad und ist deshalb der vorgesehene externe Statusbericht. Der
Anwendungsworkflow schreibt beide Dateien getrennt als `analysis.json` und
`control-flow-frontier.json`.

## Spaetere Gate-Regressionen

KR-4704 muss synthetisch pruefen:

- alle fuenf Statusklassen bilden eine disjunkte, vollstaendige Summe;
- Callback, Parameter, Stack, Objekt/VTable, Tabelle, unbeschraenkter Speicher
  und Laufzeitzeiger werden positiv oder konservativ negativ klassifiziert;
- jede offene Stelle besitzt eine nichtleere typisierte Beweisherkunft;
- ein `GuardedPartial` blockiert Anwendungs- und Buildvollstaendigkeit;
- ein `HintCandidate` mit validiertem Ziel erscheint im Detail- und
  Aggregatbericht als `guarded_partial`, behaelt aber Evidenz und Fallback;
- Hint-Zaehler halten das disjunkte Summeninvariant;
- der Aggregatbericht ist deterministisch und enthaelt weder Adressen noch
  Symbole oder Hostpfade;
- der lokale Detailbericht behaelt Klasse, Evidenz, Kandidaten und Grund.
