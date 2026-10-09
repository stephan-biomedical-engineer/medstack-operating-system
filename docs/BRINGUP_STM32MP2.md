# Registro de Engenharia — Porte para o STM32MP257 e o que ele custou

> **Status**: registro do que foi feito, por que, e com que evidência. Não é plano
> (`implementation_plan_*.md`) nem medição consolidada (`RESULTS.md`) — é a narrativa que
> conecta os dois, incluindo os erros de percurso, que num TCC valem tanto quanto os acertos.
>
> **Quando**: 2026-08-17 (porte e construção), 2026-08-18 (primeiro boot em hardware, §9).
> **Onde**: `stm32mp25-disco` (placa física STM32MP257F-DK) e `qemux86-64`.
>
> **Como ler**: cada item traz o sintoma, a causa real (que quase nunca é o sintoma), a correção
> e o que provou que a correção funcionou. Onde nada provou, está escrito que nada provou.

---

## 1. Linha do tempo

| # | Sintoma | Causa real | Correção | Evidência |
|---|---|---|---|---|
| 1 | `make stm32` falha em `do_image_wic`: `cannot stat '.../Image.gz'` | `meta-st-stm32mp` **não define `WKS_FILE`**; o default da distro venceu por ausência de adversário | `med-partitions-stm32mp2.wks.in` | GPT do `.wic` lido byte a byte |
| 2 | — (nenhum) | A correção pôs nome de placa em `meta-med-distro`; a auditoria achou mais duas violações antigas | Nova camada `meta-med-bsp` (prio 7) | `grep` de overrides nas 3 camadas = 0; `make check` 21/21 |
| 3 | Disco do host a 99%, build impossível | 152 GB em `tmp-glibc/work`: nada apagava diretório de trabalho de receita | `INHERIT += "rm_work"` + `BB_DISKMON_DIRS` | `tmp-glibc` caiu de 175 GB para **7,2 GB** |
| 4 | Caches ocupando o NVMe | — | `downloads/` e `sstate-cache/` para HD externo, via `-include local.mk` | `Sstate summary: 2531/2575 (98%)` a partir do HD |
| 5 | `/data` nunca provisionaria no STM32 | Script de provisionamento com a partição da ESP e `vfat` hardcoded | `MED_KEY_STORE_DEV`/`_FSTYPE` em `meta-med-bsp` | `.ipk` extraído e o script lido, nas duas máquinas |
| 6 | — (nenhum) | Receita `allarch` com conteúdo dependente de máquina | `PACKAGE_ARCH = "${MACHINE_ARCH}"` | pacotes saem em `qemux86_64/` e `stm32mp25_disco/` |
| 7 | Overlay do kas "aplicado", pacote com o valor antigo | kas emite `local_conf_header` **em ordem alfabética da chave** | `MED_DATA_KEY_SOURCE:forcevariable` | `bitbake -e` + `.ipk` reextraído |
| 8 | — | `tpm2` não é alcançável nas camadas atuais | documentado, não implementado | ver §7 |
| 9 | Placa reinicia em loop ~7 s após o kernel; pânico do mundo seguro em `clk_stm32_pll_init` | Sem `ACCEPT_EULA` **e** sem `nogpu`, a ST não escreve `blacklist.conf` nenhum — a única combinação em que o `etnaviv` carrega e faz bind em `48280000.gpu` | `MACHINE_FEATURES:append` condicional em `meta-med-bsp`; depois, EULA aceita e stack Vivante | Boot completo até multi-user, `Galcore version 6.4.21.1.1058597`, sem pânico (§9) |
| 10 | — (nenhum) | `/dev/disk/by-partlabel/` é **um namespace plano entre todos os discos**, e o eMMC de fábrica traz os mesmos nomes do layout da ST | `bootfs` → `med-boot`; o que não pôde ser renomeado passa a ser endereçado por PARTUUID | `med-boot` resolveu para `mmcblk0p8` — o cartão — e a chave foi escrita lá (§9) |
| 11 | `rauc status`: `fw_printenv failed with exit code: 1`, ambos os slots `boot status: bad` | `/etc/fw_env.config` não existe; a ST instala `.mmc`/`.nand`/`.nor` e nunca esse nome | não implementado | previsto por inspeção do manifest (`RESULTS.md` §8), agora **observado em execução** (§9) |
| 12 | `weston.service` falha com `status=1` e derruba a HMI por dependência | desconhecida; o DRM da placa subiu e o galcore carregou | em aberto | única unidade falha do sistema (§9) |
| 13 | — (nenhum) | O bbappend curinga só acrescentava à `SRC_URI`; o `linux-stm32mp` herda `kernel` puro e só funde o que está em `KERNEL_CONFIG_FRAGMENTS` | acrescentar às duas | os 9 símbolos passaram de `m`/coincidência para `y` no `.config` produzido (§5) |
| 14 | — (nenhum) | `med-kernel-features.cfg` nunca exigiu `CONFIG_DM_VERITY`, embora a distro imponha bundles `verity`: o `linux-yocto` traz o símbolo por default e o `linux-stm32mp` não | `CONFIG_DM_VERITY=y` + `CONFIG_BLK_DEV_LOOP=y` no fragmento da distro | `config-6.6.129` produzido, e depois `rauc install` **`succeeded`** na placa (§9.10) |
| 15 | `rauc status` num aparelho recém-gravado: todos os slots `bad`, `Activated: none` | **ninguém escreve `BOOT_ORDER` no provisionamento** — o `.wks` cria a partição vazia, o RAUC só grava durante um install, e o `mark-good` grava apenas o contador | em aberto; pertence ao script de U-Boot | `src/bootloaders/uboot.c`: `if (!found) { *good = FALSE; return TRUE; }` (§9.10) |
| 16 | `make verify-bundle` falha logo após um `make bundle` bem-sucedido | `RAUC_NATIVE` apontava para dentro do `WORKDIR`, que o `rm_work` (item 3) apaga ao fim da receita | `sysroots-components/*/rauc-native/…` | `make verify-bundle BOARD=stm32` volta a passar (§9.10) |
| 17 | SSH recusa a conexão com `REMOTE HOST IDENTIFICATION HAS CHANGED` depois de bootar o outro slot | o rootfs não traz chave de host nem `machine-id`; ambos são gerados no primeiro boot **de cada slot** | em aberto | `ls rootfs/etc/ssh/` sem chaves, `/etc/machine-id` com 0 bytes (`BOOT_SLOT_AB_STM32MP2.md` §5.1) |
| 18 | — (nenhum) | `Storage=persistent` põe o journal em `/var/log/journal`, que fica **dentro de um slot A/B** — a próxima atualização o destrói | em aberto | `10-journald-audit.conf` lido no rootfs produzido (`BOOT_SLOT_AB_STM32MP2.md` §5.2) |
| 19 | `eeg-acquisition` em laço (54 reinícios), log só com `front-end refused to start` | com o sandbox completo, `ProtectKernelTunables=yes` monta `/sys` só leitura, e o link `iio` configura o conversor por sysfs | udev liga `/run/med-afe/eeg0` ao sysfs do conversor; a unit ganha `ReadWritePaths=-/run/med-afe/eeg0` | `systemd-run` com as mesmas propriedades nos dois sentidos; serviço com `NRestarts=0` no slot B (§9.12) |
| 20 | — (nenhum) | o slot A/B era confirmado aos ~12,9 s de qualquer boot (`rauc-mark-good` espera uma `boot-complete.target` que nada exigia), e a aplicação chamava `markBootedGood()` antes do `device.start()` | unit `Type=notify`, `RequiredBy=boot-complete.target`, `READY=1` só depois do `device.start()` | fallback medido: três boots com a aquisição falhando, `BOOT_A_LEFT` 3→0, o quarto em B (§9.12) |

Note o padrão: **oito dos dezoito itens não tinham sintoma nenhum**. Não falharam build, não
falharam boot, não emitiram warning. Apareceram porque alguém foi olhar o artefato produzido. E note
o complemento que o primeiro boot acrescentou: os itens 9 a 12 são o oposto — nenhum deles poderia
ter sido encontrado sem energizar a placa, e três deles nenhuma inspeção de artefato teria revelado.

O item 16 acrescenta uma terceira categoria, e é a mais incômoda: **uma correção anterior deste
mesmo documento desativou uma verificação**. O `rm_work` do item 3 resolveu o disco cheio e, de
passagem, matou o `make verify-bundle` por dois commits, sem sintoma. Correções também têm efeitos
colaterais silenciosos.

---

## 2. O defeito que iniciou tudo

`make stm32` morreu na **última** task (5358 de 5364), com:

```
ERROR: _exec_cmd: install -m 0644 .../stm32mp25-disco/Image.gz .../tmp-wic/hdd/boot/Image.gz
       returned '1' instead of 0
output: install: cannot stat '.../Image.gz': No such file or directory
```

O erro nomeia o kernel. O defeito era o layout de disco inteiro.

`med-partitions.wks` afirmava, no próprio cabeçalho, que BSPs com TF-A em offsets fixos "trazem seu
próprio `WKS_FILE` a partir da configuração de máquina". Para o `meta-st-stm32mp` isso é **falso**:
`conf/machine/stm32mp25-disco.conf` define `WKS_FILE_DEPENDS` e deixa
`#WKS_FILE += "${OPTEE_WIC_FILE}"` **comentado**, oferecendo
`wic/sdcard-stm32mp257f-dk-optee-example.wks.in` apenas como exemplo para copiar.

Consequência: o default da distro venceu por ausência de adversário, e o wic tentou montar uma ESP
EFI/GRUB numa placa que não tem ESP. Falhou no primeiro artefato que não existe sob aquele nome — o
kernel, porque a ST publica em `${DEPLOY_DIR_IMAGE}/kernel/` e não na raiz.

**A lição, que é a mesma da §1 do `RESULTS.md`**: o defeito não era código, era uma **suposição
documentada que nenhum build verifica**. Uma frase de comentário afirmando o comportamento de
terceiros, que ninguém conferiu, e que ficou errada quando o terceiro mudou — ou, neste caso, quando
o terceiro nunca fez o que a frase dizia.

### O layout novo

`meta-med-bsp/wic/med-partitions-stm32mp2.wks.in` — cadeia de boot da ST nos offsets que o ROM code
exige, e depois a mesma política A/B de qualquer máquina MedOS:

```
fsbla1 fsbla2  metadata1 metadata2  fip-a fip-b  u-boot-env | bootfs | med-root-a | med-root-b | med-data
  TF-A (2 cópias)   FWU metadata      OP-TEE+U-Boot    env   | kernel |  (ativo)   | (inativo)  |  LUKS
```

Verificado por inspeção do `.wic`, não pelo build ter passado:

- 11 partições, nomes GPT corretos;
- os dois slots com **2097152 setores exatos** — simetria é requisito, e já saiu errada uma vez;
- `PARTUUID` de `med-root-a` = `e91c4e10-16e6-4c0e-bd0e-77becf4a3582`, **igual** ao `root=PARTUUID=`
  do `extlinux.conf` que o BSP gera a partir de `DEVICE_PARTUUID_ROOTFS:mmc0`. Qualquer outro valor
  = kernel em panic sem raiz, e nada no build reportaria;
- magic `STM2` no setor 34 (17 KiB, onde o ROM code procura o FSBL) e magic do FIP `0xaa640001` no
  setor 2082;
- `bootfs` com `Image.gz` (11.219.612 B), `extlinux/extlinux.conf` e os três devicetrees.

---

## 3. A auditoria de arquitetura, e a camada que faltava

A primeira correção foi posta em `meta-med-distro/wic/` — ou seja, **nome de placa dentro da camada
de política de OS**. Ao ser questionada, a auditoria mostrou que a fronteira já tinha sido cruzada
duas vezes antes, e nenhuma delas falhava build:

| # | Onde | O quê |
|---|---|---|
| 1 | `med-partitions.wks` | `loader=grub-efi` e `console=ttyS0,115200`, atrás de um nome de arquivo genérico |
| 2 | `med-image-base.bb:88` | `QB_KERNEL_ROOT = "/dev/vda2"` — caminho virtio **e** índice de partição de um `.wks` específico — duas linhas abaixo de um comentário afirmando que a receita é agnóstica a dispositivo |
| 3 | `med-data-provision.sh:35` | `ESP=/dev/disk/by-partlabel/esp` e `mount -t vfat` |
| 4 | `CLAUDE.md:87` | "a superfície de adaptação é deliberadamente uma variável" — eram cinco |

O mecanismo comum aos três primeiros: **nome genérico sobre conteúdo específico de máquina**.
`med-partitions.wks` parecia portátil, `med-image-base.bb` se declarava agnóstico, e
`med-data-provision.sh` não dizia "ESP" no nome. Nenhum build detecta isso, por construção: bitbake
não tem opinião sobre que camada pode saber o quê.

### `meta-med-bsp`, prioridade 7

A distinção que a camada existe para impor é **política contra colocação**:

```
meta-med-app        (10)  aplicações
meta-med-framework   (9)  APIs C++
meta-med-distro      (8)  POLÍTICA — dois slots idênticos, /data fora deles, rootfs read-only
meta-med-bsp         (7)  COLOCAÇÃO — onde o TF-A começa, qual bootloader, qual console, onde a chave mora
BSP do vendor        (6)  meta-st-stm32mp, meta-yocto-bsp
```

`meta-med-bsp` é a **única camada custom autorizada a nomear máquina, bootloader ou device node**. É
isso que torna a alegação de reuso contável em vez de retórica: placa nova ganha arquivos ali e não
muda nada acima.

Detalhes que custaram a descobrir e valem registrar:

- **`LAYERDEPENDS = "core"` apenas.** A camada fornece e não consome; um bbappend daqui sobre receita
  de camada superior inverteria a cadeia.
- **A indireção em dois passos é obrigatória.** `MED_WKS_FILE_DEFAULT:<machine>` e depois
  `MED_WKS_FILE ?= "${MED_WKS_FILE_DEFAULT}"`. Escrever `MED_WKS_FILE:<machine> =` direto tornaria a
  camada **insobreponível**: valor com sufixo de override vence atribuição sem sufixo, então um
  arquivo de projeto que tentasse mandar perderia **em silêncio**. Verificado por injeção de falha —
  `MED_WKS_FILE = ""` num fragmento kas sobrepõe a camada e dispara o guard de build.
- **`BBFILE_PATTERN_IGNORE_EMPTY_meta-med-bsp = "1"`** enquanto a camada for só de conf, senão o
  bitbake avisa uma vez por build — e warning em todo build é como warnings deixam de ser lidos.
- **`med-image-base.bb` falha o build** com mensagem acionável se nenhuma adaptação responder, em vez
  de deixar o wic errar sem causa.

### O que a camada custou de correção nas alegações

`CLAUDE.md` afirmava que a superfície de adaptação era uma variável. São **quatro com valores
distintos** entre os alvos (`MED_EEG_DRIVER`, `MED_BOOTLOADER`, `MED_DATA_KEY_SOURCE`,
`MED_WKS_FILE`) mais **um arquivo** de conteúdo de camada (o `.wks` que a quarta seleciona). A
tabela do `PROJECT_CONTEXT.md` §4.1 lista seis; `MED_EEG_REQUIRE_ENCRYPTION` vale `"true"` nos dois
e `MED_AMP_FIRMWARE` está vazio em ambos, então não entram na contagem.

A alegação **certa** — e ela continua forte — é: nenhum fonte de aplicação, de framework ou de
política de OS difere entre os alvos, e o `grep` de override de máquina nas três camadas retorna
zero.

---

## 4. Custódia de chave por máquina

`med-data-provision.sh` fixava `/dev/disk/by-partlabel/esp` e `mount -t vfat`, que só existem no
layout EFI. Na STM32MP2 não há partição `esp` **nenhuma**, e o único motivo de nada falhar era que
`MED_DATA_KEY_SOURCE = "tpm2"` recusava antes de chegar naquele código: **um defeito escondido atrás
de uma recusa**.

A restrição que decide a resposta não vem do disco, vem do caminho de atualização: o rootfs é um slot
A/B, então uma chave em `/etc` é destruída pela primeira atualização **bem-sucedida**, e o
dispositivo perde os próprios prontuários como resultado direto de um update funcionar. A chave tem
de morar numa partição que nenhum update escreve.

| máquina | `MED_KEY_STORE_DEV` | fstype |
|---|---|---|
| `qemux86-64` | `by-partlabel/esp` | `vfat` |
| `stm32mp25-disco` | `by-partlabel/bootfs` | `ext4` |

`bootfs` satisfaz o critério (fora dos dois slots) e ganha uma coisa sobre a ESP: sendo ext4, o
`chmod 0400` que o script tenta finalmente tem efeito — no vfat nunca teve, porque vfat não carrega
modos POSIX.

### Dois defeitos que essa mudança expôs

**A receita era `allarch` com conteúdo dependente de máquina.** `allarch` declara que o pacote é
idêntico para toda máquina, e não era: o `sed` já dependia de `MED_DATA_KEY_SOURCE`, que difere por
alvo. Duas máquinas construídas no mesmo `TMPDIR` escreveriam o mesmo nome de pacote no mesmo
`deploy/ipk/all/`, e a segunda sobrescreveria a primeira — um dispositivo provisionando `/data`
contra a tabela de partição de outra placa, sem nada falhar em tempo de build. Agora é
`PACKAGE_ARCH = "${MACHINE_ARCH}"`.

**O overlay do kas perdia em silêncio.** O kas emite os blocos de `local_conf_header` **ordenados
alfabeticamente pela chave**. O bloco `bringup` saiu na linha 1 do `local.conf`, o `eeg-stm32mp2` na
75 — e o `MED_DATA_KEY_SOURCE = "tpm2"` do perfil, 86 linhas abaixo, sobrescrevia o `development` do
overlay. O build passava, o overlay *parecia* aplicado, e o pacote produzido continuava dizendo
`tpm2`. Só apareceu ao extrair o `.ipk` e ler o script substituído.

A correção imediata foi `MED_DATA_KEY_SOURCE:forcevariable`, último item do `OVERRIDES` do bitbake,
que vence independentemente de onde o kas decida escrever o bloco. Renomear a chave para ordenar por
último também funcionaria e quebraria no dia em que alguém acrescentasse um bloco com letra
posterior. Mas nenhuma das duas remove a armadilha de ordenação para o próximo overlay, e é por isso
que o overlay deixou de existir (abaixo).

### O bring-up como parâmetro, não como segundo comando

O delta sempre foi **uma variável**, e por um tempo isso custou um arquivo `kas/bringup-stm32mp2.yml`
e um alvo `make stm32-bringup`. Hoje é um parâmetro:

```bash
make stm32                    # MED_DATA_KEY_SOURCE = "tpm2", o que o produto deve ser
make stm32 KEY=development    # a custódia do primeiro boot
```

`kas-base.yml` declara `MED_DATA_KEY_SOURCE` sob `env:` com valor **nulo** — no kas isso significa
"acrescente o nome ao `BB_ENV_PASSTHROUGH_ADDITIONS`, mas só defina se o ambiente já tiver"
(`libkas.py` filtra os `None` antes de entregar o ambiente ao bitbake). Um default em string ali
seria pior que inútil: entraria no datastore de toda máquina e responderia calado uma pergunta que
cada alvo deve responder por si.

Duas consequências que não são estilo:

- os perfis passaram a declarar `MED_DATA_KEY_SOURCE ?=`, não `=`. O bitbake põe uma variável de
  passthrough no datastore **antes** de parsear o `local.conf`, então um `=` no perfil venceria o
  parâmetro — de novo em silêncio, com build verde e a string errada substituída no script embarcado.
  É literalmente o mesmo modo de falha do overlay, um nível acima;
- o `kas-container` encaminha só uma whitelist fixa de variáveis para dentro do contêiner
  (`kas-container:731`), e ela é do upstream, não nossa. Por isso o `Makefile` exporta a variável no
  modo `NATIVE=1` e a entrega ao `docker run` via `--runtime-args "-e ..."` no modo contêiner.

O perfil `project-eeg-stm32mp2.yml` continua declarando `tpm2`, que é o que o produto deveria ser;
sobrescrever a configuração do alvo para fazer um primeiro boot passar deixaria o repositório
descrevendo um dispositivo que nunca foi pretendido, sem registrar que a diferença era temporária.
Um parâmetro na linha de comando registra isso melhor que um arquivo: não há configuração paralela
para alguém confundir com a do produto.

Um valor desconhecido em `KEY` agora falha no build, não na placa: o `Makefile` recusa qualquer coisa
fora de `tpm2`/`development`, e `med-data-volume_1.0.bb` repete a checagem em `bb.fatal` para pegar
também quem chame `kas build` direto.

E note o que **não** foi dispensado: `MED_EEG_REQUIRE_ENCRYPTION` continua `"true"`. Com a custódia
resolvida, `/data` é LUKS2 de verdade na placa, o `MedicalStorage` encontra um `dm/uuid` começando em
`CRYPT-`, e o serviço subir continua sendo a asserção de que a criptografia é real.

---

## 5. O fragmento de kernel nunca era aplicado (e o diagnóstico anterior era gentil demais)

Ao conferir se os símbolos de que o LUKS depende chegaram ao kernel da ST — checagem que a lição do
TPM tornou obrigatória — apareceu um padrão:

| símbolo | fragmento pede | `.config` construído |
|---|---|---|
| `CONFIG_BLK_DEV_DM`, `DM_CRYPT`, `CRYPTO_XTS` | `y` | **`m`** |
| `CONFIG_RPMSG_CHAR`, `RPMSG_CTRL` | `y` | **`m`** |
| `CONFIG_TCG_TIS_CORE`, `TCG_TIS` | `y` | **`m`** |
| `CONFIG_CRYPTO_AES`, `SHA256`, `KEYS`, `REMOTEPROC`, `TCG_TPM` | `y` | `y` ✓ |

Funcionalmente não bloqueia: os módulos estão na imagem (1054 pacotes `kernel-module-*`) e carregam
sob demanda, então `cryptsetup` puxa o `dm_crypt`. Mas a **intenção do fragmento não foi honrada**, e
nada reportou.

Isso qualifica a regra 3 do `CLAUDE.md`: o `linux-%.bbappend` curinga garante que o fragmento
*chegue* a qualquer kernel de vendor — verificado, o `med-kernel-features.cfg` está no `WORKDIR` do
`linux-stm32mp` — mas **não** garante que ele *vença*. A ordem em que a receita da ST aplica os
próprios fragmentos não foi investigada. Está em aberto.

**Confirmado no kernel em execução** (2026-08-18, `zcat /proc/config.gz` na placa):

```
CONFIG_TCG_TIS_CORE=m
CONFIG_TCG_TIS=m
CONFIG_TCG_TIS_SPI=m
```

O que a tabela acima lia do `.config` construído, o kernel que bootou confirma. Vale registrar uma
hipótese **descartada**, porque ela é a explicação intuitiva e está errada: não é o caso de o núcleo
`TCG_TPM` ter saído módulo e arrastar os filhos — a própria tabela acima mostra `CONFIG_TCG_TPM`
saindo `y`. Um `bool`/`tristate` filho vindo `m` sob um pai `y` é uma escolha ativa de alguém, não
uma consequência de dependência, e continua sem causa identificada.

Funcionalmente segue sem bloquear nada: não há TPM nesta placa (§7), então o símbolo não teria o que
controlar de qualquer forma. O que ele bloqueia é a **frase** — e ao ir atrás de por que um `bool`
filho sairia `m` sob um pai `y`, a frase acabou sendo pior do que se supunha.

### A causa, e por que "perde" era o diagnóstico errado

O `linux-%.bbappend` fazia uma coisa só: acrescentar o fragmento à `SRC_URI`. Isso basta para o
`linux-yocto`, que herda `kernel-yocto` e varre a `SRC_URI` atrás de `.cfg` sozinho — e é
exatamente por isso que o mecanismo funcionava no QEMU e *parecia* portátil. O `linux-stm32mp` herda
`kernel` puro: o `do_configure` dele (`linux-stm32mp.inc:76-95`) funde exatamente os arquivos
listados em `KERNEL_CONFIG_FRAGMENTS` com o `merge_config.sh`, verifica que cada um existe, e ignora
todo o resto da `SRC_URI`.

Medido, não deduzido:

```
$ bitbake -e virtual/kernel | grep ^KERNEL_CONFIG_FRAGMENTS=
KERNEL_CONFIG_FRAGMENTS=" .../fragment-01-defconfig-cleanup.config
                          .../fragment-02-defconfig-addons.config
                          .../fragment-03-systemd.config
                          .../fragment-04-modules.config "
$ bitbake -e virtual/kernel | grep -c med-kernel-features
12          # está na SRC_URI, e em lugar nenhum que importe
```

Então o fragmento não *perdia* uma disputa de precedência. **Ele nunca entrava na disputa.** No alvo
físico, toda a política de kernel do MedOS — cgroups, OverlayFS, dm-crypt, rpmsg, TPM — era baixada,
descompactada e ignorada, e cada símbolo valia o que o defconfig da ST dissesse. Os quatro que saíam
`y` saíam `y` porque a ST já os queria assim.

Vale encarar a consequência: **o `/data` LUKS funcionou na placa por coincidência.** `DM_CRYPT=m` é
escolha da ST. Se a ST tivesse desligado o símbolo, o resultado da §9.4 não existiria, e nada no
repositório teria avisado.

### A correção, e o que ela mede

Uma linha, no mesmo bbappend curinga: acrescentar o fragmento também a `KERNEL_CONFIG_FRAGMENTS`,
que o `linux-yocto` ignora. Os dois mecanismos ficam cobertos sem o bbappend saber qual kernel está
em uso, que é a propriedade que a regra 3 do `CLAUDE.md` promete.

Depois de `bitbake -c configure -f virtual/kernel`, lendo o `${B}/.config` produzido, a coluna de
`m` da tabela acima desapareceu inteira: `TCG_TPM`, `TCG_TIS_CORE`, `TCG_TIS`, `BLK_DEV_DM`,
`DM_CRYPT`, `CRYPTO_XTS`, `RPMSG_CHAR`, `RPMSG_CTRL` e `OVERLAY_FS` saem todos `y`. O log do
`merge_config.sh` mostra o nosso fragmento como o **último** da lista — é isso que o faz vencer — e
as únicas mensagens sobre ele são `redundant`, ou seja, nenhum símbolo nosso conflitava em silêncio
com os da ST.

Não validado em hardware: o kernel novo não foi construído nem bootado. O que está medido é o
`.config` produzido, que é precisamente o degrau que faltava antes.

---

## 6. Infraestrutura do host

O porte encheu um disco de 468 GB até 99%. A investigação:

```
build/tmp-glibc     175 GB   ← 152 GB só em work/
sstate-cache         12 GB
downloads           8,8 GB
build/buildhistory  286 MB   ← evidência do RESULTS.md; fica FORA de tmp-glibc
```

`work/` guarda o diretório de trabalho de **cada receita** — fonte desempacotado, árvore de build —
e nada apagava. 5364 tasks deixaram 152 GB de entulho.

**Correção**: `INHERIT += "rm_work"`, com `RM_WORK_EXCLUDE` preservando imagens e kernels — porque
neste repositório ler o `work/` não é conveniência de depuração, é como as alegações são conferidas —
e `BB_DISKMON_DIRS = "STOPTASKS,${TMPDIR},10G HALT,${TMPDIR},3G"`, porque sem isso o modo de falha
não é build quebrado, é disco cheio levando a sessão gráfica junto. (`HALT`, não `ABORT`: o bitbake
renomeou a ação e avisa no nome antigo, `monitordisk.py:79`.)

**Resultado medido**: o mesmo tipo de build passou a deixar **7,2 GB** de `tmp-glibc`, contra 175 GB.

### O HD externo, e por que não se põe o `build/` nele

Medição com origem em `/dev/shm` (RAM), para o disco medido ser só o destino; 20.000 arquivos de
6,5 KiB de média, que é o perfil de `do_unpack`/`do_install`:

| fase | NVMe | HD USB (480M) | razão |
|---|---|---|---|
| escrever 20k arquivos + `sync` | 0,69 s — 29.052 arq/s | 9,02 s — 2.216 arq/s | ~13× |
| apagar 20k + `sync` | 0,20 s — 101k arq/s | 0,82 s — 24k arq/s | 4,1× |
| sequencial escrita `O_DIRECT` | 971 MB/s | 25,0 MB/s | 39× |
| sequencial leitura `O_DIRECT` | 1,9 GB/s | 35,9 MB/s | 53× |
| `stat` | **não medido** | **não medido** | — |

Duas honestidades: `stat` saiu idêntico nos dois porque os metadados estavam no page cache — não é
medida de disco, e obter uma exige dropar cache, que exige root. E na primeira rodada o `delete`
também saiu idêntico, porque `rmtree` sem `sync` só suja o journal em memória; com `sync` a diferença
apareceu.

**A decisão que os números impõem**: `downloads/` e `sstate-cache/` vão para o HD (leitura
sequencial, tolera disco lento); `build/` fica no NVMe (milhões de arquivos pequenos, ~10× pior).
Mecanismo: `-include local.mk` no `Makefile`, com `local.mk` no `.gitignore` — onde a máquina guarda
seus caches é propriedade da máquina, não do projeto, pelo mesmo motivo que nenhuma camada nomeia
placa.

Verificado ponta a ponta: `SSTATE_DIR="/sstate"` dentro do container (não o
`${TOPDIR}/../sstate-cache` do default, que não é ponto de montagem e erraria em silêncio), `df`
mostrando `/dev/sda1` nos dois mounts, e `Sstate summary: Wanted 2575 Local 2531 (98% match)`.

### Duas armadilhas do disco externo

1. **Sistema de arquivos**: tem de ser ext4/xfs/btrfs. Em NTFS ou exFAT — padrão de fábrica — não é
   lentidão, é impossibilidade: `pseudo` precisa de extended attributes para simular ownership de
   root, e o build usa hardlinks e symlinks.
2. **Ponto de montagem sob `$HOME`**: o `BUILD_CONTAINER.md` §5 registra que caminho fora de `$HOME`
   quebra o Docker snap, com falha **silenciosa** (monta sem erro, chega vazio). Montado em `~/hd`
   via `fstab`, e **por UUID** — o que se provou necessário: ao trocar o cabo de porta, o disco
   voltou como `/dev/sdb1` em vez de `/dev/sda1`, e o systemd remontou sozinho. Entrada por caminho
   de dispositivo teria falhado.

Após uma desconexão a quente, a integridade foi testada de verdade: `zstd -t` em **5021 objetos** de
sstate, todos íntegros.

**`make eject`** (`scripts/med-eject.sh`) automatiza a retirada segura, e existe por um motivo
específico de risco: o disco de caches e o cartão SD do alvo aparecem como o mesmo tipo de
dispositivo, com letras vizinhas. Gravar a imagem em `/dev/sdb` achando que é o cartão destrói o
cache inteiro, e é um erro de uma tecla. Retirar o disco antes de gravar não reduz esse risco — o
alvo errado deixa de existir. O script recusa e diz qual PID está segurando, em vez de forçar um
`umount -l`, cujo modo de falha é perder escrita em voo silenciosamente.

---

## 7. `tpm2`: o que se descobriu

A pergunta "o TPM não é essencial para a criptografia?" levou a uma verificação que muda o
planejamento.

**O TPM não criptografa nada.** Quem criptografa é o `dm-crypt`/LUKS, no kernel, com AES-XTS. O TPM
responde a outra pergunta: **onde a chave mora e sob que condições é liberada** — custódia, não
cifragem. Sem TPM a criptografia é real; o que muda é o modelo de ameaça.

**E o modelo de ameaça atual é mais fraco do que o `RESULTS.md` afirmava.** A chave de
desenvolvimento fica numa partição do **mesmo cartão** onde está o volume cifrado. Quem leva a mídia
leva as duas coisas. A afirmação "protege contra remoção física da mídia" estava invertida e foi
corrigida (`RESULTS.md` §7): contra remoção da mídia ela protege quase nada; o que ela dá é contra
exfiltração *parcial* — cópia só da partição `med-data`, ou backup de `/data` — e contra root no
dispositivo ligado, via a recusa do `MedicalStorage`.

**E `tpm2` não é alcançável nas camadas atuais.** Verificado:

- `meta-security/meta-tpm` oferece `swtpm` e `ibmswtpm2`, que são **emuladores em software** —
  guardam estado num arquivo do mesmo sistema de arquivos, então trocariam um arquivo de chave por
  outro com mais passos e nenhuma raiz de confiança;
- **não existe receita de fTPM** (TPM como Trusted Application dentro do OP-TEE) nem no
  `meta-security` nem no `meta-st-stm32mp` (`grep -rl ftpm` no meta-st: vazio).

As duas saídas reais são um **chip TPM discreto** no SPI/I2C da placa (os drivers `TCG_TIS_SPI` e
`TCG_TIS_I2C` já estão no kernel, como módulos), ou **portar um fTPM para o OP-TEE**, que já está no
FIP gravado — viável e elegante no MP2, mas é porte, não variável de configuração.

E mesmo com TPM, a armadilha do `implementation_plan_luks.md` §7.3 continua: selar a chave a PCRs que
medem o rootfs faz uma atualização A/B **bem-sucedida** mudar as medições, o TPM recusar liberar a
chave, e o dispositivo perder os prontuários — o mesmo desastre da chave-no-rootfs, por outro caminho.

**Formulação honesta para a tese**: a plataforma implementa e valida o *mecanismo* de volume
criptografado, nos dois alvos; a *custódia* permanece de desenvolvimento em ambos, e o caminho para
custódia real está identificado e não implementado.

---

## 8. Sobre construir na nuvem (avaliado, não adotado)

A pressão de disco levantou a hipótese de mover o build para CI. Registrado porque a conclusão é
contra-intuitiva:

- **Runner gratuito não constrói Yocto.** O `ubuntu-latest` do GitHub tem ~14–22 GB utilizáveis (o
  disco é ~72 GB, mas >50 GB vêm ocupados por SDKs pré-instalados); com actions de limpeza chega-se a
  ~45–60 GB. Some o teto de 6 h por job em 2 vCPU. O problema de fundo é conceitual: **o Yocto é
  rápido porque guarda estado entre builds**, e CI efêmero descarta exatamente esse estado, então
  todo PR vira build do zero.
- **Bamboo não é uma opção, a qualquer preço.** Não existe Bamboo Cloud; o Data Center teve a venda
  para novos clientes **encerrada em 30/03/2026** e chega a fim de vida em 28/03/2029.
- **O que funcionaria**: Bitbucket Cloud gratuito + **runner self-hosted**, que não consome minutos de
  build, numa VPS cobrada por hora com o `sstate` persistente. Custo Atlassian: zero.

Decisão: não adotado. `rm_work` + HD externo resolveram o problema real (o disco), e o custo/benefício
de manter infraestrutura para um TCC não se paga.

---

## 9. O primeiro boot na placa

**2026-08-18.** Imagem construída com `make stm32 KEY=development`, gravada em cartão SD, console
serial pelo ST-LINK V3 (`/dev/serial/by-id/usb-STMicroelectronics_STLINK-V3_*`, minicom).

É a primeira vez que qualquer coisa deste repositório executou em hardware. Tudo abaixo é log de
console e saída de comando, transcritos; onde houve inferência, está marcado como inferência.

### 9.1 A cadeia de boot fecha, e o PARTUUID prova que não foi coincidência

```
Boot over mmc0!
Scanning mmc 0:8...
Found /extlinux/extlinux.conf
Retrieving file: /Image.gz
append: root=PARTUUID=e91c4e10-16e6-4c0e-bd0e-77becf4a3582 rootwait rw ...
Retrieving file: /stm32mp257f-dk-ca35tdcid-ostl.dtb
...
Machine model: STMicroelectronics STM32MP257F-DK CA35TDCID OSTL
Linux version 6.6.129 (oe-user@oe-host) (aarch64-med-linux-gcc ...)
EXT4-fs (mmcblk0p9): mounted filesystem d60966ff-... 
VFS: Mounted root (ext4 filesystem)
```

O `mmc 0:8` é a `med-boot`, oitava partição do layout. O `e91c4e10-16e6-4c0e-bd0e-77becf4a3582` é,
literalmente, o `--uuid` que `med-partitions-stm32mp2.wks.in` fixa em `med-root-a` — o `.wks` o fixa
justamente porque o `extlinux.conf` que o BSP gera referencia `${DEVICE_PARTUUID_ROOTFS:SDCARD}`, e
a §2 registrava isso como uma restrição a respeitar. Ela foi respeitada, e o boot é a prova.

A variante `-ca35tdcid-ostl` do TF-A/FIP também se confirma pelo `Machine model` e pelo `.dtb` que o
U-Boot escolheu sozinho, derivado do devicetree embutido no FIP.

O rootfs monta `rw` (`grep ' / ' /proc/mounts` → `/dev/root / ext4 rw,relatime`). **Isso não é
defeito**: `med-image-eeg` requer `med-image-dev.inc`, que zera `MED_ROOTFS_FEATURES` de propósito.
Rootfs imutável é política do `med-image-prod`, que segue nunca construído.

### 9.2 A GPU: a hipótese estava certa

O item 9 da §1 é o defeito que impediu qualquer boot antes deste. O sintoma era pânico do mundo
seguro em `clk_stm32_pll_init` (`clk-stm32mp25.c:2002`) cerca de sete segundos após o kernel,
imediatamente depois de `etnaviv etnaviv: bound 48280000.gpu`, seguido de reboot em loop —
reproduzido duas vezes, e sobreviveu à troca completa da variante TF-A/OP-TEE/U-Boot, o que
descartou incompatibilidade de devicetree.

A causa é um buraco de configuração: `linux-stm32mp.inc` só escreve
`/etc/modprobe.d/blacklist.conf` se `MACHINE_FEATURES` contiver `gpu` **ou** `nogpu`. Sem nenhum dos
dois — sem `ACCEPT_EULA`, sem `nogpu` — nenhum arquivo é escrito, que é a única combinação em que o
`etnaviv` efetivamente carrega, faz bind na GPU e pede ao mundo seguro, via SCMI, uma PLL que aquela
configuração de firmware não tem preparada.

A correção imediata foi `nogpu` condicional em `meta-med-bsp`. A configuração que bootou vai além:
com `ACCEPT_EULA_stm32mp25-disco = "1"` no arquivo de projeto, o `nogpu` se desliga sozinho, entra o
stack proprietário Vivante, e o `MED_GPU_PACKAGES` do `packagegroup-med-gui` traz os pacotes que o
`GPU_IMAGE_INSTALL` da ST não entregaria por conta própria (a imagem não instala
`packagegroup-base`, então nada no grafo alcançava `MACHINE_EXTRA_RRECOMMENDS`).

Resultado:

```
[    7.284737] galcore: loading out-of-tree module taints kernel.
[    7.446393] Galcore version 6.4.21.1.1058597
```

Sem pânico. O boot chegou a multi-user e a placa ficou de pé por mais de oito minutos. **A hipótese
registrada no `layer.conf` — "o stack proprietário da ST usa os clocks que o firmware da ST prepara"
— era hipótese até este boot, e agora é observação.**

### 9.3 A colisão de `by-partlabel`, e a descoberta de que a enumeração é uma corrida

O registro anterior media que `/dev/disk/by-partlabel/bootfs` apontava para `mmcblk0p6` — o eMMC de
fábrica — enquanto a nossa partição era `mmcblk1p8`, e concluía que "o eMMC ganha por enumerar
primeiro". Este boot mostra que a conclusão estava certa pelo motivo errado:

```
mmc2: new HS200 MMC card at address 0001
mmcblk2: mmc2:0001 008GB1 7.28 GiB
 mmcblk2: p1 p2 p3 p4 p5 p6 p7 p8            <- eMMC de fábrica (OpenSTLinux)

mmc0: new ultra high speed SDR104 SDHC card at address aaaa
mmcblk0: mmc0:aaaa WC16G 14.8 GiB
 mmcblk0: p1 p2 p3 p4 p5 p6 p7 p8 p9 p10 p11 <- nosso cartão
```

Os papéis **inverteram**: o cartão virou `mmcblk0` e o eMMC virou `mmcblk2`, o oposto da medição
anterior, no mesmo hardware e sem nenhuma mudança de configuração relacionada. A numeração
`mmcblkN` não é uma propriedade da placa: é o resultado de uma corrida entre controladores MMC.

Isso é mais forte do que o argumento que o `layer.conf` registra hoje. Não é que "o eMMC ganha" —
é que **não existe ordem em que se apoiar**, e portanto nenhum caminho `/dev/mmcblkNpM` pode ser
escrito em lugar nenhum deste repositório, nem sequer como comentário de referência.

O placar dos nomes, com o que é medição e o que é inferência separados:

| nome | resolve para | como se sabe |
|---|---|---|
| `med-data` | `/dev/mmcblk0p11` (cartão) | `cryptsetup status med-data` imprime o device |
| `med-boot` | `/dev/mmcblk0p8` (cartão) | montar por `by-partlabel` imprimiu `EXT4-fs (mmcblk0p8)` |
| `med-root-a` | `/dev/mmcblk0p9` (cartão) | `rauc status`: `Booted from: rootfs.0 (/dev/mmcblk0p9)` |
| `bootfs`, `rootfs`, `vendorfs` | eMMC | inferência sólida: estes nomes **não existem** no nosso layout |
| `u-boot-env` | **`/dev/mmcblk2p5` — o eMMC de fábrica** | `readlink -f`, medido depois (§9.8); o nosso é `mmcblk0p7` |
| `fip-a`, `fip-b`, `metadata1`, `metadata2` | não determinado | os nomes existem nos dois layouts; a saída do `ls -l` foi truncada |

Os quatro nomes `med-*` são únicos por construção e todos foram para o cartão — que é exatamente o
que a renomeação `bootfs` → `med-boot` existia para garantir, e o motivo pelo qual a chave do
`/data` foi parar no disco certo.

A linha do `u-boot-env` era uma pendência, não um detalhe: **é dela que dependia a forma do
`/etc/fw_env.config`** (§9.5). Está fechada, e da pior maneira possível — o nome foi para o disco
errado. Os quatro nomes restantes continuam sem medição, e o comando que fecha sem truncamento é:

```sh
for l in /dev/disk/by-partlabel/*; do echo "$l -> $(readlink -f $l)"; done
```

### 9.4 `/data` criptografado, em hardware

O resultado principal deste boot, e o que faz o §7 do `RESULTS.md` deixar de ser exclusivo do QEMU.

```
$ systemctl status med-data-provision.service
     Active: active (exited) since Thu 2025-05-29 18:48:39 UTC
    Process: 188 ExecStart=/usr/libexec/medplatform/med-data-provision.sh (code=exited, status=0/SUCCESS)
        CPU: 16.778s

$ cryptsetup status med-data
/dev/mapper/med-data is active and is in use.
  type:    LUKS2
  cipher:  aes-xts-plain64
  keysize: 512 bits
  device:  /dev/mmcblk0p11
  offset:  32768 sectors
  size:    1015808 sectors

$ cat /sys/block/dm-0/dm/uuid
CRYPT-LUKS2-c1480c70814d49579d3baf01a04a64be-med-data

$ awk '$2=="/data"{print $1,$3}' /proc/mounts
/dev/mapper/med-data ext4

$ mount -o ro /dev/disk/by-partlabel/med-boot /mnt && stat -c '%n %U:%G %a' /mnt/medplatform/data.key
[  539.738686] EXT4-fs (mmcblk0p8): mounted filesystem ff539d9e-... ro
/mnt/medplatform/data.key root:root 400
```

Quatro coisas que isto fecha:

1. **O provisionamento de primeiro boot funciona no alvo físico**, não apenas no QEMU: 13 s de
   relógio (18:48:26 → 18:48:39), 16,8 s de CPU. O custo é o PBKDF do LUKS2 num Cortex-A35, e é a
   razão pela qual o `data.mount` esperou 13 s por `/dev/mapper/med-data` sem que isso seja defeito.
2. **A asserção do `MedicalStorage` vale aqui**: o `dm/uuid` começa com `CRYPT-`. Com
   `MED_EEG_REQUIRE_ENCRYPTION = "true"`, o serviço de aquisição ter subido é, por si, a afirmação
   de que a criptografia é real.
3. **O `400` é a medição que justifica a mudança `bootfs` → `med-boot`.** O argumento registrado em
   `meta-med-bsp/conf/layer.conf` era que ext4 honra o `chmod 0400` que o vfat da ESP ignora. Estava
   escrito como raciocínio; agora é uma permissão lida do sistema de arquivos.
4. **A chave está no disco certo.** `med-boot` resolveu para `mmcblk0p8`, o cartão — se tivesse
   resolvido para o eMMC, a chave do volume de dados de paciente teria sido escrita num disco que
   não acompanha o dispositivo, e o sintoma só apareceria ao mover o cartão para outra placa.

Duas observações menores, registradas para não se perderem:

- **`/data` tem 496 MiB.** `1015808` setores × 512 B, que é a partição de 512 MiB do `.wks` menos o
  cabeçalho LUKS2 de 16 MiB. Num cartão de 14,8 GiB. Para o PoC serve; para a frase "o único meio
  persistente e sobrevivente a atualização do dispositivo", é pequeno, e a §9.7 explica por que os
  ~12 GiB restantes estão inacessíveis.
- **`EXT4-fs (mmcblk0p8): orphan cleanup on readonly fs`** ao montar a `med-boot`. Indica inodes
  órfãos pendentes, isto é, um desmonte não limpo em algum ponto anterior. Como esta é a partição
  onde a chave mora, o caminho de `umount` do `med-data-provision.sh` merece revisão.

### 9.5 RAUC: metade funciona, e a metade que falta falha exatamente onde estava previsto

```
$ rauc status
(rauc:1827): rauc-WARNING: Failed getting primary slot: uboot backend: fw_printenv failed with exit code: 1

=== System Info ===
Compatible:  med-os-stm32mp25-disco
Booted from: rootfs.0 (/dev/mmcblk0p9)

=== Bootloader ===
Activated: none

=== Slot States ===
o [rootfs.1] (/dev/disk/by-partlabel/med-root-b, ext4, inactive)
      bootname: B
      boot status: bad
o [rootfs.0] (/dev/disk/by-partlabel/med-root-a, ext4, booted)
      bootname: A
      boot status: bad

$ ls -l /etc/fw_env.config; fw_printenv
ls: /etc/fw_env.config: No such file or directory
Cannot initialize environment
```

**O que funciona**: os dois slots resolvem por rótulo GPT, o RAUC identifica corretamente de qual
bootou, e o `Compatible` bate com o que os bundles declaram — as três asserções `rauc-*` da suíte,
com a diferença de que `rauc-booted-partition` casa `/dev/mmcblk0p9` no lugar de `/dev/vda2`.

**O que falha**: o item 4 da lista "o que isso não significa" do `RESULTS.md` §8 dizia
*"achado por inspeção do manifest, não observado em execução"*. Agora está observado, com a
mensagem de erro.

E a consequência é maior que o aviso sugere. Sem conseguir ler o ambiente do U-Boot, o RAUC não tem
como saber o estado de boot de slot nenhum, e por segurança reporta **ambos como `bad` — inclusive
aquele de onde o dispositivo acabou de bootar** — com `Activated: none`. Na prática: um
`rauc install` escreveria o slot inativo e nunca conseguiria ativá-lo. O caminho A/B está bloqueado
na interface com o bootloader, precisamente onde os documentos diziam que estava.

A correção é um `/etc/fw_env.config` em `meta-med-bsp`, e a §9.3 já determinou a forma que ele
**não** pode ter: endereçar `u-boot-env` por `by-partlabel`, porque esse nome colide com o do eMMC
de fábrica. Tem de ser por PARTUUID.

### 9.6 O que continua falhando, e o que ainda não foi perguntado

**`weston.service` é a única unidade falha do sistema** (`systemctl list-units --state=failed`), com
`status=1/FAILURE` após 73 ms, derrubando `eeg-hmi.service` por dependência. Aqui isto **não** é o
caso previsto do QEMU, onde o `weston` está no `EXPECTED_FAILED` da suíte por não haver DRM/KMS sob
`nographic`: nesta placa o DRM subiu
(`[drm] Initialized stm 1.0.0 20170330 for 48010000.display-controller`) e o galcore carregou. A
suspeita de trabalho é incompatibilidade de `libgbm`/EGL entre o que o weston linkou em build e o
userland Vivante em runtime — exatamente a forma de falha que o comentário do `MED_GPU_PACKAGES`
antecipa ("uma build que passa e um compositor que falha em runtime"), pelo lado do userland em vez
do módulo. **Sem causa confirmada**; falta `journalctl -u weston -b -l`.

**O serviço de aquisição pode estar reiniciando.** O log de boot traz
`Started MedPlatform EEG acquisition service.` **duas vezes**. Pela lição que a injeção de falha
deixou — `systemctl is-active` dá PASS para um serviço em crash loop — isso é precisamente o
sintoma que não se pode aceitar sem olhar `NRestarts`. Com `MED_EEG_DRIVER = "rpmsg"` e nenhum
firmware no Cortex-M33, falha na abertura do canal é a hipótese óbvia. **Não verificado**:

```sh
systemctl show eeg-acquisition.service -p ActiveState -p NRestarts
```

### 9.7 Achados colaterais do primeiro boot

**O relógio, que é um problema regulatório e não cosmético.**

```
stm32_rtc 46000000.rtc: Date/Time must be initialized
systemd[1]: System time before build time, advancing clock.
```

A data observada durante toda a sessão foi `Thu 2025-05-29`, com a imagem construída em 2026-08. Sem
RTC com bateria e sem sincronização de rede antes de `/data` montar, **todo registro que o
`MedicalLogger` e o `MedicalStorage` escreverem carrega carimbo de tempo errado**. Para um argumento
de rastreabilidade IEC 62304 isso pesa mais que o weston: um registro de paciente com data errada é
pior que um registro ausente. Nada no repositório trata disso hoje.

**O Cortex-M33 está disponível, e exige firmware assinado.**

```
remoteproc remoteproc0: m0 is available
remoteproc remoteproc1: m33 is available
stm32-rproc 0.m33: Support of signed firmware only
stm32-rproc 0.m33: mbox_request_channel_byname() could not locate ...
remoteproc remoteproc1: cannot get detach mbox
```

O caminho AMP existe no hardware — é a primeira evidência disso. E a linha
`Support of signed firmware only` é uma restrição que o `implementation_plan_ads1299.md` **não**
contempla: o firmware do M33 que publicará os quadros do ADS1299 terá de ser assinado, o que
acrescenta uma cadeia de chaves ao plano. Os avisos de mailbox sugerem que o `detach` não está
configurado no devicetree desta variante; consequência não avaliada.

**O GPT de reserva está no lugar errado.**

```
GPT:Primary header thinks Alt. header is not at the end of the device
GPT:5394465 != 31116287
```

O `.wic` de 2,57 GiB foi gravado num cartão de 14,8 GiB, e o GPT secundário ficou no fim da imagem,
não no fim do cartão. Bootou, mas: o kernel reclama a cada boot, ~12 GiB ficam inacessíveis (é por
isso que `/data` são 496 MiB), e uma recuperação a partir do GPT primário danificado encontraria um
backup obsoleto no meio do disco. `sgdisk -e` no host corrige, e depois disso a `med-data` pode
crescer.

**Ruído do BSP, catalogado para não ser reinvestigado**: `No EFI system partition` e
`Failed to persist EFI variables` no U-Boot (placa sem ESP, esperado); `FWU metadata read failed`
(metadados de atualização do TF-A — não bloqueou o boot, mas não foi investigado); firmwares
ausentes de `brcmfmac`, Bluetooth e `imx335` (WiFi, BT e câmera não estão na imagem por escolha);
dezenas de `Fixed dependency cycle(s)` do devicetree da ST; `regulatory.db` ausente. Nada disso é
deste repositório.

**Auditoria do kernel desligada**: `systemd-journald: Collecting audit messages is disabled`. A
trilha de auditoria do MedOS é o journald com Forward Secure Sealing, que **validou na placa**
(`journalctl --verify` → `PASS`), então isto não invalida a §5 do `RESULTS.md` — mas convém saber
que o subsistema `audit` do kernel não está coletando.

---

### 9.8 Segunda sessão na placa: o `fw_env.config` validado, e o que ele destravou

Ainda 2026-08-18, com a placa ligada e o rootfs `rw` — o que permitiu testar o arquivo **antes** de
reconstruir a imagem, escrevendo-o à mão com o mesmo conteúdo que a receita gera.

**A colisão de nomes, medida.**

```
$ readlink -f /dev/disk/by-partlabel/u-boot-env
/dev/mmcblk2p5                       <- eMMC de fábrica
$ ls -l /dev/disk/by-partuuid/ | grep d7ba3548
d7ba3548-04a4-4269-aab6-913b1bc68d06 -> ../../mmcblk0p7   <- a nossa
```

O `fw_env.config.mmc` que a ST distribui endereça essa partição por *partlabel*. Usá-lo teria
apontado o `fw_setenv` para o ambiente de bootloader **de outro sistema operacional**, no disco
errado — e teria funcionado, em silêncio, até alguém se perguntar por que uma troca de slot nunca
surtia efeito. Isto não é mais um argumento sobre unicidade de nomes; é uma medição.

**O arquivo funciona.**

```
$ fw_setenv BOOT_ORDER "A B" && fw_printenv BOOT_ORDER
BOOT_ORDER=A B
```

Um round-trip exercita as três decisões de uma vez: o endereço por PARTUUID, o `ENV_SIZE` de
`0x2000`, e o formato redundante de duas entradas em `-0x2000`/`-0x4000` que diverge do arquivo da
ST. As duas primeiras eram indícios (o default do U-Boot e a própria declaração da ST); a terceira
era derivação a partir do `env/mmc.c`. As três passam.

**E o RAUC avançou uma etapa — a mensagem de erro mudou.**

```
antes:  Failed getting primary slot: uboot backend: fw_printenv failed with exit code: 1
agora:  Failed getting primary slot: uboot backend: Unable to find primary boot slot
```

Isso é progresso, não um segundo defeito: o RAUC passou a **ler** o ambiente e agora falha um passo
adiante. O backend `uboot` escolhe o slot primário percorrendo `BOOT_ORDER` e pegando o primeiro
cujo `BOOT_<bootname>_LEFT` seja maior que zero. `BOOT_A_LEFT` e `BOOT_B_LEFT` não existem — o
ambiente foi criado zerado pelo `.wks` e nunca semeado — então nenhum slot se qualifica, e é a mesma
ausência que faz os dois aparecerem como `boot status: bad`.

Semear é atribuição do `rauc status mark-good`, que o pacote `rauc-mark-good` (presente na imagem)
executa no boot. Ele não teve efeito no primeiro boot porque o `fw_setenv` falhava. Está por
verificar se passa a ter.

Vale registrar o que este teste mostra sobre método: o arquivo foi validado **sem reconstruir a
imagem**, porque o perfil de desenvolvimento monta o rootfs `rw`. Um rootfs imutável — que é o que
o `med-image-prod` terá — teria exigido um ciclo de build e regravação de cartão para descobrir a
mesma coisa. A imutabilidade é política correta para produto e é atrito puro no bring-up.

---

### 9.9 Terceira sessão na placa (2026-08-27): a aquisição medida e o RAUC destravado

Imagem reconstruída em 26/08 com a correção do `mincore` já dentro, cartão regravado com
`bmaptool`, GPT de reserva movido para o fim do cartão com `sgdisk -e`, e — pela primeira vez — um
**monitor HDMI ligado**. A metade gráfica desta sessão tem registro próprio — **`BRINGUP_HMI_STM32MP2.md`**, companheiro em
hardware do `BRINGUP_HMI_QEMU.md` —, com os quatro defeitos entre o compositor e a tela. Aqui ficam as duas outras coisas que a sessão mediu.

#### O serviço de aquisição: o laço existe, e tem número

A §9.6 registrava *"pode estar reiniciando — dois `Started` no log de boot, `NRestarts` nunca
consultado"*. Consultado:

```
$ systemctl show eeg-acquisition.service -p ActiveState -p NRestarts -p ExecMainStatus
NRestarts=163
ExecMainStatus=1
ActiveState=activating
```

E o journal nomeia o passo exato em que morre, sempre o mesmo:

```
eeg-acquisition-service[550]: EEG acquisition service starting
eeg-acquisition-service[550]: configuration verified
eeg-acquisition-service[550]: front-end self test failed
systemd[1]: eeg-acquisition.service: Main process exited, code=exited, status=1/FAILURE
```

A causa é a esperada e agora confirmada: `MED_EEG_DRIVER` é `rpmsg` neste alvo, o
`RpmsgDevice::selfTest()` é literalmente uma tentativa de abrir o canal (*"the link itself is the
thing under test"*), o Cortex-M33 não tem firmware, e `/dev/rpmsg0` não existe.

Os tempos, dos monotônicos:

| Medida | Valor |
|---|---|
| Período do laço | **2,500 s**, exatos (419,201 → 421,701 → 424,201 → 426,701) |
| Vida por tentativa | **~280 ms** (iniciado 419,228, morto 419,508) |
| Até o primeiro registro de auditoria | ~260 ms desde o `exec` |
| Total antes de ser parado à mão | 163 reinícios ≈ **6 min 48 s** |

**O achado que não é sobre o rpmsg**: nada parou o laço. Nenhuma das units declara
`StartLimitBurst`/`StartLimitIntervalSec`, então valem os defaults do systemd — 5 partidas em 10 s.
Com `RestartSec=2s` mais ~0,3 s de vida, a cadência de 2,5 s faz cinco partidas ocuparem ~12 s, um
pouco **fora** da janela. O limitador que parece proteger nunca dispara, a unidade **nunca chega ao
estado `failed`**, e `systemctl list-units --state=failed` não reporta nada — que é exatamente o que
a asserção `failed-units` da suíte consulta.

Um detalhe de acoplamento observado ao vivo: iniciar a HMI **ressuscita** o laço, porque
`eeg-hmi.service` declara `Wants=eeg-acquisition.service`. Comportamento correto da unit, efeito
previsível num alvo onde o serviço não pode subir.

#### RAUC: o `Activated: none` acabou

O `/etc/fw_env.config` vindo da receita foi lido na placa e confere com o que a §9.8 validou à mão.
`BOOT_ORDER=A B` sobreviveu desde 18/08 — a escrita no ambiente persiste através de regravação do
cartão, porque a partição `u-boot-env` não é tocada pela imagem. Mas:

```
$ fw_printenv BOOT_ORDER BOOT_A_LEFT BOOT_B_LEFT
BOOT_ORDER=A B
BOOT_A_LEFT=
BOOT_B_LEFT=
```

Os contadores continuavam vazios. **A causa é uma condição de unidade, e ela não falha — ela
desaparece**:

```
$ systemctl status rauc-mark-good.service
○ rauc-mark-good.service - RAUC Good-marking Service
     Active: inactive (dead)
  Condition: start condition unmet
             ├─ ConditionKernelCommandLine=|bootchooser.active was not met
             └─ ConditionKernelCommandLine=|rauc.slot was not met
```

A unidade do meta-rauc só roda se a linha de comando do kernel trouxer `rauc.slot=` ou
`bootchooser.active`. O `extlinux.conf` gerado pelo BSP da ST não passa nenhum dos dois, então o
systemd a pulou em **todos** os boots desde o começo, sem erro em lugar nenhum.

Isso **não é um defeito separado**: é a outra metade do mesmo buraco da seleção de slot. O script de
U-Boot que escolher o slot lendo `BOOT_ORDER` é também quem passa `rauc.slot=A` ao kernel — uma peça
resolve as duas.

O resto do caminho funciona. Executado à mão:

```
$ rauc status mark-good
rauc status: marked slot(s) rootfs.0 as good

$ fw_setenv BOOT_A_LEFT 3 && fw_setenv BOOT_B_LEFT 3
$ rauc status
=== Bootloader ===
Activated: rootfs.0 (A)

x [rootfs.0] (med-root-a, ext4, booted)   bootname: A   boot status: good
o [rootfs.1] (med-root-b, ext4, inactive) bootname: B   boot status: good
```

**`Activated: none` deixou de existir**, e o backend `uboot` resolve slot primário. É o maior avanço
do caminho de atualização desde o início do porte.

Duas ressalvas, porque isto é bancada e não conserto: semear `BOOT_A_LEFT` à mão é o que o
bootloader deveria fazer ao bootar, e `BOOT_B_LEFT=3` **afirma uma inverdade** — o slot B está
vazio, nunca foi escrito. Enquanto nada honra o `BOOT_ORDER`, é inofensivo; não deve virar estado
permanente.

O `rauc install` continua sem ter acontecido nesta placa. Agora está desbloqueado.

#### A terceira enumeração

```
Booted from: rootfs.0 (/dev/mmcblk2p9)
```

`mmcblk2`. No primeiro boot o cartão foi `mmcblk1`, no segundo `mmcblk0`, agora `mmcblk2` — **três
enumerações diferentes no mesmo hardware**, e nada quebrou, porque tudo que importa é endereçado por
rótulo `med-*` ou por PARTUUID. A regra 9 ganha a terceira observação, e a decisão de projeto que ela
motivou é validada pela terceira vez.

### 9.10 Quarta sessão na placa (2026-08-30): o primeiro `rauc install` em hardware

A §9.9 terminou com *"o `rauc install` continua sem ter acontecido nesta placa. Agora está
desbloqueado."* Aconteceu, e o desbloqueio não era o que aquela seção supunha.

#### O bloqueio real era o kernel, e o diagnóstico separou userspace de kernel

`rauc info` e `rauc install` leem o mesmo arquivo por dois caminhos diferentes, e essa diferença é
um teste pronto:

| comando | como abre o bundle | o que exercita |
|---|---|---|
| `rauc info` | `load_manifest_from_bundle()` → `unsquashfs()` em userspace (`src/bundle.c`) | assinatura, chaveiro, manifesto |
| `rauc install` | `mount_bundle()` → loop + alvo **dm-verity** via `ioctl(DM_TABLE_LOAD)` (`src/dm.c`) | tudo acima **mais o kernel** |

O `info` sempre passou. O `install` falharia — e a mensagem que ele emitiria nomeia a causa sem
ambiguidade: *"Failed to load dm table: … check DM_VERITY, DM_CRYPT or CRYPTO_AES kernel options."*

A causa é a mesma família do `WKS_FILE` da §2: **o default do vendor escondia uma política que nunca
escrevemos**. `med-bundle-eeg.bb` fixa `RAUC_BUNDLE_FORMAT = "verity"` porque o `system.conf`
declara `bundle-formats=-plain`, logo montar um bundle verity é requisito de *distro* — mas
`med-kernel-features.cfg` exigia `CONFIG_SQUASHFS` e nunca exigiu `CONFIG_DM_VERITY`. O defconfig do
`linux-yocto` traz `CONFIG_DM_VERITY=y`, o do `linux-stm32mp` não. O QEMU passava por herança e a
placa quebraria — e nenhum build tinha como dizer isso.

A correção é uma linha de política no lugar certo (`meta-med-distro`, bbappend curinga):

```
CONFIG_DM_VERITY=y
CONFIG_BLK_DEV_LOOP=y
```

Verificada no artefato, não no build verde — regra 6:

```
$ grep -E 'DM_VERITY|BLK_DEV_LOOP' build/.../deploy/images/stm32mp25-disco/kernel/config-6.6.129
CONFIG_BLK_DEV_LOOP=y
CONFIG_DM_VERITY=y

$ md5sum .../med-image-eeg/1.0/rootfs/boot/Image.gz-6.6.129 .../deploy/.../kernel/Image.gz--...bin
f8fb7f9bc2e1d064d76fcbb1b4148093  (idênticos)
```

O segundo comando não é redundante: `med-boot` vem de `${IMAGE_ROOTFS}/boot`, então o hash é o que
prova que o kernel com `DM_VERITY` é o que chega ao cartão, e não apenas o que foi compilado.

#### O estado de fábrica não existe: um `BOOT_ORDER` que ninguém escreve

Placa recém-gravada, antes de qualquer intervenção:

```
$ fw_printenv BOOT_ORDER BOOT_A_LEFT BOOT_B_LEFT
BOOT_ORDER=
BOOT_A_LEFT=
BOOT_B_LEFT=

$ rauc status mark-good
(rauc:708): rauc-WARNING **: Failed getting primary slot: uboot backend: Unable to find primary boot slot
rauc status: marked slot(s) rootfs.0 as good        ← diz que marcou

$ rauc status
Activated: none
o [rootfs.1] … boot status: bad
o [rootfs.0] … boot status: bad                     ← inclusive o slot em execução
```

O `mark-good` relata sucesso e nada muda. A leitura de `src/bootloaders/uboot.c` (rauc 1.15.2)
explica os dois fatos de uma vez:

```c
/* We assume bootstate to be good if slot is listed in 'BOOT_ORDER' and its
 * remaining attempts counter is > 0 */
gboolean r_uboot_get_state(RaucSlot *slot, gboolean *good, GError **error)
{
        if (!uboot_env_get("BOOT_ORDER", &order, &ierror)) { … }
        /* Scan boot order list for given slot */
        …
        if (!found) {
                *good = FALSE;
                return TRUE;      /* sai aqui — BOOT_A_LEFT nunca é lido */
        }
```

`BOOT_ORDER` vazio ⇒ nenhum bootname casa ⇒ **todo slot é `bad` sem que o contador chegue a ser
consultado**. `r_uboot_get_primary()` sai pelo mesmo ramo, e é ali que nasce literalmente a string
`"Unable to find primary boot slot"`. E `r_uboot_set_state(good=TRUE)` — o que o `mark-good` chama —
escreve **apenas** `BOOT_<bootname>_LEFT`. Ele não pode consertar isso por construção.

Quem escreve `BOOT_ORDER` no RAUC são `set_primary()` (durante um install) e `set_state(bad)`.
Ninguém no provisionamento. E o `.wks` cria a partição com `--source empty`.

**Consequência, e é um defeito da plataforma, não do teste**: um dispositivo saído do `bmaptool`
reporta todos os slots `bad` e nenhum primário — *para sempre*, até o primeiro install. Um operador
que consultasse o `rauc status` de um aparelho novo leria "ambos os slots ruins" num aparelho
perfeitamente saudável. O lugar definitivo do conserto é o script de U-Boot que fará a seleção de
slot: é ele quem semeia `BOOT_ORDER` e quem passa `rauc.slot=` na cmdline (§9.9). Uma peça, três
buracos.

##### Correção a uma afirmação da §9.9

A §9.9 escreveu que *"`BOOT_ORDER=A B` sobreviveu desde 18/08 — a escrita no ambiente persiste
através de regravação do cartão, porque a partição `u-boot-env` não é tocada pela imagem"*. **Não
sobreviveu à regravação de 28/08**, e a causa foi encontrada depois, lendo o ambiente padrão que a
`meta-st` publica:

```
env_check=if env info -p -d -q; then env save; fi
```

`env info -d` é verdadeiro quando o ambiente em uso **é** o default embutido — ou seja, quando o
armazenado falhou o CRC. O `bootcmd_stm32mp` roda `env_check` em todo boot, então a placa, ao
encontrar a partição zerada pela regravação, gravou ela mesma as 62 variáveis do default. Não foi
persistência: foi recuperação. As 62 entradas e o round-trip `MED_RT=hello` funcionando eram
consistentes com isso o tempo todo, e a leitura de "não está em branco, logo alguém preservou" era o
erro.

##### E qual `fw_printenv` está instalado muda o comportamento do RAUC

Detalhe que decide o caso e não está documentado em lugar nenhum. O `libubootenv`, que é o que esta
imagem instala, imprime a variável **mesmo quando ela não existe**, com status 0
(`src/fw_printenv.c`):

```c
fprintf(stdout, "%s=%s\n", argv[i], value ? value : "");
```

O `fw_printenv` do `u-boot-tools` faz o oposto: `## Error: "X" not defined` e status diferente de
zero. E `uboot_env_get()` do RAUC trata status ≠ 0 como erro — caminho em que `set_primary()` tem
fallback (`r_bootchooser_order_primary`). Com o `libubootenv`, o RAUC recebe `""` como valor
legítimo, nenhum fallback dispara, e "variável nunca inicializada" vira "slot ruim". Nenhuma das
duas implementações está errada; a diferença entre elas escolhe o comportamento.

#### O install

Semeado à mão antes (regra 12 — isto é medição, não conserto):

```
fw_setenv BOOT_ORDER "A B"
fw_setenv BOOT_A_LEFT 3
fw_setenv BOOT_B_LEFT 0     ← 0, e desta vez é verdade: o slot B estava vazio
```

O `BOOT_B_LEFT=0` é a correção da ressalva da §9.9, que semeou `3` e com isso afirmou que um slot
nunca escrito era bootável.

Bundle: `med-bundle-eeg-stm32mp25-disco.raucb`, 115.475.853 bytes, `Build: '20260828034740'`,
transferido por `scp` sobre IPv6 link-local para `/tmp` (tmpfs, 1,8 G).

| medida | valor |
|---|---|
| `rauc info` — assinatura | `Verified inline signature by 'O = MedPlatform, OU = Update Infrastructure, CN = MedPlatform Bundle Signing'` |
| `Compatible` do bundle × do dispositivo | `med-os-stm32mp25-disco` — iguais |
| Formato | `verity`, salt `7ca3ad0e…`, hash `9c0d0bbb…`, tamanho da árvore 909.312 B |
| Cadeia verificada contra o chaveiro do dispositivo | 2 certificados, com `check-crl=true` e `check-purpose=codesign` |
| `rauc install` | **`succeeded`**, sem uma única advertência |
| Slot escrito | `rootfs.1` (B) — o inativo, escolhido pelo RAUC a partir do slot bootado |
| Payload | 747.560.960 B numa partição de 1024 MiB |
| Ativação | `Activated: rootfs.1 (B)`, ambos os slots `good` |
| `BOOT_ORDER` depois | **`B A`** — B primeiro, **A preservado como fallback** |
| Contadores depois | `BOOT_A_LEFT=3`, `BOOT_B_LEFT=3` |

O `BOOT_ORDER=B A` é o resultado que importa para a alegação de reversibilidade: o RAUC não trocou o
slot ativo, ele **reordenou uma lista mantendo o anterior**. É a política A/B do IEC 62304 §5.8
visível numa variável de ambiente.

##### O slot escrito, conferido byte a byte

O install relatar sucesso não é evidência sobre o conteúdo do slot. O handler para `ext4→ext4` é
`img_to_fs_handler` = `write_image_to_dev()` cru, e `resize` não está declarado no nosso
`system.conf`, então os primeiros 747.560.960 bytes da partição têm de ser a imagem:

```
$ dd if=/dev/disk/by-partlabel/med-root-b bs=4096 count=182510 | sha256sum
4b07dae3c278ab0a92f2389660bdc31f7a089dc2d91cdafe6440d65062c9345c
```

Idêntico ao `Checksum` que o `rauc info` imprime para a imagem `rootfs`. O slot B contém exatamente
o payload do bundle.

(Primeira tentativa: `head -c 747560960 … | sha256sum` devolveu
`e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855`. O `head` do BusyBox não tem
`-c`; aquele valor é o SHA-256 da string vazia. **Vale reconhecê-lo de vista: é o que aparece toda
vez que uma medição não mediu nada.**)

#### A quarta enumeração

`Booted from: rootfs.0 (/dev/mmcblk0p9)`, e `by-partuuid/d7ba3548-…` → `/dev/mmcblk0p7`. Quatro
boots, quatro enumerações: `mmcblk1`, `mmcblk0`, `mmcblk2`, `mmcblk0`. A regra 9 ganha a quarta
observação — e desta vez ela foi *usada*, não só constatada: o `fw_env.config` por PARTUUID mandou o
`fw_setenv` para o cartão (`mmcblk0p7`) e não para o eMMC de fábrica, que carrega uma partição
`u-boot-env` homônima.

#### Achado no host: `make verify-bundle` estava morto havia dois commits

`make verify-bundle BOARD=stm32` falhou com `no rauc-native - run 'make bundle' first` logo depois de
um `make bundle` bem-sucedido. O `RAUC_NATIVE` apontava para o `recipe-sysroot-native` **dentro do
`WORKDIR` da receita do bundle**, e o `local.conf` herda `rm_work` desde o commit `e18bce4`
("build: stop the build tree from eating the disk"), que apaga o `WORKDIR` assim que a receita
termina. O caminho só era válido na janela entre `do_bundle` e `rm_work`.

Corrigido para `tmp-glibc/sysroots-components/*/rauc-native/usr/bin/rauc`, que é o que o sstate
instala e o `rm_work` não toca — e que, sendo por arquitetura de *build*, dispensa a variável de
máquina que a versão anterior carregava.

O que fica registrado não é o caminho: é que **uma medida de economia de disco desativou uma
verificação do caminho de atualização, em silêncio, e ninguém notou por dois commits** — a mesma
forma dos cinco defeitos da §8.3 do plano do RAUC.

#### O slot escrito boota — e o que isso revelou

O `sha256sum` prova os bytes, não a bootabilidade. Editando à mão o `root=PARTUUID=` do
`extlinux.conf` para o slot B e reiniciando:

```
$ cat /proc/cmdline
root=PARTUUID=2997c20d-239f-43cd-8658-9c7010186c9b rootwait rw   earlycon console=ttySTM0,115200

$ rauc status
Booted from: rootfs.1 (/dev/mmcblk0p10)
Activated: rootfs.1 (B)
```

O slot escrito pelo RAUC sobe até multi-user, abre o `/data` e se reconhece corretamente. **O ensaio
completo, os quatro achados que ele produziu e a especificação da peça que falta estão em
`BOOT_SLOT_AB_STM32MP2.md`** — este é o documento a ler antes de escrever o script de U-Boot.

O mais grave dos quatro merece estar aqui também, porque não é sobre boot: **cada slot tem uma
identidade de máquina diferente**. O rootfs não traz chave de host SSH nem `machine-id`
(`/etc/machine-id` tem 0 bytes), então `sshdgenkeys.service` e `systemd-machine-id-commit.service`
geram esse material por slot, no primeiro boot de cada um. Foi o SSH que denunciou, com o aviso de
identificação alterada, na mesma placa e no mesmo endereço. E como `Storage=persistent` grava o
journal em `/var/log/journal/<machine-id>/`, **a trilha de auditoria mora dentro de um slot A/B** —
a mesma armadilha que o plano do LUKS já havia resolvido para a chave do `/data`, repetida para a
evidência de rastreabilidade.

#### Os limites deste resultado

1. **Não é a troca de slot.** O `extlinux.conf` continua fixando `root=PARTUUID=e91c4e10-…` (slot A).
   `BOOT_ORDER=B A` não tem leitor. Um reboot boota A, e isso não é falha do RAUC.
2. **O `BOOT_ORDER` inicial foi semeado à mão.** Sem isso o install ainda ocorreria — ele deriva o
   alvo do slot bootado, não do primário —, mas o estado antes e depois não seria comparável.
3. **O que foi medido é escrita + ativação.** A frase segue sendo "a política A/B está validada no
   QEMU, a integração com o bootloader em hardware"; o que mudou é que "em hardware" agora cobre
   verificar o bundle, escrever o slot inativo e reordenar o ambiente do bootloader, o que antes era
   zero.

### 9.11 Quinta sessão (2026-08-31): o dispositivo escolhe o slot sozinho

Cartão gravado com a imagem que traz o ambiente do U-Boot construído — `med-uboot-env-image`, um
binário de 512 KiB que o wic escreve em `u-boot-env` por `rawcopy`, com o `bootcmd` A/B, o
`BOOT_ORDER` de fábrica e o `bootcmd` do próprio vendor preservado como `med_rescue`. Primeiro boot,
**nenhum comando manual antes**:

```
$ cat /proc/cmdline
root=PARTUUID=e91c4e10-... rootwait rw earlycon console=ttySTM0,115200 rauc.slot=A

$ rauc status
Booted from: rootfs.0 (A)      Activated: rootfs.0 (A)

$ systemctl status rauc-mark-good.service
Active: active (exited)   ExecStart=/usr/bin/rauc status mark-good (status=0/SUCCESS)

$ fw_printenv BOOT_ORDER BOOT_A_LEFT BOOT_B_LEFT
BOOT_ORDER=A B   BOOT_A_LEFT=3   BOOT_B_LEFT=0
```

O `rauc.slot=A` é o discriminador desta medição: sem ele o boot teria vindo do `med_rescue` e
pareceria idêntico em todo o resto. A regra 11 deste documento nasceu do `rauc-mark-good` que nunca
rodava por `Condition*` não satisfeita; ele agora roda. E o `BOOT_A_LEFT=3` lido em userspace, depois
de o `med_select` ter decrementado para 2 e gravado antes de carregar o kernel, é a evidência de que
o ciclo bootloader→userspace fecha.

Detalhe que mudou sozinho e vale notar: o RAUC passou a dizer `Booted from: rootfs.0 (A)` em vez de
`rootfs.0 (/dev/mmcblk0p9)`. Ele identifica o slot pelo `rauc.slot=` que o bootloader declara, em vez
de inferi-lo do dispositivo de root — a regra 9 resolvida por construção, em vez de contornada.

Duas correções de bancada que esta sessão produziu, ambas sobre *como verificar* e não sobre o
produto:

- **O enlace USB do leitor falhou** com `Sense Key: Aborted Command` /
  `Add. Sense: Data phase CRC error` — transporte, não mídia; um cartão morrendo diria
  `Medium Error`. E como o USB tem CRC na camada de dados, corrupção silenciosa é improvável: o erro
  *é* o CRC funcionando.
- **Conferir uma gravação por hash cru de partição só vale onde o `.bmap` cobre 100%.** O
  `u-boot-env` tem 129 de 129 blocos mapeados e bateu; a `med-boot` tem 4047 de 16385 (24,7%) e não
  podia bater, porque o `bmaptool` não escreve o resto e a mídia conserva o que havia. **Uma
  verificação que não pode passar é pior que nenhuma**: produz alarme falso e consome a atenção que
  o alarme verdadeiro precisaria. Para sistemas de arquivos, compare os arquivos.

#### E a troca, no mesmo dia

Bundle reconstruído da imagem que a placa roda (`Build: '20260831034731'`), instalado, reiniciado:

```
depois do install, ainda em A:   Activated: rootfs.1 (B)   BOOT_ORDER=B A   BOOT_A_LEFT=3  BOOT_B_LEFT=3
                                 sha256 do slot B = bb6cc1e7… = Checksum do manifesto

depois do reboot:                root=PARTUUID=35822773-… rauc.slot=B
                                 Booted from: rootfs.1 (B)   A: inactive, good
```

A cadeia inteira, ponta a ponta: o RAUC escolheu o slot inativo, escreveu, verificou e reordenou o
ambiente; o U-Boot leu `BOOT_ORDER`, gastou uma tentativa, montou o `root=` e declarou `rauc.slot=B`;
o kernel montou o slot que o bootloader escolheu; o `mark-good` restaurou o contador de dentro do
slot novo. **`BOOT_A_LEFT=3` intocado** é a outra metade: o slot anterior segue elegível, que é o que
faz a atualização ser reversível em vez de destrutiva.

De quebra, o §5.1 do `BOOT_SLOT_AB_STM32MP2.md` deixou de ser observação de laboratório: a
fingerprint SSH mudou de `SHA256:lAW0DlTk…` para `SHA256:25lyKZU9…` **numa atualização de verdade**,
que é o cenário em que aquele defeito importa — um aparelho em campo troca de identidade ao se
atualizar.

O que **não** foi medido: o fallback por injeção de falha, e o pulo de um slot com contador zerado.
Ver `implementation_plan_uboot_ab.md` §8, passos 5 e 6.

---

### 9.12 Sexta sessão (2026-10-04): o front-end real na placa, e o fallback medido

A ponte USB-SPI e o conversor da placa de AFE, ligados numa porta USB da DK. O registro completo
está em `BRINGUP_AFE.md` §6.4 e, para o A/B, em `implementation_plan_uboot_ab.md` §8, "passo 6".
Aqui fica o que a sessão ensinou sobre **esta placa e o seu caminho de atualização**.

- **A placa estava acessível por IPv4** (`192.168.1.11`, DHCP, interface `end0`), ao contrário do
  que registra o `BRINGUP_HMI_STM32MP2.md` §11. O IPv6 link-local continua sendo o endereço
  estável, porque sai do MAC; o IPv4 pode mudar a cada boot.
- **Um kernel novo exige regravar o cartão.** A placa rodava o kernel de fábrica da ST, sem os
  drivers do front-end. O kernel mora na `med-boot`, que os dois slots compartilham e que nenhum
  bundle troca (§5.4 do `BOOT_SLOT_AB_STM32MP2.md`). Daí em diante, tudo o que mudou no rootfs foi
  por OTA: três `rauc install` na sessão.
- **`make bundle` ignorava `KERNEL=med`**, e teria empacotado módulos de outro kernel numa instalação
  assinada, verificada e bem-sucedida. Corrigido no `Makefile`; o bundle foi conferido por dentro
  (`/lib/modules/6.6.129-gb8dbcb083402`).
- **Itens 19 e 20 da tabela do topo.** O 20 é o mais grave da sessão e não tinha sintoma: o slot
  era confirmado sem que nada da aplicação tivesse rodado. Com a correção, a confirmação vem aos
  13,64 s, depois do aviso de prontidão do serviço aos 12,53 s.
- **O fallback, pela primeira vez.** A falha injetada foi uma regra udev em `/etc` sem o link de
  `/run`, para que o boot fosse inteiro e só a aquisição falhasse. Resultado: `BOOT_A_LEFT` 2, 1,
  0, nenhuma `boot-complete.target`, e o boot seguinte em `rauc.slot=B`, sem intervenção.
  Recuperado reinstalando o slot A.
- **A chave de host SSH mudou a cada troca de slot** (item 17), quatro vezes nesta sessão.

- **O governador de frequência custava 0,7% das amostras** (`BRINGUP_AFE.md` §6.5): o `schedutil`
  alternava o A35 entre 1,2 e 1,5 GHz 86 vezes por segundo. Fixado em `performance` por
  `med-cpufreq-policy`, uma receita do `meta-med-bsp` com uma linha de tmpfiles. Ficou no rootfs,
  e não no fragmento de kernel, para que chegue por OTA.
- **Desplugar a ponte com o driver carregado** não afeta o kernel do 6.6, mas encerrava a aquisição
  para sempre: o serviço saía com 0 e o `Restart=on-failure` não reiniciava. Corrigido e validado
  com a mesma falha: a aquisição volta 1,7 s depois da ponte (`BRINGUP_AFE.md` §6.6).

**Estado ao fim da sessão**: slot B (quarto `rauc install` do dia), `BOOT_B_LEFT=3`, governador
`performance`, `eeg-acquisition` adquirindo pelo link `usb`. As ~60 sessões vazias deixadas pelo
laço do item 19 continuam no `/data`.

### 9.13 Sétima sessão (2026-10-06): o M33 carrega firmware assinado, e recusa o adulterado

Fases 0 e 1 do `implementation_plan_m33_firmware.md`, numa sessão só e sem regravar o cartão: a
imagem era a da §9.12 (kernel `6.6.129-gb8dbcb083402`, slot A, `MED_EEG_LINK=usb`). Acesso por SSH
no IPv6 link-local. Nada foi escrito no cartão: o firmware ficou em `/run/med-m33/`, e o
`firmware_class.path` apontou para lá até o próximo boot.

**A placa de AFE passou para o header de 40 pinos**, sem ponte, para a ligação `amp`. A fiação é
fato desta placa e é a que o firmware do M33 vai ter de usar:

| Sinal | Função do pino | Porta | Pino do header |
|---|---|---|---|
| MOSI | `SPI6_MOSI` | PC7 | 19 |
| MISO | `SPI6_MISO` | PC4 | 21 |
| SCK | `SPI6_SCK` | PF7 | 23 |
| CS | `SPI6_NSS` | PF4 | 24 |
| PWDN | `TIM8_CH4` | PC10 | 29 |
| DRDY | `TIM5_CH1` | PH8 | 31 |

RESET, START e CLKSEL não chegam ao header; **ainda não conferido** se estão fixos na placa de AFE e
em que nível. Como o SPI6 continua `disabled` no devicetree do Linux, nada fala com o conversor
nesta sessão: o que se mediu foi o mecanismo de carga do coprocessador, não o front-end.

**Fase 0: o que a placa diz antes de qualquer firmware.**

```
remoteproc1  name=m33  state=offline  fw_format=TEE
stm32-rproc 0.m33: Support of signed firmware only
/dev/tee0, e a TA 80a4c275-0a47-4905-8285-1486a9771a08 registrada no barramento tee
/lib/firmware: não existe (e o rootfs é somente leitura)
```

**Fase 1: o firmware da ST, com zero linhas nossas.** `bitbake m33projects-stm32mp2` no container
(2328 tarefas, 2167 do sstate). O único projeto da DK que a receita compila é o
`USBPD_DRP_UCSI`, que é o firmware padrão da ST para esta placa. O `OpenAMP_TTY_echo` da DK existe no
STM32CubeMP2, mas só como projeto do CubeIDE, sem `CMakeLists.txt`, e a receita pula projeto sem
CMake. A camada assina sozinha (`sign_copro`, `m33projects.inc`) com a chave de desenvolvimento em
que este OP-TEE confia. O `_sign.bin` começa com `0x3543A468`, o magic do formato de imagem de
remoteproc do OP-TEE.

| Ensaio | Imagem | Resultado |
|---|---|---|
| Carga | `USBPD_DRP_UCSI_CM33_NonSecure_sign.bin`, 148.892 B, sha256 `648b3859…` | `running`; `rpmsg host is online`; canais `rpmsg-intc` (0x400) e `rpmsg_i2c` (0x401); aparece `/dev/rpmsg_ctrl0` |
| Parada | a mesma | `offline`; `/sys/bus/rpmsg/devices` vazio, nenhum `/dev/rpmsg*` |
| Injeção 1 | um bit trocado no offset 0x10000 (payload) | **recusada**: `TA_RPROC_FW_CMD_LOAD_FW` → `0xffff000f` (`TEE_ERROR_SECURITY`), `Boot failed: -5` |
| Injeção 2 | um bit trocado no offset 0x100 (região da assinatura) | **recusada**: `0xffff3072` (`TEE_ERROR_SIGNATURE_INVALID`), `Boot failed: -5` |
| Controle | a imagem boa de novo, depois das duas recusas | `running` |

O que isso fecha: **a Estrada A existe nesta placa e é a escolhida.** "Support of signed firmware
only" não é obstáculo, é a propriedade que se quer defender, e custou zero chaves novas e zero
mudanças de devicetree. A Estrada B não foi testada e não precisa ser.

O que isso **não** prova: que o *nosso* firmware vai carregar. A chave que assinou é a de
desenvolvimento da ST, e uma receita nossa tem de chegar à mesma assinatura pelo mesmo caminho
(`m33fw-utils-stm32mp.bbclass`). Também não prova nada sobre o SPI6, cuja atribuição RIF ao M33
continua sem arquivo conhecido (`implementation_plan_ads1299.md` §11.4), nem sobre os GPIOs PF4,
PC10 e PH8, que precisam da mesma atribuição.

**A primeira tentativa de injeção não injetou nada**, e isso merece mais destaque que o resultado.
O byte foi trocado na placa com `od -An` → `printf` → `dd`. O `od` do busybox não aceita `-An`, a
troca nunca aconteceu, o "firmware corrompido" era idêntico ao bom, e subiu `running` duas vezes.
Lido sem conferir, o registro teria sido "o OP-TEE aceita firmware adulterado", o que é falso e
grave. Quem denunciou foi o `cmp -l`, que não imprimiu nada. A repetição gerou os arquivos no host,
conferiu a diferença com `cmp -l` **dos dois lados** (65537: 117→116; 257: 220→221), e só então
carregou. É a regra do `CLAUDE.md` em outra roupa: uma injeção que nunca viu a falha que procura é
alegação, e "nunca viu" inclui "nunca chegou a acontecer".

Três observações para as próximas fases:

- **Nenhum canal se chama `rpmsg-raw`**, então nenhum `/dev/rpmsgN` aparece sozinho. O
  `/dev/rpmsg_ctrl0` existe, o que confirma no kernel da placa o caminho de `RPMSG_CREATE_EPT_IOCTL`
  da §6.1 do plano.
- **O firmware da ST é o gerente de USB-PD da porta USB-C DRP.** O `rpmsg_i2c` é um I²C virtual que
  o driver `ucsi-stm32g0-i2c` do Linux usa; a cada parada ele registra `i2c write 35, 08 error:
  -110`. É inofensivo aqui, mas um firmware nosso no lugar deste deixa essa porta sem gerente de PD,
  e isso tem de ser dito, não descoberto.
- **O serviço de aquisição estava em laço** (`NRestarts=129`, `front-end self test failed`): a
  imagem é `usb` e a ponte não está mais na placa. Esperado com essa imagem e sem relação com o M33,
  mas a mensagem é a de autoteste reprovado e não a de dispositivo ausente, o que **não foi
  investigado**.

**Estado ao fim da sessão**: M33 `offline`, nenhum firmware no cartão, `firmware_class.path` em
`/run/med-m33` até o próximo boot.

### 9.14 Oitava sessão (2026-10-07): o nosso firmware no M33, e o serviço adquirindo por `amp`

Fase 3 do `implementation_plan_m33_firmware.md`. **O sinal é sintético. O ADS1299 não foi tocado**:
está no header (§9.13), mas o SPI6 continua `disabled` no Linux e sem atribuição RIF ao M33, e o
firmware não tem driver do conversor. O que esta sessão validou é o **caminho** M33 → rpmsg → Linux →
serviço → `/data`, com um produtor que gera, por fórmula, 10 Hz de 20 µV mais 50 Hz de 5 µV. Nada
aqui é medida de eletrofisiologia nem do front-end.

O firmware é `med-m33-firmware` (camada adjunta, `dynamic-layers/stm-st-stm32mp/recipes-firmware/`):
bare metal, um endpoint `rpmsg-raw`, a amostra datada na interrupção do SysTick, assinado pela mesma
`sign_copro_fw_m33` da ST. O formato vem da receita nova `med-amp-abi` (§5.1 do plano). Tudo em
`/run`, nada gravado no cartão.

**Quatro defeitos no caminho até o primeiro quadro**, nenhum visível no build:

1. **`Trace not available`.** O buffer de trace da ST fica em `.bss`, na RAM do M33 (`0x80a00000`),
   e com o OP-TEE carregando o firmware o Linux mapeia só os carveouts listados
   (`carveout_memories`: `ipc-shmem-1`, as vrings, `cm33-sram2`). O buffer foi para uma seção
   `.resource_table.trace`, que o `.ld` da ST já põe em `ipc-shmem-1`. Sem isso, o firmware era mudo.
2. **A FPU desligada.** O `SystemInit()` não-seguro da ST não escreve `SCB->CPACR`, e o firmware é
   *hard-float*. O sintoma foi o mais enganoso da sessão: canal anunciado, a prescrição chegou
   (`message from 0x400, 464 bytes` no trace), e depois silêncio para sempre, porque o parser da
   prescrição é o primeiro código com `double`, e a UsageFault caía num handler que girava calado.
   Corrigido no `main()`, com o `CPACR` relido no trace (TrustZone pode vetar pelo NSACR). Os
   handlers de falha agora escrevem CFSR/HFSR/PC no trace antes de parar.
3. **O `mbox_ipcc.c` da ST zera a flag de mensagem depois de processar a vring.** Uma interrupção
   que chega durante o processamento é apagada. Corrigido por leitura, antes de qualquer sintoma: a
   flag é zerada antes, e é `volatile`. Não foi observado em falha.
4. **O `HAL_InitTick()` não é refeito depois do `SystemCoreClockUpdate()`** no exemplo da ST.
   Corrigido por leitura. O trace confirma o clock: `core clock 400000000 Hz`.

**A bancada** (`scripts/amp-bench.sh` na placa, `scripts/amp-bench.py` no host). O host escreve a
`ControlMessage` e julga a captura; a placa só tem busybox. O analisador recalcula **cada amostra**
pela especificação do sinal, em precisão dupla, com o CRC do `zlib`, que é uma fonte de fora do
repositório.

| Captura | Resultado |
|---|---|
| 120 s, prescrição aceitável (lead-off e bias `false`) | ack primeiro; 2.143 quadros, **0 lacunas, 0 CRC errado**; 240.016 amostras a ≤ 0,52 nV do especificado; **249,975 amostras/s/canal pelo relógio do Linux**; 250,0000 pelo do M33, deltas de 55.999 a 56.001 µs |
| sinal de teste interno, 300 quadros | quadrada de ±1.875.000 nV, período 256 amostras, erro 0,00 nV |
| a prescrição que o `eeg.conf` envia (lead-off e bias `true`) | só o ack: `rejectedIndex 1`, *"afe.bias_drive: synthetic producer, no electrodes"*; nenhum quadro |

**O serviço real, pelo link `amp`**, com o binário instalado e um `eeg.conf` de bancada em `/run`
(`--config`, selado com o `.sha256`; lead-off e bias `false`): `front-end self test passed`,
`acquisition session started`, `metadata.json` com `"driver": "rpmsg"` e `"link": "amp"`, e um
`raw.bin` de 69.296 B = **142 × 488 B**, que é o `MED_FRAMES=142` do journal.

**Dois defeitos do lado Linux, previstos lendo o `rpmsg_char` do 6.6 e depois observados:**

- **Quadros velhos no lugar do ack.** No canal `rpmsg-raw`, `rpmsg_ept_cb` enfileira toda mensagem,
  mesmo com o `/dev/rpmsgN` fechado; o `release` esvazia a fila no fechamento, mas o `open` não. Na
  bancada: abrir, prescrever, fechar, reabrir 1 s depois, e a primeira mensagem lida tinha **488
  bytes**, um quadro, e não os 72 do ack. No serviço, sem mudar código, deixando quadros na fila
  antes de iniciá-lo: `self test failed`, *"the front-end answered the control message with
  something else"*, contra um produtor saudável. **Sem a injeção o serviço passava**, porque reabre
  o canal antes dos 56 ms do primeiro quadro: um defeito latente, escondido por uma corrida.
  Corrigido no framework (`sendControl` descarta quadros até o ack, sob um prazo único), e a mesma
  injeção passou: `self test passed`, 142 quadros.
- **Fila sem limite no kernel.** Com o leitor fechado por 20 s, 357 mensagens já estavam na fila,
  lidas em 10 ms, e o `Slab` cresceu 456 KB (~23 KB/s, ~2 GB/dia com o serviço parado e o firmware
  transmitindo). **Não corrigido**: exige uma mensagem de parada no protocolo, e é decisão a tomar.
  A bancada para o M33 ao fim de cada captura por causa disso.

E duas armadilhas da bancada que valem registro. O `od -An` do busybox voltou a falhar (é a mesma
da §9.13); os tamanhos das mensagens bastaram para a conclusão, mas o comando estava errado de
novo. E o `/run/medplatform` só existe enquanto a unidade roda (`RuntimeDirectory=`), então o
serviço rodado à mão falha no `bind` até o diretório ser criado.

**O que isto não significa.** Nada sobre o ADS1299, nem sobre o SPI6 com o M33. Também não
significa que um cartão novo adquire sozinho: o firmware não está na imagem, e a prescrição do
`eeg.conf` é recusada por um produtor sintético. Por fim, o carimbo é o relógio do M33 desde a
subida, que o framework lê como tempo Unix, e por isso a sessão aparece datada perto de 1970 no
fluxo (`fromUnixMicros`).

**Estado ao fim da sessão**: M33 `offline`; `eeg-acquisition.service` **parado** à mão (estava em
laço pelo link `usb` sem ponte, `NRestarts=2031`) e volta no próximo boot; biblioteca corrigida
só em `/run/med-bench/lib`.

### 9.15 Nona sessão (2026-10-07): o RIF do SPI6 no M33, e o ADS1299 responde ao M33

**O marco: pelo SPI6, a partir do Cortex-M33, o registrador de ID do ADS1299 respondeu `0x3E`**
(família `111`, campo de canais `10` = 8 canais, revisão 1), o mesmo byte que a ponte USB leu em
04/10. O DRDY leu 1, o repouso correto com o conversor em SDATAC. É o primeiro contato do link `amp`
com o conversor real. **Ainda não é aquisição**: nenhuma conversão, nenhum DRDY como interrupção,
e o fluxo de quadros continua sintético.

**A atribuição** (`meta-med-afe-ads1299`, `external-dt_%.bbappend`, aplicada só com
`MED_EEG_LINK = "amp"`): oito recursos do devicetree do OP-TEE passaram de CID1 a CID2,
não-seguros e privilegiados. São o SPI6 (RIFSC 27), PC4, PC7, PC10, PF4, PF7, PH8 e a linha EXTI1 8.
Antes de escrever, conferido na placa: nenhum dos seis pinos é reivindicado pelo Linux (pinctrl
`UNCLAIMED`), e as interrupções de GPIO dele usam as linhas EXTI 3, 4 e 5. O clock de kernel do SPI6
(`FLEXGEN_18`, da PLL5) já é configurado pelo OP-TEE, e o gate segue o RIFSC 27
(`clk-stm32mp25.c`: `SEC_RIFSC(27)` para `ck_icn_p_spi6` e `ck_ker_spi6`).

**Conferido no artefato, não na fonte.** O DTB do OP-TEE foi extraído de dentro do FIP e o
`st,protreg` decodificado (`PER_ID` 0–7, `SCID` a partir do bit 12, `SEC` 16, `PRIV` 17, da macro
`RIFPROT` do patch da ST). As 8 entradas estão em SCID=2 no FIP novo e em SCID=1 no FIP que estava no
cartão. Duas entradas de controle, que não mudam (GPIOH 4: CID1, seguro; SPI3: sem CID), saem iguais
nos dois, então o decodificador distingue. No `diff` do DTB inteiro, as únicas linhas diferentes são
as cinco `st,protreg` tocadas, cada uma só nas células previstas (`0x21…` → `0x22…`), e os outros
três DTBs embutidos (TF-A, U-Boot) são idênticos byte a byte. **Não comparado**: os binários de código
do FIP, que foram reconstruídos.

**A gravação, e o que ela ensinou sobre endereçar partições.** O FIP vai só por cartão: nenhum bundle
RAUC o atualiza. Três achados antes de gravar:

1. **O PARTUUID das partições FIP colide entre discos.** `by-partuuid` da `fip-a` dava `mmcblk0p5`
   (o cartão) e o da `fip-b` dava `mmcblk1p4`, a eMMC de fábrica. Os UUIDs que o nosso `.wks` fixa
   para as FIPs são os do layout de exemplo da ST, gravado também na eMMC. A regra da §9.3
   ("endereçar por PARTUUID, nunca por partlabel") **não basta para essas partições**. A gravação
   identificou o disco pela partição que é só nossa (`med-root-a`, a do `root=` em uso) e, dentro
   dele, a `fip-a` pelo `PARTNAME` do `uevent`. A correção de raiz, UUIDs próprios no `.wks`, não foi
   feita.
2. **O banco ativo é o 0.** Metadados FWU v2 iguais nas duas cópias: `active_index 0`,
   `previous_active_index 1`, os dois bancos aceitos (`0xfc`). A `fip-b` do cartão difere da `fip-a`
   (o RIF é o mesmo), o que o `.wks`, que grava o mesmo arquivo nas duas, não explica. **Não
   investigado.**
3. **O `dd` do busybox não aceita `conv=fsync`.** A primeira tentativa não gravou nada, e o
   `| tail -1` engoliu o erro. Quem pegou foi a releitura com sha256 depois da escrita; a `fip-a`
   foi conferida idêntica ao backup antes de repetir. A segunda gravou, e a releitura bateu
   (`13446233…`).

Backup do FIP anterior em `/data/med-bench/` e no host em `build/med-bench-backups/`.

**O teste negativo, com o FIP antigo.** A sondagem (`afe_probe.c`) pergunta ao RIF por cada
recurso com o `ResMgr_Request()` da ST antes de tocar nele. Com o FIP antigo: sete recusas
`SCID != CPU2`, *"front-end NOT probed"*, nenhum acesso ao hardware, e o fluxo sintético seguiu
(1 ack + 30 quadros). Com o FIP novo: *"all belong to this core"*.

**O reset do SoC inteiro.** A primeira sondagem com o FIP novo **reiniciou a placa toda**, e não só o
M33: o journal do boot anterior termina sem nem a linha "powering up m33", e o trace se perdeu com a
DDR. Sem console serial e sem `devmem` (o `/dev/mem` não devolve a região), a causa foi isolada por
eliminação. A sondagem escrevia `__HAL_RCC_GPIOx_CLK_ENABLE()` para os bancos C, F e H, que são
registros do RCC do lado seguro: o driver do Linux nem os controla (`CS_GATE` só de resumo). Tirar
**só** essas três escritas eliminou o reset, e o boot id ficou igual antes e depois. Mecanismo provável:
acesso ilegal reportado pelo RIF ao mundo seguro, que reinicia. Isso é **inferência**: a mensagem do
OP-TEE iria para o console serial, que não está ligado. Os bancos já têm clock porque o Linux usa outros
pinos deles.

**O conversor sem alimentação.** Com o RIF certo e sem reset, a primeira leitura deu `0xFF` e DRDY 0,
e a sondagem recusou (*"not a converter of this family"*). A placa do AFE estava ligada ao header só
pelos sinais; AVDD (5 V), DVDD (3,3 V) e GND vinham da ponte USB nas sessões anteriores. Com 5 V,
3,3 V e GND do header ligados, o mesmo firmware leu `0x3E`. É o controle negativo natural da leitura:
uma linha MISO solta dá `0xFF`, e o teste de família a recusa.

**Três armadilhas da bancada**, registradas porque cada uma custou uma rodada:

- **A numeração do remoteproc trocou num boot** (o M33 virou `remoteproc0` e o M0 `remoteproc1`).
  Um comando avulso com `remoteproc1` fixo mandou o firmware do M33 para o M0, que recusou com
  *"Image is corrupted (bad magic)"*. O `amp-bench.sh` procura pelo nome; os comandos de diagnóstico
  agora também. É a regra "um nome de dispositivo é uma corrida", desta vez com um remoteproc.
- **O trace de 1.048 bytes perdia o início.** O Linux lê o trace até o primeiro NUL, e o anel da ST
  escreve um NUL depois da posição atual, então uma volta no anel esconde tudo o que é mais antigo.
  Agora o buffer tem 4 KiB. A primeira tentativa de mudar o tamanho não pegou por causa do CRLF do
  arquivo da ST.
- **O `head -N` do busybox** (só `head -n N`) quebrou dois comandos de diagnóstico.

O `weston.service` falha neste boot, mas também falhava no de 04/10, antes de qualquer mudança no
FIP. É a falha conhecida do HMI (§9.9 e `BRINGUP_HMI_STM32MP2.md`), e não regressão do RIF.

**O que isto não significa.** Que o M33 adquire do conversor: falta o DRDY como interrupção (EXTI1 8,
já atribuída), o `RDATAC`, o quadro de 27 bytes por amostra e o fim do gerador sintético no
produtor. Nada sobre o SPI6 em velocidade: a leitura foi com o prescaler mais lento, que dá algumas
centenas de kHz. E a imagem no cartão ainda é a do link `usb`. Só o FIP é novo; a imagem `amp`
(firmware em `/lib/firmware`, `eeg.conf` com `rpmsg`) não foi gravada.

**Estado ao fim da sessão**: FIP novo na `fip-a`; M33 `offline`; placa do AFE alimentada pelo header.

### 9.16 Décima sessão (2026-10-07 e 2026-10-08): o M33 adquire do ADS1299

**O marco: o M33 adquiriu do conversor por 120 s, com DRDY por interrupção, `RDATAC` e quadros
reais.** 2.143 quadros, **0 lacunas, 0 CRC errado, 0 conversões perdidas, 0 erros de SPI e 0 palavras
de status fora de sincronia** em 29.894 interrupções. O sinal é o gerador de teste interno da peça,
e ele saiu dentro da especificação nos 8 canais: amplitude de 1.859,3–1.860,1 µV contra ±VREF/2400 =
1.875 µV, e período de 256,00 conversões contra f_CLK/2²¹. A taxa pelo relógio do M33 foi
**250,031 conversões/s**. Os números e o comando estão em `RESULTS.md` §3.

**Como esta seção foi escrita.** A placa caiu em 07/10, entre a primeira captura com o conversor
(12:44) e a de 120 s que validaria a última correção. A sessão de 07/10 não foi registrada. Os três
defeitos abaixo foram reconstruídos a partir dos comentários que as correções deixaram em `afe.c` e
`main.c` e da captura de 12:44, que está em `build/med-bench/`. A parte de 08/10 foi medida aqui.

**Três defeitos no firmware**, cada um com a correção já no código:

1. **`0xFF` de um conversor que tinha respondido `0x3E`.** Um `echo stop > state` reinicia o M33,
   mas não o SPI6 nem o conversor. O controlador pode ficar no meio de uma transferência, e o
   conversor fica em `RDATAC` com ninguém lendo. A sondagem seguinte lia `0xFF`. A correção faz as
   duas coisas: reset do SPI6 pelo RCC (o gate dele segue o RIFSC 27, que agora é do M33) e um ciclo
   de PWDN antes do `RESET`. O desligamento ordenado também mudou: `CoproSync_ShutdownCb` deixa o
   conversor em power-down antes de confirmar o stop ao Linux.
2. **HardFault em `main+4`** (CFSR `0x00080000`, NOCP, PC `0x80101114`). O `SystemInit()` não
   seguro da ST não liga CP10/CP11, e o firmware é hard-float. Ligar a FPU na primeira linha de
   `main()` funcionava só enquanto `main()` não tinha ponto flutuante. Quando o caminho do conversor
   foi inlinado, o prólogo passou a salvar `d8` antes da primeira linha. A FPU agora é ligada num
   construtor, que roda antes de `main()`.
3. **Carimbos errados por um milissegundo inteiro.** A captura de 12:44 deu intervalos entre quadros
   de **55.009 e 57.009 µs** (nominal 56.000). O DRDY tinha prioridade maior que o SysTick e
   corrigia o relógio pelo bit pendente. Isso perde um caso: o DRDY interrompendo o próprio handler
   do SysTick, depois que o bit pendente foi limpo e antes do `++ms_ticks`. A correção põe o SysTick
   na prioridade mais alta (0), o DRDY abaixo (1) e o mailbox abaixo dos dois (2), e lê o par
   (ms, `VAL`) com releitura.

**A validação da correção 3 (08/10).** O mesmo firmware de 12:51 de ontem (`sha256 37a52677…`),
captura de 120 s com o gerador de teste: intervalos de **55.986 a 56.000 µs**, σ = 1,95 µs, em 2.142
intervalos. O erro de 1 ms sumiu. A média, 55.993,04 µs, é o oscilador interno da peça medido pelo
relógio do M33. Essa diferença de 125 ppm não diz qual dos dois relógios está certo.

**A injeção da Fase 5, antecipada: o A35 carregado.** Dois `dd if=/dev/zero of=/dev/null bs=8M`
em laço (tráfego de DDR além do cache) e dois `md5sum /dev/zero`, durante toda a captura. O load
average foi a 3,60.

| | ocioso | A35 carregado |
|---|---|---|
| lacunas / CRC / perdidas / descartadas | 0 / 0 / 0 / 0 | 0 / 0 / 0 / 0 |
| intervalo entre quadros, mín–máx | 55.986–56.000 µs | 55.980–56.002 µs |
| σ / p1–p99 | 1,95 µs / 55.989–55.997 | 2,16 µs / 55.985–55.995 |
| **leitura SPI de 27 bytes no M33** | **207–220 µs** | **252–259 µs** |

**A segregação por processador entrega o tempo, mas não isola o M33.** Sob carga, a datação mudou
pouco: σ subiu 0,2 µs e a faixa passou de 14 para 22 µs. A leitura SPI ficou **18% mais lenta**, e
as duas faixas não se sobrepõem. A causa está no mapa de memória do firmware: o código fica em
`0x80100000` e os dados em `0x80a00000`, os dois **na DDR**, a mesma memória que os `dd` saturavam.
O laço de polling do `HAL_SPI_TransmitReceive` busca instruções de lá. É a explicação que basta, não
uma medida: não se tentou isolar a contenção (código na SRAM/RETRAM, ou o cache do M33). O que fica
para o argumento de particionamento (`RESULTS.md` §4) é que **o processador é separado, a memória não**.
A margem continua larga: 259 µs contra o prazo de 4 ms entre DRDYs.

**Também visto**: o governor no cartão é `schedutil`. A imagem gravada é anterior ao commit que
fixa `performance`. Isso não afeta o relógio do M33, mas afeta qualquer número do lado Linux desta
sessão. E o `scp` não funciona para a placa (`Connection closed`, não há servidor SFTP). Os arquivos
foram por `ssh … 'cat > …'`, com sha256 conferido do outro lado.

**O que isto não significa.** Que um cartão novo adquire sozinho: o firmware continua fora da imagem,
foi carregado pelo `amp-bench.sh`, e quem leu foi `dd`, não o serviço (Fase 4). Nada sobre sinal
biológico: as entradas estavam no gerador de teste, não em eletrodos, então o ruído e o CMRR não
foram medidos. A carga foi uma rodada de 120 s com uma receita de carga. A HMI, que é a carga real do
produto, não roda nesta imagem (`weston.service` falha). A taxa pelo relógio do Linux (249,66) inclui
o tempo até o ack, e por isso não é taxa.

**Estado ao fim da manhã**: M33 `offline` (o stop do script desligou o conversor);
`eeg-acquisition.service` parado à mão (estava em laço pelo link `usb` sem ponte, `NRestarts=460`),
volta no próximo boot; capturas em `build/med-bench/cap-conv-test24-120s{,-load}.bin`.

#### A tarde de 08/10: a Fase 4 na placa viva, e o lead-off

**As três peças da Fase 4, testadas antes de existir imagem.** A receita do firmware ganhou um
carregador (`med-m33-load`), uma unidade sem `[Install]` (`med-m33-firmware.service`) e uma regra de
udev (`90-med-amp.rules`). As três foram postas em `/run` na placa e funcionaram, sem tocar no rootfs:

- `udevadm trigger` no remoteproc chamado `m33` iniciou a unidade, e o firmware foi a `running`.
  `WantedBy` da unidade é o dispositivo do remoteproc, não um target.
- `/dev/med-amp0 -> rpmsg0`. Os atributos que a regra usa foram lidos antes com `udevadm info -a`.
  `ATTR{name}=="rpmsg-raw"` vem do nó, e `ATTRS{name}=="m33"` vem do ancestral remoteproc.
- O `systemctl stop` levou o M33 a `offline` e o link sumiu.
- O serviço real, com a sandbox nova (`DeviceAllow=char-rpmsg_char`, no lugar de `/dev/rpmsg0` e
  `/dev/rpmsg_ctrl0`) e um `eeg.conf` `amp` em `/run`, **adquiriu do conversor por `/dev/med-amp0`**.
  Ficou `active/running` com `NRestarts=0` e prontidão reportada, a sessão foi gravada em `/data` com
  `"link": "amp"`, e não houve descartes. Um `bad status 00 00 00` apareceu na conversão 1 da segunda
  configuração, a do `start()` depois do autoteste. Foi descartado e contado, e não foi investigado.
- Com a prescrição de fábrica (`lead_off_detection` e `bias_drive` = `true`) o firmware da manhã
  **recusou**: `afe.bias_drive: not implemented by this firmware yet`. O motivo chega ao journal no
  campo `MED_DETAIL`. A mensagem mostra só "front-end self test failed", e lê-la com `-o cat` dá a
  impressão errada de que o motivo se perde.

**O BIAS e o lead-off, e um defeito no link `usb` que isso revelou.** Para o BIAS, o driver de kernel
é oráculo: `set_bias(DERIVED)`. Para o lead-off ele **não** é. O driver só liga os comparadores
(`CONFIG4.PD_LOFF_COMP`) e deixa `LOFF_SENSP/N` em 0, e o próprio comentário dele diz que isso é "só
metade" e que o `lead_off_status` responde `disabled`. **No link `usb`, `lead_off_detection = true`
passa no build e não é cumprido**: o descarte silencioso que o firmware se recusa a fazer. Não corrigido.

O M33 agora programa os dois. O estado dos eletrodos vai ao Linux num **`med-amp-abi` v2**, um bloco de
8 bytes depois das amostras (monitorado e solto, positivo e negativo), coberto pelo CRC, num quadro
de 496 bytes, que é a carga inteira de um buffer `rpmsg`. O layout da palavra de status do `RDATAC`
não tem oráculo, e por isso o firmware o confere contra os registradores `LOFF_STAT` a cada
configuração com detecção. Lead-off com o sinal de teste interno é recusado: não há eletrodo na
medição. O serviço audita as transições (`ELECTRODE_CONTACT_CHANGED`), e a HMI mostra o eletrodo solto
em vermelho. Testes de host e injeções em `RESULTS.md` §3.

**Imagem `amp` construída** (`make stm32 KEY=development KERNEL=med`, 5669 tarefas, verde) e conferida
no artefato: no manifesto, `med-m33-firmware` e nenhum módulo do front-end; no ext4, o `eeg.conf`
com `rpmsg`, `/dev/med-amp0` e a prescrição de fábrica, a regra, a unidade, o carregador com o nome
substituído, `DeviceAllow=char-rpmsg_char`, e o firmware igual byte a byte ao do deploy (`4e380bc0…`).
**Não gravada.**

**O que interrompeu a validação na placa.** A placa reiniciou às 16:16 UTC, e a bancada tinha sido
mexida. Desde então o conversor responde ID `0xFF` com DRDY `0`, a mesma assinatura do conversor
sem alimentação da §9.15. A sondagem não mudou hoje. Por isso o firmware v2 **não foi
exercitado contra o conversor**: nem o lead-off, nem a conferência do layout, nem a prescrição de
fábrica.

**Um boot inteiro sumiu do journal persistente.** O boot desta manhã (`3a9d06f5…`, ~15:07–16:15 UTC),
em que rodaram as capturas e a sessão de aquisição das 15:40, **não aparece** em
`journalctl --list-boots`, e `journalctl _BOOT_ID=3a9d06f5…` dá `-- No entries --`. A lista pula
de 07/10 16:02 para 08/10 16:16. A sessão das 15:40 continua em `/data/eeg`
(`session-2026-10-08T15:40:20.815Z`), mas a trilha de auditoria dela não existe mais. No diretório do
journal há um arquivo de 8 MiB com sufixo `~`, de 07/10 15:22, que é a marca do journald para um
arquivo encontrado sujo na abertura. Causa não investigada.
Hipótese a testar: o journal nunca saiu de `/run` naquele boot, ou o reinício o cortou antes de um
sync. É o tipo de lacuna que a IEC 62304 cobra, e fica aberto.

**O fallback A/B disparou sozinho, por uma falha real, e por um motivo que não é de software.** Depois
de mais reinícios, a placa voltou como **slot B**: `rauc.slot=B`, `Booted from: rootfs.1 (B)`,
`BOOT_A_LEFT=0`, slot A `bad`, `BOOT_B_LEFT=2`. A chave SSH e o machine-id mudaram junto
(`642fd642…` → `e6ecbd27…`), como `BOOT_SLOT_AB_STM32MP2.md` registra. Ninguém gravou o cartão.
O mecanismo foi este: a imagem do slot A é a do link `usb`, a ponte não está mais ligada, o serviço de
aquisição nunca reporta pronto, e por isso nenhum boot de A é confirmado (`boot-complete.target`
espera por ele desde 04/10). Cada reinício gastou uma tentativa. **É o primeiro fallback não injetado,
e ele mostra um limite da política**: amarrar "o slot é bom" à aquisição não distingue software
quebrado de front-end ausente. O dispositivo reverteu uma atualização porque faltava um periférico. O
slot B também é da imagem `usb` (`NRestarts=127` ao chegar), então também não será confirmado. Depois
de mais dois boots o `med_select` esgota os dois slots, restaura os contadores e reinicia em A.

**A placa desliga sozinha depois de muito tempo ligada** (relato do operador, 08/10). Isso explica ao
menos parte dos reinícios desta tarde. Ela está em observação só com a alimentação da própria placa,
sem o AFE. Causa desconhecida. Não confundir com o reset de SoC da §9.15, que tinha causa e correção
conhecidas.

**O AFE continuou mudo depois de realimentado**: ID `0xFF`, DRDY `0`, com o firmware v2 (`4e380bc0…`),
e o RIF certo ("all belong to this core"). O DRDY é saída do conversor, lida como GPIO, sem nenhum
código de SPI no caminho, e em repouso alimentado fica em 1 (lido assim de manhã). Em 0, ele aponta
para alimentação ou fiação, não para o firmware. Não verificado com multímetro.

**Estado ao fim da tarde**: slot B, M33 `offline`, `eeg-acquisition.service` parado à mão. Testes
interrompidos pelo operador.

#### A noite de 08/10: o AFE consertado, e o firmware v2 no silício

**A causa do AFE mudo era um resistor mal soldado na placa do AFE** (achado e corrigido pelo
operador). Confirmado primeiro pela ponte USB, no host, com o `scripts/afe-alive.py`. Antes do
conserto: MISO `ff` em todos os modos SPI e o DRDY parado em alto depois do START. Depois do conserto:
ID `0x3E` nos modos 1, 2 e 3, e o DRDY desce com o START. O parágrafo final do script, a "Leitura",
continua dizendo que o DOUT não chega à MISO mesmo nessa execução aprovada. Isso é defeito do
script, não da placa, e não foi corrigido.

**A placa voltou sozinha ao slot A**, como previsto: os dois slots esgotaram as tentativas, o
`med_select` restaurou os contadores (`BOOT_A_LEFT=2`, `BOOT_B_LEFT=3` ao chegar) e a chave SSH voltou
a ser a do slot A (`5ywK7…`, a mesma conferida no início do dia). Cada reinício com a imagem `usb`
gasta uma tentativa de novo.

**O firmware v2 (`4e380bc0…`) contra o conversor, pelo `amp-bench`:**

| Teste | Resultado |
|---|---|
| prescrição de fábrica (lead-off e BIAS `true`, sinal `off`) | **aceita**; relido `CONFIG3 ec`, `SENS ff ff ff ff`, `CONFIG4 02` |
| layout da palavra de status contra `LOFF_STAT` | **confere no silício**: `P 42 N 00` pelos dois caminhos. `0x42` não é zero nem simétrico |
| 180 quadros com a prescrição de fábrica, sem eletrodos | 0 CRC, 0 lacunas, monitorado `ff/ff`; entradas positivas soltas em 173–180 dos 180 quadros, negativas em ~110; as 16 soltas ao mesmo tempo em 107 quadros; amostras no fundo de escala, +187,5 mV |
| sinal de teste interno, 120 s (regressão do v2) | 2.143 quadros, 0 lacunas, 0 CRC, 0 perdas, 0 status inválido; os 8 canais dentro da especificação, iguais aos da manhã; monitorado `0000`; arquivo de 72 + 2143 × 496 bytes |
| lead-off com o sinal de teste | **recusado** na placa: `afe.lead_off_detection: on the test signal, no electrodes` |

**O que isto fecha.** O layout `[SBAS499?]` da palavra de status deixa de ser leitura do datasheet:
foi a peça que respondeu, pelos dois caminhos e com um valor que distingue os campos. Também se viu,
pela primeira vez, a prescrição de fábrica cumprida, e não recusada.

**O que isto não significa.** Que a detecção acerta um eletrodo que se solta de um paciente: não há
eletrodos na bancada, e "tudo solto" é o resultado trivial. O `P 42` da conferência, que vira tudo
solto nos quadros, é compatível com entradas flutuando que demoram a derivar sob 6 nA. Isso é
interpretação, não medida. O BIAS derivado foi programado e relido, mas não medido eletricamente.

**Ainda aberto, visto de novo**: um `bad status 00 00 00` na **conversão 1** da execução com a
prescrição de fábrica (descartado e contado), e nenhum na execução de 120 s com o sinal de teste. O
mesmo apareceu na tarde, na segunda configuração do serviço. É um padrão (a primeira conversão
depois de certas configurações), e não foi investigado.

A taxa pelo relógio do M33 foi **249,950** conversões/s, contra 250,031 de manhã. Os intervalos entre
quadros ficaram em 55.991–56.022 µs, contra 55.986–56.000. Entre as duas medidas a placa do AFE foi
retrabalhada e a bancada mudou. A diferença é registrada e não explicada.

#### A Fase 4 no cartão: gravado do zero, adquirindo sozinho

Imagem `amp` v2 (`make stm32 KEY=development KERNEL=med`, 08/10 13:42) gravada com `bmaptool`.
**Sem `sgdisk -e`**: o GPT de reserva ficou no fim da imagem, com o mesmo efeito da §9.7. Foi
corrigido depois dos quatro boots da tabela abaixo, no host. O desktop tinha montado sozinho
`med-boot` e os dois slots quando o cartão entrou, e o `sgdisk -e` rodou com eles montados (ele só
reescreve os cabeçalhos do GPT). O `eject` desmontou antes de remover. No boot seguinte, o quinto,
não houve nenhuma linha de GPT no `dmesg`, onde antes havia "Alt. header is not at the end of the
device" a cada boot, nem aviso de ext4. A aquisição saiu igual às outras quatro: autoteste em
12,3 s, `READY=1` em 12,5 s, slot confirmado em 13,1 s, `NRestarts=0`. A placa não tem `sgdisk`
para conferir o GPT diretamente; a evidência é a ausência do aviso. O cartão novo deu uma chave SSH
nova (`CBZ/9s3n…`), aceita no primeiro uso depois de conferido o MAC.

| | 1º boot do cartão | reinício 1 | reinício 2 | reinício 3 |
|---|---|---|---|---|
| M33 carregado pelo udev | 7,7 s | 8,4 s | 8,0 s | 8,1 s |
| autoteste aprovado, com a prescrição de fábrica | 22,2 s | 12,3 s | 12,1 s | 12,1 s |
| `READY=1` | 22,4 s | 12,5 s | 12,3 s | 12,3 s |
| slot confirmado (`rauc-mark-good`) | 23,0 s | 13,2 s | 12,9 s | 12,9 s |
| `NRestarts` do serviço | 0 | 0 | 0 | 0 |
| `BOOT_A_LEFT` depois da confirmação | 3 | 3 | 3 | 3 |

Nenhum comando na placa entre gravar e medir: os quatro boots saíram de um `systemctl reboot` e de
uma verificação só de leitura (`build/med-bench/phase4-boots.log`). O primeiro boot é 10 s mais
lento porque provisiona o `/data`. **A confirmação do slot vem depois da aquisição, nos quatro.**
É a política de 04/10 funcionando como foi desenhada, e é também o que encerra o ciclo de
fallback que a imagem `usb` sem a ponte tinha causado à tarde.

**O que isto não significa.** A enumeração não variou: o M33 foi `remoteproc1` e o canal `rpmsg0`
nos quatro boots. Seguir pelo nome está implementado, mas não foi testado contra uma troca de índice.
O `weston.service` falha nos quatro (§9.9), e a HMI, com o aviso de eletrodo solto, não foi vista na
tela. A injeção de falha da Fase 4 (tirar o firmware da imagem) não foi feita, porque depende da
política de reinício (`StartLimit*`), que não existe.

**Dois achados do primeiro boot.**

- **A auditoria de contato registrou 8 eventos em 4 s e depois parou.** Com as entradas abertas, o
  conjunto de eletrodos soltos mudou quadro a quadro enquanto elas derivavam, e cada mudança é uma
  transição. Não é uma inundação, mas não há limite de taxa: um eletrodo de contato ruim, oscilando,
  produziria um registro por quadro. Aberto.
- **O journal do primeiro boot começa em 20,6 s**, no instante em que o `systemd-timesyncd` salta o
  relógio de 2025-05-29 para 2026-10-09. Tudo antes disso falta, inclusive a linha do carregador do
  M33 e o provisionamento do `/data`. Nos três reinícios seguintes a linha das 8 s está lá. Isso
  restringe a hipótese da manhã (o boot inteiro sumido) ao **primeiro salto de relógio**, e não a
  todo boot. Não investigado.

## 10. Estado ao fim deste registro

**Construído e verificado por inspeção**: `med-image-eeg` para `stm32mp25-disco`, `.wic` de
2.761.966.592 bytes, GPT conferido, e reproduzido idêntico depois de o cache inteiro migrar para
disco externo e voltar.

**Validado em execução no QEMU**: `make check` 21/21, e continuou 21/21 depois da refatoração da
custódia de chave — que é o que garante que nada regrediu no alvo que já tinha evidência.

**Validado em execução na placa** (§9, 2026-08-18) — a coluna que até este registro estava vazia:

| item | evidência |
|---|---|
| Boot completo até multi-user | console serial, sistema de pé > 8 min |
| Cadeia TF-A → OP-TEE → U-Boot → kernel | `Machine model: ... CA35TDCID OSTL`, `Linux 6.6.129` |
| Slot A boota pelo PARTUUID que o `.wks` fixa | `root=PARTUUID=e91c4e10-…` = `--uuid` de `med-root-a` |
| GPU não derruba o mundo seguro | `Galcore version 6.4.21.1.1058597`, sem pânico |
| `/data` LUKS2 provisionado no primeiro boot | `cryptsetup status`, 13 s, `dm/uuid` `CRYPT-…` |
| Chave em partição fora dos slots A/B, `0400` | `stat` em `med-boot` (`mmcblk0p8`, ext4) |
| RAUC enumera slots e identifica o bootado | `rauc status`: `Booted from: rootfs.0` |
| Trilha de auditoria selada | `journalctl --verify` → `PASS` |
| Cortex-M33 presente e disponível | `remoteproc remoteproc1: m33 is available` |
| **HMI Qt renderizando em monitor HDMI** (§9.9) | `NRestarts=0`, 1,099 s de CPU em 2 min 22 s; plano DRM de 1030x633 = a janela do QML |
| **`/etc/fw_env.config` da receita, leitura e escrita** (§9.9) | `fw_printenv`/`fw_setenv` round-trip; `BOOT_ORDER` persiste entre regravações do cartão |
| **RAUC resolve slot primário e ativa** (§9.9) | `Activated: rootfs.0 (A)`, ambos os slots `good` |
| **`rauc install` de bundle assinado, na placa** (§9.10) | `succeeded`; slot B conferido byte a byte contra o `Checksum` do manifesto |
| **Reordenação do ambiente do bootloader preservando o fallback** (§9.10) | `BOOT_ORDER=B A`, `Activated: rootfs.1 (B)` |
| **Montagem de bundle `verity` no kernel do alvo** (§9.10) | `CONFIG_DM_VERITY=y` em `config-6.6.129`; `install` passa onde falharia |
| **O slot escrito pelo RAUC boota** (`BOOT_SLOT_AB_STM32MP2.md`) | `Booted from: rootfs.1 (/dev/mmcblk0p10)` após editar o `extlinux.conf` à mão |
| **O dispositivo escolhe o slot sozinho** (§9.11) | `rauc.slot=A` na cmdline de um cartão recém-gravado, sem `fw_setenv` |
| **`rauc-mark-good` roda no boot** (§9.11) | `active (exited)`, e `BOOT_A_LEFT` volta a 3 depois de o bootloader decrementá-lo |
| **A troca A/B, ponta a ponta** (§9.11) | `rauc install` → `BOOT_ORDER=B A` → reboot → `Booted from: rootfs.1 (B)`, com A ainda `good` |
| **O M33 carrega firmware assinado pelo OP-TEE** (§9.13) | `state=running`, canais rpmsg criados; dois bits trocados → `TEE_ERROR_SECURITY` e `TEE_ERROR_SIGNATURE_INVALID` |
| **O M33 lê o ADS1299 pelo SPI6** (§9.15) | RIF do SPI6, pinos e EXTI 8 em CID2 no FIP; ID `0x3E` lido pelo M33; com o conversor sem alimentação, `0xFF` e recusa |
| **O M33 adquire do ADS1299** (§9.16) | 120 s, DRDY por interrupção: 0 lacunas, 0 perdas, gerador de teste dentro da especificação nos 8 canais; o mesmo com o A35 carregado |

**Continua sem evidência de execução na placa** — cada item com causa conhecida:

| bloqueio | causa | estado |
|---|---|---|
| Fallback A/B em boot falho | o `bootcmd` o implementa; nunca foi exercitado | **não medido** — exige injeção de falha (`implementation_plan_uboot_ab.md` §8, passo 6) |
| Identidade estável através de uma atualização | chave SSH e `machine-id` gerados por slot | **medido numa atualização real** (§9.11); sem correção |
| Identidade estável através de uma atualização | chave de host SSH e `machine-id` são gerados por slot | **defeito novo** (`BOOT_SLOT_AB_STM32MP2.md` §5.1) |
| Continuidade da trilha de auditoria | `/var/log/journal` mora dentro de um slot A/B | **defeito novo**, previsto por inspeção (`BOOT_SLOT_AB_STM32MP2.md` §5.2) |
| Atualização que troca a versão do kernel | `med-boot` é compartilhada; o kernel está fora dos slots | não suportado pelo esquema atual (`BOOT_SLOT_AB_STM32MP2.md` §5.4) |
| RAUC instalar, marcar e ativar slot | ~~`fw_env.config` ausente~~ e ~~`DM_VERITY` ausente no kernel~~ → ambos resolvidos; falta `rauc.slot=` na cmdline para o `mark-good` rodar sozinho | **`rauc install` executado e conferido byte a byte** (§9.10); automação do `mark-good` depende do script de U-Boot |
| HMI Qt | quatro defeitos em série: seat, permissão do `/dev/galcore`, ocioso de 300 s, timing não-CEA | **resolvido e visto na tela** (§9.9 e `BRINGUP_HMI_STM32MP2.md`); 3 correções ainda voláteis |
| Caminho AMP / `rpmsg` | firmware nosso, assinado, adquirindo sintético pelo serviço (§9.14); SPI6 no M33 pelo RIF, ID do conversor lido (§9.15) | **aquisição do conversor medida pelo `amp-bench`** (§9.16); falta o firmware na imagem e o serviço como leitor (Fase 4) |
| Serviço de aquisição estável | sem firmware no M33, o autoteste do driver `rpmsg` falha | **medido**: 163 reinícios, período de 2,500 s (§9.9) |
| Carimbo de tempo confiável | sem RTC inicializado, sem NTP antes de `/data` | **defeito novo, não tratado** (§9.7) |
| Custódia real de chave | sem TPM alcançável (§7) | investigado, não implementado |

---

## 11. As regras que ficam

1. **Suposição documentada sobre comportamento de terceiros é dívida.** O comentário que afirmava o
   que o BSP da ST faz estava errado e custou um build. Ou se verifica, ou se escreve "não
   verificado".
2. **Nome genérico sobre conteúdo específico de máquina é o disfarce padrão** de vazamento de camada.
   `med-partitions.wks`, `med-image-base.bb` "agnóstico", `med-data-provision.sh` sem "ESP" no nome —
   os três.
3. **Defeito escondido atrás de uma recusa continua sendo defeito.** O `tpm2` recusando provisionar
   mascarava um script que não poderia funcionar naquela placa de forma alguma.
4. **`allarch` é uma afirmação verificável**, não um atalho de empacotamento.
5. **Composição de fragmento kas depende da ordem alfabética da chave**, e por isso a variação de
   um alvo é melhor como *parâmetro* que como arquivo. `:forcevariable` corrige a instância e deixa
   a armadilha de pé para o próximo overlay; a variável de ambiente declarada em `env:` (§4) não
   tem ordenação para perder. Onde um overlay for mesmo necessário, `:forcevariable`.
6. **Fragmento de kernel chegar não é fragmento de kernel vencer — e "não vencer" ainda era
   otimista.** Ele não estava sequer sendo fundido: `kernel-yocto` varre a `SRC_URI` sozinho,
   `kernel.bbclass` puro não varre nada. Leia o `.config` produzido, e antes disso confira se o
   arquivo aparece na lista que a receita do vendor realmente consome.
7. **Extraia o pacote e leia o arquivo instalado.** Foi o que pegou o overlay que perdia em silêncio —
   e nem `bitbake -e` teria pego, porque a variável estava certa no datastore e errada no `local.conf`
   que a sobrescrevia depois.
8. **Inspecionar o artefato não substitui energizar a placa.** As regras 1 a 7 vieram de defeitos
   achados lendo o que o build produziu; os quatro defeitos da §9 não tinham como ser achados assim.
   Três deles — o pânico da GPU, a colisão de `by-partlabel` com um segundo disco, o relógio sem RTC
   — dependem de *hardware que a imagem não contém*. É o mesmo salto que o `make check` representou
   em relação ao build verde, um degrau acima.
9. **Nome de dispositivo é sempre uma corrida, nunca um fato.** `mmcblk0` e `mmcblk2` trocaram de
   papel entre dois boots do mesmo hardware sem mudança de configuração. Endereçar por rótulo é
   melhor que por caminho, mas só é suficiente se o rótulo for único no *conjunto de discos
   presentes* — e um eMMC de fábrica é um disco presente. Por PARTUUID quando não puder ser único.
10. **Uma correção que "não foi exercitada" pode já ter sido invalidada.** O plano do ADS1299
    descreve carregar firmware no Cortex-M33; a placa respondeu `Support of signed firmware only`
    antes de qualquer linha desse plano ser escrita em código. Boots exploratórios pagam-se em
    restrições descobertas cedo.
11. **Uma unidade pulada por `Condition*` não falha — ela desaparece.** O `rauc-mark-good` nunca
    rodou em boot nenhum porque a linha de comando do kernel não traz `rauc.slot=`, e um
    `ConditionKernelCommandLine` não satisfeito não é erro: não aparece em `--state=failed`, não
    emite aviso, e some do radar. É a mesma família do serviço em laço que nunca chega a `failed`
    porque a cadência de reinício passa raspando pela janela do limitador. **Estado do systemd não
    é sinônimo de saúde**; quando algo "não está falhando", pergunte se está rodando.
12. **Um valor escrito à mão numa bancada é medição, nunca correção.** `fw_setenv BOOT_A_LEFT 3`
    destravou o RAUC e provou onde estava o bloqueio — e o `BOOT_B_LEFT=3` do mesmo teste afirma que
    um slot vazio é bootável, o que é falso. Anote o que foi digitado à mão, ou a próxima sessão
    herda um estado que ninguém sabe explicar.
13. **O estado de fábrica é um estado, e alguém precisa criá-lo.** `BOOT_ORDER` nunca foi escrito
    por ninguém: o `.wks` cria a partição vazia, o RAUC só a escreve durante um install, e o
    `mark-good` — que parece ser exatamente o comando para isso — grava apenas o contador. O
    resultado é um aparelho novo que reporta *todos* os slots ruins, inclusive o que está rodando.
    Ao projetar uma máquina de estados persistente, pergunte quem escreve o primeiro valor; se a
    resposta for "o primeiro uso", o dispositivo passa a vida útil inteira antes disso num estado
    que ninguém especificou.
14. **Dois programas com o mesmo nome não são o mesmo programa.** O `fw_printenv` do `libubootenv`
    imprime `VAR=` e sai com 0 para uma variável inexistente; o do `u-boot-tools` imprime
    `## Error: "VAR" not defined` e sai com erro. O RAUC ramifica no status de saída, então **qual
    implementação a imagem instala decide se "nunca inicializado" vira um erro com fallback ou um
    valor vazio aceito em silêncio.** Nenhuma das duas está errada. Quando um componente chama uma
    ferramenta pelo nome, o contrato é a implementação empacotada, não o nome.
15. **`rauc info` passar não diz nada sobre `rauc install`.** Um lê o bundle com `unsquashfs` em
    userspace, o outro empilha dm-verity sobre um loop device dentro do kernel. Quando dois comandos
    de uma mesma ferramenta tocam camadas diferentes, eles são dois testes — e a diferença entre
    eles é um bissector de graça: se o primeiro passa e o segundo falha, o defeito está abaixo do
    userspace.
16. **Um segundo rootfs é um segundo dispositivo, até prova em contrário.** Bootar o slot B não
    revelou nada sobre boot — revelou que a placa tem duas chaves de host SSH, dois `machine-id` e
    dois journals, porque tudo que é gerado "no primeiro boot" é gerado uma vez **por slot**.
    Nenhuma inspeção do `.wic` mostraria isso, porque no artefato os dois slots são idênticos: a
    diferença nasce em execução. Ao projetar A/B, faça a lista do que o dispositivo cria sozinho na
    primeira vez e decida, item a item, se aquilo é identidade (tem de ser único e estável),
    conteúdo (pode ser duplicado) ou histórico (tem de ficar fora dos slots).
17. **Compilar o objeto não é linkar o módulo.** `make drivers/hid/hid-mcp2210.o` passou limpo e o
    build do Yocto quebrou em `do_compile_kernelmodules` com
    `uses symbol counter_priv from namespace COUNTER, but does not import it` — um
    `MODULE_IMPORT_NS` que falta só aparece no `modpost`, que é o estágio de *link* e que o alvo do
    `.o` nunca alcança. A suíte de host também não linka nada, então ela era igualmente cega. A
    validação original destes drivers incluía "`modpost` resolvendo tudo"; esse passo caiu sem que
    nada ficasse vermelho. Um driver de módulo só está verificado depois de um `.ko` que linka.
18. **Um `git commit --amend -a` varre o que estiver na árvore.** Uma correção de uma linha num
    arquivo levou 183 linhas de outro assunto, em outro subsistema, para dentro do commit errado —
    numa série destinada ao mainline, onde "uma mudança lógica por patch" é a regra mais dura de
    todas. Foi desfeito com `reset --soft` e dois `git add` por caminho, conferindo que a árvore
    resultante era byte a byte a mesma. Antes de emendar, `git status`; e para emendar só um
    arquivo, `git add <caminho>` e `--amend` sem `-a`.
19. **Um comando composto reporta o exit code do último elemento.**
    `make stm32 > log 2>&1; echo $?; tail log` devolveu 0 num build que falhou, porque o 0 era do
    `tail`. A notificação de fim de tarefa dizia sucesso. A regra é pôr o comando que importa por
    último, ou capturar o código antes de qualquer coisa que rode depois dele — e, de todo modo,
    abrir o log em vez de acreditar no código de saída.
20. **`bitbake -e` imprime o corpo das funções python anônimas.** Um `grep` por nome de arquivo casa
    com o texto do código-fonte e não com o valor de uma variável, o que transformou um
    não-achado em achado por alguns minutos. Ancorar em `^VARIAVEL=` é a diferença entre ler o
    datastore e ler o recipe.

