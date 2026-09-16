# Registro de Engenharia — A HMI no QEMU, e por que ninguém nunca a tinha visto

> **Status**: registro do que foi feito, por que, e com que evidência. Não é plano
> (`implementation_plan_*.md`) nem medição consolidada (`RESULTS.md`) — é a narrativa que
> conecta os dois, no mesmo formato do `BRINGUP_STM32MP2.md`.
>
> **Quando**: 2026-08-24/25.
> **Onde**: `qemux86-64`, imagem `med-image-eeg` construída em 2026-08-18 (a mesma que já traz o
> `seatd` e a correção do `XDG_RUNTIME_DIR`).
>
> **Como ler**: cada item traz o sintoma, a causa real (que quase nunca é o sintoma), a correção e
> o que provou que a correção funcionou. Onde nada foi implementado, está escrito que nada foi
> implementado.

![Remmina](/home/stephan/Pictures/Screenshots/remmina.png)
---

## 1. Linha do tempo

| # | Sintoma | Causa real | Correção | Evidência |
|---|---|---|---|---|
| 1 | `runqemu … sdl` não abre janela | `qemu-system-native` do poky é construído **sem** SDL e sem GTK | usar VNC | `qemu-system-x86_64 -display help` → só `none` e `egl-headless` |
| 2 | Terminal "travado" após subir a VM | sem `nographic`, o runqemu manda console **e** monitor para `-serial mon:vc`, invisível sob `-display none` | opção `serialstdio` | `ss -ltn` mostra 5900/2222 escutando: a VM estava viva o tempo todo |
| 3 | Console do convidado despejando OOM killer sem parar | `QB_MEM ?= "-m 256"` do poky; **nada neste repositório jamais nomeou esse valor** | não implementado (§9) | `qb_mem = -m 256` no `.qemuboot.conf`; `65398 pages RAM` no dump do kernel |
| 4 | — (nenhum) | A suíte declara a HMI saudável 30 s após o boot; o primeiro OOM chega aos 150 s | não implementado (§9) | `hmi-active` passa; a VM entra em laço dois minutos depois |
| 5 | HMI morre com `SIGSYS` a cada ~13 s | `mincore` **não pertence a nenhum grupo funcional do systemd**, e a unidade não define `SystemCallErrorNumber` | não implementado (§9) | `audit type=1326 … syscall=27 … code=0x80000000` |
| 6 | — (nenhum) | Três linhas de documentação afirmavam comportamentos que ninguém tinha conferido | não implementado (§10) | §10 |

Note o padrão, que é o mesmo do registro anterior: **dois dos seis itens não tinham sintoma nenhum**
— apareceram porque alguém foi olhar. E note o complemento: os itens 3 e 5 são o oposto, nenhum
deles poderia ter sido encontrado sem **abrir a tela**, que é um degrau acima de "bootar sob
`nographic` e ler o console".

---

## 2. O ponto de partida

A pergunta era simples: a parte gráfica da aquisição simulada é possível no QEMU? A resposta curta
acabou sendo **sim, e já estava rodando** — o que faltava era um frontend de display. A premissa que
a suíte carregava no `EXPECTED_FAILED`, *"nographic: não há dispositivo DRM/KMS para um compositor
abrir"*, já tinha expirado quando o `seatd` entrou (commit `822e833`), e o motivo técnico é que
`-nographic` desliga o *frontend* de display, não o dispositivo de vídeo. O kernel do QEMU tem
`CONFIG_DRM_BOCHS=y` e `CONFIG_DRM_VIRTIO_GPU=y`, então há `/dev/dri/card0`, o weston abre e o Qt
desenha — num framebuffer que ninguém estava olhando.

---

## 3. O que a build produziu, e o que ela não produziu

A saída "mais simples" que a `BUILD_CONTAINER.md` recomendava — uma janela SDL — **não existe nesta
build**:

```
$ qemu-system-x86_64 -display help
Available display backend types:
none
egl-headless
```

A causa é o default do próprio poky: `qemu-system-native.bb` declara
`PACKAGECONFIG ??= "fdt alsa kvm pie slirp png …"`, e nem `sdl` nem `gtk+` entram. O `runqemu` sabe
disso: ele lê o `--help` do binário e, não achando `-display gtk` nem `-display sdl`, cai em
`-display none` (`scripts/runqemu:1442`). VNC, por outro lado, **está** compilado (`-vnc help`
responde), e é por isso que o caminho gráfico deste repositório é VNC.

---

## 4. O caminho que funciona

```sh
kas shell kas/project-eeg-qemu.yml -c 'runqemu qemux86-64 \
  $BUILDDIR/tmp-glibc/deploy/images/qemux86-64/med-image-eeg-qemux86-64.rootfs.wic \
  slirp kvm serialstdio qemuparams="-m 1024 -vnc 127.0.0.1:0"'
```

Cada palavra ali é um defeito encontrado no caminho:

- **`-vnc 127.0.0.1:0`** em vez da opção `publicvnc` do runqemu, porque `publicvnc` emite `-vnc :0`
  (`scripts/runqemu:539`), que escuta em **todas as interfaces, sem senha**. Numa máquina de
  trabalho isso é exposição desnecessária.
- **`serialstdio`**, sem o qual console e monitor vão para `-serial mon:vc` e somem. O sintoma é um
  terminal que parece travado; a VM está de pé, e `ss -ltn` prova isso.
- **`kvm`**, que o runqemu **não** infere: sem a opção, não há `-enable-kvm` na linha de comando e
  tudo roda em emulação pura.
- **`-m 1024`**, que é a §5.

Vale registrar o que o SSH resolve de graça: a imagem de desenvolvimento traz `ssh-server-openssh` e
`debug-tweaks`, e o runqemu já publica `hostfwd=tcp:127.0.0.1:2222-:22`. `ssh -p 2222 root@127.0.0.1`
dá um shell utilizável **mesmo com o console serial perdido**, e foi por ele que o registro do
seccomp da §7 foi extraído.

---

## 5. O primeiro defeito: 256 MiB

Com a tela finalmente acessível, o convidado passou a despejar OOM killer. Os números do dump do
kernel:

```
65398 pages RAM                                    ->  255 MiB
Total swap = 0kB
Out of memory: Killed process 1347 (weston) total-vm:277812kB
```

**O weston sozinho mapeia 271 MiB, mais do que a máquina inteira tem.** O ciclo se repete 28 vezes
no log, a cada ~50–60 s, e o mecanismo que o sustenta merece nota: `weston.service` (oe-core) não
tem `Restart=`. Quem o ressuscita é o `eeg-hmi.service`, que declara `Requires=weston.service` e
`Restart=on-failure` — a política de reinício da HMI é o que mantém o laço de OOM vivo.

A causa é `QB_MEM ?= "-m 256"`, em `qemuboot.bbclass:96`, e **nenhuma camada `meta-med-*` jamais
sobrescreveu isso**. É a terceira ocorrência da mesma forma de defeito registrada neste repositório:
um default upstream vencendo por ausência de adversário, exatamente como o `WKS_FILE` da distro
venceu no QEMU e como o `WKS_FILE` ausente do `meta-st-stm32mp` venceu no STM32 (§1 e §2 do
`BRINGUP_STM32MP2.md`). O perfil que instala compositor + Qt + renderização por software nunca
declarou de quanta RAM precisa.

Com `-m 1024` o OOM desaparece por completo, o que isolou o defeito da §7.

---

## 6. O ponto cego da suíte, medido

`make check` roda com os mesmos 256 MiB, e a asserção `hmi-active` espera até 30 s e exige
`ActiveState=active` com `NRestarts=0`. **O primeiro OOM desta execução aconteceu aos 150 s.**

Ou seja: a suíte declara a HMI saudável, e o dispositivo entra em laço de OOM dois minutos depois,
sem que nada reporte. É literalmente a lição que a injeção de falha do `acq-active` deixou —
*uma asserção que nunca viu a falha que procura é uma afirmação, não uma verificação* — reencontrada
numa terceira unidade, e agora numa dimensão nova: não é o predicado que está errado, é a **janela
de observação**. Um monitor que passa nos primeiros 30 s e morre no terceiro minuto passa na suíte.

---

## 7. O segundo defeito: `mincore`

Com memória suficiente, sobrou a falha real, e ela finalmente se deixou nomear:

```
audit: type=1326 audit(1787626943.772:36): … pid=538 comm="QSGRenderThread"
       exe="/usr/bin/eeg-hmi-gui" sig=31 arch=c000003e syscall=27 compat=0
       ip=0x7f638355a52b code=0x80000000
```

Três leituras:

- **`syscall=27` em x86_64 é `mincore`** (`/usr/include/x86_64-linux-gnu/asm/unistd_64.h`).
- **`comm="QSGRenderThread"`** é a thread de render do Qt Scene Graph, onde o llvmpipe executa.
- **`code=0x80000000` é `SECCOMP_RET_KILL_PROCESS`**: o processo é morto, não recebe erro. Isso
  acontece porque `eeg-hmi.service` declara `SystemCallFilter=@system-service` e **não** declara
  `SystemCallErrorNumber=`, ao contrário do `eeg-acquisition.service`, que usa `EPERM`.

E o achado que muda a correção:

```
$ systemd-analyze syscall-filter @system-service | grep -cw mincore
0
$ # varrendo todos os grupos, o único que contém mincore é:
@known
```

`mincore` **não pertence a nenhum grupo funcional** — `@known` é a lista do que o systemd conhece,
não um conjunto concedível. Portanto `@system-service`, que é a união dos grupos funcionais, nunca o
contém, e **nenhum `@grupo` adicional resolveria**: a correção tem obrigatoriamente de nomear a
syscall.

A hipótese anterior, registrada no commit `77e48c9` como "llvmpipe chamando `@resources`
(`sched_setaffinity` e vizinhos)", **estava errada**. Ela era plausível e não custava nada testar; o
que custou foi ter ficado como explicação por escrito sem que o registro de auditoria fosse lido.

Uma observação sobre por que isso demorou: as duas falhas estavam **sobrepostas**. Sob 256 MiB, o
OOM matava o weston a cada ~55 s e o SIGSYS aparecia esparsamente, o que produziu a leitura
"a HMI morre em ~90 s por SIGSYS" — uma frase que mistura as duas e explica mal cada uma. Só depois
de eliminar a pressão de memória o intervalo real ficou visível: **~13 s**, três registros seguidos
(03:02:23, 03:02:37, 03:02:50).

---

## 8. A confirmação: a HMI renderiza

Com o serviço parado e o binário executado à mão pelo SSH — sem systemd, portanto sem filtro de
syscalls:

```sh
systemctl stop eeg-hmi
XDG_RUNTIME_DIR=/run WAYLAND_DISPLAY=wayland-0 \
  MED_EEG_SOCKET=/run/medplatform/eeg.sock QT_QPA_PLATFORM=wayland \
  /usr/bin/eeg-hmi-gui
```

**A HMI renderizou, e foi vista pelo VNC** (2026-08-25). É a primeira vez que a interface de
operador deste projeto foi observada por um ser humano em qualquer alvo.

O que isso estabelece: o `EegClient`, o `Main.qml`, o caminho `MedicalIPC` do serviço até a HMI e a
pilha Qt/Wayland/weston/llvmpipe funcionam. O que **não** estabelece: nada sobre a HMI sob o sandbox
que ela terá em produção — a execução foi como root e sem `SystemCallFilter`, que é precisamente a
condição que a §7 descreve como defeituosa.

**Ainda não registrado**: nenhuma captura de tela foi arquivada. A `RESULTS.md` continua sem figura
da interface de operador, e o caminho para obtê-la é o monitor do QEMU (`Ctrl-A c` →
`screendump /tmp/hmi.ppm`, que exige o `serialstdio` da §4) seguido de `pnmtopng`.

---

## 9. Correções pendentes — duas implementadas, duas não

Atualizado em 2026-09-07. Duas linhas desta tabela deixaram de ser pendências, e a do OOM só saiu
porque **cobrou o preço de novo**: durante a entrega do front-end analógico, `make check` reportou as
quinze primeiras asserções passando e as sete restantes como "sem resposta do guest", sem nenhuma
unidade falha. O convidado tinha entrado exatamente no laço descrito na §5, e a evidência veio do
dump do próprio kernel:

```
oom-kill:constraint=CONSTRAINT_NONE,nodemask=(null),cpuset=/,mems_allowed=0,
         global_oom,task_memcg=/user.slice/...,task=weston,pid=341,uid=1000
Out of memory: Killed process 341 (weston) total-vm:277812kB, anon-rss:77056kB
```

Vale registrar por que ficou escondido por tanto tempo: enquanto um defeito **anterior** ao OOM
matava o serviço de aquisição, a HMI nunca chegava a subir, e o consumo que estoura os 256 MiB nunca
acontecia. Corrigido o primeiro defeito, o segundo apareceu na hora. Um defeito mascarado por outro
defeito é a mesma forma do "defeito escondido atrás de uma recusa" que o `BRINGUP_STM32MP2.md` §11
já registra.

| defeito | correção | camada | estado |
|---|---|---|---|
| §5 OOM | `QB_MEM:qemux86-64 = "-m 1024"` | `meta-med-bsp` | **implementado** (2026-09-07) — a RAM da máquina virtual é fato de máquina, e é a única camada autorizada a nomear uma; `QB_FSINFO` e `QB_KERNEL_ROOT` já moravam lá. Verificado no artefato: `qb_mem = -m 1024` no `.qemuboot.conf` produzido |
| §7 SIGSYS | `SystemCallFilter=mincore` (linha adicional; allowlists se somam) | `meta-med-app` | **implementado** — está em `eeg-hmi.service:57` |
| §7 (2) | avaliar `SystemCallErrorNumber=EPERM` na HMI | `meta-med-app` | **pendente** — hoje a metade **não** relevante para segurança morre de vez numa syscall imprevista, enquanto a relevante apenas recebe erro; invertido em relação ao que se esperaria |
| §6 janela | asserção de estabilidade com horizonte maior que 30 s | `scripts/med-check.py` | **pendente**, e é a que continua custando caro: a suíte declara a HMI saudável aos 30 s e o primeiro OOM chegava aos 150 s, então ela via o sistema morrer *depois* de já ter aprovado. Ver §11.3 |

O valor de `-m 1024` é o que foi **medido como suficiente** (OOM desaparece, HMI sobrevive ao ponto
de ser morta por outra causa). Não é um requisito derivado: ninguém mediu o mínimo, e o número
merece ser tratado como "sabidamente suficiente", não como "necessário".

---

## 10. Três afirmações de documentação que estavam erradas

Todas do mesmo tipo — comportamento de terceiros afirmado sem conferência, que é a regra 1 do
`BRINGUP_STM32MP2.md` §11:

1. **`BUILD_CONTAINER.md` §5**: *"`make runqemu NATIVE=1` — kas do host, janela SDL normal. É a mais
   simples"*. Errada duas vezes: o alvo do Makefile passa `nographic` fixo, então `NATIVE=1` só troca
   container por kas do host; e mesmo sem `nographic` não há SDL nesta build (§3).
2. **`Makefile`, alvo `runqemu`**: passa `nographic slirp` e **não** passa `kvm`.
3. **`make help`**: *"runqemu — boot the GPT disk image, KVM accelerated, serial console"*. O console
   serial ele acerta (via `nographic`); o KVM, não.

---

## 11. As regras que ficam

1. **Rodar sob `nographic` não substitui abrir a tela.** É o mesmo salto que "inspecionar o artefato
   não substitui energizar a placa" (§11.8 do registro do STM32), um degrau adiante: a HMI passava
   na asserção da suíte, tinha `NRestarts=0` na janela medida, e nunca tinha sido **vista**.
2. **Duas falhas sobrepostas produzem uma explicação errada de cada uma.** A frase "morre em ~90 s
   por SIGSYS" era a média de dois fenômenos independentes. Isolar uma variável — aqui, dar memória
   suficiente — foi o que tornou a outra legível.
3. **Uma asserção tem um predicado e uma janela, e as duas podem estar erradas.** O predicado de
   `hmi-active` está correto; sua janela de 30 s é menor que o tempo até a falha. Toda asserção de
   "está saudável" deveria declarar por quanto tempo ela olhou.
4. **Um grupo de syscalls não é uma taxonomia completa.** `@system-service` é a união dos grupos
   *funcionais* do systemd, e existem syscalls — `mincore` entre elas — que não estão em grupo
   funcional nenhum. Ler o registro `type=1326` custa um comando; supor qual grupo faltava custou um
   commit com a explicação errada.
5. **Um default upstream vence por ausência de adversário — pela terceira vez.** `WKS_FILE` da
   distro no QEMU, `WKS_FILE` ausente do BSP da ST, e agora `QB_MEM`. Quando um valor decide como o
   produto se comporta, ele tem de estar escrito em alguma camada deste repositório, mesmo que o
   valor coincida com o default.
6. **Um perfil que instala uma pilha gráfica precisa declarar quanta memória ela custa.** Isso não é
   configuração de conveniência do QEMU: é um requisito de recurso do dispositivo, e no alvo físico
   ele reaparece como dimensionamento de RAM, onde não há `-m` para corrigir depois.
