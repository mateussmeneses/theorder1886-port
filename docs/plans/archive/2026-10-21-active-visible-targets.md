# Plano: aguardar pipelines somente para alvos ativos — issue #41

Issue: https://github.com/mateussmeneses/theorder1886-port/issues/41

## Problema e limite

TargetsVisibleSceneAttachment considera registradores de buffers de cor grandes,
mesmo quando a operação de cor está desativada ou a máscara de escrita é zero.
PrepareRenderState não vincula esses alvos. A classificação divergente pode
provocar uma espera desnecessária pela compilação de um pipeline assíncrono.

## Implementação realizada

`TargetsVisibleSceneAttachment()` agora retorna falso quando a operação de cor
está em `Disable` e ignora cada buffer sem máscara de escrita ou sem seleção no
`mrt_mask` do pipeline pendente. `ResolvePendingGraphics()` passa a máscara da
chave que já está resolvendo. O limiar existente (256×256) e o caminho de
espera/log continuam inalterados.

A classificação ainda usa o tamanho do alvo como aproximação de visibilidade;
isso não prova que o draw pertence à cena visível.

## Verificação realizada

- Um harness temporário em `out/issue41-visible-targets-test.cpp` compila os
  corpos atuais de `TargetsVisibleSceneAttachment()` e
  `ResolvePendingGraphics()` extraídos diretamente do `.cpp`, com doubles para
  registradores, pipeline cache e captura de frame. Foi gerado por
  `out/issue41_visible_targets_test.py` e compilado/executado com:

  ```sh
  python3 out/issue41_visible_targets_test.py
  g++ -std=c++20 -Wall -Wextra -Werror -O0 out/issue41-visible-targets-test.cpp -o out/issue41-visible-targets-test
  out/issue41-visible-targets-test
  ```

  Resultado: compilação e execução passaram. Casos: alvo grande ativo, máscara
  zero, modo `Disable`, depth-only, alvo pequeno, alvo inválido, slots múltiplos
  com máscaras por slot, slot grande ausente do `mrt_mask`, pipeline pronto
  (sem espera/log), compilação pendente visível (uma espera/log de stall),
  pendente com máscara zero (sem espera/log de skip) e lookup não pendente (sem
  espera/log).
- Revisão independente ampliou os casos para máscara MRT zero em alvo grande
  válido, escrita parcial no slot 7, seleção entre slots pequenos/grandes,
  propagação da chave e falha de `WaitGraphics`. Todos passaram. O mesmo harness
  com `--baseline` compilou os corpos de `origin/master` e falhou na regressão de
  máscara de escrita zero. Os artefatos em `out/` são ignorados; não constituem
  um novo teste permanente da suíte.
- `python3 -m unittest discover -s tests`: 292 testes; 214 passaram, 76 foram
  ignorados e 2 falharam no `setUp` por falta de `tkinter` em
  `test_launcher_game_files.py`.
- `bash tools/run_tests.sh`: não completou. O gate parou na compilação porque
  `ninja` não está instalado (`ninja: command not found`); o ambiente também
  não tem `out/gpu/build.ninja`.
- Não há captura do cenário do reporter; não afirmar que o stutter/flicker da
  issue foi resolvido. O PR deve referenciar a issue sem fechamento automático.
