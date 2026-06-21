# Brief: consolidar_e_ajustar_prototipos

## Roteamento
T2 — Esta tarefa envolve reestruturação e criação de 5 arquivos HTML de protótipo de animação com base na fusão e refatoração de código existente.

## Objetivo
Consolidar e refatorar os protótipos de animação do maindeck-wm em 5 versões finais baseadas no feedback do usuário em `planilha-consideracoes.csv`:
1.  **Secos (P6, P7 e P1.1) ➔ Criar `prototype-anim-p8.html`** (Calibração minimalista rápida baseada no P7).
2.  **Suaves (P3 e P3.1) ➔ Criar `prototype-anim-p3.2.html`** (Velocidade e harmonia do P3 + transição de deck `muDeck` do P3.1).
3.  **Ajustar P4.1 ➔ Criar `prototype-anim-p4.2.html`** (Adicionar animação de foco do P5.1 + consistência síncrona/fade ao abrir/fechar a única janela).
4.  **Ajustar P5.1 ➔ Criar `prototype-anim-p5.2.html`** (Amortecer overshoot elástico exagerado no Maximize/Restore).
5.  **Ajustar P2.2 ➔ Criar `prototype-anim-p2.3.html`** (Corrigir comportamento de teleportar/corte brusco ao Restaurar/Fechar sincronizando layout e CSS transitions).

## Contexto e Dados Brutos (planilha-consideracoes.csv)
Aqui está o feedback específico mapeado para cada protótipo:
- **P1.1:** Switch e Alternar Deck inexistentes. Fade rápido sutil demais para fechar/abrir/maximizar. (Grupo Secos)
- **P2.2:** Boa animação e janelas trocam rápido no Switch/Alternar. Foco tem "Tremidinha sutil balança de um lado para o outro". Maximizar/Restaurar: "janela expande e contrai em um fade, mas fechar eh um pouco estranho pq ela contrai ocupando a tela toda e de repente bruscamente cai no tamanho main ou no tamanho deck." Fechar rápido de esq para dir.
- **P3.1:** Alternar Deck é o ponto superior ("ganha rise de 8px e escala"). Foco sutil demais. Maximizar/Restaurar/Fechar/Abrir: prefere velocidade e consistência do P3.
- **P3:** Crescendo muito bem feito para Maximizar. Abrir/Fechar suave (app surge/some). Deck e Foco muito secos/sutis.
- **P4.1:** Excelente troca de janelas e Alternar Deck. Foco inexistente (pessimo). Maximizar: excelente metáfora. Fechar/Abrir: deslizar lateral é legal, mas quando é a primeira/última janela, deveria surgir do centro (fade+scale) e não do lado.
- **P5.1:** Excelente Switch e Foco ("janela cresce e volta com sombra"). Maximizar/Restaurar: cresce demais e volta ("balança muito não gostei").
- **P6 e P7:** Calibração muito rápida e limpa de movimento ("Igual ou melhor que o P6", "um pouco melhor que o P6" em todas as categorias no P7).

## Detalhes das Alterações Solicitadas

### A. Criar `prototype-anim-p8.html`
*   Baseado no [prototype-anim-p7.html](file:///home/tcfialho/Documents/poc/maindeck-wm/docs/prototype-anim-p7.html). Manter a calibração precisa e sem firulas.

### B. Criar `prototype-anim-p3.2.html`
*   Baseado no [prototype-anim-p3.html](file:///home/tcfialho/Documents/poc/maindeck-wm/docs/prototype-anim-p3.html).
*   Manter a velocidade original de Abrir (0.18s), Fechar (0.10s) e Maximizar/Restaurar (0.22s).
*   Substituir a animação de Deck (`muFadeIn` de opacity puro) pela animação `muDeck` do [prototype-anim-p3.1.html](file:///home/tcfialho/Documents/poc/maindeck-wm/docs/prototype-anim-p3.1.html):
    ```css
    @keyframes muDeck {
        from { opacity:0; transform: translateY(8px) scale(0.93); }
        to { opacity:1; transform: translateY(0) scale(1); }
    }
    ```

### C. Criar `prototype-anim-p4.2.html`
*   Baseado no [prototype-anim-p4.1.html](file:///home/tcfialho/Documents/poc/maindeck-wm/docs/prototype-anim-p4.1.html).
*   **Foco:** Adicionar a animação do P5.1 (um pulso leve de escala + elevação com sombra):
    ```css
    @keyframes focusLift {
        0% { transform: scale(1); box-shadow: 0 4px 6px rgba(0,0,0,0.1); }
        50% { transform: scale(1.01); box-shadow: 0 12px 24px rgba(0,0,0,0.25); }
        100% { transform: scale(1.01); box-shadow: 0 10px 20px rgba(0,0,0,0.2); }
    }
    ```
*   **Abrir/Fechar:** Se `wm.windows.length === 1` ao abrir ou fechar, usar animação central (fade + scale) em vez de translação lateral. O deslizar lateral só ocorre se houver outras janelas no deck.

### D. Criar `prototype-anim-p5.2.html`
*   Baseado no [prototype-anim-p5.1.html](file:///home/tcfialho/Documents/poc/maindeck-wm/docs/prototype-anim-p5.1.html).
*   **Maximizar/Restaurar:** Ajustar `@keyframes springExpand` e `springRetract` reduzindo a escala de rebote de `1.02` para `1.005`, atenuando a oscilação elástica incômoda.

### E. Criar `prototype-anim-p2.3.html`
*   Baseado no [prototype-anim-p2.2.html](file:///home/tcfialho/Documents/poc/maindeck-wm/docs/prototype-anim-p2.2.html).
*   **Maximizar/Restaurar:** Resolver o corte brusco em `actionRestore`. Chamar `wm.maximized = false` e `applyLayout()` imediatamente (síncronamente) junto com a animação para permitir que o CSS transition (`transition: left 0.22s, width 0.22s`) anime o encolhimento de forma contínua até a posição alvo, ajustando `@keyframes maxRetract` para suavizar e convergir para a escala final (1.0) no lugar do salto abrupto.

## Perguntas para o Judge
1.  Como estruturar a detecção síncrona de 1 janela no JS do P4.2 para alternar entre as animações laterais e a animação centralizada sem quebrar o motor existente?
2.  Quais os valores de cubic-bezier e keyframes amortecidos ideais para o P5.2 e P2.3 para dar sensação física realista sem gerar balanço excessivo e sem "teleportar"?
