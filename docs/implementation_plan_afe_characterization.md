# Plano de Implementação — Caracterização analógica do front-end, pelos links `usb` e `amp`, com a tela ao vivo

> **Status**: **nada aqui foi executado.** O plano foi escrito em 2026-10-08, no fim do dia em que o
> link `amp` passou a adquirir do conversor num cartão gravado do zero (`BRINGUP_STM32MP2.md` §9.16).
> O que já existe e este plano usa: os dois links adquirindo do mesmo ADS1299 na placa, o
> `amp-bench` e as ferramentas de host das Fases 1–5, e as medidas estáticas de 2026-10-04 com as
> referências internas (`BRINGUP_AFE.md` §5). O que **não** existe: a HMI na tela da placa depois de
> um reboot (as três correções de `BRINGUP_HMI_STM32MP2.md` §9 continuam voláteis, e o
> `weston.service` falha em todo boot), um caminho para mudar a condição de medida sem regravar o
> cartão, e qualquer medida com fonte externa ou eletrodo.
>
> **Escopo**: extrair do canal analógico tudo o que ele permite medir (nível DC, ruído, ganho,
> resposta em frequência, características temporais, rejeição, perdas e os recursos de eletrodo),
> **pelos dois links, com a mesma prescrição**, em três condições: sem eletrodos, com simulador de
> eletrodo e com eletrodos reais num fantoma. E, junto, a tela HDMI da placa mostrando os 8 canais
> ao vivo durante tudo isso.
>
> **Relação com os outros planos.** Este plano é **dono das métricas de sinal**. Onde ele e o
> `implementation_plan_board_stats.md` se encontram (§1.4 e E4/E6 de lá), a definição de cada
> medida de sinal é a daqui, e a de integridade, tempo de carimbo e custo continua sendo a de lá.
> Do `implementation_plan_afe_bench.md` ele herda a Fase 0 (datasheet), a §3 (segurança) e as
> §8.1–8.5 já executadas no host, que aqui se repetem **na placa e pelos dois links**. As §12.3–12.6
> (canal, lead-off, bias, matriz de ruído) passam a ser executadas por este plano.

---

## 0. A linha, dita primeiro

| | Hoje |
|---|---|
| ganho, escala, MUX, gerador interno | **medidos no host** pelo `usb` (`BRINGUP_AFE.md` §5): PASS, só com as referências internas |
| piso de ruído | 40 conversões avulsas por ganho, no host, entradas em curto. **Sem veredito**, porque a tabela de ruído do SBAS499 não está transcrita |
| gerador interno pelo `amp` | 120 s, 8 canais dentro da especificação, ocioso e com o A35 carregado (`BRINGUP_STM32MP2.md` §9.16) |
| fonte externa (DC, seno, modo comum) | **nada medido** |
| eletrodo ou simulador de eletrodo | **nada medido**. O lead-off "tudo solto" sem eletrodos é o resultado trivial |
| lead-off no `usb` | **aceito e não cumprido**: o driver de kernel não liga `LOFF_SENSP/N` (§9.16) |
| montagem referencial (SRB1) | **nenhum dos dois links** a configura |
| perda no `usb`, nesta placa | **indetectável pelo sistema**: o DRDY está no GP5, e a ponte só conta bordas no GP6 (`BRINGUP_AFE.md` §2.3) |
| HMI na tela HDMI da placa | vista uma vez, em 2026-08-27, com três correções feitas à mão; **não sobrevive a um reboot** |

Dois fatos desta tabela mudam o desenho do plano inteiro:

1. **No `usb` desta placa, a perda só aparece no sinal.** Nenhum contador a vê. Por isso a §3.7 usa
   um seno de frequência conhecida como **detector de perda independente**: uma amostra que falta é
   um salto de fase de 360°·f/f<sub>s</sub>, ou seja 14,4° para 10 Hz a 250 SPS. Isso vale nos dois
   links e não exige analisador lógico.
2. **A tela e o paciente puxam em sentidos opostos.** Um monitor HDMI ligado à tomada liga o terra
   da placa ao da rede pela malha do cabo. Com eletrodos num fantoma, isso é ruído de 60 Hz, e o
   ruído é um resultado. Com eletrodos numa pessoa, é um caminho de corrente de fuga. A §5.4 trata
   disso.

---

## 1. Decisões que este plano toma

**D1 — Toda condição de medida é uma prescrição selada, e passa pelo serviço.** O serviço aceita
`--config` (`main.cpp:188`). Uma condição de bancada é um `eeg.conf` completo em
`/run/medplatform/bench/`, com o `.sha256` gerado na hora, e um drop-in em
`/run/systemd/system/eeg-acquisition.service.d/` que aponta o `ExecStart` para ele. O que se ganha:
a sessão vai para o `/data` com a prescrição e o `config_digest` no metadado, e a HMI mostra
exatamente o que está sendo gravado. O `/run` some no reboot, então a imagem continua a de produto.
Isso já foi feito à mão em 2026-10-08 (§9.16, "um `eeg.conf` `amp` em `/run`"), e o plano o torna
procedimento.

O `amp-bench` e o sysfs direto ficam **só** para o que a prescrição não sabe dizer (por exemplo bias
`reference`, que é o controle experimental do `derived`), e cada uso é marcado "fora do serviço" na
tabela.

**D2 — A análise sai do registro, nunca da tela.** A HMI serve para duas coisas aqui: o operador
confirma que a condição é a que ele acha (o gerador aparece, o curto é plano, o eletrodo solto fica
vermelho), e a HMI ligada é o nível de carga que o produto realmente tem. Nenhum número deste plano
vem dela.

**D3 — Processamento de tela é só de tela, e aparece escrito na tela.** Com eletrodos, o offset DC é
de dezenas de mV num conversor acoplado em DC, e com escala fixa de ±100 µV o traço sai da faixa. A
HMI ganha remoção de DC e notch de 60 Hz **de exibição**, desligados por padrão. Quando um deles
está ligado, isso aparece no cabeçalho. O registro continua cru. A regra do `Main.qml` ("fixa, não
autoescalada") continua valendo: a escala é escolhida pelo operador entre valores fixos e
aparece na tela, e nunca muda sozinha.

**D4 — Os dois links medem o mesmo conversor, nunca ao mesmo tempo.** Dois mestres no mesmo SPI são
o risco do `implementation_plan_ads1299.md` §4.4. Para trocar de link, o mestre inativo é
desconectado fisicamente, a troca vai para o manifesto, e a ordem entre os links é intercalada
por dia (`board_stats` §4).

**D5 — O vocabulário da prescrição cresce só pelo que o produto também usaria.** Duas entradas
novas, e as duas passam pelo `grep` da regra 9 (nenhum nome de peça):

| Chave | Valores | Por que o produto também a quer |
|---|---|---|
| `afe.input` | `electrodes` (padrão), `shorted`, `supply`, `temperature` | um autoteste de ruído com entrada em curto é verificação de desempenho essencial (IEC 60601-1 §14), não ferramenta de bancada |
| `afe.montage` | `bipolar` (padrão), `referential` | EEG clínico usa referência comum. Sem ela, cada canal pede dois eletrodos |

Cada link traduz no `do_derive_device_options`, e **um link que não sabe cumprir recusa no build**,
como o firmware já recusa (`afe.bias_drive: not implemented by this firmware yet`). O caso do
lead-off no `usb` hoje, aceito e não cumprido, é exatamente o que essa regra proíbe.

---

## 2. Instrumentos e insumos

| Item | Para quê | Observação |
|---|---|---|
| gerador de função | seno, quadrada e varredura | com saída flutuante, se possível |
| atenuador resistivo 1000:1 e 10 000:1 | sinais de µV a partir de mV | resistores de 0,1%; a razão é **medida** com o multímetro, não assumida |
| fonte DC de precisão, ou divisor a partir de uma referência | ganho, linearidade, fundo de escala | |
| multímetro de 6½ dígitos | referência de amplitude | é a fonte independente do ganho |
| osciloscópio | ripple de AVDD, forma do DRDY, BIAS_OUT | |
| analisador lógico | DRDY, CS, SCLK | o canal 0 é o DRDY, como no `board_stats` §3.3 |
| fonte de bancada | AVDD fora da USB | limite de corrente ajustado **antes** de energizar |
| caixa metálica aterrada | ruído sem o ambiente | e fora dela, porque o ambiente também é resultado |
| simulador de eletrodo | R∥C por entrada, BIAS e SRB1 | a rede 51 kΩ ∥ 47 nF das normas particulares `[60601-2-26?]`, mais resistores de 10 kΩ a 10 MΩ em série |
| eletrodos Ag/AgCl descartáveis com gel | fantoma | do mesmo lote, anotado no manifesto |
| fantoma salino | eletrodos reais sem pessoa | NaCl 0,9% num recipiente não condutor, com o gerador injetado por dois eletrodos de excitação |
| termômetro de contato | sensor interno | |

**Rede elétrica de 60 Hz.** A 250 SPS, a frequência de Nyquist é 125 Hz, e por isso 60 Hz e 120 Hz
caem dentro da banda. Todo espectro deste plano marca as duas.

---

## 3. O catálogo de medidas

Cada medida tem uma **definição**, um **esperado** com a fonte, um **critério** e uma **injeção de
falha**: a forma de ver o instrumento falhar antes de acreditar nele. Constantes do datasheet levam
`[SBAS499?]` até a Fase 0 do `afe_bench` conferi-las. Os números entre colchetes abaixo vêm de
leitura, não de transcrição conferida.

Salvo indicação, cada medida é feita **por canal, nos 8**, com 3 repetições intercaladas e ganho 24
a 250 SPS. No `amp`, as que dependem de ODR varrem de 250 a 16 000 SPS. No `usb`, só 250 SPS
(`BRINGUP_AFE.md` §6.7).

### 3.1 Nível DC

| Medida | Definição | Condição | Critério |
|---|---|---|---|
| offset | média de 10 s com `afe.input = shorted` | ganhos 1…24 | registrar. O host deu −1002 a −780 códigos, quase constantes em código e não em µV (§5.1). A placa tem de reproduzir o padrão |
| deriva | offset em janelas de 10 s ao longo de 30 min a partir do power-up | ganho 24 | registrar a curva. O aquecimento é o efeito esperado |
| coeficiente térmico | offset contra o sensor interno (`afe.input = temperature`, intercalado) e contra as zonas do SoC | 30 min | registrar µV/°C. Sem fator controlado, isso é correlação |
| casamento entre canais | dispersão do offset entre os 8 | ganho 24 | registrar |

**Injeção**: aplicar uma DC de 1 mV na entrada com o MUX em `shorted`. A média não pode se mover
(a injeção de `BRINGUP_AFE.md` §5.4, repetida pelo serviço e pelos dois links).

### 3.2 Ruído

| Medida | Definição | Critério |
|---|---|---|
| RMS e pico a pico | sobre 10 s, `afe.input = shorted`, depois de remover a média | contra a tabela de ruído `[SBAS499?]` (~1 µVpp em ganho 24, 0,01–70 Hz) |
| densidade espectral | Welch, segmentos de 10 s, 30 min | o patamar branco, mais 60/120 Hz marcados |
| canto 1/f | onde a densidade cruza o patamar branco +3 dB | registrar |
| ruído em banda | RMS integrado em 0,5–40 Hz (banda EEG) e em 0,01–70 Hz (banda do datasheet) | a banda do datasheet é a comparável |
| matriz | ganho × ODR (só `amp` acima de 250) × alimentação (USB × bancada) × link × caixa (dentro × fora) | é a matriz das §8.4 e §12.6 do `afe_bench` |
| entre power-ups | o mesmo RMS em N = 20 inicializações | explica, ou não, a variação de 49–205 códigos do autoteste na placa contra 23 no PC (`board_stats` §1.4) |
| ruído com impedância de fonte | entrada pelo simulador de eletrodo, 10 kΩ a 10 MΩ | o ruído térmico de R mais a corrente de ruído × R. A curva mostra onde a corrente de entrada passa a dominar |

**Injeção**: ligar o gerador de seno com o MUX em `shorted`. O espectro não pode ganhar a linha do
gerador. E, no sentido oposto, um seno de 1 µVpp pelo atenuador 10 000:1 tem de aparecer na
densidade espectral, o que prova que o piso medido não é um canal morto.

### 3.3 Ganho e faixa

| Medida | Definição | Critério |
|---|---|---|
| exatidão de ganho | DC externa medida no multímetro contra a leitura, nos 7 ganhos | `raw × scale` constante dentro de 1% e igual à DC aplicada dentro do erro do multímetro (o critério de `afe_bench` §8.1, agora com fonte externa e pela entrada do eletrodo, que o host não validou) |
| linearidade (INL) | 11 níveis de −90% a +90% do fundo de escala; resíduo de um ajuste linear | registrar em % do FS |
| casamento de ganho | a mesma DC nos 8 canais | dispersão em % |
| fundo de escala | DC subindo até saturar, ganhos 1 e 24 | satura em ±VREF/ganho `[SBAS499?]`, **sem troca de sinal nem wrap** |
| recuperação de sobrecarga | quadrada de 2× o FS por 1 s, depois 0; tempo até a saída voltar a ±1% | registrar em amostras. É o que um artefato de movimento clínico faz |

**Injeção**: trocar o ganho na prescrição **sem** trocar a DC. A leitura em µV não muda, e a leitura
em código muda na razão dos ganhos. Um erro de 4× na tabela, ou de 12,5% na VREF, aparece aqui como
dois padrões diferentes.

### 3.4 Resposta em frequência e características temporais

| Medida | Definição | Esperado | Critério |
|---|---|---|---|
| resposta em amplitude | varredura de seno de 0,1 Hz a 0,45·f<sub>s</sub>, amplitude fixa, ganho por FFT em cada ponto | filtro sinc³, −3 dB em 0,262·f<sub>DR</sub> `[SBAS499?]` (65,5 Hz a 250 SPS) | a curva medida sobre a teórica, ±0,5 dB até 0,2·f<sub>s</sub> |
| planura na banda EEG | ondulação de 0,5 a 40 Hz | | registrar em dB |
| atraso de grupo | quadrada sincronizada ao analisador lógico: borda do gerador → amostra que cruza 50% | latência do sinc³ `[SBAS499?]` | registrar em µs e em amostras |
| simultaneidade entre canais | o mesmo seno nos 8; diferença de fase entre pares | 0 (amostragem simultânea) | \|Δφ\| < 0,1° a 10 Hz. Um valor maior mostra um defeito na ordem do quadro ou no desentrelaçamento |
| frequência real de amostragem | um seno de frequência conferida no osciloscópio, medido pela FFT | f<sub>CLK</sub>/… `[SBAS499?]` | ppm do oscilador. O M33 contou 250,031 e 249,950 conversões/s em dois momentos de 08/10 (§9.16), sem explicação; este é o número do sinal, não do relógio |
| jitter entre amostras e erro de fase do carimbo | **do `board_stats`** §1.2 (E1, E5) | | não se repete aqui; é citado na tabela final |

**Injeção**: apagar uma amostra de um arquivo de captura e reanalisar. A simultaneidade entre canais
não pode mudar (a amostra some dos 8 canais juntos), e o detector da §3.7 tem de encontrá-la.

### 3.5 Rejeição

| Medida | Definição | Esperado | Critério |
|---|---|---|---|
| CMRR | as duas entradas de um canal ligadas juntas e movidas por um seno contra o terra analógico, a 10 Hz e a 60 Hz; 20·log(saída/entrada) | −110 dB `[SBAS499?]` | registrar **com bias `off`, `reference` e `derived`**. A diferença entre os três é o que o BIAS faz |
| diafonia | seno de 80% do FS num canal, os outros em `shorted` **pelo pino** (não pelo MUX) | −110 dB `[SBAS499?]` | registrar a pior combinação |
| sensibilidade à alimentação | o mesmo piso com AVDD da USB e da bancada; ripple de AVDD no osciloscópio | | as linhas do ripple no espectro, se aparecerem |

**Injeção do CMRR**: desfazer o curto entre as entradas, com o mesmo seno numa delas só. A saída tem
de saltar umas 100 dB. Se não saltar, o "modo comum" estava aplicado errado.

### 3.6 Canais internos

Repetidas na placa e pelos dois links as `afe_bench` §8.3 e §8.5: gerador de teste (amplitude e
frequência, pelo serviço, em modo contínuo), MVDD contra o multímetro (±2%) e temperatura contra o
termômetro de contato (±5 °C). Isso pede a fórmula transcrita, que hoje é "lida de memória".

### 3.7 Perdas, vistas no sinal

| Medida | Definição | Fonte independente de |
|---|---|---|
| perda pelo seno | seno de 10 Hz a ~50% do FS, 10 min; ajuste de fase por janela de 1 s; uma amostra a menos é um salto de 360°·f/f<sub>s</sub> | `lost_samples` no `usb` (que esta placa não tem), contador do firmware no `amp`, `sequence` do quadro |
| descontinuidade | resíduo do ajuste de seno amostra a amostra; um pico isolado é uma amostra corrompida que passou no CRC | CRC do quadro |
| primeira conversão | as N primeiras amostras de cada sessão, contra as seguintes | o `bad status 00 00 00` da conversão 1 (§9.16), que está aberto |

**Critério**: o detector e o contador do link concordam, ±1 amostra por janela, no `amp`. No `usb`,
o detector é a única fonte, e a tabela diz isso. **Injeção**: arquivos com 1, 2 e 7 amostras
apagadas em posições conhecidas, e uma sessão `usb` em `schedutil`, que perde ~0,7% (`BRINGUP_AFE.md`
§6.5). O detector tem de achar as três posições e a taxa.

Este é o item que dá ao `usb` desta placa uma medida de perda que o hardware não fornece, e com isso
ele deixa de ser "não medido" na `RESULTS.md` §9.

### 3.8 Recursos de eletrodo

| Medida | Definição | Esperado | Critério |
|---|---|---|---|
| corrente de lead-off | queda num resistor conhecido (1 MΩ) entre a entrada e o terra, para cada corrente programável | 6 nA, 24 nA, 6 µA, 24 µA `[SBAS499?]` | ±20% |
| limiar do comparador | resistor em série aumentado em década até a flag virar, por entrada P e N | o limiar programado `[SBAS499?]` | a impedância de virada, por entrada |
| latência de detecção | um relé ou chave abre a entrada num instante marcado no analisador; primeiro quadro com a flag | | registrar em ms |
| falso positivo | 30 min com todas as entradas no simulador de 51 kΩ ∥ 47 nF | 0 transições | 0 |
| auditoria | cada transição gera um `ELECTRODE_CONTACT_CHANGED`, e não um por quadro | | contagem do journal = contagem do arquivo |
| BIAS_OUT | tensão no osciloscópio, em `reference` e `derived` | (AVDD+AVSS)/2 `[SBAS499?]` em `reference` | ±2% |
| impedância por excitação AC | lead-off AC em f<sub>DR</sub>/4, amplitude da linha / corrente | `[SBAS499?]` | **opcional**. Nenhum dos dois links o implementa (§4.3) |

**Injeção**: soltar um eletrodo conhecido. A flag certa (canal e polaridade) tem de virar, e só ela.
É o teste que a §9.16 não pôde fazer ("tudo solto" é trivial).

### 3.9 Com eletrodos reais, no fantoma

| Medida | Definição |
|---|---|
| offset de eletrodo | DC por canal, com remoção do offset do conversor medido em 3.1 |
| deriva de eletrodo | os primeiros 10 min depois do gel (polarização), e uma janela de 30 min |
| ruído com eletrodo | 3.2 repetido, contra o mesmo canal com o simulador |
| 60 Hz no ambiente | amplitude da linha de 60 Hz com bias `off`/`reference`/`derived` e montagem bipolar/referencial, dentro e fora da caixa, com o monitor HDMI ligado e desligado |
| sinal conhecido | o gerador injetado no fantoma por dois eletrodos de excitação (10 Hz, ~50 µV no ponto de medida): amplitude e fase pelos 8 canais |
| lead-off real | 3.8, retirando um eletrodo do fantoma |

**O que isto não mede**: a interface eletrodo-pele, o suor, o movimento, nem qualquer sinal
fisiológico. O fantoma salino é um condutor, não uma pele.

---

## 4. O que precisa ser implementado

### 4.1 A tela HDMI, persistente

As três correções de `BRINGUP_HMI_STM32MP2.md` §9, que hoje só existem como `chmod`/`vi` num cartão:

| # | Correção | Camada | Verificação |
|---|---|---|---|
| H1 | regra udev `KERNEL=="galcore", GROUP="video", MODE="0660"` | `meta-med-bsp`, por máquina | `ls -l /dev/galcore` no artefato **e** na placa |
| H2 | `idle-time=0` num `weston.ini` próprio | `meta-med-distro` (é política: tela de operador não apaga) | o `weston.ini` lido do ext4 produzido |
| H3 | modo CEA fixo (`HDMI-A-1`, `1920x1080@50`) por gancho de máquina | `meta-med-bsp` | o `weston.ini` ou o fragmento por máquina no artefato |
| H4 | a janela em tela cheia, no tamanho da saída (hoje 1024×600 fixos) | `meta-med-app`, `Main.qml` | o plano DRM do tamanho do modo (`modetest`, como em 2026-08-27) |

**Critério**: **dois reboots seguidos**, nas imagens `amp` e `usb`, com `weston.service` ativo,
`eeg-hmi.service` com `NRestarts=0` e imagem no monitor. **Injeção**: um terceiro boot sem o
monitor, depois plugando-o com tudo de pé. A imagem tem de aparecer, ou o comportamento fica
registrado. Lembrete de §10.1 de lá: ausência de erro não é evidência de saída.

### 4.2 A HMI de bancada

Tudo em QML/C++ dentro do `eeg-hmi-gui`, sem dependência nova (regra 6):

| # | Item | Observação |
|---|---|---|
| V1 | janela em **segundos**, não em amostras | `windowSamples_ = 750` são 3 s a 250 SPS e 47 ms a 16 kSPS |
| V2 | escala escolhida de uma lista fixa (10, 50, 100, 500, 5000 µV, 200 mV) e escrita no cabeçalho | D3. O padrão continua ±100 µV |
| V3 | remoção de DC e notch de 60 Hz de exibição, desligados por padrão e escritos no cabeçalho quando ligados | D3. Um passa-altas de 1ª ordem a 0,5 Hz e um notch IIR de 2ª ordem. Nada disso vai para o registro |
| V4 | lead-off por canal, na faixa do canal (P e N), e não só o status global em vermelho | os bits já chegam no `LeadOff` do quadro |
| V5 | contadores visíveis: quadros recebidos e rejeitados, e o link | `framesRejected` já existe |
| V6 | controle por teclado USB (escala, DC, notch); sem teclado, os padrões | a placa não tem entrada. Teclado ligado também faz o ocioso do weston contar, e por isso H2 continua necessário |

**Custo, medido e não presumido.** O `Canvas` 2D do QML rasteriza na CPU. A 1080p, com 8 canais e
17,9 quadros/s, o custo da HMI e do weston entra como nível de carga do `board_stats` §4. **Critério**:
com a HMI ligada, a taxa entregue e a perda (§3.7) não mudam, dentro da repetição, e `viewer missed`
no `AcquisitionStopped` é 0. Se o custo for alto, a troca é por um `QQuickItem` com
`QSGGeometryNode` (Qt puro), **só depois de medido**.

**Injeção**: ligar V3 com o gerador de teste interno (onda quadrada de ~1 Hz). A tela tem de mostrar a
forma distorcida pelo passa-altas, e o registro tem de continuar quadrado. É o teste de D2 e D3
juntos.

### 4.3 O caminho das condições

| # | Item | Camada |
|---|---|---|
| C1 | `scripts/afe-condition.sh` (BusyBox `sh`, roda na placa): parte do `eeg.conf` instalado, aplica `chave=valor` da linha de comando, **recusa** comentário inline e `@NOME@` restante (as mesmas guardas do `do_seal_configuration`), sela com `sha256sum`, instala o drop-in em `/run`, reinicia o serviço e espera o `READY=1` | `scripts/` (bancada) |
| C2 | `afe.input` e `afe.montage` (D5): tradução por link no `do_derive_device_options`; no `usb`, `input_mux` e o SRB1 no driver da branch da placa; no `amp`, opções do `ControlMessage` no firmware, com recusa para o que não for implementado | `meta-med-app`, `linux-med`, adjunta |
| C3 | lead-off no `usb`: o driver de kernel liga `LOFF_SENSP/N` para os canais habilitados e passa a reportar `lead_off_status`; até isso existir, o `do_derive_device_options` **recusa** `lead_off_detection = true` no `usb` | `linux-med` (`board/medplatform-afe`) e `meta-med-app` |
| C4 | a recusa em C3 é implementada **antes** do driver, porque é barata e fecha o defeito hoje | `meta-med-app` |
| C5 | lead-off AC (impedância): **fora**, salvo decisão. Fica registrado como lacuna nos dois links | — |

**Injeção de C1**: um `afe.gain = 24  # PGA` tem de ser recusado com a linha nomeada. Um `.sha256`
adulterado à mão tem de fazer o serviço recusar a partida.

### 4.4 A análise, no PC

`scripts/afe-analysis/`, Python com **numpy** e nada além. A disciplina "só biblioteca padrão" do
`board_stats` §3.2 vale para os contadores de lá. Aqui há FFT e Welch, e escrevê-los à mão seria
código de teste sem teste.

- `records.py`: lê os três formatos (a sessão do serviço, a captura do `amp-bench` e o buffer IIO
  do `usb`) e devolve canais em µV, carimbos, `sequence` e lead-off. O formato da sessão é o mesmo
  `amp::decodeFrame` do framework, portado e testado contra arquivos gerados pelo próprio framework;
- `noise.py`, `gain.py`, `freq.py`, `cmrr.py`, `loss.py`, `leadoff.py`: uma medida da §3 cada;
- `report.py`: o manifesto (`board_stats` §3.4, mais a condição da prescrição e o `config_digest`),
  as tabelas e a seção "o que isto não significa".

**Fase E0 deste plano**: cada analisador é visto falhando contra um arquivo sintético com o defeito
plantado (seno com salto de fase, ruído branco de RMS conhecido, filtro sinc³ simulado, canal com
ganho 1% fora) antes de ler um arquivo real.

---

## 5. Condições, e a fronteira de segurança

### 5.1 Sem eletrodos

Fontes internas (curto, gerador de teste, MVDD, temperatura) e fontes externas por atenuador, entrada
a entrada. Medidas: §3.1 a §3.7 e §3.8 (corrente e limiar, com resistores).

### 5.2 Simulador de eletrodo

Redes R∥C em todas as entradas, BIAS e SRB1, com o gerador por atenuador. Medidas: §3.2 (ruído com
impedância), §3.5 (CMRR com desequilíbrio de impedância de ±20% entre P e N, que é a condição
realista) e §3.8.

### 5.3 Eletrodos reais no fantoma

§3.9. Montagem bipolar primeiro (não depende de C2), referencial depois.

### 5.4 Eletrodos numa pessoa — **fora deste plano**

A fronteira do `implementation_plan_afe_bench.md` §3 continua onde está: **nenhuma pessoa é ligada ao
conversor**. O que segue registra o que atravessá-la exigiria, para que a decisão, se vier, seja
tomada com os requisitos à vista e não por conveniência de cronograma:

- **Isolamento galvânico de toda a cadeia em relação à rede.** Placa a bateria. **Nada ligado à
  tomada que toque a placa**: nem o monitor HDMI (a malha do cabo liga os terras), nem o PC pelo
  `usb` (só um laptop na bateria, sem o carregador), nem Ethernet, nem o console serial. Um monitor
  HDMI a bateria resolve a tela. A outra saída é uma barreira de isolamento na placa do AFE (DC-DC
  isolado e isolador digital no SPI), que **não se sabe se esta placa tem**, e tem de ser conferido
  no esquemático.
- **Corrente de fuga de paciente medida**, contra os limites da IEC 60601-1 para parte aplicada
  tipo BF, com o arranjo exato da sessão.
- **Aprovação ética.** Pesquisa com seres humanos no Brasil passa pelo sistema CEP/CONEP (Resolução
  CNS 466/2012). Se isso vale para autoexperimentação num TCC é pergunta para o orientador, não para
  este documento.

Sem os três, a §5.3 é o limite, e ela já responde a quase tudo deste plano.

---

## 6. Fases

| Fase | Conteúdo | Links | Depende de |
|---|---|---|---|
| **A0** | dívida: tabelas de ruído, CMRR, filtro, lead-off e fórmula de temperatura do SBAS499 transcritas e conferidas (`afe_bench` Fase 0); C4; o cartão com `performance` (a imagem de 08/10 já tem) | — | — |
| **A1** | HDMI persistente (H1–H4), dois reboots, nas duas imagens | ambos | — |
| **A2** | HMI de bancada (V1–V6), com custo medido e injeção | ambos | A1 |
| **A3** | C1, e as ferramentas de análise com o E0 deste plano | ambos | — |
| **A4** | sem eletrodos, fontes internas: §3.1, §3.2 (curto), §3.6 | `amp` em todas as ODR, `usb` a 250 | A0, A3 |
| **A5** | sem eletrodos, fontes externas: §3.3, §3.4, §3.5, §3.7 | ambos | A4 |
| **A6** | C2 (`afe.input`, `afe.montage`) e C3 (lead-off no `usb`) | ambos | A3 |
| **A7** | simulador de eletrodo: §3.2 com impedância, §3.5 desequilibrado, §3.8 | ambos (o `usb` só depois de C3) | A5, A6 |
| **A8** | fantoma: §3.9, bipolar e referencial, com a HMI ligada e desligada | ambos | A7 |
| **A9** | a tabela: uma coluna por link, cada célula com número, método, fonte (uma ou duas), condição e link para a execução. **Célula vazia fica vazia e nomeada** | — | A4–A8 |

```
A0 ──┬──────────────► A4 ─► A5 ─────────────┐
A3 ──┘                                      ├─► A7 ─► A8 ─► A9
A3 ─► A6 (C2, C3) ──────────────────────────┘
A1 ─► A2 ─────────────────────────────────────────► (A4 em diante, como carga e como olho)
```

A1 e A3 podem começar já e não dependem um do outro. A4 não espera a tela: ela entra como carga a
partir do momento em que existe, e cada execução registra no manifesto se estava ligada.

---

## 7. Riscos

- **A HMI perturbar o que mede.** É por isso que ela é um fator (ligada × desligada) e que a §4.2 tem
  critério de custo. Um resultado do tipo "o Canvas custa X% da CPU e não move a perda" já é um
  resultado.
- **Um ruído bom demais.** Um piso abaixo do datasheet quer dizer que a entrada não estava onde se
  pensava. As injeções de §3.1 e §3.2 existem para isso.
- **Atenuador mal caracterizado.** Toda medida de µV externa herda o erro dele, então a razão é
  medida com o multímetro, e não lida do valor nominal.
- **Os dois mestres no SPI.** D4. A troca de link é física e entra no manifesto.
- **O `bad status` da primeira conversão contaminar estatísticas.** A §3.7 o mede antes de qualquer
  outra estatística descartá-lo. Descartar sem medir seria esconder.
- **A placa desliga sozinha depois de muito tempo ligada** (relato do operador, §9.16, causa
  desconhecida). As sessões de 30 min e 2 h podem não terminar. Uma sessão interrompida é registrada
  como tal e não é cortada para caber.
- **O journal perder um boot** (§9.16, aberto). A auditoria de lead-off (§3.8) depende dele: conferir
  `journalctl --list-boots` no fim de cada sessão.
- **Transcrição incompleta do datasheet.** A regra do `implementation_plan_mcp2210.md`: uma
  transcrição incompleta produz a categoria errada, não uma resposta parcial. O A0 vem antes das
  medidas que ele julga.

---

## 8. Onde cada número vai

- cada execução: `BRINGUP_AFE.md`, numa seção nova por fase (A1–A8), com manifesto e comando;
  a HDMI, em `BRINGUP_HMI_STM32MP2.md`, fechando a §9 de lá;
- os números que o TCC cita: `RESULTS.md` §3, com a frase do que cada um **não** significa; o que
  sair de "não medido" sai da §9;
- a tabela final (A9): aqui e na Fase 7 do `implementation_plan_afe_bench.md`;
- os dados brutos: fora do repositório, com o hash no manifesto, pela mesma decisão pendente do
  `board_stats` §8.
