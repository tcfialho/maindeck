# Local session configuration

This file records the current notebook-local River session used while
developing MainDeck. These files live outside the repository and must be kept in
sync manually when deploying to another machine.

## River Entry Point

Path:

```txt
/home/tcfialho/.config/river/init
```

Current flow:

- Exports the River session identity:
  - `XDG_CURRENT_DESKTOP=river`
  - `XDG_SESSION_DESKTOP=river`
- Sets MainDeck production logging:
  - `MAINDECK_LOG` empty by default; set `MAINDECK_LOG=debug` for verbose logs.
- Enables the Steam implicit-parent workaround:
  - `MAINDECK_IMPLICIT_PARENT_APP_ID=steam`
  - `MAINDECK_IMPLICIT_PARENT_TITLES=Steam|Steam Big Picture`
- Starts the Sunshine handoff:
  - `/home/tcfialho/.local/bin/sunshine-session-start &`
- Configures the internal panel mode/refresh:
  - `wlr-randr --output eDP-1 --mode 1920x1080@165.014999Hz --adaptive-sync enabled`
- Starts `maindeck-bar` in a restart loop.
- Starts `maindeck-wm` in the foreground restart loop.

The old `maindeck-proxy` path is disabled. Waybar is not part of the current
River MainDeck session.

## MainDeck Binaries

The session expects locally installed binaries here:

```txt
/home/tcfialho/.local/bin/maindeck-wm
/home/tcfialho/.local/bin/maindeck-bar
/home/tcfialho/.local/bin/maindeck-menu
```

The bar is launched with:

```sh
env XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR}" \
    GDK_BACKEND=wayland \
    WAYLAND_DISPLAY="wayland-1" \
    MAINDECK_LOG="${MAINDECK_LOG}" \
    /home/tcfialho/.local/bin/maindeck-bar
```

`maindeck-wm` connects to the same River display through the session
environment.

## Bar Config

Path:

```txt
/home/tcfialho/.config/maindeck/bar.json
```

Current responsibilities:

- bar height, font, and icon theme;
- quick-launch buttons;
- status modules: volume, battery, clock, power;
- power menu command;
- clock format.

The current quick-launch commands are:

```txt
maindeck-menu --mouse
chromium
thunar
kitty
```

The power button calls:

```txt
/home/tcfialho/.config/wlogout/power.sh
```

## Notifications

`maindeck-wm` uses `notify-send` for sparse OSD messages. The session expects
`mako` to be available and configured by the user session. The OSD uses the
`x-canonical-private-synchronous=maindeck-osd` hint so repeated messages replace
each other instead of stacking.

`maindeck-bar` also emits short mode notifications when game/fullscreen mode is
activated or deactivated.

## Sunshine Handoff

River starts Sunshine through:

```txt
/home/tcfialho/.local/bin/sunshine-session-start
```

That script exports/imports the active Wayland session environment into the
systemd user manager, clears stale X11 Sunshine config for Wayland sessions, and
restarts `sunshine-after-login.service`.

The pre-login SDDM path is handled separately by:

```txt
/etc/systemd/system/sunshine-prelogin.service
/usr/local/bin/sunshine-prelogin-start
```

The pre-login wrapper uses the X11/NvFBC config while the logged-in Wayland
session uses the default KMS config.

## Important Local Files

| Path | Purpose |
|---|---|
| `/etc/keyd/default.conf` | Key remapping configuration (Win→F19, Tab/←/→→F23/F24). |
| `~/.config/river/init` | River session entry point. |
| `~/.config/maindeck/bar.json` | Native MainDeck bar config. |
| `~/.config/mako/config` | Notification behavior for OSD/mode messages. |
| `~/.config/wlogout/power.sh` | Power menu wrapper used by the bar. |
| `~/.local/bin/sunshine-session-start` | Sunshine login-session handoff. |
| `/usr/local/bin/sunshine-prelogin-start` | Sunshine pre-login SDDM launcher. |

## Keyboard Configuration (keyd)

The WM binds the F-key combos keyd emits. The local keyd configuration at `/etc/keyd/default.conf` remaps physical modifier sequences to synthetic keysyms that survive standard layout mappings:

```ini
[main]
leftmeta = overload(meta, f19)

[meta]
# Tap/Hold do MainDeck via timeout(tap, 360ms, hold).
# F19/F23/F24 são as F-keys que sobrevivem como keysym REAL até o compositor neste
# layout (br,us-intl); as demais (ex.: F14) viram XF86* ou não casam o keysym esperado.
# Por isso as 3 teclas de navegação compartilham F23/F24 com modificadores distintos:
#   tab  TAP=Alt+F23      HOLD=Alt+Ctrl+F23
#   ←    TAP=F23          HOLD=Ctrl+F23
#   →    TAP=F24          HOLD=Ctrl+F24
tab   = timeout(A-f23, 360, A-C-f23)
left  = timeout(f23,   360, C-f23)
right = timeout(f24,   360, C-f24)
# Win+Shift tocado (solta rápido, SEM outra tecla) = Ctrl+F19 → toggle maximize no WM.
# Win+Shift SEGURADO + outra tecla (ex.: Escape) → Shift age como modificador →
# Super+Shift+<tecla> chega ao compositor (assim Super+Shift+Escape = exit sobrevive).
leftshift = overload(shift, C-f19)
```

### What keyd does, and why

- **Tap Win**: emits `F19` (opens fuzzel launcher).
- **Hold Win**: acts as `Super`/`Mod` (triggers window manager bindings like `Super+Return`).
- **Win+Shift (tap)**: keyd overloaded Shift inside `[meta]` to emit `Ctrl+F19` on tap (triggering `ACTION_TOGGLE_MAXIMIZE` in the WM).
- **Win+Shift (hold) + Key**: Shift acts as modifier, so `Super+Shift+Escape` still triggers `ACTION_EXIT`.

## Quick Checks

```sh
# session / init
sh -n ~/.config/river/init
ps -eo pid,comm,args | rg -i 'waybar|maindeck|river|sunshine|keyd'

# keyboard (keyd)
systemctl status keyd --no-pager          # must be enabled + active
sudo keyd monitor                          # live: shows what keyd emits per key
#   tap Win → f19 ; hold Win+Tab → A-f23 / A-C-f23 ; tap Win+Shift → C-f19 ; etc.
#   NOTE: this shows the evdev KEYCODE, NOT the XKB keysym the compositor sees.

# what maindeck-wm actually receives (after keyd) — it logs each bound key. THIS,
# not `keyd monitor`, is the proof a binding matched (no line = keysym mismatch):
tail -f ~/.local/state/maindeck/maindeck.log   # "[EVENT] key pressed: tap=N hold=M"

# sunshine
systemctl --user status sunshine-after-login.service
systemctl status sunshine-prelogin.service
```
