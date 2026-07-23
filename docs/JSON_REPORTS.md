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

## Anwendungsjob und Buildplan

`katana-application-job` Version 7 unterscheidet die Endzustaende `completed`,
`partial`, `failed` und `cancelled`. `partial` ist kein erfolgreicher Build:
Analyseartefakte bleiben nutzbar, Codegen und Hostkompilierung werden jedoch
unterdrueckt. Das Feld `analysis` enthaelt committed ausfuehrbare Bytes,
analysierte und nicht analysierte ausfuehrbare Bytes, Instruktions-/
Funktionszahlen, vollstaendige und partielle Guards, reine Laufzeit- und
ungeloeste Kontrollflussstellen, unbekannte Instruktionen, erreichbare
Abbruchkanten und `control_flow_complete`. Vollstaendig bedeutet exakt: null
unbekannte Instruktionen, null partielle und ungeloeste Kontrollflussstellen,
null nicht analysierte committed ausfuehrbare Bytes und null erreichbare
Abbruchkanten. Reine Laufzeitstellen sind seit KR-4718 vollstaendig abgedeckt,
wenn ihre IR-Klasse den validierenden Runtime-only-Dispatcher erzwingt. Es gibt
keine heuristische Prozentgrenze.

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

`katana-system-replay` verwendet `replay_version=2`. Die konfigurierbare
Kapazitaet betraegt standardmaessig 4.096 und maximal 65.536 Ereignisse; ein
portabler Ereigniscode ist auf 64 Zeichen begrenzt. `record()` markiert einen
Kapazitaetsueberlauf genau einmal. Ein von `try_record()` an einem
unversiegelten Log abgewiesener Best-effort-Aufnahmeversuch erhoeht den
Dropzaehler ebenfalls genau einmal; ein versiegelter Log bleibt unveraendert.
Ein Log mit Drop darf weder versiegelt noch von `DeterministicSystemReplay`
abgespielt werden; dasselbe gilt fuer einen unversiegelten Log.

Der interne Ereignisvergleich und seine Hashes bleiben bytegenau. Im
Standardmodus `serialize_values=false` werden dagegen `code`, `address`,
`value`, `detail`, `auxiliary`, `event_hash` und
`final_guest_state_hash` als `null` ausgegeben. Die Flags
`codes_redacted`, `addresses_redacted`, `numeric_payloads_redacted` und
`hashes_redacted` machen diese Grenze maschinenlesbar. Exakte Werte duerfen
nur ueber ein ausdrueckliches lokales Opt-in serialisiert werden.

## Dreamcast-Hardwareaudit

Ein einzelner Bericht verwendet `katana.hardware-audit.v3`; die
Mehrquellen-Huelle bleibt `katana.hardware-audit-set.v1`. Version 3 erkennt
skalierbar ueber Dominatoren echte Natural Loops und klassifiziert sie als
`counter`, `ram_poll`, `mmio_poll`, `mixed` oder `unknown`. Jeder Loop traegt
Backedge-, Block-, Counter- und Zugriffsevidenz; Zugriffe unterscheiden
linearen Speicher, Geraeteapertur, Runtimeunterstuetzung und `guards_loop`.

Area-3-Haupt-RAM-Spiegel werden auf dieselbe physische Herkunft kanonisiert.
Ungeklaerte Definitionen oder Vorgaenger, nicht gemappte P4-Zugriffe und
rootlose SCCs werden konservativ nicht als belegter Hardware-Waitloop
ausgegeben. Delay-Slot-Doppelkontext, nichtdominierende Schleifenkandidaten und
eine synthetische 4.096-Block-Skalierungsfixture sichern diese Grenze. Lokale
Detailberichte duerfen Gastadressen enthalten; oeffentliche Aggregate und
Fehlerpakete bleiben adress- und inhaltsredigiert.
