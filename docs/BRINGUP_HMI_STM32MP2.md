# Registro de Engenharia — A interface gráfica na STM32MP257F-DK

> **Status**: registro do que foi feito, por que, e com que evidência. Companheiro do
> `BRINGUP_HMI_QEMU.md`, que trata do mesmo problema no alvo simulado, e do
> `BRINGUP_STM32MP2.md`, que trata do porte para esta placa.
>
> **Quando**: 2026-08-27, madrugada. **Onde**: `stm32mp25-disco` (STM32MP257F-DK física), imagem
> `med-image-eeg` construída em 26/08 às 23:48 com a correção do `mincore` já dentro.
>
> **Resultado**: **a interface de operador apareceu na tela**, pela primeira vez em hardware. Quatro
> defeitos encontrados e vencidos em sequência; três das correções ainda são voláteis e precisam
> descer para as camadas.
>
> **Como ler**: cada defeito traz o sintoma, a causa real, a correção e o que provou. Onde a causa
> não foi identificada, está escrito que não foi.

---

## 1. Linha do tempo

| # | Sintoma | Causa real | Correção | Evidência |
|---|---|---|---|---|
| 1 | `weston.service` morre em **73 ms** | compositor sem privilégio não consegue seat; `libseat` cai num backend que exige root | `seatd.service` (já aplicada, `meta-med-distro`) | a falha passou a acontecer 6 s depois, e o `seatd` registra sessão no `seat0` |
| 2 | `weston.service` morre em **6 s** | `/dev/galcore` é `crw------- root root`; o weston roda como uid 1000 | `chmod 0660` + `chgrp video` (volátil) | `su -s /bin/sh -c 'cat /dev/galcore' weston` → `Permission denied` |
| 3 | weston **de pé**, tela apagada | ocioso de 300 s sem evento de entrada; a placa não tem teclado nem mouse, então o relógio nunca reinicia | `idle-time=0` no `weston.ini` | CRTC saiu de `enable=0/active=0` para `enable=1/active=1` |
| 4 | CRTC **ativo**, modo programado, **sem imagem** | timings de PC (CVT/DMT) não saem; timings CEA-861 saem — não é taxa de pixel | fixar um modo CEA (`1920x1080@50`) | imagem na tela; 148,5 MHz passa e 119,0 MHz não |
| — | (hipótese descartada) | trocar de VT não pinta nada: o kernel não tem `fbcon` | — | `# CONFIG_FRAMEBUFFER_CONSOLE is not set` |

Cada correção mediu a anterior: **73 ms → 6 s → de pé e invisível → imagem**. É a forma mais barata
de saber que um conserto funcionou — a falha muda de lugar.

---

## 2. Defeito 1 — o seat, validado em hardware

O `seatd` entrou no repositório em 18/08 com a nota "verificado no rootfs montado, **não** verificado
em hardware". Esta sessão fecha isso:

```
● seatd.service - Seat management daemon
     Active: active (running); Main PID: 305 (seatd)
             /usr/bin/seatd -g video
```

E o weston, que antes morria em 73 ms com `Could not open tty0 to update VT: Permission denied`,
passou a chegar até aqui:

```
[04:14:44.530] Trying libseat launcher...
[04:14:44.532] Seat opened with backend 'seatd'
[04:14:44.532] libseat: session control granted
[04:14:44.539] using /dev/dri/card0
[04:14:44.539] DRM: supports atomic modesetting
```

**Correção já implementada e agora validada no alvo físico.**

---

## 3. Defeito 2 — o nó do driver proprietário sem permissão

### Sintoma

```
[04:14:44.540] Loading module '/usr/lib/libweston-13/gl-renderer.so'
[     1] Failed to open device: No such file or directory, Try again...
[     5] _OpenDevice(1276): FATAL: Failed to open device, errno=No such file or directory.
```

Cinco tentativas com um segundo de intervalo — os 6 s de vida da unidade.

### Causa

O `_OpenDevice()` e o formato de retentativa são da HAL da Vivante. O dispositivo que ela não abre é
`/dev/galcore`. E ele **existe**:

```
$ lsmod | grep galcore
galcore               389120  0
$ ls -l /dev/galcore
crw-------    1 root     root      199,   0 /dev/galcore
$ dmesg | grep -i galcore
[    7.508112] galcore: loading out-of-tree module taints kernel.
[    7.566234] Galcore version 6.4.21.1.1058597
```

Modo **0600, dono root** — e o `weston.service` do oe-core declara `User=weston`, uid 1000. A prova
direta:

```
$ su -s /bin/sh -c 'cat /dev/galcore' weston
cat: can't open '/dev/galcore': Permission denied
```

O `errno=No such file or directory` reportado pela HAL não contradiz isso: ela tenta `/dev/galcore`,
recebe `EACCES`, tenta `/dev/graphics/galcore`, recebe `ENOENT`, e reporta o último. **O erro que ela
imprime não é o erro que a impediu.**

### Por que a imagem chega assim

Não há regra de udev nenhuma para o galcore na imagem — conferido no `.ext4` produzido:
`/etc/udev/rules.d/` tem apenas `72-rpmsg.rules` e `touchscreen.rules`. O pacote
`kernel-module-galcore` da ST entrega só o módulo; quem ajusta dono e modo do nó nas imagens
OpenSTLinux vem de outro pacote, e esta imagem instala apenas o `GPU_IMAGE_INSTALL`.

É **exatamente o mesmo formato** do defeito que originou o `MED_GPU_PACKAGES`: a ST entrega a peça, e
o passo que a imagem completa dela faria não acontece aqui. Um build que passa e um compositor que
falha em runtime, pela segunda vez, na mesma placa, pela mesma razão estrutural.

### Correção — **pendente**

```
KERNEL=="galcore", GROUP="video", MODE="0660"
```

Grupo `video` e não `0666` porque o usuário do weston já pertence a `video` — é o mesmo grupo do
`seatd -g video`, então a permissão fica coerente e o dispositivo não abre para todo mundo.

Camada: **`meta-med-bsp`**, pelo gancho `MED_BSP_INSTALL`. "Esta placa tem uma GPU Vivante cujo nó
precisa de acesso por grupo" é fato de placa.

---

## 4. Defeito 3 — o compositor de pé e a saída desligada

Com o galcore acessível, o weston subiu:

```
[04:14:55.777] Output 'HDMI-A-1' enabled with head(s) HDMI-A-1
[04:14:55.779] Loading module '/usr/lib/weston/desktop-shell.so'
[04:14:55.790] launching '/usr/libexec/weston-desktop-shell'
systemd[1]: Started Weston, a Wayland compositor, as a system service.
```

O monitor continuou apagado. E o estado atômico do DRM mostrava a contradição:

```
crtc[41]: crtc-0
	enable=0
	active=0
	mode: "": 0 0 0 0 0 0 0 0 0 0 0x0 0x0
	connector_mask=0
connector[32]: HDMI-A-1
	crtc=(null)
```

…enquanto os planos ainda carregavam framebuffers alocados pelo weston. Ou seja: ele compôs, e
depois a saída foi desligada.

### Causa

O tempo de ocioso padrão do weston é de **300 s sem evento de entrada**, e ao expirar ele apaga as
saídas por DPMS. A placa não tem teclado nem mouse: o relógio começa a correr quando o compositor
sobe e **nunca é reiniciado**. O `weston.ini` de fábrica do oe-core não define `idle-time`.

### Correção — **pendente**

```
[core]
require-input=false
idle-time=0
```

Efeito medido, no mesmo boot, sem mais nada mudado:

```
crtc[41]: crtc-0
	enable=1
	active=1
	plane_mask=3   connector_mask=1   encoder_mask=1
	mode: "1920x1080": 60 138500 1920 1968 2000 2080 1080 1083 1088 1111 0x48 0x9
```

Camada: **`meta-med-distro`**. *"A tela de operador nunca apaga"* é política de dispositivo médico,
vale em qualquer placa, e não nomeia máquina nenhuma. Um monitor de EEG que se apaga sozinho depois
de cinco minutos porque ninguém encostou no teclado é defeito de dispositivo, não economia de
energia.

---

## 5. Defeito 4 — tudo certo, e nenhuma imagem

Este é o mais instrutivo da sessão.

Com o CRTC ativo, o estado dizia que **tudo estava correto**:

```
mode: "1920x1080": 60 138500 …     modo programado
plane_mask=3  connector_mask=1  encoder_mask=1
transfer_error=0
fifo_underrun_error=0
fifo_underrun_warning=0
```

e o `dmesg` filtrado por `ltdc|dsi|adv|underrun|drm` não trazia **uma única linha de erro** — só o
ruído de `Fixed dependency cycle(s)` do devicetree da ST, já catalogado, e o
`[drm] Initialized stm 1.0.0 … on minor 0`.

A tela seguia apagada. A correção foi fixar o modo:

```
[output]
name=HDMI-A-1
mode=1280x720
```

e a imagem apareceu. Depois da caracterização abaixo, o modo a fixar passa a ser
**`1920x1080@50`** — resolução cheia, e 50 Hz é indiferente para uma tela de forma de onda.

### A caracterização, com quatro pontos medidos

Dois testes adicionais, de um minuto cada, separaram o padrão:

| Modo | Timing | Clock | Resultado |
|---|---|---|---|
| 1920x1080@59.9 (preferido do EDID) | 2080×1111, **CVT reduced blanking** | 138,5 MHz | falha |
| 1920x1080@50 | **CEA-861 VIC 31** (2640×1125) | 148,5 MHz | **funciona** |
| 1680x1050@59.9 | **DMT/CVT** de PC | 119,0 MHz | falha |
| 1280x720@60 | **CEA-861 VIC 4** (1650×750) | 74,2 MHz | **funciona** |

**Não é taxa de pixel, e isso está descartado por medição**: 148,5 MHz funciona enquanto 119,0 MHz
falha. A hipótese de *underrun de FIFO no LTDC* já tinha sido refutada pelos contadores zerados; a
de largura de banda cai aqui.

O que separa as duas colunas é o **tipo de timing**: os modos que funcionam são CEA-861 (televisão),
os que falham são timings de computador (CVT/DMT). O `mode:` capturado do estado atômico confirma do
lado do kernel que o preferido do monitor é um timing de PC — `2080 … 1111` é blanking reduzido, e
não o `2200×1125` do 1080p60 padrão HDMI.

**Hipótese resultante, ainda não confirmada**: a ponte ADV7535 em modo DSI, ou o caminho DSI da ST,
só configura corretamente formatos CEA. Um teste fecharia o argumento — `720x576@50` (27,0 MHz,
CEA VIC 18) deve funcionar apesar do clock baixíssimo, o que mostraria que a variável é o formato e
não a frequência. **Não executado**.

### Correção — **pendente**

Camada: **`meta-med-bsp`**. *"Esta cadeia LTDC → DSI → ADV7535 não entrega 1080p"* é fato desta
placa, do mesmo jeito que o layout de disco é.

Repare que os defeitos 3 e 4 caem dos dois lados da linha que o `meta-med-bsp` existe para traçar —
política contra placement — e que o arquivo de configuração é o mesmo. A implementação consistente é
um `weston.ini` nosso em `meta-med-distro` com um gancho (`MED_WESTON_OUTPUT`) que o `meta-med-bsp`
preenche por máquina, no mesmo formato de `MED_GPU_PACKAGES`, `MED_AMP_FIRMWARE` e `MED_BSP_INSTALL`.

---

## 6. A hipótese descartada, e por que ela fica registrada

Entre os defeitos 3 e 4, uma pista falsa consumiu tempo: o VT ativo era o `tty1` enquanto a unit do
weston aloca `TTYPath=/dev/tty7`. `chvt 7` não mudou nada, e a configuração do kernel explica:

```
CONFIG_VT=y
# CONFIG_FRAMEBUFFER_CONSOLE is not set
# CONFIG_DRM_FBDEV_EMULATION is not set
```

Os VTs existem, mas **nada os desenha**. Trocar de VT nesta placa não tem efeito visual algum, e o
único desenho possível na saída é o de um cliente DRM. Fica escrito para que ninguém volte a
perseguir isso — custou um comando descobrir, e custaria uma hora sem o registro.

---

## 7. O que ficou provado

| Item | Evidência |
|---|---|
| A HMI Qt roda em hardware | `ActiveState=active`, `NRestarts=0`, `ExecMainStatus=0` |
| E é estável | 70,2 MB de memória (pico 70,5), **1,099 s de CPU em 2 min 22 s** |
| A correção do `mincore` vale no alvo físico | no QEMU a HMI morria de `SIGSYS` aos 90 s; aqui passa disso com o stack Vivante |
| A cadeia gráfica inteira funciona | LTDC → DSI → ADV7535 → monitor, com EDID lido e modo programado |
| O que está na tela é a nossa aplicação | `plane-1` com framebuffer **1030x633** AR24 — a janela de 1024x600 do `Main.qml` mais a decoração do shell |
| O conector é o esperado | `card0-HDMI-A-1: connected`, único conector; sem LVDS, como o devicetree previa |

O item do `BRINGUP_STM32MP2.md` §10 que dizia *"HMI Qt — falha observada, causa desconhecida"* está
fechado, e com quatro causas em vez de uma.

---

## 8. Um efeito colateral observado

Iniciar a HMI **ressuscitou** o laço de reinício do serviço de aquisição: `eeg-hmi.service` declara
`Wants=eeg-acquisition.service`, e o systemd trouxe junto o serviço que havia sido parado.

Comportamento correto da unit — a HMI *quer* dados — e efeito previsível de um serviço que não
consegue subir neste alvo, porque `MED_EEG_DRIVER` é `rpmsg` e o Cortex-M33 não tem firmware. Os
números desse laço, medidos nesta mesma sessão, estão no `BRINGUP_STM32MP2.md`: 163 reinícios,
período de 2,500 s, ~280 ms de vida por tentativa, `front-end self test failed` como última mensagem.

Consequência para o roteiro de bancada: **a tela mostra a HMI com o indicador de "aguardando o
serviço de aquisição"**, e não a forma de onda. Para ter o sinal na tela da placa, o caminho de uma
linha é `MED_EEG_DRIVER = "simulated"` no arquivo de projeto KAS — que é o argumento de
portabilidade sendo usado como instrumento, não uma concessão.

---

## 9. Correções pendentes

Nenhuma das três sobrevive a um reboot. O próximo boot volta a falhar em 6 s.

| # | Correção | Camada | Sem ela |
|---|---|---|---|
| 2 | regra udev `KERNEL=="galcore", GROUP="video", MODE="0660"` | `meta-med-bsp` | weston morre em 6 s |
| 3 | `idle-time=0` num `weston.ini` próprio | `meta-med-distro` | sobe e a tela apaga em 5 min |
| 4 | `[output] name=HDMI-A-1 mode=1920x1080@50`, via gancho por máquina | `meta-med-bsp` | CRTC ativo e tela apagada |

---

## 10. As regras que ficam

1. **Ausência de erro não é evidência de saída.** Modo programado, conector ligado, zero underrun,
   `dmesg` limpo — e nada no monitor. É a mesma família de "build verde não é evidência sobre o
   caminho de atualização", agora no plano do DRM.
2. **O erro que um componente imprime não é necessariamente o erro que o impediu.** A HAL da Vivante
   reportou `ENOENT` de um segundo caminho enquanto o problema era `EACCES` no primeiro.
3. **Cada correção deve mover a falha.** Quatro defeitos empilhados, e o que deu confiança em cada
   conserto foi o sintoma mudar de tempo e de lugar: 73 ms, 6 s, de pé e invisível, imagem.
4. **Um default de projeto de desktop pode ser um defeito de dispositivo.** O ocioso de 300 s é
   sensato num laptop e inaceitável numa tela de operador.
5. **Integrar um BSP de fabricante sem a imagem dele significa herdar as peças e não a montagem.**
   Pacotes de GPU sem regra de udev, driver sem `modules-load`, `WKS_FILE` que não existe — três
   ocorrências do mesmo padrão nesta placa.

---

## 11. Nota de bancada — como se chega ao dispositivo

Registrado aqui porque custou meia hora e não estava escrito em lugar nenhum.

O MedOS é **indescobrível por projeto**: o `80-wired.network` define `SendHostname=false`,
`MulticastDNS=no` e `LLMNR=no`, então a placa não aparece com nome na tabela de leases do roteador
nem responde mDNS. Some-se a isso que, nesta rede, o ARP em IPv4 não atravessa a ponte entre o cabo e
o WiFi — `nmap -sn -PR` não encontra nada e o `ping` responde `Destination Host Unreachable` da
própria máquina de origem.

**Atualização de 2026-10-04**: nessa data o IPv4 chegou (`ssh root@192.168.1.11`, endereço lido em
`end0` pela sessão IPv6). Não se sabe o que mudou na rede, e o endereço vem de DHCP. O caminho
abaixo continua sendo o estável.

O que funciona, e é o caminho a usar em bancada:

```sh
ssh root@fe80::12e7:7aff:fee3:d562%wlp0s20f3
scp arquivo root@\[fe80::12e7:7aff:fee3:d562%wlp0s20f3\]:/tmp/
```

A descoberta por vizinhança IPv6 atravessa onde o ARP não atravessa — o endereço link-local aparece
na tabela de vizinhos do host com o MAC correto mesmo quando o IPv4 falha. O console serial continua
sendo a fonte de verdade para descobrir o endereço, e o `dmesg -n 1` é o que torna esse console
utilizável quando algo está inundando o log.

---

## 12. O que esta sessão não fez

O roteiro de bancada previa também o caminho de atualização, e ele ficou inteiro para a próxima:
`/etc/fw_env.config` vindo da receita, `rauc-mark-good` semeando `BOOT_A_LEFT`, e um `rauc install`
real do bundle. Nada disso foi tocado.
