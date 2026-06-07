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

Machine-local River, native MainDeck bar/menu, Sunshine, and notification
configuration used on the notebook is documented in
`docs/local-session-config.md`. Those files live outside this repository and
must be managed separately from the source tree.

Future platform plans:

- `docs/plano-maindeck-x11.md`
- `docs/plano-maindeck-windows.md`

## Current bindings

Tap/hold is implemented with a 360 ms threshold. The common tap path is kept
immediate; hold actions fire only after the threshold.

```txt
Super+Return          open kitty
Super+Tab             tap: alternate ALVO MAIN <-> DECK
Super+Tab             hold: swap MAIN <-> visible DECK
Super+Tab             double tap: next DECK card
Super+Right           tap: next DECK card
Super+Right           hold: send ALVO to bottom of DECK
Super+Left            tap: previous DECK card
Super+Left            hold: promote ALVO to MAIN
Super+Up              maximize ALVO
Super+Down            restore
Super+Delete/F4       close ALVO
Alt+F4                close ALVO
Super+Shift+Escape    exit river session
```

Quick-launch buttons are configured by `~/.config/maindeck/bar.json`.
