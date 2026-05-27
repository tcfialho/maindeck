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

## Local deployment notes

Machine-local River, Waybar, and Sunshine configuration used on the notebook is
documented in `docs/local-session-config.md`. Those files live outside this
repository and must be managed separately from the source tree.

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
