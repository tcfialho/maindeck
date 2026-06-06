#!/usr/bin/env python3
#
# External performance smoke/benchmark for maindeck-wm.
#
# This deliberately keeps MAINDECK_LOG disabled. It measures the WM process via
# /proc instead of adding counters to the compositor hot path.

import argparse
import os
import select
import shutil
import socket
import statistics
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path


REPO = Path(__file__).resolve().parent.parent
HZ = os.sysconf(os.sysconf_names["SC_CLK_TCK"])


@dataclass
class ProcSample:
    cpu_ticks: int
    rss_kb: int
    read_bytes: int
    write_bytes: int
    syscr: int
    syscw: int
    voluntary_cs: int
    nonvoluntary_cs: int


@dataclass
class ScenarioResult:
    name: str
    ops: int
    wall_ms: float
    wm_cpu_ms: float
    bar_cpu_ms: float
    wm_rss_delta_kb: int
    bar_rss_delta_kb: int
    wm_read_bytes: int
    wm_write_bytes: int
    bar_read_bytes: int
    bar_write_bytes: int
    wm_syscalls_io: int
    bar_syscalls_io: int
    wm_ctx_switches: int
    bar_ctx_switches: int
    p50_ack_ms: float
    p95_ack_ms: float


def die(msg: str) -> None:
    print(f"perf-wm: ERROR: {msg}", file=sys.stderr)
    raise SystemExit(1)


def run(args, **kwargs):
    print("+", " ".join(map(str, args)))
    return subprocess.run(args, check=True, **kwargs)


def proc_exists(pid: int) -> bool:
    return Path(f"/proc/{pid}").exists()


def read_proc_sample(pid: int) -> ProcSample:
    stat = Path(f"/proc/{pid}/stat").read_text()
    rparen = stat.rfind(")")
    fields = stat[rparen + 2 :].split()
    utime = int(fields[11])
    stime = int(fields[12])

    status = {}
    for line in Path(f"/proc/{pid}/status").read_text().splitlines():
        if ":" not in line:
            continue
        key, value = line.split(":", 1)
        status[key] = value.strip()
    rss_kb = int(status.get("VmRSS", "0 kB").split()[0])
    voluntary_cs = int(status.get("voluntary_ctxt_switches", "0"))
    nonvoluntary_cs = int(status.get("nonvoluntary_ctxt_switches", "0"))

    io = {}
    try:
        for line in Path(f"/proc/{pid}/io").read_text().splitlines():
            key, value = line.split(":", 1)
            io[key] = int(value.strip())
    except PermissionError:
        pass

    return ProcSample(
        cpu_ticks=utime + stime,
        rss_kb=rss_kb,
        read_bytes=io.get("read_bytes", 0),
        write_bytes=io.get("write_bytes", 0),
        syscr=io.get("syscr", 0),
        syscw=io.get("syscw", 0),
        voluntary_cs=voluntary_cs,
        nonvoluntary_cs=nonvoluntary_cs,
    )


def read_environ(pid: int) -> dict:
    data = Path(f"/proc/{pid}/environ").read_bytes()
    env = {}
    for item in data.split(b"\0"):
        if b"=" in item:
            key, value = item.split(b"=", 1)
            env[key.decode(errors="replace")] = value.decode(errors="replace")
    return env


def percentile(values, pct: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    idx = int(round((len(ordered) - 1) * pct))
    return ordered[idx]


class Bench:
    def __init__(self, args):
        self.args = args
        self.tmp = Path(tempfile.mkdtemp(prefix="maindeck-perf."))
        self.river = None
        self.bar = None
        self.dialog = None
        self.wm_pid = None
        self.nested = None

    def cleanup(self):
        for proc in (self.dialog, self.bar, self.river):
            if proc and proc.poll() is None:
                proc.terminate()
        for proc in (self.dialog, self.bar, self.river):
            if proc:
                try:
                    proc.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    proc.kill()
        if self.wm_pid and proc_exists(self.wm_pid):
            try:
                os.kill(self.wm_pid, 15)
            except ProcessLookupError:
                pass
        shutil.rmtree(self.tmp, ignore_errors=True)

    def setup(self):
        if os.environ.get("MAINDECK_LOG", "").lower().startswith("d"):
            die("MAINDECK_LOG=debug no ambiente chamador; desligue antes de medir")
        if "WAYLAND_DISPLAY" not in os.environ or "XDG_RUNTIME_DIR" not in os.environ:
            die("precisa rodar dentro de uma sessão Wayland")

        if not self.args.no_build:
            run(["meson", "compile", "-C", str(REPO / "build")], cwd=REPO)

        real_sock = Path(os.environ["XDG_RUNTIME_DIR"]) / os.environ["WAYLAND_DISPLAY"]
        if not real_sock.exists():
            die(f"socket Wayland real não existe: {real_sock}")
        (self.tmp / "wayland-parent").symlink_to(real_sock)

        wm_bin = REPO / "build" / "maindeck-wm"
        init = self.tmp / "river-init.sh"
        pid_file = self.tmp / "wm.pid"
        init.write_text(f"""#!/bin/sh
MAINDECK_LOG= MAINDECK_LOG_PATH="{self.tmp / 'maindeck.log'}" \\
MAINDECK_IMPLICIT_PARENT_APP_ID=steam \\
MAINDECK_IMPLICIT_PARENT_TITLES='Steam|Steam Big Picture' \\
"{wm_bin}" &
echo $! > "{pid_file}"
""")
        init.chmod(0o755)

        env = os.environ.copy()
        env.update(
            {
                "XDG_RUNTIME_DIR": str(self.tmp),
                "WAYLAND_DISPLAY": "wayland-parent",
                "MAINDECK_LOG": "",
                "MAINDECK_LOG_PATH": str(self.tmp / "maindeck.log"),
                "MAINDECK_IMPLICIT_PARENT_APP_ID": "steam",
                "MAINDECK_IMPLICIT_PARENT_TITLES": "Steam|Steam Big Picture",
            }
        )
        self.river = subprocess.Popen(
            ["river", "-c", str(init)],
            env=env,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )

        deadline = time.monotonic() + 12
        while time.monotonic() < deadline:
            for name in ("wayland-0", "wayland-1", "wayland-2"):
                if (self.tmp / name).exists():
                    self.nested = name
                    break
            if self.nested:
                break
            time.sleep(0.05)
        if not self.nested:
            die("River nested não subiu")

        deadline = time.monotonic() + 8
        while time.monotonic() < deadline and not pid_file.exists():
            time.sleep(0.05)
        if not pid_file.exists():
            die("WM PID não apareceu")
        self.wm_pid = int(pid_file.read_text().strip())

        wm_sock = self.tmp / "maindeck-wm.sock"
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline and not wm_sock.exists():
            time.sleep(0.05)
        if not wm_sock.exists():
            die("WM IPC socket não apareceu")

        child_env = env.copy()
        child_env["WAYLAND_DISPLAY"] = self.nested
        bar_bin = REPO / "build" / "maindeck-bar"
        self.bar = subprocess.Popen(
            [str(bar_bin)],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            env=child_env,
        )
        time.sleep(0.4)
        if self.bar.poll() is not None:
            die("maindeck-bar nested encerrou durante startup")

        self.dialog = subprocess.Popen(
            [sys.executable, str(REPO / "tools" / "test-dialog.py")],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
            bufsize=1,
            env=child_env,
        )

        wm_env = read_environ(self.wm_pid)
        if wm_env.get("MAINDECK_LOG", "").lower().startswith("d"):
            die("WM nested subiu com MAINDECK_LOG=debug")
        bar_env = read_environ(self.bar.pid)
        if bar_env.get("MAINDECK_LOG", "").lower().startswith("d"):
            die("bar nested subiu com MAINDECK_LOG=debug")

        self.send("open PERF_READY none perfready")
        self.send("close PERF_READY")
        time.sleep(0.25)

    def send(self, cmd: str) -> float:
        if self.dialog is None or self.dialog.stdin is None or self.dialog.stdout is None:
            die("dialog não inicializado")
        start = time.perf_counter()
        self.dialog.stdin.write(cmd + "\n")
        self.dialog.stdin.flush()
        deadline = time.monotonic() + 4
        expected = f"ack {cmd}"
        while time.monotonic() < deadline:
            ready, _, _ = select.select([self.dialog.stdout], [], [], 0.1)
            if not ready:
                continue
            line = self.dialog.stdout.readline()
            if not line:
                die("test-dialog encerrou")
            line = line.strip()
            if line == expected or line.startswith("WINDOW_ALREADY_OPEN"):
                return (time.perf_counter() - start) * 1000.0
            if line.startswith("UNKNOWN_COMMAND"):
                die(line)
        die(f"timeout esperando ack: {cmd}")
        return 0.0

    def measure(self, name: str, ops, settle: float = 0.15) -> ScenarioResult:
        before_wm = read_proc_sample(self.wm_pid)
        before_bar = read_proc_sample(self.bar.pid)
        start = time.perf_counter()
        ack_times = ops()
        if settle > 0:
            time.sleep(settle)
        wall_ms = (time.perf_counter() - start) * 1000.0
        after_wm = read_proc_sample(self.wm_pid)
        after_bar = read_proc_sample(self.bar.pid)
        return ScenarioResult(
            name=name,
            ops=len(ack_times),
            wall_ms=wall_ms,
            wm_cpu_ms=((after_wm.cpu_ticks - before_wm.cpu_ticks) * 1000.0) / HZ,
            bar_cpu_ms=((after_bar.cpu_ticks - before_bar.cpu_ticks) * 1000.0) / HZ,
            wm_rss_delta_kb=after_wm.rss_kb - before_wm.rss_kb,
            bar_rss_delta_kb=after_bar.rss_kb - before_bar.rss_kb,
            wm_read_bytes=after_wm.read_bytes - before_wm.read_bytes,
            wm_write_bytes=after_wm.write_bytes - before_wm.write_bytes,
            bar_read_bytes=after_bar.read_bytes - before_bar.read_bytes,
            bar_write_bytes=after_bar.write_bytes - before_bar.write_bytes,
            wm_syscalls_io=(after_wm.syscr - before_wm.syscr) + (after_wm.syscw - before_wm.syscw),
            bar_syscalls_io=(after_bar.syscr - before_bar.syscr) + (after_bar.syscw - before_bar.syscw),
            wm_ctx_switches=(after_wm.voluntary_cs - before_wm.voluntary_cs)
            + (after_wm.nonvoluntary_cs - before_wm.nonvoluntary_cs),
            bar_ctx_switches=(after_bar.voluntary_cs - before_bar.voluntary_cs)
            + (after_bar.nonvoluntary_cs - before_bar.nonvoluntary_cs),
            p50_ack_ms=statistics.median(ack_times) if ack_times else 0.0,
            p95_ack_ms=percentile(ack_times, 0.95),
        )

    def run_scenarios(self):
        n = self.args.iterations
        results = []

        results.append(self.measure("idle_2s", lambda: (time.sleep(2.0), [])[1], settle=0))

        def fullscreen_idle():
            times = [self.send("fullscreen TESTPARENT")]
            time.sleep(0.35)
            start_sample = read_proc_sample(self.wm_pid)
            start = time.perf_counter()
            time.sleep(2.0)
            end_sample = read_proc_sample(self.wm_pid)
            times.append(self.send("unfullscreen TESTPARENT"))
            cpu_ms = ((end_sample.cpu_ticks - start_sample.cpu_ticks) * 1000.0) / HZ
            print(f"fullscreen_idle_window cpu_ms={cpu_ms:.3f} wall_ms={(time.perf_counter() - start) * 1000.0:.3f}")
            return times

        results.append(self.measure("fullscreen_idle_2s", fullscreen_idle, settle=0.15))

        def protocol_child_churn():
            times = []
            for i in range(n):
                name = f"PROTO{i}"
                times.append(self.send(f"open {name} TESTPARENT protochild"))
                times.append(self.send(f"close {name}"))
            return times

        results.append(self.measure("protocol_child_churn", protocol_child_churn))

        def implicit_child_churn():
            times = [self.send("open Steam none steam")]
            time.sleep(0.25)
            for i in range(n):
                name = f"AboutSteam{i}"
                times.append(self.send(f"open {name} none steam"))
                times.append(self.send(f"close {name}"))
            times.append(self.send("close Steam"))
            return times

        results.append(self.measure("implicit_steam_child_churn", implicit_child_churn))

        def fullscreen_child_churn():
            times = [self.send("fullscreen TESTPARENT")]
            time.sleep(0.25)
            for i in range(max(1, n // 2)):
                name = f"FULL{i}"
                times.append(self.send(f"open {name} TESTPARENT fullchild"))
                times.append(self.send(f"close {name}"))
            times.append(self.send("unfullscreen TESTPARENT"))
            return times

        results.append(self.measure("fullscreen_child_churn", fullscreen_child_churn))
        return results


def print_results(results):
    print("")
    print("scenario,ops,wall_ms,wm_cpu_ms,bar_cpu_ms,total_cpu_ms,total_cpu_per_op_ms,wm_rss_delta_kb,bar_rss_delta_kb,wm_read_bytes,wm_write_bytes,bar_read_bytes,bar_write_bytes,wm_io_syscalls,bar_io_syscalls,wm_ctx_switches,bar_ctx_switches,p50_ack_ms,p95_ack_ms")
    for r in results:
        total_cpu_ms = r.wm_cpu_ms + r.bar_cpu_ms
        cpu_per_op = total_cpu_ms / r.ops if r.ops else 0.0
        print(
            f"{r.name},{r.ops},{r.wall_ms:.3f},{r.wm_cpu_ms:.3f},{r.bar_cpu_ms:.3f},"
            f"{total_cpu_ms:.3f},{cpu_per_op:.4f},{r.wm_rss_delta_kb},{r.bar_rss_delta_kb},"
            f"{r.wm_read_bytes},{r.wm_write_bytes},{r.bar_read_bytes},{r.bar_write_bytes},"
            f"{r.wm_syscalls_io},{r.bar_syscalls_io},{r.wm_ctx_switches},{r.bar_ctx_switches},"
            f"{r.p50_ack_ms:.3f},{r.p95_ack_ms:.3f}"
        )


def main():
    parser = argparse.ArgumentParser(description="External maindeck-wm performance benchmark")
    parser.add_argument("-n", "--iterations", type=int, default=30, help="open/close iterations per churn scenario")
    parser.add_argument("--no-build", action="store_true", help="do not run meson compile first")
    args = parser.parse_args()

    bench = Bench(args)
    try:
        bench.setup()
        print(f"nested_display={bench.nested} wm_pid={bench.wm_pid} bar_pid={bench.bar.pid} debug=off iterations={args.iterations}")
        results = bench.run_scenarios()
        print_results(results)
    finally:
        bench.cleanup()


if __name__ == "__main__":
    main()
