# Plano de Implementação — O caminho IIO para o modo USB, e a camada adjunta de front-end

> **Status**: plano corrente. Decide a arquitetura do **modo USB** do front-end analógico e
> **substitui a Ligação B** do `implementation_plan_ads1299.md` §2.2, que previa uma ponte em
> *userspace* sobre `/dev/hidraw*`. A Ligação A (hat, AMP, Cortex-M33) segue **inalterada e
> confirmada** — os dois modos coexistem.
>
> **Escopo**: dois drivers de kernel escritos neste projeto, uma camada Yocto adjunta que os
> hospeda, um terceiro driver no MedFramework, e o que isso custa e compra no argumento da
> IEC 62304.
>
> **Verificado** (2026-08-27, contra a árvore `linux-stm32mp` 6.6.129 que este repositório
> constrói): não existe driver de MCP2210 no kernel — `grep -rli mcp2210 drivers/` retorna nada,
> enquanto `hid-mcp2200.c` e `hid-mcp2221.c` existem; `CONFIG_HIDRAW` está **desligado** nos dois
> alvos; não há driver IIO do ADS1299 em `drivers/iio/adc`. Os dados sobre o driver de terceiro
> (§2) foram levantados na mesma data. Todo o resto é projeto, não medição.
>
> **A conferir antes do texto final**: SBAS499 (ADS1299) e DS20005176 (MCP2210), incluindo VID/PID.
>
> **Correção de método, 2026-09-06.** A primeira redação deste plano concluiu "escrever os dois
> drivers do zero" a partir de uma busca de **escopo insuficiente**: procurou-se o *part-number*
> ADS1299 na árvore 6.6 deste repositório, não se encontrou, e parou-se aí. A pergunta adjacente —
> *existe driver para um chip irmão, em algum kernel?* — não foi feita. Ela tem resposta: o
> **ADS1298**, da mesma família ADS129x, ganhou driver IIO no **mainline 6.9**. Isso muda §2, §3,
> §6, §11 e §12, e está corrigido abaixo. Fica registrado porque o erro é de método e não de fato:
> verificou-se a negativa esperada em vez de se testar a pergunta que importava.

---

## 1. A decisão

Duas ligações, ambas definitivas, uma escolhida por configuração:

| | **Modo hat** (Ligação A) | **Modo USB** (este plano) |
|---|---|---|
| Caminho | ADS1299 → SPI → Cortex-M33 → `rpmsg` → userspace | ADS1299 → SPI → MCP2210 → USB → **kernel** → IIO → userspace |
| Driver do `MedicalDevice` | `rpmsg` | **`iio`** (novo) |
| Quem atende o `DRDY` | ISR no M33 | contador de GPIO do MCP2210, lido por *polling* |
| Carimbo de tempo | instante da conversão | instante em que o host leu o relatório USB |
| Jitter esperado | microssegundos | **milissegundos** (quadro USB *full-speed* = 1 ms) |
| Código de kernel novo | nenhum | **dois drivers**, ambos escritos aqui |
| Argumento IEC 62304 §5.3 | o mais forte: Linux não toca o conversor | mais fraco: o AFE passa a viver no kernel (§10) |

**O que o IIO compra**: a ABI padrão de aquisição do Linux — buffers com `kfifo`, *triggers*,
canais com escala e taxa expostos em `sysfs`, e o ferramental que já fala esse protocolo. O que ele
**não** compra no modo USB é fidelidade de carimbo de tempo: o `DRDY` não gera interrupção no host,
e o barramento entrega em quadros de 1 ms. Isso está dito aqui para que a expectativa não seja
construída errada — e a comparação entre os dois modos vira resultado do trabalho, não desculpa.

---

## 2. De onde parte cada driver

São dois drivers com pontos de partida **muito diferentes**, e tratá-los como um só problema foi o
erro da primeira redação.

### 2.1 O AFE: adaptação de driver mainline, não escrita do zero

`drivers/iio/adc/ti-ads1298.c` entrou no **Linux 6.9**, escrito por Mike Looijmans (Topic Embedded
Products), GPL-2.0, ~900 linhas. O ADS1298 é o irmão do 1299 na família ADS129x: 8 canais, 24 bits,
ΔΣ com amostragem simultânea, mesmo SPI, mesmo `DRDY`, mesmo comando `RDATA`. O 1298 é a variante
para ECG; o 1299, para EEG. As diferenças estão em bits de configuração e faixas de ganho — **não**
na estrutura do driver.

Consequências, todas favoráveis:

- **O ponto de partida é código revisado pelos mantenedores do IIO**, não uma folha em branco.
- O kernel deste repositório é o 6.6.129, então o arquivo não está na árvore: é preciso um
  *backport* de **uma** release. Trabalho pequeno, e mensurável.
- Abre a possibilidade de **submeter suporte a ADS1299 upstream**, partindo de um driver cujo autor
  e cujo subsistema já conhecem o caminho. Para o TCC, uma contribuição aceita no kernel é resultado
  de outra categoria — e não está no caminho crítico, o que a torna um bônus e não um risco.
- O que falta no `ads1298` para o nosso uso é pequeno e identificável: ele **não declara canal de
  carimbo de tempo** (`IIO_CHAN_SOFT_TIMESTAMP`), que é justamente o que interessa a um registro
  clínico. Ver §6.

### 2.2 A ponte: aí sim, provavelmente escrita aqui

O `daniel-santos/mcp2210-linux` é o candidato óbvio. Levantamento de 2026-08-27:

| | |
|---|---|
| Último commit de código | **dezembro de 2018** (kernel 4.19/4.20) |
| Licença | **GPL-2.0-or-later** — os fontes trazem cabeçalho GPLv2+ e `MODULE_LICENSE("GPL")`; a API do GitHub reporta "nenhuma" apenas por não haver arquivo `LICENSE` na raiz |
| Tamanho | ~398 KB, ~12 arquivos C |
| Issues abertas | 28 |
| Autodeclaração no README | instalação "*a pain in the ass*" por conflito com o driver HID; formato binário "creek" "*unstable and subject to change without a version bump*" |

**Licença não é impedimento** — GPLv2+ é redistribuível num produto e exprimível no Yocto. A decisão
é por outras quatro razões:

1. **Sete anos de deriva de API.** Entre 4.19 e 6.6: `access_ok()` perdeu um argumento (e esse driver
   tem ABI de ioctl), `spi_master` virou `spi_controller`, a API de `gpio_chip` mudou, o HID mexeu
   em detalhes de requisição. Portar é trabalho real, e o resultado é mantido por nós de qualquer
   forma.
2. **Ele traz o que não queremos**: ABI de configfs/ioctl para userspace, formato de configuração
   binário próprio declarado instável, emulação de interrupção. Em espaço de kernel, num dispositivo
   médico, cada linha excedente é superfície.
3. **Como SOUP é um mau negócio, e escrever o nosso inverte o sinal** — ver §3.

**O que colher dele.** A GPLv2 permite ler e derivar (derivando, o nosso sai GPLv2 e credita a
origem). O valor está no que o datasheet subespecifica: a máquina de estados da transferência, a
semântica de repetição quando o integrado responde ocupado, os tempos de *chip select*, e a ordem
das mensagens de configuração. São as lições caras, e ele já as pagou.

### 2.3 O que ainda não foi avaliado, e deve ser antes de escrever

Confirmado em 2026-09-06: **não há MCP2210 em mainline** — a busca por identificador no
cross-referencer retorna vazio, com um identificador de controle validando o método, e nenhuma
evidência de merge. Mas o repositório do Daniel **não é a única** implementação fora da árvore, e as
outras não foram examinadas:

| Origem | O que promete | Estado |
|---|---|---|
| `lkundrak/mcp2210-linux` | fork | **não avaliado** |
| `vianpl/mcp2210` | "USB to SPI master & GPIO evaluation drivers" | **não avaliado** |
| `HaoTNN/mcp2210` | driver para a ponte | **não avaliado** |
| branch `mcp2210-driver` em `kernel.googlesource.com` (árvore `wpan-next`) | trabalho de um desenvolvedor de kernel | **não avaliado** |

Aplicar a estes os mesmos critérios da tabela acima — data do último commit, kernel alvo, licença nos
fontes, tamanho, escopo excedente — é **pré-requisito da etapa 2 da §12**. Escrever do zero continua
sendo a hipótese de trabalho; deixou de ser conclusão.

---

## 3. SOUP: o que é, e o que este plano faz com ele

**SOUP — *Software of Unknown Provenance*** (IEC 62304 §3.29): software já desenvolvido e
disponível, **não** desenvolvido para ser incorporado a este dispositivo, ou cujo processo de
desenvolvimento não tem registros adequados. Obrigações que ele carrega:

| Cláusula | Exigência |
|---|---|
| §5.3.3 | especificar requisitos funcionais e de desempenho do item |
| §5.3.4 | especificar o hardware e software de que o item depende |
| §7.1.2–7.1.3 | avaliar se anomalias conhecidas contribuem para situação perigosa, consultando listas publicadas |
| §8.1.2 | identificar em controle de configuração: título, fabricante, designação de versão |

Classificação dos componentes deste caminho:

| Componente | Classificação | Consequência |
|---|---|---|
| Kernel Linux, glibc, systemd, Qt | SOUP | já contabilizados; SBOM gerado pelo `create-spdx` |
| `galcore` (GPU Vivante) | SOUP **em kernel** | já presente, e já *taints* o kernel |
| `daniel-santos/mcp2210-linux`, se adotado | **SOUP em kernel, sem manutenção** | a "lista publicada de anomalias" da §7.1.3 seriam 28 issues abertas de um repositório parado há sete anos |
| **`mcp2210` escrito aqui** | **não é SOUP** | item de software próprio: requisitos que escrevemos, testes que escrevemos, registros que temos |
| **`ads1299-iio`, derivado do `ti-ads1298.c` mainline** | **não é SOUP** — e é a melhor posição das três | item nosso, derivado de código **revisado por mantenedor de subsistema**, com autoria, histórico e processo de revisão públicos e citáveis |

É aqui que o argumento se inverte: adotar um driver de terceiro sem manutenção acrescenta SOUP no
domínio de falha mais crítico do sistema; os dois caminhos escolhidos acrescentam **itens de
software sob controle de configuração próprio**. Para a defesa, *"o único código de kernel que este
dispositivo acrescenta são dois drivers sob nosso controle, um deles derivado de driver mainline
revisado"* é uma posição substancialmente mais forte — e a proveniência do derivado é **melhor** que
a do escrito do zero, porque tem revisão de terceiros documentada.

---

## 4. A camada adjunta

Os dois drivers **não** entram nas quatro camadas do `MedStack`. Entram numa camada nova:

```
meta-custom/meta-med-afe-ads1299/
├── conf/layer.conf                       prioridade 7, LAYERDEPENDS = "core"
├── recipes-kernel/
│   ├── mcp2210/            mcp2210-spi_1.0.bb          (module.bbclass)
│   └── ads1299/            ads1299-iio_1.0.bb          (module.bbclass)
├── recipes-bsp/
│   └── ads1299-overlay/    overlay de devicetree do modo hat
└── recipes-core/udev/      regra por VID/PID e permissões do /dev/iio:device*
```

Três razões, e a terceira é a que vale para a banca:

1. **A pilha de quatro camadas continua sendo o que a tese descreve.** A métrica de reuso é calculada
   sobre as `meta-med-*`; um adjunto de habilitação de hardware não a contamina.
2. **Quarentena.** Todo o código de kernel específico de um front-end fica numa camada só, que o SBOM
   identifica isolada.
3. **Vira um resultado.** *"Um front-end analógico novo custa uma camada e uma variável, e zero
   linhas nas quatro camadas da plataforma"* é uma demonstração de extensibilidade mais forte do que
   qualquer coisa obtida escondendo os drivers dentro do `meta-med-bsp`. É a mesma jogada que o
   `meta-med-bsp` fez pela portabilidade entre placas, agora para classe de front-end.

**Regra que a camada herda**: como o `meta-med-bsp`, ela **provê e não consome** — nada de bbappend
sobre receita de camada superior. Ela preenche ganchos já declarados
(`MED_BSP_INSTALL:append:<máquina>`, `MED_AMP_FIRMWARE`) e nada mais.

E a documentação tem de dizer que o `MedStack` são **quatro** camadas e que esta é adjunta — senão,
em seis meses, alguém conta cinco. Atualizar `PROJECT_CONTEXT.md` e `CLAUDE.md` faz parte da entrega.

---

## 5. Driver 1 — `mcp2210`: um `spi_controller` sobre HID

### O que ele faz

Liga-se como `hid_driver` ao integrado (VID/PID a confirmar no DS20005176) e registra um
`spi_controller`. A partir daí, qualquer driver SPI do kernel — incluindo o da §6 — pode fazer
*bind* nos dispositivos filhos, exatamente como fariam num SPI de SoC.

### Fatos do protocolo que dimensionam o driver

| Fato | Consequência de projeto |
|---|---|
| Relatórios HID de **64 bytes**, comando/resposta | toda transferência é uma troca de relatórios |
| Payload SPI de **até 60 bytes** por comando | um quadro do ADS1299 (**27 bytes**) cabe em um comando |
| Taxa SPI até 12 MHz | folgado para 27 bytes por período de amostra |
| GP0–GP8, com contador de interrupção no GP6 | `CS`, `RESET`, `PWDN`, `START` como GPIO; `DRDY` só por contagem |
| **Não entrega interrupção ao host** | não há IRQ real: o *trigger* do IIO será temporizado (§6) |
| Sem FIFO no ADS1299 | os 27 bytes têm de sair antes do próximo `DRDY` — é o teto de taxa deste modo |

### Escopo — e o que fica de fora, deliberadamente

**Dentro**: `probe`/`remove` como HID; registro do `spi_controller`; `transfer_one_message` com
fragmentação para transferências acima de 60 bytes; controle de *chip select* com os atrasos que o
integrado configura; `gpiochip` para `RESET`/`PWDN`/`START`; leitura do contador de interrupção do
GP6 exposta ao driver da §6.

**Fora**: ABI de ioctl ou configfs para userspace; formato de configuração binário próprio;
persistência em NVRAM do integrado; emulação de IRQ genérica. Nada disso é necessário para o
caminho que este plano descreve, e cada um deles é superfície em espaço de kernel.

### Riscos técnicos identificados

- **Concorrência com o `hid-generic`.** O driver precisa vencer a associação genérica; é o problema
  que o README do projeto de terceiro chama de doloroso. Verificar se a tabela de IDs específica
  basta ou se será preciso um *quirk*.
- **Fragmentação com `CS` contínuo.** Uma mensagem SPI maior que 60 bytes vira vários comandos; o
  `chip select` precisa permanecer ativo entre eles. Confirmar no datasheet como o integrado
  expressa isso.
- **Latência por transferência.** Cada troca custa pelo menos um quadro USB (1 ms). O período de
  amostra mínimo utilizável decorre disso, e deve ser **medido**, não estimado.

---

## 6. Driver 2 — `ads1299-iio`

### Ponto de partida: um *diff* sobre o `ti-ads1298.c`

O trabalho não é escrever um driver IIO; é **adaptar um** (§2.1). A sequência:

1. *Backport* do `ti-ads1298.c` de 6.9 para 6.6 — uma release de deriva de API.
2. Adaptação ADS1298 → ADS1299: bits de configuração, faixas de ganho, referência interna de 4,5 V,
   e o que o SBAS499 mostrar de diferente no mapa de registradores.
3. **Acrescentar o canal de carimbo de tempo**, que o driver mainline não declara — é a mudança que
   mais importa para um registro clínico, e a primeira candidata a voltar para o upstream.
4. *Trigger* temporizado para o modo USB, onde não há IRQ (abaixo).

O que segue descreve o **resultado**, que é também o critério para revisar o *diff*.

### Forma

`spi_driver` que registra um `iio_device` com **8 canais de tensão diferencial** mais o canal de
carimbo de tempo. Cada canal declara:

- `scan_type`: 24 bits reais, 32 bits de armazenamento, big-endian, com sinal;
- `IIO_CHAN_INFO_SCALE`: V<sub>REF</sub>/(ganho·2²³) — **22,35 nV** com V<sub>REF</sub> = 4,5 V e
  ganho 24. Exposto em `sysfs` para que **nada** em userspace precise embutir essa constante;
- `IIO_CHAN_INFO_SAMP_FREQ` com `_available` listando o conjunto **discreto** {250, 500, 1k, 2k, 4k,
  8k, 16k} SPS — o conversor não aceita valores contínuos, e é a ABI do IIO que passa a impor isso;
- ganho do PGA {1, 2, 4, 6, 8, 12, 24} como atributo.

### Aquisição

*Buffer* com `iio_triggered_buffer_setup`. A origem do *trigger* é a diferença entre os dois modos:

- **modo hat**: IRQ real do `DRDY`, e o carimbo de tempo é gerado na ISR — é o ganho de verdade do
  IIO;
- **modo USB**: não há IRQ. O *trigger* é temporizado (`iio-trig-hrtimer` ou temporizador interno),
  com leitura do contador de GP6 para detectar perda de amostra. O carimbo é do host.

**Registrar essa assimetria como resultado**, não escondê-la: é a medição que separa as duas
ligações e a justificativa quantitativa para o modo hat existir.

### Autoteste

O conversor traz sinal de teste interno (±1 mV / ±2 mV, ~1 Hz / ~2 Hz), detecção de eletrodo solto e
sensor de temperatura. É com isso que o `MedicalDevice::selfTest()` deixa de ser "o canal abriu" e
passa a ser um teste do caminho analógico — o que a IEC 60601-1 §14 espera de um autoteste.

---

## 7. O framework: um terceiro driver, sem uma terceira dependência

`MedicalDevice.h` já antecipa este ponto de extensão: *"a driver for a directly attached AFE/ADC
(industrial I/O) plugs in the same way"*. O driver novo chama-se **`iio`** e entra na fábrica ao
lado de `simulated` e `rpmsg`.

**Decisão importante: não usar `libiio`.** O framework tem exatamente duas dependências externas
(`libsystemd` e `libcrypto`), e isso é propriedade da tese, não detalhe de build. A ABI do IIO é
alcançável só com POSIX:

- `/dev/iio:deviceN` — leitura do buffer, blocos de tamanho fixo;
- `/sys/bus/iio/devices/iio:deviceN/` — `scale`, `sampling_frequency`, `scan_elements/`, `buffer/`.

`open`, `read`, `poll` e leitura de arquivos de texto. Nenhuma biblioteca nova, e o driver `iio` fica
com a mesma forma dos outros dois: converte o que leu no **mesmo `FrameHeader` de 40 bytes**, com
`scaleNanoUnitsPerLsb` vindo do `scale` do `sysfs` em vez de ser constante no código.

`DeviceConfig` ganha o endereço (`/dev/iio:device0`) e as opções opacas (`ganho`, `ODR`), que a §7.1
do plano do ADS1299 já previa como `driverOptions`.

**Nenhuma linha das aplicações muda.** É o mesmo critério das outras duas ligações.

---

## 8. Integração de build

| Peça                             | Onde           | Como                                                                                 |
| -------------------------------- | -------------- | ------------------------------------------------------------------------------------ |
| Receita do `mcp2210`             | camada adjunta | `inherit module`; `KERNEL_MODULE_AUTOLOAD`                                           |
| Receita do `ads1299-iio`         | camada adjunta | `inherit module`                                                                     |
| Overlay de devicetree (modo hat) | camada adjunta | nó SPI, `cs-gpios`, `DRDY`, atribuição RIF ao M33                                    |
| Regra de udev                    | camada adjunta | permissões de `/dev/iio:device*` por grupo, não `0666`                               |
| Instalação                       | gancho         | `MED_BSP_INSTALL:append:<máquina>` — `:append` para não colidir com o `meta-med-bsp` |
| Seleção do modo                  | KAS            | `MED_EEG_DRIVER = "rpmsg"` ou `"iio"`, como hoje                                     |
| Inclusão da camada               | KAS            | apenas nos arquivos de projeto que a usam                                            |

O fragmento de kernel do MedOS ganha o que a ABI exige e nada específico de *part-number*:
`CONFIG_IIO`, `CONFIG_IIO_BUFFER`, `CONFIG_IIO_TRIGGERED_BUFFER`, `CONFIG_IIO_HRTIMER_TRIGGER` —
política genérica, em `meta-med-distro`, do mesmo modo que `CONFIG_SPI` já está lá.

`CONFIG_HIDRAW` **deixa de ser necessário**: sem ponte em userspace, ninguém abre `/dev/hidraw*`.
Registrar isso é importante porque o plano anterior pedia essa linha.

---

## 9. Critérios de aceitação

1. **Contenção de *part-number***, verificável por comando:
   ```sh
   grep -rn 'ADS1299\|MCP2210' meta-custom/meta-med-{app,framework,distro,bsp}   # → vazio
   ```
2. **Aplicação inalterada**: o mesmo binário de `eeg-acquisition-service` consome dos três drivers
   (`simulated`, `rpmsg`, `iio`), trocando um valor de configuração.
3. **O `scale` vem do kernel**: nenhuma constante de conversão de LSB no código de userspace.
4. **Medições obrigatórias**, nos dois modos: jitter entre amostras, perda de amostra sob carga,
   granularidade do carimbo de tempo, custo de CPU. São elas que fecham a lacuna que a `RESULTS.md`
   §9 lista como "latência e jitter — não medidos".
5. **Autoteste real**: `selfTest()` usando o sinal interno do conversor, com critério numérico.

---

## 10. O que este plano custa ao argumento de particionamento

Precisa estar escrito, porque um avaliador vai perguntar.

Hoje o trabalho argumenta que o item de software Linux pode ser de classe de segurança inferior sob
a IEC 62304 §5.3 **porque a aquisição acontece no Cortex-M33** e o Linux nunca toca o conversor. No
modo USB com IIO, o AFE volta para dentro do kernel Linux, e esse argumento não se aplica àquele
modo.

Duas maneiras honestas de tratar isso, e recomenda-se a segunda:

1. Restringir a afirmação de particionamento ao modo hat, e apresentar o modo USB como configuração
   de desenvolvimento e de bancada.
2. **Apresentar os dois como um resultado comparativo**: a mesma aplicação, sem alteração, sobre três
   origens de dado com propriedades de tempo real diferentes — e usar os números da §9.4 para
   sustentar *por que* a arquitetura de produto é a do coprocessador. Uma decisão de arquitetura
   defendida com medição vale mais que uma defendida com argumento.

---

## 11. Riscos e o que este plano não cobre

| Risco | Efeito | Mitigação |
|---|---|---|
| Conflito com `hid-generic` na associação | o driver não pega o dispositivo | investigar cedo; é o problema que o projeto de terceiro relata |
| Fragmentação SPI acima de 60 bytes com `CS` contínuo | transferências corrompidas | confirmar no DS20005176 antes de escrever a máquina de estados |
| Jitter do modo USB pior que o útil | o modo vira só demonstração | medir cedo; o modo hat não depende disso |
| Escrever a ponte é maior que parece | atraso na Fase 4 | escopo mínimo (§5); avaliar antes as implementações da §2.3; e o modo hat entrega o argumento sozinho |
| *Backport* 6.9 → 6.6 do `ads1298` esconder surpresa | atraso pequeno | é **uma** release de deriva; medir cedo, na etapa 3 da §12 |
| Firmware assinado no M33 (modo hat) | bloqueia a Ligação A | já registrado no plano do ADS1299 |

**Não cobre**: o firmware do Cortex-M33 (plano do ADS1299 §2.1), a cadeia de assinatura desse
firmware, a abstração que escolhe entre hat e USB em tempo de execução — que é problema futuro e
declarado como tal —, e a submissão de qualquer um dos drivers para *upstream*.

---

## 12. Ordem de execução

Encaixe no `PLANO_TCC.md`, sem inflar o cronograma:

| Etapa | Onde entra | Entrega |
|---|---|---|
| 1. Camada adjunta vazia, com duas receitas construindo módulos triviais | Fase 3, início | prova que o caminho de build fecha antes de existir driver |
| 2. **Avaliar as implementações de MCP2210 da §2.3** | Fase 3 | decisão registrada: adaptar uma delas ou escrever |
| 3. ***Backport* do `ti-ads1298.c` 6.9 → 6.6**, construindo e carregando no alvo | Fase 3 | módulo carrega; `/sys/bus/iio/devices/` responde, ainda sem o 1299 |
| 4. `mcp2210` até um `spi_controller` que faz uma transferência conhecida | Fase 3 | `dmesg` mostrando o controlador registrado |
| 5. Adaptação ADS1298 → ADS1299 e o canal de carimbo de tempo | Fase 4 | leitura pontual correta em `in_voltage0_raw`, com `scale` conferido contra o SBAS499 |
| 6. Buffer com *trigger* e os 8 canais | Fase 4 | `iio:device0` entregando blocos |
| 7. Driver `iio` no framework | Fase 4 | a mesma aplicação consumindo do IIO |
| 8. Medições comparativas dos dois modos | Fase 5 | os números da §9.4 |
| 9. *(bônus, fora do caminho crítico)* preparar o suporte a ADS1299 para submissão upstream | pós-Fase 5 | patch enviado à lista do IIO |

Duas escolhas de ordem, ambas deliberadas:

**A etapa 1 é a primeira** porque verifica o *tooling* — `module.bbclass`, camada, gancho, autoload —
enquanto o custo de errar ainda é zero. É a disciplina do resto do repositório: fazer o caminho
falhar cedo, onde falhar é barato.

**A etapa 3 vem antes da 4** porque o *backport* do driver do AFE é o item de **menor** risco e maior
retorno: ele valida a camada adjunta com código real, e o resultado — um driver IIO carregando no
alvo — já é demonstrável mesmo que a ponte atrase. A ponte é o item incerto; ela não deve bloquear
o que não depende dela.
