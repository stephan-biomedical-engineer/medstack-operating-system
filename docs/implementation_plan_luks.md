# Plano de Implementação — Provisionamento do Volume Criptografado `/data` (LUKS)

> **Status**: plano corrente (não histórico). Complementa `PROJECT_CONTEXT.md`, que continua sendo
> a referência arquitetural autoritativa. Os `implementation_plan.md`, `implementation_plan_EEG.md`
> e `implementation_plan_improvements.md` são iterações anteriores e **não** descrevem o estado
> atual do repositório.
>
> **Escopo**: criar o passo de provisionamento de primeiro boot que converte a partição `med-data`
> num volume LUKS, e decidir a custódia da chave. Enumera as mudanças arquivo a arquivo.
>
> **Verificado nesta análise** (2026-08-16, na árvore construída e no boot do QEMU):
> - o passo de provisionamento **não existe**: `grep -rl "luksFormat\|cryptenroll"` sobre
>   `meta-custom/` só encontra menções em comentários (`crypttab`, `med-partitions.wks`,
>   `MedicalStorage.h`);
> - `MedicalStorage.cpp:113` (`hasEncryptedBacking`) faz `stat()` no caminho, lê
>   `/sys/dev/block/<major>:<minor>/dm/uuid` e retorna verdadeiro **apenas** se o UUID começa com
>   `CRYPT-`; `MedicalStorage.cpp:201` recusa a abertura quando `requireEncryptedBacking` está
>   ligado e isso é falso;
> - `systemd-cryptenroll` está na imagem, no pacote `systemd-crypt` (confirmado no manifesto), e o
>   `cryptsetup` 2.7.5 também;
> - `med-os.conf:60` tem `PACKAGECONFIG:append:pn-systemd = " gcrypt cryptsetup"` — **sem `tpm2`**;
> - o fragmento de kernel tem `CONFIG_DM_CRYPT=y` e `CONFIG_BLK_DEV_DM=y`, e **nenhuma** opção de
>   TPM;
> - `data.mount` declara `Requires=systemd-cryptsetup@med\x2ddata.service` *e*
>   `ConditionPathExists=/dev/mapper/med-data`.
>
> **Não verificado** (análise): o comportamento do `systemd-cryptenroll --tpm2-device` no
> STM32MP257; se o `meta-st-stm32mp` expõe um TPM ou um caminho OP-TEE utilizável; o custo real de
> re-selar a chave após uma atualização RAUC (ver §7.3, que é o risco mais sério deste plano).

---

## 1. O que está quebrado, e por que só apareceu agora

```
[FAILED] Failed to start Cryptography Setup for med-data.
[DEPEND] Dependency failed for /dev/mapper/med-data.
[DEPEND] Dependency failed for MedPlatform persistent data volume (/data).
```

A falha **não é regressão**: é a consequência direta de o layout A/B finalmente existir. Antes da
correção do `WKS_FILE` (ver `implementation_plan_rauc.md` §8.3), a imagem QEMU era um `ext4` cru
sem tabela de partições, `/dev/disk/by-partlabel/med-data` nunca aparecia, a unidade `.device`
nunca ativava e o `systemd-cryptsetup` nunca chegava a rodar. Nada falhava porque nada era
tentado.

Com a partição existindo, o systemd faz o que o `crypttab` manda e o `luksOpen` falha, porque a
partição é **ext4 puro sem cabeçalho LUKS**. O `med-partitions.wks` já sabia disso — *"wic cannot
produce a LUKS header, so the partition is created as plain ext4 and converted on first boot by the
provisioning step"* — e esse passo nunca foi escrito.

Detalhe que confunde na leitura do console: o `data.mount` tem `ConditionPathExists` justamente
para pular limpo quando o volume não existe, mas também tem `Requires=` sobre a unidade de
cryptsetup. O systemd derruba a unidade por dependência **antes** de avaliar a condição, então a
condição nunca chega a proteger nada. Os dois mecanismos, juntos, se anulam — ver §4.4.

---

## 2. O critério de aceitação já é código, não inspeção

Isto é o que torna este plano barato de verificar. O framework não confia em "rodamos
`luksFormat`": `MedicalStorage::hasEncryptedBacking` resolve o dispositivo que provê `/data` e
exige que o `dm/uuid` correspondente comece com `CRYPT-`. Ou seja, ele aceita o volume se, e
somente se, `/data` está sobre um mapeamento dm-crypt de verdade.

Consequência prática, e é o teste de ponta a ponta deste plano:

> **`MED_EEG_REQUIRE_ENCRYPTION = "true"` no `project-eeg-qemu.yml`, e o
> `eeg-acquisition-service` sobe.**

Hoje esse valor é `"false"` precisamente porque não há volume criptografado, e o serviço registra
um `AuditEvent::SecurityEvent` dizendo que aceitou um armazenamento sem criptografia. Quando o
provisionamento funcionar, o valor vira `"true"` e aquele registro de auditoria some. Se o
provisionamento estiver errado, o serviço **se recusa a iniciar** — sozinho, sem precisar de
inspeção manual.

---

## 3. Decisão: custódia de chave plugável, TPM só no alvo físico

O `crypttab` já cravou o critério, e ele está certo: *"a key stored in a read-only image would be
identical on every unit shipped and therefore worthless"*. Mas o QEMU não tem TPM, e emular um com
`swtpm` é um subprojeto — o mesmo argumento que fez o Nível 2 do RAUC ser adiado.

A decisão é a mesma que o repositório já toma três vezes (`MED_EEG_DRIVER`, `MED_BOOTLOADER`,
`MED_WKS_FILE`): **uma variável, valorada no arquivo de projeto do alvo**.

```
MED_DATA_KEY_SOURCE ?= "tpm2"      # default
MED_DATA_KEY_SOURCE = "development" # apenas em project-eeg-qemu.yml
```

O default é `tpm2`, não `development`, e essa direção importa. Um alvo que **esqueceu** de declarar
sua fonte de chave falha ao provisionar e não monta `/data` — o serviço então se recusa a iniciar,
alto e claro. O contrário — provisionar em silêncio com uma chave de desenvolvimento — produziria
um dispositivo que parece criptografado e não está, que é a pior falha possível nesta categoria. É
o mesmo raciocínio de `MED_EEG_REQUIRE_ENCRYPTION` já ter default `"true"`.

`development` **não** é "sem criptografia". O volume é LUKS de verdade, com chave aleatória por
unidade gerada no primeiro boot; o que falta é a chave estar selada em hardware, e por isso ela
precisa repousar em algum lugar do dispositivo. Isso é uma limitação declarada, auditada em cada
boot, e o texto do TCC deve descrevê-la exatamente assim: **protege contra remoção física da mídia,
não contra um atacante com acesso root ao dispositivo ligado.**

---

## 4. Mudanças concretas

### 4.1. Novo: `meta-med-distro/recipes-core/med-data-volume/files/med-data-provision.sh`

Executado uma vez, antes do `systemd-cryptsetup`. Esqueleto da lógica, na ordem em que importa:

```sh
DEV=/dev/disk/by-partlabel/med-data

# 1. Já provisionado? Nada a fazer. Idempotência é requisito, não conveniência:
#    esta unidade roda em todo boot.
cryptsetup isLuks "$DEV" && exit 0

# 2. NÃO é LUKS. Antes de formatar qualquer coisa, provar que este é um
#    dispositivo virgem e não um dispositivo com o cabeçalho corrompido - ver
#    §4.2, este é o ponto mais perigoso do plano.
assert_pristine "$DEV" || fail_loudly

# 3. Chave, conforme MED_DATA_KEY_SOURCE (substituído em tempo de build).
# 4. cryptsetup luksFormat / luksOpen / mkfs.ext4
# 5. Registrar no journal o que foi feito e com que fonte de chave.
```

### 4.2. A salvaguarda que decide se este plano é seguro

Um passo de provisionamento que formata uma partição **nunca** pode rodar num dispositivo já em
uso. "Não tem cabeçalho LUKS" não é evidência suficiente de dispositivo virgem: um cabeçalho
corrompido produz exatamente o mesmo sintoma, e ali reformatar destrói prontuários.

A distinção precisa de um marcador positivo de "nunca provisionado". O `.wks` já cria a partição
com um rótulo de sistema de arquivos conhecido (`--label med-data`), então:

> Provisiona **apenas** se o dispositivo é um ext4 cujo rótulo é exatamente o que o `.wks` grava.
> Qualquer outro estado — ext4 sem rótulo, rótulo diferente, sistema de arquivos desconhecido,
> lixo — é tratado como **dispositivo danificado**: falha alto, registra em auditoria, não formata.

O `luksFormat` limpa o rótulo por construção, então o estado virgem não é reproduzível por
acidente. Um dispositivo danificado exige intervenção deliberada, que é a resposta certa quando a
alternativa é apagar dados de paciente.

### 4.3. `crypttab` — precisa de opções por fonte de chave

Hoje o campo de chave é `none`, o que faz o systemd pedir passphrase interativamente: um
dispositivo desatendido trava no boot. As duas fontes divergem aqui:

| `MED_DATA_KEY_SOURCE` | terceiro campo | opções |
|---|---|---|
| `development` | caminho do arquivo de chave gerado no provisionamento | `luks,nofail,discard` |
| `tpm2` | `none` | `luks,discard,tpm2-device=auto` |

Vira template com `@MED_DATA_KEYFILE@` / `@MED_DATA_CRYPTOPTS@`, substituídos no recipe do mesmo
jeito que `rauc-conf.bbappend` faz — incluindo a verificação de placeholder não substituído, que
lá já pegou a classe de erro certa.

### 4.4. `data.mount` — resolver a contradição `Requires` + `Condition`

Os dois juntos se anulam (§1). Com o provisionamento existindo, `/dev/mapper/med-data` **sempre**
deve existir num dispositivo saudável, então a decisão é:

- **manter** `Requires=systemd-cryptsetup@med\x2ddata.service` — o volume é obrigatório;
- **remover** `ConditionPathExists=/dev/mapper/med-data` — ele só existia para o caso "QEMU sem
  partição", que deixou de existir quando o `wic` virou o caminho único de boot, e hoje sua única
  função é esconder falhas.

Consequência deliberada: um dispositivo que não consegue abrir `/data` passa a **falhar visível**
em vez de bootar aparentemente bem e gravar prontuários no rootfs.

### 4.5. `med-data-provision.service`

```ini
[Unit]
DefaultDependencies=no
After=dev-disk-by\x2dpartlabel-med\x2ddata.device
Requires=dev-disk-by\x2dpartlabel-med\x2ddata.device
Before=systemd-cryptsetup@med\x2ddata.service
ConditionPathExists=/dev/disk/by-partlabel/med-data

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/usr/libexec/medplatform/med-data-provision.sh
```

### 4.6. Kernel — TPM no fragmento, e por que isso não é "TPM no QEMU"

As opções de TPM entram em `med-kernel-features.cfg`, que é distro-wide via o bbappend curinga.
Isso **não** contraria a decisão da §3: sem dispositivo TPM presente, os drivers simplesmente não
ligam, e o custo no QEMU é alguns KB de kernel. Alternativa seria um fragmento por máquina, o que a
regra 3 do `CLAUDE.md` existe para evitar.

```
CONFIG_TCG_TPM=y
CONFIG_TCG_TIS_CORE=y
CONFIG_TCG_TIS=y
CONFIG_HW_RANDOM_TPM=y
```

### 4.7. `systemd` com `tpm2`, apenas onde há TPM

`systemd-cryptenroll --tpm2-device` exige systemd compilado com suporte a TPM2, que hoje não está
(`med-os.conf:60` tem só `gcrypt cryptsetup`). Isso é `PACKAGECONFIG:append:pn-systemd = " tpm2"`,
e arrasta `tpm2-tss` para a imagem — peso que não faz sentido no perfil QEMU. Fica no
`project-eeg-stm32mp2.yml`, não no `med-os.conf`.

### 4.8. `project-eeg-qemu.yml` — o teste de ponta a ponta

```
MED_DATA_KEY_SOURCE = "development"
MED_EEG_REQUIRE_ENCRYPTION = "true"    # deixa de ser "false"
```

A segunda linha é a verificação da §2 e só deve ser trocada **depois** que o provisionamento
funcionar, para que a mudança de comportamento seja atribuível.

---

## 5. Ordem de execução

1. §4.1 + §4.2 + §4.5 — o script, a salvaguarda e a unidade, ainda com `MED_DATA_KEY_SOURCE` só
   implementando `development`. É o que faz o boot parar de falhar.
2. §4.3 — `crypttab` como template.
3. §4.4 — resolver `Requires`/`Condition`. Fazer **depois** de 1–2: antes disso, remover a condição
   transforma a falha atual em boot quebrado.
4. §4.8 — virar `MED_EEG_REQUIRE_ENCRYPTION` para `"true"` e confirmar que o serviço sobe.
5. §4.6 + §4.7 + o caminho `tpm2` — **junto com o build do STM32MP257**, não antes. Sem hardware
   para exercitar, seria metadata não verificada, que é exatamente o que este repositório passou a
   evitar.

---

## 6. Plano de verificação

**Primeiro boot** (imagem recém-construída):

```bash
journalctl -u med-data-provision -b            # formatou, e com que fonte de chave
lsblk -o NAME,TYPE,MOUNTPOINT | grep med-data  # tipo "crypt"
findmnt -n -o SOURCE /data                     # /dev/mapper/med-data
cat /sys/dev/block/$(stat -c '%Hd:%Ld' /data)/dm/uuid   # tem de começar com CRYPT-
```

A última linha é literalmente o que o `MedicalStorage.cpp:113` faz.

**Segundo boot** (idempotência): `med-data-provision` termina sem reformatar, os dados escritos no
primeiro boot sobrevivem, e `systemd-cryptsetup@med-data` fica `active`.

**Aceitação**: com `MED_EEG_REQUIRE_ENCRYPTION = "true"`, o `eeg-acquisition-service` sobe e o
`AuditEvent::SecurityEvent` de "record encryption requirement disabled by configuration" **não**
aparece mais no journal.

**Salvaguarda (§4.2)**: corromper deliberadamente o cabeçalho LUKS
(`dd if=/dev/zero of=/dev/disk/by-partlabel/med-data bs=1M count=1`) e confirmar que o próximo boot
**recusa** provisionar, registra em auditoria e falha visível — em vez de reformatar.

---

## 7. Riscos e questões em aberto

1. **A chave `development` repousa no dispositivo.** É a limitação inteira desse modo. Protege
   contra remoção física da mídia; não protege contra root no dispositivo ligado. Tem de estar
   auditada a cada boot e descrita assim no TCC, sem eufemismo.

2. **A salvaguarda da §4.2 é a parte que pode destruir dados.** Todo o resto do plano é
   recuperável; um provisionamento que reformata um dispositivo danificado não é. Se houver dúvida
   entre "virgem" e "danificado", a resposta correta é sempre recusar.

3. **TPM2 e RAUC interagem, e mal.** Selar a chave LUKS a PCRs que cobrem kernel/initrd significa
   que **uma atualização A/B muda as medições e o dispositivo deixa de destravar `/data`** — perda
   de todos os prontuários, causada por uma atualização bem-sucedida. É um problema clássico e
   precisa de resposta explícita antes de qualquer uso real: re-selar a chave no `mark-good` do
   RAUC (via handler), selar a PCRs que não cubram o rootfs, ou usar uma política de assinatura
   (`systemd-cryptenroll --tpm2-public-key`). **Nenhuma dessas foi avaliada.** Este é o item mais
   sério do plano e conecta os dois caminhos — ver `implementation_plan_rauc.md`.

4. **`/data` obrigatório muda o comportamento de falha** (§4.4). Hoje um dispositivo sem `/data`
   boota e grava no rootfs; depois desta mudança ele falha. Isso é o correto para um produto e é
   uma mudança de comportamento que precisa ser registrada, não descoberta.

5. **O `tomograph` herda tudo isso** sem ter sido considerado: ele usa o mesmo `med-image-base` e o
   mesmo `crypttab`. Confirmar que o perfil dele declara uma fonte de chave, ou que o default
   `tpm2` produz nele a falha alta esperada e não uma surpresa.

---

## 8. Resultado da execução (2026-08-16)

**Implementado e validado.** O que segue é medição, não previsão. A verificação corre por
`make check` (20 asserções), não à mão.

### 8.1. Resultado

`20/20` no primeiro boot (provisionamento), `20/20` no segundo (idempotência). As cinco asserções
que este plano acrescentou:

```
PASS  data-provisioned             o volume /data foi provisionado e aberto
PASS  data-mounted                 /data montado a partir do mapeamento dm-crypt
PASS  data-encrypted               o backing de /data é LUKS2 ativo
PASS  storage-requires-encryption  a aplicação exige backing criptografado
PASS  no-encryption-waiver         nenhum registro de auditoria dispensando criptografia
```

O teste de ponta a ponta da §2 fechou: com `MED_EEG_REQUIRE_ENCRYPTION = "true"`, o serviço de
aquisição só sobe porque o `MedicalStorage` encontrou um `dm/uuid` começando em `CRYPT-`. E o
`AuditEvent::SecurityEvent` de dispensa de criptografia sumiu do journal.

Confirmado também fora do guest, no artefato: o offset da partição `med-data` no `.wic` passou a
conter o magic LUKS `4c554b53babe`.

### 8.2. A salvaguarda da §4.2, exercitada

Zerado 1 MB no início da partição (`dd if=/dev/zero`), o próximo boot registrou:

```
refusing to provision: /dev/disk/by-partlabel/med-data is not pristine (type='' label='',
expected 'ext4'/'med-data'). This is also what a damaged LUKS header looks like, so refusing
to format over what may be patient data. This needs deliberate intervention.
```

**Não reformatou.** E a cascata é a correta: o serviço de aquisição registrou `refusing to store
medical records on the unencrypted backing of /data` e saiu; `/data` ficou **vazio**, zero
registros. O dispositivo degradou para *não gravar*, não para *gravar em claro* — que é a
distinção que importa numa avaliação de segurança.

Este é o caminho de código que, errado, apagaria prontuários. Agora está exercitado em vez de
argumentado.

### 8.3. Três desvios do plano, todos forçados pela implementação

1. **A chave de desenvolvimento mora na ESP, não no rootfs.** O plano não previu: o rootfs é um
   slot A/B, então uma chave em `/etc` seria destruída pela primeira atualização RAUC
   **bem-sucedida**, deixando o dispositivo sem conseguir decifrar os próprios prontuários. É a
   mesma classe de problema do §7.3, que eu havia atribuído só ao TPM. A ESP é a única área
   gravável deste layout que nenhuma atualização toca. Limitação declarada: vfat não carrega modo
   POSIX, então o `chmod 0400` é melhor-esforço.

2. **O `crypttab` fica vazio, não templatizado** (§4.3). O `systemd-cryptsetup` roda antes do
   `local-fs.target`, então não lê chave de um sistema de arquivos ainda não montado, e ordenar a
   montagem da ESP antes disso é ciclo de dependência. A unidade de provisionamento monta a ESP
   privadamente e é dona da abertura. O caminho `tpm2` não terá esse problema (a chave é dessatelada
   pelo TPM) e deve trazer o `crypttab` de volta.

3. **`tpm2` recusa em vez de existir pela metade.** Sem hardware para exercitar, uma implementação
   seria metadata não verificada.

### 8.4. Um defeito no próprio teste, achado pela injeção de falha

A asserção `acq-active` usava `systemctl is-active`. Um serviço `Type=simple` com
`Restart=on-failure` passa por uma janela breve de `active` a cada tentativa, então a asserção
retornava PASS para um serviço em *crash loop* — precisamente o cenário que ela existia para pegar.
Passou a exigir `NRestarts=0` também.

Vale registrar como método: **uma asserção que nunca viu a falha que procura é uma afirmação, não
uma verificação.** Só a injeção de falha distinguiu as duas.

### 8.5. O que continua em aberto

- O caminho `tpm2`, com o build do STM32MP257 (§5 passo 5). Ler o §7.3 antes.
- O §7.3 continua **não avaliado** e é o item mais sério: selar a chave a PCRs que cobrem o rootfs
  faz uma atualização A/B bem-sucedida impedir o dispositivo de decifrar `/data`. A descoberta do
  §8.3.1 mostra que o problema não é exclusivo do TPM — é a persistência da chave através de uma
  troca de slot, e vale para qualquer fonte.

---

## 9. Segunda execução (2026-08-17): custódia por máquina, e o `tpm2` que não existe

O §8.5 deixou dois itens em aberto e datou o primeiro como "com o build do STM32MP257". O build
aconteceu, e mudou o que se sabe sobre os dois.

### 9.1. O defeito escondido atrás da recusa

O `med-data-provision.sh` fixava `ESP=/dev/disk/by-partlabel/esp` e `mount -t vfat` — partição e
sistema de arquivos que existem **apenas** no layout EFI. Na STM32MP2 não há partição `esp` nenhuma.

O plano nunca notou porque `MED_DATA_KEY_SOURCE = "tpm2"` recusa antes de alcançar aquele código: um
script incapaz de funcionar naquela placa, mascarado por uma recusa que parecia ser sobre outra
coisa. **Uma recusa correta não é evidência de que o resto do caminho está correto.**

### 9.2. Custódia é propriedade da placa, não da distro

A restrição que decide onde a chave mora não vem do disco, vem do caminho de atualização — é o
achado do §8.3.1 restated: o rootfs é um slot A/B, então a chave tem de morar numa partição que
nenhum update escreve. *Qual* partição é isso é uma tabela de partições, ou seja um fato de placa.

Duas variáveis novas, substituídas em tempo de build pela receita e respondidas por `meta-med-bsp`
(a camada de adaptação criada neste mesmo dia — ver `docs/BRINGUP_STM32MP2.md` §3):

| máquina | `MED_KEY_STORE_DEV` | `MED_KEY_STORE_FSTYPE` |
|---|---|---|
| `qemux86-64` | `by-partlabel/esp` | `vfat` |
| `stm32mp25-disco` | `by-partlabel/bootfs` | `ext4` |

`bootfs` satisfaz o critério e ganha algo sobre a ESP: sendo ext4, o `chmod 0400` que o script tenta
tem efeito — no vfat nunca teve, porque vfat não carrega modos POSIX. A receita falha o build se um
alvo pedir chave `development` sem declarar onde ela mora.

Verificado extraindo o `.ipk` e lendo o script instalado nas duas máquinas; `make check` no QEMU
seguiu **21/21**, que é o que prova que a refatoração não regrediu o alvo que tem evidência.

### 9.3. Dois defeitos que a mudança expôs

**A receita era `allarch`** — declarando que o pacote é idêntico para toda máquina — enquanto seu
conteúdo já dependia de `MED_DATA_KEY_SOURCE`. Duas máquinas no mesmo `TMPDIR` escreveriam o mesmo
nome de pacote no mesmo `deploy/ipk/all/`, e a segunda sobrescreveria a primeira: um dispositivo
provisionando `/data` contra a tabela de partições de outra placa, sem nada falhar em build. Agora
`PACKAGE_ARCH = "${MACHINE_ARCH}"`.

**O overlay do kas perdia em silêncio.** O kas emite blocos de `local_conf_header` ordenados
alfabeticamente pela chave: `bringup` na linha 1 do `local.conf`, `eeg-stm32mp2` na 75, e o `tpm2`
do perfil sobrescrevia o `development` do overlay 86 linhas depois. O build passava e o pacote saía
com o valor errado. Corrigido com `MED_DATA_KEY_SOURCE:forcevariable`, último item do `OVERRIDES`.
Achado ao extrair o `.ipk` — `bitbake -e` sozinho **não** teria pego, porque o valor no datastore
estava certo e o `local.conf` o sobrescrevia depois.

### 9.4. `tpm2`: reavaliação do §5 passo 5 e do §7.3

O plano assume que `tpm2` "chega com o STM32MP257". Verificado nas camadas que o projeto usa hoje,
isso **não tem como acontecer**:

- `meta-security/meta-tpm` oferece `swtpm` e `ibmswtpm2`, que são **emuladores em software**: guardam
  o estado num arquivo do mesmo sistema de arquivos. Trocariam um arquivo de chave por outro, com
  mais passos e nenhuma raiz de confiança — exatamente o tipo de metadado de segurança não
  exercitado que este repositório parou de enviar;
- **não existe receita de fTPM** (TPM como Trusted Application dentro do OP-TEE) nem no
  `meta-security` nem no `meta-st-stm32mp` (`grep -rl ftpm` no meta-st retorna vazio).

As duas saídas reais:

1. **chip TPM discreto** no SPI ou I2C da placa — os drivers `TCG_TIS_SPI`/`TCG_TIS_I2C` já estão no
   kernel construído, como módulos. Exige confirmar se a DK traz um, e provavelmente hardware
   adicional;
2. **portar um fTPM para o OP-TEE**, que já está no FIP gravado no cartão. É a saída elegante no MP2
   e é trabalho de porte, não uma variável de configuração.

Enquanto nenhuma das duas existir, **a custódia permanece de desenvolvimento nos dois alvos**, e não
apenas no de simulação como este plano supunha.

E o §7.3 continua não avaliado, com uma qualificação: ter TPM não o resolve. Selar a chave a PCRs que
medem o rootfs faz uma atualização A/B **bem-sucedida** mudar as medições, o TPM recusar liberar a
chave, e o dispositivo perder os próprios prontuários — o mesmo desastre do §8.3.1, por outro
caminho.

### 9.5. Correção de uma alegação de segurança

O `RESULTS.md` §7 afirmava que a chave de desenvolvimento "protege contra remoção física da mídia,
não contra root no dispositivo ligado". Está invertido, e foi corrigido: a chave mora numa partição
do **mesmo disco** que o volume cifrado, então quem leva a mídia leva as duas coisas. O que a
configuração atual protege é contra exfiltração *parcial* (cópia só da `med-data`) e, via a recusa
do `MedicalStorage`, contra gravar em claro num dispositivo comprometido.

O que está validado é o **mecanismo** de volume criptografado. A custódia, não.

### 9.6. O que continua em aberto depois desta execução

- Custódia real de chave, nos dois alvos (§9.4). É agora um item de porte ou de hardware, não de
  configuração.
- O §7.3, não avaliado, e agora sabidamente não resolvido pela mera presença de um TPM.
- Nada de tudo isto foi executado na placa: o `/data` provisionando de verdade no STM32MP257 é
  previsão baseada em inspeção de pacote, não medição.
