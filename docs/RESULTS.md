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

> **Remedido em 2026-09-07.** A medição anterior (sessão de 5 min 22 s, 3.226 quadros de 840 bytes,
> resto zero) descrevia uma geometria de quadro que **não existe mais**: `samples_per_frame` passou
> de 25 para 14 quando o front-end analógico entrou, porque 14 é o teto de um buffer `rpmsg` padrão
> para 8 canais (§7.2 do `implementation_plan_ads1299.md`). O quadro passou de 840 para 488 bytes e
> a taxa de quadro de 10 para 17,857 Hz. Os números antigos ficam como registro histórico e **não
> descrevem a configuração entregue**.
>
> A remedição encontrou um defeito que a antiga não podia ver, e que nenhuma das 22 asserções da
> suíte conseguia ver. Está abaixo, com a correção e a injeção de falha que a valida.

### Método

Duas janelas na mesma sessão, medindo o **delta** entre dois `stat` do mesmo registro — e não
tamanho dividido por uptime, porque `/data` persiste entre boots e a sessão pode já conter execuções
anteriores. A segunda janela repete a primeira com a HMI parada, que é o que separa as duas causas
possíveis de bloqueio no laço de aquisição.

```sh
S=$(ls -d /data/eeg/session-* | tail -n 1)
date +%s; stat -c %s $S/raw.bin      # início da janela
# ... 120 s ...
date +%s; stat -c %s $S/raw.bin      # fim da janela
systemctl stop eeg-hmi.service       # entre as duas janelas
```

### Resultado — o formato está íntegro

```
quadro = 40 (FrameHeader) + 8 canais × 14 amostras × 4 bytes = 488 bytes

acumulado   983.808 bytes ÷ 488 = 2.016 quadros,  resto ZERO
delta       594.872 bytes ÷ 488 = 1.219 quadros,  resto ZERO
delta       1.056.520 bytes ÷ 488 = 2.165 quadros, resto ZERO
```

**O que isso demonstra**: o formato em disco bate com o `static_assert(sizeof(FrameHeader) == 40)`
do header, e **resto zero em três medidas independentes** significa que não houve escrita parcial
nem quadro truncado. Essa metade do resultado antigo se mantém, com outro divisor.

### Resultado — a taxa **não** é a configurada quando a HMI está conectada

| janela | duração | quadros | taxa de quadro | amostras/canal/s | contra os 250 configurados |
|---|---|---|---|---|---|
| **com** a HMI conectada | 181 s | 1.219 | 6,735 Hz | 94,3 | **−62%** |
| **sem** a HMI (parada) | 121 s | 2.165 | 17,893 Hz | 250,5 | **0%** |

Sem a HMI a aquisição entrega exatamente a taxa configurada. Com ela, entrega 38% dela — e o
`metadata.json` da sessão continua declarando `"sample_rate_hz": 250.000000`.

**A causa não é o armazenamento.** O `append` no volume LUKS sustenta 17,9 Hz sem esforço, o que
esta tabela isola. O custo por quadro decompõe-se assim:

```
período real por quadro  = 148 ms
  sleep intencional      =  56 ms  (1/17,857 Hz)
  CPU medido             = 5,1 ms  (1.026 ticks / 2.016 quadros)
  bloqueado              ≈  87 ms
```

O bloqueio é a **publicação para a HMI**, e o mecanismo está no código, não em hipótese:
`MedicalIpcServer::accept` chama `accept4(fd_, nullptr, nullptr, SOCK_CLOEXEC)` sem `SOCK_NONBLOCK`,
e `MedicalIpcChannel::send` faz um `::write()` bloqueante. Quando o buffer de recepção do
visualizador enche, o laço de aquisição para dentro do `write` até ele drenar: **a taxa de aquisição
passa a ser a taxa de consumo da tela.**

### O que isso significa, e é mais do que um número de desempenho

A `eeg-acquisition.service` ignora `SIGPIPE` com o comentário "a departing HMI must not kill
acquisition". A intenção declarada é que o visualizador não afete a aquisição. Uma HMI que **sai**
de fato não afeta; uma HMI **lenta** afeta, e em silêncio — o registro não fica menor, ele fica com
menos amostras do que afirma ter. Para um registro clínico isso é pior que uma falha: uma falha é
visível.

**Este defeito é anterior à mudança de geometria e foi exposto por ela.** A 10 Hz o visualizador
dava conta e o `write` não bloqueava; a 17,857 Hz não dá. É a mesma forma do OOM do `QB_MEM`
(`BRINGUP_HMI_QEMU.md` §9): um defeito que só aparece quando outro sai da frente.

### Correção, e o que ela mudou

Aplicada no mesmo dia, depois de a medição isolar a causa:

| | antes | depois |
|---|---|---|
| `MedicalIpcServer::accept` | `accept4(..., SOCK_CLOEXEC)` | `accept4(..., SOCK_CLOEXEC \| SOCK_NONBLOCK)` |
| `MedicalIpcChannel::send` | `write` bloqueante; qualquer erro é erro | `EAGAIN` → `Status::WouldBlock`, distinto |
| serviço de aquisição | qualquer status ≠ Ok derruba o visualizador | `WouldBlock` mantém o visualizador e conta o quadro |
| registro | perda invisível | `viewer missed` no evento `AcquisitionStopped` |

`Status::WouldBlock` foi acrescentado **depois** de `Internal`, de modo que nenhum enumerador
existente muda de valor. Ele não vem de `statusFromErrno`: lá `EAGAIN` continua sendo `Timeout`, que
é o correto para `receive()` ("nada chegou na janela que você pediu"). No `send()` não existe janela,
então `EAGAIN` só pode significar "o par está atrasado" — e é essa distinção que permite ao chamador
não confundir um visualizador **lento** com um visualizador **ausente**. O canal obtido por
`connect()` continua bloqueante de propósito: quem fala com um coprocessador quer que o `write`
complete; quem publica para visualizadores nunca pode esperar por um.

A escolha de projeto por trás disso, dita uma vez: **um visualizador pode perder dados; o registro
não.** Era o que a unit já declarava ao ignorar `SIGPIPE`, e agora é o que o código faz.

### A asserção que faltava

`acq-sample-rate` (23ª) compara a taxa **entregue** com a **configurada**, com a HMI conectada,
tolerância de 10% numa janela de 20 s. Ela lê a geometria do quadro do próprio `eeg.conf` em vez de
embutir 488 bytes — senão passaria a mentir no dia em que a geometria mudasse, que é exatamente o
dia em que precisa funcionar.

Ela existe porque as outras 22 **não podiam** ver este defeito: todas medem *estado* (ativo, sem
reinícios, socket publicado), e o serviço estava perfeitamente ativo enquanto perdia 62% das
amostras. Medir estado não é medir função.

**E ela viu a falha que procura**, o que é a condição para contar como verificação neste
repositório. Injetada revertendo **uma única flag** — `SOCK_NONBLOCK` fora do `accept4`, todo o
resto da correção intacto — e reconstruindo a imagem:

| imagem | resultado |
|---|---|
| com `SOCK_NONBLOCK` | **23/23** |
| sem `SOCK_NONBLOCK` | **22/23**, única falha `acq-sample-rate` → `RATE_LOW` |

As outras 22 continuaram verdes na imagem defeituosa, o que fecha o argumento nos dois sentidos: a
asserção nova isola exatamente o defeito para o qual foi escrita, e as antigas de fato não
conseguiam vê-lo.

**Limite**: sob QEMU, sem garantia de tempo real. Estes números são de *vazão e correção de
formato*, **não** de latência. Latência só tem significado no STM32MP257, e continua não medida.

---

### Resultado — o front-end real, pelo link `usb`, num host (2026-10-04)

Primeira aquisição com silício. A ponte MCP2210 foi plugada num PC (Ubuntu, kernel 6.8, Secure Boot
com uma chave de bancada), com o `hid-mcp2210` e o `ti-ads1299` do `linux-med` compilados para esse
kernel, e o ADS1299 numa placa de AFE **sem eletrodos** (entradas flutuando; toda medida abaixo usa
entradas internas do MUX). O registro completo, com cada comando, está em `BRINGUP_AFE.md` §2–§6.
**Nada disto é da STM32MP257**: é o mesmo código de driver num kernel e num controlador USB
diferentes.

| Medida | Resultado | Comando |
|---|---|---|
| Identidade e reset | ID `0x3E` (ADS1299, 8 canais); os 23 registradores de configuração no valor da Tabela 11 | `afe-spi-oracle.py`, sem driver |
| Amplitude do gerador (a ambiguidade A1) | ±VREF/2400 de **pico**; 167 259–167 335 códigos pp nos 8 canais, contra 167 772 | idem |
| Tabela de ganhos e escala | `raw × scale` constante em 0,72% (1x) e 0,34% (2x) nos 7 ganhos; a −0,04% e −0,18% do previsto | `afe-phase4.py` |
| Ruído com entradas em curto, ganho 24 | 0,13 µV RMS, 0,63 µV pp (40 conversões avulsas) | idem |
| MUX comuta | em curto, ligar o gerador move a leitura 2 códigos, contra 167 300 | idem |
| Taxa do link `usb`, a 250 SPS | **167/s** com o driver original (3 trocas HID × 2 ms); **250/s** depois de não reenviar transfer settings inalteradas | `afe-spi-oracle.py timing`, `afe-phase4.py --only ac` |
| Sessão de 30 min, 250 SPS | 449 998 devidas, 449 873 entregues, **125 perdidas (0,0278%)**, diferença não contada **0**; **0** amostras corrompidas | `afe-phase5-long.py` |

**Três defeitos que só a execução viu**, nenhum visível a build nem à suíte de host como estava:

1. **Desplugar a ponte derrubava o kernel** (`kernel BUG at mm/slub.c:553`): o driver não tinha
   `.remove`, o núcleo HID parava o hardware e a ação devm parava de novo, e o `usbhid` liberava os
   mesmos buffers duas vezes. Corrigido com um `.remove` vazio. A suíte de host agora reproduz a
   ordem de remoção do núcleo HID.
2. **O autoteste de probe reprovava silício bom**, por desenho. Em single-shot, cada START reinicia o
   divisor do gerador de teste, e a onda de ~1 Hz nunca parece oscilar. Valeria também na ligação
   `spi`. Corrigido medindo o nível DC do gerador contra o offset em curto: 83 542–83 829 códigos
   contra 83 886.
3. **A ponte reenviava as transfer settings antes de cada transação**, e a 250 SPS um terço das
   amostras se perdia. Isso não aparece como erro, só como taxa: o contador do driver acusava a
   perda corretamente, e o serviço teria registrado uma sessão com um terço a menos do que declarava.

**O que isto significa**: o teto do link `usb` é do protocolo de comando e resposta da ponte sobre o
quadro de 1 ms do USB full-speed. São duas trocas no mínimo por amostra, ~4 ms, ou seja, **~250
amostras por segundo, que é a menor taxa do conversor**. O link funciona exatamente na borda, com
margem zero: o p90 de uma leitura é 4,084 ms. A perda medida não é zero, e não há como torná-la
zero com garantia neste link. É um argumento quantitativo, e não de adjetivo, para a topologia de
produto ser a do coprocessador (`amp`).

**E o que um registro precisa saber**: em 30 minutos houve 125 perdas, mas só 49 lacunas visíveis
nos timestamps, com o maior intervalo de 9,04 ms (≤ 2 amostras). Depois de uma leitura atrasada o
driver dispara a seguinte imediatamente, e a amostra de recuperação chega com um intervalo de
aparência normal. **A perda neste link não pode ser inferida dos timestamps**; só o `lost_samples`
do driver a conta, e foi exato na sessão inteira. O serviço de aquisição tem de levar esse número
para o registro.

**O que isto não significa**: nada sobre a placa (6.6, outro controlador USB); nada sob carga (o PC
estava ocioso); nada sobre as ligações `spi` e `amp`, então a comparação entre ligações continua por
fazer; nada sobre o caminho do eletrodo (sem eletrodos, sem fonte DC externa); e o detector de
corrupção só enxerga erros acima de 5% da excursão do sinal de teste.

### Resultado — o mesmo front-end, pelo link `usb`, na STM32MP257 (2026-10-04)

A ponte numa porta USB da STM32MP257F-DK, com o kernel `6.6.129-gb8dbcb083402` do `linux-med`
(cartão regravado: o kernel mora na `med-boot`, que nenhum bundle troca) e o
`eeg-acquisition-service` gravando no `/data` criptografado. É a primeira aquisição de silício
pela plataforma inteira, do driver de kernel ao registro de sessão, no alvo físico.

```
$ make stm32 KERNEL=med KEY=development       # depois: make bundle BOARD=stm32 KERNEL=med KEY=development
mcp2210 0003:04D8:00DE.0001: USB-SPI bridge ready, 9 GPIOs, 8 chip selects, ads1299 attached
ads1299 spi0.4: self test passed: test signal DC level 83453..83783 codes from offset (expected 83886), shorted-input noise 117 codes
```

| Medida, 20 s | Resultado |
|---|---|
| gravado em `raw.bin` | 355 quadros × 488 bytes = **4970 amostras, 248/s** |
| perdidas (`lost_samples`) | **34**; 4970 + 34 = 5004, contra ~5002 devidas |
| perda | **0,68%**, contra 0,03% no PC ocioso |
| ruído em curto no autoteste | 117 e 49 códigos, em dois boots, contra 23 no PC |

**Três defeitos que só a placa mostrou, nenhum visível a build, QEMU ou PC** (`BRINGUP_AFE.md` §6.4):
o sandbox da unit montava `/sys` só leitura e o link `iio` configura o conversor por sysfs (54
reinícios); o motivo da falha não chegava ao log; e cada tentativa falha deixava uma sessão vazia
no registro. Mais um, que é deste documento: **o slot A/B era confirmado com a aquisição quebrada**
(§8, "o fallback").

**O que isto não significa**: uma janela de 20 s, não uma sessão longa. A perda e o ruído maiores
que no PC são observações e não conclusões, até uma sessão longa na placa. Nada sobre jitter, CPU
ou carga, e nada ainda sobre eletrodos.

**A sessão longa, e a causa da perda** (mesmo dia, `BRINGUP_AFE.md` §6.5). Em 30 min com o
governador padrão (`schedutil`), a perda foi de **0,67%**, com 0 saltos de sequência entre os
quadros gravados, intervalo entre quadros com mediana de 56,01 ms (esperado 56) e p99,9 de 62,9 ms,
e o serviço usando ~1,4% de um núcleo. A causa foi isolada trocando só o governador de frequência
da CPU:

| Governador | Trocas de frequência/s | Perda |
|---|---|---|
| `schedutil` (padrão) | ~86 | 0,63–0,72% |
| `ondemand`, parado em 1,2 GHz | 0–0,1 | 0,005–0,021% |
| `performance`, fixo em 1,5 GHz, 30 min | 0 | **0,000–0,003%** |

**A perda acompanha a taxa de troca de frequência, não a frequência.** Sem trocas, o link `usb`
na placa perde tanto quanto no PC ocioso. É um fato de plataforma, e não do front-end: qualquer
aquisição temporizada pelo host nesta placa paga pelo DVFS. A política que o corrige ainda não foi
escolhida, e o custo de energia de cada uma não foi medido.

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
`MED_BOOTLOADER = "noop"`. A §8 estende este resultado ao alvo físico, onde o backend é `uboot` de
verdade e o ambiente do bootloader é reordenado para `BOOT_ORDER=B A`; o que continua sem medição,
nos dois alvos, é um boot que **use** essa ordem. Além disso, `boot-attempts` é rejeitado pelo RAUC para qualquer backend
que não seja `uboot`/`barebox`, então o mecanismo de fallback do QEMU seria diferente do que o
STM32MP257 usa. Ver `implementation_plan_rauc.md` §3. **Atualização de 2026-10-04**: no
STM32MP257, a troca de slot (2026-08-31) e o fallback por falha injetada (§8) estão medidos; no
QEMU, continua valendo o que este parágrafo diz.

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
- **`fip-a/b` e `metadata1/2` colidem por nome com o eMMC de fábrica** e não se determinou qual
  disco venceu (saída truncada no terminal). O `u-boot-env`, que era o caso que importava, foi
  medido depois — ver abaixo.

#### Segunda sessão na placa: `/etc/fw_env.config` validado

Escrito à mão no rootfs (o perfil de desenvolvimento monta `rw`), com o mesmo conteúdo que a receita
`med-uboot-env-config` gera — de modo que o arquivo foi validado sem reconstruir a imagem.

| # | O que se mediu | Comando | Resultado |
|---|---|---|---|
| 12 | Para onde vai o nome que a ST usaria | `readlink -f /dev/disk/by-partlabel/u-boot-env` | **`/dev/mmcblk2p5`** — o eMMC de fábrica |
| 13 | Para onde vai o nosso endereço | `readlink -f /dev/disk/by-partuuid/d7ba3548-…` | `/dev/mmcblk0p7` — o cartão |
| 14 | O ambiente do U-Boot é legível e gravável | `fw_setenv BOOT_ORDER "A B" && fw_printenv BOOT_ORDER` | `BOOT_ORDER=A B` |
| 15 | O RAUC passa a ler o ambiente | `rauc status` | erro muda de `fw_printenv failed` para `Unable to find primary boot slot` |

A linha 12 é o resultado que fecha a discussão sobre endereçamento: o `fw_env.config.mmc` que a ST
distribui teria apontado o `fw_setenv` para o ambiente de bootloader **de outro sistema operacional**
— e teria funcionado em silêncio, até alguém perguntar por que uma troca de slot não surtia efeito.

A linha 14 exercita as três decisões do arquivo de uma vez: endereço por PARTUUID, `ENV_SIZE` de
`0x2000`, e o formato redundante de duas entradas em `-0x2000`/`-0x4000`, que **diverge** do arquivo
da ST (ele lista `-0x2000` duas vezes). As duas primeiras eram indícios; a terceira era derivação a
partir do `env/mmc.c` do U-Boot. As três passam.

A linha 15 é progresso e não um segundo defeito. O backend `uboot` do RAUC escolhe o slot primário
percorrendo `BOOT_ORDER` e pegando o primeiro cujo `BOOT_<bootname>_LEFT` seja maior que zero;
`BOOT_A_LEFT` e `BOOT_B_LEFT` não existem, porque o `.wks` cria a partição zerada e nada a semeou
ainda. É a mesma ausência que faz os dois slots aparecerem como `boot status: bad`. Semear é
atribuição do `rauc status mark-good`, que não teve efeito no primeiro boot porque o `fw_setenv`
falhava.

**Ainda não medido**: se o `mark-good` semeia corretamente, se o RAUC passa a reportar um slot
primário, e a instalação de um bundle na placa. A seleção de slot em si continua não implementada —
o `extlinux.conf` fixa o slot A, independentemente do que o `BOOT_ORDER` diga.

### Resultado — instalação de bundle assinado na placa (2026-08-30)

A §6 mede o caminho de atualização no QEMU. Esta subseção mede o mesmo caminho no alvo físico, que é
o que a §9 listava como não medido.

#### Método

```bash
make stm32 KEY=development        # imagem, com CONFIG_DM_VERITY já no fragmento de kernel
make bundle BOARD=stm32 KEY=development
make verify-bundle BOARD=stm32    # host: verifica com as opções do dispositivo
# placa: fw_setenv BOOT_ORDER "A B"; fw_setenv BOOT_A_LEFT 3; fw_setenv BOOT_B_LEFT 0
#        rauc info <bundle>; rauc install <bundle>; rauc status
```

O bundle foi transferido por `scp` sobre IPv6 link-local para `/tmp` (tmpfs) da placa.

#### Resultado

| medida | valor |
|---|---|
| Bundle | `med-bundle-eeg-stm32mp25-disco.raucb`, 115.475.853 B, `Build: '20260828034740'` |
| Assinatura, verificada **na placa** contra o chaveiro **da placa** | `Verified inline signature by 'O = MedPlatform, OU = Update Infrastructure, CN = MedPlatform Bundle Signing'` |
| Política de verificação em vigor | `check-purpose=codesign`, `check-crl=true`, cadeia de 2 certificados |
| `Compatible` do bundle × do dispositivo | `med-os-stm32mp25-disco` — iguais |
| Formato | `verity` (salt `7ca3ad0e…`, hash `9c0d0bbb…`, árvore de 909.312 B) |
| `rauc install` | `succeeded`, sem advertências |
| Slot escrito | `rootfs.1` (B), o inativo — escolhido pelo RAUC a partir do slot bootado |
| Payload escrito | 747.560.960 B numa partição de 1024 MiB |
| **Conteúdo do slot, conferido byte a byte** | `dd … bs=4096 count=182510 \| sha256sum` = `4b07dae3…345c` = o `Checksum` do manifesto |
| Estado após o install | `Activated: rootfs.1 (B)`, ambos os slots `good` |
| Ambiente do bootloader após o install | `BOOT_ORDER=B A`, `BOOT_A_LEFT=3`, `BOOT_B_LEFT=3` |

O `BOOT_ORDER=B A` é o número que sustenta a alegação de reversibilidade: o RAUC **reordenou** a
lista mantendo o slot anterior como fallback, em vez de substituí-la.

A verificação byte a byte é deliberada e não redundante. O `install` relatar `succeeded` é o
relatório da ferramenta sobre si mesma; o `sha256sum` da partição é o artefato. O handler para
`ext4→ext4` é `img_to_fs_handler` = `write_image_to_dev()` cru, sem `resize` declarado no
`system.conf`, então a igualdade com o checksum do manifesto é exigível.

#### A condição que precisou ser criada — e o defeito que ela revelou

Antes deste resultado, `rauc install` **não podia** funcionar nesta placa, por uma razão que nenhum
build reportava: `med-kernel-features.cfg` exigia `CONFIG_SQUASHFS` e nunca exigiu
`CONFIG_DM_VERITY`, embora o `system.conf` da distro declare `bundle-formats=-plain` e o bundle seja
`verity`. O defconfig do `linux-yocto` traz `DM_VERITY=y` e o do `linux-stm32mp` não — o QEMU
passava por herança do vendor. É o **oitavo** defeito da família da §1, e o mesmo mecanismo do
sétimo: uma política da distro que só existia como default de terceiro.

Duas coisas foram medidas, não deduzidas: `CONFIG_DM_VERITY=y` e `CONFIG_BLK_DEV_LOOP=y` no
`config-6.6.129` **produzido**, e o `md5sum` do `Image.gz` do rootfs igual ao do kernel implantado —
o que prova que é esse kernel que chega à partição `med-boot`.

E um segundo defeito, este ainda aberto: **`BOOT_ORDER` não é escrito por ninguém no
provisionamento**. O `.wks` cria `u-boot-env` com `--source empty`, o RAUC só grava essa variável
durante um install, e `rauc status mark-good` grava apenas `BOOT_<slot>_LEFT`. Um dispositivo
recém-gravado reporta *todos* os slots `bad` — inclusive o que está executando — e `Activated: none`.
O valor inicial foi semeado à mão para esta medição (`BOOT_ORDER="A B"`, `BOOT_A_LEFT=3`,
`BOOT_B_LEFT=0`). Ver `BRINGUP_STM32MP2.md` §9.10.

#### O slot escrito boota (mesma sessão)

A comparação byte a byte prova o conteúdo, não a bootabilidade. Com o `root=PARTUUID=` do
`extlinux.conf` editado à mão para o slot B e a placa reiniciada:

| medida | valor |
|---|---|
| Linha de comando do kernel | `root=PARTUUID=2997c20d-239f-43cd-8658-9c7010186c9b` — o slot B |
| Slot em execução, segundo o próprio sistema | `Booted from: rootfs.1 (/dev/mmcblk0p10)` |
| Estado do RAUC visto de dentro do slot B | `Activated: rootfs.1 (B)`, B `booted`/`good`, A `inactive`/`good` |
| Serviços | multi-user, rede, SSH e `/data` LUKS abertos normalmente |

O que isso mede: **o rootfs que o RAUC escreveu é intercambiável com o original.** Nada dentro do
sistema nomeia um slot — o `fstab` monta `/` como `/dev/root`, o `system.conf` endereça os slots por
`by-partlabel`, o `crypttab` está vazio por projeto. A única coisa que nomeia o slot A é o
`extlinux.conf`, que é justamente a peça a ser substituída.

O que isso **não** mede: a troca de slot. Foi um arquivo de texto editado à mão, não uma decisão do
dispositivo. Ver `BOOT_SLOT_AB_STM32MP2.md`.

##### Dois defeitos que esse boot revelou, e que não são sobre boot

1. **Cada slot tem identidade de máquina própria.** O rootfs não traz chave de host SSH nem
   `machine-id` (`/etc/machine-id` com 0 bytes), então cada slot gera as suas no primeiro boot. A
   fingerprint SSH da placa mudou entre os dois slots, mesmo endereço, mesmo hardware. Uma
   atualização A/B, portanto, **troca a identidade do dispositivo**.
2. **A trilha de auditoria mora dentro de um slot A/B.** `Storage=persistent` grava em
   `/var/log/journal/<machine-id>/`, no rootfs. A próxima atualização escreve por cima de um dos dois
   históricos, e o selo (FSS) é por `machine-id`, que também é por slot. É a mesma armadilha que o
   `implementation_plan_luks.md` resolveu para a chave do `/data` — *"uma chave no rootfs seria
   destruída pela primeira atualização bem-sucedida"* — repetida para a evidência de rastreabilidade
   IEC 62304. Previsto por inspeção do artefato; **ainda não observado na placa**.

#### Seleção de slot pelo bootloader (2026-08-31)

O que faltava para a política A/B existir no dispositivo e não só no `rauc status`: um `bootcmd` que
lê `BOOT_ORDER`. Ele agora vem na imagem — `med-uboot-env-image` monta um ambiente de U-Boot de
512 KiB que o wic escreve na partição `u-boot-env` por `rawcopy`.

**Método**: `make stm32 KEY=development`, `bmaptool`, `sgdisk -e`, ligar. **Nenhum comando na placa
antes da medição** — é essa a condição que o resultado afirma.

| medida | valor |
|---|---|
| Linha de comando do kernel | `root=PARTUUID=e91c4e10-…  rauc.slot=A` |
| Slot escolhido | A, o primeiro de `BOOT_ORDER=A B` com contador > 0 |
| Estado do RAUC num aparelho recém-gravado | `Activated: rootfs.0 (A)`; A `good`, B `bad` |
| `rauc-mark-good.service` | `active (exited)`, `status=0/SUCCESS` |
| Contador depois do boot | `BOOT_A_LEFT=3` — o bootloader gastou uma tentativa, o userspace restaurou |
| `BOOT_B_LEFT` | `0` — slot B nunca foi escrito neste cartão, e o valor diz isso |
| Ambiente no cartão × no `.wic` | `sha256` idêntico (`8fc40565…`), 129 de 129 blocos cobertos pelo `.bmap` |

Três coisas que este número fecha, e que estavam abertas desde o início do porte:

1. **`Activated: none` num aparelho novo deixou de existir.** Antes, nada escrevia `BOOT_ORDER` em
   momento algum da vida do dispositivo: o wic criava a partição vazia, o RAUC só a escreve durante
   um install, e `mark-good` escreve apenas o contador. Um cartão recém-gravado reportava *todos* os
   slots `bad`, inclusive o que estava executando.
2. **`rauc-mark-good.service` rodou.** A unidade era pulada por
   `ConditionKernelCommandLine=|rauc.slot` em todos os boots desde 2026-08-18 — e uma unidade pulada
   por `Condition*` não falha, não avisa e não aparece em `--state=failed`.
3. **O RAUC identifica o slot pelo bootloader**, não pelo dispositivo de root: `Booted from:
   rootfs.0 (A)` em vez de `rootfs.0 (/dev/mmcblk0p9)`. Com quatro enumerações diferentes medidas no
   mesmo hardware, isso troca uma inferência sujeita a corrida por uma declaração.

#### A troca de slot (2026-08-31)

O ciclo completo, na mesma sessão. Bundle reconstruído da imagem que a placa executa
(`Build: '20260831034731'`, payload 747.569.152 B), instalado e a placa reiniciada.

| medida | valor |
|---|---|
| Slot escrito e conferido byte a byte | `bb6cc1e7…` = `Checksum` do manifesto |
| Ambiente do bootloader após o install | `BOOT_ORDER=B A`, `BOOT_B_LEFT=3`, **`BOOT_A_LEFT=3`** |
| Linha de comando após o reboot | `root=PARTUUID=35822773-…`, `rauc.slot=B` |
| Slot em execução | `Booted from: rootfs.1 (B)`, `Activated: rootfs.1 (B)` |
| Slot anterior | `inactive`, `boot status: good` — segue elegível |

O `BOOT_A_LEFT=3` intocado é metade do resultado, e a metade que sustenta a alegação regulatória: a
atualização é **reversível**, porque o slot anterior continua bootável e continua na ordem. O RAUC
reordenou uma lista, não a substituiu.

Com isto, a frase da §6 muda pela primeira vez desde que foi escrita. Era *"a política A/B está
validada no QEMU, a integração com o bootloader não está validada em lugar nenhum"*. Passa a ser:
**a política A/B está validada no QEMU, e no STM32MP257 estão validadas a instalação, a ativação e a
troca de slot — o fallback não.** Em 2026-10-04 mudou outra vez: **o fallback também**, por injeção
de falha (ver "o fallback, por injeção de falha", abaixo).

##### O que ainda **não** foi medido

- ~~**O fallback.**~~ Medido em 2026-10-04, com uma aquisição que não parte como falha injetada
  (abaixo). Continua não medido para um kernel que entra em pânico.
- **Nada disso tem rede de proteção automatizada.** O `make check` roda no QEMU, onde
  `MED_BOOTLOADER` é `noop`; nenhuma das 21 asserções pode ver uma regressão aqui. A bancada é o
  único teste.

#### O que este resultado **não** significa

1. **Não é a troca de slot.** O `extlinux.conf` continua fixando `root=PARTUUID=e91c4e10-…`, o slot
   A. `BOOT_ORDER=B A` não tem leitor: o script de U-Boot não existe. Um reboot boota A.
2. **Não é um dispositivo em estado de fábrica.** `BOOT_ORDER` foi semeado à mão; sem isso o install
   ainda ocorreria, mas o estado antes/depois não seria comparável.
3. **Não é a configuração de produto.** `KEY=development`, `MED_DATA_KEY_SOURCE` de desenvolvimento,
   e o bundle assinado pela CA de desenvolvimento.

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
4. ~~**Não significa que o RAUC consegue marcar um slot.**~~ **Superado em 2026-08-30**: marca,
   ativa e instala — a subseção seguinte mede. Mantido riscado porque a afirmação estava certa
   quando escrita: `u-boot-fw-config-stm32mp` instala `fw_env.config.mmc`, `.nand` e `.nor` e
   **nunca** `/etc/fw_env.config`, e em 2026-08-18 isso produziu `fw_printenv failed with exit
   code: 1`, ambos os slots `bad`, `Activated: none`. A correção foi a receita
   `med-uboot-env-config` em `meta-med-bsp`, que escreve esse arquivo endereçando a partição por
   PARTUUID.
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

### Resultado — a árvore do kernel como fonte de verdade (2026-09-27)

Os drivers do front-end saíram das receitas *out-of-tree* e passaram a viver numa árvore Linux que é
a única fonte do código deles; este repositório integra e configura. O registro de engenharia é o
`BRINGUP_AFE.md` §12; aqui ficam os números.

#### A equivalência, provada antes de migrar

Trocar a aquisição do kernel só é seguro se a árvore Git for demonstravelmente a mesma que o build já
usava. `scripts/med-kernel-fingerprint.sh` compara **conteúdo** e nunca metadados — modo, mtime e
diretórios vazios divergem legitimamente entre tarball desempacotado e checkout git, e nenhum deles
muda o kernel compilado.

```sh
scripts/med-kernel-fingerprint.sh build/tmp-glibc/work-shared/stm32mp25-disco/kernel-source
scripts/med-kernel-fingerprint.sh linux-med      # checkout em v6.6-stm32mp-r3.1
```

| | |
|---|---|
| Base | 6.6.129 + patch `r3.1` da ST (4,9 MB, 162.253 linhas, **749 arquivos**) |
| sha256 do tarball | confere com o `SRC_URI[kernel.sha256sum]` da receita |
| Ponto Git | `v6.6-stm32mp-r3.1` → `548f960c059bc4b9165cb69895fd67551f6061ca`, conferido por `git tag --points-at` |
| Arquivos, cada lado | 81889 |
| Comuns **efetivamente comparados** | 81889 |
| Com hash divergente | **0** |
| Digest das duas árvores | `35e19311ab288cf56345ea6396d47d0cc071dab2dfe7ed028c575f2202b3bf50` |

**O que isso não significa**: não é uma afirmação sobre o driver nem sobre a placa. É só a prova de
que adotar a árvore Git não muda uma linha do kernel que a placa já executou.

#### Os dois builds

| Build | Resultado |
|---|---|
| `make qemu` | 5653 tarefas, todas bem-sucedidas. Nenhum `ti-ads1299.ko` produzido, que é o correto: o `linux-yocto` não tem o driver |
| `make stm32` com `MED_KERNEL_GIT`/`MED_KERNEL_SRCREV` | 5596 tarefas, todas bem-sucedidas. `PV = 6.6.129-stm32mp-r3.1+medda723f985714`, `S` terminando em `/git`, `SRCREV` fixo; `CONFIG_TI_ADS1299=m` no `.config` final e `kernel-module-ti-ads1299` empacotado |

O kernel se identifica pelo commit sem ninguém pedir: o arquivo de configuração entregue chama-se
`config-6.6.129-gda723f985714`, porque o `setlocalversion` lê o git da árvore. Nenhum artefato desse
build pode ser confundido com o release da ST.

#### O particionamento de software, medido no manifesto e não no argumento

Esta é a medida que importa para a §4, e ela **reprovou antes de passar**.

```sh
grep -E 'ads1299|^kernel-modules ' \
  build/tmp-glibc/deploy/images/stm32mp25-disco/med-image-eeg-stm32mp25-disco.rootfs.manifest
grep -c '^CONFIG_TI_ADS1299' \
  build/tmp-glibc/deploy/images/stm32mp25-disco/kernel/config-6.6.129-g*
```

| | antes | depois |
|---|---|---|
| Linhas com `ads1299` no manifesto (perfil `amp`) | **1** — `kernel-module-ti-ads1299` | **0** |
| `CONFIG_TI_ADS1299` no `.config` | `=m` | **ausente** |
| Tarefas do build | 5596, todas bem-sucedidas | 5596, todas bem-sucedidas |

A linha de baixo é o ponto: **os dois builds foram verdes.** O driver do conversor estava na imagem
de produto — cujo argumento inteiro é que o conversor vive no coprocessador e o lado Linux não carrega
código de conversor — e nada reclamou. A causa é o metapacote `kernel-modules`, de que a imagem
depende e que o `kernel.bbclass` faz depender dos ~1040 módulos que o kernel constrói. Ler o
manifesto é o que achou; o código de saída não diria nunca.

**O que isso significa para a §4**: a afirmação de particionamento do perfil de produto passa a ter
uma medida de manifesto por trás, e não só o desenho das camadas. **O que não significa**: nada sobre
execução — nenhum módulo foi carregado, nenhuma amostra adquirida.

#### A guarda de configuração de kernel

Substitui uma verificação que a migração destruiu: um módulo *out-of-tree* falha ao compilar contra
um kernel que não tem o que ele precisa, e esse acidente acabou. `do_med_check_kernel_config` lê o
`.config` **final** e o compara com os fragmentos que o MedOS declara como requisito seu.

| Execução | Resultado |
|---|---|
| Lógica, contra o `.config` da placa já construído | 63 símbolos pedidos, **63 satisfeitos** |
| Primeira execução real (`make qemu`, símbolo no fragmento errado) | **reprovou**, nomeando `CONFIG_TI_ADS1299` |
| `make qemu` depois da correção | passou, nos dois kernels |
| `make stm32` com o fork, ligação `amp` | passou; o fragmento do driver não é aplicado nesse perfil |
| **Injeção de falha** no perfil STM32, ligação `spi` | **reprovou** |

A linha do meio é o primeiro resultado: a guarda **viu a falha que procura na primeira vez que
rodou**, o que pela regra da §10 é a diferença entre verificação e afirmação. E o que ela encontrou
não era o esperado — era a fronteira entre capacidade de distro e conteúdo de uma árvore de kernel
específica (`BRINGUP_AFE.md` §11 D5).

A última linha é a injeção de falha deliberada, e ela fecha o modelo. Um commit temporário no fork
removeu a entrada `config TI_ADS1299` do `drivers/iio/adc/Kconfig`, e o build do kernel com
`MED_EEG_LINK = "spi"` deu:

```
do_med_check_kernel_config: Failed
  CONFIG_TI_ADS1299: asked for by med-stm32mp-drivers.cfg, not in the final .config
Tasks Summary: Attempted 1277 tasks of which 1266 didn't need to be rerun and 1 failed.
```

Ela prova **duas** coisas de uma vez, e isso era o desenho do teste: o fragmento do BSP *é* aplicado
na ligação que usa o driver — senão aquele símbolo não estaria sendo conferido — e a ausência dele *é*
detectada no perfil do alvo físico. A mensagem nomeia o fragmento que pediu, que é o que separa "falta
um símbolo" de "qual arquivo deste repositório mentiu". O commit temporário foi descartado depois
(`git reset --hard`), e a branch voltou a `da723f985714`.

O modelo que isso congela, as três linhas medidas:

| Perfil | Fragmento do driver | Guarda | Build |
|---|---|---|---|
| QEMU / `linux-yocto` | não aplicado | não exige o símbolo | passa |
| STM32 / fork, `amp` | não aplicado | não exige o símbolo | passa |
| STM32 / fork, `spi` | aplicado | exige, e o símbolo existe | passa |
| STM32 / fork, `spi`, `Kconfig` quebrado | aplicado | exige, e o símbolo falta | **falha, nomeando o fragmento** |

---

### Resultado — o fallback, por injeção de falha (2026-10-04)

**Antes de medir, foi preciso corrigir o que "slot bom" significava.** O `rauc-mark-good.service`
confirmava o slot aos ~12,9 s de qualquer boot, porque espera a `boot-complete.target` e nada
exigia essa target. E o serviço de aquisição chamava `markBootedGood()` antes do `device.start()`.
Na placa, com a partida falhando 54 vezes, o slot foi confirmado em todas. Correção: *boot
assessment* do systemd. A unit virou `Type=notify` e passou a ser exigida pela
`boot-complete.target`, com `READY=1` por `MedicalUpdate::reportReady()` depois do
`device.start()`. O SO passou a ser o único escritor dos contadores.

| Boot | `boot-complete.target` | `BOOT_A_LEFT` | Slot |
|---|---|---|---|
| normal: pronto 12,53 s, target 12,59 s, `marked … rootfs.0 as good` 13,64 s | atingida | 3 | A |
| aquisição não parte (injetada), 1º | não atingida | **2** | A |
| idem, 2º | não atingida | **1** | A |
| idem, 3º | não atingida | **0** | A |
| seguinte, sem intervenção | atingida | 0 | **B** (`rauc.slot=B`, marcado `good`) |

Nenhum `fw_setenv`. Recuperado com `rauc install` no slot A (`BOOT_A_LEFT=3`).

**O que isto não significa**: nada sobre um kernel em pânico, que não foi a falha injetada. E o
`rauc status` disse `good` durante as três falhas: com o backend U-Boot isso quer dizer "restam
tentativas", não "confirmado", e só o contador distingue os dois. Detalhes em
`implementation_plan_uboot_ab.md` §8, "passo 6".

---

## 9. O que **não** foi medido

Registrado explicitamente para que a ausência não seja lida como resultado:

- **Caminho AMP / `rpmsg` / Cortex-M33** — o QEMU não tem co-processador. O driver medido é o
  `simulated`. O alvo que tem co-processador foi energizado em 2026-08-18 e o kernel reporta
  `remoteproc remoteproc1: m33 is available`, mas nenhum firmware foi carregado nele: o driver
  `rpmsg` continua não exercitado em alvo nenhum. O boot acrescentou uma restrição ao plano —
  `stm32-rproc 0.m33: Support of signed firmware only`.
- **Latência e jitter de tempo real** — o timing do QEMU não é significativo.
- **Integração com bootloader e fallback A/B em boot falho** — **medidos no STM32MP257 em
  2026-08-31 (troca) e 2026-10-04 (fallback, §8)**; o texto a seguir é de antes. §6. A §8 não muda isso, nem depois
  do `rauc install` de 2026-08-30: no STM32MP257 a seleção de slot **não está implementada**
  (`med-boot` compartilhada, `extlinux.conf` fixando o slot A), então `BOOT_ORDER=B A` é escrito e
  nunca lido; no QEMU o mecanismo é outro. Também não medido: o **estado de fábrica** desse
  ambiente, que hoje não é escrito por ninguém e faz um aparelho novo reportar todos os slots
  `bad` (`BRINGUP_STM32MP2.md` §9.10).
- **A imagem de produto no STM32MP257** — tudo o que executou na placa foi
  `make stm32 KEY=development`. Não foi executado o perfil como o arquivo de projeto o declara
  (`MED_DATA_KEY_SOURCE = "tpm2"`, que recusa provisionar). **Continua não medida** a seleção de
  slot A/B: o `extlinux.conf` fixa o PARTUUID do slot A, então nem o boot no slot recém-escrito nem
  o fallback para o slot anterior foram observados em lugar nenhum. A instalação de bundle na placa
  deixou esta lista em 2026-08-30 (§8), e a HMI em 2026-08-27 (`BRINGUP_HMI_STM32MP2.md`).
- **Estabilidade do serviço de aquisição na placa** — subiu, mas o log de boot traz dois
  `Started …` e o `NRestarts` não foi consultado. Não se pode afirmar que roda sem reiniciar.
  **Atualização de 2026-10-04**: no link `usb`, `NRestarts=0` depois das correções (§3); em sessão
  longa, ainda não medido.
- **Carimbo de tempo confiável no alvo físico** — a placa correu sem RTC inicializado, com data de
  2025-05-29. Nenhuma medida deste documento tomada na placa depende de tempo absoluto, mas
  qualquer registro que o dispositivo escreva depende, e isso não está tratado.
- **TPM, secure boot, OP-TEE** — nenhum exercitado. O OP-TEE está no FIP do disco da §8, mas nunca
  executou. E o TPM não é apenas "não medido": não há implementação alcançável nas camadas atuais —
  só emuladores em software, sem fTPM para o OP-TEE — de modo que a custódia de chave permanece de
  desenvolvimento **nos dois alvos**, e não apenas no de simulação (§7).
- **A HMI Qt em execução** — exige `NATIVE=1` com runqemu gráfico; sob `nographic` o
  `weston.service` falha por projeto.
- **A taxa de aquisição com a HMI conectada, depois de corrigida** — hoje ela é medida e está
  errada (§3, −62%), e a correção não foi feita. Quando for, isto volta como medida, não como
  ausência.
- **Continuidade de identidade e de trilha de auditoria através de uma atualização** — a chave de
  host SSH e o `machine-id` são gerados por slot (observado), e o journal persistente mora dentro do
  slot (previsto por inspeção, não observado). Nenhum dos dois tem correção implementada. Ver
  `BOOT_SLOT_AB_STM32MP2.md` §5.1 e §5.2.
- **Atualização que troque a versão do kernel** — `med-boot` é compartilhada pelos dois slots e o
  kernel mora lá, então o esquema A/B atual entrega rootfs e nada mais. Com `CONFIG_MODVERSIONS=y`,
  um desencontro apareceria em tempo de carga de módulo e não em tempo de boot.
- **Perfil `med-image-prod`** — nunca construído. Rootfs read-only não foi exercitado.
- **Perfil tomógrafo no STM32MP257** — nunca construído; a §2 é inteira sobre `qemux86-64`.
- **O front-end analógico, em qualquer das quatro ligações.** Desde 2026-09-27 o driver do AFE é
  **in-tree**, numa árvore Linux que é a fonte única do código dele, compilado pelo caminho do Yocto
  e empacotado como `kernel-module-ti-ads1299` (§8). Isso é build, não aquisição. Continuam **não
  medidos**, e são novos ou permanecem:
  - **nenhuma imagem `spi` ou `usb` foi construída.** A ligação que de fato usa o driver está
    resolvida corretamente (`bitbake -e`) e não compilada. E a `spi` tem um obstáculo de projeto e não
    de ferramenta: o `do_derive_device_options` **recusa** a prescrição `afe.bias_drive = true` nessa
    ligação, porque o driver de kernel deixa o amplificador de bias desligado — então uma imagem
    `spi` deste dispositivo exige mudar a prescrição ou o driver, e a recusa nunca foi exercitada;
  - **o driver da ponte MCP2210 não migrou.** A receita *out-of-tree* dele continua na camada
    adjunta, e com ela o `static char *spi_device = "ads1299"`, que é a dependência real entre os dois
    drivers — logo os dois só *parecem* independentes;
  - **as 8 marcas `[DS20005176?]`** do MCP2210 continuam intactas: aquele datasheet não foi aberto.

  E do que foi medido antes, continua valendo que era **de build e de host**: os dois módulos compilam e linkam limpos para arm64 contra o kernel real
  da placa (`W=1`, sem avisos), o *backport* 6.9→6.6 custou zero linhas de API, e 80 verificações
  funcionais do comportamento novo passam no host. Nada disso é uma medida de aquisição. Continuam
  **não medidos**, e são exatamente o que os dois planos pedem:
  - **jitter entre amostras, perda de amostra sob carga, granularidade do carimbo de tempo e custo
    de CPU**, em cada ligação e **comparados entre elas** — a comparação é o resultado, e é o que
    justificaria com número (e não com adjetivo) por que a arquitetura de produto é a do
    coprocessador;
  - a vazão da ligação USB, que continua sendo a **conta** do `implementation_plan_ads1299.md` §8 e
    não uma medição: os multiplicadores de 2–4 relatórios por transação vêm do protocolo e nunca
    foram observados;
  - o autoteste real contra sinal de teste interno e ruído com entradas em curto — o código existe
    no probe do driver de kernel (`ads1299_self_test()`, movido do framework em 2026-09-16), nunca
    rodou contra silício;
  - o contador de bordas do GP6 como referência de perda de amostra;
  - **se os módulos sequer carregam**. Compilar não é `insmod`, e em particular a disputa de
    associação com o `hid-generic` está *lida no fonte do kernel* e não observada.
  **Atualização de 2026-10-04 (§3, "o front-end real, pelo link `usb`, num host")**: num PC,
  não na placa, deixaram de ser "não medidos" os módulos carregarem, a associação contra o
  `hid-generic` (observada), a vazão da ligação USB (167/s, depois 250/s, explicada por trocas HID
  medidas), o autoteste contra o gerador interno (reprovava por desenho e foi corrigido), o ruído em
  curto e a perda de amostra numa sessão de 30 min com o host ocioso. Na STM32MP257, no mesmo dia:
  os módulos carregam, o autoteste passa e o serviço adquire, mas só numa janela de 20 s (§3).
  Continuam não medidos: a sessão longa e o jitter na STM32MP257, a perda **sob carga**, as ligações `spi` e `amp`, a comparação entre elas, e o
  contador do GP6, que a placa de AFE não pode usar porque o DRDY está no GP5.
  E há uma classe de erro anterior a todas essas: **nenhuma constante de datasheet foi conferida**.
  Mapa de registradores do ADS1299, opcodes e deslocamentos do MCP2210, VID/PID. Estão marcadas nos
  fontes e agrupadas para que conferir seja uma passada só; enquanto não for, qualquer medida feita
  com esses drivers estaria medindo a interpretação do datasheet junto com o hardware.

---

## 10. Reprodutibilidade

Todos os números acima saem de:

```bash
make pki          # uma vez: gera a CA de desenvolvimento
make qemu
make tomograph
make bundle && make verify-bundle
make bundle-disk
make check            # 23 asserções: plataforma + perfil EEG
python3 scripts/med-check.py tomograph   # 11 asserções: só as de plataforma
make stm32                      # §8: o alvo físico, configuração de produto
make stm32 KEY=development      # §8: a configuração que foi executada na placa

# §8, a árvore do kernel como fonte de verdade. O digest de referência é
# 35e19311ab288cf56345ea6396d47d0cc071dab2dfe7ed028c575f2202b3bf50
scripts/med-kernel-fingerprint.sh build/tmp-glibc/work-shared/stm32mp25-disco/kernel-source
scripts/med-kernel-fingerprint.sh linux-med

# e o build a partir do fork. MED_KERNEL_GIT/MED_KERNEL_SRCREV são parâmetros de
# default nulo: sem eles o build é exatamente o da ST.
./.kas-container/kas-container-5.2 --runtime-args \
  "-e MED_KERNEL_GIT=git:///work/linux-med;protocol=file;branch=<branch> \
   -e MED_KERNEL_SRCREV=<sha>" build kas/project-eeg-stm32mp2.yml
```

Duas ressalvas sobre esses dois últimos. O caminho do fork **difere entre o contêiner e o host** —
dentro dele o repositório está em `/work` —, e um `SRCREV` fixo é o que torna a medida citável:
`${AUTOREV}` busca a ponta da branch a cada build e um número medido sobre ele não é reproduzível.

As medidas de execução na placa (§8, "Resultado — execução na placa") não são reproduzidas por
`make`: exigem gravar o `.wic` num cartão, alimentar a STM32MP257F-DK e um console serial. Cada uma
traz o comando que a produziu na própria tabela. **A suíte `med-check.py` não alcança a placa** —
ela conversa por pty com o `runqemu` — de modo que as asserções ali foram executadas à mão, que é
precisamente a condição que a suíte existe para eliminar. Automatizá-las contra um alvo serial é
trabalho pendente, e até lá o alvo físico está no regime "verificado uma vez", não "verificável".

**A verificação de runtime é automatizada.** `make check` boota a imagem, executa 23 asserções e
sai com código não-zero se alguma falhar — o que transforma "verificado uma vez" em "verificável",
que é o que a IEC 62304 pede de uma atividade de verificação.

Uma ressalva sobre a suíte, aprendida ao exercitá-la: a asserção `acq-active` usava
`systemctl is-active`, e um serviço `Type=simple` com `Restart=on-failure` passa por uma janela
breve de `active` a cada tentativa — ela dava PASS para um serviço em *crash loop*, precisamente o
caso que existia para pegar. Só a injeção de falha da §7 revelou isso. **Uma asserção que nunca viu
a falha que procura é uma afirmação, não uma verificação**, e o mesmo ceticismo vale para as outras
vinte.
