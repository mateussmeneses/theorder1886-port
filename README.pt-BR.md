# The Order: 1886 PC Native Port

[English](README.md) | **Português (Brasil)** | [Español](README.es.md)

[![Apoie no Ko-fi](https://img.shields.io/badge/Ko--fi-Pague%20um%20Café-ff5e5b?style=for-the-badge&logo=ko-fi&logoColor=white)](https://ko-fi.com/mateusmeneses)

> [!TIP]
> ### ☕ Apoie o Desenvolvimento / Compre um Café
> Se você gosta deste projeto e quer incentivar o desenvolvimento contínuo, engenharia reversa e otimização do port nativo de **The Order: 1886 para PC**, considere me pagar um café! Sua contribuição ajuda a manter o projeto ativo, motivado e trazendo novidades e correções constantes.
>
> 👉 **[Clique aqui para contribuir no Ko-fi: https://ko-fi.com/mateusmeneses](https://ko-fi.com/mateusmeneses)** ☕❤️

Port para PC, em desenvolvimento, de **The Order: 1886** (PlayStation 4) para Windows 10 e 11.

A parte de CPU é nativa: o executável original do jogo roda direto no processador do PC (o PS4
usa a mesma arquitetura x86-64) e um runtime escrito para este port substitui as bibliotecas de
sistema do PS4, então não há emulador de CPU. Os gráficos são traduzidos: rodam no núcleo de vídeo
do [shadPS4](https://github.com/shadps4-emu/shadPS4), que converte os comandos e shaders da GPU do
PS4 para Vulkan. Este projeto começou como uma cópia do
[port de God of War III Remastered](https://github.com/FelipePrado0/gow3_pc) e está sendo
adaptado ao motor da Ready at Dawn.

> **Nenhum arquivo do jogo está incluído.** Você precisa de um dump da sua própria cópia.
> Este projeto não tem relação com a Sony Interactive Entertainment, a Ready at Dawn ou o Santa
> Monica Studio. The Order: 1886 é marca registrada da Sony Interactive Entertainment.

## Situação

Verificado no Windows 11 com uma NVIDIA GeForce RTX 5060 Laptop GPU e o dump **CUSA00035,
versão 01.02** (`eboot.bin` sha256 `f742eb17…`):

- O jogo passa pelos logos e pela tela de título e inicia um novo jogo; a cinemática do prólogo é
  renderizada pelo motor (personagens, iluminação e texturas conferidos em dumps de quadros).
- Uma sessão de 4 minutos no prólogo manteve os 30 FPS do próprio jogo em todas as janelas de
  5 segundos, com frame time no percentil 99 de cerca de 34,5 ms, e não houve nenhum crash em
  mais de 20 execuções automatizadas seguidas (`tools/batch_runs.py`).
- A camada de validação do Vulkan mostra uma única classe de aviso restante nessas cenas
  (`magFilter-04553`, sampler linear usado com uma textura de inteiros).

Ainda não verificado: gameplay depois do prólogo, som (as execuções automatizadas não o conferem),
salvar e carregar, placas AMD e Intel, outras regiões e versões do jogo, Linux. O registro do
trabalho, as correções e as pendências estão em [docs/PROGRESS.md](docs/PROGRESS.md).

## Requisitos

- Windows 10 (1903 ou mais novo) ou Windows 11, 64 bits.
- Processador de 64 bits com AVX. Em CPUs sem MOVBE, BMI1 ou LZCNT (Sandy/Ivy Bridge, AMD FX) o
  carregador reescreve as instruções do jogo que as usam para rotinas emuladas e registra uma
  linha `ISA:` no log; CPUs mais antigas param na inicialização com a linha `STOP: this CPU lacks`.
- Placa de vídeo com Vulkan 1.3 e driver atualizado.
- O dump do seu jogo: a pasta com `eboot.bin`, `sce_module` e `sce_sys`. Só o CUSA00035 01.02
  foi testado; o arquivo de patches também lista CUSA00070, CUSA00076, CUSA00100, CUSA00670 e
  CUSA00785.
- [MSYS2](https://www.msys2.org) e [Python 3](https://www.python.org) para compilar (abaixo).

## Compilando

1. Instale o MSYS2 (`winget install MSYS2.MSYS2`) e, no shell **MSYS2 CLANG64**, os pacotes
   listados em [packaging/windows/README.md](packaging/windows/README.md).
2. Clone com os submódulos e compile:

   ```bash
   git clone --recursive https://github.com/mateussmeneses/theorder1886-port
   cd theorder1886-port
   bash build.sh
   ```

   O resultado é `out/theorder-probe.exe` (também copiado como `out/theorder-probe.exe`). Se o build
   disser que não consegue substituir o executável, ainda há um processo do jogo rodando.

## Jogando

A pasta do jogo vem de `THEORDER_GAME_DIR`; sem ela, o `run.py` procura uma pasta `CUSA00035` ao
lado da pasta do projeto.

```powershell
$env:THEORDER_GAME_DIR = 'D:\caminho\CUSA00035'
python run.py
```

### Opções do motor

Estas opções existem no executável do jogo e o `run.py` as repassa. O efeito delas na versão
comercial ainda não foi verificado: com `--level`, o jogo continua parando na tela de título.

| `run.py` | Opção do jogo |
|---|---|
| `--level <nome>` | `-startlevel <nome>` (por exemplo `cat_010_streets`, `emb_020_tunnels`, `air_010_rappel`, `pro_020_westminster`) |
| `--checkpoint <nome>` | `-startcheckpoint <nome>` |
| `--no-cloth` | `-disablecloth` |
| `--no-cam-springs`, `--no-cam-additive` | `-nocamsprings`, `-nocamadditive` |
| `--msaa <n>` | `-msaa <n>` |
| `--frontend` | `-frontend` |
| `--perf` | `-disablecloth -nocamsprings -nocamadditive -msaa 1` |

## Controles

Controles (DualSense, Xbox e outros) funcionam pelo SDL3, junto com o teclado.

| Tecla | Botão |
|---|---|
| WASD | analógico esquerdo |
| Setas | analógico direito (câmera) e direcional |
| Espaço | ✕ (Cross) |
| Enter | Options |
| Shift esquerdo ou Esc | ○ (Circle) |
| E | □ (Square) |
| Q | △ (Triangle) |
| 1 e 3 | L1 e R1 |
| R e F | L2 e R2 |
| Z e C | L3 e R3 |
| I, K, J, L | direcional |
| Tab e Backspace | clique esquerdo e direito do touchpad |
| Insert | menu do port (R3+L2 no controle) |
| Mouse | câmera; botão esquerdo R2 (atirar), direito L2 (mirar), meio R3, laterais L1/R1 |
| F7 | liga/desliga a câmera pelo mouse |

Em controles sem touchpad, View (Back) é o clique direito do touchpad.
`THEORDER_KEY_MAP` e `THEORDER_PAD_MAP` trocam os botões (`src/runtime_pad.c`).

O menu do port (Insert) tem as opções de tela, campo de visão, mouse, limite de quadros e o
overlay de desempenho. O campo de visão (50-200%) muda na hora; acima de 100% mostra mais em
todos os lados.

## Patches

O `patches/TheOrder1886-Orbis.xml` é aplicado na memória ao iniciar; os arquivos do jogo não são
alterados. Todos os patches vêm desligados.

- **Frame Rate Patch - 60 FPS** (experimental): troca as quatro constantes de tempo de quadro de
  30 FPS (33333 µs) do executável 01.02 por 16667; os pontos foram conferidos no binário. O jogo
  também pede apresentação a 30 Hz, então o port precisa apresentar a cada vblank e limitar a 60:

  ```powershell
  Set-Content fps60.ini 'engine_fps=120'   # a lógica do launcher cai para o patch de 60 FPS
  $env:THEORDER_CONFIG = (Resolve-Path fps60.ini)
  $env:THEORDER_FLIP_RATE = '0'; $env:THEORDER_FPS_LIMIT = '60'
  python run.py
  ```

  A velocidade do jogo continua correta (os dumps de quadros batem no tempo com uma execução a
  30 FPS). Logos e título ficam em 60 FPS; numa sessão de 240 s do prólogo cada janela de 5 s ficou
  entre 41 e 60 FPS (57 na média), com as quedas onde a thread de comandos da GPU fica ocupada o
  quadro todo (600 a 700 draws por quadro).
- **Aspect Ratio - 16:9 / 16:10 / 21:9** (launcher e menu do jogo: *Proporção da imagem*, vale ao
  reiniciar): o jogo desenha uma imagem 2.40:1 com faixas pretas. Estes patches mudam o formato da
  imagem (o float 2.4 em 0x44ce8c) e o fator 0.5625 com que cada câmera converte o campo de visão
  horizontal (definido para 16:9) em vertical, então a visão mantém a largura original e mostra mais
  em cima e embaixo (Vert+) em vez de ser cortada nas laterais. O port mostra só a imagem, ajustada
  à janela: telas 16:10 e 21:9 também ficam cheias. A cena tem mais pixels (1920x1080 em vez de
  1920x800 em 16:9); a 60 FPS no prólogo (RTX 5060 Laptop) isso custou cerca de 1% (57,8 contra 58,3
  FPS na média).
- Os outros patches escritos em sessões anteriores foram retirados depois de testados: pular
  logos não mudava nada, dispensar a tela de título deixava a tela preta, os dois de pular o
  prólogo não puderam ser verificados, e os de 900p, 720p e de remover as faixas pretas escreviam
  no meio de instruções (ativá-los corrompia o código).

## Configurações e diagnóstico

O núcleo gráfico lê estas variáveis de ambiente:

| Variável | Efeito |
|---|---|
| `THEORDER_NO_MSAA=1` / `0` | imagens do jogo sem ou com multisampling (com, por padrão, na NVIDIA) |
| `THEORDER_ASYNC_COMPUTE=0` / `1` | pipelines de compute nunca montados antes para este jogo: sempre no thread da GPU / sempre em segundo plano (os dispatches são pulados enquanto isso; os menus podem perder textos e ícones). Padrão: em segundo plano só para programas que rodam todo quadro e não escrevem imagens |
| `THEORDER_PIPELINE_BINARY_CACHE_MB=N` | tamanho do cache de binários de pipeline (padrão 4096); os mais antigos além disso são removidos ao iniciar |
| `THEORDER_LAZY_SHADER_CACHE=0` | traduz todos os shaders de novo em vez de carregar os do cache |
| `THEORDER_FLIP_RATE`, `THEORDER_FPS_LIMIT` | taxa de apresentação (ver o patch de 60 FPS) |
| `THEORDER_VK_VALIDATION=1` | camada de validação do Vulkan (com `VK_ADD_LAYER_PATH` apontando para ela) |
| `THEORDER_GPU_LOG=info` | nível do log da GPU |
| `THEORDER_PERMUTATION_LOG=1` | por que cada nova permutação de shader foi compilada |
| `THEORDER_BIG_TRACK_LOG=1` | quem rastreia faixas de memória acima de 256 MiB |
| `THEORDER_PAD_FILE=<arquivo>` | entrada de controle roteirizada para execuções automatizadas |

Os saves e o cache de shaders ficam na pasta `user` do projeto (`user/savedata`,
`user/cache/CUSA00035`).

## Testes

```bash
python -m unittest discover -s tests          # 273 testes, todos passando em 15/10/2026
python tools/batch_runs.py --runs 8 --secs 40 --pad "24:cross 28:cross 32:cross"
python tools/fault_stacks.py out/batch/*.log
```

O `tools/batch_runs.py` inicia o jogo várias vezes, compara condições (`--cond`), aperta botões
(`--pad`, para passar da tela de título) e informa crashes, travamentos e tempo de compilação; o
`tools/fault_stacks.py` simboliza os relatórios de crash.

## Como funciona

| Parte | Onde | O que faz |
|---|---|---|
| Preparação | `scripts/` | Lê o `eboot.bin`, aplica as relocações e liga a `libc` e a `libSceFios2` do próprio jogo numa imagem só |
| Carregador | `src/probe.c` | Carrega a imagem e inicia o código original do jogo |
| Runtime | `src/runtime_*.c` | Reimplementa as funções de sistema do PS4: memória, threads, arquivos, áudio, vídeo, controle, saves e serviços |
| GPU | `gpu/shadps4/` | O núcleo de vídeo do shadPS4 com as correções do cuesta4 e o trabalho dos ports de God of War III e The Order (compilação assíncrona de shaders e pipelines, cache de shaders) |
| Menu do port | `gpu/shim/port_overlay.cpp` | O menu Dear ImGui e o overlay de desempenho |
| Patches | `patches/TheOrder1886-Orbis.xml`, `scripts/patches.py` | Aplicados na memória ao iniciar |

O launcher em `launcher/` e o empacotamento para Linux em `packaging/linux/` vêm do port de God
of War III e ainda não foram adaptados a este jogo.

## Créditos

- **Igor Augusto e colaboradores do TOport**: o [TOport](https://github.com/IgorAugustoHH/The-Order-1886-PC-Port) publicou anteriormente
  as implementações adaptadas aqui para [correção de profundidade facial](https://github.com/IgorAugustoHH/The-Order-1886-PC-Port/commit/c83f951),
  [normalização SRT de partículas](https://github.com/IgorAugustoHH/The-Order-1886-PC-Port/commit/c83f951),
  [bias de profundidade em redesenhos](https://github.com/IgorAugustoHH/The-Order-1886-PC-Port/commit/fff668c) e
  [sombras EVSM/HTILE](https://github.com/IgorAugustoHH/The-Order-1886-PC-Port/commit/dfc66af).
  Essas soluções foram adaptadas e modificadas neste port; não são trabalho exclusivamente original deste repositório.
  Consulte a [correção de atribuição da v0.7.3](docs/RELEASE-v0.7.3-ATTRIBUTION.md) (issue #69).

- **Felipe Prado**: o [port de God of War III Remastered para PC](https://github.com/FelipePrado0/gow3_pc)
  do qual este projeto foi copiado.
- **deadinside28**: [bloodborne_pc](https://github.com/deadinside28/bloodborne_pc), o port nativo
  original, e **Supermedo** (Mohammed Albarghouthi), o
  [fork para Windows](https://github.com/Supermedo/bloodborne_pc).
- **cuesta4**: o [fork do shadPS4](https://github.com/cuesta4/shadPS4/tree/eltutz) (branch
  `eltutz`) cujas correções o núcleo de vídeo carrega.
- **Equipe do shadPS4**: o núcleo de vídeo do [shadPS4](https://github.com/shadps4-emu/shadPS4).

Também usados: [LibAtrac9](https://github.com/Thealexbarney/LibAtrac9), [FFmpeg](https://ffmpeg.org),
[SDL3](https://github.com/libsdl-org/SDL), [Dear ImGui](https://github.com/ocornut/imgui),
[sirit](https://github.com/shadps4-emu/sirit), [magic_enum](https://github.com/Neargye/magic_enum),
[miniz](https://github.com/richgel999/miniz), [xbyak](https://github.com/herumi/xbyak),
[Vulkan Memory Allocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator),
[FSR-Vulkan](https://github.com/FireBurn/FSR-Vulkan) e o AMD FidelityFX SDK,
[MSYS2](https://www.msys2.org), [LLVM](https://llvm.org) e [Pillow](https://python-pillow.org).

## Licença

GNU GPL v2 ou posterior ([LICENSE](LICENSE)). Componentes de terceiros mantêm suas próprias
licenças.
