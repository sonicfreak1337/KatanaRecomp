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

No verifiable upstream version or compatible license is recorded for the
planning reference called dcrecomp. Its code is therefore prohibited unless a
future provenance and license review explicitly clears a precise snapshot.

Renesas SH-4 manuals are used as behavioral specifications. They are linked,
not redistributed. See `docs/REFERENCE_PROVENANCE.md` for exact document
revisions, reference commits, reviewed scope, and synthetic-fixture origins.

KatanaRecomp itself currently has no repository license file. External
distribution remains blocked until the explicit project-license decision in
KR-4902 before the first public Alpha release.
