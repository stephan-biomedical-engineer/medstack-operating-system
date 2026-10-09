#!/bin/sh
# SPDX-License-Identifier: MIT
#
# Teste do LED de estado da ponte (GP6 na placa de AFE).
#
# O que o driver promete, com led_gpio=6: pisca a 1 Hz enquanto o barramento
# SPI está ocioso e fica aceso fixo enquanto ele está em uso. A razão é o
# custo: cada troca de nível é uma troca HID na mesma USB das amostras, e o
# link não tem margem para isso durante uma aquisição (BRINGUP_AFE.md §6.1).
#
#   sudo sh scripts/afe-led-test.sh
#
# Roda no host e na placa: só sh, sysfs e debugfs. O nível do pino sai do
# debugfs de gpio, onde o driver registra a linha como "status-led". Cada
# leitura ali custa duas trocas HID de GPIO, que não contam como uso do SPI e
# portanto não mudam o que se está medindo.
#
# O que ele NÃO prova: que o LED acende. O nível lido é o latch de saída da
# ponte, não a luz; a placa pode ter o LED queimado ou invertido. Por isso o
# script termina pedindo a confirmação a olho, como a §2.3.2 fez.

set -u

PASS=0; FAIL=0; SKIP=0
MOD=hid_mcp2210
DBG=/sys/kernel/debug/gpio

say()  { printf '%s\n' "$*"; }
ok()   { PASS=$((PASS+1)); printf '  PASS  %s\n' "$*"; }
no()   { FAIL=$((FAIL+1)); printf '  FAIL  %s\n' "$*"; }
skip() { SKIP=$((SKIP+1)); printf '  SKIP  %s\n' "$*"; }
head_() { printf '\n=== %s\n' "$*"; }

# Centésimos de segundo desde o boot. /proc/uptime existe em todo kernel, e o
# date da busybox da placa não tem %N.
now_cs() { awk '{ printf "%d\n", $1 * 100 }' /proc/uptime; }

# "hi", "lo" ou vazio, do debugfs.
led_level() { grep 'status-led' "$DBG" 2>/dev/null | awk '{ print $NF == "LOW" ? $(NF-2) : $NF }' | head -1; }

# Amostra o LED a cada 100 ms por $1 segundos. Imprime "transicoes amostras
# altos duracao_cs".
sample_led() {
	n=$(( $1 * 10 ))
	prev=""; trans=0; highs=0; i=0
	t0=$(now_cs)
	while [ "$i" -lt "$n" ]; do
		v=$(led_level)
		[ "$v" = "hi" ] && highs=$((highs+1))
		[ -n "$prev" ] && [ "$v" != "$prev" ] && trans=$((trans+1))
		prev=$v
		i=$((i+1))
		sleep 0.1
	done
	t1=$(now_cs)
	echo "$trans $n $highs $((t1 - t0))"
}

# ---------------------------------------------------------------- 0. driver

head_ "0. O driver, e se ele tem LED"

if ! lsmod 2>/dev/null | grep -q "^$MOD"; then
	no "$MOD nao esta carregado"
	say "        sudo modprobe $MOD spi_chip_select=4 led_gpio=6"
	exit 1
fi

LED_GPIO=$(cat /sys/module/$MOD/parameters/led_gpio 2>/dev/null || echo '?')
say "  led_gpio = $LED_GPIO"
if [ "$LED_GPIO" = "?" ]; then
	no "o modulo nao tem o parametro led_gpio: e um driver anterior ao LED"
	say "        recompile e reinstale (ROTEIRO_BANCADA_AFE.md §4)"
	exit 1
fi
if [ "$LED_GPIO" -lt 0 ]; then
	no "carregado sem LED (led_gpio=$LED_GPIO)"
	say "        sudo modprobe -r $MOD && sudo modprobe $MOD spi_chip_select=4 led_gpio=6"
	exit 1
fi
ok "carregado com led_gpio=$LED_GPIO"

[ -r "$DBG" ] || mount -t debugfs none /sys/kernel/debug 2>/dev/null
if [ ! -r "$DBG" ]; then
	no "sem $DBG; debugfs montado? (mount -t debugfs none /sys/kernel/debug)"
	exit 1
fi

LINE=$(grep 'status-led' "$DBG" | head -1)
if [ -z "$LINE" ]; then
	no "nenhuma linha 'status-led' no debugfs: o driver recusou o LED"
	say "        o motivo esta no dmesg:"
	dmesg | grep -E 'status LED|cannot drive a LED' | tail -3 | sed 's/^/          /'
	exit 1
fi
say "  $LINE"
case "$LINE" in
*" out "*) ok "a linha do LED e uma saida reivindicada pelo driver" ;;
*)         no "a linha do LED nao e saida" ;;
esac

# O front-end, se houver, e o que gera trafego SPI na etapa 2.
IIO=""
for d in /sys/bus/iio/devices/iio:device*; do
	case "$(cat "$d/name" 2>/dev/null)" in
	ads1299*) IIO=$d ;;
	esac
done

BUSY_NOW=0
if [ -n "$IIO" ] && [ "$(cat "$IIO/buffer/enable" 2>/dev/null)" = "1" ]; then
	BUSY_NOW=1
fi

# ------------------------------------------------- 1. ocioso: pisca a 1 Hz

head_ "1. Barramento ocioso: o LED pisca a 1 Hz (10 s, amostrado a 10 Hz)"

if [ "$BUSY_NOW" = 1 ]; then
	skip "o buffer do front-end esta ligado: ha uma aquisicao em curso"
	say "        pare a aquisicao (o servico, ou o script de fase) e rode de novo"
else
	say "  esperando 1,5 s de silencio no SPI..."
	sleep 1.5
	set -- $(sample_led 10)
	TR=$1; N=$2; HI=$3; DUR=$4
	# 1 Hz = 2 trocas por segundo
	FREQ_X100=$(( TR * 100 * 100 / (2 * DUR) ))
	say "  $TR trocas em $N amostras, $((DUR / 100)).$((DUR % 100)) s; aceso em $HI"
	say "  frequencia: $((FREQ_X100 / 100)).$(printf '%02d' $((FREQ_X100 % 100))) Hz"
	# O work roda a cada 500 ms de relogio do kernel; a amostragem a 10 Hz
	# com sleep erra uma troca para mais ou para menos em 10 s.
	if [ "$FREQ_X100" -ge 85 ] && [ "$FREQ_X100" -le 115 ]; then
		ok "pisca a ~1 Hz (criterio 0,85 a 1,15 Hz)"
	else
		no "a frequencia nao e ~1 Hz"
	fi
	# Ciclo de trabalho: metade aceso, metade apagado.
	if [ "$HI" -ge $(( N * 35 / 100 )) ] && [ "$HI" -le $(( N * 65 / 100 )) ]; then
		ok "aceso em ~50% do tempo ($HI de $N)"
	else
		no "aceso em $HI de $N amostras, esperado ~50%"
	fi
fi

# ----------------------------------------- 2. em uso: aceso fixo, sem piscar

head_ "2. Barramento em uso: o LED fica aceso fixo"

if [ -z "$IIO" ]; then
	skip "nenhum front-end ads1299 registrado: nada para gerar trafego SPI"
	say "        carregue com o conversor anexado e rode de novo"
elif [ "$BUSY_NOW" = 1 ]; then
	say "  ja ha uma aquisicao em curso; medindo nela mesma"
	set -- $(sample_led 5)
	[ "$1" = 0 ] && [ "$3" = "$2" ] && ok "aceso em $3 de $2 amostras, 0 trocas" \
		|| no "durante a aquisicao: $1 trocas, aceso em $3 de $2"
else
	# Leituras avulsas pelo sysfs: cada uma e uma conversao pelo SPI, e
	# nenhuma fica mais de 1 s sem a seguinte, entao o barramento conta como
	# ocupado o tempo todo. Nao liga o buffer nem muda nenhum ajuste.
	say "  gerando trafego SPI com leituras de $IIO/in_voltage0_raw por ~6 s"
	(
		end=$(( $(now_cs) + 600 ))
		while [ "$(now_cs)" -lt "$end" ]; do
			cat "$IIO/in_voltage0_raw" >/dev/null 2>&1
		done
	) &
	BG=$!
	# A primeira meia volta do work pode cair antes do primeiro uso.
	sleep 1
	set -- $(sample_led 4)
	wait "$BG" 2>/dev/null
	say "  $1 trocas em $2 amostras; aceso em $3"
	if [ "$1" = 0 ] && [ "$3" = "$2" ]; then
		ok "aceso fixo enquanto o SPI esta em uso"
	else
		no "o LED mudou de nivel com o SPI em uso"
	fi

	# ---------------------------- 3. e volta a piscar quando o uso para

	head_ "3. Depois do uso: volta a piscar"
	sleep 1.5
	set -- $(sample_led 3)
	say "  $1 trocas em 3 s"
	if [ "$1" -ge 4 ]; then
		ok "voltou a piscar"
	else
		no "nao voltou a piscar 1,5 s depois do fim do trafego"
	fi
fi

# --------------------------------------------------------------- 4. a olho

head_ "4. A olho (MANUAL)"
say "  O nivel lido acima e o latch da ponte, nao a luz. Confira na placa:"
say "    - agora, com o SPI ocioso: o LED pisca, ~1 vez por segundo"
say "    - durante uma aquisicao: aceso fixo"
say "    - sudo modprobe -r $MOD: volta ao nivel de power-up (aceso nesta placa)"

# ------------------------------------------------------------------ resumo

head_ "Resumo"
say "  PASS: $PASS   FAIL: $FAIL   SKIP: $SKIP"
[ "$FAIL" -eq 0 ]
