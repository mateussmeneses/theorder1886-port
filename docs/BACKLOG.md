# Backlog de Desenvolvimento & Resolução de Issues — The Order: 1886 PC Port

Estado atual: docs/STATUS.md. Plano e tarefas em execução: docs/plans/2026-11-12-master-plan/ (06-MASTER_ROADMAP.md, 07-EXECUTION_BACKLOG.md).

Estado consultado em 2026-11-09: 38 issues abertas (seção "Atualização 2026-11-09"); a tabela de
2026-10-25 abaixo continua válida para as issues antigas; as seções históricas mantidas depois dela não são
um status atual nem evidência de que um issue foi corrigido. Prioridade organiza
a ordem de investigação, não promete implementação nem substitui reteste em
hardware/cena reportados.

## Atualização 2026-11-09 (38 issues abertas)

Issues novas desde 2026-10-25 e o que já foi verificado localmente. Rascunhos de resposta em
`docs/ISSUE_REPLY_DRAFTS_2026-11-09.md` (nada publicado).

| Prioridade | Issue | Resumo | Estado observado / próxima ação |
| :---: | :---: | :--- | :--- |
| P1 | [#113](https://github.com/mateussmeneses/theorder1886-port/issues/113) | Stalls de pipeline voltam após reiniciar | Reproduzido: cadeia de 5 sessões na rota do prólogo; nenhum pipeline salvo é recompilado, os stalls vêm de permutações novas do shader de material (98 → 56 → 50 → 40 por sessão). `docs/PIPELINE_SESSION_REBUILDS.md`. Redução possível listada lá, não implementada. |
| P1 | [#65](https://github.com/mateussmeneses/theorder1886-port/issues/65) | Não inicia em Intel de 2ª/3ª geração | Reproduzido o caminho forçado: a reescrita não emula F16C (5 sites `vcvtph2ps`), então Sandy Bridge (2ª geração) para no início com `STOP … 5 sites`. F16C agora emulado; caminhos Ivy/Sandy completam o prólogo nesta máquina (`docs/OLD_CPU_ISA_PATH.md`); falta hardware real. Executável do port sem AVX2/FMA/BMI2. |
| P1 | [#101](https://github.com/mateussmeneses/theorder1886-port/issues/101) | QTE de apertar botão não termina acima de 30 FPS | Causa encontrada: `sub_b105b0` multiplica cada aperto pelo tempo de frame. Patch "QTE Fix" (`9f25ab0`) aplicado junto com o patch de FPS; falta confirmação em jogo. |
| P1 | [#116](https://github.com/mateussmeneses/theorder1886-port/issues/116) | Tela preta (RX 6500 XT), 0.6.1 funciona | Mesma família de #47. O zip do relatório veio sem log: defeito do exportador corrigido (`af81721`). Falta o `last_run.log`. |
| P1 | [#98](https://github.com/mateussmeneses/theorder1886-port/issues/98) | Série 0.7: tela preta em gameplay/FMV, 0.6.1 funciona (RTX 5090) | Log mostra o jogo apresentando frames e compilando pipelines; causa não identificada. |
| P1 | [#115](https://github.com/mateussmeneses/theorder1886-port/issues/115) | Erros gráficos (RX 580, v0.8.0) | Capturas mostram molduras/armadura brancas superexpostas; o fallback do formato de normais também roda na NVIDIA local sem esse sintoma. Falta log completo. |
| P2 | [#114](https://github.com/mateussmeneses/theorder1886-port/issues/114) | FPS baixo e stutter (RTX 3050) | Sem log. |
| P2 | [#100](https://github.com/mateussmeneses/theorder1886-port/issues/100) | Nenhum upscaler funciona (RTX 3070 Ti, 2560x1080) | Sem log. |
| P2 | [#99](https://github.com/mateussmeneses/theorder1886-port/issues/99) | Texturas ausentes e flicker (RTX 5090) | Sem log; outro usuário sugere 0.6.1. |
| P2 | [#95](https://github.com/mateussmeneses/theorder1886-port/issues/95) | Flicker de texturas | Sem dados de hardware. |
| P2 | [#102](https://github.com/mateussmeneses/theorder1886-port/issues/102) | Cache de pipeline do driver vazio | Correção proposta é proibida pela especificação Vulkan (VUIDs 09616/09617), confirmado pelo autor. |
| P3 | [#107](https://github.com/mateussmeneses/theorder1886-port/issues/107) | Instalação de DLC | Feature request. |
| P3 | [#83](https://github.com/mateussmeneses/theorder1886-port/issues/83) | Port para Mac | Feature request. |
| P3 | [#71](https://github.com/mateussmeneses/theorder1886-port/issues/71) | `port_dlss.dll` | Respondida: ponte DLSS opcional, não incluída no pacote. |

Falhas de boot locais ainda abertas (plano `docs/plans/2026-11-09-autonomous-backlog.md`, S1/S2):
destruição do contexto FSR 4 no frame 1 (2 em 50 execuções) e falha no driver NVIDIA na espera de
fence do presenter (1 em 50).

## Relatos locais em investigação

- **Adiado pelo usuário: legendas de diálogo ausentes:** relato anterior à última mudança do
  launcher. `-enablesubtitles` chega ao guest; ainda faltam confirmar a opção
  Subtitles do jogo e uma fala reproduzível no checkpoint afetado. Capturas
  isoladas mostram menus/tutorial legíveis, não recuperação das legendas.
- **P1: novos stutters percebidos:** há outliers no log e no trace isolado, mas
  sem comparação equivalente para atribuir regressão. Não alterar defaults de
  compilação ou remover waits/draws com base nesses ensaios.

## Estado atual das issues abertas

| Prioridade | Issue | Resumo | Estado observado / próxima ação |
| :---: | :---: | :--- | :--- |
| P0 | [#43](https://github.com/mateussmeneses/theorder1886-port/issues/43) | Tela preta/branca, texto e áudio presentes | Reproduzida pelo autor em v0.7.7 com RTX 3060 Ti, CUSA00035 01.02; relatos adicionais em RX 560/AMD. A correção local de overlap ainda não foi testada pelo reporter e não resolve o issue por inferência. |
| P0 | [#47](https://github.com/mateussmeneses/theorder1886-port/issues/47) | Tela preta na RX 6500 XT | Issue aberta; há um relato adicional de tela preta em RX 560 no v0.7.7. O reteste do reporter original ainda não confirma o resultado. |
| P0 | [#24](https://github.com/mateussmeneses/theorder1886-port/issues/24) | Tela preta/branca e corpos ausentes | Relatos em mais de uma GPU e releases; resultado varia entre usuários. Captura e build exatos ainda necessários. |
| P0 | [#39](https://github.com/mateussmeneses/theorder1886-port/issues/39) | Flares/artefatos verdes e azuis | Autor retestou v0.7.7 em Chapter 5 com RX 9070 e confirmou persistência; log e vídeo anexados. |
| P0 | [#63](https://github.com/mateussmeneses/theorder1886-port/issues/63) | Artefatos gráficos graves e stutters | RX 9070: autor confirmou artefatos e stutters em Chapters 8/9 no v0.7.7. v0.6.1 melhora Chapter 8 segundo o autor, mas Chapter 9 ainda tem artefatos. Há relato adicional em RX 6900 XT. |
| P0 | [#29](https://github.com/mateussmeneses/theorder1886-port/issues/29) | Polígonos pretos e flicker | Aberta; não há confirmação de resolução em build atual. Separar falha visual de stalls. |
| P0 | [#30](https://github.com/mateussmeneses/theorder1886-port/issues/30) | Objetos/luzes piscam ou somem; stutter em combate | Aberta; sintomas e cenas precisam de captura equivalente. Não assumir causa única para geometria e pacing. |
| P0 | [#11](https://github.com/mateussmeneses/theorder1886-port/issues/11) | Crash em Chapter 9 | Aberta; validar no hardware AMD e recolher log/callsite atuais antes de declarar correção. |
| P1 | [#26](https://github.com/mateussmeneses/theorder1886-port/issues/26) | Sombras solares ausentes | Autor retestou v0.7.7 e confirmou persistência; sombras/granadas ainda sem correção comprovada. |
| P1 | [#36](https://github.com/mateussmeneses/theorder1886-port/issues/36) | Iluminação incorreta | Autor RX 6700 XT disse que permanece no v0.7.7, independente do upscaler. Local/câmera precisam ser identificados. |
| P1 | [#45](https://github.com/mateussmeneses/theorder1886-port/issues/45) | Superexposição e sombras esmagadas em NVIDIA | Bug visual aberto; requer captura do mesmo local/configuração para diagnóstico. |
| P1 | [#68](https://github.com/mateussmeneses/theorder1886-port/issues/68) | Stutter severo em atualização recente | Reporte novo após v0.7.7, com relatos de cutscenes e gameplay; medir rota e cache comparáveis. |
| P1 | [#40](https://github.com/mateussmeneses/theorder1886-port/issues/40) | Uso de CPU em 100% | Sem telemetria por thread/tempo suficiente para atribuir causa. |
| P1 | [#65](https://github.com/mateussmeneses/theorder1886-port/issues/65) | Não inicia em CPUs Intel de 2ª/3ª geração | Hardware, log e mensagem de falha ainda não informados. |
| P1 | [#10](https://github.com/mateussmeneses/theorder1886-port/issues/10) | Personagens pretos em Intel HD 630 | Issue aberta; suporte Intel não confirmado em hardware atual. |
| P1 | [#7](https://github.com/mateussmeneses/theorder1886-port/issues/7) | Tela preta | Issue antiga aberta; a correção histórica anunciada não substitui confirmação em release atual. |
| P1 | [#38](https://github.com/mateussmeneses/theorder1886-port/issues/38) | Menu de pausa não responde ao teclado | Reteste v0.7.7 pendente; a build alinha Return→Options e limpa o estado de tecla ao fechar o overlay. |
| P2 | [#37](https://github.com/mateussmeneses/theorder1886-port/issues/37) | Sensibilidade do mouse | Autor relatou melhora no v0.7.7. Nova correção local normaliza contagens por tempo: teste no runtime passou de 68/48 em 30/60 Hz para 48 em 30/60/120/1000 Hz. Preserva a curva de 60 Hz; reteste de câmera em gameplay pendente. Giros completos continuam limitados pelo stick da câmera. |
| P2 | [#2](https://github.com/mateussmeneses/theorder1886-port/issues/2) | Suporte Linux oficial | Solicitação de plataforma; não há compromisso de suporte validado. |
| P2 | [#22](https://github.com/mateussmeneses/theorder1886-port/issues/22) | FSR 4.1.1 e 120 FPS | O código do caminho FSR 4.1.1 existe, condicionado ao suporte do dispositivo e aos assets do provider; no RTX 5060 testado houve fallback para FSR 3.1. O patch de 120 FPS existe, mas o log local teve mediana de 78.7 FPS por janela em 4K; estabilidade a 120 FPS não está estabelecida. |
| P2 | [#27](https://github.com/mateussmeneses/theorder1886-port/issues/27) | Ultrawide 32:9 | Reporter em monitor 3840x1080 informou que a resolução escolhida não fazia efeito. O modo 3840x1080 estava ausente da tabela C++ e 3440x1440 era descartado pelo parser; ambos foram adicionados como novos índices e expostos no launcher/overlay. Testes sintéticos de load/save/selection passam; enquadramento, HUD e imagem em gameplay no monitor do reporter continuam sem validação. Separar output target, render interno e aspecto 32:9. |
| P2 | [#28](https://github.com/mateussmeneses/theorder1886-port/issues/28) | Português-Brasil | Fallback de idioma não prova disponibilidade de assets PT-BR em todo SKU. |
| P2 | [#31](https://github.com/mateussmeneses/theorder1886-port/issues/31) | DLSS, XeSS e Frame Generation | Feature request; depende de integração/licença/hardware e avaliação de latência. |
| P3 | [#32](https://github.com/mateussmeneses/theorder1886-port/issues/32) | Toggles de pós-processamento | Sem controle por efeito identificado no renderer. O pass `pp_pass` é composição do host; primeiro localizar os passes/shaders guest de cada efeito e suas dependências. |
| P3 | [#34](https://github.com/mateussmeneses/theorder1886-port/issues/34) | Prompts de teclado/mouse | O launcher já tem glyphs para seus controles, mas isso não altera prompts desenhados pelo jogo. Requer localizar assets/passes guest e mapear os estados de input antes de estimar. |
| P2 | [#35](https://github.com/mateussmeneses/theorder1886-port/issues/35) | Resolução interna ajustável | Reporter comparou com patch 4K de PS5 e diz não notar diferença na resolução interna/FSR 4.1.1; a build anterior oferecia output target, não uma garantia de raster guest 4K. O preset temporal escolhe o render raster e o Image overlay reporta o valor; faltam SKU/GPU exatos e captura equivalente para separar fallback, preset 1.0x e falha de resolução interna. Não declarar placebo corrigido sem replay do cenário. |
| P2 | [#42](https://github.com/mateussmeneses/theorder1886-port/issues/42) | DLSS 4.5 | Feature request; sem hardware/cenário fornecido. |
| P3 | [#46](https://github.com/mateussmeneses/theorder1886-port/issues/46) | Compressão ZAR | Não iniciar leitor/launcher sem especificação pública verificável e arquivo de teste legítimo. O issue só descreve “ZIP-like” e acesso aleatório. |

### Trabalho local ainda sem validação de reporter

- Foi implementado um caminho de cópia color→depth com MSAA, com fallback para
  o dispatch guest quando a cópia não é suportada. Testes automatizados e smoke
  local passaram; isso não confirma correção das telas pretas em #43/#47/#24.
- A priorização de pipeline visível continua ligada por padrão. O A/B de warm-up
  foi uma execução por condição, com cache reportado como desconhecido; não
  sustenta troca de default nem promessa de eliminar stutters.
- Não fechar issues por commit, release note ou smoke local. Registrar como
  resolvido apenas após reprodução equivalente no hardware/cena do relato e
  confirmação do reporter.

Snapshot histórico de 2026-10-20 (superado): docs/plans/archive/2026-10-20-backlog-snapshot.md
