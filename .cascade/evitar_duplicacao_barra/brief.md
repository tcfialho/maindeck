# Brief: evitar_duplicacao_barra

## Roteamento
T2 — O design envolve concorrência entre processos (instâncias concorrentes da barra) e uso de sinais/locks (flock, kill), com controle de concorrência e gerenciamento de processos.

## Objetivo
Evitar que múltiplas instâncias da barra `maindeck-bar` fiquem rodando simultaneamente após o reinício da sessão do River. A solução deve garantir de forma cirúrgica e performática que a instância mais nova (iniciada pela sessão atual) encerre a instância anterior (se houver) e assuma o controle.

## Contexto da base
- Arquivo: `bar-main.c` (ponto de entrada da barra, gerencia conexões Wayland, IPC sockets e o loop principal de eventos).
- Padrões a seguir: Uso de syscalls POSIX padrão (C99 compatível com as flags de compilação existentes).
- Gate: `ninja -C build`

## Contratos e descobertas
- Atualmente, a barra é executada via loop infinito em background no script `~/.config/river/init`:
  ```sh
  (
      while true; do
          env XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR}" \
              GDK_BACKEND=wayland \
              WAYLAND_DISPLAY="wayland-1" \
              MAINDECK_LOG="${MAINDECK_LOG}" \
              /home/tcfialho/.local/bin/maindeck-bar
          status=$?
          echo "maindeck-bar exited with status ${status}; restarting in 1s"
          sleep 1
      done
  ) &
  ```
- Quando o River reinicia, o script `init` é executado novamente, gerando um novo loop em background. O loop anterior continua vivo (herdado pelo init/systemd) e tenta reconectar à barra assim que o display Wayland estiver disponível de novo, resultando em duas instâncias conectadas e barras duplicadas.
- O diretório `XDG_RUNTIME_DIR` está disponível no ambiente da barra (obtido via `getenv("XDG_RUNTIME_DIR")` em `ipc_init`).

## Restrições e invariantes
- Não devemos travar a inicialização se a instância antiga já tiver morrido ou crashado. O lock deve ser limpo automaticamente pelo sistema operacional (advisory lock via `flock`).
- Se a instância anterior estiver ativa, devemos tentar finalizá-la graciosamente (SIGTERM).
- Evitar falsos positivos se o PID gravado no arquivo lock pertencer a um processo não relacionado (verificar o nome do processo em `/proc/[pid]/comm` ou similar, ou enviar um sinal de teste 0 antes de matar).
- O timeout de espera para a liberação do lock deve ser curto (no máximo 1.5 a 2 segundos) para não travar a inicialização da nova barra.

## Critério de aceitação
- O projeto compila sem alertas em `bar-main.c`.
- Ao rodar uma nova instância de `maindeck-bar`, a instância anterior em execução deve receber `SIGTERM`, encerrar seu loop e liberar o lock, permitindo que a nova assuma o controle sem duplicação de interface.
- Se nenhuma instância estiver rodando, a nova barra inicia instantaneamente (sem timeouts artificiais).

## Aberto (T2: peça ao judge para decidir)
1. Qual a melhor estratégia para garantir que o PID lido do lockfile pertence de fato à instância anterior de `maindeck-bar` (e não a um processo aleatório que herdou o PID)?
2. Como projetar o loop de espera ativa e timeout no processo novo para que ele aguarde a liberação do lock sem consumir CPU excessiva?
3. Onde em `bar-main.c` devemos inserir a chamada de inicialização do lock para que o takeover ocorra o mais cedo possível antes de inicializar recursos pesados do Wayland?
