# CFG- und Callgraph-Export

KatanaRecomp exportiert das Ergebnis derselben Kontrollflussanalyse, die auch
IR und Port-Codegen speist, in deterministischem JSON und Graphviz-DOT. Fuer ein
Projektmanifest stehen vier CLI-Kommandos bereit:

```text
katana-recomp cfg-json project.katana [directives.json]
katana-recomp cfg-dot project.katana [directives.json]
katana-recomp callgraph-json project.katana [directives.json]
katana-recomp callgraph-dot project.katana [directives.json]
```

Ein Portexport legt dieselben Darstellungen als `cfg.json`, `cfg.dot`,
`callgraph.json` und `callgraph.dot` unter `generated/metadata/` ab. Dadurch
bleiben sie Teil des verwalteten Artefaktmanifests und veraltete Graphen werden
bei einer Regenerierung selektiv entfernt.

## Vertrag

JSON-Berichte verwenden den gemeinsamen Reportkopf, das Schema
`katana-analysis-graph` und `graph_version` 1. Knoten sind nach Gastadresse,
Endadresse und Symbol sortiert. Kanten sind nach Quelle, Callsite, Ziel und Art
sortiert; identische Kanten werden zusammengefasst. Adressen bleiben immer als
achtstellige hexadezimale Gastadressen erhalten, auch wenn ein Symbol vorhanden
ist.

Der CFG enthaelt Basic-Block-Knoten und unterscheidet Fallthrough, Branches,
bedingte Kanten sowie statisch aufgeloeste indirekte Spruenge. Der Callgraph
unterscheidet direkte und statisch aufgeloeste indirekte Calls. Nicht
aufgeloeste indirekte Spruenge und Calls bleiben in JSON als `target: null` und
in DOT als eigener Rautenknoten sichtbar. Exakte Symbole werden zusaetzlich
angegeben, ersetzen jedoch nie die numerische Adresse.

Absolute Hostpfade, Hostzeit und private Eingabebytes sind nicht Bestandteil
der Graphen. Dieselbe Eingabe, dieselben Analyseanweisungen und dieselbe
Werkzeugversion erzeugen bytegleiche Graphdateien.
