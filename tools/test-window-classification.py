#!/usr/bin/env python3
"""Headless regression: independent same-app windows and natural dialog sizing."""
import os
from pathlib import Path
import re
import signal
import subprocess
import tempfile
import time

repo = Path(__file__).resolve().parent.parent
wm = repo / 'build/maindeck-wm'
river = Path.home() / '.local/bin/river'
probe_source = repo / 'janelas-filhas-task/autofloat-test/probe.c'


def wait_for(predicate, seconds=8):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(.05)
    raise AssertionError('Timed out waiting for compositor/test state')


with tempfile.TemporaryDirectory(prefix='maindeck-regression-') as tmp:
    tmp = Path(tmp)
    for mode, output in [('client-header', 'xdg-shell-protocol.h'),
                         ('private-code', 'xdg-shell-protocol.c')]:
        subprocess.run(['wayland-scanner', mode,
                        '/usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml',
                        str(tmp / output)], check=True)
    probe = tmp / 'probe'
    subprocess.run(['cc', str(probe_source), str(tmp / 'xdg-shell-protocol.c'),
                    '-I' + str(tmp), '-lwayland-client', '-o', str(probe)], check=True)
    runtime = tmp / 'runtime'
    runtime.mkdir(mode=0o700)
    log = tmp / 'wm.log'
    init = tmp / 'init'
    init.write_text(f'#!/bin/sh\nexec "{wm}"\n')
    init.chmod(0o700)
    env = dict(os.environ, XDG_RUNTIME_DIR=str(runtime), WLR_BACKENDS='headless',
               WLR_HEADLESS_OUTPUTS='1', WLR_RENDERER='pixman',
               MAINDECK_LOG='debug', MAINDECK_LOG_PATH=str(log),
               XDG_CONFIG_HOME=str(tmp / 'config'))
    env.pop('WAYLAND_DISPLAY', None)
    env.pop('MAINDECK_IMPLICIT_PARENT_APP_ID', None)
    env.pop('MAINDECK_IMPLICIT_PARENT_TITLES', None)
    processes = []
    with (tmp / 'river.log').open('w') as river_log:
        compositor = subprocess.Popen([str(river), '-no-xwayland', '-c', str(init)],
                                      env=env, stdout=river_log, stderr=river_log,
                                      start_new_session=True)
        try:
            wait_for(lambda: list(runtime.glob('wayland-*')) and log.exists())
            env['WAYLAND_DISPLAY'] = next(p.name for p in runtime.glob('wayland-*')
                                          if p.is_socket())
            def launch(mode, size=None):
                output = (tmp / f'probe-{len(processes)}.log').open('w')
                p = subprocess.Popen([str(probe), mode] + ([size] if size else []),
                                     env=env, stdout=output, stderr=output)
                processes.append((p, output))
                time.sleep(.35)
                assert p.poll() is None, 'Probe exited unexpectedly'
            launch('--normal')
            launch('--normal')
            text = log.read_text()
            assert text.count('classified as independent root: "probe-normal"') == 2, text
            assert 'auto-classified as child' not in text
            print('PASS: two same-app toplevels remain independent')
            launch('--min-hint')
            text = log.read_text()
            assert 'classified as independent root: "probe-normal"' in text
            assert 'min=179x36' in text
            assert 'proposed=227x76' not in text
            print('PASS: Chromium-like minimum hints do not shrink a new toplevel')
            launch('--child', '800x600')
            wait_for(lambda: '[CHILD] "probe-child"' in log.read_text())
            launch('--normal')  # Move the parent to the smaller DECK box.
            text = log.read_text()
            assert 'child dimensions proposed: "probe-child" implicit=0 proposed=0x0' in text
            sizes = re.findall(r'\[CHILD\] "probe-child".*size=(\d+)x(\d+)', text)
            mapped = [(int(w), int(h)) for w, h in sizes if int(w) and int(h)]
            assert mapped and all(w >= 800 and h >= 600 for w, h in mapped), sizes
            assert 'child clamped: "probe-child"' not in text
            launch('--fixed', '400x300')
            text = log.read_text()
            assert 'window designated as floating (hint-capped)' in text
            assert '[FLOAT] "probe-fixed"' in text
            print('PASS: fixed-size dialogs still float after pending classification')
            assert compositor.poll() is None
            print('PASS: declared dialog preserves 800x600 natural size above DECK')
        except Exception:
            print(log.read_text()[-15000:] if log.exists() else (tmp / 'river.log').read_text())
            raise
        finally:
            for p, output in processes:
                if p.poll() is None:
                    p.terminate()
                p.wait(timeout=5)
                output.close()
            os.killpg(compositor.pid, signal.SIGTERM)
            compositor.wait(timeout=5)
