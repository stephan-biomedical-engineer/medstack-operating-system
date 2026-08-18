# Registro de Engenharia — Porte para o STM32MP257 e o que ele custou

> **Status**: registro do que foi feito, por que, e com que evidência. Não é plano
> (`implementation_plan_*.md`) nem medição consolidada (`RESULTS.md`) — é a narrativa que
> conecta os dois, incluindo os erros de percurso, que num TCC valem tanto quanto os acertos.
>
> **Quando**: 2026-08-17. **Onde**: `stm32mp25-disco` e `qemux86-64`.
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

Note o padrão: **quatro dos oito itens não tinham sintoma nenhum**. Não falharam build, não falharam
boot, não emitiram warning. Apareceram porque alguém foi olhar o artefato produzido.

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

A correção é `MED_DATA_KEY_SOURCE:forcevariable`, último item do `OVERRIDES` do bitbake, que vence
independentemente de onde o kas decida escrever o bloco. Renomear a chave para ordenar por último
também funcionaria e quebraria no dia em que alguém acrescentasse um bloco com letra posterior.

### O overlay de bring-up

`kas/bringup-stm32mp2.yml`, composto com `make stm32-bringup`. O delta é **uma variável**. O perfil
`project-eeg-stm32mp2.yml` continua declarando `tpm2`, que é o que o produto deveria ser; sobrescrever
a configuração do alvo para fazer um primeiro boot passar deixaria o repositório descrevendo um
dispositivo que nunca foi pretendido, sem registrar que a diferença era temporária.

E note o que **não** foi dispensado: `MED_EEG_REQUIRE_ENCRYPTION` continua `"true"`. Com a custódia
resolvida, `/data` é LUKS2 de verdade na placa, o `MedicalStorage` encontra um `dm/uuid` começando em
`CRYPT-`, e o serviço subir continua sendo a asserção de que a criptografia é real.

---

## 5. O fragmento de kernel perde para a ST

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

## 9. Estado ao fim deste registro

**Construído e verificado por inspeção**: `med-image-eeg` para `stm32mp25-disco`, `.wic` de
2.761.966.592 bytes, GPT conferido, e reproduzido idêntico depois de o cache inteiro migrar para
disco externo e voltar.

**Validado em execução**: nada, no STM32. O `make check` 21/21 é no QEMU, e continuou 21/21 depois
da refatoração da custódia de chave — que é o que garante que nada regrediu no alvo que tem
evidência.

**Continua sem evidência de execução na placa** — e cada item tem causa conhecida:

| bloqueio | causa | estado |
|---|---|---|
| Seleção de slot A/B | `bootfs` compartilhada; `extlinux.conf` fixa o slot A | não implementado |
| RAUC marcar slot | `/etc/fw_env.config` não existe (a ST instala `.mmc`/`.nand`/`.nor`) | não implementado |
| Caminho AMP / `rpmsg` | `MED_AMP_FIRMWARE` vazio; firmware do ADS1299 não existe | não implementado |
| HMI Qt | `ACCEPT_EULA` não definido → pacotes de GPU excluídos | decisão pendente |
| Custódia real de chave | sem TPM alcançável (§7) | investigado, não implementado |
| Qualquer boot | nada foi gravado em cartão nem energizado | pendente |

---

## 10. As regras que ficam

1. **Suposição documentada sobre comportamento de terceiros é dívida.** O comentário que afirmava o
   que o BSP da ST faz estava errado e custou um build. Ou se verifica, ou se escreve "não
   verificado".
2. **Nome genérico sobre conteúdo específico de máquina é o disfarce padrão** de vazamento de camada.
   `med-partitions.wks`, `med-image-base.bb` "agnóstico", `med-data-provision.sh` sem "ESP" no nome —
   os três.
3. **Defeito escondido atrás de uma recusa continua sendo defeito.** O `tpm2` recusando provisionar
   mascarava um script que não poderia funcionar naquela placa de forma alguma.
4. **`allarch` é uma afirmação verificável**, não um atalho de empacotamento.
5. **Composição de fragmento kas depende da ordem alfabética da chave.** Use `:forcevariable` quando
   o fragmento *precisa* vencer.
6. **Fragmento de kernel chegar não é fragmento de kernel vencer.** Leia o `.config` produzido.
7. **Extraia o pacote e leia o arquivo instalado.** Foi o que pegou o overlay que perdia em silêncio —
   e nem `bitbake -e` teria pego, porque a variável estava certa no datastore e errada no `local.conf`
   que a sobrescrevia depois.
