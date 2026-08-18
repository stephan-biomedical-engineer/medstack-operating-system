# Resultados Medidos — MedPlatform

> **Status**: registro de medição, não de projeto. Os `implementation_plan_*.md` dizem o que se
> pretende fazer; este arquivo diz o que foi **observado**, com o método ao lado de cada número.
> `PROJECT_CONTEXT.md` continua sendo a referência arquitetural.
>
> **Quando**: 2026-08-16, exceto a §8 (portabilidade para o STM32MP257), de 2026-08-17.
> **Onde**: `qemux86-64`, perfis `med-image-eeg` e `med-image-tomograph`; a §8 em
> `stm32mp25-disco`. A medição de reuso (§2) é sobre o commit `f0e4427`; as demais foram tomadas
> no mesmo dia, sobre os commits que as introduziram.
>
> **Regra deste arquivo**: cada número traz o comando que o produziu e o que ele **não** significa.
> Um número sem limite declarado é mais perigoso que nenhum número.

---

## 1. Como ler os resultados

Este projeto aprendeu, da forma cara, que num sistema com caminho de atualização **"compilou" e
"bootou" não são evidência**. Dos seis defeitos encontrados ao implementar o caminho A/B (registrados
em `implementation_plan_rauc.md` §8.3), cinco não fizeram build nenhum falhar: produziram artefatos
que compilavam, bootavam e rodavam, e só se manifestariam num dispositivo em campo tentando
atualizar.

Por isso todo número abaixo vem de **executar o artefato ou inspecionar o artefato**, nunca de o
build ter terminado com sucesso.

---

## 2. Reuso entre classes de dispositivo

A afirmação central da tese é que a arquitetura em camadas permite construir classes distintas de
dispositivo médico reaproveitando a plataforma. `med-image-tomograph` existe como caso de controle
dessa afirmação.

### Método

```bash
make qemu && make tomograph
BH=build/buildhistory/images/qemux86_64/glibc
diff <(cut -d' ' -f1 $BH/med-image-eeg/installed-package-names.txt) \
     <(cut -d' ' -f1 $BH/med-image-tomograph/installed-package-names.txt)
grep IMAGESIZE $BH/med-image-*/image-info.txt
```

### Resultado

| | Pacotes instalados | `IMAGESIZE` |
|---|---|---|
| `med-image-eeg` | 209 | 275.540 KB |
| `med-image-tomograph` | 206 | 275.424 KB |
| **Diferença** | **3** | **116 KB** |

O `diff` produz apenas remoções: o conjunto do tomógrafo é **subconjunto estrito** do conjunto do
EEG. Os três pacotes de diferença são `eeg-acquisition-service`, `eeg-hmi-gui` e
`packagegroup-med-amp`.

### O que isso significa — e o que não significa

**Não** apresente como "os dois perfis compartilham 98,6%". O tomógrafo **não instala nenhuma
aplicação específica de tomografia**, porque essa aplicação não existe. O delta dele é zero *por
construção*, não por mérito da arquitetura, e um percentual de similaridade sozinho seria enganoso.

A leitura defensável são dois números independentes:

1. **A plataforma que ambas as classes herdam: 206 pacotes.**
2. **O delta específico de dispositivo: EEG = 3 pacotes / 112 KB; tomógrafo = 0.**

E o resultado mais forte não é percentual nenhum:

> O tomógrafo carrega `libmedframework1`, `packagegroup-med-core`, `packagegroup-med-framework` e
> `packagegroup-med-gui` **com zero aplicações instaladas**.

Isto é a afirmação da camada demonstrada em vez de argumentada: a plataforma se sustenta completa —
runtime do MedFramework, pilha gráfica, stack de atualização, volume de dados — sem uma única linha
de lógica de dispositivo. Uma nova classe de dispositivo parte de 206 pacotes funcionando e
acrescenta apenas a própria aplicação.

E isso deixou de ser inferência sobre listas de pacotes: **`python3 scripts/med-check.py tomograph`
boota a imagem do tomógrafo e passa 11/11** nas asserções de plataforma — journal persistente e selado, slots A/B
resolvidos, as quatro partições GPT, e o volume `/data` provisionado, montado e criptografado. Um
sistema completo, em execução, com zero aplicações instaladas.

Uma asserção precisou mudar de lista para isso, e a mudança é informativa. `rauc.service` é
`Type=dbus`: no tomógrafo ele fica inativo porque nenhuma aplicação pede nada a ele, o que é
correto. A plataforma responde por `rauc-available` (existe e não falhou); o perfil EEG responde
por `rauc-active`, e numa unit ativada por D-Bus estar *ativa* significa que **alguém falou com
ela** — evidência de que o `MedicalUpdate` alcançou o daemon, não apenas de que o daemon existe.

**Sobre os 116 KB**: eles são pequenos porque o Qt já está nas duas imagens (`packagegroup-med-gui`
está em ambas), então a HMI é apenas o binário linkando bibliotecas compartilhadas. O número mede o
**custo marginal de uma aplicação sobre uma plataforma que já tem o toolkit** — não "uma HMI Qt cabe
em 116 KB".

### Nível de metadata

Complementa o número de pacotes e é inspecionável em dez segundos por quem estiver avaliando:

| Recipe | Linhas significativas |
|---|---|
| `med-image-base.bb` | 51 |
| `med-image-eeg.bb` | 17 |
| `med-image-tomograph.bb` | 16 |

O conteúdo comportamental de cada perfil de dispositivo é: um `require` da imagem base, um `require`
do perfil de desenvolvimento, e uma lista de instalação.

### Uma condição que precisou ser criada para a medição valer

O número acima só é honesto porque os dois perfis diferem **exclusivamente** por classe de
dispositivo. Antes do commit `f0e4427` eles não diferiam: a configuração de boot QEMU, o disco A/B e
o conjunto de ferramentas de verificação estavam apenas no perfil do EEG, e o `diff` teria contado
essa deriva acidental como se fosse diferença de classe de dispositivo. O caso de controle estava
contaminado. Ver `med-image-dev.inc`.

**E uma correção**: os números publicados antes (275.500 / 275.388, diferença de 112 KB) vinham de
builds de **commits diferentes** — o tomógrafo não havia sido reconstruído desde antes do trabalho
de LUKS, então não carregava os ~36 KB do passo de provisionamento que o EEG já carregava. A
diferença de 4 KB é pequena; o método não é, e num documento cuja disciplina é essa a comparação
cruzada é o defeito, não o valor. Os números acima são do mesmo commit.

---

## 3. Caminho de aquisição

### Método

Sessão de aquisição com o driver `simulated`, e conferência do arquivo de registro contra o formato
de quadro declarado em `MedicalDevice.h`.

### Resultado

`raw.bin` = **2.709.840 bytes** após uma sessão de 5 min 22 s.

```
40 (FrameHeader) + 8 canais × 25 amostras × 4 bytes = 840 bytes/quadro
2.709.840 ÷ 840 = 3.226 quadros, resto ZERO
3.226 quadros ÷ 10 Hz (25 amostras a 250 Hz = 100 ms) = 322,6 s
```

E 322,6 s é exatamente o intervalo entre o início da sessão e o `mtime` do arquivo.

**O que isso demonstra**: a taxa de amostragem configurada é a taxa real; o formato em disco bate
com o `static_assert(sizeof(FrameHeader) == 40)` do header; e **resto zero** significa que não houve
escrita parcial nem quadro truncado.

**Custo**: 19,353 s de CPU em ~322 s de aquisição ≈ **6% de um núcleo** para 8 canais a 250 Hz.

**Limite**: sob QEMU, sem garantia de tempo real. Este número é de *throughput e correção de
formato*, **não** de latência. Latência só tem significado no STM32MP257.

---

## 4. Particionamento de software (IEC 62304 §5.3)

### Método

```bash
systemd-analyze security eeg-acquisition.service
chrt -p $(pidof eeg-acquisition-service)
```

### Resultado

**Exposure level: 3.7** (faixa `OK` do systemd; as faixas são `SAFE` < 1.0, `OK` até 5.0, depois
`MEDIUM`/`EXPOSED`/`UNSAFE`). Escalonamento: **`SCHED_RR`, prioridade 50** — o
`CPUSchedulingPolicy` da unit é concedido pelo kernel, não apenas declarado.

**O que isso transforma**: o particionamento deixa de ser afirmação em prosa e vira número medido.

**Lacuna conhecida, ainda não corrigida**: a unit não define `CapabilityBoundingSet=`. Somando as
linhas de capability do relatório dá ~3.0 dos ~5.5 pontos brutos — é o maior item isolado por
margem larga. Também faltam `UMask=0077`, `ProtectProc=invisible`, `ProcSubset=pid` e
`IPAddressDeny=any`. Corrigir e remedir daria um antes/depois; **não foi feito**.

Itens abertos **por projeto**, não por descuido, e que devem ser descritos assim:
`RestrictAddressFamilies=~AF_UNIX` e `RestrictRealtime=` (o serviço precisa dos dois);
`PrivateDevices=` (incompatível com o acesso a `/dev/rpmsg0` no alvo AMP).

---

## 5. Trilha de auditoria

### Método

```bash
ls -ld /var/log/journal && journalctl --verify
reboot && journalctl --list-boots
```

### Resultado

`/var/log/journal` é diretório real com setgid `systemd-journal`; `journalctl --verify` retorna
`PASS` (o *Forward Secure Sealing* valida); e `--list-boots` lista o boot anterior depois de um
reboot.

**O que isso demonstra**: a persistência e o selo do journal funcionam de ponta a ponta. Isto é
teste de regressão de um defeito real — o `VOLATILE_LOG_DIR` default do OE transformava `/var/log`
em symlink para um tmpfs, descartando a trilha a cada boot e anulando em silêncio o
`Storage=persistent`/`Seal=yes` do `10-journald-audit.conf`.

---

## 6. Caminho de atualização A/B

### Método

```bash
make bundle && make verify-bundle          # host
make bundle-disk && make runqemu           # guest: rauc status; rauc install
```

### Resultado

**No host** — a falha intermediária é parte do resultado. Com as opções default do `rauc info`, o
bundle é **recusado**:

```
signature verification failed: Verify error: unsuitable certificate purpose
```

Com a configuração do dispositivo (`check-purpose=codesign`, `check-crl=true`):

```
Verified inline signature by 'CN = MedPlatform Bundle Signing'
Compatible:     'med-os-qemux86-64'
Bundle Format:  verity
```

**No dispositivo**:

```
Booted from: rootfs.0 (/dev/vda2)
o [rootfs.1] (/dev/disk/by-partlabel/med-root-b, ext4, inactive)  bootname: B
o [rootfs.0] (/dev/disk/by-partlabel/med-root-a, ext4, booted)    bootname: A

Installing `/mnt/b/update.raucb` succeeded
```

Com `device-mapper: verity` no log do kernel: o bundle foi verificado **no dispositivo**, contra o
keyring do dispositivo, e escrito no slot inativo. Slots simétricos, 2.097.152 setores cada.

**Limite, e ele é importante**: a *política* A/B está validada (atomicidade, escrita no slot
inativo, verificação criptográfica). A *integração com bootloader* **não está** — o QEMU roda com
`MED_BOOTLOADER = "noop"`. Além disso, `boot-attempts` é rejeitado pelo RAUC para qualquer backend
que não seja `uboot`/`barebox`, então o mecanismo de fallback do QEMU seria diferente do que o
STM32MP257 usa. Ver `implementation_plan_rauc.md` §3.

---

## 7. Volume `/data` criptografado

### Método

`make check` (asserções `data-provisioned`, `data-mounted`, `data-encrypted`,
`storage-requires-encryption`, `no-encryption-waiver`), mais inspeção do artefato e uma injeção de
falha deliberada.

### Resultado

`21/21` no primeiro boot (provisionamento) e no segundo (idempotência: o volume é aberto, não
reformatado). O mesmo volume é provisionado e verificado no perfil tomógrafo (`11/11`), sem
nenhuma aplicação instalada. No `.wic`, o offset da partição `med-data` passou a conter o magic LUKS
`4c554b53babe`.

O teste de ponta a ponta é o do `implementation_plan_luks.md` §2: com
`MED_EEG_REQUIRE_ENCRYPTION = "true"`, o serviço de aquisição **só sobe** porque o
`MedicalStorage` resolveu o dispositivo por trás de `/data` e encontrou um `dm/uuid` começando em
`CRYPT-`. O registro de auditoria de dispensa de criptografia desapareceu do journal.

### Injeção de falha — o resultado mais informativo

Zerado 1 MB no início da partição, simulando um cabeçalho LUKS corrompido:

```
refusing to provision: /dev/disk/by-partlabel/med-data is not pristine (type='' label='',
expected 'ext4'/'med-data'). This is also what a damaged LUKS header looks like, so refusing
to format over what may be patient data. This needs deliberate intervention.
```

**Não reformatou.** E a cascata é a correta: o serviço registrou `refusing to store medical records
on the unencrypted backing of /data` e saiu; `/data` ficou **vazio**, zero registros. O dispositivo
degradou para *não gravar*, não para *gravar em claro*.

**Limite** (corrigido em 2026-08-17; a redação anterior estava invertida): a fonte de chave medida
é `development` — LUKS2 real, com chave aleatória de 256 bits única por dispositivo. Mas a chave
mora numa partição **do mesmo disco** onde está o volume cifrado (a ESP aqui, a `bootfs` no
STM32MP257). Quem leva a mídia leva as duas coisas.

Este arquivo afirmava "protege contra remoção física da mídia, não contra root no dispositivo
ligado", e isso está trocado. O que a chave de desenvolvimento dá é:

* contra **exfiltração parcial** — cópia apenas da partição `med-data`, ou um backup de `/data` — o
  texto cifrado sozinho é inútil;
* contra **root no dispositivo ligado**, alguma coisa, via a recusa do `MedicalStorage` em gravar em
  backing não criptografado (§7, injeção de falha);
* contra **remoção da mídia**, quase nada.

O que está validado aqui é o **mecanismo** — que a plataforma cifra de verdade, que o provisionamento
é idempotente e tem salvaguarda, que a aplicação se recusa a gravar em claro. Não é um controle de
segurança enquanto a custódia não sair da mídia.

**E `tpm2` não é alcançável nas camadas atuais**, o que muda o planejamento e não só o cronograma. O
TPM não criptografa nada — quem cifra é o `dm-crypt` com AES-XTS; o TPM decide onde a chave mora e
sob que condições é liberada. Verificado: `meta-security/meta-tpm` oferece `swtpm` e `ibmswtpm2`, que
são **emuladores em software** guardando estado no mesmo sistema de arquivos (trocariam um arquivo de
chave por outro, sem raiz de confiança), e **não existe receita de fTPM** — TPM como Trusted
Application do OP-TEE — nem no `meta-security` nem no `meta-st-stm32mp`. As saídas reais são um chip
TPM discreto no SPI/I2C da placa, ou portar um fTPM para o OP-TEE que já está no FIP. Ver
`docs/BRINGUP_STM32MP2.md` §7 e `implementation_plan_luks.md` §9.

**Achado que vale para além do TPM**: a chave tem de sobreviver a uma troca de slot A/B. Uma chave
no rootfs seria destruída pela primeira atualização **bem-sucedida**, deixando o dispositivo sem
decifrar os próprios prontuários. Isso já era um risco conhecido do caminho TPM/PCR
(`implementation_plan_luks.md` §7.3) e vale para qualquer fonte de chave.

---

## 8. Portabilidade para o alvo físico (STM32MP257)

`make stm32` constrói **o mesmo** `med-image-eeg` para `MACHINE=stm32mp25-disco` (Cortex-A35,
aarch64). É o teste da afirmação central da §2 vista pelo outro eixo: a §2 mede reuso entre classes
de dispositivo na mesma máquina; esta mede reuso da mesma classe entre máquinas. Se alguma camada
acima do BSP soubesse em que placa está, é aqui que apareceria.

### Método

```bash
make stm32
W=build/tmp-glibc/deploy/images/stm32mp25-disco/med-image-eeg-stm32mp25-disco.rootfs.wic
sfdisk -l $W                                          # tabela de partições
dd if=$W bs=512 skip=34    count=1 | head -c8 | xxd   # magic do TF-A, em 17 KiB
dd if=$W bs=512 skip=2082  count=1 | head -c8 | xxd   # magic do FIP
dd if=$W bs=512 skip=19490 count=131072 > bootfs.ext4
debugfs -R "ls -l /" bootfs.ext4                      # o que o U-Boot vai encontrar
# nomes e PARTUUID lidos direto das entradas do GPT (as do sfdisk não trazem o nome)
BH=build/buildhistory/images
diff <(grep -v ^kernel-module- $BH/qemux86_64/glibc/med-image-eeg/installed-package-names.txt) \
     <(grep -v ^kernel-module- $BH/stm32mp25_disco/glibc/med-image-eeg/installed-package-names.txt)
grep -rnE ':(qemux86-64|stm32mp[0-9]+|stm32mp25-disco)\b' meta-custom/meta-med-{distro,framework,app}
```

Nenhum número abaixo vem de "o build terminou". Todos vêm de ler o `.wic` produzido ou o
buildhistory — pela regra da §1.

### Resultado — o metadado atravessa a fronteira

`5364` tasks, todas com sucesso. `TARGET_SYS = aarch64-med-linux`,
`TUNE_FEATURES = "aarch64 crc cortexa35"`. Artefato: `med-image-eeg-stm32mp25-disco.rootfs.wic`,
`2.761.966.592` bytes.

**Nenhum arquivo** de `meta-med-app`, `meta-med-framework` ou `meta-med-distro` difere entre os dois
alvos, e o `grep` de overrides de máquina nas três camadas retorna **zero ocorrências**.

Contado com precisão, o que difere entre construir para QEMU e para o STM32MP257 é:

* **quatro variáveis** com valores distintos — `MED_EEG_DRIVER` (`simulated`/`rpmsg`),
  `MED_BOOTLOADER` (`noop`/`uboot`), `MED_DATA_KEY_SOURCE` (`development`/`tpm2`) e `MED_WKS_FILE`;
* **um arquivo de conteúdo de camada**: o `.wks` que a quarta variável seleciona, em `meta-med-bsp`;
* mais o repositório do BSP no arquivo kas do alvo (`meta-st-stm32mp`), que é a definição de um BSP.

A tabela em `PROJECT_CONTEXT.md` §4.1 lista seis variáveis de adaptação, não quatro: as outras duas
não entram nesta contagem porque não diferem — `MED_EEG_REQUIRE_ENCRYPTION` vale `"true"` nos dois
alvos e `MED_AMP_FIRMWARE` está sem valor em ambos (é um gancho declarado, não um valor). É esse
número — quatro variáveis e um arquivo — que a tese pode afirmar, não "portabilidade" no abstrato.

### Resultado — composição da imagem

Excluindo `kernel-module-*`, pela ressalva logo abaixo:

| | Pacotes | Exclusivos deste alvo |
|---|---|---|
| Comuns aos dois alvos | **201** | — |
| `qemux86-64` | 207 | 6 |
| `stm32mp25-disco` | 216 | 15 |

Os 15 exclusivos do STM32MP257 rastreiam, um por um, para o kernel ou para uma variável de adaptação
declarada — nenhum é vazamento de plataforma:

| Pacotes | Origem |
|---|---|
| `kernel-6.6.129`, `kernel-image-image.gz-*`, `kernel-devicetree`, `kernel-modules` | BSP (`MACHINE_ESSENTIAL_EXTRA_RDEPENDS` da ST) |
| `stm32mp-extlinux`, `u-boot-stm32mp-splash` | BSP: caminho de boot da placa |
| `libubootenv0`, `libubootenv-bin`, `u-boot-fw-config-stm32mp`, `libyaml-0-2` | `MED_BOOTLOADER = "uboot"`, via RAUC |
| `tpm2-tss`, `libtss2`, `libtss2-mu0`, `libtss2-tcti-device0` | `MED_DATA_KEY_SOURCE = "tpm2"`, via `PACKAGECONFIG` do systemd |
| `rpmsg-tools` | BSP (`st-machine-common-stm32mp.inc:583`), **não** o `packagegroup-med-amp` |

Os 6 exclusivos do QEMU são simétricos: `kernel-6.6.144-yocto-standard` e dois pacotes de imagem de
kernel, mais `libdrm-intel1`, `libpciaccess0` e `v86d` — gráficos x86 do BSP do QEMU.

**Ressalva que impede a comparação ingênua**: contando tudo, são `209` pacotes no QEMU contra
`1270` no STM32MP257, e `IMAGESIZE` vai de `275.544 KB` para `338.572 KB` (+22,9%). Isso **não** é
a plataforma crescendo: são `1054` pacotes `kernel-module-*` do kernel da ST contra `2` do
`linux-yocto`. Não relate "a imagem cresceu 23% ao portar" sem dizer que o crescimento é o BSP
empacotando módulos.

### Resultado — o disco, por inspeção

| # | Partição | Setores | Tamanho | Conteúdo verificado |
|---|---|---|---|---|
| 1–2 | `fsbla1`, `fsbla2` | 512 | 256K | magic `STM2` (`5354 4d32`) no setor 34 = 17 KiB, onde o ROM code procura o FSBL |
| 3–4 | `metadata1`, `metadata2` | 512 | 256K | metadata de firmware-update do TF-A |
| 5–6 | `fip-a`, `fip-b` | 8192 | 4M | magic do FIP `0xaa640001` no setor 2082 |
| 7 | `u-boot-env` | 1024 | 512K | vazia, por projeto |
| 8 | `bootfs` | 131072 | 64M | `Image.gz` (11.219.612 B), `extlinux/extlinux.conf`, 3 devicetrees |
| 9 | `med-root-a` | **2097152** | 1G | `ext4`, label `med-root-a` |
| 10 | `med-root-b` | **2097152** | 1G | `ext4`, label `med-root-b`, vazia |
| 11 | `med-data` | 1048576 | 512M | `ext4`, label `med-data` |

Três verificações são o motivo de a inspeção existir:

* **Slots simétricos**: `2097152` setores exatos cada, não "1G" arredondado. É a propriedade que a
  §6 exige e que já saiu errada uma vez, quando `--size` virou `1,33 GB` num slot e `1,0 GB` no
  outro sem falhar build nenhum.
* **`PARTUUID` de `med-root-a` = `e91c4e10-16e6-4c0e-bd0e-77becf4a3582`**, igual ao
  `root=PARTUUID=` do `extlinux.conf` que o BSP gera a partir de `DEVICE_PARTUUID_ROOTFS:mmc0`.
  Qualquer outro valor produz um kernel em panic sem raiz — e nada no build reportaria.
* **`med-data` em `ext4` com label `med-data`**: exatamente o par que o safeguard da §7 exige para
  distinguir "nunca provisionado" de "cabeçalho LUKS corrompido".

### Resultado — o fragmento de kernel não chegava a ser aplicado (corrigido)

Este resultado mudou de diagnóstico depois do primeiro boot, e a diferença entre os dois
diagnósticos importa mais que a tabela.

**O que se media antes.** Conferido no `.config` que a receita da ST construiu:

| símbolo | fragmento pede | `.config` construído |
|---|---|---|
| `CONFIG_BLK_DEV_DM`, `DM_CRYPT`, `CRYPTO_XTS` | `y` | **`m`** |
| `CONFIG_RPMSG_CHAR`, `RPMSG_CTRL` | `y` | **`m`** |
| `CONFIG_TCG_TIS_CORE`, `TCG_TIS` | `y` | **`m`** |
| `CONFIG_CRYPTO_AES`, `SHA256`, `KEYS`, `REMOTEPROC`, `TCG_TPM` | `y` | `y` |

A leitura era "o fragmento chega mas não vence", e ela estava errada — cortês demais com o defeito.
Confirmado no kernel em execução na placa (`zcat /proc/config.gz`, 2026-08-18), e depois investigado
até a causa.

**O que era.** O `linux-%.bbappend` só acrescentava o fragmento à `SRC_URI`. Isso basta para o
`linux-yocto`, que herda `kernel-yocto` e varre a `SRC_URI` atrás de `.cfg` por conta própria — e é
por isso que o mecanismo funcionava no QEMU e *parecia* portátil. O `linux-stm32mp` herda `kernel`
puro: seu `do_configure` funde exatamente os arquivos listados em `KERNEL_CONFIG_FRAGMENTS` com o
`merge_config.sh` e ignora todo o resto da `SRC_URI`.

Medido, não deduzido:

```bash
kas shell kas/project-eeg-stm32mp2.yml -c "bitbake -e virtual/kernel | grep ^KERNEL_CONFIG_FRAGMENTS="
# lista os quatro fragmentos da ST e não o nosso
kas shell kas/project-eeg-stm32mp2.yml -c "bitbake -e virtual/kernel | grep -c med-kernel-features"
# não-zero: está na SRC_URI
```

Ou seja: no alvo físico, **a política de kernel do MedOS era descompactada e nunca aplicada**. Os
quatro símbolos que saíam `y` saíam `y` porque o defconfig da ST já os tinha — por coincidência, que
é a pior forma de um requisito de segurança ser satisfeito. Foi assim que o `/data` LUKS funcionou
na placa: `DM_CRYPT=m` é escolha da ST, não nossa.

**A correção** é uma linha no mesmo bbappend curinga — acrescentar também a
`KERNEL_CONFIG_FRAGMENTS`, que o `linux-yocto` ignora, de modo que os dois mecanismos passam a ser
cobertos sem o bbappend saber qual kernel está em uso.

**Resultado depois** (`bitbake -c configure -f virtual/kernel`, lendo o `${B}/.config` produzido):

| símbolo | pede | saiu |
|---|---|---|
| `TCG_TPM`, `TCG_TIS_CORE`, `TCG_TIS` | `y` | `y` ✓ |
| `BLK_DEV_DM`, `DM_CRYPT`, `CRYPTO_XTS` | `y` | `y` ✓ |
| `RPMSG_CHAR`, `RPMSG_CTRL` | `y` | `y` ✓ |
| `OVERLAY_FS` | `y` | `y` ✓ |

A coluna inteira de `m` desapareceu. O log do `merge_config.sh` confirma que o nosso fragmento é o
**último** da lista — que é o que o faz vencer — e que as únicas mensagens sobre ele são
`redundant`, isto é, nenhum símbolo nosso conflitava em silêncio com os da ST.

Isto **não** foi validado em hardware: o kernel novo não foi construído nem bootado. O que está
medido é o `.config` produzido.

### Resultado — execução na placa (primeiro boot, 2026-08-18)

Até aqui a §8 media o artefato construído e o disco por inspeção. Esta subseção é a primeira em que
o alvo físico **executou**. Método: `make stm32 KEY=development`, cartão SD gravado com o `.wic`,
console serial pelo ST-LINK V3. O registro narrativo, com os defeitos e suas causas, está em
`BRINGUP_STM32MP2.md` §9; aqui ficam as medidas.

| # | O que se mediu | Comando na placa | Resultado |
|---|---|---|---|
| 1 | Boot completo | console serial | multi-user alcançado; sistema de pé > 8 min |
| 2 | Slot do qual bootou | `rauc status` | `Booted from: rootfs.0 (/dev/mmcblk0p9)` |
| 3 | O PARTUUID é o que o `.wks` fixa | `cat /proc/cmdline` | `root=PARTUUID=e91c4e10-16e6-4c0e-bd0e-77becf4a3582` |
| 4 | Provisionamento do `/data` | `systemctl status med-data-provision` | `active (exited)`, `status=0`, **13 s** (16,8 s CPU) |
| 5 | `/data` é LUKS2 real | `cryptsetup status med-data` | LUKS2, `aes-xts-plain64`, 512 bits, sobre `/dev/mmcblk0p11` |
| 6 | A asserção do `MedicalStorage` | `cat /sys/block/dm-0/dm/uuid` | `CRYPT-LUKS2-c1480c70…-med-data` |
| 7 | `/data` montado do mapeamento | `awk '$2=="/data"' /proc/mounts` | `/dev/mapper/med-data ext4` |
| 8 | Custódia da chave | `stat -c '%U:%G %a'` em `med-boot` | `root:root 400` |
| 9 | Trilha de auditoria | `journalctl --verify` | `PASS` |
| 10 | Unidades falhas | `systemctl list-units --state=failed` | apenas `weston.service` |
| 11 | Fragmento de kernel | `zcat /proc/config.gz \| grep TCG_TIS` | `CONFIG_TCG_TIS=m` (o fragmento pede `y`) |

**As linhas 4 a 8 são o resultado mais importante deste documento desde a §7.** Elas transportam o
volume `/data` criptografado do QEMU para hardware real: mesmo `med-data-provision.service`, mesmo
`MedicalStorage`, mesma exigência `MED_EEG_REQUIRE_ENCRYPTION = "true"` — e o serviço de aquisição
subiu, o que sob essa exigência *é* a afirmação de que a criptografia é real.

Duas delas merecem leitura cuidadosa:

- **A linha 8 mede uma decisão de arquitetura, não um detalhe.** O `400` só é possível porque a
  chave está em ext4; na ESP em vfat de `qemux86-64` o `chmod` do script é silenciosamente
  ignorado. A separação `MED_KEY_STORE_DEV`/`MED_KEY_STORE_FSTYPE` por máquina existia por esse
  argumento, escrito como raciocínio. Agora é uma permissão lida do sistema de arquivos.
- **A linha 4 explica um atraso que parece defeito e não é.** O `data.mount` esperou 13 s pelo
  `/dev/mapper/med-data`; o custo é o PBKDF do LUKS2 num Cortex-A35. Vale como ordem de grandeza
  para qualquer orçamento de tempo de boot.

**Dimensionamento**: `size: 1015808` setores × 512 B = **496 MiB** de `/data` — a partição de
512 MiB do `.wks` menos 16 MiB de cabeçalho LUKS2 — num cartão de 14,8 GiB. O restante está
inacessível porque o GPT de reserva ficou no fim da *imagem* e não do cartão
(`GPT:5394465 != 31116287`), o que o kernel reporta a cada boot.

#### O que o RAUC faz e o que não faz, medido

```
Compatible:  med-os-stm32mp25-disco
Booted from: rootfs.0 (/dev/mmcblk0p9)
Activated: none
o [rootfs.1] (med-root-b, ext4, inactive)  bootname: B  boot status: bad
o [rootfs.0] (med-root-a, ext4, booted)    bootname: A  boot status: bad

rauc-WARNING: Failed getting primary slot: uboot backend: fw_printenv failed with exit code: 1
ls: /etc/fw_env.config: No such file or directory
```

Funciona: os dois slots resolvem por rótulo GPT, o slot bootado é identificado, e o `Compatible`
casa com o dos bundles. Não funciona: **o item 4 da lista abaixo deixou de ser "achado por inspeção
do manifest" e passou a ser observado em execução**, com a mensagem de erro. E a consequência é
maior que o aviso: sem ler o ambiente do U-Boot o RAUC reporta **ambos os slots como `bad`,
inclusive o que está rodando**, e `Activated: none` — um `rauc install` escreveria o slot inativo e
nunca conseguiria ativá-lo.

#### Um defeito novo que nenhuma inspeção de artefato encontraria

`stm32_rtc: Date/Time must be initialized` e `System time before build time, advancing clock`: a
placa não tem relógio inicializado, e toda a sessão correu datada de **2025-05-29** para uma imagem
construída em 2026-08. Sem RTC com bateria e sem sincronização de rede antes de `/data` montar,
**todo registro escrito pelo `MedicalLogger` e pelo `MedicalStorage` carrega carimbo de tempo
errado**. Para o argumento de rastreabilidade da §5 isso é material: um registro de paciente com
data errada é pior que um registro ausente. Nada no repositório trata disso, e a §9 passa a listá-lo.

#### O que ficou aberto neste boot

- **`weston.service` falha** (`status=1`, 73 ms) e derruba a HMI por dependência. Não é o caso
  previsto do QEMU: aqui o DRM da placa subiu e o `galcore` carregou. Causa não determinada.
- **O serviço de aquisição pode estar reiniciando**: `Started …` aparece duas vezes no log de boot.
  `NRestarts` não foi consultado. Pela ressalva da §10, este é exatamente o sintoma que
  `systemctl is-active` esconde — a suíte tem a asserção certa, ela só não foi executada aqui.
- **`u-boot-env`, `fip-a/b`, `metadata1/2` colidem por nome com o eMMC de fábrica** e não se
  determinou qual disco venceu (saída truncada no terminal). É disso que depende a forma do
  `/etc/fw_env.config` a ser escrito.

### O que isso **não** significa

1. ~~**Não significa que o dispositivo boota.**~~ **Superado em 2026-08-18**: boota, e a subseção
   anterior mede o quê. Mantido riscado em vez de apagado porque a afirmação original estava certa
   quando foi escrita, e o que a derrubou não foi um argumento melhor — foi energizar a placa.
2. **Não significa A/B funcional no hardware.** `bootfs` é compartilhada pelos dois slots e o
   `extlinux.conf` fixa o PARTUUID do slot A: um dispositivo que o RAUC marcou "boot B" ainda boota
   A. Falta um script de U-Boot lendo `BOOT_ORDER`/`BOOT_<slot>_LEFT`; a partição `u-boot-env`
   existe no disco para isso. A frase da §6 continua valendo sem alteração — *a política A/B é
   validada no QEMU, a integração com o bootloader não foi validada em lugar nenhum*.
3. **Continua não significando `/data` criptografado no alvo *com a configuração de produto*.** O
   que foi executado é `make stm32 KEY=development`: a chave vive em `med-boot`, fora dos dois slots
   A/B e em ext4, e quem consegue ler o cartão consegue ler a chave. A configuração que o perfil
   declara, `MED_DATA_KEY_SOURCE ?= "tpm2"`, **recusa provisionar** em vez de existir pela metade, e
   com `MED_EEG_REQUIRE_ENCRYPTION = "true"` o serviço de aquisição não subiria. Essa configuração
   nunca foi executada em hardware, e não pode ser: não há TPM alcançável (§7 do
   `BRINGUP_STM32MP2.md`). O que a subseção anterior mede é o **mecanismo** — LUKS2, provisionamento
   de primeiro boot, a asserção do `MedicalStorage` — não a custódia de produto.
4. **Não significa que o RAUC consegue marcar um slot.** O backend `uboot` chama `fw_setenv`, que lê
   `/etc/fw_env.config`; `u-boot-fw-config-stm32mp` instala `fw_env.config.mmc`, `.nand` e `.nor` e
   **nunca** esse nome. Previsto por inspeção do manifest e **confirmado em execução em 2026-08-18**
   (`fw_printenv failed with exit code: 1`, ambos os slots reportados `bad`, `Activated: none`).
5. **Os `.tsv` de flashlayout gerados não descrevem este disco.** O BSP os produz a partir do layout
   da ST (`rootfs`/`vendorfs`/`userfs`, um único rootfs, sem `med-root-*` e sem `med-data`).
   Gravar por eles produz um disco que o MedOS não usa: grave o `.wic`.

### O sétimo defeito da família da §1

O caminho até este resultado acrescentou um defeito ao conjunto de seis da §1, da mesma espécie e
com a mesma causa: **uma suposição documentada que nenhum build verifica**. O
`med-partitions.wks` afirmava, no próprio cabeçalho, que BSPs com TF-A em offsets fixos "trazem seu
próprio `WKS_FILE` a partir da configuração de máquina". O `meta-st-stm32mp` não traz — define
`WKS_FILE_DEPENDS` e deixa `#WKS_FILE += "${OPTEE_WIC_FILE}"` comentado. O default da distro venceu
por ausência de adversário e o `do_image_wic` tentou montar uma ESP EFI/GRUB numa placa sem ESP,
falhando em `install: cannot stat '.../Image.gz'` — a ST publica o kernel em
`${DEPLOY_DIR_IMAGE}/kernel/`, então o nome procurado não existe em lugar nenhum. **O sintoma
nomeava o kernel; o defeito era o layout inteiro.**

A correção estrutural foi criar `meta-med-bsp` (prioridade 7) e mover para lá os dois `.wks` e os
`QB_*`, porque a auditoria que seguiu mostrou que a fronteira já havia sido cruzada duas vezes sem
falhar build nenhum: `med-partitions.wks` fixava `loader=grub-efi` atrás de um nome de arquivo
genérico, e `med-image-base.bb` carregava `QB_KERNEL_ROOT = "/dev/vda2"` duas linhas abaixo de um
comentário afirmando ser agnóstico a dispositivo. **Nomes genéricos sobre conteúdo específico de
máquina** são o mecanismo comum aos dois casos, e é o que a regra 1 do `CLAUDE.md` passou a nomear.

Revalidação após a mudança: `make qemu` verde, `make check` `21/21`, `make stm32` verde, GPT do
QEMU idêntico ao de antes do rename, `bitbake -p` sem warnings, e `MED_WKS_FILE` resolvendo por
máquina a partir da camada nova. A precedência da indireção `_DEFAULT` foi verificada por injeção de
falha, não por raciocínio: `MED_WKS_FILE = ""` num fragmento kas **sobrepõe** a camada e dispara o
guard de build — o que um `MED_WKS_FILE:<machine> =` direto não permitiria, pois venceria
silenciosamente o arquivo de projeto.

---

## 9. O que **não** foi medido

Registrado explicitamente para que a ausência não seja lida como resultado:

- **Caminho AMP / `rpmsg` / Cortex-M33** — o QEMU não tem co-processador. O driver medido é o
  `simulated`. O alvo que tem co-processador foi energizado em 2026-08-18 e o kernel reporta
  `remoteproc remoteproc1: m33 is available`, mas nenhum firmware foi carregado nele: o driver
  `rpmsg` continua não exercitado em alvo nenhum. O boot acrescentou uma restrição ao plano —
  `stm32-rproc 0.m33: Support of signed firmware only`.
- **Latência e jitter de tempo real** — o timing do QEMU não é significativo.
- **Integração com bootloader e fallback A/B em boot falho** — §6. A §8 não muda isso: no
  STM32MP257 a seleção de slot **não está implementada** (`bootfs` compartilhada, `extlinux.conf`
  fixando o slot A), e no QEMU o mecanismo é outro.
- **A imagem de produto no STM32MP257** — o que executou em 2026-08-18 foi
  `make stm32 KEY=development`. Não foi executado: o perfil como o arquivo de projeto o declara
  (`MED_DATA_KEY_SOURCE = "tpm2"`, que recusa provisionar), nem instalação de bundle na placa (o
  RAUC não consegue ativar slot), nem seleção de slot A/B (o `extlinux.conf` fixa o slot A), nem a
  HMI (o `weston` falha).
- **Estabilidade do serviço de aquisição na placa** — subiu, mas o log de boot traz dois
  `Started …` e o `NRestarts` não foi consultado. Não se pode afirmar que roda sem reiniciar.
- **Carimbo de tempo confiável no alvo físico** — a placa correu sem RTC inicializado, com data de
  2025-05-29. Nenhuma medida deste documento tomada na placa depende de tempo absoluto, mas
  qualquer registro que o dispositivo escreva depende, e isso não está tratado.
- **TPM, secure boot, OP-TEE** — nenhum exercitado. O OP-TEE está no FIP do disco da §8, mas nunca
  executou. E o TPM não é apenas "não medido": não há implementação alcançável nas camadas atuais —
  só emuladores em software, sem fTPM para o OP-TEE — de modo que a custódia de chave permanece de
  desenvolvimento **nos dois alvos**, e não apenas no de simulação (§7).
- **A HMI Qt em execução** — exige `NATIVE=1` com runqemu gráfico; sob `nographic` o
  `weston.service` falha por projeto.
- **Perfil `med-image-prod`** — nunca construído. Rootfs read-only não foi exercitado.
- **Perfil tomógrafo no STM32MP257** — nunca construído; a §2 é inteira sobre `qemux86-64`.

---

## 10. Reprodutibilidade

Todos os números acima saem de:

```bash
make pki          # uma vez: gera a CA de desenvolvimento
make qemu
make tomograph
make bundle && make verify-bundle
make bundle-disk
make check            # 21 asserções: plataforma + perfil EEG
python3 scripts/med-check.py tomograph   # 11 asserções: só as de plataforma
make stm32                      # §8: o alvo físico, configuração de produto
make stm32 KEY=development      # §8: a configuração que foi executada na placa
```

As medidas de execução na placa (§8, "Resultado — execução na placa") não são reproduzidas por
`make`: exigem gravar o `.wic` num cartão, alimentar a STM32MP257F-DK e um console serial. Cada uma
traz o comando que a produziu na própria tabela. **A suíte `med-check.py` não alcança a placa** —
ela conversa por pty com o `runqemu` — de modo que as asserções ali foram executadas à mão, que é
precisamente a condição que a suíte existe para eliminar. Automatizá-las contra um alvo serial é
trabalho pendente, e até lá o alvo físico está no regime "verificado uma vez", não "verificável".

**A verificação de runtime é automatizada.** `make check` boota a imagem, executa 21 asserções e
sai com código não-zero se alguma falhar — o que transforma "verificado uma vez" em "verificável",
que é o que a IEC 62304 pede de uma atividade de verificação.

Uma ressalva sobre a suíte, aprendida ao exercitá-la: a asserção `acq-active` usava
`systemctl is-active`, e um serviço `Type=simple` com `Restart=on-failure` passa por uma janela
breve de `active` a cada tentativa — ela dava PASS para um serviço em *crash loop*, precisamente o
caso que existia para pegar. Só a injeção de falha da §7 revelou isso. **Uma asserção que nunca viu
a falha que procura é uma afirmação, não uma verificação**, e o mesmo ceticismo vale para as outras
vinte.
