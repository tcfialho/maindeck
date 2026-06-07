# Plano MainDeck para X11

Este plano descreve a via X11 do MainDeck. O objetivo e ter uma stack
pragmatica para jogos Proton/X11 sem tentar portar todo o modelo Wayland atual
de uma vez.

## Objetivo

Construir uma versao X11 composta por:

- `maindeck-bar-x11` ou modo X11 do `maindeck-bar`;
- `maindeck-menu-x11` ou modo X11 do `maindeck-menu`;
- uma politica de janelas MainDeck sobre X11;
- compatibilidade conservadora com Steam, Proton, launchers e dialogs.

Ha duas rotas viaveis:

1. MainDeck em cima de Openbox, com um organizador X11 pequeno.
2. Um WM X11 proprio/forkado, provavelmente usando `dwm` como base inicial.

## Rota A: Openbox minimo

Esta e a rota recomendada para baseline rapido.

Stack:

- `Xorg`;
- `openbox`;
- `maindeck-bar` em modo X11/painel proprio;
- `maindeck-menu`;
- `maindeck-x11-organizer`, se for necessario impor layout MainDeck por cima do
  Openbox.

Vantagens:

- EWMH/ICCCM, foco, fullscreen e dialogs ja ficam sob responsabilidade de um WM
  maduro.
- Menor risco para jogos Proton e launchers.
- Facil comparar FPS/CPU contra River/Wayland.
- Boa rota para validar se X11 realmente entrega ganho pratico antes de criar
  um WM proprio.

Limites:

- A politica de janelas MainDeck fica limitada pelo que o Openbox permite ou
  pelo que o organizador consegue ajustar depois.
- Pode haver conflito entre regras do Openbox e layout MainDeck se a
  organizacao ficar agressiva.

## Rota B: WM X11 proprio

Esta rota so deve vir depois do baseline Openbox.

Base provavel:

- fork de `dwm`, ou outro WM X11 pequeno;
- suporte EWMH/ICCCM suficiente para Steam/Proton;
- fullscreen robusto;
- transients/dialogs;
- focus model previsivel;
- IPC para a barra;
- lista de janelas para taskbar;
- regras por app somente quando forem inevitaveis.

Vantagens:

- Controle total sobre a politica MainDeck.
- Menos camadas se comparado a Openbox + organizador.

Riscos:

- Alto custo em EWMH/ICCCM e casos de borda.
- Jogos/launchers tendem a exercitar caminhos estranhos de janela.
- Mais manutencao do que a rota Openbox.

## Barra e Menu em X11

A barra pode seguir dois caminhos:

- janela X11 normal/topmost com `_NET_WM_STRUT_PARTIAL` para reservar area;
- painel sem reserva durante modo jogo, escondendo/desmapeando no fullscreen.

Requisitos:

- quick-launch;
- taskbar;
- status basico;
- power menu;
- esconder/desmapear no modo jogo;
- nao usar compositor X11 se o objetivo for medir latencia/FPS puro.

O menu deve fechar ao perder foco e deve evitar superficies/capture windows que
interfiram com fullscreen.

## Modo Jogo

Em X11, o objetivo e sair do caminho:

- detectar `_NET_WM_STATE_FULLSCREEN`;
- esconder/desmapear a barra;
- nao usar compositor;
- evitar overlays topmost;
- deixar o jogo controlar a janela fullscreen/borderless;
- medir Steam, jogo, Xorg, WM e barra separadamente.

## Caminho Recomendado

1. Rodar baseline `Xorg + openbox` sem compositor, com Sunshine em NvFBC.
2. Portar/ativar `maindeck-bar` e `maindeck-menu` no X11 sem organizador.
3. Medir jogos Proton contra River/MainDeck atual.
4. Criar `maindeck-x11-organizer` pequeno apenas se o layout for necessario.
5. So considerar fork de `dwm` se o Openbox provar ganho real mas limitar demais
   a experiencia MainDeck.

## Esforco Estimado

| Entrega | Esforco aproximado com IA | Observacao |
|---|---:|---|
| Baseline Openbox configurado | `10k-30k` | Principalmente scripts/config. |
| Barra/menu X11 basicos | `60k-160k` | Depende do reaproveitamento do codigo atual. |
| Organizer X11 sobre Openbox | `80k-200k` | Precisa filtrar janelas com cuidado. |
| WM X11 proprio baseado em dwm | `250k-700k` | EWMH/ICCCM e jogos sao o custo maior. |

## Decisao Atual

Nao criar um WM X11 completo agora. Primeiro medir Openbox minimo com
MainDeck-bar/menu e Sunshine/NvFBC. Se a diferenca de FPS/CPU justificar, evoluir
para organizador X11; so depois considerar WM proprio.
