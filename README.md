# KatanaRecomp

KatanaRecomp ist ein unabhängiges Framework zur statischen Rekompilierung
von Dreamcast-Software für moderne Plattformen.

## Aktueller Stand

Der erste Meilenstein enthält:

- C++20-Buildsystem mit CMake und Ninja
- grundlegenden SH-4-Decoder
- Kommandozeilenprogramm
- automatische Decoder-Tests

Unterstützte Instruktionen:

- NOP
- RTS
- MOV #imm,Rn
- ADD #imm,Rn
- MOV Rm,Rn
- ADD Rm,Rn

## Grundsätze

KatanaRecomp enthält keine kommerziellen Spieldaten, Disc-Abbilder,
BIOS-Dateien oder generierten Originalcode.

Die Implementierung wird unabhängig neu entwickelt. Andere Projekte
werden ausschließlich zum Verständnis allgemeiner Arbeitsabläufe betrachtet.

## Bauen

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

## Beispiel

```powershell
.\build\katana-recomp.exe E1FF
```
