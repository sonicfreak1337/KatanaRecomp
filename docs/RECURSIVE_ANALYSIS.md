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

`analyze_control_flow` fuehrt diese Entdeckung zusammen mit der indirekten
Zielanalyse bis zum Fixpunkt aus. Neu bewiesene Jumpziele werden als Nachfolger,
indirekte Callziele zusaetzlich als Funktionskandidaten eingespeist. Calls
behalten ihren Rueckkehrpfad nach dem Delay Slot; Jumps erzeugen keinen solchen
Fallthrough. Vollstaendig gueltige Jump Tables speisen alle Ziele gemeinsam ein,
teilweise ungueltige Tabellen keines. Jede Seed-Adresse und jede Instruktion wird
deterministisch dedupliziert, sodass Selbstspruenge und Zyklen terminieren.

Alle Einstiegspunkte und nachtraeglichen Ziele muessen gerade sein und mindestens
zwei committed Bytes in einem ausfuehrbaren Code-Segment besitzen. Zero-Fill ist
kein Codebeweis. Die gemeinsame Validierung gilt ebenso fuer Overrides und
Jump-Table-Ziele.

Ein unbekannter Opcode wird als problematische Instruktion mit Adresse, 16-Bit-
Opcode und dem stabilen Grund `unknown-opcode` aufgenommen. An dieser Stelle
endet nur der betroffene Pfad; andere bereits bewiesene Worklist-Pfade laufen
weiter. Auch ein unbekannter Delay Slot verhindert unsichere Nachfolger.

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
Einstiegspunkte sind `certain`; gueltige direkte und indirekte Callziele,
Call-Tabellenziele und Funktionssymbole sind `high`. Manuelle Funktionshinweise
tragen `user-override`. Treffen mehrere Gruende auf dieselbe Adresse, werden ihre
Evidenzen deterministisch zusammengefuehrt und die hoechste Konfidenz bleibt erhalten.

## Nicht erreichbare Bereiche

Committed Bytes eines ausfuehrbaren Code-Segments, die keine entdeckte
Instruktion abdeckt, werden zusaetzlich als `unreachable_code` ausgegeben.
Zero-Fill, Datensegmente und unbekannte Segmente gehoeren nicht zu dieser Menge.

## Ueberlappende Rollen

Ein Funktions- oder Image-Einstieg innerhalb eines Delay Slots ist eine
mehrdeutige Rollenbelegung. Die Analyse verarbeitet die Adresse weiterhin nur
einmal und gibt einen deterministischen Konfliktbereich mit dem Grund
`function-entry-in-delay-slot` aus. Physisch ueberlappende Image-Segmente bleiben
bereits durch das Executable-Image-Modell ungueltig.

## Analysebericht

`format_recursive_analysis_report` erzeugt eine deterministische Textausgabe mit
Summen und adresssortierten Details. Jede Funktion nennt Konfidenz und alle
Herkunftsgruende; Bereiche, unerreichbarer Code und Konflikte enthalten Adresse,
Groesse, Klassifikation beziehungsweise Grund. Pfadabbrueche durch unbekannte
Opcodes erscheinen adresssortiert mit Opcode und Diagnosegrund.
