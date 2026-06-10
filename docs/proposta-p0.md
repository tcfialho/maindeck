# Proposta de Otimização P0 — maindeck (wm + bar + menu)

Este documento detalha as propostas de implementação técnica para a otimização dos itens de nível **P0** (Latência Interativa), focando em eficiência máxima de CPU e tempo de resposta O(1).

---

## 1. P0.1 — Otimização de Resolução de Ícones

### Problema
No estado atual ([bar-icons.c](file:///home/tcfialho/Documents/poc/maindeck-wm/bar-icons.c)), o resolvedor de ícones possui um cache linear simples (`g_cache`) limitado a 64 posições estáticas. Quando ocorrem mais de 64 ícones (ou buscas por ícones inexistentes), o cache satura e o resolvedor é forçado a fazer varreduras recursivas em disco (`scan_dir_recursive`) e dezenas de `access()` a cada render/tecla pressionada no menu ([maindeck-menu.c](file:///home/tcfialho/Documents/poc/maindeck-wm/maindeck-menu.c)), travando a interface.

### Proposta de Implementação
1. **Cache de Misses (Ícones não encontrados):**
   - Quando `find_icon_path` falhar e o ícone não for encontrado, ele será adicionado no cache com `surf = NULL` e `not_found = true`.
   - Consultas subsequentes a esse ícone falharão imediatamente sem bater no disco.
2. **Tabela Hash Global (FNV-1a):**
   - Substituir o cache linear `g_cache[64]` por uma Tabela Hash robusta e dinâmica escrita em C.
   - Usar a função de hash **FNV-1a** para calcular o hash da string do nome do ícone:
     ```c
     uint32_t fnv1a_hash(const char *str) {
         uint32_t hash = 2166136261u;
         while (*str) {
             hash ^= (unsigned char)*str++;
             hash *= 16777619u;
         }
         return hash;
     }
     ```
   - Usar endereçamento aberto (open addressing) com sondagem linear para resolução de colisões. A tabela iniciará com 128 slots e dobrará de tamanho se ultrapassar 70% de capacidade.
3. **Memoização Lazy no `struct App` (Menu):**
   - Adicionar o campo `cairo_surface_t *icon_surface` na struct de cada app em [maindeck-menu.c](file:///home/tcfialho/Documents/poc/maindeck-wm/maindeck-menu.c).
   - Ao desenhar o app no menu pela primeira vez, a surface é resolvida via resolvedor global e guardada no App.
   - Nos frames subsequentes, a interface lê diretamente o ponteiro `icon_surface` da struct, eliminando lookups de hash por frame.

---

## 2. P0.2 — Tray Assíncrono (D-Bus)

### Problema
O subsistema de tray ([bar-tray.c](file:///home/tcfialho/Documents/poc/maindeck-wm/bar-tray.c)) realiza chamadas síncronas bloqueantes `GetAll` e `GetLayout` na API do D-Bus (`dbus_connection_send_with_reply_and_block`) com timeouts de 1s e 2s, respectivamente. Se um aplicativo da área de notificação travar ou demorar para responder, a barra inteira congela (hover, cliques e relógio param de responder).

### Proposta de Implementação
1. **Chamadas Não-Bloqueantes (Assíncronas):**
   - Substituir as chamadas síncronas bloqueantes por `dbus_connection_send_with_reply` que retorna imediatamente com um `DBusPendingCall`.
2. **Callbacks do Main Loop:**
   - Registrar uma callback para processar a resposta quando ela chegar usando `dbus_pending_call_set_notify`.
   - A resposta do D-Bus será processada assim que os dados chegarem ao descritor de arquivo (fd) do D-Bus, o qual já está integrado no `poll()` principal do main loop da barra.
3. **Placeholders Visuais:**
   - Enquanto a chamada estiver pendente, o item do tray será renderizado com dimensões provisórias ou um ícone placeholder genérico.
   - Assim que a callback assíncrona for disparada com o layout ou propriedades corretas, a barra atualizará o ícone de forma suave e redesenhará o item.

---

## 3. P0.3 — Eliminação de `pkill` Síncrono

### Problema
Ao clicar em qualquer janela do sistema, a função `close_launcher()` no [wm-input.c](file:///home/tcfialho/Documents/poc/maindeck-wm/wm-input.c) realiza um `fork()` seguido de `execlp("pkill", ...)` para fechar o `maindeck-menu`. Isso força o sistema a varrer a tabela de processos inteira no `/proc` por clique, gerando overhead de IO/CPU e latência na interação de janelas.

### Proposta de Implementação
1. **IPC via Socket Unix (UDP/Unix Domain Socket):**
   - O `maindeck-menu` já escuta em um socket Unix de controle local ([maindeck-menu.c](file:///home/tcfialho/Documents/poc/maindeck-wm/maindeck-menu.c)).
   - O WM manterá um descritor de arquivo ou abrirá uma conexão UDP Unix Socket rápida para o socket do menu e enviará a mensagem de fechar (`"C"`). A escrita é realizada na memória do kernel e executada em menos de 5 microsegundos, sem gerar processos adicionais.
2. **Alternativa Direct Kill (Fallback):**
   - Se a conexão via socket falhar ou não estiver ativa, o WM utilizará `kill(menu_pid, SIGTERM)` diretamente no PID do menu previamente armazenado no momento do spawn, o que é uma chamada de sistema direta e O(1).
