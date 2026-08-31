# Seleção de slot A/B no STM32MP257 — o ensaio com o U-Boot

**Alvo**: STM32MP257F-DK, `MACHINE=stm32mp25-disco`, `med-image-eeg`, `KEY=development`
**Data do ensaio**: 2026-08-30
**Companheiros**: `BRINGUP_STM32MP2.md` §9.10 (a sessão inteira), `RESULTS.md` §8 (as medidas),
`implementation_plan_rauc.md` (a política que isto implementa)

---

## 1. O que este documento é

O `BRINGUP_STM32MP2.md` registra o porte inteiro em ordem cronológica. Este arquivo isola **um item
aberto** que atravessa quase todos aqueles registros e nunca teve seção própria: *o dispositivo não
escolhe de qual slot bootar*.

Ele existe porque o ensaio de 30/08 mudou o estado dessa questão de "não implementado" para "medido
em três partes, com uma peça faltando e a especificação dela escrita". É também onde ficam quatro
achados que só apareceram quando a placa bootou o slot B — nenhum deles é sobre o U-Boot, e todos
os quatro são consequência de haver dois rootfs.

---

## 2. O que já estava estabelecido antes do ensaio

Em ordem, e cada um com seu registro:

| # | fato | onde |
|---|---|---|
| 1 | O disco tem forma A/B: dois slots de 1024 MiB fixos, `/data` fora deles, `u-boot-env` de 512 KiB | `med-partitions-stm32mp2.wks.in`, GPT lido do `.wic` |
| 2 | O RAUC enumera os dois slots e sabe de qual bootou | §9.5 (2026-08-18) |
| 3 | `fw_printenv`/`fw_setenv` alcançam o ambiente do U-Boot, endereçado por PARTUUID | §9.8, revalidado §9.10 |
| 4 | O kernel do alvo monta bundles `verity` (`CONFIG_DM_VERITY`) | §9.10 |
| 5 | `rauc install` escreve o slot inativo, conferido byte a byte | §9.10, `RESULTS.md` §8 |
| 6 | O RAUC reordena `BOOT_ORDER` para `B A`, preservando o slot anterior | §9.10 |
| 7 | *(acrescentado depois)* O `bootcmd` que lê `BOOT_ORDER` vem na imagem e escolhe o slot no primeiro boot | 2026-08-31, `implementation_plan_uboot_ab.md` §8 |

O item 6 é onde a cadeia parava quando este documento foi escrito: **`BOOT_ORDER` era escrito e
nunca lido.** O item 7 é a peça que a §6 daqui especificou, hoje implementada — o resto deste arquivo
continua sendo o registro de como o problema foi medido, não de como foi resolvido.

---

## 3. A cadeia de boot como ela é hoje, medida

```
ROM code  ──lê GPT, procura fsbla1──▶  TF-A BL2
                                        │
                                        ▼
                                     OP-TEE + U-Boot  (FIP em fip-a)
                                        │
                                        │  distro bootcmd: "Scanning mmc 0:8"
                                        ▼
                              med-boot (part. 8, ext4, compartilhada)
                                        │
                                        ▼
                              /extlinux/extlinux.conf
```

O arquivo, exatamente como o BSP o gera:

```
MENU BACKGROUND /splash_landscape.bmp
TIMEOUT 20
LABEL OpenSTLinux
	KERNEL /Image.gz
	FDTDIR /
	APPEND root=PARTUUID=e91c4e10-16e6-4c0e-bd0e-77becf4a3582 rootwait rw   earlycon console=${console},${baudrate}
```

Esse PARTUUID é o `--uuid` que o `.wks` fixa para `med-root-a`. **Nenhum elemento dessa cadeia lê
`BOOT_ORDER`.**

Três detalhes que só ficam visíveis lendo o BSP, e os três importam para a solução:

1. **Não há `boot.scr.uimg` na `med-boot`.** O `stm32mp-extlinux.bb` só gera o script quando produz
   *mais de um* `extlinux.conf`; esta máquina declara um devicetree só, então o caminho em uso é o
   `bootcmd` distro puro, direto para o `extlinux.conf`. Confirmado no conteúdo da partição.
2. **A própria ST diz para não usar aquele script.** O cabeçalho do `boot.scr.cmd` dela:
   *"SAMPLE BOOT SCRIPT: PLEASE DON'T USE this SCRIPT in REAL PRODUCT … for real product with only
   one supported configuration change the bootcmd in U-Boot"*. A solução que este documento propõe é
   literalmente a que o vendor recomenda.
3. **`med-boot` é compartilhada pelos dois slots** — inclusive o kernel. Isso tem consequência, e ela
   está na §5.4.

---

## 4. O ensaio: bootar o slot B à mão

### Objetivo

Separar duas afirmações que o `sha256sum` do slot **não** separa:

- *"o RAUC escreveu os bytes certos"* — provado pela comparação com o checksum do manifesto;
- *"o slot escrito boota"* — não provado por aquilo. Um rootfs pode ter os bytes corretos e mesmo
  assim não subir, se algo dentro dele pressupuser o slot A.

### Método

O `extlinux.conf` mora na `med-boot`, que não está no `fstab` — precisa ser montada:

```sh
blkid /dev/disk/by-partlabel/med-root-b
# PARTUUID="2997c20d-239f-43cd-8658-9c7010186c9b"

mkdir -p /mnt/boot && mount /dev/disk/by-partlabel/med-boot /mnt/boot
cp /mnt/boot/extlinux/extlinux.conf /mnt/boot/extlinux/extlinux.conf.slotA
vi /mnt/boot/extlinux/extlinux.conf     # troca só o root=PARTUUID=
sync && reboot
```

### Resultado

```
$ cat /proc/cmdline
root=PARTUUID=2997c20d-239f-43cd-8658-9c7010186c9b rootwait rw   earlycon console=ttySTM0,115200

$ rauc status
Booted from: rootfs.1 (/dev/mmcblk0p10)

=== Bootloader ===
Activated: rootfs.1 (B)

x [rootfs.1] (med-root-b, ext4, booted)    bootname: B   boot status: good
o [rootfs.0] (med-root-a, ext4, inactive)  bootname: A   boot status: good
```

**O slot escrito pelo RAUC boota.** O sistema sobe até multi-user, a rede sobe, o SSH atende, o
`/data` LUKS abre, e o RAUC — rodando *de dentro* do slot B — se reconhece corretamente como
`rootfs.1` e concorda com o `BOOT_ORDER` que ele próprio havia escrito.

Vale notar o que **não** foi preciso ajustar, porque é o que torna o slot B genuinamente
intercambiável com o A: nada no rootfs nomeia um slot. O `/etc/fstab` monta `/` como `/dev/root`
(vem da cmdline), as linhas de `/boot` estão comentadas, o `crypttab` está vazio por projeto e o
`system.conf` do RAUC endereça os slots por `by-partlabel`. A única coisa no sistema inteiro que
nomeia o slot A é o `extlinux.conf` — e é exatamente ela que a peça faltante substitui.

### O que este ensaio **não** é

**Não é a troca A/B.** Foi um arquivo de texto editado à mão. Ele prova que o destino existe e é
alcançável; não prova que o dispositivo sabe escolhê-lo, nem que ele volta ao slot anterior quando o
novo falha.

*Atualização de 2026-08-31*: o dispositivo agora **sabe** escolher, e a troca foi medida. O `bootcmd`
de `med-uboot-env-image` lê `BOOT_ORDER` e monta o `root=PARTUUID=` a partir dos dois `--uuid` que a
§5.3 exigiu; um `rauc install` seguido de `reboot` levou a placa ao slot B sozinha, com `rauc.slot=B`
na cmdline e o slot A ainda `good` e ainda na ordem. A frase honesta passa a ser: *a seleção e a
troca de slot estão validadas em hardware; o fallback, não.* Ver `implementation_plan_uboot_ab.md`
§8.

---

## 5. Os quatro achados que o ensaio produziu

Nenhum deles era o objetivo. Todos são consequência de existir um segundo rootfs — e por isso
nenhum poderia ter aparecido antes de alguém bootar o outro slot.

### 5.1 Cada slot tem uma identidade diferente

O SSH avisou, e é o tipo de aviso que se aprende a ignorar por hábito:

```
@    WARNING: REMOTE HOST IDENTIFICATION HAS CHANGED!     @
ED25519 key fingerprint … SHA256:Z+zhXBtC8VYgssK6sD0ZqdUM61YjoqIGdL2+aVp1K90
```

Antes do reboot, a mesma placa apresentava `SHA256:/K295+vnOwqdzaGjACfp3JfjduvWNR7jtmziaL+U9fw`.
A causa está no rootfs produzido:

```
$ ls rootfs/etc/ssh/
moduli  ssh_config  sshd_config  sshd_config_readonly      ← nenhuma chave de host

$ wc -c rootfs/etc/machine-id
0 rootfs/etc/machine-id                                    ← vazio, gerado no primeiro boot
```

`sshdgenkeys.service` e `systemd-machine-id-commit.service` geram esse material **no primeiro boot,
dentro do rootfs**. Como cada slot é um rootfs, **cada slot tem sua própria identidade de máquina**.

Consequências, em ordem de gravidade:

- **A cada atualização A/B, a chave de host SSH do dispositivo muda.** Todo cliente remoto vê o
  aviso de MITM. Para um equipamento médico gerenciado remotamente isso é pior do que incômodo: é
  treinar o operador a aceitar exatamente o aviso que existe para detectar um ataque.
- **O `machine-id` muda.** Ele nomeia o diretório do journal (`/var/log/journal/<machine-id>/`) e é
  a identidade que o `sd-bus` e o `MedicalLogger` usam. Um registro de auditoria emitido antes e
  depois de uma atualização vem de dois `machine-id` diferentes no mesmo aparelho físico.
- O endereço IPv6 link-local **não** mudou (`fe80::12e7:7aff:fee3:d562`), porque é derivado do MAC.
  É por isso que o SSH gritou em vez de simplesmente não conectar: mesmo endereço, outra identidade.

Isto não tem correção implementada. As opções conhecidas — mover `/etc/ssh` e o `machine-id` para
`/data`, ou derivá-los de um segredo do hardware — têm implicações de custódia que pertencem ao
`implementation_plan_luks.md`, não a este documento.

### 5.2 A trilha de auditoria mora dentro de um slot A/B

Achado por inspeção do artefato ao investigar o 5.1, e ele é mais sério que o 5.1:

```
# rootfs/etc/systemd/journald.conf.d/10-journald-audit.conf
Storage=persistent
Seal=yes
```

`Storage=persistent` grava em `/var/log/journal`, que fica **no rootfs** — ou seja, **dentro de um
slot A/B**. Três consequências:

1. Bootar o slot B mostra um journal vazio: o histórico do slot A não está lá.
2. A próxima atualização escreve por cima de um dos dois históricos.
3. O selo (Forward Secure Sealing) é por `machine-id`, que também é por slot, então as duas metades
   nem sequer formam uma cadeia verificável única.

**É a mesma armadilha que o `implementation_plan_luks.md` já havia identificado para a chave do
`/data`** — *"uma chave no rootfs seria destruída pela primeira atualização bem-sucedida"* — e a
conclusão de lá foi tirar a chave do rootfs. A trilha de auditoria tem exatamente o mesmo problema e
continua no rootfs. `/data` existe, sobrevive a atualizações e é onde o `MedicalStorage` já escreve.

Estado: **previsto por inspeção do artefato, ainda não observado na placa.** Os comandos que
fechariam a medição estão na §8. (O §5.1, este sim, foi observado numa atualização real em
2026-08-31: a fingerprint SSH mudou de `SHA256:lAW0DlTk…` para `SHA256:25lyKZU9…` ao trocar de
slot — não num boot manual, mas no cenário em que o defeito importa.)

### 5.3 O PARTUUID do slot B é aleatório, e isso bloqueia o script

O `.wks` fixa `--uuid` para `med-root-a` e deliberadamente não fixa para `med-root-b`, com este
comentário:

> `--uuid` … *"Slot B has no such constraint yet precisely because nothing boots it yet."*

**Isso deixou de ser verdade neste ensaio.** Um script de U-Boot que monte `root=PARTUUID=` para o
slot B precisa de um valor estável; hoje o wic sorteia um UUID novo a cada build da imagem. Duas
consequências práticas:

- é uma mudança de uma linha que precisa acontecer **antes** do script, não depois;
- e é um aviso de bancada imediato: regravar o cartão com uma imagem nova invalida o
  `extlinux.conf` editado na §4, porque o UUID que ele nomeia deixa de existir. O sintoma seria um
  kernel sem rootfs.

### 5.4 O esquema A/B, como está, não entrega kernel

O ensaio bootou o slot B **com o kernel do slot A** — não por acidente, mas porque só existe um: a
`med-boot` é compartilhada e o `Image.gz` mora lá. Como os dois slots vieram do mesmo build, os
módulos em `/lib/modules` do slot B casam com esse kernel e nada apareceu.

Não casariam se o bundle trouxesse outra versão de kernel. E o kernel deste alvo tem
`CONFIG_MODVERSIONS=y`, então o desencontro seria em tempo de carga de módulo, não em tempo de boot:
o sistema subiria e perderia funcionalidade em silêncio.

Portanto, o que o caminho A/B atualiza hoje é **o rootfs, e só ele**. Entregar kernel exige uma de
duas decisões, e nenhuma foi tomada:

- pôr o kernel dentro do slot (U-Boot carregaria `Image.gz` de `med-root-{a,b}`), ou
- tornar `med-boot` também A/B, com o custo de mais duas partições e de uma troca coordenada.

---

## 6. O que a peça faltante tem de fazer

Uma peça só resolve **quatro** itens abertos, e é por isso que ela é a próxima prioridade:

| item aberto | como esta peça o fecha |
|---|---|
| Seleção de slot A/B | monta `root=PARTUUID=` a partir de `BOOT_ORDER` |
| `rauc-mark-good.service` nunca roda (`ConditionKernelCommandLine` não satisfeita, §9.9) | passa `rauc.slot=<A\|B>` na cmdline |
| `BOOT_ORDER` de fábrica não existe (§9.10, regra 13) | semeia o valor inicial quando ausente |
| Fallback em boot falho | decrementa `BOOT_<slot>_LEFT` **antes** de bootar, e cai para o próximo da lista quando chega a zero |

### Forma

`bootcmd` no ambiente do U-Boot — que é o que a ST recomenda (§3, item 2) e que já tem partição
própria no disco. **Não** exige patch no U-Boot nem fork do BSP: é dado, não código.

Esboço da lógica, com os nomes que o backend `uboot` do RAUC já usa (`src/bootloaders/uboot.c`):

```
for slot in ${BOOT_ORDER}:
    if ${BOOT_${slot}_LEFT} > 0:
        setexpr BOOT_${slot}_LEFT ${BOOT_${slot}_LEFT} - 1
        saveenv                                    # antes de bootar, nunca depois
        setenv bootargs "root=PARTUUID=${uuid_${slot}} rootwait rw rauc.slot=${slot} ..."
        load ${devtype} ${devnum}:8 ${kernel_addr_r} /Image.gz
        booti ...
# nenhum slot com tentativas: reseta os contadores e tenta de novo
```

Dois pontos onde é fácil errar, e ambos já foram pagos neste projeto:

- **`saveenv` antes do boot.** Se o contador só for decrementado depois de o sistema subir, um
  kernel que trava em pânico nunca decrementa e o dispositivo entra em laço no slot ruim para
  sempre. O contador é a evidência de que a tentativa *começou*, não de que ela terminou.
- **Endereçar por PARTUUID, não por número de partição nem por rótulo.** Regra 9: a numeração de
  dispositivo é uma corrida (quatro boots, quatro enumerações), e `by-partlabel` é um namespace
  plano que o eMMC de fábrica desta placa já disputou e venceu uma vez.

### Provisionamento

O `u-boot-env` nasce vazio (`--source empty`), então alguém precisa escrever `bootcmd`,
`BOOT_ORDER`, `BOOT_A_LEFT` e `BOOT_B_LEFT` na primeira vez. O molde já existe neste repositório:
uma unit de primeiro boot em `meta-med-bsp`, do mesmo formato do `med-data-provision.service` — que
grava só se o valor estiver ausente, e nunca sobrescreve o que já está lá.

O primeiro boot de um cartão recém-gravado continuaria usando o `extlinux.conf` (slot A), o que é
seguro e é o comportamento de hoje. A partir do segundo, o `bootcmd` provisionado assume.

### Onde cada peça mora

Tudo abaixo é fato de placa e vai para **`meta-med-bsp`** — nenhuma linha destas pode subir para
`meta-med-distro`, que não pode nomear bootloader, partição nem máquina (regra 1 do `CLAUDE.md`):

| peça | arquivo |
|---|---|
| `--uuid` fixo para `med-root-b` | `wic/med-partitions-stm32mp2.wks.in` |
| Os dois PARTUUIDs, declarados uma vez | `conf/layer.conf`, ao lado de `MED_UBOOT_ENV_PARTUUID` |
| O `bootcmd` e o provisionamento inicial | receita nova, irmã de `med-uboot-env-config` |

---

## 7. Estado da bancada ao fim do ensaio

Regra 12 — **valor escrito à mão é medição, nunca correção**. A placa não está no estado que o build
produz:

| o quê | valor | como voltar |
|---|---|---|
| `extlinux.conf` na `med-boot` | editado para o PARTUUID do slot B | `cp extlinux.conf.slotA extlinux.conf` (a cópia está lá) |
| Slot em execução | **B** (`rootfs.1`, `mmcblk0p10`) | reverter o arquivo acima e reiniciar |
| `BOOT_ORDER` | `B A` — escrito pelo RAUC, este não é manual | — |
| `BOOT_A_LEFT` / `BOOT_B_LEFT` | `3` / `3` — o valor inicial foi semeado à mão | — |
| Chave de host SSH conhecida pelo host | a do **slot B** | volta a mudar ao voltar para A |

E o aviso que segue do 5.3: **regravar o cartão com uma imagem nova torna aquele `extlinux.conf`
editado inbootável**, porque o PARTUUID do slot B será outro. Reverter o arquivo antes de regravar,
ou reverter mentalmente que a regravação sobrescreve a `med-boot` de qualquer forma.

---

## 8. O que continua sem medição

- **A troca de slot pelo dispositivo.** Nunca aconteceu. O que foi medido é um destino alcançável
  por edição manual.
- **O fallback.** Um boot que falha e volta ao slot anterior não foi exercitado em alvo nenhum — no
  QEMU o mecanismo é outro (`MED_BOOTLOADER = "noop"`, e `boot-attempts` é `uboot`/`barebox`).
- **A continuidade do journal através de uma atualização** (§5.2). Previsto por inspeção; os
  comandos que fecham isso, com a placa no slot B:

  ```sh
  ls /var/log/journal/                    # quantos machine-id existem neste slot
  cat /etc/machine-id                     # e compare com o do slot A
  journalctl --list-boots | head          # o histórico do slot A aparece?
  journalctl --verify                     # o selo ainda valida neste slot
  ```

- **Atualização que troca a versão do kernel** (§5.4). Nunca tentada; o esquema atual não a suporta.
- **Um bundle com conteúdo diferente do slot em execução.** Os dois slots hoje carregam o mesmo
  build, então nada distinguiria "atualizou" de "não atualizou" a não ser o `Booted from:`. Um
  próximo ensaio deve alterar o `DISTRO_VERSION` para que a diferença seja observável.
