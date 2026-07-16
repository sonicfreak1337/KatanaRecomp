# Phase 10 GUI architecture

## Decision

KatanaRecomp uses a small native C++20 desktop shell for the internal alpha
scope. Windows uses Win32; Linux uses X11. Both shells link the same portable
GUI model and application service. No browser runtime, interpreter, framework
installer, or second manifest implementation is required.

Supported internal alpha targets are Windows 10/11 x64 and contemporary x64
Linux desktops with X11 compatibility. Wayland runs through XWayland for the
alpha scope. Native Wayland and macOS are outside the alpha contract.

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

## Project and source contract

The version-2 `katana-project` manifest is the saved GUI project. Phase 10 adds
`input.format = gdi` and optional `analysis.overrides` to that existing schema.
Paths are serialized relative to the project where possible. Source bytes are
never embedded. Saving first writes and parses a validation candidate before
replacing the project file.

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

`run-preflight` intentionally ends after the shared analysis, codegen and
build-plan checks. Native host execution belongs to the Phase-11 native
runtime scope. The GUI labels this boundary instead of pretending that a game
was executed.

Errors become structured diagnostics with a code, severity, message and a
recovery action. Exported job, source and shell JSON contains only relative
artifact paths and portable source names. Host paths, firmware bytes, flash
fields and serial identifiers are redacted.

## Accessibility, DPI and lifecycle

The Windows shell is system-DPI aware and uses native tab stops. `F6` and
`Shift+F6` traverse the stable navigation order; `Escape` requests job
cancellation. The portable model provides an accessible textual summary for
screen-reader adapters and automation. Long jobs execute outside the window
thread, while the shell refreshes observable progress.

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
