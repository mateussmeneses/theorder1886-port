# Plano: apresentação durante minimização — issue #10

Issue: https://github.com/mateussmeneses/theorder1886-port/issues/10

## Problema e limite

O relato inclui um crash ao alternar de fullscreen com Alt+Tab. O estado SDL de
minimização não é consumido pela janela; os caminhos de apresentação só verificam
dimensões zero, que não são garantidas ao minimizar. A recriação da swapchain
também preserva um índice de frame que pode exceder a nova quantidade de imagens.
Esses defeitos são verificáveis separadamente do OOM do driver relatado.

## Implementação

1. Publicar estado de minimização de forma segura entre as threads de janela e
   apresentação, incluindo os eventos de restauração.
2. Não adquirir/apresentar imagens durante minimização, tanto no carregamento
   quanto no gameplay; manter o retorno dos frames à fila existente.
3. Reiniciar índices quando imagens/semaforos da swapchain forem recriados.
4. Preservar resolução da janela, captura de input e comportamento de janelas
   apenas sem foco. Evitar mudanças especulativas nas políticas de memória Vulkan.

## Verificação

- Harness nativo em `out/test_minimized_native.py` (ignorado): compila os corpos reais de
  `PollEvents` e `RefreshSemaphores` e os trechos de entrada reais das duas rotas de
  apresentação com dublês de SDL/Vulkan. Passou para perda de foco sem minimização,
  minimização com tamanho não zero, resize enquanto minimizada, restauração,
  maximização, retorno do frame à fila sem aquisição e índices antigos altos com
  nova quantidade de duas imagens. Não executa o restante do renderizador Vulkan.
- O mesmo harness sobre `origin/master` falhou na transição de minimização. Com
  estado minimizado simulado, falhou também na aquisição indevida; o teste isolado
  dos índices falhou após a recriação dos semáforos.
- `git diff --check` passou. A suíte Python executada no worktree teve 214 testes
  aprovados, 76 ignorados e dois erros preexistentes ligados a Tk. O gate
  `tools/run_tests.sh` não chegou à compilação: `ninja` está indisponível.
- Alt+Tab fullscreen no HD 630, renderização de silhuetas/legendas e o restante
  do Vulkan permanecem sem validação local; a issue não deve ser fechada só por
  este patch.
