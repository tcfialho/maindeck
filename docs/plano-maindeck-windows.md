# Plano MainDeck para Windows

Este plano descreve a via Windows do MainDeck: substituir a experiencia de shell do Windows onde for viavel, sem tentar substituir o DWM.

## Objetivo

Construir uma versao Windows do MainDeck composta por:

- `maindeck-shell.exe`: processo principal iniciado no login;
- `maindeck-bar.exe`: barra/taskbar propria;
- `maindeck-menu.exe`: menu iniciar/launcher proprio;
- `maindeck-window-organizer.exe`: organizador de janelas via Win32;
- opcionalmente, `maindeck-indexer.exe`: indexador leve de apps/jogos.

A ideia e substituir `Explorer.exe` como shell em um usuario controlado, deixar o DWM ativo, e impor a organizacao de janelas por cima das APIs Win32.

## Processos do Windows

| Area | Processo/host do Windows | Como tratar |
|---|---|---|
| Shell principal, desktop e taskbar | `explorer.exe` | Principal alvo de substituicao. Usar Shell Launcher para iniciar `maindeck-shell.exe` no lugar dele. |
| Menu iniciar | `StartMenuExperienceHost.exe` | Nao deve ser necessario se `explorer.exe` nao for o shell. Substituir por `maindeck-menu.exe`. |
| Experiencias visuais do shell | `ShellExperienceHost.exe` | Evitar depender. Nao tratar como alvo primario de kill. |
| Busca do menu iniciar | `SearchHost.exe` | Opcional. Substituir por busca propria ou desabilitar se o appliance nao precisar. |
| Indexacao de busca | `SearchIndexer.exe`, `SearchProtocolHost.exe`, `SearchFilterHost.exe` | Opcional. Desabilitar ou substituir por indexador proprio simples. |
| Widgets Windows 11 | `Widgets.exe`, `WidgetService.exe` | Opcional. Remover/desabilitar para reduzir ruido. |
| Infraestrutura de shell | `sihost.exe` | Nao matar em producao. Pode ser necessario para partes da sessao do usuario. |
| Compositor | `dwm.exe` | Nao substituir e nao matar. Continua sendo o compositor do sistema. |
| Processos criticos de sessao | `winlogon.exe`, `csrss.exe`, `services.exe`, `lsass.exe` | Nunca tratar como alvo do MainDeck. |

## O que substituimos

### Menu iniciar

Substituicao direta por `maindeck-menu.exe`.

Funcionalidades esperadas:

- lista de jogos e aplicativos;
- busca local;
- atalhos fixos;
- desligar/reiniciar/suspender;
- acao de abrir Steam/game launcher;
- fechamento automatico ao perder foco.

### Barra/taskbar

Substituicao por `maindeck-bar.exe`.

Implementacao provavel:

- janela borderless/topmost;
- registro como appbar com `SHAppBarMessage`;
- area reservada na borda da tela;
- botoes de launcher;
- lista de janelas abertas;
- tray proprio somente se for realmente necessario.

Em modo jogo, a barra deve sumir e deixar de interferir na apresentacao do jogo.

### Organizacao de janelas

Nao existe um "openbox.exe" no Windows para trocar. O organizador deve agir por cima do sistema:

- observar criacao, destruicao, ativacao e mudanca de janelas com `SetWinEventHook`;
- enumerar janelas com `EnumWindows`;
- filtrar janelas relevantes por estilo, processo, visibilidade e owner;
- mover/redimensionar com `SetWindowPos`, `MoveWindow` ou `BeginDeferWindowPos`;
- respeitar fullscreen e janelas de dialogo/transientes;
- evitar intervir em jogos fullscreen/borderless.

Isso cria uma camada parecida com o `maindeck-wm` em termos de politica de layout, mas nao substitui o gerenciador/compositor interno do Windows.

## Snap, DWM e analogias

O Windows ja tem Snap, Snap Assist e Snap Layouts. No Windows 11, Snap Layouts aparecem ao passar o mouse no botao maximizar ou com `Win+Z`, e o usuario escolhe zonas de layout.

Esses recursos nao ficam em um processo unico simples que podemos matar como se fosse um WM separado. Eles fazem parte da experiencia de shell/window management do Windows, com participacao de Explorer/shell, Win32/User32 e DWM.

Analogias corretas:

| Linux/MainDeck atual | Windows |
|---|---|
| `river` | compositor Wayland + parte do gerenciamento de janelas. No Windows nao ha equivalente substituivel direto. |
| `wlroots`/DRM/KMS/render backend | mais proximo do papel de baixo nivel do DWM + stack grafica WDDM, mas nao e trocavel pelo app. |
| `maindeck-wm` | `maindeck-window-organizer.exe`, mas no Windows ele opera por cima das janelas existentes. |
| `maindeck-bar` | `maindeck-bar.exe` como appbar/topmost. |
| `maindeck-menu` | `maindeck-menu.exe`. |
| Openbox/dwm em X11 | nao ha substituto direto suportado no Windows moderno. |

Portanto, o DWM nao e exatamente o River. O River e compositor + protocolo Wayland + politica de janelas delegada ao cliente WM. O DWM e principalmente o compositor do Windows. A politica de janelas fica espalhada entre Win32/User32, shell/Explorer e componentes do sistema. O nosso MainDeck Windows seria uma camada de shell e politica por cima, nao um substituto do DWM.

## Modo jogo no Windows

Para jogos, a estrategia deve ser nao atrapalhar o caminho rapido do Windows:

- deixar o jogo em fullscreen exclusivo ou borderless fullscreen conforme melhor resultado;
- esconder/desregistrar a appbar enquanto o jogo esta ativo;
- nao manter overlays topmost por cima do jogo;
- evitar captura/preview DWM durante gameplay;
- deixar o Windows/driver usar DXGI flip model, DirectFlip, MPO ou independent flip quando possivel.

O equivalente conceitual ao direct scanout do Wayland/wlroots, no Windows, e o conjunto de otimizacoes de apresentacao como flip model, DirectFlip, multiplane overlay e independent flip. O MainDeck deve sair do caminho quando o jogo entrar nesse modo.

## Riscos

- Shell replacement mal feito pode deixar o usuario sem desktop facil de recuperar.
- Appbar propria pode conflitar com Explorer se ambos estiverem ativos.
- Organizacao agressiva de janelas pode quebrar launchers, installers, dialogs e jogos borderless.
- UWP/Settings/Store podem se comportar diferente quando Explorer nao e o shell.
- Snap Assist e o organizador proprio podem disputar posicao de janelas se ambos estiverem ativos.

## Caminho recomendado

1. Criar `maindeck-window-organizer.exe` rodando junto com Explorer, apenas observando e logando eventos.
2. Implementar layout MainDeck em modo opt-in, ainda com Explorer ativo.
3. Criar `maindeck-bar.exe` como appbar e testar convivencia.
4. Criar `maindeck-menu.exe`.
5. So depois testar Shell Launcher para substituir `Explorer.exe` em usuario separado.
6. Implementar modo jogo: suspender barra/organizer durante fullscreen.
7. Medir CPU, memoria e impacto em jogos.

## Esforco estimado

| Entrega | Tokens aproximados com IA | Observacao |
|---|---:|---|
| Organizer observador/logador | `30k-80k` | Baixo risco. |
| Organizer com layout MainDeck basico | `80k-200k` | Principal complexidade sao filtros de janela. |
| Barra/appbar propria | `60k-160k` | Viavel, mas tray pode complicar. |
| Menu/launcher proprio | `50k-150k` | Reaproveita conceitos do MainDeck atual. |
| Shell replacement com rollback | `80k-200k` | Precisa ser muito conservador. |
| Modo jogo Windows | `40k-120k` | Depende de deteccao confiavel de fullscreen/jogos. |

## Fontes

- Shell Launcher: https://learn.microsoft.com/pt-br/windows/configuration/shell-launcher/
- DWM overview: https://learn.microsoft.com/pt-br/windows/win32/dwm/dwm-overview
- Snap Layouts: https://learn.microsoft.com/en-us/windows/apps/desktop/modernize/apply-snap-layout-menu
- Appbars: https://learn.microsoft.com/en-us/windows/win32/shell/application-desktop-toolbars
- SetWinEventHook: https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setwineventhook
- Window positioning APIs: https://learn.microsoft.com/en-us/windows/win32/winmsg/window-features
