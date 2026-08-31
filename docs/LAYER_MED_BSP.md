# Camada `meta-med-bsp` — adaptação de placa

> **Status**: documentação de referência da camada. O documento arquitetural autoritativo continua
> sendo `PROJECT_CONTEXT.md`; aqui está o detalhamento de uma camada só.
>
> **Posição**: camada 1/4 do `MedStack`, prioridade **7** — acima das camadas de BSP de fabricante
> (6), abaixo de `meta-med-distro` (8), `meta-med-framework` (9) e `meta-med-app` (10).
>
> **Regra que a define**: **esta é a única camada do repositório autorizada a nomear uma máquina,
> um bootloader, um offset de partição ou um nó de dispositivo.** Em qualquer camada acima, isso é
> o defeito — e nenhum build reporta.

---

## 1. Por que esta camada existe

Ela é a mais nova das quatro e nasceu de uma auditoria, não de um projeto. As três camadas acima
deveriam não conter conhecimento de máquina nenhum, e **duas vezes continham**:

| Violação | Onde estava | Por que ninguém viu |
|---|---|---|
| `loader=grub-efi` e `console=ttyS0,115200` | `meta-med-distro/wic/med-partitions.wks` | O nome do arquivo parecia portátil; só uma segunda máquina exigiria resposta diferente |
| `QB_KERNEL_ROOT = "/dev/vda2"` | `meta-med-distro/.../med-image-base.bb` | Caminho virtio + índice de partição de um disco específico, duas linhas abaixo de um comentário afirmando que a receita é agnóstica de classe de dispositivo |

Nenhuma das duas quebrou build. Ambas só apareceram quando o `stm32mp25-disco` precisou de uma
resposta diferente e o default da distro venceu por ausência de adversário, produzindo uma ESP
EFI/GRUB para uma placa que não tem ESP.

A distinção que a camada passa a impor é **política vs. placement**:

| | Onde mora | Exemplo |
|---|---|---|
| **Política** | `meta-med-distro` | dois slots de rootfs, de tamanho idêntico, chamados `med-root-a`/`med-root-b`; `/data` não é slot; rootfs imutável |
| **Placement** | **aqui** | TF-A começa em 17 KiB; o GRUB lê a ESP; o kernel recebe `console=ttyS0`; o emulador acha o kernel assim |

A política é a tese. O placement é o que muda quando a placa muda — e o que torna a métrica de
portabilidade **enumerável**: uma placa nova acrescenta arquivos *aqui* e não altera nada acima.

---

## 2. Estrutura

```
meta-med-bsp/
├── conf/layer.conf                                    262 linhas
├── recipes-bsp/med-uboot-env/
│   └── med-uboot-env-config_1.0.bb                    100
└── wic/
    ├── med-partitions-efi.wks                          72
    └── med-partitions-stm32mp2.wks.in                 173
```

Note a proporção: **262 das 607 linhas são o `layer.conf`**, e quase todas são comentário. Isso é
deliberado — cada valor específico de máquina carrega ao lado a medição ou o defeito que o
justifica, porque é a única defesa contra alguém "simplificar" um valor que parece arbitrário.

---

## 3. Direção da dependência

```
LAYERDEPENDS_meta-med-bsp = "core openembedded-layer"
```

Duas propriedades, e ambas são verificáveis:

1. **Não depende de nenhuma camada `meta-med-*` acima.** É o que permite que a camada seja
   substituída por outra ao portar para uma placa nova.
2. **Não há nenhum `.bbappend` aqui sobre receita de camada superior, e não pode haver.** Isso
   inverteria a cadeia sobre a qual a arquitetura está construída. Esta camada **provê e não
   consome**.

O acoplamento que resta é o mais fraco possível: os `.wks` são *selecionados* por `MED_WKS_FILE`,
uma variável que `meta-med-distro` declara e lê. Referir-se ao nome de uma variável de cima é o
mesmo tipo de acoplamento suave que um BSP faz ao definir `PREFERRED_PROVIDER_virtual/kernel` — e
é até onde vai.

`openembedded-layer` entrou quando a camada ganhou a primeira receita: `med-uboot-env-config`
depende em runtime de `libubootenv-bin`, que vive em `meta-oe`. Um arquivo de configuração cuja
ferramenta não está garantidamente instalada é um arquivo que silenciosamente não faz nada.

---

## 4. As variáveis que esta camada responde

Toda a comunicação com as camadas de cima passa por variáveis. Nenhuma delas nomeia uma máquina do
lado de quem lê; todas nomeiam do lado de quem responde.

| Variável | Quem declara | Quem responde aqui | Para quê |
|---|---|---|---|
| `MED_WKS_FILE` | `meta-med-distro` (`med-os.conf`) | `MED_WKS_FILE_DEFAULT:<máquina>` | Qual layout de disco a máquina usa |
| `QB_FSINFO`, `QB_KERNEL_ROOT` | poky (`qemuboot.bbclass`) | `:qemux86-64` | Como o emulador boota o disco |
| `MACHINE_FEATURES` | poky | `:append:stm32mp25-disco` | Declarar `nogpu` quando a EULA não é aceita |
| `MED_GPU_PACKAGES` | `packagegroup-med-gui` | `:stm32mp25-disco` | Pacotes de GPU que só existem se a EULA for aceita |
| `MED_BSP_INSTALL` | `med-image-base.bb` | `:stm32mp25-disco` | Pacotes que só esta placa precisa |
| `MED_UBOOT_ENV_PARTUUID` | esta camada | `:stm32mp25-disco` | Endereço do ambiente do U-Boot, consumido pelo `.wks` **e** pela receita |
| `MED_KEY_STORE_DEV` / `_FSTYPE` | `med-data-volume` | por máquina | Onde a chave de desenvolvimento do `/data` pode morar |

### 4.1 A indireção de dois passos, e por que ela é obrigatória

```
MED_WKS_FILE_DEFAULT:qemux86-64    = "med-partitions-efi.wks"
MED_WKS_FILE_DEFAULT:stm32mp25-disco = "med-partitions-stm32mp2.wks.in"
MED_WKS_FILE ?= "${MED_WKS_FILE_DEFAULT}"
```

Escrever `MED_WKS_FILE:qemux86-64 = ...` diretamente tornaria esta camada **insobrepujável**: um
valor com sufixo de máquina vence a atribuição sem sufixo que um arquivo de projeto KAS faz, e o
arquivo de projeto perderia em silêncio. Com `_DEFAULT` por máquina e `?=` depois, a precedência
fica correta: **esta camada responde quando ninguém mais responde**, e qualquer `local.conf` ainda
vence.

Isso foi verificado por injeção de falha, não por raciocínio: `MED_WKS_FILE = ""` vindo de um
fragmento KAS de fato sobrepõe o default e de fato dispara o guarda de build do
`med-image-base.bb`.

---

## 5. Os dois layouts de disco

### 5.1 `med-partitions-efi.wks` — máquinas GRUB/EFI

Antes chamado `med-partitions.wks`, em `meta-med-distro`. **O rename é o conteúdo da correção**:
não é o disco da plataforma, é o disco das máquinas que bootam por GRUB/EFI.

| # | Nome GPT | Tipo | Tamanho | Papel |
|---|---|---|---|---|
| 1 | `esp` | vfat | 64 MiB | ESP com GRUB; também o depósito da chave de desenvolvimento no QEMU |
| 2 | `med-root-a` | ext4 | 1 GiB fixo | Slot A |
| 3 | `med-root-b` | ext4 | 1 GiB fixo | Slot B |
| 4 | `med-data` | ext4 | 512 MiB | Volume de dados; convertido em LUKS no primeiro boot |

`--fixed-size` nos dois slots, e não `--size`: o wic trata `--size` como **mínimo** e aplica um
fator de sobrecarga de 1,3 apenas à partição que tem `--source`. Com `--size` os dois slots saíram
de tamanhos diferentes — e slots assimétricos quebram a atualização A/B sem quebrar build nenhum.

### 5.2 `med-partitions-stm32mp2.wks.in` — a STM32MP257F-DK

Onze partições: a cadeia de boot que o ROM code da ST exige, e depois a **mesma política A/B** de
qualquer máquina MedOS.

```
fsbla1 fsbla2   metadata1 metadata2   fip-a fip-b   u-boot-env | med-boot | med-root-a | med-root-b | med-data
   TF-A (2 cópias)   metadados FWU      OP-TEE+U-Boot     env   |  kernel  |  (ativo)   | (inativo)  |  LUKS
```

Quatro detalhes que só existem porque a placa os exige:

- **`--align 17`** na primeira partição: o TF-A tem de começar no offset de 17 KiB que o ROM code
  procura.
- **`--part-type 19d5df83-…`** nas FIP: é o GUID que o TF-A usa para localizar o firmware.
- **`--uuid` fixo em `med-root-a`**: o `extlinux.conf` que o BSP da ST gera referencia
  `${DEVICE_PARTUUID_ROOTFS:SDCARD}`, então o PARTUUID não pode ser sorteado a cada build. O
  primeiro boot da placa confirmou o pareamento: `root=PARTUUID=e91c4e10-…` é literalmente o valor
  deste arquivo.
- **`med-boot` e não `bootfs`**: `/dev/disk/by-partlabel/` é um namespace plano entre **todos** os
  discos, e o eMMC de fábrica desta placa traz `bootfs`, `rootfs`, `vendorfs`, `u-boot-env`,
  `fip-a` e `fip-b`. Um nome colidente vai para o disco que enumerar primeiro — e a enumeração é
  uma corrida, medida invertendo-se entre dois boots do mesmo hardware.

Este arquivo existe porque a suposição documentada de que "BSPs com TF-A em offsets fixos trazem
seu próprio `WKS_FILE`" era **falsa para o `meta-st-stm32mp`**: sua conf de máquina define
`WKS_FILE_DEPENDS` e deixa `#WKS_FILE += "${OPTEE_WIC_FILE}"` comentado.

---

## 6. `med-uboot-env-config` — a primeira receita da camada

Instala `/etc/fw_env.config`, sem o qual o backend `uboot` do RAUC falha por completo. Medido na
placa em 18/08/2026: `fw_printenv failed with exit code: 1`, e o RAUC passa a reportar **todos** os
slots como `bad` — inclusive aquele de onde acabou de bootar — com `Activated: none`.

Copiar o `fw_env.config.mmc` que a ST distribui estaria errado **duas vezes**:

1. **Ele endereça a partição por partlabel**, e `u-boot-env` colide com o nome do eMMC de fábrica.
   Medido: `readlink -f /dev/disk/by-partlabel/u-boot-env` → `/dev/mmcblk2p5`, o eMMC. Usá-lo
   apontaria o `fw_setenv` para o ambiente de bootloader **de outro sistema operacional** — e
   funcionaria, em silêncio, até alguém se perguntar por que a troca de slot nunca surte efeito.
   Por isso o endereçamento é **por PARTUUID**.
2. **Ele lista as duas cópias redundantes no mesmo offset** (`-0x2000` duas vezes). O
   `env/mmc.c` do U-Boot calcula `(info.start + info.size - (1 + copy) * len) * info.blksz`, de
   modo que a cópia 1 fica um `ENV_SIZE` mais atrás — `-0x4000`. As duas entradas são obrigatórias,
   não opcionais: `CONFIG_SYS_REDUNDAND_ENVIRONMENT=y` muda o formato em mídia, e um arquivo de
   entrada única falharia o CRC e leria nada.

O PARTUUID é declarado **uma vez** (`MED_UBOOT_ENV_PARTUUID`) e substituído tanto no `.wks` que
cria a partição quanto no arquivo que a endereça. É o argumento inteiro para a existência desta
camada: *a tabela de partições e o endereço de uma partição nela são o mesmo fato de placa e não
podem poder discordar.*

A receita traz um guarda que falha o build se a máquina não declarou o UUID — exercitado por
injeção de falha, construindo-a para `qemux86-64`.

---

## 7. A GPU que a placa tem e a plataforma não usa

O trecho mais longo do `layer.conf`, e o que impediu qualquer boot antes de 18/08.

**Sintoma**: pânico do mundo seguro em `clk_stm32_pll_init` (`clk-stm32mp25.c:2002`) cerca de sete
segundos após o kernel, logo depois de `etnaviv etnaviv: bound 48280000.gpu`, seguido de reboot em
loop — reproduzido duas vezes e sobrevivente à troca completa da variante TF-A/OP-TEE/U-Boot, o
que descartou incompatibilidade de devicetree.

**Causa**: um buraco de configuração no BSP. O `linux-stm32mp.inc` da ST só escreve
`/etc/modprobe.d/blacklist.conf` se `MACHINE_FEATURES` contiver `gpu` **ou** `nogpu`. Sem nenhum
dos dois, nenhum arquivo é escrito — que é a única combinação em que o `etnaviv` carrega, faz bind
na GPU e pede ao mundo seguro, via SCMI, uma PLL que aquela configuração de firmware não preparou.

**Correção**, e note que é condicional e não um append plano:

```
MACHINE_FEATURES:append:stm32mp25-disco = "${@' nogpu' if d.getVar('ACCEPT_EULA_stm32mp25-disco') != '1' else ''}"
MED_GPU_PACKAGES:stm32mp25-disco = "${GPU_IMAGE_INSTALL}"
```

Os dois estados que o BSP suporta são mutuamente exclusivos, e um append plano deixaria as duas
features ligadas no dia em que alguém aceitasse a EULA. Escrito assim, `ACCEPT_EULA_stm32mp25-disco
= "1"` no arquivo de projeto é o interruptor inteiro.

O `MED_GPU_PACKAGES` existe porque a imagem não ganharia os pacotes de graça: a ST os coloca em
`GPU_IMAGE_INSTALL`, que vai para `MACHINE_EXTRA_RRECOMMENDS`, puxado por `packagegroup-base` — e
`med-image-base.bb` instala `packagegroup-core-boot`. Conferido, não suposto: nada no grafo da
imagem alcança aquela variável.

Há um argumento de conformidade junto: o stack Vivante é SOUP binário com restrição de
redistribuição, e uma plataforma que argumenta rastreabilidade IEC 62304 precisa contabilizá-lo no
SBOM. "Sem GPU até que um produto peça" é o default defensável.

---

## 8. Como adicionar uma placa nova

O procedimento é a métrica de portabilidade em forma de checklist. Uma placa nova acrescenta
arquivos **apenas aqui**:

1. `wic/med-partitions-<placa>.wks[.in]` — a cadeia de boot que a placa exige, seguida da política
   A/B inalterada (dois slots de tamanho idêntico com os nomes `med-root-a`/`med-root-b`, `med-data`
   fora deles).
2. `MED_WKS_FILE_DEFAULT:<máquina>` no `layer.conf`, apontando para ele.
3. `MED_KEY_STORE_DEV:<máquina>` e `MED_KEY_STORE_FSTYPE:<máquina>` — uma partição que **nenhuma
   atualização escreve**, de preferência num filesystem que honre `chmod 0400`.
4. Se o RAUC usar backend `uboot`: `MED_UBOOT_ENV_PARTUUID:<máquina>` e
   `MED_BSP_INSTALL:<máquina> = "med-uboot-env-config"`.
5. Se a placa tiver coprocessador: `MED_AMP_FIRMWARE`.
6. Se houver fatos de máquina do BSP de fabricante a corrigir (como o `nogpu`), aqui.

**Nenhum arquivo das camadas 2, 3 ou 4 muda.** Se algum precisar mudar, ou a placa expôs uma
política nova (e o lugar é `meta-med-distro`), ou a fronteira foi rompida.

---

## 9. O que esta camada ainda não tem

- **Nenhuma máquina além de `qemux86-64` e `stm32mp25-disco`.** A afirmação "uma placa nova custa
  N linhas aqui" continua sendo uma previsão, não uma medida — só um terceiro alvo a converte em
  medida, e isso está listado como trabalho futuro no `PLANO_TCC.md`.
- **Nenhum firmware de coprocessador.** `MED_AMP_FIRMWARE` está vazio nos dois alvos; a placa
  reporta `Support of signed firmware only`, restrição que o `implementation_plan_ads1299.md` ainda
  não contempla.
- **Nenhuma resposta para seleção de slot A/B.** A `med-boot` é compartilhada pelos dois slots e o
  `extlinux.conf` gerado pelo BSP fixa o PARTUUID do slot A, então um dispositivo que o RAUC marcou
  "bootar B" ainda boota A. Fechar isso exige um script de U-Boot lendo `BOOT_ORDER` — é para isso
  que a partição `u-boot-env` existe.
