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
- `/home/tcfialho/.local/bin/maindeck-launch`
  - **Not built from this repo — a small wrapper script** (embedded verbatim in
    the "Launch feedback" section below). Shows an "Abrindo aplicativo…"
    notification, then execs the app. `maindeck-wm` dismisses the notification
    when a window maps. Used as fuzzel's `--launch-prefix`, so the session's
    launcher depends on it — if it's missing, launching from the menu breaks.

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
    # --launch-prefix runs the picked app through maindeck-launch (loading
    # feedback). Absolute path: it resolves via PATH at fuzzel's exec time, and
    # the bar-launched fuzzel is a child of waybar whose PATH we don't control.
    exec fuzzel \
        --anchor=bottom-left --x-margin=2 --y-margin=34 \
        --width=36 --lines=12 --layer=overlay --border-radius=2 \
        --launch-prefix="$HOME/.local/bin/maindeck-launch"
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

## Launch feedback (`maindeck-launch`)

A heavy app (e.g. a game) can take many seconds to show a window, with no
on-screen feedback in between. `~/.local/bin/maindeck-launch` shows an "Abrindo
aplicativo…" notification at launch; `maindeck-wm` dismisses it when the next
window maps (in `wm_handle_window`, committed source). The mako daemon must be
running for the notification to show (it is started in the session).

Wiring:

- **fuzzel menu** (the heavy/game path): `--launch-prefix=$HOME/.local/bin/maindeck-launch`
  in `fuzzel-toggle.sh` (above), so every picked app runs through it.
- **Keybind launches** (`Super+Return` → kitty) do **not** use it — they open
  fast and don't need a loading hint. (Easy to add later: wrap the command in
  `maindeck-wm`'s `spawn_*`.)

Design notes:

- Correlation is **coarse on purpose**: the notification is cleared by the
  *next* window to map, not matched to a specific `app_id`. A game launched via
  Steam carries an `app_id` unrelated to the command, so per-app matching can't
  work. The id is tracked via `${XDG_RUNTIME_DIR}/maindeck-loading.id`.
- The notification **self-expires** (`-t 30000`) as a backstop for apps that
  never open a window (crash / no-window tool). A leftover id file is harmless
  (dismissing an already-expired id is a no-op) and is cleared by the next map.
- Trade-off: every menu launch flashes the notification, even light apps — but
  it clears in well under a second when their window maps.

`~/.local/bin/maindeck-launch` (verbatim):

```sh
#!/usr/bin/env bash
# maindeck-launch CMD [ARGS...]
# Shows a "loading" notification, then execs the command. maindeck-wm clears it
# when the next window maps. Correlation is coarse (one pending state via an id
# file, not per-app_id) because a game launched via Steam has an unrelated
# app_id. The notification self-expires (-t) as a backstop.
set -u
ID_FILE="${XDG_RUNTIME_DIR:-/tmp}/maindeck-loading.id"
nid="$(notify-send -a maindeck-loading -p -t 30000 \
    "Abrindo aplicativo…" "Aguarde, carregando." 2>/dev/null || true)"
if [ -n "${nid:-}" ]; then
    printf '%s\n' "$nid" > "$ID_FILE" 2>/dev/null || true
fi
exec "$@"
```

The WM side (committed) dismisses it via `makoctl dismiss -n <id>` read from
that id file when a window maps.

## Notifications (mako)

The notification daemon is **mako**, run as the systemd user service
`mako.service` (shared across sessions; started on demand when a Wayland
display exists). River's init does not spawn it. Both the MainDeck OSD and the
launch-feedback notification depend on it.

Two machine-local concerns live outside the repo:

1. **`~/.config/mako/config`** — a 10-second ceiling so nothing lingers:

   ```ini
   # Rede de segurança: nenhuma notificação fica mais que 10s na tela.
   # Sem isso, o mako usa default-timeout=0 (nunca expira sozinho), e qualquer
   # notificação sem --expire-time explícito ficaria empilhada para sempre.
   # O OSD do MainDeck já pede 1200ms via notify-send; este teto só pega o resto.
   default-timeout=10000
   ```

   Apply without restarting: `makoctl reload`. Without this file mako's
   `default-timeout` is `0` (never auto-expires) — that's why OSDs used to pile
   up and stay forever.

2. **The OSD synchronous hint (committed source, listed here for context).**
   `maindeck-wm`'s `osd()` passes
   `--hint=string:x-canonical-private-synchronous:maindeck-osd`. mako collapses
   all notifications sharing that value into one slot, so a new OSD **replaces**
   the previous one instead of stacking. This is a mako feature; a different
   notification daemon may not honor the hint (OSDs would stack again).

The OSD itself is intentionally sparse: the WM only notifies for the two
navigation no-ops (`sem janela invisível à direita/esquerda`) where nothing
moves on screen. Every command with a visible effect is silent.

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
- Power menu: `/home/tcfialho/.config/wlogout/power.sh` (Windows-style wlogout
  menu — see the "Power menu" section below)

`wlr/taskbar` (`all-outputs: true`): `on-click` = `activate` (raise/focus the
window — this is the Windows-style taskbar behavior), `on-click-middle` =
`close`. The taskbar only shows buttons because `maindeck-proxy` injects the
`output_enter` event (see the proxy notes above).

Style:

- Windows 7/10-like bottom taskbar.
- Opaque dark gradient background.
- Compact square-ish launcher buttons.
- Highlighted active taskbar item.

The packaged user unit `/usr/lib/systemd/user/waybar.service` exists but is
disabled. River owns Waybar startup for this session.

## Power menu (wlogout, Windows-style)

The Waybar power button (`custom/power` → `~/.config/wlogout/power.sh`) opens a
**wlogout** menu styled like the Windows power menu: a small centered row of
four large icon buttons — **Sair · Desligar · Reiniciar · Reiniciar no
Windows**.

Architecture (one source of truth): **wlogout is only the front-end.** Each
button's action calls `~/.config/waybar/power-menu.sh <action>`, which keeps all
the robust logic (River-correct logout, reboot-into-Windows via
`efibootmgr --bootnext` then reboot). `power-menu.sh` still opens its old
fuzzel/wofi/rofi/zenity menu when called with **no argument**; the
direct-action mode (`logout`/`poweroff`/`reboot`/`reboot-windows`) was added so
wlogout (or anything) can trigger one action without a menu.

**"Reiniciar no Windows" uses `sudo -n`, not `pkexec`.** River runs no polkit
agent and the menu is launched with no controlling tty, so `pkexec` can't prompt
and fails silently (the original bug — it set no BootNext and didn't reboot).
Since sudo here is passwordless (`NOPASSWD: ALL`), `reboot_windows()` runs
`sudo -n efibootmgr --bootnext <Windows entry> && systemctl reboot` (pkexec kept
only as a fallback). Note: `efibootmgr --delete-bootnext` is broken on this
firmware, but that's irrelevant — BootNext is a one-shot consumed by the next
boot, and the flow sets it *and* reboots immediately.

Requirements / pieces (all out-of-repo):

- **Package:** `wlogout` (installed from the `cachyos` repo:
  `sudo pacman -S wlogout`). Not a daemon — it runs only while the menu is open
  (0 RAM at rest).
- `~/.config/wlogout/power.sh` — the launcher/wrapper (toggles wlogout; sets the
  layout/CSS and the big margins that keep the small button row centered instead
  of stretched across the screen).
- `~/.config/wlogout/layout` — the four buttons → `power-menu.sh <action>`.
- `~/.config/wlogout/style.css` — Windows-look CSS (fixed 140px buttons so
  wlogout can't stretch them; 56px icons at `background-position: center 32%`;
  17px labels; Windows-blue hover). NOTE: do **not** add `padding-top` to push
  the label down — it inflates the button height and opens a big gap between
  icon and label; let GTK vertically center the label instead.
- `~/.config/wlogout/icons/windows.svg` — the 4-square Windows logo for the
  "Reiniciar no Windows" button (wlogout's icon set has no Windows glyph).
- `~/.config/waybar/power-menu.sh` — the action backend (logout / poweroff /
  reboot / reboot-windows), unchanged except for the direct-action arg parsing.
- The Waybar `custom/power` `on-click` points at `power.sh` (in
  `config.jsonc`). Waybar caches its config in memory; after editing it, reload
  with `kill -USR2 $(pgrep -x waybar)` (a soft reload — does NOT restart the
  process or drop its proxy connection) or just re-login.

`~/.config/wlogout/power.sh` (verbatim):

```sh
#!/usr/bin/env bash
# Windows-style power menu (wlogout front-end → power-menu.sh actions).
# Small row of compact buttons centered on screen. Toggle: if open, close it.
set -u
if pkill -x wlogout 2>/dev/null; then
    exit 0
fi
exec wlogout \
    --buttons-per-row 4 \
    --column-spacing 8 \
    --row-spacing 8 \
    --margin-top 470 --margin-bottom 470 \
    --margin-left 650 --margin-right 650 \
    --layout "$HOME/.config/wlogout/layout" \
    --css "$HOME/.config/wlogout/style.css" \
    --protocol layer-shell
```

`~/.config/wlogout/layout` (verbatim):

```
{ "label":"logout",   "action":"/home/tcfialho/.config/waybar/power-menu.sh logout",         "text":"Sair",                 "keybind":"e" }
{ "label":"shutdown", "action":"/home/tcfialho/.config/waybar/power-menu.sh poweroff",       "text":"Desligar",             "keybind":"s" }
{ "label":"reboot",   "action":"/home/tcfialho/.config/waybar/power-menu.sh reboot",         "text":"Reiniciar",            "keybind":"r" }
{ "label":"windows",  "action":"/home/tcfialho/.config/waybar/power-menu.sh reboot-windows", "text":"Reiniciar no Windows", "keybind":"w" }
```

`~/.config/wlogout/style.css` and `icons/windows.svg` are short; see those files
for the exact look. The `power-menu.sh` backend logs errors to
`${XDG_CACHE_HOME:-$HOME/.cache}/waybar-power-menu.log`.

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
| `~/.config/mako/config` | user | Notification 10s ceiling (`default-timeout`). Without it OSDs never auto-expire. |
| `~/.config/wlogout/{power.sh,layout,style.css,icons/windows.svg}` | user | Windows-style power menu (front-end → power-menu.sh). |
| `wlogout` (pkg) | system | Power-menu UI; install `sudo pacman -S wlogout` (cachyos repo). |
| `/usr/share/wayland-sessions/river.desktop` | root | How SDDM launches River. |
| `~/.local/bin/maindeck-wm`, `~/.local/bin/maindeck-proxy` | user | Installed builds of this repo. |
| `~/.local/bin/maindeck-launch` | user | Launcher wrapper — loading notification (fuzzel `--launch-prefix`). |
