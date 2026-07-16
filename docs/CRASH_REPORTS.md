# Kontrollierte Crashberichte

Der versionierte Bericht `katana-crash-report` beschreibt einen kontrollierten
Runtime-Abbruch ohne freien Hostfehlertext. Sein `stop_code` und alle Herkunfts-
beziehungsweise Aktionsfelder sind portable Tokens; Pfade, Hostzeiger und
Speicherinhalte koennen deshalb nicht versehentlich ueber diese Felder
serialisiert werden.

Erfasst werden:

- virtueller PC und kanonische physische Adresse
- allgemeine und banked Register sowie PR, SR, FPSCR und Exceptionregister
- Trap-, Exception- und Delay-Slot-Zustand mit SPC-Owner-PC
- Blockadresse, Endtyp, Provenienz und alle Blockvariantengenerationen
- logischer Schedulerzyklus und Anzahl ausstehender Ereignisse
- letzter Dispatch mit Callsite, Ziel, PR, Herkunft und Aktion

Die physische Blockadresse wird beim Capture kanonisiert. Blockadresse,
Blockvariante und Blockprovenienz muessen gemeinsam vorliegen. Ein Dispatch mit
Adresse verlangt Herkunft und Aktion. Bei einer Exception im Delay Slot wird
ein fehlender Owner aus dem architektonischen SPC uebernommen.

Der Bericht enthaelt bewusst keinen Runtime-Speicherdump und keine freie
Exceptionnachricht. Symbol- und Source-Map-Werkzeuge koennen die numerischen
Gastadressen nachtraeglich anreichern, ohne den Crashvertrag zu veraendern.
