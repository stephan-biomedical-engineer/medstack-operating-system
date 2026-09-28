#!/bin/sh
# SPDX-License-Identifier: MIT
#
# Fase 8-B do implementation_plan_afe_bench.md: a varredura do datasheet da
# ponte, exercitando o MCP2210 como componente por direito próprio.
#
# RODA NA PLACA.  scp scripts/afe-phase8b.sh root@<placa>:/tmp/
#
# NÃO DEPENDE DO ADS1299, e essa é a propriedade mais útil desta fase: ela roda
# com uma placa de avaliação do MCP2210 e um jumper, antes de a placa do
# front-end existir. Se os componentes chegarem em ordens diferentes, é por
# aqui que a bancada começa.
#
# Pré-requisito: Fases 1 e 2.
#
# A maior parte desta fase são TESTES NEGATIVOS, e o mais importante deles é
# "a NVRAM está byte a byte intocada depois de uma sessão inteira". Desde a
# inversão de escopo de 2026-09-27 o driver SABE escrever NVRAM - então o que
# se mede deixou de ser a ausência do código e passou a ser que o caminho de
# aquisição não a alcança, que é a afirmação mais forte das duas.
#
# Um driver que escrevesse NVRAM por engano mudaria o VID/PID de um aparelho em
# campo. Peça soldada, falha permanente, e o próprio driver dele deixaria de dar
# match.

set -u

PASS=0; FAIL=0; SKIP=0
MOD=hid_mcp2210
VID=04d8
SNAP=/tmp/8b-nvram-antes.txt

say()  { printf '%s\n' "$*"; }
ok()   { PASS=$((PASS+1)); printf '  PASS  %s\n' "$*"; }
no()   { FAIL=$((FAIL+1)); printf '  FAIL  %s\n' "$*"; }
skip() { SKIP=$((SKIP+1)); printf '  SKIP  %s\n' "$*"; }
head_() { printf '\n=== %s\n' "$*"; }

head_ "0. Contexto"
say "  data:    $(date -Is 2>/dev/null || date)"
say "  kernel:  $(uname -r)"

SYSUSB=""
for d in /sys/bus/usb/devices/*; do
	[ -r "$d/idVendor" ] || continue
	[ "$(cat "$d/idVendor")" = "$VID" ] || continue
	SYSUSB="$d"; break
done
[ -z "$SYSUSB" ] && { say "  ponte nao encontrada. Pare."; exit 1; }
say "  ponte:   $SYSUSB ($(cat "$SYSUSB/idVendor"):$(cat "$SYSUSB/idProduct"))"

# ================================================ 13.3 o controlador SPI

head_ "1. §13.3 - o controlador SPI, como o núcleo o vê"
CTLR=$(ls -d /sys/class/spi_master/spi* 2>/dev/null | head -1)
if [ -n "$CTLR" ]; then
	say "  $CTLR"
	for f in "$CTLR"/*; do
		case "$(basename "$f")" in
		of_node|subsystem|power|uevent|device|driver) ;;
		*) [ -r "$f" ] && printf '    %-24s %s\n' "$(basename "$f")" "$(cat "$f" 2>/dev/null)" ;;
		esac
	done
	ok "spi_master presente"
else
	no "nenhum spi_master. A Fase 1 passou?"
fi

# ============================================ 13.4 os nove GPIO

head_ "2. §13.4 - os nove GPIO e as funções dedicadas"
say "  Nove pinos existem, OITO podem ser chip select, e o GP8 e o que nao pode."
say "  A linha do probe diz os dois numeros de proposito:"
dmesg | grep -F 'USB-SPI bridge ready' | tail -1 | sed 's/^/    /'
say ""
GC=""
for g in /sys/bus/gpio/devices/gpiochip*; do
	[ -r "$g/label" ] || continue
	case "$(cat "$g/label")" in *mcp2210*) GC="$g" ;; esac
done
if [ -n "$GC" ]; then
	NG=$(cat "$GC/ngpio" 2>/dev/null)
	say "  gpiochip: $GC  ngpio=$NG"
	[ "$NG" = "9" ] && ok "nove linhas" || no "ngpio=$NG, esperado 9"
else
	no "gpiochip de rotulo mcp2210 nao encontrado"
fi
say ""
say "  O que resta aqui e MANUAL, com o multimetro ou o analisador:"
say "    - cada pino designado GPIO responde a direcao e ao valor"
say "    - um pino designado CHIP SELECT e recusado pelo gpiochip (-EBUSY)"
say "    - o GP8 recusa ser saida (-EIO) e le sempre como entrada"
skip "§13.4 - a verificacao eletrica de cada pino precisa de instrumento"

# ======================================== 13.5 o contador do GP6

head_ "3. §13.5 - o contador do GP6"
CNT=$(ls -d /sys/bus/counter/devices/counter* 2>/dev/null | head -1)
if [ -z "$CNT" ] || [ ! -r "$CNT/count0/count" ]; then
	no "nenhum counter. modprobe counter? MED_EEG_LINK=usb?"
else
	say "  counter:  $CNT"
	say "  sinal:    $(cat "$CNT/signal0/name" 2>/dev/null)"
	say "  funcao:   $(cat "$CNT/count0/function" 2>/dev/null)"
	say "  acao:     $(cat "$CNT/count0/signal0_action" 2>/dev/null)"
	say ""
	say "  O achado que muda a Fase 5, e ele continua valendo com o Counter:"
	say "  a ponte conta em 16 BITS e limpa na leitura. O valor exposto e um"
	say "  acumulador u64 do driver, entao o wrap deixou de ser problema de"
	say "  quem le - MAS bordas alem de 65536 ENTRE DUAS LEITURAS somem antes"
	say "  de o driver ve-las. 262 s a 250 SPS, 4,1 s a 16 kSPS."
	say ""
	say "  A medida desta fase: 65536+N pulsos numa unica janela sem leitura,"
	say "  e confirmar que o contador perde exatamente o que a aritmetica diz."
	say "  E um teste de que o LIMITE e o documentado, nao de que nao ha limite."
	skip "§13.5 - a medida do wrap precisa do gerador"
fi

# ==================== 13.6 NVRAM, EEPROM e senha - os testes NEGATIVOS

head_ "4. §13.6 - os testes negativos (o núcleo desta fase)"
say "  Snapshot ANTES. O criterio e byte a byte identico ao final da sessao."
say ""
{
	say "# snapshot da NVRAM/identidade, $(date -Is 2>/dev/null || date)"
	say "idVendor  $(cat "$SYSUSB/idVendor")"
	say "idProduct $(cat "$SYSUSB/idProduct")"
	say "manufacturer $(cat "$SYSUSB/manufacturer" 2>/dev/null)"
	say "product      $(cat "$SYSUSB/product" 2>/dev/null)"
	say "serial       $(cat "$SYSUSB/serial" 2>/dev/null)"
	say "bcdDevice    $(cat "$SYSUSB/bcdDevice" 2>/dev/null)"
} > "$SNAP"
cat "$SNAP" | sed 's/^/    /'
ok "snapshot gravado em $SNAP"
say ""
say "  ATENCAO sobre o que este snapshot alcanca. Ele le os descritores USB,"
say "  que sao o que a NVRAM produz na enumeracao - VID, PID e as strings."
say "  Ele NAO le os ajustes de power-up (0x61) nem a EEPROM (0x50), porque"
say "  o driver ainda nao expoe nenhum dos dois a userspace: 6A.1 leu a NVRAM"
say "  so para o log, e a EEPROM e a Fase 6B/6C do plano do MCP2210."
say ""
say "  Entao o teste negativo COMPLETO desta fase esta incompleto hoje, e"
say "  dizer isso e melhor que um snapshot que parece cobrir tudo."
say ""
say "  O que o log ja diz sobre os ajustes de arranque, lido no probe:"
dmesg | grep -iE 'power-up settings|only in RAM' | tail -3 | sed 's/^/    /'
say ""
say "  Ao FIM da sessao, repita e compare:"
say "    diff $SNAP <(este script de novo) "
say "  e confirme com um CICLO DE ENERGIA que o aparelho reenumera igual."

head_ "5. §13.6 continuação - nenhum comando de escrita foi emitido"
say "  A suite de host ja verifica isto a cada grupo (check_nvram_not_written),"
say "  mas ali contra um dispositivo falso. Aqui a unica evidencia disponivel"
say "  de dentro da placa e negativa por construcao: se o driver tivesse"
say "  escrito, o snapshot mudaria."
say ""
say "  A evidencia FORTE e um analisador USB, e ela e a unica que fecha o"
say "  item (c) da §13.1: uma captura do trafego real e a fonte independente"
say "  que transforma os deslocamentos [DS20005176?] de consistencia em"
say "  correcao. Se houver analisador na bancada, capture a sessao inteira e"
say "  procure por 0x60, 0x70, 0x50 e 0x51 - nenhum deve aparecer."
skip "§13.6 - a prova positiva precisa de analisador USB"

# ======================== 13.7 recuperação de erro, e o cabo saindo

head_ "6. §13.7 - o que acontece quando o cabo sai"
say "  Esta e a que da para fazer AGORA, sem instrumento, e e a mais util das"
say "  manuais. Sequencia:"
say ""
say "    1. com transferencias em curso, desconecte o cabo USB"
say "    2. dmesg deve mostrar o desligamento, e NAO um oops"
say "    3. reconecte"
say "    4. o modulo reassume? aparece de novo o spi_master e o counter?"
say "    5. as transferencias voltam a funcionar sem recarregar nada?"
say ""
say "  E a variante que interessa mais: desconectar NO MEIO de uma transacao."
say "  O caminho de recuperacao da Fase 4 do plano do MCP2210 (o 0x11 em todo"
say "  caminho de erro, e a recuperacao de uma ponte ja ocupada) foi verificado"
say "  contra um dispositivo FALSO. Um cabo saindo e a primeira vez que ele"
say "  encontra um dispositivo que sumiu de verdade."
say ""
say "  Registre: quantas linhas de erro, se houve oops, e se a recuperacao"
say "  precisou de intervencao."
skip "§13.7 - precisa de uma pessoa puxando o cabo"

# ================================================== os achados de leitura

head_ "7. §13.8 - os dois achados de leitura de código, e se ainda valem"
say "  O plano registrou dois defeitos lidos no fonte e nao observados. Os"
say "  dois ja foram CORRIGIDOS desde entao, e esta fase e onde se confirma"
say "  que a correcao vale no silicio e nao so no dispositivo falso:"
say ""
say "  A. o eco do comando nao era conferido -> corrigido na Fase 3 do plano"
say "     do MCP2210. Evidencia aqui: stray_replies deveria ficar em zero"
say "     numa sessao limpa. Nao ha atributo para ele, entao a evidencia e"
say "     a ausencia da linha de aviso:"
dmesg | grep -ci 'discarded; further ones are counted silently' | sed 's/^/       linhas de descarte: /'
say ""
say "  B. o contador de 16 bits -> continua sendo 16 bits no hardware; o que"
say "     mudou e a representacao. Ver §13.5 acima."

head_ "8. Resumo"
say "  PASS: $PASS   FAIL: $FAIL   SKIP: $SKIP"
say ""
say "  Registro: BRINGUP_AFE.md §10."
say ""
say "  O que esta fase NAO fecha hoje, e vale dizer no registro: os ajustes de"
say "  power-up e a EEPROM nao sao legiveis de userspace, entao o teste"
say "  negativo cobre a identidade USB e nao o conteudo inteiro da NVRAM."
say "  Isso fecha quando a Fase 6A.2/6C do plano do MCP2210 decidir a interface."
