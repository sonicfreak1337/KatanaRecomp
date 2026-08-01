#!/usr/bin/env python3
"""Exercise the public KR-4974 NativeDisc stress fixture end to end."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import signal
import subprocess
import sys
import tempfile
import threading
import time
from dataclasses import dataclass


class ProcessTreeError(RuntimeError):
    def __init__(self, message: str, output: str, returncode: int):
        super().__init__(message)
        self.output = output
        self.returncode = returncode


class WindowsJobSupervisor:
    """Own a suspended root from pre-exec job binding through quiescence."""

    def __init__(self, process: subprocess.Popen[str]):
        import ctypes
        from ctypes import wintypes

        class IoCounters(ctypes.Structure):
            _fields_ = [
                (name, ctypes.c_ulonglong)
                for name in (
                    "ReadOperationCount",
                    "WriteOperationCount",
                    "OtherOperationCount",
                    "ReadTransferCount",
                    "WriteTransferCount",
                    "OtherTransferCount",
                )
            ]

        class BasicLimitInformation(ctypes.Structure):
            _fields_ = [
                ("PerProcessUserTimeLimit", ctypes.c_longlong),
                ("PerJobUserTimeLimit", ctypes.c_longlong),
                ("LimitFlags", wintypes.DWORD),
                ("MinimumWorkingSetSize", ctypes.c_size_t),
                ("MaximumWorkingSetSize", ctypes.c_size_t),
                ("ActiveProcessLimit", wintypes.DWORD),
                ("Affinity", ctypes.c_size_t),
                ("PriorityClass", wintypes.DWORD),
                ("SchedulingClass", wintypes.DWORD),
            ]

        class ExtendedLimitInformation(ctypes.Structure):
            _fields_ = [
                ("BasicLimitInformation", BasicLimitInformation),
                ("IoInfo", IoCounters),
                ("ProcessMemoryLimit", ctypes.c_size_t),
                ("JobMemoryLimit", ctypes.c_size_t),
                ("PeakProcessMemoryUsed", ctypes.c_size_t),
                ("PeakJobMemoryUsed", ctypes.c_size_t),
            ]

        class BasicAccountingInformation(ctypes.Structure):
            _fields_ = [
                ("TotalUserTime", ctypes.c_longlong),
                ("TotalKernelTime", ctypes.c_longlong),
                ("ThisPeriodTotalUserTime", ctypes.c_longlong),
                ("ThisPeriodTotalKernelTime", ctypes.c_longlong),
                ("TotalPageFaultCount", wintypes.DWORD),
                ("TotalProcesses", wintypes.DWORD),
                ("ActiveProcesses", wintypes.DWORD),
                ("TotalTerminatedProcesses", wintypes.DWORD),
            ]

        class ThreadEntry32(ctypes.Structure):
            _fields_ = [
                ("dwSize", wintypes.DWORD),
                ("cntUsage", wintypes.DWORD),
                ("th32ThreadID", wintypes.DWORD),
                ("th32OwnerProcessID", wintypes.DWORD),
                ("tpBasePri", wintypes.LONG),
                ("tpDeltaPri", wintypes.LONG),
                ("dwFlags", wintypes.DWORD),
            ]

        self._ctypes = ctypes
        self._wintypes = wintypes
        self._accounting_type = BasicAccountingInformation
        self._thread_entry_type = ThreadEntry32
        self._process = process
        self._job: int | None = None
        self._closed = False
        self._assigned = False
        self._kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        api = self._kernel32
        api.CreateJobObjectW.argtypes = [ctypes.c_void_p, wintypes.LPCWSTR]
        api.CreateJobObjectW.restype = wintypes.HANDLE
        api.SetInformationJobObject.argtypes = [
            wintypes.HANDLE,
            ctypes.c_int,
            ctypes.c_void_p,
            wintypes.DWORD,
        ]
        api.SetInformationJobObject.restype = wintypes.BOOL
        api.AssignProcessToJobObject.argtypes = [wintypes.HANDLE, wintypes.HANDLE]
        api.AssignProcessToJobObject.restype = wintypes.BOOL
        api.QueryInformationJobObject.argtypes = [
            wintypes.HANDLE,
            ctypes.c_int,
            ctypes.c_void_p,
            wintypes.DWORD,
            ctypes.POINTER(wintypes.DWORD),
        ]
        api.QueryInformationJobObject.restype = wintypes.BOOL
        api.TerminateJobObject.argtypes = [wintypes.HANDLE, wintypes.UINT]
        api.TerminateJobObject.restype = wintypes.BOOL
        api.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
        api.WaitForSingleObject.restype = wintypes.DWORD
        api.TerminateProcess.argtypes = [wintypes.HANDLE, wintypes.UINT]
        api.TerminateProcess.restype = wintypes.BOOL
        api.CreateToolhelp32Snapshot.argtypes = [wintypes.DWORD, wintypes.DWORD]
        api.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
        api.Thread32First.argtypes = [wintypes.HANDLE, ctypes.POINTER(ThreadEntry32)]
        api.Thread32First.restype = wintypes.BOOL
        api.Thread32Next.argtypes = [wintypes.HANDLE, ctypes.POINTER(ThreadEntry32)]
        api.Thread32Next.restype = wintypes.BOOL
        api.OpenThread.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
        api.OpenThread.restype = wintypes.HANDLE
        api.ResumeThread.argtypes = [wintypes.HANDLE]
        api.ResumeThread.restype = wintypes.DWORD
        api.CloseHandle.argtypes = [wintypes.HANDLE]
        api.CloseHandle.restype = wintypes.BOOL

        try:
            job = api.CreateJobObjectW(None, None)
            if not job:
                self._raise_last_error("CreateJobObjectW failed")
            self._job = int(job)
            limits = ExtendedLimitInformation()
            limits.BasicLimitInformation.LimitFlags = 0x00002000
            if not api.SetInformationJobObject(
                wintypes.HANDLE(self._job),
                9,
                ctypes.byref(limits),
                ctypes.sizeof(limits),
            ):
                self._raise_last_error("SetInformationJobObject failed")
            thread = self._open_primary_thread()
            try:
                if not api.AssignProcessToJobObject(
                    wintypes.HANDLE(self._job),
                    wintypes.HANDLE(process._handle),
                ):
                    self._raise_last_error("AssignProcessToJobObject failed")
                self._assigned = True
                previous_suspend_count = api.ResumeThread(wintypes.HANDLE(thread))
                if previous_suspend_count == 0xFFFFFFFF or previous_suspend_count == 0:
                    self._raise_last_error("ResumeThread failed")
            finally:
                self._close_raw_handle(thread)
        except BaseException:
            self._cleanup_failed_launch()
            raise

    def _raise_last_error(self, message: str) -> None:
        error = self._ctypes.get_last_error()
        raise OSError(error, message)

    def _close_raw_handle(self, handle: int) -> None:
        if not self._kernel32.CloseHandle(self._wintypes.HANDLE(handle)):
            self._raise_last_error("CloseHandle failed")

    def _open_primary_thread(self) -> int:
        snapshot = self._kernel32.CreateToolhelp32Snapshot(0x00000004, 0)
        invalid = self._ctypes.c_void_p(-1).value
        if not snapshot or int(snapshot) == invalid:
            self._raise_last_error("CreateToolhelp32Snapshot failed")
        snapshot_value = int(snapshot)
        thread_id: int | None = None
        try:
            entry = self._thread_entry_type()
            entry.dwSize = self._ctypes.sizeof(entry)
            if not self._kernel32.Thread32First(
                self._wintypes.HANDLE(snapshot_value), self._ctypes.byref(entry)
            ):
                self._raise_last_error("Thread32First failed")
            while True:
                if entry.th32OwnerProcessID == self._process.pid:
                    thread_id = int(entry.th32ThreadID)
                    break
                if not self._kernel32.Thread32Next(
                    self._wintypes.HANDLE(snapshot_value),
                    self._ctypes.byref(entry),
                ):
                    break
        finally:
            self._close_raw_handle(snapshot_value)
        if thread_id is None:
            raise OSError("suspended process exposed no primary thread")
        thread = self._kernel32.OpenThread(0x0002, False, thread_id)
        if not thread:
            self._raise_last_error("OpenThread failed")
        return int(thread)

    def _cleanup_failed_launch(self) -> None:
        try:
            if self._assigned and self._job is not None:
                if not self._kernel32.TerminateJobObject(
                    self._wintypes.HANDLE(self._job), 125
                ):
                    self._raise_last_error("TerminateJobObject failed")
                if not self.wait_for_quiescence(5_000):
                    raise OSError(
                        "Windows job did not quiesce after a failed launch"
                    )
            else:
                if not self._kernel32.TerminateProcess(
                    self._wintypes.HANDLE(self._process._handle), 125
                ):
                    self._raise_last_error("TerminateProcess failed")
                wait = self._kernel32.WaitForSingleObject(
                    self._wintypes.HANDLE(self._process._handle), 5_000
                )
                if wait != 0:
                    raise OSError(
                        "Windows root process did not terminate after a failed launch"
                    )
            self._process.wait(timeout=5)
        finally:
            if self._job is not None:
                try:
                    self._close_raw_handle(self._job)
                finally:
                    self._job = None
                    self._closed = True

    def root_terminal(self, milliseconds: int = 0) -> bool:
        result = self._kernel32.WaitForSingleObject(
            self._wintypes.HANDLE(self._process._handle), milliseconds
        )
        if result == 0:
            return True
        if result == 258:
            return False
        self._raise_last_error("WaitForSingleObject(root) failed")
        return False

    def active_processes(self) -> int:
        if self._job is None:
            raise RuntimeError("job handle is already closed")
        information = self._accounting_type()
        returned = self._wintypes.DWORD()
        if not self._kernel32.QueryInformationJobObject(
            self._wintypes.HANDLE(self._job),
            1,
            self._ctypes.byref(information),
            self._ctypes.sizeof(information),
            self._ctypes.byref(returned),
        ):
            self._raise_last_error("QueryInformationJobObject failed")
        if returned.value != self._ctypes.sizeof(information):
            raise OSError("QueryInformationJobObject returned a partial record")
        return int(information.ActiveProcesses)

    def wait_for_quiescence(self, milliseconds: int) -> bool:
        deadline = time.monotonic() + milliseconds / 1_000.0
        while self.active_processes() != 0:
            if time.monotonic() >= deadline:
                return False
            time.sleep(0.01)
        return True

    def terminate_and_prove_quiescence(self, exit_code: int) -> None:
        if self._job is None:
            raise RuntimeError("job handle is already closed")
        if self.active_processes() != 0 and not self._kernel32.TerminateJobObject(
            self._wintypes.HANDLE(self._job), exit_code
        ):
            self._raise_last_error("TerminateJobObject failed")
        if not self.wait_for_quiescence(5_000):
            raise OSError("Windows job did not reach ActiveProcesses == 0")
        self._process.wait(timeout=5)

    def abort_and_close(self, exit_code: int) -> None:
        """Best-effort exceptional cleanup with KILL_ON_JOB_CLOSE fallback."""
        if self._closed:
            return
        try:
            if self._job is None:
                raise RuntimeError("job handle is unavailable")
            if not self._kernel32.TerminateJobObject(
                self._wintypes.HANDLE(self._job), exit_code
            ):
                self._raise_last_error("TerminateJobObject failed")
            if not self.wait_for_quiescence(5_000):
                raise OSError("Windows job did not quiesce during cleanup")
            self._process.wait(timeout=5)
        finally:
            # Closing a job configured with KILL_ON_JOB_CLOSE is the last
            # fail-closed containment boundary even if accounting itself has
            # failed. Never leak that ownership handle on an observer error.
            if self._job is not None:
                try:
                    self._close_raw_handle(self._job)
                finally:
                    self._job = None
                    self._closed = True

    def close(self) -> None:
        if self._closed:
            return
        if self._job is None:
            raise RuntimeError("job handle is unavailable")
        if self.active_processes() != 0:
            raise OSError("refusing to close a non-quiescent Windows job")
        self._close_raw_handle(self._job)
        self._job = None
        self._closed = True


def posix_root_terminal(process: subprocess.Popen[str]) -> bool:
    information = os.waitid(
        os.P_PID, process.pid, os.WEXITED | os.WNOHANG | os.WNOWAIT
    )
    return information is not None and information.si_pid == process.pid


def posix_group_members(process_group: int) -> set[int]:
    proc_root = pathlib.Path("/proc")
    if proc_root.is_dir():
        members: set[int] = set()
        for entry in proc_root.iterdir():
            if not entry.name.isdecimal():
                continue
            try:
                stat = (entry / "stat").read_text(encoding="ascii")
                tail = stat[stat.rfind(")") + 2 :].split()
                if len(tail) >= 3 and int(tail[2]) == process_group:
                    members.add(int(entry.name))
            except (FileNotFoundError, PermissionError, ValueError):
                continue
        return members
    captured = subprocess.run(
        ["ps", "-e", "-o", "pid=", "-o", "pgid="],
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        errors="replace",
        check=False,
        timeout=10,
    )
    if captured.returncode != 0:
        raise OSError("could not inspect the POSIX process group")
    members = set()
    for line in captured.stdout.splitlines():
        fields = line.split()
        if len(fields) == 2 and int(fields[1]) == process_group:
            members.add(int(fields[0]))
    return members


def terminate_posix_group_and_reap(
    process: subprocess.Popen[str], initial_signal: signal.Signals
) -> None:
    try:
        os.killpg(process.pid, initial_signal)
    except ProcessLookupError:
        pass
    grace_deadline = time.monotonic() + (0.0 if initial_signal == signal.SIGKILL else 2.0)
    kill_deadline = grace_deadline + 5.0
    hard_kill_sent = initial_signal == signal.SIGKILL
    while True:
        terminal = process.returncode is not None or posix_root_terminal(process)
        members = posix_group_members(process.pid)
        if terminal and not (members - {process.pid}):
            if process.returncode is None:
                process.wait(timeout=5)
            return
        now = time.monotonic()
        if now >= grace_deadline and not hard_kill_sent:
            os.killpg(process.pid, signal.SIGKILL)
            hard_kill_sent = True
        if now >= kill_deadline:
            raise OSError("POSIX process group did not become quiescent")
        time.sleep(0.02)


def run(
    command: list[str],
    timeout: float = 600,
    environment: dict[str, str] | None = None,
    stage_name: str | None = None,
) -> str:
    label = stage_name or pathlib.Path(command[0]).name
    print(f"[start] stage={label}", flush=True)
    started = time.monotonic()
    creation_flags = 0
    popen_options: dict[str, object] = {}
    if os.name == "nt":
        creation_flags = subprocess.CREATE_NEW_PROCESS_GROUP | 0x00000004
    else:
        popen_options["start_new_session"] = True
    try:
        process = subprocess.Popen(
            command,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            stdin=subprocess.DEVNULL,
            text=True,
            encoding="utf-8",
            errors="replace",
            env=environment,
            creationflags=creation_flags,
            **popen_options,
        )
    except OSError as error:
        raise SystemExit(f"command could not start: {command[0]}: {error}") from error
    windows_supervisor: WindowsJobSupervisor | None = None
    try:
        if os.name == "nt":
            windows_supervisor = WindowsJobSupervisor(process)
    except OSError as error:
        raise SystemExit(f"process tree supervision could not start: {error}") from error

    lines: list[str] = []
    reader_errors: list[BaseException] = []

    def read_output() -> None:
        try:
            assert process.stdout is not None
            for line in process.stdout:
                lines.append(line)
                print(line, end="", flush=True)
        except BaseException as error:
            reader_errors.append(error)

    reader = threading.Thread(target=read_output, name="stress-output", daemon=True)
    tree_finalized = False
    try:
        reader.start()
        next_heartbeat = started + 5.0
        timed_out = False
        while True:
            if windows_supervisor is not None:
                tree_quiescent = windows_supervisor.active_processes() == 0
            else:
                terminal = posix_root_terminal(process)
                members = posix_group_members(process.pid)
                tree_quiescent = terminal and not (members - {process.pid})
            if tree_quiescent:
                process.wait(timeout=5)
                tree_finalized = True
                break
            now = time.monotonic()
            if now >= next_heartbeat:
                print(
                    f"[heartbeat] stage={label} pid={process.pid} "
                    f"elapsed={now - started:.1f}s",
                    flush=True,
                )
                next_heartbeat = now + 5.0
            if now - started > timeout:
                timed_out = True
                if windows_supervisor is not None:
                    windows_supervisor.terminate_and_prove_quiescence(124)
                else:
                    terminate_posix_group_and_reap(process, signal.SIGKILL)
                tree_finalized = True
                break
            time.sleep(0.05)

        reader.join(timeout=5)
        if reader.is_alive():
            if process.stdout is not None:
                process.stdout.close()
            reader.join(timeout=5)
            raise OSError(f"stdout reader did not quiesce: {command[0]}")
        if reader_errors:
            raise OSError(f"stdout reader failed: {command[0]}") from reader_errors[0]
        if windows_supervisor is not None:
            windows_supervisor.close()
            windows_supervisor = None
        if timed_out:
            raise ProcessTreeError(
                f"process tree timed out after {timeout}s with exit_code=124: {command[0]}",
                "".join(lines),
                124,
            )
        elapsed = time.monotonic() - started
        print(
            f"[done] stage={label} rc={process.returncode} "
            f"elapsed={elapsed:.3f}s",
            flush=True,
        )
        if process.returncode != 0:
            raise SystemExit(process.returncode)
        return "".join(lines)
    except BaseException as primary_error:
        cleanup_error: BaseException | None = None
        if windows_supervisor is not None:
            try:
                windows_supervisor.abort_and_close(125)
            except BaseException as error:
                cleanup_error = error
        elif not tree_finalized:
            try:
                terminate_posix_group_and_reap(process, signal.SIGKILL)
            except BaseException as error:
                cleanup_error = error
        if reader.is_alive():
            if process.stdout is not None:
                process.stdout.close()
            reader.join(timeout=5)
            if reader.is_alive() and cleanup_error is None:
                cleanup_error = OSError(
                    f"stdout reader survived exceptional cleanup: {command[0]}"
                )
        if cleanup_error is not None:
            raise cleanup_error from primary_error
        raise


def verify_process_tree_case(child_closes_stdout: bool) -> None:
    with tempfile.TemporaryDirectory(prefix="katana-process-tree-timeout-") as temporary:
        marker = pathlib.Path(temporary) / "late-child-marker.txt"
        child_code = (
            "import pathlib,time; time.sleep(1.0); "
            f"pathlib.Path({str(marker)!r}).write_text('leaked',encoding='ascii')"
        )
        child_arguments = "[sys.executable,'-c'," + repr(child_code) + "]"
        if child_closes_stdout:
            spawn = (
                "p=subprocess.Popen(" + child_arguments
                + ",stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL); "
            )
            case = "closed-stdout-child"
        else:
            spawn = "p=subprocess.Popen(" + child_arguments + "); "
            case = "inherited-stdout-child"
        leader_code = (
            "import subprocess,sys; "
            + spawn
            + "print('TREE_CHILD_PID='+str(p.pid),flush=True)"
        )
        try:
            run(
                [sys.executable, "-c", leader_code],
                timeout=0.25,
                stage_name=f"process-tree-{case}",
            )
        except ProcessTreeError as error:
            lines = [
                line
                for line in error.output.splitlines()
                if line.startswith("TREE_CHILD_PID=")
            ]
            if len(lines) != 1 or error.returncode != 124:
                raise SystemExit(
                    "process-tree timeout did not expose its child and exit 124"
                ) from error
            time.sleep(1.0)
            if marker.exists():
                raise SystemExit(
                    "process-tree timeout allowed the exact child work to escape"
                ) from error
            print(f"[ok] {case} timed out with 124 and was force-quiesced", flush=True)
            return
        raise SystemExit("process-tree timeout unexpectedly accepted a live child")


def verify_natural_post_root_child() -> None:
    with tempfile.TemporaryDirectory(prefix="katana-process-tree-natural-") as temporary:
        marker = pathlib.Path(temporary) / "natural-child-marker.txt"
        child_code = (
            "import pathlib,time; time.sleep(0.25); "
            f"pathlib.Path({str(marker)!r}).write_text('complete',encoding='ascii')"
        )
        leader_code = (
            "import subprocess,sys; "
            "p=subprocess.Popen([sys.executable,'-c',"
            + repr(child_code)
            + "],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL); "
            "print('NATURAL_CHILD_PID='+str(p.pid),flush=True)"
        )
        output = run(
            [sys.executable, "-c", leader_code],
            timeout=5,
            stage_name="process-tree-natural-post-root-child",
        )
        if (
            len(
                [
                    line
                    for line in output.splitlines()
                    if line.startswith("NATURAL_CHILD_PID=")
                ]
            )
            != 1
            or not marker.is_file()
            or marker.read_text(encoding="ascii") != "complete"
        ):
            raise SystemExit("run returned before its natural post-root child completed")
        print("[ok] natural post-root child completed before run returned", flush=True)


def verify_process_tree_supervision() -> None:
    verify_natural_post_root_child()
    verify_process_tree_case(False)
    verify_process_tree_case(True)


def host_build_environment(cxx_compiler: pathlib.Path | None) -> dict[str, str]:
    environment = os.environ.copy()
    if os.name != "nt" or cxx_compiler is None:
        return environment
    compiler_name = cxx_compiler.name.lower()
    if compiler_name not in {"cl.exe", "clang-cl.exe"} or environment.get("INCLUDE"):
        return environment
    compiler = cxx_compiler.resolve(strict=True)
    dev_commands = [
        parent / "Common7" / "Tools" / "VsDevCmd.bat"
        for parent in compiler.parents
        if (parent / "Common7" / "Tools" / "VsDevCmd.bat").is_file()
    ]
    if len(dev_commands) != 1:
        raise SystemExit("the configured compiler does not resolve one MSVC environment")
    dev_command = dev_commands[0]
    captured = subprocess.run(
        f'call "{dev_command}" -arch=x64 -host_arch=x64 >nul && set',
        shell=True,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=30,
        env=environment,
    )
    if captured.returncode != 0:
        raise SystemExit(
            "VsDevCmd failed while preparing the stress host build: "
            + captured.stdout.strip()
        )
    for line in captured.stdout.splitlines():
        if "=" not in line or line.startswith("="):
            continue
        name, value = line.split("=", 1)
        environment[name] = value
    return environment


def generated_translation_units(port_root: pathlib.Path) -> list[pathlib.Path]:
    code_root = port_root / "generated" / "code"
    if not code_root.is_dir() or code_root.is_symlink():
        raise SystemExit("generated code directory is missing or unsafe")
    units = sorted(
        path
        for path in code_root.iterdir()
        if path.is_file()
        and not path.is_symlink()
        and path.name.startswith("unit-v")
        and path.suffix == ".cpp"
    )
    cmake_path = port_root / "generated" / "CMakeLists.txt"
    if not cmake_path.is_file() or cmake_path.is_symlink():
        raise SystemExit("generated CMake source manifest is missing or unsafe")
    cmake = cmake_path.read_text(encoding="utf-8")
    if cmake.count("code/unit-v") != len(units):
        raise SystemExit("generated CMake does not bind exactly the emitted unit-v sources")
    for unit in units:
        if cmake.count(f"code/{unit.name}") != 1:
            raise SystemExit(f"generated CMake source binding drifted: {unit.name}")
    return units


def parse_stress_result(output: str) -> dict[str, object]:
    result_lines = [
        line
        for line in output.splitlines()
        if '"schema":"katana-native-disc-cold-build-stress-result-v2"' in line
    ]
    if len(result_lines) != 1:
        raise SystemExit("stress runner emitted no unique terminal result record")
    result = json.loads(result_lines[0])
    if not isinstance(result, dict):
        raise SystemExit("stress runner terminal result is not an object")
    return result


def regular_file_tree(root: pathlib.Path) -> dict[str, bytes]:
    files: dict[str, bytes] = {}
    for path in sorted(root.rglob("*")):
        if path.is_symlink():
            raise SystemExit(f"stress output contains a symlink: {path}")
        if path.is_file():
            files[path.relative_to(root).as_posix()] = path.read_bytes()
    if not files:
        raise SystemExit(f"stress output contains no regular files: {root}")
    return files


def sha256_regular_file(path: pathlib.Path) -> str:
    status = path.lstat()
    if not path.is_file() or path.is_symlink() or status.st_size < 0:
        raise SystemExit(f"published binding is not a regular file: {path}")
    digest = hashlib.sha256()
    with path.open("rb") as source:
        while chunk := source.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def verify_published_install_binding(
    root: pathlib.Path, target_name: str
) -> None:
    executable_name = target_name + (".exe" if os.name == "nt" else "")
    executable_path = root / executable_name
    recipe_path = root / "content" / "game.katana-install"
    manifest_path = root / "content" / "game.katana-install.json"
    if manifest_path.lstat().st_size > 1024 * 1024 or manifest_path.is_symlink():
        raise SystemExit("published install manifest is unsafe or oversized")
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise SystemExit(f"published install manifest is invalid: {error}") from error
    artifacts = manifest.get("artifacts") if isinstance(manifest, dict) else None
    by_role = {
        artifact.get("role"): artifact
        for artifact in artifacts
        if isinstance(artifact, dict) and isinstance(artifact.get("role"), str)
    } if isinstance(artifacts, list) else {}
    expected_artifacts = {
        "disc_install_recipe": (
            "game.katana-install",
            sha256_regular_file(recipe_path),
        ),
        "host_executable": (
            f"../{executable_name}",
            sha256_regular_file(executable_path),
        ),
    }
    recipe_lines = recipe_path.read_text(encoding="ascii").splitlines()
    recipe_fields = {}
    for line in recipe_lines[1:]:
        fields = line.split()
        if len(fields) == 2 and fields[0] in {"job", "content"}:
            recipe_fields[fields[0]] = fields[1]
    if not (
        isinstance(manifest, dict)
        and manifest.get("schema") == "katana-disc-install"
        and manifest.get("version") == 1
        and isinstance(artifacts, list)
        and len(artifacts) == 2
        and set(by_role) == set(expected_artifacts)
        and len(by_role) == len(artifacts)
        and manifest.get("job_generation") == recipe_fields.get("job")
        and manifest.get("content_identity") == recipe_fields.get("content")
        and all(
            by_role[role].get("path") == expected_path
            and by_role[role].get("sha256") == expected_sha256
            for role, (expected_path, expected_sha256) in expected_artifacts.items()
        )
    ):
        raise SystemExit(
            "published install manifest is not bound to its exact recipe and executable"
        )


def canonical_port_result_tree(
    root: pathlib.Path, target_name: str
) -> dict[str, bytes]:
    """Return analysis/export results, excluding host-linker identity bytes."""
    files = regular_file_tree(root)
    executable = target_name + (".exe" if os.name == "nt" else "")
    derived_install_manifest = "content/game.katana-install.json"
    for relative in (executable, derived_install_manifest):
        if relative not in files:
            raise SystemExit(
                f"published port omitted telemetry-canonicality binding: {relative}"
            )
        del files[relative]
    # RelWithDebInfo host executables can legitimately embed a toolchain/build-
    # directory identity; the derived install manifest consequently binds a
    # different executable digest. Everything upstream of that host-linker
    # boundary (IR-derived source, metadata, recipes and runtime contract) must
    # remain byte-identical when observational telemetry is toggled.
    if not files:
        raise SystemExit("published port contains no canonical result files")
    return files


def require_identical_file_trees(
    expected: dict[str, bytes], actual: dict[str, bytes], label: str
) -> None:
    if actual == expected:
        return
    expected_names = set(expected)
    actual_names = set(actual)
    missing = sorted(expected_names - actual_names)
    unexpected = sorted(actual_names - expected_names)
    changed = sorted(
        name
        for name in expected_names & actual_names
        if expected[name] != actual[name]
    )
    raise SystemExit(
        f"{label} file tree drifted: missing={missing} "
        f"unexpected={unexpected} changed={changed}"
    )


def require_host_product_binding(
    port_root: pathlib.Path,
    host_build: pathlib.Path,
    multi_config: bool,
    expected_units: int,
) -> pathlib.Path:
    units = generated_translation_units(port_root)
    if len(units) != expected_units:
        raise SystemExit("host-build source set changed after the real port export")
    port_cmake = (port_root / "generated" / "katana-port.cmake").read_text(
        encoding="utf-8"
    )
    if "target_link_libraries(katana_kr4974_stress PRIVATE katana_generated" not in port_cmake:
        raise SystemExit("host executable is not linked to the generated source target")
    executable_name = "katana_kr4974_stress.exe" if os.name == "nt" else "katana_kr4974_stress"
    candidates = []
    if multi_config:
        candidates.append(host_build / "RelWithDebInfo" / executable_name)
    candidates.append(host_build / executable_name)
    candidates.extend(host_build.glob(f"**/{executable_name}"))
    existing = []
    seen: set[pathlib.Path] = set()
    for candidate in candidates:
        resolved = candidate.resolve()
        if resolved in seen or not resolved.is_file():
            continue
        seen.add(resolved)
        existing.append(resolved)
    if not existing:
        raise SystemExit("host build produced no real stress executable")
    executable = existing[0]
    if executable.stat().st_size == 0:
        raise SystemExit("host build produced an empty stress executable")
    return executable


@dataclass(frozen=True)
class CliPortPass:
    name: str
    wall_ms: int
    phase_total_ms: int
    phase_durations_ms: dict[str, int]
    parallel_module_durations_ms: dict[str, int]
    output: str
    telemetry: list[dict[str, object]]


def read_complete_port_telemetry(
    path: pathlib.Path,
    expected_workers: int,
    expected_codegen_jobs: int,
    expected_requested_states: dict[str, str],
    expected_build_profile: str,
) -> list[dict[str, object]]:
    status = path.lstat()
    if not path.is_file() or path.is_symlink() or status.st_size > 64 * 1024 * 1024:
        raise SystemExit(f"unsafe or oversized CLI telemetry file: {path}")
    records: list[dict[str, object]] = []
    for line_number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        try:
            record = json.loads(line)
        except json.JSONDecodeError as error:
            raise SystemExit(
                f"invalid CLI telemetry JSON at line {line_number}: {error}"
            ) from error
        if not isinstance(record, dict):
            raise SystemExit("CLI telemetry record is not an object")
        records.append(record)
    if not records:
        raise SystemExit("CLI telemetry stream is empty")
    for sequence, record in enumerate(records):
        if not (
            record.get("sequence") == sequence
            and record.get("schema_version") == 1
            and record.get("stream_schema") == "katana-port-build-telemetry"
            and record.get("stream_schema_version") == 1
            and record.get("lost_records") == 0
            and record.get("upstream_dropped_observations") == 0
            and record.get("upstream_incomplete") is False
            and record.get("telemetry_complete") is True
        ):
            raise SystemExit(f"CLI telemetry stream is incomplete: {record}")
    schemas = {record.get("schema") for record in records}
    required_schemas = {
        "katana-port-build-manifest",
        "katana-port-build-resolved-environment",
        "katana-port-build-host-command",
        "katana-port-phase-timings",
        "katana-port-build-progress",
        "katana-port-build-resource",
        "katana-port-build-terminal",
    }
    if not required_schemas.issubset(schemas):
        raise SystemExit(f"CLI telemetry schemas are incomplete: {schemas}")
    terminal = records[-1]
    if not (
        terminal.get("schema") == "katana-port-build-terminal"
        and terminal.get("outcome") == "completed"
        and terminal.get("exit_code") == 0
    ):
        raise SystemExit(f"CLI telemetry has no successful terminal record: {terminal}")
    elapsed = [record.get("elapsed_ms") for record in records]
    if not all(isinstance(value, int) and value >= 0 for value in elapsed):
        raise SystemExit("CLI telemetry has invalid elapsed counters")
    if any(right < left or right - left > 10_000 for left, right in zip(elapsed, elapsed[1:])):
        raise SystemExit("CLI telemetry contains an unobserved >10s interval")
    host_commands = [
        record.get("host_command")
        for record in records
        if record.get("schema") == "katana-port-build-host-command"
    ]
    if os.name == "nt":
        expected_tree_scope = "job-object-tree"
        expected_tree_query_complete = True
    elif sys.platform.startswith("linux"):
        expected_tree_scope = "subreaper-descendant-tree"
        expected_tree_query_complete = True
    else:
        expected_tree_scope = "process-group-only"
        expected_tree_query_complete = False
    if not host_commands or any(
        not isinstance(command, dict)
        or command.get("contract_version") != 2
        or command.get("host_exit_code") != 0
        or command.get("timed_out") is not False
        or command.get("interrupted") is not False
        or command.get("process_tree_quiescent") is not True
        or command.get("process_tree_scope") != expected_tree_scope
        or command.get("process_tree_query_complete")
        is not expected_tree_query_complete
        for command in host_commands
    ):
        raise SystemExit(f"CLI host-command supervision drifted: {host_commands}")
    host_stages = {command.get("stage") for command in host_commands if isinstance(command, dict)}
    if not {"runtime-sdk-build", "configure", "host-build"}.issubset(host_stages):
        raise SystemExit(f"CLI did not exercise every host-build stage: {host_stages}")
    terminal_operations = set()
    for record in records:
        if record.get("schema") != "katana-port-build-progress":
            continue
        progress = record.get("progress")
        if isinstance(progress, dict) and progress.get("state") in {
            "completed",
            "cached",
            "skipped",
        }:
            terminal_operations.add(progress.get("operation"))
    if not {"configure", "compilation", "linking", "packaging"}.issubset(
        terminal_operations
    ):
        raise SystemExit(
            f"CLI telemetry skipped Configure/Compile/Link/Packaging: {terminal_operations}"
        )

    manifests = [
        record
        for record in records
        if record.get("schema") == "katana-port-build-manifest"
    ]
    manifest = manifests[0].get("manifest") if len(manifests) == 1 else None
    job_manifest = manifest.get("job") if isinstance(manifest, dict) else None
    requested = (
        job_manifest.get("requested_environment")
        if isinstance(job_manifest, dict)
        else None
    )
    requested_states = {
        entry.get("name"): entry.get("state")
        for entry in requested
        if isinstance(entry, dict)
    } if isinstance(requested, list) else {}
    compile_budget = (
        job_manifest.get("host_compile_budget")
        if isinstance(job_manifest, dict)
        else None
    )
    host_manifest = manifest.get("host") if isinstance(manifest, dict) else None
    telemetry_binary = (
        manifest.get("telemetry_binary") if isinstance(manifest, dict) else None
    )
    manifest_privacy = (
        manifest.get("privacy") if isinstance(manifest, dict) else None
    )
    if not (
        expected_requested_states.keys() <= requested_states.keys()
        and all(
            requested_states[name] == state
            for name, state in expected_requested_states.items()
        )
        and isinstance(compile_budget, dict)
        and compile_budget.get("requested") == expected_workers
        and compile_budget.get("effective") == expected_workers
        and compile_budget.get("quality")
        == "hard-process-wide-upper-bound"
    ):
        raise SystemExit(
            "CLI requested job manifest is not concrete: "
            f"states={requested_states} budget={compile_budget}"
        )
    if not (
        isinstance(host_manifest, dict)
        and all(
            isinstance(host_manifest.get(name), str)
            and bool(host_manifest[name])
            for name in ("os_family", "os_version", "architecture", "cpu_model")
        )
        and isinstance(host_manifest.get("physical_cores"), int)
        and host_manifest["physical_cores"] >= 1
        and isinstance(host_manifest.get("logical_processors"), int)
        and host_manifest["logical_processors"] >= host_manifest["physical_cores"]
        and isinstance(host_manifest.get("ram_bytes"), int)
        and host_manifest["ram_bytes"] > 0
        and isinstance(host_manifest.get("smt_present"), bool)
        and isinstance(telemetry_binary, dict)
        and telemetry_binary.get("role") == "observer"
        and isinstance(telemetry_binary.get("compiler_identity"), str)
        and bool(telemetry_binary["compiler_identity"])
        and isinstance(telemetry_binary.get("cplusplus"), int)
        and telemetry_binary["cplusplus"] > 0
        and telemetry_binary.get("build_profile") == expected_build_profile
        and manifest_privacy
        == {
            "private_paths": "omitted",
            "guest_addresses": "omitted",
            "raw_environment_values": "omitted",
        }
    ):
        raise SystemExit(
            "CLI host/toolchain/profile manifest is incomplete: "
            f"host={host_manifest} telemetry_binary={telemetry_binary}"
        )

    resolved_records = [
        record
        for record in records
        if record.get("schema") == "katana-port-build-resolved-environment"
    ]
    resolved = (
        resolved_records[0].get("resolved_environment")
        if len(resolved_records) == 1
        else None
    )
    resolved_jobs = resolved.get("jobs") if isinstance(resolved, dict) else None
    resolved_toolchain = (
        resolved.get("toolchain") if isinstance(resolved, dict) else None
    )
    resolved_compiler = (
        resolved_toolchain.get("compiler")
        if isinstance(resolved_toolchain, dict)
        else None
    )
    resolved_linker = (
        resolved_toolchain.get("linker")
        if isinstance(resolved_toolchain, dict)
        else None
    )
    resolved_cmake = resolved.get("cmake") if isinstance(resolved, dict) else None
    resolved_generator = (
        resolved.get("generator") if isinstance(resolved, dict) else None
    )
    resolved_launcher = (
        resolved.get("cache_launcher") if isinstance(resolved, dict) else None
    )
    resolved_platform = (
        resolved.get("platform") if isinstance(resolved, dict) else None
    )
    resolved_privacy = (
        resolved.get("privacy") if isinstance(resolved, dict) else None
    )
    resolved_labels: list[object] = []
    for component, names in (
        (resolved_compiler, ("identity", "version", "quality")),
        (resolved_linker, ("identity", "version", "quality")),
        (resolved_cmake, ("version",)),
        (resolved_generator, ("identity", "version", "version_quality")),
        (resolved_launcher, ("identity", "quality")),
    ):
        if not isinstance(component, dict):
            resolved_labels.append(None)
            continue
        resolved_labels.extend(component.get(name) for name in names)
    for component_name, field_name in (
        ("filesystem", "type"),
        ("storage", "type"),
        ("energy", "profile"),
    ):
        component = (
            resolved_platform.get(component_name)
            if isinstance(resolved_platform, dict)
            else None
        )
        if not isinstance(component, dict):
            resolved_labels.extend((None, None))
        else:
            resolved_labels.extend((component.get(field_name), component.get("quality")))
    if not (
        isinstance(resolved_jobs, dict)
        and resolved_jobs.get("analysis") == expected_workers
        and resolved_jobs.get("codegen") == expected_codegen_jobs
        and resolved_jobs.get("host_compile_requested") == expected_workers
        and resolved_jobs.get("host_compile_effective") == expected_workers
        and resolved_jobs.get("host_build") == expected_workers
        and resolved_jobs.get("runtime_parallel_work") == expected_workers
        and resolved_jobs.get("runtime_parallel_work_quality")
        == "effective-runtime-worker-capacity-not-sdk-compile"
        and resolved_jobs.get("quality")
        == "phase-specific-effective-capacity"
        and isinstance(resolved, dict)
        and resolved.get("contract_version") == 2
        and resolved.get("source") == "post-configure-cmake-state"
        and all(isinstance(value, str) and bool(value) for value in resolved_labels)
        and isinstance(resolved_launcher, dict)
        and isinstance(resolved_launcher.get("enabled"), bool)
        and (resolved_launcher.get("identity") != "none")
        == resolved_launcher.get("enabled")
        and resolved_privacy
        == {
            "private_paths": "omitted",
            "raw_environment_values": "omitted",
        }
    ):
        raise SystemExit(
            "CLI effective toolchain/job/platform manifest drifted: "
            f"{resolved}"
        )
    return records


def verify_host_resource_telemetry(records: list[dict[str, object]]) -> None:
    host_stages = {"runtime-sdk-build", "configure", "host-build"}
    samples: list[tuple[int, str, dict[str, object], dict[str, object]]] = []
    for record in records:
        if record.get("schema") != "katana-port-build-resource":
            continue
        resource = record.get("resource")
        gpu = record.get("gpu")
        phase = record.get("phase")
        sequence = record.get("sequence")
        if not (
            isinstance(resource, dict)
            and isinstance(gpu, dict)
            and isinstance(phase, str)
            and isinstance(sequence, int)
        ):
            raise SystemExit(f"CLI resource record is malformed: {record}")
        samples.append((sequence, phase, resource, gpu))
    if not samples:
        raise SystemExit("CLI emitted no resource samples")

    utilization_samples = []
    supported_families: dict[str, list[dict[str, object]]] = {
        name: [] for name in ("cpu", "memory", "faults", "processes")
    }
    supported_io_seen = False
    for _, _, resource, gpu in samples:
        if resource.get("quality") not in {"unsupported", "partial", "sampled"}:
            raise SystemExit(f"CLI global resource quality is invalid: {resource}")
        family_specs = {
            "cpu": (
                "cpu_quality",
                ("cpu_user_ms", "cpu_kernel_ms", "cpu_total_ms"),
            ),
            "memory": (
                "memory_quality",
                (
                    "working_set_bytes_current",
                    "working_set_bytes_peak",
                    "private_commit_bytes_current",
                    "private_commit_bytes_peak",
                ),
            ),
            "faults": ("faults_quality", ("page_faults",)),
            "processes": ("processes_quality", ("active_processes",)),
        }
        for family, (quality_field, fields) in family_specs.items():
            quality = resource.get(quality_field)
            values = [resource.get(field) for field in fields]
            if quality == "unsupported":
                if any(value is not None for value in values):
                    raise SystemExit(
                        f"CLI unsupported {family} family contains values: {resource}"
                    )
            elif isinstance(quality, str) and quality:
                if any(not isinstance(value, int) or value < 0 for value in values):
                    raise SystemExit(
                        f"CLI supported {family} family is incomplete: {resource}"
                    )
                supported_families[family].append(resource)
            else:
                raise SystemExit(
                    f"CLI {family} family quality is absent: {resource}"
                )

        if resource in supported_families["memory"] and (
            resource["working_set_bytes_peak"]
            < resource["working_set_bytes_current"]
            or resource["private_commit_bytes_peak"]
            < resource["private_commit_bytes_current"]
        ):
            raise SystemExit(f"CLI resource peak is below current usage: {resource}")
        core_percent = resource.get("effective_core_percent_milli")
        host_percent = resource.get("effective_host_percent_milli")
        if resource.get("cpu_quality") == "unsupported" and any(
            value is not None for value in (core_percent, host_percent)
        ):
            raise SystemExit(
                f"CLI unsupported CPU utilization contains values: {resource}"
            )
        if any(
            value is not None and (not isinstance(value, int) or value < 0)
            for value in (core_percent, host_percent)
        ):
            raise SystemExit(f"CLI CPU utilization is malformed: {resource}")
        if isinstance(core_percent, int) and isinstance(host_percent, int):
            if host_percent > 100_000:
                raise SystemExit(f"CLI host CPU utilization exceeds 100%: {resource}")
            utilization_samples.append((core_percent, host_percent))

        io_families = (
            (
                "io_bytes_quality",
                (
                    "io_read_bytes",
                    "io_write_bytes",
                    "io_other_bytes",
                    "io_read_operations",
                    "io_write_operations",
                    "io_other_operations",
                ),
            ),
            (
                "io_blocks_quality",
                ("io_input_blocks", "io_output_blocks"),
            ),
        )
        supported_io = False
        for quality_field, fields in io_families:
            quality = resource.get(quality_field)
            values = [resource.get(field) for field in fields]
            if quality == "unsupported":
                if any(value is not None for value in values):
                    raise SystemExit(
                        f"CLI unsupported I/O family contains values: {resource}"
                    )
            elif isinstance(quality, str) and quality:
                if any(not isinstance(value, int) or value < 0 for value in values):
                    raise SystemExit(
                        f"CLI supported I/O family is incomplete: {resource}"
                    )
                supported_io = True
            else:
                raise SystemExit(f"CLI I/O quality is absent: {resource}")
        supported_io_seen |= supported_io

        gpu_quality = gpu.get("quality")
        gpu_values = [
            gpu.get(name)
            for name in (
                "utilization_percent_milli",
                "memory_bytes_current",
                "memory_bytes_peak",
                "host_to_device_bytes",
                "device_to_host_bytes",
            )
        ]
        if gpu_quality == "unsupported" and any(
            value is not None for value in gpu_values
        ):
            raise SystemExit("CLI unsupported GPU telemetry contains values")
        if not isinstance(gpu_quality, str) or not gpu_quality:
            raise SystemExit("CLI GPU telemetry has no explicit quality")

    if any(not values for values in supported_families.values()):
        raise SystemExit(
            "CLI resource stream never supported every required metric family: "
            f"{ {name: len(values) for name, values in supported_families.items()} }"
        )
    if not supported_io_seen:
        raise SystemExit("CLI exposes no platform-qualified I/O in the build stream")
    if not utilization_samples:
        raise SystemExit("CLI emitted no measured process-tree CPU utilization")

    for field in (
        "cpu_total_ms",
        "working_set_bytes_peak",
        "private_commit_bytes_peak",
        "page_faults",
    ):
        values = [
            resource[field]
            for _, _, resource, _ in samples
            if isinstance(resource.get(field), int)
        ]
        if any(right < left for left, right in zip(values, values[1:])):
            raise SystemExit(f"CLI cumulative resource counter regressed: {field}")
    cpu_totals = [
        resource["cpu_total_ms"] for resource in supported_families["cpu"]
    ]
    page_faults = [
        resource["page_faults"] for resource in supported_families["faults"]
    ]
    memory_activity = any(
        resource["working_set_bytes_peak"] > 0
        and (
            resource.get("memory_quality") == "wait4-root-peak-only"
            or (
                resource["working_set_bytes_current"] > 0
                and resource["private_commit_bytes_current"] > 0
                and resource["private_commit_bytes_peak"] > 0
            )
        )
        for resource in supported_families["memory"]
    )
    if not (
        cpu_totals[-1] > cpu_totals[0]
        and page_faults[-1] > page_faults[0]
        and memory_activity
    ):
        raise SystemExit(
            "CLI resource stream did not measure real CPU, RAM and fault activity"
        )

    active = [
        sample
        for sample in samples
        if sample[1] in host_stages and sample[2].get("tree_registered") is True
    ]
    retired = [
        sample
        for sample in samples
        if sample[1] in host_stages
        and sample[2].get("tree_registered") is False
        and sample[2].get("retired_trees_included") is True
    ]
    if not active or not retired:
        raise SystemExit(
            "CLI did not expose active and retired host process-tree resources"
        )
    windows_job_tree = os.name == "nt"
    linux_subreaper_tree = sys.platform.startswith("linux")
    minimum_active_processes = (
        2 if windows_job_tree or linux_subreaper_tree else 1
    )

    def resource_tree_contract(resource: dict[str, object]) -> bool:
        if windows_job_tree:
            return resource.get("tree_query_complete") is True
        if linux_subreaper_tree:
            # /proc is a sampled live view and deliberately never claims the
            # terminal completeness proven separately by HostCommand-v2.
            return (
                resource.get("tree_query_complete") is False
                and resource.get("processes_quality")
                in {
                    "process-group-sampled",
                    "subreaper-descendant-tree-sampled",
                }
            )
        return resource.get("tree_query_complete") is False

    if not any(
        (resource.get("active_processes") or 0) >= minimum_active_processes
        and resource_tree_contract(resource)
        for _, _, resource, _ in active
    ):
        raise SystemExit(
            "CLI never sampled the platform-qualified active supervised host tree"
        )
    active_host_work = [
        resource
        for _, _, resource, _ in active
        if (resource.get("active_processes") or 0) >= minimum_active_processes
        and resource_tree_contract(resource)
    ]
    if not (
        any(
            (resource.get("effective_core_percent_milli") or 0) > 0
            or (resource.get("effective_host_percent_milli") or 0) > 0
            for resource in active_host_work
        )
        and any((resource.get("cpu_total_ms") or 0) > 0 for resource in active_host_work)
        and any((resource.get("page_faults") or 0) > 0 for resource in active_host_work)
    ):
        raise SystemExit(
            "CLI active host process tree showed no real CPU/utilization/fault work"
        )
    for sequence, phase, resource, _ in active:
        if (resource.get("active_processes") or 0) < 1 or not any(
            retired_sequence > sequence and retired_phase == phase
            for retired_sequence, retired_phase, _, _ in retired
        ):
            raise SystemExit(
                f"CLI active host tree has no later retired proof: {phase} {resource}"
            )

    quiescent_host_stages = {
        command.get("stage")
        for record in records
        if record.get("schema") == "katana-port-build-host-command"
        for command in [record.get("host_command")]
        if isinstance(command, dict)
        and command.get("process_tree_quiescent") is True
    }
    if not {phase for _, phase, _, _ in retired}.issubset(quiescent_host_stages):
        raise SystemExit(
            "CLI retired resource trees are not bound to quiescent host commands"
        )


def parse_phase_timings(
    output: str,
) -> tuple[int, dict[str, int], dict[str, int], list[dict[str, object]]]:
    prefix = "KATANA_PORT_PHASE_TIMINGS "
    lines = [line[len(prefix) :] for line in output.splitlines() if line.startswith(prefix)]
    if len(lines) != 1:
        raise SystemExit("CLI emitted no unique phase-timing record")
    timing = json.loads(lines[0])
    if timing.get("schema") != "katana-port-phase-timings-v1":
        raise SystemExit(f"CLI phase-timing schema drifted: {timing}")
    total = timing.get("total_ms")
    samples = timing.get("phases")
    if not isinstance(total, int) or total < 0 or not isinstance(samples, list):
        raise SystemExit(f"CLI phase timings are malformed: {timing}")
    durations: dict[str, int] = {}
    parallel_modules: dict[str, int] = {}
    normalized_samples: list[dict[str, object]] = []
    for sample in samples:
        if not isinstance(sample, dict):
            raise SystemExit("CLI phase sample is not an object")
        phase = sample.get("phase")
        duration = sample.get("duration_ms")
        parallel = sample.get("parallel")
        if (
            not isinstance(phase, str)
            or not isinstance(duration, int)
            or duration < 0
            or not isinstance(parallel, bool)
        ):
            raise SystemExit(f"CLI phase sample is invalid: {sample}")
        durations[phase] = durations.get(phase, 0) + duration
        normalized_samples.append(
            {"phase": phase, "duration_ms": duration, "parallel": parallel}
        )
        if phase.startswith("export:latent-aot-module-analysis-"):
            if not parallel or phase in parallel_modules:
                raise SystemExit(
                    f"CLI latent module timer is not uniquely parallel: {sample}"
                )
            parallel_modules[phase] = duration
    for required in ("disc-load", "runtime-sdk-build", "configure", "host-build", "package"):
        if required not in durations:
            raise SystemExit(f"CLI phase timing omitted {required}: {durations}")
    return total, durations, parallel_modules, normalized_samples


def verify_phase_timing_telemetry(
    records: list[dict[str, object]],
    human_total_ms: int,
    human_samples: list[dict[str, object]],
) -> None:
    matching = [
        (index, record)
        for index, record in enumerate(records)
        if record.get("schema") == "katana-port-phase-timings"
    ]
    if len(matching) != 1:
        raise SystemExit(
            f"CLI JSONL contains no unique phase-timing record: {len(matching)}"
        )
    index, record = matching[0]
    phase_timings = record.get("phase_timings")
    if not (
        index < len(records) - 1
        and isinstance(phase_timings, dict)
        and phase_timings.get("contract_version") == 1
        and phase_timings.get("total_ms") == human_total_ms
        and phase_timings.get("samples") == human_samples
    ):
        raise SystemExit(
            "CLI JSONL phase timings differ from ordered human timing JSON: "
            f"{phase_timings}"
        )


def output_counter(output: str, label: str) -> int:
    prefix = label + ": "
    values = [line[len(prefix) :] for line in output.splitlines() if line.startswith(prefix)]
    if len(values) != 1 or not values[0].isdecimal():
        raise SystemExit(f"CLI output omitted numeric counter {label}")
    return int(values[0])


def cached_progress_operations(
    records: list[dict[str, object]], label: str
) -> set[str]:
    operations: set[str] = set()
    for record in records:
        progress = record.get("progress")
        if not isinstance(progress, dict):
            continue
        if progress.get("state") == "cached" and progress.get("label") == label:
            operation = progress.get("operation")
            if isinstance(operation, str):
                operations.add(operation)
    return operations


def run_cli_port_pass(
    name: str,
    arguments: argparse.Namespace,
    fixture_root: pathlib.Path,
    port_root: pathlib.Path,
    telemetry_path: pathlib.Path | None,
    hints: list[str],
    environment: dict[str, str],
    console_profile: str,
    expected_codegen_jobs: int | None = None,
) -> CliPortPass:
    command = [
        str(arguments.katana_cli),
        "port",
        str(fixture_root / "disc.gdi"),
        "--output",
        str(port_root),
        "--target-name",
        "katana_kr4974_cli_stress",
        "--console-profile",
        console_profile,
        "--latent-aot-mode",
        "exact-only",
    ]
    if telemetry_path is not None:
        command.extend(["--telemetry-jsonl", str(telemetry_path)])
    for hint in hints:
        command.extend(["--latent-aot-entry", hint])
    started = time.monotonic()
    output = run(
        command,
        timeout=600,
        environment=environment,
        stage_name=f"cli-port-{name}",
    )
    wall_ms = round((time.monotonic() - started) * 1000)
    verify_published_install_binding(
        port_root, "katana_kr4974_cli_stress"
    )
    partitions = output_counter(output, "Partitionen")
    if expected_codegen_jobs is None:
        requested_codegen = int(
            environment.get(
                "KATANA_CODEGEN_JOBS",
                str(max(1, os.cpu_count() or 1)),
            )
        )
        if "KATANA_PORT_CODEGEN_JOBS" in environment:
            requested_codegen = min(
                requested_codegen,
                int(environment["KATANA_PORT_CODEGEN_JOBS"]),
            )
        expected_codegen_jobs = min(partitions, requested_codegen)
    requested_names = {
        "KATANA_ANALYSIS_JOBS",
        "KATANA_CODEGEN_JOBS",
        "KATANA_PORT_CODEGEN_JOBS",
        "KATANA_HOST_BUILD_JOBS",
        "KATANA_HOST_COMPILE_JOBS",
        "CMAKE_BUILD_PARALLEL_LEVEL",
        "KATANA_RUNTIME_JOBS",
    }
    telemetry = (
        read_complete_port_telemetry(
            telemetry_path,
            arguments.workers,
            expected_codegen_jobs,
        {
            name: "configured" if name in environment else "unset"
            for name in requested_names
        },
        environment.get("KATANA_PORT_BUILD_PROFILE", "bringup"),
    )
        if telemetry_path is not None
        else []
    )
    (
        phase_total_ms,
        phase_durations,
        parallel_module_durations,
        phase_samples,
    ) = (
        parse_phase_timings(output)
    )
    if telemetry_path is not None:
        verify_phase_timing_telemetry(
            telemetry, phase_total_ms, phase_samples
        )
    if phase_total_ms > wall_ms + 1_000:
        raise SystemExit("CLI phase total exceeds supervised wall time")
    return CliPortPass(
        name,
        wall_ms,
        phase_total_ms,
        phase_durations,
        parallel_module_durations,
        output,
        telemetry,
    )


def verify_cli_cold_warm_component_change(
    arguments: argparse.Namespace,
    fixture_root: pathlib.Path,
    root: pathlib.Path,
    environment: dict[str, str],
) -> dict[str, int]:
    hints = [
        line.strip()
        for line in (fixture_root / "latent-aot-entries.txt")
        .read_text(encoding="ascii")
        .splitlines()
        if line.strip()
    ]
    if len(hints) != 5 or len(set(hints)) != len(hints):
        raise SystemExit("CLI E2E did not load the exact fixture hint set")
    port_root = root / "cli-cache-port"
    expected_whole_cache_operations = {
        "program-validation",
        "control-flow-analysis",
        "function-value-analysis",
        "candidate-resolution",
        "latent-aot-analysis",
        "ir-generation",
        "ir-optimization",
        "source-generation",
        "metadata-generation",
        "artifact-write",
    }

    def require_whole_cache(pass_result: CliPortPass, label: str) -> None:
        if not (
            "KATANA_PORT_SUBPHASE whole-program-analysis-ir-cache-hit"
            in pass_result.output
            and "Analyse-/IR-Cache-Hit: ja" in pass_result.output
            and cached_progress_operations(
                pass_result.telemetry, "whole-export-cache"
            )
            == expected_whole_cache_operations
        ):
            raise SystemExit(f"CLI {label} did not hit the whole-export cache")

    cold = run_cli_port_pass(
        "cold",
        arguments,
        fixture_root,
        port_root,
        root / "cli-cold.jsonl",
        hints,
        environment,
        "japan-ntsc",
    )
    verify_host_resource_telemetry(cold.telemetry)
    # Detailed analysis telemetry is enabled solely by --telemetry-jsonl. Run
    # the same genuinely cold port again in an isolated workspace without that
    # option and require every canonical analysis/export artifact in the
    # published distribution to stay byte-for-byte identical. Reusing the same
    # output pathname excludes publish-path provenance while the distinct
    # workspace prevents a whole-export/cache hit from masking an analysis-side
    # effect. The toolchain-dependent host executable and its derived digest
    # manifest are deliberately checked as bindings, not mistaken for canonical
    # analyzer output.
    telemetry_on_tree = canonical_port_result_tree(
        port_root, "katana_kr4974_cli_stress"
    )
    telemetry_off_environment = dict(environment)
    telemetry_off_environment["KATANA_PORT_WORKSPACE_ROOT"] = str(
        (root / "cli-workspace-telemetry-off").resolve()
    )
    telemetry_off = run_cli_port_pass(
        "cold-telemetry-off",
        arguments,
        fixture_root,
        port_root,
        None,
        hints,
        telemetry_off_environment,
        "japan-ntsc",
    )
    require_identical_file_trees(
        telemetry_on_tree,
        canonical_port_result_tree(
            port_root, "katana_kr4974_cli_stress"
        ),
        "CLI telemetry-on/off canonical result",
    )
    if not (
        not telemetry_off.telemetry
        and "Analyse-/IR-Cache-Hit: nein" in telemetry_off.output
        and output_counter(telemetry_off.output, "Codegen-Cache-Misses") > 0
        and output_counter(telemetry_off.output, "Funktionen")
        == output_counter(cold.output, "Funktionen")
        and output_counter(telemetry_off.output, "Partitionen")
        == output_counter(cold.output, "Partitionen")
    ):
        raise SystemExit(
            "CLI telemetry-on/off runs did not produce the same canonical port"
        )
    warm = run_cli_port_pass(
        "warm-identical",
        arguments,
        fixture_root,
        port_root,
        root / "cli-warm.jsonl",
        hints,
        environment,
        "japan-ntsc",
    )
    codegen_matrix: list[tuple[str, dict[str, str], int]] = []
    primary_only = dict(environment)
    primary_only.pop("KATANA_PORT_CODEGEN_JOBS", None)
    codegen_matrix.append(
        (
            "codegen-primary-only",
            primary_only,
            min(arguments.workers, output_counter(warm.output, "Partitionen")),
        )
    )
    legacy_only = dict(environment)
    legacy_only.pop("KATANA_CODEGEN_JOBS", None)
    legacy_only["KATANA_PORT_CODEGEN_JOBS"] = "2"
    codegen_matrix.append(("codegen-legacy-only", legacy_only, 2))
    both_differing = dict(environment)
    both_differing["KATANA_CODEGEN_JOBS"] = str(arguments.workers)
    both_differing["KATANA_PORT_CODEGEN_JOBS"] = "2"
    codegen_matrix.append(("codegen-both-differing", both_differing, 2))
    for label, matrix_environment, expected_jobs in codegen_matrix:
        matrix_pass = run_cli_port_pass(
            label,
            arguments,
            fixture_root,
            port_root,
            root / f"cli-{label}.jsonl",
            hints,
            matrix_environment,
            "japan-ntsc",
            expected_jobs,
        )
        require_whole_cache(matrix_pass, label)
    changed_environment = dict(environment)
    changed_environment["KATANA_PORT_BUILD_PROFILE"] = "gate"
    changed = run_cli_port_pass(
        "component-change",
        arguments,
        fixture_root,
        port_root,
        root / "cli-component-change.jsonl",
        hints,
        changed_environment,
        "japan-ntsc",
    )

    if (
        len(cold.parallel_module_durations_ms) < 2
        or any(
            duration <= 0
            for duration in cold.parallel_module_durations_ms.values()
        )
    ):
        raise SystemExit(
            "CLI cold pass did not publish real parallel latent-module timers: "
            f"{cold.parallel_module_durations_ms}"
        )

    if (
        "Analyse-/IR-Cache-Hit: nein" not in cold.output
        or output_counter(cold.output, "Codegen-Cache-Misses") == 0
        or cached_progress_operations(cold.telemetry, "whole-export-cache")
    ):
        raise SystemExit("CLI cold pass was not observably cold")
    require_whole_cache(warm, "identical warm pass")
    changed_manifests = [
        record
        for record in changed.telemetry
        if record.get("schema") == "katana-port-build-manifest"
    ]
    changed_manifest = (
        changed_manifests[0].get("manifest")
        if len(changed_manifests) == 1
        else None
    )
    changed_binary = (
        changed_manifest.get("telemetry_binary")
        if isinstance(changed_manifest, dict)
        else None
    )
    if not (
        "KATANA_PORT_SUBPHASE whole-program-analysis-ir-cache-hit"
        in changed.output
        and "Analyse-/IR-Cache-Hit: ja" in changed.output
        and "Buildprofil: gate" in changed.output
        and isinstance(changed_binary, dict)
        and changed_binary.get("build_profile") == "gate"
        and cached_progress_operations(changed.telemetry, "whole-export-cache")
        == expected_whole_cache_operations
        and output_counter(changed.output, "Codegen-Cache-Misses") == 0
        and output_counter(changed.output, "Codegen-Cache-Hits") > 0
    ):
        raise SystemExit("CLI component change did not reuse only unaffected cache layers")
    metadata_path = port_root / "generated" / "metadata" / "port-project.json"
    metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
    if metadata.get("console_profile") != "japan-ntsc":
        raise SystemExit("CLI component-change output was not published")
    if not warm.phase_total_ms < cold.phase_total_ms:
        raise SystemExit(
            "CLI cold/warm timings do not show material reuse: "
            f"cold={cold.phase_total_ms} warm={warm.phase_total_ms} "
            f"changed={changed.phase_total_ms}"
        )
    result = {
        "cold_ms": cold.phase_total_ms,
        "cold_telemetry_off_ms": telemetry_off.phase_total_ms,
        "warm_identical_ms": warm.phase_total_ms,
        "component_change_ms": changed.phase_total_ms,
    }
    print(
        json.dumps(
            {"schema": "katana-native-disc-cli-cache-e2e-v1", **result},
            sort_keys=True,
        ),
        flush=True,
    )
    return result


def main() -> int:
    total_started = time.monotonic()
    parser = argparse.ArgumentParser()
    parser.add_argument("--writer", required=True, type=pathlib.Path)
    parser.add_argument("--verifier", required=True, type=pathlib.Path)
    parser.add_argument("--katana-cli", required=True, type=pathlib.Path)
    parser.add_argument("--stress-runner", required=True, type=pathlib.Path)
    parser.add_argument("--cmake", required=True, type=pathlib.Path)
    parser.add_argument("--generator", required=True)
    parser.add_argument("--generator-platform")
    parser.add_argument("--cxx-compiler", type=pathlib.Path)
    parser.add_argument("--make-program", type=pathlib.Path)
    parser.add_argument("--runtime-targets", required=True, type=pathlib.Path)
    parser.add_argument("--workers", required=True, type=int)
    arguments = parser.parse_args()

    for label, path in (
        ("writer", arguments.writer),
        ("verifier", arguments.verifier),
        ("katana CLI", arguments.katana_cli),
        ("stress runner", arguments.stress_runner),
        ("CMake", arguments.cmake),
        ("runtime targets", arguments.runtime_targets),
    ):
        if not path.is_file():
            parser.error(f"{label} is not a regular file: {path}")
    for label, path in (
        ("C++ compiler", arguments.cxx_compiler),
        ("build program", arguments.make_program),
    ):
        if path is not None and not path.is_file():
            parser.error(f"{label} is not a regular file: {path}")
    if arguments.workers < 1 or arguments.workers > 64:
        parser.error("--workers must be between 1 and 64")

    verify_process_tree_supervision()

    with tempfile.TemporaryDirectory(
        prefix="katana-native-disc-cold-build-stress-"
    ) as temporary:
        root = pathlib.Path(temporary)
        first = root / "smoke-a"
        second = root / "smoke-b"
        for output in (first, second):
            run(
                [
                    sys.executable,
                    "-B",
                    str(arguments.writer),
                    "--profile",
                    "smoke",
                    "--output",
                    str(output),
                ],
                stage_name=f"fixture-writer-{output.name}",
            )
        run(
            [
                sys.executable,
                "-B",
                str(arguments.verifier),
                str(first),
                "--profile",
                "smoke",
                "--compare",
                str(second),
                "--katana-cli",
                str(arguments.katana_cli),
            ],
            stage_name="fixture-independent-verifier",
        )
        port_root = root / "real-port"
        stress_environment = os.environ.copy()
        stress_environment.pop("CODEX_ANALYZER_STACK_DIAGNOSTICS", None)
        stress_environment.pop("KATANA_PORT_CODEGEN_JOBS", None)
        stress_environment["KATANA_ANALYSIS_JOBS"] = str(arguments.workers)
        stress_environment["KATANA_CODEGEN_JOBS"] = str(arguments.workers)
        runner_output = run(
            [
                str(arguments.stress_runner),
                str(first),
                str(port_root),
                str(arguments.workers),
            ],
            environment=stress_environment,
            stage_name="real-component-stress-runner",
        )
        result = parse_stress_result(runner_output)
        expected = {
            "roots": 14,
            "seed_waves": 4,
            "fva_runs": 5,
            "replay_passes": 1,
            "functions": 16,
            "blocks": 224,
            "boot_partitions": 4,
            "latent_modules": 2,
            "latent_entries": 3,
            "latent_source_bindings": 3,
            "maximum_workers": arguments.workers,
            "source_configured_workers": min(
                arguments.workers, result.get("port_partitions", 0)
            ),
            "latent_configured_workers": min(arguments.workers, 2),
        }
        if any(result.get(key) != value for key, value in expected.items()):
            raise SystemExit(f"stress runner contract drifted: {result}")
        reference_tree = regular_file_tree(port_root)
        for label, primary, legacy, expected_codegen_workers in (
            ("legacy-only", None, 2, 2),
            ("both-differing", arguments.workers, 2, 2),
        ):
            matrix_environment = dict(stress_environment)
            if primary is None:
                matrix_environment.pop("KATANA_CODEGEN_JOBS", None)
            else:
                matrix_environment["KATANA_CODEGEN_JOBS"] = str(primary)
            matrix_environment["KATANA_PORT_CODEGEN_JOBS"] = str(legacy)
            matrix_port = root / f"real-port-codegen-{label}"
            matrix_result = parse_stress_result(
                run(
                    [
                        str(arguments.stress_runner),
                        str(first),
                        str(matrix_port),
                        str(arguments.workers),
                    ],
                    environment=matrix_environment,
                    stage_name=f"real-component-codegen-{label}",
                )
            )
            require_identical_file_trees(
                reference_tree,
                regular_file_tree(matrix_port),
                f"real port_codegen_jobs matrix {label}",
            )
            if not (
                matrix_result.get("source_configured_workers")
                == expected_codegen_workers
                and matrix_result.get("port_functions")
                == result.get("port_functions")
                and matrix_result.get("port_partitions")
                == result.get("port_partitions")
                and matrix_result.get("translation_units")
                == result.get("translation_units")
            ):
                raise SystemExit(
                    f"real port_codegen_jobs matrix drifted for {label}: "
                    f"{matrix_result}"
                )
        if not (
            result.get("logical_evaluations", 0) > 0
            and result.get("cache_lookups", 0)
            == result.get("cache_hits", 0) + result.get("cache_misses", 0)
            and result.get("cache_ready_hits", 0)
            + result.get("cache_in_flight_coalesces", 0)
            == result.get("cache_hits", 0)
            and result.get("physical_evaluations", -1)
            == result.get("cache_misses", 0)
            + result.get("cache_replay_fallback_recomputes", 0)
            + result.get("cache_diagnostic_bypass_evaluations", 0)
            and result.get("logical_evaluations", 0)
            >= result.get("physical_evaluations", 0)
            and result.get("cache_hits", 0) > 0
            and result.get("cache_misses", 0) > 0
            and isinstance(result.get("cache_evictions"), int)
            and result.get("cache_evictions", -1) >= 0
            and result.get("cache_entries", 0) > 0
            and result.get("cache_retained_payload_bytes", 0) > 0
            and result.get("replay_hits", 0) > 0
            and result.get("replay_misses") == 0
            and result.get("cache_diagnostic_bypass_evaluations") == 0
        ):
            raise SystemExit(f"real stress FVA/cache ledger is unbalanced: {result}")
        miss_reason_names = {
            "cold",
            "evicted",
            "oversize_or_no_exact_replay",
            "function_shape_changed",
            "projected_ingress_changed",
            "summary_dependency_changed",
            "abi_contract_changed",
            "resolution_lens_changed",
            "inventory_sink_changed",
            "isolation_partition_changed",
            "contextual_summary_changed",
            "tail_ingress_changed",
        }
        miss_reasons = result.get("miss_reasons")
        if not (
            isinstance(miss_reasons, dict)
            and set(miss_reasons) == miss_reason_names
            and sum(miss_reasons.values()) == result["cache_misses"]
            and miss_reasons["cold"] > 0
        ):
            raise SystemExit(f"primary cache miss-reason ledger drifted: {result}")
        waves = result.get("fva_wave_ledger")
        if not isinstance(waves, list) or len(waves) != 5:
            raise SystemExit("FVA wave ledger does not cover four growth waves plus replay")
        for index, wave in enumerate(waves):
            if not (
                wave.get("index") == index
                and wave.get("boundaries") == wave.get("summaries")
                and wave.get("logical_evaluations") == wave.get("cache_lookups")
                and wave.get("cache_lookups")
                == wave.get("cache_hits") + wave.get("cache_misses")
                and wave.get("cache_hits")
                == wave.get("cache_ready_hits")
                + wave.get("cache_in_flight_coalesces")
                and wave.get("physical_evaluations")
                == wave.get("cache_misses")
                + wave.get("cache_replay_fallback_recomputes")
                + wave.get("cache_diagnostic_bypass_evaluations")
                and set(wave.get("miss_reasons", {})) == miss_reason_names
                and sum(wave["miss_reasons"].values()) == wave["cache_misses"]
                and wave.get("cache_diagnostic_bypass_evaluations") == 0
            ):
                raise SystemExit(f"FVA wave {index} is not exactly balanced: {wave}")
            if index < 4:
                if not (
                    wave.get("replay") is False
                    and wave.get("added_roots", 0) > 0
                    and wave.get("cache_misses", 0) > 0
                    and (index == 0 or wave.get("cache_hits", 0) > 0)
                ):
                    raise SystemExit(f"FVA growth wave {index} is not real: {wave}")
            elif not (
                wave.get("replay") is True
                and wave.get("added_roots") == 0
                and wave.get("cache_lookups") == wave.get("cache_hits") > 0
                and wave.get("cache_misses") == 0
                and wave.get("physical_evaluations") == 0
            ):
                raise SystemExit(f"final exact FVA replay drifted: {wave}")
        expected_targeted_reasons = dict.fromkeys(miss_reason_names, 0)
        expected_targeted_reasons.update(
            {
                "function_shape_changed": 1,
                "projected_ingress_changed": 17,
                "summary_dependency_changed": 2,
                "resolution_lens_changed": 1,
            }
        )
        if not (
            result.get("semantic_invalidation_baseline_version") == 1
            and result.get("semantic_targeted_hits") == 6
            and result.get("semantic_targeted_misses") == 21
            and result.get("semantic_targeted_miss_reasons")
            == expected_targeted_reasons
            and result.get("semantic_ready_ahead", 0) >= 2
            and isinstance(result.get("semantic_hol_ms"), int)
            and result.get("semantic_hol_ms", -1) >= 0
            and result.get("semantic_sccs") == 5
            and result.get("semantic_targeted_miss_functions")
            == [
                0x8D000000,
                0x8D005000,
                0x8D005040,
                0x8D005080,
                0x8D0050C0,
            ]
            and result.get("semantic_targeted_hit_only_functions")
            == [0x8D005100]
        ):
            raise SystemExit(f"semantic SCC/HOL/invalidation contract drifted: {result}")
        timings = result.get("timings_ms")
        if not (
            isinstance(timings, dict)
            and set(timings) == {"cfa", "fva_waves", "semantic_fva", "latent", "export"}
            and all(isinstance(value, int) and value >= 0 for value in timings.values())
            and result.get("component_suite_elapsed_ms", 0) >= sum(timings.values())
            and result.get("structured_progress_events", 0) > 0
            and result.get("structured_progress_max_gap_ms", 10_001) <= 10_000
        ):
            raise SystemExit(f"component timing/progress evidence drifted: {result}")
        if not (
            result.get("port_functions", 0)
            >= result["functions"] + result["latent_entries"]
            and result.get("port_partitions", 0) > result["boot_partitions"]
            and result.get("translation_units") == result.get("port_partitions")
        ):
            raise SystemExit(f"real export/TU contract is unbalanced: {result}")
        emitted_units = generated_translation_units(port_root)
        if len(emitted_units) != result["translation_units"]:
            raise SystemExit("runner result is not bound to the emitted unit-v sources")
        build_environment = host_build_environment(arguments.cxx_compiler)
        build_environment["CMAKE_BUILD_PARALLEL_LEVEL"] = str(arguments.workers)
        build_environment["KATANA_ANALYSIS_JOBS"] = str(arguments.workers)
        build_environment["KATANA_CODEGEN_JOBS"] = str(arguments.workers)
        build_environment["KATANA_PORT_CODEGEN_JOBS"] = str(arguments.workers)
        build_environment["KATANA_HOST_BUILD_JOBS"] = str(arguments.workers)
        build_environment["KATANA_HOST_COMPILE_JOBS"] = str(arguments.workers)
        build_environment["KATANA_RUNTIME_JOBS"] = str(arguments.workers)
        build_environment["KATANA_PORT_BUILD_PROFILE"] = "bringup"
        build_environment["KATANA_PORT_WORKSPACE_ROOT"] = str(
            (root / "cli-workspace").resolve()
        )
        build_environment["KATANA_RUNTIME_BUILD_TARGETS"] = str(
            arguments.runtime_targets.resolve()
        )
        build_environment.pop("KATANA_RUNTIME_ROOT", None)
        build_environment.pop("KATANA_RUNTIME_PREFIX", None)
        if arguments.make_program is not None:
            build_environment["KATANA_HOST_BUILD_MAKE_PROGRAM"] = str(
                arguments.make_program.resolve()
            )
        if os.name == "nt":
            build_environment["KATANA_HOST_BUILD_GENERATOR"] = (
                "Ninja" if "Ninja" in arguments.generator else "Visual Studio"
            )
            if arguments.cxx_compiler is not None:
                build_environment["KATANA_PORT_CXX_COMPILER"] = (
                    "clang-cl"
                    if arguments.cxx_compiler.name.lower() == "clang-cl.exe"
                    else "msvc"
                )
        cli_cache_timings = verify_cli_cold_warm_component_change(
            arguments,
            first,
            root,
            build_environment,
        )
        host_build = root / "host-build"
        configure = [
            str(arguments.cmake),
            "-S",
            str(port_root),
            "-B",
            str(host_build),
            "-G",
            arguments.generator,
        ]
        if arguments.generator_platform:
            configure.extend(["-A", arguments.generator_platform])
        multi_config = (
            "Visual Studio" in arguments.generator
            or "Multi-Config" in arguments.generator
            or "Xcode" in arguments.generator
        )
        if not multi_config:
            configure.extend(
                [
                    "-DCMAKE_BUILD_TYPE=RelWithDebInfo",
                    "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
                ]
            )
        if arguments.cxx_compiler is not None:
            configure.append(
                f"-DCMAKE_CXX_COMPILER={arguments.cxx_compiler.resolve()}"
            )
            if (
                os.name == "nt"
                and arguments.cxx_compiler.name.lower() == "cl.exe"
            ):
                # /Zi launches mspdbsrv, whose intentional idle lifetime can
                # outlive CMake by minutes. /Z7 keeps equivalent debug records
                # in the object files, so supervised tree quiescence remains a
                # real end-of-work signal instead of a PDB-server idle timer.
                configure.append(
                    "-DCMAKE_MSVC_DEBUG_INFORMATION_FORMAT=Embedded"
                )
        if arguments.make_program is not None and "Ninja" in arguments.generator:
            configure.append(
                f"-DCMAKE_MAKE_PROGRAM={arguments.make_program.resolve()}"
            )
        configure.extend(
            [
                "-DKATANA_PORT_BUILD_PROFILE=bringup",
                f"-DKATANA_RUNTIME_BUILD_TARGETS={arguments.runtime_targets.resolve()}",
            ]
        )
        configure_started = time.monotonic()
        run(
            configure,
            environment=build_environment,
            stage_name="component-host-configure",
        )
        configure_elapsed_ms = round((time.monotonic() - configure_started) * 1000)
        generated_build = [
            str(arguments.cmake),
            "--build",
            str(host_build),
            "--target",
            "katana_generated",
            "--parallel",
            str(arguments.workers),
        ]
        if multi_config:
            generated_build.extend(["--config", "RelWithDebInfo"])
        generated_compile_started = time.monotonic()
        run(
            generated_build,
            environment=build_environment,
            stage_name="component-generated-compile",
        )
        generated_compile_elapsed_ms = round(
            (time.monotonic() - generated_compile_started) * 1000
        )
        host_build_command = [
            str(arguments.cmake),
            "--build",
            str(host_build),
            "--target",
            "katana_kr4974_stress",
            "--parallel",
            str(arguments.workers),
        ]
        if multi_config:
            host_build_command.extend(["--config", "RelWithDebInfo"])
        host_compile_link_started = time.monotonic()
        run(
            host_build_command,
            environment=build_environment,
            stage_name="component-host-compile-link",
        )
        host_compile_link_elapsed_ms = round(
            (time.monotonic() - host_compile_link_started) * 1000
        )
        executable = require_host_product_binding(
            port_root,
            host_build,
            multi_config,
            result["translation_units"],
        )
        print(
            json.dumps(
                {
                    "schema": "katana-native-disc-cold-build-stress-host-result-v2",
                    "stress_suite_elapsed_ms": round(
                        (time.monotonic() - total_started) * 1000
                    ),
                    "configure_elapsed_ms": configure_elapsed_ms,
                    "generated_compile_elapsed_ms": generated_compile_elapsed_ms,
                    "host_compile_link_elapsed_ms": host_compile_link_elapsed_ms,
                    "cli_cache_timings_ms": cli_cache_timings,
                    "executable": str(executable),
                    "translation_units": result["translation_units"],
                    "source_binding": "generated-cmake-to-katana_generated-to-host-executable",
                },
                sort_keys=True,
            ),
            flush=True,
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
