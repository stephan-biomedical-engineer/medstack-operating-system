# Plano de implementação — o driver ADS1299 como release no kernel Linux

> **Estado**: **nada submetido**, e nada aqui foi executado. O que já existe medido é o ponto de
> partida da §1, e ele é melhor do que a média de uma primeira submissão.
>
> **Objetivo**: `drivers/iio/adc/ti-ads1299.c` aceito na árvore principal, com o *binding* YAML e a
> documentação de ABI que a aceitação exige.
>
> **A verdade sobre o calendário, dita primeiro**: uma submissão pode ser feita antes de dezembro;
> uma **aceitação não pode ser agendada**. Um driver de IIO passa tipicamente por 2 a 5 revisões ao
> longo de semanas a meses, no ritmo dos mantenedores e não no nosso. Este plano é escrito para que a
> *submissão* seja um resultado defensável do TCC por si mesma — ver §12, que diz exatamente o que se
> pode afirmar em cada estado.
>
> **Leitura obrigatória antes**: `implementation_plan_iio_afe.md` §3 (a proveniência, que é o que
> torna isto possível) e §13 (a linha entre verificado e escrito), e
> `implementation_plan_afe_bench.md` §4 (a dívida de datasheet, que aqui deixa de ser dívida e passa
> a ser bloqueio).

---

## 1. Onde estamos, medido

`checkpatch.pl --strict --file`, com o `scripts/` do próprio kernel da placa:

```
total: 0 errors, 3 warnings, 2 checks, 1718 lines checked
```

Os cinco achados, um por um, porque nenhum deles é trabalho de verdade:

| Linha | Achado | O que é |
|---|---|---|
| 251 | `CamelCase: ADS1299_REG_CHnSET` | nome do datasheet; o irmão mainline usa o mesmo, e a citação dele é a resposta |
| 414 | `externs should be avoided in .c files` + falta linha em branco | falso positivo: o `checkpatch` lê mal a struct anônima com `s64 timestamp __aligned(8)` |
| 428 | `Macro argument reuse 'index'` | idioma herdado, e o argumento aparece uma vez |
| 913 | `Block comments use a trailing */ on a separate line` | comentário de banner; conserto de uma linha |

Além disso, já estão no lugar: `SPDX-License-Identifier: GPL-2.0`, o *copyright* do autor original
preservado com a atribuição da derivação, `IIO_DMA_MINALIGN` no buffer de SPI, `__aligned(8)` no canal de timestamp, `get_unaligned_be24` para as amostras, `MODULE_LICENSE`/`AUTHOR`/`DESCRIPTION`, e compilação limpa com `W=1` para arm64.

**A distância até um `v1` submetível não é higiene de código.** É o §3, o §5 e o §6.

---

## 2. O ponto de partida obriga a um crédito, e ele é vantagem

Este arquivo é derivado de `drivers/iio/adc/ti-ads1298.c` (Mike Looijmans / Topic Embedded, mainline
desde 6.9). Três consequências práticas:

1. **A estrutura já é idioma mainline** — `regmap` com `.reg_read`/`.reg_write`, `REGCACHE_MAPLE`,  `devm_iio_kfifo_buffer_setup`, `dev_err_probe`. Metade do que uma revisão normalmente exige reescrever já está no formato certo porque o original passou por essa revisão.
2. **O *copyright* do autor original tem de permanecer**, e permanece. É obra derivada de arquivo
   GPL-2.0; apagar a linha seria fatal para a aceitação, além de errado.
3. **O revisor vai fazer o `diff` com o irmão** e perguntar o motivo de cada divergência. Isso é
   bom: as divergências reais estão documentadas no cabeçalho do arquivo (tabela de ganhos, VREF de 4,5 V, ausência do bit HR/LP, reset de `CHnSET`, layout do registrador de ID) e cada uma tem justificativa de datasheet. **O que não estiver justificado será pedido de volta.**

### 2.1 Três objetivos independentes, e o critério é um `grep`

Decisão de arquitetura de 2026-09-26, e ela governa o resto deste plano:

| #   | Objetivo                                            | Conhece                                 |
| --- | --------------------------------------------------- | --------------------------------------- |
| 1   | `ti-ads1299`: driver IIO completo                   | **só a interface SPI padrão do kernel** |
| 2   | `mcp2210`: `spi_controller` sobre USB/HID, completo | **nada sobre o ADS1299**                |
| 3   | Camada de integração da plataforma                  | os dois, e é o único lugar que pode     |

```
ADS1299 ──► ti-ads1299 (IIO) ──► framework SPI ──► mcp2210 (spi_controller) ──► USB/HID
                    └──────────── nenhum header, nenhuma chamada, entre os dois ───────┘
```

A integração acontece pelo **modelo de dispositivos do kernel** e por nada mais. O ganho não é
estético: cada driver pode ser testado e submetido por conta própria, e nenhuma solução específica desta placa vira dependência de um driver upstream.

**E o critério é contável**, no mesmo espírito da regra 9 do `CLAUDE.md`:

```sh
grep -ci 'mcp2210\|usb\|bridge' drivers/iio/adc/ti-ads1299.c   # tem de ser 0
grep -ci 'ads1299'               drivers/spi/mcp2210-spi.c      # tem de ser 0
```

**Estado medido hoje, e os dois lados falham** — por motivos de peso muito diferente:

| Onde                | O que                                                                                                                 | Gravidade                                                                                                                                 |
| ------------------- | --------------------------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------- |
| `ti-ads1299.c`      | **6 comentários** justificam decisões em termos do MCP2210 e da ligação USB (linhas 46, 397-398, 617, 1158, 1606)     | nenhuma dependência de código, mas o *raciocínio* está amarrado a uma ponte específica — e comentário entra nessa disciplina de propósito |
| `mcp2210-spi.c:149` | `static char *spi_device = "ads1299";` — **a ponte instancia um dispositivo SPI por nome, e o nome é o do conversor** | **dependência real, em código**                                                                                                           |

O segundo é o que importa. Um `spi_controller` upstream não instancia um chip específico: ele
registra o barramento e alguém mais diz o que está nele. Para uma ponte USB isso é um problema
conhecido e sem DT node óbvio, e a resposta é o **objetivo 3** — a instanciação sai da ponte e vai
para a cola de plataforma (nó de software / `spi_board_info` / instanciação por userspace). Enquanto o parâmetro de módulo estiver na ponte, os objetivos 1 e 2 não são independentes, apenas parecem.

O primeiro é trabalho de redação: os seis comentários passam a falar de *"uma plataforma que não
pode entregar o dado-pronto como interrupção"*, que é a formulação genérica e verdadeira — e é
exatamente o que a §5.2 passou a tratar.

### 2.2 Onde o código mora: a árvore do kernel é a fonte de verdade

Decisão de 2026-09-26, e ela substitui o que este plano dizia antes (uma branch neste repositório com
um `.c` paralelo e um script de sincronia — duas fontes de verdade disfarçadas de uma).

```text
Linux (fork/clone, branch de desenvolvimento)     ← FONTE DE VERDADE do código
    drivers/iio/adc/ti-ads1299.c
    drivers/spi/mcp2210-spi.c
    Kconfig, Makefile, bindings, Documentation/ABI
    │
    ├── SRC_URI/SRCREV  ──►  o Yocto, durante todo o desenvolvimento
    └── git format-patch ──►  a série, só no congelamento para upstream

Yocto (este repositório)                          ← INTEGRAÇÃO e construção
    meta-med-afe-ads1299/   overlay, udev, e a integração de plataforma (objetivo 3)
    recipes-kernel/linux/   consome uma REVISÃO FIXA do fork
    med-kernel-features.cfg CONFIG_TI_ADS1299, CONFIG_SPI_MCP2210

Este repositório, docs/                           ← planejamento, resultados, TCC
```

**O que isso apaga da camada adjunta**: as duas receitas de módulo *out-of-tree*
(`ti-ads1299_1.0.bb`, `mcp2210-spi_1.0.bb`, `med-afe-module.inc`, os dois `Makefile`) deixam de
existir, e com elas o `inherit module`. A camada fica com o *overlay*, a regra de udev e a integração
de plataforma — o que **encolhe** a camada adjunta e melhora a métrica de reuso em vez de diluí-la.

As guardas `do_check_kernel_config` mudam de sentido e continuam necessárias: hoje verificam se o
kernel tem os símbolos de que um módulo externo precisa; passam a verificar se **o nosso driver está
de fato no kernel que está sendo construído** — um `CONFIG_TI_ADS1299` ausente produziria uma imagem
sem front-end e nenhum erro.

#### Uma base, duas árvores — e é um portão, não um detalhe

A submissão é contra a **mainline**; a placa roda **`linux-stm32mp` 6.6**. Um arquivo, duas bases.
Isso não é novo e já foi medido (o *backport* 6.9→6.6 custou zero linhas de API), mas passa a ser
**verificação contínua**: compilar limpo nas duas árvores é critério de cada fase, porque o dia em
que divergirem é o dia em que "o código que testo é o código que submeto" deixa de ser verdade.

#### Como o Yocto consome — decidido

Decisão de 2026-09-26. **Uma estratégia só**, para que não haja duas fontes de verdade nem em
desenvolvimento nem no congelamento:

| Momento | Mecanismo |
|---|---|
| **Durante o desenvolvimento** | o Yocto consome **diretamente uma revisão fixa** do fork/branch do Linux, por `SRC_URI`/`SRCREV` |
| **Build reproduzível** | o mesmo mecanismo: `SRCREV` **fixo no commit validado**. Reprodutibilidade vem do commit ser fixo, não de haver patch |
| **Upstream / congelamento** | a **mesma história de commits** vira série com `git format-patch` |

**Patches não são mantidos como segunda fonte de verdade durante o desenvolvimento.** O projeto não
paga um passo de regeneração a cada mudança de driver, e a série de upstream continua sendo *derivada*
da mesma história que foi testada na placa — não uma tradução paralela dela.

Duas consequências que vale ter escritas, porque são o preço desta escolha:

- **Você passa a ser dono da árvore do kernel da imagem.** Apontar `SRC_URI` para o fork substitui a
  aquisição da ST, e com isso vem a responsabilidade de rebasar sobre as atualizações dela
  (`r3.2`, `r4.0`). É trabalho conhecido e periódico, não surpresa.
- **`${AUTOREV}` não entra em imagem.** Ele busca a ponta da branch a cada build e é cômodo no laço;
  qualquer imagem que vá para um cartão, ou que produza número citável, sai de `SRCREV` fixo. Um
  resultado medido sobre `AUTOREV` não é reproduzível e portanto não é resultado.

### 2.3 A base, validada — e a referência que a migração tem de bater

Levantado e medido em 2026-09-26, **antes** de existir clone, branch ou bbappend. Esta subseção é o
portão do passo 4 da sequência acordada:

> 1. validar a base exata do `linux-stm32mp` 6.6 que o projeto já usa; 2. criar o clone/fork dessa
> base; 3. criar a branch de desenvolvimento dos dois drivers; 4. **confirmar que a árvore Git
> corresponde à árvore atualmente consumida pelo Yocto**; 5. só então migrar os drivers e ajustar o
> consumo pelo Yocto.

#### A base exata

| Item | Valor | Onde |
|---|---|---|
| Versão | **6.6.129** | `LINUX_VERSION = "6.6"`, `LINUX_SUBVERSION = ".129"` |
| Release da ST | **`r3.1`** | `LINUX_TARGET = "stm32mp"`, `LINUX_RELEASE = "r3.1"` |
| `PV` efetivo | **`6.6.129-stm32mp-r3.1`** | `linux-stm32mp_6.6.bb:28` |
| Tarball | `linux-6.6.129.tar.xz`, sha256 `caa08f0122224fbbfab177e2a37cc2a94a0046bd2e7e87f03f8913f2b812448a` | `SRC_URI` + `SRC_URI[kernel.sha256sum]` |
| Patch da ST | `0001-v6.6-stm32mp-r3.1.patch`, sha256 `5cb306abf84baf76ae1d1b1d8581ec66b2ab6297c06634b3871c355ffba95d6c` | `recipes-kernel/linux/linux-stm32mp/6.6/6.6.129/` |
| Tamanho do delta da ST | 4,9 MB, **162.253 linhas, 749 arquivos** | `grep -c '^diff --git'` no patch |
| `S` | `${WORKDIR}/linux-6.6.129` | `linux-stm32mp_6.6.bb:35` |
| **Ponto Git equivalente** | `github.com/STMicroelectronics/linux`, branch `v6.6-stm32mp`, **`548f960c059bc4b9165cb69895fd67551f6061ca`** | `SRC_URI:class-devupstream` / `SRCREV:class-devupstream` |
| Tag que a receita indica | `v6.6-stm32mp-r3.1` | `ARCHIVER_ST_REVISION` |

Verificações já feitas, com o comando:

```sh
sha256sum downloads/linux-6.6.129.tar.xz      # confere com SRC_URI[kernel.sha256sum]
grep -c '^diff --git' .../0001-v6.6-stm32mp-r3.1.patch   # 749
```

O tarball está em `downloads/` e **seu sha256 confere byte a byte** com o que a receita declara. A
aquisição padrão é, portanto, *tarball + um patch*, e **não** uma árvore Git: a rota Git existe
porque a ST publica `BBCLASSEXTEND = "devupstream:target"`, e é ela que torna este plano possível sem
reconstruir uma história a partir de um patch.

**A conferir no clone, e é pré-requisito do passo 4**: que a tag `v6.6-stm32mp-r3.1` aponte para
`548f960c…`. Se não apontar, a rota Git e o tarball+patch não são a mesma árvore, e isso precisa de
nome antes de qualquer migração.

#### A referência: o digest da árvore que o Yocto consome hoje

```
árvore:     build/tmp-glibc/work-shared/stm32mp25-disco/kernel-source
ferramenta: scripts/med-kernel-fingerprint.sh
arquivos:   81889
digest:     35e19311ab288cf56345ea6396d47d0cc071dab2dfe7ed028c575f2202b3bf50
```

**Rastro do número, porque ele mudou uma vez e o motivo importa mais que o valor.** A primeira
medição desta árvore deu `81890` arquivos e digest
`b2558f189a16f0d6c40806ad69900885038c5fd5a7098f8c07d3a72887c0f043`. Ela **precede a exclusão de
`patches/`**, descoberta ao executar a comparação — o diretório de trabalho do quilt, que um checkout
Git jamais tem. O valor acima é o que vale; o anterior fica registrado para que a mudança de um
número medido tenha causa escrita, e não seja substituída em silêncio.

**Este digest é a referência da árvore atualmente consumida pelo Yocto** — tarball 6.6.129 mais o
patch `r3.1` aplicado, como o build a desempacota. Ele não é uma afirmação sobre a árvore Git: a
equivalência entre as duas **tem de ser demonstrada** antes da migração, e a demonstração é o digest
do checkout git bater com este. Enquanto não bater, a migração não começa.

Determinismo verificado por dupla execução, com manifestos idênticos (`cmp`). 1,5 GB em ~2 s.

#### O critério de comparação, e por que ele é de conteúdo

A ferramenta compara **conteúdo**, nunca metadados: modo de arquivo, mtime e diretórios vazios
divergem legitimamente entre um tarball desempacotado e um checkout Git, e **nenhum deles muda o
kernel compilado**. O manifesto é uma lista ordenada de `(sha256, caminho relativo)`, e o digest é o
hash sobre ela.

O manifesto é o artefato que importa: quando o digest não bate, o `diff` entre dois manifestos
**nomeia os arquivos**, que é a diferença entre "as árvores divergem" e "as árvores divergem nestes
três arquivos".

**As exclusões, e cada uma tem motivo.** A primeira versão desta ferramenta contava 82.660 arquivos e
**não podia passar**, porque a árvore que o Yocto desempacota carrega 770 arquivos que um checkout
Git jamais terá:

| Excluído | Por quê |
|---|---|
| `.git/` | o repositório não é conteúdo do kernel |
| `.pc/` | **768 arquivos**: os backups pré-patch do quilt, que é como o Yocto aplica o patch da ST — uma cópia de cada arquivo tocado |
| `patches/` (só na raiz) | o diretório de trabalho do quilt: o symlink para o patch da ST e o arquivo `series`. Restringido à raiz de propósito — um `patches/` aninhado poderia ser conteúdo de verdade |
| `.scmversion` | escrito pela classe `kernel` do Yocto |
| `.checkpatch-camelcase*` | cache que o `checkpatch` escreve ao ser rodado de dentro da árvore |
| `.config`, `.config.old`, `Module.symvers`, `include/generated/`, `include/config/` | artefatos de build: ausentes numa árvore `work-shared`, presentes se a ferramenta for apontada para uma árvore construída — e aí a exclusão é o que salva a comparação |

Sem elas a comparação acusaria 770 divergências onde não há nenhuma. É o mesmo defeito que este
repositório já catalogou em outra forma: **uma verificação que não pode passar é pior que nenhuma**,
porque produz alarme falso e consome a atenção que o alarme verdadeiro precisaria.

#### O passo 4, executado — 2026-09-26

| Verificação | Resultado |
|---|---|
| `git tag --points-at 548f960c059bc4b9165cb69895fd67551f6061ca` | **`v6.6-stm32mp-r3.1`** — a tag aponta para o commit que a receita fixa |
| Arquivos, árvore do Yocto | 81889 |
| Arquivos, checkout Git em `548f960c…` | 81889 |
| Arquivos comuns efetivamente comparados | **81889** |
| Dos quais com hash divergente | **0** |
| Única diferença antes da exclusão de `patches/` | `./patches/series`, 33 bytes, artefato do quilt |
| Digest das duas árvores | **`35e19311ab288cf56345ea6396d47d0cc071dab2dfe7ed028c575f2202b3bf50`** |
| `cmp` dos dois manifestos | idênticos, arquivo por arquivo |
| Branch de desenvolvimento | **`feature/ads1299-mcp2210-driver`**, criada a partir de `548f960c…` |
| Digest da branch, antes do primeiro commit | `35e19311…` — a branch parte da árvore provada, não da ponta da branch da ST |

**A equivalência está demonstrada**: o tarball 6.6.129 mais o patch `r3.1`, como o Yocto os
desempacota, e o checkout Git em `548f960c…` são a **mesma árvore em conteúdo**. O portão do passo 4
está aberto, e a árvore Git pode ser adotada como fonte de verdade sem que isso mude uma linha do
kernel que a placa já executou.

A contagem de arquivos comuns foi conferida explicitamente (81889, e não zero) antes de aceitar
"nenhum hash divergente" como resultado — uma comparação que não compara nada também devolve
"nenhuma diferença".

**Nota de método**: as duas exclusões que esta ferramenta precisou — `.pc/` e `patches/` — foram
descobertas **executando-a**, não raciocinando sobre ela. Nos dois casos o sintoma seria o mesmo:
divergência acusada onde não havia nenhuma, isto é, uma verificação incapaz de passar.

#### O que pode divergir sem ser problema, e o que não pode

| Divergência | Veredito |
|---|---|
| Arquivos de CI ou de projeto que a ST mantenha no Git e o tarball do kernel.org não tenha, confinados à raiz e a dotfiles | aceitável; registrar quais |
| Qualquer arquivo em `arch/`, `drivers/`, `include/`, `kernel/` | **inaceitável**: são árvores diferentes, e a migração para |

#### Uma consequência para o passo 5

O patch da ST **toca `drivers/iio/adc/Kconfig` e `drivers/iio/adc/Makefile`** (20 arquivos sob
`drivers/iio` e `drivers/spi` no total) — exatamente os dois arquivos que os nossos commits também
alteram para registrar `TI_ADS1299` e `SPI_MCP2210`. Consequência prática: esses dois *hunks* são a
parte da futura série com mais chance de precisar de ajuste entre as duas bases, porque no 6.6 da ST
eles já estão modificados e na mainline não. Os `.c` dos drivers, sendo arquivos novos, atravessam
sem atrito.

#### Pendência explícita: `S:class-devupstream`

A variante `class-devupstream` da ST ajusta `SRC_URI`, `SRCREV`, `PV` e reacrescenta os fragmentos de
configuração, e **aparentemente não ajusta `S`**, que continua `${WORKDIR}/linux-6.6.129` enquanto um
fetch Git desempacota em `${WORKDIR}/git` (sem `destsuffix`).

Se isso se confirmar, a correção é uma linha no nosso bbappend:

```bitbake
S:class-devupstream = "${WORKDIR}/git"
```

**Isto é leitura da receita e não observação de build, e fica como pendência a validar — não como
fato.** A falha esperada é alta (o `do_configure` não encontra a árvore), então é barato descobrir;
até o build dizer, a linha acima é hipótese.

---

---

## 3. A tensão entre as duas diretrizes — e ela se resolve

Há duas decisões de projeto em vigor que, de fora, parecem colidir:

- *"Toda funcionalidade presente no datasheet precisa ser desenvolvida no driver"* (2026-09-26);
- *"o driver tem de entrar como release no kernel"* (este plano).

Colidem porque **os mantenedores do IIO resistem a ABI privada de driver**. A resposta padrão a um
atributo novo é "use a ABI existente, ou proponha uma extensão *genérica* do IIO" — e um driver que
chega com vinte arquivos sysfs inventados não é aceito, é reescrito.

**A resolução é a regra do mecanismo padrão primeiro**, que é exatamente o mapeamento já proposto em
2026-09-26. As duas diretrizes são compatíveis *se* cada funcionalidade for exposta pelo mecanismo
que o kernel já tem para ela:

| Funcionalidade | Registrador | Mecanismo | Aceitabilidade upstream |
|---|---|---|---|
| Ganho | CHnSET 6:4 | `hardwaregain` | **padrão** |
| Taxa | CONFIG1 DR | `sampling_frequency` | **padrão** |
| Sensor de temperatura | MUX=100 | **canal `IIO_TEMP`** | **padrão** |
| Medida de MVDD | MUX=011 | canal de tensão de alimentação | **padrão** |
| Lead-off (status e habilitação) | LOFF_SENS*/LOFF_STAT* | **eventos IIO** por canal | **padrão**, e melhor: com carimbo e *poll* |
| Bias status | CONFIG3 BIAS_STAT | evento IIO | **padrão** |
| GPIO (4 pinos) | 0x14 | **gpiochip** | **padrão** |
| Power-down de canal | CHnSET bit 7 | ver §5.5 — conflita com `available_scan_masks` | a decidir |
| SRB1, SRB2 | MISC1 bit 5, CHnSET bit 3 | atributo (global e por canal) | **customizado, defensável**: não há ABI de montagem de eletrodo |
| Corrente/frequência de lead-off | LOFF 3:2, 1:0 | atributo | customizado, defensável |
| MUX de entrada | CHnSET 2:0 | `input_mux` por canal | customizado, já existe no arquivo |
| Sinal de teste | CONFIG2 | `test_signal` | customizado, já existe |
| Daisy-chain, clock out | CONFIG1 6, 5 | ver §5.6 | a decidir |
| MISC2 | 0x16 | reservado: escrever 0 | nada a expor |

O saldo: a maior parte da diretriz de completude é atendida por **ABI padrão**, e o que sobra de
customizado é pequeno, coerente e justificável — montagem de eletrodo e parâmetros de lead-off, para
os quais o IIO realmente não tem vocabulário.

**E há um ganho colateral que vale dizer**: cada atributo customizado é uma palavra que a tabela de
tradução do `do_derive_device_options` precisa conhecer. Mecanismo padrão é acoplamento que não
existe. Upstreamabilidade e a métrica de reuso deste trabalho pedem a mesma coisa.

---

## 4. Os artefatos obrigatórios

Uma submissão é uma **série** de patches, nesta ordem (o *binding* antes do driver, sempre):

| # | Artefato | Onde | Porta mecânica |
|---|---|---|---|
| 1 | *Binding* | `Documentation/devicetree/bindings/iio/adc/ti,ads1299.yaml` | `make dt_binding_check DT_SCHEMA_FILES=...` |
| 2 | Driver | `drivers/iio/adc/ti-ads1299.c` + entrada em `Kconfig` + linha no `Makefile` | `checkpatch --strict`, `sparse`, `smatch`, `W=1` |
| 3 | ABI | `Documentation/ABI/testing/sysfs-bus-iio-ads1299` | só para o que sobrou customizado no §3 |
| 4 | `MAINTAINERS` | entrada para os três arquivos | `get_maintainer.pl` passa a apontar para você |

O *binding* descreve **fiação**, não configuração — e essa distinção é onde primeiras submissões
morrem. Vão para o DT: `compatible` (`ti,ads1299`, `-6`, `-4`), `reg`, `spi-max-frequency`,
`interrupts` (DRDY), `reset-gpios`, `avdd-supply`, `vref-supply` opcional, `clocks`/`clock-names`.
**Não** vão para o DT: ganho, taxa, corrente de lead-off, SRB1 — são configuração, e configuração
mora no sysfs. Um `ti,lead-off-current-na` no *binding* é pedido de reescrita garantido.

---

## 5. As objeções previsíveis, e a resposta de cada uma

Escritas antes de submeter, porque cada uma que chegar sem resposta pronta custa uma rodada de
semanas.

### 5.1 "O autoteste recusa o *probe*"

`ads1299_self_test()` roda no *probe* e, falhando, não registra o dispositivo.
**Resposta**: há precedente mainline — `adis_self_test()` em `drivers/iio/imu/adis.c` e
`ak8974_selftest()` no magnetômetro fazem exatamente isso. E o argumento de domínio é forte: um
front-end de biopotencial que não reproduz o próprio sinal de teste não deve aparecer como
dispositivo utilizável.
**Compromisso preparado, se houver insistência**: manter o teste e degradar a falha a `dev_warn` com
um atributo que reporta o resultado. **Não** oferecer parâmetro de módulo — é o que se pede para
não oferecer.

### 5.2 Como um driver IIO suporta uma plataforma sem IRQ de dado-pronto

**Pergunta genérica, e ela ainda não tem resposta fechada.** A formulação anterior deste plano
amarrava o assunto a um chip específico ("o ramo de `hrtimer` não tem usuário in-tree porque a nossa
ponte não está upstream"); e a versão seguinte errou na direção oposta, concluindo que
`SINGLE_SHOT` + trigger IIO resolvia. **Não resolve, e a lacuna é precisa:**

> Um trigger diz **quando começar**. Sem DRDY, o driver ainda precisa saber **quando a conversão
> terminou** antes de emitir o `RDATA`. O trigger não responde isso.

E um `RDATA` emitido cedo devolve a conversão anterior — indistinguível de uma amostra nova de valor
igual. É a duplicata silenciosa, que é pior que a amostra perdida, porque a perdida é contável.

#### A máquina de DRDY não está em questão

Com `spi->irq`, o modelo é o do irmão mainline e **já está implementado aqui**: `spi_async` na borda,
uma contagem de transferências pendentes (`rdata_xfer_busy`) e `lost_samples` incrementado quando uma
borda chega com transferência em voo. Verificado no arquivo. Nada disso muda; o que se abstrai é
apenas a **fonte do disparo**.

#### Três capacidades de plataforma, não duas

A taxonomia honesta tem um degrau no meio que as versões anteriores deste plano não tinham:

| Capacidade da plataforma | Fim de conversão | Carimbo | Fidelidade |
|---|---|---|---|
| **DRDY como IRQ** | a borda **é** o fim | na borda | melhor; é o modelo já implementado |
| **DRDY legível ou contável, sem IRQ** (nível de GPIO, contador de bordas) | conhecível, por leitura | lado do host | intermediária; perda é **detectável** |
| **DRDY indisponível** | apenas **presumido** por tempo decorrido | lado do host | pior; perda e duplicata são indistinguíveis de dados |

O degrau do meio é genérico — "a plataforma expõe o sinal como entrada legível e não como
interrupção" — e não menciona ponte nenhuma. É o degrau em que a perda deixa de ser invisível.

#### Portanto: hipótese de arquitetura, a validar

`SINGLE_SHOT` + trigger IIO fica registrado como **hipótese**, com a pergunta de sincronização
aberta. As candidatas para o fim de conversão, na terceira capacidade:

1. **espera de pior caso** pelo tempo de conversão da DR configurada, depois `RDATA` — determinística,
   custa latência, e depende de uma constante de datasheet (dívida);
2. **leitura do sinal** quando a plataforma a oferece (capacidade do meio);
3. **não oferecer captura em buffer sem DRDY**, só leitura direta — a posição mais limpa upstream e a
   menos útil.

#### O que precisa ser medido antes de congelar

Nenhuma das três se decide no papel. As medidas, por ligação e **por taxa** do ADS1299:

| Medida | Por que decide |
|---|---|
| tempo de conversão real por DR | valida (ou derruba) a constante da candidata 1 |
| latência disparo → amostra | é o custo da candidata escolhida |
| jitter entre amostras | separa "funciona" de "funciona com fidelidade" |
| **perda e duplicação**, contadas separadamente | é o critério; duplicata silenciosa reprova a candidata |
| comportamento nas taxas extremas (250 e 16 kSPS) | 4 ms e 62,5 µs por conversão não falham igual |

Isso entra na Fase 4 e usa o contador de bordas quando a plataforma o tiver, como referência
independente. **Até esses números existirem, o caminho sem DRDY não entra numa submissão** — é o
único pedaço do driver que este plano deliberadamente não congela.

#### O custo que continua valendo

Se o timer privado sair em favor de trigger padrão, `CONFIG_IIO_HRTIMER_TRIGGER` passa a ser
necessário para a ligação sem IRQ, arrastando `IIO_SW_TRIGGER` → `IIO_CONFIGFS` → `CONFIGFS_FS` num
rootfs somente-leitura — e isso reverte a decisão registrada no `CLAUDE.md`, que se justificava
precisamente por o driver carregar o próprio timer. Custo a declarar quando a hipótese fechar, não
antes.

### 5.3 "Isso é configuração, não fiação"

Já tratada no §4. A regra é mecânica: se um operador pode querer mudar em campo, não é DT.

### 5.4 "Por que um atributo novo?"

Para cada um dos que sobram (§3), a carta traz a justificativa em uma linha e a alternativa
considerada. Montagem de eletrodo (`SRB1`/`SRB2`) não tem ABI no IIO, e é defensável; se o
mantenedor preferir uma ABI genérica, **propor a extensão** é a resposta certa e é um resultado
melhor que o driver.

### 5.5 `available_scan_masks` — **eu estava errado, e o irmão já respondeu**

A versão anterior desta seção dizia que não declarar `available_scan_masks` era item de revisão por
si, e que isso conflitava com derivar o power-down de canal do `scan_mask`. As duas afirmações caem:

- o `ti-ads1298.c` mainline **não declara** `available_scan_masks` e usa `update_scan_mode()` para
  programar o power-down dos canais conforme o `scan_mask` — o padrão existe e é do irmão;
- e o nosso driver **já faz isso**: `ads1299_update_scan_mode()` existe e está ligado em
  `.update_scan_mode`. Não há conflito nenhum, e não há trabalho aqui.

O que sobra é uma pergunta de **semântica do ADS1299**, a ser decidida pelo datasheet e pelo
comportamento real do quadro, não por analogia: o `RDATA` devolve sempre todos os canais, então o
que muda quando um canal é posto em power-down — o layout do quadro, ou apenas o conteúdo daquele
campo? Se o layout não muda, a demultiplexação atual está certa. **É item da Fase 4, com silício**,
e não uma objeção de upstream.

A lição de processo é a de sempre nesta casa, aplicada a mim: a analogia com um chip irmão vale como
hipótese e não como conclusão, e o oráculo é o arquivo mainline, que estava a uma leitura de
distância.

### 5.6 Daisy-chain

`DAISY_EN` só faz sentido com mais de um chip em cascata, o que muda a leitura de quadro inteira.
Expor o bit sem suportar a topologia é oferecer um interruptor que quebra o driver.
**Recomendação**: implementar o bit, documentar que a topologia em cascata não é suportada, e
recusar `DAISY_EN=1` enquanto não houver. Uma recusa explícita é honesta; um bit que corrompe o
fluxo de amostras não é.

---

## 6. Pré-requisitos não negociáveis

| Pré-requisito | Por quê aqui é mais grave que no nosso repositório |
|---|---|
| **Fase 0 do `implementation_plan_afe_bench.md`**: toda constante conferida | uma posição de bit errada em mainline é bug em kernel estável, para qualquer pessoa. Já achamos dois (`CONFIG2`, `CONFIG1`) lendo o datasheet — presumir que não há um terceiro é presunção |
| **A ambiguidade da amplitude do gerador** (`ADS1299_TEST_SIGNAL_PP_DIVISOR`) | um driver upstream não pode embarcar um autoteste cuja tolerância é palpite. Se o fator de dois estiver errado, ele reprova silício bom **de todo mundo** |
| **Silício** | a revisão pergunta "testado em quê?". Sem placa, `dmesg` e saída da ABI, a submissão não é credível — e submeter sem teste queima a primeira impressão, que não se recupera |

Os dois primeiros são leitura e não dependem de hardware. O terceiro é o mesmo bloqueio de todo o
resto do front-end.

---

## 7. Os portões mecânicos

Rodam todos sem placa, e nenhum é opinião:

```sh
scripts/checkpatch.pl --strict --file drivers/iio/adc/ti-ads1299.c
make C=1 CHECK="sparse" drivers/iio/adc/ti-ads1299.o      # sparse
make coccicheck MODE=report M=drivers/iio/adc
make W=1 drivers/iio/adc/ti-ads1299.o
make dt_binding_check DT_SCHEMA_FILES=Documentation/devicetree/bindings/iio/adc/ti,ads1299.yaml
scripts/get_maintainer.pl --patch 0001-*.patch
```

Mais `smatch` se disponível, e `kernel-doc` para qualquer função exportada (hoje não há nenhuma).

---

## 8. Fases

### Fase 1 — higiene e o arquivo no lugar certo `[sem hardware]`

**Objetivo**: o arquivo compila como driver in-tree, não como módulo out-of-tree.
**Procedimento**: árvore mainline recente clonada; arquivo em `drivers/iio/adc/`; `Kconfig`
(`TI_ADS1299`, `depends on SPI`, `select IIO_BUFFER`, `select IIO_KFIFO_BUF`, `select REGMAP`);
linha no `Makefile`; os cinco achados do §1 resolvidos.
**Critérios**: `checkpatch --strict` com **0 erros, 0 avisos, 0 checks**; `sparse` e `W=1` limpos;
`allmodconfig` e `allyesconfig` constroem.
**Injeção de falha**: remover o `select IIO_KFIFO_BUF` e confirmar que a construção quebra — é o
que prova que o `Kconfig` está exprimindo dependência de verdade e não decoração.
**Registro**: `RESULTS.md`, junto das verificações de host.

### Fase 2 — o *binding* YAML `[sem hardware]`

**Objetivo**: um *binding* que valida e que descreve só fiação.
**Critérios**: `dt_binding_check` limpo; o `.dtso` do *hat* deste repositório valida contra ele;
`dtbs_check` sem aviso para um `.dts` de exemplo.
**Injeção de falha**: acrescentar ao exemplo uma propriedade que o esquema não permite, e confirmar
que o `dt_binding_check` **reprova**. Um esquema que aceita qualquer coisa é pior que nenhum.
**Registro**: o próprio arquivo, e `RESULTS.md`.

### Fase 3 — reconciliar a ABI com o padrão `[sem hardware]`

**Objetivo**: a diretriz de completude implementada pelos mecanismos do §3.
**Ordem interna**: canal de temperatura e MVDD → `gpiochip` → eventos IIO para lead-off e
`BIAS_STAT` → atributos que sobram (SRB1, SRB2, MUX por canal, `LOFF`, `BIAS_SENS*`) → `daisy-chain`
recusado explicitamente.
**Critérios**: cada funcionalidade da Tabela 11 do datasheet tem **um** mecanismo; `grep` da tabela
contra o driver não deixa linha sem cobertura; a documentação de ABI cobre exatamente os
customizados e nada mais.
**Injeção de falha**: para cada atributo, escrever um valor inválido e confirmar `-EINVAL` — nunca
aceitação silenciosa. É a mesma regra do `ControlAck`.
**Registro**: `implementation_plan_iio_afe.md`, que é a autoridade sobre o driver.

### Fase 4 — silício `[precisa do módulo]`

**Objetivo**: a evidência que a submissão cita.
**Procedimento**: as Fases 1 a 4 do `implementation_plan_afe_bench.md`, mais a resolução da
ambiguidade da amplitude.
**Critérios**: `dmesg` do *probe* limpo; a varredura de registradores conferida; o autoteste
passando com a tolerância **medida** e não presumida; um registro de amostras com o sinal de teste
interno reconhecível.
**Registro**: `BRINGUP_AFE.md`, e é o material que vai na carta de apresentação.

### Fase 5 — revisão privada antes da pública

**Objetivo**: gastar as objeções baratas antes de gastar a atenção do mantenedor.
**Procedimento**: `b4 prep` e a série montada; leitura por alguém que não escreveu o código;
comparar campo a campo com o irmão `ti-ads1298.c` e justificar cada divergência por escrito.
**Critério**: cada divergência do §2 tem uma frase de justificativa citando datasheet.

### Fase 6 — `v1` e o ciclo

**Procedimento**: `git send-email` para `linux-iio@vger.kernel.org`, `devicetree@vger.kernel.org` e
o que o `get_maintainer.pl` apontar, com carta de apresentação que diz o que foi testado, em qual
hardware, e o que **não** foi.
**Critério de "pronto" desta fase**: a série postada e arquivada na lore, com *Message-ID* citável.
**O que vem depois não é agendável**: responder cada revisão, repostar `v2`, `v3`. Regra de
etiqueta que vale seguir: responder tudo, não discutir estilo, e nunca repostar sem changelog.

### Fase 7 — o que muda neste repositório quando (e se) entrar

Pela §2.2, o driver **nunca morou** neste repositório sob o modelo novo: a migração das duas
receitas de módulo para a árvore do kernel é a Fase 0 prática deste plano, e acontece antes da Fase 1.
O que a aceitação upstream muda depois é só de onde o código vem para a imagem: de "uma revisão fixa
do nosso fork" para "já está no kernel que a ST rebasa" — e o bbappend que aponta para o fork deixa
de ser necessário. A camada adjunta fica com o *overlay*, a udev e a
integração de plataforma, e o custo de um front-end novo cai para "habilitar um `CONFIG_` e escrever
um *overlay*" — o que **fortalece** a métrica de reuso.

---

## 9. Ordem, e o que bloqueia o quê

```
Fase 1 ──► Fase 2 ──► Fase 3 ──────────┐
                                       ├──► Fase 5 ──► Fase 6 ──► (revisões) ──► Fase 7
Fase 0 do plano de bancada ──► Fase 4 ─┘
```

**As Fases 1, 2 e 3 não precisam de hardware** e somam a maior parte do trabalho. É mais uma frente
que converte o atraso do módulo em progresso, em vez de espera.

---

## 10. Calendário honesto

| Etapa | Depende de | Cabe antes de dezembro? |
|---|---|---|
| Fases 1 e 2 | nada | sim, com folga |
| Fase 3 | nada | sim, e é a maior |
| Fase 0 (datasheet) | os dois datasheets | sim |
| Fase 4 | **o módulo chegar** | depende da entrega |
| Fase 6 (submissão) | Fase 4 | possível, se o módulo chegar até novembro |
| Aceitação | os mantenedores | **não agendável. Não prometer** |

---

## 11. Riscos

| Risco | Mitigação |
|---|---|
| A sincronização trigger → conversão → `RDATA` não fecha (§5.2) | o caminho sem DRDY **não entra no `v1`**; as candidatas 1 a 3 são medidas na Fase 4 antes de qualquer congelamento |
| O arquivo divergir entre mainline e o kernel 6.6 da placa | compilar limpo nas duas bases é critério de cada fase (§2.2) |
| A ponte instancia um dispositivo por nome (§2.1) | a instanciação sai da ponte e vai para a cola do objetivo 3, **antes** de qualquer submissão dos dois |
| Um mantenedor pede ABI genérica em vez de atributo | é um resultado melhor, não um obstáculo — e vira seção do texto |
| Uma terceira constante errada aparece em revisão | Fase 0 primeiro; e um erro achado por nós é barato, achado por um mantenedor é caro |
| O módulo não chega e a Fase 4 não acontece | submeter sem teste **não** é opção; as Fases 1 a 3 continuam valendo como resultado |
| O arquivo submetido divergir do nosso | a Fase 7 existe por isso; e nada de *delta* local (§5.2, saída 2) |
| O ciclo de revisão consumir dezembro | a submissão é o entregável, não a aceitação (§12) |

---

## 12. O que o TCC pode afirmar em cada estado

Isto existe para que nenhuma frase do texto prometa mais do que o estado sustenta:

| Estado | Afirmação defensável |
|---|---|
| Fases 1–3 feitas | *"o driver satisfaz os portões mecânicos de submissão do kernel — `checkpatch --strict`, `sparse`, `W=1` e `dt_binding_check` — e sua ABI foi reconciliada com os mecanismos padrão do subsistema IIO"* |
| Fase 4 feita | *"validado em silício, com o roteiro e os números em `BRINGUP_AFE.md`"* |
| Fase 6 feita | *"submetido à `linux-iio` em <data>, Message-ID <…>"* — verificável por terceiro na lore |
| Revisão em curso | *"em revisão pública, com <n> rodadas"*, citando os arquivos |
| Aceito | *"aceito em <árvore>, commit <sha>"* |

O que **não** se pode escrever em nenhum estado anterior ao último: "o driver foi aceito no kernel
Linux", nem "será". E vale notar que a Fase 6 já é, por si, um resultado incomum num TCC — código
em revisão pública num subsistema do kernel, com proveniência documentada e uma trilha de revisão
citável, é evidência de qualidade externa que nenhuma seção de resultados consegue fabricar.

---

## 13. Validação

*(Vazio de propósito.)*

| Fase | Critério | Resultado | Data |
|---|---|---|---|
| 0 | base validada: sha256 do tarball confere; 749 arquivos no patch da ST | **feito** | 2026-09-26 |
| 0 | digest de referência da árvore consumida pelo Yocto | **`35e19311…`, 81889 arquivos** | 2026-09-26 |
| 0 | tag `v6.6-stm32mp-r3.1` aponta para `548f960c…` | **confirmado** | 2026-09-26 |
| 0 | digest do checkout Git **bate** com a referência (§2.3) | **bate**: 81889 comuns, 0 divergentes | 2026-09-26 |
| 0 | branch `feature/ads1299-mcp2210-driver` criada sobre `548f960c…`, digest inalterado | **feito** | 2026-09-26 |
| 0 | `S:class-devupstream` confirmado em build | — | — |
| 1 | `checkpatch --strict` 0/0/0; `sparse` e `W=1` limpos | — | — |
| 2 | `dt_binding_check` limpo; injeção de falha vista | — | — |
| 3 | toda linha da Tabela 11 coberta por um mecanismo | — | — |
| 4 | autoteste passa com tolerância medida | — | — |
| 5 | cada divergência do irmão justificada por escrito | — | — |
| 6 | série na lore, Message-ID | — | — |
