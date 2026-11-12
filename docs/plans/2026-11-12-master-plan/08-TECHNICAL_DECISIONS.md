# 08 — Registro de decisões técnicas

Documento central de decisões deste plano. Uma entrada nova por decisão; não reescrever as
antigas — marcar como "substituída por D-xx". Estado: **Proposta** (aguarda o mantenedor),
**Aceita**, **Herdada** (já vigente no repositório), **Substituída**.

| ID | Decisão | Estado |
|---|---|---|
| D-01 | Manter a arquitetura de reconstrução de runtime | Herdada |
| D-02 | Manter o launcher Python | Herdada |
| D-03 | Visibilidade antes de correção | Aceita |
| D-04 | Chegar às cenas por `-startlevel`; saves como alternativa | Aceita |
| D-05 | Stutter: pré-compilar no carregamento, não descartar draws | Proposta |
| D-06 | Atualizador próprio em Python, transacional | Proposta |
| D-07 | Subagentes com modelo fixo e worktrees; sem Agent Teams | Aceita |
| D-08 | Toda mudança de comportamento atrás de variável reversível | Herdada, formalizada |
| D-09 | Distribuir só chaves de pipeline, nunca shaders derivados; lista dentro do pacote | Aceita |
| D-10 | Sem metas numéricas antes do baseline | Proposta |
| D-11 | Validação entre GPUs por relatores, com kit e builds por issue | Proposta |
| D-12 | Upscaler: especificar por RE antes de mexer no renderer | Proposta |
| D-13 | Documentos do plano em `docs/plans/2026-11-12-master-plan/` | Aceita |

---

### D-01 — Manter a arquitetura de reconstrução de runtime
**Contexto:** o guest é x86-64; o eboot original roda sobre o runtime C e o video core do shadPS4.
**Decisão:** não migrar para recompilação estática nem para decompilação.
**Justificativa:** as ferramentas de recompilação estudadas (N64Recomp, XenonRecomp, ReXGlue…)
resolvem tradução de ISA, problema que não temos. O único projeto do catálogo com o mesmo perfil
(Uncharted NDC) usa esta mesma arquitetura.
**Alternativas descartadas:** recompilação estática do eboot (sem ganho: já é nativo);
decompilação (anos de trabalho, sem fonte).

### D-02 — Manter o launcher Python
**Contexto e justificativa:** `docs/plans/2026-11-12-launcher-decision.md`.
**Reavaliar quando:** os módulos específicos do launcher Rust do SpartanPort virarem dados, ou o
empacotamento com PyInstaller se tornar um problema (por exemplo, falsos positivos de antivírus
recorrentes).

### D-03 — Visibilidade antes de correção
**Contexto:** uma rota, uma GPU, métrica só interna; issues P0 em cenas não alcançadas.
**Decisão:** a Onda 1 (F0, F1, F6) vem antes de novo trabalho de correção gráfica especulativa.
**Justificativa:** o histórico mostra o custo de corrigir sem reproduzir — a normalização do SRT
de partículas ficou várias releases no código sem nunca ser aplicada; issues marcadas como
resolvidas seguem abertas.
**Alternativa descartada:** continuar portando correções candidatas e esperar retestes. É barato
por PR, mas sem cena não há como provar ativação.
**Custo assumido:** um marco (v0.10.0) sem promessa de correção gráfica visível.

### D-04 — Chegar às cenas por `-startlevel`; saves como alternativa
**Decisão:** investir primeiro em RE do caminho de nível direto (T-020); se o retail não permitir,
biblioteca de saves por checkpoint (T-021).
**Justificativa:** o caminho por argumento é determinístico, não depende de terceiros e não exige
guardar saves; a evidência de que o parser existe já está em `ENGINE_RE.md`.
**Risco:** estado inicial diferente do de um jogador. Mitigação: confirmar cada achado importante
com um save real antes de fechar.
**Alternativas descartadas:** rota de pad jogando do início (frágil e longa); só pedir vídeos aos
relatores (não permite A/B).

### D-05 — Stutter: pré-compilar no carregamento, não descartar draws
**Decisão:** atacar a primeira visita com prefetch de área e lista de chaves (padrões P1/P2 do
doc 03), mantendo a espera por pipeline em draw visível.
**Justificativa:** descartar draws foi a origem do flicker de primeira visita ("Classe C");
`AGENTS.md` proíbe descartar draw visível para melhorar métrica. O Unleashed Recompiled mostra
que pré-compilação no carregamento resolve sem artefato.
**Alternativas descartadas:** `THEORDER_PENDING_DRAWS=skip` como padrão (artefato);
`VK_EXT_graphics_pipeline_library` (esforço grande, risco por fornecedor, GPU mais lenta até a
versão otimizada); ubershader (inviável para shaders arbitrários de PS4).
**Portão:** só executar T-051/T-052 se a T-050 medir o problema nas áreas novas.

### D-06 — Atualizador próprio em Python, transacional
**Decisão:** evoluir o atualizador existente (sha256, staging, backup, rollback, manifesto) em vez
de adotar Velopack/Squirrel/WinSparkle.
**Justificativa:** distribuição por ZIP portátil com dados do usuário dentro da pasta; os
frameworks pressupõem layout próprio (`current/` + `Update.exe`) e migração de todos os usuários;
dois ports comparáveis fizeram o mesmo num launcher equivalente.
**Alternativa descartada:** apenas notificar e abrir a página (mais simples; perde o que já existe).
**Pendência:** sem assinatura de código, o SmartScreen continua avisando.

### D-07 — Subagentes com modelo fixo e worktrees; sem Agent Teams
**Decisão e justificativa:** doc 05 §1.
**Alternativas descartadas:** Agent Teams (experimental, custo maior, sem `/resume`, sem ganho
para tarefas de briefing fechado); uma sessão única trocando de modelo (perde paralelismo e
mistura contextos).
**Reavaliar quando:** Agent Teams sair do estado experimental.

### D-08 — Toda mudança de comportamento atrás de variável reversível
**Decisão:** formalizar a prática: variável `THEORDER_*` com o valor antigo, contador de ativação,
remoção só depois de uma release sem relato contrário.
**Custo assumido:** mais variáveis (já são 263). Mitigação: catálogo da F7 com dono e data de
remoção prevista.

### D-09 — Distribuir só chaves de pipeline, nunca shaders derivados
**Decisão:** uma eventual lista de pré-compilação distribuída contém apenas hashes e estados.
**Justificativa:** SPIR-V traduzido dos shaders do jogo é obra derivada do conteúdo do jogo; o
repositório não distribui arquivos do jogo, caches nem dumps.
**Pendência do mantenedor:** se a lista entra no pacote ou é baixada à parte; para quais
SKUs/versões é gravada.

### D-10 — Sem metas numéricas antes do baseline
**Decisão:** os critérios de aceitação de desempenho são "melhor que o baseline, fora do ruído
A/A, n ≥ 3"; limiares absolutos só depois da F0.
**Justificativa:** as únicas séries comparáveis são do prólogo numa GPU.

### D-11 — Validação entre GPUs por relatores, com kit e builds por issue
**Decisão:** não tentar inferir correção em AMD/Intel a partir de NVIDIA; investir no canal (kit,
builds de teste, matriz) e em ICD de software para erros de uso da API.
**Alternativa descartada:** comprar ou alugar hardware — fora do alcance deste plano; [PENDENTE]
do mantenedor.

### D-12 — Upscaler: especificar por RE antes de mexer no renderer
**Decisão:** T-040 produz um documento de entradas temporais da 01.02; T-041 só começa com ele.
**Justificativa:** baixar o limiar de 5 para 3 alvos já foi tentado e não basta; as heurísticas
foram herdadas de outro jogo.
**Alternativa descartada:** ajustar heurísticas por tentativa no renderer.

### D-13 — Local dos documentos
**Decisão:** os nove entregáveis ficam em `docs/plans/2026-11-12-master-plan/`, seguindo a regra
de `AGENTS.md` (planos em `docs/plans/`, arquivados quando concluídos). `docs/STATUS.md` continua
sendo a página única de estado.

---

## Respostas do mantenedor (2026-11-12)

1. PresentMon, Tracy (submódulo), RenderDoc e Mesa lavapipe no CI: **aprovados**.
2. Disco: pode usar o segundo SSD (`D:`, 259 GB livres) e apagar material antigo do The Order.
   `out/` de medição passa a apontar para `D:` (T-005).
3. DownpourRecomp: link fornecido e analisado (doc 03). God of War Recomp: link desconhecido.
4. Saves por checkpoint como alternativa à T-020: aceito.
5. Lista de chaves de pipeline: "a melhor opção" → **dentro do pacote**, um arquivo por
   SKU/versão do jogo, só hashes e estados (D-09). Motivo: funciona offline e na primeira
   execução, que é exatamente o caso a resolver; o DownpourRecomp distribui ~1.370 PSOs
   pré-aquecidos do mesmo modo. Um arquivo ausente ou de outra versão é ignorado.
6. `.claude/agents/`: criado (cinco papéis do doc 05 §4).
7. Leitura do código de referência (Uncharted NDC e os demais projetos estudados): liberada.

Pendência que resta: assinatura de código do executável (D-06) e hardware AMD/Intel (D-11).
