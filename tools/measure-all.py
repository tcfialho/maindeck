#!/usr/bin/env python3
#
# Quick live-session sampler for river and MainDeck processes.
#
# It measures CPU, RSS/PSS, VSZ, file descriptors, and threads for the
# running river / maindeck-wm / maindeck-bar / maindeck-menu processes.
# If maindeck-menu is not already running, it starts a temporary one for the
# measurement window and shuts it down at the end.

import argparse
import os
import pwd
import shutil
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path


REPO = Path(__file__).resolve().parent.parent
HZ = os.sysconf(os.sysconf_names["SC_CLK_TCK"])
CURRENT_USER = pwd.getpwuid(os.getuid()).pw_name


@dataclass
class ProcSample:
    cpu_ticks: int
    rss_kb: int
    pss_kb: int
    private_dirty_kb: int
    vsz_kb: int
    read_bytes: int
    write_bytes: int
    syscr: int
    syscw: int
    voluntary_cs: int
    nonvoluntary_cs: int
    fd_count: int
    thread_count: int


def die(msg: str) -> None:
    print(f"measure-all: ERROR: {msg}", file=sys.stderr)
    raise SystemExit(1)


def is_executable(path: Path) -> bool:
    return path.is_file() and os.access(path, os.X_OK)


def resolve_binary(name: str) -> Path:
    candidates = [
        REPO / "build" / name,
    ]
    which = shutil.which(name)
    if which:
        candidates.append(Path(which))
    candidates.append(Path.home() / ".local" / "bin" / name)

    for candidate in candidates:
        if is_executable(candidate):
            return candidate
    die(f"não encontrei binário executável para {name}")
    return Path()  # unreachable


def proc_exists(pid: int) -> bool:
    return Path(f"/proc/{pid}").exists()


def read_proc_sample(pid: int) -> ProcSample:
    proc = Path(f"/proc/{pid}")

    stat = (proc / "stat").read_text()
    rparen = stat.rfind(")")
    fields = stat[rparen + 2 :].split()
    utime = int(fields[11])
    stime = int(fields[12])
    vsz_kb = int(fields[20]) // 1024
    thread_count = int(fields[17])

    status = {}
    for line in (proc / "status").read_text().splitlines():
        if ":" not in line:
            continue
        key, value = line.split(":", 1)
        status[key] = value.strip()

    rss_kb = int(status.get("VmRSS", "0 kB").split()[0])
    voluntary_cs = int(status.get("voluntary_ctxt_switches", "0"))
    nonvoluntary_cs = int(status.get("nonvoluntary_ctxt_switches", "0"))

    smaps = proc / "smaps_rollup"
    if not smaps.exists():
        smaps = proc / "smaps"

    pss_kb = 0
    private_dirty_kb = 0
    if smaps.exists():
        try:
            for line in smaps.read_text().splitlines():
                if line.startswith("Pss:"):
                    pss_kb += int(line.split()[1])
                elif line.startswith("Private_Dirty:"):
                    private_dirty_kb += int(line.split()[1])
        except Exception:
            pass

    io = {}
    try:
        for line in (proc / "io").read_text().splitlines():
            key, value = line.split(":", 1)
            io[key] = int(value.strip())
    except PermissionError:
        pass

    try:
        fd_count = len(list((proc / "fd").glob("*")))
    except Exception:
        fd_count = 0

    return ProcSample(
        cpu_ticks=utime + stime,
        rss_kb=rss_kb,
        pss_kb=pss_kb,
        private_dirty_kb=private_dirty_kb,
        vsz_kb=vsz_kb,
        read_bytes=io.get("read_bytes", 0),
        write_bytes=io.get("write_bytes", 0),
        syscr=io.get("syscr", 0),
        syscw=io.get("syscw", 0),
        voluntary_cs=voluntary_cs,
        nonvoluntary_cs=nonvoluntary_cs,
        fd_count=fd_count,
        thread_count=thread_count,
    )


def find_pids() -> dict[str, int]:
    pids = {}
    for proc in Path("/proc").glob("[0-9]*"):
        try:
            pid = int(proc.name)
            if proc.owner() != CURRENT_USER:
                continue
            exe_path = proc / "exe"
            if not exe_path.exists():
                continue
            exe = os.readlink(str(exe_path))
            # Um binário atualizado embaixo de um processo vivo vira "... (deleted)".
            exe = exe.removesuffix(" (deleted)")
            if exe.endswith("/river") or exe == "river":
                pids["river"] = pid
            elif exe.endswith("/maindeck-wm") or exe == "maindeck-wm":
                pids["maindeck-wm"] = pid
            elif exe.endswith("/maindeck-bar") or exe == "maindeck-bar":
                pids["maindeck-bar"] = pid
            elif exe.endswith("/maindeck-menu") or exe == "maindeck-menu":
                pids["maindeck-menu"] = pid
        except Exception:
            continue
    return pids


def get_cpu_percent(before: ProcSample, after: ProcSample, seconds: float) -> float:
    if seconds <= 0:
        return 0.0
    return ((after.cpu_ticks - before.cpu_ticks) / HZ) / seconds * 100.0


def print_report(
    pids: dict[str, int],
    before: dict[str, ProcSample],
    after: dict[str, ProcSample],
    duration: float,
) -> None:
    print(f"\n--- Resource Consumption Report ({duration:.2f}s sampling window) ---")
    print(
        "Process         | PID    | CPU%  | RSS (MB)  | PSS (MB)  | VSZ (MB)  | "
        "FDs | Threads | Private Dirty"
    )
    print("-" * 108)

    for name in sorted(before.keys()):
        b = before[name]
        a = after[name]
        cpu_percent = get_cpu_percent(b, a, duration)
        rss_mb = a.rss_kb / 1024.0
        pss_mb = a.pss_kb / 1024.0
        vsz_mb = a.vsz_kb / 1024.0
        priv_dirty_mb = a.private_dirty_kb / 1024.0

        print(
            f"{name:<15} | {pids.get(name, 0):<6} | {cpu_percent:>5.2f}% | {rss_mb:>8.2f} | "
            f"{pss_mb:>8.2f} | {vsz_mb:>8.2f} | {a.fd_count:>3} | {a.thread_count:>7} | "
            f"{priv_dirty_mb:>13.2f}"
        )


class Sampler:
    def __init__(self, args):
        self.args = args
        self.tmp = Path(tempfile.mkdtemp(prefix="maindeck-measure."))
        self.river = None
        self.menu = None
        self.menu_pid = None
        self.menu_bin = resolve_binary("maindeck-menu")

    def cleanup(self):
        if self.menu and self.menu.poll() is None:
            self.menu.terminate()
        if self.river and self.river.poll() is None:
            self.river.terminate()

        for proc in (self.menu, self.river):
            if proc:
                try:
                    proc.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    proc.kill()

        if self.menu_pid and proc_exists(self.menu_pid):
            try:
                os.kill(self.menu_pid, 15)
            except ProcessLookupError:
                pass

        shutil.rmtree(self.tmp, ignore_errors=True)

    def maybe_start_menu(self) -> None:
        if "maindeck-menu" in self.pids:
            return

        print(f"maindeck-menu não está rodando; iniciando temporariamente: {self.menu_bin}")
        self.menu = subprocess.Popen(
            [str(self.menu_bin)],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            env=os.environ.copy(),
        )
        self.menu_pid = self.menu.pid
        time.sleep(1.0)
        self.pids = find_pids()
        if "maindeck-menu" not in self.pids:
            die("não consegui subir o maindeck-menu temporário")

    def run(self) -> None:
        if "WAYLAND_DISPLAY" not in os.environ or "XDG_RUNTIME_DIR" not in os.environ:
            die("precisa rodar dentro de uma sessão Wayland")

        self.pids = find_pids()
        if not self.pids:
            die("não achei river ou processos maindeck na sessão atual")

        self.maybe_start_menu()
        self.pids = find_pids()

        print("Found PIDs:", self.pids)

        cpu_start = {name: read_proc_sample(pid) for name, pid in self.pids.items()}
        time_start = time.monotonic()

        time.sleep(self.args.seconds)

        cpu_end = {name: read_proc_sample(pid) for name, pid in self.pids.items()}
        time_end = time.monotonic()

        print_report(self.pids, cpu_start, cpu_end, time_end - time_start)


def main():
    parser = argparse.ArgumentParser(description="Quick live-session sampler for MainDeck and River")
    parser.add_argument(
        "-s",
        "--seconds",
        type=float,
        default=3.0,
        help="sampling window in seconds",
    )
    args = parser.parse_args()

    sampler = Sampler(args)
    try:
        sampler.run()
    finally:
        sampler.cleanup()


if __name__ == "__main__":
    main()
