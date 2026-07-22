# Initial-Snapshot-Vertrag

`EntryPointStraightLineQuiescent` ist ein explizites Loader- und
Plattformversprechen, kein aus dem SH-4-Code abgeleiteter Beweis. Es gilt nur
vom geladenen Reset-Snapshot bis zur ersten Gast-sichtbaren Kontrollflussgrenze.
In diesem Fenster muessen alle asynchronen Produzenten ruhen:

- DMA-Engines duerfen den Gastspeicher nicht veraendern,
- Geraete- und Hostcallbacks duerfen nicht in den Gastspeicher schreiben,
- Interruptpfade duerfen noch nicht ausgefuehrt werden,
- externe Runtime-Injektionen und Debugger-Schreibzugriffe muessen inaktiv sein.

Nur unter diesem Vertrag darf der gerade Pfad ab dem explizit ausgezeichneten
`initial_snapshot_entry` ein beschreibbares Literal zeitlich beweisen. Weitere
Analyse-Eintritte koennen davor oder in anderen Segmenten liegen und oeffnen
dieses Fenster nicht. Fehlt die Markierung, darf nur ein Image mit genau einem
Entry diesen als kompatiblen Fallback verwenden. Ein Join, eine Adressluecke,
Kontrollfluss, ein unbekanntes Schreibziel oder ein ueberdeckender Store beendet
das Fenster. Loader ohne dieses explizite Versprechen verwenden
`ImmutableOnly`; dort bleibt derselbe beschreibbare Wert `guarded`.

Der maschinenlesbare Kontrollflussbericht besitzt zwei Summenvertraege:

```text
resolved + guarded_complete + guarded_partial + runtime_only + unresolved
    == indirect_total
proven_instructions + guarded_candidate_instructions == instructions
```

`unresolved_frontier` ist `guarded_partial + runtime_only + unresolved`.
Guarded entdeckte Instruktionen sind analysier- und kompilierbare
Laufzeitkandidaten, aber nur `guarded_complete` ist eine vollstaendige
Zielmenge. Deshalb duerfen partielle Kandidaten nicht als statisch bewiesener
Portfortschritt ausgegeben werden. `runtime_only` bleibt Teil der offenen
Analysefront, darf seit KR-4718 aber als explizite Laufzeitabdeckung exportiert
werden; jedes konkrete Ziel muss dann den separaten Runtime-only-
Dispatchvertrag erfuellen.

Absolute Pointertabellen duerfen unter diesem Vertrag als endliche AOT-
Kandidaten gelesen werden, wenn Literal, Tabelle und Ziele committed sind und
die Tabelle aus der initialen Ladephase statt aus Runtime-Memory stammt. Bei
beschreibbarem Speicher bleibt die Dispatch-Site dennoch `runtime_only`; ihre
Herkunft ist `guarded_partial` und die Anfangswerte werden ausschliesslich als
`analysis_candidates` gefuehrt. Sie erzeugen keine bewiesenen CFG-Kanten und
keine Funktionsseeds. Das Lowering darf solche Ziele nur als Basic-Block-Leader
vorbereiten, damit ein spaeter validierter Runtimeeintritt normalen Fallthrough
innerhalb der bestehenden Funktion behaelt. Der aktuelle Laufzeitwert ist
immer autoritativ und jedes nicht vorbereitete Ziel endet ueber den sichtbaren
Runtime-only-Default.

Dieselbe Nicht-Beweis-Regel gilt fuer strukturell erkannte relative `BSRF`-
Handlerinseln. Ein initiales ausfuehrbares Nicht-RuntimeMemory-Segment darf eine
eindeutig maximale Folge gleich weit auseinanderliegender `RTS`-/Delay-Slot-
Handler mit obligatorischem terminalem `BRA`-Slot als endliche
`analysis_candidates` liefern. Der geladene relative Laufzeitwert bleibt
autoritativ; die Site ist `runtime_only`, und weder CFG-Kanten noch Funktionen
werden aus der Snapshotstruktur erfunden.
