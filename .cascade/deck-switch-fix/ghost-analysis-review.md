VERDICT: APPROVE
BLOCKERS: 0
[N1] A Opção C (Abordagem Híbrida) é a recomendada por combinar a atenuação da translação com o desvanecimento rápido da opacidade, eliminando o ghost visual de forma eficaz.
[N2] Ao implementar no wlroots, certifique-se de disparar a marcação de dano de superfície (surface damage) a cada frame em que a opacidade ou posição do órfão mudar para evitar artefatos de renderização.