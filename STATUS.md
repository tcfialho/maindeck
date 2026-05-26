# MainDeck WM status

## Implementado

- Base C derivada do `tinyrwm`.
- Bind em `river_window_manager_v1` e `river_xkb_bindings_v1`.
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
- `Super+1..9` com slots configuraveis por ambiente.

## Atalhos atuais

Tap/hold real ainda nao foi implementado. Por enquanto:

- `Super+Tab`: alterna ALVO.
- `Super+Shift+Tab`: swap MAIN/DECK.
- `Super+Right`: proxima carta do DECK.
- `Super+Left`: carta anterior do DECK.
- `Super+Shift+Right`: manda ALVO para o fundo do DECK.
- `Super+Shift+Left`: promove ALVO para MAIN.
- `Super+Up`: maximiza ALVO.
- `Super+Down`: restaura.
- `Super+Delete`: fecha ALVO.

## Falta

- Tap/hold real com timer de 360ms.
- OSD visual como shell surface.
- IPC/comando para Waybar ativar janelas/apps pelo item 6 da spec.
- Usar area util do River/layer-shell para nao cobrir Waybar.
- Config externa para proporcoes, comandos e slots.
- Teste manual em sessao River real.
