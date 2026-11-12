# 01 — Auditoria técnica (2026-11-12, `master` bc9a70a2, Windows v0.9.0)

Convenção usada nos nove documentos deste plano:
**[FATO]** verificado no código, no git, em issue ou em medição registrada ·
**[HIPÓTESE]** explicação plausível ainda não demonstrada ·
**[RECOMENDAÇÃO]** proposta deste plano · **[PENDENTE]** decisão do mantenedor.

Fontes: leitura do repositório local, `gh issue list`, `docs/STATUS.md`, `docs/PROGRESS.md`,
`docs/plans/*`, `docs/PERFORMANCE_BASELINE.md`, `git ls-tree spartan/main`. Nada foi compilado nem
executado nesta auditoria; nenhum número novo de desempenho foi medido.

## 1. Arquitetura atual

| Camada | Onde | Tamanho | Papel |
|---|---|---|---|
| Runtime do guest (C) | `src/` | 10,7 mil linhas | Executa o `eboot.bin` x86-64 original do PS4: memória (`runtime_memory.c`), threads/locks, arquivos, pad, áudio (AJM/MP3/AudioOut), savedata, troféus, reescrita de ISA para CPUs antigas (`isa_rewrite.c`, `isa_emulate.c`), tratamento de falhas (`probe.c`: VEH + filtro de exceção) |
| Núcleo de vídeo (C++) | `gpu/shadps4/` | ~32 mil linhas só em renderer/caches | Fork embutido do video core do shadPS4: Gnm → Vulkan, `shader_recompiler` (GCN → SPIR-V em tempo de execução), caches de textura/buffer/pipeline |
| Shim do port | `gpu/shim/` | — | Overlay ImGui (`port_overlay.cpp`), configurações (`port_settings.cpp`), estatísticas, correções específicas (`bb_particle_srt.h`, `bb_face_depth_fix.cpp`), i18n do menu |
| Preparação e execução | `run.py`, `scripts/` | ~3 mil linhas Python | Converte eboot em imagem, liga libc/módulos, aplica patches (`patches/TheOrder1886-Orbis.xml`), mescla update, mods |
| Launcher | `launcher/port_launcher_win.py` | 2 785 linhas (Tk, PyInstaller) | Configuração, backups de save, verificação e instalação de atualização, relatório de diagnóstico (`port_report.py`) |
| Ferramentas | `tools/` | 44 arquivos | `batch_runs.py` (A/B intercalado com rota de pad), `tools/bench/*`, `flicker_scan.py`, `smoke_package.py`, `release.sh`, `soak.sh` |
| Testes | `tests/` | 137 arquivos | C, C++ e Python; `tools/run_tests.sh` com pré-checagem de dispositivo Vulkan |
| CI | `.github/workflows/` | 14 workflows | `gate` obrigatório (`windows-native-validation.yml`: build CLANG64 + testes), ciclo de vida de issues |

[FATO] O port é uma **reconstrução de runtime** (HLE das bibliotecas do PS4 em volta do binário
original), não uma recompilação estática: o código do jogo roda nativo porque PS4 e PC são x86-64.
Isso define o que é transferível de outros projetos (ver doc 03).

[FATO] Configuração em três camadas: `theorder.ini` (57 chaves, menu in-game e launcher),
`settings.json` (launcher) e **263 variáveis `THEORDER_*`** distintas no código. Não há um
catálogo único que ligue chave → variável → efeito; o SpartanPort tem (`core/catalog` +
`tests/env_contract.rs`).

## 2. Estado por subsistema

| Subsistema | Estado comprovado | Lacuna |
|---|---|---|
| Pipeline/shader cache | [FATO] Cache de permutações e pipelines em disco, pipeline binaries, cache de driver com cabeçalho validado, prioridade para pipeline de draw visível, compilação assíncrona. Após o ajuste do SRT de partículas (v0.8.3): janelas >100 ms na rota do prólogo 15/7/7 → 3/1/1; builds de driver após a 1ª sessão 38 → 0 (`reference-ports-review.md`) | Medido em **uma rota** (prólogo). Primeira visita de cada área ainda compila; não há pré-compilação. Cache de driver salvo fica com 60 bytes no modo de pipeline binaries (#102) |
| Upscaler temporal | [FATO] Código de FSR 3.1/4, TAA, DLSS e frame generation presente | [FATO] **Não executa** em CUSA00035 01.02 nas cenas testadas (#138): o gatilho `0x9a9cf8a9` nunca é despachado, o G-buffer tem 3 alvos (heurística exige 5+) e as constantes de câmera de 864 bytes não aparecem. Toda a linha de upscaling/FG está bloqueada por isso |
| Correção gráfica | [FATO] Longa cadeia de correções (sombras solares, HTILE, barreiras SSBO, DOF, overlap color/depth) | [FATO] Flicker (#95/#99/#30/#29), tela preta (#98/#116/#47/#24/#43), artefatos verde/azul (#63/#39), fumaça de granada ausente: **não reproduzidos na rota disponível**; faltam cenas além do prólogo |
| Memória | [FATO] Commit sob demanda (v0.9.0), guarda de RAM, handles < 1 TiB | Sem medição de sessão longa por capítulo; `MEMORY_PROFILING.md` cobre só o menu |
| Threads/CPU | [FATO] Callback de idle nativo para job workers (v0.8.0), `SwitchToThread` no yield | #40 (CPU 100%) sem telemetria por thread atribuída |
| Entrada | [FATO] Mouse normalizado por tempo, até 4 teclas por botão, menu navegável por d-pad | #37/#38 aguardam reteste; prompts de teclado (#34) exigem RE dos assets |
| Atualização | [FATO] O launcher baixa o ZIP da última release e copia por cima (`install_update`) | [FATO] **Sem verificação de sha256** (o `.sha256` é publicado por `release.sh`, mas não é lido), cópia in-place não atômica, sem rollback, arquivos removidos numa versão permanecem, sem canal de pré-release |
| Diagnóstico | [FATO] `port_report.py` (ZIP redigido), `THEORDER_VK_VALIDATION`, `THEORDER_VK_CRASH_DIAGNOSTIC`, dumps de frame, `SLOW_WAITS`, `fault_stacks.py` | [FATO] Tracy é um **stub** (`gpu/shim/tracy/Tracy.hpp`: macros vazias). Sem minidump. Sem medição de frame apresentado independente do log interno |
| Build/distribuição | [FATO] `tools/release.sh` faz build, testes, pacote, smoke frio/morno, sha256, tag e release — **na máquina do mantenedor** | [FATO] Nenhum workflow constrói o pacote; não há builds de teste por issue |
| Testes/CI | [FATO] 137 arquivos de teste; `gate` obrigatório | [FATO] Runner hospedado não tem ICD Vulkan (`WINDOWS_NATIVE_CI.md`): testes de GPU só rodam localmente. `runtime-test` trava no Windows e está fora do gate |

## 3. Evolução dos problemas pedidos na investigação

| Tema | Histórico (fonte) | Situação real hoje |
|---|---|---|
| Texture flickering / vertex explosions | Classes A/B/C corrigidas em 2026-10-20 (layout de depth, espera por pipeline visível); D/E/F ficaram como hipótese (`PROGRESS.md` l. 653). Correções do cache de textura do SpartanPort entraram na v0.9.0 | [FATO] Zero flicker de objeto no tutorial de cobertura nos dois ports (`flicker_scan.py`). [PENDENTE] Cenas dos relatos (combate, Whitechapel) nunca foram alcançadas por automação |
| Sombras e iluminação | Sombras solares: cadeia de correções verificada em RDNA4 em 2026-11-06 (`GRAPHICS_REGRESSIONS.md`) | #26 em `needs-retest`; #36/#45 sem captura equivalente |
| Microstuttering | Causa raiz das permutações extras achada e corrigida (SRT de partículas) | Sessões seguintes: 0–1 janela >100 ms no prólogo. Primeira visita por área: não medida fora do prólogo |
| Picos de CPU | Idle nativo dos workers; A/B sob afinidade restrita (`AFFINITY_STABILITY_RETEST.md`) | #40 aberto, sem dado do relator |
| NVIDIA/AMD/Intel | Correções pontuais por fornecedor (MSAA off em Intel, alinhamento 16-bit AMD, barreira SSBO NVIDIA) | [FATO] Só há hardware NVIDIA local (RTX 5060 Laptop). AMD/Intel dependem de relatores |
| Sessões prolongadas | `soak.sh` existe; guarda de RAM | Sem série registrada de sessões longas por capítulo; #11 (cap. 9) e #118 (cap. 3) abertos — o relatório anexado ao #118 **ainda não foi lido** (`STATUS.md`) |
| Mouse e menus | Reescrita por tempo (v0.7.8), pausa com menu aberto, d-pad no menu (v0.9.0) | Aguardando retestes |

## 4. Achados estruturais (o que limita o próximo salto)

1. **[FATO] Cobertura de cena é o gargalo nº 1.** Toda medição e todo A/B usam a rota do prólogo.
   `-startlevel`/`-startcheckpoint` chegam ao guest, mas o jogo continua na tela-título
   (`ENGINE_RE.md`: lidos por `0x447880` apenas enquanto `*(*(0x16b31d8)+0x208) == 0`). Quase
   todas as issues P0 exigem cenas que a automação não alcança.
2. **[FATO] O upscaler temporal está inativo** no SKU mais testado; FG, FSR 4.1, DLSS e os itens
   de upscaling do SpartanPort dependem de localizar câmera e passes na 01.02.
3. **[FATO] Não há instrumentação de perfil real** (Tracy é stub) nem métrica de apresentação
   externa; o diagnóstico de stutter depende de logs próprios.
4. **[FATO] Um único vetor de hardware.** Sem AMD/Intel local e sem ICD de software no CI, toda
   correção por fornecedor é validada só por relator.
5. **[FATO] Distribuição depende de uma máquina** e o atualizador não verifica integridade.
6. **[FATO] Disco local com 16 GB livres** (99 % usado); cada execução morna de `batch_runs`
   copia ~1 GB. Limita o tamanho dos lotes de benchmark.
7. **[FATO] `docs/BACKLOG.md` mistura estado atual com um snapshot superado** que marca #26, #29
   e #30 como "Resolvido" enquanto as issues seguem abertas. Risco de um executor ler o bloco errado.

## 5. O que já existe e não deve ser reproposto

Cache de pipelines/permutações em disco, pipeline binaries, prioridade de pipeline visível,
compilação assíncrona, FSR 3.1/FSR 4/TAA/DLSS (código), frame generation FSR 3.1 (código),
resoluções até 4K e ultrawide 21:9/32:9, 60/120 FPS por patch, correção de QTE, menu in-game com
pausa/tradução/áudio, 5.1/7.1, troféus persistentes, até 4 teclas por botão, backups de save,
mods, atalho Steam, verificação de atualização, relatório de diagnóstico redigido, guarda de RAM,
caminho para CPUs sem AVX2/F16C, perfil `recomplauncher.json`, ciclo de vida de issues automatizado,
revisão em dois eixos por PR.
