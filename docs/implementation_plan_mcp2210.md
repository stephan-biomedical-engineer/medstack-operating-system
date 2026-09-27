# Plano de implementação — o driver MCP2210 no padrão do irmão mainline

**Estado:** nada neste plano foi executado. O driver existe, compila limpo e
passa o `checkpatch --strict` sem uma única observação; o que ele ainda não faz
é estar correto contra o protocolo, e três dos defeitos abaixo invalidam
medições que o `implementation_plan_afe_bench.md` pretende fazer.

**O que este plano é:** a lista completa do que separa
`linux-med/drivers/hid/hid-mcp2210.c` de um driver que (a) funciona e (b) pode
ser postado para a `linux-input`, tomando `drivers/hid/hid-mcp2221.c` — o irmão
da mesma família, já em mainline — como a definição operacional de "padrão".

**O que este plano não é:** não é a sessão de bancada. A verificação das
constantes de protocolo contra o datasheet e a medição em silício estão no
`implementation_plan_afe_bench.md` §8-B, e este plano depende dela em vez de a
repetir. Também não é o plano do conversor: esse é o
`implementation_plan_ads1299_upstream.md`, e a relação entre os dois está no §10.

**O que bloqueia o quê:** a Fase 0 (verificação das constantes) bloqueia
qualquer afirmação sobre as Fases 1–5, porque as correções abaixo foram
derivadas de `docs/Register_Map_MCP2210.md`, que é uma transcrição — e uma
transcrição com lacunas conhecidas (§3.7). As Fases 1 e 2 bloqueiam a bancada.
A Fase 6 não bloqueia nada e não desbloqueia nada: é o preço de admissão do
mainline, e é a única que pode correr em paralelo com tudo.

---

## 1. Onde estamos, medido

| Fato | Valor | Como foi obtido |
| :--- | :--- | :--- |
| Tamanho do driver | 863 linhas | `wc -l drivers/hid/hid-mcp2210.c` |
| Estilo | 0 erros, 0 avisos, 0 checks | `scripts/checkpatch.pl --no-tree --file --strict` |
| Integração na árvore | `Kconfig`, `Makefile`, `MAINTAINERS` já editados | `git status` em `linux-med` |
| Comandos do protocolo implementados | 9 de 18 | §4 |
| Constantes definidas e nunca emitidas | 3 (`0x10`, `0x11`, `0x41`) | contagem de referências por símbolo |
| Compilação arm64 contra o kernel da placa | limpa, `W=1` | `implementation_plan_iio_afe.md` §13 |
| Módulo alguma vez carregado | **não** | — |
| Constantes de protocolo verificadas | **nenhuma** | os marcadores `[DS20005176?]` no cabeçalho |

O `checkpatch` limpo merece uma frase, porque é exatamente o tipo de número que
engana: ele mede a forma das linhas e não diz nada sobre o conteúdo delas. Um
driver que troca a polaridade de um bit de comando passa no `checkpatch` com a
mesma nota de um que não troca. Esta é a mesma lição que o `CLAUDE.md` registra
como "medir estado não é medir função", aplicada a uma ferramenta de estilo.

---

## 2. A referência: o que `hid-mcp2221.c` estabelece

O MCP2221 é o irmão I²C do MCP2210: mesma família, mesma ponte HID, mesmos
relatórios de 64 bytes, mesmo fabricante. Está em mainline
(`drivers/hid/hid-mcp2221.c`, 1251 linhas, Rishi Gupta). Não é uma inspiração
vaga — é a prova de que um driver desta classe é aceitável, e a forma dele é a
resposta pronta para quase toda objeção que um revisor faria ao nosso.

Oito padrões que ele estabelece, e onde estamos em relação a cada um:

| # | Padrão no `hid-mcp2221.c` | Evidência | Nós |
| :-- | :--- | :--- | :--- |
| 1 | `raw_event` despacha por `data[0]`, o eco do código de comando | `:760`, um `switch (data[0])` com um `case` por comando | ✓ Fase 3 |
| 2 | Traduz o byte de estado em `errno` **por comando**, dentro do `raw_event` | `mcp_get_i2c_eng_state()`, `:716` | ✓ Fase 3 |
| 3 | Comentário de cabeçalho com URL do datasheet | `:7` | ✗ |
| 4 | VID/PID vindos de `hid-ids.h`, não de `#define` local | `:1233` | ✗ |
| 5 | Um comando de cancelamento no caminho de erro | `mcp_chk_last_cmd_status_free_bus()`, `:194` | ✓ Fase 4 |
| 6 | Uma consulta de estado do motor, usada | `mcp_chk_last_cmd_status()`, `:173` | ✓ Fase 4 |
| 7 | `hid_hw_start(hdev, 0)` sem flags de conexão + `hid_device_io_start()` | `:1148`, `:1173` | ✓ |
| 8 | Múltiplos subsistemas num só módulo HID (i2c + gpio + iio), cada um sob `IS_REACHABLE` | `:609`, `:1202`, `:1224` | ✓ (spi + gpio) |

O padrão 1 é o mais importante e é de graça. O nosso `mcp2210_raw_event()`
(`:251`) copia qualquer relatório de entrada para o `rxbuf` e chama
`complete()`, sem olhar para o byte 0. O documento de protocolo diz que *toda*
resposta ecoa o código do comando — inclusive um comando não suportado, que
volta com o próprio código e `0xF9` no byte 1 (§3.6.2, Tabela 3-72). O 2221
usa esse fato como mecanismo central; nós temos a mesma garantia e não a usamos.

A consequência não é teórica e já está registrada no `CLAUDE.md`: uma resposta
de `0x12` que chegou depois do `-ETIMEDOUT` é consumida pelo comando seguinte,
e se esse comando for um `0x42` os bytes do contador de eventos entram no fluxo
de amostras com o CRC do `FrameHeader` calculado por cima deles. O dado corrompido
fica assinado como íntegro. A correção são quatro linhas.

O padrão 8 é o que autoriza a nossa forma: um revisor que perguntar "por que um
driver HID registra um `spi_controller` e um `gpiochip`?" tem como resposta que
o irmão dele registra um `i2c_adapter`, um `gpiochip` e um dispositivo IIO, no
mesmo diretório, desde 2020.

---

## 3. Os defeitos confirmados contra o documento de protocolo

Os seis abaixo foram obtidos lendo `hid-mcp2210.c` contra
`docs/Register_Map_MCP2210.md`. Nenhum foi observado em execução. Pela regra 1
do `BRINGUP_STM32MP2.md` §11 eles são exatamente o que são: leitura, não medida
— mas leitura contra um documento é uma classe de evidência acima de leitura
contra a memória de quem escreveu o código, que é a origem de todos eles.

### 3.1 O contador de interrupções faz o oposto do que o comentário diz

**Evidência.** Tabela 3-56, byte 1: `0x00` — lê **e repõe** o contador;
qualquer outro valor — lê **sem repor**.

**Código.** `interrupt_count_show()` (`:619`) escreve `txbuf[1] = 0`, sob um
comentário de sete linhas explicando que não se deve repor o contador na
leitura. `interrupt_count_reset_store()` (`:648`) escreve `txbuf[1] = 1`.
Os dois atributos estão trocados: toda leitura zera, e a reposição não faz nada.

**Efeito.** Destrói a única medição que justifica o contador existir. A
comparação "bordas que o conversor produziu" contra "amostras que o host
coletou" vira uma comparação contra um delta desde a última leitura, e o modo
de falha é silencioso: os números continuam plausíveis. O `afe_bench` §5 já
tinha um erro aritmético nesta mesma medição (a soma de janelas curtas por
causa do `wrap` em 16 bits); este é o segundo, e os dois se somam.

**Correção.** Trocar os dois valores. Melhor ainda: eliminar o par de atributos
e expor um acumulado de 64 bits mantido pelo driver, que faz a leitura com
reposição a cada consulta e soma — o que resolve o `wrap` e o defeito de uma
vez. Ver §5.3 antes de decidir, porque a forma do atributo é um assunto de ABI.

**Injeção de falha.** Ler `interrupt_count` duas vezes seguidas com o pino em
repouso. Antes da correção, a segunda leitura devolve zero. Depois, devolve o
mesmo valor da primeira.

### 3.2 Escrever as configurações do integrado apaga a configuração de GPIO

**Evidência.** Tabela 3-40 (Set (VM) Current Chip Settings, `0x21`): byte 13 =
*Default GPIO Output* (LSB), 14 = (MSB), 15 = *Default GPIO Direction* (LSB),
16 = (MSB). Não são reservados.

**Código.** `mcp2210_write_chip_settings()` (`:292`) preenche apenas as
designações (bytes 4–12) e o byte 17; `mcp2210_begin()` já zerou o resto. E os
campos 13–16 nem sequer são lidos: `mcp2210_read_chip_settings()` (`:269`) só
guarda `pin_designation[]` e `other_settings`, e a `struct mcp2210` (`:189`)
não tem onde guardá-los.

**Efeito.** O `0x21` emitido no probe — o único, e emitido justamente para
ligar a contagem de bordas — põe todos os GPs designados GPIO em saída a nível
baixo. Num chapéu onde `RESET`/`PWDN` do conversor são ativos-baixos, isso é o
probe segurando o conversor em reset. O comentário em `:286-291` argumenta
corretamente que um *read-modify-write* é obrigatório porque reconstruir as
designações a partir de suposições estragaria uma placa ligada de outro jeito;
o argumento está certo e a implementação está metade feita.

**Correção.** Guardar `gpio_default_value` e `gpio_default_dir` (ambos `u16`)
na `struct mcp2210`, preenchê-los na leitura, reescrevê-los na escrita.

**Injeção de falha.** Ler as configurações voláteis (`0x20`) antes e depois do
probe, comparar os bytes 13–16. Antes da correção diferem; depois são iguais.

### 3.3 São oito chip selects, não nove

**Evidência.** §1.3.1: "up to 8 Chip Select lines". Tabela 3-1 e Tabela 3-40,
byte 12: o GP8 aceita só `Input = 0x00` ou `Dedicated Function = 0x02` — não há
opção *Chip Select*. Tabelas 3-35/3-36, bytes 9 e 11: o byte alto dos valores
*Idle* e *Active Chip Select* é inteiramente `x`.

**Código.** `MCP2210_NGPIO` (9) é usado para três coisas diferentes:
`ctlr->num_chipselect` (`:799`), a validação `spi_chip_select >= MCP2210_NGPIO`
(`:723`) e a máscara `idle_cs = GENMASK(MCP2210_NGPIO - 1, 0)` (`:308`), que
liga o bit 8 — um bit *don't care*.

**Efeito.** Configurar o CS no GP8 passa na validação e produz um chip select
que nunca é afirmado. Um `num_chipselect` de 9 também mente para o núcleo SPI.

**Correção.** Separar as duas contagens: `MCP2210_NGPIO` (9) para o `gpiochip`,
`MCP2210_NCS` (8) para o controlador e para a máscara.

### 3.4 Nada designa o pino de chip select, e o padrão de fábrica não é o nosso

**Evidência.** §1.3.2, configuração de arranque do módulo SPI: "GP1 as Chip
Select line". O parâmetro `spi_chip_select` (`:154`) tem valor padrão 0.

**Código.** O driver lê as designações e recusa-se corretamente a alterá-las
por conta própria, mas nunca verifica se o pino que vai usar como CS *é* um CS.

**Efeito.** Este é o defeito que impede o arranque. Com a NVRAM de fábrica, o
GP1 é chip select e o GP0 é GPIO; o driver registra o filho no GP0, o CS nunca
é afirmado, e o conversor nunca vê a transação. O sintoma na bancada é uma
transferência que "funciona" e devolve lixo — o pior sintoma possível, porque
não há erro nenhum a investigar.

**Correção.** Verificar `pin_designation[cs] == MCP2210_PIN_CHIP_SELECT` antes
de registrar o filho. Se não for, há duas respostas defensáveis: falhar o probe
com `dev_err_probe()` nomeando o pino, ou corrigir a designação via `0x21`
(volátil — não toca na NVRAM, e portanto não viola o teste negativo do
`afe_bench` §8-B). A segunda é mais útil e já usa um caminho que existe;
a primeira é mais honesta sobre quem é o dono da configuração da placa. Escolher
uma e escrever o porquê no código.

**Injeção de falha.** Apontar `spi_chip_select` para um pino designado GPIO.
Depois da correção, o probe recusa (ou corrige) e diz qual pino; antes, ele
anuncia "USB-SPI bridge ready" e a primeira transferência devolve `0xFF`.

### 3.5 Os três códigos de estado do motor SPI estão errados — **corrigido**

**Evidência.** As três tabelas, uma por estrutura de resposta do `0x42`:

| Código | Tabela | Significado |
| :--- | :--- | :--- |
| `0x20` | 3-60, Resposta 2 | transferência iniciada — sem dados a receber |
| `0x30` | 3-62, Resposta 4 | transferência **não** concluída; dados disponíveis |
| `0x10` | 3-63, Resposta 5 | transferência **concluída** — não há mais dados |

**Código, antes.** `MCP2210_SPI_STARTED_NO_DATA` = `0x10`,
`MCP2210_SPI_NOT_FINISHED` = `0x20`, `MCP2210_SPI_FINISHED` = `0x30`. Os três,
numa rotação: `FINISHED` valia o código de "não concluída", e
`STARTED_NO_DATA` valia o de "concluída".

**Como isso acontece.** Um campo com três valores, documentado em três tabelas
distintas, cada uma dentro de uma estrutura de resposta diferente. Não há um
lugar no documento onde os três apareçam lado a lado, e é exatamente essa a
forma de um erro de leitura que sobrevive à revisão.

**Efeito: nenhum, e isso foi medido e não deduzido.** O laço de
`mcp2210_do_transaction()` é conduzido por contagem de bytes; o único lugar que
lê o estado é uma saída antecipada já protegida por `received >= len`. Com as
constantes erradas de volta, a suíte de host fica em 130 verificações, 0 falhas
e os mesmos 5 defeitos. A Fase 7 já tinha antecipado isso por outro caminho: a
transferência completa mesmo quando o dispositivo devolve um byte de estado que
não está em tabela nenhuma.

**Correção.** Feita. A aplicação natural do `FINISHED` corrigido — detectar um
dispositivo que se declara concluído devendo bytes — foi resolvida pela Fase 4
por outro caminho e mais forte: qualquer contagem de recepção impossível, e não
só a do estado final, agora é `-EPROTO`.

### 3.6 Os atrasos e o `cs_change` do `spi_message` são descartados

**Evidência.** Tabela 3-35, bytes 12–17: *CS to Data*, *Last Data Byte to CS* e
*Delay Between Subsequent Data Bytes*, todos em quanta de 100 µs.

**Código.** `mcp2210_set_transfer_settings()` escreve zero nos três (`:318-320`).
`mcp2210_transfer_one_message()` (`:402`) achata todas as `spi_transfer` numa
transação só, somando `len` e ignorando `xfer->delay`, `xfer->cs_change` e
`xfer->bits_per_word`.

**Efeito.** Com um `transfer_one_message` próprio, o núcleo SPI **não** executa
os atrasos — é o controlador que responde por eles. O `ti-ads1299.c` pede
`.delay = 2 µs` em três lugares (`:468`, `:787`, `:808`) e não recebe nenhum.
Aqui o resultado é benigno **por acidente**: cada ida-e-volta HID custa da ordem
de 1 ms, o que cobre 2 µs com folga de três ordens de grandeza. Um `cs_change`
no meio de uma mensagem, esse, é silenciosamente fundido numa única afirmação
de CS, e isso não é benigno em nenhuma circunstância.

**Correção.** Duas partes, e a segunda importa mais que a primeira. (a) Traduzir
`xfer->delay` para o campo *Last Data Byte to CS*, arredondando para cima no
quantum de 100 µs — e documentar que 2 µs vira 100 µs, porque o hardware não
sabe exprimir menos. (b) Recusar explicitamente o que não se sabe honrar: um
`cs_change` no meio da mensagem devolve `-EINVAL` em vez de ser fundido. Uma
recusa é um defeito visível; uma fusão silenciosa é um defeito que só aparece
no sinal.

### 3.7 A lacuna no próprio documento — **fechada**

`docs/Register_Map_MCP2210.md` não continha as Tabelas 3-62 e 3-63, que a
Figura 3-23 referencia como Respostas 4 e 5 do `0x42` — precisamente as duas
que definiriam dois dos três códigos do motor SPI. Elas foram acrescentadas em
2026-09-27, e o §3.5 acima é o resultado imediato disso.

O que a lacuna custou vale registrar, porque é barato e se repete: durante uma
análise inteira, três constantes erradas ficaram classificadas como "não
verificável contra a transcrição" em vez de "erradas". **Uma transcrição
incompleta não é uma fonte parcial, é uma fonte que produz a categoria errada**
— e a categoria errada é mais cara que a ausência, porque parece uma decisão
tomada.

---

## 4. Os comandos: cobertura, omissões deliberadas e constantes mortas

| Código | Comando | Estado | Categoria |
| :--- | :--- | :--- | :--- |
| `0x40` | Set (VM) SPI Transfer Settings | implementado | — |
| `0x42` | Transfer SPI Data | implementado | — |
| `0x20` | Get (VM) Chip Settings | implementado | — |
| `0x21` | Set (VM) Chip Settings | implementado, incompleto | §3.2 |
| `0x30` `0x31` `0x32` `0x33` | GPIO valor e direção, get/set | implementados | — |
| `0x12` | Get interrupt event count | implementado, polaridade trocada | §3.1 |
| `0x11` | Cancel the current SPI transfer | implementado, Fase 4 | §4.1 |
| `0x10` | Get MCP2210 Status | implementado (diagnóstico de `0xF7`), Fase 4 | §4.1 |
| `0x41` | Get (VM) SPI Transfer Settings | implementado (taxa real, uma vez), Fase 4 | §4.1 |
| `0x80` | Request SPI Bus Release | ausente | §4.2 |
| `0x50` `0x51` | Read / Write EEPROM | ausente | §4.3 |
| `0x60` `0x61` | Set / Get NVRAM (5 sub-comandos cada) | ausente | §4.3 |
| `0x70` | Send Access Password | ausente | §4.3 |

### 4.1 As três constantes mortas eram o caminho de recuperação — **implementadas**

Este é o segundo defeito que impede o arranque confiável, e é maior que parece.

Quando o limite de estagnação de `mcp2210_do_transaction()` dispara (`:366` e
`:393`, 100 iterações), a função devolve `-ETIMEDOUT` e deixa a ponte no meio de
uma transação. Ninguém envia `0x11`. Na transferência seguinte,
`mcp2210_set_transfer_settings()` emite um `0x40` — e a Figura 3-13 diz
explicitamente que um `0x40` com transferência em curso devolve a Resposta 2
(`0xF8`, definições **não escritas**), que o driver propaga como `-EBUSY` sem
cancelar e sem repetir.

**Um único timeout trancava o controlador para sempre.** A constante para
desfazer isso já estava no arquivo, na linha 79. Agora `mcp2210_do_transaction()`
cancela em todo caminho de erro, e uma ponte encontrada já ocupada — sessão
anterior que morreu sem cancelar, ou um cancelamento perdido — é recuperada na
hora: o `0x40` recusado é repetido uma vez depois de um cancelamento.

O limite em si também está mal formado: 100 iterações de `usleep_range(100, 200)`
assume que o custo dominante é a espera, quando cada troca HID custa ~1 ms. O
limite deve ser derivado de `len / speed_hz` mais uma margem, não ser um número
redondo. Com o mínimo declarado de 1500 Hz (`:802`) e os 512 bytes de
`MCP2210_MAX_MESSAGE_BYTES`, uma transação legítima leva quase três segundos.

O `0x10` (*Get MCP2210 Status*) é o companheiro disso: devolve o dono atual do
barramento e o estado do pedido de liberação — exatamente o que se quer saber
quando um `0xF7` aparece. O 2221 tem essa consulta e a usa em todo caminho de
erro (`mcp_chk_last_cmd_status_free_bus()`).

O `0x41` é o menos urgente dos três, e ainda assim vale: ler de volta as
configurações depois de escrevê-las é o que transforma "pedimos 2,048 MHz" em
"o integrado confirmou 2,048 MHz", e a ponte pode arredondar.

### 4.2 O `0x80` é ausência legítima — e o `0xF7` foi separado do `0xF8`

*Request SPI Bus Release* só faz sentido num barramento com um segundo host, e
o bit 0 das "outras configurações" controla se a ponte libera o barramento entre
transferências. Não implementar é defensável; o que não é defensável é o
tratamento atual do `0xF7`, que mapeia "barramento pertence a um host externo"
para `-EBUSY` e **repete**, como se fosse a mesma coisa que "motor ocupado"
(`0xF8`). São condições diferentes com remédios diferentes: uma se resolve
esperando, a outra não se resolve nunca sem o `0x80`.

**Feito na Fase 3:** `0xF8` é `-EAGAIN` e `0xF7` é `-EBUSY`, o laço de repetição
só repete o primeiro, e um barramento com dono externo falha de imediato em vez
de custar cem esperas inúteis. A Fase 4 acrescentou o diagnóstico: depois de um
`0xF7` o driver pergunta ao `0x10` quem é o dono e põe a resposta no log, porque
"um host externo tem o barramento" é um fato de integração que o chamador não
extrai de um `-EBUSY`.

### 4.3 As omissões deliberadas, e por que continuam deliberadas

NVRAM, EEPROM e senha estão fora de escopo por decisão registrada no cabeçalho
do arquivo ("no ioctl or configfs ABI, no NVRAM provisioning, no persistent chip
settings"), e essa decisão tem duas justificativas independentes que continuam
valendo:

1. **IEC 62304.** Cada comando é superfície dentro do domínio de confiança do
   kernel, e o motivo de este driver ter sido escrito em vez de adotado foi
   precisamente recusar superfície (`implementation_plan_iio_afe.md` §2.2).
2. **O teste negativo do `afe_bench` §8-B.** "A NVRAM está byte a byte
   inalterada depois de uma sessão inteira" é um critério de aceitação, e a
   forma mais barata de garanti-lo é não ter o código que a escreve. Um driver
   que escrevesse a NVRAM por acidente mudaria o VID/PID de um dispositivo em
   campo, numa peça soldada, de modo que o próprio driver dele deixaria de dar
   match.

O que muda com este plano: a omissão passa a ser **declarada no código**, num
bloco de comentário que lista os códigos não implementados e o motivo. Hoje ela
é inferível pela ausência, e ausência não é documentação. Um revisor de mainline
vai perguntar; é melhor a resposta já estar lá.

---

## 5. O que impede o mainline e não é defeito

### 5.1 Parâmetros de módulo descrevem a topologia da placa

Quatro parâmetros — `spi_device` (`:149`), `spi_chip_select` (`:154`),
`spi_max_speed_hz` (`:158`), `spi_mode_param` (`:166`) — descrevem *qual
dispositivo está atrás da ponte*. Isso não passa em mainline. O 2221 tem um
parâmetro de módulo, mas é `i2c_clk_freq`: um ajuste do próprio integrado, não
uma descrição do que está pendurado nele. Para I²C a questão nem se coloca,
porque o núcleo I²C tem `new_device` em sysfs e sondagem por endereço; o núcleo
SPI não tem equivalente, e é justamente por isso que a pergunta é difícil e que
a resposta precisa estar certa.

O caminho canônico existe e está disponível no kernel que construímos:
`struct spi_board_info` tem o campo `swnode` (verificado em
`include/linux/spi/spi.h` na árvore 6.6), de modo que um `software_node`
estático descreve o filho — modalias, `max-speed`, modo, propriedades — e
`spi_new_device()` o consome. A escolha entre nós de software passa a ser feita
por uma tabela indexada pelo PID, ou por um único parâmetro de topologia, em vez
de quatro parâmetros soltos que qualquer um pode combinar de forma inválida.

**Nota de fronteira.** Isto tem consequência para a regra 9 do `CLAUDE.md`. O
nome `ads1299` como valor padrão de um parâmetro de módulo é aceitável dentro da
camada adjunta; num arquivo de `drivers/hid/` da árvore Linux, um `spi_controller`
genérico com o nome de um conversor de EEG cravado como padrão é exatamente o
tipo de coisa que um revisor pede para remover — e ele tem razão. A versão
mainline não conhece o ADS1299. Quem conhece é o `software_node`, e a ponte
entre os dois é o que a Fase 6 tem de projetar.

### 5.2 Identificadores fora do `hid-ids.h`

`hid-ids.h` já tem `USB_VENDOR_ID_MICROCHIP` (`0x04d8`, `:949`),
`USB_DEVICE_ID_MCP2200` (`0x00df`, `:957`) e `USB_DEVICE_ID_MCP2221` (`0x00dd`,
`:958`). O nosso driver define os seus localmente (`:66-67`). A convenção do
diretório é o header; mover é trivial e o patch fica mais limpo.

Uma observação sobre o `0x00de`, ainda marcado `[DS20005176?]`: ele cai
exatamente entre o `0x00dd` do 2221 e o `0x00df` do 2200, que são valores
verificados na árvore. Isso é corroboração, não verificação — vizinhança
plausível num espaço de PIDs sequencial. Continua sendo uma linha da Fase 0.

### 5.3 O contador de eventos em sysfs é uma ABI nova

`interrupt_count` e `interrupt_count_reset` (`:619`, `:648`) criam dois arquivos
sysfs que nenhum outro driver tem. Isso levanta três exigências em mainline:

1. **Documentação obrigatória.** `Documentation/ABI/testing/sysfs-driver-hid-mcp2210`,
   no formato que os treze `sysfs-driver-hid-*` existentes seguem. Sem isso o
   patch não avança.
2. **Estabilidade.** Uma ABI de sysfs é para sempre. O par
   "lê / repõe separadamente" já se mostrou capaz de ser implementado ao
   contrário (§3.1) e já se sabe que sofre de `wrap` em 16 bits; congelar essa
   forma é uma escolha ruim para congelar.
3. **A objeção provável: isto pertence ao subsistema `counter`.** É uma objeção
   forte. `drivers/counter/` existe, tem `devm_counter_alloc()`/`devm_counter_add()`
   na 6.6, e tem inclusive um precedente quase idêntico em
   `drivers/counter/interrupt-cnt.c`. Contar bordas num pino é literalmente o
   que o subsistema faz, e ele resolve de graça o problema de largura
   (`COUNTER_COMP_COUNT_U64`).

**Recomendação:** expor o contador via `counter`, sob `IS_REACHABLE(CONFIG_COUNTER)`,
no mesmo padrão em que o 2221 expõe o ADC via `IS_REACHABLE(CONFIG_IIO)`. Isso
resolve os três pontos de uma vez, elimina a ABI customizada, e o `wrap` de 16
bits deixa de ser um problema do consumidor. O custo é que o `afe_bench` §5
passa a ler de `/sys/bus/counter/` em vez de um atributo do dispositivo HID —
uma mudança de caminho no roteiro, não de método.

O custo real, e ele deve ser declarado: um terceiro subsistema no mesmo módulo.
O precedente do 2221 (i2c + gpio + iio) cobre isso, mas não gratuitamente.

### 5.4 Metadados

- **URL do datasheet** no comentário de cabeçalho, como `hid-mcp2221.c:7`. O
  nosso cabeçalho é excelente em explicar *por que* o driver foi escrito e não
  diz onde está o protocolo que ele implementa.
- **`MAINTAINERS`.** A entrada já existe e a ordem dos tipos (`M:` `L:` `S:` `F:`)
  está correta — o `checkpatch` verifica isso. O que está errado é a posição da
  seção: ela foi inserida entre `MCP2221A MICROCHIP...` e `MCP251XFD...`, mas o
  título é `MICROCHIP MCP2210 HID USB-TO-SPI DRIVER`, que pertence ao bloco
  `MICROCHIP`, muito abaixo. E falta `L: linux-spi@vger.kernel.org`: o driver
  registra um `spi_controller`, e o 2221 lista `linux-i2c` pelo motivo simétrico.
- **Comentário de escopo** listando os comandos deliberadamente não
  implementados (§4.3).

### 5.5 Um item de rebase, não de correção

`gpio_chip.set` devolve `void` na 6.6 (`include/linux/gpio/driver.h:437`), que é
a assinatura que o driver usa, e ela mudou em kernels posteriores. Verificar
contra a árvore do dia da postagem, não contra esta. O mesmo vale para
`init_valid_mask` (`:451`), que é o mecanismo correto para marcar o GP8 como
não-saída em vez de deixar `mcp2210_gpio_set()` escrever silenciosamente num bit
*don't care* (§3.3).

---

## 6. O contrato do `spi_controller`

Vale separar isto do §3 porque não é um defeito contra o datasheet — é um
defeito contra o núcleo SPI, e a diferença importa para saber contra o que
testar.

Ao fornecer `transfer_one_message`, o driver assume responsabilidade integral
pela mensagem. O que ele deve a cada `spi_transfer` e hoje não entrega:

| Campo | Contrato | Hoje |
| :--- | :--- | :--- |
| `delay` | executar após a transferência | ignorado (§3.6) |
| `cs_change` | desafirmar CS entre transferências | fundido |
| `bits_per_word` | honrar ou recusar | ignorado; `bits_per_word_mask` declara só 8 |
| `speed_hz` | por transferência | o mínimo é aplicado a tudo |
| `msg->actual_length` | bytes efetivamente transferidos | correto |
| `msg->status` + `spi_finalize_current_message()` | uma finalização por mensagem | correto, e o comentário em `:452-458` explica o porquê melhor que a maioria |

Os dois últimos estarem certos é significativo: são os que o núcleo pune com
corrupção de lista, e alguém pensou neles.

Para os quatro primeiros, o princípio é o mesmo do §3.6(b): **honrar ou recusar,
nunca ignorar em silêncio.** `bits_per_word_mask = SPI_BPW_MASK(8)` já faz o
núcleo rejeitar outros tamanhos antes de chegar aqui — esse já está resolvido
pela declaração, e é o modelo para os outros. A velocidade única por transação é
uma limitação real do hardware (o bitrate está nas configurações da transação,
não na transferência) e deve virar um comentário, não um conserto.

---

## 7. Fases

Cada fase tem um critério numérico e uma injeção de falha. Uma fase sem a
injeção executada não está feita — é uma alegação, no sentido exato que o
`CLAUDE.md` dá à palavra.

### Fase 0 — Verificar as constantes — **quase fechada**

Delegada ao `implementation_plan_afe_bench.md` §8-B Fase 0. Ela deixou de
bloquear as Fases 1 a 5 em 2026-09-27, quando as tabelas faltantes entraram na
transcrição (§3.7) e a varredura pôde ser feita contra ela.

Feito: os doze opcodes, os valores do byte de estado, os deslocamentos de
campo, o layout das configurações de transferência, as designações de pino e os
três estados do motor foram lidos contra as tabelas e cada um cita a tabela de
onde veio. Quatro estavam errados e foram corrigidos: o byte de reposição do
`0x12` (§3.1) e os três do motor SPI (§3.5).

**O que falta, e é exatamente um item:** o PID `0x00de`. A documentação de
protocolo não o contém — ela diz que VID e PID são configuráveis e nunca dá os
valores de fábrica. Só o datasheet completo ou uma enumeração real resolvem.

**Critério, revisado:** um marcador `[DS20005176?]` restante, no PID, e cada
outra constante citando a tabela de onde veio. *Zero* marcadores continua sendo
o critério, mas ele agora depende de uma fonte que a transcrição não é.
**Injeção:** nenhuma aplicável — é uma leitura. A contagem de constantes
corrigidas durante a varredura é o resultado, e ela foi quatro.

### Fase 1 — Os dois defeitos que impedem o arranque

Corrigir §3.1 (polaridade do `0x12`) e §3.4 (designação do CS).

**Critério:** duas leituras consecutivas de `interrupt_count` em repouso
devolvem o mesmo valor; um `spi_chip_select` apontado para um pino GPIO produz
uma mensagem nomeando o pino (ou uma correção registrada), não um
"bridge ready".
**Injeção:** reverter cada correção isoladamente e confirmar que o critério
correspondente falha e **só** ele.

### Fase 2 — Integridade do read-modify-write

Corrigir §3.2 (bytes 13–16 do `0x21`).

**Critério:** os bytes 13–16 de um `0x20` são idênticos antes e depois do probe.
**Injeção:** escrever um valor não-nulo na direção padrão dos GPIOs antes de
carregar o módulo e confirmar que ele sobrevive.

### Fase 3 — Correlação de resposta, no padrão do 2221 — **fechada**

Reescrever `mcp2210_raw_event()` (`:251`) para despachar por `data[0]` e
traduzir o byte de estado em `errno` por comando, como `mcp_get_i2c_eng_state()`.
Descartar um relatório cujo eco não corresponde ao comando pendente, em vez de
completá-lo. Distinguir `0xF7` de `0xF8` (§4.2). Tratar `0xF9` explicitamente.

**Critério, cumprido:** uma resposta com eco divergente não completa a espera;
o comando pendente termina em `-ETIMEDOUT` e `stray_replies` incrementa.
**Injeção, executada:** removendo a verificação do eco, a suíte dá 5 falhas,
todas em E1 e E2 — a fase que mais precisava de injeção e a mais difícil de
fazer com hardware, feita sem nenhum.

### Fase 4 — Recuperação — **fechada**

Emitir `0x11` em todo caminho de erro de `mcp2210_do_transaction()`. Usar `0x10`
no diagnóstico quando um `0xF7` aparecer. Derivar o limite de estagnação de
`len / speed_hz` em vez de contar iterações (§4.1). Opcionalmente, ler de volta
com `0x41` (§4.1).

**Critério, cumprido:** depois de um `-ETIMEDOUT` forçado a transferência
seguinte tem sucesso, com os dados certos, e a ponte está no repouso em vez de
segurando a transação.
**Injeções, executadas** — quatro, cada uma falhando só o que lhe diz respeito:
neutralizar o cancelamento dá 9 falhas; desligar a recuperação de uma ponte já
ocupada dá 3 (todas em E5); repor o corte silencioso no lugar do `-EPROTO` dá 4;
e voltar o limite de estagnação ao 100 fixo dá 3, em D4. **O travamento era
real**, e não uma leitura pessimista: a suíte o observou antes de a correção
existir.

### Fase 5 — O contrato SPI

`MCP2210_NCS` = 8 e `init_valid_mask` para o GP8 (§3.3). Traduzir `xfer->delay`
para o quantum de 100 µs e recusar `cs_change` no meio da mensagem (§3.6).

**Critério:** uma mensagem com `cs_change` devolve `-EINVAL`; um `cs` = 8 é
recusado no probe; um `gpiod` de saída no GP8 é recusado pelo gpiolib.
**Injeção:** os três acima são a própria injeção, desde que o caso positivo
correspondente continue passando.

### Fase 6 — A forma mainline (paralelizável)

`hid-ids.h` (§5.2), `software_node` no lugar dos parâmetros de topologia (§5.1),
contador via subsistema `counter` (§5.3), `Documentation/ABI/`, URL do datasheet,
posição da entrada em `MAINTAINERS`, `L: linux-spi`, bloco de escopo (§4.3).

**Critério:** `checkpatch --strict` continua em 0/0/0; `grep -c 'module_param'`
devolve 0 ou 1 (e o 1, se existir, não descreve o que está atrás da ponte);
`grep -ri 'ads1299' drivers/hid/hid-mcp2210.c` devolve 0.
**Injeção:** não se aplica — é forma, e o critério é mecânico.

### Fase 7 — Testes de host, sem hardware

Um harness que exercita a máquina de estados de transferência contra um backend
HID falso. O `implementation_plan_afe_bench.md` §13.1 já especifica isto para a
fragmentação, o limite de estagnação e o `-EMSGSIZE`; este plano acrescenta três
casos que só existem depois da Fase 3 e da Fase 4:

- resposta com eco divergente → descartada, não completada;
- resposta atrasada chegando depois de um `-ETIMEDOUT` → não contamina o
  comando seguinte;
- `0xF8` seguido de sucesso → a transação completa; `0xF7` → erro distinto.

Vale repetir aqui a distinção que o `afe_bench` §13.1 faz e que se aplica
inteira: um teste escrito a partir da mesma leitura do datasheet que escreveu o
código prova consistência, não correção. Estes três casos são **lógica**, não
offsets, e portanto são testáveis hoje com valor real. Os offsets continuam
precisando da Fase 0.

**Critério:** os casos passam; e cada um falha quando a correção correspondente
é revertida.

### Fase 8 — Bancada

Delegada ao `implementation_plan_afe_bench.md` §8-B, que já está estruturada e
que — importante — não depende do conversor: roda com uma placa de avaliação e
um jumper.

### Fase 9 — Submissão

RFC para `linux-input@vger.kernel.org` com cópia para `linux-spi@vger.kernel.org`.
Ordem de patches, cada um compilável isoladamente: (1) `hid-ids.h`;
(2) o driver; (3) `Kconfig`/`Makefile`; (4) `Documentation/ABI/`;
(5) `MAINTAINERS`.

**Critério:** não é "aceito". É "postado, com as Fases 0–8 fechadas e os números
delas na carta de apresentação". O que acontece depois não é um critério que
este projeto controla — a mesma posição que o
`implementation_plan_ads1299_upstream.md` §12 assume.

---

## 8. Integração de build: repetir o que o ADS1299 já fez

O driver hoje existe em duas cópias: `linux-med/drivers/hid/hid-mcp2210.c` e
`meta-custom/meta-med-afe-ads1299/recipes-kernel/mcp2210/files/mcp2210-spi.c`.
Um `diff` mostra que elas divergem em exatamente uma linha — a `MODULE_AUTHOR`.

Duas cópias de um driver que divergem em uma linha hoje divergirão em mais
amanhã, e nada no build reclama. Este é o mesmo problema que o `ti-ads1299` já
resolveu e a solução já está escrita e validada: o driver mora na árvore do
kernel, que é a fonte de verdade, e é habilitado por um símbolo num fragmento
(`meta-med-bsp/dynamic-layers/stm-st-stm32mp/.../med-stm32mp-drivers.cfg`).

As mudanças, por analogia direta com o que já foi feito:

1. `CONFIG_HID_MCP2210=m` em `med-stm32mp-drivers.cfg`, ao lado de
   `CONFIG_TI_ADS1299=m`, com a mesma justificativa `=m` (construir é decisão do
   fragmento; instalar continua por link).
2. `MED_AFE_KCONFIG_usb` em `meta-med-afe-ads1299/conf/layer.conf` passa a
   `"CONFIG_TI_ADS1299 CONFIG_HID_MCP2210"`. A verificação cruzada que o
   `linux-stm32mp_%.bbappend` já faz — um símbolo declarado pela variável e
   ausente do fragmento é uma requisição que silenciosamente nunca acontece —
   passa a cobrir os dois drivers sem alteração nenhuma.
3. `MED_AFE_INSTALL_usb` troca `mcp2210-spi` por `kernel-module-hid-mcp2210`,
   o nome que a `kernel.bbclass` dá a um módulo da árvore.
4. Apagar `recipes-kernel/mcp2210/` inteiro. O `KERNEL_MODULE_AUTOLOAD` que a
   receita carrega, e o raciocínio de oito linhas sobre o `hid_generic_match()`
   que o acompanha, **precisam sobreviver** — mudam de lugar, não deixam de
   existir. O destino natural é o `linux-stm32mp_%.bbappend`, junto do fragmento.
5. `RRECOMMENDS` do bridge para o conversor: hoje está na receita que vai
   sumir. Como os dois viram módulos da mesma árvore, a relação passa a ser
   expressa só pelo `MED_AFE_INSTALL_usb` listar ambos — que é onde ela sempre
   pertenceu.

Uma assimetria a declarar antes de ser descoberta, exatamente como o comentário
em `med-kernel-features.cfg:165-169` já declarou para o conversor: um driver
in-tree só existe na árvore que o tem. A ponte USB, como o conversor, é
construível na máquina cujo kernel controlamos e **não** no `qemux86-64`, onde
o perfil é `simulated`. Isso não é uma regressão — é a mesma propriedade, agora
válida para os dois módulos, e o `MED_AFE_KCONFIG_simulated` vazio é o que a
mantém honesta.

**Vale a pena?** O `ti-ads1299` já pagou esse custo e o argumento é o mesmo,
mais um: este plano termina com o driver postado para mainline. Um driver que
mora em `drivers/hid/` da nossa árvore é um `git format-patch` de distância da
lista; um que mora em `files/` de uma receita é uma conversão manual toda vez.

---

## 9. Critérios de aceitação

Um plano fechado é este conjunto verdadeiro ao mesmo tempo:

1. Zero marcadores `[DS20005176?]` no arquivo. *(Fase 0 — hoje resta **um**, no
   PID, e a transcrição não pode fechá-lo: ver a Fase 0)*
2. `checkpatch --no-tree --file --strict` em 0/0/0. *(Fase 6; já verdadeiro hoje,
   e o ponto é que continue depois de ~200 linhas de mudança)*
3. Compilação arm64 `W=1` sem avisos contra o kernel da placa. *(hoje verdadeiro)*
4. Os casos de host da Fase 7 passam, e cada um falha quando a correção que ele
   cobre é revertida.
5. Uma transferência SPI real devolve os bytes esperados de uma placa de
   avaliação com jumper MOSI→MISO. *(Fase 8)*
6. Depois de um `-ETIMEDOUT` forçado, a transferência seguinte tem sucesso.
   *(Fase 4 — o critério que hoje, por leitura, falharia)*
7. A NVRAM está byte a byte inalterada depois de uma sessão inteira.
   *(`afe_bench` §8-B)*
8. `grep -ri 'ads1299' drivers/hid/hid-mcp2210.c` devolve zero. *(Fase 6 / §5.1)*
9. Uma cópia só do arquivo no repositório. *(Fase 8 de build, §8)*
10. Os patches estão postados. *(Fase 9)*

Note quais destes um build verde satisfaz: o 3, e mais nenhum. É a regra do
`CLAUDE.md` — "'compila e arranca' não é evidência sobre o caminho de
atualização" — aplicada a um driver.

---

## 10. Ordem, e o que bloqueia o quê

```
Fase 0 (constantes + tabelas faltantes)
   │
   ├──> Fase 1 (polaridade 0x12, designação do CS)  ──┐
   ├──> Fase 2 (RMW das chip settings)               ──┤
   ├──> Fase 3 (correlação de resposta)              ──┼──> Fase 7 (testes de host)
   ├──> Fase 4 (recuperação: 0x11, 0x10, limite)     ──┤         │
   └──> Fase 5 (contrato SPI)                        ──┘         │
                                                                 v
Fase 6 (forma mainline) ── paralela, não bloqueia nada ──> Fase 8 (bancada, §8-B)
                                                                 │
                                                                 v
                                                          Fase 9 (submissão)
```

Relação com os outros planos:

- `implementation_plan_afe_bench.md` §8-B **é** a Fase 8 e a Fase 0 deste plano.
  Este documento não duplica nenhuma das duas; acrescenta §3.7 e §5.2 à Fase 0.
- `implementation_plan_ads1299_upstream.md` é o gêmeo deste para o conversor. Os
  dois podem correr em paralelo e têm exatamente uma dependência em comum: a
  Fase 8 de build (§8) tira o último módulo out-of-tree da camada adjunta, o
  que fecha a migração que o conversor começou.
- `implementation_plan_iio_afe.md` §13 é onde o estado atual foi medido; o
  registro de implementação deste plano vai no §13 **deste** arquivo, não lá.

Uma assimetria honesta entre os dois planos de upstream: o ADS1299 é um backport
de código revisado por um mantenedor, o que o mantém fora da categoria SOUP. O
MCP2210 é código novo, escrito aqui, sem oráculo. É o driver com o datasheet
menos verificado e nenhuma referência cruzada — que é exatamente o argumento que
o `afe_bench` §8-B usa para dedicar uma fase inteira a ele, e o mesmo argumento
diz que a revisão de mainline vale mais para este do que para o conversor.

---

## 11. Riscos, e o que este plano não cobre

**O que não cobre, explicitamente:**

- Suspend/resume. O 2221 usa `hid_hw_power(PM_HINT_FULLON)` em torno de cada
  transferência; o nosso não usa nada. Numa placa que suspende, uma aquisição
  em curso e uma ponte USB suspensa é um cenário não analisado. Fora de escopo
  aqui, e merece uma linha no `RESULTS.md` §9.
- Múltiplos dispositivos SPI atrás de uma ponte. O hardware suporta oito chip
  selects; o driver registra um filho. Não é um defeito — é escopo — mas um
  revisor de mainline vai perguntar, e a resposta deve estar pronta.
- Concorrência entre o `gpiochip` e uma transferência em curso. O `mutex`
  serializa as trocas HID, o que basta para a integridade dos relatórios, e
  `mcp2210_gpio_request()` (`:607`) recusa pinos não-GPIO. Não foi analisado o
  que acontece quando um `gpiod_set_value()` chega entre dois chunks de uma
  transação de 512 bytes — e a leitura do laço sugere que ele **não** pode
  chegar, porque o `mutex` é tomado por toda a transação em
  `mcp2210_transfer_one_message()`. Leitura, não medida.

**Riscos:**

| Risco | Por que importa | Mitigação |
| :--- | :--- | :--- |
| A Fase 0 muda mais constantes do que se espera | As Fases 1–5 seriam reescritas | A Fase 0 é primeira e é barata; o custo de a adiar é maior |
| O subsistema `counter` (§5.3) é rejeitado na revisão | Retrabalho na única ABI do driver | Postar como RFC e perguntar antes, na própria carta |
| O `software_node` (§5.1) não convence | É o item de mainline de maior risco | É o item de maior risco *e* o de maior valor; se cair, o driver ainda serve ao TCC — só não sobe |
| A bancada nunca acontece | Fases 0 e 8 ficam abertas para sempre | A Fase 7 não precisa de hardware e cobre a lógica; declarar o resto como não medido, como o `RESULTS.md` §9 já faz |
| A migração in-tree (§8) quebra o build da placa | O perfil `usb` deixa de construir | A verificação cruzada símbolo↔fragmento que o bbappend já faz pega isso no parse |

---

## 12. O que o TCC pode afirmar em cada estado

| Estado | Afirmação defensável | Afirmação que seria falsa |
| :--- | :--- | :--- |
| Hoje | "O driver existe, compila limpo para arm64 e passa o `checkpatch --strict`." | "O modo USB funciona." |
| Fases 0–5 | "O driver implementa o protocolo documentado; seis divergências foram encontradas por inspeção contra o datasheet e corrigidas." | "O driver está correto." |
| + Fase 7 | "A máquina de estados de transferência é verificada por testes que falham quando a correção correspondente é revertida." | "O driver foi validado." |
| + Fase 8 | "O modo USB adquire de silício; a NVRAM permanece inalterada; N amostras em M segundos." | "O modo USB é clinicamente equivalente ao AMP." *(nunca — é o argumento de fase do `CLAUDE.md`)* |
| + Fase 9 | "O driver foi submetido à `linux-input`." | "O driver está em mainline." |

A coluna da direita é o ponto do quadro. Cada linha dela é uma frase que seria
fácil escrever e que este plano existe para tornar impossível.

---

## 13. Registro de implementação

### 2026-09-27 — Fases 1 e 2, e a metade "chip select" do §3.3

Branch `feature/ads1299-mcp2210-driver` em `linux-med`. Quatro commits, um por
mudança lógica, na forma que a Fase 9 vai precisar:

| Commit | Cobre |
| :--- | :--- |
| `181cc5c26a46` `HID: mcp2210: add Microchip USB-to-SPI bridge driver` | o estado anterior, registrado como base |
| `f8eb7c53e896` `...read the interrupt counter without resetting it` | §3.1 |
| `6db1fc893f2f` `...do not zero the GPIO settings when writing chip settings` | §3.2 |
| `363f13f1ebc3` `...make sure the chip select pin really is a chip select` | §3.4 + a metade "contagem de CS" do §3.3 |

**O que foi decidido onde o plano deixava a escolha em aberto.** O §3.4 admitia
recusar o probe ou corrigir a designação; a escolha foi corrigir, em três casos
distintos. Um pino já designado chip select é deixado em paz; um designado GPIO
é reivindicado escrevendo **apenas** as configurações voláteis, de modo que um
ciclo de energia restaura o que a placa foi provisionada com e o teste negativo
do `afe_bench` §8-B continua valendo; um pino com função dedicada é **recusado**
com `-EBUSY`, nomeando o pino, porque esses são os sinais da própria ponte e
reaproveitar um porque um parâmetro pediu é exatamente a reconstrução silenciosa
contra a qual `mcp2210_write_chip_settings()` já avisava.

A metade "contagem de chip selects" do §3.3 entrou junto porque a correção do
§3.4 a exigia: sem `MCP2210_NCS` = 8, a nova reivindicação escreveria a
designação *chip select* no GP8, que não a aceita — a correção teria criado o
defeito que ela existe para evitar. A outra metade do §3.3 (o `init_valid_mask`
que impede o GP8 de ser saída no `gpiochip`) **não** entrou e continua na Fase 5.

**Medido.**

| Verificação | Comando | Resultado |
| :--- | :--- | :--- |
| Compilação arm64 | `make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- W=1 drivers/hid/hid-mcp2210.o` | limpa, sem avisos, nos quatro commits |
| Estilo | `scripts/checkpatch.pl --no-tree --file --strict` | 0 erros, 0 avisos, 0 checks — 957 linhas |
| Símbolos externos novos | `aarch64-linux-gnu-nm -u`, contra `181cc5c26a46` | 2: `_dev_err` e `__asan_load2_noabort` |
| Fidelidade do split | `diff` da árvore final contra a versão testada antes de dividir | idêntica exceto uma requebra de comentário |

Os dois símbolos novos são esperados e ambos exportados pelo kernel:
`_dev_err` vem do `hid_err()` novo (`drivers/base/core.c:5019`, via
`define_dev_printk_level`) e `__asan_load2_noabort` é instrumentação das leituras
`u16` novas, com `CONFIG_KASAN=y` neste `.config`
(`mm/kasan/generic.c:258`). Registrado porque uma primeira tentativa desta
comparação foi feita contra uma árvore limpa com `git stash`, que não guardou
nada, e portanto comparou o binário consigo mesmo — o número acima é o da
segunda tentativa, contra o commit base.

**O que isto NÃO significava, no dia.** Nenhuma das três correções tinha sido
executada. O módulo nunca tinha sido carregado, nenhuma transferência tinha sido
feita, e as injeções de falha das Fases 1 e 2 — duas leituras consecutivas de
`interrupt_count` em repouso; os bytes 13–16 de um `0x20` antes e depois do
probe; um `spi_chip_select` apontado para um pino GPIO — não tinham sido
executadas. Pelo critério do §7 as duas fases estavam escritas, não fechadas.

> **Atualizado no mesmo dia pela Fase 7**: as três injeções foram executadas, no
> host, e passam. Ver a entrada seguinte. O que continua verdadeiro é a segunda
> metade: nenhum módulo foi carregado em silício e nenhuma constante foi
> verificada contra o datasheet.

**Em aberto, e por que cada uma parou onde parou.** A Fase 0 continua bloqueada
pelo datasheet original: o §3.5 (códigos do motor SPI) não é resolvível contra
a transcrição, porque as Tabelas 3-62 e 3-63 não estão nela. *(Deixou de valer
no mesmo dia: as tabelas entraram, o §3.5 foi resolvido e a Fase 0 está quase
fechada. Ver a terceira entrada abaixo.)* As Fases 3 e 4 —
correlação de resposta e recuperação, que incluem o travamento permanente após
um `-ETIMEDOUT` — não foram tocadas e são o maior item restante. A Fase 7
(testes de host) é o que torna as injeções das Fases 3 e 4 executáveis sem
hardware, e é a próxima coisa a fazer se a bancada continuar distante.

### 2026-09-27 — Fase 7, os testes de host

`tests/mcp2210/`, ~1400 linhas. Roda em segundos por `make test` na raiz, ao
lado das verificações do MedFramework. **124 verificações, 0 falhas, 5 defeitos
confirmados** — 130 depois das seis asserções que a entrada seguinte
acrescentou.

**A decisão de arquitetura, e por que não foi KUnit.** O driver é compilado
byte a byte como embarca — sem macro de teste, sem `#ifdef`, sem costura
própria. Os `#include <linux/...>` resolvem para stubs que apontam para um shim
de kernel, e a substituição acontece um nível **abaixo** do driver, em
`hid_hw_output_report()`: o dispositivo falso recebe o relatório de 64 bytes que
sai, monta a resposta e a entrega chamando o `.raw_event` do próprio driver, que
é o caminho que o núcleo HID usa. Consequência — `mcp2210_command()`, a
decodificação do byte de estado e `mcp2210_raw_event()` ficam **sob teste** em
vez de substituídos, que é exatamente onde moram os defeitos da Fase 3. KUnit
foi recusado porque a costura de que a suíte precisa teria de virar um ponteiro
de função ou um `#ifdef` dentro de um arquivo destinado à `linux-input`, e
andaime de teste num patch de mainline é o que um revisor pede para remover.
Isto custa menos ao driver do que o §13.1 do `afe_bench` previa: aquele plano
declarava a refatoração como custo inevitável, e ela não foi necessária.

**O que fechou.** As três injeções que as Fases 1 e 2 deviam e a do corte de
recepção, cada uma revertida no driver isoladamente:

| Injeção | O que a suíte fez |
| :--- | :--- |
| polaridade do byte de reposição do `0x12` | 3 falhas, todas no grupo B |
| os bytes 13–16 das *chip settings* | 2 falhas, ambas em A2 |
| a reivindicação do pino de chip select | 4 falhas, em A3 e A4 |
| o corte em `received + got > len` | `heap-buffer-overflow`, `WRITE of size 60`, `hid-mcp2210.c:414` |

Cada uma falha **só** as verificações que lhe dizem respeito — o que prova as
duas coisas que importam: que a suíte enxerga o defeito, e que as outras
verificações não o enxergam por acidente. **As Fases 1 e 2 estão fechadas pelo
critério do §7.** O AddressSanitizer está ligado por padrão e é o que transforma
a quarta injeção de um `free(): invalid pointer` do glibc, em ponto posterior e
não relacionado, numa linha de arquivo.

**O que a suíte achou e o plano não tinha.** Um quinto defeito, que saiu de
escrever o dispositivo falso e não de ler o driver: **o driver confia no byte 2
da resposta sem nunca compará-lo com o que ainda deve**, então um dispositivo
que infla a contagem de recepção faz a transferência parar cedo e **devolver
`0`** com dados incompletos. Um erro de transporte vira um valor plausível, que
é a forma de defeito contra a qual todo o resto deste repositório é construído.
Não tem seção no plano; a correção natural é junto da Fase 4.

Os outros quatro defeitos confirmados são os que já estavam previstos: `0xF7`
repetido como se fosse `0xF8` (§4.2), o eco divergente aceito (Fase 3), a
resposta atrasada virando a contagem de bordas (Fase 3) e o travamento
permanente após um `-ETIMEDOUT` (§4.1, Fase 4). **O travamento deixou de ser uma
leitura e passou a ser uma observação**: a suíte força o timeout, verifica que a
ponte ficou no meio de uma transação, e mostra a transferência seguinte falhando
com `cmd_count[0x11] == 0` como causa nomeada.

Vale registrar uma verificação que virou evidência por acaso: a transferência
completa corretamente mesmo quando o dispositivo devolve um byte de estado do
motor que **não está em tabela nenhuma**. Isso prova que o laço é conduzido por
contagem de bytes e não pelas constantes então em disputa do §3.5 (resolvidas
na entrada seguinte, e as três estavam erradas) — ou seja, aquele
defeito é hoje cosmético, e agora isso é medido em vez de raciocinado.

**O que isto NÃO diz**, impresso a cada execução inclusive numa verde: nada
sobre deslocamentos e opcodes, porque o dispositivo falso saiu da mesma
transcrição que o driver e os dois concordarem prova consistência e não correção
(`afe_bench` §13.1); nada sobre silício; nada sobre concorrência, tempo real ou
memória do kernel, porque o shim não tem threads, não dorme e não falha ao
alocar; e nada sobre o contrato do núcleo SPI da Fase 5, que continua
descartando `delay` e `cs_change` em silêncio sem que esta suíte cobre.

### 2026-09-27 — As tabelas que faltavam, e o que elas revelaram

`Register_Map_MCP2210.md` recebeu as Tabelas 3-62 e 3-63 (+25 linhas). São as
Respostas 4 e 5 do `0x42`, e com elas os três valores do byte de estado do
motor SPI passam a estar documentados. Consequência imediata: **os três
estavam errados no driver**, numa rotação — `FINISHED` valia o código de "não
concluída" e `STARTED_NO_DATA` valia o de "concluída". Corrigido em
`1dd73f4854b4`.

**O efeito é nulo, e isso é medida e não dedução.** Com as constantes erradas
reinjetadas, a suíte de host fica em 130 verificações, 0 falhas e os mesmos 5
defeitos. O laço é conduzido por contagem de bytes e o único uso do estado é
uma saída antecipada já protegida por `received >= len`. A Fase 7 tinha
antecipado isso por outro caminho — a transferência completa sob um byte de
estado que não está em tabela nenhuma — e agora há as duas evidências.

**A Fase 0 saiu do bloqueio.** Com a transcrição completa, a varredura foi
feita: doze opcodes, valores do byte de estado, deslocamentos de campo, layout
das configurações de transferência, designações de pino e os três estados do
motor, cada um citando agora a tabela de onde veio. Os marcadores
`[DS20005176?]` caíram de 8 para 1. O que resta é o PID `0x00de`, e ele resta
porque a documentação de protocolo **não o contém**: ela diz que VID e PID são
configuráveis e nunca dá os valores de fábrica. Só o datasheet completo ou uma
enumeração real fecham esse.

**A lição, e ela é barata de repetir.** Durante uma análise inteira essas três
constantes ficaram classificadas como "não verificáveis contra a transcrição"
em vez de "erradas". Uma transcrição incompleta não é uma fonte parcial — é uma
fonte que produz a **categoria errada**, e a categoria errada custa mais que a
ausência, porque parece uma decisão tomada em vez de uma pergunta em aberto. O
campo ajudou a esconder: três valores de um byte, documentados em três tabelas
diferentes, sem nenhum lugar onde apareçam lado a lado.

Uma nota de processo, porque custou trabalho: as correções foram perdidas uma
vez por um `git checkout --` usado para desfazer uma injeção de falha, num
arquivo cujas correções ainda não estavam commitadas. Injetar falha em código
não commitado apaga o código. Commitar antes de injetar.

### 2026-09-27 — Fases 3 e 4, e os cinco defeitos fechados

`3b7d91556580` (Fase 3) e `fdadc13ce0d9` (Fase 4) em `linux-med`. A suíte de
host passou de **136 verificações / 5 defeitos** para **150 / 0**.

| Defeito | Fase | O que passou a acontecer |
| :--- | :--- | :--- |
| eco divergente aceito | 3 | descartado e contado; o comando pendente dá `-ETIMEDOUT` |
| resposta atrasada vira contagem de bordas | 3 | não cruza mais entre comandos; vira uma sequência de timeouts |
| `0xF7` repetido como se fosse `0xF8` | 3 | `-EBUSY` imediato contra `-EAGAIN` repetível; zero esperas |
| contagem de recepção inflada corrompe dados com status 0 | 4 | `-EPROTO`, e a ponte volta ao repouso |
| travamento permanente após `-ETIMEDOUT` | 4 | cancelamento em todo caminho de erro, e recuperação se mesmo assim ficar ocupada |

**O limite do que a Fase 3 pode prometer, escrito no código e não descoberto
depois.** O eco carrega um código de comando, não um número de sequência, então
uma resposta atrasada a um comando que é reemitido ainda casa. O que a
verificação garante é que uma resposta velha nunca cruza de um *tipo* de
comando para outro, e que um dispositivo atrasado produz uma corrida de
timeouts — alto — no lugar de valores plausíveis errados — silencioso.

**As três constantes mortas ganharam um uso cada, todas na direção da falha.**
O `0x11` cancela em todo caminho de erro. O `0x10` roda depois de um `0xF7` para
pôr no log quem é o dono do barramento. O `0x41` lê a taxa de volta **uma vez na
vida do dispositivo** — a ponte divide um relógio fixo, então a taxa que ela
programa não precisa ser a pedida, e um driver que nunca olha não sabe dizer em
que relógio uma gravação foi feita.

**O limite de estagnação virou orçamento.** Cem sondagens fixas não têm relação
com quantos bytes estão sendo movidos: no mínimo do próprio controlador, 1500
Hz, uma transação de 512 bytes clocka por quase três segundos, e o limite antigo
teria dado `-ETIMEDOUT` numa transferência que era apenas lenta. A parte fixa
continua, agora nomeada pelo que sempre foi — a margem de latência USB.

**Um defeito de método, achado pela própria suíte.** A injeção do E4 mirava por
*índice de troca* (`silent_start = exchanges + 2`). O read-back de taxa da Fase 4
inseriu uma troca entre o `0x40` e o primeiro `0x42`, e a injeção passou a
derrubar a resposta errada — **o teste continuou passando enquanto testava outra
coisa**. As injeções agora miram por código de comando. É a mesma família do
`acq-active` que passava com um serviço em *crash loop*: uma asserção pode parar
de ver a falha que procura sem que nada fique vermelho.

**O que continua fora.** Nada disto tocou silício. As Fases 5 (contrato SPI:
`delay`, `cs_change`, `init_valid_mask` do GP8), 6 (forma mainline) e 8 (bancada)
seguem abertas, e o §8 de integração de build também — o driver ainda existe em
duas cópias.

