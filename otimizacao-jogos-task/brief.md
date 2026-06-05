# Brief: Otimização do Maindeck e Correção de Foco para Jogos

## Objetivo
Analisar as oportunidades de otimização de CPU/GPU e I/O no `maindeck-wm` e `maindeck-bar`, além de solucionar a quebra de sincronia de foco da barra de tarefas (race conditions assíncronas) ao inicializar jogos.

## Contexto da base de código
- **Stack:** Linguagem C, protocolo Wayland (compositor River), Cairo para renderização, Pango para texto, IPC Unix Domain Sockets.
- **Arquivos relevantes:**
  - `wm-handlers.c` (callbacks do compositor, assinaturas de layout)
  - `wm-layout.c` (geometria, estados de tela cheia, `log_state`)
  - `bar-main.c` (event loop da barra de tarefas)
  - `bar-taskbar.c` (recebimento de toplevels via APIs `zwlr` e `ext` de forma assíncrona)
  - `bar-render.c` (desenho Cairo/Pango)
  - `bar-state.h` (estruturas de dados da barra)

## Contratos e descobertas (resultado da análise de logs e código)
1. **Redesenhos Fantasmas da Barra:** A barra intercepta atualizações de janelas de jogos (como mudança de título frequente para FPS/loadings) e dispara `bar_render()` redesenhando via Cairo, mesmo estando invisível abaixo do jogo em tela cheia.
2. **Race Condition de Pareamento:** Em `bar-taskbar.c`, os handles de `zwlr_foreign_toplevel_handle_v1` (usado para gerenciar ações) e `ext_foreign_toplevel_handle_v1` (usado para obter o identificador do IPC) são pareados confiando estritamente em sua ordem indexada de chegada no event loop. Quando launchers ou splash screens rápidos são criados e destruídos (comum em jogos), as APIs assíncronas dessincronizam seus índices, vinculando IDs errados às janelas na barra. Isso resulta em warnings de `taskbar activate: identifier desconhecido` e falha de foco silenciosa.
3. **Chamadas Redundantes de Wayland:** O WM envia comandos repetidos de `place_top` no protocolo Wayland para janelas fullscreen e alvos a cada frame, mesmo sem mudanças reais de layout.
4. **Sobrecarga de Logs Verbosos:** A sessão está rodando com `MAINDECK_LOG=debug` ativado, causando escritas físicas de I/O em disco com buffer de linha a cada fração de segundo.

## Restrições e invariantes
- A barra de tarefas deve voltar a renderizar imediatamente assim que o jogo em fullscreen for fechado, minimizado ou perder o foco para outra janela normal.
- Manter compatibilidade estrita com os protocolos River e as especificações wlroots.

## Aberto / incerto (Peça ao Opus para decidir)
- Qual a melhor estratégia técnica para parear de forma robusta e livre de race conditions as toplevels de `zwlr` (que tem ações) e `ext` (que tem o ID único) no `maindeck-bar` sem usar índices do array? Devemos correlacioná-los via `app_id` + `title` ou usar outra abordagem?
