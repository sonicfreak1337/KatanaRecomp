# VMU-/Flash-Arbeitskopien und Host-Pacing

KR-4703 trennt veraenderliche Dreamcast-Daten strikt von Nutzerquellen und
koppelt die native Ausgabe an eine monotone Hostuhr, ohne Hostzeit zur
Gastsemantik zu machen.

## Persistente Arbeitskopien

`PersistentImage` verwaltet Flash und VMU in einem versionierten Container.
Eine optionale Quelle wird nur gelesen und bei jedem Speichern erneut per
SHA-256 geprueft. Ohne Quelle beginnt das Abbild mit dem konfigurierten
Erase-Wert. Quelle, primaere Arbeitskopie und `.recovery`-Datei muessen
verschiedene, nicht symbolisch verlinkte Dateien sein.

Der Container bindet Typ, Groesse, Quell-SHA-256 und Nutzdaten-SHA-256. Ein
Speichern schreibt zuerst eine neue temporaere Datei, synchronisiert sie auf
den Datentraeger, verschiebt die bisherige primaere Datei nach `.recovery` und
veroeffentlicht erst danach die neue primaere Datei. Schlaegt der Publish fehl,
wird die vorherige primaere Datei wiederhergestellt. Eine gueltige Recovery
wird verwendet, wenn die primaere Datei fehlt oder ihre Signatur, Identitaet,
Groesse oder Nutzdatenpruefung ungueltig ist. Sind beide Kopien ungueltig,
bricht das Oeffnen sichtbar ab; die Quelle wird nie repariert oder beschrieben.

`DreamcastMutableStorage` legt beide Arbeitskopien unter
`<user-data>/saves/<project-identity>/` ab. `KATANA_USER_DATA_ROOT` kann die
Nutzerdatenwurzel explizit setzen. Sonst gilt unter Windows
`LOCALAPPDATA/KatanaRecomp`, unter Unix `XDG_DATA_HOME/katana-recomp` oder
`HOME/.local/share/katana-recomp`. Die portable SHA-256-Projektidentitaet
verhindert Kollisionen zwischen Ports. Die GDI, optionale Flash-/VMU-Quellen
und alle exportierten Portartefakte bleiben unveraendert.

Flash-Kommandos und Maple-VMU-Blockwrites veraendern ausschliesslich die
gemeinsamen Arbeitsabbilder. Der Shutdown speichert beide Abbilder genau
einmal. `katana-persistent-image-v1` und `katana-dreamcast-storage-v1` melden
nur Typ, Groesse, Dirty-Zustand, Recoveryklasse und Speicherzaehler; Pfade,
Hashes und Nutzdaten sind ausgeschlossen.

## Host-Pacing

`HostPacer` Version 1 bildet Gastzyklen ganzzahlig auf Nanosekunden einer
monotonen Hostuhr ab. Video-Ticks rufen `pace()` auf; Audio, Eingabe,
Schedulerereignisse und Gastresultate bleiben weiterhin ausschliesslich an die
Gastzyklusuhr gebunden. Der Pacer darf den Host nur bis zu einer berechneten
Deadline warten lassen. Er darf weder Gastzyklen erzeugen noch ueberspringen.

Pause und Resume setzen einen neuen Hostzeitanker am aktuellen Gastzyklus.
Gastzyklus- oder Hostuhrregression, Deadline-Ueberlauf und ein zu frueh
zurueckkehrender Wait stoppen sichtbar mit `HostPacingException`.
`katana-host-pacing-v1` zaehlt Waits und verspaetete Deadlines und haelt den
ersten Fehler fest. Der generierte Port schreibt einen Fehler als
`KATANA_HOST_PACING_ERROR`-JSON-Zeile und beendet sich ungleich null.

`HostRuntimeSession` Version 2 ordnet Resume, Media-Clock, Audio, Pacer und
Shutdown deterministisch. Shutdown stoppt Media-Clock und Audio, leert den
Scheduler, stoppt den Pacer und ruft danach den Persistenz-Callback genau
einmal auf. Ein Speicherfehler bleibt trotz `noexcept`-Shutdown ueber
`require_clean_shutdown()` sichtbar.

## In KR-4704 umzusetzende Regressionen

- Quelle bleibt bei Erzeugung, Write, Save, defekter Primaerdatei, fehlender
  Primaerdatei und ungueltiger Recovery bytegleich.
- Primaer- und Recoverycontainer werden getrennt auf Signatur, Typ, Groesse,
  Quellidentitaet und Nutzdatenhash geprueft; zwei ungueltige Kopien brechen ab.
- Flash-Program/Erase und VMU-Blockwrite markieren nur die Arbeitskopie dirty,
  persistieren nach Save und laden in einer neuen Sitzung dieselben Bytes.
- Gleiche Quell-/Arbeitsdatei, Symlinks, Schreibschutz und Publishfehler enden
  sichtbar, ohne Quelle oder letzte gueltige Kopie zu veraendern.
- Eine Fake-Hostuhr prueft exakte ganzzahlige Deadlines, Wait-/Late-Zaehler,
  Pause-/Resume-Reankern sowie alle vier typisierten Pacingfehler.
- Hostruntime und generierter Port pruefen geordnetes Resume, Pause, Shutdown,
  genau einen Save, Fehlerweitergabe, Storage-/Pacer-Integration sowie die
  JSON-Summen- und Datenschutzinvarianten.
- Runtime-ABI 13, Hostruntimevertrag 2 und Portprojektvertrag 5 werden im
  frischen Paket- und Portbuild erzwungen.
