# Initial-Snapshot-Vertrag

`EntryPointStraightLineQuiescent` ist ein explizites Loader- und
Plattformversprechen, kein aus dem SH-4-Code abgeleiteter Beweis. Es gilt nur
vom geladenen Reset-Snapshot bis zur ersten Gast-sichtbaren Kontrollflussgrenze.
In diesem Fenster muessen alle asynchronen Produzenten ruhen:

- DMA-Engines duerfen den Gastspeicher nicht veraendern,
- Geraete- und Hostcallbacks duerfen nicht in den Gastspeicher schreiben,
- Interruptpfade duerfen noch nicht ausgefuehrt werden,
- externe Runtime-Injektionen und Debugger-Schreibzugriffe muessen inaktiv sein.

Nur unter diesem Vertrag darf der gerade Pfad ab dem einzigen Entry ein
beschreibbares Literal zeitlich beweisen. Ein Join, eine Adressluecke,
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
Portfortschritt ausgegeben werden.
