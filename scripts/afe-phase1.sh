#!/bin/sh
# SPDX-License-Identifier: MIT
#
# Fase 1 do implementation_plan_afe_bench.md: a ponte sozinha, sem conversor.
#
# RODA NA PLACA, não no host. Não depende de libgpiod (que não está na imagem),
# não escreve nada no rootfs (que é read-only), e não precisa de rede.
#
#   scp scripts/afe-phase1.sh root@<placa>:/tmp/ && ssh root@<placa> /tmp/afe-phase1.sh
#
# O que ele NÃO faz: injetar pulsos no GP6. Isso precisa de gerador de sinais e
# de uma pessoa, e é o critério 6. O script prepara a medida e pede o número.
#
# Saída: um PASS/FAIL por critério, e no fim um bloco pronto para colar em
# BRINGUP_AFE.md §2. Um FAIL nunca é interpretado aqui - o plano diz o que cada
# um derruba, e adivinhar seria pior que reportar.

set -u

PASS=0; FAIL=0; SKIP=0
VID=04d8
PID=00de
MOD=hid_mcp2210
KO=hid-mcp2210

say()  { printf '%s\n' "$*"; }
ok()   { PASS=$((PASS+1)); printf '  PASS  %s\n' "$*"; }
no()   { FAIL=$((FAIL+1)); printf '  FAIL  %s\n' "$*"; }
skip() { SKIP=$((SKIP+1)); printf '  SKIP  %s\n' "$*"; }
head_() { printf '\n=== %s\n' "$*"; }

# ---------------------------------------------------------------- 0. contexto

head_ "0. Contexto, para o registro"
say "  data:     $(date -Is 2>/dev/null || date)"
say "  kernel:   $(uname -r)"
say "  maquina:  $(cat /sys/firmware/devicetree/base/model 2>/dev/null | tr -d '\0' || echo '?')"
say "  rtc:      $(cat /sys/class/rtc/rtc0/since_epoch 2>/dev/null || echo 'sem RTC')"
say ""
say "  ATENCAO: se o RTC nao esta inicializado, a data acima esta errada e todo"
say "  registro desta sessao herda isso. E o defeito aberto do BRINGUP_STM32MP2."

# ------------------------------------------------- 1. o dispositivo bruto

head_ "1. Critério 1 - o dispositivo USB, antes de qualquer driver nosso"

SYSUSB=""
for d in /sys/bus/usb/devices/*; do
	[ -r "$d/idVendor" ] || continue
	[ "$(cat "$d/idVendor")" = "$VID" ] || continue
	SYSUSB="$d"
	break
done

if [ -z "$SYSUSB" ]; then
	no "nenhum dispositivo USB com idVendor=$VID. A ponte esta plugada?"
	say ""
	say "  Sem isto nada abaixo tem significado. Pare aqui."
	exit 1
fi

GOT_PID=$(cat "$SYSUSB/idProduct")
GOT_SPEED=$(cat "$SYSUSB/speed" 2>/dev/null || echo '?')
say "  sysfs:    $SYSUSB"
say "  idVendor: $VID   idProduct: $GOT_PID   speed: ${GOT_SPEED} Mbit/s"
say "  produto:  $(cat "$SYSUSB/product" 2>/dev/null || echo '(sem string)')"
say "  fabricante: $(cat "$SYSUSB/manufacturer" 2>/dev/null || echo '(sem string)')"

if [ "$GOT_PID" = "$PID" ]; then
	ok "PID $PID - o valor que tres fontes terceiras davam, agora visto na peca"
else
	no "PID e $GOT_PID e o driver espera $PID. Isto DERRUBA a constante corroborada."
	say "        Corrija USB_DEVICE_ID_MCP2210 em hid-ids.h antes de seguir."
fi

# 12 Mbit/s = full speed, que e o que o datasheet diz (USB 2.0 full-speed)
if [ "$GOT_SPEED" = "12" ]; then
	ok "full-speed, como o datasheet declara"
else
	no "velocidade ${GOT_SPEED}, esperado 12 (full-speed)"
fi

# ---------------------------------------------- 2. carregar sem nada atras

head_ "2. Carregar o driver com NADA atras da ponte"
say "  E para isto que o parametro spi_device existe."

if lsmod 2>/dev/null | grep -q "^$MOD"; then
	say "  o modulo ja esta carregado (autoload); descarregando para controlar o parametro"
	rmmod "$MOD" 2>/dev/null || say "  rmmod falhou - pode haver um filho SPI preso"
fi

modprobe "$MOD" spi_device= 2>&1 | sed 's/^/  modprobe: /'
sleep 1

if lsmod 2>/dev/null | grep -q "^$MOD"; then
	ok "modulo carregado"
else
	no "o modulo nao carregou. Veja o dmesg abaixo."
fi

# ------------------------------------------------- 3. critério 2 - o probe

head_ "3. Critério 2 - a linha do probe"
PROBE=$(dmesg | grep -F 'USB-SPI bridge ready' | tail -1)
say "  $( [ -n "$PROBE" ] && echo "$PROBE" || echo '(nenhuma linha)')"

# A linha atual diz "N GPIOs, M chip selects, X attached". Nove e oito, e
# "nothing", porque nada foi pedido atras da ponte.
if printf '%s' "$PROBE" | grep -q '9 GPIOs, 8 chip selects, nothing attached'; then
	ok "9 GPIOs, 8 chip selects, nothing attached"
else
	no "a linha nao e a esperada (9 GPIOs, 8 chip selects, nothing attached)"
fi

say ""
say "  Outras linhas nossas no dmesg, que sao diagnostico e nao critério:"
dmesg | grep -iE 'mcp2210' | tail -12 | sed 's/^/    /'

# --------------------------------------------- 4. critério 3 - quem deu bind

head_ "4. Critério 3 - o bind, e a hipotese do hid-generic"
OURS=$(ls /sys/bus/hid/drivers/mcp2210/ 2>/dev/null | grep -i "$VID" || true)
GENERIC=$(ls /sys/bus/hid/drivers/hid-generic/ 2>/dev/null | grep -i "$VID" || true)

say "  em mcp2210:     ${OURS:-(nada)}"
say "  em hid-generic: ${GENERIC:-(nada)}"

if [ -n "$OURS" ] && [ -z "$GENERIC" ]; then
	ok "o nosso driver ganhou o bind e hid-generic nao tem o dispositivo"
	say "        Isto RESOLVE o item que o plano §13.3 marca como 'lido no fonte"
	say "        do kernel e nao observado': o id_table basta, sem HID_QUIRK."
else
	no "o bind nao esta como esperado"
fi

# ------------------------------------------ 5. critério 4 - o gpiochip

head_ "5. Critério 4 - o gpiochip, 9 linhas"
say "  (libgpiod nao esta na imagem; isto le o debugfs, que esta)"
GPIOBLOCK=$(grep -A1 -i 'mcp2210' /sys/kernel/debug/gpio 2>/dev/null | head -4)
if [ -n "$GPIOBLOCK" ]; then
	printf '%s\n' "$GPIOBLOCK" | sed 's/^/    /'
else
	say "    (nada com 'mcp2210' em /sys/kernel/debug/gpio)"
fi

NG=""
for g in /sys/bus/gpio/devices/gpiochip*; do
	[ -r "$g/label" ] || continue
	case "$(cat "$g/label")" in
	*mcp2210*) NG=$(cat "$g/ngpio" 2>/dev/null); say "    $g label=mcp2210 ngpio=$NG" ;;
	esac
done

if [ "$NG" = "9" ]; then
	ok "gpiochip de rotulo mcp2210 com 9 linhas"
elif [ -n "$NG" ]; then
	no "o gpiochip tem $NG linhas, esperado 9"
else
	skip "nao foi possivel ler ngpio pelo sysfs; confira o bloco do debugfs acima"
fi

# ------------------------------- 6. critério 5 - o contador, via subsistema

head_ "6. Critério 5 - o contador de bordas"
say "  O contador NAO e mais um atributo sysfs do dispositivo HID. Ele e um"
say "  counter_device, e o caminho mudou - o roteiro antigo do plano esta velho."

CNT=""
for c in /sys/bus/counter/devices/counter*; do
	[ -r "$c/name" ] || continue
	case "$(cat "$c/name")" in
	*"$VID"*|*mcp2210*) CNT="$c" ;;
	esac
done
[ -z "$CNT" ] && CNT=$(ls -d /sys/bus/counter/devices/counter* 2>/dev/null | head -1)

if [ -z "$CNT" ] || [ ! -r "$CNT/count0/count" ]; then
	no "nenhum counter com count0/count. CONFIG_COUNTER carregou? (modprobe counter)"
	say "        Sem isto a ligacao USB nao tem como saber que perdeu amostra."
else
	say "  counter:  $CNT  ($(cat "$CNT/name" 2>/dev/null))"
	say "  sinal:    $(cat "$CNT/signal0/name" 2>/dev/null || echo '?')"
	say "  funcao:   $(cat "$CNT/count0/function" 2>/dev/null || echo '?')"
	say "  acao:     $(cat "$CNT/count0/signal0_action" 2>/dev/null || echo '?')"

	A=$(cat "$CNT/count0/count"); B=$(cat "$CNT/count0/count")
	say "  duas leituras em repouso: $A e $B"
	if [ "$A" = "$B" ]; then
		ok "duas leituras consecutivas dao o mesmo valor (o acumulador funciona)"
	else
		no "as leituras diferem sem bordas: $A depois $B"
	fi

	if echo 0 > "$CNT/count0/count" 2>/dev/null; then
		Z=$(cat "$CNT/count0/count")
		[ "$Z" = "0" ] && ok "escrever 0 zera (le $Z)" || no "escrevi 0 e le $Z"
	else
		no "nao consegui escrever 0 em count0/count"
	fi

	if echo 1 > "$CNT/count0/count" 2>/dev/null; then
		no "escrever 1 foi ACEITO, e devia ser recusado (-EINVAL)"
	else
		ok "escrever um valor nao-zero e recusado, como deve"
	fi

	say ""
	say "  ISTO E A PRIMEIRA CONFIRMACAO EMPIRICA das constantes [DS20005176?]:"
	say "  ler o contador exige GET_CHIP_SETTINGS, SET_CHIP_SETTINGS e"
	say "  GET_INT_COUNT com os deslocamentos certos. Se chegou aqui, os tres"
	say "  opcodes e o byte 1 do 0x12 estao corretos no silicio."
fi

# ------------------------------------- 7. critério 6 - os 1000 pulsos

head_ "7. Critério 6 - 1000 pulsos no GP6 (MANUAL, precisa do gerador)"
if [ -n "${CNT:-}" ] && [ -r "$CNT/count0/count" ]; then
	echo 0 > "$CNT/count0/count" 2>/dev/null
	say "  contador zerado. Agora, na bancada:"
	say ""
	say "    1. gerador em 1 kHz, onda quadrada, nivel compativel com o VDD da ponte"
	say "    2. habilitar a saida por exatamente 1000 pulsos (burst, nao continuo)"
	say "    3. ler:  cat $CNT/count0/count"
	say ""
	say "  O critério e 1000 +- 0. NAO e 'avancou'. Um numero proximo mas"
	say "  diferente e um defeito, nao um arredondamento."
	say ""
	say "  Injecao de falha obrigatoria depois: reprogramar o GP6 como GPIO comum"
	say "  e repetir. O contador tem de ficar PARADO. Se avancar de qualquer"
	say "  jeito, o teste dos 1000 pulsos nao estava medindo o que parecia."
	skip "critério 6 - requer gerador de sinais e uma pessoa"
else
	skip "critério 6 - sem contador, nao ha o que medir"
fi

# ------------------------------------------ 8. injecoes de falha restantes

head_ "8. As outras duas injecoes de falha (MANUAIS)"
say "  A. hid-generic contra o nosso id_table"
say "     rmmod $MOD ; (replugar a ponte) ; ls /sys/bus/hid/drivers/hid-generic/"
say "     -> hid-generic DEVE reivindicar o dispositivo com o nosso modulo fora."
say "     Depois: modprobe $MOD spi_device= com o dispositivo ja plugado."
say "     -> ele consegue tomar o bind, ou precisa de unbind explicito?"
say "     A resposta diz se KERNEL_MODULE_AUTOLOAD basta ou se falta HID_QUIRK."
say ""
say "  B. Ordem de carga"
say "     plugar com o modulo carregado, e plugar com ele ausente carregando"
say "     depois. Os dois caminhos tem de terminar no MESMO estado."

# ------------------------------------------------------------- 9. resumo

head_ "9. Resumo"
say "  PASS: $PASS   FAIL: $FAIL   SKIP: $SKIP"
say ""
if [ "$FAIL" -gt 0 ]; then
	say "  HA FALHAS. O plano §5 diz o que cada critério derruba:"
	say "    (1)-(3) derrubam o VID/PID ou a hipotese do hid-generic"
	say "    (4)-(5) derrubam os deslocamentos dos relatorios -> volta para a Fase 0"
	say "    (6)     derruba o modo do pino de interrupcao"
	say "  Nao interprete aqui. Registre o observado e va ao plano."
fi

head_ "10. Bloco para colar em BRINGUP_AFE.md §2"
cat <<BLOCK
## 2. Fase 1 — a ponte sozinha, sem conversor · **$(date +%Y-%m-%d)**

| Critério | Observado | Veredito |
| :--- | :--- | :--- |
| 1. VID/PID, full-speed | \`$VID:$GOT_PID\`, ${GOT_SPEED} Mbit/s | |
| 2. linha do probe | \`$(printf '%s' "$PROBE" | sed 's/.*: //')\` | |
| 3. bind nosso, nao hid-generic | mcp2210=\`${OURS:-nada}\` hid-generic=\`${GENERIC:-nada}\` | |
| 4. gpiochip com 9 linhas | ngpio=\`${NG:-?}\` | |
| 5. contador le e zera | | |
| 6. 1000 pulsos -> 1000 ± 0 | | |

Kernel: \`$(uname -r)\`. Placa: \`$(cat /sys/firmware/devicetree/base/model 2>/dev/null | tr -d '\0')\`.

**O que isto NAO mediu**: nada analogico. Nenhum conversor estava ligado — e o
parametro \`spi_device=\` existe para garantir isso.
BLOCK
