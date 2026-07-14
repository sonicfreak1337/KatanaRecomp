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

## Code-, Daten- und Unknown-Bereiche

Das Analyseergebnis enthaelt normalisierte, adresssortierte Bereiche. Erreichbare
Instruktionsbytes in ausfuehrbaren Code-Segmenten sind `Code`. Der Inhalt eines
deklarierten Datensegments ist `Data`. Nicht erreichte Bytes eines Code-Segments,
Zero-Fill und unbekannte oder nicht ausfuehrbare Segmente bleiben `Unknown`.

## Funktionsherkunft und Konfidenz

Funktionskandidaten speichern alle beobachteten Herkunftsgründe. Image-
Einstiegspunkte sind `certain`; gueltige direkte Callziele und Funktionssymbole
sind `high`. Treffen mehrere Gruende auf dieselbe Adresse, werden ihre Evidenzen
deterministisch zusammengefuehrt und die hoechste Konfidenz bleibt erhalten.

## Nicht erreichbare Bereiche

Committed Bytes eines ausfuehrbaren Code-Segments, die keine entdeckte
Instruktion abdeckt, werden zusaetzlich als `unreachable_code` ausgegeben.
Zero-Fill, Datensegmente und unbekannte Segmente gehoeren nicht zu dieser Menge.
