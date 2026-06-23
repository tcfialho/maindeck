# Análise do Crash no Deck Switch (Win+Seta Esquerda)

## Sintoma
Após as alterações para tornar a animação de deck switch mais pronunciada, pressionar `Win + Seta Esquerda` (que aciona `ACTION_DECK_PREV` -> `md_deck_prev`) causa o crash instantâneo do compositor `river`. O atalho `Win + Seta Direita` funciona normalmente.

## Código Modificado e Contexto
1. Em `Animation.zig`:
   - `armDeckOut` (animação de saída, que gera um órfão):
     ```zig
     .clip_travel = (dx < 0.0),
     .clip_travel_x = if (dx < 0.0) @abs(dx) else 0.0,
     ```
     Como `Win + Seta Esquerda` faz o deck anterior sair para a esquerda, `dx` é negativo (`-30%` da largura), ativando `.clip_travel = true` no órfão.
   - Em `advanceOrphans` (processamento de órfãos):
     ```zig
     if (orphan.anim.clip_travel) {
         const p = orphan.anim.progress(now_ns);
         const clip_x_f: f32 = orphan.anim.clip_travel_x * p;
         const clip_x: i32 = @intFromFloat(@round(clip_x_f));
         const clip: wlr.Box = .{ .x = clip_x, .y = 0, .width = orphan.nat_w, .height = orphan.nat_h };
         orphan.tree.node.subsurfaceTreeSetClip(&clip);
     }
     ```

## Análise da Causa Raiz
1. O compositor `river` usa a biblioteca `wlroots` para gerenciar a cena (`wlr_scene`).
2. A função `subsurfaceTreeSetClip` mapeia diretamente para `wlr_scene_subsurface_tree_set_clip` em wlroots.
3. No arquivo `types/scene/subsurface_tree.c` do wlroots, a função é implementada assim:
   ```c
   void wlr_scene_subsurface_tree_set_clip(struct wlr_scene_node *node, const struct wlr_box *clip) {
       bool found = subsurface_tree_set_clip(node, clip);
       assert(found);
   }
   ```
   A função faz um assert `assert(found)` que falha (causando o crash) se nenhuma subsurface tree for encontrada sob o nó especificado.
4. As janelas ativas (`window.surfaces.tree`) são criadas pela shell e envelopadas como subsurface trees, portanto possuem o addon de subsurface e a busca retorna `true`.
5. No entanto, as árvores órfãs (`orphan.tree`) criadas em `spawnDeckOut` são árvores de cena comuns (`createSceneTree()`) e seus filhos são buffers copiados comuns (`createSceneBuffer()`), sem qualquer subsurface tree associada.
6. Portanto, chamar `orphan.tree.node.subsurfaceTreeSetClip(&clip)` retorna `false` no wlroots, engatilhando o `assert(found)` e crashando o compositor imediatamente.
7. O crash ocorre apenas no `Win + Seta Esquerda` porque ele move o órfão para a esquerda (`dx < 0`), ativando a flag `.clip_travel = true`, enquanto `Win + Seta Direita` move para a direita (`dx > 0`), mantendo `.clip_travel = false`.

## Proposta de Correção
Reverter a aplicação de clipe na saída do órfão (mantendo `.clip_travel = false` na saída). A janela órfã irá apenas deslizar e fazer fade-out sem máscara de clipe (o que dura apenas 130ms e é imperceptível).
Remover o bloco de clip em `advanceOrphans`.
Manter o clipe na entrada da janela live (`deck_in_left`), que não sofre desse problema pois a janela possui subsurfaces válidas.

Por favor, valide esta hipótese e confirme se a correção proposta resolve a causa raiz de forma limpa e segura.
