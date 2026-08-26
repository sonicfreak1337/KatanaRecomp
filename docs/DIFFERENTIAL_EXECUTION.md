# Differenztests der Ausfuehrungswege

Fuer den nativen Produkt-Bring-up gilt zusaetzlich
[`NATIVE_BRINGUP_WORKFLOW.md`](NATIVE_BRINGUP_WORKFLOW.md): Zwei gebundene
Replays werden zuerst ueber grobe CPU-/Speicher-/Modul-/Providerdigests
verglichen und dann nur im ersten abweichenden Intervall feiner aufgezeichnet.
Der erste Unterschied erzeugt den naechsten konkreten Task; ein spaeter
Folgecrash ist keine bessere Fehlerlokalisierung.

Die nachfolgende synthetische Drei-Runner-Suite ist ein historischer,
isolierter Semantik-Harness. Ihr Interpreterpfad ist weder Bring-up- noch
Produktfallback und wird nicht in einen Port gelinkt.

Der KR-3707-Harness verlangt fuer jedes synthetische Mikroprogramm genau drei
Runner: `ir-reference`, `generated-cpp` und `interpreter-fallback`. Die Adapter
sind absichtlich explizit; ein Pfad darf nicht unter einem anderen Namen
wiederverwendet oder ausgelassen werden.

Jeder Runner liefert Checkpoints an denselben semantischen Grenzen. Der
Runtime-Checkpoint erfasst alle allgemeinen, gebankten und FPU-Register,
Sonderregister, sichtbare und rohe SR-/FPSCR-Zustaende, Ausnahmezustand,
Prefetchzustand, ausgewaehlte Speicherbytes, geordnete MMIO-Beobachtungen und
Schedulerstand. Zustandspfade werden sortiert und doppelte Pfade abgelehnt.

Verglichen wird checkpointweise mit der IR-Referenz. Der erste Unterschied
nennt:

- Checkpoint und Gast-PC
- Referenz- und abweichenden Ausfuehrungsweg
- exakten Zustandspfad
- erwarteten und tatsaechlichen Wert

Das versionierte JSON-Gegenbeispiel enthaelt Mikroprogrammidentitaet, Korpus,
Seed, Einstiegspunkt und Opcodes, aber keine Hostpfade oder externen Binaerdaten.
Das eingebaute synthetische Korpus reserviert feste Seeds fuer Delay Slots,
FPU-Modi, MMU-Uebersetzung, Store Queues und Busfehler. Die Regression beweist
mit einem absichtlich fehlerhaften Generated-C++-Adapter, dass die erste
Abweichung erkannt wird. Kompilation und Ausfuehrung erfolgen gesammelt in
KR-3709.
