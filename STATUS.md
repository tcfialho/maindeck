# MainDeck WM status

## Implementado

- Base C derivada do `tinyrwm`.
- Bind em `river_window_manager_v1`, `river_xkb_bindings_v1`,
  `river_layer_shell_v1` e cursor-shape.
- Ordem interna MainDeck:
  - `windows[0]` = MAIN
  - `windows[1]` = DECK visivel
  - `windows[2...]` = cartas ocultas do DECK
- Entrada de novas janelas preservando o DECK visivel.
- Layout 2/3 MAIN + 1/3 DECK.
- `hide/show` para cartas ocultas do DECK.
- ALVO sempre limitado a MAIN ou DECK visivel.
- Bordas: MAIN azul, DECK roxo, ALVO amarelo.
- Maximizar/restaurar ALVO.
- Fechar ALVO.
- Navegar cartas do DECK.
- Promover ALVO para MAIN.
- Mandar ALVO para o fundo do DECK.
- Tap/hold real com timer de 360 ms.
- `Super+Tab` com double tap para avancar o DECK.
- Barra nativa `maindeck-bar` com quick-launch, taskbar, status e tray.
- IPC `maindeck-bar` -> `maindeck-wm` para ativar janelas pela taskbar.
- Area util via River/layer-shell para respeitar zonas exclusivas.
- Modo jogo/fullscreen: o WM notifica a barra, a barra suprime render e destroi
  a layer-surface principal enquanto o fullscreen esta ativo.
- Fallback de cursor via cursor-shape no WM, barra e menu.
- Config externa da barra em `~/.config/maindeck/bar.json`.

## Atalhos atuais

- `Super+Tab` tap: alterna ALVO.
- `Super+Tab` hold: swap MAIN/DECK.
- `Super+Tab` double tap: proxima carta do DECK.
- `Super+Right` tap: proxima carta do DECK.
- `Super+Right` hold: manda ALVO para o fundo do DECK.
- `Super+Left` tap: carta anterior do DECK.
- `Super+Left` hold: promove ALVO para MAIN.
- `Super+Up`: maximiza ALVO.
- `Super+Down`: restaura.
- `Super+Delete`, `Super+F4` e `Alt+F4`: fecha ALVO.
- `Super+Return`: abre `kitty`.
- `Super+Shift+Escape`: sai da sessao River.

## Falta

- OSD visual como shell surface.
- Pareamento robusto entre `zwlr` e `ext_foreign_toplevel` na taskbar; plano em
  `otimizacao-jogos-task/future-matching-plan.md`.
- Config externa do WM para proporcoes, comandos e politicas.
- Investigar direct scanout/presentation async no caminho River/wlroots.
- Planos futuros fora do Wayland atual:
  - `docs/plano-maindeck-x11.md`
  - `docs/plano-maindeck-windows.md`
