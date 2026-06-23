# Plano: paridade total das animações com o protótipo P15

Protótipo: `/home/tcfialho/Documents/poc/maindeck-wm/docs/prototype-anim-p17.html`
River: `/home/tcfialho/Documents/poc/references/river/river/`
WM: `/home/tcfialho/Documents/poc/maindeck-wm/`

## Contexto

O Bug 1 (fechar como fade) foi resolvido bumpando o bind do WM para v7
(`wm-handlers.c:977`). Os intents de close agora chegam ao river e o `unmap`
mapeia `close_intent` → `CloseStyle` (slide_right/slide_left/fade) corretamente.

Faltam 3 bugs para paridade total com o protótipo P15.

## Matriz alvo (protótipo → river)

| Gesto | Protótipo | River alvo |
|---|---|---|
| DECK NEXT (sai) | `p9DeckOutRight`: translateX 0→+28px, opacity 1→0, 130ms ease-in | novo `armDeckOut` |
| DECK NEXT (entra) | `p9DeckInLeft`: translateX -28px→0, opacity 0→1, clip-path inset(0 0 0 28px)→0, 200ms ease-out | novo `armDeckIn` com clip-que-viaja |
| DECK PREV (sai) | `p9DeckOutLeft`: translateX 0→-28px, opacity 1→0, clip inset(0 0 0 0)→28px, 130ms ease-in | `armDeckOut` direção esquerda |
| DECK PREV (entra) | `p9DeckInRight`: translateX +28px→0, opacity 0→1, clip inset(0 0 0 28px)→0, 200ms ease-out | `armDeckIn` direção direita |
| MINIMIZE | `p10Minimize`: translateY 0→+60px, scale 1→0.55, opacity 1→0, origin bottom center, 200ms ease-in | novo `armMinimize` |
| UNMINIMIZE | `p10Unminimize`: translateY 60→0, scale 0.55→1, opacity 0→1, origin bottom center, 220ms ease-out | novo `armUnminimize` |

## Bug 4 — Minimize/Unminimize

### Animation.zig
1. Adicionar campo à struct `Animation`:
   ```zig
   /// Scale origin: true = bottom-center (minimize/unminimize), false = center (default).
   /// When true, the driver's recenter offset Y is (1-fy)*h (not /2); X stays /2.
   scale_origin_bottom: bool = false,
   ```
2. Novas funções:
   ```zig
   pub fn armMinimize(x, y, dy, from_scale, to_scale, from_op, to_op, dur, ease) Animation
   pub fn armUnminimize(x, y, dy, from_scale, to_scale, from_op, to_op, dur, ease) Animation
   ```
   Ambas: kind `.move`, `start_y = y`, `target_y = y + dy`, scale+opacity endpoints,
   `scale_origin_bottom = true`, `start_fx = start_fy = 1.0` (sem size tween).

### Output.zig (driver advanceAnimations, ~linha 635-641)
No branch `else if (anim.scales() or anim.resizes())`, o offset Y hoje é
`r.dy = (1-fy)*h/2` (centro). Adicionar: se `anim.scale_origin_bottom`,
recalcular `off_y = (1-fy)*h` (origem embaixo). Pode ser feito em
`applyScaleXY` (passando um parâmetro) OU no driver pós-`applyScaleXY`.
**Preferência:** adicionar parâmetro `origin_bottom: bool` a `applyScaleXY`
ou criar `applyScaleXYBottom`. Menos invasivo: criar wrapper que recalcula
só o `dy` após `applyScaleXY` retornar.

### Window.zig (renderFinish, ~linha 1125-1258)
Adicionar branches antes do move-branch genérico:
- `if (open_intent == .minimize)` → `armMinimize(box.x, box.y, 60, 1, 0.55, 1, 0, 200, .ease_in)`
- `if (open_intent == .unminimize)` → `armUnminimize(box.x, box.y, -60, 0.55, 1, 0, 1, 220, .ease_out)`
Zerar `animation_intent` após armar (one-shot).

## Bug 2+3 — Deck switch (deck-out anima + deck-in clip-que-viaja)

### Animation.zig
1. Adicionar campos à struct:
   ```zig
   /// Deck-switch clip-que-viaja: o clip esquerdo começa em clip_travel_x px e
   /// vai a 0 conforme o slide chega ao destino (clip acompanha o translate).
   clip_travel: bool = false,
   clip_travel_x: f32 = 0,
   ```
2. Novas funções:
   ```zig
   /// Deck-out: slide lateral + fade out. dx>0 direita (NEXT), dx<0 esquerda (PREV).
   /// opacity 1→0, scale fixo 1, 130ms ease-in (espelho p9DeckOutRight/Left).
   pub fn armDeckOut(x, y, dx, dur, ease) Animation

   /// Deck-in: slide lateral + fade in + clip-que-viaja. dx>0 vem da direita (PREV),
   /// dx<0 vem da esquerda (NEXT). opacity 0→1, scale fixo 1, 200ms ease-out.
   /// clip_travel=true, clip_travel_x = |dx| (clip esquerdo inicial).
   pub fn armDeckIn(x, y, dx, dur, ease) Animation
   ```

### Output.zig (driver)
No loop `advanceAnimations`, adicionar tratamento de `clip_travel`:
- Se `anim.clip_travel` e não finished: aplicar
  `window.surfaces.tree.node.subsurfaceTreeSetClip(&clip)` onde
  `clip.x = round(anim.clip_travel_x * (1 - progress))`, `clip.y = 0`,
  `clip.width = window.box.width`, `clip.height = window.box.height`.
  (O clip esquerdo diminui conforme a janela chega ao destino.)
- Se finished: `clearClipReveal` (clip vazio).

### Window.zig (renderFinish)
Distinguir deck-switch de open:
- Hoje `open_intent == .slide_in` arma `armSlide` (open em grupo, vira MAIN, sem clip).
- Precisa distinguir: se a janela está entrando no **deck** (slot 1, vem de hidden)
  vs entrando como **main** (slot 0). O deck-in precisa `armDeckIn` com clip.
- **Como saber:** o WM precisa emitir um intent diferente, OU o river decide por
  geometria (box.x > 0 = deck). **Decisão:** o river decide por `window.box.x > 0`
  (janela no slot deck) → deck-in com clip; `box.x == 0` (slot main) → open slide.
  Isso NÃO é inferência de animação (o intent SLIDE_IN já foi declarado pela ação);
  é só escolher a variante visual pelo slot, que é geometria estática, não diff.

  **Alternativa mais limpa:** adicionar intents novos no enum (DECK_IN, DECK_OUT)
  e a ação DECK_NEXT/PREV emitir esses. MAS isso exige mudar protocolo+WM+river.
  **Vou pela alternativa limpa** — é o espírito declarativo do projeto.

  Hmm, mas DECK_OUT já existe no enum (`ANIMATION_INTENT_SLIDE_DECK_OUT`).
  E SLIDE_IN já existe. O problema é que SLIDE_IN é usado tanto para open-em-grupo
  (vira main) quanto para deck-switch (vira deck). Preciso de um intent para
  deck-switch-in distinto de open-in.

  **Decisão final:** reusar SLIDE_IN para deck-in (a janela que entra no deck).
  No river, distinguir open-em-grupo (slot main, box.x==0, sem clip) de
  deck-switch-in (slot deck, box.x>0, com clip) pela posição. Isso é geometria
  estática (não diff), consistente com o design declarativo (o intent já foi
  declarado; a variante visual vem do slot).

- Para deck-out (janela que SAI do deck e fica hidden): hoje `hidden=true` →
  `enabled=false` → cai no `else { SNAP }`. Preciso animar ANTES de hidden.
  **Solução:** no renderFinish, se `requested.hidden` (vai ficar hidden neste
  render) E `was_visible` (estava visível) E `open_intent == .slide_deck_out`:
  armar `armDeckOut` ANTES de aplicar hidden. Mas `window.tree.node.setEnabled(false)`
  já foi chamado em `:1067` (enabled = !hidden). Preciso adiar o setEnabled(false)
  até a animação terminar, OU rodar a animação num orphan tree (como spawnClose).

  **Solução mais simples:** usar `spawnClose`-style orphan para o deck-out.
  Snapshot dos buffers numa tree standalone, animar slide+fade, auto-destruir.
  Reusar a infra de `OrphanClose` mas com `CloseStyle` novo ou um `armDeckOut`.

  **Ainda mais simples:** tratar deck-out como um close-like: a janela sai do
  slot visível, então o WM a marca hidden. No river, se `hidden && was_visible
  && open_intent == .slide_deck_out`, fazer spawnClose com slide (não fade).
  Mas spawnClose é para unmap (janela destruída). Para hidden (janela vive),
  preciso de um orphan que snapshot e anima, mas a janela real fica hidden atrás.

  **Decisão:** criar `spawnDeckOut` em Animation.zig (orphan tree, slide+fade,
  auto-destrói), análogo a `spawnClose` mas para janelas que só ficam hidden.

## Ordem de implementação

1. **Bug 4 (minimize)** — mais auto-contido, sem clip, sem orphan.
2. **Bug 2 (deck-out não anima)** — precisa `spawnDeckOut` orphan.
3. **Bug 3 (deck-in cruza main)** — precisa `armDeckIn` com clip-que-viaja no driver.

## Verificação

Build: `cd /home/tcfialho/Documents/poc/references/river && zig build -Doptimize=ReleaseFast`
Install: `cp zig-out/bin/river /tmp/river-new && mv /tmp/river-new ~/.local/bin/river`
Teste por gesto (3 janelas):
- Win+↓ hold → minimize: desce + encolhe + fade (origem baixo)
- Win+↑ hold (com minimizada) → unminimize: sobe + cresce + fade in
- Win+→ → deck next: atual sai direita+fade, nova entra esquerda+fade+clip
- Win+← → deck prev: atual sai esquerda+fade+clip, nova entra direita+fade+clip

## Notas

- Os logs `[ANIM-DIAG]` e `[CLOSE-DIAG]` estão ativos. Manter até paridade confirmada,
  remover depois.
- Não mexer no Bug 1 (já resolvido).
- Durações/easings: seguir o protótipo (130ms deck-out, 200ms deck-in, 200ms minimize,
  220ms unminimize). O WM já envia durações via `set_animation_intent` — conferir se
  o river honra `animation_duration_ms` nos novos branches.
