# Einheitliche Gastwrites und Codeinvalidierung

KR-4613 macht `Memory` zur einzigen beobachtbaren Commitgrenze fuer
Gastschreibzugriffe. Generierte CPU- und FPU-Stores, DMA, Store Queues,
Boot-/Modulkopien und kontrollierte Fallbacks verwenden dieselben skalaren
`write_u8`-, `write_u16`- und `write_u32`-Operationen oder den gebuendelten
`write_bytes`-Pfad.

## Writevertrag

Nach einem erfolgreichen Speichercommit meldet `Memory` genau ein
`GuestWriteEvent` mit virtueller Startadresse, Groesse, Herkunft und dem
Ergebnis des Bytevergleichs. Die Herkunft unterscheidet `Cpu`, `Fpu`, `Dma`,
`StoreQueue`, `Copy` und `Fallback`.

Bei `LinearMemoryDevice` wird der vorhandene Inhalt vor dem Commit mit dem
neuen Wert verglichen. Ein identischer Write bleibt fuer Speichertraces und
Watchpoints sichtbar, erhoeht aber keine Codegeneration und invalidiert keinen
Block. MMIO und Speichergeraete ohne sicheren, nebenwirkungsfreien
Bytevergleich gelten konservativ als geaendert. Ein `write_bytes`-Commit
erzeugt eine gemeinsame Codebeobachtung fuer den gesamten Bereich statt einer
Invalidierung pro Byte.

Direkte Zugriffe auf `MemoryDevice::write_*` und `writable_bytes()` sind keine
Gastwrite-API. Sie bleiben auf die Initialisierung eines noch nicht
beobachteten Backings begrenzt.

## Invalidierung

Die Dreamcast-Runtime verbindet den Writeobserver mit dem
`ExecutableCodeTracker`. Ein geaenderter Write wird auf die physische Adresse
kanonisiert und fuehrt atomar aus Sicht des naechsten Dispatchs zu:

1. Generationserhoehung aller ueberdeckten Codeseiten,
2. Invalidierung aller ueberlappenden registrierten Bloecke und ihrer
   physischen Aliase,
3. Erfassung und Trennung der eingehenden Quelllinks,
4. Entfernung ueberlappender Eintraege aus der Runtime-Blocktabelle.

Zusaetzlich kann jede `RuntimeBlockTable` an denselben Tracker gebunden werden.
Virtuelle, physische und Alias-Lookups liefern dann keinen Block mehr, sobald
ein ueberlappender Trackerbereich invalidiert ist. Der Direktdispatcher und
der monomorphe Inline-Cache muessen ihr Ziel erneut ueber diese Lookups
validieren und koennen deshalb keinen stale Hostblock ausfuehren. Tabellen und
Tracker verwenden dafuer dieselbe `stable_runtime_block_identity`. Der
erzeugte Port bindet seine lokale statische Tabelle vor der ersten
Registrierung und registriert genau diese stabile Identitaet.

PlatformServices-ABI 4 stellt dem erzeugten Port dafuer den optionalen
`ExecutableCodeTracker` bereit. Dienste ohne ausfuehrbares RAM koennen wie
bisher `nullptr` liefern.

## Spaetere Testanforderungen

KR-4617 muss synthetisch und ohne private Spieldaten pruefen:

- generierte CPU-Stores aller Breiten und FPU-Einzel-/Paarstores invalidieren
  denselben ueberdeckten Code wie der Referenzpfad,
- DMA-Groessen 1, 2, 4 und 32 Byte, Store-Queue-PREF, MOVCA, Copy und Fallback
  melden korrekte Bereiche und Herkunft,
- ein identischer Write behaelt Seitengeneration, Blockgueltigkeit,
  eingehende Links und Dispatchcache bei,
- geaenderte, teilweise ueberlappende Writes invalidieren genau alle
  ueberdeckten Bloecke,
- P0-, P1- und P2-Aliase desselben Haupt-RAM-Backings werden gemeinsam
  unerreichbar,
- `lookup`, `lookup_physical`, `aliases`, Direktdispatch und
  `MonomorphicDispatchCache` koennen nach dem Write keinen stale Block liefern,
- sicherer `Memory`-Write und `GuardedMemoryFastpath` liefern fuer identische
  Eingaben bytegleiche RAM-Inhalte, Generationsfolgen und Invalidierungen.

KR-4618 fuehrt diese Regressionen im frischen Core-Gate-Build aus. Bis dahin
werden gemaess dem Entwicklungsvertrag keine Builds oder Tests vorgezogen.
