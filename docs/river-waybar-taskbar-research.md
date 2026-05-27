# River / Waybar taskbar research handoff

Generated during the 2026-05-26/2026-05-27 debugging session.

This report is a handoff document for continuing the investigation without
needing the prior chat context. It explains why Waybar's `wlr/taskbar` is not
showing windows in the current River + `maindeck-wm` session, what was
downloaded, what was skipped, and which architecture choices remain.

## Short conclusion

1. The local Waybar setup is not the main problem.
   - Waybar is `v0.15.0`.
   - `/usr/bin/waybar` links against `libgtk-layer-shell.so.0`.
   - `/home/tcfialho/.config/waybar/config.jsonc` already includes
     `wlr/taskbar` with `"all-outputs": true`.

2. `wlr/taskbar` depends on `zwlr_foreign_toplevel_handle_v1.output_enter`.
   In Waybar `0.15.0`, the task button is added inside
   `Task::handle_output_enter()`. No `output_enter` means no button.

3. The installed River version is `river 0.4.5-1.1`. The River `v0.4.5`
   source creates `wlr.ForeignToplevelHandleV1` handles and sets title, app id,
   and activated state, but this survey found no call to
   `outputEnter`/`outputLeave` or `output_enter`/`output_leave` for those
   handles.

4. The local smoke test confirms the same behavior:
   - Direct River display `wayland-1`: `toplevels=3 with_output=0`.
   - Proxy display `maindeck-0`: `toplevels=3 with_output=3`.

5. Therefore the proxy "worked" because it fabricated `output_enter` events.
   It is a compatibility shim, not a clean architecture. It should not be the
   final design.

6. A strict WM-contained fix cannot make Waybar's built-in `wlr/taskbar`
   receive compositor-owned foreign-toplevel output events. To keep the exact
   Waybar `wlr/taskbar` module, River must emit correct `output_enter`/
   `output_leave` events. To keep the solution fully inside `maindeck-wm`, use
   a WM-owned taskbar/status architecture instead of `wlr/taskbar`.

## Current repo and session state

Main repo:

- `/home/tcfialho/Documents/poc/maindeck-wm`

Relevant existing documentation:

- `/home/tcfialho/Documents/poc/maindeck-wm/docs/local-session-config.md`

Current previous commit before this report:

- `c3ca255 Document local session setup`

Installed package versions observed:

```txt
river 0.4.5-1.1
waybar 0.15.0-2.1
```

Waybar runtime checks observed:

```txt
Waybar v0.15.0
libgtk-layer-shell.so.0 => /usr/lib/libgtk-layer-shell.so.0
```

Current Waybar taskbar config:

- `/home/tcfialho/.config/waybar/config.jsonc:7`
- `/home/tcfialho/.config/waybar/config.jsonc:42`

Important excerpt:

```jsonc
"modules-left": [
    "custom/start",
    "custom/browser",
    "custom/files",
    "custom/terminal",
    "wlr/taskbar"
],
"wlr/taskbar": {
    "format": "{icon} {title:.22}",
    "icon-size": 18,
    "icon-theme": "Papirus",
    "tooltip-format": "{title}",
    "on-click": "activate",
    "on-click-middle": "close",
    "all-outputs": true,
    "sort-by-app-id": false
}
```

## Local proof: direct River vs proxy

Smoke test source:

- `/home/tcfialho/Documents/poc/maindeck-wm/tools/toplevel-smoke.c`

Built binary:

- `/home/tcfialho/Documents/poc/maindeck-wm/build/maindeck-toplevel-smoke`

Observed result on direct River display:

```txt
WAYLAND_DISPLAY=wayland-1 ./build/maindeck-toplevel-smoke

top title="Terminal - ..." app_id="xfce4-terminal" activated=1 output_enters=0 done=6
top title="Nova guia - Chromium" app_id="chromium" activated=0 output_enters=0 done=1
top title="river wlr/taskbar - Pesquisa Google - Chromium" app_id="chromium" activated=0 output_enters=0 done=1
summary outputs=1 first_output_name=56 foreign_globals=1 toplevels=3 with_output=0
```

Observed result on the proxy display:

```txt
WAYLAND_DISPLAY=maindeck-0 ./build/maindeck-toplevel-smoke

top title="Terminal - ..." app_id="xfce4-terminal" activated=1 output_enters=1 done=7
top title="Nova guia - Chromium" app_id="chromium" activated=0 output_enters=1 done=2
top title="river wlr/taskbar - Pesquisa Google - Chromium" app_id="chromium" activated=0 output_enters=1 done=2
summary outputs=1 first_output_name=56 foreign_globals=1 toplevels=3 with_output=3
```

Interpretation:

- River advertises the foreign-toplevel manager and sends toplevels.
- River does not send output membership events in this session.
- The proxy adds synthetic output membership events.
- Waybar sees buttons only when output membership events exist.

## Waybar evidence

Downloaded local copy of Waybar source used for this check:

- `/tmp/maindeck-river-full-survey/upstream-waybar/taskbar-v0.15.0.cpp`

Source URL:

- `https://raw.githubusercontent.com/Alexays/Waybar/0.15.0/src/modules/wlr/taskbar.cpp`

Waybar manual saved locally:

- `/tmp/maindeck-river-full-survey/upstream-waybar/waybar-wlr-taskbar.arch-man.html`

Manual URL:

- `https://man.archlinux.org/man/extra/waybar/waybar-wlr-taskbar.5.en`

Relevant Waybar source:

- `/tmp/maindeck-river-full-survey/upstream-waybar/taskbar-v0.15.0.cpp:283`
- `/tmp/maindeck-river-full-survey/upstream-waybar/taskbar-v0.15.0.cpp:291`
- `/tmp/maindeck-river-full-survey/upstream-waybar/taskbar-v0.15.0.cpp:295`
- `/tmp/maindeck-river-full-survey/upstream-waybar/taskbar-v0.15.0.cpp:548`
- `/tmp/maindeck-river-full-survey/upstream-waybar/taskbar-v0.15.0.cpp:718`
- `/tmp/maindeck-river-full-survey/upstream-waybar/taskbar-v0.15.0.cpp:748`

Important excerpt:

```cpp
void Task::handle_output_enter(struct wl_output* output) {
  if (!button_visible_ && (tbar_->all_outputs() || tbar_->show_output(output))) {
    tbar_->add_button(button);
    button.show();
    button_visible_ = true;
  }
}
```

Key detail:

- `all-outputs` only changes the condition inside `handle_output_enter`.
- It does not make Waybar add a task button when no `output_enter` event is
  ever received.

## River evidence

Installed River binary contains foreign-toplevel symbols:

```txt
wlr_foreign_toplevel_manager_v1_create
wlr_ext_foreign_toplevel_list_v1_create
wlr_foreign_toplevel_handle_v1_create
wlr_foreign_toplevel_handle_v1_set_title
wlr_foreign_toplevel_handle_v1_set_app_id
wlr_foreign_toplevel_handle_v1_set_activated
```

River source downloaded:

- Current mirror snapshot:
  - `/tmp/maindeck-river-full-survey/repos/river`
  - commit `65e1199`
- Installed-version source:
  - `/tmp/maindeck-river-full-survey/repos/river-v0.4.5`
  - tag `v0.4.5`
  - commit `f6d9617`

River source URLs:

- `https://github.com/riverwm/river`
- `https://isaacfreund.com/software/river/`
- `https://isaacfreund.com/docs/wayland/river-window-management-v1/`

River protocol doc saved locally:

- `/tmp/maindeck-river-full-survey/upstream-river/river-window-management-v1.html`

Relevant River `v0.4.5` source:

- `/tmp/maindeck-river-full-survey/repos/river-v0.4.5/river/Server.zig:175`
  creates the `wlr.ForeignToplevelManagerV1`.
- `/tmp/maindeck-river-full-survey/repos/river-v0.4.5/river/Window.zig:434`
  creates the `wlr.ForeignToplevelHandleV1`.
- `/tmp/maindeck-river-full-survey/repos/river-v0.4.5/river/Window.zig:437`
  sets title.
- `/tmp/maindeck-river-full-survey/repos/river-v0.4.5/river/Window.zig:438`
  sets app id.
- `/tmp/maindeck-river-full-survey/repos/river-v0.4.5/river/Window.zig:786`
  sets activated.
- `/tmp/maindeck-river-full-survey/repos/river-v0.4.5/river/Window.zig:1205`
  updates title.
- `/tmp/maindeck-river-full-survey/repos/river-v0.4.5/river/Window.zig:1220`
  updates app id.

Search result:

```txt
rg -n "ForeignToplevelHandleV1.*output|outputEnter|output_enter|outputLeave|output_leave" \
  /tmp/maindeck-river-full-survey/repos/river-v0.4.5/river

# no matches
```

Relevant wlroots API exists locally:

- `/usr/include/wlroots-0.20/wlr/types/wlr_foreign_toplevel_management_v1.h:129`
- `/usr/include/wlroots-0.20/wlr/types/wlr_foreign_toplevel_management_v1.h:131`

```c
void wlr_foreign_toplevel_handle_v1_output_enter(
    struct wlr_foreign_toplevel_handle_v1 *toplevel, struct wlr_output *output);
void wlr_foreign_toplevel_handle_v1_output_leave(
    struct wlr_foreign_toplevel_handle_v1 *toplevel, struct wlr_output *output);
```

Interpretation:

- River has the manager and handles.
- The source survey did not find calls to the output membership API used by
  Waybar to display buttons.
- This matches the smoke test exactly.

## maindeck-wm evidence

Relevant local WM code:

- `/home/tcfialho/Documents/poc/maindeck-wm/maindeck-wm.c:262`
- `/home/tcfialho/Documents/poc/maindeck-wm/maindeck-wm.c:580`
- `/home/tcfialho/Documents/poc/maindeck-wm/maindeck-wm.c:591`

Current behavior:

```c
static bool window_is_visible_index(size_t index) {
    if (wm.maximized) return index == wm.target_index;
    return index < 2;
}

static void window_manage_layout(struct Window *window, size_t index) {
    if (!window_is_visible_index(index)) return;
    ...
}

static void window_render_layout(struct Window *window, size_t index) {
    if (!window_is_visible_index(index)) {
        river_window_v1_hide(window->obj);
        ...
        return;
    }

    river_window_v1_show(window->obj);
    river_node_v1_set_position(window->node, ...);
    river_node_v1_place_top(window->node);
}
```

This matters for WM behavior, but the local smoke test shows no
`output_enter` even for currently visible windows. So the immediate taskbar
failure is not only "hidden deck windows"; it is that the direct River
foreign-toplevel stream has no output membership events at all.

## Proxy evidence

Relevant proxy code:

- `/home/tcfialho/Documents/poc/maindeck-wm/maindeck-proxy.c:1`
- `/home/tcfialho/Documents/poc/maindeck-wm/maindeck-proxy.c:392`
- `/home/tcfialho/Documents/poc/maindeck-wm/maindeck-proxy.c:407`
- `/home/tcfialho/Documents/poc/maindeck-wm/maindeck-proxy.c:414`

The file explicitly says it adds missing `output_enter` events, and the code
does so in `emit_output_enter_once()` / `flush_toplevel()`.

Conclusion:

- The proxy is not the right final architecture.
- It is useful only as a diagnostic proof that Waybar shows buttons once
  `output_enter` exists.

## Downloaded survey material

Primary survey root:

- `/tmp/maindeck-river-full-survey`

Secondary older survey root reused because some Codeberg clones failed:

- `/tmp/maindeck-river-wm-survey`

Official River WM list saved here:

- `/tmp/maindeck-river-full-survey/wm-list.md`

Official list URL:

- `https://codeberg.org/river/wiki/src/branch/main/pages/wm-list.md`

Note: `/tmp` may be cleared by reboot. If these folders disappear, use this
report's URLs and paths as a redownload map.

### Cloned or extracted repositories

| Project | Local path | Source status |
| --- | --- | --- |
| anvl | `/tmp/maindeck-river-wm-survey/anvl` | cloned earlier, commit `8eab9e7` |
| argen | none | skipped, Codeberg returned 504/timeouts |
| ashrwm | `/tmp/maindeck-river-full-survey/repos/ashrwm` | cloned, commit `29be633` |
| beansprout | `/tmp/maindeck-river-full-survey/repos/beansprout` | extracted from `/tmp/beansprout-main.tar.gz` |
| bridge | `/tmp/maindeck-river-full-survey/repos/bridge` | cloned, commit `4db5649` |
| Canoe | `/tmp/maindeck-river-full-survey/repos/canoe` | cloned, commit `4a5cb93` |
| CoW | `/tmp/maindeck-river-wm-survey/cow` | cloned earlier, commit `998a845` |
| crofflewm | `/tmp/maindeck-river-full-survey/repos/crofflewm` | cloned, commit `556fe55` |
| kwm | `/tmp/maindeck-river-full-survey/repos/kwm` | cloned, commit `497d7c4` |
| machi | `/tmp/maindeck-river-full-survey/repos/machi` | cloned, commit `5ea5b4c` |
| mousetrap | `/tmp/maindeck-river-full-survey/repos/mousetrap` | cloned, commit `e370b6b` |
| orilla | `/tmp/maindeck-river-full-survey/repos/orilla` | cloned, commit `bd77afb` |
| pwm | `/tmp/maindeck-river-full-survey/repos/pwm` | cloned, commit `4f0687f` |
| reka | `/tmp/maindeck-river-full-survey/repos/reka` | cloned, commit `90e89c2` |
| rhine | `/tmp/maindeck-river-full-survey/repos/rhine` | extracted from `/tmp/rhine-main.tar.gz` |
| rijan | `/tmp/maindeck-river-full-survey/repos/rijan` | cloned, commit `e4d390e` |
| rill | `/tmp/maindeck-river-full-survey/repos/rill` | cloned, commit `892f3af` |
| ropotamo | `/tmp/maindeck-river-full-survey/repos/ropotamo` | cloned, commit `14d6976` |
| rrwm | `/tmp/maindeck-river-full-survey/repos/rrwm` | cloned, commit `22c65be` |
| tarazed | `/tmp/maindeck-river-full-survey/repos/tarazed` | cloned, commit `0dc46fc` |
| zrwm | `/tmp/maindeck-river-full-survey/repos/zrwm` | cloned, commit `812bc9d` |
| river | `/tmp/maindeck-river-full-survey/repos/river` | cloned mirror, commit `65e1199` |
| river-v0.4.5 | `/tmp/maindeck-river-full-survey/repos/river-v0.4.5` | cloned tag, commit `f6d9617` |
| niri | `/tmp/maindeck-river-full-survey/repos/niri` | cloned for comparison, commit `4294948` |

## Survey findings by project

### anvl

- Path: `/tmp/maindeck-river-wm-survey/anvl`
- Language: C.
- Minimal River WM.
- No Waybar taskbar architecture found.
- Calls `river_window_v1_hide()` / `show()`:
  - `/tmp/maindeck-river-wm-survey/anvl/src/anvl.c:426`
  - `/tmp/maindeck-river-wm-survey/anvl/src/anvl.c:438`

### argen

- Skipped.
- Clone/tarball attempts failed with Codeberg 504/timeouts.

### ashrwm

- Path: `/tmp/maindeck-river-full-survey/repos/ashrwm`
- Language: Janet.
- Has hot reload / REPL-style control through `scripts/ashrwm-msg`.
- No `wlr/taskbar` integration found.
- Useful inspiration: runtime command/control without restarting the compositor.

### beansprout

- Path: `/tmp/maindeck-river-full-survey/repos/beansprout`
- Language: Zig.
- Built-in optional bar, not Waybar `wlr/taskbar`.
- Creates bar as a River shell surface:
  - `/tmp/maindeck-river-full-survey/repos/beansprout/src/Bar.zig:91`
  - `/tmp/maindeck-river-full-survey/repos/beansprout/src/Bar.zig:94`
- Reconfigures an existing bar in place:
  - `/tmp/maindeck-river-full-survey/repos/beansprout/src/Bar.zig:135`
  - `/tmp/maindeck-river-full-survey/repos/beansprout/src/Context.zig:191`
  - `/tmp/maindeck-river-full-survey/repos/beansprout/src/Context.zig:198`
- Good inspiration for the "do not restart everything for config changes"
  requirement.
- Still uses hide/show for windows:
  - `/tmp/maindeck-river-full-survey/repos/beansprout/src/Window.zig:422`
  - `/tmp/maindeck-river-full-survey/repos/beansprout/src/Window.zig:424`

### bridge

- Path: `/tmp/maindeck-river-full-survey/repos/bridge`
- Language: Zig.
- Has a built-in icon/status bar.
- Creates its bar as a River shell surface:
  - `/tmp/maindeck-river-full-survey/repos/bridge/src/Bar.zig:50`
  - `/tmp/maindeck-river-full-survey/repos/bridge/src/Bar.zig:54`
- No Waybar `wlr/taskbar` solution found.

### Canoe

- Path: `/tmp/maindeck-river-full-survey/repos/canoe`
- Language: Rust.
- Stacking WM with classic titlebars and minimized-window desktop icons.
- Not a Waybar `wlr/taskbar` architecture.

### CoW

- Path: `/tmp/maindeck-river-wm-survey/cow`
- Language: C.
- Uses Waybar, but not `wlr/taskbar`.
- Pattern: WM exports status, `cowbar` converts it into Waybar custom-module
  JSON.
- Waybar config:
  - `/tmp/maindeck-river-wm-survey/cow/config/waybar/config:6`
  - `/tmp/maindeck-river-wm-survey/cow/config/waybar/config:19`
  - `/tmp/maindeck-river-wm-survey/cow/config/waybar/config:25`
- `cowbar` Waybar mode:
  - `/tmp/maindeck-river-wm-survey/cow/cowbar/src/cowbar.c:28`
  - `/tmp/maindeck-river-wm-survey/cow/cowbar/src/cowbar.c:33`

### crofflewm

- Path: `/tmp/maindeck-river-full-survey/repos/crofflewm`
- Language: Go.
- Static tiling WM.
- No Waybar taskbar architecture found.

### kwm

- Path: `/tmp/maindeck-river-full-survey/repos/kwm`
- Language: Zig.
- Built-in dwm-like status bar.
- Creates bar shell surface:
  - `/tmp/maindeck-river-full-survey/repos/kwm/src/kwm/bar.zig:748`
  - `/tmp/maindeck-river-full-survey/repos/kwm/src/kwm/bar.zig:751`
- No Waybar `wlr/taskbar` architecture found.

### machi

- Path: `/tmp/maindeck-river-full-survey/repos/machi`
- Language: Zig.
- Has a custom state protocol / socket approach.
- Socket path builder:
  - `/tmp/maindeck-river-full-survey/repos/machi/common/socket.zig:17`
  - `/tmp/maindeck-river-full-survey/repos/machi/common/socket.zig:19`
- Main process creates state socket:
  - `/tmp/maindeck-river-full-survey/repos/machi/src/main.zig:66`
  - `/tmp/maindeck-river-full-survey/repos/machi/src/State.zig:31`
- Useful inspiration for WM-owned state broadcasting.

### mousetrap

- Path: `/tmp/maindeck-river-full-survey/repos/mousetrap`
- Language: C++.
- Minimal stumpwm/ratpoison-like WM.
- No taskbar architecture found.

### orilla

- Path: `/tmp/maindeck-river-full-survey/repos/orilla`
- Language: Rust.
- Has WM-owned JSON snapshot IPC including windows, app_id, title, and focus.
- Relevant files:
  - `/tmp/maindeck-river-full-survey/repos/orilla/crates/orilla/src/ipc.rs:33`
  - `/tmp/maindeck-river-full-survey/repos/orilla/crates/orilla/src/ipc.rs:40`
  - `/tmp/maindeck-river-full-survey/repos/orilla/crates/orilla/src/ipc.rs:61`
  - `/tmp/maindeck-river-full-survey/repos/orilla/crates/orilla/src/ipc.rs:66`
- Useful inspiration for WM-owned state snapshots.

### pwm

- Path: `/tmp/maindeck-river-full-survey/repos/pwm`
- Language: Python.
- Exposes i3/sway-compatible IPC to consumers such as Waybar.
- Sets `I3SOCK` and `SWAYSOCK`:
  - `/tmp/maindeck-river-full-survey/repos/pwm/pwm/riverwm.py:846`
  - `/tmp/maindeck-river-full-survey/repos/pwm/pwm/riverwm.py:848`
  - `/tmp/maindeck-river-full-survey/repos/pwm/pwm/riverwm.py:849`
- Example Waybar uses `sway/workspaces`, not `wlr/taskbar`:
  - `/tmp/maindeck-river-full-survey/repos/pwm/waybar-config.json:7`
  - `/tmp/maindeck-river-full-survey/repos/pwm/waybar-config.json:22`

### reka

- Path: `/tmp/maindeck-river-full-survey/repos/reka`
- Language: Elisp + Rust.
- Emacs-based WM for River.
- No Waybar `wlr/taskbar` architecture found in the quick survey.

### rhine

- Path: `/tmp/maindeck-river-full-survey/repos/rhine`
- Language: Zig.
- Explicitly supports parts of Hyprland IPC for external bars including
  Waybar.
- README:
  - `/tmp/maindeck-river-full-survey/repos/rhine/README.asciidoc:10`
  - `/tmp/maindeck-river-full-survey/repos/rhine/README.asciidoc:15`
  - `/tmp/maindeck-river-full-survey/repos/rhine/README.asciidoc:279`
- IPC socket/signature implementation:
  - `/tmp/maindeck-river-full-survey/repos/rhine/src/hyprIPC.zig:31`
  - `/tmp/maindeck-river-full-survey/repos/rhine/src/hyprIPC.zig:39`
  - `/tmp/maindeck-river-full-survey/repos/rhine/src/hyprIPC.zig:69`
  - `/tmp/maindeck-river-full-survey/repos/rhine/src/hyprIPC.zig:76`
- Not a `wlr/taskbar` solution, but relevant if choosing a compatibility API
  for external bars.

### rijan

- Path: `/tmp/maindeck-river-full-survey/repos/rijan`
- Language: Janet.
- Small dynamic WM.
- REPL/control style, no taskbar architecture found.

### rill

- Path: `/tmp/maindeck-river-full-survey/repos/rill`
- Language: Zig.
- Live-reload config / animations.
- No Waybar taskbar architecture found.

### ropotamo

- Path: `/tmp/maindeck-river-full-survey/repos/ropotamo`
- Language: Janet.
- REPL/control oriented.
- No Waybar taskbar architecture found.

### rrwm

- Path: `/tmp/maindeck-river-full-survey/repos/rrwm`
- Language: Rust.
- Uses Waybar custom module fed by WM socket.
- Waybar example:
  - `/tmp/maindeck-river-full-survey/repos/rrwm/example/waybar_example_config.jsonc:3`
  - `/tmp/maindeck-river-full-survey/repos/rrwm/example/waybar_example_config.jsonc:7`
- Socket setup:
  - `/tmp/maindeck-river-full-survey/repos/rrwm/src/main.rs:51`
  - `/tmp/maindeck-river-full-survey/repos/rrwm/src/main.rs:60`
- Waybar client mode:
  - `/tmp/maindeck-river-full-survey/repos/rrwm/src/main.rs:233`
  - `/tmp/maindeck-river-full-survey/repos/rrwm/src/main.rs:242`
- Not a `wlr/taskbar` solution.

### tarazed

- Path: `/tmp/maindeck-river-full-survey/repos/tarazed`
- Language: C.
- Good River layer-shell handling.
- No Waybar `wlr/taskbar` architecture found.

### zrwm

- Path: `/tmp/maindeck-river-full-survey/repos/zrwm`
- Language: C.
- Important inspiration for no-restart runtime control.
- Opens Unix command socket:
  - `/tmp/maindeck-river-full-survey/repos/zrwm/zrwm.c:3595`
  - `/tmp/maindeck-river-full-survey/repos/zrwm/zrwm.c:3605`
  - `/tmp/maindeck-river-full-survey/repos/zrwm/zrwm.c:3621`
  - `/tmp/maindeck-river-full-survey/repos/zrwm/zrwm.c:3627`
- `zrwm-msg` sends argv over the socket:
  - `/tmp/maindeck-river-full-survey/repos/zrwm/zrwm-msg.c:20`
  - `/tmp/maindeck-river-full-survey/repos/zrwm/zrwm-msg.c:27`
  - `/tmp/maindeck-river-full-survey/repos/zrwm/zrwm-msg.c:34`
  - `/tmp/maindeck-river-full-survey/repos/zrwm/zrwm-msg.c:42`
- No Waybar `wlr/taskbar` solution found.

## What the other WMs suggest

The survey did not find another River WM using Waybar `wlr/taskbar` as its
core taskbar solution.

The patterns found were:

1. Built-in bar using River shell surfaces:
   - `beansprout`
   - `bridge`
   - `kwm`

2. WM-owned IPC/status feeding external bars:
   - `CoW`
   - `rrwm`
   - `orilla`
   - `machi`

3. Compatibility IPC for existing external bar ecosystems:
   - `pwm` exposes i3/sway IPC.
   - `rhine` exposes partial Hyprland IPC.

4. CLI/REPL reload/control:
   - `zrwm`
   - `ashrwm`
   - `rijan`
   - `ropotamo`

For our goal of "adjust without restarting maindeck-wm/Waybar/proxy and
crashing windows", the strongest inspirations are:

- `zrwm`: small C Unix-socket command API.
- `beansprout`: in-place reconfiguration of bar surfaces.
- `orilla`/`machi`: WM-owned state snapshots/broadcasts.
- `CoW`/`rrwm`: Waybar custom module fed by WM state, though this does not
  provide Waybar's built-in per-window task buttons.

## Niri comparison

Niri was downloaded for comparison after the River evidence pointed at missing
foreign-toplevel output membership.

Path:

- `/tmp/maindeck-river-full-survey/repos/niri`

Commit:

- `4294948`

Relevant source:

- `/tmp/maindeck-river-full-survey/repos/niri/src/protocols/foreign_toplevel.rs:48`
  stores an `output: Option<Output>` in the toplevel data.
- `/tmp/maindeck-river-full-survey/repos/niri/src/protocols/foreign_toplevel.rs:186`
  sends `output_enter()` when an output is bound.
- `/tmp/maindeck-river-full-survey/repos/niri/src/protocols/foreign_toplevel.rs:268`
  handles output changes by sending leave/enter.
- `/tmp/maindeck-river-full-survey/repos/niri/src/protocols/foreign_toplevel.rs:371`
  sends initial `output_enter()` events when a Waybar-style WLR toplevel handle
  is created.
- `/tmp/maindeck-river-full-survey/repos/niri/src/layout/mod.rs:3972`
  updates a window's output from layout code.

Why this matters:

- Niri is a compositor/WM in one codebase, so the foreign-toplevel protocol
  implementation has direct access to the output chosen by its layout.
- River deliberately splits compositor and WM policy. In the River `v0.4.5`
  source checked here, foreign-toplevel handles are created but output
  membership is not propagated to the WLR foreign-toplevel manager.
- This is why Niri can make Waybar `wlr/taskbar` work without a proxy while
  the current River path does not.

## Architecture options

There are two clean choices. Mixing them through a proxy is the wrong middle
ground.

### Option A: keep Waybar `wlr/taskbar`

Use the standard Waybar module exactly as configured.

Required fix:

- River must emit `wlr_foreign_toplevel_handle_v1_output_enter()` and
  `wlr_foreign_toplevel_handle_v1_output_leave()` for each visible toplevel.

Consequences:

- This is not fully inside `maindeck-wm`.
- It requires patching/running River or using a River version that implements
  this behavior.
- It keeps Waybar's real per-window buttons and click actions.
- The proxy can be deleted from the final session.

Minimum validation:

```sh
WAYLAND_DISPLAY=wayland-1 ./build/maindeck-toplevel-smoke
# success requires: with_output > 0
```

### Option B: keep the solution inside `maindeck-wm`

Do not use Waybar's `wlr/taskbar` as the taskbar source.

Possible designs:

- Add a `maindeckctl` Unix socket, inspired by `zrwm-msg`, for runtime
  commands and config reload.
- Add a WM-owned status/task snapshot stream, inspired by `orilla`, `machi`,
  `CoW`, and `rrwm`.
- If we still want Waybar, use Waybar `custom/*` modules for status summaries.
  This is good for tags/title/state, but it is not a full replacement for
  Waybar's built-in per-window clickable task buttons.
- For true Windows-like clickable task buttons fully inside the WM, build a
  `river_shell_surface_v1` taskbar inside `maindeck-wm` or a companion
  `maindeck-bar` process owned by the same project.

Consequences:

- Cleanly inside WM/project scope.
- No River patch required.
- No proxy.
- If implemented as a real shell-surface taskbar, it replaces only the taskbar
  behavior, not necessarily all Waybar system modules. But integrating it
  visually with the current bottom Waybar needs design work.

## Recommended next steps

1. Stop treating the proxy as architecture.
   - Keep it only as diagnostic proof if needed.
   - Do not route Waybar through it in the final setup.

2. Choose one clean path:
   - If exact Waybar `wlr/taskbar` is mandatory, patch/test River output
     membership.
   - If WM containment is mandatory, build WM-owned IPC/status/taskbar and do
     not depend on `wlr/taskbar`.

3. Add a no-restart control plane to `maindeck-wm`.
   - Start with a Unix socket like `zrwm`.
   - Commands should include reload config, dump state, switch target, move
     window, close window, and maybe toggle diagnostics.
   - This lets us adjust behavior without restarting River, Waybar, or all
     clients.

4. Test in a nested River session first.
   - Avoid live-session restarts.
   - Success criteria:
     - no client windows crash,
     - `maindeck-toplevel-smoke` result is understood,
     - taskbar behavior matches chosen architecture.

## Open questions for the next agent

1. Which invariant wins?
   - "Use Waybar's built-in `wlr/taskbar`" means patch River.
   - "Stay fully in our WM scope" means replace `wlr/taskbar` with WM-owned
     taskbar/status.

2. If patching River, should the patch be local-only or maintained as a fork?

3. If building WM-owned taskbar, should it be:
   - built directly into `maindeck-wm`,
   - a separate `maindeck-bar` using `river_shell_surface_v1`,
   - or a Waybar custom status integration with reduced click behavior?

4. The current `maindeck-wm` hide/show policy still needs design review for
   deck windows, but it is not the immediate cause of the blank Waybar
   `wlr/taskbar` on direct River because visible windows also lack
   `output_enter`.

## External references

- River WM list:
  - `https://codeberg.org/river/wiki/src/branch/main/pages/wm-list.md`
- River overview:
  - `https://isaacfreund.com/software/river/`
- River window management protocol:
  - `https://isaacfreund.com/docs/wayland/river-window-management-v1/`
- River source mirror:
  - `https://github.com/riverwm/river`
- Waybar `wlr/taskbar` manual:
  - `https://man.archlinux.org/man/extra/waybar/waybar-wlr-taskbar.5.en`
- Waybar `0.15.0` taskbar source:
  - `https://raw.githubusercontent.com/Alexays/Waybar/0.15.0/src/modules/wlr/taskbar.cpp`
- Niri source:
  - `https://github.com/YaLTeR/niri`
- Historical wlroots issue about missing initial `output_enter`:
  - `https://github.com/swaywm/wlroots/issues/1567`
