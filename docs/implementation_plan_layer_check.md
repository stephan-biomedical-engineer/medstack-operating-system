# Plano — verificação automática das fronteiras entre camadas

> **Criado em:** 02/11/2026, a partir da revisão da introdução do TCC (`Dissertacao_PTBR/02-elementos-textuais/introducao.tex`).
> **Para quem:** o agente (ou a pessoa) que vai trabalhar neste repositório (`yocto_workspace`).
> **Estado:** **implementado em 02/11/2026** — níveis A e B, nível C pelo caminho 2 (caminho 1 testado, não implementado); resultados em `docs/RESULTS.md` §11, registro no §10. Falta o passo 9 no `CLAUDE.md` (patch preparado, aguardando o autor, §8) e o commit (passo 10). O "Estado atual" do §2 é a medição de antes, no `HEAD` = `0d7f5c5`, com a árvore de trabalho suja (ver §8).
> **Resultado esperado:** um alvo `make check-layers` (e, se possível, uma falha do próprio `bitbake`) que **reprova** a construção quando uma regra de camada é violada. A checagem precisa ter sido **vista falhando** contra violações injetadas e contra as duas violações históricas reais.

---

## 0. Por que isto existe (resumo em cinco linhas)

1. O TCC afirma que as fronteiras entre camadas são **impostas e verificadas durante a construção** (texto citado no §1).
2. Hoje isso **não é verdade**. O próprio `CLAUDE.md`, regra 1, diz: *"anywhere above it, that is the bug, and note that **no build will report it**"*.
3. Os objetivos do TCC são cobrados no capítulo 8 por uma tabela "objetivo → evidência → nível de evidência". Uma afirmação sem mecanismo não tem evidência.
4. Pelo método do próprio projeto, *"uma verificação que nunca foi vista falhando é apenas uma afirmação"*. Por isso a checagem só vale depois de reprovar violações injetadas.
5. A medição de 02/11 mostrou que a regra 9 do `CLAUDE.md` (contenção de *part number*) **já está quebrada no código commitado** (§2.4). É a prova de que uma regra sem checagem se degrada sozinha.

---

## 1. O que o TCC afirma, palavra por palavra

Estes trechos estão na dissertação e serão defendidos na banca. A implementação precisa torná-los verdadeiros, ou o texto precisa ser suavizado (§9).

**Lacuna (fim da seção 1.2, `introducao.tex`):**
> "…organizar o próprio Linux embarcado como plataforma em camadas para dispositivos médicos, com **fronteiras verificadas durante a construção**, reuso medido entre classes de equipamento e avaliação em hardware real."

**Pergunta de pesquisa (seção 1.2):**
> "…de modo que o reuso entre equipamentos possa ser medido, que **as fronteiras entre as camadas sejam impostas e verificadas durante a construção** e que o comportamento do sistema seja demonstrado no hardware de destino…"

**Objetivo geral (proposto para a seção 1.3):**
> "…na qual a aplicação clínica fique separada do sistema operacional e do hardware por **regras de fronteira verificáveis**…"

**Proposta aprovada (`docs/PROPOSTA_TCC.md`):**
- §5, objetivo 1: *"definir regras de fronteira cuja violação seja **detectável mecanicamente**"*.
- §6.1: *"Fronteira de camada verificável por máquina… Uma restrição arquitetural que nada verifica é uma recomendação."*

**O que vai ser cobrado:** quando a banca perguntar "como se verifica a fronteira?", a resposta tem de ser "este alvo, que reprova estes N tipos de violação, e aqui está ele reprovando cada uma". Não pode ser "uma regra no `CLAUDE.md`".

---

## 2. Estado atual (medido em 02/11/2026)

### 2.1 A pilha e as dependências declaradas

Conforme `CLAUDE.md` §"Architecture" e os `conf/layer.conf` reais:

| Camada | Papel | Prioridade | `LAYERDEPENDS` **real** (no arquivo) | O que o `CLAUDE.md` diz |
|---|---|---|---|---|
| `meta-med-app` | aplicações, HMI, perfis/imagens | 10 | `core meta-med-framework qt6-layer` | igual |
| `meta-med-framework` | MedFramework (C++) | 9 | `core meta-med-distro` | — |
| `meta-med-distro` | MedOS, política | 8 | `core openembedded-layer filesystems-layer rauc` | — |
| `meta-med-bsp` | adaptação de placa | 7 | `core openembedded-layer` | **"`LAYERDEPENDS` is `core` only"** (desatualizado) |
| `meta-med-afe-ads1299` | adjunta: um front-end | 7 | `core meta-med-framework` | **"declares `LAYERDEPENDS = "core"` only, and provides without consuming"** (desatualizado: desde 07/10 consome o ABI `med-amp-abi`; justificativa no próprio `layer.conf`, linhas 55–66) |
| `meta-med-desktop` | adjunta: desktop de bancada | 7 | `core openembedded-layer` | — |
| `meta-med-host` | adjunta: estação no Ubuntu | 7 | `core meta-med-app` | "the only one that consumes" |

> **Ação lateral:** corrigir no `CLAUDE.md` as duas descrições desatualizadas de `LAYERDEPENDS` (BSP e AFE). Não é o objetivo deste plano, mas a checagem nova vai ler os `layer.conf`, e o documento de referência não pode contradizer o que ela lê.

### 2.2 O que já existe e verifica alguma coisa

| Mecanismo | Onde | O que verifica | O que **não** verifica |
|---|---|---|---|
| `LAYERDEPENDS` | cada `conf/layer.conf` | que as camadas declaradas **existem** no `bblayers` | não impede uma receita de usar `DEPENDS`/`RDEPENDS` sobre receita de camada **não declarada**. O BitBake resolve provedores entre todas as camadas da pilha |
| `bitbake-layers show-depends` | citado no comentário de `meta-med-app/conf/layer.conf` (l. 22–26) | mostra as dependências declaradas | **manual**; não está em nenhum alvo do `Makefile` |
| `bb.fatal` em `med-uboot-env-image_1.0.bb` | `meta-med-bsp` | coerência interna do ambiente do U-Boot (PARTUUID, tamanhos, CRC, placeholders) | nada sobre camadas |
| trava do `MED_WKS_FILE` | `med-os.conf` + `meta-med-bsp` | um `MED_WKS_FILE` vazio derruba o build (com injeção de falha, `CLAUDE.md` l. 111) | não impede o distro de nomear um layout |
| `bbfatal` de placeholder em `rauc-conf.bbappend` | `meta-med-distro` | `@MED_*@` não substituído | nada sobre camadas |
| `do_med_check_kernel_config` | `meta-med-distro` | símbolos do fragmento presentes no `.config` final | nada sobre camadas |
| `grep` de *part number* (regra 9) | `CLAUDE.md`; `implementation_plan_ads1299.md` §3 | contenção de `ADS1299`/`MCP2210` | **manual**; não está em nenhum alvo, e hoje falha (§2.4) |
| `make check` (`scripts/med-check.py`) | QEMU | comportamento em execução (23 a 37 asserções) | nada estático |
| `make test` (`tests/*`) | host | framework, drivers, derivação, exportação | nada sobre camadas |

**Conclusão:** não existe hoje **nenhum** alvo, teste ou trava que reprove a violação de uma regra de camada.

### 2.3 Regra 1 (conhecimento de máquina acima do BSP): medição ingênua

Comando usado, em `meta-custom/`:

```sh
P='qemux86-64|qemux86|stm32mp25|stm32mp2|stm32mp257|grub|u-boot|uboot|extlinux|/dev/vda|/dev/sd[a-z]|mmcblk|ttyS[0-9]|ttySTM|ttyAMA|by-partlabel|by-partuuid'
/usr/bin/grep -rniE "$P" meta-med-distro meta-med-framework meta-med-app
```

> Use `/usr/bin/grep`: neste ambiente, `grep` é um *alias* para `ugrep`, que recusa partes da expressão por limite de complexidade.

| Camada | Linhas com ocorrência | Arquivos | Fora de comentário (`#`, `//`, `*`) |
|---|---|---|---|
| `meta-med-distro` | 39 | 10 | 5 |
| `meta-med-framework` | 6 | 5 | 1 |
| `meta-med-app` | 7 | 6 | 3 |

As 9 ocorrências fora de comentário, uma a uma, e a minha leitura de cada uma. **Confirme cada leitura antes de agir.**

| Arquivo:linha | Trecho | Leitura provável |
|---|---|---|
| `meta-med-distro/recipes-core/rauc/rauc-conf.bbappend:67` | `uboot\|barebox)` | **Zona cinzenta.** Ramifica pelo *backend* do RAUC (`MED_BOOTLOADER`), que é um valor vindo do BSP. Não escolhe bootloader, mas conhece nomes de família. Candidato a lista de exceções, com motivo |
| `meta-med-distro/recipes-core/rauc/rauc-conf/system.conf:81,86` | `device=/dev/disk/by-partlabel/med-root-a` / `-b` | **Permitido.** `med-root-a`/`med-root-b` são nomes de **política** que o próprio distro define (`meta-med-bsp/conf/layer.conf`: "policy (meta-med-distro) - two rootfs slots… named med-root-a/med-root-b"). O padrão deve proibir `by-partlabel/` seguido de nome que **não** seja `med-*` |
| `meta-med-distro/recipes-core/med-data-volume/files/med-data-provision.sh:17` | `DEV=/dev/disk/by-partlabel/med-data` | **Permitido** pelo mesmo motivo (`med-data` é nome de política) |
| `meta-med-distro/recipes-core/med-data-volume/files/med-data-provision.service:18` | `ConditionPathExists=/dev/disk/by-partlabel/med-data` | **Permitido**, idem |
| `meta-med-framework/.../include/MedicalUpdate.h:40` | `///< "/dev/disk/by-partlabel/med-root-a"` | Comentário Doxygen (`///<`), que o filtro não pegou. Nome de política: **permitido** |
| `meta-med-app/recipes-core/packagegroups/packagegroup-med-amp.bb:4` | `Cortex-A35 on the STM32MP257)` em `DESCRIPTION` | **Decidir.** É prosa num campo de metadado, não uma escolha. Reescrever sem nome de placa ("o núcleo de aplicação") ou pôr na lista de exceções |
| `meta-med-app/recipes-core/images/med-image-eeg.bb:3–4` | `Builds unchanged for qemux86-64 and for the STM32MP257` em `DESCRIPTION` | **Decidir**, idem. Repare na ironia: a frase afirma portabilidade nomeando as duas máquinas |

**Lição para o desenho:** um `grep` puro dá falso positivo (nomes de política `med-*`) e falso negativo (`///<` não foi tratado como comentário; um valor que vem por variável não aparece no texto). A checagem precisa de: (a) padrões mais precisos; (b) classificação de comentário por tipo de arquivo; (c) lista de exceções com motivo; (d) um segundo nível que olhe **variáveis expandidas** e o **grafo de dependências**, não só texto (§4).

### 2.4 Regra 9 (contenção de *part number*): **já quebrada**

O `CLAUDE.md`, regra 9, afirma: *"`grep -rni 'ads1299\|mcp2210' meta-custom/meta-med-{app,framework}` returns nothing at all"*, e acrescenta que os comentários entram nessa disciplina **de propósito**.

Medido em 02/11/2026 (as duas linhas estão **no `HEAD`**, não na árvore suja):

```
meta-med-app/recipes-apps/eeg-acquisition-service/eeg-acquisition-service_1.0.0.bb:362:  # writes (tests/ads1299 holds the driver against it), and that the
meta-med-app/recipes-apps/eeg-acquisition-service/eeg-acquisition-service_1.0.0.bb:403:  # carries (the ads1299-linux out-of-tree repository, and linux-med's
```

Os dois são comentários, um com o nome de um diretório de teste e outro com o de um repositório. Pela regra como está escrita, são violações. Há duas saídas, e a decisão é do autor:
- reescrever os dois comentários ("o teste do driver do conversor", "o repositório fora da árvore do driver");
- **ou** mudar a regra para permitir nomes de **repositório/diretório** e registrar isso no `CLAUDE.md`.

Seja qual for a escolha, isto entra no TCC como dado: **a regra ficou ~2 semanas quebrada no código commitado sem que nada acusasse**. É o mesmo formato das duas violações de agosto.

### 2.5 Regra 6 (aplicação sem segunda dependência): estado

`DEPENDS` reais, lidos das receitas:

| Receita | Camada | `DEPENDS` | Regra 6 |
|---|---|---|---|
| `eeg-acquisition-service_1.0.0.bb` | app | `med-framework-api` | ok |
| `eeg-hmi-gui_1.0.0.bb` | app | `qtbase qtdeclarative qtdeclarative-native med-framework-api` | ok (Qt é permitido) |
| `edfbrowser_2.15.bb` | app | `qtbase` (+ `RDEPENDS qtwayland`) | **terceiro, empacotado sem modificação**. A regra 6 fala de `eeg-acquisition-service` e `eeg-hmi-gui`. Precisa de classificação explícita: "aplicação de terceiros, fora da regra 6" |
| `med-review_1.0.bb` | app | — (`RDEPENDS edfbrowser`) | idem |
| `med-framework-api_1.0.0.bb` | framework | `systemd openssl med-amp-abi` | ok: `libsystemd` + `libcrypto` + o ABI da própria camada |

Hoje a regra 6 está respeitada, mas só porque ninguém a quebrou. Nada a impõe.

### 2.6 `bbappend`s das camadas MedPlatform

```
meta-med-distro/recipes-kernel/linux/linux-%.bbappend
meta-med-distro/recipes-graphics/wayland/weston-init.bbappend
meta-med-distro/recipes-core/rauc/rauc-conf.bbappend
meta-med-distro/recipes-core/seatd/seatd_%.bbappend
meta-med-distro/recipes-core/systemd/systemd_%.bbappend
meta-med-bsp/dynamic-layers/stm-st-stm32mp/recipes-kernel/linux/linux-stm32mp_%.bbappend
meta-med-bsp/dynamic-layers/stm-st-stm32mp/recipes-bsp/u-boot/u-boot-stm32mp_%.bbappend
meta-med-afe-ads1299/dynamic-layers/stm-st-stm32mp/recipes-extended/external-dt/external-dt_%.bbappend
```

Todos apontam para receitas de terceiros (poky, meta-oe, meta-rauc, meta-st). **Nenhum** aponta para receita de uma camada MedPlatform superior. Ok hoje, sem nada que imponha.

### 2.7 As duas violações históricas, preservadas no git

O commit **`dd58dd6`** (pai de `85ca4da`, *"feat(bsp): add meta-med-bsp…"*, 17/08/2026) ainda contém as duas violações que motivaram o `meta-med-bsp`:

```
meta-custom/meta-med-distro/recipes-core/images/med-image-base.bb:89:  QB_KERNEL_ROOT = "/dev/vda2"
meta-custom/meta-med-distro/wic/med-partitions.wks:29:  bootloader ... --append="rootwait console=ttyS0,115200 console=tty0"
meta-custom/meta-med-distro/wic/med-partitions.wks:40:  part /boot --source bootimg-efi --sourceparams="loader=grub-efi" ...
```

É o **caso de teste mais valioso** do plano: rodar a checagem nova sobre `dd58dd6` tem de reprovar exatamente essas linhas (§5.2). Dá para escrever no TCC: *"a checagem, aplicada retroativamente, teria reprovado as duas violações que só foram descobertas quando um segundo alvo precisou de uma resposta diferente."*

---

## 3. As regras, escritas para serem verificáveis

Cada regra tem um identificador estável, porque o TCC vai citá-las. A coluna "Nível" diz qual mecanismo do §4 a verifica.

Postos das camadas: `bsp` = 1, `distro` = 2, `framework` = 3, `app` = 4. As adjuntas (`afe`, `desktop`, `host`) ficam **fora** da pilha e têm regras próprias. Camadas de terceiros (poky, meta-oe, meta-st, meta-rauc, meta-qt6, meta-security) são "externas".

| ID | Regra | De onde vem | Nível |
|---|---|---|---|
| **F1** | `LAYERDEPENDS` de uma camada da pilha só pode citar camadas de posto **menor** ou externas. O BSP não cita nenhuma camada MedPlatform | `CLAUDE.md` §Architecture | A |
| **F2** | Nenhuma camada da pilha cita uma adjunta em `LAYERDEPENDS`. Uma adjunta só consome o que está listado para ela: `afe` → `med-amp-abi` (ABI só de cabeçalho); `host` → `meta-med-app`; `desktop` → nada da pilha | `CLAUDE.md` regra 1; `meta-med-afe-ads1299/conf/layer.conf` l. 55–66 | A |
| **F3** | Nenhum `.bbappend` de uma camada MedPlatform pode apontar para receita de camada MedPlatform de posto **maior**. `bsp` e adjuntas só fazem `bbappend` de receitas externas | `CLAUDE.md` (BSP: *"there must never be a bbappend here onto a recipe from a higher layer"*) | A (pelo nome) + B (`show-appends`) |
| **F4** | Toda aresta de dependência efetiva (`DEPENDS`, `RDEPENDS`, `RRECOMMENDS`, `do_*[depends]`) de uma receita de posto *p* para uma receita MedPlatform de posto maior que *p* é proibida. Arestas **para** adjuntas a partir da pilha só são permitidas nas **imagens**, e só por variável-gancho declarada (`MED_BSP_INSTALL`, `MED_AFE_INSTALL*`, `MED_AMP_FIRMWARE`, `MED_GPU_PACKAGES`, `MED_GUI_COMPOSITOR`) | `CLAUDE.md` §Architecture; padrão de ganchos em `meta-med-bsp/conf/layer.conf` | B |
| **F5** | Nas receitas de **aplicação própria** (`eeg-acquisition-service`, `eeg-hmi-gui`), `DEPENDS` ⊆ {`med-framework-api`} ∪ {Qt: `qtbase`, `qtdeclarative`, `qtdeclarative-native`, …}. No framework, `DEPENDS` ⊆ {`systemd`, `openssl`, `med-amp-abi`}. Aplicações de terceiros (`edfbrowser`, `med-review`) ficam marcadas como tal, numa lista explícita | `CLAUDE.md` regra 6 | A (texto) + B (valor expandido) |
| **F6** | Nenhum nome de máquina, bootloader, console serial, offset de partição ou nó de dispositivo acima de `meta-med-bsp`. Exceção estrutural: nomes de partição de **política** (`med-root-a`, `med-root-b`, `med-data`, `med-*`) | `CLAUDE.md` regra 1 | A (texto) + B (variáveis expandidas da imagem) |
| **F7** | Nenhum *part number* (`ADS1299`, `ADS129x`, `MCP2210`) em `meta-med-app` e `meta-med-framework`, **comentários incluídos**. Em `meta-med-bsp` só os símbolos Kconfig (`CONFIG_TI_ADS1299`, `CONFIG_HID_MCP2210`) e o código que os lê. Em `meta-med-distro` só nome de documento e nome da camada adjunta | `CLAUDE.md` regra 9 | A |
| **F8** *(opcional)* | Os drivers do framework têm nome de **transporte** (`simulated`, `rpmsg`, `iio`), nunca de componente | `CLAUDE.md`, descrição do `meta-med-framework` | A |
| **F9** | Nenhum caminho numerado de dispositivo (`/dev/mmcblkNpM`, `mmcblkN`, `rpmsgN`, `remoteprocN` como identidade) **em nenhuma camada, BSP incluído, nem em comentário** | `CLAUDE.md`, parágrafo do primeiro boot: *"device numbering is a race, so no `/dev/mmcblkNpM` path belongs anywhere in this repository, not even in a comment"* | A |

**F9 já está quebrada (medido em 02/11/2026)**, só no `meta-med-bsp` e só em comentários que **registram uma medição** ("by-partlabel/bootfs pointed at mmcblk0p6… ours was mmcblk1p8"):

```
meta-med-bsp/conf/layer.conf:419-420
meta-med-bsp/wic/med-partitions-stm32mp2.wks.in:119-120, 150
meta-med-bsp/recipes-bsp/med-uboot-env/med-uboot-env-config_1.0.bb:62-63 (saída de readlink citada)
```

Aqui a regra e a prática se contradizem: o comentário cita o número **justamente** para mostrar que ele mudou. Decisão D6 (§9).

**O que conta como "acima do BSP" para F6 e F7:** `meta-med-distro`, `meta-med-framework` e `meta-med-app`, **incluindo** `files/` (scripts `.sh`, unidades `.service`, `.conf`, `.wks`, código C++). As adjuntas não entram em F6, porque podem ser específicas de placa por definição, mas entram em F2 e F4.

**Decisão a registrar (F6 e comentários):** a regra 9 inclui comentários de propósito. Para F6, 34 das 39 ocorrências no distro são comentários que **explicam** a fronteira (ex.: "naming a bootloader here is forbidden"). Recomendação: **F6 reprova só fora de comentário e relata os comentários como contagem informativa; F7 reprova também comentário**, como a regra 9 já diz. Registre a escolha no `CLAUDE.md`.

---

## 4. Desenho da implementação

Três níveis. **A** é obrigatório e barato. **B** é o que permite dizer "o grafo de dependências efetivo respeita a pilha". **C** é o que torna literal a frase "verificadas durante a construção". Recomendo fazer **A e B** e investigar **C**, decidindo pelo custo.

### 4.1 Nível A — checagem estática, sem BitBake

- **Arquivo:** `scripts/med-layer-check.py`, Python 3 puro, só biblioteca padrão (o mesmo estilo de `scripts/med-check.py`, com docstring "WHY THIS EXISTS").
- **Alvo:** `make check-layers`, que roda em segundos, fora do container KAS.
- **Encaixe no laço:** acrescentar ao alvo `test` do `Makefile`, depois de `tests/derive`, porque `test` é o laço rápido que roda sempre. Opcional: também ao `parse`.
- **Entrada:** a árvore `meta-custom/meta-med-*`. Com `--root <dir>`, para poder rodar numa *worktree* de um commit antigo (§5.2).
- **O que faz:**
  1. **F1/F2:** lê cada `conf/layer.conf`, extrai `LAYERDEPENDS_<coleção>` (atenção a continuação de linha e `+=`) e compara com a matriz de postos.
  2. **F3:** lista todos os `*.bbappend` de cada camada MedPlatform. Para cada um, procura se existe um `.bb` de mesmo PN (com `%` como curinga) numa camada MedPlatform de posto maior. Encontrou → reprova.
  3. **F5 (textual):** nas receitas listadas, extrai `DEPENDS`/`RDEPENDS:${PN}` com `=`, `+=`, `:append`, juntando as continuações `\`, e compara com as listas permitidas. Um valor com `${…}` que não dê para resolver textualmente **não** passa em silêncio: vira aviso "verificado no nível B".
  4. **F6:** procura os padrões por tipo de arquivo, com classificação de comentário por linguagem:
     - BitBake/shell/`.conf`/`.wks`/`.service`/`.cfg`: comentário = linha cujo primeiro caractere não branco é `#`;
     - C/C++ (`.h`, `.cpp`, `.c`): `//`, `///`, `///<`, `/* … */`, `*` de continuação;
     - QML/JS: `//`, `/* */`.
     - `DESCRIPTION`/`SUMMARY` contam como **código** (são metadados publicados no pacote), a menos que a decisão do §2.3 diga o contrário.
  5. **F7/F8:** `grep` sem diferenciar maiúsculas, com as exceções do §3.
- **Padrões de F6 (ponto de partida; refine e documente cada um):**
  - máquinas: `qemux86-64`, `qemux86`, `qemuarm64`, `stm32mp\d+`, `stm32mp25-disco`, `raspberrypi\d*`. **Melhor ainda:** gerar a lista de máquinas a partir dos `conf/machine/*.conf` de todas as camadas em `layers/`, para que uma placa nova entre sozinha;
  - bootloaders: `grub(-efi)?`, `u-?boot`, `barebox`, `extlinux`, `systemd-boot`, `bootimg-efi`;
  - consoles: `ttyS\d`, `ttySTM\d`, `ttyAMA\d`, `ttymxc\d`, `hvc\d`;
  - nós de dispositivo: `/dev/(vd|sd|hd)[a-z]\d*`, `mmcblk\d`, `nvme\d`, `/dev/disk/by-(partlabel|partuuid|label|uuid)/(?!med-)`;
  - offsets: `--offset`, `--align` com número em arquivo `.wks` (um `.wks` acima do BSP já é suspeito por si só: **qualquer** `*.wks*` fora de `meta-med-bsp` reprova).
- **Saída:** uma linha por violação, no formato `F6 meta-med-distro/…/med-image-base.bb:89: '/dev/vda2' (nó de dispositivo) — só meta-med-bsp pode nomear isto`. No fim, um resumo por regra (`F1 ok`, `F6 1 violação`, …) e o código de saída ≠ 0 se houver qualquer violação.

### 4.2 Lista de exceções (o mesmo rigor do `EXPECTED_FAILED`)

`scripts/med-check.py` já tem a filosofia certa: *"an allowlist nobody justifies is just a muted alarm"*, e *"An allowlist entry whose justification has expired is a muted alarm"*. Aplique a mesma coisa aqui.

- **Onde:** um dicionário no topo de `scripts/med-layer-check.py` (preferível a um arquivo separado: fica junto do código que o lê e aparece no mesmo *diff*).
- **Formato de cada entrada:** `(regra, caminho, trecho literal, motivo, data)`. O motivo é obrigatório e não pode ser vazio.
- **Exceção vencida reprova:** se o trecho de uma entrada não aparece mais no arquivo, a checagem **falha** com "exceção sem uso; remova". Senão a lista só cresce.
- **Sem curinga de diretório:** cada entrada é um arquivo e um trecho, nunca "tudo em `meta-med-distro/recipes-core/rauc/`".
- **Candidatas iniciais** (§2.3): `rauc-conf.bbappend` `uboot|barebox` (F6, motivo: ramifica pelo *backend* do RAUC que o BSP escolhe, não escolhe bootloader). Decidir se as `DESCRIPTION` de `med-image-eeg.bb` e `packagegroup-med-amp.bb` viram exceção ou são reescritas. **Recomendo reescrever:** é mais limpo e não custa nada.

### 4.3 Nível B — grafo de dependências efetivo, com BitBake

O nível A lê texto; o B lê o que o BitBake **resolveu**. É o B que pega uma dependência que chega por variável (`DEPENDS += "${MED_ALGUMA_COISA}"`) ou por `packagegroup`.

- **Alvo:** `make check-layers-graph`, dentro do container KAS (`$(KAS) shell $(BOARD_CFG) -c …`), como os outros alvos.
- **Perfis a cobrir:** `kas/project-eeg-qemu.yml`, `kas/project-eeg-stm32mp2.yml` com `LINK=amp`, `usb`, `spi` e `simulated`, e `kas/project-tomograph.yml`. O `project-eeg-host.yml` entra com a regra própria do `meta-med-host`, que consome `meta-med-app`. O conjunto de dependências muda por link (`MED_EEG_LINK` decide o que o AFE instala), então rodar só um perfil não basta.
- **Passos:**
  1. `bitbake -g <imagem>` gera `task-depends.dot` e `pn-buildlist`. Use só `bitbake -g`; não precisa construir.
  2. Mapear cada PN para a camada que forneceu a receita escolhida. Opções, da mais robusta para a menos:
     - `bitbake-layers show-recipes -f` → caminho do `.bb` escolhido → camada pelo prefixo de diretório (ou pelo `BBFILE_PATTERN_<coleção>`);
     - `bitbake -e <pn> | grep ^FILE=` por receita (lento; só para os PNs da `pn-buildlist`).
  3. Para cada aresta `A → B` do `.dot` (tarefa `do_*` de A depende de tarefa de B), aplicar F4 com os postos de A e B.
  4. **F3 confirmado:** `bitbake-layers show-appends` → para cada receita MedPlatform, quais `bbappend` foram aplicados e de que camada vieram.
  5. **F5/F6 sobre valores expandidos:** `bitbake -e eeg-acquisition-service` e `bitbake -e eeg-hmi-gui` → `DEPENDS` e `RDEPENDS:${PN}` expandidos, comparados com as listas de F5. `bitbake -e <imagem>` → procurar padrões de F6 nas variáveis **definidas** por camadas acima do BSP. O `bitbake -e` mostra em comentário o arquivo de cada atribuição (`# set <arquivo>:<linha>`), e é isso que permite dizer **de qual camada** veio o valor.
- **Saída:** o mesmo formato do nível A, com o perfil: `F4 [eeg-stm32mp2 LINK=usb] eeg-acquisition-service (app) → hid-mcp2210 (afe): aresta da pilha para adjunta fora de imagem`.
- **Cuidado:** arestas para receitas **externas** (poky, Qt, systemd) são normais e não entram em F4. F4 só olha arestas entre camadas MedPlatform.

### 4.4 Nível C — falha do próprio `bitbake` (investigar)

Para que "verificadas **durante a construção**" seja literal, a violação tem de derrubar o `bitbake`, não só um alvo à parte. Caminhos a investigar; **não assuma que funcionam, teste**:

1. **Classe com tratador de eventos**, por exemplo `meta-med-distro/classes/med-layer-guard.bbclass`, herdada pelo `med-os.conf` via `INHERIT += "med-layer-guard"`.
   - Um tratador de `bb.event.RecipeParsed` recebe o datastore `d` de cada receita: `FILE`, `DEPENDS`, `RDEPENDS:${PN}` e as variáveis expandidas. Com isso dá para impor **F5** e **F6** por receita no *parse*. A camada da receita sai de `FILE` comparado com os `BBFILE_PATTERN_<coleção>`.
   - Limite: no *parse* o BitBake ainda não resolveu provedores, então F4 completo (camada do **alvo** da aresta) não sai daqui. Pode sair de um tratador de `bb.event.DepTreeGenerated`, mas este só dispara com `-g`. Investigue se há evento pós-resolução que dispare num `bitbake <imagem>` normal.
   - Ironia a resolver no texto: a classe que impõe a fronteira mora no `meta-med-distro`, que é parte da pilha. Isso é aceitável (é política), mas a classe **não pode** nomear máquina nenhuma; ela mesma precisa passar em F6.
2. **Tarefa na imagem**, por exemplo `do_med_layer_check` adicionada em `med-image-base.bb` antes de `do_rootfs`, que roda o script do nível A sobre `${MED_LAYERS_ROOT}`. Simples e literal ("a imagem não é gerada se a fronteira for violada"), mas cobre só o nível A.
3. **Mínimo aceitável**, se 1 e 2 se mostrarem caros: o nível A dentro de `make stm32`, `make qemu`, `make tomograph` e `make host` como pré-requisito. Assim nenhum alvo de construção do projeto roda sem a checagem. Nesse caso, o texto do TCC deve dizer "verificadas a cada construção, por um alvo que a precede", e não "pelo BitBake".

**Recomendação:** fazer o caminho 2, ou o 3 como piso, e documentar o 1 como investigado, com o resultado. Não gaste mais que um dia no 1.

---

## 5. Validação: a checagem precisa ser vista falhando

Esta seção **não é opcional**. É o protocolo do próprio projeto, e é o que vira evidência no capítulo 8.

### 5.1 Injeção de falha, uma por regra

Para cada linha: aplicar a mudança numa árvore limpa, rodar a checagem, **confirmar que ela reprova com a mensagem certa e na linha certa**, reverter e confirmar que volta a passar. Guarde a saída de cada rodada (§7).

| # | Regra | Violação injetada | Nível que deve pegar |
|---|---|---|---|
| I1 | F1 | `LAYERDEPENDS_meta-med-distro += "meta-med-app"` | A |
| I2 | F1 | `LAYERDEPENDS_meta-med-bsp += "meta-med-distro"` | A |
| I3 | F2 | `LAYERDEPENDS_meta-med-framework += "meta-med-afe-ads1299"` | A |
| I4 | F3 | criar `meta-med-bsp/recipes-apps/eeg-acquisition-service/eeg-acquisition-service_%.bbappend` (vazio) | A e B |
| I5 | F3 | criar em `meta-med-afe-ads1299` um `med-framework-api_%.bbappend` | A e B |
| I6 | F4 | em `eeg-acquisition-service_1.0.0.bb`: `RDEPENDS:${PN} += "kernel-module-hid-mcp2210"` (aresta app → adjunta fora de imagem) | B |
| I7 | F4 | dependência **por variável**: `MED_X = "med-uboot-env-config"` num `.conf` do distro e `RDEPENDS:${PN} += "${MED_X}"` no framework. O texto não mostra o nome; só o B pega | B (A deve no máximo avisar) |
| I8 | F5 | `DEPENDS += "libiio"` em `eeg-acquisition-service_1.0.0.bb` (o exemplo exato que a regra 6 cita) | A e B |
| I9 | F5 | `DEPENDS += "zlib"` em `med-framework-api_1.0.0.bb` | A e B |
| I10 | F6 | reintroduzir `QB_KERNEL_ROOT = "/dev/vda2"` em `meta-med-distro/recipes-core/images/med-image-base.bb` | A e B |
| I11 | F6 | criar `meta-med-distro/wic/x.wks` com `--sourceparams="loader=grub-efi"` | A |
| I12 | F6 | `console=ttySTM0` numa unidade `.service` em `meta-med-app/.../files/` | A |
| I13 | F6 | `/dev/mmcblk1p8` num `.cpp` do framework, **fora** de comentário | A |
| I14 | F6 (controle negativo) | `/dev/disk/by-partlabel/med-data` num script do distro | **deve passar**: é nome de política |
| I15 | F6 (controle negativo) | `# naming a bootloader such as grub here is forbidden` (comentário) | **deve passar** e aparecer só na contagem informativa |
| I16 | F7 | `// an ADS1299 has 8 channels` num cabeçalho do framework (comentário!) | A |
| I17 | F7 | `MCP2210` numa `DESCRIPTION` de `meta-med-app` | A |
| I18 | exceções | apagar a linha que uma exceção cobre e deixar a exceção | A: deve reprovar com "exceção sem uso" |
| I19 | exceções | criar exceção com motivo vazio | A: deve reprovar |

Os casos I14 e I15 são o **controle negativo**: uma checagem que reprova tudo é tão inútil quanto uma que não reprova nada.

### 5.2 Replay histórico (o caso mais forte para o TCC)

```sh
git worktree add /tmp/med-dd58dd6 dd58dd6
python3 scripts/med-layer-check.py --root /tmp/med-dd58dd6/meta-custom
git worktree remove /tmp/med-dd58dd6
```

**Critério de aceitação:** reprova, no mínimo:
- `meta-med-distro/recipes-core/images/med-image-base.bb:89` (`/dev/vda2`);
- `meta-med-distro/wic/med-partitions.wks:29` (`ttyS0`);
- `meta-med-distro/wic/med-partitions.wks:40` (`grub-efi`, e o `.wks` estar fora do BSP).

Registre a saída inteira. Se reprovar **outras** coisas nesse commit, liste-as também: é informação sobre o que mais estava errado em agosto.

> O nível B não roda em `dd58dd6` sem reconstruir o ambiente daquela época (`layers/` em outro estado). Não tente; o replay é só do nível A.

### 5.3 Árvore atual limpa

Depois de corrigir ou isentar o que o §2 encontrou (as duas linhas da regra 9, as duas `DESCRIPTION`, o `uboot|barebox`), `make check-layers` e `make check-layers-graph` têm de **passar** em todos os perfis do §4.3. É esse o estado que vai para o TCC.

---

## 6. Ordem de trabalho sugerida

1. **Ler** `CLAUDE.md` (regras 1, 6 e 9 e a seção Architecture), os `conf/layer.conf` das sete camadas, `scripts/med-check.py` (docstring e `EXPECTED_FAILED`) e este plano inteiro.
2. **Reproduzir** as medições do §2.3–§2.5 e conferir os números. Se divergirem, o plano está desatualizado: anote a diferença no §10.
3. **Decidir com o autor** (§9): comentários em F6; as `DESCRIPTION`; a regra 9 para nomes de repositório.
4. **Escrever** o nível A, com a injeção I1–I19 **antes** de corrigir a árvore. Primeiro ver a checagem reprovar a árvore atual (§2.4 é uma violação real); depois injetar.
5. **Replay** em `dd58dd6` (§5.2).
6. **Corrigir/isentar** a árvore atual até passar (§5.3).
7. **Escrever** o nível B e rodar os perfis do §4.3. Repetir I4–I10 contra o B.
8. **Investigar** o nível C (§4.4) e implementar o caminho 2 ou o 3.
9. **Atualizar** `CLAUDE.md` (regras 1, 6 e 9 passam a dizer qual alvo as verifica, e as duas descrições de `LAYERDEPENDS` desatualizadas são corrigidas) e o `docs/RESULTS.md`, com uma seção nova.
10. **Commit** só quando o autor pedir, num ramo próprio (ex.: `feature/layer-check`). Ver §8.

---

## 7. O que entregar de volta para o TCC

O texto do TCC vai usar estes números e artefatos. Junte tudo num lugar só: uma seção nova no `docs/RESULTS.md`, com data e o comando de cada número.

1. **Lista final de regras** (F1–F8) como foram implementadas, com qualquer mudança em relação ao §3 explicada.
2. **Tabela de injeção:** para cada I1–I19, a regra, a violação, o nível que pegou, a mensagem exata da saída e "reprovou/passou como esperado". Vira tabela no capítulo de Resultados.
3. **Replay de `dd58dd6`:** a saída completa. Vira uma frase forte no capítulo de Resultados e na seção 1.2.
4. **Contagens da árvore atual:** violações encontradas antes da correção (incluindo as 2 da regra 9), exceções finais com motivo, ocorrências em comentário (contagem informativa de F6).
5. **Tempo de execução:** nível A (segundos) e nível B por perfil.
6. **Onde a checagem roda:** quais alvos do `Makefile` a chamam e se ela derruba o `bitbake` (nível C) ou só o alvo.
7. **O que ela não pega:** lista honesta. Exemplos prováveis: uma dependência em tempo de execução que não passa pelo BitBake (um `dlopen`, um caminho montado em tempo de execução); conhecimento de máquina codificado sem nenhum dos padrões; um valor injetado por um arquivo KAS de projeto. Vai para as Limitações (capítulo 8).

---

## 8. Cuidados com o estado do repositório (02/11/2026)

- **Árvore suja.** Há mudanças não commitadas em `CLAUDE.md`, `Makefile`, `meta-med-afe-ads1299/conf/layer.conf`, `eeg-acquisition-service_1.0.0.bb` e `med-stm32mp-drivers.cfg`, e arquivos não rastreados em `meta-med-afe-ads1299/recipes-kernel/`. São de outro trabalho (driver `ti-ads1299` fora da árvore). **Não reverta, não faça `git stash` sem combinar e não misture esses arquivos no commit deste plano.** Ao editar `CLAUDE.md` ou `Makefile`, que já têm mudanças pendentes, mostre o *diff* ao autor antes.
- **Chaves privadas soltas.** `MOK.der` e **`MOK.priv`** estão na raiz do `yocto_workspace`, não rastreados e **não ignorados**. Nunca use `git add -A` ou `git add .`. Adicione arquivo por arquivo.
- **`grep` é `ugrep`** neste ambiente. Use `/usr/bin/grep` nos scripts e nos comandos manuais, ou implemente a busca em Python (melhor para o script).
- **Injeções de falha:** faça numa *worktree* separada (`git worktree add`) ou garanta a reversão de cada uma. Uma injeção esquecida na árvore é exatamente o tipo de defeito que este plano quer evitar.
- **Commit só quando pedido**, num ramo próprio.

---

## 9. Decisões que são do autor (perguntar antes de implementar)

| # | Pergunta | Recomendação |
|---|---|---|
| D1 | F6 reprova comentários? | Não: reprova fora de comentário e relata os comentários como contagem. F7 continua reprovando comentário, como a regra 9 já diz |
| D2 | As `DESCRIPTION` de `med-image-eeg.bb` e `packagegroup-med-amp.bb` que nomeiam máquinas | Reescrever sem nome de máquina |
| D3 | As duas linhas da regra 9 (`tests/ads1299`, `ads1299-linux`) | Reescrever os comentários e manter a regra como está |
| D4 | Até onde ir no nível C | Caminho 2 (tarefa na imagem) ou 3 (pré-requisito dos alvos). Caminho 1 só como investigação |
| D5 | O `meta-med-host` (consome `meta-med-app`) e o `meta-med-desktop` entram na checagem? | Sim, com as regras próprias de F2/F4. Ficam fora da métrica de reuso, como já está |
| D6 | F9: os comentários do BSP que citam `mmcblk0p6`/`mmcblk1p8` como registro de medição | Ajustar a regra do `CLAUDE.md` para "nenhum caminho numerado **usado como endereço**; citá-lo num registro de medição é permitido com o marcador `measured:`", e a checagem aceita só linhas com esse marcador. A alternativa (reescrever os comentários como "a partição 6 do eMMC de fábrica") também serve |
| D7 | Os arquivos `kas/*.yml` entram em F6? | Não: o `CLAUDE.md` diz que eles **devem** nomear a máquina (`machine:`) e carregar as variáveis por alvo. São metadados de projeto, fora da pilha. Mas declare isso na saída da checagem ("kas/ fora do escopo, por definição"), para que a ausência não pareça esquecimento |

**Decidido pelo autor em 02/11/2026:** D1, D2, D3, D5 e D7 como recomendado; D4 = caminho 2, com o 1 investigado por no máximo um dia; D6 = marcador `measured:` nos comentários que registram uma medição, os outros reescritos sem número, e o padrão `/dev/rpmsg0` do driver `rpmsg` (`MedicalDevice.cpp:717`, a única ocorrência fora de comentário) substituído por recusa de endereço vazio. Novo, das divergências do §10: **D8** (F7 no `meta-med-distro`) = reescrever os comentários que nomeiam a peça ou o símbolo; a regra 9 fica como está e passa a ser imposta no distro também.

**Se o autor desistir da checagem automática** (decisão (b) da revisão do TCC), o texto da dissertação tem de mudar de "impostas e verificadas durante a construção" para "declaradas e conferidas por inspeção". Este plano deixa de ser necessário nesse caso, mas os achados do §2.4 (regra 9 quebrada) e do §2.1 (`CLAUDE.md` desatualizado) continuam valendo e devem ser corrigidos.

---

## 10. Registro de execução

*(Preencha conforme o trabalho for feito: data, o que foi feito, saída relevante, divergências com este plano.)*

| Data | Passo | Resultado | Observação |
|---|---|---|---|
| 02/11/2026 | Medição inicial (§2) | 9 ocorrências de F6 fora de comentário; F7 quebrada (2 linhas no `HEAD`); F9 quebrada (3 arquivos do BSP, só comentários); `CLAUDE.md` desatualizado em 2 `LAYERDEPENDS`; `dd58dd6` contém as 2 violações históricas | feita a partir da revisão do TCC, sem alterar o repositório |
| 02/11/2026 | §6 passos 1–2: leitura e reprodução | §2.1 (`LAYERDEPENDS`), §2.3 (39/10/5, 6/5/1, 7/6/3 e as 9 linhas), §2.4 (as 2 linhas, no `HEAD`), §2.5 e §2.6 (8 `bbappend`) **conferem** | — |
| 02/11/2026 | Divergência: F9 é mais larga que o §3 diz | O padrão `mmcblk[0-9]\|rpmsg[0-9]\|remoteproc[0-9]` no `HEAD` acha 17 linhas em **4 camadas**, não 3 arquivos do BSP: BSP 11 (o §3 omitiu `med-uboot-env-config_1.0.bb:57,65` e `.wks.in:115-116`), framework 3, app 1, adjunta 2. **Uma está fora de comentário:** `MedicalDevice.cpp:717`, `endpoint.address = config_.address.empty() ? "/dev/rpmsg0" : …` — um nó numerado como valor padrão no framework (F9 e F6). Nenhum perfil o usa: `kas/project-eeg-stm32mp2.yml:92` dá `/dev/med-amp0` ao `amp`. As outras (`MedicalDevice.h:136`, `MedicalIPC.h:41`, `eeg-acquisition.service:86`, `90-med-amp.rules:18`, `med-m33-load.sh:10`) são comentários | o padrão de F6 do §2.3 não inclui `rpmsg`, por isso a medição inicial não viu |
| 02/11/2026 | Divergência: F7 no distro | A regra 9 diz que no `meta-med-distro` só aparecem o nome de um documento e o da camada adjunta. No `HEAD` são 8 linhas, todas comentário, e várias nomeiam a peça ou o símbolo: `linux-%.bbappend:44,81,91` (`CONFIG_TI_ADS1299`, "o MCP2210 só está no…", `kernel-module-hid-mcp2210`), `med-kernel-bench.cfg:20` (`ti-ads1299`), `med-kernel-features.cfg:158,164` (`CONFIG_TI_ADS1299`); `:115,188` são nome de documento/camada | o §3 (F7) repete a regra 9 sem ter medido o distro |
| 02/11/2026 | Divergência menor | `scripts/med-check.py` existe e tem `EXPECTED_FAILED` como o §4.2 descreve | nenhuma |
| 02/11/2026 | §6 passo 3: decisões | D1-D7 como recomendado, D4 = caminho 2 + investigar 1, D8 novo (F7 no distro: reescrever) | registradas no §9 |
| 02/11/2026 | §6 passo 4: nível A | `scripts/med-layer-check.py` reprova a árvore: F6 3, F7 8, F9 26. Achado novo: `MedicalDevice.cpp` também caía em `/dev/iio:device0` (mesma decisão D6 aplicada) | F6 do plano ampliado: SoC (`stm32mp\d+`), máquinas lidas de `layers/*/conf/machine/`; F9 inclui `iio:deviceN` e `hidrawN` |
| 02/11/2026 | §5.1 injeção | `tests/layer-check` (permanente, em `make test`): 26 casos, 0 falhas. **I3** também dá F7 (o nome da adjunta carrega o *part number*); **I7** como o plano o escreveu é pego por F6 no nível A (o nome `med-uboot-env-config` contém `uboot`), então foi acrescentado o **I7b** (framework → `eeg-hmi-gui` por variável), o caso puro de F4 | +7 casos: I3b, I7b, I20 (F8), I21/I22 (F9 e o marcador), I23, I24 |
| 02/11/2026 | §5.2 replay | `dd58dd6` reprova as 3 linhas do critério e o `.wks`; acha também `by-partlabel/esp` e `/dev/rpmsg0` em código (unidade, `eeg.conf`, framework) | F6 9, F9 6, EX 1 (esperado) |
| 02/11/2026 | §6 passo 6: correção | D2, D3, D8 reescritos; D6: 6 comentários de medição ganharam `measured:`, 3 reescritos; `rpmsg`/`iio` recusam endereço vazio (teste novo em `tests/framework/test_device.cpp`, visto falhando). Exceção única: `rauc-conf.bbappend`. Árvore passa | regra 9 no BSP: aceitos também nomes de módulo do kernel (`ti-ads1299`, `hid-mcp2210`), nome da adjunta e de plano — o `med-stm32mp-drivers.cfg` é de outro trabalho e não foi tocado |
| 02/11/2026 | §6 passo 7: nível B | `scripts/med-layer-graph.py`, `make check-layers-graph`: 7 perfis, 0 violações; injeção I4-I10 contra o B: 8 achados, todos esperados (I7b só o B pega) | 2 falsos positivos do analisador corrigidos (fecho do `do_rootfs`; atribuição de F6 a uma referência vazia). `simulated` na placa não é um `LINK` aceito; ficou só nos perfis host |
| 02/11/2026 | §6 passo 8: nível C | caminho 2: `med-layer-guard.bbclass`, tarefa antes do `do_rootfs` assinada pelo conteúdo (`[file-checksums]`); vista falhando e voltando. Caminho 1: tratador em `BuildStarted` testado por `bitbake -R`, para o build antes de qualquer tarefa | `[nostamp]` rejeitado: faria o `do_rootfs` rodar sempre |
| 02/11/2026 | §6 passo 9 | `RESULTS.md` §11 e item no §9 escritos; `CLAUDE.md`: patch preparado, **não aplicado** (§8) | — |
