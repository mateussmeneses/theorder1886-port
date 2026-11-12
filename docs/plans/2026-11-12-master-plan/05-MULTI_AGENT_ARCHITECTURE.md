# 05 — Arquitetura multi-modelo

Base factual: documentação oficial do Claude Code consultada em 2026-11-12
(`code.claude.com/docs/en/sub-agents`, `agent-teams`, `worktrees`, `headless`, `model-config`,
`costs`) e o comportamento observado nesta sessão. Os fatos vieram de resumos das páginas:
conferir a redação exata antes de depender de um detalhe.

## 1. O que o ambiente oferece de fato

| Recurso | Estado | Fato relevante |
|---|---|---|
| Subagentes (`.claude/agents/*.md`) | **Disponível** | Frontmatter aceita `model` (`fable`, `opus`, `sonnet`, `haiku`, id completo ou `inherit`), `tools`, `skills`, `effort`, `isolation: worktree`, `maxTurns`, `memory`. Ordem de resolução do modelo: parâmetro da chamada → frontmatter → `CLAUDE_CODE_SUBAGENT_MODEL` → modelo da sessão |
| Execução paralela | **Real** | Subagentes rodam concorrentes e em segundo plano (limite padrão 20). Nesta sessão, quatro rodaram em paralelo |
| Contexto do subagente | Limitado por projeto | Recebe o próprio prompt, `CLAUDE.md`, um snapshot do git status e as skills pré-carregadas. **Não** recebe a conversa do planejador nem os arquivos já lidos. Devolve só o relatório final |
| Worktrees | **Disponível** | `isolation: worktree` cria uma árvore isolada; sem mudanças, é removida sozinha |
| Agent Teams | **Experimental, desligado** | `CLAUDE_CODE_EXPERIMENTAL_AGENT_TEAMS=1`. Lista de tarefas compartilhada e mensagens entre colegas. No Windows só o modo in-process; sem `/resume` dos colegas; custo documentado bem maior que o de uma sessão |
| Sessões headless | **Disponível** | `claude -p --model … --output-format json --max-turns … --permission-mode …` |
| Modelo por sessão | `/model` | O planejador (Fable) e os executores são escolhidos separadamente |

**Decisão (D-07 no doc 08):** usar **subagentes com modelo fixo no frontmatter + worktrees**.
Não usar Agent Teams: é experimental, custa mais, e nossas tarefas não precisam de conversa entre
executores — precisam de briefings fechados e de um único integrador.

## 2. Divisão de responsabilidade ≠ paralelismo

- **Paralelo de verdade:** pesquisa e leitura (Haiku/Sonnet), tarefas em arquivos disjuntos, cada
  uma no seu worktree.
- **Sempre serial:** tudo que precisa do jogo rodando. Há uma GPU, um dump e 16 GB de disco;
  duas medições simultâneas se contaminam. Build completo e `batch_runs` têm **um dono por vez**.
- **Serial por subsistema:** `vk_rasterizer.cpp` (4,6 mil linhas), `vk_pipeline_cache.cpp` e
  `runtime_memory.c` são pontos de conflito; uma tarefa aberta por arquivo.

## 3. Papéis

| Papel | Modelo | Faz | Não faz |
|---|---|---|---|
| **Planejador** | Fable 5.2 (sessão principal) | Mantém docs 06–08, escreve briefings, decide escalonamento, avalia evidência, atualiza `docs/STATUS.md` | Implementar; rodar lotes longos |
| **Executor principal** | Opus 5.5 | Renderer, cache de pipelines, runtime de memória/threads, RE com REA, causa raiz de artefatos, integração | Levantamentos, documentação, tarefas mecânicas |
| **Executor especializado** | Sonnet 5.5 | Launcher e atualizador, ferramentas de medição, testes, CI, refatorações com teste, portes mecânicos do SpartanPort | Decisões de arquitetura; mudanças sem teste em `gpu/shadps4` |
| **Auxiliar** | Haiku 5.5 | Ler e resumir logs/anexos de issues, triagem e rótulos, inventários (`grep`), rascunhos de resposta, checagem de notas de release | Editar código de produção; julgar causa raiz |
| **Revisor** | modelo diferente do autor | Revisão em dois eixos exigida por `AGENTS.md` | Revisar o próprio trabalho |

### Critérios de escalonamento

Vai para **Opus** quando qualquer um vale: toca `gpu/shadps4/video_core` ou
`shader_recompiler` além de uma linha óbvia; envolve sincronização, tempo de vida de objeto
Vulkan ou layout de memória do guest; exige RE do eboot; a causa é desconhecida; um Sonnet falhou
duas vezes no mesmo critério de aceitação.

Fica com **Sonnet** quando: o comportamento esperado cabe num teste escrito antes; o arquivo é
Python, shell, YAML ou C isolado; é um porte com commit de origem identificado.

Fica com **Haiku** quando: a saída é texto ou tabela; nenhuma decisão depende de entender o
renderer; o erro custa pouco e é fácil de ver.

**Revisão cruzada:** código do Opus é revisado por Sonnet (eixo Standards) e pelo planejador
(eixo Spec, contra o briefing). Código do Sonnet em `gpu/` ou `src/` é revisado pelo Opus.

## 4. Subagentes a criar (`.claude/agents/`) — tarefa T-002

| Arquivo | `model` | `tools` | `skills` | `isolation` |
|---|---|---|---|---|
| `renderer-engineer.md` | opus | todas | `wb-vulkan-frame-pacing`, `wb-shader-cache-triage`, `wb-debug-rigorous` | worktree |
| `reverse-engineer.md` | opus | todas + MCP `rea` | `reverse-engineer-anything`, `wb-reverse-engineering` | — (não edita código) |
| `port-engineer.md` | sonnet | todas | `wb-test-planning`, `wb-oracle-testing` | worktree |
| `scout.md` | haiku | Read, Grep, Glob, Bash, WebFetch | — | — |
| `reviewer.md` | sonnet | Read, Grep, Glob, Bash | `wb-change-review` | — |

O corpo de cada arquivo carrega só o que é estável: regras de `AGENTS.md` que o papel mais viola
(provar ativação, tamanho da amostra, não fechar issue por commit), o shell correto para testes
e o formato de saída abaixo. Os textos em `.ai-workbench/agents/` servem de ponto de partida.

## 5. Contrato de entrada e saída

**Entrada (briefing)** — exatamente os campos do doc 07: objetivo, contexto técnico mínimo,
arquivos, restrições, testes, critérios de aceitação, evidências, condição de conclusão. O
executor não recebe o roadmap; recebe um ID de tarefa e, no máximo, um documento de tópico.

**Saída (relatório)** — texto curto e fixo:

```
TAREFA: T-0xx          RESULTADO: concluída | parcial | bloqueada
BRANCH/PR: …           COMMITS: …
MUDANÇAS: arquivo — o quê (1 linha cada)
EVIDÊNCIA: comando → resultado (testes, contadores de ativação, manifestos de batch)
NÃO VERIFICADO: o que depende de hardware/cena/relator
ACHADOS FORA DO ESCOPO: …
```

Tudo que for maior que isso (tabelas de medição, saídas de RE) vai para um arquivo — manifesto em
`out/`, documento de tópico em `docs/` — e o relatório cita o caminho.

## 6. Compartilhamento de contexto

| Estado | Onde vive | Quem escreve |
|---|---|---|
| Situação atual | `docs/STATUS.md` | Planejador, no PR que muda o estado |
| Fases e tarefas | `06-MASTER_ROADMAP.md`, `07-EXECUTION_BACKLOG.md` | Planejador |
| Decisões | `08-TECHNICAL_DECISIONS.md` | Planejador (executores propõem no relatório) |
| Conhecimento de RE | `docs/ENGINE_RE.md` + nomes/comentários na sessão REA | `reverse-engineer` |
| Medições | manifestos de `batch_runs` + `docs/PERFORMANCE_BASELINE.md` | Quem mediu |
| Preferências duráveis | memória do projeto | Planejador |

Regra: **o que um executor descobre e outro vai precisar entra num arquivo antes do fim da
tarefa.** Nada depende de memória de conversa.

## 7. Controle de alterações simultâneas

1. Um worktree e um branch por tarefa (`fix/…`, `feat/…`, `perf/…`), um PR por mudança lógica — já
   é regra do repositório.
2. O backlog marca os arquivos de cada tarefa; o planejador não libera duas tarefas abertas com
   interseção.
3. Só o planejador (ou o mantenedor) faz merge, depois do `gate` verde e da revisão.
4. Medição com o jogo: fila única. Uma tarefa que precisa medir declara isso no briefing e roda
   sozinha.

## 8. Alternativa sem subagentes

Se a delegação nativa não estiver disponível (outra máquina, limite de uso), o mesmo contrato
funciona com sessões independentes: `claude --model opus --worktree T-0xx`, colando o bloco da
tarefa do doc 07; ou `claude -p --model sonnet --output-format json --max-turns N` para tarefas
sem interação. O relatório volta pelo PR.

## 9. Orçamento

- O planejador não lê código que possa pedir a um `scout` para localizar; lê o trecho apontado.
- Cada briefing nomeia os arquivos; "explore o repositório" não é um briefing.
- Haiku para tudo que é leitura em volume (anexos de issue, logs, inventários).
- Opus só entra com a hipótese e a cena já definidas; a fase F0/F1 existe para isso.
- Uma tarefa bloqueada devolve o relatório e para; não tenta um segundo caminho por conta própria.
