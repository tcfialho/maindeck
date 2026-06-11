# River — Plano de Otimizações (CPU, latência, IO, gaming)

Gerado em 2026-06-10 por análise estática do código. **Nada foi aplicado.**

- Repo: `/home/tcfialho/Documents/poc/references/river`
- Base: upstream `riverwm/river` main em `76b9f61` + 1 commit local `e24e2bc` ("Log direct scanout transitions") + diff não commitado em `river/Output.zig` e `river/TextInput.zig` (instrumentação diagnóstica — ver `RIVER_CPU_HANDOFF.md` na raiz do repo).
- Versão: `0.5.0-dev.74+e24e2bc`, wlroots **0.20.1** (sistema), Zig 0.16, bindings zig-wlroots v0.20.1.
- **Os números de linha abaixo referem-se à árvore atual (com o diff não commitado aplicado).**

## Arquitetura (contexto mínimo para quem implementa)

River master é não-monolítico: o window manager é um cliente externo (`maindeck-wm`) falando `river-window-management-v1`. O ciclo central é:

```
algo muda → dirtyWindowing()/dirtyWindowingLazy()/dirtyRendering()
  → idle callback → manageStart (diff de estado → eventos pro WM) → wm responde manage_finish
  → manageFinish (configures pros apps; espera acks, timeout 100ms)
  → renderStart → wm responde render_finish
  → renderFinish (aplica scene graph, commitOutputState, cursor.updateState, processa input enfileirado)
```

Pontos críticos de performance:
1. **`scheduled.dirty` (hard) bloqueia o processamento de input** até o ciclo fechar (`WindowManager.zig:46-47`, `Seat.zig:360-396`). `dirty_lazy` não bloqueia.
2. `renderFinish` roda `cursor.updateState()` + `commitOutputState()` + `idle_inhibit_manager.checkActive()` em **todo** ciclo (`WindowManager.zig:546-561`).
3. Todo evento de input é enfileirado (`Seat.event_queue`, Deque fixo de 1024, sem alocação) e processado só com WM em `.idle`.

## Regras de execução (obrigatórias)

1. **Medir antes, medir depois.** Nenhum item entra sem baseline e comparação. Não instalar no ambiente real sem validação. Não reiniciar o River automaticamente (derruba a sessão gráfica) — pedir ao usuário.
2. **Pré-requisito**: completar o fluxo do `RIVER_CPU_HANDOFF.md` (a instrumentação já instalada precisa de reboot/relogin para carregar; ler `frame diagnostics` e `client_pid` no log da sessão). Os dados dela decidem prioridades reais.
3. Um item por commit, com justificativa e medição no corpo do commit. `git diff` contra o original antes de afirmar qualquer coisa.
4. Não "otimizar" escondendo log de erro nem quebrando o contrato Wayland (ver seção "Não mexer").

### Comandos de medição

```bash
# build/install (igual ao handoff)
cd /home/tcfialho/Documents/poc/references/river
zig build -Dllvm -Doptimize=ReleaseSafe -Dxwayland --prefix ~/.local install --summary all

# CPU idle (baseline e pós-mudança; ≥60s de janela, desktop parado)
river_pid="$(pgrep -n river)"
top -b -d 1 -n 60 -p "$river_pid" | awk '/river/ {s+=$9; n++} END {print s/n "% avg"}'

# wakeups e syscalls (ioctl = atomic commits/tests DRM)
perf stat -e context-switches -p "$river_pid" -- sleep 30
perf trace -e ioctl -p "$river_pid" -- sleep 10 2>&1 | wc -l

# onde a CPU vai
perf top -p "$river_pid"

# diagnósticos já instrumentados
grep -n 'frame diagnostics\|direct scanout\|zero-copy\|client_pid' \
  ~/.local/share/sddm/wayland-session.log | tail -n 100

# fps/frametime em jogo: mangohud; tearing real: cor do log "direct scanout active ... tearing=true"
```

---

## P1 — Responsividade / latência de input

### [x] 1.1 Título/app_id/parent não devem bloquear input (alto impacto, risco baixo)

**Problema.** `Window.notifyTitle()` (`river/Window.zig:1202-1215`) e `Window.notifyAppId()` (`river/Window.zig:1217-1230`) chamam `server.wm.dirtyWindowing()` — dirty **hard**. `XdgToplevel.handleSetParent` (`river/XdgToplevel.zig:463-465`) idem, sem nem checar se o parent mudou. Mudança de título é puramente informativa, mas congela o processamento de input até completar manage+render com o WM client. Terminais (kitty atualiza título por comando/cwd), browsers e Steam mudam título o tempo todo → stalls de input recorrentes e ciclos com input bloqueado.

**Mudança.** Trocar para `server.wm.dirtyWindowingLazy()` nesses 3 pontos (o WM continua recebendo o evento no próximo ciclo idle; só deixa de bloquear a fila de input). Em `handleSetParent`, adicionalmente só dirtiar se `getParent()` difere de `wm_sent.parent`.

**Validação.** `while :; do printf '\e]0;%s\a' $RANDOM; done` num terminal + mover o mouse: antes deve haver micro-stalls/ciclos; depois, input flui. Conferir no log (wm scope debug) que manage sequences continuam ocorrendo.

### [x] 1.2 Um único hit-test de cena por evento de mouse (hot path, risco baixo)

**Problema.** Em modo passthrough, **cada** motion event faz `server.scene.at()` 2×: em `updateHovered()` (`river/Cursor.zig:439`) e em `passthrough()` (`river/Cursor.zig:806`) — mesmas coordenadas. Com pointer constraint presente mas inativa há um 3º walk em `PointerConstraint.maybeActivate()` (`river/PointerConstraint.zig:68`). A 1000Hz de polling são 2000-3000 walks/s da scene graph.

**Mudança.** Em `processMotionRelative` (`river/Cursor.zig:390-415`), fazer 1 chamada a `scene.at()` e passar o resultado para `updateHovered(result)`, `passthrough(result, time)` e `maybeActivate(result)` (assinaturas novas; manter as versões sem argumento para os call sites frios como `updateState`).

**Validação.** `perf top -p` movendo o mouse em círculos: tempo em `wlr_scene_node_at` deve cair ~metade. Comportamento: hover/focus/click inalterados, drag também (modo `.drag` usa o mesmo caminho).

### [x] 1.3 Não re-notificar pointer com coordenadas idênticas a cada ciclo (risco médio)

**Problema.** `renderFinish` chama `seat.cursor.updateState()` em **todo** ciclo (`river/WindowManager.zig:549-551`), que em passthrough faz mais um `scene.at()` + `pointerNotifyEnter` + `pointerNotifyMotion(now, sx, sy)` (`river/Cursor.zig:788-821`) mesmo quando nada mudou sob o cursor. Enter é deduplicado pelo wlroots, mas o **motion com coordenadas iguais é enviado de novo** → acorda o cliente focado a cada ciclo manage (que com 1.1 ainda ocorre por título etc.).

**Mudança.** Em `Cursor`, guardar `(surface, sx, sy)` do último notify; em `updateState`/`passthrough`, pular `pointerNotifyMotion` quando o trio é idêntico (continuar chamando `pointerNotifyEnter`, que é barato/no-op). Não aplicar o skip no caminho de motion real (lá as coords sempre mudam).

**Validação.** `WAYLAND_DEBUG=1` num cliente parado sob o cursor enquanto outro cliente spamma título: eventos `wl_pointer.motion` repetidos devem sumir. Janela movida sob o cursor (op do WM) deve continuar recebendo enter/motion corretos — testar move interativo.

### [x] 1.4 (Opcional) Coalescing de ciclos lazy (risco médio)

**Problema.** Mesmo lazy, cada mudança de título agenda um ciclo manage completo (idle callback dispara imediatamente quando o loop esvazia). Spam de título a 100Hz = 100 ciclos/s (protocolo + Blake3 + scheduleFrame + updateState…).

**Mudança.** Em `WindowManager.dirtyIdle`/`addDirtyIdle` (`river/WindowManager.zig:265-301`): quando **apenas** `dirty_lazy` está pendente (sem `dirty` hard e sem `rendering dirty`), agendar via timer de ~8-15ms em vez de idle imediato, coalescendo rajadas. Hard dirty continua imediato.

**Risco.** Latência adicional para eventos genuinamente lazy (posição do ponteiro pro WM durante op — hoje `opUpdate` usa lazy, `river/Seat.zig:866-871`; mover janela ficaria com passos de ~10ms → **usar timer só se a fila não contiver op ativa**, ou excluir op do coalescing). Item só vale se a medição pós-1.1 ainda mostrar churn relevante.

### 1.5 (Avançado, opt-in) Render delay configurável (estilo `max_render_time` do sway)

Hoje o render acontece no frame event, logo após o vblank anterior (`Output.handleFrame`, `river/Output.zig:465-477`) → até 1 frame inteiro de latência input→foton. Um delay opcional (renderizar N ms antes do próximo vblank, com timer armado a partir do `present`) reduz latência percebida fora de jogos com scanout.

**Risco alto** (frame drops se N mal calibrado, interação com VRR/tearing). Só implementar com medição de `presentation feedback` antes/depois e como opção do protocolo/env var, default off. Fica por último.

---

## P2 — CPU idle / desperdício por ciclo

### [x] 2.1 `commitOutputState` força frame em todos os outputs a cada ciclo (alto impacto no idle)

**Problema.** `renderFinish` → `OutputManager.commitOutputState()` (`river/OutputManager.zig:252-412`) roda em **todo** ciclo manage/render e, para cada output enabled:
- `scene_output.setPosition` + `om.output_layout.add(...)` (`river/OutputManager.zig:264-268`) — mesmo sem mudança de posição;
- **`wlr_output.scheduleFrame()`** (branch `.enabled` no fim, `river/OutputManager.zig:~392-396`) — incondicional. Sem frame pendente, o backend DRM agenda/dispara um frame event **sem haver damage** → wakeup + `handleFrame` + `sendFrameDone` para todas as surfaces visíveis (acordando clientes com frame callback pendente) por ciclo;
- `om.sendConfig()` (último statement) — aloca `wlr.OutputConfigurationV1` + heads e chama set_configuration a cada ciclo.

Isso casa exatamente com o padrão `frame_events alto + needs_frame=false` que a instrumentação do handoff procura. Combinado com clientes que dirtiam com frequência (título!), é candidato forte para os 11-25% de CPU idle.

**Mudança.**
- `scheduleFrame()` apenas quando: houve modeset (`need_modeset`), output recém-habilitado, ou posição/escala/transform mudou neste ciclo (comparar `output.current` vs `output.sent` **antes** de `output.current = output.sent`). O wlr_scene já agenda frames sozinho quando há damage real.
- `output_layout.add` e `scene_output.setPosition` só quando x/y mudou ou output é novo no layout. (Verificar no wlroots 0.20 `types/wlr_output_layout.c::wlr_output_layout_add` se há early-return com coords iguais — mesmo que haja, o gate no River é grátis e explícito.)
- `sendConfig()` só quando algum estado de output mudou neste ciclo (flag acumulada no loop acima). (Verificar `types/wlr_output_management_v1.c::wlr_output_manager_v1_set_configuration` — há diffing por head, mas a alocação do config + heads por ciclo é desperdício de qualquer forma.)

**Risco.** Médio: o `scheduleFrame` incondicional também "garante" primeiro frame após enable/unlock. Manter explicitamente nos casos: `need_modeset`, transição `disabled→enabled`, `lock_render_state != .unlocked` mudando, e primeira passada (`first_modeset`). Testar: ligar/desligar output via WM, lock/unlock de sessão, hotplug.

**Validação.** `frame diagnostics` no log: `no_needs_frame` deve despencar. `perf stat context-switches` em idle com um terminal spammando título.

### [x] 2.2 Blake3 → hash não-criptográfico no render order (micro, trivial)

`renderFinish` calcula **Blake3** sobre a lista de nodes para detectar reordenação (`river/WindowManager.zig:491-508`). Trocar por `std.hash.Wyhash` (seed fixa). Mesma semântica, fração do custo, roda a cada ciclo.

### [x] 2.3 Blindar contra layer-surface "chatty" (bar) (risco baixo)

**Problema.** `LayerSurface.handleCommit` (`river/LayerSurface.zig:150-172`) re-arranja o output **e** dá hard `dirtyWindowing()` sempre que `current.committed != 0`. O wlroots seta as flags `committed` quando o cliente **reenvia** um set_anchor/set_exclusive_zone/etc., mesmo com o mesmo valor. Uma bar que reenviar estado por frame geraria 1 ciclo manage por frame da bar (com input bloqueado — é hard dirty).

**Mudança.** Guardar cópia do último estado aplicado (anchor, exclusive_zone, margins, desired size, layer, keyboard_interactive) na `LayerSurface` e só `arrange()+dirtyWindowing()` quando algum **valor** mudou (reparent por layer já é condicional hoje).

**Validação.** Contador agregado (estilo frame diagnostics) de commits de layer-surface vs ciclos disparados; rodar com maindeck-bar real e conferir se a bar reenvia estado (se reenviar, corrigir a bar também — ganho duplo).

### [x] 2.4 Clip da capture scene por commit (hot em jogos, condicional)

**Problema.** `XdgToplevel.handleCommit` roda `window.capture_scene.tree.node.subsurfaceTreeSetClip(&geometry)` em **todo commit** de toda janela (`river/XdgToplevel.zig:324`) — um jogo a 240fps paga isso 240×/s, mesmo sem capture session ativa.

**Mudança.** (a) Verificar no wlroots 0.20 (`types/scene/subsurface_tree.c`) se `wlr_scene_subsurface_tree_set_clip` early-returna com box igual — provavelmente sim; nesse caso o custo é só a chamada e o item vira "skip quando `window.wm_scheduled/sent.capture_session_count == 0` e geometry não mudou" (comparação local barata). (b) Se não houver early-return, cachear o último clip aplicado e comparar antes de chamar.

**Validação.** `perf top` com jogo rodando; procurar `scene_*` no perfil antes/depois.

### [x] 2.5 Agregação de log repetido no `logFn` (IO, risco baixo)

**Problema.** Cada linha de log é um write não-bufferizado no stderr → arquivo de sessão (+journal). O caso real "inactive text input tried to commit…" mostrou spam de erro idêntico degradando IO/CPU. A causa-raiz deve ser corrigida (handoff), mas o compositor não deveria pagar IO linear em spam de cliente bugado.

**Mudança.** Em `logFn` (`river/main.zig:310-321`): se a mensagem formatada (nível+scope+fmt ptr) é idêntica à anterior, contar e segurar; ao chegar mensagem diferente (ou a cada ~5s), emitir `last message repeated N times`. Não suprime conteúdo — agrega. (Comparar por ponteiro de `format` + scope + level é O(1) e pega o caso de spam de um mesmo call site; evitar formatar para comparar.)

**Validação.** Reproduzir spam (cliente de teste) e medir bytes escritos no log e CPU do river.

### [x] 2.6 Caso "inactive text input" (seguir o handoff — pré-requisito de tudo)

Já instrumentado (`river/TextInput.zig::logInactiveCommit`, não commitado). Após reboot: identificar `client_pid` → confirmar fluxo de foco que gera commit inativo → corrigir causa (provável candidato: relay não enviando leave/disable ao perder foco, ou cliente Steam/CEF bugado — nesse caso documentar e aplicar 2.5). **Não** suprimir o log antes de identificar. Decidir destino da instrumentação (reduzir para debug ou remover) só depois.

---

## P3 — Gaming / fullscreen

### [x] 3.1 Cache do teste de tearing (1 ioctl a menos por frame com tearing ativo)

**Problema.** Com tearing pedido pelo WM, `renderAndCommit` chama `wlr_output.testState()` (atomic commit TEST_ONLY no DRM) **todo frame** enquanto funciona (`river/Output.zig:492-514`); o cooldown local existente (commit `e24e2bc`) só protege o caminho de **falha**. A 165-240Hz são 165-240 ioctls extras/s no caminho mais sensível a latência.

**Mudança.** Cachear o resultado de sucesso e pular o teste enquanto a "forma" do commit não muda. Chave de cache mínima: (direct scanout ativo? `scene_output.private.prev_scanout`) + (formato/modifier do buffer? na prática: transição scanout↔composição, modeset, ou mudança de modo invalidam). Re-testar na primeira frame após qualquer invalidação e opcionalmente a cada N segundos como salvaguarda. Se o commit real falhar com tearing, invalidar e cair para o caminho atual.

**Validação.** `perf trace -e ioctl` durante jogo com tearing: contagem/s deve cair ~pela metade no caminho feliz. Conferir que a transição scanout↔composição (abrir overlay/bar) continua caindo para vsync sem erro de commit.

### 3.2 Política de tearing no maindeck-wm (integração — sem isso, tearing nunca liga)

**Constatação.** River já: expõe `tearing-control-v1` (`river/Server.zig:180`), entrega `presentation_hint` async/vsync por janela ao WM (`river/Window.zig:915-933`) e aceita `set_presentation_mode(async)` por output (`river/Output.zig:423-433`). O teste do Cyberpunk registrou **scanout ativo mas tearing inativo** — ou seja, o **maindeck-wm nunca pediu async**.

**Mudança (no maindeck-wm, não no River).** Ao receber `presentation_hint=async` de janela fullscreen no output → `river_output_v1.set_presentation_mode(async)`; voltar a vsync quando sair de fullscreen/hint mudar. Adicionar override de config (forçar async em fullscreen para jogos que não setam o hint). Nota Proton: o hint chega via Xwayland (≥23.1 repassa tearing-control quando o jogo desliga vsync); jogo nativo Wayland precisa setar tearing-control ele mesmo.

**Validação.** Log local já cobre: `direct scanout active ... tearing=true` + ausência de "tearing page flip test failed". VRR off durante o teste para ver tearing de verdade.

### [ ] 3.3 Toggle de VRR sem caminho de modeset (risco médio)

**Problema.** Qualquer divergência de `adaptive_sync` marca `need_modeset=true` (`river/OutputManager.zig:307-309`) e entra no caminho pesado de `OutputSwapchainManager.prepare/apply` (`river/OutputManager.zig:314-364`) — recriação de swapchain para algo que na maioria dos drivers é uma propriedade atômica leve (`VRR_ENABLED`).

**Mudança.** Tratar mudança *apenas* de adaptive_sync (sem mudança de modo/enable) como commit normal: aplicar `setAdaptiveSyncEnabled` num commit não-modeset (pode ir no próprio `applyNoModeset`/caminho do frame seguinte). Manter o caminho atual quando combinada com mudança de modo. Conferir comportamento do wlroots 0.20 quando o driver rejeita (precisa fallback limpo → manter `need_modeset` como fallback em caso de falha de commit).

**Validação.** `wlr-randr --output X --adaptive-sync enabled/disabled` em loop: sem flicker/blank e sem log de modeset; `perf trace` mostra só atomic commit normal.

### 3.4 Confirmar que capture (Sunshine) não mata o scanout/zero-copy

A instrumentação local já loga `capture_sessions` e transições de zero-copy (`river/Output.zig:599-657`). Com Sunshine streamando um jogo fullscreen: conferir `direct scanout active ... capture_sessions=N` e `zero_copy=true`. Se capture de **output** estiver derrubando scanout, avaliar migrar Sunshine para capture de **toplevel** (`ext-image-capture-source` por janela já é suportado, `river/Server.zig:548-568`) ou documentar o custo. Item de medição/decisão, não de código a priori.

### 3.5 Caminho de input em jogo já é enxuto (sem ação; não regredir)

Com pointer constraint `locked` ativa, `processMotionRelative` só faz `sendRelativeMotion` e retorna (`river/Cursor.zig:381-388`) — zero hit-test. Os itens 1.2/1.3 não devem mudar isso. Teclado: `KeyboardGroup.handleKey` itera bindings 2× por tecla (`river/Seat.zig:790-840`) — N pequeno, ok; `XkbKeyboard.sendState` já faz diff antes de mandar layout/caps/num pros clientes (`river/XkbKeyboard.zig:175-215`). Nada a fazer.

### 3.6 Build flags

- **Sempre `-Dllvm`**: desde `8a1afd9` o default segue o Zig; o backend self-hosted x86_64 gera código pior. O comando do handoff já usa.
- `ReleaseFast` vs `ReleaseSafe`: upstream recomenda Safe (checks de bounds/overflow). Ganho de Fast no river-side é modesto (a maior parte do trabalho quente é no wlroots/C). Recomendação: **manter ReleaseSafe**; revisitar só se `perf top` mostrar tempo relevante em código Zig do river com checks.

---

## Não mexer (contratos que parecem "otimizáveis" mas não são)

- **`sendFrameDone` em `handleFrame`** (`river/Output.zig:476`): contrato de frame callbacks; remover/condicionar quebra throttling de clientes e animações.
- **Sistema de transações/frame perfection** (save de buffers, `inflight_configures`, timeout 100ms em `river/WindowManager.zig:381-386`): é a feature central do river master. O timeout curto já limita o pior caso; não "otimizar" pulando acks.
- **Bloqueio de input durante hard dirty** (`Seat.processEvents`): correção de bindings/focus depende disso. O caminho certo é reduzir *o que* é hard (item 1.1), não remover o bloqueio.
- **`event_queue` fixa de 1024** (`river/Seat.zig:253-256`): bounded e sem alocação por evento — bom como está.
- Log de erro de cliente bugado: agregar (2.5), nunca suprimir antes de identificar a causa (lição do caso text-input).

## Ordem sugerida de execução

1. **Medição pós-reboot** (2.6 + baseline de tudo; decide se 2.1 é mesmo o vilão do idle).
2. 1.1 (lazy p/ título) + 2.2 (Wyhash) — pequenos, seguros, já reduzem churn.
3. 2.1 (gate do commitOutputState/scheduleFrame/sendConfig) — medir frame diagnostics antes/depois.
4. 1.2 (hit-test único) — hot path de mouse.
5. 3.1 (cache do teste de tearing) + 3.2 (política async no maindeck-wm) — juntos destravam tearing barato.
6. 2.3 / 2.4 / 2.5 conforme dados (logs/perfil) confirmarem relevância.
7. 1.3 → 3.3 → 1.4 → 1.5 (crescente em risco; cada um só com medição própria).

Cada item: branch/commit isolado, build com `-Dllvm -Doptimize=ReleaseSafe -Dxwayland`, instalar em `~/.local`, **usuário reinicia a sessão**, medir, registrar antes/depois no commit.
