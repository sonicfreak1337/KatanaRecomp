# Aktueller Projektstand

Stand: 26. August 2026. Diese Datei enthaelt nur die aktuelle
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

- Der private native Integrationsport hat Gameplay erreicht.
- Der zuvor gemeinsame Grafik-Contract-Stop ist geschlossen.
- Der erreichte Gameplaypfad besitzt weiterhin deutlich sichtbare
  Grafikfehler und deckt neue Callback-/AOT- sowie Providerauftraege auf.
- Automatisierte Produktlaeufe laufen standardmaessig stumm und unsichtbar.
- Schwere Dauertelemetrie ist aus dem Standardprofil ausgeschlossen; sie
  verlangsamt den realen Lauf zu stark und wird nur gezielt offline eingesetzt.

Private Titeladressen, Disassemblybytes und Retailidentitaeten bleiben im
externen Spielprojekt und seinen Diagnoseartefakten.

## Aktiver Entwicklungsweg

Verbindlich ist
[`NATIVE_BRINGUP_WORKFLOW.md`](NATIVE_BRINGUP_WORKFLOW.md):

1. Ein seltener `strict-product`-Lauf erzeugt statische Closure, bekannten
   nativen AOT-Code, Overlays, einen stabilen AOT-Pack und seine Allowlist.
2. Die schnelle `native-bringup`-Schleife wiederholt gebundene Replays mit
   demselben Pack und baut nur Runtime, Adapter oder Manifest inkrementell.
3. Die erste Divergenz oder der erste typisierte Stop erzeugt den naechsten
   konkreten Task.
4. Nur ein echter AOT-Miss oder eine AOT-wirksame Aenderung kehrt zur
   Vollanalyse zurueck.

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
  Static-AOT-Bloecke, ohne rohe Hostfunktionszeiger oder Tabellenmutation;
- stabile Trennung von AOT-Pack und Runtime-/Adapterbuild;
- Witness Store und Evidence Promotion Report als Offline-Evidencepfad;
- Replay-/Crash-Signaturen und First-Divergence statt terminaler
  Crashvermutung;
- private, exakt byte- und funktionsgebundene Disassembly-Evidence fuer
  titelbezogene Ziele; generische Regeln nur bei address-agnostischem Muster.

## Naechste Gates

1. Den privaten Pack mit den aktuell belegten exakten Targets neu erzeugen.
2. Den stillen Replaygurt gegen denselben Pack ausfuehren und den ersten
   neuen Stop beziehungsweise die erste Divergenz klassifizieren.
3. Runtime-/Adapterfixes in der kleinen Schleife wiederholen; Analyse nur bei
   nachgewiesener AOT-Luecke.
4. Erst nach stabiler Spielabdeckung den strikten Produktlauf und das
   Releasegate ausfuehren.

## Quellenhierarchie

Bei Widerspruechen gilt:

1. aktuelle Nutzeranweisung;
2. `AGENTS.md`;
3. `NATIVE_BRINGUP_WORKFLOW.md` und
   `NATIVE_PORT_PRODUCT_CONTRACT.md`;
4. diese aktuelle Zustandsseite;
5. Roadmap, Taskkatalog, Handoff und historische Statusabschnitte.
