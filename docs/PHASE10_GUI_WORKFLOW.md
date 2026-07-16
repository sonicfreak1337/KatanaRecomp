# Internal Phase 10 GUI workflow

1. Start `katana-recomp-gui` from the build or internal package directory.
   MSVC AddressSanitizer Debug builds place their matching runtime DLL beside
   the executable automatically, so a normal PowerShell is sufficient.
2. Select exactly one `.gdi` source and an output directory. The GUI creates
   any manifest needed by the shared core internally; users do not open or
   maintain project files.
3. Inspect the portable source name, SHA-256 and size. The source view also shows
   every read-only track in descriptor order with its exact descriptor line,
   role, LBA, sector format, offset and provenance hash.
4. Start recompilation. The GUI keeps validation, analysis, code generation and
   the host build visible as stable progress stages and writes `sourcecode/`
   plus `game.exe` to the selected directory. A scrollable diagnostic log shows
   live compiler output, errors and recovery hints for debugging; the redacted
   copy remains as `recompile.log`. `Escape` or **Cancel** requests a
   controlled stop.
6. Use Results for the deterministic function, segment, source and provenance
   index. Use Diagnostics for warnings, errors and recovery instructions.

The equivalent lower-level CLI command remains available for automation:

```powershell
.\build-current\katana-recomp.exe workflow build .\project.katana --output .\work-output
```

The model/application-service automation path is:

```powershell
.\build-current\katana-recomp-gui.exe --automation .\project.katana .\work-output
```

This command does not automate native buttons, dialogs, focus or DPI behavior.
Both commands use the same application service. `run-preflight` does not start
a native game in Phase 10; it verifies that the project is ready for the later
native host runtime.

If a GDI track was moved, restore it below the descriptor directory or select
a valid project source. The GUI never rewrites a GDI or track. If settings are
from an unsupported future schema, move the settings file aside and restart;
safe defaults are restored automatically and the project files remain intact.
