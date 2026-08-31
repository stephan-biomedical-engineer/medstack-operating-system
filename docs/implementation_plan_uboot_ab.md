# Plano de implementação — seleção de slot A/B no U-Boot (STM32MP257)

**Estado**: **implementado e validado em hardware, menos o fallback.** O ciclo completo foi medido em
2026-08-31: `make bundle` → `rauc install` → `BOOT_ORDER=B A` → `reboot` → a placa boota o slot B
pelo PARTUUID que o bootloader escolheu, com `rauc.slot=B` na cmdline, e o slot A permanece elegível.
Nenhum `fw_setenv` em nenhum ponto. O que resta é o passo 6 do §8 — o fallback por injeção de falha,
o único item que exercita o mecanismo que só existe para quando algo dá errado. A §10 registra as
sete coisas que o plano dizia errado e a implementação corrigiu.
**Pré-requisitos**: todos satisfeitos — ver §2.
**Leitura obrigatória antes**: `BOOT_SLOT_AB_STM32MP2.md` (o ensaio que mediu o problema),
`BRINGUP_STM32MP2.md` §9.10, `implementation_plan_rauc.md` §3.

---

## 1. Objetivo

Fazer o dispositivo **escolher o slot sozinho**, com a decisão inteira vindo da imagem construída.
Concretamente, um cartão recém-gravado tem de sair da gravação com:

- `BOOT_ORDER`, `BOOT_A_LEFT` e `BOOT_B_LEFT` já preenchidos — nada de `fw_setenv` à mão;
- um `bootcmd` que lê essas variáveis, escolhe o slot, decrementa o contador e boota;
- `rauc.slot=<A|B>` na linha de comando do kernel, para o `rauc-mark-good.service` deixar de ser
  pulado por `Condition*`;
- fallback: um slot que falha `n` vezes seguidas cede a vez ao outro.

Isso fecha, com uma peça só, os quatro itens que a tabela de bloqueios do
`BRINGUP_STM32MP2.md` §10 lista separadamente.

### O que este plano **não** faz

- **Não** resolve identidade por slot (chave SSH, `machine-id`) nem a trilha de auditoria dentro do
  slot — `BOOT_SLOT_AB_STM32MP2.md` §5.1 e §5.2. São problemas reais e independentes, e o §5.2 fica
  **pior** quando a troca de slot passar a funcionar de verdade, porque aí o histórico começa a
  alternar entre dois lugares. Merecem plano próprio.
- **Não** habilita atualização de kernel. `med-boot` continua compartilhada (§5.4).
- **Não** toca no QEMU. Lá `MED_BOOTLOADER = "noop"` e nada abaixo se aplica.

---

## 2. O que já está pronto

Nenhum item abaixo precisa ser refeito; todos têm evidência registrada:

| pré-requisito | estado | evidência |
|---|---|---|
| Partição `u-boot-env` no disco, 512 KiB, fora dos slots | pronto | GPT do `.wic` |
| `fw_printenv`/`fw_setenv` funcionando, por PARTUUID | pronto | round-trip `MED_RT`, §9.10 |
| Ambiente em formato redundante (2 cópias, `flags` após o CRC) | pronto | o `fw_env.config` de duas entradas lê e escreve; uma entrada só falharia o CRC |
| RAUC escreve `BOOT_ORDER` e os contadores | pronto | `BOOT_ORDER=B A` após o install, §9.10 |
| O slot escrito boota | pronto | `Booted from: rootfs.1`, `BOOT_SLOT_AB_STM32MP2.md` §4 |
| `system.conf` com `bootloader=uboot` e `bootname` A/B | pronto | `rauc-conf.bbappend` |

Falta exatamente uma coisa: **alguém que leia `BOOT_ORDER`**.

---

## 3. Decisões de projeto

### 3.1 `bootcmd` no ambiente, não `boot.scr` na `med-boot`

O RAUC recomenda `boot.scr`. Aqui não serve, e a razão é o `SCAN_DEV_FOR_BOOT` do U-Boot distro,
que roda `scan_dev_for_extlinux` **antes** de `scan_dev_for_scripts`: enquanto existir
`extlinux/extlinux.conf` na `med-boot`, um `boot.scr` ao lado nunca executaria. As saídas seriam
apagar o `extlinux.conf` do BSP — brigar com um pacote de terceiro por um arquivo — ou usar
`bootcmd`, que roda antes de qualquer varredura.

**Verificado**, não suposto — e sem console serial: o `u-boot-initial-env` que a `meta-st` publica
traz a variável inteira, e a ordem está nela.

```
scan_dev_for_boot=echo Scanning ${devtype} ${devnum}:${distro_bootpart}...; \
  for prefix in ${boot_prefixes}; do run scan_dev_for_extlinux; run scan_dev_for_scripts; done; \
  run scan_dev_for_efi;
```

E a própria ST manda fazer assim. O cabeçalho do `boot.scr.cmd` dela:

> `SAMPLE BOOT SCRIPT: PLEASE DON'T USE this SCRIPT in REAL PRODUCT … for real product with only one
> supported configuration change the bootcmd in U-Boot`

Há ainda um risco de colisão de arquivo. A `extlinuxconf-stm32mp.bbclass` **já** produz um
`boot.scr.uimg` — só que apenas quando gera mais de um `extlinux.conf`, e esta máquina declara um
devicetree só. Um `boot.scr.uimg` nosso conviveria hoje e passaria a disputar o mesmo caminho com o
pacote da ST no dia em que um segundo devicetree entrasse. Dois pacotes, um caminho, nenhum aviso
até o `do_rootfs`.

**O que a opção rejeitada teria de melhor, e é honesto dizer**: um `boot.scr` dispensaria o §4.3
inteiro. Ele é um arquivo num sistema de arquivos, versionado com a imagem, sem `mkenvimage`, sem
CRC, sem formato redundante, sem deslocamentos — e sem o problema do §4.3.1, que é a parte mais
delicada deste plano. O preço de escolhê-lo seria apagar o `extlinux.conf` do BSP, e com ele o
caminho de resgate do §3.2. **É essa troca que decide, não a precedência**: a precedência apenas
explica por que não dá para ter os dois.

### 3.2 O `extlinux.conf` do BSP fica intacto, como caminho de resgate

Consequência direta de 3.1 e é a decisão mais importante deste plano.

Se o ambiente for perdido ou corromper, o U-Boot cai no `bootcmd` embutido no binário — o distro
padrão — que acha `extlinux/extlinux.conf` e boota o slot A com `root=PARTUUID=` fixo. Ou seja:
**o modo de falha do ambiente é exatamente o comportamento de hoje**, que é conhecido e funciona.

Isso descarta a alternativa que parecia mais elegante: tornar o `extlinux.conf` agnóstico de slot,
trocando `root=PARTUUID=e91c…` por `root=PARTUUID=${med_root}`. Funcionaria — o U-Boot expande
variáveis dentro do `APPEND`, e este projeto já tem prova disso, porque o `console=${console},
${baudrate}` gerado pelo BSP chega ao kernel como `console=ttySTM0,115200`. Mas aí um ambiente
corrompido produz `root=PARTUUID=` vazio e pânico do kernel. **Trocar um caminho de resgate
funcional por um brick silencioso é o oposto do que a política A/B existe para fazer.**

Custo aceito: o `bootcmd` tem de carregar kernel e DTB por conta própria, em vez de delegar ao
`sysboot`. São quatro linhas a mais e o §7.1 diz o que precisa ser medido para escrevê-las.

### 3.3 O ambiente é artefato de build, gravado pelo wic

A partição passa de `--source empty` para `--source rawcopy` de um binário gerado por receita. Três
consequências, e as três são desejadas:

1. um cartão recém-gravado já está no estado de fábrica — que é literalmente o pedido deste plano;
2. o defeito da regra 13 (*"ninguém escreve `BOOT_ORDER` no provisionamento"*) deixa de existir, sem
   unit de primeiro boot, sem correção em runtime;
3. regravar o cartão **reseta** o estado de campo. Correto para uma imagem de fábrica, e encerra em
   definitivo a afirmação da §9.9 de que o ambiente sobrevive a regravações.

A alternativa — semear via `fw_setenv` numa unit systemd de primeiro boot — foi rejeitada porque o
primeiro boot aconteceria *antes* dela, sem seleção de slot, e porque põe em Linux uma decisão que
pertence ao bootloader.

### 3.4 Contadores em hexadecimal, valor máximo 9

O `setexpr` do U-Boot interpreta operandos em **hexadecimal**; o `test` interpreta em decimal salvo
prefixo. Por isso o script de referência do RAUC escreve `test 0x${BOOT_A_LEFT} -gt 0` e por isso o
`uboot.c` do RAUC lê com `g_ascii_strtoull(attempts->str, NULL, 16)` e escreve com `%x`.

Enquanto o valor for ≤ 9 as duas bases coincidem e nada disso importa. A partir de 10 elas divergem
em silêncio — `boot-attempts=16` viraria `"10"`, que o `test` lê como dez e o `setexpr` como
dezesseis. **Fixar 3 e documentar o teto de 9 no arquivo de configuração.**

---

## 4. Mudanças, arquivo a arquivo

Tudo em `meta-med-bsp`. Nada sobe para `meta-med-distro` — isto é bootloader, partição e máquina,
os três nomes que a regra 1 proíbe acima da camada de placa.

### 4.1 `conf/layer.conf` — os dois PARTUUID viram dado declarado

```
MED_ROOT_A_PARTUUID:stm32mp25-disco = "e91c4e10-16e6-4c0e-bd0e-77becf4a3582"
MED_ROOT_B_PARTUUID:stm32mp25-disco = "<novo, fixo, gerado uma vez>"
```

O de A já existe como literal dentro do `.wks`; move para cá e passa a ser substituído, para que a
tabela de partições e o `bootcmd` não possam discordar — mesmo argumento do
`MED_UBOOT_ENV_PARTUUID`, que já mora nessa linha.

**Guarda de build**: o BSP da ST gera o `extlinux.conf` com
`root=PARTUUID=${DEVICE_PARTUUID_ROOTFS:SDCARD}`. Esse valor tem de ser igual a
`MED_ROOT_A_PARTUUID`, senão o caminho de resgate do §3.2 aponta para lugar nenhum. Uma linha de
`bbfatal` comparando os dois — porque hoje eles coincidem por acidente histórico, não por
construção.

### 4.2 `wic/med-partitions-stm32mp2.wks.in`

```diff
-part / --source rootfs --part-name med-root-a … --uuid e91c4e10-16e6-4c0e-bd0e-77becf4a3582
+part / --source rootfs --part-name med-root-a … --uuid ${MED_ROOT_A_PARTUUID}

-part --part-name med-root-b --label med-root-b --ondisk mmcblk --fstype=ext4 --align 1024 --fixed-size 1024
+part --part-name med-root-b --label med-root-b --ondisk mmcblk --fstype=ext4 --align 1024 --fixed-size 1024 --uuid ${MED_ROOT_B_PARTUUID}

-part u-boot-env --source empty --part-name=u-boot-env … --fixed-size 512K --uuid ${MED_UBOOT_ENV_PARTUUID}
+part u-boot-env --source rawcopy --sourceparams="file=${DEPLOY_DIR_IMAGE}/med-uboot-env.bin" \
+     --part-name=u-boot-env … --fixed-size 512K --uuid ${MED_UBOOT_ENV_PARTUUID}
```

O comentário que hoje diz *"Slot B has no such constraint yet precisely because nothing boots it
yet"* precisa ser reescrito, não apagado: ele estava certo e o ensaio de 30/08 o derrubou.

E `WKS_FILE_DEPENDS:append:stm32mp25-disco = " med-uboot-env-image"`, senão o wic corre antes de o
arquivo existir e falha com "cannot stat" — o mesmo sintoma três passos distante da causa que custou
a §2 deste porte.

**Nome puro, nunca `nome:do_deploy`** — ver §10.5.

### 4.3 `recipes-bsp/med-uboot-env/med-uboot-env-image_1.0.bb` — receita nova

Gera o binário do ambiente com `mkenvimage` (de `u-boot-mkimage-native`, já usado pelo BSP) e o
deposita em `DEPLOY_DIR_IMAGE`.

#### 4.3.1 O ambiente gravado tem de ser **completo**, não só as nossas variáveis

Este é o ponto que faz ou quebra a receita, e ele é contraintuitivo.

Quando o U-Boot encontra um ambiente salvo válido, ele **substitui** o ambiente embutido por
inteiro: `env_import()` chama `himport_r()` sem `H_NOCLEAR`, o que limpa a tabela antes de importar.
Não há mescla. Um binário contendo só `bootcmd`, `BOOT_ORDER` e os contadores produziria um U-Boot
**sem** `distro_bootcmd`, sem `boot_targets`, sem `kernel_addr_r`, sem `console` e sem `baudrate` —
ou seja, sem o caminho de resgate do §3.2, sem os endereços que o passo 5 do §5 usa e sem as
variáveis que o `extlinux.conf` expande. O aparelho não bootaria, e a causa estaria a três passos do
sintoma.

Portanto a receita **parte do ambiente padrão do próprio U-Boot** e aplica as nossas variáveis por
cima:

1. `u-boot-stm32mp` já gera `u-boot-initial-env` — `u-boot-stm32mp.inc` faz
   `require recipes-bsp/u-boot/u-boot.inc`, e lá `UBOOT_INITIAL_ENV ?= "${PN}-initial-env"` dispara
   um `oe_runmake u-boot-initial-env`. Nada precisa ser habilitado;
2. sobre esse arquivo, substituir `bootcmd` e acrescentar `BOOT_ORDER`, `BOOT_A_LEFT`,
   `BOOT_B_LEFT`, `med_root_a` e `med_root_b`;
3. só então `mkenvimage`.

**Consequência estrutural**: poky gera o arquivo mas só o *instala*, em `${sysconfdir}` do pacote do
U-Boot; o `do_deploy` não o toca, e `${sysconfdir}` não está em `SYSROOT_DIRS`. É preciso um bbappend
em `u-boot-stm32mp` que o copie para o `DEPLOYDIR`.

A regra da camada ("*provides and does not consume*") continua respeitada, porque o append é sobre
uma receita de uma camada **abaixo** (prioridade 6), não acima. Mas há uma armadilha: `meta-med-bsp`
está na pilha dos **dois** perfis, e um `.bbappend` para uma receita que nenhuma camada fornece é
erro de parse — foi assim que o `rauc-conf_%.bbappend` quebrou este build uma vez. Então **não** é
`LAYERDEPENDS`, que tornaria a camada da ST obrigatória e quebraria o QEMU; é `BBFILES_DYNAMIC` com
o append sob `dynamic-layers/`, mais `LAYERRECOMMENDS`. E a coleção chama-se `stm-st-stm32mp`, não
`meta-st-stm32mp`.

**Onde o arquivo aparece não é onde se espera.** A `meta-st` define
`do_deploy[sstate-outputdirs] = "${DEPLOY_DIR_IMAGE}${FIP_DIR_UBOOT}"`, de modo que tudo o que essa
tarefa publica — inclusive o que o nosso append põe em `${DEPLOYDIR}` — é remapeado para o
subdiretório `u-boot/`, e não para o topo do `DEPLOY_DIR_IMAGE`. Ver §10.6.

E o conteúdo confirma que nada mais falta: `kernel_addr_r`, `fdt_addr_r`, `kernel_comp_addr_r`,
`kernel_comp_size`, `console` e `baudrate` estão todos lá, nas 55 variáveis do
`u-boot-initial-env-default_stm32mp25`.

**A rota de resgate é capturada, não escrita.** O `bootcmd` do vendor — `run bootcmd_stm32mp` neste
alvo — é lido em tempo de build e regravado como `med_rescue` antes de `bootcmd` ser substituído.
Assim o fallback é, por construção, exatamente o que a placa fazia antes; escrever
`run bootcmd_stm32mp` à mão estaria certo hoje e silenciosamente errado depois de um bump do BSP.

Uma verificação de sanidade grátis vem da placa: o ambiente atual tem **62 entradas**
(`fw_printenv | wc -l`). Se a receita produzir um binário com meia dúzia, ela está errada, e essa é
a asserção mais barata do §4.3.


- `PACKAGE_ARCH = "${MACHINE_ARCH}"` — o conteúdo carrega PARTUUID de uma tabela de partições
  específica. Mesmo motivo do `med-uboot-env-config` e do `med-data-volume`.
- Formato: **redundante** (`mkenvimage -r`), `-s ${MED_UBOOT_ENV_SIZE}` (0x2000), duas cópias
  idênticas nos deslocamentos que o `env/mmc.c` calcula — `tamanho − 0x2000` e `tamanho − 0x4000` —
  com o resto da partição em zeros.
- Nada de novo aqui: esses deslocamentos já estão derivados e documentados no
  `med-uboot-env-config_1.0.bb`, que é a *outra* metade da mesma decisão. As duas receitas têm de ler
  a mesma variável, nunca dois literais.

**A receita verifica o próprio artefato**, em `do_deploy[postfuncs]`, e falha o build se:

- o arquivo não tiver exatamente o tamanho da partição;
- as duas cópias não forem byte a byte iguais;
- o CRC32 armazenado não bater com o recalculado sobre a área de dados (a área de dados exclui os 4
  bytes de CRC **e** o byte de `flags` — errar isso produz um ambiente que o U-Boot descarta em
  silêncio, caindo no default embutido, e o sintoma seria "boota normalmente", que é o pior sintoma
  possível);
- `BOOT_ORDER`, `BOOT_A_LEFT`, `BOOT_B_LEFT` e `bootcmd` não estiverem presentes;
- o número de variáveis for muito menor que o do `u-boot-initial-env` de origem — a assinatura
  de ter gerado um ambiente parcial (§4.3.1).

Isso é deliberado e é a lição da suíte `make check`: *uma asserção que nunca viu a falha que procura
é uma alegação, não uma verificação*. Aqui a falha é fácil de injetar — mudar o cálculo do CRC e
confirmar que o build quebra.

### 4.4 `recipes-core/rauc/rauc-conf.bbappend` — tornar explícito o que hoje é default

São os defaults do RAUC (`UBOOT_DEFAULT_ATTEMPTS` / `UBOOT_ATTEMPTS_PRIMARY`), mas o `bootcmd` vai
depender do valor, e um número que dois componentes precisam concordar não deve existir só como
default implícito de um deles.

**E não podem ser escritos literalmente no arquivo.** O `system.conf` é compartilhado por todos os
alvos, e o RAUC **recusa carregar a configuração inteira** se qualquer dos dois for definido para um
backend que não seja `uboot`/`barebox`:

```c
/* src/config_file.c:393 */
if (c->boot_default_attempts > 0 || c->boot_attempts_primary > 0) {
        if ((g_strcmp0(c->system_bootloader, "uboot") != 0) && (g_strcmp0(c->system_bootloader, "barebox") != 0)) {
                g_set_error(..., "Configuring boot attempts is valid for uboot or barebox only (not for %s)", ...);
```

No QEMU o `MED_BOOTLOADER` é `noop`, então um valor fixo aqui pararia o `rauc.service` naquele alvo
e derrubaria o `make check` — enquanto o build do STM32 seguiria verde. Exatamente a forma de defeito
que este repositório continua redescobrindo. A linha é um placeholder `@MED_BOOT_ATTEMPTS@` que o
bbappend substitui pelas duas chaves em `uboot`/`barebox` e **apaga** em qualquer outro caso.

O valor em si mora em `med-os.conf` como `MED_BOOT_ATTEMPTS`, porque tem dois consumidores em duas
camadas: o `system.conf` aqui e o valor de fábrica de `BOOT_<slot>_LEFT` que `meta-med-bsp` grava no
ambiente. Com o comentário do teto de 9 do §3.4.

*(Esse arquivo está em `meta-med-distro`. Não é violação de camada: são políticas de atualização,
não nomes de placa.)*

---

## 5. O `bootcmd`

Estrutura, derivada de `contrib/uboot.sh` do RAUC e adaptada a este disco:

```
# 1. estado de fábrica, caso o ambiente seja mais velho que este bootcmd
test -n "${BOOT_ORDER}"  || setenv BOOT_ORDER "A B"
test -n "${BOOT_A_LEFT}" || setenv BOOT_A_LEFT 3
test -n "${BOOT_B_LEFT}" || setenv BOOT_B_LEFT 0

# 2. escolher o primeiro slot da ordem com tentativas restantes
setenv med_slot
for BOOT_SLOT in "${BOOT_ORDER}"; do
  if test "x${med_slot}" != "x"; then
    # já escolhido, ignora o resto
  elif test "x${BOOT_SLOT}" = "xA"; then
    if test 0x${BOOT_A_LEFT} -gt 0; then
      setexpr BOOT_A_LEFT ${BOOT_A_LEFT} - 1
      setenv med_slot A
      setenv med_root ${med_root_a}
    fi
  elif test "x${BOOT_SLOT}" = "xB"; then
    if test 0x${BOOT_B_LEFT} -gt 0; then
      setexpr BOOT_B_LEFT ${BOOT_B_LEFT} - 1
      setenv med_slot B
      setenv med_root ${med_root_b}
    fi
  fi
done

# 3. nenhum slot elegível: restaura e reinicia, em vez de travar
if test "x${med_slot}" = "x"; then
  echo "MedOS: no bootable slot, restoring attempt counters"
  setenv BOOT_ORDER "A B"; setenv BOOT_A_LEFT 3; setenv BOOT_B_LEFT 3
  saveenv
  reset
fi

# 4. persistir ANTES de bootar
saveenv

# 5. bootar
setenv bootargs "root=PARTUUID=${med_root} rootwait rw earlycon console=${console},${baudrate} rauc.slot=${med_slot}"
<carregar Image.gz e o DTB da med-boot>
booti ${kernel_addr_r} - ${fdt_addr_r}

# 6. se chegou aqui, o boot falhou sem resetar: cai no caminho de resgate
run distro_bootcmd
```

Três pontos onde este projeto já pagou o preço de errar:

- **`saveenv` no passo 4, antes de bootar, nunca depois.** O contador registra que a tentativa
  *começou*, não que ela terminou. Decrementar depois que o sistema subiu significa que um kernel
  que entra em pânico nunca decrementa, e o aparelho fica no slot ruim para sempre — o fallback
  existiria e nunca dispararia. É a mesma família do laço de reinício que nunca chega a `failed`
  (regra 11): o mecanismo de proteção que não dispara é pior que a ausência dele, porque ninguém
  procura.
- **`rauc.slot=${med_slot}` no passo 5** é o que faz o `rauc-mark-good.service` deixar de ser
  pulado. Sem ele, o contador é decrementado a cada boot e nunca restaurado: o aparelho gasta as
  três tentativas em três boots bem-sucedidos e cai para o outro slot sem que nada tenha falhado.
  **Os dois itens andam juntos ou o dispositivo fica pior do que está hoje.**
- **PARTUUID no passo 5, nunca `mmcblkN` nem `by-partlabel`.** Regra 9 — quatro boots, quatro
  enumerações — e o eMMC de fábrica já venceu uma disputa de rótulo nesta placa.

---

## 6. Sequenciamento

Cada passo é verificável isolado, e os dois primeiros não podem ser adiados para depois do terceiro:

1. **PARTUUID fixo do slot B** (§4.1, §4.2). Sem isso o `bootcmd` não tem para onde apontar.
   *Verificação*: GPT do `.wic` produzido mostra o valor declarado.
2. **Deploy do `u-boot-initial-env`** (bbappend em `u-boot-stm32mp`, §4.3.1) e **receita do ambiente
   + autoverificação** (§4.3), ainda com a partição `--source empty`.
   *Verificação*: `med-uboot-env.bin` deployado com contagem de variáveis próxima das 62 da placa;
   injetar erro no CRC e confirmar que o build quebra.
3. **`--source rawcopy` na `.wks`** (§4.2). *Verificação*: extrair a partição do `.wic` e ler o
   ambiente dela com o `fw_printenv` do host.
4. **`bootcmd` completo** (§5) dentro do ambiente. *Verificação*: a matriz do §8.
5. **`boot-attempts` explícito** (§4.4).

Depois de 1–3 e antes de 4, o cartão já sai de fábrica com `BOOT_ORDER` preenchido e continua
bootando por `extlinux` — estado intermediário útil, que sozinho já elimina o defeito da regra 13.

---

## 7. Riscos

### 7.1 Como carregar o kernel — o único desconhecido real

O `bootcmd` precisa de número do dispositivo MMC e da partição `med-boot`. Ambos têm de vir de
variável, não de literal: a numeração é uma corrida. O U-Boot da ST expõe `${boot_device}` e
`${boot_instance}` (o `boot.scr.cmd` deles os usa), e o log de boot mostra `Scanning mmc 0:8`.

**Respondido sem console serial**, lendo o `u-boot-initial-env` publicado: `kernel_addr_r`
(`0x8a000000`), `fdt_addr_r` (`0x90000000`), `kernel_comp_addr_r`, `kernel_comp_size`, `console` e
`baudrate` estão todos compilados no ambiente padrão, e a receita falha o build se algum sumir.
`boot_device`, `boot_instance` e `fdtfile` **não** estão — são definidos pelo código de placa da ST
em tempo de execução, o que é o esperado; `med_seed` tem fallback para os dois primeiros (`mmc`/`0`)
e, sem `fdtfile`, o `load` falha e o `bootcmd` cai na rota de resgate, que é a degradação desenhada.

O que continua exigindo a placa é confirmar que `${boot_device}${boot_instance}` de fato aponta para
o cartão, e que `load mmc N#med-boot` aceita nome de partição. As duas falham de forma segura.

### 7.2 A ST pode reescrever o ambiente

O `boot.scr.cmd` da ST faz `env save` e `env default` em vários pontos. Aquele script não está em
uso nesta máquina (só um `extlinux.conf` é gerado, então nenhum `boot.scr.uimg` é produzido —
confirmado no conteúdo da `med-boot`), mas o ambiente da placa tinha 62 entradas e um `BOOT_ORDER`
que sumiu na regravação de 28/08 sem explicação estabelecida. **Se alguma coisa no U-Boot da ST
restaura defaults sob alguma condição, ela apagaria o `bootcmd` deste plano.**

**Resolvido, e a causa do mistério de 28/08 apareceu junto.** O ambiente padrão da ST contém:

```
env_check=if env info -p -d -q; then env save; fi
```

`env info -d` é verdadeiro quando o ambiente **é** o default embutido — isto é, quando o armazenado
falhou o CRC ou não existe. Então o U-Boot regrava o default sempre que o encontra ausente, e é
exatamente por isso que a placa, depois da regravação de 28/08, apresentou 62 variáveis e nenhum
`BOOT_ORDER`: o cartão apagou a partição e o próprio U-Boot repovoou-a com o default. Não foi a
`meta-st` sobrescrevendo nada — foi o comportamento correto de recuperação.

A consequência para este plano é favorável: com um ambiente **válido** gravado pela imagem,
`env info -d` é falso e o `env_check` não escreve nada. E num cartão corrompido ele restaura um
estado que boota. Detecção mesmo assim: `fw_printenv bootcmd` na matriz do §8.

### 7.3 O ambiente congela uma cópia do padrão do U-Boot

Consequência direta do §4.3.1: o binário gravado carrega o `u-boot-initial-env` da versão do U-Boot
com que foi construído. Atualizar o U-Boot (FIP) sem regravar a partição de ambiente deixa o
aparelho rodando com o ambiente antigo — e o RAUC hoje não atualiza nem o FIP nem o `u-boot-env`, de
modo que essa divergência é o estado normal de qualquer aparelho já em campo.

Não é regressão: é a situação de hoje, apenas nomeada. Mas é a razão pela qual uma futura
atualização de bootloader terá de tratar os dois juntos, e pertence ao mesmo plano que resolver o
§5.4.

### 7.4 `saveenv` a cada boot

Escrita de 8 KiB no cartão por boot, e uma janela de corte de energia durante ela. É exatamente para
isso que o ambiente redundante existe, e é por isso que o §4.3 exige as duas cópias. Não medido:
desgaste ao longo do tempo.

---

## 8. Validação

Nada abaixo é "o build passou". Cada linha é um artefato lido ou um comportamento observado.

### No host

| verificação | como |
|---|---|
| PARTUUID de B no disco | `sgdisk -i <n>` no `.wic` = `MED_ROOT_B_PARTUUID` |
| A do disco = A do `extlinux.conf` | comparar os dois no `.wic` produzido; a guarda do §4.1 deve tornar isso impossível de divergir |
| Ambiente válido dentro do `.wic` | extrair a partição, apontar um `fw_env.config` para o arquivo, `fw_printenv` |
| Autoverificação do §4.3 funciona | injetar erro no CRC e confirmar que o build falha |
| Nada regrediu no QEMU | `make qemu` verde, `make check` 21/21 |

**Conferindo o cartão depois de gravar**, e há uma armadilha: o `bmaptool` escreve **só os blocos
que o `.bmap` mapeia**, e o resto da mídia conserva o que já estava lá. Um `sha256sum` sobre uma
faixa crua do cartão só pode bater onde a cobertura é de 100%.

| região | blocos mapeados | hash cru do cartão serve? |
|---|---|---|
| `u-boot-env` | 129 de 129 (100%) | **sim** — é rawcopy de um arquivo com zeros explícitos |
| `med-boot` | 4047 de 16385 (24,7%) | **não** — 75% da partição nunca é escrita |

Para a `med-boot`, compare os arquivos (`Image.gz-6.6.129`, `extlinux/extlinux.conf`, o `.dtb`) com
os que o build produziu, montando `-o ro`. Uma verificação que não pode passar é pior que nenhuma:
ela produz um alarme falso e consome a atenção que o alarme verdadeiro precisaria.

### Na placa

Depois de gravar, **sem tocar em `fw_setenv`** — este é o ponto do plano inteiro:

| # | ação | esperado |
|---|---|---|
| 1 | primeiro boot | `BOOT_ORDER=A B` já preenchido; `cat /proc/cmdline` traz `rauc.slot=A`; `rauc status` → `Activated: rootfs.0 (A)`, A `good` |
| 2 | `systemctl status rauc-mark-good.service` | `active (exited)` — não mais "condition unmet" |
| 3 | `fw_printenv BOOT_A_LEFT` após 3 boots limpos | continua `3` — prova que o `mark-good` restaura |
| 4 | `rauc install` e reboot | boota **B**, sem editar arquivo nenhum |
| 5 | `fw_setenv BOOT_ORDER "B A"; fw_setenv BOOT_B_LEFT 0`, reboot | boota **A** — testa o *pulo* do slot sem quebrar nada |
| 6 | **injeção de falha**: zerar o superbloco do slot B, `BOOT_ORDER="B A"`, `BOOT_B_LEFT=3`, reboot | três tentativas em B, pânico em cada uma, e o quarto boot cai em **A**. Recuperável reinstalando o bundle |

O passo 6 é o único que prova o fallback, e é obrigatório. Sem ele, o fallback é uma alegação — a
mesma armadilha que o `acq-active` da suíte de aceitação já ensinou: uma asserção que nunca viu a
falha que procura não é uma verificação.

### Resultado medido — passos 1 e 2 (2026-08-31)

Cartão recém-gravado, primeiro boot, **nenhum comando manual antes**:

```
$ cat /proc/cmdline
root=PARTUUID=e91c4e10-16e6-4c0e-bd0e-77becf4a3582 rootwait rw earlycon console=ttySTM0,115200 rauc.slot=A

$ rauc status
Booted from: rootfs.0 (A)
Activated:   rootfs.0 (A)
x [rootfs.0] (med-root-a, ext4, booted)   bootname: A   boot status: good
o [rootfs.1] (med-root-b, ext4, inactive) bootname: B   boot status: bad

$ systemctl status rauc-mark-good.service
Active: active (exited) since Mon 2026-08-31 03:37:41 UTC
Process: 522 ExecStart=/usr/bin/rauc status mark-good (code=exited, status=0/SUCCESS)

$ fw_printenv BOOT_ORDER BOOT_A_LEFT BOOT_B_LEFT
BOOT_ORDER=A B
BOOT_A_LEFT=3
BOOT_B_LEFT=0
```

Cinco coisas, e cada uma fecha um item que estava aberto:

1. **`rauc.slot=A` na linha de comando.** Prova que o `bootcmd` novo executou; se tivesse caído no
   `med_rescue`, o argumento não existiria. É o discriminador desenhado para esta medição e ele
   funcionou como discriminador.
2. **`Activated: rootfs.0 (A)` num aparelho recém-gravado.** Antes disto, um cartão saído do
   `bmaptool` reportava `Activated: none` e *todos* os slots `bad`, inclusive o que estava
   executando. O estado de fábrica agora vem da imagem.
3. **`rauc-mark-good.service` rodou sozinho.** Desde o início do porte a unidade era pulada por
   `ConditionKernelCommandLine=|rauc.slot` — e uma unidade pulada por `Condition*` não falha, não
   avisa e não aparece em `--state=failed` (regra 11). Ela agora aparece como `active (exited)`.
4. **`BOOT_A_LEFT=3` depois do boot é evidência de que o laço fechou.** O `med_select` decrementou
   para 2 e gravou antes de carregar o kernel; ler 3 em userspace significa que o `mark-good`
   restaurou. Escrita pelo bootloader, restauração pelo userspace, no mesmo ciclo.
5. **`Booted from: rootfs.0 (A)`**, e não `(/dev/mmcblk0p9)` como em toda medição anterior. O RAUC
   passou a identificar o slot pelo `rauc.slot=` que o bootloader declara, em vez de inferi-lo do
   dispositivo de root. A identificação deixou de depender de uma corrida de enumeração.

### Resultado medido — passo 4, a troca (2026-08-31)

Bundle reconstruído a partir da imagem que a placa está rodando (`Build: '20260831034731'`,
payload 747.569.152 B, checksum `bb6cc1e7…`), transferido por `scp`, instalado e a placa reiniciada.

**Depois do `rauc install`, ainda no slot A:**

```
Activated: rootfs.1 (B)          B: boot status good     A: boot status good
BOOT_ORDER=B A   BOOT_A_LEFT=3   BOOT_B_LEFT=3

$ dd if=/dev/disk/by-partlabel/med-root-b bs=4096 count=182512 | sha256sum
bb6cc1e7457ba8e9a300beb27935a53867a83747f6546a099b76c7403c084ad0    ← = Checksum do manifesto
```

**Depois do `reboot`:**

```
$ cat /proc/cmdline
root=PARTUUID=35822773-a851-41e9-af87-4f6fa8e8b905 rootwait rw earlycon console=ttySTM0,115200 rauc.slot=B

$ rauc status
Booted from: rootfs.1 (B)        Activated: rootfs.1 (B)
x [rootfs.1] (med-root-b) bootname: B  boot status: good  booted
o [rootfs.0] (med-root-a) bootname: A  boot status: good  inactive

$ fw_printenv BOOT_ORDER BOOT_A_LEFT BOOT_B_LEFT
BOOT_ORDER=B A   BOOT_A_LEFT=3   BOOT_B_LEFT=3
```

Aquele PARTUUID é o `--uuid` que a §4.2 fixou para o slot B, e ele fecha a cadeia inteira: o RAUC
escolheu o slot inativo, escreveu, verificou, reordenou o ambiente; o U-Boot leu `BOOT_ORDER`,
gastou uma tentativa, montou o `root=` correspondente e declarou `rauc.slot=B`; o kernel montou o
slot que o bootloader escolheu; e o `mark-good` restaurou `BOOT_B_LEFT` para 3 de dentro do slot
novo. **`BOOT_A_LEFT=3` intocado é a outra metade do resultado** — o slot anterior segue elegível,
que é o que torna a atualização reversível em vez de destrutiva.

Uma confirmação que veio de graça, e que é do §5.1 do `BOOT_SLOT_AB_STM32MP2.md`: a fingerprint SSH
mudou de `SHA256:lAW0DlTkHJ63PgC1jHXNjjfsnXuvDHhQYteDSZR3czo` (slot A) para
`SHA256:25lyKZU9Sb2zgzhrpVyN/yo/4xxWoEezZwmQJowLuhQ` (slot B). Aquele defeito estava registrado a
partir de um boot manual; agora está medido **numa atualização de verdade**, que é o cenário em que
ele importa: um aparelho em campo troca de identidade ao se atualizar.

**Não medido ainda**: passos 3, 5 e 6 — a permanência do contador em boots repetidos, o pulo de um
slot com contador zerado, e o fallback por injeção de falha. O sexto é o único que exercita o
mecanismo que existe para falhar.

Registrar os resultados em `RESULTS.md` §8 e em `BOOT_SLOT_AB_STM32MP2.md`; a §6 deste arquivo
recebe o que foi medido, como fizeram os planos do RAUC e do LUKS.

---

## 9. O que continua em aberto depois deste plano

- Identidade por slot e trilha de auditoria dentro do slot (`BOOT_SLOT_AB_STM32MP2.md` §5.1, §5.2).
  O §5.2 **piora** com este plano em vigor.
- Atualização de kernel (§5.4) — exige decidir entre kernel dentro do slot ou `med-boot` também A/B.
- Boot verificado / assinatura da cadeia. O `bootcmd` passa a ser dado confiável no disco sem
  autenticação nenhuma; quem escreve o cartão escolhe o que boota. Consistente com o resto do
  estado de desenvolvimento (chave em `med-boot`, CA de desenvolvimento), e não deve ser descrito
  como resolvido.
- Desgaste do cartão pelo `saveenv` por boot (§7.4).

---

## 10. O que a implementação corrigiu no plano

Registrado porque as duas correções são do mesmo tipo — *o plano estava certo sobre o objetivo e
errado sobre o mecanismo* — e porque a segunda teria quebrado um alvo que ninguém estava olhando.

### 10.1 `LAYERDEPENDS` seria a forma errada de depender do BSP da ST

O §4.3.1 dizia que `meta-med-bsp` teria de declarar `meta-st-stm32mp` em `LAYERDEPENDS`. Isso
tornaria a camada da ST **obrigatória**, e `meta-med-bsp` está na pilha dos dois perfis: o QEMU
deixaria de parsear. E o `.bbappend` seria pior ainda — um append para uma receita que nenhuma
camada fornece é erro duro de parse, que é como o `rauc-conf_%.bbappend` quebrou este build uma vez.

A forma correta é `BBFILES_DYNAMIC`, com o append sob `dynamic-layers/<coleção>/`, mais
`LAYERRECOMMENDS`. E a coleção chama-se **`stm-st-stm32mp`**, não `meta-st-stm32mp` — um nome que
não casa com coleção alguma não é reportado, é ignorado, e a falha apareceria dois passos adiante
como um `u-boot-initial-env` inexistente.

### 10.2 `boot-attempts` literal pararia o RAUC no QEMU

Esta é a mais séria, e foi encontrada lendo o `config_file.c` para conferir o nome exato das chaves —
não por um teste. O RAUC recusa **a configuração inteira** se `boot-attempts` estiver presente com um
backend que não seja `uboot`/`barebox`. O `system.conf` é um arquivo só para todos os alvos, e no
QEMU o backend é `noop`.

Efeito de escrever o valor literal: `make stm32` verde, `make qemu` verde, `.wic` correto — e o
`rauc.service` morto no QEMU, derrubando o `make check`, que é a única suíte automatizada do
repositório. Um alvo quebrado por uma mudança feita para o outro.

Correção: placeholder `@MED_BOOT_ATTEMPTS@`, substituído pelas duas chaves em `uboot`/`barebox` e
**removido** em qualquer outro caso. A guarda que já existia no bbappend — falhar o build se sobrar
qualquer `@MED_*@` — cobre o caso de alguém acrescentar um terceiro backend e esquecer do `case`.

### 10.3 O binário não continha o estado de fábrica (achado por teste)

A primeira versão do `med-bootcmd.env` definia `med_seed`, que cria `BOOT_ORDER` e os contadores
**em tempo de boot**, e não os declarava como variáveis. O binário teria saído sem eles — ou seja,
sem a única coisa que este plano existe para entregar, e o primeiro boot voltaria a criar o estado
em runtime.

Foi pego por um teste da lógica de montagem e verificação rodado fora do bitbake, antes de qualquer
build: a asserção "`BOOT_ORDER` está no binário" falhou. O `med_seed` continua lá como segunda linha
de defesa para um aparelho cujo ambiente é mais antigo que este `bootcmd`; em vida normal suas
guardas nunca disparam.

### 10.4 A verificação foi exercitada contra as falhas que procura

Três defeitos injetados na lógica de verificação, todos detectados:

| falha injetada | detectada |
|---|---|
| um byte trocado na área de dados | sim |
| CRC calculado sobre dados **+** o byte de `flags` (o erro clássico de um byte) | sim |
| formato não-redundante, sem o byte de `flags` | sim |

Os três produziriam, na placa, o mesmo sintoma: U-Boot descarta o ambiente, cai no default embutido,
boota o slot A pelo `extlinux.conf` e **tudo parece normal**. É por isso que são asserções de build e
não itens de uma lista de bancada.

### 10.5 `WKS_FILE_DEPENDS` não aceita sufixo de tarefa

`make parse BOARD=stm32` passou o `bitbake -p` (2916 receitas, 0 erros — a receita nova e o bbappend
dinâmico estão corretos) e falhou no grafo de tarefas:

```
ERROR: Nothing PROVIDES 'med-uboot-env-image:do_deploy' (but .../med-image-eeg.bb DEPENDS on
or otherwise requires it). Close matches:
  med-uboot-env-image
```

`WKS_FILE_DEPENDS` vai para `DEPENDS` (`image_types_wic.bbclass:119`), que aceita nomes de receita e
não `receita:tarefa`, então o sufixo virou parte do nome. Quem garante que os artefatos de deploy
existem é outra linha da mesma classe:

```
# We ensure all artfacts are deployed (e.g virtual/bootloader)
do_image_wic[recrdeptask] += "do_deploy"
```

`recrdeptask` é recursivo sobre a árvore, de modo que nomear a receita basta — e é exatamente assim
que a `meta-st` põe o TF-A e o FIP no `DEPLOY_DIR_IMAGE` antes de suas próprias linhas `rawcopy` os
lerem. A referência estava disponível ao lado o tempo todo; eu inventei uma sintaxe em vez de ler a
que já funcionava três linhas acima no mesmo arquivo.

### 10.6 O arquivo estava num subdiretório, e eu apaguei o código que funcionava

Dois ciclos de build, um erro só, e vale registrar inteiro porque o erro não foi de código.

**Ciclo 1.** A receita procurou `u-boot-initial-env-default_stm32mp25` no topo do
`DEPLOY_DIR_IMAGE` e não achou. A mensagem dizia *"the initial environments actually deployed are:
none"*, porque listava apenas aquele nível. Os arquivos estavam em `u-boot/`, um nível abaixo, com
exatamente aquele nome. **Um diagnóstico que procura no lugar errado é pior que nenhum, porque
afirma uma ausência e é acreditado.**

**Ciclo 2, e este foi mais caro.** Ao investigar, encontrei os três arquivos em
`deploy/images/stm32mp25-disco/u-boot/` e concluí que a `meta-st` os publicava sozinha — logo, que o
bbappend era supérfluo. Apaguei o append, o `dynamic-layers/`, o `BBFILES_DYNAMIC` e a
`LAYERRECOMMENDS`, e reescrevi esta seção celebrando a simplificação. O build seguinte falhou com
*"no u-boot-initial-env at all"*: os arquivos tinham vindo **do append**, que havia funcionado
perfeitamente. Ele os instala em `${DEPLOYDIR}`, e `${DEPLOYDIR}` para essa tarefa **é** o
subdiretório `u-boot/`, porque a `meta-st` remapeia a saída com
`do_deploy[sstate-outputdirs]`. Poky nunca publica esse arquivo — apenas o instala no pacote, o que
se confirma em três segundos lendo o `do_deploy` do `u-boot.inc`.

A cadeia de raciocínio errada foi: *"a raiz do deploy está vazia"* → *"o append não rodou"* →
*"então quem pôs os arquivos foi o vendor"* → *"logo o append é desnecessário"*. O primeiro passo já
estava errado, porque `DEPLOYDIR` não é a raiz do deploy quando alguém remapeia
`sstate-outputdirs` — e nada nos três passos seguintes tinha como corrigir isso.

**A regra que fica**: quando uma evidência negativa (*"o arquivo não está aqui"*) sustenta uma
conclusão sobre causa, verifique primeiro que "aqui" é o lugar certo. E antes de remover código como
redundante, prove que a coisa que ele produz existiria sem ele — um `grep` no `do_deploy` do vendor
teria bastado, e teria custado menos que dois builds.

O que *funcionou* nos dois ciclos: a falha aconteceu na receita certa, na tarefa certa, com o valor
esperado impresso e a variável a ajustar nomeada. O defeito não foi ter falhado — foi ter falado com
confiança sobre um diretório que não tinha olhado. A mensagem agora varre a árvore inteira com
`os.walk`.

### 10.7 `PACKAGES = ""` não é o mesmo que "não empacote"

A receita do ambiente não produz pacote nenhum — o artefato é entrada do wic, não conteúdo do
rootfs — e `PACKAGES = ""` parecia dizer isso. Não diz. O `do_package` continua rodando, vê que não
há nada a fazer e **sai antes de criar `packages-split/`**; o postfunc que o buildhistory pendura
nessa tarefa então falha em `find .../packages-split/*: No such file or directory`.

O correto é `inherit nopackages`, que apaga as tarefas de empacotamento com `deltask` — sem tarefa,
não há onde pendurar o postfunc. Uma lista vazia descreve o resultado; `deltask` descreve a
intenção, e só a segunda sobrevive a alguém acrescentar um postfunc.

---

## 11. O que falta para fechar

Nada disso rodou ainda — o disco de cache estava desconectado quando o código foi escrito.

1. `make parse BOARD=stm32` e `make parse` (QEMU). O segundo é o que prova que o `BBFILES_DYNAMIC` e
   o `@MED_BOOT_ATTEMPTS@` não quebraram o alvo que não recebeu a funcionalidade.
2. `make stm32 KEY=development`. As asserções do §4.3 rodam sozinhas; se alguma falhar, a mensagem
   diz o que fazer.
3. `make check` no QEMU — 21/21, ou o §10.2 não foi corrigido tão bem quanto parece.
4. A matriz de seis passos do §8, na placa. O sexto é obrigatório.
