# Plano Futuro: Correção da Race Condition de Pareamento da Barra de Tarefas

Este plano detalha a estratégia técnica consensualizada com o Claude Opus para corrigir os desalinhamentos de foco na barra de tarefas (`maindeck-bar`), causados por concorrência assíncrona entre os protocolos `zwlr` e `ext` na inicialização rápida de janelas.

## O Problema Atual
Em `bar-taskbar.c`, a barra mapeia e vincula as janelas criadas via `zwlr_foreign_toplevel_handle_v1` (ações de foco) e `ext_foreign_toplevel_handle_v1` (identificador IPC único) confiando na ordem exata de chegada no event loop (lockstep por índices). 

Ao inicializar jogos com launchers/splash screens rápidos, a criação e destruição célere de janelas faz com que as duas APIs assíncronas dessincronizem seus índices. Isso causa cruzamento de identificadores (cliques na barra focam a janela errada) e warnings de `taskbar activate: identifier desconhecido` (no-op).

## Estratégia Proposta (Snapshot no primeiro `done` com filas de pendentes)

### 1. Estruturas de Dados (`bar-state.h`)
- Substituir o pareamento cego por índice por:
  - Duas filas dinâmicas de handles pendentes (`zwlr_pending_list` e `ext_pending_list`).
  - Uma estrutura de par de janelas resolvido contendo:
    - Ponteiro do handle `zwlr` (para emitir comandos).
    - String do `identifier` `ext` (para comunicação via IPC).
    - `app_id` (imutável).
    - `title_snapshot` (título capturado na criação).

### 2. Fluxo de Pareamento (`bar-taskbar.c`)
- **Registro:** Não tentar fazer o pareamento no evento de criação (`new_toplevel`). Em vez disso, apenas inserir os respectivos handles nas suas filas de pendentes correspondentes.
- **Trigger de Sincronia:** O pareamento é engatilhado no **primeiro evento `done`** de cada handle (momento em que o compositor garante que as propriedades iniciais de `title` e `app_id` foram entregues e estão estáveis).
- **Rede de Segurança:** Caso ocorra o evento `done` mas o `app_id` ou `identifier` de string ainda esteja vazio, **adiar** o pareamento até que estes campos mínimos sejam preenchidos por eventos subsequentes.
- **Match de Snapshot:** A chave de correlação é o tuplo `(app_id, title_inicial)`. Alterações posteriores de títulos (por exemplo, jogos mudando de título para exibir FPS a cada frame) **não** acionam novas tentativas de pareamento.
- **Resolução de Conflitos (FIFO):** Caso duas janelas tenham o mesmo snapshot de `app_id` + `title`, o pareamento será resolvido por FIFO (First-In, First-Out) no grupo de handles idênticos.

### 3. Remoção e Limpeza
- O evento `closed` em qualquer um dos dois protocolos remove o handle correspondente de suas respectivas filas ou estruturas de par resolvido e destrói os objetos de forma limpa.

### 4. Ativação via IPC
- Ao executar a ação `activate`, buscar o identificador resolvido no lookup. Caso esteja ausente (par não resolvido em tempo hábil), tentar re-correlacionar as filas pendentes sob demanda antes de emitir o aviso de warning.
