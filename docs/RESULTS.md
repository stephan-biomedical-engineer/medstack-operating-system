# Resultados Medidos — MedPlatform

> **Status**: registro de medição, não de projeto. Os `implementation_plan_*.md` dizem o que se
> pretende fazer; este arquivo diz o que foi **observado**, com o método ao lado de cada número.
> `PROJECT_CONTEXT.md` continua sendo a referência arquitetural.
>
> **Quando**: 2026-08-16. **Onde**: `qemux86-64`, perfis `med-image-eeg` e `med-image-tomograph`.
> A medição de reuso (§2) é sobre o commit `f0e4427`; as demais foram tomadas no mesmo dia, sobre
> os commits que as introduziram.
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
| `med-image-eeg` | 209 | 275.500 KB |
| `med-image-tomograph` | 206 | 275.388 KB |
| **Diferença** | **3** | **112 KB** |

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

**Sobre os 112 KB**: eles são pequenos porque o Qt já está nas duas imagens (`packagegroup-med-gui`
está em ambas), então a HMI é apenas o binário linkando bibliotecas compartilhadas. O número mede o
**custo marginal de uma aplicação sobre uma plataforma que já tem o toolkit** — não "uma HMI Qt cabe
em 112 KB".

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

## 7. O que **não** foi medido

Registrado explicitamente para que a ausência não seja lida como resultado:

- **Caminho AMP / `rpmsg` / Cortex-M4** — o QEMU não tem co-processador. O driver medido é o
  `simulated`.
- **Latência e jitter de tempo real** — o timing do QEMU não é significativo.
- **Integração com bootloader e fallback A/B em boot falho** — §6.
- **TPM, secure boot, OP-TEE** — nenhum presente.
- **Volume `/data` criptografado** — `systemd-cryptsetup@med-data` falha hoje; o provisionamento
  LUKS de primeiro boot não existe (`implementation_plan_luks.md`).
- **A HMI Qt em execução** — exige `NATIVE=1` com runqemu gráfico; sob `nographic` o
  `weston.service` falha por projeto.
- **Perfil `med-image-prod`** — nunca construído. Rootfs read-only não foi exercitado.
- **Alvo STM32MP257** — nunca construído.

---

## 8. Reprodutibilidade

Todos os números acima saem de:

```bash
make pki          # uma vez: gera a CA de desenvolvimento
make qemu
make tomograph
make bundle && make verify-bundle
make bundle-disk
make runqemu
```

**Ainda não há suíte automatizada.** Toda a verificação de runtime desta página foi conduzida
manualmente (ou por um driver de console descartável). Converter isto num `make check` que boota e
executa asserções é o próximo passo natural, e é o que transforma "verificado uma vez" em
"verificável", que é o que a IEC 62304 pede de uma atividade de verificação.
