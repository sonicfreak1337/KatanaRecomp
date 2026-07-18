# Runtime-Blockregistry

Dieses Dokument beschreibt den mit `KR-4615` eingefuehrten Vertrag. Die
ausfuehrbaren Last- und Mutationsregressionen werden gemaess
`docs/CODEX_HANDOFF.md` gesammelt bei `KR-4617` implementiert und im frischen
Gate-Build von `KR-4618` ausgefuehrt.

## Handle- und Lebenszeitvertrag

Ein `RuntimeBlockHandle` besteht aus einer tabellenlokalen Record-ID und einer
Generation. Aufrufer speichern nur diesen Wert und rufen unmittelbar vor dem
Zugriff `RuntimeBlockTable::resolve` auf. Eine Aufloesung gelingt nur, wenn

- ID und Generation zum Record passen,
- der Record aktiv ist und
- ein gebundener `ExecutableCodeTracker` die stabile Blockidentitaet weiterhin
  als dispatchbar meldet.

Erase und physische Invalidierung deaktivieren einen Record und erhoehen seine
Generation. Ein zuvor ausgegebenes Handle kann danach nicht versehentlich auf
einen anderen oder reaktivierten Block zeigen. Dynamische Registrierung einer
deaktivierten identischen Runtime-Identitaet reaktiviert denselben Record und
gibt dessen aktuelle Generation aus. Statische Records werden nicht ueber die
dynamische API reaktiviert.

`stable_runtime_block_identity` bleibt inhaltlich unveraendert. Handles sind
nur Prozess- und Tabellenlokatoren und ersetzen keine portable Identitaet in
Metadaten, Cacheeintraegen oder Diagnosen.

## Indizes und Komplexitaet

Der generierte Port sammelt statische Bloecke, sortiert sie deterministisch,
registriert sie mit `register_static_bulk` und versiegelt die statische
Registry. Einzelregistrierung bleibt fuer kleine Runtime-Szenarien bis zur
Versiegelung zulaessig. Die Tabelle fuehrt getrennte geordnete Indizes fuer

- statische und dynamische virtuelle Starts samt Variante,
- statische und dynamische physische Urspruenge samt Variante und virtuellem
  Alias sowie
- statische und dynamische Aliasmengen je physischem Ursprung.

Exakte virtuelle und physische Lookups sind `O(log N)`. Ein gemeinsamer
geordneter Index aktiver virtueller Bereiche prueft Ueberlappungen bei der
Registrierung. Ein Seitenindex aktiver physischer Bereiche begrenzt
Invalidierungen auf Records, die Seiten des geschriebenen Bereichs beruehren.
Stale statische Eintraege koennen im unveraenderlichen Lookupindex verbleiben,
werden aber durch den Recordstatus und die Handle-Generation niemals geliefert.

Bei mehreren physischen Aliasen waehlt der Lookup weiterhin deterministisch
nach der stabilen Blocksortierung. Aliaslisten werden in derselben Ordnung
ausgegeben, unabhaengig davon, ob ihre Records statisch oder dynamisch sind.

## Nachzuholende Regressionen

`KR-4617` muss mindestens folgende unabhaengige Faelle implementieren:

1. 100.000 nicht ueberlappende statische Bloecke werden als ein Bulk
   registriert; virtuelle und physische Treffer sowie Misses werden gegen eine
   unabhaengige Referenzabbildung geprueft.
2. Das Lastprofil misst Registrierung und Lookup in festen, dokumentierten
   Budgets und weist kein lineares Lookupwachstum aus.
3. Ein Dispatchloop behaelt Handles ueber wiederholte dynamische
   Registrierungen hinweg; kein bestaetigtes Handle wird durch Containerwachstum
   entwertet.
4. Erase und physische Invalidierung machen alte Handles stale. Die
   Reaktivierung derselben dynamischen Identitaet behaelt die Record-ID, gibt
   eine neue Generation aus und laesst das alte Handle stale.
5. Varianten, P1-/P2-Aliase und gemischte statische/dynamische physische Aliase
   liefern deterministische Treffer und Listen.
6. Ueberlappung, Nullgroesse, fehlende Backendfunktion, leere Provenienz,
   Adressraumueberlauf und statische Registrierung nach der Versiegelung werden
   sichtbar abgelehnt.
7. Trackerinvalidierung zwischen Lookup und Resolve verhindert die Ausfuehrung
   auch dann, wenn der Tabellenrecord noch nicht explizit geloescht wurde.

`KR-4618` erstellt danach den einzigen frischen Gate-Build und fuehrt diese
Regressionen zusammen mit der vollstaendigen Core-Suite aus.
