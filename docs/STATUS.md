# Projektstatus

Interner Entwicklungsmeilenstein: `v0.46.0`
Phase: Core-Stabilisierung vor v0.47
Naechster Task: `KR-4611`
Naechstes Gate: `v0.47.0` - Core-Stabilisierung und generische Retail-Runtime
Erster oeffentlicher Release: `v0.50.0` Alpha

Ein globaler Projektprozentsatz wird nicht gepflegt. Die aktive Phase und ihre
Gatekriterien sind aussagekraeftiger als eine Zahl, die durch neu entdeckte
Arbeit rueckwaerts laeuft.

## Reviewurteil

Die Schichten und das Produktziel bleiben sinnvoll. Eine repositoryweite
statische Core-Review hat jedoch mehrere P0-Vertragsfehler in bereits
vorhandenen Funktionen gefunden. Neue Retail-Kontrollflussarbeit ist deshalb
bis zum Core-Korrektheits- und Performancegate blockiert.

Hauptblocker:

- effektive SH-4-Registerbank ignoriert `SR.MD`
- Store Queue verwendet das falsche Adressbit
- PR-Delay-Slot, RTE, SLEEP und Interruptfortsetzung sind im generierten Pfad
  nicht korrekt
- normale generierte Gaststores umgehen die Codeinvalidierung
- Analyse kann unbekannte Caller oder abweichende Callkontexte unterschlagen
- Runtime-Blocktabelle besitzt instabile Zeiger und lineare Hotpaths
- GD-ROM und andere Geraete besitzen keinen gemeinsamen Gastzyklusvertrag
- optimierte Builds werden nicht dauerhaft regressionsgeprueft

Der vollstaendige Befund steht in
[`CODE_REVIEW_2026-07-17.md`](CODE_REVIEW_2026-07-17.md).

## Naechste Reihenfolge

```text
P0 Core-Korrektheit:
KR-4611 bis KR-4617 -> KR-4618

P1 Performance und Build:
KR-4621 bis KR-4624 -> KR-4625

Retail-Kontrollfluss:
KR-4715 -> KR-4716 und KR-4717 -> KR-4718 -> KR-4719
        -> KR-4703 -> KR-4704 -> KR-4705
```

Unabhaengige Tasks innerhalb einer Stufe duerfen parallel entwickelt werden.
Eine spaetere Stufe beginnt erst, wenn das vorherige Gate vollstaendig besteht.

## Unveraenderte Produktgrenze

Vor Abschluss von v0.47 darf Sonic Adventure lokal analysiert und gebaut werden.
Die erzeugte `game.exe` wird nicht gestartet.

Der erste echte Sonic-Prozessstart, `SA_MAIN_ENTERED`, `SA_FIRST_FRAME`,
`SA_MENU_INTERACTIVE` und `SA_ALPHA_PLAYABLE` gehoeren weiterhin zur
Alpha-Entwicklung.

## Bestehende Funktionssicherung

- keine bestehende korrekte Regression wird entfernt oder abgeschwaecht
- falsche historische Erwartungen werden durch unabhaengige
  Spezifikationsvektoren ersetzt
- jeder Fastpath behaelt einen deaktivierbaren sicheren Referenzpfad
- Debug und RelWithDebInfo muessen dieselben Gastresultate liefern
- Performanceverbesserungen duerfen Gastzustand und Ereignisreihenfolge nicht
  veraendern
