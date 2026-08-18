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

Note o padrão: **seis dos treze itens não tinham sintoma nenhum**. Não falharam build, não falharam
boot, não emitiram warning. Apareceram porque alguém foi olhar o artefato produzido. E note o
complemento que o primeiro boot acrescentou: os itens 9 a 12 são o oposto — nenhum deles poderia ter
sido encontrado sem energizar a placa, e três deles nenhuma inspeção de artefato teria revelado.

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
| `u-boot-env`, `fip-a`, `fip-b`, `metadata1`, `metadata2` | **não determinado** | os nomes existem nos dois layouts; a saída do `ls -l` foi truncada pela largura do terminal |

Os quatro nomes `med-*` são únicos por construção e todos foram para o cartão — que é exatamente o
que a renomeação `bootfs` → `med-boot` existia para garantir, e o motivo pelo qual a chave do
`/data` foi parar no disco certo.

A última linha da tabela é uma pendência, não um detalhe: **é dela que depende o
`/etc/fw_env.config`** (§9.5). Para fechar sem truncamento:

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

**Continua sem evidência de execução na placa** — cada item com causa conhecida:

| bloqueio | causa | estado |
|---|---|---|
| Seleção de slot A/B | `med-boot` compartilhada; `extlinux.conf` fixa o slot A | não implementado |
| RAUC marcar/ativar slot | `/etc/fw_env.config` não existe | **observado falhando** (§9.5); não implementado |
| HMI Qt | `weston.service` falha com `status=1`, DRM presente | **falha observada, causa desconhecida** (§9.6) |
| Caminho AMP / `rpmsg` | `MED_AMP_FIRMWARE` vazio; firmware do ADS1299 não existe — e agora sabe-se que terá de ser **assinado** (§9.7) | não implementado |
| Serviço de aquisição estável | dois `Started` no boot; `NRestarts` não consultado | **não verificado** (§9.6) |
| Carimbo de tempo confiável | sem RTC inicializado, sem NTP antes de `/data` | **defeito novo, não tratado** (§9.7) |
| Custódia real de chave | sem TPM alcançável (§7) | investigado, não implementado |
| Instalação de bundle real na placa | depende do `fw_env.config` | pendente |

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
