# v0.46-Gate-Vorbereitung

Der vorherige Lauf auf Commit `0e2fb51` wurde nach Reviewbefunden invalidiert.
Nach den Korrekturen an produktiver HLE-/ASIC-Verdrahtung, Jobveroeffentlichung,
Retail-Harness, Pfadisolation, Live-Log und Linux-Prozessbehandlung wurde das
Gate auf Commit `e7b19baea3d68b2bc5e7a441c7f419f74c1752c4` vollstaendig neu ausgefuehrt.

KR-4604 wurde auf dem korrigierten Commit mit genau einem frischen MSVC-x64-Debug-Build in
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

Der Nutzer hat vor dem Lauf ausdruecklich festgelegt, dass ein erfolgreiches
Gate KR-4605 freigibt. Diese Bedingung ist erfuellt. Die Freigabe erlaubt den
Beginn von Phase 12 / v0.47, jedoch keine Release-, Tag- oder Paketaktion.
