# Análise do Ghost da Janela Órfã sobre a Window MAIN

## Sintoma
Na animação de Deck Switch (`Win + Seta Esquerda`), a janela que está saindo (órfã) desliza para a esquerda em direção à janela MAIN. Como os órfãos rodam na camada `close_overlay` (que fica acima de `wm` e de todas as janelas live normais), a janela órfã renderiza por cima da janela MAIN, criando um "ghost" opaco desagradável vazando sobre a janela principal durante a transição de 130ms.

## Hipóteses de Solução

### Opção A: Dessincronizar a Opacidade (Queda Acelerada de Opacidade)
Atualmente, tanto a posição `x` quanto a opacidade decaem usando o mesmo progresso suavizado pelo easing `.ease_in`. No `.ease_in`, a opacidade cai de forma muito lenta no início e acelera apenas no final.
* **Solução:** Modificar a opacidade do órfão de saída para decair muito mais rapidamente (ex: de forma quadrática com o tempo linear `t * t` ou usando um fator acelerador), fazendo com que o órfão desapareça quase imediatamente enquanto continua deslizando.
* **Vantagem:** Muito simples de implementar, mantém a suavidade da curva física da translação e o ghost some bem antes de cruzar a divisão entre as telas.

### Opção B: Reduzir a Distância de Translação na Saída (`dx`)
Atualmente, definimos a distância `dx` do deck switch como 30% da largura da janela tanto para a entrada (IN) quanto para a saída (OUT). Na entrada, 30% de translação é ótimo para dar impacto visual. Mas na saída, o órfão só precisa dar a ilusão de movimento lateral antes de sumir.
* **Solução:** Reduzir o deslocamento lateral do órfão de saída para 10% ou 15% de sua largura (`dx = -0.10 * old_w` ou `dx = -0.15 * old_w`), mantendo a entrada da nova janela em 30%.
* **Vantagem:** O deslocamento curto garante que o órfão quase não invada a área da janela MAIN, eliminando fisicamente o vazamento visual.

### Opção C: Combinar Translação Curta na Saída com Queda Acelerada de Opacidade (Abordagem Híbrida)
* **Solução:** Reduzir o deslocamento do órfão de saída para 15% de sua largura e fazer sua opacidade decair com o quadrado do tempo.
* **Vantagem:** A combinação das duas técnicas garante que o órfão deslize apenas o suficiente para indicar a direção do movimento e desvaneça rapidamente antes de cruzar a bordinha preta divisória, mantendo a transição extremamente limpa, sem qualquer ghost perceptível e muito fluida.

---

Por favor, valide as opções acima, elenque considerações técnicas adicionais sobre a renderização do wlroots e ajude-nos a selecionar a melhor abordagem para ser implementada.
