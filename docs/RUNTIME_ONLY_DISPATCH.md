# Runtime-only-Dispatchvertrag

Stand: KR-4718

`runtime_only` ist eine explizite Laufzeitabdeckung und kein statischer Beweis.
Die Kontrollflussanalyse darf die Klasse nur fuer belastbar erkannte Callback-,
Parameter-, Stack-, Objekt/VTable- oder unbeschraenkte Speicherquellen setzen.
Ein Hint, Forced Override oder allgemeiner unbekannter Laufzeitzeiger bleibt
`guarded_partial` beziehungsweise `unresolved` und kann den Runtime-only-Pfad
nicht auswaehlen.

## Durchgaengige Klassifikation

Jede indirekte Stelle wird beim Lowering einer IR-Klasse zugeordnet:

- `guarded_complete` und `guarded_partial` verwenden den bewachten
  Tabellendefault;
- `runtime_only` verwendet ausschliesslich den Runtime-only-Dispatcher;
- `unresolved` bricht im gemeinsamen Runtimekern ohne Blocktabellenlookup ab;
- eine statisch aufgeloeste Stelle besitzt keinen dynamischen Default.

Optimierungen duerfen die Klasse nur entfernen, wenn sie den dynamischen
Kontrollfluss tatsaechlich durch eine Konstante ersetzen. Runtime-only-IR darf
keine geratenen `resolved_targets` tragen. Text- und JSON-IR serialisieren die
Klasse, damit Codegen und Review denselben Vertrag sehen.

Export und Anwendungsbuild verlangen weiterhin `unresolved == 0` und lehnen
`guarded_partial` als unvollstaendig ab. `runtime_only` ist exportierbar, weil
der generierte Port seine dynamischen Ziele an der folgenden Runtimegrenze
vollstaendig validiert.

## Zielvalidierung

Vor jeder Zustandsaenderung muss der Dispatcher bestaetigen:

1. Das 32-Bit-Gastziel ist mindestens 16-Bit-ausgerichtet.
2. Ein exakter virtueller Eintrag oder ein exakter kanonischer physischer Alias
   existiert fuer die aktuelle `BlockVariantKey`.
3. Das generationsgesicherte Handle ist weiterhin aktiv und aufloesbar.
4. Der Eintrag besitzt eine Backendfunktion, mindestens eine SH-4-Instruktion
   und einen ausgerichteten exakten Blockanfang.
5. Eine gebundene Codeinvalidierung haelt den Block weiterhin dispatchbar.

Die versiegelte statische beziehungsweise registrierte dynamische Blocktabelle
bildet dabei das ausfuehrbare Image und seine bewiesenen Instruktionsgrenzen ab.
Beliebige Hostadressen, Ziele in der Mitte eines Blocks, stale Handles,
Nullfunktionen und lediglich plausible oder geratene Adressen werden nicht
ausgefuehrt.

Ein Fehler veraendert weder `PC` noch `PR`, zaehlt den Miss und wirft einen
`IndirectDispatchError`. Der aktuelle generierte Port besitzt absichtlich
keinen Interpreter- oder No-op-Fallback. Ein Miss beendet deshalb den Lauf und
kann keinen Erfolg oder nachfolgenden Sonic-Checkpoint erzeugen.

## Maschinenmetriken

`IndirectDispatchMetrics` verwendet gesaettigte 64-Bit-Zaehler fuer:

- alle Hits, Misses und kontrollierten Fallbacks;
- Runtime-only-Hits, -Misses und -Fallbacks;
- den ersten Fehler mit Fehlerklasse, Dispatchklasse, Callsite und Ziel.

`katana-indirect-dispatch-v1` serialisiert diese Felder deterministisch. Der
Portadapter uebernimmt sie in `RuntimeRunResult` und die
`KATANA_RUNTIME_METRICS`-Zeile. Bei der aktuellen Stop-on-Miss-Policy bleiben
Fallbackzaehler null. Der Miss-Snapshot wird in der typisierten Exception
erhalten und vor dem Fehlerexit als `KATANA_RUNTIME_DISPATCH_ERROR`-JSON-Zeile
ausgegeben. Ein spaeterer kontrollierter Fallback muss vor jedem Fortsetzen
explizit `record_fallback()` aufrufen; ein stiller Fallback ist kein gueltiger
Vertrag.

Runtime-ABI 12 versioniert Dispatchklasse, Request und Metriken.
Backend-Interface-ABI 2 versioniert die IR-/Codegenweitergabe;
Portprojektvertrag 4 versioniert Adapter und Laufresultat.

## Gate-Regressionen fuer KR-4704

Das frische Gate muss ohne private Daten pruefen:

- alle fuenf Berichtsklassen werden eindeutig in die IR uebernommen und in
  Text sowie JSON serialisiert;
- nur belastbar klassifizierte Runtimequellen erzeugen `runtime_only_call` oder
  `runtime_only_jump`; Hints, Overrides und unbekannte Zeiger tun dies nie;
- Runtime-only-IR mit geratenen statischen Zielen wird verworfen;
- gueltige virtuelle und physische Aliasziele treffen den exakten aktiven
  Blockanfang und zaehlen getrennte Hits;
- ungerade, unregistrierte, stale, mittige, zu kleine und funktionslose Ziele
  schlagen vor `PC`-/`PR`-Mutation fehl;
- ein hostadressenaehnlicher 32-Bit-Wert ohne Blockeintrag und ein No-op-
  Ersatzpfad werden sichtbar abgelehnt;
- Misszaehler und erster Fehler bleiben auch beim Wurf lesbar und das JSON
  enthaelt Gesamt- sowie Runtime-only-Zahlen;
- der generierte Stop-on-Miss-Pfad meldet weder Erfolg noch einen Checkpoint
  nach `SA_ANALYSIS_CONTINUES`;
- Portexport und Anwendungsbuild akzeptieren `runtime_only`, lehnen aber
  `guarded_partial` und `unresolved` ab;
- Runtime-ABI 12, Backend-Interface-ABI 2 und Portprojektvertrag 4 werden von
  In-Tree- und Out-of-Tree-Consumern konsistent geprueft.

Gemaess Handoff werden diese Regressionen erst in KR-4704 implementiert,
gebaut und ausgefuehrt.
