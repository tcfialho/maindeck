# Local session configuration

This file documents machine-local configuration that is required by the
current MainDeck/River session but is not stored directly in this repository.

The paths below describe the current notebook setup. They should be treated as
deployment notes, not as portable project source. Everything here reflects the
**current, working** state (verified 2026-05-29); git history is the changelog.

> The single most important / least reproducible dependency is **keyd** (see
> the [Keyboard](#keyboard-keyd--the-key-mapping-contract) section). Without it
> the MainDeck keybindings (Win+Tab/←/→, the Win-tap launcher) do not work,
> because the window manager binds the F-keys that keyd synthesizes, not the
> raw keys.

## Build and install

```sh
meson setup build
ninja -C build
# install the built binaries where the River init script expects them:
install -m755 build/maindeck-wm    ~/.local/bin/maindeck-wm
install -m755 build/maindeck-proxy ~/.local/bin/maindeck-proxy
```

`maindeck-wm` is hot-reloadable: after installing, `pkill -x maindeck-wm` and
the River init loop re-execs it (no relogin needed). The proxy is NOT — every
GUI client connects through it, so restarting it ends the session; install it
and pick it up on the next login.

## Installed MainDeck binaries

- `/home/tcfialho/.local/bin/maindeck-wm`
  - Installed copy of the compositor helper built from this repository.
  - Run by the River init script in a restart loop (`while true; … maindeck-wm`).
- `/home/tcfialho/.local/bin/maindeck-proxy`
  - Installed copy of the Wayland proxy built from this repository.
  - **Waybar (and every GUI app launched in the session) connects through this
    proxy** on the `maindeck-0` display. The proxy forwards River's real
    foreign-toplevel handles and injects the `output_enter` event that River
    0.4.5 never sends — without it Waybar's `wlr/taskbar` shows no buttons.
  - Started by the River init script in its own restart loop so a proxy crash
    self-heals (it is a session-wide single point of failure).
  - See `docs/river-waybar-taskbar-research.md` for the full root-cause of the
    earlier EINVAL/configure-timeout crash and the "Direction B" fix.

## River

Main file:

- `/home/tcfialho/.config/river/init`

Current responsibilities:

- Exports `XDG_CURRENT_DESKTOP=river` and `XDG_SESSION_DESKTOP=river`.
- Configures XKB with Brazilian and US layouts:
  - `XKB_DEFAULT_LAYOUT=br,us`
  - `XKB_DEFAULT_VARIANT=,intl`
  - `XKB_DEFAULT_OPTIONS=grp:win_space_toggle`
- Redirects session output to:
  - `/home/tcfialho/.local/state/river/session.log`
- Imports the Wayland and desktop environment variables into the user systemd
  and D-Bus activation environments.
- Starts `/home/tcfialho/.local/bin/sunshine-session-start` in the background.
- Forces the internal display mode:
  - `eDP-1`
  - `1920x1080@165.014999Hz`
- Starts `maindeck-proxy` in a restart loop (background).
- Starts Waybar in a restart loop on the **proxy** display (`maindeck-0`).
- Ends with the `maindeck-wm` restart loop (the foreground of the init script).

The session is launched by SDDM via `/usr/share/wayland-sessions/river.desktop`
(`Exec=river`), so `~/.config/river/init` is the entry point for everything
above.

Proxy launch (its own restart loop — a proxy crash must self-heal because every
client depends on it):

```sh
(
    while true; do
        maindeck-proxy 2>>"$HOME/.local/state/river/session.log"
        echo "maindeck-proxy exited with status $?; restarting in 1s" \
            >>"$HOME/.local/state/river/session.log"
        sleep 1
    done
) &
```

Waybar launch (on `maindeck-0`, i.e. through the proxy — this is what makes the
taskbar show buttons):

```sh
(
    while true; do
        env XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR}" \
            GDK_BACKEND=wayland \
            WAYLAND_DISPLAY="maindeck-0" \
            waybar
        status=$?
        echo "waybar exited with status ${status}; restarting in 1s"
        sleep 1
    done
) &
```

Notes:

- `WAYLAND_DISPLAY="maindeck-0"` (the proxy), **not** `wayland-1`. The earlier
  EINVAL / `configure timeout` crash was a proxy bug (synthetic handle IDs),
  fixed in the proxy itself, not by bypassing it.
- There is **no** `env -u DISPLAY` here. Waybar must keep `DISPLAY` so that X11
  apps launched from its taskbar/start-menu (e.g. Steam) inherit it and can
  reach Xwayland. (Apps launched via the WM keybindings already inherit
  `DISPLAY` from `maindeck-wm`.)

## Keyboard (keyd) — the key-mapping contract

This is the part of the setup that lives **entirely outside any dotfiles repo**
and is required for the MainDeck keybindings to work. `keyd` runs as a system
daemon *below* the compositor, so it transforms keys before River ever sees
them. `maindeck-wm` is configured to bind the keys keyd emits, not the physical
keys.

### Install / enable

- File: `/etc/keyd/default.conf` (root-owned; embedded verbatim below).
- Requires the `keyd` package, installed with root, and the service enabled:
  ```sh
  sudo install -m644 default.conf /etc/keyd/default.conf
  sudo systemctl enable --now keyd
  ```
- Current state on this machine: **enabled and active**.

### `/etc/keyd/default.conf` (verbatim)

```ini
[ids]
*

[main]
# Win mantido + outra tecla = Super (Mod). Win tocado sozinho = F19 (toggle fuzzel).
leftmeta = overload(meta, f19)

[meta]
# Tap/Hold do MainDeck via timeout(tap, 360ms, hold).
# Só F23/F24 chegam ao niri como keysym real neste layout (br,us-intl) — as demais
# F-keys viram XF86* (mídia/launch). Por isso as 3 teclas compartilham F23/F24 com
# modificadores distintos: Tab usa Alt+F23, ← usa F23, → usa F24.
#   tab  TAP=Alt+F23      HOLD=Alt+Ctrl+F23
#   ←    TAP=F23          HOLD=Ctrl+F23
#   →    TAP=F24          HOLD=Ctrl+F24
tab   = timeout(A-f23, 360, A-C-f23)
left  = timeout(f23,   360, C-f23)
right = timeout(f24,   360, C-f24)
```

### What keyd does, and why

- `leftmeta = overload(meta, f19)`:
  - **Tap** Win alone → emits `F19` → opens the fuzzel launcher.
  - **Hold** Win → activates the `[meta]` layer = acts as the real `Super`/`Mod`
    modifier (so `Super+Return`, `Super+Up/Down`, `Super+1..9`, etc. work).
- The `[meta]` layer (active while Win is held) resolves **tap vs hold itself**
  via `timeout(tap, 360ms, hold)` and emits **F-keys**, *not* the raw key —
  and it does **not** pass `Super` through. So the compositor never sees
  `Super+Tab`/`Super+Left`/`Super+Right`; it sees the F-key combos below.
- Only `F23`/`F24` survive as real keysyms in the `br,us-intl` layout (other
  F-keys become `XF86*` media keys), which is why the three keys share `F23`/
  `F24` distinguished by modifiers.

| Physical keys | keyd emits (tap) | keyd emits (hold) |
| --- | --- | --- |
| `Win`              | `F19`         | acts as `Super`/`Mod` |
| `Win` + `Tab`      | `Alt+F23`     | `Alt+Ctrl+F23` |
| `Win` + `←`        | `F23`         | `Ctrl+F23` |
| `Win` + `→`        | `F24`         | `Ctrl+F24` |
| `Win` + `↑` / `↓`  | (passthrough — real `Super+Up/Down`) | — |

### How `maindeck-wm` consumes this (the linkage)

The keyd side above and the `maindeck-wm` side below must agree; this doc is the
only place they are connected. All of this is in committed source
(`seat_manage`, `maindeck-wm.c`). The WM binds the F-key combos keyd emits,
**with no `Super`**:

- `Alt+F23` → toggle ALVO · `Alt+Ctrl+F23` → swap MAIN/DECK
- `F23` → DECK prev · `Ctrl+F23` → promote ALVO to MAIN
- `F24` → DECK next · `Ctrl+F24` → send ALVO to DECK bottom
- `F19` → launcher (fuzzel)

Keys that keyd does **not** remap reach the compositor directly, so the WM binds
them with the real modifier: `Super+Return` (kitty), `Super+Up`/`Super+Down`
(maximize/restore), `Super+Delete` (close), `Alt+F4` (close),
`Super+Shift+Escape` (exit).

The raw `Super+Tab`/`Super+Left`/`Super+Right` are also bound as a fallback for
when keyd is not running; with keyd active they never fire (keyd emits the
F-keys instead).

### XKB layout (set in the River init)

```
XKB_DEFAULT_LAYOUT=br,us
XKB_DEFAULT_VARIANT=,intl
XKB_DEFAULT_OPTIONS=grp:win_space_toggle   # Super+Space toggles br/us
```

The `br,us-intl` choice is why only `F23`/`F24` are usable F-keys (see the keyd
comment).

## Launcher (fuzzel)

- Config: `~/.config/fuzzel/fuzzel.ini`.
- Toggle script: `~/.config/niri/fuzzel-toggle.sh`.

**Path footgun:** the toggle script lives under `~/.config/niri/` for historical
reasons, but the River session depends on it: the `maindeck-wm` `F19` launcher
action runs `bash …/niri/fuzzel-toggle.sh`, and the Waybar start button's
`on-click` points at the same path. Do not move or delete it assuming the
directory name implies it is unused by River.

Current script (final form):

```sh
#!/usr/bin/env bash
set -eu

# Under River, Waybar runs on the proxy display (maindeck-0). If this script is
# invoked from a Waybar button it inherits WAYLAND_DISPLAY=maindeck-0, and a
# fuzzel mapped there does NOT get keyboard focus (the proxy doesn't grant
# layer-shell focus) — so you can't type and "click away to dismiss" never
# fires. Always launch fuzzel on the real River display (the one maindeck-wm
# uses). Derived, not hardcoded, since the socket name can vary. If maindeck-wm
# isn't running, keep the inherited WAYLAND_DISPLAY.
wm_pid="$(pgrep -x maindeck-wm | head -n1 || true)"
if [ -n "${wm_pid:-}" ] && [ -r "/proc/${wm_pid}/environ" ]; then
    wm_disp="$(tr '\0' '\n' < "/proc/${wm_pid}/environ" | sed -n 's/^WAYLAND_DISPLAY=//p' | head -n1 || true)"
    if [ -n "${wm_disp:-}" ]; then
        export WAYLAND_DISPLAY="$wm_disp"
    fi
fi

if pgrep -x fuzzel >/dev/null 2>&1; then
    pkill -x fuzzel
else
    exec fuzzel \
        --anchor=bottom-left --x-margin=2 --y-margin=34 \
        --width=36 --lines=12 --layer=overlay --border-radius=2
fi
```

Behavior notes:

- It is a **toggle**: Win (or the start button) opens it; pressing again closes
  it.
- It does **not** pass `--keyboard-focus`, so fuzzel uses its default
  (`exclusive`) and opens already focused — you can type immediately. **This
  only holds if fuzzel runs on the real River display**, which is why the script
  forces `WAYLAND_DISPLAY` to the WM's display (see the comment above): a fuzzel
  on `maindeck-0` gets no keyboard focus, so launching it from the Waybar button
  used to open an unfocused, undismissable launcher.
- "Click outside to dismiss":
  - Fuzzel's own `exit-on-keyboard-focus-loss=yes` (in `fuzzel.ini`) closes it
    when keyboard focus moves away — this is the primary mechanism and now works
    from both the Win key and the Waybar button (both run on the WM's display).
  - `maindeck-wm` also closes it when focus moves to a normal window (a backup
    via `close_launcher`/`pkill -x fuzzel`).
  - On the empty desktop (no windows) there is no window to click, so void-clicks
    do not dismiss it in River — use `Esc` or Win again.

## Waybar

Main files:

- `/home/tcfialho/.config/waybar/config.jsonc`
- `/home/tcfialho/.config/waybar/style.css`
- `/home/tcfialho/.config/waybar/power-menu.sh`
- `/home/tcfialho/.config/waybar/power_menu.xml`

Behavior:

- Bottom bar, `height: 32`, `layer: top`.
- Left modules:
  - `custom/start`
  - `custom/browser`
  - `custom/files`
  - `custom/terminal`
  - `wlr/taskbar`
- Right modules:
  - `tray`
  - `pulseaudio`
  - `battery`
  - `clock`
  - `custom/power`

Launcher actions (`on-click`):

- Start/menu button: `/home/tcfialho/.config/niri/fuzzel-toggle.sh`
- Browser: `chromium`
- Files: `thunar`
- Terminal: `kitty`
- Power menu: `/home/tcfialho/.config/waybar/power-menu.sh`

`wlr/taskbar` (`all-outputs: true`): `on-click` = `activate` (raise/focus the
window — this is the Windows-style taskbar behavior), `on-click-middle` =
`close`. The taskbar only shows buttons because `maindeck-proxy` injects the
`output_enter` event (see the proxy notes above).

Style:

- Windows 7/10-like bottom taskbar.
- Opaque dark gradient background.
- Compact square-ish launcher buttons.
- Highlighted active taskbar item.

Power menu:

- Prefers `fuzzel --dmenu`.
- Falls back to `wofi`, `rofi`, `bemenu`, `tofi`, then `zenity`.
- Supports:
  - lock
  - logout
  - suspend
  - hibernate
  - reboot into Windows through `efibootmgr --bootnext`
  - reboot
  - power off
- Detects `niri`, `labwc`, `river`, `sway`, and `hyprland` for logout.
- Logs errors to:
  - `${XDG_CACHE_HOME:-$HOME/.cache}/waybar-power-menu.log`

The packaged user unit `/usr/lib/systemd/user/waybar.service` exists but is
disabled. River owns Waybar startup for this session.

## Sunshine

Sunshine is split into two phases:

1. A system service starts Sunshine at the SDDM login screen.
2. A user service stops that pre-login instance and starts Sunshine again
   inside the graphical user session.

### User session starter

File:

- `/home/tcfialho/.local/bin/sunshine-session-start`

Behavior:

- Imports session variables into D-Bus activation and user systemd:
  - `WAYLAND_DISPLAY`
  - `XDG_CURRENT_DESKTOP`
  - `XDG_SESSION_DESKTOP`
  - `XDG_SESSION_TYPE`
  - `DISPLAY`
  - `DBUS_SESSION_BUS_ADDRESS`
  - `XDG_SESSION_ID`
- Starts:
  - `sunshine-after-login.service`

### User systemd service

File:

- `/home/tcfialho/.config/systemd/user/sunshine-after-login.service`

State:

- Enabled.
- Wanted by `graphical-session.target`.

Important behavior:

- Runs after `graphical-session.target` and `xdg-desktop-portal.service`.
- Stops the system pre-login service before starting the user-session Sunshine:
  - `/usr/bin/sudo -n /usr/bin/systemctl stop sunshine-prelogin.service`
- Kills any remaining Sunshine process for `tcfialho`.
- Sleeps 5 seconds before launching `/usr/bin/sunshine`.
- Restarts the pre-login service after the user service stops:
  - `/usr/bin/sudo -n /usr/bin/systemctl start sunshine-prelogin.service`
- Uses `Restart=on-failure`.

The local sudo setup currently allows passwordless sudo, which is why the
`sudo -n systemctl ...` calls work from the service.

### System pre-login service

File:

- `/etc/systemd/system/sunshine-prelogin.service`

State:

- Enabled.
- Wanted by `multi-user.target`.

Important behavior:

- Waits for `display-manager.service` and `systemd-user-sessions.service`.
- Sleeps 5 seconds.
- Starts:
  - `/usr/local/bin/sunshine-prelogin-start`
- Uses `Restart=on-failure`.

### Pre-login launcher

File:

- `/usr/local/bin/sunshine-prelogin-start`

Important behavior:

- Runs Sunshine as user `tcfialho`.
- Waits for `/run/user/1000` and the user D-Bus socket.
- Waits for SDDM Xorg on `DISPLAY=:0`.
- Copies SDDM's Xauthority file into:
  - `/run/user/1000/sunshine-sddm.xauthority`
- Changes `/dev/uinput` ownership to `tcfialho:tcfialho` when present.
- Executes `/usr/bin/sunshine` through `runuser` with:
  - `HOME=/home/tcfialho`
  - `XDG_RUNTIME_DIR=/run/user/1000`
  - `DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus`
  - `DISPLAY=:0`
  - `XAUTHORITY=/run/user/1000/sunshine-sddm.xauthority`

### Sunshine application config

Files:

- `/home/tcfialho/.config/sunshine/sunshine.conf`
- `/home/tcfialho/.config/sunshine/apps.json`

Current `sunshine.conf` settings:

- `controller = enabled`
- `keyboard = enabled`
- `mouse = enabled`
- `nvenc_spatial_aq = enabled`
- `sw_preset = ultrafast`
- `upnp = enabled`
- `vk_rc_mode = 4`
- `vk_tune = 3`
- `wan_encryption_mode = 0`

Configured Sunshine apps:

- `Desktop`
- `Low Res Desktop`
  - `do`: `xrandr --output HDMI-1 --mode 1920x1080`
  - `undo`: `xrandr --output HDMI-1 --mode 1920x1200`
- `Steam Big Picture`
  - starts `setsid steam steam://open/bigpicture`
  - stops with `setsid steam steam://close/bigpicture`

Do not commit or copy Sunshine credentials:

- `/home/tcfialho/.config/sunshine/credentials/`

## Useful checks

```sh
# session / init
sh -n ~/.config/river/init
ps -eo pid,comm,args | rg -i 'waybar|maindeck|river|sunshine|keyd'

# keyboard (keyd)
systemctl status keyd --no-pager          # must be enabled + active
sudo keyd monitor                          # live: shows what keyd emits per key
#   tap Win → f19 ; hold Win + Tab → A-f23 / A-C-f23 ; etc.

# what maindeck-wm actually receives (after keyd) — it logs each bound key:
tail -f ~/.local/state/maindeck/maindeck.log   # "[EVENT] key pressed: tap=N hold=M"

# sunshine
systemctl --user status sunshine-after-login.service --no-pager
systemctl status sunshine-prelogin.service --no-pager
journalctl --user -b --no-pager | rg -i 'waybar|sunshine|river'
```

## Summary: out-of-repo files this WM depends on

| File | Owner | Purpose |
| --- | --- | --- |
| `/etc/keyd/default.conf` | root | Key remapping (Win→F19, Tab/←/→→F23/F24). **Critical.** |
| `~/.config/river/init` | user | River session entry: env, proxy, waybar, WM loops. |
| `~/.config/niri/fuzzel-toggle.sh` | user | Launcher toggle (path is historical; used by River). |
| `~/.config/fuzzel/fuzzel.ini` | user | Launcher appearance. |
| `~/.config/waybar/{config.jsonc,style.css,power-menu.sh}` | user | Taskbar + start menu. |
| `/usr/share/wayland-sessions/river.desktop` | root | How SDDM launches River. |
| `~/.local/bin/maindeck-wm`, `~/.local/bin/maindeck-proxy` | user | Installed builds of this repo. |
