# maindeck-wm

Prototype MainDeck window manager for river.

This starts from the C tinyrwm example and implements the behavior described in
`../poc-spec.md`:

- `MAIN` is the first window in the internal order.
- `DECK` is every other window.
- `windows[1]` is the visible DECK card.
- `windows[2...]` are hidden DECK cards.
- `ALVO` is always `MAIN` or the visible DECK card.

## Build

```sh
meson setup build
ninja -C build
```

## Run in a test river session

```sh
river -c ./build/maindeck-wm
```

## Visual feedback

- **Borders:** only the focused window (`ALVO`) draws a border (yellow).
  Unfocused windows (MAIN/DECK) have a transparent border — same width, alpha 0
  — so the layout never shifts when focus changes.
- **OSD:** intentionally sparse. The WM notifies only for the two navigation
  no-ops (`sem janela invisível à direita/esquerda`), where nothing moves on
  screen. Every command with a visible effect (swap, deck cycle, promote,
  send-to-deck, new window, close, maximize, restore) is silent. The OSD uses
  the mako `x-canonical-private-synchronous` hint, so a new message replaces the
  previous one instead of stacking.

## Local deployment notes

Machine-local River, Waybar, Sunshine, and notification (mako) configuration
used on the notebook is documented in `docs/local-session-config.md`. Those
files live outside this repository and must be managed separately from the
source tree. In particular the OSD's "replace, don't stack" behavior and its
auto-expiry rely on mako: see the **Notifications (mako)** section there for
`~/.config/mako/config` (the 10-second ceiling) — without it, notifications
never auto-expire.

The River/Waybar taskbar investigation and architecture handoff is documented
in `docs/river-waybar-taskbar-research.md`.

## Current bindings

The spec asks for tap/hold. This first implementation uses `Super+Shift` as the
temporary hold equivalent while the timer-based tap/hold layer is still pending.

```txt
Super+Return          open foot
Super+Tab             alternate ALVO MAIN <-> DECK
Super+Shift+Tab       swap MAIN <-> visible DECK
Super+Right           next DECK card
Super+Left            previous DECK card
Super+Shift+Right     send ALVO to bottom of DECK
Super+Shift+Left      promote ALVO to MAIN
Super+Up              maximize ALVO
Super+Down            restore
Super+Delete          close ALVO
Super+1..9            activate configured app slot
Super+Shift+Escape    exit river session
```

App slots can be overridden with environment variables:

```txt
MAINDECK_APP_1=code
MAINDECK_CMD_1=code
```

The default app ids/commands are `code`, `chromium`, `foot`, `thunar`,
`spotify`, `discord`, `firefox`, `org.wezfurlong.wezterm`/`wezterm`, and
`Alacritty`/`alacritty`.
