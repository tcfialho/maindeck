#!/usr/bin/env python3
import sys
import os
import gi
gi.require_version('Gtk', '3.0')
from gi.repository import Gtk, Gdk, GLib

# Evita bufferização no stdout para sincronização em tempo real com o test runner
sys.stdout.reconfigure(line_buffering=True)

class TestApp:
    def __init__(self):
        # Configurar estilos CSS para diferenciar as cores das janelas
        css_provider = Gtk.CssProvider()
        css_provider.load_from_data(b"""
            .child-bg {
                background-color: #512da8; /* Roxo Escuro */
            }
            .child-text {
                color: #ffffff;
                font-size: 14px;
                font-weight: bold;
            }
            .dummy-bg {
                background-color: #388e3c; /* Verde */
            }
            .dummy-text {
                color: #ffffff;
                font-size: 12px;
            }
        """)
        Gtk.StyleContext.add_provider_for_screen(
            Gdk.Screen.get_default(),
            css_provider,
            Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION
        )

        self.windows = {}
        self.dummy_count = 0
        
        # Cria a janela pai principal
        self.create_parent()

    def create_parent(self):
        parent = Gtk.Window(title="TESTPARENT")
        parent.set_wmclass("test-parent", "test-parent")
        parent.set_role("TESTPARENT")
        parent.set_default_size(400, 300)
        parent.connect("destroy", Gtk.main_quit)
        
        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=10)
        lbl = Gtk.Label(label="Eu sou a JANELA PAI (TESTPARENT)")
        box.pack_start(lbl, True, True, 0)
        parent.add(box)
        
        parent.show_all()
        self.windows["TESTPARENT"] = parent

    def open_window(self, name, parent_name=None):
        if name in self.windows:
            print(f"WINDOW_ALREADY_OPEN {name}")
            return
            
        is_child = parent_name is not None and parent_name in self.windows
        
        window = Gtk.Window(title=name)
        window.set_wmclass(name.lower(), name.lower())
        window.set_role(name)
        window.set_modal(False)
        
        if is_child:
            window.set_transient_for(self.windows[parent_name])
            window.set_default_size(200, 150)
            window.get_style_context().add_class("child-bg")
            lbl_text = f"Eu sou a JANELA FILHA ({name}) - de {parent_name}"
            lbl_class = "child-text"
        else:
            window.set_default_size(150, 100)
            window.get_style_context().add_class("dummy-bg")
            lbl_text = f"Eu sou uma JANELA NORMAL ({name})"
            lbl_class = "dummy-text"
            
        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=10)
        lbl = Gtk.Label(label=lbl_text)
        lbl.get_style_context().add_class(lbl_class)
        box.pack_start(lbl, True, True, 0)
        window.add(box)
        
        window.show_all()
        self.windows[name] = window

    def close_window(self, name):
        if name in self.windows:
            w = self.windows.pop(name)
            w.destroy()
            if name == "TESTPARENT":
                Gtk.main_quit()

    def fullscreen_window(self, name):
        if name in self.windows:
            self.windows[name].fullscreen()

    def unfullscreen_window(self, name):
        if name in self.windows:
            self.windows[name].unfullscreen()

    def minimize_window(self, name):
        if name in self.windows:
            self.windows[name].iconify()

app = TestApp()

def read_stdin(source, condition):
    if condition & GLib.IO_HUP:
        Gtk.main_quit()
        return False
        
    line = sys.stdin.readline()
    if not line:
        Gtk.main_quit()
        return False
        
    cmd_line = line.strip()
    if not cmd_line:
        return True
        
    # Tratamento de aliases retrocompatíveis
    if cmd_line == "child_open":
        app.open_window("TESTCHILD1", "TESTPARENT")
    elif cmd_line == "child_close":
        app.close_window("TESTCHILD1")
    elif cmd_line == "dummy_open":
        app.dummy_count += 1
        app.open_window(f"TESTDUMMY{app.dummy_count}")
    elif cmd_line == "parent_minimize":
        app.minimize_window("TESTPARENT")
    elif cmd_line == "parent_close":
        app.close_window("TESTPARENT")
    elif cmd_line == "quit":
        Gtk.main_quit()
        return False
    else:
        # Parser geral: cmd [arg1] [arg2]
        parts = cmd_line.split()
        if not parts:
            return True
        action = parts[0]
        if action == "open" and len(parts) >= 2:
            parent = parts[2] if len(parts) >= 3 else None
            app.open_window(parts[1], parent)
        elif action == "close" and len(parts) >= 2:
            app.close_window(parts[1])
        elif action == "fullscreen" and len(parts) >= 2:
            app.fullscreen_window(parts[1])
        elif action == "unfullscreen" and len(parts) >= 2:
            app.unfullscreen_window(parts[1])
        elif action == "minimize" and len(parts) >= 2:
            app.minimize_window(parts[1])
        else:
            print(f"UNKNOWN_COMMAND {cmd_line}")
            return True
            
    print(f"ack {cmd_line}")
    return True

# Configura o stdin como não bloqueante e adiciona ao loop do GLib
fd = sys.stdin.fileno()
import fcntl
fl = fcntl.fcntl(fd, fcntl.F_GETFL)
fcntl.fcntl(fd, fcntl.F_SETFL, fl | os.O_NONBLOCK)

GLib.io_add_watch(sys.stdin, GLib.IO_IN | GLib.IO_HUP, read_stdin)

Gtk.main()
