# Camada `meta-med-distro` — MedOS, a política de sistema operacional

> **Status**: documentação de referência da camada. O documento arquitetural autoritativo continua
> sendo `PROJECT_CONTEXT.md`.
>
> **Posição**: camada 2/4 do `MedStack`, prioridade **8** — acima de `meta-med-bsp` (7), abaixo de
> `meta-med-framework` (9) e `meta-med-app` (10).
>
> **Regra que a define**: aqui mora **que tipo de sistema operacional** todo dispositivo
> MedPlatform recebe, independentemente do hardware abaixo e do produto acima. Um EEG, uma bomba de
> infusão e um tomógrafo compartilham esta distro **exatamente igual** e diferem apenas na receita
> de imagem — é essa propriedade que a métrica de reuso mede.

---

## 1. Estrutura

```
meta-med-distro/
├── conf/
│   ├── layer.conf                                       46 linhas
│   └── distro/med-os.conf                              125
├── recipes-core/
│   ├── images/
│   │   ├── med-image-base.bb                           131
│   │   ├── med-image-dev.inc                            34
│   │   ├── med-image-prod.bb                            32
│   │   └── med-image-test.bb                            35
│   ├── med-data-volume/                                 (recipe 114 + 4 arquivos)
│   ├── rauc/                                            (bbappend 63 + system.conf 70 + keyring)
│   ├── seatd/                                           (bbappend 46 + unit 20)
│   └── systemd/                                         (bbappend 29 + 2 arquivos de configuração)
└── recipes-kernel/linux/
    ├── linux-%.bbappend                                 37
    └── files/med-kernel-features.cfg                    91
```

`LAYERDEPENDS` é `core openembedded-layer filesystems-layer rauc`. As duas últimas não são
decoração: `filesystems-layer` traz o ferramental de overlay/squashfs do rootfs imutável, e `rauc` é
exigida pelo `rauc-conf.bbappend`.

**A camada não tem diretório `wic/`, e essa ausência é estrutural.** Um layout de disco precisa
dizer onde o bootloader mora, o que o torna uma afirmação sobre uma placa. Os dois layouts que
moravam aqui estão em `meta-med-bsp`, e `med-os.conf` seleciona um por variável sem jamais nomear
um arquivo.

---

## 2. `med-os.conf` — a política da distro

### 2.1 Init manager, e por que não é preferência

```
INIT_MANAGER = "systemd"
```

É **dependência arquitetural**, não gosto: o `MedicalLogger` do framework é implementado sobre o
journald (sd-journal) e o `MedicalUpdate` sobre a API D-Bus do RAUC (sd-bus). Trocar o init
manager não é trocar um componente, é invalidar duas das seis APIs da camada acima.

### 2.2 Distro features

```
MED_DISTRO_FEATURES = "wayland opengl pam seccomp usrmerge polkit rauc"
DISTRO_FEATURES:remove = "x11 3g nfc"
```

| Feature | Por quê |
|---|---|
| `wayland`, `opengl` | Perfis com HMI (Qt/Wayland); ignorados por perfis headless |
| `pam` | Ganchos de política de autenticação (controle de acesso IEC 62304) |
| `seccomp` | Filtragem de syscalls dos serviços médicos em sandbox |
| `usrmerge` | Árvore `/usr` única, pré-requisito de um rootfs imutável limpo |
| `polkit` | Autorização fina sobre a interface D-Bus do RAUC |
| `rauc` | Marca a plataforma como atualizável A/B (consumido por BSPs) |

A remoção de `x11`, `3g` e `nfc` é redução de superfície de ataque: um equipamento médico não tem
uso para nenhuma das três, e toda feature removida é superfície removida. (O teste gráfico do QEMU
ir por VNC **não** vem daqui — vem do `PACKAGECONFIG` default do `qemu-system-native`, que não
inclui `sdl` nem `gtk+`; ver `BRINGUP_HMI_QEMU.md` §3.)

### 2.3 Endurecimento e rastreabilidade

```
require conf/distro/include/security_flags.inc
PACKAGECONFIG:append:pn-systemd = " gcrypt cryptsetup"
INHERIT += "create-spdx"
INHERIT += "buildhistory"
PACKAGE_FEED_URIS ?= ""
```

- `security_flags.inc` liga `-fstack-protector-strong`, `_FORTIFY_SOURCE`, RELRO completo e PIE para
  toda receita que não optar por fora. **É o ajuste mais invasivo do arquivo**: se uma receita de
  BSP de fabricante falhar com flags incomuns, é a primeira coisa a bissectar.
- `gcrypt` é o que habilita o *Forward Secure Sealing* do journald; `cryptsetup`, o que permite ao
  systemd abrir o volume LUKS do `/etc/crypttab`.
- `create-spdx` produz o SBOM por imagem — o artefato que submissões regulatórias pedem — e
  `buildhistory` é a fonte bruta da métrica de footprint da avaliação experimental.
- `PACKAGE_FEED_URIS` vazio declara que **não há gerenciador de pacotes no campo**: a única mutação
  suportada é um bundle RAUC assinado aplicado ao slot inativo.

### 2.4 `VOLATILE_LOG_DIR = "no"` — a linha que salva a trilha de auditoria

O default do OE transforma `/var/log` em symlink para um tmpfs, limpo a cada boot. Isso anula em
silêncio o `Storage=persistent`, o `Seal=yes` e o `MaxRetentionSec=1year` do
`10-journald-audit.conf`: a trilha seria descartada exatamente no momento em que uma revisão de
incidente precisa dela.

Foi descoberto por uma verificação de QA do scarthgap falhando (`installs files in /var/volatile,
but it is expected to be empty`) — mantenha essa verificação ligada, é a única coisa que pega isso.

### 2.5 `WKS_FILE = "${MED_WKS_FILE}"` — atribuição forte, de propósito

Era `?=`, na suposição de que um `?=` aqui só perderia para um BSP que soubesse melhor. **Não
perde só para esse**: o `bitbake.conf` inclui a conf de máquina *antes* da conf de distro
(linhas 829 e 831), então qualquer máquina que defina `WKS_FILE` com `?=` vence — inclusive o
`qemux86-64.conf:41` do próprio poky, que aponta para `qemux86-directdisk.wks`.

O resultado era um disco MBR com duas partições e nenhum partlabel, em vez do layout A/B — e nada
reportava: a imagem construía, bootava e rodava. Só apareceu ao inspecionar o `.wic` produzido.

---

## 3. Política de kernel

### 3.1 O bbappend curinga

```
linux-%.bbappend  →  aplica-se a linux-yocto, linux-stm32mp, linux-raspberrypi, …
```

O curinga é deliberado (regra 3 do `CLAUDE.md`): portar para um BSP novo custa **zero linhas**
aqui, que é exatamente o que a métrica de portabilidade mede. Nunca criar um
`linux-<fabricante>_%.bbappend` nesta camada.

Duas armadilhas resolvidas dentro dele:

**`linux-%` também casa com não-kernels** (`linux-libc-headers`, `linux-firmware`), que falhariam
com uma entrada inesperada em `SRC_URI`. Daí o guarda `bb.data.inherits_class('kernel', d)`.

**"Receita de kernel" não é uma coisa só** — e esta foi a descoberta cara:

| Mecanismo | Comportamento |
|---|---|
| `kernel-yocto` (linux-yocto) | Varre a `SRC_URI` sozinho procurando `.cfg` e os alimenta à sua própria maquinaria. `SRC_URI` basta. |
| `kernel.bbclass` puro (linux-stm32mp, e a maioria dos kernels de fabricante) | Funde **apenas** o que está em `KERNEL_CONFIG_FRAGMENTS`, com `merge_config.sh`, e ignora o resto da `SRC_URI`. |

Acrescentar só à `SRC_URI` *desempacotava* o fragmento no diretório de trabalho e **não aplicava
uma linha dele**. Não era hipótese: `KERNEL_CONFIG_FRAGMENTS` do `linux-stm32mp` listava os quatro
fragmentos da ST e não o nosso. Todo símbolo que o MedOS pede era, naquele alvo, o que o defconfig
da ST dissesse — e os que saíram certos, saíram por coincidência. Por isso o bbappend acrescenta
**aos dois**, e acrescentar por último importa: o `merge_config.sh` aplica na ordem e a última
atribuição vence.

### 3.2 O fragmento `med-kernel-features.cfg`

São as capacidades que as camadas acima **assumem existir em todo alvo**:

| Grupo | Para quê |
|---|---|
| Control groups v2 | Particionamento de recursos entre o serviço de aquisição e a HMI (segregação IEC 62304) |
| Namespaces + seccomp | Sandbox dos serviços médicos aplicado pelo systemd |
| OverlayFS + SquashFS + xattr em tmpfs | Suporte ao rootfs imutável |
| dm-crypt / LUKS | Volume `/data` cifrado |
| remoteproc + rpmsg + virtio | Comunicação entre núcleos (Cortex-A ↔ Cortex-M) — inofensivo em alvos sem coprocessador |
| Industrial I/O | Lado do kernel para AFEs e ADCs biomédicos |
| Audit + integridade | Rastreabilidade |
| Watchdog | Obrigatório num equipamento desassistido; o systemd o dirige por `RuntimeWatchdogSec` |
| TPM | Custódia de chave do `/data`; distro-wide e não por máquina, porque sem dispositivo presente os drivers não ligam |

---

## 4. As quatro imagens

```
med-image-base.bb            (a metade agnóstica de classe de dispositivo)
├── med-image-prod.bb        require base                → produção
├── med-image-test.bb        require base + dev.inc      → bancada de desenvolvimento
└── (meta-med-app)           require base + dev.inc      → med-image-eeg, med-image-tomograph
```

### 4.1 `med-image-base.bb`

Instala `packagegroup-core-boot` mais `MED_OS_INSTALL`: `rauc`, `med-data-volume`, `cryptsetup`,
`blkid`, `lsblk`, `mke2fs`, `resize2fs`, `ca-certificates`, `dbus`. **Nada de aplicação.**

Quatro mecanismos merecem destaque:

**`MED_ROOTFS_FEATURES ?= "read-only-rootfs"`** — rootfs imutável é o *default*, não uma opção. É
o que torna sólida a história de atualização A/B (o slot em execução não pode divergir do slot que
foi validado) e o que limita o efeito de uma aplicação comprometida. Perfis de desenvolvimento
zeram a variável explicitamente.

**`MED_VERIFICATION_TOOLSET` com `MED_VERIFICATION_TOOLS ?= ""`** — `systemd-analyze`, `chrt`,
`taskset`, `procps`: as ferramentas que transformam as afirmações de plataforma em medições. O
default vazio é o ponto: uma imagem nova sai **sem** ferramental de introspecção a menos que peça,
de modo que `med-image-prod` não pode adquiri-lo por esquecer de optar por fora.

**`MED_BSP_INSTALL ?= ""`** — o gancho pelo qual `meta-med-bsp` instala pacotes de placa sem que
esta receita nomeie uma máquina e sem que aquela camada precise de um bbappend para cima.

**O guarda de `MED_WKS_FILE`** — uma máquina sem layout de disco produziria uma falha de wic sem
causa útil (o sintoma foi `install: cannot stat '.../Image.gz'`, que nomeia o kernel e não o
layout, três passos longe da causa). O guarda dispara em tempo de parse e nomeia a correção:
adicionar `MED_WKS_FILE_DEFAULT:<máquina>` em `meta-med-bsp`.

E o pós-processamento `med_harden_rootfs` grava `RuntimeWatchdogSec=30s`: um equipamento
desassistido deve se reiniciar em vez de ficar travado.

### 4.2 `med-image-dev.inc` — e por que ele existe

Rootfs gravável, `ssh-server-openssh`, `debug-tweaks` e o toolset de verificação, **num só lugar**.

O motivo está escrito no arquivo: `med-image-eeg` e `med-image-tomograph` carregavam cópias próprias
dessas configurações e **elas divergiram** — a configuração de boot QEMU e o ferramental foram parar
só no perfil do EEG. Isso importa mais aqui do que importaria em outro lugar, porque o tomógrafo é
*o caso de controle da avaliação experimental*: divergência entre os dois é contada pela métrica
como se fosse diferença de classe de dispositivo.

Perfis de produção requerem **apenas a base** e portanto não podem adquirir nada disso por
esquecimento. Produção difere de desenvolvimento **por não optar por dentro**, não por lembrar de
optar por fora.

### 4.3 `med-image-prod.bb` e `med-image-test.bb`

A de produção **não instala nada** que já não esteja na base: difere por **remoção**
(`debug-tweaks`, `tools-debug`, `empty-root-password`, `allow-root-login`…), o que mantém as duas
em passo e torna o diff auditável. A de teste acrescenta o que um desenvolvedor precisa num prompt
(`strace`, `gdb`, `tcpdump`, `vim-tiny`).

**`med-image-prod` nunca foi construída.** O rootfs imutável, portanto, nunca foi exercitado — está
na lista de resultados esperados do `PLANO_TCC.md`.

---

## 5. A política de atualização A/B

### 5.1 `rauc-conf.bbappend`

Três decisões, todas com defeito real por trás:

**O nome do arquivo é `rauc-conf.bbappend`, sem `_%`.** O `meta-rauc` do scarthgap distribui a
receita **sem versão** (`rauc-conf.bb`), e o bitbake casa appends contra o *nome do arquivo*: a
forma `_%` exige um sublinhado no `.bb` e portanto não casa com nada — erro duro de parse. Reconferir
após cada bump do `meta-rauc`.

**`RAUC_KEYRING_FILE = "med-keyring.pem"`.** O default do `meta-rauc` aponta para o próprio
`ca.cert.pem`, que não é certificado nenhum — são 358 bytes de comentários mandando o integrador
substituir, e a receita apenas emite um aviso. Embarcar isso dá um dispositivo sem keyring
utilizável, que não falha no build, não falha no boot, e falha **na primeira tentativa de atualizar
um dispositivo em campo**. O arquivo instalado contém a CA raiz **e uma CRL** no mesmo PEM, porque
`check-crl=true` faz o OpenSSL exigir CRL para todo certificado da cadeia.

**`MED_BOOTLOADER ?= "noop"`.** O bootloader é propriedade da máquina, não da distro — nomeá-lo aqui
faria a camada de política de SO carregar conhecimento de BSP. O default é `noop` em vez de um
backend real porque um alvo que esquece de declarar seu bootloader ganha um RAUC que sobe, enumera
slots e se recusa a marcar qualquer coisa como bootável — falha legível, ao contrário de um serviço
morto ou, pior, de uma troca de slot que o bootloader não honra.

A substituição das três variáveis no `system.conf` termina com um guarda que **falha o build** se
sobrar um placeholder — porque a falha, do contrário, só apareceria num dispositivo.

### 5.2 `system.conf`

Quatro linhas fazem o trabalho pesado, e três delas foram descobertas por defeito:

| Linha | Por quê |
|---|---|
| `bundle-formats=-plain` | O dispositivo recusa o formato legado. Sem isso, o default da classe de bundle avisa baixinho e produz um bundle plain que o dispositivo recusa na instalação |
| `check-crl=true` | Recusa bundle assinado por certificado revogado — e é o que obriga a CRL a estar no keyring |
| `check-purpose=codesign` | **Load-bearing, não redundância**: o RAUC registra um propósito X509 próprio e só o instala quando esta chave está presente. Sem ela, o OpenSSL cai no propósito `smime_sign`, que rejeita certificados com EKU codeSigning — o certificado *corretamente emitido* passa a ser a causa da recusa |
| `data-directory=/data/rauc` | O estado dos slots mora no volume que sobrevive à atualização |

Os dois slots são endereçados por rótulo GPT (`med-root-a`/`med-root-b`), únicos por construção. E
**`/data` deliberadamente não é um slot**: registros de paciente, tabelas de calibração e a trilha
de auditoria atravessam a atualização intocados.

---

## 6. O volume `/data`

A receita `med-data-volume` é dona do ponto de montagem, do passo de provisionamento de primeiro
boot e da unidade de montagem.

### 6.1 O `crypttab` intencionalmente vazio

Duas razões, ambas só visíveis depois que o disco A/B ficou real:

1. **O wic não sabe escrever um cabeçalho LUKS**, então `med-data` chega como ext4 puro e precisa
   ser convertida no primeiro boot. Uma entrada no crypttab faria o systemd tentar abri-la antes de
   qualquer coisa ter tido chance de criá-la, e a unidade falharia em todo boot de um dispositivo
   não provisionado.
2. **A chave de desenvolvimento não pode morar no rootfs**, que é um slot A/B — seria destruída
   pela primeira atualização *bem-sucedida*, deixando o dispositivo incapaz de decifrar os próprios
   registros de paciente como resultado direto de um update ter funcionado. E o
   `systemd-cryptsetup` roda antes de `local-fs.target`, então não pode ler chave de um filesystem
   ainda não montado. A unidade de provisionamento monta o depósito privadamente.

O arquivo documenta a entrada que voltará quando a fonte de chave for `tpm2` — aí a chave é
desselada pelo TPM e nenhum dos dois problemas existe.

### 6.2 O safeguard do script de provisionamento

O problema: "nunca provisionado" e "cabeçalho LUKS danificado" produzem **sintomas idênticos**, e
reformatar no segundo caso destrói registros de paciente.

A decisão é chaveada no par `ext4`/`med-data` — o rótulo pristino que o wic escreve. O
`luksFormat` destrói esse rótulo por construção, então o estado pristino **não pode ser reproduzido
por acidente**. Qualquer outra coisa (sem rótulo, rótulo diferente, filesystem desconhecido, lixo)
faz o script **recusar** em vez de formatar. Foi exercitado zerando o cabeçalho, e segura.

### 6.3 `PACKAGE_ARCH` e os dois guardas de parse

A receita **não é `allarch`**, e já foi. `allarch` declara que o pacote produzido é idêntico para
toda máquina, e este não é: o `do_install` substitui `MED_KEY_STORE_DEV`, `MED_KEY_STORE_FSTYPE` e
`MED_DATA_KEY_SOURCE`. Duas máquinas construídas no mesmo `TMPDIR` escreveriam o mesmo nome de
pacote no mesmo diretório `all/` com conteúdos diferentes, e a segunda build sobrescreveria a
primeira em silêncio — um dispositivo provisionando `/data` contra a tabela de partições de outra
placa, sem nada falhar no build.

Os dois guardas em `python ()` movem para tempo de build falhas que de outro modo apareceriam no
primeiro boot do dispositivo, com `/data` não provisionado: fonte de chave desconhecida, e chave de
desenvolvimento sem depósito declarado.

E `MED_DATA_KEY_SOURCE ?= "tpm2"` — o default aponta para a opção **mais** rigorosa, de propósito:
um alvo que esquece de declarar a fonte de chave falha ao provisionar e se recusa a rodar, em vez de
embarcar em silêncio um dispositivo que parece cifrado e não é.

---

## 7. systemd e journald

O `systemd_%.bbappend` instala três coisas: a política de journal, uma configuração de rede
determinística, e — crucialmente — **cria `/var/log/journal` em tempo de build**, porque um rootfs
somente-leitura não pode criá-lo em runtime.

O `10-journald-audit.conf` é o backend de armazenamento do `MedicalLogger`:

| Ajuste | Razão |
|---|---|
| `Storage=persistent` | A trilha sobrevive a ciclo de energia |
| `Seal=yes` | *Forward Secure Sealing*: o journald sela periodicamente com uma chave que depois descarta, tornando detectável qualquer edição retroativa. É a metade do SO da evidência de adulteração; a metade da aplicação é a cadeia de hash do `MedicalLogger` |
| `SystemMaxUse=256M`, `MaxRetentionSec=1year` | Crescimento limitado: um dispositivo que para de adquirir porque o log encheu é problema de segurança, não de logging |
| `RateLimitIntervalSec=0` | Nunca descartar registros de auditoria em silêncio sob carga |
| `ForwardTo*=no` | Nada sai do dispositivo por default; exportar é ação explícita e auditada |
| `Audit=yes` | Mensagens de kernel (remoteproc, rpmsg, dm-crypt) na mesma linha do tempo dos registros de aplicação |

O `seatd_%.bbappend` é o mais recente: o `weston.service` do oe-core roda como `User=weston` e não
consegue abrir `/dev/dri/card0` sozinho — pede ao libseat, que tenta seatd, logind e por fim um
backend embutido que precisa de root. Para um compositor sem privilégio isso não é fallback, é
falha. O oe-core empacota o daemon `seatd` e, sob systemd, **nenhuma unidade**; o bbappend fornece
a unidade. É política de SO — verdadeira em toda máquina MedOS com compositor, sem nomear máquina
nem nó de dispositivo — e por isso está aqui e não em `meta-med-bsp`.

---

## 8. As invariantes desta camada

1. **Nenhum nome de máquina, de bootloader ou de nó de dispositivo.** Duas violações históricas
   originaram `meta-med-bsp`.
2. **Nenhuma lógica de aplicação.** Esta camada não sabe o que é um EEG.
3. **Mudança de kernel entra pelo bbappend curinga**, nunca por um vendor-específico.
4. **Toda substituição de placeholder tem guarda que falha o build** — em `rauc-conf.bbappend` e em
   `med-data-volume`.
5. **Segurança é restrição arquitetural, não hardening opcional**: rootfs imutável, A/B, LUKS e log
   auditável existem para satisfazer requisitos de particionamento e rastreabilidade da IEC 62304.

---

## 9. O que esta camada ainda não tem

- **Controle de acesso obrigatório.** A decisão está registrada (`AppArmor` via `meta-security`, em
  `implementation_plan_mac.md`) e **nenhuma linha foi escrita**.
- **Fonte de chave `tpm2` implementada.** As opções de kernel já entram no fragmento, mas não há
  implementação alcançável no conjunto de camadas atual — o `meta-tpm` traz apenas emuladores em
  software e não há receita de fTPM para OP-TEE. A custódia é, portanto, de desenvolvimento **nos
  dois alvos**.
- **`QB_MEM` declarado.** O default do poky (256 MiB) vence por ausência de adversário e é
  insuficiente para o perfil com HMI — ver `BRINGUP_HMI_QEMU.md` §5. A correção pertence a
  `meta-med-bsp`, mas a lacuna aparece aqui, no perfil que instala a pilha gráfica.
