# Sanitizer-Debugprofil

Das CMake-Profil `sanitizer-debug` ist ein Debug-Build im einzigen erlaubten
Buildverzeichnis `build-current/`. Es wird vor jeder Verwendung frisch
konfiguriert:

```text
cmake --preset sanitizer-debug --fresh
cmake --build --preset sanitizer-debug --parallel
ctest --preset sanitizer-debug
```

GNU und Clang verwenden `-fsanitize=address,undefined`, behalten Framepointer
und brechen beim ersten Sanitizerfehler ab. MSVC besitzt keinen gleichwertigen
UndefinedBehaviorSanitizer; dort aktiviert dasselbe semantische Profil
`/fsanitize=address`, entfernt das damit inkompatible Debug-`/RTC1` und
deaktiviert inkrementelles Linken.

`ASAN_OPTIONS` und `UBSAN_OPTIONS` werden ausschliesslich ueber das Testpreset
gesetzt und verlangen sofortigen Abbruch. Eine Leakpruefung ist nicht Teil des
plattformuebergreifenden Vertrags, weil MSVC-ASan dafuer keine gleichwertige
Unterstuetzung bietet. Das Profil enthaelt keine Unterdrueckungsliste, die
echte Fehler still ausblenden koennte.

Das Profil wird in KR-3702 nur bereitgestellt. Es wird weder pro Task noch als
separater zweiter Build ausgefuehrt. KR-3709 waehlt das Sanitizer-Debugprofil
fuer den einen frischen Phase-8-Build, sodass Debugregression und Sanitizerlauf
dieselben Testfaelle und dasselbe `build-current/` verwenden.
