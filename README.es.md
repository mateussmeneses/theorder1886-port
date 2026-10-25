# The Order: 1886 PC Native Port

[English](README.md) | [Português (Brasil)](README.pt-BR.md) | **Español**

[![Apoya en Ko-fi](https://img.shields.io/badge/Ko--fi-Invítame%20un%20Café-ff5e5b?style=for-the-badge&logo=ko-fi&logoColor=white)](https://ko-fi.com/mateusmeneses)

> [!TIP]
> ### ☕ Apoya el Proyecto / Invítame un Café
> Si te gusta este proyecto y deseas apoyar el desarrollo continuo, ingeniería inversa y optimización del port nativo de **The Order: 1886 para PC**, ¡considera invitarme un café! Cada contribución ayuda a mantener el proyecto activo con actualizaciones constantes.
>
> 👉 **[Haz clic aquí para contribuir en Ko-fi: https://ko-fi.com/mateusmeneses](https://ko-fi.com/mateusmeneses)** ☕❤️

Port para PC, en desarrollo, de **The Order: 1886** (PlayStation 4) para Windows 10 y 11.

La parte de CPU es nativa: el ejecutable original del juego corre directamente en el procesador
del PC (la PS4 usa la misma arquitectura x86-64) y un runtime escrito para este port reemplaza las
bibliotecas de sistema de la PS4, así que no hay emulador de CPU. Los gráficos se traducen: corren
en el núcleo de vídeo de [shadPS4](https://github.com/shadps4-emu/shadPS4), que convierte los
comandos y shaders de la GPU de la PS4 a Vulkan. Este proyecto empezó como una copia del
[port de God of War III Remastered](https://github.com/FelipePrado0/gow3_pc) y se está adaptando
al motor de Ready at Dawn.

> **No se incluye ningún archivo del juego.** Necesitas un volcado de tu propia copia.
> Este proyecto no está afiliado a Sony Interactive Entertainment, Ready at Dawn ni Santa Monica
> Studio. The Order: 1886 es una marca de Sony Interactive Entertainment.

## Estado

Comprobado en Windows 11 con una NVIDIA GeForce RTX 5060 Laptop GPU y el volcado **CUSA00035,
versión 01.02** (`eboot.bin` sha256 `f742eb17…`):

- El juego pasa por los logos y la pantalla de título e inicia una partida nueva; la cinemática
  del prólogo se renderiza en el motor (personajes, iluminación y texturas revisados en volcados
  de fotogramas).
- Una sesión de 4 minutos del prólogo mantuvo los 30 FPS propios del juego en cada ventana de
  5 segundos, con un tiempo de fotograma en el percentil 99 de unos 34,5 ms, y ningún fallo en
  más de 20 ejecuciones automatizadas seguidas (`tools/batch_runs.py`).
- La capa de validación de Vulkan muestra una sola clase de aviso restante en estas escenas
  (`magFilter-04553`, un sampler lineal usado con una textura de enteros).

Aún sin comprobar: el juego después del prólogo, el sonido (las ejecuciones automatizadas no lo
comprueban), guardar y cargar, tarjetas AMD e Intel, otras regiones y versiones del juego, Linux.
El registro del trabajo, las correcciones y los pendientes están en
[docs/PROGRESS.md](docs/PROGRESS.md).

## Requisitos

- Windows 10 (1903 o más reciente) o Windows 11, 64 bits.
- Procesador de 64 bits con AVX. En CPUs sin MOVBE, BMI1 o LZCNT (Sandy/Ivy Bridge, AMD FX) el
  cargador reescribe las instrucciones del juego que las usan con rutinas emuladas y registra una
  línea `ISA:`; las CPUs más antiguas se detienen al iniciar con la línea `STOP: this CPU lacks`.
- Tarjeta gráfica con Vulkan 1.3 y controlador actualizado.
- El volcado de tu juego: la carpeta con `eboot.bin`, `sce_module` y `sce_sys`. Solo se probó
  CUSA00035 01.02; el archivo de parches también lista CUSA00070, CUSA00076, CUSA00100, CUSA00670
  y CUSA00785.
- [MSYS2](https://www.msys2.org) y [Python 3](https://www.python.org) para compilar (abajo).

## Compilación

1. Instala MSYS2 (`winget install MSYS2.MSYS2`) y, en el shell **MSYS2 CLANG64**, los paquetes de
   [packaging/windows/README.md](packaging/windows/README.md).
2. Clona con los submódulos y compila:

   ```bash
   git clone --recursive https://github.com/mateussmeneses/theorder1886-port
   cd theorder1886-port
   bash build.sh
   ```

   El resultado es `out/theorder-probe.exe` (también copiado como `out/theorder-probe.exe`). Si la
   compilación no puede reemplazar el ejecutable, todavía hay un proceso del juego en marcha.

## Jugar

La carpeta del juego se toma de `THEORDER_GAME_DIR`; sin ella, `run.py` busca una carpeta
`CUSA00035` junto a la carpeta del proyecto.

```powershell
$env:THEORDER_GAME_DIR = 'D:\ruta\CUSA00035'
python run.py
```

### Opciones del motor

Estas opciones existen en el ejecutable del juego y `run.py` las pasa. Su efecto en la versión
comercial aún no está comprobado: con `--level`, el juego sigue deteniéndose en la pantalla de
título.

| `run.py` | Opción del juego |
|---|---|
| `--level <nombre>` | `-startlevel <nombre>` (por ejemplo `cat_010_streets`, `emb_020_tunnels`, `air_010_rappel`, `pro_020_westminster`) |
| `--checkpoint <nombre>` | `-startcheckpoint <nombre>` |
| `--no-cloth` | `-disablecloth` |
| `--no-cam-springs`, `--no-cam-additive` | `-nocamsprings`, `-nocamadditive` |
| `--msaa <n>` | `-msaa <n>` |
| `--frontend` | `-frontend` |
| `--perf` | `-disablecloth -nocamsprings -nocamadditive -msaa 1` |

## Controles

Los mandos (DualSense, Xbox y otros) funcionan con SDL3, junto con el teclado.

| Tecla | Botón |
|---|---|
| WASD | stick izquierdo |
| Flechas | stick derecho (cámara) y cruceta |
| Espacio | Cruz |
| Enter | Options |
| Shift izquierdo o Esc | Círculo |
| E | Cuadrado |
| Q | Triángulo |
| 1 y 3 | L1 y R1 |
| R y F | L2 y R2 |
| Z y C | L3 y R3 |
| I, K, J, L | cruceta |
| Tab y Retroceso | clic izquierdo y derecho del panel táctil |
| Insert | el menú del port (R3+L2 en el mando) |
| Ratón | cámara; botón izquierdo R2 (disparar), derecho L2 (apuntar), central R3, laterales L1/R1 |
| F7 | activa/desactiva la cámara con el ratón |

En mandos sin panel táctil, View (Back) es el clic derecho del panel.
`THEORDER_KEY_MAP` y `THEORDER_PAD_MAP` cambian los botones (`src/runtime_pad.c`).

El menú del port (Insert) tiene las opciones de pantalla, campo de visión, ratón, límite de
fotogramas y la superposición de rendimiento. El campo de visión (50-200%) cambia al momento; por
encima de 100% muestra más por todos lados.

## Parches

`patches/TheOrder1886-Orbis.xml` se aplica en memoria al iniciar; los archivos del juego no se
modifican. Todos los parches vienen desactivados.

- **Frame Rate Patch - 60 FPS** (experimental): cambia las cuatro constantes de tiempo de
  fotograma de 30 FPS (33333 µs) del ejecutable 01.02 por 16667; los puntos se comprobaron en el
  binario. El juego además pide presentar a 30 Hz, así que el port debe presentar en cada vblank y
  limitar a 60:

  ```powershell
  Set-Content fps60.ini 'engine_fps=120'   # la lógica del lanzador recurre al parche de 60 FPS
  $env:THEORDER_CONFIG = (Resolve-Path fps60.ini)
  $env:THEORDER_FLIP_RATE = '0'; $env:THEORDER_FPS_LIMIT = '60'
  python run.py
  ```

  La velocidad del juego sigue siendo correcta (los volcados de fotogramas coinciden en el tiempo
  con una ejecución a 30 FPS). Logos y título se mantienen a 60 FPS; en una sesión de 240 s del
  prólogo cada ventana de 5 s quedó entre 41 y 60 FPS (57 de media), con las caídas donde el hilo de
  comandos de la GPU está ocupado todo el fotograma (600 a 700 draws por fotograma).
- **Aspect Ratio - 16:9 / 16:10 / 21:9** (lanzador y menú del juego: *Relación de aspecto*, al
  reiniciar): el juego dibuja una imagen 2.40:1 con franjas negras. Estos parches cambian la forma
  de la imagen (el float 2.4 en 0x44ce8c) y el factor 0.5625 con el que cada cámara convierte su
  campo de visión horizontal (definido para 16:9) en vertical, así que la vista conserva el ancho
  original y muestra más arriba y abajo (Vert+) en lugar de recortarse por los lados. El port
  muestra solo la imagen, ajustada a la ventana: las pantallas 16:10 y 21:9 también se llenan. La
  escena tiene más píxeles (1920x1080 en lugar de 1920x800 en 16:9); a 60 FPS en el prólogo (RTX
  5060 Laptop) eso costó alrededor de un 1% (57,8 frente a 58,3 FPS de media).
- Los demás parches escritos en sesiones anteriores se retiraron tras probarlos: saltar logos no
  cambiaba nada, cerrar la pantalla de título dejaba la pantalla en negro, los dos de saltar el
  prólogo no se pudieron comprobar, y los de 900p, 720p y de quitar las franjas negras escribían en
  medio de instrucciones (activarlos corrompía el código).

## Ajustes y diagnóstico

El núcleo gráfico lee estas variables de entorno:

| Variable | Efecto |
|---|---|
| `THEORDER_NO_MSAA=1` / `0` | imágenes del juego sin o con multisampling (con, por defecto, en NVIDIA) |
| `THEORDER_ASYNC_COMPUTE=0` / `1` | pipelines de compute nunca creados antes para este juego: siempre en el hilo de la GPU / siempre en segundo plano (los dispatches se saltan mientras tanto; los menús pueden perder textos e iconos). Por defecto: en segundo plano solo para programas que corren cada fotograma y no escriben imágenes |
| `THEORDER_PIPELINE_BINARY_CACHE_MB=N` | tamaño de la caché de binarios de pipeline (4096 por defecto); los más antiguos por encima se borran al iniciar |
| `THEORDER_LAZY_SHADER_CACHE=0` | vuelve a traducir todos los shaders en vez de cargar los de la caché |
| `THEORDER_FLIP_RATE`, `THEORDER_FPS_LIMIT` | tasa de presentación (ver el parche de 60 FPS) |
| `THEORDER_VK_VALIDATION=1` | capa de validación de Vulkan (con `VK_ADD_LAYER_PATH` apuntando a ella) |
| `THEORDER_GPU_LOG=info` | nivel del registro de la GPU |
| `THEORDER_PERMUTATION_LOG=1` | por qué se compiló cada nueva permutación de shader |
| `THEORDER_BIG_TRACK_LOG=1` | quién rastrea rangos de memoria de más de 256 MiB |
| `THEORDER_PAD_FILE=<archivo>` | entrada de mando programada para ejecuciones automatizadas |

Las partidas guardadas y la caché de shaders están en la carpeta `user` del proyecto
(`user/savedata`, `user/cache/CUSA00035`).

## Pruebas

```bash
python -m unittest discover -s tests          # 273 pruebas, todas pasan el 15/10/2026
python tools/batch_runs.py --runs 8 --secs 40 --pad "24:cross 28:cross 32:cross"
python tools/fault_stacks.py out/batch/*.log
```

`tools/batch_runs.py` inicia el juego varias veces, compara condiciones (`--cond`), pulsa botones
(`--pad`, para pasar la pantalla de título) e informa fallos, bloqueos y tiempo de compilación;
`tools/fault_stacks.py` simboliza los informes de fallo.

## Cómo funciona

| Parte | Dónde | Qué hace |
|---|---|---|
| Preparación | `scripts/` | Lee `eboot.bin`, aplica sus reubicaciones y enlaza la `libc` y la `libSceFios2` del propio juego en una sola imagen |
| Cargador | `src/probe.c` | Carga la imagen e inicia el código original del juego |
| Runtime | `src/runtime_*.c` | Reimplementa las funciones de sistema de la PS4: memoria, hilos, archivos, audio, vídeo, mando, partidas guardadas y servicios |
| GPU | `gpu/shadps4/` | El núcleo de vídeo de shadPS4 con las correcciones de cuesta4 y el trabajo de los ports de God of War III y The Order (compilación asíncrona de shaders y pipelines, caché de shaders) |
| Menú del port | `gpu/shim/port_overlay.cpp` | El menú Dear ImGui y la superposición de rendimiento |
| Parches | `patches/TheOrder1886-Orbis.xml`, `scripts/patches.py` | Se aplican en memoria al iniciar |

El lanzador de `launcher/` y el empaquetado para Linux de `packaging/linux/` vienen del port de
God of War III y aún no se adaptaron a este juego.

## Créditos

- **Igor Augusto y colaboradores de TOport**: [TOport](https://github.com/IgorAugustoHH/The-Order-1886-PC-Port) publicó anteriormente
  implementaciones adaptadas aquí para [profundidad facial](https://github.com/IgorAugustoHH/The-Order-1886-PC-Port/commit/c83f951),
  [normalización SRT de partículas](https://github.com/IgorAugustoHH/The-Order-1886-PC-Port/commit/c83f951),
  [sesgo de profundidad al redibujar](https://github.com/IgorAugustoHH/The-Order-1886-PC-Port/commit/fff668c) y
  [sombras EVSM/HTILE](https://github.com/IgorAugustoHH/The-Order-1886-PC-Port/commit/dfc66af).
  Estas soluciones se adaptaron y modificaron en este port; no son trabajo exclusivamente original de este repositorio.
  Consulta la [corrección de atribución de v0.7.3](docs/RELEASE-v0.7.3-ATTRIBUTION.md) (issue #69).

- **Felipe Prado**: el [port de God of War III Remastered para PC](https://github.com/FelipePrado0/gow3_pc)
  del que se copió este proyecto.
- **deadinside28**: [bloodborne_pc](https://github.com/deadinside28/bloodborne_pc), el port nativo
  original, y **Supermedo** (Mohammed Albarghouthi), su
  [fork para Windows](https://github.com/Supermedo/bloodborne_pc).
- **cuesta4**: el [fork de shadPS4](https://github.com/cuesta4/shadPS4/tree/eltutz) (rama `eltutz`)
  cuyas correcciones lleva el núcleo de vídeo.
- **Equipo de shadPS4**: el núcleo de vídeo de [shadPS4](https://github.com/shadps4-emu/shadPS4).

También se usan: [LibAtrac9](https://github.com/Thealexbarney/LibAtrac9), [FFmpeg](https://ffmpeg.org),
[SDL3](https://github.com/libsdl-org/SDL), [Dear ImGui](https://github.com/ocornut/imgui),
[sirit](https://github.com/shadps4-emu/sirit), [magic_enum](https://github.com/Neargye/magic_enum),
[miniz](https://github.com/richgel999/miniz), [xbyak](https://github.com/herumi/xbyak),
[Vulkan Memory Allocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator),
[FSR-Vulkan](https://github.com/FireBurn/FSR-Vulkan) y el AMD FidelityFX SDK,
[MSYS2](https://www.msys2.org), [LLVM](https://llvm.org) y [Pillow](https://python-pillow.org).

## Licencia

GNU GPL v2 o posterior ([LICENSE](LICENSE)). Los componentes de terceros conservan sus propias
licencias.
