# Dispatch- und Fallbackdiagnostik

Der versionierte `DispatchDiagnosticRecorder` beschreibt indirekte
Dispatchentscheidungen und kontrollierte Fallbacks, ohne selbst eine
Entscheidung zu treffen. Die Runtimepfade verwenden ausschliesslich die
ausnahmesichere `try_record`-Grenze. Ein Diagnosefehler kann deshalb weder PC,
PR oder Speicherzustand aendern noch einen erfolgreichen Tabellenlookup in
einen Fallback verwandeln.

Diese Trennung ist Teil des Evidence-Vertrags in
[`NATIVE_BRINGUP_WORKFLOW.md`](NATIVE_BRINGUP_WORKFLOW.md): Diagnoseereignisse
sind `Observed`, niemals `Proven`. Sie duerfen eine statische Behauptung
widerlegen und einen gerichteten Promotion-Auftrag erzeugen, aber keine
Dispatchklasse, Allowlist oder Closure selbst veraendern.

Der Standard-Bring-up speichert nur begrenzte, vorallokierte Ereignisse oder
Digests. Symbolisierung, Clustering, Last-Writer- und First-Divergence-
Auswertung laufen offline. Ein Diagnoseprofil, das den Produktlauf auf
sekundenlange Frames verlangsamt, ist kein Standardgurt.

Jedes Ereignis enthaelt:

- Callsite sowie virtuelle und kanonische Quelladresse
- virtuelles Ziel und getrennte kanonische Zieladresse
- den sichtbaren PR-Wert und den Blockendtyp
- Herkunft als statischer Beweis, Override, Tabellenlookup, Inline-Cache oder Fallback
- Aliasherkunft als exakter virtueller oder kanonisch-physischer Treffer
- Fallbackgrund und -aktion
- ausgefuehrte Gastinstruktionen und Austritts-PC
- einen stabilen Fehlercode

Die Fehlercodes unterscheiden unbekannten Code, unbekanntes Ziel, ungemappten
Speicher, verbotenen Firmwarepfad, ungueltige Ausrichtung und eine verletzte
Fallbackgrenze. Freie Hostfehlermeldungen, Pfade und Speicherinhalte werden
nicht serialisiert.

Identische Ereignisse werden nicht erneut angehaengt; nur `occurrences` und
`total_occurrences` werden erhoeht. Bis zur konfigurierten Kapazitaet bleibt
der erste vollstaendige Datensatz erhalten. Ist sie gefuellt, ersetzt ein
neuer Schluessel den Eintrag mit der kleinsten Vorkommenszahl. Wiederholt sich
der neue Schluessel, wird er ab dann normal aggregiert. So bleibt der Speicher
streng begrenzt, ohne einen erst spaet auftretenden echten Hotspot bei jeder
Wiederholung erneut als vermeintlich eindeutigen Verlust zu zaehlen.
