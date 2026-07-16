# Deterministisches Fuzzing

`katana-fuzz` besitzt die drei ausschliesslich synthetischen Ziele `decoder`,
`loader` und `ir`. `all` fuehrt sie in dieser Reihenfolge aus. Ein Lauf ist durch
Ziel, Seed, Iterationszahl und maximale Eingabegroesse vollstaendig bestimmt:

```text
katana-fuzz --target all --seed 0x3703 --iterations 256 --max-input-size 4096
```

Der Decoder verarbeitet rohe SH-4-Opcodes und direkte Kontrollflussziele. Der
Loader verwendet einen gueltigen synthetischen ELF32-SH-Seed sowie verkuerzte
und zufaellige Bytefolgen ueber den dateilosen Loader-Einstieg. Das IR-Ziel baut
begrenzte Funktionen, prueft ihre Invarianten und laesst gueltige Programme
durch die Optimierung mit anschliessender erneuter Verifikation laufen.

Ungueltige Eingaben duerfen mit den dokumentierten Validierungsfehlern
abgewiesen werden. Ein unerwarteter Fehler nennt Ziel, Seed und Iteration. Der
angegebene Aufruf mit derselben Iterationszahl reproduziert exakt denselben
letzten Fall. Es werden weder Eingaben noch Crasher ungefragt auf die Festplatte
geschrieben.

Das CMake-Profil `fuzz-debug` aktiviert den Runner im einzigen Buildverzeichnis
`build-current/` und erbt das Sanitizer-Debugprofil. Der feste Kurzlauf ist als
CTest registriert, wird nach dem Phase-8-Workflow aber erst gesammelt in KR-3709
gebaut und ausgefuehrt. Externe Langlaeufe skalieren ausschliesslich
`--iterations` und `--max-input-size`.
