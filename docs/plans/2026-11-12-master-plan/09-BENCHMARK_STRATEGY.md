# 09 — Estratégia de benchmark, estabilidade e regressão

Princípio (já em `AGENTS.md`): toda afirmação de melhora nomeia rota, estado de cache, número de
execuções e build; primeira visita e revisita morna são medições separadas; nenhum draw visível é
descartado para melhorar número.

## 1. O que já existe

| Peça | Onde | Cobre |
|---|---|---|
| A/B intercalado com rota de pad, perfis isolados, afinidade | `tools/batch_runs.py` | Tempo de frame por janela de 5 s, falhas, contagem de compilações |
| Análise | `tools/bench/analyze.py`, `pipeline_chain_compare.py`, `permutation_gaps.py`, `retest_combine.py` (intervalos de Wilson) | Percentis, misses repetidos entre sessões |
| Guarda de recursos | `tools/bench/memwatch.ps1` | Commit do sistema e bytes privados a cada 2 s |
| Flicker | `tools/flicker_scan.py` (padrão A-B-A, validado com objeto plantado) | Flicker de um frame em câmera parada |
| Comparação de capturas | `tools/regression_report.py` | Diferença entre dumps de frame |
| Smoke de pacote | `tools/smoke_package.py` | Frio e morno, em toda release |
| Soak | `tools/soak.sh` | Sobrevivência com rotação de câmera |
| Referência externa | TOport compilado em `C:\tb\toport` | Mesma rota, mesma máquina |

## 2. Lacunas

1. **Uma rota só** (prólogo: tutorial de cobertura + abertura). Nenhum cenário de combate,
   Whitechapel, capítulos 3, 5, 8, 9 — onde estão os relatos.
2. **Métrica só interna.** Tempo de frame vem do log do próprio port; não há medição do frame
   apresentado.
3. **Uma GPU** (RTX 5060 Laptop). Sem AMD/Intel.
4. **Sem baseline por release** num formato comparável: `PERFORMANCE_BASELINE.md` é uma série de
   experimentos, cada um com seu protocolo.
5. **Disco:** 16 GB livres; cada execução morna copia ~1 GB.

## 3. Cenários (S)

Sem baseline não há meta numérica: a tabela define **o que medir**; os valores entram na fase F0.

| ID | Cenário | Como chegar | Estado |
|---|---|---|---|
| S1 | Menu 3D do título, 60 s | boot | Disponível |
| S2 | Prólogo: tutorial de cobertura (câmera parada) | rota de pad `24..60 s: cross` | Disponível |
| S3 | Prólogo: cutscene de abertura | mesma rota | Disponível |
| S4 | Whitechapel, combate (#95, #30) | **depende de F1** (`-startlevel` ou save) | Bloqueado |
| S5 | Capítulo 3, porta (#118) | F1 | Bloqueado |
| S6 | The Atrium, fumaça de granada | F1 | Bloqueado |
| S7 | Capítulos 8/9 (#63, #11) | F1 | Bloqueado |
| S8 | Sessão longa: S2→S3→gameplay, 30 min | F1 para ir além do prólogo | Parcial |

Cada cenário tem um arquivo de definição versionado (`tools/bench/scenarios/<id>.json`: argumentos
do jogo, rota de pad, duração, janela de medição, configurações fixas).

## 4. Condições controladas

- Build identificado pelo hash do commit **e** pelo sha256 do executável, gravados no manifesto.
- Configuração fixa: 1080p, janela, vsync ligado, limite 60, upscaler desligado, patch de FPS
  declarado. Um `.ini` de benchmark versionado, nunca o do usuário.
- Estado de cache declarado: **frio** (perfil só com `savedata`), **morno-1** (perfil da sessão
  fria), **morno-2**. Nunca "desconhecido".
- Perfil isolado por execução (`--user-template`); nunca tocar em `user/`.
- Condições intercaladas execução a execução (já é o comportamento de `batch_runs`).
- Driver, GPU, Windows e energia (tomada, modo de desempenho) no manifesto; em notebook, registrar
  a GPU ativa.
- **n ≥ 3 por condição** para qualquer afirmação em nota de release; n=1 só com o rótulo "n=1".

## 5. Métricas

| Métrica | Fonte | Observação |
|---|---|---|
| FPS médio, P50/P95/P99/P99.9 de tempo de frame, pior frame | log interno **e** PresentMon (R1) | Divergência entre as duas fontes é um achado, não ruído |
| 1% low | derivado do P99 de tempo de frame apresentado | Reportar junto com o P99, não no lugar |
| Janelas de 5 s com frame > 50 / > 100 ms | `batch_runs` | Métrica de stutter já usada na comparação com o TOport |
| Compilações de pipeline na thread de GPU, builds de driver, misses de binário | contadores internos | Explicam os stalls; não são tempo de parede exclusivo |
| CPU por thread | Tracy (R2) ou `analyze.py` | Para #40 |
| GPU %, VRAM do processo | contadores WDDM (`MEMORY_PROFILING.md`) | — |
| RAM: bytes privados, commit do sistema | `memwatch.ps1` | Inclinação ao longo de S8 |
| Tempo de carregamento | boot → primeiro frame apresentado; checkpoint → controle | Precisa de marcador no log (tarefa T-006) |
| Artefatos | `flicker_scan` (A-B-A), `regression_report` contra captura de referência | Só em câmera parada/determinística |
| Estabilidade | taxa de falha por lote com intervalo de Wilson; sobrevivência em S8 | Já implementado em `retest_combine.py` |

## 6. Linha de base e comparação entre versões

1. **Baseline por release:** a cada tag, `tools/bench/baseline.py` (T-005) roda S1–S3 em frio,
   morno-1 e morno-2, n=3, e grava `docs/baselines/<tag>.json` (pequeno, versionado) com resumo
   por cenário. Os dados brutos ficam em `out/` e são apagados.
2. **Comparação:** `baseline.py --compare <tagA> <tagB>` produz a tabela usada na nota de release.
3. **Critério de regressão (a calibrar na F0):** primeiro medir a variância entre execuções
   idênticas (A/A, n=5) em cada cenário; o limiar de regressão é definido a partir dela. Antes
   disso, qualquer limiar seria arbitrário.
4. **Referência externa:** repetir S2/S3 no TOport a cada baseline, enquanto ele compilar.

## 7. Compatibilidade entre GPUs sem ter o hardware

- **Kit do relator (T-012):** um botão no launcher que roda S1–S3 com o `.ini` de benchmark,
  junta manifesto, `vulkaninfo`, contadores e duas capturas de frame de pontos fixos no ZIP do
  `port_report.py`. O relator anexa um arquivo; o Haiku lê.
- **Builds de teste por issue (T-010):** fecha o ciclo em AMD/Intel sem release.
- **Matriz** `docs/GPU_MATRIX.md`: fornecedor, GPU, driver, build, cenário, resultado, issue.
  Alimentada só por kits recebidos.
- **ICD de software no CI (T-013):** pega erro de uso da API e de validação, não bug de driver.

## 8. Regressão automatizada

| Camada | O quê | Quando |
|---|---|---|
| Unidade (existente) | 137 arquivos de teste | Todo PR (`gate`) |
| GPU em ICD de software (novo) | testes de dispositivo hoje pulados no CI | Todo PR, depois de T-013 |
| Smoke de pacote (existente) | frio + morno | Toda release |
| Baseline (novo) | S1–S3 × 3 estados × n=3 | Toda release e todo PR com rótulo `perf` ou que toque `vk_pipeline_cache`/`vk_rasterizer` |
| Imagem (novo) | captura de frame de ponto fixo vs. referência, tolerância calibrada | Depois de F1; depende de cena determinística |
| Soak (existente, a estender) | S8, 30 min, inclinação de memória | Antes de release |

## 9. Validação e rollback de cada mudança

- Toda mudança de comportamento do renderer/runtime entra **atrás de uma variável `THEORDER_*`**
  com o valor antigo disponível (prática já usada: `THEORDER_PARTICLE_SRT_FIX=0`,
  `THEORDER_VISIBLE_PIPELINE_PRIORITY=0`). O A/B usa a própria variável como condição.
- Um PR por mudança lógica: reverter é `git revert` de um merge.
- Contador ou linha de log de ativação, verificado numa execução real (regra "provar ativação").
- A variável de rollback só é removida depois de uma release inteira sem relato contrário.

## 10. Orçamento de disco e tempo

Um baseline completo = 3 cenários × 3 estados × 3 execuções ≈ 27 execuções de ~150 s (≈ 70 min) e
até ~18 GB se os perfis mornos forem mantidos — **não cabe no disco atual**. `baseline.py` deve
apagar cada perfil ao terminar a cadeia frio→morno-1→morno-2 e manter só o JSON de resumo.
[PENDENTE] Liberar espaço em disco ou apontar `out/` para outro volume antes da F0.
