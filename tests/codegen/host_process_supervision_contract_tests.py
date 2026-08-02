#!/usr/bin/env python3
"""Platform-neutral source gate for the POSIX host-process supervisor."""

from __future__ import annotations

import argparse
import json
import os
import pathlib
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import time


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(f"host supervision contract failed: {message}")


def host_command(arguments: list[str]) -> str:
    if os.name == "nt":
        return subprocess.list2cmdline(arguments)
    return shlex.join(arguments)


def run_product_regressions(katana_cli: pathlib.Path) -> None:
    with tempfile.TemporaryDirectory(
        prefix="katana-host-supervision-"
    ) as directory_text:
        directory = pathlib.Path(directory_text)
        helper = directory / "descendant.py"
        helper.write_text(
            """import os
import pathlib
import subprocess
import sys
import time

mode = sys.argv[1]
artifact = pathlib.Path(sys.argv[2])
delay = float(sys.argv[3])
marker = sys.argv[4]
if mode == "environment":
    artifact.write_text(
        os.environ.get("CL", "<missing>") + "\\n" +
        os.environ.get("_CL_", "<missing>"),
        encoding="utf-8",
    )
    raise SystemExit(0)
if mode == "path-environment":
    artifact.write_text(os.environ.get("PATH", "<missing>"), encoding="utf-8")
    raise SystemExit(0)
if mode == "percent-environment":
    artifact.write_text(
        os.environ.get("KATANA_PERCENT_VALUE", "<missing>"),
        encoding="utf-8",
    )
    print(marker, flush=True)
    raise SystemExit(0)
if mode in {"root", "escaped-root"}:
    subprocess.Popen(
        [sys.executable, __file__, "child", str(artifact), str(delay), marker],
        close_fds=False,
        start_new_session=(mode == "escaped-root"),
    )
    os._exit(0)
if mode != "child":
    raise SystemExit(3)
time.sleep(delay)
artifact.write_text("descendant-finished", encoding="utf-8")
print(marker, flush=True)
""",
            encoding="utf-8",
        )

        artifact = directory / "natural-child.txt"
        command = host_command(
            [
                sys.executable,
                str(helper),
                "root",
                str(artifact),
                "0.25",
                "DESCENDANT_STDOUT",
            ]
        )
        started = time.monotonic()
        completed = subprocess.run(
            [
                str(katana_cli),
                "__host-supervision-probe",
                "3000",
                command,
            ],
            check=False,
            capture_output=True,
            text=True,
            timeout=8,
        )
        elapsed = time.monotonic() - started
        require(completed.returncode == 0, completed.stderr)
        require(
            artifact.read_text(encoding="utf-8") == "descendant-finished",
            "root exit truncated a legitimate descendant",
        )
        require(elapsed >= 0.20, "supervisor returned before descendant exit")
        require(
            "DESCENDANT_STDOUT" in completed.stdout
            and "process_tree_quiescent=1" in completed.stdout,
            "inherited stdout or the quiescence result was lost",
        )

        closed_artifact = directory / "closed-stdout-child.txt"
        closed_command = host_command(
            [
                sys.executable,
                str(helper),
                "root",
                str(closed_artifact),
                "0.10",
                "CLOSED_STDOUT",
            ]
        )
        with open(os.devnull, "wb") as null_output:
            closed = subprocess.run(
                [
                    str(katana_cli),
                    "__host-supervision-probe",
                    "3000",
                    closed_command,
                ],
                check=False,
                stdout=null_output,
                stderr=subprocess.PIPE,
                timeout=8,
            )
        require(closed.returncode == 0, closed.stderr.decode(errors="replace"))
        require(
            closed_artifact.is_file(),
            "closed stdout prevented descendant completion",
        )

        timeout_artifact = directory / "timed-out-child.txt"
        timeout_command = host_command(
            [
                sys.executable,
                str(helper),
                "root",
                str(timeout_artifact),
                "3.0",
                "TOO_LATE",
            ]
        )
        timed_out = subprocess.run(
            [
                str(katana_cli),
                "__host-supervision-probe",
                "150",
                timeout_command,
            ],
            check=False,
            capture_output=True,
            text=True,
            timeout=8,
        )
        require(timed_out.returncode == 124, timed_out.stderr)
        require(
            "timed_out=1" in timed_out.stdout
            and "process_tree_quiescent=1" in timed_out.stdout,
            "deadline did not publish timeout plus tree quiescence",
        )
        time.sleep(0.25)
        require(
            not timeout_artifact.exists(),
            "timed-out descendant survived the process-tree kill",
        )

        if os.name != "nt":
            escaped_artifact = directory / "escaped-session-child.txt"
            escaped_command = host_command(
                [
                    sys.executable,
                    str(helper),
                    "escaped-root",
                    str(escaped_artifact),
                    "0.25",
                    "ESCAPED_SESSION_STDOUT",
                ]
            )
            escaped = subprocess.run(
                [
                    str(katana_cli),
                    "__host-supervision-probe",
                    "3000",
                    escaped_command,
                ],
                check=False,
                capture_output=True,
                text=True,
                timeout=8,
            )
            require(escaped.returncode == 0, escaped.stderr)
            require(
                escaped_artifact.is_file()
                and "ESCAPED_SESSION_STDOUT" in escaped.stdout,
                "setsid descendant was lost after the root exited",
            )

            escaped_timeout_artifact = (
                directory / "escaped-session-timeout.txt"
            )
            escaped_timeout_command = host_command(
                [
                    sys.executable,
                    str(helper),
                    "escaped-root",
                    str(escaped_timeout_artifact),
                    "3.0",
                    "ESCAPED_TOO_LATE",
                ]
            )
            escaped_timeout = subprocess.run(
                [
                    str(katana_cli),
                    "__host-supervision-probe",
                    "150",
                    escaped_timeout_command,
                ],
                check=False,
                capture_output=True,
                text=True,
                timeout=8,
            )
            require(escaped_timeout.returncode == 124, escaped_timeout.stderr)
            time.sleep(0.25)
            require(
                not escaped_timeout_artifact.exists(),
                "timed-out setsid descendant escaped the subreaper kill",
            )

            escaped_telemetry_artifact = (
                directory / "escaped-session-telemetry-child.txt"
            )
            escaped_telemetry_path = directory / "escaped-tree.jsonl"
            escaped_telemetry_command = host_command(
                [
                    sys.executable,
                    str(helper),
                    "escaped-root",
                    str(escaped_telemetry_artifact),
                    "0.45",
                    "ESCAPED_TELEMETRY_STDOUT",
                ]
            )
            escaped_telemetry = subprocess.run(
                [
                    str(katana_cli),
                    "__host-supervision-probe",
                    "3000",
                    escaped_telemetry_command,
                    "--telemetry-jsonl",
                    str(escaped_telemetry_path),
                ],
                check=False,
                capture_output=True,
                text=True,
                timeout=8,
            )
            require(escaped_telemetry.returncode == 0, escaped_telemetry.stderr)
            telemetry_records = [
                json.loads(line)
                for line in escaped_telemetry_path.read_text(
                    encoding="utf-8"
                ).splitlines()
                if line
            ]
            resource_records = [
                record
                for record in telemetry_records
                if record.get("schema") == "katana-port-build-resource"
            ]
            require(
                any(
                    record["resource"]["tree_registered"]
                    and not record["resource"]["tree_query_complete"]
                    and record["resource"]["processes_quality"]
                    == "subreaper-descendant-tree-sampled"
                    for record in resource_records
                ),
                "telemetry never sampled an escaped setsid descendant",
            )
            host_records = [
                record
                for record in telemetry_records
                if record.get("schema") == "katana-port-build-host-command"
            ]
            require(
                len(host_records) == 1
                and host_records[0]["host_command"]["process_tree_scope"]
                == "subreaper-descendant-tree"
                and host_records[0]["host_command"][
                    "process_tree_query_complete"
                ],
                "host quiescence and sampled resource quality were conflated",
            )

        zero = subprocess.run(
            [
                str(katana_cli),
                "__host-supervision-probe",
                "0",
                host_command([sys.executable, "--version"]),
            ],
            check=False,
            capture_output=True,
            text=True,
            timeout=8,
        )
        require(
            zero.returncode != 0 and "15 Minuten" in zero.stderr,
            "zero still enables an unbounded host command",
        )

        event_root = directory / "archive-events"
        event_root.mkdir()
        archive_wrapper = directory / (
            "katana-host-archive-wrapper.exe"
            if os.name == "nt"
            else "katana-host-archive-wrapper"
        )
        shutil.copy2(katana_cli, archive_wrapper)
        wrapper_environment = dict(os.environ)
        wrapper_environment["KATANA_HOST_BUILD_EVENT_ROOT"] = str(
            event_root
        )
        wrapper_environment.pop("KATANA_HOST_BUILD_REAL_ARCHIVER", None)
        ninja_archive_artifact = directory / "ninja-archive-mode.txt"
        ninja_mode = subprocess.run(
            [
                str(archive_wrapper),
                sys.executable,
                str(helper),
                "environment",
                str(ninja_archive_artifact),
                "0",
                "unused",
            ],
            check=False,
            capture_output=True,
            text=True,
            timeout=8,
            env=wrapper_environment,
        )
        require(ninja_mode.returncode == 0, ninja_mode.stderr)
        require(
            ninja_archive_artifact.is_file(),
            "Ninja archive prefix did not consume argv[1] as real tool",
        )

        wrapper_environment["KATANA_HOST_BUILD_REAL_ARCHIVER"] = (
            sys.executable
        )
        vs_archive_artifact = directory / "vs-archive-mode.txt"
        vs_mode = subprocess.run(
            [
                str(archive_wrapper),
                str(helper),
                "environment",
                str(vs_archive_artifact),
                "0",
                "unused",
            ],
            check=False,
            capture_output=True,
            text=True,
            timeout=8,
            env=wrapper_environment,
        )
        require(vs_mode.returncode == 0, vs_mode.stderr)
        require(
            vs_archive_artifact.is_file(),
            "VS archive replacement forwarded a fake original-tool arg",
        )

        if os.name == "nt":
            bang_directory = directory / "bang!workspace"
            bang_directory.mkdir()
            bang_helper = bang_directory / "descendant!.py"
            shutil.copy2(helper, bang_helper)
            bang_artifact = bang_directory / "path!artifact.txt"
            bang_command = host_command(
                [
                    sys.executable,
                    str(bang_helper),
                    "path-environment",
                    str(bang_artifact),
                    "0",
                    "unused",
                ]
            )
            bang_environment = dict(os.environ)
            bang_environment["PATH"] = (
                str(bang_directory)
                + os.pathsep
                + bang_environment.get("PATH", "")
            )
            bang_probe = subprocess.run(
                [
                    str(katana_cli),
                    "__host-supervision-probe",
                    "3000",
                    bang_command,
                ],
                check=False,
                capture_output=True,
                text=True,
                timeout=8,
                env=bang_environment,
            )
            require(bang_probe.returncode == 0, bang_probe.stderr)
            require(
                bang_artifact.is_file()
                and str(bang_directory).lower()
                in bang_artifact.read_text(encoding="utf-8").lower(),
                "'!' in PATH, workspace or host argument was expanded away",
            )

            percent_variable = "KATANA_PERCENT_SENTINEL"
            percent_directory = (
                directory / f"percent%{percent_variable}%!workspace"
            )
            percent_directory.mkdir()
            percent_helper = (
                percent_directory / f"descendant%{percent_variable}%!.py"
            )
            shutil.copy2(helper, percent_helper)
            percent_artifact = (
                percent_directory / f"artifact%{percent_variable}%!.txt"
            )
            percent_marker = f"%{percent_variable}%!_ARGUMENT"
            percent_child = host_command(
                [
                    sys.executable,
                    str(percent_helper),
                    "percent-environment",
                    str(percent_artifact),
                    "0",
                    percent_marker,
                ]
            )
            percent_command = (
                f'set "KATANA_PERCENT_VALUE={percent_directory}" && '
                + percent_child
            )
            percent_environment = dict(os.environ)
            percent_environment[percent_variable] = "EXPANDED_AWAY"
            spoofed_artifact = directory / "spoofed-raw-command.txt"
            percent_environment["katana_raw_host_command"] = host_command(
                [
                    sys.executable,
                    str(helper),
                    "child",
                    str(spoofed_artifact),
                    "0",
                    "SPOOFED",
                ]
            )
            percent_probe = subprocess.run(
                [
                    str(katana_cli),
                    "__host-supervision-probe",
                    "3000",
                    percent_command,
                ],
                check=False,
                capture_output=True,
                text=True,
                timeout=8,
                env=percent_environment,
            )
            require(percent_probe.returncode == 0, percent_probe.stderr)
            require(
                percent_artifact.is_file()
                and percent_artifact.read_text(encoding="utf-8")
                == str(percent_directory)
                and percent_marker in percent_probe.stdout,
                "'%' in a SET value, workspace or argument was not byte-stable",
            )
            require(
                not spoofed_artifact.exists(),
                "inherited case-variant raw-command transport was executed",
            )

            environment_artifact = directory / "msvc-environment.txt"
            environment_command = host_command(
                [
                    sys.executable,
                    str(helper),
                    "environment",
                    str(environment_artifact),
                    "0",
                    "unused",
                ]
            )
            probe_environment = dict(os.environ)
            probe_environment["CL"] = "/DKEEP_PRIMARY=1 /MP2"
            probe_environment["_CL_"] = "/DKEEP_TRAILING=1 /MP99"
            normalized = subprocess.run(
                [
                    str(katana_cli),
                    "__host-supervision-probe",
                    "3000",
                    environment_command,
                    "7",
                ],
                check=False,
                capture_output=True,
                text=True,
                timeout=8,
                env=probe_environment,
            )
            require(normalized.returncode == 0, normalized.stderr)
            primary, trailing = environment_artifact.read_text(
                encoding="utf-8"
            ).splitlines()
            require(
                "/DKEEP_PRIMARY=1" in primary
                and "/DKEEP_TRAILING=1" in trailing,
                "existing CL/_CL_ flags were overwritten",
            )
            require(
                "/MP2" not in primary
                and "/MP99" not in trailing
                and trailing.split().count("/MP7") == 1,
                "conflicting /MP options were not normalized to exact N",
            )

            chrono_source = directory / "chrono_probe.cpp"
            chrono_executable = directory / "chrono_probe.exe"
            chrono_source.write_text(
                "#include <chrono>\n"
                "int main(){return std::chrono::steady_clock::now() < "
                "std::chrono::steady_clock::time_point{};}\n",
                encoding="utf-8",
            )
            compile_environment = dict(probe_environment)
            compile_environment.pop("INCLUDE", None)
            compile_environment.pop("LIB", None)
            compiled = subprocess.run(
                [
                    str(katana_cli),
                    "__host-msvc-environment-probe",
                    str(chrono_source),
                    str(chrono_executable),
                    "7",
                ],
                check=False,
                capture_output=True,
                text=True,
                timeout=150,
                env=compile_environment,
            )
            require(compiled.returncode == 0, compiled.stderr)
            require(
                chrono_executable.is_file(),
                "VsDevCmd binding did not compile/link <chrono>",
            )

            percent_compile_source = (
                percent_directory /
                f"chrono%{percent_variable}%!probe.cpp"
            )
            percent_compile_source.write_text(
                "int main(){return 0;}\n", encoding="utf-8"
            )
            percent_compile_executable = (
                percent_directory /
                f"chrono%{percent_variable}%!probe.exe"
            )
            percent_compile = subprocess.run(
                [
                    str(katana_cli),
                    "__host-msvc-environment-probe",
                    str(percent_compile_source),
                    str(percent_compile_executable),
                    "7",
                ],
                check=False,
                capture_output=True,
                text=True,
                timeout=30,
                env=compile_environment,
            )
            require(
                percent_compile.returncode == 0
                and percent_compile_executable.is_file(),
                "VsDevCmd did not preserve a literal '%'/'!' compile/link path",
            )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", type=pathlib.Path, required=True)
    parser.add_argument("--katana-cli", type=pathlib.Path, required=True)
    arguments = parser.parse_args()

    source_root = arguments.source_root.resolve()
    main_source = (source_root / "src/cli/main.cpp").read_text(
        encoding="utf-8"
    )
    telemetry_header = (
        source_root / "src/cli/port_build_telemetry.hpp"
    ).read_text(encoding="utf-8")
    telemetry_source = (
        source_root / "src/cli/port_build_telemetry.cpp"
    ).read_text(encoding="utf-8")

    require(
        "WEXITED | WNOHANG | WNOWAIT" in main_source,
        "direct child is not retained with waitid(WNOWAIT)",
    )
    require(
        main_source.count("::wait4(") == 2
        and "const auto wait_options =" in main_source
        and "root_state == PosixRootState::Terminal ? 0 : WNOHANG"
        in main_source
        and "!hard_group_kill_sent_" in main_source,
        "wait4 has an unguarded or unexpected early-reap path",
    )
    require(
        "probe_posix_process_group(child_, child_)" in main_source
        and "process_tree_quiescent_ = true;" in main_source,
        "quiescence is not backed by an OS process-group probe",
    )
    probe = main_source.index("probe_posix_process_group(child_, child_)")
    reap = main_source.index(
        "::wait4(child_, &status_, wait_options, &usage)", probe
    )
    proof = main_source.index("process_tree_quiescent_ = true;", reap)
    require(
        probe < reap < proof,
        "direct child is reaped before group proof or result publication",
    )
    require(
        "::wait4(process, &status, WNOHANG, &usage)" in main_source
        and "accumulate_final_usage(usage);" in main_source
        and "update_posix_process_tree_members" in main_source
        and "refresh_descendant_telemetry()" in main_source
        and "Linux-Descendant-Telemetrie" in main_source,
        "Linux escaped descendants are not sampled and cumulatively reaped",
    )
    require(
        "fallback_group = ::getpgid(candidate_pid)" in main_source
        and "if (errno == ESRCH) continue;" in main_source,
        "ordinary disappearing /proc PIDs do not have a safe getpgid fallback",
    )
    require(
        "~PosixChildSupervisor() noexcept" in main_source
        and "terminate_and_reap(SIGKILL)" in main_source,
        "exception paths do not retain an RAII kill/reap failsafe",
    )
    require(
        "const char launch_state = group_ready ? 'R' : 'E';" in main_source
        and "launch_commit != 'C'" in main_source
        and main_source.index("launch_state = group_ready")
        < main_source.index("launch_commit != 'C'")
        < main_source.index('::execl("/bin/sh"')
        and main_source.index("PosixChildSupervisor supervisor(")
        < main_source.index("commit_written = ::write("),
        "child can exec before two-way group supervision commit",
    )
    require(
        "class PosixUncommittedChildGuard" in main_source
        and "uncommitted_child.terminate_and_reap()" in main_source,
        "pre-commit launch failures have no direct-child RAII cleanup proof",
    )
    require(
        "PR_SET_CHILD_SUBREAPER" in main_source
        and "probe_linux_descendants" in main_source
        and "reap_terminal_linux_adoptees" in main_source
        and "empty_descendant_observations_" in main_source,
        "Linux setsid/setpgid escape is not contained by a subreaper proof",
    )

    handler = re.search(
        r"void capture_posix_host_signal\([^)]*\) noexcept \{(?P<body>.*?)\n\}",
        main_source,
        re.DOTALL,
    )
    require(handler is not None, "SIGINT/SIGTERM capture handler is absent")
    handler_body = handler.group("body")
    require(
        "posix_host_pending_signal" in handler_body
        and not re.search(
            r"\b(kill|wait|sleep|lock|mutex|new|delete|throw)\b", handler_body
        ),
        "signal handler performs work beyond sig_atomic_t capture",
    )
    require(
        "forwarding_signals.pending_signal()" in main_source
        and "supervisor.terminate_and_reap(" in main_source,
        "captured termination signals are not forwarded by the supervisor loop",
    )

    require(
        "katana-port-build-host-command" in telemetry_header
        and "record_host_command" in telemetry_source
        and "host_exit_code" in telemetry_source
        and "process_tree_quiescent" in telemetry_source,
        "host exit/timeout/quiescence lacks a separate critical record",
    )
    require(
        "class SupervisedHostCommandTelemetryAttempt" in main_source
        and "~SupervisedHostCommandTelemetryAttempt() noexcept" in main_source
        and main_source.count("telemetry_->record_host_command(") == 1,
        "host-command throw paths do not emit exactly one RAII result record",
    )
    require(
        "ru_inblock" in main_source
        and "ru_oublock" in main_source
        and "io_input_blocks" in telemetry_header
        and "io_output_blocks" in telemetry_header
        and "wait4-cumulative-block-count" in telemetry_source
        and "sampled-procfs" in telemetry_source,
        "portable wait4 block accounting is absent or mixed with sampled bytes",
    )
    require(
        "set_failure_exit_code(124" not in main_source
        and "failure_exit_code_" not in main_source,
        "host timeout 124 can still overwrite the actual CLI terminal exit",
    )
    require(
        "maximum_port_host_command_runtime =\n    std::chrono::minutes(15)"
        in main_source
        and 'if (*value == "unlimited") return std::nullopt' in main_source
        and "if (timeout &&" in main_source
        and "timeout->count() <= 0" in main_source,
        "host commands lack the explicit no-limit grant or can exceed the default cap",
    )
    require(
        "cmd.exe /d /v:off /s /c" in main_source
        and "!PATH!" not in main_source
        and "KATANA_RAW_HOST_COMMAND" in main_source
        and "raw_host_command.find('\\0')" in main_source
        and "variable_matches(entry, raw_command_name)" in main_source
        and "windows_child_environment(windows_cl_jobs, command)"
        in main_source
        and 'prefix = "call \\\""' not in main_source,
        "Windows host commands do not preserve literal !/% values through "
        "a deduplicated child environment",
    )
    require(
        "subreaper-descendant-tree-sampled" in telemetry_source
        and "retired_posix_descendants_present_" in telemetry_source
        and "--telemetry-jsonl" in main_source,
        "escaped Linux descendants have no telemetry-enabled product path",
    )
    require(
        "try_natural_quiescent_reap" in main_source
        and "root_terminal && empty.empty" in main_source,
        "root termination is still accepted before tree quiescence",
    )
    require(
        "mark_upstream_incomplete" in telemetry_header
        and "upstream_incomplete" in telemetry_source
        and "upstream_incomplete_reason" in telemetry_source
        and main_source.count("reporter_.dropped_observations()") == 2,
        "failed progress seals are not bridged losslessly into terminal telemetry",
    )

    claims = list(re.finditer(r"process_tree=terminated", main_source))
    require(len(claims) == 3, "unexpected terminated-claim count")
    for claim in claims:
        preceding_contract = main_source[max(0, claim.start() - 900) : claim.start()]
        require(
            "process_tree_quiescent" in preceding_contract,
            "a caller claims termination without checking proven quiescence",
        )

    run_product_regressions(arguments.katana_cli.resolve())
    print("host process supervision source and product contract: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
