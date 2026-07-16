# Internal Phase 10 GUI workflow

1. Start `katana-recomp-gui` from the build or internal package directory.
   MSVC AddressSanitizer Debug builds place their matching runtime DLL beside
   the executable automatically, so a normal PowerShell is sufficient. Keep
   the packaged `runtime-sdk/` beside the executable; alternatively set
   `KATANA_RUNTIME_ROOT` to a compatible KatanaRecomp runtime source/SDK root.
2. Select exactly one `.gdi` source and an output directory. The GUI creates
   any manifest needed by the shared core internally; users do not open or
   maintain project files.
3. Inspect the portable source name, SHA-256 and size. The source view also shows
   every read-only track in descriptor order with its exact descriptor line,
   role, LBA, sector format, offset and provenance hash.
4. Start recompilation. The GUI keeps validation, analysis, code generation and
   the host build visible as stable progress stages. It writes `sourcecode/`
   plus `game.exe` only after control-flow analysis is complete. A `partial`
   result writes analysis, result index and build plan but deliberately skips
   code generation and host compilation. A scrollable diagnostic log shows
   live compiler output, errors and recovery hints for debugging; the redacted
   copy remains as `recompile.log`. `Escape` or **Cancel** requests a
   controlled stop.
5. Use Results for the deterministic function, segment, source and provenance
   index. Use Diagnostics for warnings, errors and recovery instructions.

The three terminal job states are distinct: `completed` means every committed
executable byte is analyzed and no unknown instruction, unresolved control-flow
site or reachable abort edge remains. `partial` keeps useful analysis when any
of those proofs is missing, and `failed` means an I/O, validation, tool or
host-build error prevented the request.

Output publication is atomic at job granularity. Cancelled and failed jobs do
not expose staged `sourcecode/`, `recompile.log` or `game.exe` as current
results. A failed rebuild moves an older successful result beside the output as
an explicitly stale recovery copy. A second KatanaRecomp process cannot write
the same or an overlapping output directory concurrently on Windows or Linux.
Repeated failures with the stable `cli-workflow` ID preserve the last successful
stale recovery copy instead of replacing it with an earlier failure report.

The equivalent lower-level CLI command remains available for automation:

```powershell
.\build-current\katana-recomp.exe workflow build .\project.katana --output .\work-output
```

During this command, `stderr` is a `katana-job-event` JSONL stream shared with
the GUI, while `stdout` remains the final `katana-application-job` document.
Validation, hashing, boot image, analysis, IR, codegen, host configuration,
host compilation and finalization remain ordered. Unknown step totals are
shown as indeterminate and failure/cancellation names the active step.

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
