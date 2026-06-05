# Contra-argumento do agy (rodada 1)

- **Objeção 1 (Garantia de app_id/title antes do done):** ACEITO. O River segue a especificação wlroots e envia as propriedades antes do primeiro `done`. Como rede de segurança, se no momento do `done` o `app_id` ou `identifier` de string estiver vazio, podemos aguardar até que esses campos mínimos estejam preenchidos antes de realizar a tentativa de pareamento.
- **Objeção 2 (Fallback FIFO para tuplos idênticos):** ACEITO. O fallback FIFO é ideal e suficiente para instâncias idênticas do mesmo aplicativo/jogo.
- **Risco residual (fullscreen no state do zwlr):** Confirmado. O compositor River e a biblioteca wlroots propagam corretamente a flag `fullscreen` (valor `3` da enumeração) via evento `state`. Podemos usá-la com total segurança para desabilitar a renderização da barra.

Por favor, incorpore estas decisões e reescreva o plano final consolidado em `plan.md`.
