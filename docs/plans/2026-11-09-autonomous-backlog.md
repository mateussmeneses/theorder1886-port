# Plano de atuação autônoma — backlog pós-v0.8.0 (2026-11-09)

Plano para uma sessão longa sem o usuário presente. Cada item tem um critério de aceite
verificável nesta máquina; o que depende de hardware ou de reporter fica registrado como
pendente, nunca como resolvido. O andamento é registrado na seção "Diário" no fim deste
arquivo, item por item.

## 1. Estado de partida

- `master` local = `e3b0289` (4 commits à frente de `origin/master` = `4c9db6d`).
- Correção do `WaitGraphics` commitada (`922a4bb`) e **já validada**: reteste 4P 20 + 20 em
  `docs/AFFINITY_STABILITY_RETEST.md` (19/20 + 19/20, 0 `bad_alloc`, 0 falha `+0x126ce4`).
- WIP do usuário, intocável: `src/probe.c` (legendas), `src/runtime_file.c`,
  `src/runtime_kernel.c`, submódulo `gpu/third_party/fsr-vulkan`. O WIP de `runtime_file.c`
  dá tela preta, então toda medição roda no worktree limpo `../theorder1886-port-wg`.
- Host: i7-14650HX, RTX 5060 Laptop (NVIDIA 621.14), ~39 GB livres em C:, limite de commit
  51,6 GB. Jogo: `C:\games\CUSA00035` 01.02.
- GitHub: 38 issues abertas (eram 28 no `docs/BACKLOG.md` de 2026-10-25).

## 2. Regras da sessão

Permitido sem perguntar:
- Editar, compilar, testar e commitar **localmente** na branch de trabalho
  `auto/backlog-20261109` (worktree `../theorder1886-port-wg`).
- Rodar o jogo pelo `tools/batch_runs.py` com perfis isolados e cópia do cache.
- Leitura do GitHub (`gh issue view/list`) e análise estática do eboot (REA/Ghidra).

Proibido nesta sessão:
- `git push`, tags, releases, comentários/fechamento de issues, qualquer ação pública.
  Respostas a reporters ficam como rascunho em `docs/`.
- Tocar no WIP do usuário, em `user/`, saves, caches reais ou na pasta do jogo.
- Trocar default, remover wait/draw/sincronização ou "corrigir" gráfico com override amplo
  sem A/B pareado (mesma rota, mesmo estado de cache, percentis crus).
- Declarar issue resolvida por commit, smoke local ou inferência.

Segurança de recursos:
- Antes de cada lote: ≥ 20 GB livres em C: e nenhum `theorder-probe` rodando; depois de cada
  lote, apagar só os perfis/cópias criados pelo próprio lote (manter `frames.csv`, logs,
  manifests).
- Guarda de memória (`memwatch.ps1`) nos lotes; parar se commit do sistema encostar no limite.

Portão por item: teste focado → `bash tools/run_tests.sh` completo no worktree (shell MSYS2
CLANG64) → commit na branch. Item que não fecha em ~3 tentativas com hipóteses distintas é
registrado como aberto, com a evidência coletada, e a sessão segue para o próximo.

Integração: ao fim de cada fase com suíte verde, `master` local recebe fast-forward da branch
(os arquivos de WIP não são tocados pelos itens; se houver sobreposição, não integra e
registra). Nada é publicado.

## 3. Backlog

Ordem = prioridade de execução. `[L]` verificável localmente; `[R]` depende de
reporter/hardware externo (só triagem e instrumentação).

### Fase 0 — Higiene (rápido, sem risco)

| ID | Item | Aceite |
|---|---|---|
| H1 | `docs/AUTONOMOUS_CONTINUATION_REPORT.md` foi commitado com 3 marcadores não preenchidos (`CONFIRMATION_BATCH_PLACEHOLDER`, `FINAL_STATE_PLACEHOLDER`, `NEXT_TASK_PLACEHOLDER`). Analisar o lote de confirmação `r4p5` (já executado, dados em `out/affinity-bench/`) e preencher. | Nenhum `PLACEHOLDER` em `docs/`; números reproduzíveis por script. |
| H2 | `docs/NEXT_SESSION.md` desatualizado (diz "nada commitado" e lista o reteste como pendente). | Reflete o estado real. |
| H3 | Worktree limpo passa para `master` atual na branch de trabalho; build + suíte verdes. | "PASS: all … tests". |

### Fase 1 — Estabilidade no boot [L]

| ID | Item | Aceite |
|---|---|---|
| S1 | Falha única no boot: `Scheduler::Finish` com `this` nulo vindo de `Fsr4Upscaler::Impl::Destroy` na troca de upscaler do frame 1 (`vk_fsr4.cpp:93`, `vk_temporal_upscaler.cpp:317`). Revisar o ciclo de vida do `Impl` (construção parcial, destruição dupla, ordem de membros). | Causa demonstrada por leitura de código + teste unitário que falha no padrão antigo; ou registrado como "não reproduzido" com o que foi descartado. |
| S2 | Falha única: leitura nula dentro de `nvoglv64.dll` em `vkWaitForFences` de `Presenter::GetRenderFrame` antes do primeiro frame. Verificar validade do fence/frame (uso antes de criar, após destruir, corrida com recriação de swapchain). | Idem S1. |
| S3 | Lote de boot curto (30 × ~20 s, 4P e sem afinidade) para medir a taxa das falhas S1/S2 antes/depois de qualquer mudança. | Taxa com intervalo de Wilson, antes e depois. |
| S4 | Commit do sistema: a seção `SEC_COMMIT` de ~6 GiB (`src/runtime_memory.c`) cobra o pagefile inteiro por jogo. Medir commit por processo; avaliar `SEC_RESERVE` + commit sob demanda. Relaciona-se a #11 (crash no Cap. 9 com pagefile pequeno). | Medição publicada; mudança só se o A/B mostrar commit menor sem custo de frame time e a suíte passar. Senão, só o diagnóstico. |

### Fase 2 — Stutter de pipeline (#113, #114, #68, #102) [L]

| ID | Item | Aceite |
|---|---|---|
| P1 | #113 pedido 1: por que pipelines salvos são recompilados na sessão seguinte. Duas execuções consecutivas da mesma rota com o mesmo perfil; listar chaves compiladas na 2ª que já estavam salvas na 1ª; achar o campo da chave que muda (endereço de runtime, ordem, estado dinâmico). | Tabela "compilados no run 2 que existiam no run 1" e a causa por família; se for chave instável, correção com teste de regressão e A/B de `compile_wait`. |
| P2 | Cobertura do warm-up: #113 relata 751/777 pipelines aquecidos e stalls nos mesmos lugares. Medir quais chaves dos stalls do run 2 estavam fora da hot list. | Contagem por causa (fora da lista / chave diferente / binário rejeitado). |
| P3 | Atribuição de frames lentos no reteste 4P: `compile_wait` 2,1–2,6 s por execução. Separar espera por compilação ativa do driver vs. fila. | Tabela por execução; alimenta P1/P2. |
| P4 | `docs/SHADER_PRECOMPILATION_PLAN.md` §3 (pré-compilação antes do gameplay, pedido 2 de #113): atualizar o plano com o que P1–P3 mostrarem; implementar só o primeiro passo que for mensurável. | Plano atualizado; implementação opcional atrás de variável de ambiente, default inalterado. |

### Fase 3 — Readbacks na thread principal [L]

| ID | Item | Aceite |
|---|---|---|
| R1 | Hipótese 1 de `docs/GPU_READBACK_INVESTIGATION.md`: medir a distância entre o tick da última escrita da GPU na janela e o tick atual quando a falha de escrita chega (só instrumentação, opt-in). | Histograma da distância por execução. |
| R2 | Se R1 mostrar que o escritor quase sempre já terminou: esperar o tick do escritor em vez de `scheduler.Finish()`, atrás de variável de ambiente, default desligado. | A/B pareado (≥ 5 + 5, warm): ms/s de download, P99/P99.9, e comparação de imagem da cena; coerência idêntica (mesmos bytes baixados, verificada por hash opt-in). |
| R3 | Hipóteses 2 e 3 (janela menor; reaproveitar a cópia do read-watch) só se R2 não bastar. | Mesmo protocolo de R2. |

### Fase 4 — CPU e threads (#40, #65) [L parcial]

| ID | Item | Aceite |
|---|---|---|
| C1 | #40: perfil por thread (`THEORDER_SAMPLE=guest:` + `tools/sample_report.py`) da rota do prólogo com `TASK_IDLE` 0 e 2, sem afinidade. | Tabela de CPU por thread; o que ainda gira em espera ativa. |
| C2 | #65 (CPUs Intel de 2ª/3ª geração): revisar `cpu-check`/`isa-emulate`/`isa-stub` contra o conjunto de instruções de Sandy/Ivy Bridge (sem AVX2/BMI/FMA/F16C); varrer o eboot por instruções fora desse conjunto ainda não cobertas. | Lista de instruções não cobertas (ou prova de cobertura) + teste; sem hardware, fica `[R]` para confirmação. |

### Fase 5 — Relatos gráficos [R] (triagem e instrumentação)

| ID | Item | Aceite |
|---|---|---|
| G1 | Ler as issues novas (#95, #98, #99, #100, #101, #107, #114, #115, #116) e as retestadas (#43, #47, #63, #26, #27); extrair GPU/driver/build/cena/log. | `docs/BACKLOG.md` e `docs/ISSUES_TRIAGE.md` atualizados para 38 issues, por assinatura. |
| G2 | #98 ("série 0.7 tela preta, 0.6.1 funciona") e #43/#47: cruzar com `docs/V07_GRAPHICS_BISECT.md`; listar os commits candidatos e o que um log do reporter precisa conter para decidir. | Pedido de evidência por issue (rascunho, não postado). |
| G3 | #101 (preso no primeiro QTE): reproduzir localmente pela rota com pad; é input/QTE, verificável aqui. | Reproduzido ou não, com log; se reproduzido, causa e correção com teste. |
| G4 | #100 ("nenhum upscaler funciona") e #71 (DLSS/`port_dlss.dll`): verificar o caminho de seleção/fallback de upscaler e as mensagens de log. | Matriz provider × condição de fallback; correção só de defeito comprovado. |
| G5 | #107 (instalação de DLC): conferir `src/runtime_content.c` contra o relato. | Passos documentados ou defeito reproduzido. |
| G6 | Rascunhos de resposta para cada issue triada (PT/EN conforme o autor), pedindo exatamente a evidência que falta. | `docs/ISSUE_REPLY_DRAFTS_2026-11-09.md`, nada postado. |

### Fase 6 — Qualidade e manutenção [L]

| ID | Item | Aceite |
|---|---|---|
| Q1 | `batch_runs.py` rotula `bad_alloc` e falhas nativas como `HUNG`/`EXIT 23`. Classificar a causa lendo o log (bad_alloc, falha nativa com módulo+offset, watchdog, starvation). | Teste no `test_regression_harness.py` com logs sintéticos. |
| Q2 | Scripts de análise que vivem só em `out/affinity-bench/` (`analyze.py`, `combine.py`, `retest_combine.py`, `memwatch.ps1`) passam para `tools/` com teste mínimo. | Reproduzem os números já publicados. |
| Q3 | Guarda de commit por processo no lote (pendência do reteste: "worth a per-process commit sample"). | Coluna nova no manifest; usada em S4. |
| Q4 | Revisão de código (`ponytail-review`) dos commits desta sessão antes de integrar em `master`. | Achados corrigidos ou registrados. |
| Q5 | Atualizar `docs/PROGRESS.md`, `docs/PERFORMANCE_BASELINE.md` e `docs/NEXT_SESSION.md` a cada fase. | Docs batem com o estado do código. |

### Fora de escopo nesta sessão

Features sem validação possível aqui ou com dependência externa: #2 Linux, #83 Mac, #31/#42
DLSS/XeSS/FG, #46 ZAR, #32 toggles de pós-processamento, #34 prompts de teclado, #16 Armoury.
Ficam no `docs/BACKLOG.md` com o estado atual.

## 4. Condições de parada

- Suíte vermelha que não volta a verde revertendo a última mudança.
- Menos de 20 GB livres depois de limpar as cópias do próprio lote.
- Falha repetida do host (TDR, commit no limite) fora do controle do experimento.
- Qualquer passo que exigiria ação pública ou tocar no WIP: registrar e pular.

## 5. Entrega ao final

- `docs/NEXT_SESSION.md` com: o que foi feito, o que foi medido (com manifest), o que ficou
  aberto e a próxima ação exata.
- Commits locais na branch `auto/backlog-20261109` (e `master` local em fast-forward quando a
  fase fechou verde). Push e respostas públicas ficam para o usuário decidir.

## 6. Diário

(preenchido durante a execução)
