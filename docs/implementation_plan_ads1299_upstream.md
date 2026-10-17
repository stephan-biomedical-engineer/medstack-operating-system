# Plano de implementação — o driver ADS1299 como release no kernel Linux

> **Estado (2026-10-16)**: **nada submetido**. A Fase 0 está feita (§13), e a branch de submissão
> `feature/ads1299-mcp2210-driver` tem o driver, o *binding* e a entrada em `MAINTAINERS`. Uma
> auditoria campo a campo, feita em 2026-10-16 (§3.1), mediu quanto do CI ela cobre: dos **33 campos
> funcionais** dos registradores, **6** estão expostos ao usuário, **5** são escritos com um valor
> fixo, **7** estão cobertos pela metade e **15** não têm caminho nenhum. A `board/medplatform-afe`
> chega a 9/10/5/9, e **nenhuma das duas cobre o CI**. A Fase 3 (§8) foi reescrita em subfases para
> fechar os 33.
>
> **Atualização (2026-10-17)**: a feature chegou a **26 / 5 / 0 / 2** (CLK_EN, CAL_FREQ, BIAS_MEAS,
> BIASREF_INT, BIAS_LOFF_SENS, BIAS_STAT, os três campos do LOFF, LOFF_SENSP, LOFF_SENSN, LOFF_FLIP,
> LOFF_STATP, LOFF_STATN, SRB1, SRB2, BIAS_SENSP, BIAS_SENSN o PGA e o MUX fechados; DAISY_EN passou a
> ser escrito, sem opção; PD_LOFF_COMP passou a ser derivado). Só testado no host. A coluna **PR** da
> §3.1 acompanha a feature; a auditoria acima fica como foi medida.
>
> **Decisão reafirmada em 2026-10-16**: o driver cobre **todas** as funcionalidades do CI, **inclusive
> as que a placa deste projeto não usa nem pode usar** (GPIO, SRB2, saída de clock, lead-off AC). A
> diretriz de 2026-09-26 (§3) deixa de ser interpretada como "o que a plataforma precisa". Cobertura é
> propriedade do driver e não da placa.
>
> **A exceção, decidida em 2026-10-17, é a daisy-chain** (§5.6): o *binding* a descreve, o driver do
> `v1` a recusa no *probe*, e o código que a suporta espera numa branch própria por uma série separada.
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

**A resolução é a regra do mecanismo padrão primeiro.** As duas diretrizes são compatíveis *se*
cada funcionalidade for exposta pelo mecanismo que o kernel já tem para ela, e se cada campo tiver
**um** mecanismo, não dois. A tabela de 2026-09-26 que estava aqui era o esboço desse mapeamento.
A §3.1 a substitui, campo a campo e contra o código das duas branches.

Três regras decidem o mecanismo de cada campo, e elas são as mesmas do §4:

1. **Fiação vai para o DT.** Se o campo é consequência de como a placa foi montada (um BIASREF
   externo, uma cascata de chips, um clock no pino CLK), ele vira propriedade do *binding*, e o
   driver o escreve a partir dela. O usuário não escolhe.
2. **Configuração vai para o sysfs.** Se um operador pode querer mudar em campo, é atributo, e
   **padrão do IIO antes de customizado**.
3. **Política fica fora do kernel.** O driver expõe o mecanismo do CI e não decide por ele. A
   `board/medplatform-afe` hoje decide em dois lugares: em referencial, zera o `BIAS_SENSN` e reduz
   o `LOFF_SENSN` a um bit. Essa decisão é correta para EEG e não é do driver. Ela desce para a
   tradução por ligação (`do_derive_device_options`), que já é onde a plataforma traduz uma
   prescrição em opções de dispositivo.

**E há um ganho colateral que vale dizer**: cada atributo customizado é uma palavra que a tabela de
tradução do `do_derive_device_options` precisa conhecer. Mecanismo padrão é acoplamento que não
existe. Upstreamabilidade e a métrica de reuso deste trabalho pedem a mesma coisa.

### 3.1 Auditoria campo a campo — 2026-10-16

Feita contra `ti-ads1299.c` nas duas branches, com o datasheet transcrito em
`Register_Map_ADS1299.md` (Tabelas 11 a 28 do SBAS499). Os bits reservados ficam de fora, e o MISC2
inteiro também, porque o registrador todo é reservado.

Legenda: ✅ exposto ao usuário · ⚙️ o driver escreve um valor fixo ou derivado, sem opção · ⚠️
coberto pela metade ou com defeito · ❌ sem caminho (fica no valor de reset). **Alvo** é o mecanismo
que fecha o campo, e a coluna **Sub** aponta a subfase da Fase 3 (§8) que o implementa.

A coluna **PR (atual)** acompanha a `feature/ads1299-mcp2210-driver`: um campo fechado depois da
auditoria leva o símbolo de agora e o commit que o fechou. A coluna **Board** é a da auditoria de
2026-10-16, porque nada disso foi para a `board/medplatform-afe`.

| Reg | Campo | PR (atual) | Board | Alvo | Tipo | Sub |
|---|---|---|---|---|---|---|
| ID | Device ID | ⚙️ | ⚙️ | confere a família no *probe*; `REV_ID` vai para o `dmesg` | — (fica ⚙️) | 3.0 |
| ID | Nº de canais | ✅ | ✅ | já fechado | — | — |
| CONFIG1 | DAISY_EN | ⚙️ `ed48db8df4c8` | ❌ | gravado 0 no *probe*: é o modo daisy-chain, em que um chip só lê igual; a cadeia é recusada (§5.6) | — (fica ⚙️) | 3.8 |
| CONFIG1 | CLK_EN | ✅ `ceaa3f09e909` | ❌ | **provedor de clock** (`#clock-cells = <0>`), ligado por `clk_prepare_enable` de quem consome | padrão (clk) | 3.8 |
| CONFIG1 | DR | ✅ | ✅ | já fechado | padrão | — |
| CONFIG2 | INT_CAL | ✅ | ✅ | já fechado | custom | — |
| CONFIG2 | CAL_AMP | ✅ | ✅ | já fechado | custom | — |
| CONFIG2 | CAL_FREQ | ✅ `ad3a2ec3641b` | ⚠️ sem DC | `test_signal` com `1x_dc` e `2x_dc` (o código 10 não é usado), `test_signal_available`, `-EBUSY` com o buffer ligado; a entrada de teste nunca fica sem o gerador (`17eccf84c5d6`) | custom | 3.1 |
| CONFIG3 | PD_REFBUF | ⚙️ | ⚙️ | segue o `vref-supply` (fiação) | — (fica ⚙️) | — |
| CONFIG3 | BIAS_MEAS | ✅ `89bed0807417` | ❌ | ligado sempre que algum canal tem MUX=010, desligado quando nenhum tem (conserta D1) | via MUX | 3.1 |
| CONFIG3 | BIASREF_INT | ✅ `89bed0807417` | ⚙️ | `biasref-supply` no DT (`b82dc6df6759`): com ela a placa alimenta o pino e o bit fica 0; sem ela, o CI gera (AVDD + AVSS) / 2 | DT (padrão) | 3.4 |
| CONFIG3 | PD_BIAS | ✅ `8c0d40ff063a` | ✅ | `bias_drive` = `off`/`on` e só isso; `reference` e `derived` viraram combinações com o BIAS_SENS | custom | 3.4 |
| CONFIG3 | BIAS_LOFF_SENS | ✅ `b27cb88553d0` | ❌ | atributo `bias_lead_off` (`off`/`on`), que liga também os comparadores; `-EBUSY` com o buffer ligado | custom | 3.5 |
| CONFIG3 | BIAS_STAT | ✅ `b27cb88553d0` | ❌ | lido **sem cache** (CONFIG3 volátil, D3): `bias_lead_off_status` = `disabled`/`connected`/`disconnected`. Ele não vem na *status word*, então o registrador é o único caminho; se o lead-off ganhar evento na 3.5, este bit vai junto | custom | 3.5 |
| LOFF | COMP_TH | ✅ `a6edbf1cfde1` | ⚙️ 95/5 % | `lead_off_threshold_percent` + `_available` (8 pares; o valor é o lado positivo, 95 = 95/5 % … 70 = 70/30 %) | custom | 3.5 |
| LOFF | ILEAD_OFF | ✅ `a6edbf1cfde1` | ⚙️ 6 nA | `lead_off_current_nanoamp` + `_available` (6, 24, 6000, 24000). 6 e 24 µA em DC passam do limite da IEC 60601-1: é a plataforma que não os oferece (§11), não o driver | custom | 3.5 |
| LOFF | FLEAD_OFF | ✅ `a6edbf1cfde1` | ⚙️ DC | `lead_off_frequency` + `_available` (`dc`, `ac_7.8`, `ac_31.2`, `ac_fdr_div_4`; Hz do clock interno de 2,048 MHz). O significado do LOFF_STAT em AC segue sem confirmação (§3.1.2) | custom | 3.5 |
| CHnSET | PD | ✅ | ✅ | já fechado (máscara de varredura) | padrão | — |
| CHnSET | PGA | ✅ `c880fb1f02f6` | ⚠️ idem, + limite no canal de temperatura | `hardwaregain` **por canal** (`info_mask_separate`, com `_available`), `-EBUSY` com o buffer ligado; o autoteste julga cada canal pelo seu ganho. Ganho igual para todos vira política da plataforma | padrão | 3.2 |
| CHnSET | SRB2 | ✅ `11a174bc5f1f` | ❌ | `in_voltageN_srb2` por canal (0/1, `ext_info`) | custom | 3.3 |
| CHnSET | MUX | ✅ `e749f79fd717` | ⚠️ idem, + `temperature_channel` | `in_voltageN_input_mux` por canal, um `_available` por tipo; BIAS_MEAS e a entrada de teste valem para "algum canal". Os canais de temperatura e alimentação da §3.1.1 seguem em aberto (3.2) | custom | 3.1, 3.2 |
| BIAS_SENSP | IN1P…IN8P | ✅ `8c0d40ff063a` | ⚠️ tudo ou nada, menos o sensor | `in_voltageN_bias_sense_p` por canal (0/1, `ext_info`) | custom | 3.4 |
| BIAS_SENSN | IN1N…IN8N | ✅ `8c0d40ff063a` | ⚠️ idem, 0 em referencial (política) | `in_voltageN_bias_sense_n` por canal (0/1, `ext_info`); o 0 em referencial vai para a tradução da plataforma | custom | 3.4 |
| LOFF_SENSP | IN1P…IN8P | ✅ `308e6166ef98` | ⚙️ todos com `lead_off=dc` | `in_voltageN_lead_off_sense_p` por canal (0/1, `ext_info`) | custom | 3.5 |
| LOFF_SENSN | IN1N…IN8N | ✅ `308e6166ef98` | ⚙️ todos, ou um bit em referencial (política) | `in_voltageN_lead_off_sense_n` por canal (0/1, `ext_info`); a política do referencial fica na plataforma | custom | 3.5 |
| LOFF_FLIP | canal 1…8 | ✅ `308e6166ef98` | ❌ | `in_voltageN_lead_off_flip` por canal (0/1, `ext_info`) | custom | 3.5 |
| LOFF_STATP | canal 1…8 | ✅ `8b545d83bad2` | ✅ | evento `fault`/`openwire` por canal **e** a palavra por amostra `in_voltage8_lead_off` no buffer (§3.1.2); `lead_off_status` para leitura avulsa | padrão + custom | 3.5 |
| LOFF_STATN | canal 1…8 | ✅ `8b545d83bad2` | ✅ | idem | padrão + custom | 3.5 |
| GPIO | GPIOD[4:1] | ❌ | ❌ | **`gpiochip`** de 4 linhas, registrador volátil | padrão | 3.6 |
| GPIO | GPIOC[4:1] | ❌ | ❌ | direção do mesmo `gpiochip` | padrão | 3.6 |
| MISC1 | SRB1 | ✅ `11a174bc5f1f` | ✅ `montage` | `montage` (`bipolar`/`referential`), o nome e os valores da board, mas **só a chave**: a regra do referencial (BIAS_SENSN = 0, um bit no LOFF_SENSN) sai do kernel e vai para a tradução da plataforma (§3.3). O nome vai para a pergunta de ABI da §5.4 | custom | 3.3 |
| CONFIG4 | SINGLE_SHOT | ⚙️ | ⚙️ | segue o modo, single-shot na leitura direta e contínuo no buffer | — (fica ⚙️) | — |
| CONFIG4 | PD_LOFF_COMP | ⚙️ derivado (`308e6166ef98`) | ⚙️ segue `lead_off` | ligado se e só se algum bit de `LOFF_SENS*` ou o `BIAS_LOFF_SENS` estiver ligado | — (fica ⚙️) | 3.5 |

**Saldo do alvo**: 5 campos ficam ⚙️ **por desenho**, porque cada um é função da fiação ou de outro
mecanismo e não tem valor que o usuário possa querer escolher à parte: Device ID, PD_REFBUF,
SINGLE_SHOT, PD_LOFF_COMP e DAISY_EN (desde 2026-10-17, §5.6). 2 vão para o DT: BIASREF_INT e a
descrição da cadeia, que o driver do `v1` recusa. Os outros 26 têm atributo ou mecanismo padrão. O critério da Fase 3 é que nenhuma linha fique ❌ ou ⚠️.

**O que não é registrador e também é funcionalidade do CI**, porque a diretriz é "todas as
funcionalidades" e não "todos os registradores":

| Funcionalidade | Hoje (as duas) | Alvo | Tipo | Sub |
|---|---|---|---|---|
| Pino START | só o comando `START` | `start-gpios` opcional no DT. Com ele, o início é pelo pino, que é o que sincroniza vários chips. **Em aberto**: no modo *cascaded* cada chip é um dispositivo IIO, e um pino de START compartilhado precisa de um dono que nenhum dos drivers é (§5.6) | DT | 3.8 |
| Pino PWDN | não descrito | `powerdown-gpios` opcional, usado no *suspend* | DT + PM | 3.7 |
| Pino CLKSEL | inferido da presença de `clocks` | idem, documentado no *binding* | DT | 3.8 |
| `STANDBY` / `WAKEUP` | definidos, sem uso | `runtime_pm` e `system sleep` (`pm_ptr`, `DEFINE_RUNTIME_DEV_PM_OPS`) | padrão (PM) | 3.7 |
| `RDATAC` | sem uso (`RDATA` por amostra) | **decisão de projeto**: mede-se a 16 kSPS se `RDATA` por amostra fecha a janela; se fechar, fica registrado como não necessário, com o número | — | 3.7 |
| `RREG`/`WREG` de vários registradores | sempre um | não muda a cobertura; fica como está | — | — |
| Bits GPIO[7:4] da *status word* em cada amostra | descartados | opcional: canal de varredura. Não é exigido para cobrir GPIOD, que o `gpiochip` já cobre | custom | 3.6 |

#### 3.1.1 Temperatura e alimentação: um campo de MUX, dois mecanismos de leitura

MUX=100 (sensor de temperatura) e MUX=011 (MVDD) são valores do MUX de um canal físico, e não canais
próprios. O IIO tem canal padrão para os dois (`IIO_TEMP` com `_raw`/`_scale`/`_offset`, e uma tensão
de alimentação), e o caminho padrão é:

- **leitura direta** de `in_temp_raw` e da tensão de alimentação: o driver troca o MUX de um canal,
  faz uma conversão single-shot e devolve o MUX. É mecanismo padrão, e é o que um mantenedor espera.
  A conversão para °C fica em `_scale`/`_offset`, e as constantes vêm do datasheet: 145.300 µV a
  25 °C e 490 µV/°C, **marcadas `[SBAS499?]`**. Elas estão em §9.3.1, fora das tabelas que
  `Register_Map_ADS1299.md` transcreveu, e entram na Fase 0 do plano de bancada como dívida. A
  plataforma converte hoje no *build* (`implementation_plan_hmi_desktop.md` §12.8), e as duas
  conversões têm de concordar, ou uma das duas está errada.
- **no buffer**, um canal dedicado ao sensor por toda a sessão: é o `temperature_channel` da
  `board/medplatform-afe`, que a plataforma usa, e continua customizado. Ele expressa o
  `in_voltageN_input_mux = temperature` com o ganho limitado a 12, que é a parte que vale guardar.
  Quando o MUX virar por canal (3.1), esse atributo vira redundante e **sai**, e o limite de ganho
  passa a ser recusado com `-EINVAL` quando o MUX daquele canal é o sensor e o ganho passa de 12.
  Um campo, um mecanismo.

Para o MVDD, o que cada canal mede é diferente: (AVDD − AVSS)/2 nos canais 1, 2 e 5–8, DVDD/4 nos 3
e 4. **`[SBAS499?]`, e a confirmar antes de escrever o `_scale`.** É o tipo de constante que, errada,
sai como bug em kernel estável.

#### 3.1.2 Status de lead-off: evento e palavra por amostra não são duplicação

A regra "um campo, um mecanismo" tem uma exceção aqui, e ela é justificada. O evento IIO diz
**quando** um eletrodo se soltou, com carimbo e *poll*. A palavra no buffer diz **em que estado cada
amostra foi adquirida**, que é o que o registro clínico precisa (`implementation_plan_usb_parity.md`
§7.2). São duas funções distintas sobre o mesmo bit, e a segunda não se reconstrói a partir da
primeira sem perder a associação com a amostra.

**O tipo de evento foi decidido em 2026-10-17, e a tensão entre as duas bases (§2.2) ficou com custo
declarado.** É o `IIO_EV_TYPE_FAULT` com `IIO_EV_DIR_FAULT_OPENWIRE`, que **existe na mainline**: entrou
em 2025 com a detecção de fio aberto do AD4111 no `ad7173` (commit de núcleo `34934d79965518548d33c0513a9572ae936d8c29`,
"iio: introduce the FAULT event type", revisado no IIO), e o `ad7173` é o modelo seguido - um evento por
canal, uma habilitação para todos. No 6.6 da placa ele não existia; a feature carrega o **backport** desse
commit (`66874e950a96`, autor original, marca `[ Upstream commit ... ]`), que **sai da série na submissão**. A
segunda diferença entre as bases é a assinatura: o `write_event_config` recebe `int state` no 6.6 e
`bool` na mainline atual, e o driver muda isso ao ir para lá. Implementado em `8b545d83bad2`: o evento sai na
transição para "fora", uma vez, com o carimbo da amostra; só com o buffer ligado, porque o estado vem
na *status word*; e de novo se o eletrodo ainda estiver fora quando o buffer religa. O canal
`in_voltageN_lead_off`, de tipo `IIO_VOLTAGE` sem ser tensão, foi trazido igual ao da board, para a
tradução da plataforma continuar valendo, e continua sendo o ponto que mais vai ser discutido na
revisão: entra na carta como pergunta, não como afirmação.

**E o modo AC muda o que o bit significa - confirmado no datasheet em 2026-10-17.** Os comparadores
são o método DC (SBAS499 §9.3.2.4.3.1: "if dc lead-off is not used, the lead-off comparators can be
powered down"). Em AC (§9.3.2.4.3.2), a excitação é **dentro** da banda (7,8 Hz e 31,2 Hz, e não fora,
como esta seção dizia), passa pelo canal, e o eletrodo se mede nos dados, pela amplitude na
frequência da excitação; o `LOFF_STAT*` não é o status dele. Por isso o `lead_off_status` responde
`ac` nesse modo (`308e6166ef98`), e o evento, quando existir, é suprimido em AC. Medir a amplitude é trabalho
de quem lê os dados, não do driver.

### 3.2 Defeitos achados pela auditoria

Nenhum deles é visto por um *build*, e são consertados antes de qualquer funcionalidade nova, porque
cobertura nova sobre um mecanismo quebrado só multiplica o defeito.

| # | Onde | Defeito | Conserto | Sub |
|---|---|---|---|---|
| D1 | as duas | `input_mux = bias_measurement` grava MUX=010 sem `BIAS_MEAS`. O datasheet diz que 010 é "usado em conjunto com o bit `BIAS_MEAS`" (`Register_Map_ADS1299.md`, CHnSET), então a opção está exposta e não mede o BIAS | `BIAS_MEAS` segue o MUX (§3.1). **Corrigido na feature** (`89bed0807417`) | 3.1 |
| D2 | PR | o `LOFF_SENSP/N` nunca é gravado, então `lead_off_status` responde sempre `disabled`: o código existe e nunca é alcançado. E os comparadores são ligados no *probe* sem nenhum canal sensível | trazer o lead-off da board (§3.3). **Corrigido na feature** (`308e6166ef98`): *sense* por canal e comparadores só com algo monitorado | 3.0 |
| D3 | as duas | o `BIAS_STAT` é bit de status num registrador em cache, então uma leitura devolveria sempre "conectado". A board só o mascara na releitura | leitura sem cache desse bit, ou CONFIG3 volátil com a releitura ciente disso. **Corrigido na feature** (`b27cb88553d0`: CONFIG3 volátil) | 3.5 |
| D4 | board | o comentário acima de `ADS1299_REG_LOFF_SENSP` ainda abre com "this driver never writes either of them", e o próprio bloco diz depois que `ads1299_set_lead_off()` os grava | reescrever o comentário | 3.0 |
| D5 | as duas | `ads1299_read_one()` limpa o `PD` do canal lido e nunca o devolve, então uma leitura direta deixa um canal ligado contra a máscara de varredura | restaurar o `PD` ao fim da leitura | 3.0 |
| D6 | PR | `input_mux = temperature`/`supply` devolve a escala de tensão, sem conversão | §3.1.1 | 3.2 |
| D7 | *binding* | `interrupts` é obrigatório, mas o driver tem um caminho sem IRQ. É coerente **só se** o caminho sem DRDY ficar fora do `v1` (§5.2), e o *binding* e o driver têm de dizer a mesma coisa na série | decidir junto com a §5.2 | 3.0 |

### 3.3 As duas branches, e para onde vai o trabalho

A cobertura é feita na **`feature/ads1299-mcp2210-driver`**, que é a que vira série. A
`board/medplatform-afe` está 801 linhas à frente nesse arquivo, e boa parte do que ela tem a mais é
cobertura que a série precisa:

| Commit da board | O que traz | Destino |
|---|---|---|
| `df866328fcff` | releitura de toda escrita de configuração + checagem da *status word* | **sobe como está**: robustez sem ABI nova |
| `763fa13369bd` | lead-off DC, `lead_off_monitored`, palavra por amostra | sobe **remodelado** pela §3.1: parâmetros de LOFF, *sense* por canal, sem a política de referencial |
| `983c5cf63b54` | `montage` (SRB1), com a derivação do bias trocada junto | sobe **remodelado**: o SRB1 como atributo, sem mexer no `BIAS_SENS*` |
| `efba42f1ee08` | canal de temperatura e todos os CHnSET derivados de um estado | sobe **o estado único**. O `temperature_channel` vira MUX por canal (§3.1.1) |
| `2d1f7fb7efc1` | autoteste pelo nível DC | já está na feature como `e577cc080449` |
| `801dbf215569`, `c2291fd29166`, `643c0d757892` | *pump* de SPI em tempo real; driver da placa do MCP2210 | ficam na board: são da placa, não da cobertura |

Depois disso a board é **rebaseada sobre a feature** e fica só com o que é da placa. É a regra da
§2.2 aplicada ao conteúdo: o que a placa testa e o que se submete são a mesma história.

**O preço disso fica escrito aqui, porque cai fora deste plano**: a ABI de que a plataforma depende
hoje muda. A tradução das ligações `spi`/`usb` (`do_derive_device_options`, no recipe do
`eeg-acquisition-service`) e o driver `iio` do framework usam `hardwaregain` compartilhado,
`input_mux`, `test_signal`, `bias_drive` (três modos), `montage`, `lead_off`, `lead_off_monitored`,
`temperature_channel` e `in_voltageN_lead_off`. Com a §3.1, o ganho e o MUX passam a ser por canal,
o `bias_drive` perde o modo `derived` (que vira escrita por canal em `bias_sense_*`), o
`temperature_channel` sai e a política de referencial passa a ser da plataforma. **Uma imagem `usb`
construída com a receita antiga sobre um módulo novo não adquire**, e o `CLAUDE.md` já descreve o
mesmo efeito para o `montage`. Por isso a troca na plataforma tem de entrar no **mesmo** `SRCREV` que
leva a ABI nova para a imagem, e `make check` mais a suíte `tests/derive` são o portão.

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

**O que a cobertura total acrescenta ao *binding*** (§3.1). Tudo isto é fiação, e nada disto está no
`ti,ads1299.yaml` da feature hoje:

| Propriedade | Por quê | Sub |
|---|---|---|
| `gpio-controller`, `#gpio-cells = <2>` | o `gpiochip` dos GPIO1–4 | 3.6 |
| `#clock-cells = <0>` | o CI como provedor de clock (CLK_EN) quando roda do oscilador interno. **Feito** (`8c3d602693f1`), com `clock-output-names` | 3.8 |
| `start-gpios` | o pino START, que sincroniza chips em cascata | 3.8 |
| `powerdown-gpios` | o pino PWDN | 3.7 |
| `dvdd-supply` | a alimentação digital, que hoje não é descrita | 3.7 |
| `biasref-supply` | `BIASREF_INT = 0`: a placa alimenta o BIASREF de fora. Uma alimentação e não um booleano `ti,`: tensão num pino é o que o `-supply` descreve, como o `vref-supply` (padrão antes de customizado). **Feito** (`b82dc6df6759`) | 3.4 |
| `#daisy-chained-devices` (a propriedade comum, `common-properties.txt`) | quantos chips dividem o CS, o que muda o quadro inteiro. O *binding* descreve o hardware mesmo que o driver não o suporte: com N > 1 exige `clocks` e proíbe `#clock-cells`, e o driver do `v1` recusa a cadeia. **Feito** (`8c3d602693f1`) | 3.8 |

Cada propriedade nova entra com o exemplo correspondente, e a injeção de falha da Fase 2 é refeita
para ela.

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

**Decisão de 2026-10-17: a daisy-chain sai do `v1`.** O *binding* a descreve, o driver a recusa com
`-EOPNOTSUPP` no *probe*, e o código que a suporta está pronto e guardado na branch
`wip/ads1299-daisy-chain` do `linux-med` (`7c9cb5cf7277`), com os testes de host dele no histórico
de `tests/ads1299/feature.c` (`dcb13d3`). Ela vira uma série separada, depois do `v1`.

Ela chegou a ser implementada (2026-10-17) e foi o que mostrou por que não deve ir agora:

- **O datasheet corrige o que este plano dizia** (SBAS499 §10.1.4.2 e Tabela 13). `DAISY_EN = 0` é o
  modo daisy-chain, e é o **reset**; o 1, leitura múltipla, só repete dados sob `RDATAC`, que o driver
  não usa. E a cadeia divide o DIN, então **todo registro escrito chega a todos os chips**: o canal *n*
  de cada chip divide um CHnSET, a cadeia roda de um clock externo e o bias não pode ser usado. O que
  um RREG devolve numa cadeia não está documentado, então só o ID do primeiro chip pode ser conferido.
  A versão anterior desta seção dizia o contrário nos dois pontos.
- **O custo cresce com o resto da cobertura.** O padrão da mainline para cadeias (`gpio-max3191x`, e o
  `ad7280a`, cujos chips têm endereço) é "N chips iguais, uma configuração". Num conversor com
  configuração por canal, cada funcionalidade das subfases 3.1 e 3.2 teria de amarrar o canal *n* de
  todos os chips, e a cadeia passaria a ser caso especial em toda a ABI por canal.
- **Não há como testá-la em silício aqui**: a bancada tem um chip.

**O que ficou no `v1`**, porque vale para um chip só (`ed48db8df4c8`): `DAISY_EN = 0` gravado em vez
de assumido do reset; a *status word* conferida (1100b) em toda leitura do autoteste; e a recusa de
`#daisy-chained-devices` > 1. Sem a recusa, uma cadeia de dois chips passaria no *probe* como um
chip de 8 canais e perderia os outros 8 sem aviso - medido no *fake*, é a injeção de falha do teste.

**Quem tiver a placa em cadeia**, até a série separada: aplica o `7c9cb5cf7277` como patch fora da
árvore (nesta plataforma, a camada adjunta é o lugar dele); ou religa em modo *cascaded*, um CS por
chip, que o `v1` atende - cada chip é um dispositivo IIO, e um deles fornece o clock aos outros pelo
`#clock-cells`. O que o *cascaded* ainda não tem é o início simultâneo: sem `start-gpios`, o START vai
por comando, chip a chip, e a defasagem entre eles fica constante (o clock é comum) mas existe. Um
pino de START compartilhado entre dispositivos IIO separados precisa de um dono, e esse é o ponto em
aberto da 3.8.

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

**Objetivo**: os 33 campos da §3.1 e as funcionalidades fora de registrador fechados, cada um com
**um** mecanismo, sem nenhuma linha ❌ ou ⚠️.

**Onde**: na `feature/ads1299-mcp2210-driver`, um commit por funcionalidade, para que a série se
leia como uma lista de capacidades e o revisor possa recusar uma sem levar as outras.

**Critérios que valem para todas as subfases:**
- **`tests/ads1299` cobre cada campo novo**: o byte gravado em cada registrador para cada valor do
  atributo, contra o *fake* do conversor. Onde o firmware do M33 também escreve o campo, o
  `afe_regs.c` dele é o oráculo, como já é hoje. Um teste escrito a partir da mesma leitura do
  datasheet que escreveu o código prova consistência e não correção (`implementation_plan_afe_bench.md`
  §13.1), e o §13 registra cada campo dizendo qual dos dois ele tem.
- **Valor inválido dá `-EINVAL`** em cada atributo, nunca aceitação silenciosa. É a mesma regra do
  `ControlAck`.
- **Mudança com o buffer ligado dá `-EBUSY`** em tudo o que muda o significado de uma amostra (MUX,
  SRB, *sense*, ganho), pelo mesmo argumento que o `bias_drive_store` já escreve: duas configurações
  num registro, sem marca de onde uma termina.
- **`checkpatch --strict` 0/0/0, `W=1` e `sparse` limpos nas duas bases** (§2.2), a cada commit e não
  só no fim.
- **A documentação de ABI** (`sysfs-bus-iio-ads1299`) **cresce no mesmo commit** que o atributo.

**Subfases, em ordem.** Cada uma depende da anterior só onde está dito.

| Sub | Conteúdo | Campos (§3.1) | Injeção de falha que prova o teste |
|---|---|---|---|
| **3.0** | **Base**: trazer da board a releitura, a *status word*, o estado único dos CHnSET e o lead-off (§3.3); corrigir D2, D4, D5 e D7 | — | reverter a releitura e ver o teste de "registro que a peça não guardou" falhar |
| **3.1** | **MUX por canal** + `BIAS_MEAS` seguindo o MUX + `test_signal` com DC | MUX, BIAS_MEAS, CAL_FREQ | gravar MUX=010 sem `BIAS_MEAS` e ver o teste de D1 falhar |
| **3.2** | **Ganho por canal** (`hardwaregain` separado); `in_temp_*` e alimentação na leitura direta; `temperature_channel` sai | PGA, MUX (100, 011) | trocar o `_offset` da temperatura e ver a conversão de referência (145.300 µV → 25 °C) falhar |
| **3.3** | **SRB1** (global) e **SRB2** (por canal) | SRB1, SRB2 | deslocar a máscara do SRB2 para o bit 4 (que é do PGA) e ver o teste do byte do CHnSET falhar |
| **3.4** | **Bias por campo**: `bias_drive` = PD_BIAS; `bias_sense_p/n` por canal; BIASREF externo pelo DT. A política de referencial sai do driver | PD_BIAS, BIAS_SENSP/N, BIASREF_INT | deixar a política de referencial ligada e ver o teste "SRB1 não mexe em BIAS_SENSN" falhar |
| **3.5** | **Lead-off completo**: limiar, corrente, frequência; *sense* P/N e FLIP por canal; `BIAS_LOFF_SENS` e `BIAS_STAT` sem cache; PD_LOFF_COMP derivado; evento + palavra por amostra (§3.1.2) | LOFF (3), LOFF_SENSP/N, LOFF_FLIP, LOFF_STATP/N, BIAS_LOFF_SENS, BIAS_STAT, PD_LOFF_COMP | pôr o CONFIG3 em cache e ver o teste do `BIAS_STAT` falhar (D3) |
| **3.6** | **`gpiochip`** dos GPIO1–4, registrador volátil; *binding* com `gpio-controller` | GPIOD, GPIOC | inverter o sentido do GPIOC (0 = saída) e ver o teste de direção falhar |
| **3.7** | **Energia**: `STANDBY`/`WAKEUP` em PM, `powerdown-gpios`, `dvdd-supply`; a medida de `RDATAC` a 16 kSPS | — (comandos e pinos) | retomar sem reprogramar o cache (`regcache_sync`) e ver o teste de *resume* falhar |
| **3.8** | **Vários chips**: provedor de clock (CLK_EN), `DAISY_EN` gravado, a cadeia descrita no *binding* e recusada no driver, `start-gpios` (§5.6) | DAISY_EN, CLK_EN | tirar a recusa e ver uma cadeia de dois passar como um chip de 8 canais |

**A cobertura não depende de uso, e isso tem um custo de verificação que fica escrito.** As subfases
3.3 (SRB2), 3.6, 3.7 e 3.8, e o modo AC da 3.5, cobrem funções que **esta placa não usa nem pode
usar**: a chave SRB1 é manual e externa (`BRINGUP_AFE.md` §6.9), os GPIO não estão ligados a nada, e
há um chip só. Para elas o teste de host é a evidência máxima disponível, e o §13 diz isso campo a
campo, em vez de deixar "coberto" ser lido como "testado em silício".

**Registro**: §13 deste plano, campo a campo; `implementation_plan_iio_afe.md` para o que muda na
arquitetura do driver.

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
| A cobertura total multiplica a ABI customizada (de 8 atributos para ~20, metade por canal) e reabre a tensão do §3 | a ABI é a pergunta explícita da carta (§5.4); cada atributo customizado tem a alternativa padrão considerada escrita; uma série por capacidade (Fase 3) deixa o mantenedor recusar uma sem levar as outras |
| A ABI nova quebra a plataforma (§3.3) | a troca na tradução por ligação entra no mesmo `SRCREV`; `tests/derive` e `make check` são o portão |
| Funções sem silício nesta bancada (SRB2, GPIO, saída de clock, lead-off AC, PM) | registradas como "testado no host", nunca como "validado"; a daisy-chain já saiu do `v1` (§5.6) |
| `IIO_EV_DIR_FAULT_OPENWIRE` não existe no 6.6 da placa (§3.1.2) | **resolvido em 2026-10-17**: existe na mainline; a feature carrega o *backport* `66874e950a96`, que sai na submissão, e o `write_event_config` passa de `int` para `bool` lá |
| O driver expõe ILEAD_OFF de 6 e 24 µA, e em DC isso passa do limite de corrente auxiliar de paciente da IEC 60601-1 para corrente contínua (10 µA, citado de memória; conferir na norma) | política fora do kernel (§3): o driver é genérico e expõe o campo inteiro, e a tradução por ligação (`do_derive_device_options`) não oferece esses valores em DC - hoje não oferece nenhum valor de corrente, e uma linha nova de opções tem de respeitar isso |
| Constantes fora das tabelas transcritas (temperatura, MVDD) | `[SBAS499?]` até a Fase 0 do plano de bancada; nenhum `_scale`/`_offset` é submetido com elas pendentes |

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
| 3 | auditoria campo a campo (§3.1): PR 6 ✅ / 5 ⚙️ / 7 ⚠️ / 15 ❌; board 9 / 10 / 5 / 9, de 33 | **medido**, 7 defeitos (§3.2) | 2026-10-16 |
| 3.0 | base trazida da board; D2, D4, D5, D7 corrigidos | — | — |
| 3.1 | MUX por canal, `BIAS_MEAS`, `test_signal` DC; D1 corrigido | `test_signal` DC (`ad3a2ec3641b`), entrada de teste nunca sem o gerador (`17eccf84c5d6`), `BIAS_MEAS` seguindo o MUX e D1 (`89bed0807417`), MUX por canal (`e749f79fd717`). **Só host** | **feito**, 2026-10-17 |
| 3.2 | ganho por canal; `in_temp_*` e alimentação; constantes `[SBAS499?]` conferidas | ganho por canal (`c880fb1f02f6`; só host). `in_temp_*`, alimentação e as constantes em aberto: dependem do MUX por canal (3.1) e da Fase 0 do plano de bancada | 2026-10-17 |
| 3.3 | SRB1, SRB2 (SRB2: só host) | `montage` e `in_voltageN_srb2` (`11a174bc5f1f`); injeção do plano (máscara do SRB2 no bit 4) vista. **Só host** | **feito**, 2026-10-17 |
| 3.4 | bias por campo; BIASREF externo no DT | `biasref-supply` (`b82dc6df6759`, `89bed0807417`); derivação por canal e `bias_drive` = `off`/`on` (`8c0d40ff063a`); a política do referencial saiu do kernel com o `montage` (`11a174bc5f1f`). **Só host** | **feito**, 2026-10-17 |
| 3.5 | lead-off completo; tipo de evento decidido na mainline; D3 corrigido (AC: só host) | eletrodo de bias e D3 (`b27cb88553d0`); limiar, corrente e frequência (`a6edbf1cfde1`); *sense* e FLIP por canal, comparadores derivados, D2 (`308e6166ef98`); evento `fault`/`openwire` com o *backport* `66874e950a96`, e a palavra por amostra (`8b545d83bad2`). **Só host** | **feito**, 2026-10-17 |
| 3.6 | `gpiochip` (só host) | — | — |
| 3.7 | PM, PWDN, DVDD; `RDATAC` medido a 16 kSPS | — | — |
| 3.8 | CLK_EN como provedor de clock; `DAISY_EN` gravado; *status word* no autoteste; cadeia recusada (`8c3d602693f1`, `ceaa3f09e909`, `ed48db8df4c8`); `dt_binding_check` com duas violações injetadas e vistas; `W=1` arm64 e `checkpatch` 0/0/0 por commit; `tests/ads1299/feature.c`. **Só host**. `start-gpios` em aberto | **feito**, menos START | 2026-10-17 |
| 3 | nenhuma linha da §3.1 em ❌ ou ⚠️; ABI documentada = atributos customizados | — | — |
| 4 | autoteste passa com tolerância medida | — | — |
| 5 | cada divergência do irmão justificada por escrito | — | — |
| 6 | série na lore, Message-ID | — | — |
