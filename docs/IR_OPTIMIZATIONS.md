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

## Dead-Code-Elimination

`eliminate_dead_code` entfernt nur reine Registerdefinitionen, die im selben Block
vor jeder Nutzung durch eine weitere reine Definition desselben Registers ersetzt
werden. Blockanfaenge bleiben stabil. Delay Slots sowie Status-, Speicher-,
Akkumulator-, Privileg- und unbekannte Effekte bilden harte Grenzen.

## CFG-Simplifizierung

`simplify_cfg` berechnet die vom Funktionseintritt erreichbaren Bloecke und entfernt
den nicht erreichbaren Rest. Nachfolgerlisten werden sortiert und dedupliziert;
Blockreihenfolgen werden nach Startadresse kanonisiert. Kontrollinstruktionen und
ihre Delay Slots werden nicht umgeschrieben.

## Load-Store-Vereinfachung

`simplify_load_store` erkennt einen unmittelbar auf `MOV.L Rm,@Rn` folgenden
`MOV.L @Rn,Rd` im selben Block. Der Load erhaelt den zuvor gespeicherten
Registerwert, fuehrt seinen `read_u32` aber weiterhin aus. Damit bleiben
Speichergrenzen und spaetere beobachtbare Bus- oder MMIO-Effekte erhalten.

Andere Transferbreiten, verschiedene Adressregister und nicht direkt benachbarte
Zugriffe werden nicht veraendert. Der Verifier akzeptiert eine Weiterleitung nur,
wenn der passende Store direkt vor dem Load steht.
