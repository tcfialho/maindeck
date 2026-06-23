# Review Input: Tornar a Animação de Deck Switch mais pronunciada

## Contexto da tarefa
- **Objetivo**: Tornar a animação de deck switch (Win+Tab+Tab / Win+→ / Win+←) muito mais pronunciada no compositor `river`. A distância de translação fixa de 28px foi alterada para 30% da largura da janela, e corrigimos o traveling clip para rodar na entrada e na saída adequadamente.
- **Critério de aceitação**: Movimento nítido e fluido com clipping correto no divisor main-deck.
- **Resultado do Gate**: Compilação de river com zig bem-sucedida e cópia bem-sucedida para local bin.

## Plano
- Modificar `dx` de translação em `Window.zig` para 30% da largura da janela (tanto para entrada como para saída).
- Modificar `Animation.zig` para ativar traveling clip apenas quando `dx < 0` (slide left).
- Adicionar suporte para clipe em órfãos de slide-out no `advanceOrphans` de `Animation.zig` (crescendo de 0 a `clip_travel_x`).
- Remover checagem de subsurfaceTree empty em `advanceOrphans` e em `Output.zig` para que o clipe funcione para todas as janelas (incluindo as sem subsurfaces).

## Diff
```diff
diff --git a/river/Animation.zig b/river/Animation.zig
index 198b55f..6e88b0e 100644
--- a/river/Animation.zig
+++ b/river/Animation.zig
@@ -508,6 +508,8 @@ pub fn armDeckOut(x: i32, y: i32, dx: f32, duration_ms: u32, easing: Easing) Ani
         .start_fy = 1.0,
         .last_fx = 1.0,
         .last_fy = 1.0,
+        .clip_travel = (dx < 0.0),
+        .clip_travel_x = if (dx < 0.0) @abs(dx) else 0.0,
     };
 }
 
@@ -543,8 +545,8 @@ pub fn armDeckIn(x: i32, y: i32, dx: f32, duration_ms: u32, easing: Easing) Anim
         .start_fy = 1.0,
         .last_fx = 1.0,
         .last_fy = 1.0,
-        .clip_travel = true,
-        .clip_travel_x = @abs(dx),
+        .clip_travel = (dx < 0.0),
+        .clip_travel_x = if (dx < 0.0) @abs(dx) else 0.0,
     };
 }
 
@@ -1028,6 +1030,15 @@ pub fn advanceOrphans(now_ns: i64) bool {
             const h_f: f32 = @floatFromInt(orphan.nat_h);
             oy = @intFromFloat(@round((1.0 - s.scale) * h_f));
         }
+        if (orphan.anim.clip_travel) {
+            const p = orphan.anim.progress(now_ns);
+            const clip_x_f: f32 = orphan.anim.clip_travel_x * p;
+            const clip_x: i32 = @intFromFloat(@round(clip_x_f));
+            const clip: wlr.Box = .{ .x = clip_x, .y = 0, .width = orphan.nat_w, .height = orphan.nat_h };
+            orphan.tree.node.subsurfaceTreeSetClip(&clip);
+        }
         orphan.tree.node.setPosition(s.x + r.dx, s.y + oy);
         any_active = true;
     }
diff --git a/river/Output.zig b/river/Output.zig
index 920f262..282e7af 100644
--- a/river/Output.zig
+++ b/river/Output.zig
@@ -660,9 +660,7 @@ pub fn advanceAnimations(now_ns: i64) bool {
                 const clip_x_f: f32 = anim.clip_travel_x * (1.0 - p);
                 const clip_x: i32 = @intFromFloat(@round(clip_x_f));
                 const clip: wlr.Box = .{ .x = clip_x, .y = 0, .width = window.box.width, .height = window.box.height };
-                if (!window.surfaces.tree.children.empty()) {
-                    window.surfaces.tree.node.subsurfaceTreeSetClip(&clip);
-                }
+                window.surfaces.tree.node.subsurfaceTreeSetClip(&clip);
             }
         }
 
diff --git a/river/Window.zig b/river/Window.zig
index 4b8062f..e7af9c8 100644
--- a/river/Window.zig
+++ b/river/Window.zig
@@ -1065,7 +1065,10 @@ pub fn renderFinish(window: *Window) void {
         window.wm_requested.fullscreen == null and
         (early_intent == .slide_deck_out or early_intent == .slide_deck_out_left))
     {
-        const dx: f32 = if (early_intent == .slide_deck_out_left) -28.0 else 28.0;
+        const dx: f32 = if (early_intent == .slide_deck_out_left)
+            -@as(f32, @floatFromInt(old_w)) * 0.30
+        else
+            @as(f32, @floatFromInt(old_w)) * 0.30;
         Animation.spawnDeckOut(
             &window.surfaces.tree.node,
             old_x,
@@ -1076,7 +1079,7 @@ pub fn renderFinish(window: *Window) void {
             130,
             .ease_in,
         );
-        log.info("[ANIM-DIAG]   -> spawned DECK-OUT orphan (slide+fade, dx={d})", .{dx});
+        log.info("[ANIM-DIAG]   -> spawned DECK-OUT orphan (slide+fade, dx={d:.2})", .{dx});
         // The intent is consumed by the orphan; clear so it does not leak.
         window.rendering_requested.animation_intent = 0;
         window.rendering_requested.animation_duration_ms = 0;
@@ -1243,9 +1243,9 @@ pub fn renderFinish(window: *Window) void {
         // Honour the intent explicitly. The intent carries the FULL semantics —
         // direction comes from the ENUM, never from geometry (no box.x sniffing):
         //   - deck_in_right (DECK_PREV, Win+←): deck-switch IN from the RIGHT.
-        //     +28px slide + fade-in + traveling clip (P2.3 p9DeckInRight).
+        //     proportional slide + fade-in (no traveling clip, matching P2.3 p9DeckInRight).
         //   - deck_in_left (DECK_NEXT, Win+→): deck-switch IN from the LEFT.
-        //     -28px slide + fade-in + traveling clip (p9DeckInLeft). The clip pins
+        //     proportional slide + fade-in + traveling clip (p9DeckInLeft). The clip pins
         //     the visible left border at the main<->deck division so the window
         //     does not appear to cross over the main slot.
         //   - slide_in: group-open entrance (becomes main). -45% width solid slide
@@ -1253,17 +1256,21 @@ pub fn renderFinish(window: *Window) void {
             open_anim_ms;
         const easing = animationEasingFromProtocol(window.rendering_requested.animation_easing, .ease_out);
         if (open_intent == .deck_in_right) {
-            // Deck-switch IN from the right (+28px) + fade + traveling clip.
-            window.anim = Animation.armDeckIn(window.box.x, window.box.y, 28.0, 200, .ease_out);
-            window.tree.node.setPosition(window.box.x + 28, window.box.y);
-            window.popup_tree.node.setPosition(window.box.x + 28, window.box.y);
-            log.info("[ANIM-DIAG]   -> armed DECK-IN right (slide+fade+clip, dx=+28)", .{});
+            // Deck-switch IN from the right + fade + traveling clip (no clip for right entering).
+            const dx: f32 = @as(f32, @floatFromInt(window.box.width)) * 0.30;
+            const dx_i: i32 = @intFromFloat(@round(dx));
+            window.anim = Animation.armDeckIn(window.box.x, window.box.y, dx, 200, .ease_out);
+            window.tree.node.setPosition(window.box.x + dx_i, window.box.y);
+            window.popup_tree.node.setPosition(window.box.x + dx_i, window.box.y);
+            log.info("[ANIM-DIAG]   -> armed DECK-IN right (slide+fade, dx={d:.2})", .{dx});
         } else if (open_intent == .deck_in_left) {
-            // Deck-switch IN from the left (-28px) + fade + traveling clip.
-            window.anim = Animation.armDeckIn(window.box.x, window.box.y, -28.0, 200, .ease_out);
-            window.tree.node.setPosition(window.box.x - 28, window.box.y);
-            window.popup_tree.node.setPosition(window.box.x - 28, window.box.y);
-            log.info("[ANIM-DIAG]   -> armed DECK-IN left (slide+fade+clip, dx=-28)", .{});
+            // Deck-switch IN from the left - fade + traveling clip.
+            const dx: f32 = -@as(f32, @floatFromInt(window.box.width)) * 0.30;
+            const dx_i: i32 = @intFromFloat(@round(dx));
+            window.anim = Animation.armDeckIn(window.box.x, window.box.y, dx, 200, .ease_out);
+            window.tree.node.setPosition(window.box.x + dx_i, window.box.y);
+            window.popup_tree.node.setPosition(window.box.x + dx_i, window.box.y);
+            log.info("[ANIM-DIAG]   -> armed DECK-IN left (slide+fade+clip, dx={d:.2})", .{dx});
         } else {
             // slide_in: group-open entrance. -45% solid slide, no clip.
```
