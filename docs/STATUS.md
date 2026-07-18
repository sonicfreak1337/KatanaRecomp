# Projektstatus

Interner Entwicklungsmeilenstein: `v0.46.0`
Phase: Core-Stabilisierung vor v0.47
Naechster Task: `KR-4611`
Naechstes Gate: `v0.47.0` - Core-Stabilisierung und generische Retail-Runtime
Weitere interne Gates: `v0.48.0` Integration und `v0.49.0` Alpha-Candidate
Erster oeffentlicher Release: `v0.50.0` Alpha

## Aktuelles Reviewurteil

Die Grundarchitektur bleibt sinnvoll. Die Kontrollflusspruefung hat zusaetzlich
zu den bisherigen Corebefunden weitere P0-Soundnessrisiken gefunden:

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

## Naechste Reihenfolge

```text
v0.47:
KR-4611 bis KR-4617 -> KR-4618
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
