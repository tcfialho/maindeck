#!/usr/bin/env python3
"""Headless integration for resident launcher, asynchronous icons and live logs."""
import os
from pathlib import Path
import signal
import socket
import subprocess
import tempfile
import time

repo = Path(__file__).resolve().parent.parent

def wait_for(check, message, timeout=8):
    until=time.monotonic()+timeout
    while time.monotonic()<until:
        if check(): return
        time.sleep(.03)
    raise AssertionError(message)

with tempfile.TemporaryDirectory(prefix='maindeck-desktop-perf-') as directory:
    tmp=Path(directory)
    runtime=tmp/'runtime'; runtime.mkdir(mode=0o700)
    data=tmp/'data/applications'; data.mkdir(parents=True)
    # Deterministic, isolated catalog; launching an entry must work with the worker alive.
    (data/'test.desktop').write_text('[Desktop Entry]\nType=Application\nName=Test entry\nExec=touch '+str(tmp/'launched')+'\nIcon=application-x-executable\n')
    init=tmp/'init'; init.write_text(f'#!/bin/sh\nexec "{repo / "build/maindeck-wm"}"\n'); init.chmod(0o700)
    env=dict(os.environ,XDG_RUNTIME_DIR=str(runtime),XDG_DATA_HOME=str(tmp/'data'),
             XDG_DATA_DIRS=str(tmp/'system'),XDG_CACHE_HOME=str(tmp/'cache'),
             XDG_CONFIG_HOME=str(tmp/'config'),MAINDECK_LOG='warn',
             MAINDECK_LOG_PATH=str(tmp/'wm.log'),WLR_BACKENDS='headless',
             WLR_RENDERER='pixman',WLR_HEADLESS_OUTPUTS='1',
             DBUS_SESSION_BUS_ADDRESS='unix:path='+str(tmp/'no-dbus'),
             PULSE_SERVER='unix:'+str(tmp/'no-pulse'))
    env.pop('WAYLAND_DISPLAY',None)
    processes=[]
    with (tmp/'session.txt').open('w') as log:
        river=subprocess.Popen([str(Path.home()/'.local/bin/river'),'-no-xwayland','-c',str(init)],env=env,stdout=log,stderr=log,start_new_session=True)
        try:
            wait_for(lambda:(runtime/'maindeck-wm.sock').is_socket(),'WM did not start')
            env['WAYLAND_DISPLAY']=next(p.name for p in runtime.glob('wayland-*') if p.is_socket())
            keyboard=subprocess.Popen(['wtype','-s','60000'],env=env,stdout=log,stderr=log);processes.append(keyboard)
            bar=subprocess.Popen([str(repo/'build/maindeck-bar')],env=env,stdout=log,stderr=log);processes.append(bar)
            wait_for(lambda:(runtime/'maindeck-bar.sock').is_socket(),'Bar did not start')
            def image():
                raw=subprocess.check_output(['grim','-t','ppm','-'],env=env)
                magic,dimensions,maximum,pixels=raw.split(b'\n',3)
                width,height=map(int,dimensions.split())
                # Left region excludes the clock; compare actual menu pixels.
                return b''.join(pixels[y*width*3:y*width*3+330*3] for y in range(height))
            time.sleep(.3)
            hidden=image()
            menu=subprocess.Popen([str(repo/'build/maindeck-menu')],env=env,stdout=log,stderr=log);processes.append(menu)
            wait_for(lambda:(runtime/'maindeck-menu.sock').is_socket(),'Launcher socket missing')
            def ipc(command):
                with socket.socket(socket.AF_UNIX,socket.SOCK_DGRAM) as sock:
                    sock.sendto(command.encode(),str(runtime/'maindeck-menu.sock'))
            time.sleep(.5)
            shown=image(); assert shown!=hidden,'Menu did not draw'
            pid=menu.pid
            samples=[]
            for _ in range(8):
                ipc('C'); time.sleep(.08)
                assert image()==hidden,'Closing launcher leaves stale pixels'
                start=time.monotonic(); ipc('T')
                wait_for(lambda:image()!=hidden,'Resident launcher did not show')
                samples.append((time.monotonic()-start)*1000)
                assert menu.pid==pid and menu.poll() is None
            print('PASS: 8 show/hide cycles reuse PID and clear every surface; observed show+capture times ms:', ', '.join(f'{v:.1f}' for v in samples),flush=True)
            # Escape must dismiss without killing the resident process.
            subprocess.run(['wtype','-k','Escape'],env=env,check=True)
            wait_for(lambda:image()==hidden,'Escape did not hide menu')
            assert menu.poll() is None
            subprocess.run(['wtype','-k','F19'],env=env,check=True)
            wait_for(lambda:image()!=hidden,'WM shortcut did not show resident menu')
            subprocess.run(['wtype','-k','F19'],env=env,check=True)
            wait_for(lambda:image()==hidden,'WM shortcut did not toggle resident menu')
            assert menu.pid==pid and menu.poll() is None
            print('PASS: WM shortcut toggles resident launcher without a new process',flush=True)
            ipc('T'); time.sleep(.15)
            subprocess.run(['wtype','zz-no-such-app'],env=env,check=True)
            searched=image(); assert searched!=shown
            ipc('C'); time.sleep(.05); ipc('T'); time.sleep(.15)
            assert image()==shown,'Reopen must reset query/selection without changing appearance'
            print('PASS: Escape, query reset and visual identity survive resident reopen',flush=True)
            subprocess.run(['wtype','-k','Return'],env=env,check=True)
            wait_for(lambda:(tmp/'launched').exists(),'Launching app failed')
            wait_for(lambda:image()==hidden,'Launch did not dismiss menu')
            assert menu.poll() is None
            print('PASS: application launch works with asynchronous loader active',flush=True)
            # Refresh catalog while hidden; a new entry must be searchable and launchable.
            (data/'new.desktop').write_text('[Desktop Entry]\nType=Application\nName=New entry\nExec=touch '+str(tmp/'new-launched')+'\n')
            ipc('T'); time.sleep(.15)
            subprocess.run(['wtype','New entry','-k','Return'],env=env,check=True)
            wait_for(lambda:(tmp/'new-launched').exists(),'Resident catalog failed to refresh')
            print('PASS: installed apps are detected on reopen',flush=True)
            (data/'new.desktop').write_text('[Desktop Entry]\nType=Application\nName=New entry\nExec=touch '+str(tmp/'updated-launch')+'\n')
            time.sleep(.1); ipc('T'); time.sleep(.15)
            subprocess.run(['wtype','New entry','-k','Return'],env=env,check=True)
            wait_for(lambda:(tmp/'updated-launch').exists(),'In-place desktop file edit did not refresh')
            print('PASS: catalog watcher detects in-place application edits',flush=True)
            # Switch diagnostics live, without replacing any process.
            subprocess.run([str(repo/'tools/maindeck-diagnostics'),'debug'],env=env,check=True)
            time.sleep(1.05); ipc('T'); time.sleep(.1); ipc('C'); time.sleep(.1)
            wait_for(lambda:'menu shown (resident)' in (tmp/'session.txt').read_text(),'Live debug did not enable')
            subprocess.run([str(repo/'tools/maindeck-diagnostics'),'off'],env=env,check=True)
            time.sleep(1.05)
            start=(tmp/'session.txt').stat().st_size
            ipc('T'); time.sleep(.1); ipc('C'); time.sleep(.1)
            assert 'menu shown (resident)' not in (tmp/'session.txt').read_text()[start:]
            assert all(p.poll() is None for p in (bar,menu,river))
            print('PASS: diagnostics toggle live while processes remain unchanged',flush=True)
            for _ in range(3):
                with socket.socket(socket.AF_UNIX,socket.SOCK_DGRAM) as sock:
                    sock.sendto(b'fullscreen_on',str(runtime/'maindeck-bar.sock'))
                wait_for(lambda:image()!=hidden,'Fullscreen did not remove bar surface')
                with socket.socket(socket.AF_UNIX,socket.SOCK_DGRAM) as sock:
                    sock.sendto(b'fullscreen_off',str(runtime/'maindeck-bar.sock'))
                wait_for(lambda:image()==hidden,'Bar did not restore with identical pixels')
            assert bar.poll() is None
            print('PASS: 3 fullscreen surface recreation cycles preserve the bar image',flush=True)
        except Exception:
            print((tmp/'session.txt').read_text()[-12000:])
            if (tmp/'wm.log').exists():print((tmp/'wm.log').read_text()[-12000:])
            raise
        finally:
            for process in reversed(processes):
                if process.poll() is None:process.terminate()
                process.wait(timeout=5)
            os.killpg(river.pid,signal.SIGTERM);river.wait(timeout=5)
