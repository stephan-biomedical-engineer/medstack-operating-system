# Roteiro de bancada na placa — os links `usb` e `amp` na STM32MP257F-DK

> **O que este documento é**: a rotina para rodar toda vez que o hardware muda (a placa de AFE foi
> retrabalhada, um cabo trocou, o AFE passou do USB para o header ou de volta). Ela responde a uma
> pergunta só, **"o hardware está funcionando?"**, e responde do mesmo jeito todas as vezes:
> mesmos passos, mesmos comandos, mesmo critério de aprovação, e um lugar para anotar o número.
>
> **O que ele não é**: nem o plano nem o registro. O porquê de cada critério está no
> `implementation_plan_afe_bench.md` e no `implementation_plan_m33_firmware.md`. O que já foi medido
> está no `BRINGUP_STM32MP2.md` §9 (a placa), no `BRINGUP_AFE.md` §6.4–§6.7 (o USB na placa) e no
> `RESULTS.md`. O `ROTEIRO_BANCADA_AFE.md` é o irmão deste: o link `usb` num **PC Ubuntu**, com as
> ferramentas Python, que a placa não tem.
>
> **Escopo**: os links `amp` e `usb`. O `spi` não tem imagem que funcione ainda (falta o overlay
> com os pinos e o `fdt apply` no bootcmd).
>
> Executado inteiro, com o link `amp`, em 2026-10-09: os valores esperados da Fase 4 vêm dessa
> execução. Com o link `usb` a Fase 4 foi executada em 2026-10-10 (`BRINGUP_AFE.md` §6.8), e
> encontrou um defeito que só ela podia ver: o arquivo clínico era recusado em toda sessão do link.

---

## 0. O algoritmo, numa página

```
ENTRADA: o link desejado L ∈ {amp, usb}

 1. PREPARAR  a imagem do cartão é do link L?
              ├─ sim, e o software não mudou ........................ pule para 3
              └─ não (ou o software mudou) → construir L, CONFERIR O MANIFESTO, gravar
 2. MONTAR    placa desligada → montar o AFE conforme L → ligar
 3. ACHAR     achar o IP da placa e fixar B=root@<ip>
 4. FASE 1    plataforma de pé?           ── falhou → parar: o defeito não é do AFE
 5. FASE 2    front-end do link L visível? ── falhou → tabela de falhas (§8), parar
 6. FASE 3    aquisição de fábrica estável? ─ falhou → tabela de falhas (§8), parar
 7. FASE 4    sinal de teste interno: gravar, copiar, conferir sha256, julgar, plotar
 8. RESTAURAR voltar à config selada, e CONFERIR que voltou
 9. REGISTRAR anotar os números (§9)

SAÍDA: APROVADO se as Fases 1–4 passaram; caso contrário, a primeira fase que falhou e o motivo
```

Três regras valem em toda a rotina:

1. **Uma fase só começa se a anterior passou.** A Fase 4 com a Fase 2 falhando mede o defeito
   errado. Pular fase é a forma mais comum de chegar a uma conclusão errada nesta bancada.
2. **A placa nunca é desligada no meio de uma escrita.** Antes de tirar a energia, pare o serviço
   (`systemctl stop eeg-acquisition`) ou desligue pelo sistema (`poweroff`).
3. **Não reinicie a placa várias vezes seguidas com o front-end ausente.** Uma imagem cuja aquisição
   não sobe nunca confirma o slot. Cada boot gasta uma tentativa, e depois de três o dispositivo cai
   no outro slot sozinho (`BRINGUP_STM32MP2.md` §9.16). Isso é correto, mas confunde a leitura depois.

Todos os comandos rodam no PC, a partir de `~/Documents/TCC/yocto_workspace`, e **no mesmo
terminal**: as Fases 3 a 8 usam as variáveis `L`, `B`, `S` e `D`.

---

## 1. Preparar a imagem

### 1.1 Que imagem está no cartão?

Se a placa liga e você sabe o IP, ela mesma responde:

```bash
ssh root@10.1.35.254 'grep -E "^(acquisition\.link|device\.driver)" /etc/medplatform/eeg.conf'
```

`acquisition.link = amp` (driver `rpmsg`) ou `acquisition.link = usb` (driver `iio`). Se for o link
desejado e o software não mudou desde a última gravação, **pule para o §2**.

### 1.2 Construir

Primeiro o HD de cache, porque o `local.mk` recusa o build sem ele:

```bash
mountpoint ~/hd
```

Se o software mudou, rode antes os testes de host (segundos):

```bash
make test
```

Depois, **um** dos dois builds:

```bash
make stm32 KEY=development KERNEL=med
```
```bash
make stm32 KEY=development KERNEL=med LINK=usb
```

O primeiro é o `amp` (o padrão). O segundo é o `usb`.

**O `KEY=development` não é opcional.** Sem ele o perfil cai em `tpm2`, que recusa provisionar o
`/data`, e a imagem constrói verde e nunca adquire. Para conferir no artefato:
`grep '^KEY_SOURCE' build/tmp-glibc/work/stm32mp25_disco-med-linux/med-image-eeg/1.0/rootfs/usr/libexec/medplatform/med-data-provision.sh`.

Se a placa já roda uma imagem do **mesmo link e do mesmo commit do `linux-med`** e só o espaço de
usuário mudou, o caminho mais curto é um bundle em vez de regravar o cartão (o `/data` fica, e um
slot que não adquire volta sozinho): `make bundle BOARD=stm32 KEY=development KERNEL=med LINK=usb`,
copiar com `ssh $B 'cat > /run/update.raucb' < ...raucb`, conferir o sha256 dos dois lados,
`rauc install /run/update.raucb` e `reboot`. O slot novo gera outra chave SSH.

### 1.3 Conferir o manifesto — obrigatório

**Os dois links geram o mesmo nome de arquivo.** O último build vence sem aviso. Em 2026-10-09
um cartão foi gravado com a imagem `amp` logo depois de um build `usb`, porque um build `amp` veio
em seguida e sobrescreveu o `.wic`. Por isso, antes de gravar, sempre:

```bash
grep -E 'mcp2210|ti-ads1299|med-afe|med-m33' build/tmp-glibc/deploy/images/stm32mp25-disco/med-image-eeg-stm32mp25-disco.rootfs.manifest
```

| Link | O manifesto tem de listar | E **não** pode listar |
|---|---|---|
| `amp` | `med-m33-firmware` | nenhum `kernel-module-*` do front-end, nenhum `med-afe-*` |
| `usb` | `kernel-module-hid-mcp2210`, `kernel-module-ti-ads1299`, `med-afe-modprobe`, `med-afe-udev` | `med-m33-firmware` |

Se não bater, **não grave**: refaça o build do link certo.

### 1.4 Gravar

Ejete o HD de cache **antes** de inserir o cartão, para que o único disco removível seja o cartão:

```bash
make eject
```

Insira o cartão e identifique o dispositivo. Confira o tamanho (~15 G) e, se o cartão já foi
gravado, as 11 partições, entre elas `med-boot`. **Um dispositivo errado apaga outro disco.**

```bash
lsblk
```

O desktop monta as partições sozinho e o `bmaptool` recusa um disco em uso. Desmonte tudo
(troque `sdX` pelo que o `lsblk` mostrou, aqui e nos dois passos seguintes):

```bash
sudo umount /dev/sdX?* 2>/dev/null; lsblk /dev/sdX
```
```bash
sudo bmaptool copy build/tmp-glibc/deploy/images/stm32mp25-disco/med-image-eeg-stm32mp25-disco.rootfs.wic /dev/sdX
```

O `sgdisk -e` move o GPT reserva para o fim do cartão. Sem ele, todo boot reclama no `dmesg`
(`BRINGUP_STM32MP2.md` §9.16):

```bash
sudo sgdisk -e /dev/sdX
```
```bash
sudo eject /dev/sdX
```

Um cartão recém-gravado gera uma **chave SSH nova**. O `ssh` vai recusar o IP antigo com
`REMOTE HOST IDENTIFICATION HAS CHANGED`. Isso é esperado; apague a chave velha:

```bash
ssh-keygen -R 10.1.35.254
```

---

## 2. Montar

**Placa desligada.**

| Link | AFE | Cabo USB da ponte MCP2210 |
|---|---|---|
| `amp` | encaixado no header (SPI6, DRDY, pinos do hat; `BRINGUP_STM32MP2.md` §9.15) | **desconectado** |
| `usb` | fora do header | numa porta USB *host* da placa |

O cabo da ponte fica fora no `amp` por precaução: a imagem `amp` não carrega o driver da ponte, e
não está documentado se a ponte e o header dividem as linhas SPI do conversor.

No `usb`, use cabo **de dados, curto**. Um cabo ruim dá `error -71` na enumeração e nada mais
(`ROTEIRO_BANCADA_AFE.md`).

No `usb`, plugue a ponte **antes** de ligar a placa. Plugada depois, a aquisição sobe mas o slot
não é confirmado (`BRINGUP_AFE.md` §6.8), e cada boot assim gasta uma tentativa.

Ligue a placa. O boot até a aquisição leva ~13 s, ou ~23 s no primeiro boot de um cartão novo
(ele provisiona o `/data`).

---

## 3. Achar a placa

O IP vem por DHCP e pode mudar. Na rede `10.1.35.0/24`, liste quem tem SSH e o banner de cada um:

```bash
for h in $(nmap -n -p22 --open 10.1.35.0/24 -oG - | awk '/22\/open/{print $2}'); do ssh-keyscan -T 3 $h 2>&1 | grep '^# '; done
```

A placa é a que responde `SSH-2.0-OpenSSH_9.6` **sem sufixo**. Debian, Ubuntu e Raspbian
acrescentam o nome da distribuição ao banner, e o Yocto não. Em 2026-10-09 ela estava em
`10.1.35.254`.

Fixe as variáveis da rotina, com o IP encontrado e o link que você montou:

```bash
B=root@10.1.35.254; L=amp
```

E confirme que é a placa (deve imprimir `stm32mp25-disco` e o link certo):

```bash
ssh $B 'hostname; grep "^acquisition.link" /etc/medplatform/eeg.conf'
```

---

## 4. Fase 1 — a plataforma está de pé?

Não tem nada a ver com o AFE. Se falhar, o defeito está antes dele, e é isso que se investiga.

```bash
ssh $B 'systemctl is-system-running; systemctl list-units --state=failed --no-legend --plain; rauc status | grep -E "Booted|Activated"; cryptsetup status med-data | grep -E "type|is active"; date -u'
```

| Item | Esperado |
|---|---|
| `is-system-running` | `running`, ou `degraded` enquanto a falha conhecida abaixo existir. Esta rotina ainda não conferiu qual dos dois a placa dá hoje |
| unidades falhas | só `weston.service`. A HMI não sobe na placa (`BRINGUP_STM32MP2.md` §9.9), defeito conhecido e aberto. Qualquer outra unidade é falha nova |
| `rauc status` | `Booted from: rootfs.0 (A)` ou `.1 (B)`, e um `Activated:`. Para conferir o slot, ver a Fase 3 |
| `/data` | `type: LUKS2` e `is active` |
| `date -u` | se mostrar 2025, a placa não sincronizou o relógio. Não impede o teste, mas os carimbos de sessão ficam errados. Defeito conhecido (sem RTC inicializado) |

---

## 5. Fase 2 — o front-end do link está visível?

### Se `L=amp`

```bash
ssh $B 'for r in /sys/class/remoteproc/*; do echo "$(cat $r/name) $(cat $r/state)"; done; ls -l /dev/med-amp0'
```

Esperado: `m33 running` e `/dev/med-amp0 -> rpmsgN`. O `m0 offline` é normal.

### Se `L=usb`

```bash
ssh $B 'dmesg | grep -E "mcp2210|ads1299"; ls -l /dev/med-afe-eeg0 /run/med-afe/eeg0'
```

Esperado (`BRINGUP_AFE.md` §6.4):

- `USB-SPI bridge ready, 9 GPIOs, 8 chip selects, ads1299 attached`;
- `ads1299 spiN.4: self test passed: test signal DC level ~83500..83800 codes ... (expected 83886), shorted-input noise ...`;
- os dois links existem: `/dev/med-afe-eeg0 -> iio:deviceN` e `/run/med-afe/eeg0`.

O ruído do autoteste já variou de 28 a 205 códigos entre boots (`BRINGUP_AFE.md` §6.6–6.7). Isso
passa, porque o teto é ~447, mas anote o valor: a variação não tem explicação medida.

---

## 6. Fase 3 — a aquisição de fábrica está estável?

A config selada, como o dispositivo sai da fábrica: prescrição de fábrica, sinal de teste
desligado e lead-off ligado.

```bash
ssh $B 'systemctl show eeg-acquisition -p ActiveState -p NRestarts; journalctl -u eeg-acquisition -b -o cat --no-pager | grep -E "self test|session started|refused|absent" | tail -4; rauc status | grep -E "Activated"; fw_printenv BOOT_ORDER BOOT_A_LEFT BOOT_B_LEFT'
```

| Item | Esperado |
|---|---|
| serviço | `ActiveState=active` e **`NRestarts=0`**. Ativo com reinícios é um laço, não saúde |
| journal | `front-end self test passed` e `acquisition session started`, sem `refused` nem `absent` |
| slot | `Activated:` igual ao `Booted from:` da Fase 1 |
| contadores | o `BOOT_<slot>_LEFT` do slot bootado de volta a **3**: o slot foi confirmado *depois* da aquisição subir |

Com as entradas abertas e o lead-off ligado, as amostras ficam no fundo de escala e os eletrodos
aparecem soltos. Isso é o esperado sem eletrodos (`BRINGUP_STM32MP2.md` §9.16), e por isso esta fase
não julga o sinal. Quem julga é a Fase 4.

---

## 7. Fase 4 — o sinal de teste interno

O conversor gera uma onda quadrada própria: **±VREF/2400 = 1875 µV de pico, período de 256
conversões** (1,024 s a 250 SPS). A Fase 4 grava essa onda **pelo serviço real** e julga
amplitude, período, lacunas e CRC. Esta é a fase que diz que o hardware mede certo.

A config selada não muda. O teste usa uma cópia em `/run` (volátil), selada de novo e apontada
por um drop-in também volátil. Um reboot desfaz tudo, e o §7.6 desfaz sem reboot.

### 7.1 Montar a config de teste

Um `sed` por link, porque cada link fala outro vocabulário de opções (o `do_derive_device_options`
da receita do serviço). Rode **o do seu link**.

`amp`: liga o sinal de teste e **desliga o lead-off**, porque o firmware recusa lead-off com o
sinal de teste (não há eletrodo na medição):

```bash
ssh $B 'mkdir -p /run/med-bench && sed -e "s/^afe.test_signal = off/afe.test_signal = internal/" -e "s/^device.option.afe.test_signal = off/device.option.afe.test_signal = internal/" -e "s/^afe.lead_off_detection = true/afe.lead_off_detection = false/" -e "s/^device.option.afe.lead_off_detection = true/device.option.afe.lead_off_detection = false/" /etc/medplatform/eeg.conf > /run/med-bench/eeg.conf && grep -E "test_signal|lead_off|input_mux" /run/med-bench/eeg.conf'
```

Esperado: 4 linhas, com `internal` e `false`.

`usb`: liga o MUX no gerador. O lead-off **fica**, porque o driver de kernel liga os comparadores
no probe e não oferece como desligá-los:

```bash
ssh $B 'mkdir -p /run/med-bench && sed -e "s/^afe.test_signal = off/afe.test_signal = internal/" -e "s/^device.option.input_mux = normal/device.option.input_mux = test_signal/" -e "s/^device.option.test_signal = off/device.option.test_signal = 1x_slow/" /etc/medplatform/eeg.conf > /run/med-bench/eeg.conf && grep -E "test_signal|lead_off|input_mux" /run/med-bench/eeg.conf'
```

Esperado: `afe.test_signal = internal`, `device.option.input_mux = test_signal` e
`device.option.test_signal = 1x_slow`. A linha `afe.lead_off_detection = true` também aparece e
não muda.

**Se alguma linha não mudou, pare.** O `sed` não casou com o arquivo instalado, e o teste
gravaria a config de fábrica achando que é a de teste.

### 7.2 Selar e reiniciar o serviço pela cópia

```bash
ssh $B 'cd /run/med-bench && sha256sum eeg.conf | cut -d" " -f1 > eeg.conf.sha256 && mkdir -p /run/systemd/system/eeg-acquisition.service.d && printf "[Service]\nExecStart=\nExecStart=/usr/bin/eeg-acquisition-service --config /run/med-bench/eeg.conf\n" > /run/systemd/system/eeg-acquisition.service.d/bench.conf && systemctl daemon-reload && systemctl restart eeg-acquisition && sleep 6 && systemctl show eeg-acquisition -p ActiveState -p NRestarts && journalctl -u eeg-acquisition -b -o cat -n 4 --no-pager'
```

Esperado: `ActiveState=active`, `configuration verified`, `front-end self test passed` e
`acquisition session started`. Se o serviço não subir, o motivo está no journal
(`journalctl -u eeg-acquisition -b -o export | grep -E "^MED_(DETAIL|STATUS)="`). Um selo que não
confere com a cópia e uma opção que o front-end recusa aparecem ali. Vá ao §7.6 e recomece do §7.1.

### 7.3 Gravar 30 s e parar

Parar fecha a sessão (`closed.json`) e dispara o conversor clínico (`record.bdf`). O caminho da
sessão vai para `S`:

```bash
S=$(ssh $B 'sleep 30; systemctl stop eeg-acquisition; ls -d /data/eeg/session-* | tail -n 1'); echo "$S"
```

### 7.4 Copiar e conferir

O `scp` não funciona com a placa (não há servidor SFTP), então a cópia é por `cat`. Conferir o
sha256 dos dois lados não é opcional: uma cópia corrompida produz um veredito sobre o cabo de
rede, não sobre o AFE.

```bash
D=build/med-bench/$(date +%Y%m%d-%H%M)-$L; mkdir -p $D; for f in raw.bin metadata.json closed.json record.bdf record.bdf.sha256; do ssh $B "cat $S/$f" > $D/$f; done; ssh $B "cd $S && sha256sum raw.bin"; (cd $D && sha256sum raw.bin && sha256sum -c record.bdf.sha256); cat $D/closed.json
```

Esperado:

- os dois sha256 do `raw.bin` iguais;
- `record.bdf: OK`;
- no `closed.json`, `"reason": "stop requested"` e `"dropped": 0`.

Se o `record.bdf` não existir, a imagem é anterior ao conversor clínico. Pule o julgamento do BDF
em 7.5.

### 7.5 Julgar e plotar

O juiz do sinal de teste se chama `amp-bench.py`, mas lê o formato de quadro, e o `raw.bin` tem o
mesmo formato nos dois links:

```bash
python3 scripts/amp-bench.py converter $D/raw.bin
```

O juiz do arquivo clínico confere o `record.bdf` código a código contra o `raw.bin`:

```bash
python3 scripts/amp-bench.py bdf $D
```

As formas de onda e o CSV:

```bash
python3 - $D <<'EOF'
import struct, sys, zlib, json, numpy as np, matplotlib
matplotlib.use("Agg"); import matplotlib.pyplot as plt
d = sys.argv[1]; raw = open(d + "/raw.bin", "rb").read(); meta = json.load(open(d + "/metadata.json"))
H = struct.Struct("<IHHIIQQiI"); o = 0; rows = []
while o + 40 <= len(raw):
    m, v, ch, spc, r, seq, ts, sc, crc = H.unpack_from(raw, o); n = ch * spc * 4; t = 8 if v >= 2 else 0
    body = raw[o + 40:o + 40 + n + t]
    if m == 0x4D454547 and zlib.crc32(body) & 0xFFFFFFFF == crc:
        rows.append(np.frombuffer(body[:n], "<i4").reshape(spc, ch) * sc / 1000.0)
    o += 40 + n + t
x = np.vstack(rows); fs = int(round(meta["sample_rate_hz"])); tt = np.arange(len(x)) / fs
np.savetxt(d + "/samples_uV.csv", np.column_stack([tt, x]), delimiter=",", fmt="%.4f",
           header="t_s," + ",".join(f"ch{c+1}_uV" for c in range(x.shape[1])), comments="")
for name, sl in (("overview", slice(None)), ("zoom-3s", slice(0, 3 * fs))):
    fig, ax = plt.subplots(x.shape[1], 1, sharex=True, figsize=(13, 11))
    for c in range(x.shape[1]):
        ax[c].plot(tt[sl], x[sl, c], lw=0.7); ax[c].set_ylabel(f"ch{c+1}\nµV", rotation=0, labelpad=22); ax[c].grid(alpha=.3)
    ax[-1].set_xlabel("tempo (s)"); fig.suptitle(f"link {meta['link']} · sessão {meta['session']}")
    fig.tight_layout(); fig.savefig(f"{d}/{name}.png", dpi=110); plt.close(fig)
print("pronto:", d)
EOF
```

Em `$D` ficam `overview.png`, `zoom-3s.png`, `samples_uV.csv` (tempo e 8 canais em µV) e o
`record.bdf`, que abre no EDFbrowser.

**Critério de aprovação da Fase 4:**

| Medida | Critério | `amp`, 2026-10-09 | `usb`, 2026-10-10 |
|---|---|---|---|
| lacunas / CRC errado | 0 / 0 | 0 / 0 | 0 / 0 |
| `dropped` no `closed.json` | 0 | 0 | 0 |
| amplitude, cada canal | 1875 µV ±20% (1500–2250) | 1859,3–1860,1 µV | 1859,3–1860,1 µV |
| período, cada canal | 256 ± 2 conversões | 256,00 | 255,78 e 256,09 (duas sessões) |
| BDF contra `raw.bin` | 0 códigos diferentes, selo `OK` | 0 em 85.232 | 0 em 182.000 |
| veredito do `converter` | `VEREDITO: OK` | OK | OK |

A coluna `usb` é da imagem com a correção da escala (`BRINGUP_AFE.md` §6.8). Numa imagem anterior
a ela o `record.bdf` não existe e o `export.json` da sessão diz `refused`.

Ao ler o `converter` no `usb`, três diferenças:

- **A linha "taxa pelo relógio do M33" não é do M33.** No `usb`, o carimbo de cada quadro é a hora
  em que o host foi avisado. A linha mede o relógio do Linux e o jitter do USB, não o conversor.
- **A perda do link aparece como amostras que faltam, não como lacuna.** Ela é contada pelo driver
  (`lost_samples`), e os quadros continuam com a sequência contínua (`BRINGUP_AFE.md` §6.5). **Só
  se o GP6 da ponte contar bordas**: com ele provisionado para o LED, o `dmesg` diz `this link
  cannot report lost samples`, e o período é o único indício. Com o
  governador `performance` ela foi ≤ 0,003% em 30 min. Uma perda maior desloca as bordas e pode
  tirar o período da tolerância de ±2.
- **O link entrega no máximo 250 leituras/s**, em qualquer ODR. Esta fase usa 250 SPS. Acima disso
  o `usb` perde amostras por construção (`BRINGUP_AFE.md` §6.7).

Para olhar a onda **sem** o sinal de teste (entradas abertas, config de fábrica), pule 7.1, 7.2 e
7.6 e use `leadoff` em vez de `converter` no 7.5. Sem o gerador não há amplitude a julgar, só a
integridade e o que o lead-off reportou.

### 7.6 Restaurar a config selada

```bash
ssh $B 'rm -f /run/systemd/system/eeg-acquisition.service.d/bench.conf && systemctl daemon-reload && systemctl restart eeg-acquisition && sleep 6 && systemctl show eeg-acquisition -p ActiveState -p NRestarts -p ExecStart | cut -c1-120'
```

**Confira a saída, não só o código de saída.** O `ExecStart` tem de voltar a citar
`/etc/medplatform/eeg.conf`, com `ActiveState=active` e `NRestarts=0`. Um serviço que sobe pela
cópia de teste parece igual a um saudável. O `/run/med-bench/` fica até o próximo boot, e é
inofensivo: nada o lê.

### 7.7 A montagem referencial (imagem de 2026-10-10 em diante; os registradores, só no `amp`)

O firmware fecha a chave do SRB1 quando a prescrição diz `afe.montage = referential`
(`implementation_plan_bdf.md` Fase 5). Este passo confere, no silício, que o conversor aceitou os
registradores. **Não** confere que a chave fechou, e para isso é preciso sinal no pino SRB1.

Mesma mecânica do 7.1 e 7.2, com outra troca. O lead-off fica ligado, porque é ele que muda:

```bash
ssh $B 'mkdir -p /run/med-bench && sed -e "s/^afe.montage = bipolar/afe.montage = referential/" -e "s/^device.option.afe.montage = bipolar/device.option.afe.montage = referential/" -e "s/^record.montage = bipolar/record.montage = referential/" /etc/medplatform/eeg.conf > /run/med-bench/eeg.conf && grep -E "montage = " /run/med-bench/eeg.conf'
```

Esperado: 3 linhas, as três com `referential`. Se vier só uma, a imagem é anterior e o firmware
recusaria a chave. Depois o comando do 7.2, sem mudança, e então o log do firmware e a auditoria:

```bash
ssh $B 'cat /sys/kernel/debug/remoteproc/remoteproc*/trace0 | tr -d "\0" | grep -E "\[afe\] (configured|lead-off)" | tail -n 3; journalctl -u eeg-acquisition -b -o export | grep -E "^MED_(MONITORED|DETACHED)=" | tail -n 2'
```

| Medida | Critério |
|---|---|
| linha `[afe] configured` | `SENS ff 00 ff 01 MISC1 20` |
| `lead-off check` | status e `LOFF_STAT` iguais pelos dois caminhos |
| `MED_MONITORED` | `1+ 2+ 3+ 4+ 5+ 6+ 7+ 8+ REF` |
| `MED_DETACHED`, bancada sem eletrodo | os oito `+` e `REF` |
| `metadata.json` da sessão | `"montage": "referential"` |

**Leia o trace logo depois do restart.** Ele guarda umas 12 linhas e o firmware escreve uma a cada
10 s, então a linha `[afe] configured` some em menos de dois minutos. **E não reinicie a placa no
meio:** a config de teste vive em `/run`, e um boot devolve o bipolar de fábrica sem avisar.

**A chave SRB1 da placa do AFE** é manual e o software não a lê. Bipolar: **GNDA**. Referencial
para adquirir: **eletrodo**. Mova a chave antes de reiniciar o serviço na montagem nova.

Depois o 7.6, e confira no log que a volta ao bipolar escreveu `SENS ff ff ff ff MISC1 00`: é a
mesma função que abre a chave, e uma sessão bipolar depois desta tem de encontrá-la aberta.

Registrar em `BRINGUP_STM32MP2.md` §9, sessão da data.

### 7.8 A chave do SRB1, vista no sinal — **executado em 2026-10-10, com um fio**

O §7.7 prova que o registrador guardou o byte. Que a chave fechou, só o sinal prova.

**Não tente com as entradas abertas.** Cada canal tem 4,7 nF entre P e N e nenhum caminho DC
(`schematic.pdf`, folha `eeg_input`): um canal aberto mede a carga desse capacitor, igual em
qualquer montagem. Três rodadas foram gastas assim. Multímetro nas entradas também não serve: ele
descarrega o nó que mede.

Um fio em Y no DB25, **com a placa desligada**: `BIAS_DRV` (pino 1) a `IN_SRB` (pino 13) e a
`IN1P` (pino 24). O pino 14, vizinho do 1, é GNDA: o fio do BIAS não pode encostar nele.

| Passo | Chave manual SRB1 | Comando | Canal 1 esperado |
|---|---|---|---|
| 1 | GNDA | `ssh $B sh -s -- bipolar < scripts/montage-bench.sh` | no topo, ou descendo dele |
| 2 | **eletrodo**, antes do comando | `… -- referential < …` | ≈ 0 mV desde a primeira amostra, ruído < 1 µV rms |
| 3 | **GNDA**, e esperar 10 s antes do comando | `… -- bipolar < …` | nos 10 s: salta ao topo |
| 4 | GNDA | `… -- fabrica < …` | — |

Os canais 2 a 8, sem fio, não podem ter degrau: são o controle. Depois, `amp-bench.py levels` em
cada `raw.bin`. Medido em 2026-10-10: 181,8 mV → 0,039 mV no passo 2, e +187,5 mV em 0,2 s no
passo 3 (`BRINGUP_STM32MP2.md` §9.17).

**No `usb` a mesma tabela vale, com três diferenças** (executado em 2026-10-10, `BRINGUP_AFE.md`
§6.9). O `montage-bench.sh` lê o link da config selada e imprime `montage attribute:` em vez da
linha do firmware; não há registrador relido, porque o driver de kernel o guarda em cache. O
lead-off não é desligado, porque esse driver não liga fonte de corrente em canal nenhum. E, sem
essas fontes, nada carrega o capacitor de entrada: no passo 1 o canal 1 fica onde estiver (foram
−52 mV), não no topo, e o degrau do passo 2 pode ser pequeno. O que decide é o passo 3. Medido:
−52 mV → 1,9 mV na primeira amostra do passo 2, e +187,5 mV no passo 3.

**Com gerador, para medir amplitude (não executado; falta material).** No referencial cada canal
mede `INnP − SRB1`, então um seno no pino SRB1 tem de aparecer nos oito canais.

**Por que só o gerador não basta.** O gerador da bancada entrega 0 a 100 mV, sem offset ajustável.
No ganho 24 as entradas trabalham entre ~0,8 V e ~4,2 V (AVDD de 5 V, unipolar), então esse sinal
direto no pino está fora da faixa de modo comum e a leitura não significaria nada. É preciso
deslocá-lo para 2,5 V, e isso pede quatro componentes:

| Componente | Para quê |
|---|---|
| 2 × 1 kΩ | divisor AVDD–GND: o nó M, 2,5 V, nas oito `INnP` |
| 1 × 100 kΩ | do nó M ao SRB1: o mesmo DC dos dois lados |
| 1 × 1 µF, cerâmico ou de filme | do gerador ao SRB1: passa o seno, bloqueia o DC |

Gerador: seno, 10 Hz, 0 a 100 mV. Esperado nos canais: ≈ 49 mV de pico. Ligar a placa primeiro,
conferir M e SRB1 em 2,5 V com o multímetro, e só então conectar o gerador. Rodar com
`afe.bias_drive` e `afe.lead_off_detection` em `false`, para a chave ser a única variável.

Três capturas de 30 s, com a mesma fiação, julgadas pelo `amp-bench.py tone`:

| Captura | Config | Comando | Esperado |
|---|---|---|---|
| A | referencial | `tone $D/raw.bin --hz 10 --uv 49000` | `VEREDITO: OK`: os 8 canais a ±10%, em fase (±5°) |
| B | bipolar | `tone $D/raw.bin --hz 10 --below 4900` | `OK`: a chave aberta não deixa o seno passar |
| C | referencial, gerador desconectado | `tone $D/raw.bin --hz 10 --below 4900` | `OK`: A não era captação |

A sozinha não basta: é B que mostra que o sinal chegou pela chave.

**Antes de montar, conferir na placa do AFE** (não está registrado em lugar nenhum): onde o SRB1 e
as `INnP` são acessíveis, e o que cada posição da chave de SRB1 da PCB liga ao pino. Se uma posição
o põe em GND, o teste é na outra.

### 7.9 Ruído com as entradas definidas — **não executado**

O piso de ruído da placa, pelos pinos de entrada. Só vale com **todas** as entradas ligadas a
algum lugar: uma entrada solta capta a rede (2 a 4 mV) e, pela soma do bias, contamina as outras.

**Plugue de curto.** Um DB25 de soldar, com um fio unindo o `BIAS_DRV` (pino 1) às dezesseis
entradas: `INnP` nos pinos 24, 23, 22, 21, 18, 17, 16, 15 e `INnN` nos pinos 11, 10, 9, 8, 5, 4,
3, 2. **Fora do fio**: os pinos `NC` (6, 7, 12, 14, 19, 20, 25), que são GNDA, e o 13 (`IN_SRB`).
A pinagem foi lida do `schematic.pdf`: confira no KiCad antes de soldar.

**Plugue de resistores**, depois do curto: o mesmo, com um resistor em série em cada entrada
(5 a 50 kΩ, a faixa de um eletrodo), todos indo ao nó do BIAS. Serve para o ruído com impedância,
e para soltar um resistor de propósito e ver o que o lead-off reporta.

| Passo | Chave manual SRB1 | Comando |
|---|---|---|
| 1 | **GNDA** (a montagem é bipolar) | placa desligada, encaixar o plugue, ligar |
| 2 | GNDA | `ssh $B sh -s -- bipolar < scripts/montage-bench.sh`, e deixar gravando 2 min |
| 3 | GNDA | `ssh $B sh -s -- fabrica < scripts/montage-bench.sh` |

Copiar a penúltima sessão como no §7.4 e julgar com
`python3 scripts/amp-bench.py noise $D/raw.bin` (descarta os 10 primeiros segundos). Não há
critério medido para esta placa ainda. A única referência é um canal, 11 s, em 2026-10-10, com as
duas entradas no BIAS: **0,48 µV rms, 1,5 µV de pico a pico, 0,7 µV de rede**, offset de 30 µV
ainda derivando 1,4 µV/s.

---

## 8. Quando uma fase falha

| Fase | Sintoma | Causa provável | O que fazer |
|---|---|---|---|
| 3 (`ssh`) | `REMOTE HOST IDENTIFICATION HAS CHANGED` | cartão regravado ou troca de slot: cada slot gera a própria chave | `ssh-keygen -R <ip>` e conferir o fingerprint ao reconectar |
| 3 | a placa não está em `10.1.35.254` | DHCP | o laço do §3 |
| 2, `amp` | `front-end absent: no converter answered on SPI6` (no journal, `MED_DETAIL`) | AFE fora do header, sem alimentação, ou fiação. Em 08/10 foi um resistor mal soldado | conferir montagem e alimentação. O DRDY parado é sinal de alimentação ou fiação, não de firmware (`BRINGUP_STM32MP2.md` §9.16) |
| 2, `amp` | `m33` não `running` | firmware ausente: a imagem não é `amp` | §1.1 e §1.3 |
| 2, `usb` | sem `/dev/med-afe-eeg0` e sem linha `mcp2210` no `dmesg` | a imagem não é `usb` (sem os módulos), ou a ponte não enumerou | §1.1. A ponte aparece no barramento mesmo sem driver: `cat /sys/bus/usb/devices/*/idVendor` deve listar `04d8` |
| 2, `usb` | `error -71` no `dmesg` | cabo USB | trocar por um cabo de dados curto |
| 3, `usb` | serviço ativo, `NRestarts` > 0 e `BOOT_<slot>_LEFT` abaixo de 3 | a ponte foi plugada depois do boot: a aquisição subiu no reinício e o slot não foi confirmado | `systemctl start rauc-mark-good.service`, e conferir `fw_printenv`. **Não reiniciar antes** |
| 4 | `VEREDITO: OK` mas sem `record.bdf` | o exportador recusou a sessão | `cat $S/export.json` diz o motivo. "not a whole number of steps" numa imagem `usb`/`spi` é a escala de `BRINGUP_AFE.md` §6.8: a imagem é anterior à correção |
| 2, `usb` | `mcp2210 ... ready` mas sem `self test passed` | o conversor não respondeu pela ponte | no PC, o `scripts/afe-alive.py` separa "chip morto" de "DOUT não chega à MISO" (`ROTEIRO_BANCADA_AFE.md`) |
| 3 | `ActiveState=activating` e `NRestarts` subindo | o front-end falha no autoteste: laço de 2,5 s, que **nunca** chega a `failed` | ver o motivo com `journalctl -u eeg-acquisition -b -o export \| grep MED_DETAIL`. Parar o laço com `systemctl stop eeg-acquisition`, e **não** reiniciar a placa em série (regra 3 do §0) |
| 3 | `Booted from: rootfs.1 (B)` sem ninguém ter instalado nada | fallback: três boots sem aquisição esgotaram o slot A | esperado, com front-end ausente. Corrigida a causa, a placa volta sozinha |
| 4 | `ack: rejectedIndex N, "...lead_off...on the test signal"` | no `amp`, lead-off ligado junto com o sinal de teste | o `sed` do §7.1 não casou: conferir o `grep` |
| 4 | amplitude ~0 ou fundo de escala (~187.500 µV) | o sinal de teste não chegou: a config de teste não foi a usada | conferir o `ExecStart` (§7.2) e o `metadata.json` (`"afe.test_signal": "internal"`) |
| 4 | `bad status 00 00 00` na conversão 1 | padrão conhecido depois de certas configurações, descartado e contado | anotar; não é falha da fase se o veredito passar (`BRINGUP_STM32MP2.md` §9.16) |

---

## 9. Registrar

Uma sessão de bancada que não foi registrada não aconteceu. O dia 07/10 teve de ser reconstruído
a partir de comentários no código (`BRINGUP_STM32MP2.md` §9.16).

Onde anotar:

- **Na placa, qualquer link:** uma subseção nova no `BRINGUP_STM32MP2.md` §9, com a data.
- **O que for do front-end** (autoteste, ruído, ponte): `BRINGUP_AFE.md`.
- **Um número que entra na tese:** `RESULTS.md`, com o comando que o produziu e o que ele não significa.

O mínimo que cada registro leva:

```
Data / hora (UTC)          :
O que mudou no hardware    :
Link / imagem              : amp | usb, commit do repositório, commit do linux-med
Slot bootado               : A | B
Fase 1                     : OK | falhou: ...
Fase 2                     : OK | falhou: ...   (usb: ruído do autoteste = ... códigos)
Fase 3                     : NRestarts=0, slot confirmado
Fase 4                     : quadros, lacunas, CRC, dropped, amplitude min–max, período, BDF, VEREDITO
Diretório                  : build/med-bench/<data>-<link>/
O que isto NÃO significa   :
```

O diretório `build/med-bench/` não vai para o git. O registro em texto é o que fica.
