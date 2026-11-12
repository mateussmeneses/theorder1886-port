# 02 — Comparação com o SpartanPort

Referência: FelipePrado0/SpartanPort @ `6a709602` (2026-11-08, launcher 3.1.0), remote local
`spartan`. [FATO] `git ls-remote spartan HEAD` em 2026-11-12 devolve o mesmo commit: **não há
nada novo desde a adoção** registrada em `docs/plans/2026-11-11-spartanport-adoption.md`.

Este documento não repete aquele plano (inventário de 120 commits, fases 1–7 concluídas nos PRs
#125–#141). Ele responde a outra pergunta: **quais decisões de engenharia fazem o SpartanPort
funcionar bem, e quais ainda não temos.**

## 1. Mesma linhagem, escolhas diferentes

| Aspecto | SpartanPort | Este port | Leitura |
|---|---|---|---|
| Estratégia | Reconstrução de runtime: eboot original + runtime C + video core shadPS4 | Idêntica (bifurcou em 2026-10-14/16, `90a0060`) | Correções de runtime/GPU são portáveis quase diretamente |
| Launcher | Rust/iced, 67,8 mil linhas, único distribuído; `runner/` reimplementa a preparação | Python/Tk, 2,8 mil linhas + `run.py`/`scripts/` | Decisão registrada: manter o Python (`2026-11-12-launcher-decision.md`) |
| Testes | 163 arquivos em `tests/` + 23 suítes Rust (`ui_snapshots`, `env_contract`, `runner_fixtures`, `packaging`) | 137 arquivos | Faltam aqui: contrato launcher→jogo e fixtures de paridade |
| CI de pacote | `windows-zip.yml`: build, pacote, sha256, **smoke sem MSYS2**, release ou **pré-release de teste por issue** | Pacote só local (`tools/release.sh`) | Lacuna relevante |
| Atualização | `core/update.rs` (830 linhas): lista releases, lê `<zip>.sha256`, baixa para `.partial`, verifica, renomeia; `release-windows.json` com canal | Baixa o ZIP da última release e copia por cima, sem sha256 | Lacuna relevante |
| Documentação ao jogador | CONTROLS/MODS/PATCHES/TROPHIES em 3 idiomas, com imagens | README em 3 idiomas, `MODS.md` | Menor prioridade |
| Método para FPS alto | `docs/high-fps/catalog.md`: catálogo de bugs por mecânica, com issue, taxa, "some a 60?", save e estado; `tools/find_rate_readers.py` | Correção pontual (QTE, #101) | Método reutilizável |

## 2. Decisões de engenharia que valem reproduzir

### D1 — Builds de teste por issue, feitos pelo CI [RECOMENDAÇÃO: adotar]
[FATO] `windows-zip.yml` aceita uma tag nova no formato `windows-v<versão>-issue<N>-test<M>` e
publica uma pré-release naquele commit com a nota "relate na issue se abre e roda".
**Por que importa aqui:** nosso maior bloqueio é a validação em AMD/Intel e em cenas que só os
relatores alcançam. Hoje um relator só testa uma correção depois de uma release completa. Com
builds por issue, o ciclo hipótese → build → reteste cai de uma release para um PR.
Adaptação: workflow novo que reutiliza `build.sh` + `packaging/windows/package.sh`; o smoke com
jogo continua local (o CI não tem o dump).

### D2 — Contrato launcher → jogo testado [RECOMENDAÇÃO: adaptar]
[FATO] `launcher-rust/tests/env_contract.rs` verifica, para cada linha do catálogo de
configurações, qual variável de ambiente o jogo recebe e o que o runner assume quando ela falta.
**Por que importa aqui:** temos 263 variáveis `THEORDER_*`, 57 chaves de `.ini` e um launcher
separado; já houve regressão de override do upscaler pelo launcher (`PROGRESS.md`, 2026-10-27).
Adaptação: um catálogo declarativo (Python) chave → variável → padrão → efeito, um teste que
falha quando uma chave do launcher não chega ao jogo, e geração do `recomplauncher.json` a partir
dele (hoje expõe só 5 configurações).

### D3 — Atualização verificada [RECOMENDAÇÃO: adaptar e superar]
[FATO] O SpartanPort verifica sha256 e baixa para arquivo parcial antes de renomear. A instalação
no Windows ainda é uma cópia sobre a pasta (`install_over`), sem rollback.
Superar: staging em pasta irmã, verificação, troca por renome de diretórios, pasta anterior
mantida até o primeiro início bem-sucedido, remoção de arquivos órfãos por manifesto (doc 06, fase F6).

### D4 — Smoke do pacote fora do ambiente de build [RECOMENDAÇÃO: adotar]
[FATO] `packaging/windows/smoke-test.ps1` abre o ZIP num runner limpo, sem MSYS2 — pega DLL
faltando antes do usuário. O nosso `smoke_package.py` roda na máquina que tem MSYS2 instalado.

### D5 — Catálogo de bugs por mecânica com protocolo de confirmação [RECOMENDAÇÃO: adotar o método]
[FATO] O catálogo de FPS alto separa relato de confirmação e agrupa por mecânica suspeita.
Aplicação aqui: um catálogo equivalente para **artefatos gráficos por cena e GPU** e outro para
**bugs dependentes de FPS** (o QTE do #101 mostra que a engine multiplica por tempo de frame em
alguns pontos; 60/120 FPS são patches).

### D6 — Testes de snapshot de UI e fixtures de paridade do runner [NÃO se aplica agora]
Fazem sentido para quem reescreve o launcher; nós o mantivemos. Reavaliar se o launcher for trocado.

## 3. Itens técnicos ainda não incorporados

| Item (commit) | Estado aqui | Classe | Ação |
|---|---|---|---|
| Upscaling: FSR 4.1 (7280104), presets escalados (9f501f6), resolução de render como saída (9bd1fec), máscara reativa por stencil (8d3fca2), ghosting (9fdc30c, 3ad12a7), olhos pretos (bf9181b) | Bloqueados por #138 | Adaptação | Fase F4, depois da câmera na 01.02 |
| Frame generation em jogo | Código portado, inativo | Adaptação | Fase F4 |
| Toggles de efeitos (3598158, 5831938, eae407c) | Ausente; precisa dos hashes dos passes do The Order | Adaptação com RE | Fase F8 |
| Opções > Gráficos ao vivo (2013147), anisotropia (d402ae6) | Parcial (`anisotropy` existe no `.ini`) | Direto | Fase F8 |
| Overlay de desempenho completo | Parcial (FPS, frametime, GPU, CPU, RAM, VRAM já no `.ini`) | — | Verificar antes de portar; provável que já esteja |
| Discord rich presence (`runner/discord.rs`) | Ausente | Direto, baixo valor | Opcional |
| Seleção de monitor e GPU | A verificar | Direto | Fase F5 (ajuda em notebooks com iGPU+dGPU) |
| Troca de confirmação Cross/Circle (fefc68f), diagramas de controle (4ccf29d) | Não aplicados | Baixo valor | Opcional |
| Testes de regressão para 1974453 e 3a555df (cache de textura) | **Faltam** (pendência registrada) | Dívida | Backlog T-030 |
| `runtime-test` travando no Windows | Fora do gate | Dívida | Backlog T-031 |
| 368d899, b776be7, 7cdbb40, d6d1aea | Pulados com motivo | — | Nenhuma |

## 4. O que não faz sentido na nossa arquitetura

Launcher Rust inteiro (metade é específica de God of War III); cheats, QTE automático, câmera
livre, troca de voz, patch `R_UI.WAD`; snapshots de UI do iced; `got_import.py` e
`spartanport_frame_map.py` (específicos do jogo).

## 5. Onde já estamos à frente

[FATO] Ferramental de medição (`batch_runs.py` com condições intercaladas, `tools/bench/*`,
`flicker_scan.py` validado com falha plantada), processo (gate obrigatório, revisão em dois eixos,
rótulos de ciclo de vida, regra de "provar ativação") e o caminho para CPUs antigas. O SpartanPort
não tem equivalente público de benchmark A/B. A meta não é alcançá-lo em processo, e sim em
**alcance de validação** (D1) e **distribuição** (D3, D4).
