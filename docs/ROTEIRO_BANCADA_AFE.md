# Roteiro de bancada — o front-end analógico num PC Ubuntu

> **O que este documento é**: o passo a passo, do zero, para pôr a placa de AFE (ADS1299 +
> MCP2210) num PC com Ubuntu, carregar os dois drivers do `linux-med` e repetir as fases de bancada
> que já foram executadas, com o valor que cada uma deve dar. Quem seguir isto numa máquina nova
> chega aos mesmos números, ou descobre que não chegou.
>
> **O que ele não é**: nem o plano nem o registro. Os critérios e o porquê de cada fase estão no
> `implementation_plan_afe_bench.md`. O que já foi medido, e o que isso significa, está no
> `BRINGUP_AFE.md`, que é onde os números de uma sessão nova vão parar. Este roteiro só diz
> **como** rodar, e cita a seção do registro de onde vem cada valor esperado.
>
> **Escopo**: o link `usb`, num host x86-64. A placa STM32MP257 tem outro caminho (imagem
> construída com `make stm32 KERNEL=med`, sem `python3` no alvo), registrado no `BRINGUP_AFE.md`
> §6.4–§6.7. As fases que pedem instrumento (2, 8-A, 8-B, critério 6 da Fase 1) também não estão
> aqui: elas não foram executadas e não têm valor esperado para conferir.

Executado até a Fase 4 em 2026-10-05: Ubuntu 24.04, kernel `6.8.0-146-generic`, `linux-med` em
`board/medplatform-afe` (`b8dbcb083402`), ponte serial `0002589022`. A Fase 5 foi rodada assim em
2026-10-04 (§6.3). O `afe-phase1.sh` foi ajustado depois da sessão de 2026-10-05 (SKIP no critério
5 e restauração do módulo), e essa versão ainda não rodou contra a peça.

---

## 1. Material

| Item | Observação |
|---|---|
| Placa de AFE montada | CS no **GP4**, DRDY no **GP5**, LED no **GP6**; GP0–GP3, GP7 e GP8 **aterrados** (`BRINGUP_AFE.md` §2.3) |
| Cabo USB **de dados, curto** | um cabo ruim dá `error -71` na enumeração e nada mais (§7) |
| PC com Ubuntu e Secure Boot | os módulos precisam ser assinados com uma chave MOK registrada (§3.2) |
| Multímetro | opcional; só para o `--avdd` da Fase 4. Sem ele, a comparação do MVDD não vale |

**Nenhum eletrodo e nenhuma pessoa ligados ao conversor.** Tudo aqui usa as entradas internas do
MUX. Com as entradas flutuando, qualquer leitura com o MUX em `normal` não significa nada
(`implementation_plan_afe_bench.md` §3, `BRINGUP_AFE.md` §4).

**Regra da placa**: nunca dirigir nível 1 em GP0–GP3, GP7 ou GP8 pelo `gpiochip`. Esses pinos
estão no terra, e o driver não tem como saber disso.

---

## 2. Antes de qualquer hardware

A árvore do kernel deve estar na branch da placa, que é a que nunca reescreve as configurações da
ponte:

```bash
cd ~/Documents/TCC/yocto_workspace
git -C linux-med branch --show-current        # board/medplatform-afe
make test                                     # tests/framework + tests/mcp2210
```

O `tests/mcp2210` escolhe a variante pela branch e imprime `variante: board`. Esperado: 0 falhas.
É a única coisa que vê defeito no código de kernel sem a placa. Se ela falhar, nenhum número de
bancada abaixo vale.

---

## 3. Preparar o host (uma vez por máquina)

### 3.1 Ferramentas

```bash
sudo apt install build-essential linux-headers-$(uname -r) mokutil python3
```

### 3.2 A chave que assina os módulos

O Ubuntu, com Secure Boot, recusa módulo sem assinatura de uma chave **registrada no MOK**. A
chave de bancada deste projeto fica **fora do repositório**, em `~/.mok/`:

| Arquivo | Conteúdo |
|---|---|
| `~/.mok/MOK.priv` | chave privada, `chmod 600` |
| `~/.mok/MOK.der` | certificado, subject `CN = MedPlatform bench module key` |

**Se ela já existe**, confirme que está registrada e que a privada é o par do certificado, e pule
para a §4:

```bash
mokutil --test-key ~/.mok/MOK.der                                   # "is already enrolled"
openssl x509 -inform DER -in ~/.mok/MOK.der -noout -pubkey | sha256sum
sudo openssl pkey -in ~/.mok/MOK.priv -pubout | sha256sum           # os dois hashes iguais
```

**Numa máquina nova**, crie e registre a chave:

```bash
mkdir -p ~/.mok && cd ~/.mok
openssl req -new -x509 -newkey rsa:2048 -nodes -days 3650 \
  -subj "/CN=MedPlatform bench module key/" \
  -addext "basicConstraints=critical,CA:FALSE" \
  -addext "keyUsage=digitalSignature" \
  -addext "extendedKeyUsage=codeSigning,1.3.6.1.4.1.2312.16.1.2" \
  -keyout MOK.priv -outform DER -out MOK.der
chmod 600 MOK.priv
sudo mokutil --import MOK.der        # pede uma senha de uso único
sudo reboot
```

**No reboot, fique olhando a tela.** Aparece uma tela azul (MokManager). Aperte uma tecla antes de
o contador de ~10 s acabar e escolha **Enroll MOK → Continue → Yes**, depois digite a senha. Se o
contador acabar, o pedido se perde: rode o `--import` de novo. Depois do boot,
`mokutil --test-key ~/.mok/MOK.der` tem de dizer `is already enrolled`.

> **Armadilha de 2026-10-05**: uma chave nova foi gerada na raiz do repositório quando a de
> `~/.mok` já existia e já estava registrada. O registro da nova se perdeu num reboot sem ninguém
> olhando, e o `--test-key` passou porque testou a chave **antiga**. O sintoma foi
> `Key was rejected by service`. A conferência que separa as duas é o `modinfo -F signer` da §4,
> comparado com o subject do certificado. E nunca gere chave dentro do repositório: ela não está no
> `.gitignore`.

---

## 4. Compilar, assinar e instalar os módulos (a cada mudança no `linux-med`)

```bash
cd ~/Documents/TCC/yocto_workspace
make -C scripts/oot-modules clean && make -C scripts/oot-modules
for m in build/oot-modules/{hid-mcp2210,ti-ads1299}.ko; do
  sudo /usr/src/linux-headers-$(uname -r)/scripts/sign-file sha256 ~/.mok/MOK.priv ~/.mok/MOK.der $m
done
sudo install -D -m644 build/oot-modules/*.ko -t /lib/modules/$(uname -r)/updates/
sudo depmod -a
modinfo -F signer hid_mcp2210        # MedPlatform bench module key
modinfo -F signer ti_ads1299         # MedPlatform bench module key
```

- O `clean` não é opcional. Assinar um `.ko` já assinado empilha uma segunda assinatura.
- Os caminhos são relativos à raiz do repositório. Fora dela, o `sign-file` falha com
  `No such file or directory`.
- O aviso `the compiler differs from the one used to build the kernel` é só a diferença entre os
  nomes `x86_64-linux-gnu-gcc-13` e `gcc-13`, mesma versão. O `Skipping BTF generation` também é
  inofensivo.

**Opcional, para não repetir o parâmetro**: instale no host o mesmo `modprobe.d` que a imagem
instala, e o autoload já sobe com o chip select certo.

```bash
sudo install -m644 meta-custom/meta-med-afe-ads1299/recipes-core/modprobe/files/med-afe-bridge.conf \
  /etc/modprobe.d/
```

O arquivo diz `spi_chip_select=4 led_gpio=6`. Sem ele, o autoload usa `spi_chip_select=0` e o
driver da placa recusa o GP0, que está aterrado (`GP0 is a GPIO in the bridge's settings, not a
chip select`, `cannot attach 'ads1299' … -16`). Essa recusa é proposital (`BRINGUP_AFE.md` §2.3,
mudança 1). Sem o `led_gpio=6`, o LED fica aceso fixo, como a NVRAM o liga.

---

## 5. A sessão

### 5.1 Plugar e conferir a enumeração

```bash
lsusb | grep 04d8                    # ID 04d8:00de Microchip Technology, Inc. MCP2210 USB to SPI Master
```

O LED do GP6 acende sozinho: é a NVRAM provisionada (§5.2). Quando o driver carrega com
`led_gpio=6`, ele passa a piscar a 1 Hz com o SPI ocioso e fica aceso fixo durante uma aquisição.
Então LED fixo sem aquisição quer dizer "sem driver". Se o `lsusb` não mostra nada, vá à §7.

### 5.2 Provisionar a ponte (uma vez por peça; esta já foi)

Uma ponte **de fábrica** briga com a placa desde o reset: põe saídas em nível alto contra pinos
aterrados (`BRINGUP_AFE.md` §2.3). Antes de mais nada, numa peça nova:

```bash
sudo modprobe -r hid_mcp2210                         # o provisionamento fala por hidraw
sudo python3 scripts/mcp2210-provision.py            # só mostra o que gravaria
sudo python3 scripts/mcp2210-provision.py --write    # grava, depois de digitar a confirmação
```

Depois, desplugue e plugue: a NVRAM só é lida no reset. A peça `0002589022` foi provisionada em
2026-10-04 (§2.3.1 e §2.3.2) e **não deve ser regravada**. A §5.3 confirma que ela continua
provisionada.

### 5.3 Linha de base da NVRAM e oráculo (com a ponte no `hid-generic`)

Os dois falam com a ponte por `/dev/hidraw`, que o nosso driver não expõe:

```bash
sudo modprobe -r hid_mcp2210
sudo python3 scripts/mcp2210-nvram-snapshot.py -o ~/nvram-antes.txt
sudo python3 scripts/afe-spi-oracle.py
```

| Saída | Esperado | Fonte |
|---|---|---|
| snapshot: quatro cruzamentos com a enumeração | `PASS` VID, PID, produto, fabricante | §2.1 |
| snapshot: `0x20 chip-power-up` documentado | `00000000010000000040ffbfff1200` | §2.3.2 |
| snapshot: `sha256-documented` | `4cad3871…9aafe45` (esta peça, provisionada) | §2.3.2 |
| oráculo: ID | `0x3e` (8 canais) | §4.2 |
| oráculo: depois de RESET | `0 registrador(es) de configuracao fora do reset` | §4.2 |
| oráculo: escrita e releitura | 3 `PASS` | §4.2 |
| oráculo: sinal de teste, contínuo | ~167 300 códigos pico a pico nos 8 canais (±VREF/2400) | §4.2, A1 |
| oráculo: sinal de teste, single-shot | ~20–40 códigos (é ruído, e é o esperado) | §4.3 |

Um `sha256-documented` diferente quer dizer que a NVRAM mudou desde a última sessão. Pare e
compare com `--compare ~/mcp2210-nvram-provisionada.txt` antes de seguir.

### 5.4 Carregar os drivers

```bash
sudo modprobe hid_mcp2210 spi_chip_select=4 led_gpio=6   # spi_device já é "ads1299" por padrão
sudo dmesg | grep -E 'mcp2210|ads1299' | tail -6
```

Esperado, nesta ordem:

```
mcp2210 …: GP6 is not provisioned to count edges; no counter, and this link cannot report lost samples
mcp2210 …: USB-SPI bridge ready, 9 GPIOs, 8 chip selects, ads1299 attached
ads1299 spi1.4: supply avdd not found, using dummy regulator
mcp2210 …: asked for 2048000 Hz, bridge programmed 2000000 Hz
ads1299 spi1.4: no DRDY interrupt: sampling on a host timer, timestamps are host side
ads1299 spi1.4: self test passed: test signal DC level ~83540..83830 codes from offset (expected 83886), shorted-input noise ~25 codes
```

- O aviso do GP6 é desta placa: o GP6 é o LED, e o DRDY está no GP5. Não há contador de bordas.
- O autoteste leva uns 2 s. Espere o `self test passed` antes de seguir.
- Um `self test failed` ou `Unknown ID` é resultado: registre e vá ao plano.

O front-end tem de aparecer como dispositivo IIO:

```bash
grep -H . /sys/bus/iio/devices/iio:device*/name      # ads1299-8
```

### 5.5 O LED de estado

```bash
sudo sh scripts/afe-led-test.sh
```

| Etapa | Esperado |
|---|---|
| 0. driver e linha | `led_gpio=6`; `status-led` no debugfs de gpio, como saída |
| 1. SPI ocioso, 10 s a 10 Hz | ~1 Hz (0,85 a 1,15) e aceso em ~50% das amostras |
| 2. SPI em uso (leituras avulsas do conversor) | 0 trocas, aceso em todas as amostras |
| 3. depois do uso | volta a piscar |
| 4. a olho | o LED pisca de verdade; o script lê o latch da ponte, não a luz |

O script não liga o buffer nem muda nenhum ajuste. Se houver uma aquisição em curso, ele mede nela
mesma e pula a etapa 1.

### 5.6 Fase 1 — a ponte

```bash
sudo sh scripts/afe-phase1.sh
```

O script recarrega o módulo com `spi_device=` vazio, porque o critério 2 pede a ponte sem nada
atrás. No fim, ele restaura os parâmetros com que o módulo estava.

| Critério | Esperado nesta placa |
|---|---|
| 1. VID/PID, full-speed | PASS, PASS (`04d8:00de`, 12 Mbit/s) |
| 2. linha do probe | PASS (`nothing attached`) |
| 3. bind nosso, não do `hid-generic` | PASS |
| 4. `gpiochip` com 9 linhas | PASS |
| 5. contador | **SKIP**: não se aplica a esta provisão (GP6 é o LED) |
| 6. 1000 pulsos | **SKIP**: idem, e pediria gerador |

Total esperado: **6 PASS, 0 FAIL, 2 SKIP**. O `cannot open /sys/firmware/devicetree/base/model`
no topo é só porque o x86 não tem devicetree.

Depois do script, confira que o conversor voltou (`self test passed` de novo no `dmesg`) antes da
Fase 4.

### 5.7 Fase 4 — o caminho analógico, estático (~2 min)

```bash
sudo python3 scripts/afe-phase4.py --avdd <AVDD medido>     # sem multímetro: omita --avdd
```

| Seção | Esperado | Medido em 2026-10-04 / 2026-10-05 (§5) |
|---|---|---|
| 8.1 dispersão entre ganhos | < 1% | 0,72 / 0,71% (1x); 0,34 / 0,34% (2x) |
| 8.1 desvio da média | < 5% de 1,875 e 3,750 mV | −0,04% (1x); −0,18% (2x), nos dois dias |
| 8.1 razão 2x/1x | ~2 | 1,992 a 1,999 |
| 8.1 offset em curto | quase constante em códigos | ~−1000 no ganho 1, ~−785 no ganho 24 |
| 8.4 ruído em curto, ganho 24 | sem veredito (tabela do datasheet não transcrita); < 10 µV pp | 0,13 / 0,12 µV RMS |
| 8.5 MVDD | ±2% de AVDD/2, **só se o `--avdd` foi medido** | 2,4972 / 2,4983 V |
| 8.5 temperatura | sem veredito (fórmula não transcrita) | 34,2 / 32,0 °C |
| injeção: curto com o gerador ligado | PASS: move só alguns códigos | 2 / 0 códigos |
| 8.3 gerador pelo buffer | PASS: −0,3% do esperado; 1249 amostras em 5 s | os quatro ajustes nos dois dias |

O ruído com só 40 leituras varia de um dia para o outro: no ganho 1 deu 5,4 µV pp num dia e 3,2 no
outro. Compare a ordem de grandeza, não o número.

### 5.8 Fase 5 — aquisição contínua (30 min)

```bash
sudo python3 scripts/afe-phase5-long.py --minutes 30 --csv ~/fase5-$(date +%F).csv
```

Deixe o host **ocioso** durante a sessão. A margem do link é nula (§6.1), e carga de CPU ou outro
dispositivo no mesmo barramento muda o resultado.

| Medida | Esperado, host ocioso (§6.3) |
|---|---|
| perda (`lost_samples`) | ~0,03% (125 em 449 998), em rajadas |
| entregues + perdidas contra devidas | fecha (diferença +0) |
| amostras corrompidas | **0** |
| intervalo entre timestamps | p99 ~4,1–4,2 ms |

**Neste link a perda só se lê no `lost_samples`**, que é a contagem do próprio driver dos ciclos
de timer em que a leitura anterior ainda estava ocupada. Ele não depende do GP6. Os buracos de
timestamp **não** contam a perda: uma leitura atrasada dispara a seguinte imediatamente, e a
amostra de recuperação chega num intervalo de aparência normal (§6.3). O que falta nesta placa é
uma referência **independente** do lado do conversor, que o contador do GP6 daria.

### 5.9 Encerrar

```bash
sudo modprobe -r ti_ads1299 hid_mcp2210
sudo python3 scripts/mcp2210-nvram-snapshot.py --compare ~/nvram-antes.txt
journalctl -k -b 0 | grep -iE 'BUG|Oops|WARNING' | grep -v 'Wi-Fi 7\|ACPI Warning\|Keylock'
```

- O `--compare` tem de dar PASS. Esse é o teste negativo "a aquisição nunca escreve a NVRAM". O
  veredito é só sobre os campos documentados; diferença em *don't care* aparece e não conta.
- A busca no journal tem de voltar vazia. Em 2026-10-03 um `kernel BUG` ficou gravado doze minutos
  antes de qualquer sintoma (§2.4).

---

## 6. Onde registrar

- Uma sessão nova vai para o `BRINGUP_AFE.md`, na seção da fase, **com data**, e com o que ela
  **não** mediu.
- Um número que muda o que a plataforma afirma vai também para o `RESULTS.md`.
- O `afe-phase1.sh` imprime no fim um bloco pronto para colar.

---

## 7. Problemas já vistos

| Sintoma | Causa | O que fazer |
|---|---|---|
| `lsusb` não mostra `04d8`; `dmesg` com `device descriptor read/64, error -71` ou `device not accepting address` | camada física: cabo, conector ou porta. O chip não responde nem ao primeiro descritor, então nenhum driver entra na história. Em 2026-10-05 foram 37 tentativas assim, e trocar o cabo resolveu | outro cabo de dados curto, outra porta; LED do GP6 apagado → medir 3,3 V em VDD/VUSB, RST e o cristal |
| `modprobe: … Key was rejected by service`; `dmesg`: `Loading of module with unavailable key is rejected` | o módulo foi assinado com uma chave que o kernel não conhece | `modinfo -F signer hid_mcp2210` contra o subject de `~/.mok/MOK.der`; repetir a §4 com a chave registrada (§3.2) |
| snapshot ou oráculo: `presa ao driver mcp2210, que nao expoe hidraw` | o nosso driver segura a ponte | `sudo modprobe -r hid_mcp2210` |
| `GP0 is a GPIO … not a chip select`, `cannot attach 'ads1299' … -16`, `nothing attached` | autoload com `spi_chip_select=0` padrão | recarregar com `spi_chip_select=4`, ou instalar o `modprobe.d` da §4 |
| `afe-phase4.py`: `nenhum iio:device com nome ads1299-*` | o conversor não está anexado (`nothing attached`), ou o autoteste ainda está rodando | conferir o `dmesg` da §5.4; esperar o `self test passed` |
| um módulo recarregado não volta ao padrão | um módulo já carregado guarda os parâmetros do último `modprobe`, e replugar não muda isso | `modprobe -r` e carregar de novo com os parâmetros explícitos |
| LED aceso fixo com o driver carregado e sem aquisição | carregado sem `led_gpio=6`, ou o driver recusou o pino (`dmesg \| grep 'status LED'`) | recarregar com `led_gpio=6`; a recusa nomeia o motivo (pino não é GPIO, ou é entrada na NVRAM) |
| `self test failed` com o sinal de teste "parado" em ~25–40 códigos | autoteste antigo, que media a oscilação em single-shot | conferir que o `linux-med` está em `e9f382da54ba` ou depois (§4.4) |
