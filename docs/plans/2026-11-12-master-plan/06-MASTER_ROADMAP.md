# 06 — Roadmap mestre

Baseado nos docs 01–05 e 09. Complexidade: P (≤ 1 dia de executor), M (2–5 dias), G (> 1 semana
ou incerteza de RE). Não há prazos: várias fases dependem de engenharia reversa e de reteste por
relatores. Tarefas detalhadas no doc 07.

## Ordem e dependências

```
Onda 1 (paralela)   F0 Medição ─────────────┐
                    F1 Acesso a cenas ──────┼──► Onda 2   F2 Artefatos por evidência
                    F6 Distribuição ────────┘             F3 Stutter de primeira visita
                                                          F5 Compatibilidade entre GPUs
                    F4 Upscaler temporal (RE independente, pode começar na Onda 1)
Onda 3              F7 Configuração e launcher · F8 Sessões longas · F9 Entrada e efeitos
```

A lógica: hoje **não conseguimos ver** a maior parte dos problemas relatados (uma rota, uma GPU,
uma métrica interna). A Onda 1 compra visibilidade — cenas, medidas e um canal rápido com os
relatores. Só então o Opus entra nos problemas gráficos com cena e hipótese definidas.

---

## F0 — Fundação de medição

1. **ID:** F0 · 2. **Nome:** Fundação de medição
3. **Objetivo técnico:** baseline reproduzível por release, com métrica de apresentação externa e limiar de regressão derivado de variância medida.
4. **Problema:** afirmações de desempenho dependem de protocolos diferentes a cada experimento; sem métrica independente do log interno; Tracy é stub.
5. **Estado atual:** `batch_runs.py`, `tools/bench/*`, `flicker_scan.py` e `memwatch.ps1` existem e funcionam; uma rota; sem baseline por tag.
6. **Referência:** doc 09; `AGENTS.md` ("State the sample size").
7. **Arquivos:** `tools/batch_runs.py`, `tools/bench/` (novos `baseline.py`, `scenarios/`), `gpu/shim/tracy/`, `gpu/CMakeLists.txt`, `docs/baselines/`.
8. **Dependências:** espaço em disco; aprovação para PresentMon e Tracy.
9. **Modelo:** Sonnet (T-004, T-005, T-006, T-007); Fable define limiares.
10. **Ferramentas/skills:** PresentMon, Tracy, `wb-vulkan-frame-pacing`, `wb-oracle-testing`.
11. **Etapas:** (a) cenários S1–S3 versionados; (b) `baseline.py` com cadeia frio→morno-1→morno-2 e limpeza de perfis; (c) PresentMon opcional no lote; (d) série A/A n=5 para variância; (e) marcadores de carregamento no log; (f) build opt-in com Tracy real.
12. **Testes:** testes Python do parser e do comparador com logs sintéticos; PresentMon ausente não quebra o lote.
13. **Aceitação:** `docs/baselines/windows-v0.9.0.json` existe; duas execuções do baseline no mesmo commit diferem menos que o limiar derivado do A/A; o build padrão não muda de tamanho nem de comportamento com Tracy desligado (sha256 do pacote igual ou diferença explicada).
14. **Riscos:** disco (16 GB livres); PresentMon pode exigir privilégio; overhead do Tracy contaminar medições se ligado por engano.
15. **Complexidade:** M.
16. **Resultado:** toda fase seguinte tem número de antes e depois comparável.

## F1 — Acesso a cenas

1. **ID:** F1 · 2. **Nome:** Acesso determinístico a cenas
3. **Objetivo técnico:** iniciar o jogo em qualquer nível/checkpoint por automação.
4. **Problema:** issues P0 (flicker, tela preta, artefatos, crashes dos capítulos 3 e 9, fumaça do Atrium) estão em cenas que a rota do prólogo não alcança.
5. **Estado atual:** [FATO] `run.py --level/--checkpoint` passa `-startlevel`/`-startcheckpoint`; o jogo permanece na tela-título. [FATO] `0x447880` só lê as opções enquanto `*(*(0x16b31d8)+0x208) == 0`; lista de checkpoints na tabela de símbolos em `0x14a9b00`.
6. **Referência:** `docs/ENGINE_RE.md` (Command line); memória `rea-eboot-setup`.
7. **Arquivos:** `docs/ENGINE_RE.md`, `patches/TheOrder1886-Orbis.xml` ou `src/probe.c`, `run.py`, `tools/bench/scenarios/`.
8. **Dependências:** REA conectado; nenhuma outra fase.
9. **Modelo:** Opus (T-020); Sonnet no caminho alternativo por saves (T-021).
10. **Ferramentas/skills:** REA, `reverse-engineer-anything`, `wb-reverse-engineering`.
11. **Etapas:** (a) decompilar `0x447880` e quem escreve `+0x208`; achar por que o front end vence (ausência de `-frontend`? ordem de inicialização? opção de build de desenvolvimento removida?); (b) patch mínimo ou sequência de argumentos; (c) extrair a lista de checkpoints; (d) alternativa: biblioteca de saves por checkpoint + `--user-template`.
12. **Testes:** teste do tradutor de argumentos; execução real por cenário com frame apresentado e nome do nível no log.
13. **Aceitação:** três cenários fora do prólogo (S4, S5, S6) iniciam por comando, 3 de 3 execuções cada, chegando a gameplay com controle; ou relatório fundamentado de inviabilidade + caminho por saves funcionando.
14. **Riscos:** o caminho de nível direto pode ter sido removido do retail; estado inicial diferente do de um jogador (inventário, flags) pode mascarar ou criar bugs — registrar como limitação.
15. **Complexidade:** G (RE).
16. **Resultado:** cenários S4–S8 desbloqueados; reprodução local dos relatos.

## F2 — Artefatos gráficos por evidência

1. **ID:** F2 · 2. **Nome:** Correção gráfica com cena reproduzida
3. **Objetivo técnico:** reproduzir, atribuir causa e corrigir flicker, tela preta, artefatos verde/azul, fumaça ausente e os crashes de capítulo.
4. **Problema:** #95, #99, #30, #29; #98, #116, #47, #24, #43; #63, #39; #118, #11; Atrium.
5. **Estado atual:** nenhum reproduzido localmente fora do Atrium; relatório do #118 anexado e não lido.
6. **Referência:** `GRAPHICS_REGRESSIONS.md`, `PROGRESS.md` (classes D/E/F em hipótese), padrão P5 (doc 03).
7. **Arquivos:** `gpu/shadps4/video_core/texture_cache/*`, `renderer_vulkan/vk_rasterizer.cpp`, `buffer_cache/*`, `shader_recompiler/*`.
8. **Dependências:** F1 (cenas); F0 (flicker_scan e captura de referência); F6 (builds por issue para AMD).
9. **Modelo:** Haiku (T-001 leitura de anexos), Opus (T-022, T-023), Sonnet (T-024, T-030).
10. **Ferramentas/skills:** `wb-debug-rigorous`, captura de frame própria, RenderDoc se aprovado, camadas de validação.
11. **Etapas:** por sintoma — reproduzir em cenário fixo → captura → hipótese falsificável → correção atrás de variável → A/B → build de teste para o relator.
12. **Testes:** um teste de regressão por correção que falhe sem ela; `flicker_scan` no cenário; contador de ativação.
13. **Aceitação:** por issue — reprodução registrada com build/cena/GPU; após a correção, `flicker_scan` em 0 no cenário (ou captura idêntica à referência) em n=3; issue passa a `status:needs-retest`. Fechamento só com reteste do relator.
14. **Riscos:** causas específicas de driver AMD sem hardware local; vários sintomas com uma causa ou o inverso; correção que troca flicker por stutter.
15. **Complexidade:** G.
16. **Resultado:** queda mensurável das issues gráficas abertas, com evidência por cena.

## F3 — Stutter de primeira visita

1. **ID:** F3 · 2. **Nome:** Pré-compilação durante o carregamento
3. **Objetivo técnico:** mover a compilação de pipelines da primeira visita para as telas de carregamento.
4. **Problema:** #113, #114, #120, #68. Depois da primeira sessão o prólogo já está em 0–1 janela >100 ms; a primeira visita de cada área ainda compila em draw visível.
5. **Estado atual:** cache em disco, pipeline binaries, prioridade de pipeline visível, compilação assíncrona. Sem noção de "área carregando". [FATO] Os três pontos de entrada do Oodle usados pelo prefetch do TOport conferem byte a byte no nosso eboot.
6. **Referência:** Unleashed Recompiled (P1, P2); TOport a5327ef (`runtime_oodle_index.c`, prefetch em `vk_pipeline_cache.cpp`); `reference-ports-review.md`.
7. **Arquivos:** `renderer_vulkan/vk_pipeline_cache.cpp`, `vk_pipeline_serialization.cpp`, `src/runtime_file.c`, novo `src/runtime_oodle_index.c`, `tools/bench/`.
8. **Dependências:** F0 (medir primeira visita por área) e F1 (áreas além do prólogo). **Portão:** só investir se a medição em S4–S7 mostrar o problema com o tamanho relatado.
9. **Modelo:** Opus (T-050, T-051); Sonnet (T-052).
10. **Ferramentas/skills:** `wb-shader-cache-triage`, `wb-vulkan-frame-pacing`, REA.
11. **Etapas:** (a) medir primeira visita em áreas novas; (b) detectar início/fim de carregamento no guest; (c) portar índice de pacote + prefetch atrás de `THEORDER_AREA_PREFETCH`; (d) gravador de chaves de pipeline e lista distribuível; (e) barreira opcional no fim do carregamento.
12. **Testes:** unidade do índice e do gravador; A/B frio em n=3 por área; verificação de que nenhum draw visível foi descartado.
13. **Aceitação:** em perfil frio, janelas >100 ms na primeira visita das áreas medidas caem em relação ao baseline, fora do ruído A/A, sem aumento do tempo de carregamento além do valor acordado após a medição (a); padrão só muda com n≥3.
14. **Riscos:** chamar o descompressor do jogo de uma thread do host; lista de chaves presa a SKU/versão; tempo de carregamento maior; licença — distribuir apenas chaves/estados, nunca SPIR-V derivado do jogo.
15. **Complexidade:** G.
16. **Resultado:** primeira sessão próxima da experiência de sessão morna.

## F4 — Upscaler temporal e frame generation

1. **ID:** F4 · 2. **Nome:** Reativar o caminho temporal na 01.02
3. **Objetivo técnico:** identificar cor de cena, profundidade, câmera e passe de composição no CUSA00035 01.02 para o upscaler rodar.
4. **Problema:** #138, #100, #35, #31, #42, #71, #22; FG e sete itens do SpartanPort bloqueados.
5. **Estado atual:** [FATO] gatilho `0x9a9cf8a9` nunca despachado; G-buffer com 3 alvos a 1920x800; constantes de câmera de 864 bytes ausentes; 38 compute shaders distintos no tutorial, dois deles a 240x100 grupos.
6. **Referência:** issue #138; `docs/upscaler.md`, `docs/motion_vectors.md`; SpartanPort 7280104, 9f501f6, 9bd1fec, 8d3fca2, 9fdc30c, 3ad12a7, bf9181b.
7. **Arquivos:** `renderer_vulkan/vk_temporal_upscaler.cpp`, `vk_camera_motion.cpp`, `vk_rasterizer.cpp`, `vk_scene_resolution.cpp`, `vk_frame_generation.cpp`.
8. **Dependências:** nenhuma para o RE (pode começar na Onda 1); F0 para validar qualidade/custo.
9. **Modelo:** Opus (T-040, T-041).
10. **Ferramentas/skills:** REA, captura de frame (`THEORDER_FRAME_CAPTURE_KEY`), `wb-reverse-engineering`.
11. **Etapas:** (a) descobrir em que build/cena o gatilho funcionava (o hash vem do commit inicial); (b) censo de passes da 01.02 (`test_pass_census` já existe); (c) localizar matrizes de visão/projeção por captura e por RE; (d) generalizar a detecção (alvos, formato, 2.40:1); (e) validar FSR 3.1 → depois FG e itens do SpartanPort.
12. **Testes:** unidade da detecção com capturas das duas variantes; contador de frames upscalados; captura de saída a 1440p.
13. **Aceitação:** `TemporalUpscaler::Run` conta >0 frames em S2 e S3 com FSR 3.1; saída na resolução alvo confirmada por captura; movimento de câmera sem ghosting grosseiro em vídeo de 30 s; sem regressão de tempo de frame fora do ruído com o upscaler desligado.
14. **Riscos:** layout de câmera diferente por SKU; vetores de movimento por objeto inexistentes; HUD a 60 FPS já cintila.
15. **Complexidade:** G (RE).
16. **Resultado:** FSR/DLSS/TAA funcionais no SKU principal; FG deixa de ser inerte.

## F5 — Compatibilidade entre fabricantes

1. **ID:** F5 · 2. **Nome:** Matriz de GPUs com evidência de relator
3. **Objetivo técnico:** transformar relatos de AMD/Intel em dados comparáveis e validar uso da API fora de NVIDIA.
4. **Problema:** #10, #47, #63, #39, #36, #33 (formato de normais em RDNA 4); só há NVIDIA local.
5. **Estado atual:** `tools/gpu_capabilities.c`, `port_report.py`, correções pontuais por fornecedor; CI sem ICD.
6. **Referência:** doc 09 §7; SpartanPort D1; Unleashed (seleção de GPU).
7. **Arquivos:** `launcher/port_report.py`, `launcher/port_launcher_win.py`, `.github/workflows/`, `docs/GPU_MATRIX.md`, `renderer_vulkan/vk_instance.cpp`.
8. **Dependências:** F0 (cenários), F6 (builds por issue).
9. **Modelo:** Sonnet (T-012, T-013); Haiku (leitura dos kits); Opus para as correções resultantes.
10. **Ferramentas/skills:** lavapipe, camadas de validação, `vulkaninfo`.
11. **Etapas:** kit do relator; ICD de software no CI; matriz; seleção explícita de GPU se ausente; verificação da chave de perfil do cache (P5).
12. **Testes:** kit gera ZIP válido e redigido em teste; testes de dispositivo passam em lavapipe.
13. **Aceitação:** testes de GPU rodam no `gate`; ≥ 1 kit AMD e ≥ 1 Intel recebidos e registrados na matriz; nenhuma mensagem de erro da camada de validação em S1–S3.
14. **Riscos:** lavapipe lento ou sem extensões exigidas; baixa adesão dos relatores.
15. **Complexidade:** M.
16. **Resultado:** correções por fornecedor passam a ter ciclo de validação.

## F6 — Distribuição e atualização

1. **ID:** F6 · 2. **Nome:** Pacote pelo CI, builds por issue, atualização transacional
3. **Objetivo técnico:** pacote reproduzível fora da máquina do mantenedor e atualização verificada com rollback.
4. **Problema:** release depende de uma máquina; atualizador sem sha256, não atômico, sem rollback; relator espera uma release para testar.
5. **Estado atual:** `tools/release.sh` (local, publica `.sha256`); `install_update` copia por cima.
6. **Referência:** SpartanPort `windows-zip.yml`, `core/update.rs`, `smoke-test.ps1`; Uncharted NDC e WWE '13/2K14 (sha256 + backup + rollback); Velopack (`.partial`, lock).
7. **Arquivos:** `.github/workflows/` (novo), `packaging/windows/`, `launcher/port_launcher_win.py`, `tools/release.sh`, `tests/test_launcher_*`.
8. **Dependências:** nenhuma. Começa na Onda 1.
9. **Modelo:** Sonnet (T-010, T-011).
10. **Ferramentas/skills:** `gh`, `wb-pr-release`, `wb-test-planning`.
11. **Etapas:** (a) workflow de pacote com sha256 e smoke sem MSYS2; (b) pré-release `windows-v<versão>-issue<N>-test<M>`; (c) atualizador: `.partial` → sha256 → extrair em pasta irmã → trocar por renome → manter anterior até o primeiro início bem-sucedido → remover órfãos por manifesto; (d) canal de pré-release opcional.
12. **Testes:** unidade com servidor local: hash errado, download truncado, ZIP corrompido, arquivo em uso, queda no meio da troca, versão nova que não inicia (rollback); dados do usuário intactos em todos os casos.
13. **Aceitação:** todos os casos de falha acima terminam com a instalação anterior funcional e `user/`, `settings.json`, `theorder.ini`, mods e caches intactos; atualização v0.9.0 → build de teste → de volta verificada à mão uma vez; pacote do CI passa no smoke local com jogo.
14. **Riscos:** antivírus/SmartScreen com executável trocado; atualizar a partir de versões antigas que têm o instalador antigo (o instalador que roda é o da versão **nova** — bom); assinatura de código inexistente.
15. **Complexidade:** M.
16. **Resultado:** ciclo relator↔correção em um PR; atualização que não quebra instalações.

## F7 — Configuração e launcher

1. **ID:** F7 · 2. **Nome:** Catálogo único de configurações e validação do dump
3. **Objetivo técnico:** uma fonte declarativa chave → variável → padrão → efeito, com contrato testado.
4. **Problema:** 263 variáveis, 57 chaves de `.ini`, `settings.json` e `recomplauncher.json` (5 configurações) mantidos à mão; relatos causados por dump/SKU/idioma incompatível.
5. **Estado atual:** decisão de manter o launcher Python; regressão de override já ocorreu.
6. **Referência:** SpartanPort `env_contract.rs`; Unleashed (hashes de instalação).
7. **Arquivos:** `launcher/`, `scripts/graphics_options.py`, `gpu/shim/port_settings.cpp`, `recomplauncher.json`, `scripts/prepare.py`.
8. **Dependências:** nenhuma técnica; prioridade menor.
9. **Modelo:** Sonnet (T-014, T-015); Haiku para o inventário das variáveis.
10. **Ferramentas/skills:** `wb-test-planning`.
11. **Etapas:** inventário → catálogo → teste de contrato → geração do `recomplauncher.json` e da documentação → manifesto de SKU/versão e checagem de pacote de idioma com aviso.
12. **Testes:** contrato falha quando uma chave não chega ao jogo; geração determinística.
13. **Aceitação:** 100 % das chaves do `.ini` no catálogo; `recomplauncher.json` gerado e idêntico em duas execuções; idioma sem pacote vira aviso em vez de tela preta.
14. **Riscos:** variáveis de diagnóstico sem dono; divergência entre parser C++ e Python.
15. **Complexidade:** M.
16. **Resultado:** menos regressões de configuração; suporte mais simples.

## F8 — Sessões prolongadas, memória e CPU

1. **ID:** F8 · 2. **Nome:** Estabilidade de longa duração
3. **Objetivo técnico:** medir e limitar crescimento de RAM/VRAM e atribuir uso de CPU por thread.
4. **Problema:** #40, #11, relatos de travamento em sessões longas.
5. **Estado atual:** `soak.sh`, `memwatch.ps1`, guarda de RAM; medições só no menu.
6. **Referência:** `MEMORY_PROFILING.md`, `AFFINITY_STABILITY_RETEST.md`.
7. **Arquivos:** `tools/soak.sh`, `tools/bench/`, `src/runtime_memory.c`, caches do video core.
8. **Dependências:** F0 (Tracy, baseline), F1 (ir além do prólogo).
9. **Modelo:** Sonnet (T-060); Opus se houver vazamento a corrigir.
10. **Ferramentas/skills:** Tracy, `wb-debug-rigorous`.
11. **Etapas:** S8 de 30 min com amostragem; inclinação de bytes privados e VRAM; perfil de CPU por thread em combate.
12. **Testes:** soak automatizado com critério de inclinação.
13. **Aceitação:** S8 completa 3 de 3 sem falha; inclinação de memória após aquecimento registrada e, se positiva, com causa identificada.
14. **Riscos:** vazamentos só em capítulos tardios; tempo de máquina.
15. **Complexidade:** M.
16. **Resultado:** dados para #40/#11 e critério de soak antes de release.

## F9 — Entrada, interface e efeitos

1. **ID:** F9 · 2. **Nome:** Qualidade de uso
3. **Objetivo técnico:** fechar pendências de mouse/teclado e oferecer toggles de pós-processamento.
4. **Problema:** #37, #38 (retestes), #34 (prompts), #32 (efeitos), bugs dependentes de FPS.
5. **Estado atual:** mouse por tempo, 4 teclas por botão, pausa no menu; sem toggles; sem catálogo de bugs de FPS alto.
6. **Referência:** SpartanPort 3598158/5831938/eae407c (skip de passe por hash) e `docs/high-fps/catalog.md` (método).
7. **Arquivos:** `src/runtime_pad.c`, `src/mouse_stick.h`, `gpu/shim/port_overlay.cpp`, `vk_rasterizer.cpp`, `patches/`.
8. **Dependências:** F4 (censo de passes serve aos toggles); F1.
9. **Modelo:** Haiku (catálogo a partir das issues), Opus (identificação de passes), Sonnet (UI).
10. **Ferramentas/skills:** REA, `wb-gameplay-acceptance`.
11. **Etapas:** catálogo de bugs por FPS; hashes dos passes de motion blur/DoF/grão; toggles atrás de configuração; prompts de teclado só após localizar os assets.
12. **Testes:** unidade dos toggles; captura com e sem efeito.
13. **Aceitação:** cada toggle muda a captura só na região/efeito esperado e não altera o tempo de frame para pior fora do ruído.
14. **Riscos:** passes compartilhados entre efeitos; dependências entre passes.
15. **Complexidade:** M–G.
16. **Resultado:** pedidos recorrentes atendidos sem tocar a correção do renderer.

---

## Áreas avaliadas e deixadas de fora (com motivo)

- **Resolução dinâmica:** depende de F4 (resolução de cena separada da de saída funcionando).
- **DLSS FG / XeSS:** o SpartanPort removeu DLSS FG (54cc763); XeSS sem demanda medida. Reavaliar depois de F4.
- **Launcher novo:** decisão registrada de manter o Python.
- **Linux oficial (#2), macOS (#83), ZAR (#46), DLC (#107):** pedidos de plataforma/recurso fora do objetivo deste ciclo.
- **`VK_EXT_graphics_pipeline_library`, ubershader:** ver doc 03, P9/P10.

## Próximo marco recomendado

**v0.10.0 — "Visibilidade"**: F0 completa, F6 completa, F1 com pelo menos três cenários fora do
prólogo, RE de F4 iniciado. Critério de saída: baseline publicado para v0.9.0 e v0.10.0; primeiro
build de teste por issue entregue a um relator AMD; uma issue gráfica P0 reproduzida localmente.
Nenhuma promessa de correção gráfica neste marco — ele existe para tornar as próximas verificáveis.
