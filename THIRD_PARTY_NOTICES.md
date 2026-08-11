# Third-Party and Tooling Notices

KatanaRecomp vendors the SkyEmu ARM7TDMI source header for the historical
Dreamcast AICA ARM7 diagnostic/analyzer path. It is compiled only into
non-product diagnostic/analyzer artifacts and is not linked by the
native-port product runtime.

SkyEmu is Copyright (c) 2021 Skyler "Sky" Saleh and licensed under the MIT
License. The exact vendored license text is retained in
`third_party/skyemu/LICENSE`; the vendored source provenance is recorded in
`third_party/skyemu/README.md`.

The Windows native-port media runtime dynamically links a pinned FFmpeg
`n8.1.2-34-g9b6c8969e0` LGPL shared build for Sofdec MPEG-PS, CRI ADX, audio
resampling and decoded video conversion. GPL and nonfree FFmpeg components are
excluded and rejected again at runtime. Katana deploys only the required
shared-library closure and never launches or packages the FFmpeg command-line
programs. A local build without the complete corresponding-source archive is
marked `FFmpeg-DEVELOPMENT-ONLY.txt` and is not a redistributable package.
Public packaging is fail-closed until the exact FFmpeg source, exact BtbN
recipe, complete pinned BtbN dependency-source cache, configure line and empty
Katana changes diff are bundled as `FFmpeg-Corresponding-Source.zip`. The
bundle, DLLs, LGPLv3 text, notice and build configuration are individually
hash-bound by the native port runtime-dependency manifest. Exact identities
and the bundle procedure are recorded in `third_party/ffmpeg/NOTICE.txt` and
`tools/dependencies/package-ffmpeg-corresponding-source.ps1`.

The project is built with a C++20 compiler and the compiler's standard and
operating-system libraries. CMake and Ninja orchestrate builds. Optional local
quality gates use clang-format, clang-tidy, Microsoft.CodeCoverage.Console, or
gcovr. These tools are invoked from the user's installation and are not copied
into KatanaRecomp packages. Internal Windows Debug GUI packages do include the
matching Microsoft compiler AddressSanitizer runtime DLL required to start the
packaged instrumented executables; this internal package is not a release.

Flycast is a GPL-2.0 reference project. KatanaRecomp does not contain or link
Flycast code. Any future direct integration requires a documented project-wide
GPL compatibility decision before it can be enabled.

The planning reference called dcrecomp is recorded as the public upstream
`sp00nznet/dcrecomp` at commit
`25bdc3d248a0084fa98335511991872e578b2b4a`. This pin makes the reviewed
behavioral snapshot reproducible; it does not grant code-use permission. The
upstream describes its core as private and includes GPLv2 Flycast subsystems,
so KatanaRecomp neither contains nor links dcrecomp code. Any future direct
integration requires a separate provenance and project-wide license review.

Renesas SH-4 manuals are used as behavioral specifications. They are linked,
not redistributed. See `docs/REFERENCE_PROVENANCE.md` for exact document
revisions, reference commits, reviewed scope, and synthetic-fixture origins.

SingleStepTests/sh4 is an optional external conformance corpus generated from
Reicast interpreter behavior. KatanaRecomp does not download it during a
normal build or CTest run and does not link its generator or any interpreter.
The supported checkout is pinned to commit
`48975cb1a9569abb5a0cba587013ea54edf79100`. Corpus data and its documentation
are Copyright (c) 2024 SingleStepTests and licensed under the MIT License:
<https://github.com/SingleStepTests/sh4/blob/48975cb1a9569abb5a0cba587013ea54edf79100/LICENSE>.
The exact pinned license text is retained in
`third_party/SingleStepTests-sh4-LICENSE.txt`.
When corpus data or a derivative containing a substantial portion of it is
redistributed, its copyright notice and MIT permission notice must accompany
that distribution.

KatanaRecomp itself currently has no repository license file. External
distribution remains blocked until the explicit project-license decision in
KR-4902 before the first public Alpha release.
