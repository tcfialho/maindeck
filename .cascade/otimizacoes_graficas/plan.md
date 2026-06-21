# PLANO DE IMPLEMENTAÇÃO: Otimização pilha Wayland (wlroots, river, maindeck)

## 1. Arquivos e Diretórios Alvo
- [wlroots](file:///home/tcfialho/Documents/poc/references/wlroots-0.20.1): `meson.build`, `tinywl/tinywl.c` (ponto de término seguro para PGO)
- [river](file:///home/tcfialho/Documents/poc/references/river): `build.zig` (injeção de flags de relocação/linkagem)
- [maindeck-wm](file:///home/tcfialho/Documents/poc/maindeck-wm): `meson.build`, scripts de inicialização de sessão do usuário

---

## 2. Passo-a-Passo e Ordem de Execução

### PASSO 0: Configuração de Variáveis Globais de Otimização
```bash
export CFLAGS="-O3 -march=x86-64-v3 -mtune=native -fno-math-errno -fno-strict-aliasing -fno-semantic-interposition"
export CXXFLAGS="$CFLAGS"
export LDFLAGS="-Wl,-z,now -Wl,-z,relro -fuse-ld=lld"
# OBS: -fno-omit-frame-pointer será adicionado especificamente nas fases de coleta (PGO-gen e BOLT)
```

### PASSO 1: Preparação do Sistema (Injeção do mimalloc)
1. Confirmar presença da biblioteca: `ldconfig -p | grep mimalloc`
2. Criar script wrapper em `/home/tcfialho/opt/river-run` para pré-carregamento seguro:
   ```bash
   #!/bin/sh
   export LD_PRELOAD="/usr/lib/libmimalloc.so.2"
   exec /home/tcfialho/opt/wlroots-opt/bin/river "$@"
   ```

### PASSO 2: Otimização PGO + BOLT do wlroots
1. **Instrumentação (PGO-generate):**
   ```bash
   cd /home/tcfialho/Documents/poc/references/wlroots-0.20.1
   # Modificar tinywl.c para chamar wl_display_terminate(server.wl_display) via timer de 5s antes de compilar
   rm -rf build-gen && meson setup build-gen --prefix=/home/tcfialho/opt/wlroots-opt \
     --buildtype=release -Db_lto=true -Db_pgo=generate -Dwerror=false -Dc_args="-fno-omit-frame-pointer"
   ninja -C build-gen install
   ```
2. **Coleta de Perfil GCOV:**
   - Executar `./build-gen/tinywl/tinywl` headless. Os arquivos `.gcda` serão gerados automaticamente no diretório `build-gen/` (já que o compilador grava nos paths absolutos de build incorporados no binário).
3. **Compilação Otimizada (PGO-use) + Relocations para BOLT:**
   ```bash
   meson setup build-use --prefix=/home/tcfialho/opt/wlroots-opt --buildtype=release \
     -Db_lto=true -Db_pgo=use -Dwerror=false -Dc_link_args="-Wl,--emit-relocs"
   ninja -C build-use install
   ```
4. **Otimização Pós-Link (BOLT) na libwlroots:**
   ```bash
   perf record -e cycles:u -j any,u -o perf.data -- /home/tcfialho/opt/wlroots-opt/bin/tinywl
   perf2bolt -p perf.data -o wlroots.fdata /home/tcfialho/opt/wlroots-opt/lib/libwlroots-0.20.so
   llvm-bolt /home/tcfialho/opt/wlroots-opt/lib/libwlroots-0.20.so -o /home/tcfialho/opt/wlroots-opt/lib/libwlroots-0.20.so.bolt \
     -data=wlroots.fdata -reorder-blocks=ext-tsp -reorder-functions=hfsort+ -split-functions -split-all-cold -dyno-stats
   mv /home/tcfialho/opt/wlroots-opt/lib/libwlroots-0.20.so.bolt /home/tcfialho/opt/wlroots-opt/lib/libwlroots-0.20.so
   ```

### PASSO 3: Otimização do river (Zig + BOLT)
1. **Ajuste no `build.zig`**: Injetar `-Wl,--emit-relocs` e desabilitar strip de símbolos para permitir instrumentação do BOLT:
   ```zig
   // No build.zig do river, garantir que ao compilar o executável:
   exe.link_gc_sections = false; // Preserva seções para o BOLT
   exe.addObjectFile(...); // se houver dependências estáticas
   // Adicionar flag de relocação para o linker do sistema
   exe.linkopt("-Wl,--emit-relocs");
   ```
2. **Compilação**:
   ```bash
   cd /home/tcfialho/Documents/poc/references/river
   export PKG_CONFIG_PATH=/home/tcfialho/opt/wlroots-opt/lib/pkgconfig
   zig build -Doptimize=ReleaseFast -Dcpu=native --prefix /home/tcfialho/opt/wlroots-opt
   ```
3. **Otimização Pós-Link (BOLT)**:
   ```bash
   perf record -e cycles:u -j any,u -o perf.data -- /home/tcfialho/opt/wlroots-opt/bin/river --help
   perf2bolt -p perf.data -o river.fdata /home/tcfialho/opt/wlroots-opt/bin/river
   llvm-bolt /home/tcfialho/opt/wlroots-opt/bin/river -o /home/tcfialho/opt/wlroots-opt/bin/river.bolt \
     -data=river.fdata -reorder-blocks=ext-tsp -reorder-functions=hfsort+ -split-functions -dyno-stats
   mv /home/tcfialho/opt/wlroots-opt/bin/river.bolt /home/tcfialho/opt/wlroots-opt/bin/river
   ```

### PASSO 4: Otimização do maindeck-wm (PGO)
1. **Instrumentação**:
   ```bash
   cd /home/tcfialho/Documents/poc/maindeck-wm
   rm -rf build-gen && meson setup build-gen --prefix=/home/tcfialho/opt/maindeck-opt \
     --buildtype=release -Db_lto=true -Db_pgo=generate -Dc_args="-fno-omit-frame-pointer"
   ninja -C build-gen install
   ```
2. **Coleta de Perfil**:
   - Iniciar o `river` (headless) em background.
   - Executar o `maindeck-wm` apontando para a sessão headless por 10s e terminar com `SIGTERM` para persistir os `.gcda`.
3. **Compilação Otimizada**:
   ```bash
   meson setup build-use --prefix=/home/tcfialho/opt/maindeck-opt --buildtype=release -Db_lto=true -Db_pgo=use
   ninja -C build-use install
   ```

---

## 3. Critérios de Aceitação (Portões de Validação)
- **Integridade Estrutural**: Todos os binários/libs modificados por BOLT devem passar em `ldd <file>` e `readelf -h <file>` sem erros de cabeçalho ELF.
- **Estabilidade Mínima**: Execução contínua da sessão gráfica river/maindeck por pelo menos 5 minutos sem crashes.
- **Desempenho (Métricas A/B)**:
  - Latência média medida via `vkmark` não deve regredir em comparação à build padrão do sistema.
  - O overhead de CPU do compositor (`river`) monitorado via `pidstat -u 1` sob carga de movimentação de janelas deve ser igual ou inferior à versão não otimizada.

---

## 4. Riscos Críticos e Mitigação
- **Risco**: Falta de suporte a LBR (Last Branch Record) no kernel ou processador impedindo a execução de `perf record -j any`.
  - *Mitigação*: Caso falhe, usar o fallback de amostragem por clock do BOLT: `perf record -e cycles:u -o perf.data -- ...` (sem a flag `-j any`), e rodar o `perf2bolt` com a flag `-w` (profile baseado em amostragem básica).
- **Risco**: Corrupção do binário Zig pelo `llvm-bolt` devido à formatação não padrão de seções de debug do compilador Zig.
  - *Mitigação*: Executar verificação estrita de funcionamento (`/bin/river --help`) imediatamente após o BOLT; abortar substituição e reverter para a build estável pré-bolt se houver SIGSEGV.