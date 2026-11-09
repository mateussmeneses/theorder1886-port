# The Order: 1886 PC Port - Windows v0.8.0

This release collects everything merged since v0.7.9: the graphics-fidelity and
temporal-stability fixes, the shader/pipeline compilation stutter work, a
RecompLauncher profile, and a new native replacement for the game's idle job
workers, which cuts the port's CPU use. It does not claim a stutter fix in every
scene, visual parity with the PS4, or that any open issue is resolved without the
reporter's retest.

## Highlights

1. **Idle job workers no longer burn four CPU cores (new, on by default):** the
   game's four "Task Kernel" job threads poll empty queues nonstop, yielding only
   every 1,800 polls (eboot 01.02 `sub_8a7380`, found with REA/Ghidra). The port
   now replaces that idle callback with native code that sleeps 200 us per idle
   period after 8 consecutive idle yields (`src/runtime_task_idle.c`). It is only
   installed when the function's bytes match eboot 01.02. A differential test runs
   the original machine code against the native version on 20,000 inputs.
   Measured in the prologue on one PC (i7-14650HX, RTX 5060 Laptop): process CPU
   22.1% -> 7.0%, frame P95 17.75 -> 17.55 ms; worst frames are unchanged.
   `THEORDER_TASK_IDLE=0` restores the original game code.
   Details: `docs/REA_NATIVE_ABSTRACTION.md`.
2. **Graphics fidelity fixes:** redraw depth bias defaults to 0 (black tearing
   and streaks on environment meshes); bit-preserving MSAA depth-to-color alias
   copies and a texture-cache leak fix on failed overlap copies; synchronized
   exposure/sun luminance readbacks and HTILE expansion on depth writes (whiteout
   and pitch-black screens, #43, #47); facial shaders kept off background workers
   across camera cuts; 1920x800 scene coordinates for TAA/DLSS/FSR reprojection;
   bilinear UI samplers at high resolutions; DOF blur and facial redraw
   regressions fixed (`docs/GRAPHICS_REGRESSIONS.md`).
3. **Shader/pipeline compilation stutters:** warm-cache permutations are
   preserved, identical stages are reused, upcoming pipelines are pre-queued and
   compiled concurrently, and background compiles are paced by a pressure budget.
   In matched 60 s title-scene runs, GPU-thread compile time fell 39-59% and the
   warm revisit reached 100% binary-cache hits (`docs/PERFORMANCE_BASELINE.md`).
   First visits to new scenes still compile pipelines.
4. **Pipeline cache size budget:** the binary pipeline cache is trimmed to 4 GB
   by default (`THEORDER_PIPELINE_BINARY_CACHE_MB`), and stale temporary files are
   removed.
5. **Tooling:** a RecompLauncher profile ships in the package (#111); the Linux
   build and HTILE tests were repaired (#112); indirect-draw clamps are reported in
   the log; the CPU sampler (`THEORDER_SAMPLE`) no longer hangs with `*`, names the
   game's threads and follows guest call stacks.

## Validation And Known Limits

- Windows CLANG64 build passed. All 56 C/C++ suites passed (including the
  task-idle differential test against the original eboot code). Python ran 348
  tests with 41 skips and no failures.
- The packaged build reached the in-engine prologue with default settings
  (idle replacement installed, 60 FPS, process CPU 7-10%).
- The idle-worker gain was measured on a 24-thread CPU. Whether it reduces
  stutter on CPUs with fewer cores is not measured yet.
- An intermittent GPU-library failure (`std::bad_alloc` at the prologue load)
  predates this release; it was seen in 1 of 14 runs without the idle change.
- First-visit stutters, flicker, lighting/shadows, AMD/Intel compatibility,
  dialogue subtitles and full-campaign stability remain open. No waits or draws
  were removed to improve benchmark numbers; stable 60/120 FPS is not promised.

For a useful report, include game SKU/version, chapter/checkpoint, camera, GPU
and driver, selected aspect/output/upscaler/preset, the in-game Image status and
a matching screenshot. For stutters, compare the same route with cache state
recorded. Redact private paths from logs.

## Package Contents

The Windows ZIP contains the launcher, direct-play launcher, native runtime,
required redistributable DLLs, preparation scripts, patch definitions and the
RecompLauncher profile. It contains no game dump, saves, player-generated caches,
private logs or proprietary DLSS runtime. Supply your own legally obtained game
dump.

## How To Play

1. Extract `theorder1886-windows.zip` into a new folder.
2. Keep the previous install and back up saves/settings. Do not clear shader
   caches as a routine update step.
3. Open `The Order 1886.exe`, select your game dump and launch.
4. If you suspect the idle-worker change, launch with the environment variable
   `THEORDER_TASK_IDLE=0` and compare.

## Portugues

1. **Threads de job ociosas nao gastam mais quatro nucleos (novo, ligado por
   padrao):** as quatro threads "Task Kernel" do jogo ficam consultando filas
   vazias sem parar. O port substitui o callback ocioso do eboot 01.02 por codigo
   nativo que dorme 200 us por periodo ocioso apos 8 yields ociosos seguidos. So e
   instalado se os bytes da funcao baterem com o eboot 01.02, e um teste
   diferencial compara o codigo original com o nativo em 20.000 entradas. Medido
   no prologo em um PC (i7-14650HX, RTX 5060 Laptop): CPU do processo 22,1% ->
   7,0%, P95 17,75 -> 17,55 ms; piores frames inalterados. `THEORDER_TASK_IDLE=0`
   volta ao codigo original.
2. **Fidelidade grafica:** depth bias de redraw em 0, copias de alias de
   profundidade MSAA, sincronizacao de exposicao/sol e HTILE (#43, #47), shaders
   faciais protegidos em cortes de camera, coordenadas 1920x800 para TAA/DLSS/FSR,
   UI bilinear em alta resolucao e correcao de DOF/redraw facial.
3. **Stutter de compilacao:** permutacoes preservadas no cache quente, reuso de
   estagios, pre-enfileiramento e compilacao concorrente; em runs equivalentes de
   60 s o tempo de compilacao na thread de GPU caiu 39-59%. A primeira visita a
   cenas novas ainda compila pipelines.
4. **Cache de pipelines limitado a 4 GB** por padrao.
5. **Ferramentas:** perfil RecompLauncher (#111), build Linux e testes HTILE
   (#112), relatorio de clamps e sampler de CPU corrigido.

Ganho de CPU medido em uma CPU de 24 threads; efeito em CPUs com menos nucleos
ainda nao medido. Uma falha intermitente da biblioteca de GPU (`std::bad_alloc` no
carregamento do prologo) ja existia antes desta versao. Stutters de primeira
visita, flicker, iluminacao/sombras, AMD/Intel, legendas e estabilidade da
campanha continuam pendentes; nao ha promessa de 60/120 FPS estaveis.

Extraia em uma pasta nova, mantenha backup de saves/configuracoes e nao limpe os
caches como procedimento de atualizacao. O ZIP nao inclui arquivos do jogo.

## Optional Support / Apoio Opcional

Development support is optional and does not unlock features. / Apoiar e
opcional e nao desbloqueia funcionalidades.

[Support on Ko-fi / Apoiar no Ko-fi](https://ko-fi.com/mateusmeneses)
