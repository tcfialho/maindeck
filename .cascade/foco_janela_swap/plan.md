# PLANO DE IMPLEMENTAÇÃO: Foco Janela Swap (T2)

## 1. Arquivos a Modificar
Os seguintes 10 arquivos HTML de protótipo de animação localizados em `docs/`:
- [docs/prototype-anim-p2.html](file:///home/tcfialho/Documents/poc/maindeck-wm/docs/prototype-anim-p2.html)
- [docs/prototype-anim-p3.html](file:///home/tcfialho/Documents/poc/maindeck-wm/docs/prototype-anim-p3.html)
- [docs/prototype-anim-p3.1.html](file:///home/tcfialho/Documents/poc/maindeck-wm/docs/prototype-anim-p3.1.html)
- [docs/prototype-anim-p5.html](file:///home/tcfialho/Documents/poc/maindeck-wm/docs/prototype-anim-p5.html)
- [docs/prototype-anim-p5.1.html](file:///home/tcfialho/Documents/poc/maindeck-wm/docs/prototype-anim-p5.1.html)
- [docs/prototype-anim-p7.html](file:///home/tcfialho/Documents/poc/maindeck-wm/docs/prototype-anim-p7.html)
- [docs/prototype-anim-p8.html](file:///home/tcfialho/Documents/poc/maindeck-wm/docs/prototype-anim-p8.html)
- [docs/prototype-anim-p9.html](file:///home/tcfialho/Documents/poc/maindeck-wm/docs/prototype-anim-p9.html)
- [docs/prototype-anim-p10.html](file:///home/tcfialho/Documents/poc/maindeck-wm/docs/prototype-anim-p10.html)
- [docs/prototype-anim-p11.html](file:///home/tcfialho/Documents/poc/maindeck-wm/docs/prototype-anim-p11.html)

## 2. Contrato da Função a Modificar
Em cada arquivo, localizar a função `actionSwapMainDeck`. O comportamento padrão esperado é:
```javascript
function actionSwapMainDeck() {
    // ... lógica de swap de wm.windows (ex: swap de elementos no array) ...
    
    // Adicionar a linha abaixo imediatamente após a mutação do array, mas antes de disparar atualizações de interface/animação:
    wm.target_index = wm.target_index === 0 ? 1 : 0;
    
    // ... atualizações visuais ou setTimeout ...
}
```

## 3. Passos de Execução
1. **Inspeção Prévia**: Ler o bloco de código correspondente a `actionSwapMainDeck` em cada um dos 10 arquivos para identificar o ponto exato da troca lógica de janelas no array.
2. **Inserção do Toggle**: Adicionar a instrução `wm.target_index = wm.target_index === 0 ? 1 : 0;` logo após o reposicionamento físico no array `wm.windows` (normalmente envolvendo uma variável temporária ou desestruturação).
3. **Validação do Escopo**: Garantir que a alteração do índice ocorra no escopo síncrono imediato e não dentro de callbacks atrasados (`setTimeout` / `requestAnimationFrame`), a menos que a própria mutação do array também esteja postergada.
4. **Verificação de Integridade**: Validar estaticamente que a estrutura do bloco HTML/script não foi corrompida.

## 4. Critérios de Aceitação
- A variável `wm.target_index` deve alternar seu valor (0 para 1, ou 1 para 0) de forma consistente com a troca de posição das duas janelas principais.
- Não devem ocorrer inconsistências de foco visual em que a janela ativa no deck não coincida com o valor de `wm.target_index`.
- Ausência de erros sintáticos ou de runtime nos consoles dos protótipos modificados.

## 5. Riscos e Mitigações
- **Divergência estrutural**: Protótipos mais antigos (p2, p3) podem ter estruturas de controle ligeiramente diferentes dos mais recentes (p10, p11).
  - *Mitigação*: O executor deve analisar o contexto circundante de `actionSwapMainDeck` de cada arquivo individualmente antes de aplicar a alteração, ajustando o ponto exato de inserção se necessário.