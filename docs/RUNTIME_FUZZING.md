# Multi-Segment-, Dispatch- und Invalidierungsfuzzing

Das KR-3708-Ziel `katana-fuzz --target runtime` erzeugt ausschliesslich
synthetische, begrenzte Runtimezustaende. Jeder Fall kombiniert:

- ein bis vier nicht ueberlappende Code-/Datensegmente mit Berechtigungen
- kanonische Dreamcast-Aliasadressen und optional eine MMU-Abbildung
- Blockvarianten aus Adressraum-, MMU-, Watchpoint-, FPSCR- und Seitengeneration
- exakten sowie kanonisch-physischen indirekten Dispatch
- ROM-RAM-Blockprovenienz, Callsite-Links und CPU-/DMA-/Copy-Writes

Ueberlappende Segmente und wechselnde Blockprovenienz muessen abgewiesen
werden. Der Manifestvertrag verbietet nun auch Aliasverkettungen: Ein
physisches Aliasziel darf nicht erneut in einem virtuellen Aliasbereich liegen.
Damit werden Selbstreferenzen, laengere Ketten und Zyklen vor der Runtime
abgelehnt.

Nach einer Watchpoint- oder Schreibgeneration wird mit dem neuen
`BlockVariantKey` erneut gesucht; die alte Variante darf nicht gefunden werden.
Eine invalidierte Code-Tracker-Registrierung darf nur mit identischer Adresse,
Groesse, Herkunft und Provenienz reaktiviert werden. Doppelte Links bleiben
idempotent.

Gastadressen sind in allen Fuzzstrukturen feste `uint32_t`-Werte. Der Fuzzer
liest weder `uintptr_t` noch Hostzeiger aus Eingabebytes und kann diese daher
nicht als Gastadresse in Blocktabelle oder Dispatch einspeisen. Crasher bleiben
ueber Ziel, Seed, Iteration und Groessenlimit reproduzierbar. Der Delta-Reducer
liefert dazu ein minimales synthetisches Hex-Abbild und einen Manifestkern mit
Segmentzahl, MMU-Modus und Schreibquelle. Der feste
`all`-Kurzlauf wird erst gesammelt in KR-3709 ausgefuehrt.
