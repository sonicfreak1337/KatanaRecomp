# Interne Phase-9-Architektur- und Profilreferenz

Diese Referenz ist fuer die weitere Entwicklung bestimmt. Sie ist keine
Installations- oder Nutzeranleitung.

## Gemeinsamer Datenfluss

`ProjectManifest` validiert Eingabeformat, Einstieg, Segmente,
Firmware-/Fallback-/Scheduler-/MMU-/Fastpathprofil, Backendfaehigkeiten,
Aliasgruppen und beschreibbaren Code. `LoadedProject` transportiert Image und
Profil gemeinsam. Analyse und IR arbeiten ausschliesslich mit Gastadressen;
Codegen bindet Runtime-/Backend-ABI und bekannte externe Funktionseinstiege.
CLI, Phase-9-Tools und die spaetere GUI muessen diesen Pfad wiederverwenden.

Die Runtime trennt CPU-Zustand, Speicherbus, Scheduler, Plattformgeraete,
Blocktabelle, Dispatch, kontrollierten Fallback und Codeinvalidierung. Runtime-
und Diagnoseprovenienz verwendet stabile Gast-/Blockidentitaeten, niemals
Hostzeiger oder eingebettete Firmwarebytes.

## Profile und Faehigkeiten

Die maschinenlesbare Matrix liegt in `tools/phase9/capability-matrix.json`.
Direct-Homebrew und gastzyklusdeterministischer Scheduler sind Pflicht fuer den
Phase-9-Nachweis. HLE, MMU, Fastmem und Inline-Caches bleiben klar als
experimentell markiert. LLE ist nicht unterstuetzt und kein Homebrew- oder
Phase-9-Gate-Blocker. Kontrollierter Fallback und selbstmodifizierender Code
sind nur innerhalb ihrer expliziten Manifest- und Generationsvertraege
unterstuetzt.

## Performance- und Korrektheitsgrenzen

Fastmem gilt nur fuer bewiesenen linearen RAM. MMU, unstabile Aliase,
Watchpoints, MMIO, falsche Ausrichtung, Berechtigungsfehler oder geaenderte
Adressraum-/Codegeneration erzwingen den generischen Speicherpfad. Der
monomorphe Dispatchcache bindet Callsite, Ziel, `BlockVariantKey`, stabile
Blockidentitaet und Generation; jeder Konflikt faellt auf den generischen
Dispatch zurueck.

Profiling ist deaktiviert, exakt oder deterministisch gesampelt. Profile binden
Eingabe-SHA sowie Runtime-/Backend-ABI. Zaehler fuer Bloecke, Kanten,
indirekte Callsites, Fallback, Invalidierung sowie Guardtreffer und -fehler sind
semantikfrei; deaktiviertes Profiling darf den Gastzustand nicht aendern.

## Bekannte Einschraenkungen

- kein ARM7-LLE und keine Retail-Firmwarequalifikation vor KR-4601
- keine oeffentliche Projektlizenz oder Homebrew-Distribution vor KR-4902
- keine native Fenster-, Audio- oder Eingabeintegration vor Phase 10/11
- Hostpacing ist vom gastzyklischen Scheduler getrennt und noch kein
  Echtzeitversprechen
- Fastpaths sind konservativ und decken weder MMIO noch aktive MMU-/Watchpoint-
  Konfigurationen ab
- LTO und PGO werden erst nach dem Alpha anhand realer Profile bewertet

Jede Faehigkeitsaussage der Matrix nennt einen automatisierten Test oder den
expliziten lokalen Nichtverfuegbarkeitscheck.
