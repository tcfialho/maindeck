# Cursor disappearing after fullscreen game exits

Current checkpoint before code changes: `786c51c test: make implicit parent scenarios generic`.

This file is a handoff plan. It intentionally contains no implementation.

## User-visible problem

- After launching a fullscreen game, especially one that styles or hides the mouse cursor, closing the game can leave the cursor invisible.
- A separate symptom was seen after opening a terminal: the cursor shape stayed as an I-beam over empty bar space until another normal app was opened.
- The most disruptive bug is the fullscreen game case: game takes the whole screen, changes cursor style/visibility, exits, and the cursor does not come back.

## Important context from the user

- Do not add Steam-specific or game-specific rules.
- Do not add polling, timers, per-frame cursor work, debug instrumentation, or anything that hurts gaming CPU/frame-time/latency.
- `bg_surface_create()` existed to support old fuzzel behavior:
  - closing fuzzel when clicking empty desktop with no normal window;
  - preserving close-on-bar-click behavior.
- The launcher has since moved toward Maindeck-owned menu/bar behavior, so the current need for `maindeck-bg` must be verified before changing it.
- Preserve expected behavior:
  - clicking the bar should close/toggle the menu as designed;
  - clicking empty desktop should close the launcher/menu if that is still part of the intended UX.

## Findings so far

- No code in the repo currently calls `wl_pointer_set_cursor`.
- No code in the repo currently binds or uses `wp_cursor_shape_manager_v1`.
- `maindeck-bar` accepts pointer input on its layer surface and tray popup but does not set a cursor shape on pointer enter.
- `maindeck-menu` accepts pointer input on its menu surface but does not set a cursor shape on pointer enter.
- `bar-surface.c` creates a persistent fullscreen transparent layer surface named `maindeck-bg` with a fullscreen input region.
- `bar-input.c` only distinguishes the tray popup from "everything else"; a pointer enter on `bg_surface` currently falls into the same branch as the real bar surface.
- `maindeck-menu.c` has an old capture-surface implementation behind `#if 0`; the current menu does not actively create that capture surface.
- `wm-input.c` receives `river_seat_v1.wl_seat` but currently ignores it. That means the WM does not use the compositor-provided seat mapping to set a WM-owned fallback cursor.
- River protocol says that, since river seat version 4, the cursor surface/shape set by the window manager on the corresponding `wl_pointer` is remembered and used when no client has pointer focus.

## Working hypothesis

There are two related but distinct problems:

1. Client-surface cursor leakage:
   - Terminal/game/Steam can leave the compositor's visible cursor as I-beam, hidden, or custom.
   - When the pointer enters Maindeck-owned surfaces, those surfaces accept pointer focus but do not set their own cursor.
   - Result: wrong cursor can be visible over Maindeck UI.

2. No-client-focus fallback after fullscreen game exit:
   - A fullscreen game may hide/style the cursor while it has pointer focus.
   - When the game exits, there may be a moment or state where no normal client owns pointer focus.
   - The WM currently does not explicitly install a default WM cursor for that seat.
   - Result: the compositor can appear to keep the hidden/custom cursor until another normal client sets a cursor.

The fullscreen game case should be addressed primarily through a generic WM/seat fallback cursor, not through app-specific matching.

## Constraints

- Event-driven only.
- Set cursor only on pointer enter, seat initialization, or cursor mode transition.
- No cursor work in pointer motion hot paths unless the cursor type actually changes.
- No logging/instrumentation in production code paths.
- Avoid removing `maindeck-bg` until its current UX role is verified.
- If `maindeck-bg` remains, it must be handled explicitly and must not be mistaken for the real bar surface.

## Implementation plan

1. Add protocol support:
   - Add `/usr/share/wayland-protocols/staging/cursor-shape/cursor-shape-v1.xml` to Meson protocol generation for components that need it.
   - Prefer `wp_cursor_shape_manager_v1` and `wp_cursor_shape_device_v1`.
   - Fallback can be considered later with xcursor surface only if cursor-shape is unavailable.

2. Fix Maindeck UI cursor ownership:
   - In `maindeck-bar`, bind cursor-shape manager if advertised.
   - When the bar gets a `wl_pointer`, create a cursor-shape device for it.
   - On `ptr_enter`:
     - if `surf == bar->wl_surface`, set `default` cursor once for that enter serial;
     - if `surf == bar->menu_surface`, set `default` cursor once for that enter serial;
     - if `surf == bar->bg_surface`, set `default` cursor if the surface remains active, but do not run normal bar hover/hit-test logic;
     - otherwise ignore the surface.
   - Store last cursor shape/surface state to avoid redundant protocol requests.

3. Fix bar surface routing:
   - Change `bar-input.c` so it explicitly branches on:
     - `bar->wl_surface`;
     - `bar->menu_surface`;
     - `bar->bg_surface`;
     - unknown.
   - Only the real bar surface should update `ptr_inside`, hover state, hit-test, and redraw.
   - `bg_surface` should not cause hover state or redraw.

4. Add WM-level fallback for fullscreen/game exit:
   - Use `river_seat_v1.wl_seat` to map River seat to the matching advertised `wl_seat`.
   - Acquire a `wl_pointer` for that seat when possible.
   - Create a `wp_cursor_shape_device_v1` for the WM pointer.
   - Set `default` cursor for the WM seat during seat initialization or after receiving the corresponding `wl_seat`.
   - Also call `river_seat_v1_set_xcursor_theme()` with `XCURSOR_THEME` and `XCURSOR_SIZE` defaults when supported, so compositor-rendered cursors have a sane theme.
   - This should be generic and apply to any fullscreen game/client.

5. Preserve menu/fuzzel behavior:
   - Before removing or changing `bg_surface_create()`, test:
     - current Maindeck menu closes/toggles when clicking the bar;
     - current menu behavior when clicking empty desktop with zero windows;
     - whether any old fuzzel path still depends on `maindeck-bg`.
   - If `maindeck-bg` is no longer needed, remove it in a separate commit after cursor fix is validated.
   - If still needed, keep it but route it explicitly and set cursor on enter.

6. Tests to add or improve:
   - Static/protocol test: verify cursor-shape protocol is generated and linked for bar/menu/WM where used.
   - Unit-ish C/smoke test where feasible: verify pointer-enter routing treats `wl_surface`, `menu_surface`, and `bg_surface` distinctly.
   - Regression script/manual checklist:
     - open terminal, place I-beam cursor, move over bar: cursor should return to default immediately;
     - launch fullscreen game that hides/styles cursor, close it: cursor should be visible without opening another app;
     - click bar while menu is open: menu behavior preserved;
     - click empty desktop with no windows: intended launcher/menu behavior preserved or explicitly documented if changed.
   - Re-run:
     - `tools/test-transient-behavior.sh`
     - `tools/perf-wm.py -n 100 --no-build`
     - build/deploy script after implementation.

7. Performance validation:
   - Confirm no new timers/poll loops.
   - Confirm cursor set requests happen only on rare events:
     - seat setup;
     - pointer enter;
     - actual cursor mode transition.
   - Compare idle CPU and memory before/after.
   - Watch for redraw count increase over bar/background; `bg_surface` must not trigger bar redraws.

## Suggested commit sequence

1. `docs: add cursor disappearance handoff plan`
2. `fix(bar): route pointer input by surface`
3. `fix(bar): restore default cursor on maindeck surfaces`
4. `fix(wm): install default seat cursor fallback`
5. Optional after validation: `fix(bar): remove obsolete fullscreen bg input surface` or keep it with explicit routing.

## Do not do

- Do not add `if steam`, `if game`, title matching, app-id matching, or Steam-only behavior.
- Do not fix by repeatedly setting cursor on every motion event.
- Do not add CPU-heavy instrumentation to production code.
- Do not remove `bg_surface_create()` before verifying whether it still supports an intended empty-desktop click behavior.
