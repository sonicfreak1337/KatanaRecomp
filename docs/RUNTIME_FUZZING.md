# Multi-Segment-, Dispatch- und Invalidierungsfuzzing

Das KR-3708-Ziel `katana-fuzz --target runtime` erzeugt ausschliesslich
synthetische, begrenzte Runtimezustaende. Jeder Fall kombiniert:

- ein bis vier nicht ueberlappende Code-/Datensegmente mit variierenden Berechtigungen und Basisadressen
- variierende kanonische Dreamcast-Aliasadressen und optionale MMU-Abbildungen mit TLB-Berechtigungen
- Blockvarianten aus Adressraum-, MMU-, Watchpoint-, FPSCR- und Seitengeneration
- variierende indirekte Ziele und Callsites, exakten sowie kanonisch-physischen Dispatch und einen Callsite-Cache-Vergleich
- echte Backendausfuehrung, ROM-RAM-Bytekopien sowie CPU-/DMA-/Copy-Schreibpfade

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
ueber Ziel, Seed, Iteration und Groessenlimit reproduzierbar. `--isolate`
startet jeden Kandidaten in einem Kindprozess, sodass auch Sanitizer- und
Prozessabbrueche eine stabile Exit-Signatur fuer den Delta-Reducer liefern. Das
minimale Abbild kann mit `--target <ziel> --input-hex <hex>` direkt wiedergegeben
werden. Der feste `all`-Kurzlauf ist Bestandteil des gebuendelten
KR-3709-Debugprofils und wurde am v0.37.0-Gate ausgefuehrt.
