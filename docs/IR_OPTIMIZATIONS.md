# Katana-IR-Optimierungen

Alle Optimierungspaesse verifizieren eine Funktion vor und nach ihrer Ausfuehrung.
Eine Transformation darf keine Status-, Speicher-, Akkumulator- oder
Kontrollflusseffekte stillschweigend entfernen.

## Constant Folding

`fold_constants` verfolgt lokale 32-Bit-Konstanten innerhalb eines Basic Blocks.
Unterstuetzt werden unmittelbare und Registervarianten einfacher Addition,
Subtraktion und Bitlogik sowie NEG und NOT. Arithmetik verwendet definierten
32-Bit-Wraparound.

Operationen mit Status- oder Speichereffekten werden nicht gefaltet. Bei einer
nicht modellierten Registerwirkung verwirft der Pass vorsichtshalber seinen
gesamten lokalen Konstantenzustand.

## Copy Propagation

`propagate_copies` verfolgt lokale `MOV Rm,Rn`-Aliase und ersetzt Quellen
modellierter arithmetischer, logischer und vergleichender Operationen. Ein
Schreibzugriff auf die kopierte Quelle, das Ziel oder eine Alias-Kette invalidiert
alle betroffenen Beziehungen. Unbekannte Registerwirkungen leeren den Aliaszustand.
