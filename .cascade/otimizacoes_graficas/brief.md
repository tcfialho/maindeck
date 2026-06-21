# Brief: Otimizações Gráficas de Sistema (wlroots, river, maindeck)

## Roteamento
T2 — Otimização global de sistema de baixo nível que impacta o motor gráfico (wlroots), o compositor de janelas (river) e utilitários de interface (maindeck). Toca múltiplos repositórios/componentes cruciais da pilha Wayland e introduz potencial instabilidade crítica.

## Objetivo
Criar um plano de implementação robusto, sequencial e validado pelo Judge para aplicar otimizações nativas extremas (C/C++ compiler CFLAGS/LDFLAGS, LTO, PGO, BOLT e alocador mimalloc) no `wlroots`, `river` e `maindeck`, garantindo que:
1. O desempenho do sistema (estabilidade de frametime e latência) seja maximizado.
2. O risco de crashs fatais de sessão ou renderização incorreta seja mitigado.
3. Métodos objetivos de medição/feedback de performance (vkmark, MangoHud, perf, pidstat) sejam empregados entre as etapas.

## Contexto da base
- **Sistema operacional**: Linux host nativo (CachyOS) em processador i7-13620H (arquitetura x86-64-v3 com AVX2, FMA e BMI2, Raptor Lake) com kernel xanmod.
- **Componente 1 (wlroots)**: `/home/tcfialho/Documents/poc/references/wlroots-0.20.1`
- **Componente 2 (river)**: `/home/tcfialho/Documents/poc/references/river`
- **Componente 3 (maindeck)**: `/home/tcfialho/Documents/poc/maindeck-wm`
- **Prefixos de Instalação Local (Isolados)**:
  - Wlroots & River: `/home/tcfialho/opt/wlroots-opt`
  - Maindeck: `/home/tcfialho/opt/maindeck-opt`

## Flags de Compilação Globais Propostas
```bash
# Flags finais de produção (PGO-use + LTO)
export CFLAGS="-O3 -march=native -mtune=native -fno-math-errno -fno-strict-aliasing -fno-semantic-interposition"
export CXXFLAGS="$CFLAGS"
export LDFLAGS="-Wl,-z,now -Wl,-z,relro -Wl,--emit-relocs -fuse-ld=lld"
```

## Restrições e invariantes de Segurança
- **NÃO usar `-ffast-math` (ou `-Ofast`)**: Quebra conformidade IEEE 754 e corrompe matrizes de projeção do OpenGL/Vulkan, causando crashes ou falhas visuais.
- **NÃO usar `-fstrict-aliasing`**: Projetos de baixo nível como `wlroots` usam type punning e conversão forçada de structs para eventos e KMS do kernel, o que seria corrompido pelas premissas de aliasing estrito do compilador. Deve-se manter `-fno-strict-aliasing`.
- **NÃO usar `-fomit-frame-pointer` na fase de perfil (PGO/BOLT)**: Pois cega o `perf record` para stack unwinding.

---

## Proposta de Plano de Implementação por Ferramenta

### 1. Injeção de Runtime (mimalloc)
- **Instalação**: Verificar que `/usr/lib/libmimalloc.so` ou `/usr/lib/libmimalloc.so.2` existe no sistema.
- **Ativação**: Configurar variáveis no wrapper de inicialização da sessão Wayland (ex: script de inicialização do river):
  ```bash
  export LD_PRELOAD="/usr/lib/libmimalloc.so"
  ```
- **Validação**: Testar com e sem para garantir que não há regressão ou falhas de heap em aplicações dinamicamente linkadas.

### 2. Otimização do wlroots
- **Passo A: Build Instrumentado (PGO-generate)**
  ```bash
  cd /home/tcfialho/Documents/poc/references/wlroots-0.20.1
  # Limpar build anterior
  rm -rf build-gen
  # Configurar com PGO generate e LTO. -Dwerror=false evita falhas por warnings de perfil incompleto
  CFLAGS="$CFLAGS -fno-omit-frame-pointer -g" LDFLAGS="$LDFLAGS" meson setup build-gen \
    --prefix=/home/tcfialho/opt/wlroots-opt --buildtype=release -Db_lto=true -Db_pgo=generate -Dwerror=false
  ninja -C build-gen
  ninja -C build-gen install
  ```
- **Passo B: Treinamento e Coleta de Perfil**
  - Modificar `tinywl` (exemplo simples em C incluído no wlroots) para encerrar de forma limpa após 5 segundos usando uma thread temporizadora que chama `wl_display_terminate(display)`.
  - Executar o `tinywl` instrumentado sob headless backend para gerar os arquivos `.gcda`:
    ```bash
    WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 GCOV_PREFIX=/home/tcfialho/Documents/poc/references/wlroots-0.20.1/build-gen/gcov_prefix ./build-gen/tinywl/tinywl
    ```
  - Copiar os arquivos `.gcda` gerados de volta para a estrutura de build-gen:
    ```bash
    cp -r build-gen/gcov_prefix/home/tcfialho/Documents/poc/references/wlroots-0.20.1/build-gen/* build-gen/
    ```
  - Verificar que os arquivos `.gcda` foram criados: `find build-gen -name '*.gcda'`
- **Passo C: Build Final Otimizado (PGO-use)**
  ```bash
  meson setup build-use --prefix=/home/tcfialho/opt/wlroots-opt --buildtype=release \
    -Db_lto=true -Db_pgo=use -Dwerror=false
  ninja -C build-use
  ninja -C build-use install
  ```
- **Passo D: Otimização Pós-Link (BOLT)**
  - Coletar dados LBR usando `perf` no binário compilado com relocações:
    ```bash
    perf record -e cycles:u -j any,u -o perf.data -- /home/tcfialho/opt/wlroots-opt/bin/tinywl
    ```
  - Converter o perfil:
    ```bash
    perf2bolt -p perf.data -o wlroots.fdata /home/tcfialho/opt/wlroots-opt/lib/libwlroots-0.20.so
    ```
  - Aplicar otimizações de layout assembly:
    ```bash
    llvm-bolt /home/tcfialho/opt/wlroots-opt/lib/libwlroots-0.20.so -o /home/tcfialho/opt/wlroots-opt/lib/libwlroots-0.20.so.bolt \
      -data=wlroots.fdata -reorder-blocks=ext-tsp -reorder-functions=hfsort+ -split-functions \
      -split-all-cold -split-eh -dyno-stats -use-gnu-stack
    ```
  - Substituir a lib `.so` original pela versão `.bolt` após conferir integridade estrutural (`ldd` e `readelf`).

### 3. Otimização do river
- `river` é compilado via Zig (`build.zig`). Não aceita flags de perfil do Meson, e seu tempo de execução no desligamento pode desviar de destrutores glibc.
- **Passo A: Compilação Nativa Otimizada**
  - Apontar o PKG_CONFIG para usar o wlroots recém-otimizado em `/home/tcfialho/opt/wlroots-opt`:
    ```bash
    export PKG_CONFIG_PATH=/home/tcfialho/opt/wlroots-opt/lib/pkgconfig
    export LD_LIBRARY_PATH=/home/tcfialho/opt/wlroots-opt/lib
    ```
  - Compilar o river nativamente com otimização agressiva e relocações habilitadas para o BOLT:
    ```bash
    cd /home/tcfialho/Documents/poc/references/river
    zig build -Doptimize=ReleaseFast -Dcpu=native --prefix /home/tcfialho/opt/wlroots-opt
    ```
- **Passo B: Otimização Pós-Link (BOLT)**
  - Coletar perfil LBR durante a execução headless do river:
    ```bash
    perf record -e cycles:u -j any,u -o perf.data -- /home/tcfialho/opt/wlroots-opt/bin/river
    ```
  - Executar `perf2bolt` e `llvm-bolt` no executável `/home/tcfialho/opt/wlroots-opt/bin/river`.
  - Substituir o binário original pelo binário `.bolt` otimizado.

### 4. Otimização do maindeck-wm
- **Passo A: Build Instrumentado (PGO-generate)**
  ```bash
  cd /home/tcfialho/Documents/poc/maindeck-wm
  rm -rf build-gen
  # Certificar que aponta para o wlroots otimizado
  export PKG_CONFIG_PATH=/home/tcfialho/opt/wlroots-opt/lib/pkgconfig
  export LD_LIBRARY_PATH=/home/tcfialho/opt/wlroots-opt/lib
  CFLAGS="$CFLAGS -fno-omit-frame-pointer -g" LDFLAGS="$LDFLAGS" meson setup build-gen \
    --prefix=/home/tcfialho/opt/maindeck-opt --buildtype=release -Db_lto=true -Db_pgo=generate
  ninja -C build-gen
  ninja -C build-gen install
  ```
- **Passo B: Treinamento e Coleta de Perfil**
  - Iniciar a sessão do `river` headless em background:
    ```bash
    WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 /home/tcfialho/opt/wlroots-opt/bin/river &
    RIVER_PID=$!
    sleep 1
    ```
  - Executar o `maindeck-wm` e `maindeck-bar` instrumentados apontando para o display Wayland headless por ~10 segundos, simulando interações e em seguida parando-os com `SIGTERM` para que gravem seus perfis `.gcda`:
    ```bash
    WAYLAND_DISPLAY=wayland-0 GCOV_PREFIX=/home/tcfialho/Documents/poc/maindeck-wm/build-gen/gcov_prefix /home/tcfialho/opt/maindeck-opt/bin/maindeck-wm &
    WM_PID=$!
    sleep 5
    kill -SIGTERM $WM_PID
    kill -SIGTERM $RIVER_PID
    ```
  - Mover arquivos `.gcda` para a raiz do build-gen:
    ```bash
    cp -r build-gen/gcov_prefix/home/tcfialho/Documents/poc/maindeck-wm/build-gen/* build-gen/
    ```
- **Passo C: Build Final Otimizado (PGO-use)**
  ```bash
  meson setup build-use --prefix=/home/tcfialho/opt/maindeck-opt --buildtype=release \
    -Db_lto=true -Db_pgo=use
  ninja -C build-use
  ninja -C build-use install
  ```

### 5. Métricas de Validação A/B e Regressão
- A cada etapa de otimização de componente, realizar as seguintes medições:
  - **FPS/Frametimes**: `MangoHud` + `vkmark` na sessão ativa para medir a latência média e 1%/0.1% lows.
  - **Uso de CPU/Stalls**: `perf stat` monitorando o processo do river e do maindeck-wm.
  - **Verificações de Regressão**: Verificar matrizes de renderização (cores, escalas das janelas), damage tracking (sem ghosting), e latência de entrada (teclado/mouse).

---

## Aberto (Pontos para o Judge decidir/validar)
1. **Validação Geral**: O Judge concorda com a sequência proposta, os comandos exatos de setup/compilação e as flags passadas?
2. **Objeções ou Melhorias**: Existem brechas que possam impedir a geração dos perfis `.gcda` no wlroots/maindeck, ou falhas no procedimento do BOLT?
3. **Produção do Plano Consolidado**: Caso concorde (ou após aplicar as correções das objeções), produza o plano de implementação final detalhado.
