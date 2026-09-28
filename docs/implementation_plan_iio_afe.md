# Plano de Implementação — O caminho IIO para o modo USB, e a camada adjunta de front-end

> **Status**: **implementado**, exceto o que a §13 lista como aberto. O que existe hoje no
> repositório: a camada adjunta `meta-custom/meta-med-afe-ads1299` com os dois drivers de kernel, o
> driver `iio` no MedFramework, os símbolos de kernel que faltavam no `meta-med-distro`, e a
> seleção de modo por `MED_EEG_LINK`. O que **não** foi executado é tudo que precisa de hardware —
> nenhuma amostra real foi adquirida por nenhum dos dois modos, e as medições da §9.4 continuam em
> aberto. Ver §13 para a separação exata entre "escrito e verificado" e "escrito e não exercitado".
>
> **Substitui a Ligação B** do `implementation_plan_ads1299.md` §2.2, que previa uma ponte em
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
> alvos; não há driver IIO do ADS1299 em `drivers/iio/adc`.
>
> **A conferir antes do texto final, e antes da bancada**: SBAS499 (ADS1299) e DS20005176
> (MCP2210), incluindo VID/PID. Toda constante de datasheet nos dois drivers está marcada no fonte
> com `[SBAS499?]` ou `[DS20005176?]` e agrupada no topo do arquivo, justamente para que conferir
> seja uma passada só. Enquanto essa passada não acontecer, elas são dívida no sentido da regra 1
> da §11 do `BRINGUP_STM32MP2.md`.
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

---

## 13. O que foi implementado, e o que isso mede

Escrito em 2026-09-07, na entrega dos dois modos. A divisão que importa é entre **verificado** e
**escrito e não exercitado**, e ela é rígida: nada abaixo da linha vale como resultado.

### 13.1 O que existe

| Peça | Onde | Estado |
|---|---|---|
| Camada adjunta | `meta-custom/meta-med-afe-ads1299/` | prioridade 7, `LAYERDEPENDS = "core"`, provê e não consome |
| Driver do AFE | `recipes-kernel/ads1299/files/ti-ads1299.c` | derivado do `ti-ads1298.c` 6.9, compila e linka para arm64 |
| Driver da ponte | `recipes-kernel/mcp2210/files/mcp2210-spi.c` | `hid_driver` → `spi_controller` + `gpiochip` + contador do GP6 |
| *Overlay* do modo hat | `dynamic-layers/stm-st-stm32mp/.../stm32mp257f-dk-ads1299-hat.dtso.in` | escrito; **recusa-se a construir** enquanto os pinos não forem declarados |
| Regras de udev | `recipes-core/udev/` | nome estável `/dev/med-afe-eeg0` a partir do `name` que o kernel leu do registrador de ID |
| Driver `iio` | `MedicalDevice.cpp` | terceiro driver da fábrica, sem dependência nova |
| Mensagem de controle | `MedicalDevice.h`, `amp::ControlMessage` | layout fixo, `static_assert`, 464 bytes, cabe num buffer rpmsg |
| `driverOptions` | `DeviceConfig` | opaco; cada driver tem só o vocabulário do seu transporte e recusa o que não honra (§14) |
| Envelope de segurança | `main.cpp` | conjunto discreto de ODR, múltiplo de 8 canais, ganho no conjunto, e a verificação relacional `max_input_uv ≤ VREF/ganho` |
| Símbolos de kernel | `med-kernel-features.cfg` | `IIO_KFIFO_BUF`, `IIO_TRIGGERED_BUFFER`, `SPI`, `GPIOLIB`, `HID`, `USB_HID` |
| Seleção de modo | `MED_EEG_LINK` | `simulated` / `amp` / `spi` / `usb`, nos arquivos KAS que já existiam |

### 13.2 O que foi medido

**O *backport* 6.9 → 6.6 custou zero linhas de API.** O plano orçou "uma release de deriva". Toda
interface que o arquivo mainline usa existe inalterada nos **dois** kernels que este repositório
constrói — `cleanup.h` e o *guard* de `spinlock_irqsave`, `REGCACHE_MAPLE`, os campos
`.reg_read`/`.reg_write` do `regmap_config`, `MICROHZ_PER_HZ`, `get_unaligned_be24`,
`devm_iio_kfifo_buffer_setup`. Verificado por leitura da árvore, e depois pela compilação.

**Os dois módulos compilam e linkam limpos, para arm64, contra o kernel real da placa** —
`linux-stm32mp` 6.6.129, com o *toolchain* do próprio build, `W=1`, zero avisos, `modpost`
resolvendo todos os símbolos. Os aliases saem certos: `hid:b0003g*v000004D8p000000DE` para a ponte,
`spi:ads1299` e `of:N*T*Cti,ads1299` para o AFE.

**A assimetria de configuração de kernel é real e agora é detectada.** Medido antes da correção:

```
linux-stm32mp 6.6.129   ti-ads1299 OK          mcp2210-spi OK
linux-yocto   6.6.144   FALTAM CONFIG_SPI      FALTAM CONFIG_SPI
                        CONFIG_IIO_KFIFO_BUF   CONFIG_GPIOLIB
```

É a forma do defeito do `CONFIG_DM_VERITY` com os papéis trocados: ali o `linux-yocto` escondia a
lacuna e a placa a exporia; aqui a placa a esconde e o QEMU a exporia. O `med-kernel-features.cfg`
passa a declarar os quatro símbolos, e a guarda `do_check_kernel_config` das duas receitas para o
build quando eles faltam — **e essa guarda já viu a falha que procura**, o que, pela regra que a
suíte de aceitação aprendeu com o `acq-active`, é a diferença entre uma verificação e uma alegação.

**A suíte de aceitação passa, 23/23, com o front-end no perfil de simulação.** Não é uma medida de
aquisição — o driver em uso é o `simulated` — mas é o que confirma que o envelope de segurança novo,
o `eeg.conf` novo, o repasse de `driverOptions` e o selo de integridade não regrediram nada do que já
estava validado: os dois slots A/B, o volume LUKS, a trilha de auditoria selada, o sandbox do
serviço e o `SCHED_RR` da aquisição. Chegar a esse número custou dois defeitos, registrados na §13.5.

**O framework compila e o comportamento novo passa em teste funcional no host.** 80 verificações,
zero falhas: os três drivers registrados; o vocabulário de opções aceito e recusado identicamente
nos três (incluindo `afe.gian` como erro, não como silêncio); toda amostra do simulador é múltiplo
exato do LSB do conversor; nenhuma passa do fundo de escala e a saturação foi de fato exercitada; o
gerador de teste entrega V<sub>REF</sub>/1200 pico a pico; 300 SPS é recusado estando dentro da
faixa; 12 canais é recusado; 24 é aceito; o ganho 3 é recusado **pela tabela da aplicação e não pelo
framework**, que é onde a decisão mora; e o ABI da mensagem de controle tem os tamanhos declarados.

O `bitbake -p` fecha nos dois perfis (2837 e 2920 receitas, zero erros), a camada aparece na
prioridade 7, e `MED_EEG_LINK` seleciona o conteúdo certo: `usb` instala os dois módulos e as
regras de udev, `spi` instala só o driver do AFE, `amp` e `simulated` não instalam nada.

### 13.3 O que **não** foi feito, e não deve ser lido como feito

- **Nenhuma amostra real, por nenhum dos dois modos.** Não há hardware. Tudo que a §9.4 pede —
  jitter, perda de amostra sob carga, granularidade do carimbo, custo de CPU — continua em aberto, e
  a `RESULTS.md` §9 continua correta ao listá-lo como não medido.
- **Nenhuma constante de datasheet foi conferida.** Mapa de registradores do ADS1299, opcodes e
  deslocamentos do MCP2210, e o VID/PID. Estão marcados no fonte e agrupados; conferir é
  pré-requisito de bancada, não de código.
- **Os módulos nunca carregaram.** Compilar não é `insmod`. Em particular, a hipótese sobre o
  `hid-generic` está **lida no fonte do kernel** (`hid_generic_match` recusa qualquer dispositivo
  que outro driver registrado case, `hid-generic.c:37-57`) e não observada; o
  `KERNEL_MODULE_AUTOLOAD` existe porque a leitura diz que a corrida é de ordem de carga, e isso
  também é uma hipótese até um `dmesg` dizer o contrário.
- **A imagem não foi construída.** O disco externo que guarda `sstate-cache/` e `downloads/` não
  estava montado; o que rodou foi *parse* completo, que não busca nada. `make qemu`, `make stm32` e
  `make check` continuam devendo.
- **O `DeviceAllow=char-iio` nunca foi exercitado**, e é a primeira linha a olhar se o serviço levar
  `EPERM` ao abrir o conversor.
- **O *overlay* não é aplicado por ninguém.** Ele compila (quando os pinos existem) e é
  implantado; falta um `fdt apply` no `bootcmd`, que é do `meta-med-bsp`, e falta confirmar que o
  `.dtb` da ST carrega `__symbols__`.
- **As outras implementações de MCP2210 da §2.3 continuam não avaliadas.** A etapa 2 da §12 foi
  pulada: escreveu-se o driver. A justificativa é a §2.2 e continua valendo, mas a avaliação era um
  pré-requisito declarado e não aconteceu — registrado aqui em vez de apagado do plano.

### 13.4 Duas decisões que se afastam do plano

**O modo hat ganhou uma segunda topologia, e ela é a que funciona hoje.** A §1 tabela o modo hat
como `rpmsg`, mas a §6 deste mesmo plano descreve o driver IIO com IRQ real de `DRDY` — o que só
existe se o conversor estiver do lado do Linux. As duas coisas são verdadeiras e são topologias
diferentes: `amp` (conversor no M33, caminho de produto, bloqueado por firmware assinado) e `spi`
(conversor no SPI6 do A35, `DRDY` como interrupção de verdade). Por isso `acquisition.link` tem
quatro valores e não três. O ganho prático é de cronograma: com o driver IIO existindo, o modo hat
deixa de depender do bloqueio da §11.1 do plano do ADS1299 para produzir amostras — só o
*argumento* da §5.3 depende.

**`CONFIG_IIO_HRTIMER_TRIGGER` não entrou**, embora a §8 o peça. Ele depende de `IIO_SW_TRIGGER` →
`IIO_CONFIGFS` → `CONFIGFS_FS`, isto é, de um pseudo-sistema de arquivos gravável e de um passo de
userspace para instanciar o *trigger*, num aparelho de rootfs somente-leitura. O driver do AFE
carrega o próprio `hrtimer` quando não há IRQ, então nada faria *bind* no *trigger* avulso. O plano
nomeou o mecanismo antes de o driver existir para dizer que não era necessário.

### 13.5 O defeito que a suíte de aceitação pegou, e que nenhum build pegava

Registrado porque é da mesma família dos seis do caminho A/B, e porque a correção mudou onde a
verificação mora.

Na primeira execução de `make check` depois desta entrega, 6 de 22 asserções falharam. A causa
não era nenhum dos drivers: era o `eeg.conf`. Ao documentar os parâmetros novos, escreveram-se
**comentários no fim da linha do valor**:

```ini
afe.gain = 24                    # PGA: 1, 2, 4, 6, 8, 12, 24
```

`MedicalConfiguration::parse` leva o valor até o fim da linha e só apara espaço — sempre levou, e o
arquivo original nunca tinha usado comentário inline, então a convenção existia e não estava escrita
em lugar nenhum. `afe.gain` virou a string `"24                    # PGA: ..."`, o envelope de
segurança reportou *not a number*, e o serviço recusou subir. Daí saíram `acq-active`, `acq-socket`
e `acq-realtime`; `hmi-active` caiu junto porque a unidade da HMI tem `Requires=weston.service`.

Duas coisas valem a pena separar aqui.

**A plataforma se comportou corretamente.** Um valor de calibração que a plataforma não consegue
interpretar tem de parar o dispositivo, não ser adivinhado — e foi o que aconteceu, com a mensagem
nomeando a chave e mostrando a string inteira. Não é um defeito do parser; é um defeito do arquivo.
Por isso o parser **não** foi alterado: um formato onde `#` dentro do valor é especial quebra
qualquer valor que contenha `#`, e a falha aqui foi alta, não silenciosa.

**Mas ela custou uma imagem inteira e um boot de QEMU para aparecer**, e isso é caro demais para um
erro de digitação. A correção real, portanto, não foi editar o `eeg.conf`: foi mover a verificação
para onde ela custa um segundo. `do_seal_configuration` agora recusa o build se o arquivo
substituído contiver (a) um comentário inline num valor ou (b) um placeholder `@NOME@` sobrevivente.
As duas checagens **já viram a falha que procuram** — a primeira foi injetada de volta de propósito
e o build parou nomeando a linha 46 — o que é o padrão que este repositório exige de uma asserção.

Havia um segundo defeito na mesma entrega, encontrado pela mesma execução: o cabeçalho do
`eeg.conf` explicava a substituição citando o padrão `@MED_EEG_*@` **literalmente**, e a asserção
`config-substituted` procura exatamente `@MED_` no arquivo entregue. Ela estava certa em reprovar:
num arquivo de configuração de dispositivo médico, um `@MED_` num comentário é indistinguível de um
que nunca foi substituído sem alguém ler a linha. O comentário foi reescrito para não soletrar o
padrão, e o guard (b) acima cobre o caso geral.

A regra transferível, e ela é sobre onde a verificação mora, não sobre `ini`: **quando a suíte de
runtime pega um defeito que um build poderia ter pegado, a correção inclui mover a checagem para o
build.** Caso contrário o custo de encontrar aquele defeito continua sendo o mesmo da próxima vez.

### 13.6 O defeito que **nenhuma** verificação pegou, e que a geometria do quadro expôs

Este é o mais sério da entrega, e ele passou por `make check` 22/22.

`samples_per_frame` foi de 25 para 14 porque 14 é o teto de um buffer `rpmsg` padrão para 8 canais
(§7.2 do plano do ADS1299). Isso subiu a taxa de quadro de 10 para 17,857 Hz — **79% a mais** — e
empurrou o laço de aquisição para fora do que o caminho de publicação sustenta. Medido, e a
`RESULTS.md` §3 traz a tabela inteira:

| janela | taxa de quadro | amostras/canal/s | contra os 250 configurados |
|---|---|---|---|
| com a HMI conectada | 6,735 Hz | 94,3 | **−62%** |
| sem a HMI | 17,893 Hz | 250,5 | 0% |

A causa **não** é o volume criptografado, que sustenta 17,9 Hz sem esforço — foi isso que a segunda
janela isolou, e é por isso que ela existe. É a publicação: `MedicalIpcServer::accept` usa
`accept4(..., SOCK_CLOEXEC)` sem `SOCK_NONBLOCK`, e `MedicalIpcChannel::send` faz um `::write()`
bloqueante, então quando o buffer do visualizador enche a aquisição para dentro do `write`.

Três coisas valem ser separadas, porque é fácil tirar a lição errada:

1. **O defeito é anterior a esta entrega.** A 10 Hz a HMI dava conta. A mudança de geometria não o
   criou, revelou — mesma forma do OOM do `QB_MEM`, que também só apareceu quando um defeito
   anterior saiu da frente. Dois casos na mesma sessão é o suficiente para tratar isso como padrão e
   não como coincidência: **corrigir um defeito é um bom momento para procurar o próximo**, porque
   até então ele estava sendo mascarado.
2. **A perda é silenciosa e o registro mente.** Não faltam bytes no arquivo — o resto da divisão por
   488 é zero em três medidas independentes. Faltam *amostras*, enquanto o `metadata.json` declara
   `"sample_rate_hz": 250.000000`. Para IEC 62304 isso é pior que uma falha, porque uma falha é
   visível.
3. **A intenção já estava escrita e não estava implementada.** A unit ignora `SIGPIPE` com o
   comentário "a departing HMI must not kill acquisition". Uma HMI que *sai* não afeta a aquisição;
   uma HMI *lenta* afeta. A declaração de intenção existia, o mecanismo que a garante não.

**Corrigido**, depois de medido e isolado: `SOCK_NONBLOCK` no `accept4`, `EAGAIN` no `send` virando
o novo `Status::WouldBlock` em vez de contrapressão, e o serviço contando os quadros que um
visualizador perdeu no registro de auditoria de fim de sessão. A regra de projeto, dita uma vez:
**um visualizador pode perder dados; o registro não** — que era o que a unit já declarava ao ignorar
`SIGPIPE`, e agora é o que o código faz.

A suíte ganhou a asserção que faltava (`acq-sample-rate`, a 23ª), e ela foi **injetada de falha
antes de ser acreditada**: revertendo a única flag `SOCK_NONBLOCK` e reconstruindo, o resultado é
22/23 com `acq-sample-rate` como única reprovação. As outras 22 seguem verdes na imagem defeituosa,
o que prova nos dois sentidos que a asserção nova era necessária.

Fica registrado o que **não** foi feito e por quê: otimizar o `append` do `MedicalStorage` foi
descartado *por medição*, não por opinião — com a HMI parada o mesmo `append` no mesmo volume LUKS
sustenta 17,893 Hz com perda zero. Continua sendo uma otimização legítima de benefício **não
medido**, e o alvo onde ela pode importar é a placa, onde o custo de I/O nunca foi medido.

---

## 14. Correção (2026-09-16): o framework tinha virado um framework de EEG

**O que estava errado.** A §13 entregou um `MedicalFramework` que passava no `grep` da regra 9 — nenhum
`ADS1299`, nenhum `MCP2210` — e ainda assim só servia a este front-end. O vazamento não foi de nome,
foi de **vocabulário e de constantes**:

- `parseAfeOptions()` conhecia exatamente `afe.gain`, `afe.reference_uv`, `afe.lead_off_detection`,
  `afe.bias_drive` e `afe.test_signal`, rodava nos **três** drivers e recusava qualquer outra chave.
  Um transdutor de pressão, ou o tomógrafo, não conseguia passar uma única opção por nenhum driver.
  O próprio `MedicalDevice.h` dizia que o mapa era opaco e "não tomava posição"; o `.cpp` tomava.
- O `rpmsg` julgava as chaves **antes** de enviá-las, o que tornava o `ControlAck` — feito para o
  produtor recusar o que não entende — código morto para qualquer outro front-end.
- O `simulated` emulava a peça: gerador de `VREF/2400` a `f_CLK/2^21`, LSB de `2^23`, 0,14 µV rms.
- O `selfTest()` do `iio` escrevia `input_mux`/`test_signal` (atributos **privados** do
  `ti-ads1299.c`) e julgava a resposta por `VREF/1200` e 10 µV — critérios de silício no userspace.
  E o `start()` escrevia `hardwaregain` **incondicionalmente**, então um ADC IIO sem ganho
  programável falhava ao iniciar.

A lição transferível: **o `grep` de part number é necessário e não suficiente.** Uma constante de
datasheet ou um vocabulário de classe de dispositivo atravessa a camada sem carregar o nome da peça.

**Como ficou.**

| Onde | O quê |
|---|---|
| `MedicalDevice.cpp` | nenhum vocabulário de classe. `simulated`: `unit`, `full_scale`, `resolution_bits`, `noise_rms`, `tones`, `square`; `rpmsg`: nada, repassa e só confere se cabe na `ControlMessage`; `iio`: nomes de atributo sysfs (sem `/` nem `.`, `sampling_frequency` reservado), escritos e lidos de volta |
| `IioDevice::selfTest()` | identidade e canais com escala — só o que a ABI do IIO sabe dizer. Dispositivo ausente é reportado como possivelmente recusado pelo driver, com a causa no log do kernel |
| `ti-ads1299.c` | `ads1299_self_test()` **no probe**: gerador interno em todas as entradas (amplitude em *códigos*, `ganho·2²³/1200`, independente de V<sub>REF</sub>, ±20%) e entradas em curto (≤ 10 µV p-p). Reprovou, não registra. É a convenção do IIO (`adis_self_test`, `ak8974_selftest`) e **não cria ABI** |
| `eeg.conf` | o bloco `afe.*` continua sendo a prescrição, na língua da aplicação |
| recipe do serviço | `do_derive_device_options` traduz a prescrição para o link de `MED_EEG_LINK` e acrescenta linhas `device.option.*` antes do selo; recusa um `MED_EEG_DRIVER` incoerente com o link |
| `main.cpp` | repassa `device.option.<nome>` sem o prefixo; nenhuma condicional de link |

**O que a correção revelou.** No link `iio`, `afe.bias_drive = true` **nunca foi honrado**: o
`ads1299_init()` escreve `CONFIG3` sem ligar o buffer de bias, e o `IioDevice` antigo lia a opção e a
descartava — a violação exata da regra "opção não honrada é recusada", escondida atrás de um parser
que parecia validar. Agora o build de `spi`/`usb` **falha** com essa prescrição, nomeando o motivo.
Ligar o bias no driver exige decidir `BIAS_SENSP`/`BIAS_SENSN` e verificar `CONFIG3[2]` contra o
datasheet `[SBAS499?]`, e fica em aberto.

**O que foi medido.** Testes de host: 1246 verificações, 0 falhas, incluindo um transdutor de pressão
configurado pelos três drivers sem mudar uma linha. Três defeitos injetados (simulador voltando a
ignorar chave desconhecida, `rpmsg` voltando a julgar chaves, `iio` aceitando `/` no nome do atributo),
os três pegos, fonte restaurado e conferido por sha256. A tradução do recipe foi exercitada fora do
bitbake nos quatro links e nas recusas.

**O que não foi medido.** O self-test de probe **nunca rodou em silício**, e seus critérios herdam a
dívida `[SBAS499?]` inteira — com um agravante: se uma constante estiver errada, o driver recusa o
dispositivo e a bancada perde o acesso ao `in_voltageN_raw` que usaria para descobrir qual. A Fase 0
do `implementation_plan_afe_bench.md` continua bloqueando, agora com mais razão.

---

## 15. Achado (2026-09-27): o perfil `usb` nunca foi construível de ponta a ponta

Descoberto ao tentar provar a integração single-source do driver da ponte
(`implementation_plan_mcp2210.md` §8): com `MED_EEG_LINK = "usb"`, a imagem
**não constrói**, e para antes do kernel, em
`eeg-acquisition-service:do_derive_device_options`:

```
afe.bias_drive = true cannot be honoured on link 'usb': the kernel driver
leaves the bias amplifier powered down and offers no control to enable it.
```

**Isto é a plataforma funcionando, não um defeito dela.** É o `do_derive_device_options`
— o mecanismo do §14 que traduz a prescrição `afe.*` por link — recusando-se a
selar uma configuração que promete ao registro de sessão um *bias drive* que o
link não entrega. Exatamente o que ele existe para fazer, e a mensagem dispensa
investigação.

O que o achado revela é outra coisa: **ninguém tinha tentado**. O perfil de
produto é `amp`, o `make stm32` o constrói, e os links `spi` e `usb` nunca
passaram de um `bitbake -p`. A recusa é pré-existente e independe de qualquer
trabalho da ponte.

### 15.1 A colisão, nos dois sentidos

`eeg.conf` prescreve, nas linhas 53 e 55:

```
afe.lead_off_detection = true
afe.bias_drive = true
```

E `ti-ads1299.c` tem comportamento fixo nos dois, em direções opostas:

| Prescrição | O que o driver faz | Resultado |
| :--- | :--- | :--- |
| `lead_off_detection = true` | liga os comparadores no probe, sem controle para desligar | coincide **por sorte** — a recusa simétrica existe e não dispara |
| `bias_drive = true` | deixa o amplificador de bias desligado, sem controle para ligar | **recusa, e o build para** |

A primeira linha é a mais inquietante das duas: a prescrição e o silício
concordam por acidente, e um `eeg.conf` que pedisse `lead_off_detection = false`
quebraria o build pelo mesmo mecanismo. Nenhuma das duas é uma escolha do
driver; são dois valores de reset que ninguém programou.

### 15.2 As duas resoluções, e a escolha é clínica

**Decidida em 2026-09-28: a opção 1.** O controle de bias foi implementado no
driver (`6d8d5f7eeff9`), e o §15.4 registra o que isso produziu. As duas
alternativas ficam abaixo como estavam, porque a razão de ter escolhido a
primeira é o que a comparação mostra.

1. **Implementar o controle de bias no `ti-ads1299`.** É uma lacuna real do
   driver: o ADS1299 tem o amplificador de bias e os registradores `BIAS_SENSP`/
   `BIAS_SENSN` para roteá-lo, e o driver não os escreve. Cai na Fase 4 do
   `implementation_plan_afe_bench.md` ("o caminho analógico, estático"), que é
   onde o bias seria medido de qualquer forma. Mais trabalho, e resolve a
   prescrição em vez de renegociá-la.

2. **A prescrição passar a ser por link.** O `amp` entrega bias porque o
   firmware do M33 o programa; o `spi` e o `usb` passariam a declarar que não
   entregam. Isso é honesto e é barato — mas é uma afirmação clínica de que uma
   aquisição sem *bias drive* é aceitável nesses links, e ela precisa de quem a
   assine, não de quem a compile.

A opção 1 é a que preserva a propriedade que este plano defende: a mesma
prescrição, a mesma aplicação, e o link como detalhe de transporte. A opção 2
transforma o link numa diferença clínica — o que ele já é para o *timestamp*
(§1), e o custo de estendê-la ao bias é que a lista de "o que difere por link"
deixa de ter um item e passa a ter dois.

### 15.3 O que isso bloqueava

O perfil `usb` (e o `spi`) não produziam imagem. **Não bloqueava** o driver da
ponte, o do conversor, nem a §8: o `bitbake virtual/kernel` constrói os dois
módulos com os símbolos no `.config` final, que é o que aquelas afirmações
precisam. Bloqueava a bancada — a Fase 6 do `afe_bench` ("o framework, a
aplicação e a suíte") precisa de uma imagem para rodar. **Desbloqueado**: ver
o §15.4.

### 15.4 Como ficou (2026-09-28)

O amplificador de bias tem agora três configurações, e elas são **nomeadas**,
porque um booleano não as distingue e elas são clinicamente diferentes:

| Valor | O que o amplificador faz |
| :--- | :--- |
| `off` | desligado. Estado de reset, e o que este driver fazia. |
| `reference` | dirige BIASREF, gerado internamente como `(AVDD + AVSS) / 2`. Um bias DC para o sujeito e **nenhuma** rejeição ativa de modo comum: nada do que é medido realimenta. |
| `derived` | dirige o inverso da média de todos os canais habilitados — o arranjo de eletrodo dirigido que de fato rejeita modo comum. |

**A tradução clínica ficou na receita, não no driver.** `afe.bias_drive = true`
vira `derived`, e o raciocínio está escrito em `do_derive_device_options`: quem
prescreve *bias drive* está pedindo a rejeição de modo comum, não um mid-supply
fixo. Se uma montagem quiser `reference`, isso precisa de uma prescrição capaz
de dizê-lo — não de um default diferente no driver. Essa separação é a mesma que
o §14 estabeleceu: o driver possui o vocabulário do seu transporte, a aplicação
prescreve, e a tradução é explícita e por link.

**Detalhes de implementação que valem saber antes de mexer.** A máscara de
`BIAS_SENSP`/`BIAS_SENSN` vem de `num_adc_channels`, que vem do registrador ID,
então os bits que uma variante não tem (`[5:4]` no -4, `[7:6]` no -4 e no -6)
saem 0 sem tabela de variante — que é o que o datasheet pede. A **ordem de
escrita** é carregada: `PD_BIAS` vai por último ao ligar e primeiro ao desligar,
para que os registradores de derivação nunca descrevam algo diferente do que o
amplificador está dirigindo. E a escrita é **recusada durante aquisição**
(`iio_device_claim_direct_mode`): mudar o que é injetado no sujeito no meio de
uma gravação poria duas configurações num registro sem nada dizendo onde é a
fronteira.

#### 15.4.1 Dois defeitos encontrados no caminho

**`BIASREF_INT` estava acoplado à referência do ADC.** O probe escrevia
`PWR_REFBUF | BIASREF_INT` juntos, condicionados a existir um regulador de VREF
externo. São referências diferentes — o bit 7 é o buffer de referência do
*conversor*, o bit 3 é a referência do *amplificador de bias* — e um VREF
externo não diz nada sobre como BIASREF é alimentado. Com o amplificador
desligado o bit não fazia efeito, então **nada estava errado em nenhuma placa**;
passaria a estar no momento em que o bias fosse ligado. Agora viaja com a
configuração de bias.

**Um comentário no lugar errado, citando o registrador errado.** O bloco acima
de `BIAS_SENSP` citava `SBAS499 Tables 20, 21 / Figures 58, 59`, que são as
tabelas de `LOFF_SENSP`/`LOFF_SENSN`. O comentário *descrevia* os registradores
de lead-off e estava fisicamente acima dos de bias — foi assim que os números
errados entraram. Movido para os registradores que ele descreve; os de bias
ganharam o próprio, com Tabelas 18, 19 e Figuras 56, 57, conferidas no
documento.

O commit que introduziu aquelas citações chamava-se *"cite the datasheet tables
that were checked"*. A lição é estreita e vale: **uma citação só é evidência
depois de a própria citação ser conferida** — e um comentário que migra de lugar
leva as citações dele para um registrador que não é o seu.

#### 15.4.2 O que isso NÃO mediu

Nenhum amplificador foi energizado, nenhuma corrente foi injetada num eletrodo,
e nenhum eletrodo mediu nada. O que está verificado é que compila limpo para
arm64 com `W=1`, que o `checkpatch --strict` reporta as mesmas quatro
observações pré-existentes, e que o perfil `usb` agora **produz uma imagem**
(5615 tarefas, todas com sucesso, `.wic` de 2.761.966.592 bytes) cujo `eeg.conf`
entregue carrega `device.option.bias_drive = derived` ao lado do `.sha256`. A
validação clínica é a Fase 4 do `implementation_plan_afe_bench.md`.

**E a recusa simétrica continua de pé.** O driver liga os comparadores de
lead-off no probe e não oferece como desligá-los, então
`afe.lead_off_detection = false` ainda quebraria o build pelo mesmo mecanismo.
Hoje não dispara porque a prescrição diz `true` — com o qual o silício concorda
**por sorte**, não por desenho (§15.1). Essa metade da colisão não foi
resolvida.

