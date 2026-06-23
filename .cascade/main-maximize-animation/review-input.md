# Review Input: Animação de crescimento ao maximizar a janela main

## Contexto da tarefa
- **Objetivo**: Garantir que, ao maximizar a janela `main` (tanto por atalho quanto via menu de contexto), ocorra uma animação de crescimento suave usando clip-reveal (`GROW_REVEAL`), em vez de mudar de tamanho abruptamente sem animação.
- **Critério de aceitação**: A janela maximizada (seja `main` ou `deck`) cresce com `GROW_REVEAL`. A restauração e o layout continuam funcionando.
- **Resultado do Gate**: Compilou com sucesso (`ninja -C build` ok).

## Plano
Substituir o intent `REFLOW_EASE` pelo intent `GROW_REVEAL` para a janela que está sendo maximizada em `wm-input.c` e `wm-layout.c`. Adicionar a marcação de animação no menu de contexto (que estava ausente).

## Diff
```diff
diff --git a/wm-input.c b/wm-input.c
index 5c99def..d0073ee 100644
--- a/wm-input.c
+++ b/wm-input.c
@@ -535,9 +535,9 @@ static void seat_action(struct Seat *seat, enum Action action) {
 		struct Window *target = target_window();
 		if (target != NULL && !wm.maximized) {
 			wm.maximized = true;
-			// O target cresce e a outra some — ambas animam REFLOW. Marca todas
-			// as visíveis (no momento, antes do maximize esconder a outra).
+			// O target cresce com GROW_REVEAL e a outra some.
 			mark_visible_tiled_anim(ANIMATION_INTENT_REFLOW_EASE);
+			target->pending_anim = ANIMATION_INTENT_GROW_REVEAL;
 			wm.focus_dirty = true;
 		}
 		break;
@@ -551,10 +551,16 @@ static void seat_action(struct Seat *seat, enum Action action) {
 		break;
 	case ACTION_TOGGLE_MAXIMIZE:
 		// Win+Shift (tap): keyd emite Ctrl+F19. Alterna maximize/restore.
-		if (target_window() != NULL) {
-			wm.maximized = !wm.maximized;
-			mark_visible_tiled_anim(ANIMATION_INTENT_REFLOW_EASE);
-			wm.focus_dirty = true;
+		{
+			struct Window *target = target_window();
+			if (target != NULL) {
+				wm.maximized = !wm.maximized;
+				mark_visible_tiled_anim(ANIMATION_INTENT_REFLOW_EASE);
+				if (wm.maximized) {
+					target->pending_anim = ANIMATION_INTENT_GROW_REVEAL;
+				}
+				wm.focus_dirty = true;
+			}
 		}
 		break;
 	case ACTION_MINIMIZE_TARGET:
diff --git a/wm-layout.c b/wm-layout.c
index c68b763..62cc3ff 100644
--- a/wm-layout.c
+++ b/wm-layout.c
@@ -366,6 +366,8 @@ void md_maximize_window(struct Window *window) {
 	if (window == NULL || window->closed || window->floating) return;
 	target_tiled_window(window);
 	wm.maximized = true;
+	mark_visible_tiled_anim(ANIMATION_INTENT_REFLOW_EASE);
+	window->pending_anim = ANIMATION_INTENT_GROW_REVEAL;
 	wm.focus_dirty = true;
 	LOG_EVENT("ctx-menu maximize: \"%s\"", window->title ? window->title : "");
 	log_state();
@@ -389,6 +391,7 @@ void md_restore_window(struct Window *window) {
 	 * segue o alvo — restaura se esta é a janela alvo e estamos maximizados. */
 	if (wm.maximized && window == target_window()) {
 		wm.maximized = false;
+		mark_visible_tiled_anim(ANIMATION_INTENT_REFLOW_EASE);
 		wm.focus_dirty = true;
 		LOG_EVENT("ctx-menu restore (unmaximize): \"%s\"", window->title ? window->title : "");
 		log_state();
```
