# Local session configuration

This file documents machine-local configuration that is required by the
current MainDeck/River session but is not stored directly in this repository.

The paths below describe the current notebook setup. They should be treated as
deployment notes, not as portable project source.

## Installed MainDeck binaries

- `/home/tcfialho/.local/bin/maindeck-wm`
  - Installed copy of the compositor helper built from this repository.
  - Started by the River init script with `exec`.
- `/home/tcfialho/.local/bin/maindeck-proxy`
  - Installed copy of the Wayland proxy built from this repository.
  - Still started by the River init script for diagnostics and future taskbar
    work.
  - Waybar is no longer launched through this proxy because the proxy-backed
    `maindeck-0` display was causing Waybar to exit after windows appeared.

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
- Starts `maindeck-proxy` in the background.
- Starts Waybar in a restart loop on the real River Wayland display.
- Replaces the shell with:
  - `/home/tcfialho/.local/bin/maindeck-wm`

Waybar launch detail:

```sh
env -u DISPLAY \
    XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR}" \
    GDK_BACKEND=wayland \
    WAYLAND_DISPLAY="${WAYLAND_DISPLAY:-wayland-1}" \
    waybar
```

The important detail is that Waybar is launched on `wayland-1`, not on
`maindeck-0`. The proxy display produced errors like:

- `Timed out waiting for initial .configure`
- `Gdk-Message: Error reading events from display: Argumento invalido`

The River init script keeps Waybar in a simple restart loop:

```sh
while true; do
    waybar
    status=$?
    echo "waybar exited with status ${status}; restarting in 1s"
    sleep 1
done
```

There is also a transient copy of that loop running in the current login
session when this note was written. After the next login, the persistent River
init script is the source of truth.

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

Launcher actions:

- Start/menu button:
  - `/home/tcfialho/.config/niri/fuzzel-toggle.sh`
- Browser:
  - `chromium`
- Files:
  - `thunar`
- Terminal:
  - `xfce4-terminal`
- Power menu:
  - `/home/tcfialho/.config/waybar/power-menu.sh`

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
sh -n ~/.config/river/init
ps -eo pid,comm,args | rg -i 'waybar|maindeck|river|sunshine'
systemctl --user status sunshine-after-login.service --no-pager
systemctl status sunshine-prelogin.service --no-pager
journalctl --user -b --no-pager | rg -i 'waybar|sunshine|river'
```
