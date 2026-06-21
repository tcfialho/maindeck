# Guia: otimização extrema (native + PGO + BOLT + mimalloc + LTO) — aplicar em river e maindeck

> **Para outra IA executar no futuro.** Receita destilada da sessão que otimizou o Proton (CP2077) em 2026-06-15.
> Mesma técnica vale para **river** (compositor Wayland) e **maindeck** (WM/barra) — que são binários ELF nativos do usuário, compilados localmente, e (diferente do Proton) **rodam direto no host, sem container** → muito mais simples.
>
> Ambiente: i7-13620H Raptor Lake (x86-64-v3, AVX2+BMI2, SEM AVX-512), RTX 4050, CachyOS, kernel xanmod.
> Fontes: river em `~/Documents/poc/references/` (ver memória), maindeck em `~/Documents/poc/maindeck-wm/`.

## Ordem de ROI (fazer nesta sequência)

### 1. `-march=native -O3 -mtune=native` (base — sempre, todas as flags)
- No `meson`/`cmake`/`Makefile` do projeto: garantir que **TODAS** as flags tenham `-O3 -march=native -mtune=native`, **incluindo flags de host/tooling** (não só CFLAGS principal).
- river usa **meson**: `meson setup build -Dc_args='-O3 -march=native -mtune=native' -Dcpp_args=... --buildtype=release -Db_lto=true`.
- maindeck: ver o build dele (provável meson ou Makefile direto — checar `~/Documents/poc/maindeck-wm`).
- **Verificação (gate):** `objdump -d <binário> | grep -cE 'mulx|shlx|shrx|rorx|andn|pdep|pext'` deve subir vs build genérico. Se não subir, a flag não chegou no binário.

### 2. LTO (`-flto` / `-Db_lto=true`)
- river/maindeck são código próprio (não cross-mingw como o DXVK) → **LTO funciona direto**, diferente do Proton onde o vkd3d tem `-fno-lto` hardcoded.
- meson: `-Db_lto=true`. Makefile: `-flto` no CFLAGS **e** LDFLAGS.
- **Risco baixo** para projetos C pequenos. Se quebrar no link, tirar.

### 3. mimalloc (allocator rápido) — quase grátis
- `pacman -S mimalloc` (já instalado no sistema, `/usr/lib/libmimalloc.so`).
- **Sem recompilar:** rodar o binário com `LD_PRELOAD=/usr/lib/libmimalloc.so`. Para river/maindeck (rodam no host, sem container) é trivial — pôr no script de launch / desktop file / wrapper.
- **Com recompilar (melhor):** linkar `-lmimalloc` no projeto.
- Ganho: alocação mais rápida em código thread-pesado / com muito malloc (compositor faz muito).

### 4. PGO (Profile-Guided Optimization) — o mais trabalhoso, melhor ganho de CPU
PGO é 3 fases. **A armadilha (resolvida na marra) é ONDE instrumentar** — instrumentar global pega as build-tools e quebra o link (sem libgcov). Lição:
- **Instrumentar SÓ os binários finais, não as ferramentas de build.** Achar a variável de CFLAGS do *binário alvo* (não a global). No wine era `UNIX_CFLAGS`/`UNIX_LIBS` por-subdir; em meson é mais fácil (instrumenta o target).
- **Fase 1 (generate):** compilar com `-fprofile-generate -fprofile-dir=<abs> -fprofile-update=atomic` no CFLAGS **E** `-fprofile-generate` no LDFLAGS (senão o link não acha `__gcov_*`).
- **Fase 2 (coleta):** **USAR o programa de verdade** — para river: usar o desktop normalmente 10-15min (abrir janelas, mover, trocar workspace, jogar). Para maindeck: idem (barra, menu, tray, taskbar). Os `.gcda` aparecem no `-fprofile-dir`.
- **Fase 3 (use):** recompilar trocando `-fprofile-generate`→`-fprofile-use -fprofile-correction -Wno-coverage-mismatch -Wno-missing-profile`.
- **meson tem suporte nativo a PGO:** `-Db_pgo=generate` → rodar → `-Db_pgo=use`. **MUITO mais fácil que o wine.** Usar isso para river/maindeck.
- **Gate:** binário final muda (MD5 ≠), fica menor (código frio sai do hot path), e tem `0` símbolos gcov (`strings bin | grep -c gcda` = 0 no build `use`).

### 5. BOLT (post-link layout optimization) — complementa PGO
- `pacman -S llvm-bolt` (pacote `cachyos/llvm-bolt`, NÃO o `bolt` que é Thunderbolt).
- **Requer:** binário linkado com `-Wl,--emit-relocs` (adicionar ao LDFLAGS do build). CPU precisa LBR (Raptor Lake tem).
- **Fase coleta:** `perf record -e cycles:u -j any,u -F 4000 -o perf.data -p <PID> -- sleep 300` enquanto usa o programa. **Para river/maindeck: capturar o PRÓPRIO processo** (não system-wide, que dilui). river/maindeck SÃO CPU-ativos (compositor renderiza todo frame) → vão gerar samples fartos, diferente do wineserver que era ocioso.
- **Otimizar:** `perf2bolt -p perf.data -o app.fdata <binário>` depois `llvm-bolt <binário> -o <binário>.bolt -data=app.fdata -reorder-blocks=ext-tsp -reorder-functions=hfsort -split-functions -split-all-cold -split-eh -dyno-stats`.
- Trocar o binário pelo `.bolt`. Gate: tem seção `.bolt.org.text` (`readelf -S`).

## ⚠️ Diferença CRÍTICA river/maindeck vs Proton (a favor!)
O Proton foi difícil por **3 motivos que NÃO se aplicam** a river/maindeck:
1. **Container SLR/bubblewrap** — Proton roda em container Ubuntu; libs custom precisam ir pra dentro e o pressure-vessel escolhe a glibc. **river/maindeck rodam direto no host** → sem essa dor.
2. **Cross-mingw (PE)** — DXVK/vkd3d são binários Windows; BOLT/PGO nativo não tocam. **river/maindeck são 100% ELF nativo** → PGO/BOLT funcionam em tudo.
3. **build-tools instrumentadas** — o wine compila makedep etc. **meson isola melhor** → menos armadilha.

→ **Em river/maindeck dá pra ir MAIS LONGE que no Proton**: native + LTO + mimalloc + PGO + BOLT, **tudo em todos os binários**, via meson (`-Db_pgo`, `-Db_lto`) que automatiza o que no wine teve que ser feito na mão.

## Medição A/B (obrigatória — não confiar em "deve melhorar")
- river: medir CPU% do compositor (era o problema — ver memória `project_river_core_optimization_plan`), latência de frame, e o benchmark que o usuário já usa.
- maindeck: tempo de resposta da barra/menu, CPU idle.
- **1 run não basta** para métricas ruidosas (lows/latência variam). Rodar 3x cada variante, comparar média; ganho tem que superar o ruído run-a-run (~3%).
- Manter mesmo com empate SE não houver regressão (preferência do usuário); reverter só se piorar.

## Resultado no Proton (referência do que esperar)
Tudo aplicado (native+mimalloc+PGO+BOLT+glibc native): **avg empatou** (155, era GPU-bound), mas **1% low subiu 82→101 e 0.1% low 53→66** num run — ganho de **estabilidade de frametime** (onde CPU/allocator/layout importam). river/maindeck NÃO são GPU-bound da mesma forma → o avg/CPU pode mover mais.

Relacionado: plano fonte completo do Proton em `~/Documents/poc/proton-supremo/PLANO-OTIMIZACAO-FONTE.md`. Planos river/maindeck em `~/.claude/plans/{river,maindeck}-otimizacoes.md`.
