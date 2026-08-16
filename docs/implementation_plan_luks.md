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
