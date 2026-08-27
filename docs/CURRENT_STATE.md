# Aktueller Projektstand

Stand: 27. August 2026. Diese Datei enthaelt nur die aktuelle
Entwicklungswahrheit. Historische Runs, ABI-Zwischenstaende und erledigte
Tasks stehen in `STATUS.md`, `TASKS.md`, `ROADMAP.md` und Git.

## Ziel und Releasegate

KatanaRecomp erzeugt statisch rekompilierte native PC-Ports. Der Produktpfad
enthaelt keinen Emulator, Interpreter, JIT, Runtime-Decoder, PVR-/TA-Replay
oder geratenen Kontrollfluss.

Der aktuelle Stand ist Pre-Alpha `v0.49.2`. `v0.5.0` bleibt gesperrt, bis der
private Sonic-Adventure-PAL-Integrationstest vollstaendig ueber den nativen
PC-Pfad spielbar ist. Ein Menue- oder einzelner Gameplay-Meilenstein ist noch
keine Releaseabnahme.

## Erreichter Produktstand

- Intro, Hauptmenue, Optionen und Character Select sind erreichbar.
- Die idle-getriggerte, selbstlaufende Gameplay-Demo laeuft teilweise.
- Spielersteuerbares Gameplay und Sonics Story-Intro sind noch nicht
  erreichbar. Der Port hat damit einen begrenzten voraufgezeichneten
  Gameplaypfad, aber noch keinen vollstaendigen Story-/Gameplay-Bring-up.
- Der zuvor gemeinsame Grafik-Contract-Stop ist geschlossen.
- Der erreichte Gameplaypfad besitzt weiterhin deutlich sichtbare
  Grafikfehler und deckt neue Callback-/AOT- sowie Providerauftraege auf.
- Automatisierte Produktlaeufe laufen standardmaessig stumm und unsichtbar.
- Schwere Dauertelemetrie ist aus dem Standardprofil ausgeschlossen; sie
  verlangsamt den realen Lauf zu stark und wird nur gezielt offline eingesetzt.

Private Titeladressen, Disassemblybytes und Retailidentitaeten bleiben im
externen Spielprojekt und seinen Diagnoseartefakten.

Die Meilensteinskala ist:

| ID | Produktcheckpoint | Stand |
| --- | --- | --- |
| M0 | Intro | erreicht |
| M1 | Hauptmenue | erreicht |
| M2 | Character Select | erreicht |
| M3 | Idle Gameplay Demo | teilweise erreicht, aktueller Stand |
| M4 | Sonic Story Intro startet | offen |
| M5 | Sonic Story Intro vollstaendig | offen |
| M6 | Station Square steuerbar | offen |
| M7 | Emerald Coast Load | offen |
| M8 | Emerald Coast steuerbar | offen |

## Aktiver Entwicklungsweg

Verbindlich ist
[`NATIVE_BRINGUP_WORKFLOW.md`](NATIVE_BRINGUP_WORKFLOW.md):

1. Eine neue Analyse erzeugt die autoritative World und den Katana-Taskpool;
   die feste read-only Fleet klassifiziert dessen erreichbare Frontiers.
2. Parallel laufen sechs gebundene Replays bis zu ihrem jeweils ersten Stop.
   Jeder Stop wird als K1 bis K5 klassifiziert und benennt die erste fehlende
   Spielkenntnis, nicht nur eine Crashsignatur.
3. Haupttask und Fleet gruppieren Replay- und Frontierbefunde nach gemeinsamer
   Callback-, Overlay-, AOT-, Provider- oder Semantikursache und priorisieren
   Story-/Gameplay-Reichweite sowie Multi-Close.
4. AOT-/Closure-wirksame Cluster kehren nach der gebuendelten Implementierung
   genau einmal in die grosse Analyse zurueck. Nur vollstaendig bekannte
   Host-, Adapter- oder Praesentationsfehler bleiben im kleinen Zyklus mit
   demselben Pack.

Evidence folgt
`Observed -> Candidate -> Proven | RuntimeContract -> Strict Product`.
Runtime-Witnesses und Disassembly erzeugen gerichtete Beweisauftraege; nur
reviewte statische beziehungsweise identity-bound Proofs oder ein validierter
RuntimeContract duerfen Strict schliessen.
Fuer den Bring-up darf ein reviewter `Candidate` nach unabhaengiger exakter
Execution-Safety-Pruefung laufen, ohne dadurch zum Proof oder Produktvertrag
zu werden.

## Implementierter Bring-up-Unterbau

- versioniertes, identity-bound Native-Bring-up-Allowlist-Artefakt;
- sicherer Bring-up-Dispatch ausschliesslich ueber aktive versiegelte
  residente `primary`-Static-AOT-Bloecke, ohne rohe Hostfunktionszeiger oder
  Tabellenmutation;
- stabile Trennung von AOT-Pack und Runtime-/Adapterbuild;
- kleiner begrenzter Dispatch-Observation-Puffer sowie erzeugter Promotion-
  und executable-Allowlist-Report;
- private, exakt byte- und funktionsgebundene Disassembly-Evidence fuer
  titelbezogene Ziele; generische Regeln nur bei address-agnostischem Muster.

Noch nicht als vollstaendiger Unterbau implementiert sind:

- ModuleInstance-/Lifecycle-Bindung fuer PRS-, Overlay- und Loaded-AOT-
  Bring-up-Dispatch; solche Records bleiben in v1 nicht executable;
- ein persistierbarer Runtime-Witness-Ring mit Replay-, Build-, Provider-,
  Last-Writer- und Callstack-Korrelation;
- Provider-level Deterministic Replay, das aufgezeichnete Hostantworten und
  Completion-Reihenfolgen wieder einspeist;
- automatische First-Divergence-Lokalisierung und Evidence-Promotion. Der
  vorhandene Replaypfad verifiziert Ereignisstrom und finalen State-Hash,
  ersetzt diese Funktionen aber noch nicht.

## Naechste Gates

1. Die sechs aktuellen Stops als K1 bis K5 und nach Story-/Gameplay-Naehe
   klassifizieren; Loaded-AOT-/Overlay-/Callback-Evidence bleibt fail-closed.
2. Nach der autoritativen Analyse sofort den neuen Pool an die bestehende
   Fleet geben und Replay-/Fleetbefunde zu wenigen gemeinsamen
   Knowledge-Gap-Clustern reconciliieren.
3. Das hoechstwertige Cluster vollstaendig schliessen. Eine kleine
   Performanceverbesserung darf nur im ohnehin bearbeiteten Pfad mitlaufen;
   Grafik wird nur bei Bring-up-Relevanz oder echtem Multi-Close aufgenommen.
4. Nach genau einem Export-/Produktbuild dieselben sechs Replays wiederholen.
   Der naechste Produktmeilenstein ist M4: Sonics Story-Intro startet.
5. Strict wird weiterhin nur auf ausdrueckliche Nutzeranweisung gebaut.

## Quellenhierarchie

Bei Widerspruechen gilt:

1. aktuelle Nutzeranweisung;
2. `AGENTS.md`;
3. `NATIVE_BRINGUP_WORKFLOW.md` und
   `NATIVE_PORT_PRODUCT_CONTRACT.md`;
4. diese aktuelle Zustandsseite;
5. Roadmap, Taskkatalog, Handoff und historische Statusabschnitte.
