#!/bin/sh
# SPDX-License-Identifier: MIT
#
# Fase 2 do implementation_plan_afe_bench.md: o barramento SPI transfere bytes,
# medido SEM o conversor.
#
# RODA NA PLACA.  scp scripts/afe-phase2.sh root@<placa>:/tmp/
#
# PRÉ-REQUISITOS, e os dois são de build, não de bancada:
#
#   1. kernel construído com MED_BENCH=1, que acrescenta CONFIG_SPI_SPIDEV=m.
#      Nenhum perfil o liga por padrão, e a imagem que o tem NAO PODE ser
#      entregue: /dev/spidevX.Y é um caminho de userspace para o barramento do
#      conversor, exatamente o que a arquitetura fecha.
#
#         MED_KERNEL_GIT=... MED_KERNEL_SRCREV=... MED_BENCH=1 make stm32
#
#   2. o binário spidev_test em /tmp da placa. Existe receita no meta-oe:
#
#         make shell   # e dentro: bitbake spidev-test
#         scp .../spidev_test root@<placa>:/tmp/
#
#   3. HARDWARE: um jumper ligando MOSI a MISO na saída da ponte. Sem ele o
#      critério 1 falha por definição - e a injeção de falha desta fase é
#      exatamente tirá-lo.
#
# O que este script NAO faz: os critérios 3, 4 e 5 (clock, tempos de CS, modo
# SPI). Os três precisam de osciloscópio ou analisador lógico, e nenhum deles é
# observável de dentro da placa. O script prepara cada um e diz o que medir.

set -u

PASS=0; FAIL=0; SKIP=0
MOD=hid_mcp2210
SPIDEV_TEST=${SPIDEV_TEST:-/tmp/spidev_test}
TABLE=/tmp/fase2-tempos.txt

say()  { printf '%s\n' "$*"; }
ok()   { PASS=$((PASS+1)); printf '  PASS  %s\n' "$*"; }
no()   { FAIL=$((FAIL+1)); printf '  FAIL  %s\n' "$*"; }
skip() { SKIP=$((SKIP+1)); printf '  SKIP  %s\n' "$*"; }
head_() { printf '\n=== %s\n' "$*"; }

head_ "0. Contexto"
say "  data:    $(date -Is 2>/dev/null || date)"
say "  kernel:  $(uname -r)"
say "  RTC:     $(cat /sys/class/rtc/rtc0/since_epoch 2>/dev/null || echo 'sem RTC - a data acima esta errada')"

# --------------------------------------------------------- pré-requisitos

head_ "1. Pré-requisitos"

if [ ! -x "$SPIDEV_TEST" ]; then
	no "spidev_test nao esta em $SPIDEV_TEST"
	say "        bitbake spidev-test, e copie o binario para a placa."
	exit 1
fi
ok "spidev_test presente"

if ! modprobe spidev 2>/dev/null && ! lsmod | grep -q '^spidev'; then
	no "spidev.ko nao carrega. O kernel foi construido com MED_BENCH=1?"
	exit 1
fi
ok "spidev.ko carregado"

# ------------------------------------------- a ponte, agora com um spidev

head_ "2. Recarregar a ponte com um spidev atras dela"
say "  O parametro spi_device decide o modalias do filho. Na Fase 1 ele foi"
say "  vazio; aqui ele e 'spidev', e nenhum conversor esta no circuito."

rmmod "$MOD" 2>/dev/null
modprobe "$MOD" spi_device=spidev spi_max_speed_hz=1000000 2>&1 | sed 's/^/  modprobe: /'
sleep 1

DEV=$(ls /dev/spidev* 2>/dev/null | head -1)
if [ -n "$DEV" ]; then
	ok "nó de dispositivo: $DEV"
else
	no "nenhum /dev/spidev*. Veja: dmesg | tail -20"
	dmesg | tail -10 | sed 's/^/    /'
	exit 1
fi

say ""
dmesg | grep -iE 'mcp2210|spidev' | tail -6 | sed 's/^/    /'

# ------------------------------------------------- critério 1: o eco

head_ "3. Critério 1 - eco byte a byte, nos tamanhos que importam"
say "  Os tamanhos NAO sao arbitrarios: 60 e MCP2210_MAX_XFER_CHUNK como o"
say "  driver acredita, e 61 e o primeiro que obriga a fragmentar. Se a"
say "  fronteira real for outra, e aqui que aparece."
say ""

: > "$TABLE"
ECHO_OK=1
for n in 1 2 59 60 61 120 512; do
	head -c "$n" /dev/urandom > /tmp/tx.$n 2>/dev/null

	T0=$(cut -d' ' -f1 /proc/uptime)
	OUT=$("$SPIDEV_TEST" -D "$DEV" -s 1000000 -I /tmp/tx.$n -v 2>&1)
	T1=$(cut -d' ' -f1 /proc/uptime)

	# spidev_test -v imprime TX e RX em hex; iguais = eco correto
	TX=$(printf '%s' "$OUT" | sed -n 's/^TX | //p' | tr -d ' .|')
	RX=$(printf '%s' "$OUT" | sed -n 's/^RX | //p' | tr -d ' .|')
	DT=$(awk -v a="$T0" -v b="$T1" 'BEGIN{printf "%.4f", b-a}')

	if [ -n "$TX" ] && [ "$TX" = "$RX" ]; then
		printf '  %4d bytes  eco OK    %s s\n' "$n" "$DT"
		printf '%s\t%s\n' "$n" "$DT" >> "$TABLE"
	else
		printf '  %4d bytes  ECO DIVERGE  %s s\n' "$n" "$DT"
		printf '%s\tDIVERGE\n' "$n" >> "$TABLE"
		ECHO_OK=0
	fi
done

if [ "$ECHO_OK" = "1" ]; then
	ok "eco identico em todos os sete tamanhos"
else
	no "houve divergencia - veja acima qual tamanho"
	say "        61 falhando e 60 passando = fragmentacao errada"
	say "        todos falhando = opcode ou deslocamento errado -> volta a Fase 0"
fi

# --------------------------------------- critério 2: tempo por transação

head_ "4. Critério 2 - tempo por transação (a previsão a falsificar)"
say "  A previsao do plano: 2 a 4 relatorios USB por transacao, o que poe o"
say "  teto do caminho entre 250 e 500 transacoes por segundo."
say ""
say "  tamanho    tempo(s)   transacoes/s estimadas"
while IFS=$(printf '\t') read -r n dt; do
	case "$dt" in
	DIVERGE) printf '  %6s     -          -\n' "$n" ;;
	*) R=$(awk -v d="$dt" 'BEGIN{ if (d>0) printf "%.0f", 1/d; else print "inf" }')
	   printf '  %6s     %-10s %s\n' "$n" "$dt" "$R" ;;
	esac
done < "$TABLE"
say ""
say "  ATENCAO: uma unica transacao medida por /proc/uptime tem resolucao de"
say "  10 ms. Para o numero que vai ao RESULTS.md, rode em laco:"
say ""
say "    time (for i in \$(seq 200); do $SPIDEV_TEST -D $DEV -s 1000000 -I /tmp/tx.60 >/dev/null; done)"
say ""
say "  e divida por 200. O valor acima serve para ver ordem de grandeza e"
say "  descobrir se algum tamanho destoa - nao para publicar."
skip "critério 2 - o número publicável precisa do laço acima"

# ----------------------------------- critérios 3-5: precisam de instrumento

head_ "5. Critérios 3, 4 e 5 - o que so o osciloscopio ve"
say "  3. CLOCK: SCK medido contra spi_max_speed_hz, em 1 MHz e em 4 MHz,"
say "     com o erro relativo declarado. A ponte divide um relogio fixo, entao"
say "     a taxa pedida nao e necessariamente realizavel - e o driver ja le a"
say "     taxa de volta uma vez e poe no log. Confira as duas contra o osciloscopio:"
dmesg | grep -i 'bridge programmed' | tail -2 | sed 's/^/       /'
say ""
say "     Para repetir em 4 MHz:"
say "       rmmod $MOD; modprobe $MOD spi_device=spidev spi_max_speed_hz=4000000"
say ""
say "  4. TEMPOS DE CS: atraso de CS ativo ate o primeiro SCK, e do ultimo SCK"
say "     ate CS inativo. Compare com o que o driver programa em CS_TO_DATA e"
say "     DATA_TO_CS - hoje zero e zero, salvo se a mensagem pedir delay."
say "     ESTA e a confirmacao de que aqueles deslocamentos [DS20005176?]"
say "     chegam ao silicio."
say ""
say "  5. MODO SPI: com spi_mode_param nos quatro valores, a polaridade e a"
say "     fase observadas no analisador tem de bater com o pedido:"
say "       for m in 0 1 2 3; do"
say "         rmmod $MOD; modprobe $MOD spi_device=spidev spi_mode_param=\$m"
say "       done"
skip "critérios 3, 4 e 5 - precisam de osciloscopio ou analisador logico"

# -------------------------------------------------- a injeção de falha

head_ "6. Injeção de falha OBRIGATÓRIA - tirar o jumper"
say "  Repita o critério 1 com o jumper REMOVIDO. O eco tem de virar constante"
say "  (0x00 ou 0xFF) e NAO o padrao enviado."
say ""
say "  Sem isso, 'o eco bateu' poderia ser o driver devolvendo o buffer de"
say "  transmissao sem nunca falar com o barramento - que e a forma classica"
say "  de um teste de loopback passar sem testar nada."
say ""
say "    (remova o jumper)"
say "    $SPIDEV_TEST -D $DEV -s 1000000 -I /tmp/tx.60 -v"
skip "injeção de falha - precisa de uma pessoa mexendo no jumper"

head_ "7. Resumo"
say "  PASS: $PASS   FAIL: $FAIL   SKIP: $SKIP"
say ""
say "  Registro: RESULTS.md (tabela de tempo por transacao) e BRINGUP_AFE.md §3."
say ""
say "  E LEMBRE de reconstruir sem MED_BENCH=1 antes de qualquer coisa que va"
say "  para uma placa entregue. A imagem com spidev nao e entregavel."
