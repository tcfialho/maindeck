# Review Request: foco_janela_swap

## Contexto
- Objetivo: Corrigir o bug de foco na janela ao fazer o swap de janelas (Ctrl+Hold+Tab) em todos os protótipos onde o toggle de `wm.target_index` estava ausente.
- Critério de aceitação: O índice de foco `wm.target_index` agora é alternado corretamente ao realizar o swap nos 10 arquivos de protótipo de animação.
- Gate de validação: Verificação estática de conformidade sintática e de fluxo síncrono. Todos os 10 arquivos foram corrigidos com sucesso.

## O Plano
Adicionar a instrução `wm.target_index = wm.target_index === 0 ? 1 : 0;` na função `actionSwapMainDeck` imediatamente após a mutação do array `wm.windows` em cada um dos 10 protótipos de animação (`docs/prototype-anim-p2.html` a `p11.html`).

## O Diff (Arquivos Tracked e Untracked)
```diff
diff --git a/docs/prototype-anim-p2.html b/docs/prototype-anim-p2.html
index f71aedf..bef1196 100644
--- a/docs/prototype-anim-p2.html
+++ b/docs/prototype-anim-p2.html
@@ -1316,6 +1316,8 @@ function actionSwapMainDeck() {
     const tmp = wm.windows[0];
     wm.windows[0] = wm.windows[1];
     wm.windows[1] = tmp;
+    // foco segue a janela: se estava em 0, agora está em 1 e vice-versa
+    wm.target_index = wm.target_index === 0 ? 1 : 0;
     applyLayout();
     renderTaskbar();
     const newMainEl = document.getElementById('win-' + wm.windows[0].id);
diff --git a/docs/prototype-anim-p3.html b/docs/prototype-anim-p3.html
index da86ec6..2b0b5d0 100644
--- a/docs/prototype-anim-p3.html
+++ b/docs/prototype-anim-p3.html
@@ -1293,6 +1293,8 @@ function actionSwapMainDeck() {
   if (mainEl) animClass(mainEl, 'anim-pulse', 120);
   if (deckEl) animClass(deckEl, 'anim-pulse', 120);
   const tmp = wm.windows[0]; wm.windows[0] = wm.windows[1]; wm.windows[1] = tmp;
+  // foco segue a janela: se estava em 0, agora está em 1 e vice-versa
+  wm.target_index = wm.target_index === 0 ? 1 : 0;
   applyLayout(); renderTaskbar();
   showToast('Swap MAIN ↔ DECK');
 }
diff --git a/docs/prototype-anim-p3.1.html b/docs/prototype-anim-p3.1.html
--- a/docs/prototype-anim-p3.1.html
+++ b/docs/prototype-anim-p3.1.html
@@ -1301,2 +1301,4 @@
   const tmp = wm.windows[0]; wm.windows[0] = wm.windows[1]; wm.windows[1] = tmp;
+  // foco segue a janela: se estava em 0, agora está em 1 e vice-versa
+  wm.target_index = wm.target_index === 0 ? 1 : 0;
   applyLayout(); renderTaskbar();
diff --git a/docs/prototype-anim-p5.html b/docs/prototype-anim-p5.html
index dd016de..57c9e38 100644
--- a/docs/prototype-anim-p5.html
+++ b/docs/prototype-anim-p5.html
@@ -1341,6 +1341,8 @@ function actionSwapMainDeck() {
   if (deckEl) animClass(deckEl, 'anim-flip', 250);
   setTimeout(() => {
     const tmp = wm.windows[0]; wm.windows[0] = wm.windows[1]; wm.windows[1] = tmp;
+    // foco segue a janela: se estava em 0, agora está em 1 e vice-versa
+    wm.target_index = wm.target_index === 0 ? 1 : 0;
     applyLayout(); renderTaskbar();
   }, 125);
   showToast('Swap MAIN ↔ DECK');
diff --git a/docs/prototype-anim-p5.1.html b/docs/prototype-anim-p5.1.html
--- a/docs/prototype-anim-p5.1.html
+++ b/docs/prototype-anim-p5.1.html
@@ -1364,2 +1364,4 @@
     const tmp = wm.windows[0]; wm.windows[0] = wm.windows[1]; wm.windows[1] = tmp;
+    // foco segue a janela: se estava em 0, agora está em 1 e vice-versa
+    wm.target_index = wm.target_index === 0 ? 1 : 0;
     applyLayout(); renderTaskbar();
diff --git a/docs/prototype-anim-p7.html b/docs/prototype-anim-p7.html
index db149cd..0c75535 100644
--- a/docs/prototype-anim-p7.html
+++ b/docs/prototype-anim-p7.html
@@ -1327,6 +1327,8 @@ function actionToggleTarget() {
 function actionSwapMainDeck() {
   if (wm.windows.length < 2) return;
   const tmp = wm.windows[0]; wm.windows[0] = wm.windows[1]; wm.windows[1] = tmp;
+  // foco segue a janela: se estava em 0, agora está em 1 e vice-versa
+  wm.target_index = wm.target_index === 0 ? 1 : 0;
   swapGeom();
   showToast('Swap MAIN ↔ DECK');
 }
diff --git a/docs/prototype-anim-p8.html b/docs/prototype-anim-p8.html
index 6f06bfe..20cbc8b 100644
--- a/docs/prototype-anim-p8.html
+++ b/docs/prototype-anim-p8.html
@@ -1293,6 +1293,8 @@ function actionSwapMainDeck() {
   animClass(document.getElementById('win-'+bId), 'anim-fadeout', 80);
   setTimeout(() => {
     const tmp = wm.windows[0]; wm.windows[0] = wm.windows[1]; wm.windows[1] = tmp;
+    // foco segue a janela: se estava em 0, agora está em 1 e vice-versa
+    wm.target_index = wm.target_index === 0 ? 1 : 0;
     noAnim(() => { applyLayout(); renderTaskbar(); });
     animClass(document.getElementById('win-'+wm.windows[0].id), 'anim-fadein', 100);
     animClass(document.getElementById('win-'+wm.windows[1].id), 'anim-fadein', 100);
@@ -1291,6 +1291,8 @@ function actionSwapMainDeck() {
   animClass(bEl, 'anim-out-left',  140);
   setTimeout(() => {
     const tmp = wm.windows[0]; wm.windows[0] = wm.windows[1]; wm.windows[1] = tmp;
+    // foco segue a janela: se estava em 0, agora está em 1 e vice-versa
+    wm.target_index = wm.target_index === 0 ? 1 : 0;
     noAnim(() => { applyLayout(); renderTaskbar(); });
     animClass(document.getElementById('win-'+wm.windows[0].id), 'anim-in-left',  180);
     animClass(document.getElementById('win-'+wm.windows[1].id), 'anim-in-right', 180);
diff --git a/docs/prototype-anim-p10.html b/docs/prototype-anim-p10.html
index db9d343..602a525 100644
--- a/docs/prototype-anim-p10.html
+++ b/docs/prototype-anim-p10.html
@@ -1294,6 +1294,8 @@ function actionToggleTarget() {
 function actionSwapMainDeck() {
   if (wm.windows.length < 2) return;
   const tmp = wm.windows[0]; wm.windows[0] = wm.windows[1]; wm.windows[1] = tmp;
+  // foco segue a janela: se estava em 0, agora está em 1 e vice-versa
+  wm.target_index = wm.target_index === 0 ? 1 : 0;
   noAnim(() => { applyLayout(); renderTaskbar(); });
   showToast('Swap MAIN ↔ DECK');
 }
diff --git a/docs/prototype-anim-p11.html b/docs/prototype-anim-p11.html
index 6d0c7e3..a27948a 100644
--- a/docs/prototype-anim-p11.html
+++ b/docs/prototype-anim-p11.html
@@ -1291,6 +1291,8 @@ function actionSwapMainDeck() {
   const aEl = document.getElementById('win-'+wm.windows[0].id);
   const bEl = document.getElementById('win-'+wm.windows[1].id);
   const tmp = wm.windows[0]; wm.windows[0] = wm.windows[1]; wm.windows[1] = tmp;
+  // foco segue a janela: se estava em 0, agora está em 1 e vice-versa
+  wm.target_index = wm.target_index === 0 ? 1 : 0;
   noAnim(() => { applyLayout(); renderTaskbar(); });
   animClass(aEl, 'anim-flash', 180);
   animClass(bEl, 'anim-flash', 180);
```
