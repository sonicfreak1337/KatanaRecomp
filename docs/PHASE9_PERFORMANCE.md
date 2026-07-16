# Interner Phase-9-Performancevertrag

Phase 9 optimiert ausschliesslich Pfade, deren semantische Waechter explizit
erfuellt sind. Das exakte oder deterministisch gesampelte Profil verwendet
Gastadressen und stabile Blockidentitaeten; Eingabe-SHA, Runtime-ABI und
Backend-ABI binden jedes Profil an seinen Ursprung. Hot-Block- und
Hot-Edge-Listen sind nach Zaehler und danach Adresse sortiert.

Der lineare RAM-Fastpath ist nur bei nachgewiesener Region, Ausrichtung,
Schreibberechtigung, deaktivierter MMU, stabiler Aliasabbildung, fehlenden
Watchpoints/MMIO-Seiteneffekten sowie passenden Adressraum- und
Codegenerationen zulaessig. Jeder Fehlschlag wechselt in den normalen
`Memory`-Pfad. Direkte Schreibzugriffe melden weiterhin Codeinvalidierung.

Der monomorphe indirekte Dispatch speichert Callsite, Ziel, gesamten
`BlockVariantKey`, Blockgeneration und stabile Blockidentitaet. Jede
Ziel-/Waechter-/Generationsabweichung verwendet den generischen Dispatch; eine
Blockinvalidierung entfernt den Cacheeintrag. Treffer und Fehlschlaege bleiben
getrennt sichtbar.

Inlining ist auf heisse, nicht rekursive Callees mit hoechstens 24
Instruktionen und ausreichendem verbleibendem Codebudget beschraenkt. LTO und
PGO bleiben wie geplant ausserhalb des Phase-9-Scopes.

`katana-phase9-benchmark` trennt Analyse-, Codegen-, Start- und Laufzeit und
berichtet erzeugte C++- sowie Korpusgroesse. Der kumulative Gate-Runner ergaenzt
die echte Hostbuilddauer und bewertet alle Werte gegen die internen Budgets.
