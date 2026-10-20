# Revisão do ciclo de vida do MainDeck WM — 20/10/2026

Foram reproduzidos e corrigidos 11 defeitos adicionais à correção inicial dos popups do Chromium e do Kitty. A revisão cobriu classificação pendente, relações pai/filha, listas MAIN/DECK, foco, minimização/restauração, fullscreen, remoção de outputs e criação de atalhos.

| Local | Defeito reproduzido e correção |
| --- | --- |
| `wm-handlers.c:628` | Pai fechado deixava uma filha pendente apontando para memória liberada. Limpar relações nas duas listas antes de liberar o pai; reinicializar o histórico de layout das órfãs. |
| `wm-layout.c:1219`, `wm-handlers.c:941` | Filhas de raízes flutuantes eram consideradas invisíveis e renderizadas antes do pai. Reconhecer a visibilidade da raiz, posicionar sobre sua geometria e renderizar filhas depois dela; encaminhar/restaurar foco ao abrir/fechar. |
| `wm-layout.c:514` | A preservação do foco de uma flutuante antiga anulava o foco explicitamente pedido por uma nova flutuante. Dar precedência ao pedido explícito. |
| `wm-layout.c:300` | Ativar uma janela tiled pela barra não retirava o foco de uma flutuante. Marcar a ativação como mudança explícita de foco; encaminhar ativações de filhas à raiz e à filha exata. |
| `wm-layout.c:699` | Minimizar uma filha a escondia independentemente do pai; maximizar podia alterar o layout global sem uma raiz válida. Restringir ações do layout MAIN/DECK a raízes tiled. |
| `wm-layout.c:93`, `wm-layout.c:136` | Uma órfã atrás de uma janela minimizada fazia o alvo apontar para a minimizada. Excluir minimizadas da indexação ativa e remover a suposição de que todas as raízes ocupam um prefixo contíguo. |
| `wm-layout.c:699`, `wm-handlers.c:358` | Minimizar durante a classificação movia a janela para outra lista mantendo `pending=true`. Manter a propriedade da lista pendente e classificá-la diretamente como minimizada. |
| `wm-handlers.c:628` | Fechar uma janela não destruía seu objeto `river_node_v1`. Destruir o objeto de protocolo ao liberar a janela. |
| `wm-handlers.c:172` | Remover um monitor deixava `fs_output` apontando para um proxy destruído. Invalidar referências de fullscreen nas duas listas antes de destruir o output. |
| `wm-input.c:144` | Falha de `calloc` ao criar um atalho de teclado causava dereferência nula. Tratar falhas de alocação e criação do proxy. |
| `wm-input.c:187` | Falha de `calloc` ao criar um atalho de mouse causava dereferência nula. Tratar falhas de alocação e criação do proxy. |

## Reprodução e validação

Os testes de estado executam os handlers e o código real do WM com as chamadas de protocolo registradas por um substituto de transporte. AddressSanitizer e UndefinedBehaviorSanitizer confirmaram o acesso à memória liberada e os dois crashes com falha de alocação injetada. Os demais defeitos produziram falhas de assertiva antes da correção. O rastreamento de destruição de nós confirma o pedido de liberação do recurso; não se trata de uma medição de memória total do compositor.

Evidências anteriores às correções: [primeira rodada](state-before.txt), [minimização pendente](pending-minimize-before.txt) e [falhas de alocação](allocation-before.txt).

Validação final: **26 cenários aprovados**:

- [14 testes de estado com ASan/UBSan](state-after.txt), incluindo uma sequência determinística de **5.000 operações** de minimizar, restaurar, maximizar, trocar/navegar pelo DECK e promover janelas. Verifica listas, contagens, indexação e validade do alvo a cada passo.
- [4 testes de protocolo em River headless](classification-after.txt), com clientes xdg-shell para janelas do mesmo aplicativo, mínimos de 179×36 semelhantes aos do Chromium, diálogos de tamanho fixo e tamanho natural de 800×600 sobre o DECK.
- [8 testes com clientes GTK reais em River headless](lifecycle-after.txt), com teclado virtual, foco observado no cliente e captura de tela para confirmar que o diálogo flutuante é efetivamente desenhado. Inclui minimização, restauração, fullscreen e a compatibilidade opt-in de parentalidade implícita.

Para repetir, na raiz do repositório:

```sh
meson compile -C build maindeck-wm
bash tests/run-state-regressions.sh
python3 tools/test-window-classification.py
python3 tools/test-wm-lifecycle.py
git diff --check
```

Dependências dos testes: compilador C com ASan/UBSan, wayland-scanner, wayland-client, xkbcommon, River local, Python do sistema com GTK 3/PyGObject, wtype e grim. Os testes de integração usam runtime e configuração temporários, sem reiniciar a sessão principal.

Também foi executado o analisador estático do GCC em todos os arquivos `wm-*.c`. As falhas de alocação indicadas em `wm-input.c` foram reproduzidas e corrigidas. O aviso de argumento `data` nulo em `window_handle_parent` foi examinado: os listeners são registrados com `Window` não nulo, e esse caminho não demonstrou um defeito reproduzível.

## Limites

A remoção de monitor foi simulada no handler; não foi feito hotplug físico. As reproduções do Chromium usam seus hints observados no log, sem repetir fluxos de autenticação externos. Não foram testadas todas as aplicações XWayland, todas as combinações de múltiplos monitores nem falhas de alocação em todas as bibliotecas. Esta rodada não constitui uma garantia de ausência de outros defeitos. Os testes antigos em shell não foram usados como evidência desta rodada.

A versão validada foi instalada e o WM da sessão principal reiniciado com sucesso. [Registro da instalação e backup](deployment.txt).
