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

### 2026-11-09 — Fase 0 concluída (`c6dc7b7`)

H1/H2/H3 feitos. O lote `r4p5` (5 + 5, já executado na sessão anterior) reproduziu a falha do
FSR 4 no boot: 2 ocorrências em 50 execuções. Totais nos 5 lotes: base 24/25, idle2 23/25.

### S1/S2 — análise, sem correção (aberto)

- `r4p5/idle2-02` simbolizado na imagem `970b9d33…`: `Scheduler::Finish` (`mov 0x40(%rcx)`,
  `rcx` = 0) ← `Fsr4Upscaler::Impl::Destroy` (vk_fsr4.cpp:93) ← `~Impl` ← `~Fsr4Upscaler` ←
  `TemporalUpscaler::OnFrameStart` (vk_temporal_upscaler.cpp:317) ← `Rasterizer::DrawRecord`.
- `Destroy` só chega ao `Finish` se `backend_ok` ou `context_ok` estiver ligado; o `Impl`
  destruído é o criado no construtor do `TemporalUpscaler` e nunca usado (as duas flags
  nascem `false`). Ler flag ligada e `scheduler` nulo num objeto intocado significa que a memória
  do `Impl` (ou o ponteiro para ele) mudou antes do frame 1: corrupção de heap ou uso após
  liberação por outro código. O `TemporalUpscaler` é criado uma vez (`vk_rasterizer.cpp:75`);
  a única chamada de outra thread (`DisplayOverride`, presenter) não toca no `fsr4`.
- ASan descartado: a sombra dinâmica do ASan no Windows reserva uma faixa grande de endereços que
  pode colidir com a faixa fixa do guest (64 GiB – 1 TiB, `--disable-high-entropy-va`). Page heap
  (gflags) exige alterar o registro da máquina (HKLM): fora das regras da sessão.
- S2 (`nvoglv64.dll` dentro de `vkWaitForFences`, 1 ocorrência) não tem mais dados que o log.
- Próximo passo possível (não feito): verificação opt-in da integridade do `Impl` a cada frame
  para datar a corrupção; ou page heap com autorização do usuário.
- S3 dispensado: a taxa já está medida (3 falhas de boot em 54 execuções do build corrigido,
  0 em 44 do v0.8.0, Fisher p = 0,25).

### Relatório de bug sem log (#116) — corrigido (`af81721`)

O exportador do launcher usava só a caixa de log, vazia depois de reabrir o launcher. Agora usa o
`last_run.log` da pasta de saves (últimos 4 MiB, linhas inteiras), cai para a caixa se não houver
arquivo, diz a fonte no `report.md` e inclui a versão do pacote. 6 testes novos.

### P1 (#113) — causa encontrada; diagnóstico melhorado (`ba9f698`)

Cadeia de 5 sessões na rota do prólogo (`docs/PIPELINE_SESSION_REBUILDS.md`): nenhum pipeline
salvo é recompilado; cada sessão traz permutações novas do shader de material (98, 56, 50, 40
pipelines novos). 64 de 72 permutações novas diferem da mais próxima em um único slot de textura.
`THEORDER_PERMUTATION_LOG` agora compara com a permutação existente mais próxima. Redução
possível (sRGB com SPIR-V idêntico, ~7 %) registrada, não implementada. P2-P4 ficam reformulados
por esse resultado.

### Q1 — `batch_runs` nomeia exceções e STOP (`1cf2aff`)

`EXCEPTION std::bad_alloc` e `STOP <motivo>` em vez de HUNG/EXIT 23. Nos 98 logs anteriores:
9 bad_alloc, 5 faults, 1 HUNG, 83 ok, igual à apuração manual.

### G1/G6 — triagem e rascunhos (`4eeded5`)

`docs/BACKLOG.md` com as 38 issues; `docs/ISSUE_REPLY_DRAFTS_2026-11-09.md` (nada publicado).

### C2 (#65) — CPUs antigas

- Executável do port: sem AVX2/FMA/BMI2/F16C/MOVBE/LZCNT; só `tzcnt`, que executa como `bsf`
  em CPUs sem BMI1 (mesmo resultado para entrada não nula, padrão do compilador).
- `THEORDER_ISA_REWRITE=1` nesta máquina: 3/3 execuções param na partida com
  `STOP … 5 sites use encodings the rewrite cannot emulate`. Os 5 são `vcvtph2ps xmmN, xmmN`
  (F16C), que a reescrita nunca emulava: Sandy Bridge (2ª geração, sem F16C) não iniciava.
- Implementado: `vcvtph2ps xmm, xmm` vira int3 e o handler do Windows converte no CONTEXT
  (XMM + metade alta do YMM zerada pelo XState). Conversão testada contra o F16C desta CPU nos
  65536 valores; teste ponta a ponta no `isa-stub-test`; mutações (NaN sem quiet bit, YMM alto
  não zerado) detectadas.
- `THEORDER_ISA_REWRITE` aceita uma lista (`MOVBE+BMI1+LZCNT` = Ivy Bridge) para exercitar o
  caminho de cada geração nesta máquina.
- Resultado em jogo (3 + 3 + 3, prólogo): nativo, Ivy e Sandy completam todas; a reescrita sobe o
  P99 de 18 para 31 ms nesta CPU (`docs/OLD_CPU_ISA_PATH.md`). Commit `5166815`. Sem hardware
  real de 2ª/3ª geração: confirmação do reporter pendente.
- Defeito encontrado no caminho: o carregador C rejeitava a tabela com int3 de 5 bytes, e no modo
  forçado o jogo seguia sem reescrita (lote anterior inválido, descartado). Agora o modo forçado
  sem tabela para.

### R1 — instrumentação de readbacks (`cf3c84c`)

`THEORDER_READBACK_TICK_LOG=1` classifica a escrita mais recente da GPU atrás de cada download
síncrono. Medido (2 × 125 s): 90 % dos downloads têm o escritor no command buffer ainda aberto
(99-100 % no gameplay). Hipótese 1 refutada como alavanca principal; R2 não implementado. Detalhes em
`docs/GPU_READBACK_INVESTIGATION.md`.

### C2 continuação — MOVBE inline (`fd773d3`, `7a6c70e`)

Relatório de hits por instrução no fim de cada execução: ~286 M chamadas ao emulador em 125 s,
um único `movbe eax,[r12]` com 114 M; os 5 F16C rodaram 30 k vezes (caminho int3/XMM exercitado).
Loads MOVBE agora são inline (MOV + BSWAP). A/B intercalado 3+3+3: P99 de volta ao nativo
(18,0-18,3 ms), ainda ~25 % mais stutters/min nos caminhos reescritos. Próximo candidato: BEXTR.

### S4, C1 — não executados

- S4 (commit da seção de memória guest): trocar `SEC_COMMIT` por reserva + commit sob demanda
  mexe no mapeamento da memória guest inteira; sem A/B seguro nesta sessão. Fica a proposta.
- C1 (#40): a medição de 2026-11-09 já mostra o efeito do idle padrão do v0.8.0 (5,2 → 1,9
  núcleos); vira pedido de reteste (rascunho no arquivo de respostas).

### 2026-11-10

- **BEXTR inline (`baf6c5c`):** resultado e flags iguais ao BEXTR da CPU em 2352 casos por
  snippet; A/B de frame time inconclusivo (máquina com carga de fundo; nativo também piorou).
- **S1 canários (`f5a9aac`):** o log diz se o ponteiro ou a memória do `Impl` mudou antes da
  destruição. 1º lote de boot: 32 execuções sem a falha do FSR 4; uma falha nova no código do jogo
  (thread principal, `guest offset 0xc00ed7`, leitura de 0x1380, antes do primeiro frame). O lote
  parou com disco cheio (cópias de perfil de 1 GB) → `--discard-profiles` no `batch_runs`.
- **#101 encontrado e corrigido (`9f25ab0`):** `sub_b105b0` multiplica cada aperto do QTE pelo
  tempo de frame (dt × 30) e drena o medidor por segundo; a 60 FPS cada aperto vale 0,5, a 120 FPS
  0,25. Patch de 1 byte (jbe → jmp): aperto vale sempre 1,0, como a 30 FPS; entra junto com todo
  patch de taxa de quadros. Não verificado em jogo (a rota com script não chega ao QTE).

## Fase 7 — rumo à v0.8.2 (após a release v0.8.1, 2026-11-10)

Relatos do usuário no teste (feito no pacote v0.8.0, cache frio) e o que já se sabe:

| ID | Item | Estado / próximo passo | Aceite |
|---|---|---|---|
| F1 | Flicker de objetos/decals (cartaz em Whitechapel alterna a cada frame) | Reproduzido com o save do usuário; existe na v0.7.9. `stale_readback` suspeito numa execução, não confirmado em 3+3. Próximo: medir só a região do objeto (máscara fixa, câmera parada), depois capturar o frame inteiro (`--frame-capture-at`) nos dois estados. | Causa demonstrada; A/B 3+3 com máscara mostra o objeto estável |
| F2 | Stutter forte | Sessão do usuário era cache frio (1374 pipelines). Separar: (a) cache frio → pré-compilação/warm-up; (b) permutações novas de material (#113). | Medir frio vs quente no mesmo save; um passo mensurável implementado |
| F3 | Granadas sem fumaça | Não reproduzido; falta a cena (perguntado ao usuário). | Efeito visível na cena, captura antes/depois |
| F4 | Legendas ausentes | Confirmado na cutscene do save. Os pacotes de localização sem parte GPU (18) não foram pedidos nessa cena. Próximo: rastrear o gerenciador de diálogos (WIP do usuário, `sub_a70fd0`) numa cutscene. | Legenda visível na cutscene |
| R1 | "Write windows" (UFC-69/PORTHEON): cópias da libc perto de dados GPU sem readback | Estudo de porte; ataca os 90 % de downloads com escritor no frame aberto. | A/B de ms/s de download e frame time |
| R2 | Backport shadPS4 #5264 (EarlyFragmentTests + DEPTH_BEFORE_SHADER) | Avaliar contra o flicker F1. | A/B visual |
| T1 | Ferramenta de captura grava `rgba` quando os dados são `bgra` (cores azuladas nas prévias) | Corrigir o rótulo no dump do build atual. | Prévia com cores corretas sem `--raw-format` |
