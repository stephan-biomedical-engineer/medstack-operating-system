# Testes de host do driver MCP2210

Fase 7 do `docs/implementation_plan_mcp2210.md`. Roda em segundos, sem bitbake,
sem imagem, sem placa e sem toolchain cruzada:

```
make -C tests/mcp2210 check     # ou `make test` na raiz, que roda as duas suites
```

## Como funciona

`linux-med/drivers/hid/hid-mcp2210.c` é compilado **byte a byte como ele
embarca** — sem macro de teste, sem `#ifdef`, sem costura própria. Os `#include
<linux/...>` resolvem para os stubs em `linux/` e `asm/` deste diretório, que
apontam para `kernel_shim.h` e `kernel_shim_dev.h`.

A substituição acontece um nível **abaixo** do driver, em
`hid_hw_output_report()`. O dispositivo falso recebe o relatório de 64 bytes que
sai, monta a resposta e a entrega chamando o `.raw_event` do próprio driver — o
mesmo caminho que o núcleo HID usa para um relatório de entrada. Consequência:
`mcp2210_command()`, a decodificação do byte de estado e `mcp2210_raw_event()`
estão **sob teste**, não substituídos.

Por que não KUnit: a costura de que esta suíte precisa é a troca de relatórios,
e dentro do kernel isso seria um ponteiro de função ou um `#ifdef` dentro de um
arquivo destinado à `linux-input`. Andaime de teste num patch de mainline é
exatamente o que um revisor pede para remover.

O AddressSanitizer está ligado por padrão e não é enfeite: `rx_flat` fica
imediatamente antes das configurações em cache dentro de `struct mcp2210`, então
o único defeito de memória que este driver pode ter escreve por cima do próprio
vizinho. Sem ASan isso aparece como `free(): invalid pointer` do glibc em algum
ponto posterior e não relacionado; com ele, é nomeado na instrução que o causou.

## O que é evidência e o que é só consistência

`fake_mcp2210.c` foi escrito a partir de `docs/Register_Map_MCP2210.md` — a
**mesma transcrição** de onde saiu o driver. Isso divide a suíte em duas
metades, e a divisão é a mesma que o `implementation_plan_afe_bench.md` §13.1
descreve:

| | Vale como | Por quê |
|---|---|---|
| Deslocamentos, opcodes, bits de modo | **consistência, não correção** | um teste escrito da mesma leitura que escreveu o código só prova que os dois concordam |
| Fragmentação, limite de estagnação, acumulação, truncamento, recuperação, correlação de resposta | **evidência** | não dependem de o datasheet estar certo; dependem de o código fazer o que ele mesmo diz |

A primeira linha só vira evidência com vetores de origem independente: exemplos
trabalhados do próprio DS20005176, o driver GPLv2 de terceiros lido como oráculo
(e não como dependência), ou uma captura com analisador USB.

## O que a suíte diz hoje

**150 verificações, 0 falhas, 0 defeitos confirmados.**

Os cinco defeitos que esta suíte demonstrou foram corrigidos pelas Fases 3 e 4
do plano, e as verificações que os documentavam foram promovidas para `CHECK`.
O mecanismo `DEFECT` fica, porque a próxima fase vai precisar dele.

Um `DEFEITO` não é falha nem passagem: é um comportamento que esta suíte
**espera estar errado**, porque a correção dele é uma fase do plano que ainda
não foi executada. Ele não afeta o código de saída. Se um deles passar a se
comportar corretamente, o runner avisa — é o sinal de que a fase entrou e a
verificação deve ser promovida para `CHECK`.

Os cinco que foram, e onde estão agora:

| Defeito | Corrigido em |
|---|---|
| `0xF7` (barramento com dono externo) repetido 100 vezes como se fosse `0xF8` | Fase 3 |
| uma resposta com eco divergente aceita em vez de descartada | Fase 3 |
| uma resposta atrasada de outro comando virando a contagem de bordas | Fase 3 |
| uma contagem de recepção inflada corrompendo os dados com status 0 | Fase 4 |
| depois de um timeout a próxima transferência falhando para sempre | Fase 4 |

O quarto não estava no plano. Ele saiu de escrever o dispositivo falso: o driver
confiava no byte 2 da resposta sem nunca compará-lo com o que ainda devia, então
um dispositivo que inflasse a contagem fazia a transferência parar cedo e
**devolver sucesso** com dados incompletos. Um erro de transporte virando um
valor plausível, que é a forma de defeito que este repositório mais teme.

## A suíte foi injetada com falhas antes de ser acreditada

Uma asserção que nunca viu a falha que procura é uma alegação. Cada correção das
Fases 1, 2 e 3.4 foi revertida no driver, uma de cada vez, e a suíte rodada:

| Injeção | Resultado |
|---|---|
| polaridade do byte de reposição do `0x12` | 3 falhas, todas em B |
| os bytes 13–16 das *chip settings* | 2 falhas, ambas em A2 |
| a reivindicação do pino de chip select | 4 falhas, em A3 e A4 |
| o limite em `received + got > len` | `heap-buffer-overflow`, `WRITE of size 60` |
| a verificação do eco do comando (Fase 3) | 5 falhas, em E1 e E2 |
| o cancelamento no caminho de erro (Fase 4) | 9 falhas |
| a recuperação de uma ponte já ocupada (Fase 4) | 3 falhas, todas em E5 |
| a recusa de uma contagem impossível (Fase 4) | 4 falhas |
| o orçamento derivado de `len`/`speed_hz` (Fase 4) | 3 falhas, em D4 |

Cada injeção falha **só** as verificações que lhe dizem respeito, o que prova as
duas coisas que interessam: que a suíte enxerga o defeito, e que as outras
verificações não o enxergam por acidente.

**E uma injeção já apodreceu uma vez.** A do E4 mirava por índice de troca
(`silent_start = exchanges + 2`). Quando a Fase 4 inseriu o *read-back* de taxa
entre o `0x40` e o primeiro `0x42`, ela passou a derrubar a resposta errada — e
o teste continuou passando enquanto testava outra coisa. As injeções agora miram
por **código de comando**. Uma asserção pode parar de ver a falha que procura
sem que nada fique vermelho, e mirar por posição é como isso acontece.

## O que isto NÃO diz

Impresso a cada execução, inclusive numa verde — um conjunto de testes que
silenciosamente testa menos do que afirma é o modo de falha que este repositório
já pagou uma vez.

- Nada sobre silício. Nenhum módulo foi carregado, nenhuma amostra adquirida,
  nenhuma constante verificada contra o datasheet.
- Nada sobre concorrência, tempo real ou memória do kernel: o shim não tem
  threads, não dorme e não falha ao alocar. Qualquer afirmação sobre travamento
  ou corrida saída daqui vale zero.
- Nada sobre o contrato do núcleo SPI que a Fase 5 cobre: `delay`, `cs_change` e
  `bits_per_word` continuam descartados em silêncio, e esta suíte não os cobra.
- Nada sobre a bancada. Isto antecipa a classe de defeito que ela encontraria do
  jeito caro, com um conversor no meio e duas explicações possíveis para cada
  sintoma.

## Arquivos

| | |
|---|---|
| `check.h` | o runner, com a terceira categoria (`DEFECT`) |
| `kernel_shim.h` | tipos, alocação, `unaligned`, mutex, completion |
| `kernel_shim_dev.h` | modelo de dispositivo, HID, SPI, GPIO, sysfs, e a costura |
| `linux/`, `asm/` | stubs que redirecionam os `#include` do driver |
| `fake_mcp2210.{h,c}` | o dispositivo falso e as políticas injetáveis |
| `main.c` | os cinco grupos de verificação |
