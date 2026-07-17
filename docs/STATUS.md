# Projektstatus

Interner Entwicklungsmeilenstein: `v0.46.0`
Phase: Pre-Alpha
Naechster Task: `KR-4715`
Naechstes Gate: `v0.47.0` - generische Retail-Runtime
Erster oeffentlicher Release: `v0.50.0` Alpha

Ein globaler Projektprozentsatz wird nicht mehr gepflegt. Die aktive Phase und
ihre Gatekriterien sind aussagekraeftiger als eine Zahl, die durch neu entdeckte
Arbeit rueckwaerts laeuft.

## Meilensteine

| Meilenstein | Ziel |
|---|---|
| `v0.47.0` | generische Runtime fertig, private Sonic-`game.exe` wird gebaut, aber nicht gestartet |
| `v0.50.0` Alpha | Sonic erreicht reproduzierbar eine kontrollierbare Spielszene |
| `v0.75.0` Beta | breite Sonic-Abdeckung, Saves, Performance und mehrere interaktive Titel |
| `v1.0.0` Stable | stabiler, dokumentierter und reproduzierbarer Supportumfang |

## Aktueller technischer Stand

Der Workflow verarbeitet Raw-, ELF32-SH- und GDI-Eingaben ueber Loader,
Kontrollflussanalyse, Katana-IR und C++-Codegen bis zu einem extern buildbaren
Hostprojekt.

Fertig sind unter anderem:

- SH-4-Integer-, Systemregister- und FPU-Grundsemantik
- Speicherbus, Exceptions, Interrupts, Scheduler, TMU, RTC und DMA
- GDI, ISO9660, GD-ROM, BIOS-HLE und System-ASIC
- Maple, Controller, VMU-Grundlage, PVR-Minimalpfad und AICA-HLE
- Runtime-Blocktabelle, guarded Dispatch, kontrollierter Fallback und
  Codeinvalidierung
- Windows-GUI, Portexport, natives Fenster, Audio, Eingabe und Lebenszyklus
- reproduzierbare synthetische Regressionen und private Retail-Harnesses

Aktuelle private Analyse:

```text
instructions=55104
functions=813
indirect_total=1826
resolved=1
guarded=1708
unresolved=117
```

Die 117 Stellen ohne endliche Zielmenge verhindern weiterhin Codegen,
Hostbuild und Runtime-Start.

## Naechste Reihenfolge

```text
KR-4715  Restfront klassifizieren
KR-4716  Callbacks, Parameter und Stack
KR-4717  Objekt- und VTable-Points-to
KR-4718  Runtime-only-Vertrag
KR-4719  private Sonic-game.exe bauen, nicht starten
KR-4703  Persistenz und Pacing
KR-4704  v0.47 Gate
KR-4705  Freigabe der Alpha-Arbeit
```

## Wichtige Grenze

Vor Abschluss von v0.47 darf Sonic Adventure lokal analysiert und gebaut werden.
Die erzeugte `game.exe` wird nicht gestartet.

Der erste echte Sonic-Prozessstart, `SA_MAIN_ENTERED`, `SA_FIRST_FRAME`,
`SA_MENU_INTERACTIVE` und `SA_ALPHA_PLAYABLE` gehoeren zur Alpha-Entwicklung.

## Bekannte offene Grundlagen

- PVR und AICA sind belastbare Minimal-/HLE-Pfade, keine vollstaendigen Modelle.
- Dynamisch geladener oder ersetzter Gastcode braucht einen ausdruecklichen
  Modul-/Overlayvertrag.
- Weitere SH-4-, FPU-, MMU-/Cache-, BIOS-, GD-ROM- und Timingluecken werden erst
  durch den Runtime-Bring-up sichtbar.
- Das eigenstaendige Homebrew-Korpus und weitere Linux-Evidenz bleiben offen.
- Linux-Desktop-GUI ist kein Alpha-Blocker; Core, CLI und Tests muessen bauen.

Die aktiven Aufgaben stehen in [`TASKS.md`](TASKS.md). Die zusammengefasste
Produktplanung steht in [`../ROADMAP.md`](../ROADMAP.md). Historische Details
bleiben in Git und den vorhandenen Gateberichten nachvollziehbar.
