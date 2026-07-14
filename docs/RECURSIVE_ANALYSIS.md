# Rekursive Programmanalyse

## Worklist ab Einstiegspunkten

`analyze_reachable_code` beginnt bei den bekannten Einstiegspunkten eines
`ExecutableImage`. Die Worklist dekodiert nur Zwei-Byte-Instruktionen innerhalb
committed Daten von ausfuehrbaren Code-Segmenten und verfolgt:

- normalen Fallthrough
- direkte bedingte und unbedingte Sprungziele
- direkte Callziele und die Rueckkehradresse
- Fallthrough nach indirekten Calls
- Delay Slots vor der Fortsetzung des Kontrollflusses

Return-, Trap-, Halt- und nicht aufgeloeste indirekte Spruenge beenden den
jeweiligen Pfad. Adressen werden genau einmal verarbeitet; die Ergebnisliste ist
nach Adresse sortiert. Nicht erreichbare Bytes werden nicht allein deshalb
dekodiert, weil sie in einem ausfuehrbaren Segment liegen.

Die bestehende lineare Disassembly bleibt als Diagnosepfad erhalten. Herkunft,
Code-/Datenklassifikation, nicht erreichbare Bereiche und Analyseberichte werden
in den nachfolgenden KR-1702- bis KR-1706-Tasks auf diesem Ergebnis aufgebaut.
