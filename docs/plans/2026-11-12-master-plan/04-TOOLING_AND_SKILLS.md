# 04 — Ferramentas, skills e integrações

Disponibilidade verificada em 2026-11-12 nesta máquina (`command -v`, `Program Files`,
`~/.claude`). Nada foi instalado. "Ausente" significa não encontrado no PATH nem nas pastas
usuais; pode existir em outro lugar.

## 1. Inventário real

| Ferramenta | Estado | Observação |
|---|---|---|
| REA (MCP `rea` + CLI) | **Instalado e conectado** | Decompilação, xrefs, renomeação, sessão persistente. Fiação com Ghidra em `docs/ENGINE_RE.md` e na memória `rea-eboot-setup` |
| Ghidra (`ghidraRun`, `analyzeHeadless`) | Ausente do PATH | Usado por baixo do REA; não chamar direto |
| IDA, x64dbg | Ausentes | Não necessários: o guest é depurado pelo próprio runtime (`fault_stacks.py`, `THEORDER_WATCH`) |
| RenderDoc | **Ausente** | Há captura própria (`THEORDER_FRAME_CAPTURE_KEY`, dumps de frame) |
| Tracy | **Stub** (`gpu/shim/tracy/Tracy.hpp`) | As macros já estão espalhadas pelo código do shadPS4; falta o cliente real |
| PresentMon | **Ausente** | — |
| WPR / xperf | Instalados (Windows Performance Toolkit) | GPUView ausente |
| PIX, Nsight | Ausentes | PIX é para D3D12 (não se aplica); Nsight Graphics serve só em NVIDIA |
| `vulkaninfo` | Instalado | Camadas de validação: `THEORDER_VK_VALIDATION` existe no código |
| clang-tidy, cppcheck, clang-format, ruff | Ausentes do PATH do Git Bash | Podem existir no MSYS2 CLANG64 (não verificado) |
| spirv-val, spirv-cross, glslangValidator | Ausentes do PATH do Git Bash | Idem |
| `gh` | Instalado e autenticado | Issues, PRs, releases |
| `cargo` | Instalado | Só relevante se o launcher Rust voltar à pauta |
| `aiwh` (AI Workbench) | Instalado | Perfis core, game-port, reverse-engineering, recompilation |

### Skills e plugins já presentes

- **Projeto (`.claude/skills/`, 17 skills `wb-*`)**: `wb-vulkan-frame-pacing`,
  `wb-shader-cache-triage`, `wb-gameplay-acceptance`, `wb-debug-rigorous`, `wb-oracle-testing`,
  `wb-reverse-engineering`, `wb-binary-analysis`, `wb-change-review`, `wb-pr-release`,
  `wb-test-planning`, `wb-context-handoff`, `wb-project-continuity`, `wb-repo-intake`,
  `wb-clean-room-rebuild`, `wb-capability-curation`, `wb-binary-recompilation`,
  `wb-project-bootstrap`.
- **Usuário (`~/.claude/skills/`)**: `reverse-engineer-anything`, `graphify`, `code-review`,
  `diagnosing-bugs`, `tdd`, `implement`, `implement-spec`, `to-spec`, `to-tickets`, `triage`,
  `handoff`, `research`, `game-development`, entre outras.
- **Plugins**: `ponytail` (ativo no projeto), `tokeneconomizer`, `context7`, `mattpocock-skills`,
  `postman`, `rust-analyzer-lsp`.
- **Papéis de referência** em `.ai-workbench/agents/`: triage-orchestrator, code-reviewer,
  root-cause-debugger, release-auditor, graphics-performance, reverse-engineer. [FATO] São textos
  de referência; **não existe `.claude/agents/`**, então nenhum deles é um subagente utilizável hoje.

## 2. Recomendações, por retorno

| # | Item | Utilidade | Custo operacional | Dependências | Benefício esperado | Prioridade |
|---|---|---|---|---|---|---|
| R1 | **PresentMon** (CLI) | Tempo de frame apresentado medido fora do processo; valida os números do log interno; única métrica comparável com outros ports | Binário único, sem instalação; integração de ~1 dia em `batch_runs.py` | Nenhuma (ETW; pode exigir grupo "Performance Log Users") | Métrica de aceitação independente (doc 09) | **Alta** |
| R2 | **Tracy real, build opt-in** | Atribui tempo por thread e por zona; ataca #40 e stalls residuais | Submódulo + flag de CMake; build separado para não pesar no pacote | Tracy cliente no build; visualizador baixado à parte | Diagnóstico de CPU/stutter sem log manual | **Alta** |
| R3 | **Subagentes do projeto** (`.claude/agents/*.md`) | Fixam modelo, ferramentas e skills por papel (doc 05) | 5 arquivos pequenos | Nenhuma | Delegação com custo previsível | **Alta** |
| R4 | **RenderDoc** | Inspeção de draw/estado/textura num frame com artefato | Instalador; capturas grandes (disco já em 99 %) | Verificar se o hook convive com o overlay e com FSR/DLSS | Diagnóstico de artefatos por cena | Média — só quando houver cena reproduzida |
| R5 | **Mesa lavapipe no CI** | ICD de software para rodar os testes de GPU no runner hospedado | Download de build do Mesa no workflow; testes mais lentos | Confirmar que os testes de dispositivo passam em lavapipe | Testes de GPU deixam de ser só locais | Média |
| R6 | **clang-tidy / cppcheck no gate** | Análise estática do `src/` (C com ponteiros do guest) e do shim | Tempo de CI; ruído inicial | Pacotes MSYS2; perfil restrito a `src/` e `gpu/shim/` (não ao código vendorizado) | Achados de memória/concorrência antes do relator | Média |
| R7 | **MCP GitHub** | Sugerido pelo AI Workbench | Configuração global | — | **Baixo**: `gh` já cobre issues/PRs/releases | Não instalar |
| R8 | **context7** | Documentação atual de Vulkan/FSR/SDL | Já instalado como plugin | — | Útil em tarefas de Vulkan e SDKs | Usar sob demanda |
| R9 | GPUView | Fila de GPU e DPCs | Curva de aprendizado alta | WPT (já presente) | Só se PresentMon + Tracy não explicarem um stall | Baixa |
| R10 | Nsight Graphics | Perfil de GPU em NVIDIA | Instalador | — | Só para ajuste fino de GPU, que não é o gargalo atual | Baixa |

[PENDENTE] R1, R2, R4, R5 e R6 pedem instalação ou mudança de CI: aprovação do mantenedor antes
de executar. `CLAUDE.md` proíbe alterar configuração global de agentes/MCP sem aprovação.

## 3. Uso das skills existentes por fase

| Fase (doc 06) | Skills |
|---|---|
| F0 medição, F3 stutter | `wb-vulkan-frame-pacing`, `wb-shader-cache-triage`, `wb-oracle-testing` |
| F1 acesso a cenas, F4 câmera | `reverse-engineer-anything` (REA), `wb-reverse-engineering` (proveniência e offsets verificados) |
| F2 artefatos | `wb-debug-rigorous`, `wb-gameplay-acceptance` |
| Todo PR | `wb-change-review` ou `ponytail:ponytail-review`; revisão em dois eixos exigida por `AGENTS.md` |
| Release | `wb-pr-release`; memória `review-before-release` |
| Troca de sessão/modelo | `wb-context-handoff` |

Não há lacuna de skill que justifique instalar outra agora: C++/sistemas, RE, Vulkan, perfil,
testes e planejamento já estão cobertos. O que falta são **ferramentas de medição** (R1, R2),
não instruções.

## 4. Cuidados já registrados na memória do projeto

- Rodar testes pelo shell de login CLANG64 do MSYS2, não pelo Git Bash (3 testes de arquivo
  temporário quebram); limpar `*.spv` antes de empacotar.
- Scripts Python de edição via heredoc corrompem `"\n"` em C: usar Write/Edit.
- Arquivos CRLF gravados por Python bloqueiam `git switch`; conferir o hash do HEAD em cada braço de A/B.
- `batch_runs` morno copia ~1 GB por execução; limpar perfis entre lotes e reportar espaço livre
  ao fim de cada tarefa.
