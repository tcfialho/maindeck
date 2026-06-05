# Relatório de Uso de Memória — `maindeck-bar`

Foi realizada uma análise do uso de memória da barra de status (`maindeck-bar`) em execução. Identificamos que o processo em execução na sessão utilizava o **AddressSanitizer (ASan)**, o que distorcia a leitura de memória virtual (VSZ), gerando um consumo aparente de ~20 Terabytes.

---

## 🛠️ Resolução do Consumo de Memória Virtual

Para resolver o consumo excessivo de endereçamento virtual, o AddressSanitizer foi desativado no build principal do projeto, seguido pela compilação, instalação e reinicialização automática do processo:

```bash
# 1. Desativação do sanitizer na pasta build
meson configure build -Db_sanitize=none

# 2. Recompilação do projeto
ninja -C build

# 3. Instalação do binário limpo no path local
meson install -C build

# 4. Reinicialização do processo
kill <pid-da-bar> # o script de inicialização do river detecta e reinicia automaticamente
```

---

## 🚀 Otimização Adicional de Dependências e Linkagem

Após analisar o código fonte e as dependências listadas no `meson.build`, identificamos e implementamos as seguintes melhorias **sem qualquer perda de funcionalidade**:

1. **Remoção do `gio-2.0`:** O GInputOutput (GIO) era listado como dependência do Meson, mas nunca foi de fato incluído ou utilizado pelo código fonte da barra.
2. **Remoção do `gdk-pixbuf-2.0`:** O GdkPixbuf era importado apenas para carregar ícones (PNG/SVG) e convertê-los em superfícies do Cairo.
   * **Substituição nativa para PNG:** Passamos a carregar PNGs diretamente com a função nativa do Cairo: `cairo_image_surface_create_from_png`.
   * **Substituição nativa para SVG:** Passamos a utilizar o `librsvg` diretamente no código (`rsvg_handle_render_document`) para renderizar SVGs vetoriais diretamente no Cairo, em vez de depender da ponte de conversão do GdkPixbuf.
3. **Ativação de LTO (Link-Time Optimization):** Forçamos a otimização de linkagem no build (`b_lto=true`), reduzindo metadados redundantes da compilação.

Essa mudança eliminou o carregamento dinâmico de bibliotecas e loaders de imagem pesados, reduzindo drasticamente o consumo de endereçamento virtual.

---

## 📊 Comparativo Antes vs. Depois da Otimização de Dependências

| Métrica | Antes (Com ASan) | Após Remover ASan (Release) | Após Otimização de Deps (Sem GdkPixbuf/GIO + LTO) | Redução Total (vs. Original) |
| :--- | :---: | :---: | :---: | :---: |
| **VSZ / VmSize** (Virtual) | **20.0 TB** | **1.65 GB** | **344 MB** (344.328 kB) | **-99.998%** (Virtual) |
| **RSS / VmRSS** (Física) | **75.8 MB** | **26.6 MB** | **26.7 MB** (27.468 kB) | **-64.5%** |
| **PSS** (Física Proporcional) | **58.7 MB** | **10.2 MB** | **11.1 MB** (11.117 kB) | **-81.0%** |
| **Private Dirty** (Exclusiva) | **50.3 MB** | **4.77 MB** | **4.70 MB** (4.708 kB) | **-90.6%** |

*(Nota: O RSS/PSS tem variações marginais dependendo dos ícones e estados de janela carregados no cache do Cairo no momento da medição).*

---

## 🔍 Conclusão
A `maindeck-bar` agora roda de forma extremamente eficiente:
* A memória virtual foi comprimida para **344 MB** (antes **20 TB** / **1.65 GB**).
* O consumo real de memória física exclusiva (`Private Dirty`) é de apenas **4.7 MB**.
* Eliminamos o overhead de inicialização e dependências redundantes que não faziam sentido para o escopo do projeto em C.
