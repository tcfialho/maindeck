# Otimizações Extremas Aplicadas (PGO + BOLT + Mimalloc + LTO)

Este documento registra as otimizações avançadas implementadas em junho de 2026 nos binários do `maindeck` (`maindeck-wm` e `maindeck-bar`), descrevendo as flags de compilação, o fluxo de coleta de perfis e os resultados obtidos.

---

## 1. Configurações de Build (`meson.build`)

O sistema de build do Meson foi atualizado para forçar flags agressivas de otimização de CPU e linkagem segura.

### Flags de Compilação (CFLAGS)
* `-O3 -march=native -mtune=native`: Habilita otimizações agressivas e vetorização nativa para o microprocessador host (Alder Lake/Raptor Lake, incluindo AVX2, BMI2 e FMA).
* `-ffast-math -fno-math-errno`: Desabilita a checagem de errno matemático do padrão C, permitindo vetorização agressiva de loops de ponto flutuante.
* `-fno-semantic-interposition`: Melhora a inlining e otimização entre chamadas de função locais em PIE.
* `-falign-functions=32`: Alinha o início das funções em limites de cacheline para otimizar o carregamento no Instruction Cache (I-Cache).
* `-fno-omit-frame-pointer`: Mantido durante as fases de perfilamento para permitir amostragem correta de stack traces.

### Flags de Linkagem (LDFLAGS) & Alocação
* `-lmimalloc`: Linkagem direta com o alocador de memória lock-free segmentado da Microsoft (`mimalloc`), eliminando fragmentação e overhead do ptmalloc padrão da glibc.
* `b_lto=true`: Link-Time Optimization (LTO) ativo para otimização entre arquivos de código fonte.
* `-Wl,--emit-relocs`: Emissão de informações de realocação no ELF para viabilizar a otimização pós-link do BOLT.

---

## 2. Fluxo de Compilação Guiada por Perfil (PGO)

A otimização PGO foi realizada em três etapas para capturar o comportamento em tempo de execução sem interromper a sessão gráfica.

1. **Geração de Perfil (Instrumentação):**
   ```bash
   meson setup build-gen --buildtype=release -Db_lto=true -Db_pgo=generate -Dc_args="-fno-omit-frame-pointer"
   ninja -C build-gen install
   ```
2. **Coleta de Dados Dinâmica (Dumping via GDB):**
   Durante a sessão gráfica ativa executando os binários instrumentados, a coleta de cobertura de código `.gcda` foi persistida sem reiniciar o computador, chamando a rotina interna `__gcov_exit` via GDB no processo ativo:
   ```bash
   # Extrair o endereço de __gcov_exit e forçar o dump de memória
   gdb -p <PID> -ex "call (void)__gcov_exit()" -ex "detach" -ex "quit"
   ```
3. **Compilação Otimizada (Uso de Perfil):**
   ```bash
   meson setup build-use --buildtype=release -Db_lto=true -Db_pgo=use
   ninja -C build-use install
   ```

---

## 3. Otimização Pós-Link (llvm-bolt)

A otimização BOLT foi aplicada nos binários estáveis obtidos a partir do PGO-use para reordenar blocos básicos e funções quentes com base em telemetria de hardware (LBR).

1. **Coleta de Amostras LBR (5 Minutos):**
   ```bash
   perf record -e cycles:u -j any,u -F 4000 -o perf_wm.data -p <PID_WM> -- sleep 300
   perf record -e cycles:u -j any,u -F 4000 -o perf_bar.data -p <PID_BAR> -- sleep 300
   ```
2. **Conversão de Perfil (Lite Mode):**
   Devido a realocações complexas em seções frias dos binários, o modo estrito foi desabilitado (`--strict=false`) para ativar o **Lite Mode**, focando a reorganização física apenas nos caminhos de código com telemetria ativa:
   ```bash
   perf2bolt --strict=false -p perf_wm.data -o maindeck-wm.fdata ~/.local/bin/maindeck-wm
   perf2bolt --strict=false -p perf_bar.data -o maindeck-bar.fdata ~/.local/bin/maindeck-bar
   ```
3. **Otimização de Layout:**
   ```bash
   llvm-bolt ~/.local/bin/maindeck-wm -o maindeck-wm.bolt -data=maindeck-wm.fdata -reorder-blocks=ext-tsp -reorder-functions=cdsort -split-functions -split-all-cold -dyno-stats --lite
   llvm-bolt ~/.local/bin/maindeck-bar -o maindeck-bar.bolt -data=maindeck-bar.fdata -reorder-blocks=ext-tsp -reorder-functions=cdsort -split-functions -split-all-cold -dyno-stats --lite
   ```

---

## 4. Resultados Obtidos (Estatísticas Dinâmicas do BOLT)

As otimizações de layout geraram as seguintes reduções de saltos e instruções executadas (baseado no dyno-stats do llvm-bolt):

### maindeck-wm (Gerenciador de Janelas)
* **Saltos condicionais tomados**: Redução de **61.7%** (de 682 para 261).
* **Saltos incondicionais executados**: Redução de **11.8%**.
* **Total de saltos executados**: Redução de **53.0%**.
* **Instruções totais executadas**: Redução de **1.1%**.
* **Layout físico**: 10 funções quentes otimizadas; 6 KB de código frio movidos para fora do hot-path.

### maindeck-bar (Barra Nativa)
* **Saltos condicionais tomados**: Redução de **42.5%** (de 398 para 229).
* **Saltos incondicionais executados**: Redução de **34.0%**.
* **Total de saltos executados**: Redução de **41.6%**.
* **Layout físico**: 13 funções quentes otimizadas; 10 KB de código frio movidos para fora do hot-path.
