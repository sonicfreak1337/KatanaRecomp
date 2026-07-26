# Third-Party and Tooling Notices

KatanaRecomp currently vendors and links no third-party source library.

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
