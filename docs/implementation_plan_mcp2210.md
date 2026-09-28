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

## 4. Os comandos: cobertura, o que ainda falta, e constantes mortas

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
| `0x80` | Request SPI Bus Release | **pendente** | §4.2 |
| `0x50` `0x51` | Read / Write EEPROM | **pendente**, Fases 6B e 6C | §4.3 |
| `0x61` | Get NVRAM (sub-comando `0x20`) | implementado, Fase 6A.1 | §4.3 |
| `0x61` | Get NVRAM (sub-comandos `0x10` `0x30` `0x40` `0x50`) | **pendente** | §4.3 |
| `0x60` | Set NVRAM (5 sub-comandos) | **pendente**, Fase 6A.2 | §4.3 |
| `0x70` | Send Access Password | **pendente**, Fase 6A.2 | §4.3 |

As quatro últimas **eram** escopo declarado e deixaram de ser: o objetivo agora
é cobertura total, faseada. O §4.3 registra a inversão e o que ela custa aos
dois argumentos que se apoiavam nela.

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

### 4.3 O que ainda não foi escrito — e **não** está fora de escopo

**Esta seção foi revista em 2026-09-27 e a decisão inverteu.** Ela dizia que
NVRAM, EEPROM e senha estavam deliberadamente fora de escopo. O objetivo do
driver agora é **cobertura funcional completa do MCP2210**: toda função
alcançável pelo protocolo HID — SPI, GPIO, contador de interrupções, chip
settings voláteis, NVRAM, EEPROM e controle de acesso por senha. A
implementação é faseada; nenhuma dessas funções é permanentemente excluída.

"Ainda não implementado" e "fora de escopo" são palavras diferentes, e o
cabeçalho do driver passou a usá-las como tais.

**As duas justificativas antigas não morreram — foram reescritas com precisão,
e uma delas ficou mais forte.**

1. **IEC 62304 e superfície.** O argumento nunca foi contra a *funcionalidade*;
   foi contra **aquela ABI**. O que se recusou no
   `daniel-santos/mcp2210-linux` foi um ioctl/configfs, um formato binário que
   o próprio projeto declara instável, e uma lista de 28 anomalias abertas
   (`implementation_plan_iio_afe.md` §2.2). Um comando de protocolo não é
   superfície; uma interface de usuário sem limites é. A consequência prática,
   registrada no cabeçalho do driver: **um comando sem interface de subsistema
   decidida fica na camada de transporte** até que ela exista, e nenhuma das
   funções pendentes pode chegar como um monte de atributos sysfs *ad hoc* ou
   um `ioctl` por comando.

2. **O teste negativo do `afe_bench` §8-B, reformulado.** Ele era "a NVRAM está
   byte a byte inalterada", garantido pela forma mais barata possível: não ter
   o código que a escreve. Essa garantia acaba. A substituta é melhor: **"o
   caminho de aquisição nunca escreve NVRAM"** — uma propriedade do runtime e
   não da ausência de código, verificável, e que a suíte de host já checa ao
   fim de cada grupo (`check_no_nvram_or_eeprom()`). O risco que o teste existe
   para cobrir não mudou: um driver que escrevesse a NVRAM por acidente
   mudaria o VID/PID de um aparelho em campo, numa peça soldada, e o próprio
   driver dele deixaria de dar match.

### 4.4 A arquitetura que a cobertura total exige

Quatro camadas, e a de transporte já existe — é o que as Fases 3 e 4
construíram. Todos os comandos compartilham a mesma máquina comando/resposta
(§3.0 do documento de protocolo), então nenhum subsistema precisa de
infraestrutura própria. Esse é o principal dividendo das Fases 3 e 4.

```text
hid-mcp2210.c
    │
    ├── transporte HID / comando-resposta        [Fases 3 e 4, feito]
    │
    ├── configuração
    │     ├── chip settings em RAM               [feito]
    │     ├── chip settings em NVRAM             [Fase 6A]
    │     └── senha / controle de acesso         [Fase 6A]
    │
    ├── periféricos
    │     ├── SPI                                [feito]
    │     ├── GPIO                               [feito]
    │     └── contador de interrupções           [feito]
    │
    └── EEPROM, 256 bytes                        [6B protocolo, 6C interface]
```

**A NVRAM não é "mais um read/write", e a API interna tem que dizer isso.** O
documento distingue quatro coisas (§2.0 e §2.1): os ajustes gravados na NVRAM;
a cópia deles carregada em RAM no *power-up*; o fato de que a RAM pode ser
alterada mesmo com a NVRAM protegida; e a escrita na NVRAM condicionada a senha
ou trava permanente. Então a API interna separa semanticamente:

```text
get_ram_settings()   /  set_ram_settings()      já existem
get_nvram_settings() /  set_nvram_settings()    Fase 6A
send_password()                                 Fase 6A
```

Sem essa separação, uma futura interface sysfs ou debugfs embaralha
"configuração em uso" com "configuração persistente" — que são a mesma
estrutura de bytes e coisas diferentes.

**E a EEPROM não vira uma coleção arbitrária de atributos.** São 256 bytes de
memória não volátil acessíveis só por comandos USB (§1.7). O protocolo pode ser
implementado antes da interface; a interface é uma decisão separada, e é por
isso que a Fase 6C existe como fase própria e não como um detalhe da 6B. O
mesmo cuidado vale para VID/PID e descritores de string, que a §1.4.1 diz serem
configuráveis: "cobertura total" tem de significar uma interface Linux
defensável para cada função, e não um `ioctl` por comando HID.

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

**O mecanismo não é o problema, e este plano errou ao sugerir que fosse.** O
kerneldoc de `spi_new_device()` está na nossa própria 6.6
(`drivers/spi/spi.c:727-731`) e diz textualmente que a função é exportada
"so that for example a USB or parport based adapter driver could add devices
(which it would learn about out-of-band)". Há dois precedentes nessa mesma
árvore — `spi-butterfly.c:266` e `spi-lm70llp.c:267`, adaptadores parport
hot-plug que registram um filho fixo com `spi_board_info` — e a mainline atual
acrescentou um terceiro que é USB: `drivers/spi/spi-ch341.c`. A evidência
estava na árvore o tempo todo e não foi lida.

O que resta é mais estreito e mais interessante: **quem fornece a topologia.**
`struct spi_board_info` tem o campo `swnode` (verificado em
`include/linux/spi/spi.h` na 6.6), então um `software_node` estático descreve o
filho — modalias, `max-speed`, modo, propriedades. O que ele não responde é
qual nó escolher, porque um dispositivo USB não tem nó de firmware: sem DT, sem
companion ACPI, e o núcleo SPI não tem o `new_device` em sysfs que o I²C tem.

A pergunta de RFC, portanto, não é "pode um adaptador USB criar um filho SPI?"
— já sabemos que pode. É: **como descrever, em mainline, um periférico SPI fixo
atrás de um controlador USB hot-plug que não tem DT nem ACPI e cuja topologia é
conhecida fora do firmware?** Isso é modelagem de topologia, não legitimidade
de mecanismo, e é uma pergunta de revisão muito melhor.

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

### 5.3 O contador de eventos em sysfs era uma ABI nova — **resolvido**

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
   que o subsistema faz.

   **Uma ressalva, para não prometer ao mantenedor o que a ponte não dá:** o
   Counter **não resolve o wrap de 16 bits no hardware**. Ele oferece a
   representação certa em userspace, e `COUNTER_COMP_COUNT_U64` permite que o
   driver acumule — mas a leitura do MCP2210 continua sendo de 16 bits e a
   semântica de ler-e-repor continua sendo do dispositivo. Acumular exige ler
   com frequência suficiente para não perder uma volta, e a que frequência isso
   é depende da ODR. A regra do `afe_bench` §5 — somar janelas curtas, nunca
   uma subtração — continua valendo.

**Feito.** O contador é um `counter_device` sob `IS_REACHABLE(CONFIG_COUNTER)`,
no mesmo padrão em que o 2221 expõe o ADC via `IS_REACHABLE(CONFIG_IIO)`: um
Count, um Signal chamado `GP6`, um Synapse cuja ação é a borda de descida que as
*chip settings* programam. Os dois atributos sysfs saíram, e com eles a
exigência de `Documentation/ABI/`.

**E a ressalva ficou escrita no código, não só aqui.** O `u64` é um acumulador
do driver; a ponte continua contando em 16 bits e continua limpando na leitura,
então bordas além de 65536 **entre duas leituras** desaparecem antes de o código
vê-las. O que mudou não é o limite — é de quem é o problema: ler com frequência
suficiente continua sendo do chamador, mas notar o *wrap* não é mais.

Um efeito colateral que não estava previsto e é o melhor da mudança: o
acumulador é o que torna seguro fazer *read-and-reset* a cada leitura, coisa que
o par sysfs não podia. Dois leitores limpam o hardware cada um, os dois recebem
o mesmo total monotônico, e nenhum consome a contagem do outro.

O `afe_bench` §5 passa a ler de `/sys/bus/counter/` — mudança de caminho no
roteiro, não de método.

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
- **Bloco de cobertura pretendida** (§4.3): o que está implementado, o que
  está pendente, e a regra de que um comando sem interface de subsistema
  decidida fica na camada de transporte. Não escrever "todos os comandos
  implementados" antes de existir cobertura de todas as famílias.

### 5.5 Um item de rebase, não de correção

`gpio_chip.set` devolve `void` na 6.6 (`include/linux/gpio/driver.h:437`), que é
a assinatura que o driver usa, e ela mudou em kernels posteriores. Verificar
contra a árvore do dia da postagem, não contra esta. **E uma correção a este plano:** `init_valid_mask` (`:451`) **não** é o
mecanismo para marcar o GP8 como não-saída. O `valid_mask` diz se uma linha
existe para ser usada — e o GP8 existe, como entrada. "Só entrada" é uma
propriedade de direção, e o gpiolib da 6.6 não tem facilidade para ela, então a
restrição vai onde a direção é escolhida: `direction_output()` recusa,
`get_direction()` responde sempre `IN`, e `set()` não gasta uma troca.
Implementado assim na Fase 5.

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

### Fase 0 — Verificar as constantes — **fechada**

Delegada ao `implementation_plan_afe_bench.md` §8-B Fase 0. Ela deixou de
bloquear as Fases 1 a 5 em 2026-09-27, quando as tabelas faltantes entraram na
transcrição (§3.7) e a varredura pôde ser feita contra ela.

Feito: os doze opcodes, os valores do byte de estado, os deslocamentos de
campo, o layout das configurações de transferência, as designações de pino e os
três estados do motor foram lidos contra as tabelas e cada um cita a tabela de
onde veio. Quatro estavam errados e foram corrigidos: o byte de reposição do
`0x12` (§3.1) e os três do motor SPI (§3.5).

**O PID `0x00de`, que a documentação de protocolo não contém** — ela diz que
VID e PID são configuráveis e nunca dá os valores de fábrica — está
**corroborado por três fontes externas a este projeto**: o `mcp2210.c` do
bfgminer, os defaults da biblioteca `mcp2210-python`, e uma captura publicada
de `lsusb -v` que lê `ID 04d8:00de Microchip Technology, Inc. MCP2210 USB to
SPI Master`. A última é uma observação de uma peça, feita por outra pessoa.

**Corroborado não é verificado**, e a distinção fica no código. Mas é também a
constante mais segura de errar em todo o arquivo: um ID errado não produz dado
ruim, produz ausência de *bind*, e o `hid-generic` leva o dispositivo no
primeiro segundo da primeira sessão de bancada. Não bloqueia nada.

**Critério:** zero marcadores `[DS20005176?]` no arquivo. **Cumprido.**
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

### Fase 5 — O contrato SPI — **fechada**

`MCP2210_NCS` = 8 e `init_valid_mask` para o GP8 (§3.3). Traduzir `xfer->delay`
para o quantum de 100 µs e recusar `cs_change` no meio da mensagem (§3.6).

**Critério, cumprido:** um `cs_change` ou um `delay` no meio da mensagem
devolvem `-EINVAL` sem gastar uma troca; um `delay` de 250 µs na última
transferência vira 3 quanta e um de 2 µs vira 1, sempre para cima; um `cs` = 8
é recusado no probe; `direction_output(8)` devolve `-EIO` e `get_direction(8)`
responde `IN`, enquanto o GP3 continua podendo ser saída.
**Injeções, executadas** — quatro, cada uma localizada: desligar a recusa de
`cs_change` dá 2 falhas (F5), a de `delay` intermediário dá 2 (F4), trocar o
arredondamento para baixo dá 2 (F1 e F2), e desligar a regra do GP8 dá 3 (F7).

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

### Fase 6A.1 — NVRAM, o lado de leitura — **fechada**

`0x61` sub-comando `0x20`, com a separação semântica do §4.4:
`mcp2210_read_nvram_chip_settings()` é uma função distinta das de RAM e o nome
diz qual cópia ela lê.

**E com um consumidor real, que é o que a manteve fora da categoria "constante
morta".** No probe, duas coisas vão para o log e nenhuma tinha outro jeito de
ser sabida: se os ajustes de arranque ainda podem ser alterados (descobrir que
uma peça está travada de fábrica na hora de reconfigurá-la é descobrir tarde
demais), e se o chip select em uso é chip select **também** na NVRAM — porque
`mcp2210_claim_chip_select()` corrige só a cópia em RAM, então um desacordo
significa que a correção acontece de novo a cada boot e o aparelho depende
deste driver para funcionar.

O transporte também cresceu o que a família exige: a correlação passou a
incluir o **sub-comando**, que volta no byte 2 e não no byte 1 em que foi
enviado — um código de comando cobre cinco operações, e casar só pelo byte 0
deixaria a resposta de uma satisfazer a espera de outra. E o vocabulário de
estado foi completado (`0xFA`, `0xFB`, `0xFC`, `0xFD`), porque esta família é
o que torna o resto dele alcançável.

**Critério, cumprido.** **Injeções, executadas** — três, cada uma localizada:
desligar a correlação por sub-comando dá 1 falha (G5), o aviso de desacordo
RAM/NVRAM dá 2 (G2), e o relato do controle de acesso dá 1 (G4).

### Fase 6A.2 — NVRAM, escrita e senha — **bloqueada na interface**

`0x60` e `0x70`. Deliberadamente **não** implementados junto com a leitura, e o
motivo é de método: eles não têm chamador enquanto não houver uma interface
Linux por onde um operador os alcance, e **um caminho de escrita sem chamador é
uma constante morta com passos a mais** — o padrão que já custou caro aqui
(§4.1, as três constantes que eram o caminho de recuperação).

Nem poderia haver um consumidor interno legítimo: o candidato óbvio seria
gravar na NVRAM a designação de chip select que hoje é corrigida a cada boot, e
isso é exatamente a mutação de um aparelho em campo que não pode ser
automática.

**A decisão que destrava:** qual interface Linux para configuração persistente
de dispositivo. Nenhuma resposta óbvia serve sozinha — sysfs vira um atributo
por campo, debugfs não é ABI, e um `ioctl` por comando é o que o §4.3 recusa. É
**a mesma pergunta que a Fase 6C faz sobre a EEPROM**, e deve ser respondida
uma vez só para as duas.

**Critério, quando existir:** a escrita é exercitada contra o dispositivo
falso, e o caminho de aquisição continua sem alcançá-la.
**Injeção obrigatória:** provocar a escrita a partir do caminho de aquisição e
confirmar que `check_nvram_not_written()` a vê. O teste negativo reformulado
do §4.3 só vale depois de ter visto a falha que procura.

### Fase 6B — EEPROM, o protocolo

Os comandos `0x50` e `0x51`, os 256 bytes, byte a byte. Só o protocolo: ler,
escrever, e os casos de borda de endereço. Sem interface de usuário.

**Critério:** um ciclo escrita-leitura sobre o dispositivo falso devolve o que
foi escrito, e um endereço fora de faixa é recusado.
**Injeção:** a de sempre — reverter cada limite e confirmar que a suíte o vê.

### Fase 6C — EEPROM, a interface Linux

Fase própria, e não um detalhe da 6B, porque é onde está a decisão. Uma memória
não volátil de 256 bytes tem abstrações prontas no Linux; escolher uma é um
trabalho de revisão, não de implementação. A regra do §4.3 vale aqui mais que
em qualquer outro lugar: **não expor a EEPROM como uma coleção arbitrária de
atributos sysfs.**

**Critério:** a interface está escolhida, justificada por escrito contra as
alternativas, e implementada.

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

1. Zero marcadores `[DS20005176?]` no arquivo. *(Fase 0 — **cumprido**; o PID
   é corroboração de terceiros e não verificação, e a Fase 0 diz por quê)*
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
Fase 0 (constantes + tabelas faltantes)          FECHADA
   │
   ├──> Fase 1 (polaridade 0x12, designação CS)  FECHADA  ──┐
   ├──> Fase 2 (RMW das chip settings)           FECHADA  ──┤
   ├──> Fase 3 (correlação de resposta)          FECHADA  ──┼─> Fase 7  FECHADA
   ├──> Fase 4 (recuperação: 0x11, 0x10, limite) FECHADA  ──┤   (testes de host)
   └──> Fase 5 (contrato SPI)                    FECHADA  ──┘         │
                                                                      │
Fase 6 (forma mainline) ── mecânicos feitos; contador e topologia abertos
   │
   ├──> Fase 6A.1 (NVRAM leitura)   FECHADA
   ├──> Fase 6A.2 (NVRAM escrita + senha) ─┐ mesma decisão de interface
   │                                       │
   ├──> Fase 6B (EEPROM, protocolo)
   └──> Fase 6C (EEPROM, interface Linux) ─┘
                     │
                     v
              Fase 8 (bancada, §8-B)  ──>  Fase 9 (submissão)
```

**Por que 6A-6C entram antes da submissão.** Submeter um driver que
deliberadamente não implementa EEPROM, NVRAM e senha contradiz o objetivo
declarado no §4.3. Submeter um que ainda não as implementou é outra coisa, e é
defensável — mas então a carta de apresentação tem de dizer isso, e a série
precisa de um caminho crível até lá. A ordem acima é esse caminho.

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

### 2026-09-27 — Fase 0 fechada, Fase 5 implementada, e três correções a este plano

`e4da301c0e3f` (PID) e `4b8185e4f988` (Fase 5) em `linux-med`. A suíte de host
foi de 150 para **176 verificações, 0 falhas, 0 defeitos**.

**Fase 0 fechada.** Zero marcadores `[DS20005176?]`. O PID entrou como
corroboração de três fontes externas, e a redação no código mantém a hierarquia
que este repositório usa: corroborado por terceiros fica entre "conferido contra
uma transcrição" e "conferido contra a peça". A bancada continua sendo o
encerramento mais forte; deixou de ser um bloqueio.

**Fase 5 implementada** — atrasos, `cs_change` e GP8, todos sob a regra "honrar
ou recusar, nunca ignorar em silêncio". A parte que exigiu decisão nova: num
`spi_message` achatado em uma transação, **só o `delay` da última transferência
tem para onde ir**. Um intermediário não é acumulado nem movido para o fim — um
atraso que acontece em outro lugar não é o atraso que foi pedido — e vira
`-EINVAL`. O arredondamento é para cima, porque dar a um dispositivo menos tempo
do que ele pediu é exatamente a falha que o atraso existia para evitar.

**Três correções a este plano, todas vindas de evidência que já existia.**

1. **O `init_valid_mask` era a ferramenta errada** para "GP8 só entrada".
   `valid_mask` diz se uma linha existe para ser usada, e o GP8 existe — como
   entrada. Direção não é validade. Corrigido no §5.5 e implementado onde a
   direção é escolhida.

2. **"Não existe outra ponte USB-SPI na árvore" era falso**, e de duas formas. A
   mainline atual tem `drivers/spi/spi-ch341.c`. Mas pior: o kerneldoc de
   `spi_new_device()` **na nossa própria 6.6** (`drivers/spi/spi.c:727-731`) já
   dizia que a função é exportada para que "a USB or parport based adapter
   driver could add devices (which it would learn about out-of-band)", e
   `spi-butterfly.c` e `spi-lm70llp.c` são dois precedentes nessa mesma árvore.
   A evidência estava local e não foi lida — que é o mesmo erro de método que o
   `implementation_plan_iio_afe.md` já registra sobre ter concluído "escrever do
   zero" a partir de uma busca limitada a este repositório.

   A consequência é boa: a pergunta de RFC encolheu de "pode um adaptador USB
   criar um filho SPI?" para "como descrever um periférico SPI fixo atrás de um
   controlador USB hot-plug sem DT nem ACPI?". Modelagem de topologia, não
   legitimidade de mecanismo.

3. **O Counter não resolve o wrap de 16 bits**, como o §5.3 chegou a afirmar. Ele
   dá a representação certa em userspace e permite acumular; a leitura da ponte
   continua de 16 bits e a semântica de ler-e-repor continua do dispositivo.
   Acumular exige ler rápido o bastante para não perder uma volta, e a regra do
   `afe_bench` §5 — somar janelas curtas — continua valendo.

**E uma nota sobre o cabeçalho do driver.** Depois da varredura ele ficou com
dois parágrafos vizinhos dizendo o oposto: "PROTOCOL CONSTANTS ARE UNVERIFIED" e
"Paid". Fundidos num só. Documentação que se contradiz é pior que documentação
ausente, porque as duas metades parecem deliberadas.

### 2026-09-27 — O escopo inverteu: cobertura total, faseada

Decisão do orientador, e ela muda a redação de três documentos e a ordem do
roadmap. O driver deixa de ter EEPROM, NVRAM e senha como **omissões
deliberadas** e passa a ter **cobertura funcional completa do MCP2210** como
objetivo, implementada em fases. "Ainda não implementado" e "fora de escopo"
são palavras diferentes e os documentos passam a usá-las como tais.

Commits: `c60cb08f79d7` (bloco de cobertura, `hid-ids.h`, URL do datasheet) e
`82f79b477cb8` (`MAINTAINERS`) em `linux-med` — os quatro mecânicos da Fase 6, com o
bloco de escopo já reescrito antes de entrar, para que o histórico não registre
uma decisão que durou um commit.

**A mudança colidia com dois argumentos que este repositório carregava, e os
dois sobrevivem — um deles mais forte do que era.**

O primeiro é a razão de este driver ter sido escrito em vez de adotado. Ela
estava redigida de um jeito que se lia como argumento de escopo: o
`daniel-santos/mcp2210-linux` carrega "an ioctl/configfs ABI [...] none of
which this path needs". O que foi recusado ali é **aquela ABI**, aquele formato
binário auto-declarado instável e aquela lista de 28 anomalias — não a
funcionalidade do integrado. Um comando de protocolo não é superfície; uma
interface de usuário sem limites é. Reescrito no cabeçalho, com a consequência
operacional junto: **um comando sem interface de subsistema decidida fica na
camada de transporte.**

O segundo é o teste negativo mais importante do `afe_bench` §8-B, que se
apoiava em o driver **não ter** a função de escrever NVRAM. Essa garantia
acaba, e a substituta é melhor: "o caminho de aquisição nunca escreve NVRAM" é
uma propriedade do runtime, verificável, e a suíte de host já a checa ao fim de
cada grupo. Medir a ausência de uma função sempre foi mais fraco do que medir
que ela não é alcançada.

**O dividendo inesperado das Fases 3 e 4.** Todos os comandos do MCP2210
compartilham a mesma máquina comando/resposta, então a camada de transporte que
aquelas duas fases construíram — correlação por eco, tradução de estado em
`errno`, recuperação — serve NVRAM, EEPROM e senha sem uma linha nova de
infraestrutura. O §4.4 desenha as quatro camadas.

**O que a cobertura total NÃO pode virar**, e está escrito nos dois lugares:
um `ioctl` por comando HID, ou a EEPROM como coleção arbitrária de atributos
sysfs. Por isso a Fase 6C existe separada da 6B — o protocolo pode ser
implementado antes da interface, mas a interface é uma decisão de revisão e não
de implementação. O mesmo vale para VID/PID e descritores de string, que a
§1.4.1 do documento diz serem configuráveis.

**E uma consequência para a Fase 9.** Submeter um driver que *deliberadamente*
não implementa essas funções contradiz o objetivo agora declarado. Submeter um
que ainda não as implementou é defensável — mas a carta de apresentação tem de
dizer isso, e a série precisa de um caminho crível até lá. As Fases 6A, 6B e 6C
são esse caminho, e entram antes da bancada no roadmap.

### 2026-09-27 — Fase 6A.1: a NVRAM se lê; escrever espera a interface

`013e9b1ab0f2` em `linux-med`. Suíte de host de 176 para **195 verificações, 0
falhas**.

**A 6A saiu dividida, e a divisão é o conteúdo.** O lado de leitura entrou; o
de escrita (`0x60`) e a senha (`0x70`) não, e o motivo não é falta de tempo:
eles não têm chamador enquanto não houver interface Linux, e um caminho de
escrita sem chamador é **uma constante morta com passos a mais**. Este driver
já pagou por esse padrão uma vez — o §4.1 registra as três constantes que eram
o caminho de recuperação e ficaram anos definidas sem serem emitidas. Repetir
com um comando que grava memória não volátil de um aparelho em campo seria
pior.

Verifiquei se havia consumidor interno legítimo antes de decidir: o candidato
óbvio é gravar na NVRAM a designação de chip select que hoje é corrigida a cada
boot, e isso é exatamente a mutação automática de um dispositivo em campo que
não pode acontecer. Não há.

**O que manteve a leitura fora da mesma armadilha** foi encontrar um consumidor
de verdade. No probe, duas coisas vão para o log e nenhuma tinha outro jeito de
ser sabida: se os ajustes de arranque ainda podem ser alterados, e se o chip
select em uso é chip select **também** na NVRAM. A segunda é a mais útil —
`mcp2210_claim_chip_select()` corrige só a cópia em RAM, de propósito, então um
desacordo significa que o aparelho depende deste driver a cada boot para ser
usável. Isso é um fato sobre a placa, não sobre o driver, e vale estar no log
de uma máquina que guarda registros.

**Dois crescimentos no transporte, e os dois eram dívida silenciosa.** A
correlação por eco da Fase 3 casava só o byte 0 — mas `0x60`/`0x61` cobrem
cinco operações cada, e o sub-comando volta **no byte 2, não no byte 1 em que
foi enviado**, então a resposta de uma sub-operação satisfaria a espera de
outra. E o vocabulário de estado estava pela metade: `0xFA`, `0xFB`, `0xFC` e
`0xFD` só ficaram alcançáveis com esta família. Os três últimos significam
"não permitido" e **não significam a mesma coisa** — senha errada com
tentativas restantes é recuperável, mecanismo bloqueado precisa de ciclo de
energia, e rejeição é uma peça travada de fábrica. O `errno` não carrega a
diferença; o log carrega.

**O teste negativo mudou de forma no código, não só no texto.**
`check_no_nvram_or_eeprom()` virou `check_nvram_not_written()`: ler a NVRAM
agora é esperado, escrever é o que nunca pode acontecer. É a reformulação do
§4.3 encarnada — de "o driver não sabe" para "o caminho não alcança".

Três injeções, cada uma localizada: correlação por sub-comando (1 falha, G5),
aviso de desacordo RAM/NVRAM (2, G2), relato do controle de acesso (1, G4).

**O que 6A.2 e 6C compartilham** e por isso devem ser decididas juntas: as
duas perguntam qual interface Linux serve para configuração persistente de um
dispositivo. Nenhuma resposta óbvia serve sozinha, e responder duas vezes
produziria duas ABIs para o mesmo tipo de coisa.

### 2026-09-27 — §8: uma origem, provada num build cruzado

`db8861a` (Makefile), `7bc1f22` (single-source) e o conserto de versionamento
que veio depois. O driver da ponte existia em duas cópias que já divergiam numa
linha; agora existe uma, na árvore do kernel, e a camada adjunta não carrega
código de kernel nenhum — só a regra udev, o overlay e o mapeamento por link,
que é a razão de ela existir.

**Medido, num build cruzado de verdade** (`bitbake virtual/kernel`,
`MACHINE=stm32mp25-disco`, `MED_EEG_LINK=usb`, kernel vindo do fork):

| Verificação | Resultado |
| :--- | :--- |
| `hid-mcp2210.ko` e `ti-ads1299.ko` | compilados para aarch64 |
| `CONFIG_HID_MCP2210=m`, `CONFIG_TI_ADS1299=m` | presentes no `.config` **final** |
| `do_med_check_kernel_config` | `Succeeded` |
| Versão do kernel | `6.6.129-g013e9b1ab0f2` |
| Cópias do driver no repositório | uma |

O `do_med_check_kernel_config` passando é o que sustenta a afirmação do commit
de que apagar o `do_check_kernel_config` da receita não perdeu capacidade: o
guard que sobrou lê o `.config` real e teria falhado se o `merge_config` tivesse
derrubado um dos símbolos por dependência não atendida.

### 2026-09-27 — Dois defeitos de build que a §8 desenterrou

Nenhum dos dois é da §8. Os dois estavam no caminho dela.

**1. O caminho containerizado descartava `MED_KERNEL_GIT`.** `kas-base.yml` lista
a variável sob `env:`, o que faz o kas entregá-la ao bitbake — mas só depois de
estar *dentro* do container, e `kas-container:731` repassa uma whitelist fixa que
não a inclui. A invocação que o próprio cabeçalho do bbappend documenta perdia
as duas variáveis, a função anônima retornava cedo, e o build produzia o kernel
padrão da ST **sem nenhum dos dois drivers** — verde e errado. O Makefile já
resolvia exatamente isso para `MED_DATA_KEY_SOURCE` com `KEY_ARGS`, e o
comentário acima daquele bloco descreve a falha inteira; nunca foi aplicado a
este par. `KERNEL_ARGS` é o mesmo conserto, com a tradução do caminho para
`/work` de brinde.

Uma correção de rota junto: o cabeçalho do bbappend dizia "NOT YET EXERCISED IN
A BUILD", e isso **já estava desatualizado** antes deste trabalho — o
`buildhistory` mostra uma imagem completa construída em 27/09 00:45 com o fork
em `da723f985714`, presumivelmente com `NATIVE=1`, onde o ambiente passa direto.
Eu repeti a frase do comentário como se fosse fato em vez de checar o
`buildhistory`. O comentário foi reescrito com o que foi medido.

**2. `version-going-backwards`, e ele dispararia a cada commit.** O
`do_packagedata` recusou o build: de `+medda723f9857140+da723f9857` para
`+med013e9b1ab0f20+013e9b1ab0`. A revisão aparecia **duas vezes** na versão do
pacote e as duas eram hashes — e hash não tem ordem, então avançar o branch
parece um downgrade.

A metade que este repositório possuía era `d.appendVar('PV', '+med%s' % rev[:12])`.
Foi removida: a revisão já estava na versão do pacote por outro componente e no
caminho do módulo (`usr/lib/modules/6.6.129-g013e9b1ab0f2`), então a terceira
cópia não comprava rastreabilidade — comprava um defeito. O marcador `+med`
fica, porque a propriedade que ele defende (um build do fork nunca ser
confundido com o release da ST) não depende do hash.

A outra metade não é removível aqui, e o check foi **rebaixado a aviso, para esta
receita e só quando se constrói do fork**, com a justificativa escrita no
código: `version-going-backwards` protege um *package feed* — um cliente
incremental que recusaria um upgrade cuja versão caiu — e este projeto não tem
feed. Ele constrói imagens inteiras e as atualiza por bundles RAUC A/B que
substituem o rootfs. Não existe cliente comparando versões de pacote em lugar
nenhum do caminho.

A alternativa era o PR service (`PRSERV_HOST`), que é o mecanismo desenhado para
isto e tornaria a ordenação verdadeira em vez de dispensada. Foi recusado porque
guarda o contador num sqlite local à máquina, e um build cuja versão depende de
quantas vezes *este host* já construiu é uma troca pior para um projeto que
afirma reprodutibilidade.

### 2026-09-27 — O contador sai do sysfs privado e entra no subsistema

`f6d401b3b892`. Suíte de host de 195 para **209 verificações, 0 falhas**.

Os dois atributos `interrupt_count`/`interrupt_count_reset` eram a única ABI
customizada do driver, e eram a que este plano mais desconfiava: o §5.3
registrava que a objeção de um revisor seria forte, porque contar bordas num
pino é o que `drivers/counter` faz, e que a forma daquele par já se tinha
mostrado capaz de ser implementada ao contrário (Fase 1). Agora são um
`counter_device` com um Count, um Signal chamado `GP6` e um Synapse de borda de
descida, seguindo `drivers/counter/interrupt-cnt.c`.

**O que a mudança resolve** é a ABI e a largura. O que ela **não** resolve está
escrito no código em maiúsculas, porque seria fácil afirmar o contrário: a ponte
continua contando em 16 bits e limpando na leitura. O `u64` é um acumulador do
driver, e bordas além de 65536 entre duas leituras desaparecem antes de este
código vê-las. Ler com frequência suficiente continua sendo do chamador; notar
o *wrap*, não.

**O ganho que não estava no plano.** O acumulador é o que torna o
*read-and-reset* seguro a cada leitura. O par sysfs não podia fazê-lo — era
justamente o defeito da Fase 1, onde a leitura consumia o contador — e a
resposta de lá foi ler sem repor, deixando o `wrap` de 16 bits exposto. Aqui as
duas coisas se resolvem juntas: o hardware é limpo em toda leitura, e o valor
exposto é um total monotônico que dois leitores podem ler sem consumir um ao
outro. O caminho "certo" para a ABI acabou sendo também o caminho certo para o
dado.

**`signal_read` foi deliberadamente omitido.** Ler o nível do GP6 significaria
o comando de valor de GPIO, e a documentação é explícita em que ele só tem
efeito sobre pinos designados GPIO — o GP6 aqui é função dedicada. Um acessor
que devolvesse o nível de um pino que ele não consegue ler é pior que acessor
nenhum.

E `CONFIG_COUNTER=m` foi escrito no fragmento do BSP, ao lado dos dois símbolos
de driver. O `.config` construído já o tinha, **por acidente do defconfig da
ST** — que é exatamente o acidente da regra 3 do `CLAUDE.md`, o mesmo do
`CONFIG_DM_VERITY` que era grátis num kernel e ausente no outro. O `imply
COUNTER` no Kconfig do driver é a metade upstream da afirmação; a linha no
fragmento é a metade que o `do_med_check_kernel_config` verifica contra o
`.config` final. Não foi para `meta-med-distro` porque o `qemux86-64` não tem
ponte e não precisa de contador: o fragmento da distro é para capacidades que
toda máquina tem, e esta não é uma.

Três injeções, cada uma localizada no grupo B: a soma virando atribuição falha
6, não repor o hardware falha 9, aceitar escrita não-zero falha 2.

