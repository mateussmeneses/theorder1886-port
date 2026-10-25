# The Order: 1886 v0.7.3 — attribution correction / correção de créditos

**Related report:** [issue #69](https://github.com/mateussmeneses/theorder1886-port/issues/69)  
**Affected release:** [windows-v0.7.3](https://github.com/mateussmeneses/theorder1886-port/releases/tag/windows-v0.7.3) (2026-10-19)

## Português (Brasil)

A descrição original da v0.7.3 não informou adequadamente a procedência de algumas correções gráficas. Atribuímos ao **TOport, de Igor Augusto e seus colaboradores**, as implementações públicas anteriores que serviram de base para as seguintes adaptações neste repositório:

| Área | Trabalho anterior do TOport | Uso/adaptação neste port |
|---|---|---|
| Profundidade facial / manchas no rosto | [c83f951](https://github.com/IgorAugustoHH/The-Order-1886-PC-Port/commit/c83f951) (13/10/2026) | `gpu/shim/bb_face_depth_fix.cpp`, `bb_face_depth_fix.h`, `bb_face_depth_spirv.h` e integração no renderer |
| Normalização SRT de partículas e efeitos | [c83f951](https://github.com/IgorAugustoHH/The-Order-1886-PC-Port/commit/c83f951) (13/10/2026) | `gpu/shim/bb_particle_srt.h` e integração ao cache de pipelines |
| Depth bias em redesenhos | [6bbcc57](https://github.com/IgorAugustoHH/The-Order-1886-PC-Port/commit/6bbcc57) e [fff668c](https://github.com/IgorAugustoHH/The-Order-1886-PC-Port/commit/fff668c) (14/10/2026) | `gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp` (adaptado e com configuração `THEORDER_*`) |
| Conversão EVSM / HTILE para sombras | [dfc66af](https://github.com/IgorAugustoHH/The-Order-1886-PC-Port/commit/dfc66af) (14/10/2026) | `gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp` (adaptado) |

Estas correções **não devem ser apresentadas como descobertas ou implementação exclusivamente originais deste port**. Houve modificações e integrações posteriores específicas deste repositório; isso não elimina a atribuição ao trabalho anterior do TOport.

A atribuição não implica que todos os defeitos visuais tenham sido resolvidos em todas as configurações ou GPUs.

A documentação, os créditos e os comentários nos arquivos-fonte passam a registrar a procedência. A licença dos componentes continua sendo [GPL-2.0-or-later](../LICENSE), com avisos de autoria de terceiros preservados.

## English

The original v0.7.3 release description did not adequately acknowledge the provenance of certain graphics fixes. We credit **Igor Augusto and the TOport contributors** for the earlier public implementations on which this port's adaptations were based:

- **Facial-depth mitigation:** [TOport c83f951](https://github.com/IgorAugustoHH/The-Order-1886-PC-Port/commit/c83f951), adapted in `gpu/shim/bb_face_depth_fix.cpp`, its associated headers, and renderer integration.
- **Particle SRT normalization:** [TOport c83f951](https://github.com/IgorAugustoHH/The-Order-1886-PC-Port/commit/c83f951), adapted in `gpu/shim/bb_particle_srt.h` and pipeline-cache integration.
- **Redraw depth bias:** [TOport 6bbcc57](https://github.com/IgorAugustoHH/The-Order-1886-PC-Port/commit/6bbcc57) and [fff668c](https://github.com/IgorAugustoHH/The-Order-1886-PC-Port/commit/fff668c), adapted in the Vulkan rasterizer.
- **EVSM/HTILE shadow conversion:** [TOport dfc66af](https://github.com/IgorAugustoHH/The-Order-1886-PC-Port/commit/dfc66af), adapted in the Vulkan rasterizer.

These fixes **must not be represented as solely original discoveries or implementations of this port**. Subsequent changes and integration in this repository do not remove the attribution to TOport's prior work. Attribution is not a claim that all visual issues are fixed on all hardware.

This correction is maintained publicly in the repository, alongside source-file notices and README credits. It records attribution for the v0.7.3 announcement without altering the historical commit record.
