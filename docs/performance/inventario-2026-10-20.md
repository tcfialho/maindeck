# Oportunidades de desempenho e resposta percebida — MainDeck

Inventário baseado no código atual do WM, barra, launcher e compositor River local. Esta etapa não alterou código executável, configuração ou UX.

**Restrição de UX confirmada:** manter os atalhos com toque/segurar, seus significados e a distinção entre os gestos. A espera pelo release ou pelo limiar de hold não é classificada como defeito de desempenho. Medir o trabalho depois que o gesto foi reconhecido.

## Ordem sugerida

| Prioridade | Oportunidade | Evidência no código | Efeito esperado para o usuário | Esforço / cuidado |
| --- | --- | --- | --- | --- |
| 1 | Resolver/carregar ícones fora do fluxo da interface, com placeholder imediato | `bar-icons.c:147`, `bar-icons.c:196`, `bar-icons.c:370`: busca recursiva, acesso a arquivos e PNG/SVG síncronos; `bar-taskbar.c:79` chama o carregador durante eventos. O notify fd já existe como interface, mas retorna -1 em `bar-icons.c:470`. | Evitar pausas ao surgir um app novo, abrir o menu ou encontrar um ícone ausente. | Médio. Manter ordenação, descarte seguro de resultados antigos e atualização após instalação/troca de tema. Publicar o resultado pronto no fluxo da UI. |
| 1 | Reutilizar o launcher entre aberturas | `wm-input.c` inicia `maindeck-menu`; `maindeck-menu.c:1912` carrega/verifica a lista antes de conectar ao Wayland e recria recursos a cada execução. Já há cache da lista em disco. | Menu reaparece mais rápido; busca e ícones continuam quentes entre aberturas. | Médio. Comparar modo residente com memória/energia, foco, restauração e atualização dos apps instalados. |
| 1 — experimento visual | Perfil de animações mais ágil, sem mudar os gestos | `wm-animation-intents.c:14`: abrir 220 ms, fechar/reorganizar 200 ms, nudge 160 ms, spring 280 ms. | Trocar/restaurar janelas pode parecer mais direto, mesmo sem reduzir o custo de CPU. | Baixo para experimentar, médio para validar. Fazer comparação de perfis por ação; por exemplo, testar reorganizações em 140–180 ms. Esses valores são candidatos, não recomendação validada. Preservar continuidade e interrupção suave. |
| 2 | Redesenhar somente as regiões alteradas da barra | `bar-render.c:604` repinta fundo, atalhos, status, tray e taskbar a cada redraw. `bar-surface.c:424` já limita o damage enviado por seção. | Hover e títulos que mudam frequentemente gastam menos trabalho de desenho; menor chance de pequenas travadas sob carga. | Médio. Damage parcial já existe; o ganho procurado é no desenho Cairo. Respeitar os dois buffers, conteúdo antigo e mudanças de largura. |
| 2 | Tirar diagnóstico verboso do uso normal, mantendo opção de ativação | `~/.config/river/init:9` exporta `MAINDECK_LOG=debug`; `wm-input.c:488` registra até ACTION_NONE. `wm-log.c` formata datas e faz flush de eventos. | Menos escrita e trabalho auxiliar durante interação. | Baixo. Ganho perceptível ainda não medido; manter avisos e diagnóstico sob demanda. |
| 2 | Enviar o estado WM→barra quando mudar, com ressincronização | `wm-handlers.c:61` reconstrói o conjunto de janelas, chama `window_index` em uma iteração e abre/fecha um socket para cada envio; o manage o publica a cada ciclo. | Menos mensagens, buscas e despertares quando nada visual mudou. | Médio. Barra reiniciada precisa receber snapshot completo; o mecanismo de poda de janelas usa freshness e não pode ficar sem renovação. Reutilizar socket e comparar snapshot são possibilidades. |
| 2 | Inicialização assíncrona da bandeja | `bar-tray.c:483` faz uma chamada D-Bus bloqueante com timeout de 2.000 ms; `bar-main.c` inicializa a tray antes do primeiro render explícito. | A barra pode aparecer sem esperar por um serviço lento da bandeja. | Médio. Os 2 segundos são limite de espera, não atraso observado. Tratar timeout, serviço ausente e resposta tardia. |
| 2 | Agrupar atualizações da barra por frame e ignorar conteúdo idêntico | `bar-taskbar.c:66` pede redraw ao receber título; `bar-main.c:504` desenha após eventos quando dirty. | Menos redraws redundantes para título animado e movimento do mouse; maior previsibilidade do tempo por frame. | Médio. O loop já agrupa eventos do mesmo despacho. Medir redundância entre despachos antes de introduzir frame pacing; não adicionar um frame de atraso ao clique. |
| 3 | Reaproveitar a visão do layout durante cada ciclo | `wm-layout.c` calcula alvo/índices/visibilidade por buscas na lista; foco e snapshots provocam passagens adicionais. | Escala melhor com muitas janelas, minimizadas e diálogos. | Médio. Baixo retorno provável com poucas janelas. Manter as invariantes corrigidas na revisão e o teste de 5.000 operações. |
| 3 — medir primeiro | Avaliar agendamento de frames no River sob carga | `../references/river/river/Output.zig:523`: render imediato por padrão; atraso opcional é ignorado com tearing ou adaptive sync. Já há render por damage e parada quando a animação termina. | Regularidade de frames e menor cauda de latência podem melhorar a fluidez. | Alto. Comparar no hardware real com/sem captura; não forçar tearing nem alterar VRR por suposição. Não foi identificado um gargalo confirmado nesse agendamento. |

## Medições exploratórias feitas

Microbenchmark do `bar-icons.c` atual, compilado isoladamente com `-O2`. Cada nome executou em um processo novo; a segunda medida é a média de 10.000 consultas ao cache do mesmo processo. O cache de arquivos do sistema não foi limpo. A primeira chamada inclui inicialização de bibliotecas; estes números não são latência de tela nem uma comparação de implementações.

| Nome consultado | Encontrado | Primeira chamada | Consulta com cache |
| --- | --- | ---: | ---: |
| kitty | Sim | 40,657 ms | 0,005 µs |
| chromium | Sim | 0,206 ms | 0,006 µs |
| org.gnome.Calculator | Não | 110,558 ms | 0,012 µs |
| nome propositalmente inexistente | Não | 37,106 ms | 0,021 µs |

Isso sustenta a prioridade de retirar o primeiro carregamento do fluxo de interação. Já existe cache positivo e negativo; criar mais um cache genérico não resolve o bloqueio da primeira consulta. São amostras pontuais, sem distribuição estatística ou garantia de ganho.

Uma observação de 5 segundos da sessão atual registrou 50 chamadas de escrita e 5.150 caracteres escritos pelo WM, com diagnóstico ativo. O contador de CPU teve variação de 0 ms para o WM, 10 ms para a barra e 160 ms para o River. A granularidade dos contadores é de 10 ms e a sessão não era um ensaio controlado de repouso. Esses valores não demonstram saturação de CPU nem justificam priorizar micro-otimizações do WM antes dos bloqueios da interface.

## Como validar o ganho percebido

- **Launcher:** do acionamento ao primeiro frame apresentado; separado entre primeira abertura e reaberturas.
- **Ícones:** maior pausa do loop ao abrir app conhecido, app ainda não visto e app sem ícone; testar também com armazenamento ocupado.
- **Troca de janela:** do reconhecimento da ação ao foco efetivo, ao primeiro frame visível e ao fim da animação. Separar as três medidas.
- **Toque/segurar:** marcar release ou reconhecimento do hold como origem da latência de execução. Preservar a UX existente; medir o tempo físico do gesto separadamente.
- **Fluidez:** distribuição do intervalo entre frames e p95/p99, especialmente durante abertura de apps, títulos animados, busca e compartilhamento de tela. A média sozinha esconde pausas raras.
- **Regressões:** repetir os testes de foco, diálogos, minimização, fullscreen e sequência de estresse após cada lote. Headless serve para lógica e microbenchmarks; validar sensação e apresentação no monitor real.

`tools/benchmark-wm.py:260` cronometra até o helper de teste imprimir `ack`. Esse ACK não comprova foco, frame apresentado ou término da animação. O benchmark atual pode servir como medida auxiliar de custo/handshake, mas precisa de instrumentação de apresentação para sustentar alegações de menor latência visual.

## Implementações já presentes

O projeto já usa cache de ícones e textos Pango, dirty flags, damage parcial na superfície da barra, assinatura de layout para pular trabalho repetido e renderização do compositor orientada a damage. A barra já evita redesenho em fullscreen quando suprimida. As oportunidades acima complementam esses mecanismos; não pressupõem que estejam ausentes.

Sequência de implementação sugerida: **ícones → launcher → comparação dos perfis de animação → desenho regional da barra → diagnóstico/IPC**. Calibrar o ganho de cada lote antes de continuar. Atalhos de segurar permanecem preservados.

## Atualização após aprovação

Os itens 1, 2, 4 e 5 foram autorizados e implementados. O experimento de encurtar animações do item 3 foi rejeitado; durações e curvas foram preservadas. [Implementação, testes e controle de diagnóstico](implementation-2026-10-20/README.md).
