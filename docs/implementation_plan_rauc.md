# Plano de Implementação — Caminho de Atualização A/B (RAUC)

> **Status**: plano corrente (não histórico). Complementa `PROJECT_CONTEXT.md`, que continua sendo
> a referência arquitetural autoritativa. Os `implementation_plan.md`, `implementation_plan_EEG.md`
> e `implementation_plan_improvements.md` são iterações anteriores e **não** descrevem o estado
> atual do repositório.
>
> **Escopo**: fazer o `rauc.service` subir, tornar o caminho de atualização exercitável, e definir
> **até onde** ele pode ser honestamente validado no QEMU. Enumera as mudanças concretas — arquivo
> a arquivo — que isso exige.
>
> **Verificado nesta análise** (2026-08-16, contra a árvore construída e o fonte em
> `build/tmp-glibc/work/core2-64-med-linux/rauc/1.15.2/`):
> - o `rauc.service` falha no boot do QEMU, e a causa está no journal do dispositivo:
>   `Failed to resolve realpath for '/dev/disk/by-partlabel/med-root-a'` seguido de
>   `Failed to determine slot states: Did not find booted slot (matching '/dev/vda')`;
> - `src/bootchooser.c:15` — os backends aceitos são exatamente
>   `barebox`, `grub`, `uboot`, `efi`, `custom`, `noop`;
> - `src/bootloaders/grub.c:6` — o backend GRUB **executa o binário** `grub-editenv`
>   (`r_subprocess_newv`), não usa biblioteca; o `grubenv` default é `/boot/grub/grubenv`
>   (`src/config_file.c:339-345`);
> - `src/config_file.c:394` — `boot-attempts` é rejeitado para qualquer backend que não seja
>   `uboot` ou `barebox`;
> - `layers/poky/meta/recipes-bsp/grub/grub_2.12.bb:12` — o binário é empacotado como
>   `grub-editenv`;
> - `qemux86-64` **não** declara `efi` em `MACHINE_FEATURES`
>   (`qemu.inc:14` + `qemux86-64.conf:35`: `alsa bluetooth usbgadget screen vfat x86 pci`);
> - `meta-rauc` fornece `rauc-native`, e `classes-recipe/bundle.bbclass` assina com
>   `RAUC_CERT_FILE`/`RAUC_KEY_FILE` chamando `rauc bundle` do sysroot nativo.
>
> **Não verificado** (análise, a confirmar durante a execução): se `runqemu` boota a imagem `wic`
> deste layout sem trabalho adicional; se o plugin `bootimg-efi` do wic exige `MACHINE_FEATURES`
> `efi`; se o OVMF é necessário e se ele está disponível no build.

---

## 1. O diagnóstico: são três problemas independentes

O `bootloader=uboot` no `system.conf` é o mais visível, mas é o **terceiro** na ordem em que o RAUC
tropeça. Tratá-los como um problema só é o que faz esse trabalho parecer maior do que é.

| # | Problema | O que falta | Custo |
|---|---|---|---|
| 1 | Os slots não existem | A imagem QEMU é `ext4` cru em `/dev/vda`, sem tabela de partições. `/dev/disk/by-partlabel/` só é populado por um disco **GPT com `--part-name`**, ou seja, pela imagem `wic`. | Baixo |
| 2 | Slot bootado não identificável | O RAUC casa o device que provê `/` contra os devices dos slots. Não basta existir o GPT — o boot tem que vir **de uma partição** (`/dev/vda2`), não do disco inteiro. | Baixo (decorre de 1) |
| 3 | Backend de bootloader errado | `bootloader=uboot` em `qemux86-64`. | Depende de até onde se quer validar — ver §3 |

### A contradição que já está na árvore

O `meta-med-distro/wic/med-partitions.wks` declara, no próprio comentário de cabeçalho, que o
layout *"targets EFI/GRUB machines (qemux86-64 and generic x86 hardware)"*, e usa
`--sourceparams="loader=grub-efi"`. O `rauc-conf/system.conf`, na **mesma camada**, diz
`bootloader=uboot`. Os dois arquivos discordam sobre a mesma máquina, e nenhum build detecta isso
porque nada consome os dois ao mesmo tempo até o dispositivo bootar.

---

## 2. Decisão: `bootloader` é propriedade da **máquina**, não da distro

A correção **não** é trocar `uboot` por `grub` no arquivo. Isso conserta o QEMU e quebra o
STM32MP257, e mantém o erro de fundo: um arquivo de política de distro nomeando um bootloader
específico.

`bootloader=` é exatamente o mesmo tipo de acoplamento que o `MED_EEG_DRIVER` existe para eliminar
no front-end de aquisição. A decisão é tratá-lo do mesmo jeito: **uma variável, substituída em
tempo de build, cujo valor mora no arquivo de projeto do alvo**.

Isso tem três consequências boas, nesta ordem de importância para o TCC:

1. Resolve a contradição com o `.wks` sem escolher um vencedor arbitrário.
2. Mantém a camada de distro livre de conhecimento de BSP — a regra 1 do `CLAUDE.md`.
3. **Vira mais um ponto de dados para a afirmação central da tese.** A superfície de adaptação de
   hardware deixa de ser "uma variável" (`MED_EEG_DRIVER`) e passa a ser um conjunto pequeno e
   enumerável de variáveis. Isso é um resultado mais forte e mais honesto do que a formulação atual,
   porque é o que de fato acontece quando o alvo muda.

---

## 3. Decisão: três níveis de validação, e por que parar no Nível 1

A pergunta "dá para validar no QEMU?" tem resposta diferente para cada parte do caminho de
atualização. Separá-las evita tanto o otimismo quanto o abandono prematuro.

### Nível 0 — verificação de bundle, **sem QEMU**

Constrói-se um bundle com o `bundle.bbclass` e verifica-se com `rauc info --keyring=...` usando
`rauc-native`, no host. Isso exercita **assinatura CMS, `check-purpose=codesign` e a CRL** — ou
seja, exatamente o trabalho de keyring que hoje está validado apenas por `openssl verify`, e nunca
pelo próprio RAUC. Não depende de slot, de partição nem de bootloader.

**É o teste mais barato do plano e o que fecha o buraco mais recente.** Deve vir primeiro.

### Nível 1 — QEMU com `bootloader=noop`

Resolve #1 e #2 (imagem `wic`, boot pela partição) e deixa o bootloader fora. Valida:

- o `rauc.service` subir sem falha;
- `rauc status` enumerar os slots A e B e identificar o bootado;
- `rauc install` verificar o bundle e **escrever no slot inativo**.

É a maior parte do caminho de atualização, e é o alvo realista deste plano.

### Nível 2 — QEMU com `bootloader=grub` — **adiado, deliberadamente**

Só o Nível 2 valida troca A/B real, `mark-good`/`mark-bad` e fallback. O custo é desproporcional:
exige `EFI_PROVIDER = "grub-efi"`, o pacote `grub-editenv` na imagem, e boot EFI sob `runqemu` (que
`qemux86-64` não declara em `MACHINE_FEATURES`, e que provavelmente exige OVMF). É um subprojeto,
não um ajuste.

**E o que ele validaria não é o que roda no alvo físico.** `boot-attempts` é rejeitado para
qualquer backend que não seja `uboot`/`barebox` (verificado no fonte): o fallback com GRUB é por
ordem no `grubenv`, o do STM32MP257 é por contador de tentativas no ambiente do U-Boot. São
mecanismos diferentes.

> **Posição para o texto do TCC**: a *política* A/B (atomicidade, escrita no slot inativo,
> verificação criptográfica antes de marcar bootável) é validada no QEMU no nível do RAUC. A
> *integração com o bootloader* é validada no STM32MP257 com U-Boot, que é onde ela importa.
> Afirmar que o QEMU valida o caminho de atualização inteiro seria generalizar demais.

---

## 4. Mudanças concretas

### 4.1. `meta-med-distro/recipes-core/rauc/rauc-conf.bbappend`

Acrescentar a substituição, junto das que já existem para `@MED_COMPATIBLE@`/`@MED_VERSION@`:

```bash
# O bootloader é propriedade da máquina, não da distro: qemux86-64 boota por
# GRUB/EFI (ver med-partitions.wks) e o STM32MP257 por U-Boot. Fixar um dos
# dois aqui obrigaria a camada de distro a conhecer o BSP.
MED_BOOTLOADER ?= "noop"

do_install:append() {
    if [ -f ${D}${sysconfdir}/rauc/system.conf ]; then
        sed -i -e "s|@MED_COMPATIBLE@|${DISTRO}-${MACHINE}|g" \
               -e "s|@MED_VERSION@|${DISTRO_VERSION}|g" \
               -e "s|@MED_BOOTLOADER@|${MED_BOOTLOADER}|g" \
               ${D}${sysconfdir}/rauc/system.conf
    fi
}
```

O default `noop` é a escolha segura: um alvo que **esqueceu** de declarar seu bootloader ganha um
RAUC que sobe, enumera slots e recusa-se a marcar qualquer coisa como bootável — em vez de um
serviço morto ou, pior, de uma troca A/B que o bootloader não vai honrar.

### 4.2. `meta-med-distro/recipes-core/rauc/rauc-conf/system.conf`

```ini
[system]
compatible=@MED_COMPATIBLE@
bootloader=@MED_BOOTLOADER@
```

Atualizar também o comentário de cabeçalho, que hoje lista só `@MED_COMPATIBLE@`/`@MED_VERSION@`.

### 4.3. `kas/project-eeg-qemu.yml` e `kas/project-eeg-stm32mp2.yml`

```yaml
# project-eeg-qemu.yml
    MED_BOOTLOADER = "noop"
```

```yaml
# project-eeg-stm32mp2.yml
    MED_BOOTLOADER = "uboot"
```

`project-tomograph.yml` herda o default. Documentar no comentário do arquivo QEMU **por que** é
`noop` e não `grub` — apontando para a §3 deste plano, para que a escolha não pareça preguiça.

### 4.4. `meta-med-app/recipes-core/images/med-image-eeg.bb`

```bitbake
# O disco GPT é o que faz /dev/disk/by-partlabel/med-root-{a,b} existir, sem o
# que o RAUC não resolve slot nenhum. Fica no perfil de *desenvolvimento* de
# propósito: a imagem de produção não tem login, e um caminho de atualização
# que só pode ser inspecionado numa imagem sem shell não é inspecionável.
IMAGE_FSTYPES += "wic wic.bmap"
```

Conferir o dimensionamento antes: o rootfs medido é ~267 MB (`IMAGESIZE = 273656`) mais
`IMAGE_ROOTFS_EXTRA_SPACE = 262144` (256 MB), ou seja ~523 MB contra os `--size 1024` (MB) de cada
slot do `.wks`. Cabe, com folga de ~50%, mas a margem some se o perfil crescer.

### 4.5. Novo: recipe de bundle — o Nível 0

`meta-custom/meta-med-app/recipes-core/bundles/med-bundle-eeg.bb`:

```bitbake
inherit bundle

# Tem de bater exatamente com o que o rauc-conf.bbappend grava em system.conf
# (${DISTRO}-${MACHINE}). O default do bundle.bbclass é ${MACHINE}-${TARGET_VENDOR},
# que aqui daria "qemux86-64-med" e seria recusado na instalação.
RAUC_BUNDLE_COMPATIBLE = "${DISTRO}-${MACHINE}"
RAUC_BUNDLE_VERSION = "${DISTRO_VERSION}"

# system.conf declara bundle-formats=-plain, ou seja, o formato "plain" está
# desabilitado no dispositivo. Um bundle plain seria recusado na instalação.
RAUC_BUNDLE_FORMAT = "verity"

RAUC_SLOT_rootfs = "med-image-eeg"
RAUC_SLOT_rootfs[fstype] = "ext4"

RAUC_KEY_FILE  = "${MED_PKI_DIR}/signing/med-signing.key.pem"
RAUC_CERT_FILE = "${MED_PKI_DIR}/signing/med-signing.cert.pem"
```

`MED_PKI_DIR` precisa ser declarado no `kas-base.yml` (`local_conf_header`), apontando para o
`pki/` na raiz do repositório. Dentro do contêiner o repositório é montado em `/repo`, então
`/repo/pki/...` resolve — mas ver o risco em §7.

### 4.6. `Makefile`

```make
bundle: $(TOOL)
	$(KAS) shell $(QEMU_CFG) -c "bitbake med-bundle-eeg"

# Boota o disco GPT em vez do ext4 solto: é o que faz o RAUC enxergar slots.
runqemu-wic: $(TOOL)
	$(KAS) $(RUNTIME_ARGS) shell $(QEMU_CFG) \
	  -c "runqemu qemux86-64 wic nographic slirp"
```

Acrescentar as duas linhas ao `help` e ao `.PHONY`.

### 4.7. Nível 2 — o que ficaria pendente

Registrado aqui para não se perder, **explicitamente fora do escopo desta rodada**:
`EFI_PROVIDER = "grub-efi"` no `med-os.conf`, `grub-editenv` no `packagegroup-med-core` (ou em
`MED_OS_INSTALL`), `MACHINE_FEATURES:append = " efi"` para `qemux86-64`, e OVMF sob `runqemu`.
Depois disso, `MED_BOOTLOADER = "grub"` no projeto QEMU.

---

## 5. Ordem de execução

1. **§4.5 + §4.6 (bundle)** — Nível 0. Não toca em imagem nem em boot, e valida o keyring pelo
   próprio RAUC. Se algo do trabalho de PKI estiver errado, aparece aqui, barato.
2. **§4.1 + §4.2 + §4.3** — a variável `MED_BOOTLOADER`. Puramente metadata, `make risks` pega erro
   de sintaxe em segundos.
3. **§4.4** — `wic` na imagem de dev. Rebuild com sstate quente.
4. **`make runqemu-wic`** — Nível 1, verificação da §6.
5. Reavaliar o Nível 2 **só depois** de o STM32MP257 construir. Se o U-Boot no alvo físico já
   validar a integração com bootloader, o Nível 2 no QEMU vira redundância cara.

---

## 6. Plano de verificação

**Nível 0 (host):**

```bash
make bundle
# no sysroot nativo, com o keyring que vai para o dispositivo:
rauc info --keyring=meta-custom/meta-med-distro/recipes-core/rauc/rauc-conf/med-keyring.pem \
          build/tmp-glibc/deploy/images/qemux86-64/med-bundle-eeg-*.raucb
```

Esperado: assinatura válida, `Compatible: med-os-qemux86-64`, formato `verity`. Um erro de
*purpose* ou de CRL aqui é o sinal de que a §Parte B do trabalho de PKI não fecha — e é o que
`openssl verify` sozinho não conseguiria mostrar.

**Nível 1 (QEMU, após `make runqemu-wic`):**

```bash
systemctl status rauc.service          # active (running), não mais failed
rauc status                            # slots rootfs.0 / rootfs.1, um deles "booted"
lsblk -o NAME,PARTLABEL                # med-root-a, med-root-b, med-data
ls -l /dev/disk/by-partlabel/
rauc install /path/to/bundle.raucb     # verifica e escreve no slot inativo
rauc status                            # o slot inativo mudou de estado
```

Esperado: a instalação **completa a escrita e falha (ou vira no-op) na marcação de bootável**, por
causa do `noop`. Essa falha é o resultado correto e precisa ser registrada como tal — não como
defeito.

**Regressão a não perder de vista**: o `/data/rauc` já é criado hoje (visto no boot de
2026-08-16), e `data-directory=/data/rauc` no `system.conf` depende de `/data` existir. No QEMU
`/data` fica no rootfs porque o `data.mount` é pulado; com o layout `wic` passa a existir uma
partição `med-data`, e o `crypttab` vai tentar abri-la como LUKS. O `.wks` cria `med-data` como
ext4 puro (wic não gera header LUKS), então **espera-se que o `systemd-cryptsetup@med-data` falhe**
até o passo de provisionamento existir. Ver §7.

---

## 7. Riscos e questões em aberto

1. **`med-data` sem LUKS quebra o boot do Nível 1.** Hoje o `data.mount` é pulado por
   `ConditionPathExists=/dev/mapper/med-data`, que nunca é satisfeita. Com o `wic`, a partição
   passa a existir e o `crypttab` (`luks,nofail,discard`) vai tentar destravá-la; sem header LUKS
   isso falha. O `nofail` deve conter o dano, mas **isso não foi verificado**. Se atrapalhar, a
   saída barata é o passo de provisionamento de primeiro boot que o próprio `.wks` já descreve como
   necessário — que é trabalho separado e provavelmente merece plano próprio.

2. **A chave privada entra no ambiente de build.** `bundle.bbclass` assina durante `do_bundle`,
   chamando `rauc bundle --key=`. Com o `pki/` na raiz do repositório e o repositório montado em
   `/repo`, a chave de assinatura fica visível dentro do contêiner de build. Aceitável para uma CA
   de desenvolvimento; **inaceitável no modelo que o `scripts/med-pki.sh` descreve para produção**
   (chave em HSM). O texto do TCC deve dizer que a assinatura em produção acontece fora do build.

3. **`RAUC_BUNDLE_COMPATIBLE` é um acoplamento silencioso.** Se o `system.conf` e o recipe de bundle
   divergirem, o bundle constrói limpo e é recusado só na instalação, no dispositivo. Ambos derivam
   de `${DISTRO}-${MACHINE}` de propósito; qualquer mudança nessa forma tem de ser feita nos dois.

4. **`wic` no perfil de desenvolvimento aumenta o tempo de build** de toda iteração do EEG, mesmo
   quando ninguém está testando atualização. Se incomodar, a alternativa é uma imagem separada
   (`med-image-eeg-ab`) em vez de somar `IMAGE_FSTYPES` — ao custo de mais uma imagem para manter em
   sincronia.

5. **O Nível 2 pode não valer o custo.** Se o STM32MP257 validar a integração U-Boot, o GRUB no
   QEMU passa a testar um mecanismo que não é o do produto. Decidir depois do build do alvo físico,
   não antes.
