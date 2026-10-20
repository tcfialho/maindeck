# Otimizações implementadas — 20/10/2026

Foram implementados os itens autorizados **1, 2, 4 e 5** do inventário. Durações, curvas e seleção das animações foram preservadas, assim como os gestos de toque/segurar e seus limiares.

## 1. Ícones sem bloquear a interface

A busca em diretórios e a decodificação PNG/SVG passaram para um worker. O fluxo da interface retorna imediatamente um placeholder e continua processando entrada. O worker publica os resultados por eventfd; apenas a thread da interface atualiza o cache e os objetos Wayland.

Pedidos repetidos são agrupados, incluindo resultados negativos. Resultados prontos não guardam ponteiros para janelas ou itens da lista, evitando referências a elementos fechados durante a carga. O encerramento recolhe os resultados, cancela a fila restante e libera os recursos. Uma atualização do catálogo permite tentar novamente os ícones antes ausentes, preservando superfícies já emprestadas aos clientes.

## 2. Launcher reaproveitado

Depois da primeira abertura, o processo permanece residente. Fechar, pressionar Escape ou iniciar um aplicativo desmonta a superfície do menu e libera o foco; não encerra o processo. As próximas aberturas reaproveitam lista, ícones, fontes e buffers. A busca e a seleção são reinicializadas para manter o comportamento de uma abertura nova.

O atalho do WM aciona diretamente o residente por IPC, sem iniciar um processo auxiliar quando ele já existe. A lista verifica mudanças de diretórios ao reabrir e usa inotify para detectar também edições em arquivos `.desktop` existentes. O lançamento de apps usa posix_spawn, evitando trabalho de alocação no filho de um processo que agora possui uma thread de ícones.

A primeira abertura ainda precisa inicializar o processo; o reaproveitamento beneficia as reaberturas. O cache residente permanece na memória entre aberturas, sem um loop de redesenho quando o menu está oculto.

## 4. Redesenho regional da barra

A barra desenha apenas as seções alteradas. Se a largura de uma seção mudar, recalcula e redesenha o quadro completo. As regiões de clique das seções não alteradas são preservadas.

Os dois buffers partem da imagem completa mais recente antes de um desenho parcial. O código acompanha os eventos de liberação e não escreve em buffers ainda ocupados pelo compositor. A recriação da superfície em fullscreen invalida os contextos Cairo antes de liberar o mapeamento antigo, mantendo os dados das janelas.

Não foram alterados cores, fontes, proporções ou desenhos dos componentes.

## 5. Diagnóstico leve e acionável

- `warn` é o padrão: avisos e erros continuam registrados.
- `info` habilita informações de operação.
- `debug` habilita decisões de foco, janelas e estado.
- `trace` inclui os detalhes de renderização, visibilidade e caches que antes poluíam o diagnóstico normal.
- `off` desliga a verbosidade e mantém avisos e erros.

Os níveis são trocados ao vivo para WM, barra e launcher. A configuração compartilhada é consultada no máximo uma vez por segundo por processo, com cache e sem bloquear esperando outra thread. A filtragem acontece antes de formatar os argumentos. O WM não registra mais uma ação vazia a cada ciclo. Logs históricos foram preservados.

```sh
maindeck-diagnostics status
maindeck-diagnostics debug
maindeck-diagnostics trace
maindeck-diagnostics warn
```

`status` mostra o nível, os processos e a presença dos sockets. É uma inspeção operacional, não uma prova de apresentação de frames. Os logs do WM continuam em `~/.local/state/maindeck/maindeck.log`; barra e launcher usam os destinos existentes da sessão.

A comunicação WM→barra reutiliza o socket e envia snapshots quando o estado muda. Há renovação periódica de cinco segundos e uma solicitação explícita de sincronização quando a barra inicia, preservando a recuperação após reinício e a poda de entradas fantasma.

## Validação

**37 cenários aprovados**, com evidências:

- [15 testes de estado](state-tests.txt), incluindo deduplicação de snapshots, ressincronização e 5.000 operações de estresse.
- [Redesenho e ícones](render-icons-tests.txt): 501 imagens parciais idênticas byte a byte às completas; publicação, deduplicação, cache negativo e encerramento do worker. O teste bloqueia a leitura de um ícone em um FIFO e comprova que a interface consegue enfileirar 2.000 consultas antes de liberar a leitura. Estes testes usam AddressSanitizer/UndefinedBehaviorSanitizer.
- [8 testes do desktop](desktop-tests.txt): oito ciclos de abrir/fechar com o mesmo PID, atalho do WM, Escape, busca, lançamento, atualização de catálogo e de arquivos existentes, níveis de diagnóstico ao vivo e três ciclos de recriação da barra em fullscreen.
- [4 testes de classificação](classification-tests.txt) e [8 testes de ciclo de vida/foco com GTK](lifecycle-tests.txt), preservando as correções anteriores.

Os tempos de renderização no teste C incluem instrumentação dos sanitizadores; servem para acompanhar esse ensaio e não representam FPS ou ganho percentual do desktop. Os tempos de reabertura registrados no teste headless incluem captura de tela e verificação por polling; não são medidas de latência no monitor físico. A prova principal é a ausência de bloqueio no fluxo da interface e a preservação da imagem e do comportamento.

Para repetir as verificações antes de instalar:

```sh
bash tools/install-maindeck.sh --test
```

## Sobre deixar as animações mais suaves

O item 3 do inventário incluía experimentar durações menores. Essa parte foi descartada a pedido do usuário: nenhuma duração foi encurtada.

Uma investigação de fluidez deve medir intervalos entre frames apresentados, pausas ocasionais e interrupções de transições. Uma animação de 200 ms continuaria com 200 ms; o objetivo seria apresentar seus quadros regularmente e continuar a partir da posição visível quando interrompida, preservando a curva e o desenho existentes. Também é necessário observar o orçamento de renderização no monitor real e sob carga. As otimizações desta entrega retiram trabalho bloqueante da interface, mas não modificam esse mecanismo de animação nem comprovam por si só que a causa dos engasgos do compositor foi eliminada.

## Instalação

Os binários validados foram instalados em `~/.local/bin`, e o WM e a barra reiniciaram com sucesso. O nível padrão da sessão passou para `warn`. O launcher residente será iniciado na próxima abertura do menu. A instalação manteve backup dos três binários anteriores e do init da sessão. [Saída da instalação](deployment-output.txt) e [verificação de PIDs, binários e backup](deployment-verification.json).
