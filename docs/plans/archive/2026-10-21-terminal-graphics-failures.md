# Plano: aposentar falhas de compilação gráfica — issue #40

Issue: https://github.com/mateussmeneses/theorder1886-port/issues/40

## Problema e limite

Uma exceção no job de compilação gráfica permanece na coleção de pendências,
percorrida repetidamente pelos lookups. O build retém snapshots de shaders. O
caminho que espera por uma compilação remove a falha, mas permite reenfileirar a
mesma chave. Isso contradiz a política de falha terminal por sessão do AsyncJob.
Não há log do reporter para atribuir a esse defeito o uso de CPU após 30 minutos.

## Implementação

1. Aposentar builds gráficos que encerraram com exceção nos caminhos de polling
   e espera, liberando snapshots e registrando a falha uma única vez.
2. Reter somente a chave com falha terminal durante a sessão, impedindo novas
   tentativas assíncronas ou síncronas e classificações falsas de ainda pendente.
3. Preservar publicação de sucessos, jobs ainda ativos e fallback de compute.
4. Consultar a chave atual depois de `RefreshGraphicsKey`, inclusive no wait de
   alvos pequenos; rejeitar chaves terminais no caminho preparado e no pump de
   warm-up (enfileiramento/publicação, ambos na GPU thread).
5. Preservar um pipeline válido que o warm-up já publicou antes da exceção do
   job concorrente. Ao esperar por sucesso concorrente, retornar o vencedor
   armazenado no cache, nunca o ponteiro do resultado descartado pelo emplace.

## Verificação

- Injetar exceção em um job real do AsyncCompiler e verificar liberação de
  snapshots, retirada da fila e ausência de reenvio em lookups repetidos.
- Cobrir polling e espera, além de uma chave distinta que compila com sucesso.
- Verificar que falhas sem vencedor válido retornam null sem sinalizar pendência.
- Revisão independente, suíte Python e tentativa do gate tools/run_tests.sh.
- Soak de 30 minutos e captura por thread continuam pendentes. Referenciar a
  issue sem declarar sua causa raiz nem fechá-la automaticamente.

## Execução e evidência — 21/10/2026

Implementado em `vk_pipeline_cache.cpp/.h` e `vk_pipeline_serialization.cpp`.
Jobs excepcionais são removidos após polling/espera; permanece somente a chave
terminal quando nenhum pipeline válido da mesma chave já venceu no cache. O
indicador de pendência deixa de classificar esses resultados como trabalho ativo.
A primeira publicação válida continua vencendo colisões com o warm-up. Compute
mantém seu fallback existente; o conjunto de falhas é acessado apenas na GPU
thread.

Regressão nativa temporária, mantida somente em `out/terminal-graphics/` (ignorado
pelo Git), sem criar abstração de produção para o teste. O gerador extrai os
corpos reais de `PublishGraphics`, `WaitGraphics`, `TryPreparedPipeline`,
`GetGraphicsPipeline` e `PumpWarmUp`, além das estruturas reais dos builds. Usa
os headers reais `AsyncCompiler`/`AsyncJob` e `WarmupInbox`/`PublishFirst`; troca
Vulkan, seleção de registros e armazenamento por doubles. A exceção é lançada
pelo construtor do pipeline dentro do job real, enfileirado pelo lookup real.
Não é um build completo do renderer nem um teste de GPU.

Comandos executados a partir deste worktree:

```sh
python3 out/terminal-graphics/extract.py origin/master
g++ -std=c++20 -O1 -g -pthread -Igpu/shadps4 \
    out/terminal-graphics/harness.cpp -o out/terminal-graphics/baseline-test
python3 out/terminal-graphics/extract.py working
g++ -std=c++20 -O1 -g -pthread -Igpu/shadps4 \
    out/terminal-graphics/harness.cpp -o out/terminal-graphics/working-test
for case in poll wait success prepared active current-key warmup warmup-collision \
            wait-collision poll-winner wait-winner; do
    out/terminal-graphics/baseline-test "$case"
    out/terminal-graphics/working-test "$case"
done
```

- **RED** em `origin/master` (`10e44b4`): nove cenários reprovam. `poll` retém o
  build/snapshot e sinaliza pendência; `wait` permite retries; falham também
  seleção síncrona da chave atual, caminho preparado, warm-up, retorno do
  vencedor em wait e exceção concorrente com um vencedor válido. Os controles
  `active` e `warmup-collision` passam na base.
- **GREEN**: os onze cenários passam. `poll`/`wait` comprovam, por `weak_ptr`,
  liberação do build e snapshot; repetem dezesseis lookups por chave falha em
  modos assíncrono, binário conhecido, síncrono e alvo pequeno, sem novas
  construções, enfileiramento, publicação nem novo log. Cobrem também outra
  chave válida, job ainda ativo, warm-up sem reenfileiramento/ressurreição,
  publicação de compute e os dois caminhos de colisão com um vencedor válido.
- Logs: `out/terminal-graphics/baseline-results.log` e
  `out/terminal-graphics/working-results.log`.
- Teste existente do compilador real compilado e executado com sucesso:

```sh
g++ -std=c++20 -O1 -g -pthread -Igpu/shadps4 \
    tests/test_async_compiler.cpp -o out/terminal-graphics/async-compiler-test
out/terminal-graphics/async-compiler-test
```

O mesmo harness passou nos onze cenários com AddressSanitizer e
UndefinedBehaviorSanitizer, incluindo leak detection padrão, sem diagnósticos:

```sh
g++ -std=c++20 -O1 -g -pthread -fsanitize=address,undefined \
    -fno-omit-frame-pointer -Igpu/shadps4 out/terminal-graphics/harness.cpp \
    -o out/terminal-graphics/sanitized-test
for case in poll wait success prepared active current-key warmup warmup-collision \
            wait-collision poll-winner wait-winner; do
    out/terminal-graphics/sanitized-test "$case"
done
```

Log: `out/terminal-graphics/sanitized-results.log`.

A suíte Python foi executada pelo coordenador neste worktree e comparada aos
outros worktrees: `python3 -m unittest discover -s tests`, 292 testes, 214 passam,
76 skips e os mesmos dois erros de ambiente por ausência de Tk. Logs locais em
`out/test-python.log`. O gate `bash tools/run_tests.sh` foi tentado e para na
compilação: `ninja: command not found` (`out/test-full.log`,
`out/tests-build.log`). CMake e o dump do jogo também não estão disponíveis;
não se declara aprovação do gate completo nem reprodução do uso de CPU da issue.
Soak de 30 minutos, captura por thread e validação no renderer/Vulkan real
continuam pendentes. A issue #40 deve ser referenciada sem fechamento automático.

A revisão independente final não encontrou pendências de correção e repetiu os
onze cenários do harness com sucesso após os ajustes de colisão do cache.
