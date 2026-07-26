# JSON-Berichte

KatanaRecomp-Berichte tragen neben ihrem fachlichen `schema` drei gemeinsame
Felder:

- `report_version`: Version des gemeinsamen Berichtvertrags, aktuell `1`
- `report_type`: stabiler Typname wie `ir`, `control-flow` oder `phase6-gate`
- `status`: maschinenlesbarer Abschlussstatus, bei erfolgreichen Berichten
  `success`

Fachfelder bleiben im jeweiligen Schema definiert. Neue optionale Felder
duerfen hinzugefuegt werden; eine inkompatible Bedeutungs- oder Typaenderung
braucht eine neue fachliche Schema-Kennung. Listen, deren Reihenfolge keine
Gastsemantik traegt, werden vor der Ausgabe nach Gastadresse und Typ sortiert.

`katana-recomp analyze-json <manifest> [overrides]` erzeugt den lokalen
Detailbericht `katana-control-flow-v3`. Version 3 fuehrt disjunkte
Vollstaendigkeitszustaende und typisierte Herkunftsklassen ein. Der
Anwendungsworkflow erzeugt daneben `katana-control-flow-frontier-v1` ohne
Gastadressen, Symbole oder Hostpfade. `katana-recomp ir-json ...` behaelt
`katana-ir-v2`. Historische Phase-6-Berichte verwenden
`katana-phase6-gate-v1` und behalten ihre Messfelder auf der obersten Ebene.

Die Funktionswertanalyse berichtet in der Kontrollfluss-Summary
`function_iteration_budget` und `function_budget_exhausted`. Jede
Registersummary trennt `complete` von `guarded`; nur `complete=true` darf eine
vollstaendige endliche Zielmenge begruenden.

Funktionssummaries tragen zusaetzlich `memory_complete` und die sortierte Liste
`memory_values`. Jeder Eintrag nennt Adresse, `complete`, `guarded` und seine
endliche Wertemenge. Diese lokalen Detailfelder duerfen Gastadressen enthalten
und werden deshalb nicht in den adressfreien Frontierbericht uebernommen.

Berichte enthalten keine Hostzeit als Determinismusquelle. Absolute lokale
Pfade, Firmwarebytes und Flash-Rohdaten sind keine portablen Berichtfelder;
spaetere Diagnosebefehle muessen solche Inhalte standardmaessig redigieren.

`katana-alpha-isa`/`alpha-isa` Vertragsversion 1 wird mit
`katana-recomp isa-report --json` erzeugt. Der Bericht zaehlt den gesamten
16-Bit-Opcoderaum, ordnet jede decodierte Instruktionsart einer Familie zu und
meldet Decoder, IR, Backend und Runtime getrennt als `supported`, `restricted`
oder `rejected`. Semantikvertrag, konkrete Einschraenkung und
Testanforderung sind Pflichtfelder; eine reine Decoderzaehlung ist keine
Faehigkeitsbehauptung.

Das separate Feld `external_evidence` ist ohne einen explizit uebergebenen
externen Bericht `null`. Mit
`katana-recomp isa-report --json --external-evidence
<katana-sh4-sst-conformance.json>` enthaelt es ausschliesslich redigierte
SingleStepTests-Evidence: Quelle, Katana- und Corpus-Commit, Corpus-Manifesthash,
Backendprofil und -version, Runtime-/Backend-ABI, Scope, `complete_scope`,
`expected_scope_vectors`, Speicherprofil, FPU-Vergleichsmodus, Zaehler fuer
Gesamtmenge, anwendbar, Pass, Fail und nicht anwendbar sowie die Waiveranzahl.
Zulaessige Scopes sind ausschliesslich `smoke` mit 65 erwarteten Vektoren und
`full` mit 116500 erwarteten Vektoren. Dateilisten,
Gegenbeispiele, Compilerpfade und andere lokale Pfade aus dem Quellbericht
werden nicht uebernommen. Der deklarierte Vier-Schichten-Status bleibt davon
getrennt und wird durch externe Evidence weder angehoben noch abgesenkt.

`stale=true` nennt stabile Gruende, wenn die lokale Source-Identity nicht durch
einen sauberen Git-Checkout belegt ist (`untrusted-build-source`), Katana-Commit,
gepinnter Corpus-Commit
oder -Manifesthash, Runtime-ABI, Backend-ABI, das externe
`external-conformance`-Backendprofil oder dessen Profilversion nicht zum
aktuellen Buildvertrag passen. Zusaetzlich wird `incomplete-scope` gemeldet,
wenn `selection.complete_scope=false` ist. Vollstaendige Evidence verlangt
exakt den Scope-Nenner, `file`, `case`, `opcode`, `family` und `shard` auf
`null` sowie `fail_fast=false`; gefilterte, geshardete, abgebrochene oder
anderweitig gekuerzte Laeufe bleiben damit sichtbar, gelten aber nie als
frische Scope-Evidence. Malformed JSON, eine fremde Schema-/Berichtsversion
oder ein anderer Scope, doppelte Schluessel, falsche Feldtypen, ein falscher
Scope-Nenner und widerspruechliche Zaehler werden als ungueltige Eingabe
abgelehnt und nicht als Evidence dargestellt.

`SH4_ALPHA_ISA.md` wird von beiden Befehlen niemals automatisch veraendert.
Eine Dokumentationsaktualisierung ist ein eigener, expliziter Review-Schritt:
Zuerst wird ein vollstaendiger, bestandener und nicht veralteter SST-Bericht
erzeugt, danach der ISA-JSON-Bericht mit `--external-evidence` erzeugt und erst
dieses gepruefte Artefakt darf manuell in die Dokumentation uebernommen werden.

## Anwendungsjob und Buildplan

`katana-application-job` Version 7 unterscheidet die Endzustaende `completed`,
`partial`, `failed` und `cancelled`. `partial` ist kein erfolgreicher Build:
Analyseartefakte bleiben nutzbar, Codegen und Hostkompilierung werden jedoch
unterdrueckt. Das Feld `analysis` enthaelt committed ausfuehrbare Bytes,
analysierte und nicht analysierte ausfuehrbare Bytes, Instruktions-/
Funktionszahlen, vollstaendige und partielle Guards, reine Laufzeit- und
ungeloeste Kontrollflussstellen, statisch verpflichtende unbekannte
Instruktionen, reine Kandidatendiagnosen, erreichbare Abbruchkanten und
`control_flow_complete`. `unknown_instructions` zaehlt nur Diagnosen, deren
Evidenz statischen Decode verlangt; `candidate_unknown_instructions` haelt
nicht bindende Guard-/Hint-Diagnosen separat sichtbar. Vollstaendig bedeutet
exakt: null statisch verpflichtende unbekannte Instruktionen, null partielle
und ungeloeste Kontrollflussstellen, null nicht analysierte committed
ausfuehrbare Bytes und null erreichbare Abbruchkanten. Reine Laufzeitstellen
sind seit KR-4718 vollstaendig abgedeckt, wenn ihre IR-Klasse den validierenden
Runtime-only-Dispatcher erzwingt. Es gibt keine heuristische Prozentgrenze.

`katana-indirect-dispatch-v1` berichtet gesaettigte Gesamt- und Runtime-only-
Zaehler fuer Hits, Misses und kontrollierte Fallbacks. `first_error` ist `null`
oder enthaelt Fehlerklasse, Dispatchklasse, Callsite und Ziel des ersten Misses.
Der aktuelle Port stoppt bei jedem Miss; seine Fallbackzaehler bleiben deshalb
null. Vor dem Fehlerexit schreibt er den Exception-Snapshot als
`KATANA_RUNTIME_DISPATCH_ERROR`-JSON-Zeile. Ein spaeterer kontrollierter
Fallback muss vor dem Fortsetzen explizit gezaehlt werden.

`failure_category` trennt `none`, `input-output`, `processing`,
`code-generation`, `build` und `internal`. Die Workflow-CLI bildet diese
Kategorien auf ihre bestehenden stabilen Exitcodes ab. `partial` und
`cancelled` sind keine versteckten Exceptions; ihr Feld bleibt `none`, der
Prozessstatus ist dennoch ungleich null, solange der Job nicht `completed` ist.

`katana-build-plan` Version 7 spiegelt denselben Zustand und dieselben Metriken.
Bei `status=partial` ist `host_compilation=false`; nur `status=built` darf eine
veroeffentlichte `game.exe` behaupten. Beide Berichte tragen `tool_version` aus
derselben CMake-Definition wie CLI, GUI und Portprovenienz.

`katana-private-retail-build` Version 1 ist der externe private
Build-only-Bericht fuer Configversion 2. Er enthaelt ausschliesslich
aggregierte Analysezaehler, Buildanzahlen, Boolwerte fuer Identitaet,
Reproduzierbarkeit, aktuelles Executable und No-run sowie eine allgemeine
Fehlerklasse. Projektidentitaet, Datei- oder Eingabehashes, Gastadressen,
Tracknamen, private Pfade und Rohlogs sind verboten. Der Bericht wird neben dem
Ziel vorbereitet, allowlist-geprueft und atomar ersetzt.
Der private Runner ermittelt Runtime-ABI und Portprojektvertrag strikt aus der
kanonischen `cmake/KatanaVersions.cmake`. Fehlende, doppelte, malformed,
nicht-positive oder ueberlaufende Deklarationen werden ebenso abgelehnt wie
als JSON-String oder Gleitkommazahl eingeschleuste Vertragswerte. Der
Anwendungskontrakt bleibt Version 7.

`katana-persistent-image-v1` und `katana-dreamcast-storage-v1` berichten den
lokalen Arbeitskopienzustand ohne Pfade, Hashes oder Nutzdaten.
`katana-host-pacing-v1` berichtet Wait-/Late-Zaehler und hoechstens den ersten
typisierten Fehler. `HostPacingException` verwendet fuer den Portexit dasselbe
Schema mit Fehlerklasse und Gastzyklus. Diese lokalen Diagnosen sind keine
Gastfortschritts- oder Kompatibilitaetsaussage.

## Live-Jobereignisse

`katana-job-event` Version 1 ist der gemeinsame geordnete Observerstrom von CLI
und GUI. `sequence` beginnt je Job bei null. `overall_percent` ist monoton;
`stage` und `step_status` benennen den aktiven Einzelschritt. `step_current` und
`step_total` sind entweder gemeinsam gesetzt oder gemeinsam `null`. Ein
unbekannter Umfang bleibt dadurch unbestimmt. `timestamp_ms` und `elapsed_ms`
geben Ereigniszeit und Joblaufzeit an. `log_chunk` enthaelt ausschliesslich neu
beobachtete, bereits redigierte Hostausgabe; Diagnosen stehen typisiert in
`diagnostic`. Fehler und Abbruch verwenden den aktiven Schritt statt eines
informationsarmen generischen `failed`-Schritts.

## Systemreplay

`katana-system-replay` verwendet `replay_version=8` und weist den
`storage_mode` explizit als `exact-events` oder `digest-stream` aus.
`event_count`, `retained_event_count`, `summarized_event_count` und
`exact_event_stream` trennen Gesamtstrom, gespeicherte Zeugen und
Zusammenfassung. Die konfigurierbare Aufbewahrungskapazitaet betraegt
standardmaessig 4.096 und maximal 65.536 Ereigniszeugen; ein portabler
Ereigniscode ist auf 64 Zeichen begrenzt.

Im exakten Modus markiert `record()` einen Kapazitaetsueberlauf genau einmal
als Drop. Ein von `try_record()` an einem unversiegelten Log abgewiesener
Best-effort-Aufnahmeversuch erhoeht den Dropzaehler ebenfalls genau einmal; ein
versiegelter Log bleibt unveraendert. Ein Log mit Drop darf weder versiegelt
noch abgespielt werden. Im Digestmodus sind Ereignisse nach dem Zeugenpraefix
dagegen `summarized`, nicht `dropped`: Sie fliessen weiter in Sequenzpruefung,
Coverage, Klassenzaehler und den geordneten FNV-Digest ein. Der ungekeyte
Digest dient der deterministischen Reproduzierbarkeit und nicht der
Authentisierung. Schema 8 bindet zusaetzlich UTLB-, Fault-Herkunfts-,
Exceptiongenerations- und Attempted-/Retired-/Cycle-Zustand in den finalen
Gastzustandshash. `expected_observed_coverage`, `hooks_complete` und
`observed_complete` trennen verfuegbare Pflichthooks von tatsaechlich
beobachteten erwartbar positiven Klassen. Gesamtzahl, beobachtete Coverage
und alle exakten zwoelf Klassenzaehler gehen in die finale Digestdomain ein.
Diese technische Steuerflussevidenz enthaelt keine Spieldaten.

`katana.runtime-probe` verwendet Runtime-Probe-Schema 5 und Device-Schema 5.
Sein Replayobjekt ergaenzt `retention_capacity` und dieselben Gesamt-, Behalte-,
Zusammenfassungs-, Exaktheits-, Coverage- und Klassenzaehlerfelder. Ein als
vollstaendig ausgegebener Digeststrom muss dropfrei sowie Hook- und
Beobachtungs-vollstaendig sein; er behauptet bei zusammengefassten Ereignissen
nicht `exact_event_stream=true`.

Der exakte Ereignisvergleich und die kanonischen Hashes bleiben bytegenau. Im
Standardmodus `serialize_values=false` werden dagegen `code`, `address`,
`value`, `detail`, `auxiliary`, `event_hash` und
`final_guest_state_hash` als `null` ausgegeben. Die Flags
`codes_redacted`, `addresses_redacted`, `numeric_payloads_redacted` und
`hashes_redacted` machen diese Grenze maschinenlesbar. Exakte Werte duerfen
nur ueber ein ausdrueckliches lokales Opt-in serialisiert werden.

## Dreamcast-Hardwareaudit

Ein einzelner Bericht verwendet `katana.hardware-audit.v4`; die
Mehrquellen-Huelle bleibt `katana.hardware-audit-set.v1`. Version 4 praezisiert
die zuvor als `initial_boot_executable` verstandene Scope-Semantik inkompatibel
zu `executable_image` beziehungsweise `native_disc_aot_boot_graph` und traegt
daher bewusst eine neue fachliche Schemaversion. Sie erkennt
skalierbar ueber Dominatoren echte Natural Loops und klassifiziert sie als
`counter`, `ram_poll`, `mmio_poll`, `mixed` oder `unknown`. Jeder Loop traegt
Backedge-, Block-, Counter- und Zugriffsevidenz; Zugriffe unterscheiden
linearen Speicher, Geraeteapertur, Runtimeunterstuetzung und `guards_loop`.
Das oberste Feld `scope` lautet fuer ein einzelnes geladenes Executable-Image
`executable_image`. Die Disc-CLI setzt explizit
`native_disc_aot_boot_graph`, weil ihr Bericht IP.BIN und BootExecutable
gemeinsam umfasst.

Die Summen `known_gap_addresses`, `rejected_addresses` und
`unmapped_addresses` beschreiben bestaetigte beziehungsweise eindeutig
abgelehnte Luecken. Nullwerte in diesen Feldern bedeuten nur, dass im
betrachteten statischen Scope keine solche Luecke belegt wurde. Sie sind kein
Beweis fuer einen vollstaendig aufgeloesten Hardwarezugriffsgraphen.
`unresolved_memory_access_sites`, `partial_addresses`, als `unknown`
klassifizierte Loops und `unresolved_poll_guard_loops` bleiben deshalb
eigenstaendig sichtbar. Sobald eine dieser Restmengen ungleich null ist, darf
der Bericht nicht als vollstaendiger Hardwarebeweis zusammengefasst werden.

Area-3-Haupt-RAM-Spiegel werden auf dieselbe physische Herkunft kanonisiert.
OCRAM bleibt eine Geraeteapertur und erhaelt deshalb keine
`linear_memory=true`-Behauptung. Der statische Zugriffskatalog umfasst
GBR-MOVs, `TST.B` als Read, `AND.B`/`XOR.B`/`OR.B` sowie `TAS.B` als RMW,
FMOV mit konservativer FPSCR.SZ-Adressunion, PC-relative `MOV.W`/`MOV.L`,
`STC.L`/`LDC.L` und beide Reads von `MAC.W`/`MAC.L`. Ist nur eine MAC-Basis
statisch bekannt, bleibt deren Zugriff erhalten und die andere Site
unaufgeloest. Predecrement-Adressen folgen dem SH-4-32-Bit-Wraparound.

Ein aufgeloester Read, der die Schleifenbedingung speist, traegt
`guards_loop=true`. `unresolved_guard_read_instruction_addresses` enthaelt
dagegen entweder einen adressseitig unaufgeloesten Guard-Read oder einen
konservativen Lesekandidaten, dessen Condition-Domaene noch nicht vollstaendig
modelliert ist. Das betrifft aktuell insbesondere FMOV-/FCMP-Pfade mit
unbewiesenem FPSCR.PR/SZ-, FR/XF-, FPUL- oder Vektorzustand. Sie bleiben
`unknown` und erhalten keinen erfundenen `guards_loop`-Beweis. Die oberste
Summe `unresolved_poll_guard_loops` zaehlt beide Klassen. T-neutrale
Instruktionen und ein eindeutiger Vorgaenger erhalten die Integerprovenienz,
echte T-Schreiber und Merges beenden sie konservativ.

`disc-audit` und `disc-audit-set` behalten `--fail-on-gap` fuer eindeutige
Luecken. `--strict` scheitert zusaetzlich an partiellen Hardwareadressen oder
einem von `unresolved_poll_guard_loops` gezaehlten unaufgeloesten oder
konservativen Poll-/Guard-Fall.
Eine reine Unknown-Schleife ohne Read-/Guard-Evidenz ist kein solcher
Strict-Fehler. Ungeklaerte Definitionen oder Vorgaenger, nicht gemappte
P4-Zugriffe und rootlose SCCs werden konservativ nicht als belegter
Hardware-Waitloop ausgegeben. Delay-Slot-Doppelkontext, nichtdominierende
Schleifenkandidaten und eine synthetische 4.096-Block-Skalierungsfixture sichern
diese Grenze. Lokale Detailberichte duerfen Gastadressen enthalten;
oeffentliche Aggregate und Fehlerpakete bleiben adress- und inhaltsredigiert.
Damit sind `--fail-on-gap` und `--strict` Ablehnungspolicies, keine positiven
Vollstaendigkeitszertifikate. Insbesondere kann ein policy-gruener Bericht
weiterhin dynamische Speicherstellen oder reine Unknown-Loops ausweisen; jede
Statusdarstellung muss diese Restmengen neben dem Gapstatus nennen.
