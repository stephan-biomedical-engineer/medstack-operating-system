# Plano de Implementação — A rotina de bancada do front-end analógico

> **Status**: **nada aqui foi executado.** Este documento é o roteiro para a primeira sessão de
> bancada com o ADS1299 e o MCP2210 em mãos, e existe porque hoje os dois drivers de kernel
> **compilam e nunca carregaram**, nenhuma amostra real foi adquirida por nenhuma das ligações, e
> nenhuma constante de datasheet foi conferida — ver `implementation_plan_iio_afe.md` §13.3, que
> traça essa linha e continua sendo a referência sobre o que é resultado e o que é código escrito.
>
> **Escopo**: uma sequência de fases, cada uma com critério numérico, injeção de falha e local de
> registro, indo de "o host enumera a ponte" até "os recursos descritos no datasheet foram
> exercitados um a um" — **para os dois integrados**: o ADS1299 na Fase 8-A (§12) e o MCP2210 na
> Fase 8-B (§13). A ponte tem varredura própria porque ela vive no domínio de confiança do kernel,
> tem o datasheet menos conferido dos dois e nenhuma referência cruzada, e porque a maior parte dos
> seus recursos foi deliberadamente **não** implementada — e uma recusa deliberada também precisa
> ser verificada como tal. A §13.1 é a parte que roda **sem hardware nenhum** e pode começar hoje.
>
> **O que este plano não é**: não é um plano de verificação e validação no sentido da IEC 62304
> §5.7 — não há protocolo aprovado, não há rastreabilidade a requisitos, e nada aqui envolve um
> sujeito humano. É engenharia de *bring-up*, do mesmo tipo que `BRINGUP_STM32MP2.md` registra para
> a placa. Dizer isso aqui evita que a tabela da §8 seja lida depois como evidência de conformidade.
>
> **Depende de**: `implementation_plan_ads1299.md` (as duas ligações, os pinos, a conta de vazão da
> §8) e `implementation_plan_iio_afe.md` (os dois drivers, a camada adjunta, o driver `iio` do
> framework). Não substitui nenhum dos dois.

---

## 1. A regra que organiza o documento inteiro

Cada fase abaixo tem a mesma estrutura, e ela não é decorativa:

| Campo | Por que está lá |
|---|---|
| **Objetivo** | uma frase, e ela nomeia *uma* coisa. Uma fase que valida duas coisas não diz qual falhou |
| **Pré-requisito** | o que tem de estar verde antes. A fase não começa sem isso |
| **Procedimento** | comandos literais, para que a sessão seja repetível por outra pessoa |
| **Critério** | **numérico e falsificável**. "Funcionou" não é critério |
| **Injeção de falha** | como provocar a falha que o critério procura, para saber que ele a enxerga |
| **O que uma reprovação significa** | qual é a hipótese que cai. Sem isso, uma reprovação vira "tentar de novo" |
| **Registro** | onde o número mora depois: `RESULTS.md` §N ou `BRINGUP_AFE.md` §N |

A coluna de injeção de falha existe por causa da regra mais cara que este repositório aprendeu:
**uma asserção que nunca viu a falha que procura é uma alegação, não uma verificação** — foi assim
com `acq-active`, que passava num serviço em *crash loop*, e com `acq-sample-rate`, que só foi
acreditada depois de reverter a flag `SOCK_NONBLOCK` e ver 22/23. Numa bancada isso é mais
importante, não menos: o instrumento pode estar desligado, a ponta de prova pode estar no terra, e
um número plausível que vem de nada é o pior resultado possível.

Três regras herdadas se aplicam a todas as fases:

1. **Medir estado não é medir função.** "O módulo carregou", "o dispositivo apareceu em
   `/sys/bus/iio`" e "o serviço está `active`" são estados. Amostra por segundo entregue, µV pico a
   pico medidos e bordas de `DRDY` contra amostras coletadas são funções.
2. **Uma variável por vez.** Trocar ligação, ganho e ODR na mesma medida produz um número que não
   pertence a nenhuma das três mudanças.
3. **Ausência se registra.** O que não foi medido vai para a `RESULTS.md` §9 como não medido, do
   mesmo jeito que os itens que já estão lá. Uma lacuna anotada é honesta; uma lacuna omitida vira
   alegação no dia em que alguém ler a tabela.

---

## 2. Instrumentos e insumos

Sem estes itens a fase correspondente **não acontece** — e a alternativa de "estimar" é o que a §8
do plano do ADS1299 já faz e já marca como conta, não medição.

| Item | Fases | Para quê | Substituível por |
|---|---|---|---|
| Multímetro 5½ dígitos | 3, 4, 8 | VREF, alimentações, corrente em *standby* | um de 3½ dígitos, com perda de resolução declarada |
| Osciloscópio ≥100 MHz, 2 canais | 2, 8 | `CS`/`SCK`/`DRDY`, tempos de *setup* | analisador lógico, para o digital |
| Analisador lógico com decodificador SPI | 1, 2, 3 | ver o que a ponte realmente põe no barramento | — (é o instrumento que separa "o driver acha" de "o fio tem") |
| Gerador de função com saída ≤10 mV | 4, 5, 8 | sinal conhecido na entrada | gerador comum + atenuador resistivo |
| **Atenuador resistivo 1:1000 e 1:10000** | 4, 8 | descer de mV para µV, que é a faixa do EEG | — obrigatório; sem ele não se mede o fundo de escala em ganho 24 |
| Fonte DC de precisão (≤1 mV, estável) | 4 | varredura de ganho e verificação de `scale` | divisor resistivo a partir de uma referência |
| Resistores de 10 kΩ / 1 MΩ / 10 MΩ | 8 | impedância de eletrodo simulada, *lead-off* | — |
| Termômetro de contato | 8 | conferir o sensor de temperatura interno | — |
| Gaiola/plano de terra e cabos curtos | 4, 8 | o piso de ruído de 1 µV<sub>pp</sub> é da **placa**, não do conversor | — |
| Analisador de protocolo USB | 1, 2, 8-B | **a fonte independente** que fecha as constantes `[DS20005176?]` e alimenta os vetores da §13.1 | os itens (a) e (b) da §13.1, mais fracos |
| Placa de avaliação do MCP2210 | 1, 2, 8-B | permite toda a Fase 8-B **antes** de a placa analógica existir | — |

Consumível: pelo menos dois MCP2210 e dois ADS1299. Um componente que morre no meio da Fase 4
transforma "o driver está errado" e "a peça está morta" no mesmo sintoma, e não há como separar os
dois com uma amostra só.

---

## 3. Segurança, dita uma vez

Nada neste plano envolve um ser humano ligado ao conversor. As entradas são alimentadas por gerador
de função através de atenuador, ou curto-circuitadas, ou carregadas com resistores. Um eletrodo em
pessoa exige isolamento galvânico verificado, corrente de fuga medida e um protocolo aprovado — três
coisas que não existem neste projeto e que a IEC 60601-1 não dispensa. A fronteira está aqui e não
se move por conveniência de cronograma.

A alimentação analógica (AVDD 5 V) sai de fonte de bancada com limite de corrente ajustado antes de
energizar, não da USB, nas fases que medem ruído — e esse é justamente um dos resultados: **o piso
de ruído com a ponte alimentada pela USB contra o mesmo conversor com alimentação de bancada** é uma
medida que diz quanto do ruído é do caminho e quanto é do conversor.

---

## 4. Fase 0 — a passada de datasheet, antes de qualquer energia

**Objetivo**: transformar cada constante marcada `[SBAS499?]` e `[DS20005176?]` em constante
conferida ou em constante corrigida.

**Pré-requisito**: os dois datasheets em mãos (SBAS499 para o ADS1299, DS20005176 para o MCP2210) e
o esquemático da placa fechado.

**Por que é a fase 0 e não um item de rodapé**: todas as outras fases interpretam números lidos do
silício usando essas constantes. Um deslocamento errado no relatório do MCP2210 faz a Fase 2
reprovar um driver correto; uma tabela de ganho errada faz a Fase 4 aprovar um erro de escala de 4×
como se fosse o valor certo. Conferir depois de medir é medir duas vezes.

**Procedimento**:

```sh
# O tamanho da dívida, hoje:
grep -c '\[SBAS499?\]'     meta-custom/meta-med-afe-ads1299/recipes-kernel/ads1299/files/ti-ads1299.c
grep -c '\[DS20005176?\]'  meta-custom/meta-med-afe-ads1299/recipes-kernel/mcp2210/files/mcp2210-spi.c
```

Para cada marca, uma de três saídas, e **nenhuma outra**:

- confirmada → a marca vira a citação (`[SBAS499 §9.1 tab. 14]`), que é verificável por terceiro;
- corrigida → o valor muda **e** o defeito é registrado na `BRINGUP_AFE.md` §11 com o que ele teria
  causado, porque um erro de constante que não fez nada ainda ensina onde a leitura falha;
- indecidível pelo datasheet → vira teste de bancada explícito na fase correspondente, e a marca
  passa a `[SBAS499? — medido na Fase N]`.

**Os itens de maior consequência**, porque são os que a §2 do `implementation_plan_iio_afe.md`
identificou como diferença real entre o ADS1298 e o ADS1299:

| Constante | Onde | O que um erro causa |
|---|---|---|
| Tabela de ganhos do PGA `{1,2,4,6,8,12,24}` | `ti-ads1299.c` | erro de escala de até 4× em todo traçado, silencioso |
| VREF interna = 4,5 V (e o bit 5 de `CONFIG3` ser reservado) | `ti-ads1299.c` | erro de ganho de 12,5% em todo traçado, silencioso |
| Deslocamento de ODR = 7, sem bit HR/LP | `ti-ads1299.c` | taxa declarada ≠ taxa entregue; defeito de rastreabilidade |
| Reset de `CHnSET` = 0x61 (ganho 24, entrada em curto) | `ti-ads1299.c` | o dispositivo "funciona" e mede nada |
| Máscaras do registrador ID | `ti-ads1299.c` | recusa de *probe* pelo motivo errado |
| VID/PID do MCP2210 | `mcp2210-spi.c` | não enumera; falha alta, barata |
| Deslocamentos dos relatórios de 64 bytes | `mcp2210-spi.c` | transferência que "funciona" e devolve lixo |
| Modo do pino de interrupção (GP6) | `mcp2210-spi.c` | contador de `DRDY` não conta; perda de amostra volta a ser indetectável |
| Posição dos campos de `CONFIG2` | `ti-ads1299.c` | **pago em 2026-09-25 — ver abaixo** |

### O primeiro item pago, e por que ele muda a Fase 3

Em 2026-09-25, a primeira passada manual desta fase, sobre um único registrador, achou um defeito.
`CONFIG2` estava transcrito com os campos **todos uma posição acima**: reservado em 7:6 em vez de
7:5, `INT_CAL` no bit 5 em vez do 4, `CAL_AMP` no bit 3 em vez do 2 — sendo que o bit 3 é o *outro*
campo reservado do registrador, que o datasheet manda escrever 0.

O que ele teria causado, calculado antes e depois da correção:

| escrita | antes | depois | o que o antes fazia |
|---|---|---|---|
| `off` | `0xc0` | `0xc0` | correto por acidente |
| `1x_slow` | `0xe0` | `0xd0` | reservado 7h (proibido) e **`INT_CAL` = 0** |
| `2x_slow` | `0xe8` | `0xd4` | idem, mais o bit 3 reservado escrito como 1 |

Ou seja: **o gerador de teste interno nunca era ligado**. O autoteste do *probe* pediria o sinal,
leria entrada em curto, o `swing` reprovaria, e o `dev_err_probe` recusaria registrar o front-end —
sintoma ("não aparece `/dev/med-afe-eeg0`") a três passos da causa (um bit).

**A lição transferível, e ela é sobre verificação e não sobre o ADS1299**: o valor de *reset* que a
transcrição errada produz é `0xc0`, que é o valor certo. Um defeito de posição de campo que preserva
o valor de reset é **invisível** para uma varredura de valores de reset — que é exatamente o critério
5 da Fase 3. Ver a ressalva acrescentada lá.

**Critério**: os dois `grep` acima retornam **0**.

**Entregável de software desta fase** (e é a aplicação da regra "quando a bancada acha um defeito
que o build podia achar, a verificação vai para o build"): um `do_check_datasheet_debt` nas duas
receitas de módulo que conta as marcas e emite `bbwarn` com o número. Enquanto for maior que zero,
todo build diz em voz alta que está construindo cima de constantes não conferidas. Não é `bbfatal`
de propósito — hoje o número é grande e o build precisa continuar rodando; o que não pode é a dívida
ser invisível.

**Registro**: `BRINGUP_AFE.md` §1, com a data e o revisor. Nenhum número das fases 3 em diante pode
ser citado como resultado enquanto esta fase não fechar.

---

## 5. Fase 1 — a ponte sozinha, sem conversor nenhum

**Objetivo**: o host enumera o MCP2210, o *nosso* driver ganha o *bind*, e o protocolo de relatórios
de 64 bytes fecha ida e volta.

**Pré-requisito**: Fase 0. Nada de analógico ligado — o parâmetro de módulo existe exatamente para
isto: `mcp2210_spi.spi_device=""` (o comentário em `mcp2210-spi.c:140-148` diz que essa é a razão de
ele ser parâmetro).

**Procedimento**: `scripts/afe-phase1.sh`, que roda **na placa**.

```sh
scp scripts/afe-phase1.sh root@<placa>:/tmp/
ssh root@<placa> /tmp/afe-phase1.sh | tee fase1-$(date +%F).log
```

Ele confere os critérios 1 a 5 sozinho, imprime um `PASS`/`FAIL` por critério com
o valor observado ao lado, e termina com um bloco pronto para colar no §2 do
registro. O critério 6 e as duas injeções de falha restantes são manuais — pedem
gerador de sinais e uma pessoa — e o script prepara cada um e diz o que medir.

Ele não interpreta uma falha: o que cada critério derruba está escrito abaixo, e
adivinhar seria pior que reportar o observado.

> **O procedimento que estava aqui foi reescrito em 2026-09-28**, porque tinha
> envelhecido em quatro pontos e cada um teria falhado na bancada por motivo
> nenhum de hardware:
>
> | O que dizia | Por que não vale mais |
> | :--- | :--- |
> | `insmod .../extra/mcp2210-spi.ko` | o módulo é *in-tree* desde a §8 do plano do MCP2210: chama-se `hid-mcp2210`, mora em `kernel/drivers/hid/` e é autocarregado. O parâmetro agora é `hid_mcp2210.spi_device=` |
> | `interrupt_count` / `interrupt_count_reset` | os dois atributos sysfs **não existem**: o contador virou um `counter_device`, e o caminho é `/sys/bus/counter/devices/counterX/count0/count` |
> | `gpiodetect` / `gpioinfo` | **libgpiod não está na imagem**. O script lê `/sys/kernel/debug/gpio` e `/sys/bus/gpio/devices/`, que estão (`CONFIG_GPIO_CDEV=y`, `CONFIG_DEBUG_FS=y`, e `CONFIG_GPIO_SYSFS` **não**) |
> | `9 chip selects` na linha do probe | são **oito**. A mensagem dizia nove porque imprimia `MCP2210_NGPIO`, e isso foi corrigido — ela agora diz `9 GPIOs, 8 chip selects`. O critério 2 abaixo acompanhou |
>
> A quarta é a que vale reter: **o critério conferia um número que o driver já
> tinha deixado de significar.** Um roteiro de bancada envelhece junto com o
> código que ele mede, e ninguém percebe até o dia caro.

**Critérios**:

1. `lsusb` mostra o VID/PID que a Fase 0 confirmou, como dispositivo HID *full-speed*;
2. `dmesg` traz exatamente `USB-SPI bridge ready, 9 GPIOs, 8 chip selects, nothing attached`;
3. `/sys/bus/hid/drivers/mcp2210/` contém o dispositivo, e `hid-generic` **não**;
4. `gpiodetect` lista um `gpiochip` de rótulo `mcp2210` com **9** linhas;
5. em `/sys/bus/counter/devices/counterX/count0/`: `count` lê o mesmo número duas vezes seguidas
   em repouso, escrever `0` o zera, e escrever qualquer outro valor é recusado. Isso prova que os
   opcodes `GET_CHIP_SETTINGS` / `SET_CHIP_SETTINGS` / `GET_INT_COUNT` e seus deslocamentos estão
   certos, isto é, **a primeira confirmação empírica das constantes `[DS20005176?]`** — que até
   hoje só foram conferidas contra uma *transcrição* do datasheet;
6. injetando pulsos no GP6 com o gerador (1 kHz, 1000 pulsos, porta habilitada), o contador avança
   de 1000 ± 0 — *não* "avança".

**Injeção de falha**, e esta fase tem três, porque é onde moram três hipóteses que hoje só existem
como leitura de código:

- **`hid-generic` contra o nosso `id_table`.** Descarregar `mcp2210-spi`, replugar, e observar que
  `hid-generic` reivindica o dispositivo. Depois carregar o nosso com o dispositivo já plugado e
  ver se ele consegue tomar o *bind* ou se precisa de `unbind` explícito. Isto resolve o item que a
  §13.3 marca como "lido no fonte do kernel e não observado" e diz se o `KERNEL_MODULE_AUTOLOAD` é
  suficiente ou se falta um `HID_QUIRK`.
- **Ordem de carga.** Plugar com o módulo carregado, e plugar com ele ausente e carregando depois.
  Os dois caminhos têm de terminar no mesmo estado.
- **Contador que não conta.** Programar o pino como GPIO comum e repetir o teste de 1000 pulsos: o
  contador tem de ficar parado. Se ele avançar de qualquer jeito, o teste de 1000 pulsos não estava
  medindo o que parecia.

**O que uma reprovação significa**: (1)–(3) derrubam o VID/PID ou a hipótese do `hid-generic`;
(4)–(5) derrubam os deslocamentos dos relatórios, e a correção volta para a Fase 0; (6) derruba o
modo do pino de interrupção — e sem ele a ligação USB perde a única maneira que tem de saber que
perdeu amostra.

**Registro**: `BRINGUP_AFE.md` §2.

---

## 6. Fase 2 — o barramento SPI existe e transfere bytes

**Objetivo**: a máquina de estados de transferência do driver está certa, **medida sem o
conversor** — de modo que um defeito aqui nunca seja confundido com um defeito do ADS1299.

**Pré-requisito**: Fase 1. Um jumper ligando MOSI a MISO na saída da ponte.

**Por que fazer isso em vez de ir direto ao conversor**: porque na Fase 3 um byte errado tem duas
causas possíveis (a ponte ou o conversor) e aqui tem uma. É a mesma disciplina que fez a placa ser
trazida em pedaços, e não inteira, em `BRINGUP_STM32MP2.md`.

**Custo declarado**: o *loopback* precisa de um consumidor de userspace do barramento, isto é,
`CONFIG_SPI_SPIDEV`, que **nenhum perfil habilita hoje e que não deve entrar em imagem de produto**
— um `/dev/spidevX.Y` é um caminho de userspace para o barramento do conversor, exatamente o que a
arquitetura fecha. Entra como fragmento de kernel **só de bancada**, num arquivo separado e com o
nome dizendo isso (`med-kernel-bench.cfg`, aplicado por uma variável, nunca por padrão). Alternativa
sem `spidev`: fazer o *loopback* com o próprio `ti-ads1299` lendo o registrador ID e comparando com
o eco — mas aí já há duas variáveis de novo, que é o que esta fase existe para evitar.

**Procedimento**: `scripts/afe-phase2.sh`, que roda **na placa**.

Ele confere o critério 1 (o eco nos sete tamanhos) sozinho, tabula o tempo por
transação para o critério 2, e prepara os critérios 3, 4 e 5 — que só um
osciloscópio ou analisador lógico enxerga, e que nenhum script pode observar de
dentro da placa.

**O fragmento de bancada agora existe**, que é o que esta fase pedia e não
tinha: `MED_BENCH = "1"` aplica `meta-med-distro/.../med-kernel-bench.cfg`, hoje
com `CONFIG_SPI_SPIDEV=m` e nada mais. Nenhum perfil o liga por padrão, ele é
repassado ao container pelo mesmo mecanismo do `MED_KERNEL_GIT`, e o bbappend
emite um `bb.warn` sempre que está ligado — porque o erro que acontece de
verdade não é alguém ligar o bench de propósito, é alguém esquecer de desligar.

```sh
MED_KERNEL_GIT=... MED_KERNEL_SRCREV=... MED_BENCH=1 make stm32
```

O `spidev_test` vem de `spidev-test.bb`, que já existe no `meta-openembedded`
que este projeto usa — `bitbake spidev-test` e copiar o binário para a placa. Ele
deliberadamente **não** foi acrescentado à imagem: um binário que fala com o
barramento do conversor não pertence a uma lista de pacotes, e copiá-lo para
`/tmp` numa sessão de bancada deixa a exceção visível em vez de instalada.

**Critérios**:

1. eco **byte a byte idêntico** para 1, 2, 59, 60, 61, 120 e 512 bytes. Os tamanhos não são
   arbitrários: 60 é `MCP2210_MAX_XFER_CHUNK` como o driver acredita, e 61 é o primeiro que obriga
   a máquina de estados a fragmentar. Se a fronteira real for outra, é aqui que aparece;
2. tempo por transação, medido para cada tamanho, **em tabela** — este é o número que converte a §8
   do plano do ADS1299 de conta para medida. A previsão a falsificar: 2 a 4 relatórios USB por
   transação, o que põe o teto do caminho entre 250 e 500 transações por segundo;
3. o *clock* programado é o pedido: `SCK` medido no osciloscópio contra `spi_max_speed_hz`, com o
   erro relativo declarado, em 1 MHz e em 4 MHz;
4. tempos de `CS`: atraso de `CS` ativo até o primeiro `SCK`, e do último `SCK` até `CS` inativo,
   medidos e comparados com o que o driver programa nos campos `CS_TO_DATA` / `DATA_TO_CS`. É a
   confirmação de que aqueles deslocamentos `[DS20005176?]` chegam ao silício;
5. modo SPI: com `spi_mode_param` nos quatro valores, a polaridade e a fase observadas no analisador
   lógico batem com o pedido.

**Injeção de falha**: tirar o jumper e repetir o critério 1. O eco tem de virar constante (0x00 ou
0xFF) e **não** o padrão enviado. Sem isso, "o eco bateu" poderia ser o driver devolvendo o buffer
de transmissão sem nunca falar com o barramento — que é uma forma clássica de um teste de
*loopback* passar sem testar nada.

**O que uma reprovação significa**: o critério 1 em 61 bytes e não em 60 é fragmentação errada;
falha em todos os tamanhos é opcode ou deslocamento errado (volta à Fase 0); o critério 2 muito pior
que a previsão fecha a ligação USB abaixo de 250 SPS e isso **muda uma decisão de projeto**, não só
um número.

**Registro**: `RESULTS.md` (tabela de tempo por transação) e `BRINGUP_AFE.md` §3.

---

## 7. Fase 3 — o conversor responde, e a identidade vem do silício

> **Esta fase mistura duas coisas, e o conserto é de ordem e não de critério.**
> Registrado em 2026-09-28, ao revisar a cadeia completa antes da bancada.
>
> `ads1299_self_test()` roda **dentro do probe** e é o que decide se
> `devm_iio_device_register()` acontece (`ti-ads1299.c`, o `dev_err_probe` de
> "self test failed, front-end not registered"). Então os critérios abaixo — o
> probe fechando sem erro, `iio:device0/name`, `/dev/med-afe-eeg0` — não medem
> "o ADS1299 respondeu pelo registrador de ID". Medem **isso e mais o caminho
> analógico ter passado num teste**.
>
> Isso importa porque a §11 deste registro documenta um **fator de dois** que
> pode reprovar aquele autoteste em silício bom: a Tabela 14 não diz se a
> amplitude do gerador é pico ou pico a pico, e se a leitura certa for VREF/2400
> o teste espera o dobro da oscilação real. O risco está escrito na Fase 4, e
> **ele morde aqui** — com o sintoma "nenhum dispositivo IIO aparece", que é
> indistinguível de a ponte USB→SPI não funcionar.
>
> A decomposição que separa as duas, e ela usa costuras que já existem:
>
> | Ordem | O que prova | Como |
> | :--- | :--- | :--- |
> | 1 | USB → HID → `spi_controller` | Fase 1, com `mcp2210_spi.spi_device=""`: a ponte sozinha, nada analógico no quadro |
> | 2 | `spi_controller` → bytes | Fase 2, jumper MOSI/MISO |
> | 3 | o conversor responde | **`dmesg`**, não sysfs: a leitura do ID acontece dentro do probe, antes do autoteste |
> | 4 | o caminho analógico | o autoteste, que é um critério da Fase 4 disfarçado de pré-requisito desta |
> | 5 | a plataforma vê o front-end | `/dev/med-afe-eeg0` |
>
> O marco 5 é mais forte do que parece e vale usar como tal: a regra udev casa em
> `ATTR{name}=="ads1299-*"`, e esse nome o driver deriva do registrador de ID que
> a peça respondeu. **O symlink existir é, ele mesmo, a prova de que houve
> silício do outro lado** — não é preciso um `cat` separado para isso.
>
> E um cuidado de sessão: com `MED_EEG_LINK=usb` o `eeg-acquisition-service` abre
> o dispositivo IIO, e se ele não existir o serviço falha e reinicia sem
> `StartLimitBurst` — inundando exatamente o log que se está lendo. Mascarar o
> serviço antes de começar (`systemctl mask eeg-acquisition.service`) e desmascarar
> na Fase 6.

**Objetivo**: o ADS1299 é reconhecido pelo registrador de ID, e o caminho `iio` existe com o nome
estável que a plataforma usa.

**Pré-requisito**: Fase 2. AVDD e DVDD medidos com o multímetro **antes** de carregar qualquer
módulo.

**Procedimento**:

```sh
rmmod mcp2210-spi
insmod mcp2210-spi.ko                     # spi_device volta ao default "ads1299"
insmod ti-ads1299.ko
dmesg | tail -30

cat /sys/bus/iio/devices/iio:device0/name
ls -l /dev/med-afe-eeg0
cat /sys/bus/iio/devices/iio:device0/timestamp_source

# O mapa de registradores inteiro, contra os valores de reset do datasheet
cat /sys/kernel/debug/regmap/spi*/registers
```

**Critérios**:

1. o *probe* fecha sem erro, e `dmesg` não traz nenhuma linha de `dev_err_probe`;
2. `name` traz o identificador que o driver deriva do registrador ID lido — e o byte cru desse
   registrador, visto no `debugfs`, é o valor de reset da peça de 8 canais que a Fase 0 confirmou;
3. `/dev/med-afe-eeg0` existe, é *symlink* para o `iio:deviceN` certo, `root:root 0660`;
4. `timestamp_source` responde `host_timer` nesta ligação — e **este é um critério, não um detalhe**:
   é a confirmação de que o `info.irq = 0` que a ponte declara chega ao driver do AFE e é ele que
   escolhe a fonte de carimbo. É o mecanismo inteiro do argumento de fidelidade temporal, num
   arquivo de texto;
5. **o mapa de registradores completo (0x00–0x17) conferido contra os valores de reset**, um a um,
   em tabela. Esta é a verificação ponto a ponto mais barata de todo o plano e a que mais rende: ela
   valida de uma vez a leitura de registrador, a escrita, o `regmap` e boa parte da Fase 0.

   **Ressalva, medida e não suposta** (§4, "o primeiro item pago"): este critério é **cego** a um
   erro de posição de campo que preserve o valor de reset — e o defeito de `CONFIG2` achado em
   2026-09-25 é exatamente disso. Ele escrevia `INT_CAL` e `CAL_AMP` um bit acima do lugar, e
   mesmo assim produzia `0xc0` no reset, que é o valor correto. **Nenhuma comparação de valores de
   reset o veria**, e uma releitura de escrita tampouco, porque o registrador é R/W e devolve o que
   se escreveu nele. Só duas coisas o pegam: a leitura do datasheet (Fase 0) e a medição do sinal
   analógico (Fase 4). Este critério continua valendo pelo que valida — endereçamento, `regmap`,
   `RREG`/`WREG` — e **não** deve ser citado como conferência do mapa de bits.

**Injeção de falha**: desligar a alimentação analógica do conversor (mantendo a ponte) e recarregar
o `ti-ads1299`. O *probe* tem de **falhar nomeando o motivo**, e não produzir um dispositivo IIO. Um
driver que registra um `iio_dev` para um conversor ausente faz todas as fases seguintes medirem
zeros com convicção. Segunda injeção: com a peça presente, forçar o CS errado
(`spi_chip_select=1`) — também tem de falhar.

**O que uma reprovação significa**: (2) errado com (5) certo é máscara de ID errada (Fase 0); (5)
errado com a Fase 2 verde é endereçamento de registrador ou o protocolo de `RREG`/`WREG`; (4)
errado é bug de plumbing entre os dois módulos, e é a primeira vez que eles interagem.

**Registro**: `BRINGUP_AFE.md` §4, com a tabela de registradores inteira colada — ela é a prova
documental de que o conversor conversa.

---

## 8. Fase 4 — o caminho analógico, estático

> **Resolvido em 2026-09-28, e a resolução dá trabalho a esta fase.**
> O perfil `usb` não construía imagem: `do_derive_device_options` recusava
> `afe.bias_drive = true` porque o `ti-ads1299` deixava o amplificador de bias
> desligado sem controle para ligá-lo. A escolha do
> `implementation_plan_iio_afe.md` §15.2 foi implementar o controle no driver, e
> o §15.4 registra o resultado — a imagem `usb` existe.
>
> O que isso acrescenta **a esta fase**: há três configurações a medir, não uma.
> `off`, `reference` (BIASREF interno, sem realimentação) e `derived` (média dos
> canais habilitados). A prescrição da plataforma traduz para `derived`, então é
> essa que precisa da medida que importa; mas `reference` é o controle
> experimental óbvio para separar "o bias está dirigindo" de "o bias está
> rejeitando modo comum", e medir as duas com o mesmo eletrodo é o que dá
> significado ao número. Vale também conferir aqui o que o §15.4.1 corrigiu sem
> poder verificar: que `BIASREF_INT` desacoplado da referência do ADC não muda o
> comportamento numa placa com VREF externo.

**Objetivo**: a escala publicada pelo kernel é verdadeira, e cada modo do MUX de entrada faz o que
diz. Ainda sem *buffer*: tudo por leitura pontual em `in_voltage0_raw`, que é o que o
`selfTest()` do framework usa.

**Pré-requisito**: Fase 3, atenuador, fonte DC de precisão, gaiola/plano de terra.

**Procedimento e critérios**, um bloco por recurso:

### 8.1 A varredura de ganho — o teste que pega o erro de 4×

Aplicar uma DC conhecida (por exemplo 1,000 mV pela saída do atenuador) e, para cada ganho do
conjunto `{1, 2, 4, 6, 8, 12, 24}`:

```sh
for g in 1 2 4 6 8 12 24; do
    echo $g > /sys/bus/iio/devices/iio:device0/hardwaregain
    echo -n "ganho $g  raw=$(cat .../in_voltage0_raw)  scale=$(cat .../in_voltage0_scale)  "
    # tensão = raw * scale (mV)
done
```

**Critério**: `raw × scale` é **constante** dentro de 1% ao longo dos sete ganhos, e igual à tensão
aplicada dentro do erro do atenuador.

Este critério é o mais valioso da fase inteira e vale explicar por quê: ele **não depende de
precisão absoluta**. Se a tabela de ganhos do driver estiver trocada (o risco que a §2 do plano IIO
nomeia), `raw × scale` deixa de ser constante e o desvio aponta qual entrada da tabela está errada.
Se a VREF estiver declarada como 2,4 V em vez de 4,5 V, todos os sete valores erram junto por
12,5% — o que se vê contra a tensão aplicada. Os dois defeitos que seriam invisíveis num traçado de
EEG aparecem aqui como dois padrões diferentes.

### 8.2 Fundo de escala e saturação

Subir a DC até o conversor saturar, em ganho 1 e em ganho 24. **Critério**: satura em ±VREF/ganho
`[SBAS499?]`, limitado pela alimentação analógica, e o código saturado é o esperado para 24 bits com
sinal — nem sinal trocado, nem *wrap-around*. O `wrap` é o modo de falha que produz um artefato
clínico com cara de evento fisiológico.

### 8.3 O gerador de teste interno

```sh
cat .../input_mux_available          # normal shorted bias_meas mvdd temperature test_signal ...
echo test_signal > .../input_mux
for s in 1x_slow 1x_fast 2x_slow 2x_fast; do echo $s > .../test_signal; done
```

**Critérios**, e os números vêm do datasheet e portanto são `[SBAS499?]` até a Fase 0:

| Ajuste | Amplitude esperada (pico a pico, referida à entrada) | Frequência esperada |
|---|---|---|
| `1x_slow` | VREF/1200 = **3750 µV** | f<sub>CLK</sub>/2²¹ = **0,977 Hz** |
| `1x_fast` | 3750 µV | f<sub>CLK</sub>/2²⁰ = **1,953 Hz** |
| `2x_slow` | 7500 µV | 0,977 Hz |
| `2x_fast` | 7500 µV | 1,953 Hz |

Tolerância de amplitude: 5% na bancada (o self-test de probe do driver usa 20%, que é o limiar de
"o estágio está morto", não de calibração). Tolerância de frequência: a do oscilador interno, medida e declarada.

**Este é o ponto onde o self-test de probe deixa de ser código e vira verificação**: o
`ads1299_self_test()` compara `ganho·2²³/1200` códigos contra o pico a pico medido. Se a bancada
disser que o valor real é outro, é o driver que muda — e esse seria um achado a registrar, porque
hoje a constante `ADS1299_TEST_SIGNAL_PP_DIVISOR` está escrita em `ti-ads1299.c` sem nunca ter visto
silício. (Até 2026-09-16 ela morava em `MedicalDevice.cpp`, no framework; ver
`implementation_plan_iio_afe.md` §14.) Atenção a um efeito colateral: se o critério estiver errado, o
probe **falha** e o `in_voltageN_raw` desta seção não existe — para medir, o critério tem de estar
certo ou ser temporariamente afrouxado num build de bancada, registrado como tal.

### 8.4 Entrada em curto — o piso de ruído

`echo shorted > input_mux`, e medir o pico a pico e o RMS sobre 10 s, para cada ganho e em 250 SPS.

**Critério**: a medida é comparada à tabela de ruído referido à entrada do datasheet, **e o
resultado vale para a placa, não para o conversor** — é a distinção que o §4.5 do plano do ADS1299
já faz. Um piso de 3 µV<sub>pp</sub> onde o datasheet promete 1 não é um conversor ruim; é um
*layout*, um retorno de terra ou uma alimentação. Medir com a ponte alimentada pela USB e depois com
alimentação de bancada separa as duas coisas, e essa diferença é um resultado publicável.

O framework usa 10 µV<sub>pp</sub> como limite de "não está quebrado". A bancada mede o valor real; a
constante do framework só muda se a medida disser que 10 é frouxo demais para pegar um estágio morto.

### 8.5 Sensor de temperatura e MVDD

`echo temperature > input_mux`, aplicar a fórmula do datasheet `[SBAS499?]` e comparar com o
termômetro de contato encostado no encapsulamento. **Critério**: ±5 °C — folgado de propósito,
porque o gradiente entre o die e o encapsulamento é real e não é o que está sendo testado. O que
está sendo testado é que o MUX comuta e a fórmula não está fora por uma ordem de grandeza.

`echo mvdd > input_mux` e comparar com o multímetro na alimentação. **Critério**: ±2%.

**Injeção de falha da fase inteira**: com `input_mux` em `shorted`, aplicar o sinal do gerador na
entrada. A leitura **não pode mudar** — se mudar, o MUX não está comutando e todas as medidas de
8.4 estavam medindo o gerador desligado, não o conversor. É a injeção mais importante do plano
porque transforma "está quieto" de resultado em artefato.

**Registro**: `RESULTS.md` (nova seção: o front-end medido) e `BRINGUP_AFE.md` §5.

---

## 9. Fase 5 — aquisição contínua, e as quatro medidas que faltam

> **O contador mudou de lugar em 2026-09-27.** A comparação borda-a-borda desta
> fase não se lê mais em `interrupt_count` no dispositivo HID: o contador é um
> `counter_device` e o caminho é `/sys/bus/counter/counter*/count0/count`
> (`implementation_plan_mcp2210.md` §5.3). Duas consequências para o roteiro, e
> a segunda é boa. O valor agora é um `u64` acumulado pelo driver, então **não é
> mais preciso somar janelas curtas para escapar do wrap de 16 bits** — a
> aritmética que a §5.1 descreve deixa de ser necessária. Mas o limite não
> desapareceu: a ponte continua contando em 16 bits e limpando na leitura, então
> **ler no máximo a cada 65536 bordas** continua obrigatório — 262 s a 250 SPS,
> 4,1 s a 16 kSPS. O que mudou é de quem é o cuidado, não que ele exista.
> Uma leitura também é destrutiva no hardware por construção, e isso é seguro:
> o total exposto é monotônico e dois leitores não consomem a contagem um do
> outro.

**Objetivo**: fechar as quatro medições que a §9.4 do `implementation_plan_iio_afe.md` exige e que a
`RESULTS.md` §9 lista como não medidas: **jitter entre amostras, perda de amostra sob carga,
granularidade do carimbo de tempo e custo de CPU** — nas duas ligações.

**Pré-requisito**: Fase 4.

**Procedimento**:

```sh
D=/sys/bus/iio/devices/iio:device0
echo 1 > $D/scan_elements/in_voltage0_en   # ... até o canal 7
echo 1 > $D/scan_elements/in_timestamp_en
cat $D/scan_elements/in_voltage0_type       # le:s24/32>>0 — o framework recusa outro formato
echo 250 > $D/sampling_frequency
echo 512 > $D/buffer/length
echo 1 > $D/buffer/enable
timeout 60 cat /dev/med-afe-eeg0 > /tmp/raw.bin
```

**Critérios**:

1. **Taxa entregue contra configurada**: contar os *scans* em `/tmp/raw.bin` (tamanho ÷ tamanho do
   *scan*, que é 8×4 + 8 = 40 bytes com carimbo) e dividir por 60 s. Tolerância de 1% na ligação
   `spi`; na ligação `usb`, **medir e declarar** — a §8 do plano do ADS1299 prevê que 250 SPS está
   no limite e 500 SPS está acima do barramento, e esta é a medida que confirma ou derruba a
   previsão;
2. **A varredura de ODR**: para cada taxa do conjunto `{250, 500, 1000, 2000, 4000, 8000, 16000}`,
   o mesmo cálculo. Em `spi` espera-se todas; em `usb` espera-se a falha a partir de alguma, e o
   valor onde ela ocorre **é o resultado**. Falha aqui não é defeito: é a fronteira do caminho, e o
   driver tem de recusar ou contar a perda, nunca entregar 94 de 250 em silêncio — que é exatamente
   o defeito que a §13.6 do plano IIO registra do lado da publicação;
3. **Jitter**: histograma dos Δt entre carimbos consecutivos, com p50, p99 e máximo. Em `spi` com
   `DRDY` como interrupção, o carimbo é do momento da conversão e o jitter esperado é de
   microssegundos; em `usb`, o carimbo é do `hrtimer` do host e o esperado é de milissegundos, com o
   quadro USB de 1 ms como piso. **A comparação dos dois histogramas é o resultado central do
   trabalho sobre front-end** — é ela que sustenta a frase "as duas ligações não são clinicamente
   equivalentes" com número em vez de argumento;
4. **Perda de amostra**: `lost_samples` do AFE e `interrupt_count` da ponte, contra o número de
   *scans* recebidos. **A janela não pode ser uma subtração só**: `interrupt_count` é um valor de 16
   bits e dá a volta a cada 65536 bordas — 262 s a 250 SPS, 4,1 s a 16 kSPS (§13.5). A medida de 10
   minutos é a **soma de janelas curtas**, cada uma bem abaixo do período de volta na ODR em uso.
   Em `spi`: perda zero é o critério.
   Em `usb`: a perda é medida, e a diferença entre bordas de `DRDY` e amostras coletadas é **a única
   coisa que torna essa perda detectável** — que é a razão do GP6 existir (§4.3 do plano do ADS1299);
5. **Sob carga**: repetir (1), (3) e (4) com a HMI conectada, um `dd` escrevendo no volume LUKS e
   uma carga de CPU. Este é o cenário que produziu a perda de 62% no caminho de publicação e não
   apareceu em nenhuma das 22 asserções de então;
6. **Custo de CPU**: tempo de CPU do processo e do *worker* de kernel por hora de aquisição, nas
   duas ligações. Na ligação USB há ainda o custo de *polling* do contador, que não existe na outra.
7. **Granularidade do carimbo**: o menor Δt não nulo observado entre carimbos, que é a resolução
   real do relógio nesse caminho — e não a resolução que `CLOCK_MONOTONIC` promete.

**Injeção de falha**: reduzir `buffer/length` para um *scan* e repetir. `lost_samples` **tem de
crescer**. Um contador de perdas que nunca contou nada não é evidência de que não houve perda; é
evidência de nada. Segunda injeção: desligar o gerador no meio da janela — os valores mudam, a
contagem de amostras não.

**Registro**: `RESULTS.md`, seção nova, com o comando ao lado de cada número e a frase explícita do
que ele **não** significa — que é o formato que o documento já usa.

---

## 10. Fase 6 — o framework, a aplicação e a suíte

**Objetivo**: o que as fases 1–5 provaram com `cat` e `echo` passa a ser exercido pelo software que
vai para o produto, sem nenhuma diferença de código entre as ligações.

**Pré-requisito**: Fase 5.

**Procedimento e critérios**:

1. **Autoteste de verdade.** Carregar o driver com o conversor ligado: o probe roda gerador interno
   e entrada em curto, com critério numérico, e registra o resultado no log do kernel. Depois subir
   o `eeg-acquisition-service` com `MED_EEG_LINK=usb`, cujo `selfTest()` confere identidade e canais
   com escala. **Critério**: passa com o conversor íntegro e **reprova com motivo nomeado** no
   `dmesg` — sem dispositivo registrado, e o serviço recusando subir por dispositivo ausente — com o
   conversor alimentado e um estágio danificado, ou com um ganho que o driver não programou. Sem
   essa segunda metade, o autoteste é decorativo.
2. **A mesma imagem, as duas ligações.** Construir com `MED_EEG_LINK=spi` e com `usb`, e confirmar
   que **nenhum arquivo de aplicação difere** — só o `eeg.conf` substituído e o conteúdo da camada
   adjunta instalado. É a mesma medida de portabilidade que o projeto já faz entre QEMU e
   STM32MP257, agora entre dois caminhos de front-end.
3. **O registro clínico diz o que é.** `metadata.json` da sessão carrega `acquisition.link` e a
   fonte de carimbo que o kernel declarou. **Critério**: numa sessão pela ligação USB o registro diz
   `host_timer`, e numa pela SPI diz `drdy_irq`. Um registro que não sabe dizer a fidelidade do
   próprio carimbo é o defeito que essa cadeia inteira existe para evitar.
4. **`DeviceAllow=char-iio`.** A §13.3 do plano IIO marca esse item como nunca exercitado, e ele é a
   primeira coisa a olhar se o serviço tomar `EPERM` ao abrir o conversor. **Critério**: o serviço
   abre `/dev/med-afe-eeg0` dentro do *sandbox*, e removendo a diretiva ele **falha** — injeção de
   falha obrigatória, porque uma permissão que nunca negou nada não se sabe se está concedendo.
5. **Asserções novas na suíte de aceitação**, no mesmo formato das 23 existentes, e cada uma
   injetada de falha antes de ser acreditada:

   | nome | o que afirma | como falha de propósito |
   |---|---|---|
   | `afe-identity` | o `name` do IIO vem do registrador de ID | desligar o AFE |
   | `afe-timestamp-source` | o `metadata.json` declara a fonte que o kernel declara | forçar o valor no config |
   | `afe-lost-samples` | `lost_samples` é zero na ligação com IRQ | reduzir `buffer/length` |
   | `afe-rate-delivered` | taxa entregue ≥ 90% da configurada, com HMI conectada | o método de `acq-sample-rate` |
   | `afe-selftest` | o autoteste roda e registra o resultado na trilha de auditoria | entrada aberta |

   **Sem alvos de `make` novos.** A ligação já é selecionada por `MED_EEG_LINK` nos arquivos KAS, e
   acrescentar um alvo por variação seria repetir um erro já corrigido neste repositório. O que
   falta é transporte: hoje o `med-check.py` conversa com o QEMU por `pty`, e a bancada precisa da
   mesma lista de asserções sobre console serial ou SSH. Isso é **uma** mudança (a classe de
   transporte), não uma suíte nova.

**Registro**: `RESULTS.md` e `BRINGUP_AFE.md` §6.

---

## 11. Fase 7 — as duas ligações, lado a lado

**Objetivo**: produzir a tabela comparativa que é o resultado do trabalho de front-end, com o
**mesmo conversor, o mesmo sinal e a mesma sessão de bancada** — trocando só o caminho.

**Pré-requisito**: Fase 6 nas duas ligações.

Mesmo gerador, mesma amplitude, mesma ODR (250 SPS, a única que as duas sustentam), mesma duração,
mesma carga:

| | `spi` (`DRDY` como IRQ) | `usb` (ponte MCP2210) |
|---|---|---|
| Taxa entregue / configurada | | |
| Jitter p50 / p99 / máx | | |
| Granularidade do carimbo | | |
| Amostras perdidas em 10 min, sem carga | | |
| Amostras perdidas em 10 min, sob carga | | |
| CPU por hora de aquisição | | |
| ODR máxima sustentada | | |
| Piso de ruído, entrada em curto | | |

A ligação `amp` entra nessa tabela quando houver firmware no Cortex-M33 — hoje bloqueada por
`Support of signed firmware only` (§11.1 do plano do ADS1299). A coluna fica na tabela, vazia e
nomeada, porque é assim que uma ausência não vira resultado.

**Critério**: não é "uma ganha". É que cada célula tenha número e método. A conclusão de projeto já
está escrita — `acquisition.link` vai no metadado de toda sessão justamente porque as ligações não
são equivalentes — e esta tabela é o que a sustenta.

---

## 12. Fase 8-A — a varredura do datasheet do conversor

**Objetivo**: exercitar os recursos do ADS1299 descritos no SBAS499, e **declarar explicitamente**
quais ficam de fora e por quê.

**Pré-requisito**: Fases 3 a 5.

A coluna "exposto hoje" é a mais útil do plano, porque ela transforma a varredura numa decisão de
arquitetura: um recurso que o produto nunca vai usar **não precisa de controle em sysfs**, e
exercitá-lo pelo `debugfs` do `regmap` é suficiente e não amplia superfície. Um recurso que o
produto usa e que não está exposto é uma lacuna de driver.

### 12.1 Comandos

| Comando | Como testar | Critério | Exposto hoje |
|---|---|---|---|
| `RESET` | emitir e reler todos os registradores | voltam aos valores de reset | interno |
| `START` / `STOP` | com *buffer* habilitado | `DRDY` pulsa / para de pulsar (contador do GP6) | interno |
| `RDATAC` / `SDATAC` | ler registrador com `RDATAC` ativo | tem de falhar; é o erro clássico da família | interno |
| `RDATA` | leitura pontual | 27 bytes: 3 de status + 8×3 | interno |
| `STANDBY` / `WAKEUP` | multímetro na alimentação | corrente cai para o valor do datasheet e volta | **não** — decidir |
| `WREG` / `RREG` | escrever e reler cada registrador gravável | ida e volta idêntica | via `debugfs` |

`STANDBY` é o único que o produto plausivelmente quereria (um equipamento de EEG passa boa parte do
tempo sem adquirir) e é também o único que hoje não tem caminho. A decisão fica registrada aqui:
ou entra como `_powerdown` no driver, ou se declara que o produto não usa gestão de energia do AFE.

### 12.2 Configuração

| Recurso | Registrador | Como testar | Exposto hoje |
|---|---|---|---|
| Conjunto de ODR | `CONFIG1[2:0]` | Fase 5, critério 2 | `sampling_frequency` |
| Saída de clock (`CLK_EN`) | `CONFIG1[5]` | osciloscópio no pino CLK | não — e o produto usa oscilador interno |
| *Daisy-chain* | `CONFIG1[6]` | exige dois conversores | **fora de escopo, declarado** |
| Gerador de teste | `CONFIG2` | Fase 4.3, quatro combinações | `test_signal` |
| Buffer de referência interno | `CONFIG3[7]` | multímetro em VREFP: 4,5 V com buffer ligado | não — sempre interno |
| Referência externa | `CONFIG3[7]`=0 + fonte | VREFP externa, e a escala publicada acompanha | **não** — decidir |
| Modo *single-shot* | `CONFIG4[3]` | um `START` → uma conversão só | não |
| Comparador de *lead-off* | `CONFIG4[1]` | §12.4 | parcial |

A linha da referência externa merece atenção: se o produto usar referência externa, a constante
`afe.reference_uv = 4500000` do `eeg.conf` passa a mentir, e é ela que o envelope de segurança usa
para o fundo de escala. O self-test de probe não herda o erro — mede a amplitude em códigos, que não
depende de V<sub>REF</sub>, e o teto de ruído usa a referência que o próprio driver conhece. É o tipo
de acoplamento que só aparece quando alguém tenta usar o recurso.

### 12.3 Canal, ganho e MUX

| Recurso | Como testar | Critério | Exposto hoje |
|---|---|---|---|
| Ganhos `{1..24}` | §8.1 | `raw × scale` constante em 1% | `hardwaregain` (global) |
| **Ganho por canal** | ganhos diferentes em canais diferentes | — | **não**: o driver escreve o mesmo em todos |
| *Power-down* de canal | `CHnSET[7]` por canal | corrente cai ~5 mW/canal; o canal lê constante | não |
| MUX: `normal` | sinal do gerador | segue a entrada | `input_mux` |
| MUX: `shorted` | §8.4 | piso de ruído | `input_mux` |
| MUX: `bias_meas` | com *bias* ligado | mede o sinal de realimentação | `input_mux` |
| MUX: `mvdd` | §8.5 | ±2% do multímetro | `input_mux` |
| MUX: `temperature` | §8.5 | ±5 °C | `input_mux` |
| MUX: `test_signal` | §8.3 | amplitude e frequência | `input_mux` |
| MUX: *bias drive* P/N | injeção de modo comum | — | `input_mux` |
| SRB1 / SRB2 | `MISC1` / `CHnSET[3]` | referência comum a todos os canais: montagem referencial de EEG | **não** — e isto é uma lacuna real |

**SRB1 é o item mais importante desta tabela.** EEG clínico é quase sempre montagem referencial, com
um eletrodo de referência comum a todos os canais — que é exatamente o que SRB1 faz. Um driver que
não o expõe obriga referência por par de eletrodos, o que dobra a eletrodagem. É a lacuna de driver
com maior consequência clínica, e sai desta varredura como item de trabalho, não como observação.

O ganho por canal está logo atrás: `applyOptions()` escreve um ganho só, e a `input_mux_store`
comenta explicitamente que o MUX vale para todos os canais de propósito (um autoteste parcial
produziria um registro meio medida, meio estímulo). Para o ganho o mesmo argumento não vale: canais
com impedância de eletrodo diferente querem ganhos diferentes. Decidir, e registrar a decisão.

### 12.4 Detecção de eletrodo solto (*lead-off*)

| Recurso | Como testar | Critério |
|---|---|---|
| Corrente de excitação `{6 nA, 24 nA, 6 µA, 24 µA}` `[SBAS499?]` | resistor conhecido na entrada, medir a queda | corrente dentro de 20% |
| Limiares do comparador | varrer a tensão de entrada | comuta no limiar programado |
| `LOFF_SENSP` / `LOFF_SENSN` | habilitar por canal | só os canais habilitados reportam |
| `LOFF_FLIP` | inverter a polaridade | o status inverte |
| `LOFF_STATP` / `LOFF_STATN` | abrir a entrada de **um** canal | `lead_off_status` mostra o bit daquele canal e só dele |
| Modo AC (`CONFIG4`) | — | mede impedância sem componente DC |

O caminho de leitura já existe (`lead_off_status`, dois bytes hexadecimais). O que não existe é
controle: o framework hoje **recusa explicitamente** `afe.lead_off_detection = false`, dizendo que
os comparadores são ligados pelo driver no *probe* e não podem ser desligados dali. Esta fase decide
se isso continua sendo verdade ou vira um controle.

O teste que interessa clinicamente: com 10 kΩ (contato bom), 1 MΩ (contato ruim) e aberto, o status
tem de distinguir os três — ou, se distinguir só dois, é isso que o registro clínico pode afirmar.

### 12.5 Realimentação de *bias* (o que a literatura chama *right-leg drive*)

| Recurso | Como testar | Critério |
|---|---|---|
| `BIAS_SENSP`/`BIAS_SENSN` | escolher quais canais alimentam a média | o sinal de *bias* acompanha os escolhidos |
| `PD_BIAS`, `BIASREF_INT` | ligar/desligar | — |
| **CMRR** | injetar 50/60 Hz de modo comum, medir a rejeição | medida e declarada, com e sem *bias* |
| `BIAS_LOFF_SENS` | eletrodo de *bias* solto | detectado |

**A CMRR com e sem realimentação de *bias* é a medida que diz se este equipamento serve para EEG**
em ambiente real, e é a que o datasheet promete em condições de laboratório. Medir na placa e
publicar os dois números, com a frase explícita de que o valor é da placa e não do conversor.

### 12.6 Ruído, a matriz

Piso de ruído referido à entrada, entrada em curto, **para cada ganho × cada ODR**, comparado à
tabela do datasheet. É a medida mais demorada do plano (49 células se for feita cheia) e a que mais
diz sobre o projeto da placa. Recorte aceitável: os sete ganhos em 250 SPS, mais ganho 24 nas sete
ODRs — 13 células, com a razão do recorte escrita.

### 12.7 O que fica de fora, e por quê

Declarado aqui para que a ausência não seja lida como esquecimento:

- ***daisy-chain*** — exige dois conversores e a placa tem um;
- **sincronização por clock externo** — o produto usa oscilador interno (decisão de §4.1);
- **medições com sujeito humano** — §3 deste plano;
- **ensaios de segurança elétrica da IEC 60601-1** — exigem laboratório acreditado;
- **deriva térmica e envelhecimento** — exigem câmara e tempo;
- **INL e erro de ganho absoluto** — exigem referência melhor que o conversor, que não há na
  bancada. O que se mede é linearidade **relativa**, pela varredura de 12.3.

---

## 13. Fase 8-B — a varredura do datasheet da ponte

**Objetivo**: exercitar o MCP2210 como componente por direito próprio, e não apenas como o fio por
onde o conversor fala.

**Pré-requisito**: Fases 1 e 2. **Não depende do ADS1299** — e isso é a propriedade mais útil desta
fase inteira: ela roda com uma placa de avaliação do MCP2210 e um jumper, antes de a placa do
front-end existir. Se os componentes chegarem em ordens diferentes, esta é a fase que começa.

### 13.0 Por que a ponte merece varredura própria

Três razões, e nenhuma é simetria de documento:

1. **Ela está no domínio de confiança do kernel.** O conversor fala por um barramento; a ponte é um
   driver que registra um `spi_controller`, um `gpiochip` e atende relatórios USB. Um defeito nela
   não produz uma amostra errada — produz um caminho de dados errado, e em qualquer transferência.
2. **É o componente com o datasheet menos conferido.** As constantes `[SBAS499?]` do conversor têm
   um driver mainline revisado como referência cruzada; as `[DS20005176?]` da ponte **não têm
   nenhuma** — não há MCP2210 em mainline, e a única implementação de terceiros conhecida foi
   deliberadamente recusada (§2.2 do `implementation_plan_iio_afe.md`). O que a Fase 0 confere aqui,
   confere sozinha.
3. **Boa parte dos recursos da ponte ainda não foi implementada** — e desde 2026-09-27 isso é
   "ainda não", não "nunca": o `implementation_plan_mcp2210.md` §4.3 passou o objetivo do driver
   para cobertura funcional completa, faseada. A verificação muda de forma junto, e para melhor.
   Era "o driver não escreve NVRAM", garantido pela ausência do código; passa a ser **"o caminho
   de aquisição nunca escreve NVRAM"**, que é uma propriedade do runtime e não da ausência, e que
   continua sendo uma alegação até que alguém leia a NVRAM antes e depois de uma sessão inteira e
   compare. O risco não mudou em nada: um driver que escrevesse NVRAM por engano poderia mudar o
   VID/PID de um aparelho em campo — falha permanente, num componente soldado.

**Procedimento**: `scripts/afe-phase8b.sh`, que roda **na placa**, e as
subseções abaixo são o que ele percorre.

Ele confere o que é observável de dentro da placa — o `spi_master`, o
`gpiochip` e suas nove linhas, o `counter` e sua descrição, e grava o snapshot
de identidade que o teste negativo compara ao final. O resto ele prepara e
nomeia, porque precisa de gerador, de analisador USB ou de uma pessoa puxando o
cabo.

**Uma limitação que o script diz na cara, e que vale registrar aqui.** O
snapshot negativo alcança os descritores USB — VID, PID, strings —, que é o que
a NVRAM produz na enumeração. Ele **não** lê os ajustes de *power-up* (`0x61`)
nem a EEPROM (`0x50`), porque o driver ainda não expõe nenhum dos dois a
userspace: a Fase 6A.1 do `implementation_plan_mcp2210.md` leu a NVRAM apenas
para o log, e a EEPROM é a 6B/6C. Então o teste negativo desta fase cobre a
identidade e não o conteúdo inteiro, e dizer isso é melhor que um snapshot que
parece cobrir tudo. Ele fecha quando a decisão de interface da 6A.2/6C existir.

### 13.1 O que dá para testar **sem hardware nenhum**, e o que isso não prova

Esta é a parte que responde a "testes unitários" no sentido estrito, e ela pode começar hoje.

A distinção que organiza tudo: **lógica e deslocamentos são duas coisas diferentes.**

| | Testável no host, sem hardware | Por quê |
|---|---|---|
| **Lógica** — fragmentação em blocos de 60, contagem de *stalls*, `-EMSGSIZE` acima de 512, achatamento de `spi_transfer` múltiplos, redistribuição do buffer de recepção | **sim, hoje** | não depende de o datasheet estar certo; depende de o código fazer o que o próprio código diz |
| **Deslocamentos e opcodes** — `MCP2210_OFF_*`, `MCP2210_CMD_*`, bits de modo do pino | **não** | um teste escrito a partir da mesma leitura do datasheet que escreveu o código prova **consistência, não correção** |

Essa segunda linha é o ponto todo, e vale escrever por extenso porque é o erro fácil: se eu escrevo
`MCP2210_OFF_BITRATE 4` no driver e depois escrevo um teste que afirma que a taxa vai no *offset* 4,
o teste passa e não sabe de nada. Ele só vira evidência com vetores de **origem independente**:

- **(a)** exemplos trabalhados dentro do próprio DS20005176 — grátis, se existirem;
- **(b)** captura do `daniel-santos/mcp2210-linux`, que é GPLv2 e portanto legível — a implementação
  foi recusada como dependência (§2.2), o que não impede usá-la como **oráculo**, e é precisamente o
  valor que a §2.2 diz que ela tem: "as lições caras, ele já as pagou";
- **(c)** captura com analisador USB do utilitário do fabricante conversando com uma ponte real —
  a fonte mais forte, exige hardware **uma vez só**, e depois alimenta testes de host para sempre.

**Entregável**: um arranjo de teste no host (~200 linhas) que compila a máquina de estados de
transferência com `mcp2210_command()` substituído por um duplo que responde a partir de uma tabela
de vetores. Com isso, sem nenhum hardware:

| Verificação | O defeito que ela pega |
|---|---|
| mensagem de 1, 59, 60, 61, 120, 512 bytes → sequência de blocos esperada | fragmentação errada na fronteira de 60 |
| mensagem de 513 bytes | tem de dar `-EMSGSIZE`, não truncar em silêncio |
| dispositivo responde `BUSY` 99 vezes e depois `OK` | a transferência completa |
| dispositivo responde `BUSY` 101 vezes | `-ETIMEDOUT`, e **limitado** — o laço não é infinito |
| dispositivo devolve menos bytes que o pedido, em vários blocos | recepção acumulada, nenhum byte perdido nem duplicado |
| dispositivo devolve **mais** bytes que o pedido | o corte em `received + got > len` acontece; sem ele é estouro de buffer |
| vários `spi_transfer` numa `spi_message` | achatamento e redistribuição preservam as fronteiras |
| `spi_message` de comprimento zero | não chega a falar com a ponte |

É o mesmo padrão das 80 verificações de host do framework, e vale o mesmo que elas valem: não
substitui bancada, e antecipa a classe de defeito que a bancada encontraria da maneira cara — com
um conversor no meio e duas explicações possíveis para cada sintoma.

Custo declarado: as funções puras estão hoje entrelaçadas com `struct mcp2210` e com as chamadas
HID. Para o arranjo funcionar, `mcp2210_do_transaction()` precisa que a troca de relatórios seja
substituível — um ponteiro de função ou uma macro de compilação. É uma refatoração pequena e é
justamente a que não se quer fazer **depois** de a bancada estar montada.

### 13.2 O protocolo de relatórios, comando a comando

Cada linha é uma ida e volta de 64 bytes. A coluna "hoje" diz o que o driver usa — e o item 3 da §13.0
explica por que as linhas com "não" importam tanto quanto as com "sim".

| Comando `[DS20005176?]` | Como testar | Critério | Hoje |
|---|---|---|---|
| `GET_CHIP_STATUS` (0x10) | emitir e decodificar | dono do barramento SPI, tentativas de senha, estado de proteção | não |
| `SPI_CANCEL` (0x11) | cancelar no meio de uma transação longa | o motor volta a ocioso e a próxima transação é limpa | **definido e não usado** |
| `GET_INT_COUNT` (0x12) | §13.5 | — | sim |
| `GET_CHIP_SETTINGS` (0x20) | ler e comparar com o que foi escrito | ida e volta idêntica em 9 designações + *other settings* | sim |
| `SET_CHIP_SETTINGS` (0x21) | §13.4 | leitura-modificação-escrita não perde o CS | sim |
| `SET/GET_GPIO_VALUE` (0x30/0x31) | §13.4 | — | sim |
| `SET/GET_GPIO_DIR` (0x32/0x33) | §13.4 | — | sim |
| `SET/GET_SPI_SETTINGS` (0x40/0x41) | §13.3 | — | sim (só `SET`) |
| `SPI_TRANSFER` (0x42) | Fase 2 | — | sim |
| Ler/escrever EEPROM de 256 bytes | ler byte a byte | §13.6 | **não ainda**, Fases 6B/6C |
| Ler/escrever NVRAM (ajustes de *power-up*) | §13.6 | **negativo**: a aquisição nunca escreve | **não ainda**, Fase 6A |
| Acesso protegido por senha / trava permanente | §13.6 | — | **não ainda**, Fase 6A |
| Pedido de liberação do barramento por mestre externo | §13.7 | — | **não ainda** |

Duas observações que saem da tabela e não do datasheet:

**`SPI_CANCEL` está definido no fonte e nunca é chamado.** É a saída de recuperação que existe
quando uma transação fica pela metade — e hoje a recuperação é esperar o `-ETIMEDOUT` do laço de
*stalls*. Decidir na bancada: ou o cancelamento entra no caminho de erro, ou a constante sai. Uma
constante definida e não usada num driver de dispositivo médico é uma intenção não implementada, que
é exatamente a forma do defeito que a §13.6 do plano IIO registra (a unit ignorava `SIGPIPE` "para
que uma HMI que sai não mate a aquisição", e uma HMI *lenta* matava).

**`GET_SPI_SETTINGS` nunca é lido.** O driver escreve os ajustes de transferência e nunca confere o
que a ponte programou. A taxa é o caso concreto: o MCP2210 tem um divisor, então nem toda taxa
pedida é realizável, e o driver de hoje não saberia. É o mesmo padrão que `applyOptions()` já aplica
do lado do conversor — escreve `sampling_frequency` e **relê**, porque "um aparelho adquirindo numa
taxa que o metadado não nomeia é um defeito de rastreabilidade". A ponte merece a mesma regra.

### 13.3 O controlador SPI

| Recurso | Como testar | Critério |
|---|---|---|
| Faixa de taxa (1,5 kbps – 12 Mbps, como o driver declara) | osciloscópio nos dois extremos e em 4 MHz | `SCK` medido contra o pedido, erro relativo declarado |
| **Quantização da taxa** | varrer taxas pedidas, ler o que a ponte programou | tabela pedido → realizado; é o dado que justifica (ou não) o *read-back* da §13.2 |
| Modos 0–3 | analisador lógico | polaridade e fase observadas batem com o pedido |
| `SPI_CS_HIGH` | idem | as máscaras `idle_cs`/`active_cs` trocam, e o CS fica ativo em alto |
| Atrasos CS→dado, dado→CS, dado→dado | osciloscópio | **o driver programa zero nos três**; medir o que zero produz de fato |
| Tamanho da transação | 27 bytes (o quadro do conversor) e 512 | CS permanece asserido pela transação inteira, mesmo com fragmentação |
| Limite de mensagem | `spi_message` de 513 bytes | `-EMSGSIZE`, sem truncar |

A linha dos atrasos é a que mais pode surpreender. O driver escreve **0** em `CS_TO_DATA`,
`DATA_TO_CS` e `DATA_TO_DATA`, isto é, pede à ponte o mínimo que ela souber fazer. O ADS1299 tem
tempos mínimos próprios entre o CS e o primeiro clock e entre comandos consecutivos `[SBAS499?]`; se
o mínimo da ponte for menor que o mínimo do conversor, o sintoma não é falha de transferência — é
comando ocasionalmente mal interpretado, que aparece como registrador que não bate e como amostra
esquisita de vez em quando. **Intermitente, num equipamento de medição, é o pior tipo** (§4.4 do
plano do ADS1299, sobre dois mestres no mesmo barramento, diz o mesmo com outras palavras). Este é o
item com maior chance de custar um dia de bancada se não for medido cedo.

A linha do CS asserido durante a transação inteira também é crítica e já está justificada no
comentário do fonte: o ADS1299 **não tem FIFO**, e o quadro de 27 bytes tem de sair antes da próxima
conversão. Se a ponte soltar o CS entre blocos de 60 bytes, o conversor vê duas transações e o
quadro se perde. Para 27 bytes não há fragmentação, então este teste **precisa** de uma transação
longa de propósito — 120 bytes com o analisador lógico olhando o CS.

### 13.4 Os nove GPIO e as funções dedicadas

| Recurso | Como testar | Critério |
|---|---|---|
| GP0–GP8 como saída | escrever 0 e 1, medir | cada pino, um a um |
| GP0–GP8 como entrada | aplicar nível, ler | idem |
| Direção | ler de volta após escrever | ida e volta idêntica |
| Designação (GPIO / *chip select* / dedicada) | escrever e reler os 9 bytes | ida e volta idêntica |
| **Interferência entre GPIO e SPI** | pôr GP1 em 0 como GPIO, fazer uma transferência, reler GP1 | **GP1 continua 0** |
| Funções dedicadas por pino | uma a uma, conforme o datasheet | cada pino tem a função que o DS20005176 atribui — e **quais são, por pino, é item da Fase 0** |

A linha em negrito é um teste de *unidade* do driver no sentido mais literal: as designações de pino
e as máscaras de CS moram no mesmo registro de ajustes, e `mcp2210_set_transfer_settings()` escreve
máscaras de 9 bits a cada transferência. O comentário do fonte diz que a leitura-modificação-escrita
existe justamente para que uma mudança de GPIO não destrua a designação do CS. **A recíproca não
está testada**: que uma transferência SPI não destrua o estado de um GP usado como saída comum. E a
§4.3 do plano do ADS1299 recomenda exatamente isso — `RESET`, `PWDN` e `START` do conversor saindo
de GPs da ponte. Se a transferência mexer neles, o conversor é resetado no meio da aquisição.

### 13.5 O contador do GP6 — e um achado que muda a Fase 5

| Recurso | Como testar | Critério |
|---|---|---|
| Modos de contagem (nenhum, borda de descida, de subida, pulso alto, pulso baixo) `[DS20005176?]` | gerador, 1000 pulsos, um modo por vez | conta 1000 ± 0 no modo certo e **0 nos que não contam** |
| Leitura sem consumir | ler duas vezes seguidas | o mesmo valor |
| `interrupt_count_reset` | zerar e reler | zero |
| Largura do contador | §abaixo | — |
| Frequência máxima contável | subir o gerador até perder contagem | o teto, medido — é o teto de ODR desta ligação |

**O achado**: `interrupt_count` é lido de um campo de **16 bits** (`get_unaligned_le16`) e exposto
como tal. Logo ele **dá a volta em 65536 bordas**. A 250 SPS isso são **262 s — 4 min 22 s**; a
16 kSPS, **4,1 s**.

A Fase 5 deste plano, como estava escrita, mandava comparar `interrupt_count` antes e depois de uma
janela de **10 minutos**. Essa comparação estaria errada por construção: o contador teria dado duas
voltas e a diferença ingênua acusaria perda de amostra que não houve, ou — pior — encobriria perda
que houve. A correção, e ela vale para qualquer uso futuro deste número:

- a janela de comparação tem de ser **mais curta que o período de volta**, com folga: 60 s a
  250 SPS é 4× de margem, e **a janela escala com a ODR** (a 16 kSPS, 1 s);
- uma janela longa se faz somando janelas curtas, nunca com uma subtração só;
- zerar com `interrupt_count_reset` no início de cada janela é a forma mais simples, ao custo de
  descartar o que se acumulou desde a última leitura.

Vale notar **de onde veio o achado**: de ler o driver enquanto se escrevia o procedimento de teste,
não de medir. É o argumento a favor de escrever o procedimento antes da bancada — e o oposto do erro
de método que o `implementation_plan_iio_afe.md` registra em si mesmo. Se o procedimento tivesse
sido improvisado na bancada, o número teria saído, teria sido plausível, e teria entrado na
`RESULTS.md`.

Decisão a tomar junto: se o driver deve acumular num `u64` em vez de expor o valor cru de 16 bits.
Ele hoje **não pode** — só lê sob demanda, e não há quem garanta que alguém leu antes da volta. Ou a
política de janela fica documentada em quem lê (a suíte e a `RESULTS.md`), ou o driver ganha uma
leitura periódica, que é estado e trabalho novos dentro do kernel. A escolha é registrada, não
adiada.

### 13.6 NVRAM, EEPROM e proteção de acesso — os testes **negativos**

Aqui a pergunta não é "funciona?", é "ficou intocado?". E note que a pergunta
sobrevive à mudança de escopo do §4.3 do plano do MCP2210: quando a Fase 6A
existir, o driver **saberá** escrever a NVRAM, e a partir daí estas
verificações passam a medir o que sempre importou de verdade — que o caminho
de aquisição não a escreve — em vez de medir a ausência de uma função.

| Verificação | Procedimento | Critério |
|---|---|---|
| **A aquisição nunca escreve NVRAM** | ler os ajustes de *power-up* antes da sessão e depois de todas as fases | **byte a byte idênticos** |
| VID/PID de *power-up* preservados | idem | o aparelho continua enumerando como o mesmo dispositivo depois de um ciclo de energia |
| EEPROM de 256 bytes | ler o conteúdo | registrado como está; o caminho de aquisição não a usa |
| Contador de tentativas de senha | `GET_CHIP_STATUS` antes e depois | não avança — nada aqui tenta autenticar |

O teste da NVRAM é o mais importante da §13 inteira e é barato: duas leituras e um `cmp`. A razão é
de produto, não de bancada — os ajustes de *power-up* decidem o VID/PID, e o VID/PID decide se o
`hid_device_id` do nosso driver casa. Um driver que escrevesse NVRAM por engano transformaria um
aparelho em campo num que **não enumera mais como o dispositivo que o seu próprio software procura**,
com o componente soldado na placa. É a mesma família do defeito que o `implementation_plan_luks.md`
resolveu para a chave: uma coisa que precisa sobreviver a um evento futuro, guardada onde esse
evento a destrói.

E uma **decisão de produto que esta varredura levanta e não resolve**: o MCP2210 suporta travar a
NVRAM por senha ou permanentemente. Num aparelho médico, uma ponte cujos ajustes qualquer software
de qualquer host pode reescrever é superfície. Travar na fabricação é uma linha no roteiro de
produção e fecha a superfície; travar permanentemente também fecha a possibilidade de corrigir. As
duas opções e a razão da escolha vão para a `BRINGUP_AFE.md`, decididas — a §15 critério 6 cobra.

### 13.7 Recuperação de erro, e o que acontece quando o cabo sai

O caminho de erro é o que menos se exercita e o que mais importa num aparelho ligado a paciente.

| Cenário | Como provocar | Critério |
|---|---|---|
| Desconexão no meio de uma aquisição | puxar o cabo | `-ETIMEDOUT` em ≤1 s (o `REPLY_TIMEOUT_MS` do driver), o serviço **para com motivo nomeado**, e o registro declara quantas amostras tem — não a taxa configurada |
| Reconexão | replugar | reenumera, o *symlink* volta, e uma **nova sessão** começa; o registro anterior continua íntegro e fechado |
| Ponte travada | segurar o barramento / alimentar mal de propósito | o laço de *stalls* limita em 100 tentativas e devolve erro; **a thread de aquisição não fica pendurada** |
| Mensagem grande demais | `spi_message` > 512 B | `-EMSGSIZE` |
| Alimentação da USB no limite | fonte com corrente limitada | enumera ou falha, nunca meio-caminho |
| `-EBUSY` sustentado | dispositivo respondendo ocupado | limite e erro, não laço infinito |

A linha da desconexão é a que liga esta fase ao resto do projeto: "um visualizador pode perder
dados; o registro não" (§13.6 do plano IIO). O cabo saindo é a versão física disso, e o critério é o
mesmo — o registro não pode declarar 250 amostras/s de um período em que o conversor não estava lá.

**Injeção de falha desta subseção**: todas as linhas acima **são** injeções de falha. É a única
subseção do plano em que o procedimento e a injeção coincidem, e por isso ela tem uma exigência
extra: cada cenário tem de ser rodado **duas vezes**, porque um caminho de erro que funciona uma vez
e deixa o driver em estado inconsistente falha na segunda. `KASAN` e `lockdep` ligados no kernel de
bancada — é onde um uso após liberação no caminho de desconexão aparece de graça.

### 13.8 Dois achados de leitura de código, e o que eles custam se não forem corrigidos antes

Os dois saíram de ler o driver para escrever esta seção. Ficam registrados aqui porque a bancada vai
encontrá-los do jeito caro — como sintoma intermitente e sem causa aparente.

**1. A resposta não é conferida contra o comando.** `mcp2210_command()` olha só o byte de status
(*offset* 1) e nunca compara o byte 0 da resposta com o comando enviado, embora o protocolo o ecoe
`[DS20005176?]`. Combinado com o segundo achado, isso vira corrupção silenciosa de dados.

**2. Uma resposta atrasada é consumida pelo comando seguinte.** Depois de um `-ETIMEDOUT`, a
resposta do comando que expirou ainda pode chegar. `mcp2210_raw_event()` a copia para `rxbuf` e
completa a espera — só que a espera pendente agora é a do **próximo** comando. Concretamente: um
`GET_INT_COUNT` expira; o `SPI_TRANSFER` seguinte recebe a resposta do contador; o driver lê a
contagem de bytes recebidos do *offset* 2 de uma resposta de contador e copia bytes arbitrários para
o fluxo de amostras. **Nenhuma camada acima consegue perceber** — o CRC do `FrameHeader` é calculado
depois, sobre os bytes já corrompidos.

A correção das duas é a mesma e é barata: conferir `rxbuf[0] == txbuf[0]` e descartar (não
completar) uma resposta que não case com o comando pendente. Custa algumas linhas, e transforma o
segundo defeito de silencioso em um `-EIO`.

Vale dizer o que eles **não** são: não são defeitos observados. São leitura de código, da mesma
categoria que a hipótese sobre o `hid-generic` — e, pela regra da §11 do `BRINGUP_STM32MP2.md`,
continuam sendo dívida até que um teste os veja. A diferença é que este par tem correção conhecida e
barata, e o custo de deixar para depois é um sintoma intermitente no meio de uma sessão de bancada,
que é o pior lugar para gastar um dia.

### 13.9 O que fica de fora, e por quê

- **NVRAM e provisionamento** — o driver não escreve, e a §13.6 testa que não escreve. Provisionar é
  passo de fabricação, com ferramenta do fabricante, fora do aparelho;
- **EEPROM de usuário** — sem consumidor. Seria um lugar plausível para número de série da placa
  analógica, e isso é uma ideia, não um requisito. Registrada e não implementada;
- **Arbitragem com mestre SPI externo** — a topologia com dois mestres foi fechada por projeto
  (§4.4 do plano do ADS1299: isolamento por jumper ou *buffer*), então o mecanismo da ponte não é
  usado. **Se** a placa vier com os dois caminhos ligados, isto sai de "fora de escopo" e vira
  requisito;
- **Suspensão/retomada USB e *remote wakeup*** — o aparelho não suspende a USB durante aquisição;
  vale um teste único de que uma suspensão do host não deixa a ponte em estado morto;
- **Modo de operação como conversor USB-serial ou GPIO puro** — não é o que este produto usa.

---

## 14. Ordem, e o que bloqueia o quê

```
§13.1  testes de host  ──────────────── sem hardware nenhum, começa hoje
                                                 │
Fase 0  datasheet ───────────────────────────────┤  (bloqueia toda medida)
                                                 ▼
Fase 1  ponte sozinha ──► Fase 2  loopback SPI ──┬──► Fase 8-B  varredura da PONTE
                                                 │    (só precisa da ponte)
                                                 ▼
                                        Fase 3  identidade
                                                 │
                                   ┌─────────────┴──────────────────┐
                                   ▼                                ▼
                          Fase 4  analógico estático       Fase 8-A  varredura
                                   │                       do CONVERSOR
                                   ▼                       (usa 3, 4 e 5)
                          Fase 5  aquisição contínua
                                   │
                                   ▼
                          Fase 6  framework e suíte
                                   │
                                   ▼
                          Fase 7  as duas ligações lado a lado
```

**A Fase 8-B não depende do conversor**, e é a consequência prática mais útil desta revisão: se a
placa de avaliação do MCP2210 chegar antes da placa analógica — o que é o mais provável, já que uma
é de prateleira e a outra está sendo projetada —, há uma fase inteira de trabalho de bancada
disponível com um jumper e um gerador de função. E a §13.1 não depende de hardware nenhum.

Duas dependências externas, que não são deste plano e podem atrasar uma ligação inteira:

- a **ligação `spi`** (conversor no SPI6 do A35, `DRDY` como interrupção de verdade) precisa dos
  três pinos declarados (`MED_AFE_CS_GPIO`, `MED_AFE_DRDY_GPIO`, `MED_AFE_RESET_GPIO` — a receita do
  *overlay* se recusa a construir sem eles, de propósito) **e** de alguém aplicando o `.dtbo`: falta
  um `fdt apply` no `bootcmd`, que é do `meta-med-bsp`, e falta confirmar que o `.dtb` da ST carrega
  `__symbols__`;
- a **ligação `amp`** está bloqueada por `Support of signed firmware only`.

A ligação `usb` **não depende de nenhum dos dois**, e é por isso que a Fase 1 começa por ela. Isso é
propriedade do cronograma, não de mérito técnico: a ligação de produto é a `amp`.

---

## 15. Critérios de aceitação do plano inteiro

1. `grep -c '\[SBAS499?\]\|\[DS20005176?\]'` nos dois drivers retorna **0**;
2. as quatro medições da §9.4 do `implementation_plan_iio_afe.md` existem, nas duas ligações, com
   comando e com a frase do que **não** significam;
3. a tabela da Fase 7 está preenchida, com a coluna `amp` presente e vazia;
4. cada asserção nova da suíte foi injetada de falha e **viu** a falha;
5. a `RESULTS.md` §9 foi atualizada: o que saiu de "não medido" saiu, e o que continua lá continua
   nomeado;
6. toda decisão de driver que a Fase 8 levantou está **decidida por escrito** — implementada ou
   declarada fora de escopo com a razão. Uma lacuna conhecida e não decidida é o pior dos três
   estados. Do conversor (8-A): SRB1, ganho por canal, `STANDBY`, referência externa, controle de
   *lead-off*. Da ponte (8-B): *read-back* dos ajustes de transferência, uso de `SPI_CANCEL` no
   caminho de erro, política de janela do contador de 16 bits, e travamento da NVRAM na fabricação;
7. **a NVRAM da ponte está byte a byte idêntica** ao que era antes da primeira sessão de bancada —
   a verificação da §13.6, que é a única do plano cujo critério é "nada aconteceu";
8. os dois achados da §13.8 estão resolvidos ou refutados por teste, não por releitura;
9. os defeitos encontrados estão em `BRINGUP_AFE.md` §11 no formato que o `BRINGUP_STM32MP2.md` §11
   usa: o defeito, a causa real, o que provou a correção — e, quando couber, a regra transferível.

---

## 16. Riscos desta bancada

**A ponte tem o datasheet menos conferido e nenhuma referência cruzada.** O conversor tem um driver
mainline revisado como oráculo; o MCP2210 não tem nada equivalente. Se um deslocamento de relatório
estiver errado, o sintoma aparece no conversor — que é o componente inocente. É a razão de as Fases
1 e 2 acontecerem antes de qualquer analógico, e de a §13.1 propor vetores de origem independente.

**A peça pode estar morta e o driver certo.** É o motivo de haver duas de cada. Sem o segundo
exemplar, um `probe` que falha tem duas explicações e nenhuma maneira barata de escolher.

**O piso de ruído medido vai ser pior que o datasheet, e isso é normal.** A tentação será tratar
isso como falha do conversor ou do driver. Não é: é a placa, o cabo, o terra e a fonte. O risco real
é o oposto — medir um piso *muito bom* porque a entrada não estava conectada onde se pensava, que é
exatamente o que a injeção de falha da Fase 4 existe para pegar.

**A ligação USB pode não sustentar 250 SPS.** A §8 do plano do ADS1299 põe 250 SPS "no limite", com
multiplicadores não medidos. Se a Fase 2 medir mais relatórios por transação do que a conta supõe, a
ligação USB cai para uma taxa menor, e isso é uma mudança de decisão de projeto — melhor descoberta
na Fase 2, com um jumper, do que na Fase 5, com o conversor no meio.

**Fase 0 vai encontrar erros, e é para isso que ela existe.** Encontrar um valor errado não é uma
crise: é o retorno de uma passada que o repositório já sabia que devia. O que seria caro é descobrir
o mesmo valor errado na Fase 5, depois de duas semanas interpretando amostras.

**Corrigir um defeito é o momento de procurar o próximo.** Esta regra já se pagou duas vezes aqui —
o OOM do `QB_MEM` que só apareceu quando a aquisição parou de morrer antes, e a perda de 62% de
amostras que só apareceu quando a taxa de quadro subiu. Numa bancada o padrão é o mesmo e mais
rápido: o primeiro defeito esconde o segundo, e o segundo costuma ser o interessante.
