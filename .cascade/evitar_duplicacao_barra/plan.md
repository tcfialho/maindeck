# Plano de Implementação: evitar_duplicacao_barra

## Decisões do Judge (perguntas em Aberto)

1. **Verificação de PID**: combinar advisory lock + checagem ativa. O `flock(LOCK_EX|LOCK_NB)` é a fonte da verdade sobre "há instância viva" (o lock some sozinho se o dono morre). O PID gravado serve só para *direcionar o SIGTERM*. Antes de matar: (a) `kill(pid,0)` para existência; (b) ler `/proc/<pid>/comm` e comparar com `"maindeck-bar"` (basename do `comm`, truncado a 15 chars pelo kernel — comparar com prefixo). Só envia SIGTERM se ambos passarem. Isso elimina falso positivo de PID reciclado.
2. **Loop de espera**: nada de busy-wait. Re-tentar `flock(LOCK_EX|LOCK_NB)` em loop com `nanosleep(50ms)` entre tentativas, teto de **40 iterações ≈ 2s**. CPU desprezível. Se estourar o teto, prosseguir mesmo assim (degradar, não travar) — log de warning.
3. **Ponto de inserção**: chamar `singleton_acquire()` **logo no início de `main()`**, antes de qualquer init Wayland/IPC/GTK. Manter o fd aberto pela vida do processo (não fechar/`flock(LOCK_UN)`); o OS libera no `exit`/crash.

## Arquivos a tocar
- `bar-main.c` — nova função estática + chamada em `main()`.
- (sem novos arquivos; sem mudança no script `init`.)

## Contrato
```c
// retorna fd do lock (>=0) que deve permanecer aberto; -1 só em erro fatal de ambiente
static int singleton_acquire(void);
```
- Caminho do lock: `"%s/maindeck-bar.lock"` com `getenv("XDG_RUNTIME_DIR")`. Se `XDG_RUNTIME_DIR` ausente → log warning e `return -1` (segue sem singleton, não aborta).
- Nome de processo esperado: `static const char *PROC_COMM = "maindeck-bar";`

## Passos

1. **Includes** (topo de `bar-main.c`, se faltarem): `<sys/file.h>` (flock), `<fcntl.h>`, `<unistd.h>`, `<signal.h>`, `<string.h>`, `<errno.h>`, `<time.h>`, `<stdio.h>`, `<stdlib.h>`.

2. **Implementar `singleton_acquire()`**:
   - Montar `path` com `snprintf` a partir de `XDG_RUNTIME_DIR`; se NULL → warning + `return -1`.
   - `int fd = open(path, O_RDWR|O_CREAT|O_CLOEXEC, 0600);` (sem `O_CLOEXEC`? usar **sem** O_CLOEXEC só se o fd precisar sobreviver a exec — aqui NÃO há exec, então **usar O_CLOEXEC**). Erro de `open` → warning + `return -1`.
   - `if (flock(fd, LOCK_EX|LOCK_NB) == 0) { goto own; }` (caminho rápido: nenhuma instância → sem timeout).
   - Se `errno == EWOULDBLOCK`: instância anterior viva.
     - Ler conteúdo atual do arquivo (`pread`/`read` no início) → parse `pid_t old` via `strtol`. 
     - `try_terminate(old)`: se `old>0 && kill(old,0)==0` e `comm_matches(old)` → `kill(old, SIGTERM)`. (Não usar SIGKILL — invariante de término gracioso.)
     - Loop: `for (int i=0;i<40;i++){ if(flock(fd,LOCK_EX|LOCK_NB)==0) goto own; nanosleep(&(struct timespec){0,50*1000*1000},NULL); }`
     - Estourou: log `"warning: timeout aguardando lock; prosseguindo"`, e `goto own` (assume mesmo sem garantia — evita travar init).
   - Outro `errno` em flock → warning, `close(fd)`, `return -1`.
   - `own:` gravar PID próprio: `ftruncate(fd,0); char buf[32]; int n=snprintf(buf,sizeof buf,"%ld\n",(long)getpid()); pwrite(fd,buf,n,0);` (ignorar erro de escrita com log). `return fd;`

3. **`comm_matches(pid_t pid)`** (helper estático): abrir `"/proc/%d/comm"`, ler 1ª linha, `strcspn(line,"\n")=0`; comparar com `PROC_COMM`. Como `comm` é truncado a 15 chars, comparar via `strncmp(line, PROC_COMM, 15)` ou igualdade direta se nome <15. Falha de open/read → `return 0` (não mata).

4. **Chamar em `main()`**: primeira coisa após parse de args/log init e **antes** de conectar Wayland:
   ```c
   int lock_fd = singleton_acquire();
   (void)lock_fd; // manter aberto até o fim do processo
   ```
   Não fechar explicitamente; opcionalmente guardar em `static int g_lock_fd` se houver shutdown ordenado, mas não é necessário.

5. **Garantir manejo de SIGTERM na barra**: confirmar que o loop principal já trata SIGTERM e sai limpo (encerra event loop, libera Wayland). Se não houver handler, instalar `signal(SIGTERM, ...)` que seta flag de saída do loop — caso contrário o default já termina o processo (aceitável, pois o OS libera o flock). **Verificar antes de adicionar** para não duplicar handler existente.

## Critérios de aceitação (verificáveis)
- `ninja -C build` compila `bar-main.c` **sem warnings** (cuidar de retornos ignorados de `pwrite/ftruncate` → cast `(void)` ou checagem).
- 2ª instância: a 1ª recebe SIGTERM, sai, libera flock; a 2ª adquire e assume sem barra duplicada.
- Sem instância prévia: `flock` LOCK_NB sucede na 1ª tentativa → **zero sleep/timeout**.
- 1ª instância morta/crashada: lock já livre → 2ª inicia imediato; PID órfão no arquivo é ignorado pela checagem `comm_matches`.

## Riscos
- **comm truncado a 15 chars**: `maindeck-bar` tem 12 — ok; usar `strncmp(...,15)` por segurança.
- **PID reciclado para outro `maindeck-bar`**: improvável; o flock é a real garantia, SIGTERM é best-effort.
- **fd O_CLOEXEC vs herança**: a barra não faz `exec`, então O_CLOEXEC é correto e evita vazar lock para subprocessos.
- **Warnings de compilação** por retornos ignorados de syscalls (`-Wunused-result`): silenciar com cast explícito.
- **Race entre ler PID e flock**: aceitável — PID só direciona o sinal; a posse vem do flock atômico.