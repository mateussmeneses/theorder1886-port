> Archived from docs/BACKLOG.md on 2026-11-12. Outdated: several items marked resolved here are still open issues.

## Snapshot histórico — 2026-10-20 (superado)

---

## 1. Mapeamento histórico de issues (superado)

| Issue | Tipo | Título Resumido | Status | Diagnóstico & Solução Técnica |
| :---: | :---: | :--- | :---: | :--- |
| **#26** | 🐛 Bug | Sun shadows are still not appearing | ✅ **Resolvido** | 1. Removida a flag inválida `eByRegion` em `BufferMemoryBarrier2` em `BufferCache::DownloadMemory` (que causava rejeição pelo driver Vulkan em transferências para CPU).<br>2. Incluído o hash de shader `0x8ea0ef5b` (segundo compute shader de redução de profundidade EVSM) no override de tile coverage `0x7F80000F` em `vk_rasterizer.cpp`.<br>3. Inserido flush de barreiras (`runtime.FlushBarriers()`) imediatamente após `runtime.FillBuffer` da HTILE uncompressed. |
| **#27** | 💡 Feature | Add ultrawide 32:9 (5120x1440) | ✅ **Resolvido** | 1. Criado o patch `Aspect Ratio - 32:9` em `patches/TheOrder1886-Orbis.xml` (aspect ratio float32 `3.55555556`, FOV ajustado `0.3796875`, projeção 2D de UI corrigida).<br>2. Adicionado suporte a `32:9` no seletor de aspecto em `scripts/graphics_options.py`.<br>3. Incluídas as resoluções `3440x1440` e `5120x1440 (32:9 Super Ultrawide)` no Launcher (`launcher/port_launcher_win.py`). |
| **#28** | 💡 Feature | Não funciona o idioma Português-Brasil | ✅ **Resolvido** | O SKU `CUSA00785` possui dublagem e textos em PT-BR nativos (`engine id 17`). O SKU `CUSA00076` (Europa/América) não possui o pacote `17` em seus manifestos de assets, caindo silenciosamente no fallback em inglês (`0`). Implementado fallback em `run.py` para mapear `ps4_lang == 17` para o `engine id 7` (Português) nos SKUs `CUSA00076`/`CUSA00035`, garantindo áudio e legendas em português. |
| **#29** | 🐛 Bug | Black polygons and texture flicker | ✅ **Resolvido** | Identificada e revertida a transição forçada de alvos de profundidade para layout `vk::ImageLayout::eGeneral` com feedback loop em `Rasterizer::BeginRenderingFull` e `BindTextures`. Depth buffers agora mantêm `eDepthStencilAttachmentOptimal` ou `eDepthAttachmentOptimal`, restaurando Hi-Z e eliminando polígonos pretos e flickers. |
| **#30** | 🐛 Bug | Everything flickering and sudden stuttering in fight scenes | ✅ **Resolvido** | 1. Correção do layout de depth attachment (eliminando flickers visuais).<br>2. Otimizado o escalonador de threads no Windows em `src/runtime_thread.c`: substituído o busy-spin do `sched_yield()` por `SwitchToThread()` com fallback para `Sleep(0)`. Isso encerra o starvation de CPU em processadores de alta contagem de núcleos (Ryzen 7950X, Threadripper 3990X) durante cenas de combate intenso. |
| **#33** | 🐛 Bug | Shiny silver objects in dark areas & dark shadows | 🟡 **Em Progresso** | Identificada ausência de suporte no driver Vulkan AMD Adrenalin (ex: RX 9070) para o formato de superfície `A2B10G10R10SnormPack32` (usado para G-Buffer Normals). O driver reporta suporte nulo para SampledImage/ColorAttachment, resultando em normais nulas e reflexão especular descalibrada. Desenvolvendo remapeamento dinâmico de formato para RDNA 4. |
| **#16** | 💡 Feature | The Armoury menu unlock & load official DLCs | 🟡 **Em Progresso** | Os DLCs oficiais (`TO1886GAMEDLC100` e `TO1886GAMEDLC200`) já são automaticamente detectados e montados em `/addcont0` e `/addcont1` via `src/runtime_content.c`. Desenvolvendo patch no `eboot.bin` para desbloquear o menu "The Armoury" fora do New Game+. |
| **#22** | 💡 Feature | FSR e Framerate de 120 FPS | ✅ **Resolvido** | Suporte a 120 FPS desbloqueado (frametime clamp de 8.33ms) e FSR 3.1 temporal upscaler integrados e validados na release `v0.7.4`. |
| **#31** | 💡 Feature | Add DLSS, XeSS, Frame Generation | 📋 **Planejado** | Integração via Streamline SDK para expor DLSS 3.7 e XeSS 1.3 paralelamente ao FSR existente. |
| **#32** | 💡 Feature | Post Processing Effect Toggles | 📋 **Planejado** | Adição de toggles para granulação de filme (film grain), aberração cromática, distorção de lente e vinheta através de patch de constantes nos pixel shaders de pós-processamento. |
| **#11** | 🐛 Bug | Crash in Chapter 9 | 🔍 **Investigado** | Ocorrência ligada a esgotamento de memória de host (`VK_ERROR_OUT_OF_HOST_MEMORY`) durante compilação massiva de pipelines em sistemas com arquivo de paginação do Windows pequeno (2 GB). |
| **#10** | 🐛 Bug | Intel HD 630: characters are black silhouettes | 🔍 **Investigado** | GPUs Intel HD Graphics 6xx possuem suporte incompleto à extensão `VK_EXT_vertex_input_dynamic_state`. |
| **#1** | 🐛 Bug | Intel Arc IGPU Texture Bug | ✅ **Resolvido** | Corrigido nas versões v0.7.2/v0.7.3 com o flag de MSAA off para drivers Intel. |

---

## 2. Otimizações de Renderização & Vulkan Backend

### 2.1 Pipeline State Object (PSO) Pre-Warming & Caching Avançado
- **Objetivo**: Eliminar completamente micro-stutters durante a transição de zonas e carregamento dinâmico de shaders de iluminação.
- **Implementação**:
  - Implementar pool de compilação em background desacoplado da render thread.
  - Implementar hashing unificado de SPIR-V com desduplicação antecipada de pipelines equivalentes de forward/deferred lighting.

### 2.2 Suporte a Formatos de Superfície Remapeados para AMD RDNA 4 (Issue #33)
- **Objetivo**: Garantir que superfícies com `A2B10G10R10SnormPack32` sejam renderizadas e amostradas corretamente em GPUs onde o driver Vulkan proprietário não expõe esse formato opcional.
- **Implementação**:
  - Aliasing inteligente de textura e view remapping para `A2B10G10R10UnormPack32` com desempacotamento de sinal no shader ou fallback para `R16G16B16A16Snorm`.

### 2.3 Calibração Completa do FSR 3.1 / FSR 4 & DLSS (Issue #31)
- **Objetivo**: Ajuste de matrizes de projeção reversa, jittering sub-pixel e vetores de movimento para upscaler temporal.
- **Implementação**:
  - Integração de vetores de câmera (camera jitter cancellation) diretamente nos compute shaders de pré-pass do FSR.
  - Ajuste de Reactive Mask e Composition Mask para evitar fantasmas (ghosting) nas roupas de lã e bordas dos personagens durante movimentos rápidos.

---

## 3. Física, Simulação & CPU Engine

### 3.1 Otimização de Simulação de Tecido (Cloth Physics)
- **Objetivo**: Aumentar a taxa de quadros em cenas com muitos personagens (fardas, capas e casacos de tecido pesado).
- **Implementação**:
  - Adicionar controle de LOD de simulação de tecidos no menu in-game / launcher para CPUs de menor contagem de núcleos.

### 3.2 Escalonamento de Threads Cooperativo no Windows
- **Objetivo**: Eliminar aquecimento desnecessário e contenção de CPU guest.
- **Implementação**:
  - Substituição de busy-wait em `thread_yield()` por `SwitchToThread() / Sleep(0)`, garantindo alívio de 100% de CPU para drivers Vulkan e worker threads.

---

## 4. Usabilidade, Controles & Customização

### 4.1 Toggles de Efeitos de Pós-Processamento (Issue #32)
- **Objetivo**: Permitir desativar granulação de filme (film grain), aberração cromática e desfoque de movimento pelo menu in-game (`Insert`) e pelo Launcher.
- **Implementação**:
  - Inserir patches XML dinâmicos ou parâmetros de uniform buffer para anular os pesos dos filtros na etapa final de composição de cena.

### 4.2 Desbloqueio do Menu "The Armoury" (Issue #16)
- **Objetivo**: Disponibilizar o menu de seleção de trajes e uniformes DLC a qualquer momento durante a campanha.
- **Implementação**:
  - Patch de bypass na checagem de New Game+ no `eboot.bin`.

### 4.3 Interface HUD Adaptativa para Ultrawide (21:9 e 32:9)
- **Objetivo**: Opção para manter os elementos de HUD ancorados nos cantos da tela ou centralizados na área 16:9 tradicional em monitores UltraWide e Super UltraWide.
- **Implementação**:
  - Hook nos comandos de desenho de UI guest (`DrawTexturedQuad` / ortho 2D) com offset configurável pelo usuário.

---

## 5. Histórico e Cronograma de Entregas

| Prioridade | Funcionalidade / Correção | Módulo | Ciclo Alvo |
| :---: | :--- | :--- | :---: |
| 🟢 **Concluído** | Correção de Sombras de Sol (HTILE uncompressed + barrier) | `gpu/shadps4/video_core` | v0.7.5 |
| 🟢 **Concluído** | Suporte Super Ultrawide 32:9 (5120x1440) | `patches/` & `launcher/` | v0.7.5 |
| 🟢 **Concluído** | Fallback de Idioma Português em SKUs CUSA00076 | `run.py` | v0.7.5 |
| 🟢 **Concluído** | Fim do flickering de profundidade e travamento de CPU | `gpu/` & `src/` | v0.7.5 |
| 🔴 **Alta** | Formato de G-Buffer normal para RDNA 4 (Issue #33) | `gpu/shadps4/video_core` | v0.7.6 |
| 🔴 **Alta** | Toggles de granulação, aberração cromática e vinheta | `patches/` & `gpu/shim` | v0.7.6 |
| 🟡 **Média** | Patch de desbloqueio permanente do The Armoury | `patches/` | v0.7.6 |
| 🟡 **Média** | Ancoragem de HUD em Ultrawide 21:9 / 32:9 | `scripts/hud_layout.py` | v0.7.7 |
| 🟢 **Baixa** | Integração Streamline (DLSS / XeSS) | `gpu/shadps4/video_core` | v0.8.0 |
