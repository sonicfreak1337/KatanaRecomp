# Projektstatus

Interner Entwicklungsmeilenstein: `v0.46.0`
Phase: Core-Stabilisierung vor v0.47
Naechster Task: `KR-4716`
Naechstes Gate: `v0.47.0` - Core-Stabilisierung und generische Retail-Runtime
Weitere interne Gates: `v0.48.0` Integration und `v0.49.0` Alpha-Candidate
Erster oeffentlicher Release: `v0.50.0` Alpha

## KR-4621 umgesetzt

Der Speicherbus verwendet einen abschaltbaren 64-KiB-Regionsindex, native
lineare 16-/32-Bit-Zugriffe und einen ereignisfreien Pfad ohne Trace oder
Watchpoint. Exakter virtueller Dispatch besitzt einen direkten Hashindex; die
Codeinvalidierung untersucht ueber einen Page-to-Block-Index nur beruehrte
Kandidaten. Geordnete beziehungsweise lineare Referenzmodi bleiben fuer
Differenzlaeufe erhalten.

Invalidierungs-, Dispatch- und Store-Queue-Diagnostik sind fest begrenzt und
melden verworfene Details ueber Aggregatzaehler. Automatische DMA-Transfers
koennen deterministisch bis vor das naechste fremde Schedulerereignis
gebuendelt werden; Einzeltransfer-, externe Request- und Round-Robin-Pfade
bleiben als Referenz erhalten. Vertrag und Gate-Messpunkte stehen in
[`P1_HOTPATHS.md`](P1_HOTPATHS.md).

## KR-4622 umgesetzt

Kontrollfluss-Fixpunkte uebernehmen bekannte Kontextschluessel und dekodieren
bei neuen Seeds nur die Deltafront. Die Funktionswertanalyse ordnet ihre
monotone Worklist nach Callgraph-SCCs und reiht Caller beziehungsweise Callees
nur bei geaenderter Summary oder geaendertem Ingress erneut ein. Immutable
Instruktionsarenen, Blockspans, gemeinsame Adressindizes und internierte
Evidence-IDs bilden die gemeinsame Analysegrundlage.

Jump-Table-Snapshots sind SHA-256-gebunden und begrenzt. Codegenpartitionen
tragen einen kanonischen IR-Hash und koennen als Delta ermittelt werden; der
Codegencache verwendet Schema 3 mit SHA-256-Schluesseln und atomarem,
konkurrenzsicherem Publish. Vertrag und Budgets stehen in
[`P1_INCREMENTAL_ANALYSIS.md`](P1_INCREMENTAL_ANALYSIS.md).

## KR-4623 umgesetzt

Trackdateien werden einmal read-only geoeffnet und danach ueber persistente
Handles gelesen. GDI besitzt Tracknummer- und LBA-Indizes, gebuendelte
Trackreads sowie einen abschaltbaren, auf 256 Sektoren begrenzten Cache.
ISO9660 cacht Verzeichnisse und Extents mit festen Obergrenzen; Cache-an und
Referenzmodus behalten identische Bytes und Pfadfehler.

Descriptor- und Track-SHA-256 werden beim Oeffnen erfasst und vom Portexport
wiederverwendet. Die portable GDI-Identitaet ist jetzt SHA-256-basiert.
GD-ROM-Completions bleiben ohne Hostuhr auf dem zentralen Gastscheduler. Der
Vertrag steht in [`P1_DISC_IO.md`](P1_DISC_IO.md).

## KR-4624 umgesetzt

Core und CLI sind der GUI-freie Standardbuild. MSVC, GCC und Clang besitzen
Debug-/RelWithDebInfo-Presets und werden in derselben sechsfachen CI-Matrix
regressionsgeprueft. Ein optionaler Compilerlauncher bindet ccache oder sccache
ein; alle Tests tragen stabile Subsystemlabels fuer getrennte Shards.

Projekt-, Paket- und ABI-Versionen entstehen aus kanonischen CMake-Werten. Das
installierbare `KatanaRecomp::runtime`-Paket bleibt von Analyzerquellen frei;
`KatanaRecomp::analyzer` ist ein getrennter Zusatz. Ein echter Out-of-Tree-
Consumer prueft diese Grenze. Vertrag und Ausgangsbaseline stehen in
[`P1_BUILD_GRAPH.md`](P1_BUILD_GRAPH.md).

## KR-4625 umgesetzt

Das Performance-/Buildgate erstellt Quality-Debug und RelWithDebInfo jeweils
frisch mit fester Linkparallelitaet. Quality-Debug bestand 168 von 168 Tests
mit MSVC-ASan und statischer Analyse; RelWithDebInfo bestand 167 von 167 Tests.
Beide Profile besitzen dasselbe Inventar aus 167 Core-Regressionen. Format-,
Qualitaetsvertrags- und Referenz-/Lizenzaudit sind erfolgreich, alle
instrumentierten Performancevertraege halten ihre Budgets und private
Retaildaten wurden nicht verwendet.

`Memory::write_bytes()` prueft gebuendelte Zielbereiche und vorhandene Bytes
vollstaendig vor dem Commit. Ein Fehler an einer Regions- oder
Schreibschutzgrenze veraendert kein Praefix; ein spaeter Geraetefehler meldet
jeden bereits geschriebenen Bereich noch vor dem Weiterwerfen zur
Codeinvalidierung. Write-only-MMIO wird dabei nicht vorgelesen und gilt
pessimistisch als geaendert. Das erzeugte Ninja-Projekt konfiguriert Runtime-
Includes, Buildvertrag und Hosttoolchain wirklich und wird in einer frischen
Regression gebaut. Das Gate wiederholt ausschliesslich erkannte Windows-
Linkerausgabesperren und protokolliert Versuch, Exitcode und Grund. Der lokale
JSON-Bericht wird durch einen identischen
Windows-CI-Buildgate als GitHub-Actions-Artifact unabhaengig nachvollziehbar.
Damit ist Stufe B abgeschlossen und KR-4715 beginnt.

## KR-4715 umgesetzt

Indirekte Stellen werden als `resolved`, `guarded_complete`,
`guarded_partial`, `runtime_only` oder `unresolved` disjunkt gezaehlt. Jede
offene Stelle traegt genau eine typisierte Callback-, Parameter-, Stack-,
Objekt/VTable-, Tabellen-, unbeschraenkte Speicher- oder Laufzeitzeigerklasse
und eine maschinenlesbare Evidenzherkunft. Partielle Kandidaten bleiben im
dynamischen Default und koennen Anwendungs-Vollstaendigkeit nicht herstellen.

Der lokale Bericht `katana-control-flow-v3` behaelt Adressen und Einzeldetails;
`katana-control-flow-frontier-v1` enthaelt ausschliesslich adressfreie
Aggregatzaehler. Anwendungsworkflow, Buildplan und Portmetadaten geben die
Klassen getrennt aus. Der Vertrag steht in
[`CONTROL_FLOW_FRONTIER.md`](CONTROL_FLOW_FRONTIER.md). KR-4716 ist der naechste
Task.

## Historischer Reviewbefund vor KR-4611 bis KR-4618

Der folgende Befund dokumentiert ausschliesslich den damaligen Ausgangspunkt.
Die Kontrollflussrisiken wurden mit KR-4611 bis KR-4614, die Harness- und
Gatepunkte mit KR-4616 bis KR-4618 abgearbeitet. Der aktuelle Stand steht in
den jeweiligen Umsetzungsabschnitten weiter unten.

Die damalige Kontrollflusspruefung hatte folgende P0-Soundnessrisiken gefunden:

- Hint-Direktiven werden als `Resolved` und damit als Beweis behandelt
- Delay-Slot-Kontext wird global nur nach Adresse gespeichert
- Basic Blocks koennen Fallthrough ueber Adressluecken erzeugen
- Site-Vollstaendigkeit wird auf ein Kantenbit reduziert
- unbekannte Caller und abweichende Callkontexte koennen Zielmengen verkleinern
- Kontextaufloesungen werden nur nach Instruktionsadresse dedupliziert

Der private Harness startet nach erfolgreichem Build automatisch `game.exe`.
Das widerspricht dem aktuellen v0.47-Build-only-Vertrag. Metriken werden noch
aus Textregexen gelesen, stdout und stderr verlieren ihre Reihenfolge und
Hostsmokes sind nicht ausreichend von Gastfortschritt getrennt.

Die Controllergrundlage existiert in Maple und Hostruntime. Der generierte Port
pollt Eingaben jedoch erst nach dem synchronen Gastlauf. Controllerunterstuetzung
braucht deshalb vor allem eine echte verschraenkte Runtime-Schleife, nicht noch
eine weitere Taste in einem Enum.

Die GUI besitzt intern mehrere Seiten und strukturierte Jobereignisse, zeigt
unter Windows aber fast nur eine Fliesstextzusammenfassung, zwei Balken und ein
Logfeld. Layout und Aktualisierung sind hart kodiert beziehungsweise
pollingbasiert.

Der vollstaendige Befund steht in
[`CONTROL_FLOW_HARNESS_GUI_REVIEW_2026-07-18.md`](CONTROL_FLOW_HARNESS_GUI_REVIEW_2026-07-18.md).

## Task-ID-Korrektur

Die historischen Bedeutungen von `KR-4801` bis `KR-4805` und `KR-4901` werden
wiederhergestellt. Die zwischenzeitlich mit diesen IDs bezeichneten
Alpha-Aufgaben erhalten `KR-4911` bis `KR-4916`.

Die kanonische Historie steht in
[`TASK_ID_REGISTRY.md`](TASK_ID_REGISTRY.md).

## KR-4611 umgesetzt

Der SH-4-Kontrollzustand verwendet fuer R0 bis R7 jetzt die effektive
Bankauswahl `SR.MD && SR.RB`. BSR, BSRF und JSR schreiben PR vor ihrem Delay
Slot; ein PR-Schreibzugriff des Slots bleibt dadurch sichtbar. RTE und SLEEP
besitzen getrennte Blockendtypen in Block-ABI 2. Der Portdispatcher setzt nach
normalen Gast-Exceptions und Interrupts am jeweiligen Handlervektor fort,
behandelt RTE als dynamische Fortsetzung bei SPC und fuehrt waehrend SLEEP
keinen weiteren Block vor der Annahme eines Interrupts aus.

Exceptionursache, Gast-Eventcode und Vektor werden aus einer gemeinsamen
Metadatentabelle abgeleitet. Die spaeter bei KR-4617 und KR-4618 umzusetzenden
und auszufuehrenden Testanforderungen stehen in
[`SH4_CONTROL_STATE.md`](SH4_CONTROL_STATE.md).

## KR-4612 umgesetzt

SQ0 und SQ1 werden jetzt durch Adressbit 5 gewaehlt. Das Schreibfenster
`0xE0000000` bis `0xE3FFFFFF`, das getrennte Longword-Lesefenster ab
`0xFF001000`, QACR0/QACR1 und PREF verwenden denselben Queuevertrag. P4- und
QACR-Zugriffe pruefen Fenster, Breite, Ausrichtung und Queuegrenzen zentral.

Operand-Cache-RAM unterstuetzt explizite Byte-, Word- und
Longword-Little-Endian-Zugriffe mit Ausrichtungs- und Bereichspruefung. ICBI
invalidiert die ausgerichtete 32-Byte-Codezeile. OCBI, OCBP und OCBWB schlagen
sichtbar fehl, solange Cachetags, Dirty-Zustand und Write-back nicht modelliert
sind. Vertrag und gesammelte Gate-Testanforderungen stehen in
[`STORE_QUEUE_CACHE.md`](STORE_QUEUE_CACHE.md).

## KR-4613 umgesetzt

`Memory` ist jetzt die gemeinsame beobachtbare Commitgrenze fuer CPU-, FPU-,
DMA-, Store-Queue-, Copy- und Fallbackwrites. Lineares RAM wird vor der
Invalidierung auf Byteidentitaet geprueft; gebuendelte Writes melden einen
gemeinsamen Bereich. Generierte Stores koennen den Tracker dadurch nicht mehr
umgehen.

Geaenderte physische Bereiche invalidieren Trackerbloecke, Aliase und
eingehende Links und entfernen ueberlappende Eintraege der zentralen
Runtime-Blocktabelle. Zusaetzlich verweigern trackergebundene Tabellen stale
virtuelle, physische und Alias-Lookups, sodass Direktdispatch und Inline-Cache
kein invalidiertes Ziel ausfuehren koennen. Vertrag und die bei KR-4617/KR-4618
nachzuholenden Tests stehen in [`GUEST_WRITES.md`](GUEST_WRITES.md).

## KR-4614 umgesetzt

Kontrollfluss-Sites, CFG-Kanten, Funktionskandidaten und Jump Tables verwenden
die sieben typisierten Evidenzklassen von `ProvenComplete` bis `Unresolved`.
Hints bleiben unverbindliche Kandidaten und koennen die ungeloeste Front oder
den Export nicht verkleinern. Forced Overrides behalten den Runtime-Default.

Die rekursive Worklist unterscheidet Adresse, eingehenden Kontext,
Delay-Slot-Owner und Evidenz. Basic Blocks erzeugen Fallthrough nur an der
exakten Folgeadresse und paaren Owner/Slot nur gegenseitig bei `PC + 2`.
Unbekannte Caller tainten Kandidateneingaenge; Zielmengen aller Callkontexte
und vollstaendige Summaries endlicher indirekter Callees werden konservativ
vereinigt. Dynamische Herkunft verwendet einen begrenzten CFG-Backward-Slice
statt eines linearen Acht-Instruktions-Fensters.

Der genaue Vertrag und die bei KR-4617/KR-4618 nachzuholenden Regressionen
stehen in [`CONTROL_FLOW_SOUNDNESS.md`](CONTROL_FLOW_SOUNDNESS.md).

## KR-4615 umgesetzt

Die Runtime-Blocktabelle gibt keine Adressen verschiebbarer Vektorelemente
mehr heraus. Dispatch, Inline-Cache und generierter Port verwenden stabile
`RuntimeBlockHandle` aus Record-ID und Generation und loesen sie unmittelbar
vor dem Zugriff erneut auf. Erase und physische Invalidierung markieren
Records stale; eine dynamische Reaktivierung derselben Identitaet behaelt die
ID und liefert eine neue Generation.

Statische Bloecke werden vom Port sortiert in einem Bulk registriert und
danach versiegelt. Statische und dynamische virtuelle, physische und
Aliasindizes bleiben getrennt; aktive virtuelle Bereiche und physische Seiten
bilden die Mutationsindizes. Exakte Lookups sind logarithmisch und physische
Invalidierungen untersuchen nur beruehrte Seiten. Runtime-ABI 10 versioniert
die Handle- und Bulk-Registry-Schnittstelle.

Der genaue Vertrag und die bei KR-4617/KR-4618 nachzuholenden Last-, Alias-
und Mutationsregressionen stehen in
[`RUNTIME_BLOCK_REGISTRY.md`](RUNTIME_BLOCK_REGISTRY.md).

## KR-4616 umgesetzt

Gastzeitvertrag 1 verwendet den `EventScheduler` als einzige monotone
64-Bit-Zyklusuhr. Generierte Bloecke verbrauchen relative Instruktionskosten
ueber PlatformServices-ABI 5; Fallback-Safepoints, Blockaustritte und
Runtimemetriken lesen denselben Schedulerstand. Runtime-ABI 11 versioniert
diese Schnittstelle.

TMU, RTC und DMA verwenden ihre Hardwarefristen weiterhin auf diesem
Scheduler. GD-ROM besitzt keine eigene fortzuschaltende Uhr mehr: `submit`
plant seine Completion direkt. PVR-Renderstarts erhalten ebenfalls eine
Schedulerfrist, bevor der System-ASIC-Abschluss sichtbar wird. Resets
reaktivieren laufende Timer-/DMA-Fristen deterministisch und entfernen stale
GD-ROM-/PVR-Completions.

`KATANA_GUEST_CYCLE_BUDGET` wird von `game.exe` als positive 64-Bit-Zahl
validiert und direkt im Scheduler durchgesetzt. SLEEP prueft zuerst bereits
annehmbare Interrupts, springt andernfalls zum naechsten Ereignis und kann
weder ohne Wakeupquelle noch ueber das Gastbudget hinaus still weiterlaufen.
Der genaue Vertrag und die bei KR-4617/KR-4618 nachzuholenden
Reihenfolge-, Budget- und Cross-Engine-Regressionen stehen in
[`GUEST_TIMING.md`](GUEST_TIMING.md).

## KR-4617 umgesetzt

Die synthetische Core-Suite enthaelt nun unabhaengige Referenzvektoren fuer
alle P0-Vertraege: die Vier-Zustaende-Registerbankmatrix, PR vor Call-Delay
Slots, RTE-/Exceptionmetadaten, SQ-Bit-5-Adressierung, alle Gastwritequellen,
Aliasinvalidierung, generationsgesicherte Blockhandles und den gemeinsamen
Gastzeitvertrag. Erfolgs-, Grenz- und sichtbare Fehlerfaelle sind getrennt.

CFG-Regressionen pruefen Hint- und Forced-Override-Evidenz, erhaltene
Runtime-Defaults, Adressluecken, partielle Sites, normale und Delay-Slot-
Kontexte, unbekannte Caller sowie exhaustive kleine Conditional-Graphen.
Fixpunktterminierung wird nur noch gegen ein oberes Budget geprueft; interne
Iterationszahlen sind kein Testvertrag mehr.

Die Tests passen zu Runtime-ABI 11, PlatformServices-ABI 5 und dem stabilen
Blockhandle-API. Gemaess Gate-Arbeitsmodell wurden bei KR-4617 weder
Konfiguration noch Build oder Tests gestartet. Die Debug-/RelWithDebInfo-
Ausfuehrung und der Konfigurationsvergleich folgen gesammelt in KR-4618.

## KR-4618 umgesetzt

Das Core-Korrektheitsgate erstellt `build-current` fuer Quality-Debug und
RelWithDebInfo jeweils frisch. Quality-Debug bestand 171 von 171 Tests mit
MSVC-ASan und statischer Analyse; RelWithDebInfo bestand 170 von 170 Tests.
Beide Profile besitzen dasselbe Inventar aus 170 Core-Regressionen. Der
zusaetzliche Debug-Test prueft gezielt die ausgelieferte MSVC-ASan-Runtime.

Format-, Qualitaetsvertrags- und Referenz-/Lizenzaudit sind erfolgreich. Die
exakten Referenzvektoren bestanden in beiden Konfigurationen; private
Retaildaten wurden nicht verwendet. Der maschinenlesbare Gatebericht wurde
unter `build-current/reports/core-correctness-gate.json` erzeugt. Damit ist die
P0-Core-Korrektheitsstufe abgeschlossen und KR-4621 beginnt die Performance-
und Buildstufe.

## Naechste Reihenfolge

```text
v0.47:
KR-4611 bis KR-4618
-> KR-4621 bis KR-4624 -> KR-4625
-> KR-4715 -> KR-4716 und KR-4717 -> KR-4718
-> KR-4719 -> KR-4703 -> KR-4704 -> KR-4705

v0.48:
Runtime-SDK, gemeinsamer Export, Harness, Controller und GUI
-> KR-4804 -> KR-4805

v0.49:
KR-4911 -> KR-4912 -> KR-4913
-> KR-4914 und KR-4915 -> KR-4916
-> KR-4901 bis KR-4903 -> KR-4904 -> KR-4905

v0.50:
KR-4999 -> KR-5000
```

## Unveraenderte Schutzgrenze

Vor Abschluss von v0.47 darf Sonic Adventure lokal analysiert und bis zur
privaten `game.exe` gebaut werden. Der Build-only-Harness darf sie technisch
nicht starten.

Der erste Sonic-Prozessstart gehoert zu v0.49. Deterministische Probes und
interaktive Sitzungen bleiben getrennt. Die interaktive Sitzung darf fuer
lokales Debugging und Controllererkundung verwendet werden, aber niemals als
Gateevidenz.

## Bestehende Funktionssicherung

- keine bestehende korrekte Regression wird entfernt oder abgeschwaecht
- falsche historische Erwartungen werden durch unabhaengige Vektoren ersetzt
- jeder Fastpath behaelt einen deaktivierbaren Referenzpfad
- Debug und RelWithDebInfo muessen dieselben Gastresultate liefern
- Performance darf keine Beweis-, Guard- oder Runtime-Default-Semantik aendern
- Retailbefunde werden nur als allgemeine, verteilbare Regression umgesetzt
