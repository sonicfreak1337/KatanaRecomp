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

## Pass-Pipeline

`optimize_program` fuehrt die Paesse deterministisch in dieser Reihenfolge aus:

1. Constant Folding
2. Copy Propagation
3. Dead-Code-Elimination
4. CFG-Simplifizierung
5. Load-Store-Vereinfachung

Jeder Pass kann ueber `OptimizationOptions` einzeln abgeschaltet werden. Mit
`capture_dumps` enthaelt der Bericht fuer jeden aktiven Pass eine deterministische
Text-IR vor und nach seiner Ausfuehrung. `enabled=false` laesst das gesamte Programm
bytegenau unveraendert.

Der CLI-Pfad `emit-cpp` verwendet die Pipeline standardmaessig. `--no-opt`
deaktiviert sie vollstaendig; `--dump-ir <Praefix>` schreibt
`<Praefix>.before.ir` und `<Praefix>.after.ir`.
