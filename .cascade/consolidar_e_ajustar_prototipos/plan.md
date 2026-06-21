# Plano: consolidar_e_ajustar_prototipos

> Pré-requisito do executor: ABRIR e LER os 7 arquivos-base antes de editar. Cada novo arquivo é uma CÓPIA do base + diffs abaixo. Preservar IDs, classes e estrutura do motor `wm` existente. Não refatorar além do pedido.

## Arquivos a criar (em `docs/`)
- `prototype-anim-p8.html`  ← cópia de `prototype-anim-p7.html`
- `prototype-anim-p3.2.html` ← cópia de `prototype-anim-p3.html`
- `prototype-anim-p4.2.html` ← cópia de `prototype-anim-p4.1.html`
- `prototype-anim-p5.2.html` ← cópia de `prototype-anim-p5.1.html`
- `prototype-anim-p2.3.html` ← cópia de `prototype-anim-p2.2.html`

## A. p8 (Secos)
1. Copiar p7 byte-a-byte. Atualizar `<title>`/label visível para "P8".
2. Verificar contra feedback P1.1: confirmar que Switch/Alternar Deck EXISTEM e fade de fechar/abrir/maximizar não está sutil demais. Se já herdado do p7 (que o usuário aprovou), NÃO mexer. p8 = validação + rótulo.
   Critério: idêntico em comportamento ao p7; nenhuma regressão.

## B. p3.2 (Suaves)
1. Manter durações p3: abrir 0.18s, fechar 0.10s, max/restore 0.22s. NÃO alterar.
2. Substituir `@keyframes muFadeIn` (opacity puro) pela keyframe `muDeck` (fornecida no brief) usada na transição de Deck.
3. Trocar a referência `animation: muFadeIn ...` do Deck para `animation: muDeck <mesma duração/easing do p3>`. Manter a duração original do Deck do p3 (não copiar a do p3.1).
4. Manter Foco e demais animações do p3 intactos.
   Critério: Deck ganha rise 8px + scale 0.93→1; resto idêntico ao p3.

## C. p4.2 (ajustar P4.1) — Foco + abrir/fechar central
1. Foco: adicionar `@keyframes focusLift` (do brief). Aplicar a classe/animação no handler de foco que hoje é inexistente (`focus`/`activate`). Localizar onde p4.1 marca janela ativa e disparar `el.style.animation = 'focusLift 0.25s ease-out forwards'` (e limpar ao perder foco).
2. **Resposta Q1 — detecção síncrona de 1 janela** (sem quebrar motor):
   - ABRIR: ler a contagem APÓS inserir a janela no array. Capturar `const isSolo = wm.windows.length === 1;` no instante imediatamente após o push e antes de aplicar classe de animação.
   - FECHAR: ler a contagem ANTES de remover. `const isSolo = wm.windows.length === 1;` no início do handler de close.
   - Em ambos: `el.classList.add(isSolo ? 'anim-center' : 'anim-slide');` e usar essa classe para selecionar keyframe. NÃO mudar a ordem de mutação de estado existente; só inserir leitura+branch.
   - Definir keyframes:
     ```css
     @keyframes centerOpen { from{opacity:0;transform:scale(0.92)} to{opacity:1;transform:scale(1)} }
     @keyframes centerClose{ from{opacity:1;transform:scale(1)} to{opacity:0;transform:scale(0.92)} }
     ```
     `.anim-slide` continua usando as translações laterais já existentes do p4.1.
   - Garantir limpeza da classe no `animationend` para não vazar entre transições.
   Critério: janela única abre/fecha do centro (fade+scale); com ≥2 janelas, mantém slide lateral; Maximizar e Alternar Deck do p4.1 inalterados.

## D. p5.2 (ajustar P5.1) — amortecer overshoot
1. Em `@keyframes springExpand` e `springRetract`: trocar o pico de rebote `scale(1.02)` → `scale(1.005)`. Localizar TODOS os keyframe-stops com 1.02 (pode haver no meio, ex. 60%/70%).
2. Opcional coerente: reduzir keyframe-stop intermediário para suavizar curva (ver Q2).
   Critério: Maximize/Restore cresce/volta com rebote quase imperceptível; sem "balança muito". Foco e Switch do p5.1 intactos.

## E. p2.3 (ajustar P2.2) — corrigir teleporte no Restore/Close
1. Garantir CSS: `transition: left 0.22s <bezier>, top 0.22s <bezier>, width 0.22s <bezier>, height 0.22s <bezier>;` na janela.
2. Em `actionRestore`: setar `wm.maximized = false` e chamar `applyLayout()` SÍNCRONO no mesmo frame em que a animação dispara, para o transition CSS interpolar geometria continuamente até o alvo. Remover qualquer set abrupto de tamanho após `animationend` que cause o salto.
3. Ajustar `@keyframes maxRetract`: convergir para `scale(1.0)` no 100% (sem manter escala inflada que depois é cortada). Garantir que keyframe e transition de geometria não conflitem (preferir transition de left/top/width/height para a geometria, keyframe só para opacity/scale residual, terminando em estado neutro).
4. Aplicar mesma lógica ao FECHAR maximizado: animar encolhimento até o tamanho-alvo (main/deck) antes de remover o nó, evitando "cai bruscamente".
   Critério: ao restaurar/fechar de estado maximizado, encolhe continuamente até a posição final; sem corte/teleporte.

## Resposta Q2 — beziers/keyframes amortecidos
- p5.2 e p2.3 (físico realista, sem balanço): saída suave `cubic-bezier(0.22, 1, 0.36, 1)` (easeOutQuint, decelera sem overshoot).
- Se quiser micro-overshoot controlado: `cubic-bezier(0.34, 1.12, 0.64, 1)` (overshoot ~2%, bem menor que o atual).
- springExpand/Retract p5.2: stops `0%{scale(1)} 60%{scale(1.005)} 100%{scale(1)}` (ou alvo final maximizado), duração 0.22–0.28s.
- maxRetract p2.3: `0%{opacity/scale do max} 100%{scale(1);opacity(1)}` linear com a geometria via transition; nunca terminar em escala ≠ 1.

## Critérios de aceitação globais
- 5 arquivos novos abrem standalone (sem deps externas além das já usadas pelos bases).
- Cada um difere do base APENAS nos pontos especificados; nenhuma regressão nas categorias aprovadas.
- Títulos/labels atualizados para a nova versão.

## Riscos
1. NÃO tenho os arquivos-base; assinaturas reais (`actionRestore`, `applyLayout`, nome do array de janelas, classes de animação) precisam ser confirmadas na leitura — adaptar nomes se divergirem.
2. Conflito keyframe×CSS-transition no p2.3: animar geometria por transition E scale por keyframe simultaneamente pode brigar; isolar responsabilidades (geometria=transition, scale/opacity=keyframe terminando neutro).
3. Vazamento de classe de animação no p4.2 se faltar limpeza em `animationend` → estados misturados.
4. Foco no p4.2: `focusLift` 100% mantém scale(1.01)/sombra — confirmar que há reset ao desfocar para não acumular janelas "levantadas".