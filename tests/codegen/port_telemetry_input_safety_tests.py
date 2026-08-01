#!/usr/bin/env python3
"""Prove that opt-in port telemetry cannot clobber NativeDisc inputs."""

from __future__ import annotations

import argparse
import hashlib
import os
import pathlib
import subprocess
import sys
import tempfile


def digest(path: pathlib.Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def run_rejected(
    cli: pathlib.Path,
    source_root: pathlib.Path,
    source: pathlib.Path,
    output: pathlib.Path,
    telemetry: pathlib.Path,
    protected: tuple[pathlib.Path, ...],
    expected_diagnostic: str,
) -> None:
    before = {path: digest(path) for path in protected}
    environment = os.environ.copy()
    environment["KATANA_RUNTIME_ROOT"] = str(source_root)
    environment["KATANA_PORT_WORKSPACE_ROOT"] = str(output.parent / "workspace")
    environment["KATANA_PORT_PUBLISH_TEST_EXIT_AFTER_RECOVERY"] = "1"
    completed = subprocess.run(
        [
            str(cli),
            "port",
            str(source),
            "--output",
            str(output),
            "--target-name",
            "telemetry_input_safety",
            "--telemetry-jsonl",
            str(telemetry),
        ],
        cwd=source_root,
        env=environment,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=30,
    )
    require(
        completed.returncode != 0,
        f"Telemetriealias wurde akzeptiert: {telemetry}\n{completed.stdout}",
    )
    require(
        expected_diagnostic in completed.stdout,
        "Telemetriealias besitzt keine stabile Diagnose "
        f"'{expected_diagnostic}':\n{completed.stdout}",
    )
    for path, expected in before.items():
        require(
            path.is_file() and digest(path) == expected,
            f"Telemetriealias hat Eingabebytes veraendert: {path}",
        )
    require(
        not output.exists(),
        f"Abgelehnter Telemetriealias erreichte den Port-Publish: {output}",
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--writer", required=True, type=pathlib.Path)
    parser.add_argument("--katana-cli", required=True, type=pathlib.Path)
    parser.add_argument("--source-root", required=True, type=pathlib.Path)
    arguments = parser.parse_args()
    for label, path in (
        ("writer", arguments.writer),
        ("katana CLI", arguments.katana_cli),
        ("source root", arguments.source_root),
    ):
        require(path.exists(), f"{label} fehlt: {path}")

    with tempfile.TemporaryDirectory(
        prefix="katana-port-telemetry-input-safety-"
    ) as temporary:
        root = pathlib.Path(temporary)
        fixture = root / "fixture"
        subprocess.run(
            [
                sys.executable,
                "-B",
                str(arguments.writer),
                "--profile",
                "smoke",
                "--output",
                str(fixture),
            ],
            check=True,
            timeout=30,
        )
        descriptor = fixture / "disc.gdi"
        track = fixture / "track03.bin"

        run_rejected(
            arguments.katana_cli,
            arguments.source_root,
            descriptor,
            root / "source-alias-port",
            descriptor,
            (descriptor,),
            "Portquelle",
        )
        run_rejected(
            arguments.katana_cli,
            arguments.source_root,
            descriptor,
            root / "track-alias-port",
            track,
            (descriptor, track),
            "GDI-Track",
        )

        hardlink = root / "track-hardlink.jsonl"
        os.link(track, hardlink)
        run_rejected(
            arguments.katana_cli,
            arguments.source_root,
            descriptor,
            root / "hardlink-alias-port",
            hardlink,
            (descriptor, track, hardlink),
            "Hardlink",
        )

        writer_lock_target = root / "writer-lock-hardlink.jsonl"
        writer_lock = pathlib.Path(
            str(writer_lock_target) + ".katana-telemetry-writer.lock"
        )
        os.link(track, writer_lock)
        run_rejected(
            arguments.katana_cli,
            arguments.source_root,
            descriptor,
            root / "writer-lock-hardlink-port",
            writer_lock_target,
            (descriptor, track, writer_lock),
            "GDI-Track",
        )

        lock_output = root / "lock-alias-port"
        run_rejected(
            arguments.katana_cli,
            arguments.source_root,
            descriptor,
            lock_output,
            pathlib.Path(str(lock_output) + ".katana-publish.lock"),
            (descriptor, track),
            "Port-Publish-Sperre",
        )

        if os.name == "nt":
            device_output = root / "device-port"
            environment = os.environ.copy()
            environment["KATANA_RUNTIME_ROOT"] = str(arguments.source_root)
            environment["KATANA_PORT_WORKSPACE_ROOT"] = str(root / "workspace")
            environment["KATANA_PORT_PUBLISH_TEST_EXIT_AFTER_RECOVERY"] = "1"
            completed = subprocess.run(
                [
                    str(arguments.katana_cli),
                    "port",
                    str(descriptor),
                    "--output",
                    str(device_output),
                    "--target-name",
                    "telemetry_device_safety",
                    "--telemetry-jsonl",
                    str(root / "NUL"),
                ],
                cwd=arguments.source_root,
                env=environment,
                check=False,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                encoding="utf-8",
                errors="replace",
                timeout=30,
            )
            require(
                completed.returncode != 0
                and "Telemetrie" in completed.stdout
                and not device_output.exists()
                and not any(path.name.startswith("NUL") for path in root.iterdir()),
                "Windows-Geraetepfad wurde als Telemetriedatei behandelt:\n"
                + completed.stdout,
            )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
