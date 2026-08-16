# Plano de Implementação — Front-end de Aquisição ADS1299 (PoC EEG)

> **Status**: plano corrente (não histórico). Complementa `PROJECT_CONTEXT.md`, que continua sendo
> a referência arquitetural autoritativa. Os demais `implementation_plan*.md` são iterações de
> projeto anteriores e **não** descrevem o estado atual do repositório.
>
> **Escopo**: define como o conversor **Texas Instruments ADS1299** (AFE/ADC de biopotenciais,
> 8 canais, 24 bits) entra na arquitetura `MedStack` sem violar a fronteira entre camadas, e
> enumera as mudanças concretas — arquivo a arquivo — que a escolha desse *part-number* impõe.
>
> **Nada aqui foi validado por bitbake ainda** (nenhum `kas build` rodou neste repositório).
> Os números de datasheet devem ser conferidos contra o documento **SBAS499** da TI antes de
> serem citados no texto final do TCC.

---

## 1. O que o ADS1299 muda — e o que ele deliberadamente não muda

A tese central do projeto é que a aplicação não conhece o hardware. A escolha de um AFE concreto
é justamente o teste dessa afirmação, então o critério de aceitação deste plano é:

> **Nenhuma linha de `eeg-acquisition-service` nem de `eeg-hmi-gui` menciona "ADS1299", e nenhum
> campo de `DeviceConfig`/`MedicalDevice` no MedFramework é específico do ADS1299.**

O que muda por camada:

| Camada | Impacto do ADS1299 |
|---|---|
| **BSP** | Onde o chip realmente existe: instância SPI, linha `DRDY`, GPIOs (`START`/`RESET`/`PWDN`/`CS`), alimentação analógica, e a atribuição desses recursos ao Cortex‑M4 no *device tree*. **Novo requisito**: hoje o repositório não tem layer de BSP própria. |
| **`meta-med-distro`** | Política de kernel genérica de barramento/AFE (`CONFIG_SPI`), nada específico do *part-number*. |
| **`meta-med-framework`** | O driver `rpmsg` ganha um canal de **controle** (não só de dados) e `DeviceConfig` ganha um dicionário de opções **opacas**; o driver `simulated` passa a emular as características elétricas do ADS1299. Nenhum símbolo `ADS1299` no framework. |
| **`meta-med-app`** | `eeg.conf` ganha os parâmetros clínicos (ganho, ODR, detecção de eletrodo solto, sinal de teste) e o envelope de segurança passa a ser derivado do conversor. Código-fonte das aplicações: **inalterado**. |
| **Firmware Cortex‑M4** | Todo o conhecimento do *part-number*: mapa de registradores, comandos SPI, tratamento de `DRDY`, conversão para nanovolts. Artefato do BSP, referenciado por `MED_AMP_FIRMWARE`. |

---

## 2. Parâmetros do conversor que a plataforma precisa conhecer

Extraídos do datasheet (conferir em SBAS499):

| Característica | Valor | Onde impacta |
|---|---|---|
| Canais simultâneos | 8 (variantes ‑6 e ‑4 com 6 e 4 canais) | `acquisition.channels = 8` já está correto |
| Resolução | 24 bits, ΔΣ, amostragem simultânea | Formato de amostra e quantização do simulador |
| Taxas de dados (ODR) | 250, 500, 1k, 2k, 4k, 8k, 16k SPS | **Conjunto discreto** — invalida o limite contínuo atual |
| Ganho do PGA | 1, 2, 4, 6, 8, 12, 24 | Novo parâmetro de configuração; define o fundo de escala |
| Referência interna | 4,5 V (ou externa) | Define a escala em conjunto com o ganho |
| Fundo de escala | ±V<sub>REF</sub>/ganho → **±187,5 mV** com ganho 24 | Verificação DERS do envelope de segurança |
| Passo do LSB | V<sub>REF</sub>/(ganho·2²³) → **22,35 nV** com ganho 24 | **Colide com o campo `scaleNanoUnitsPerLsb`** (§4.1) |
| Ruído referido à entrada | ≈1 µV<sub>pp</sub> (0,14 µV<sub>rms</sub>) a 250 SPS, ganho 24 | Piso de ruído do simulador e critério de autoteste |
| CMRR | ≈ −110 dB | Justificativa de projeto (capítulo de hardware) |
| Quadro por `DRDY` | 24 bits de status + 8 × 24 bits = **27 bytes** | Dimensionamento do buffer rpmsg (§4.2) |
| Recursos integrados | *bias drive*, detecção de eletrodo solto, gerador de sinal de teste (±1 mV / ±2 mV, ~1 Hz / ~2 Hz), sensor de temperatura, SRB1/SRB2, *daisy-chain* | Implementação real de `selfTest()` (§4.3) |
| Interface | SPI (SCLK até 20 MHz; oscilador interno 2,048 MHz), `DRDY`, `START`, `RESET`, `PWDN` | Firmware M4 / device tree |
| Alimentação | AVDD 5 V (ou ±2,5 V), DVDD 1,8–3,3 V, ≈5 mW/canal | Capítulo de hardware |

---

## 3. Decisão de topologia: o ADS1299 fica no Cortex‑M4

```text
 ADS1299 ──SPI + DRDY──► Cortex-M4 (FreeRTOS)  ──OpenAMP/rpmsg──►  Linux (Cortex-A35)
   AFE                    registradores, ISR de DRDY,               MedicalIPC → driver "rpmsg"
                          conversão LSB → nanovolt,                 → MedicalDevice → serviço EEG
                          empacotamento em med::amp::FrameHeader
```

Três razões, todas verificáveis e citáveis no TCC:

1. **Não existe driver IIO `ti-ads1299` no kernel mainline.** A família tem suporte parcial em
   *upstream* (o parente mais próximo é o driver do ADS1298, voltado a ECG); confirmar na árvore
   usada pelo BSP com `ls drivers/iio/adc | grep ads`. Colocar o chip no lado Linux implicaria
   manter um driver *out-of-tree* — exatamente o acoplamento a BSP que a arquitetura combate.
2. **Determinismo do `DRDY`.** A 16 kSPS o intervalo entre interrupções é de 62,5 µs; atender isso
   no Linux exigiria PREEMPT_RT e ainda assim sem garantia formal. No M4 é uma ISR trivial.
3. **É a demonstração do AMP** que o PoC já se propõe a fazer — o ADS1299 dá a ela uma carga real.

O perfil QEMU permanece com o driver `simulated`; a variável `MED_EEG_DRIVER` continua sendo a
única chave de retargeting. **A métrica de portabilidade do TCC não é afetada** — desde que a
regra de §4.4 seja respeitada (o driver simulado precisa aceitar as mesmas opções que o `rpmsg`).

---

## 4. Mudanças concretas

### 4.1. Precisão da escala no protocolo AMP — **corrigir antes de qualquer bancada**

`med::amp::FrameHeader::scaleNanoUnitsPerLsb` é um `int32_t` em **nanovolts por LSB**
(`MedicalDevice.h`). Com ganho 24 o passo real do ADS1299 é 22,3517 nV; arredondado para 22 nV o
resultado é um **erro de ganho sistemático de 1,6 %** em todo o traçado — inaceitável para um
equipamento de medição e, pior, invisível (o sinal continua "bonito").

Opções avaliadas:

| Opção | Efeito | Custo |
|---|---|---|
| **(A) O M4 converte para nanovolts e envia nV; `scaleNanoUnitsPerLsb = 1`** | Erro < 0,5 nV, muito abaixo do piso de ruído de 140 nV<sub>rms</sub> | Uma multiplicação por amostra no M4. **Zero mudança de ABI** — o `static_assert(sizeof(FrameHeader) == 40)` continua válido e é exatamente o que o serviço já faz ao publicar para a HMI |
| (B) Enviar contagens brutas e estender o cabeçalho com escala racional (num/den) | Exato | Quebra a ABI e o `static_assert`; obriga a versionar o protocolo |
| (C) Mudar a unidade do campo para picovolts | Exato | Muda a semântica de um campo existente sem mudar o nome — pior tipo de mudança silenciosa |

**Recomendação: (A).** Fundo de escala ±187,5 mV = ±187.500.000 nV, que cabe folgadamente em
`int32_t`. Documentar no comentário do `FrameHeader` que o produtor entrega nanovolts.

### 4.2. Dimensionamento do quadro rpmsg

Um buffer rpmsg padrão é de 512 bytes; descontando o cabeçalho de transporte sobram ~496 bytes,
menos os 40 do `FrameHeader` → 456 bytes de payload.

- Com amostras `int32` (formato atual): 114 amostras → **14 amostras/canal** para 8 canais.
- A 250 SPS isso é um quadro de 56 ms — folgado para uma HMI de EEG.
- A 16 kSPS seriam ~1140 quadros/s, o que torna o *overhead* de 40 bytes de cabeçalho relevante.

O comentário atual em `eeg.conf` cita 12 amostras/quadro; **recalcular e alinhar o comentário com
o valor efetivo** ao ajustar o arquivo. Se o perfil de alta taxa (≥4 kSPS) entrar no escopo,
avaliar empacotar as amostras nos 24 bits nativos do ADS1299 (456/3 = 152 → 19 amostras/canal,
+35 % de payload) — mas isso é mudança de formato de fio e fica fora deste plano.

### 4.3. `selfTest()` deixa de ser um *stub*

Hoje `MedicalDevice::selfTest()` é essencialmente simbólico. O ADS1299 permite uma verificação de
desempenho essencial (IEC 60601‑1 §14) genuína, executada pelo firmware M4 e reportada ao Linux
como um resultado agregado:

1. **Identidade**: ler o registrador de ID e conferir o valor da variante de 8 canais.
2. **Sinal de teste interno**: rotear o MUX de todos os canais para o gerador interno e verificar
   amplitude de 1 mV ±tolerância e período de ~1 s em cada canal — valida cadeia analógica, PGA,
   ADC e enlace rpmsg de ponta a ponta.
3. **Ruído com entradas em curto**: MUX em *shorted input*, verificar que o ruído RMS medido está
   dentro do especificado (≈0,14 µV<sub>rms</sub> a 250 SPS/ganho 24).
4. **Eletrodo solto**: acionar a detecção integrada e reportar o status por eletrodo.

O resultado atravessa `MedicalDevice::selfTest()` como `Status` + evento de auditoria via
`MedicalLogger` — a aplicação continua sem saber que existe um registrador de ID.

### 4.4. Como os parâmetros clínicos chegam ao M4 sem contaminar o framework

Ganho e ODR são prescrições **da aplicação médica**, mas registradores **do chip**. Para não criar
campos `gain`/`leadOff` em `DeviceConfig` (o que colocaria o ADS1299 dentro do MedFramework):

- `DeviceConfig` ganha **um** campo genérico: `std::map<std::string, std::string> driverOptions`.
- O driver `rpmsg` serializa essas opções em uma mensagem de **controle** no mesmo *endpoint*
  rpmsg, antes de iniciar a aquisição; o firmware traduz para escritas de registrador.
- O driver `simulated` **aceita as mesmas chaves** e as honra na geração do sinal (§4.5). É isso
  que mantém `eeg.conf` idêntico entre QEMU e STM32MP257 exceto por `MED_EEG_DRIVER`.
- Chave desconhecida → o driver falha explicitamente em vez de ignorar (uma opção de segurança
  silenciosamente descartada é um defeito, não uma tolerância).

Isso exige uma extensão pequena do protocolo AMP (um tipo de mensagem de controle além do quadro
de amostras) — descrevê-la junto ao `FrameHeader`, com o mesmo rigor de layout fixo.

### 4.5. `eeg.conf` — novos parâmetros

```ini
# --- front-end -------------------------------------------------------------
device.driver = @MED_EEG_DRIVER@
device.id = eeg0
device.address = /dev/rpmsg0
# Part-number do AFE. Consumido apenas pelo firmware/simulador; o serviço
# nunca ramifica sobre este valor.
device.model = ads1299

# --- opções do front-end (repassadas opacamente ao driver) -----------------
afe.gain = 24                    # PGA: 1,2,4,6,8,12,24
afe.reference_uv = 4500000       # referência interna de 4,5 V
afe.lead_off_detection = true
afe.bias_drive = true            # amplificador de bias (eletrodo de referência)
afe.test_signal = off            # off | internal_1mv_1hz  (usado por selfTest)

# --- acquisition -----------------------------------------------------------
acquisition.channels = 8
acquisition.sample_rate_hz = 250 # ODR válidos: 250,500,1000,2000,4000,8000,16000
acquisition.samples_per_frame = 14
```

A vedação de integridade (`do_seal_configuration`) continua valendo sem mudança: o `.sha256` é
regerado sobre o arquivo já substituído.

### 4.6. Envelope de segurança — de limites fixos para limites derivados

Os limites atuais em `safetyLimits()` (`eeg-acquisition-service/files/src/main.cpp`) ficam
inconsistentes com o conversor real:

| Limite atual | Problema com o ADS1299 | Correção |
|---|---|---|
| `acquisition.sample_rate_hz` ∈ [125, 2000] | 125 Hz é inatingível; o conversor só oferece taxas discretas | Verificação de **pertinência ao conjunto** {250, 500, 1k, 2k, 4k, 8k, 16k} |
| `acquisition.channels` ∈ [1, 64] | O chip tem 8; >8 exige *daisy-chain* | Múltiplo de 8, máximo 32 (4 dispositivos em cascata) |
| `safety.max_input_uv = 500`, limite [10, 5000] | Não há relação declarada com o fundo de escala | **Nova verificação DERS relacional**: `safety.max_input_uv ≤ 1e6·V_REF/ganho`. Com ganho 24 → 187.500 µV, então 500 µV é um limiar de artefato fisiológico, e isso passa a estar explícito |
| — | Ganho não verificado | `afe.gain` ∈ {1,2,4,6,8,12,24} |

O comportamento permanece: violação **para o serviço**, não satura o valor.

### 4.7. Driver `simulated` — passa a simular *este* conversor

Para que o QEMU exercite os mesmos caminhos de código:

- quantizar em passos de V<sub>REF</sub>/(ganho·2²³) e saturar em ±V<sub>REF</sub>/ganho;
- somar piso de ruído coerente com o datasheet (≈0,14 µV<sub>rms</sub> a 250 SPS/ganho 24);
- aceitar apenas os ODR válidos;
- simular deriva de offset DC de eletrodo e eventos de eletrodo solto (para exercitar a HMI);
- reproduzir a onda quadrada de 1 mV/1 Hz quando `afe.test_signal` estiver ativo, para que o
  `selfTest()` tenha o mesmo roteiro nos dois alvos.

`DeviceInfo::model` passa a refletir o que está sendo emulado (ex.: `"ADS1299 (simulado)"`).

### 4.8. Kernel e BSP

**`meta-med-distro/recipes-kernel/linux/files/med-kernel-features.cfg`** — adicionar ao bloco de
Industrial I/O, mantendo o caráter genérico (política de barramento, não *part-number*):

```
CONFIG_SPI=y
CONFIG_SPI_MASTER=y
```

Não incluir driver de ADS1299 aqui: no caminho AMP o Linux nunca enxerga o chip.

**BSP — lacuna a resolver.** O ADS1299 exige, no `stm32mp257f-ev1` (ou na placa filha do TCC):
instância SPI e GPIOs (`DRDY`, `START`, `RESET`, `PWDN`, `CS`) **atribuídos ao domínio do M4** via
o *Resource Isolation Framework* do STM32MP2, para que o Linux não reivindique os pinos. Isso é um
*overlay*/patch de device tree, ou seja, material de camada BSP — e hoje o repositório não tem uma.
Duas saídas, a decidir:

- **(a)** criar `meta-med-bsp-<placa>` com prioridade 7, abaixo de `meta-med-distro`; ou
- **(b)** manter um `.bbappend` de device tree dentro de `meta-st-stm32mp` via *overlay* de máquina.

A opção (a) é a coerente com a tese (a fronteira BSP fica explícita e o restante permanece
portável); a (b) é mais rápida para a bancada. Recomendação: **(a)**, mesmo que a layer nasça com
um único *overlay*.

`MED_AMP_FIRMWARE` em `kas/project-eeg-stm32mp2.yml` deixa de ser um comentário e passa a apontar
para a receita que empacota o firmware M4 do ADS1299 em `/lib/firmware`.

---

## 5. Ordem de execução sugerida

1. Corrigir a escala do protocolo AMP (§4.1) e documentar a mensagem de controle (§4.4) — é a base
   de tudo e é puramente de framework, testável no host.
2. Estender `DeviceConfig` com `driverOptions` e atualizar os dois drivers.
3. Enriquecer o driver `simulated` (§4.5) e estender o teste funcional do host com verificações de
   quantização, fundo de escala e ODR válidos.
4. Atualizar `eeg.conf` (§4.5) e `safetyLimits()` (§4.6); reexecutar o teste de configuração
   adulterada e de parâmetro fora de faixa.
5. Só então: primeiro `kas build` (perfil QEMU) — que também é a primeira validação real da
   metadata BitBake, ainda pendente.
6. Firmware M4 + layer de BSP + bancada com o ADS1299 (§4.8).

## 6. Plano de verificação

| Nível | Verificação |
|---|---|
| Host | Compilação limpa das unidades do MedFramework sob `-Wall -Wextra -Wpedantic -Wshadow -Wconversion` |
| Host | Teste funcional estendido: quantização de 22,35 nV, saturação em ±187,5 mV, rejeição de ODR inválido, rejeição de ganho inválido, `max_input_uv > FS` recusado |
| Build | `kas build kas/project-eeg-qemu.yml` e conferência do `.sha256` de `eeg.conf` no rootfs |
| QEMU | Serviço publica quadros com CRC válido; HMI reconstrói amplitude correta a partir de `scaleNanoUnitsPerLsb = 1` |
| Bancada | Frequência de `DRDY` medida = ODR programado; onda de teste de 1 mV verificada em todos os 8 canais; ruído com entradas em curto dentro do datasheet; detecção de eletrodo solto reportada até a HMI |
| Tese | Métrica de portabilidade: `diff` do código de aplicação entre os dois alvos permanece **vazio**; a diferença de configuração permanece **uma variável** (`MED_EEG_DRIVER`) |

## 7. Riscos e questões em aberto

1. **Ausência de driver mainline** — confirmar na árvore do BSP (`ls drivers/iio/adc | grep ads`).
   Se um driver aceitável aparecer, o caminho IIO vira uma comparação interessante para o TCC, não
   uma substituição do caminho AMP.
2. **Valores de datasheet** citados aqui precisam ser conferidos em SBAS499 antes de irem para o
   texto final (especialmente ID de dispositivo, tolerância do sinal de teste e figuras de ruído).
3. **Mensagem de controle no rpmsg** é uma extensão de ABI: definir layout fixo e `static_assert`,
   como já foi feito para o `FrameHeader`.
4. **Camada de BSP inexistente** (§4.8) — decisão pendente entre (a) e (b).
5. Os riscos de build já registrados em `CLAUDE.md` (bbappend de `rauc-conf`, pino do `meta-qt6`
   em 6.8, nomes de pacotes Qt) continuam válidos e vêm antes da bancada.
