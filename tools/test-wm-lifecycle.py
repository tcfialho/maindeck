#!/usr/bin/env python3
"""Real GTK clients in an isolated headless River; validate focus via GTK events."""
import json
import os
from pathlib import Path
import re
import signal
import socket
import subprocess
import tempfile
import time

repo = Path(__file__).resolve().parent.parent

def wait_for(condition, detail, timeout=8):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if condition():
            return
        time.sleep(.05)
    raise AssertionError(detail)

with tempfile.TemporaryDirectory(prefix='maindeck-lifecycle-') as directory:
    tmp = Path(directory)
    runtime = tmp / 'runtime'
    runtime.mkdir(mode=0o700)
    config = tmp / 'config/maindeck'
    config.mkdir(parents=True)
    (config / 'wm.json').write_text(json.dumps({'floating_app_ids': ['swappy']}))
    init = tmp / 'init'
    init.write_text(f'#!/bin/sh\nexec "{repo / "build/maindeck-wm"}"\n')
    init.chmod(0o700)
    log = tmp / 'wm.log'
    env = dict(os.environ, XDG_RUNTIME_DIR=str(runtime), XDG_CONFIG_HOME=str(tmp / 'config'),
               WLR_BACKENDS='headless', WLR_RENDERER='pixman', WLR_HEADLESS_OUTPUTS='1',
               MAINDECK_LOG='debug', MAINDECK_LOG_PATH=str(log), GDK_BACKEND='wayland',
               MAINDECK_IMPLICIT_PARENT_APP_ID='quirk', MAINDECK_IMPLICIT_PARENT_TITLES='ANCHOR')
    env.pop('WAYLAND_DISPLAY', None)
    app = None
    keyboard = None
    with (tmp / 'river.log').open('w') as compositor_log, (tmp / 'client.log').open('w') as client_log:
        river = subprocess.Popen([str(Path.home() / '.local/bin/river'), '-no-xwayland', '-c', str(init)],
                                 env=env, stdout=compositor_log, stderr=compositor_log, start_new_session=True)
        try:
            wait_for(lambda: (runtime / 'maindeck-wm.sock').is_socket(), 'WM did not start')
            env['WAYLAND_DISPLAY'] = next(p.name for p in runtime.glob('wayland-*') if p.is_socket())
            keyboard = subprocess.Popen(['wtype', '-s', '60000'], env=env, stdout=compositor_log, stderr=compositor_log)
            time.sleep(.2)
            app = subprocess.Popen(['/usr/bin/python3', str(repo / 'tools/test-dialog.py'), '--events'],
                                   env=env, stdin=subprocess.PIPE, stdout=client_log, stderr=client_log, text=True)
            def events():
                return (tmp / 'client.log').read_text()
            def text():
                return log.read_text()
            def send(command):
                app.stdin.write(command + '\n')
                app.stdin.flush()
                wait_for(lambda: f'ack {command}' in events(), 'Client command failed: ' + command)
                time.sleep(.15)
            def focus(title):
                wait_for(lambda: re.findall(r'^FOCUS (.+)$', events(), re.M)[-1:] == [title],
                         'Expected keyboard focus: ' + title)
            def identifier(title):
                last_id = None
                for line in text().splitlines():
                    match = re.search(r'window identifier:.* id=(\w+)', line)
                    if match:
                        last_id = match[1]
                    if last_id and f'classified as independent root: "{title}"' in line:
                        return last_id
                raise AssertionError('No identifier for ' + title)
            def ipc(verb, title):
                with socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM) as sock:
                    sock.sendto(f'{verb} {identifier(title)}'.encode(), str(runtime / 'maindeck-wm.sock'))
            focus('TESTPARENT')
            send('open ROOT1 none shared-app')
            send('open ROOT2 none shared-app')
            assert 'independent root: "ROOT1"' in text() and 'independent root: "ROOT2"' in text()
            focus('ROOT2')
            print('PASS: same-app GTK windows tile independently', flush=True)
            send('open FLOAT1 none swappy')
            focus('FLOAT1')
            send('open FLOAT2 none swappy')
            focus('FLOAT2')
            print('PASS: opening a second floating window gives it keyboard focus', flush=True)
            ipc('activate', 'ROOT2')
            focus('ROOT2')
            print('PASS: taskbar activation transfers focus from floating to tiled', flush=True)
            send('open FLOAT_DIALOG FLOAT2 float-dialog')
            focus('FLOAT_DIALOG')
            time.sleep(.2)
            # The helper paints dialogs purple; verify pixels actually reach the output.
            shot = subprocess.run(['grim', '-t', 'ppm', '-'], env=env, capture_output=True, check=True).stdout
            assert shot.count(bytes((0x51, 0x2d, 0xa8))) > 100, 'Floating dialog was not rendered'
            print('PASS: floating child is visible above its parent and receives focus', flush=True)
            send('close FLOAT_DIALOG')
            focus('FLOAT2')
            print('PASS: closing a floating child restores focus to its root', flush=True)
            ipc('activate', 'ROOT2')
            focus('ROOT2')
            ipc('minimize', 'ROOT2')
            wait_for(lambda: '[MIN] "ROOT2"' in text(), 'Root did not minimize')
            ipc('restore', 'ROOT2')
            focus('ROOT2')
            print('PASS: minimize/restore returns focus to the restored root', flush=True)
            send('fullscreen ROOT2')
            wait_for(lambda: 'fullscreen requested: "ROOT2"' in text(), 'Fullscreen request was not handled')
            focus('ROOT2')
            ipc('minimize', 'ROOT2')
            time.sleep(.2)
            ipc('restore', 'ROOT2')
            focus('ROOT2')
            send('unfullscreen ROOT2')
            print('PASS: fullscreen window can minimize, restore and exit fullscreen', flush=True)
            send('open ANCHOR none quirk')
            send('open QUIRK_DIALOG none quirk')
            wait_for(lambda: 'window became child (implicit): "QUIRK_DIALOG"' in text(), 'Opt-in adoption failed')
            assert re.search(r'child dimensions proposed: "QUIRK_DIALOG" implicit=1 proposed=0x0', text())
            wait_for(lambda: re.search(r'\[CHILD\] "QUIRK_DIALOG".*size=[1-9]\d*x[1-9]\d*', text()),
                     'Implicit dialog did not map at a natural size')
            print('PASS: opt-in compatibility dialog uses client natural dimensions', flush=True)
            assert river.poll() is None and app.poll() is None
        except Exception:
            print('CLIENT:\n' + (tmp / 'client.log').read_text()[-6000:])
            print('WM:\n' + (log.read_text()[-12000:] if log.exists() else 'missing'))
            print('RIVER:\n' + (tmp / 'river.log').read_text()[-3000:])
            raise
        finally:
            if app is not None:
                app.terminate()
                app.wait(timeout=5)
            if keyboard is not None:
                keyboard.terminate()
                keyboard.wait(timeout=5)
            os.killpg(river.pid, signal.SIGTERM)
            river.wait(timeout=5)
