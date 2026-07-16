# Internal Phase 10 GUI workflow

1. Start `katana-recomp-gui` from the build or internal package directory.
   MSVC AddressSanitizer Debug builds place their matching runtime DLL beside
   the executable automatically, so a normal PowerShell is sufficient.
2. Open an existing version-2 `.katana` project, or create one through the
   project model with a Raw, ELF32-SH or GDI source.
3. Inspect the portable source name, SHA-256 and size. GDI projects also show
   every read-only track in descriptor order with its exact descriptor line,
   role, LBA, sector format, offset and provenance hash.
4. Edit the shared manifest profile. Firmware, fallback, scheduler, MMU,
   fastpath, alias, executable-RAM and override rules are validated by the
   manifest parser before a job can start.
5. Run validation, analysis, codegen, build preparation or run preflight. The
   Jobs view reports stable progress stages. `Escape` or **Cancel** requests a
   controlled stop.
6. Use Results for the deterministic function, segment, source and provenance
   index. Use Diagnostics for warnings, errors and recovery instructions.

The equivalent automation command is:

```powershell
.\build-current\katana-recomp.exe workflow build .\project.katana --output .\work-output
```

The automated desktop path is:

```powershell
.\build-current\katana-recomp-gui.exe --automation .\project.katana .\work-output
```

Both commands use the same application service. `run-preflight` does not start
a native game in Phase 10; it verifies that the project is ready for the later
native host runtime.

If a GDI track was moved, restore it below the descriptor directory or select
a valid project source. The GUI never rewrites a GDI or track. If settings are
from an unsupported future schema, move the settings file aside and restart;
safe defaults are restored automatically and the project files remain intact.
