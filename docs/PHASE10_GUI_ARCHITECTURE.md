# Phase 10 GUI architecture

## Decision

KatanaRecomp currently has an incomplete native C++20 desktop shell prototype.
Windows uses Win32; Linux uses X11. Both shells link the same portable
GUI model and application service. No browser runtime, interpreter, framework
installer, or second manifest implementation is required.

The current Win32 shell exposes only the first project/job controls. The X11
shell is a placeholder and is not an alpha-complete workflow. Linux CLI/core
builds skip the desktop target when X11 development files are unavailable, or
can disable it explicitly with `KATANA_BUILD_DESKTOP_GUI=OFF`.

The decision keeps the package small, preserves the existing C++ ownership
model, and avoids introducing a large toolkit shortly before the alpha. Its
main risk is that platform shells need explicit accessibility and DPI work.
That risk is contained by keeping every state transition in the portable model
and testing it headlessly. If the GUI grows beyond the alpha workflow, the
native shells can be replaced by Qt or another toolkit without changing the
application or core contracts.

## Ownership boundaries

```text
Win32 / X11 shell
  -> katana::gui::Model
     -> katana::app::ApplicationService / JobCoordinator
        -> project manifest, GDI, analysis, IR, codegen and runtime libraries
```

- The shell owns windows, controls, focus, keyboard input and native dialogs.
- The GUI model owns navigation, dirty state, settings recovery and observable
  presentation state. It does not parse a GDI, lower IR, or emit C++.
- The application service owns project jobs, progress events, cancellation,
  artifact identities and redacted diagnostics.
- Existing core libraries remain the only implementation of manifest, GDI,
  analysis, firmware profile, codegen and runtime semantics.
- `katana-recomp workflow` and `katana-recomp-gui` call the same application
  service and therefore produce the same project identity and core artifacts.

## Source and output contract

The public GUI workflow has two inputs: one `.gdi` descriptor and one output
directory. A version-2 `katana-project` manifest may be generated inside the
temporary session to reuse core contracts, but is not a user-facing project
concept. Source bytes are never embedded.

The GDI inspector calls `parse_gdi_descriptor` directly. It exposes stable
track number, descriptor line, role, LBA, sector size, offset, size, basename
and SHA-256. It never writes the descriptor or its tracks. Opening a GDI job
uses `load_dreamcast_gdi_boot`, exactly like the CLI and port workflow.

## Job and recovery contract

Application jobs are `validate`, `analyze`, `codegen`, `build`, and
`run-preflight`. A job emits queued, running and terminal events with stable
stage names. Cancellation is checked before every expensive stage and before
generated output is committed. Output conflicts are rejected by
`JobCoordinator`; unrelated output roots may run concurrently.

`build` now invokes a real generated-code host compilation; GDI builds reuse
the productive port exporter and build its host target. `run-preflight`
intentionally ends after the shared analysis, codegen and host-build checks.
Native host execution belongs to the Phase-11 native
runtime scope. The GUI labels this boundary instead of pretending that a game
was executed.

Errors become structured diagnostics with a code, severity, message and a
recovery action. Exported job, source and shell JSON contains only relative
artifact paths and portable source names. Host paths, firmware bytes, flash
fields and serial identifiers are redacted.

## Accessibility, DPI and lifecycle

The Windows shell selects the GDI and output directory, shows every recompilation
stage, and exposes a read-only scrollable diagnostic log. It is system-DPI aware
and uses native tab stops. `F6` and
`Shift+F6` traverse the stable navigation order; `Escape` requests job
cancellation. The portable model provides an accessible textual summary for
screen-reader adapters and automation. Long jobs execute outside the window
thread, while the shell refreshes observable progress.

These behaviors are not yet covered by native control-level automation.
Versioned settings validate theme, scale and recent-project limits. Invalid or
future settings recover to safe defaults with a visible diagnostic. Closing
with unsaved changes requires confirmation.

## Packaging and asset policy

The internal package candidate contains `katana-recomp`,
`katana-recomp-gui`, the application logo, its machine-readable asset manifest
and internal workflow documentation. Windows Debug candidates also carry the
matching MSVC AddressSanitizer runtime so the packaged smoke test is standalone.
The package is not a release. The logo
was supplied through the private project intake and is committed with its
hash and provenance record; public distribution remains blocked until
KR-4902 completes the full data and licence audit.
