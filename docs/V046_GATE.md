# v0.46-Gate-Vorbereitung

KR-4604 wurde mit genau einem frischen MSVC-x64-Debug-Build in
`build-current/` vorbereitet. AddressSanitizer, statische Analyse und Coverage
waren aktiv.

```text
168/168 Tests bestanden
KR_PHASE9_HOMEBREW_HOST_FRAME: silent_failures=0
KR_V046_RETAIL_BOOT_SERVICES_READY
```

Der neue synthetische Vertical-Slice verbindet HLE-Bootzustand, sechs
dynamische BIOS-ABI-Vektoren, Runtimeblock-/Aliasdispatch, System-ASIC-MMIO,
PVR, Maple, GD-ROM, AICA, EventScheduler und IRL9 ohne Firmware- oder
Retailbytes. Format-, Qualitaets-, Referenz-, Lizenz- und Datenaudits sowie der
private Retail-Harness-Selbsttest und native QoL-Vertrag bestanden.

Der erste Regressionslauf fand einen Provenienz-Prioritaetsfehler: Ein
dynamisches Laufzeitsymbol ueberdeckte eine spezifischere byteverifizierte
ROM-nach-RAM-Codekopie. Der Resolver priorisiert nun Codekopie vor dynamischem
Symbol und allgemeinem Segmentmapping. Nur das betroffene Ziel wurde
inkrementell neu gebaut; danach bestand die vollstaendige 168-Test-Regression
samt Coverage auf demselben Fresh-Build.

Der Checkpoint behauptet keine Sonic-Kompatibilitaet. Es wurden weder private
Retaildaten noch Release-Commit, Tag, Download oder Paket erzeugt.

## Reviewgrenze

KR-4605 bleibt offen. Erst eine ausdrueckliche Nutzerfreigabe der unveraenderten
Vorbereitung erlaubt den Beginn von Phase 12 / v0.47.
