# Plano de implementação — o segundo perfil de dispositivo: bomba de infusão simulada

> **Status**: plano, 2026-11-02. **Nada nele foi executado** além do levantamento da §3.
>
> **Prazo**: não considerado, por decisão do autor. A §11 dá a ordem de execução e o que pode
> parar onde sem invalidar o que já foi feito. Este plano corre ao lado de
> `implementation_plan_conformity.md` e se apoia nele (§9).
>
> **Onde o resultado mora**: os números vão para `RESULTS.md` §2, que passa de "dois perfis, um
> deles vazio" para três perfis. O registro de engenharia vai para a §13 deste arquivo.

---

## 1. Para que este perfil existe

A tese afirma que a arquitetura permite construir **classes distintas** de dispositivo médico
reaproveitando a plataforma, e que esse reaproveitamento é **mensurável**. Hoje a afirmação tem um
caso de controle fraco: `med-image-tomograph` não instala aplicação nenhuma, e o seu delta é zero
por construção (`RESULTS.md` §2). Ele prova que a plataforma se sustenta sozinha. Ele não prova
que uma segunda classe cabe nela.

Uma segunda classe só é evidência se for **diferente do EEG no que importa à plataforma**. Uma
bomba de infusão é:

| | EEG (prova de conceito) | Bomba de infusão (caso de controle) |
|---|---|---|
| Papel do software | sensor: observa e registra | **atuador em malha fechada**: age sobre o paciente |
| Fluxo dominante | contínuo, alta taxa (até 16 kSPS × 8 canais) | baixa taxa, comandos e alarmes |
| Falha perigosa | dado errado ou perdido, diagnóstico errado | **sobreinfusão, subinfusão, embolia gasosa, atraso de terapia** |
| Estado seguro | parar de gravar | **parar o motor**, e não retomar sozinho |
| Classe IEC 62304 (hipótese) | B | **C** |
| Norma particular | IEC 60601-2-26 `[?]` | IEC 60601-2-24 `[?]` |

Um oxímetro ou um ECG seriam mais fáceis, e fracos: são da mesma família de sinal biológico em
fluxo, e o reuso alto seria explicado pela semelhança, não pela arquitetura.

**O que o perfil não é**: uma bomba de infusão. O mecanismo é simulado. Nenhum número deste plano
diz algo sobre a exatidão de entrega de uma bomba real, e o texto do TCC tem de dizer isso onde
cada número aparecer (§12).

---

## 2. As perguntas que o perfil responde, e como cada uma é medida

1. **Quanto de uma nova classe é plataforma herdada?** Pacotes, linhas e tarefas de build
   reaproveitadas (§8, M3–M5).
2. **Quais camadas uma nova classe obriga a mudar?** `git diff --stat` por camada entre o commit de
   base e o fim do trabalho (§8, M1). Esperado: `meta-med-bsp` = 0, `meta-med-distro` = 0, adjuntas
   = 0, `meta-med-framework` = pequeno e justificado, `meta-med-app` = o resto.
3. **Cada mudança no framework é genérica, ou é a bomba vazando para a plataforma?** Toda mudança no
   framework é classificada (§8, M2), e a busca de vocabulário de bomba no framework tem de voltar
   vazia (regra 9 do repositório, aplicada à classe de dispositivo).
4. **Os requisitos técnicos da plataforma valem para a segunda classe sem trabalho novo?** Dos RTs de
   `implementation_plan_conformity.md` §4, quantos a bomba herda com zero linhas (§9).
5. **A plataforma está "em formato de EEG"?** A medição honesta inclui o que o EEG deixou no
   framework e a bomba não usa (§3), contado antes e depois.

---

## 3. Levantamento: onde o framework está moldado pelo EEG (2026-11-02)

Achados de leitura dos cabeçalhos em `meta-med-framework/.../include/`. Cada um é uma lacuna que a
bomba vai expor, e é por isso que a bomba é um bom caso de controle.

**L1. Não existe caminho de atuação.** `MedicalDevice.h` se declara "biomedical sensor and actuator
abstraction", mas a interface é `start`, `stop`, `read`, `selfTest`. Não há comando, não há leitura
de volta do comando, e não há contrato de estado seguro. O framework **promete** atuador e não o
tem.

**L2. Uma unidade por dispositivo, e uma faixa de ±2,147 unidades.** `DeviceInfo::unit` é uma
string única para todos os canais. `SampleFrame` guarda micro-unidades, e o formato de fio as
transporta como `int32` de nano-unidades: "one that does not fit an int32 of nano-units (beyond
±2.147 units) is saturated". Para o EEG, a unidade é o volt e a faixa sobra. Para a bomba:
- a pressão de linha em mmHg satura em 2,147 mmHg, e um alarme de oclusão fica por volta de
  centenas de mmHg;
- pressão, posição do êmbolo e ar na linha têm unidades diferentes no mesmo quadro.

Funciona escolhendo unidades SI grandes (MPa, L), mas isso é contornar a API, não usá-la. É D4.

**L3. Vocabulário de biopotencial no framework.** `LeadOff` (contato de eletrodo) em
`MedicalDevice.h`, `AuditEvent::ElectrodeContactChanged` em `MedicalLogger.h`, `BiosignalWriter`
(BDF+) em `MedicalStorage.h`. Não é vocabulário de **componente** (a regra 9 continua valendo), é
de **classe de dispositivo**. A bomba não usa nenhum dos três. M2 conta isso antes e depois, e o
texto do TCC tem de dizer que a regra 9 protege contra o nome da peça e não contra a classe.

**L4. Eventos de auditoria fechados.** `AuditEvent` é um `enum class` do framework. A bomba precisa
registrar alarme disparado e reconhecido, terapia iniciada e interrompida, limite flexível
ultrapassado pelo operador. Ou o framework ganha eventos genéricos, ou passa a aceitar eventos
definidos pela aplicação (D5). O `ElectrodeContactChanged` mostra que o enum já absorveu
vocabulário do EEG uma vez.

**L5. Variáveis de plataforma com nome de EEG.** `MED_EEG_LINK` é lido no `meta-med-distro`
(`linux-%.bbappend`, guarda do `MED_BENCH`), no `meta-med-bsp` (mapeamento de símbolos de kernel
por link) e em `kas-base.yml`. A bomba não define essa variável, então as guardas não disparam.
Mesmo assim, é uma variável de **front-end analógico** com nome de **classe de dispositivo** na
camada de política do SO. Registrada como dívida revelada; a correção (`MED_AFE_LINK`) não é
exigida pela bomba e, se for feita, é contada à parte em M1 (D10).

**L6. O que serve sem mudança, a confirmar no uso.** O codec de controle (`encodeControl` /
`decodeControl`, até 8 pares chave-valor com resposta obrigatória), o `MedicalConfiguration` com
`SafetyLimit` (cujo comentário já chama o padrão de "DERS (dose error reduction)", terminologia
de bomba de infusão), o `MedicalStorage`, o `MedicalLogger`, o `MedicalUpdate` e o padrão de
confirmação de slot por `boot-complete.target`.

---

## 4. O dispositivo simulado

### 4.1 Escopo clínico

**Bomba de seringa**, um canal, programação em **mL/h**:

- programa: fármaco (da biblioteca), taxa, volume a infundir (VTBI);
- estados: `ociosa → programada → infundindo ⇄ pausada → concluída`, e `alarme` a partir de
  qualquer estado de entrega;
- **biblioteca de fármacos com DERS**: para cada fármaco, limites rígidos (a bomba recusa) e
  flexíveis (exige confirmação explícita do operador, auditada);
- **alarmes** (prioridades da IEC 60601-1-8 `[?]`): oclusão a jusante (alta), ar na linha (alta),
  falha de entrega (comandado ≠ medido, alta), fim de VTBI (média), seringa quase vazia (baixa),
  falha de comunicação com o mecanismo (alta);
- reconhecimento e silenciamento de alarme, com tempo de silêncio limitado.

**Fora do escopo** (D9): programação por dose (mg/kg/min), *bolus*, múltiplas áreas de cuidado,
KVO, bateria, proteção contra fluxo livre (mecânica), redução do *bolus* pós-oclusão. Cada um é
recurso clínico real, e ficar fora é declarado, não omitido.

### 4.2 O mecanismo simulado

Um processo separado, **`infusion-sim`**, faz o papel que o firmware do Cortex-M33 faz para o EEG:
é o **produtor** do outro lado de um transporte, e o serviço de controle não sabe se fala com um
simulador ou com hardware (D2). O modelo:

- motor de passo + fuso: deslocamento por passo; volume = deslocamento × área da seringa (tabela
  por tamanho de seringa);
- *encoder* do êmbolo: a posição medida, independente da comandada, que é o que permite detectar
  falha de entrega;
- pressão de linha, modelo de primeira ordem: `C·dP/dt = Q_bomba − P/R`. Oclusão é `R → ∞`, e a
  pressão sobe com a constante de tempo da complacência `C`;
- detector de ar: binário, por segmento de linha.

**Injeção de falhas** pelo próprio simulador: oclusão, ar, travamento do motor (o *encoder* para e
o comando continua), deriva do *encoder*, e o produtor parando de responder. A injeção existe só
no simulador, que só existe no perfil simulado: não há porta de injeção no caminho clínico.

### 4.3 O contrato de estado seguro

É o requisito que mais distingue a bomba do EEG, e o que mais vale como teste:

1. **Homem-morto**: o atuador exige um sinal de vida do controlador a cada `T_hb`. Sem sinal por
   `T_hb × k`, ele para o motor sozinho. O estado seguro é do lado do atuador, não do controlador,
   porque um controlador travado não consegue se parar.
2. **Sem retomada automática**: se o serviço de controle reiniciar (falha, `Restart=on-failure`,
   reboot, atualização), a bomba volta **parada e em alarme**, nunca infundindo. Isso conflita de
   propósito com o `Restart=on-failure` que o EEG usa, e é a decisão de produto que um atuador
   impõe e um sensor não.
3. **Atualização não acontece durante terapia** (D7).

---

## 5. Arquitetura por camada

```
meta-med-app
  infusion-control-service   máquina de estados, DERS, alarmes, homem-morto, auditoria
  infusion-hmi-gui           Qt/QML: programar, confirmar, iniciar, pausar, alarmes
  infusion-sim               o mecanismo simulado (o "produtor", D2)
  infusion-ctl               linha de comando, para o make check (o papel do medctl)
  med-image-infusion.bb      require base + dev; a lista de instalação
        │ só as APIs Medical*.h
meta-med-framework
  MedicalActuator.h          NOVO: comando, leitura de volta, contrato de estado seguro (D3)
  driver por transporte local (soquete) para MedicalDevice + MedicalActuator, se necessário (D2)
  AuditEvent                 eventos genéricos acrescentados, ou extensíveis (D5)
  unidades por canal         se D4 decidir mudar a API
meta-med-distro              esperado: 0 linhas (exceto D7, se aprovado)
meta-med-bsp                 esperado: 0 linhas
adjuntas                     esperado: 0 linhas
kas/project-infusion-qemu.yml   máquina, alvo, variáveis do perfil
```

Os nomes seguem a regra dos drivers: o framework só ganha nomes de **transporte** e de
**conceito genérico** (atuador, alarme, terapia), nunca "bomba", "seringa", "oclusão" ou "infusão".

---

## 6. Decisões

- **D1. Seringa, não peristáltica.** A posição do êmbolo é uma medida direta do volume entregue, e
  isso dá à falha de entrega um detector simples e testável. A peristáltica exigiria modelar a
  deformação do tubo.
- **D2. O simulador é um processo produtor, fora do serviço e fora do framework.**
  - Dentro do framework, o modelo de uma seringa seria vocabulário de classe na plataforma (L3).
  - Dentro do serviço, a aplicação testaria a si mesma, e o homem-morto não teria o que testar.

  O transporte entre os dois é decisão da Fase 2: um driver de soquete local no framework,
  genérico, ou o `rpmsg` com um *endpoint* simulado. Um driver novo nomeado pelo transporte é
  aceitável pela regra dos drivers; um nomeado pelo dispositivo não é.
- **D3. `MedicalActuator` é uma interface nova, não um método a mais em `MedicalDevice`.** Um
  sensor e um atuador têm contratos de falha opostos: o sensor que para deixa de informar, e o
  atuador que para tem de parar **num estado definido**. A interface proposta:
  - `command(setpoint)` com resposta obrigatória (o padrão do `ControlAck`);
  - `readback()` do que o atuador está de fato fazendo;
  - `heartbeat()`, e `safeState()` declarado pelo driver;
  - um *timeout* de homem-morto que o driver **tem** de implementar do lado do produtor.

  Ela fica genérica: serve a uma válvula de ventilador ou a um estimulador. A versão final sai da
  Fase 2, com testes de host antes do serviço.
- **D4. Unidades e faixa** (L2). Três opções:
  - (a) unidades SI grandes, sem mudar a API: o atalho, e é contornar;
  - (b) unidade por canal em `DeviceInfo` e um fator de escala por canal no quadro: muda o formato
    de fio, que é compartilhado com o M33, e vira uma versão nova do `med-amp-abi`;
  - (c) a bomba usa `MedicalDevice` só para canais de mesma unidade, um dispositivo por grandeza.

  **Recomendação**: (b), porque é o defeito real da API. Se o custo no `med-amp-abi` for alto,
  (c), com a limitação registrada. A (a) só como último recurso, e contada como contorno em M2.
- **D5. Eventos de auditoria.** Acrescentar eventos genéricos ao fim do enum (`AlarmRaised`,
  `AlarmAcknowledged`, `TherapyStarted`, `TherapyStopped`, `SafetyLimitOverridden`,
  `ActuatorFault`), seguindo o precedente do `ElectrodeContactChanged`, que também foi acrescentado
  ao fim. A alternativa, códigos definidos pela aplicação, tira do framework a garantia de um
  vocabulário consultável por quem audita. Recomendação: genéricos.
- **D6. O tomógrafo fica como perfil de base.** Ele passa a ser o terceiro ponto de medida: a
  plataforma com zero aplicação. A troca de nome (por exemplo, `med-image-baseline`) fica a decidir
  com o autor: o nome atual é difícil de defender, mas aparece em `RESULTS.md`, no `Makefile`, no
  `med-check.py` e na proposta.
- **D7. Atualização bloqueada durante terapia.** É requisito de produto (não reiniciar no meio de
  uma infusão), e vale também para o EEG (não reiniciar no meio de um registro). Um mecanismo de
  plataforma: a aplicação declara "não interromper" pelo `MedicalUpdate`, e um *handler*
  `pre-install` do RAUC no `meta-med-distro` recusa a instalação enquanto a declaração existir.
  Toca o framework **e** o distro, por isso é decisão explícita: se entrar, entra como recurso de
  plataforma que as duas classes usam, contado à parte em M1 como "melhoria revelada".
- **D8. Classe C como hipótese declarada**, como a B do EEG em
  `implementation_plan_conformity.md` (D5 daquele plano). Ela leva mais cláusulas da IEC 62304 a se
  aplicar (projeto detalhado `[62304 §5.4?]`, critérios adicionais de unidade `[§5.5.4?]`).
- **D9. Fora do escopo clínico** (§4.1), declarado.
- **D10. A dívida `MED_EEG_LINK`** (L5) não é corrigida por este plano. Se for corrigida, o commit
  é separado e não entra na contagem da bomba.
- **D11. Comando de build.** Uma classe de dispositivo é um arquivo de projeto KAS, como o
  tomógrafo (`kas/project-tomograph.yml`, `make tomograph`). Proposta: `make infusion` e
  `make runqemu-infusion`, e o perfil `infusion` no `med-check.py`. É um alvo por classe de
  dispositivo, não por variação de uma variável. As variações (chave, link) continuam sendo
  parâmetros.

---

## 7. Fases

Cada fase tem critério, injeção de falha e onde o número é registrado.

### Fase 0 — linha de base

- Marcar o commit de base (`git tag base-infusao`). **Todas** as métricas de M1 são diferença a
  partir dele.
- Registrar o estado do EEG nesse commit: `make check` (39 asserções desde o selo; 37 quando este plano foi escrito), testes de host (`make test`) e as
  listas de pacotes dos dois perfis atuais, **do mesmo commit** (a lição de `RESULTS.md` §2,
  "Uma correção").
- Contar o vocabulário de classe no framework antes (L3, M2).
- Decidir D6.

**Critério**: tudo registrado na §13, com os comandos.

### Fase 1 — requisitos e risco

- Requisitos da bomba, derivados da IEC 60601-2-24 `[?]`, da IEC 60601-1-8 `[?]` e da orientação da
  FDA para bombas de infusão `[?]`, como RTs `INF-xx` no formato de
  `implementation_plan_conformity.md` §4.
- Análise de perigos (ISO 14971 `[?]`): sobreinfusão, subinfusão, ar, atraso de terapia, retomada
  indevida. Cada perigo com o controle de risco e o RT que o verifica.
- Justificativa da classe C (D8).

**Critério**: cada perigo tem pelo menos um controle e uma asserção prevista na Fase 7. Um perigo
sem asserção é declarado como tal.

**Feita em 2026-11-05**: `docs/REQUISITOS_INFUSAO.md`, com 16 RTs `INF-xx`, 8 perigos e 26 causas. Ela
acrescentou duas asserções à Fase 7 (`producer-lost`, `alarm-silence`) e os parâmetros do modelo
(`T_hb` = 200 ms, `k` = 3, `P_occ` = 500 mmHg, ...).

### Fase 2 — framework

Na ordem, cada uma com teste de host antes de qualquer receita:

1. `MedicalActuator` (D3), com um driver simulado mínimo para os testes.
2. O transporte para o produtor externo (D2).
3. Eventos de auditoria (D5).
4. Unidades por canal (D4), se a decisão for (b).

**Critério**:
- `make test` verde;
- `make check` do EEG continua igual ao da base (§13: 38/39, só `journal-sealed` falhando, até D11 do plano de conformidade), porque o framework mudou e o EEG não pode regredir;
- `make check-layers` sem violação;
- a busca por `infus\|pump\|syringe\|seringa\|occlu\|oclus\|vtbi\|bolus` em `meta-med-framework`
  volta vazia.

**Injeção**: o homem-morto testado no host. Com o *heartbeat* parado, o driver simulado entra no
estado seguro em até `T_hb × k`, e uma asserção que não espera isso falha.
*Revisto na execução (§13)*: o temporizador mora do lado do produtor, então esta injeção passa para
a Fase 3 (`infusion-sim`). A Fase 2 testa a metade do framework: a janela é declarada, exigida e
respeitada pelo lado do controlador.

### Fase 3 — `infusion-sim`

O modelo da §4.2 e a injeção de falhas. Testes de host do modelo: volume entregue, constante de
tempo da pressão sob oclusão, travamento detectável.
**Critério**: o volume entregue pelo modelo bate com a integral da taxa comandada dentro da
resolução de um passo. Isso testa o modelo, não uma bomba.

### Fase 4 — `infusion-control-service`

- Máquina de estados (§4.1).
- DERS a partir de uma biblioteca de fármacos **selada** na construção (o padrão do
  `do_seal_configuration` do EEG), validada por `SafetyLimit`.
- Gerenciador de alarmes, homem-morto do lado do controlador, auditoria de cada transição.
- `Type=notify` com `READY=1` só depois do autoteste do mecanismo, e participação no
  `boot-complete.target`: o mesmo padrão do EEG, para que um slot com a bomba quebrada não seja
  confirmado.
- **Sem retomada automática** (§4.3): o estado de terapia **não** é restaurado; um reinício com
  terapia em curso gera alarme.

**Critério**: os testes de host da máquina de estados cobrem toda transição, incluindo as
proibidas. Configuração com fármaco fora do envelope recusa a subir (o padrão do EEG).

### Fase 5 — `infusion-hmi-gui` e `infusion-ctl`

A HMI: programar, confirmar, iniciar, pausar e parar, apresentação de alarme por prioridade,
reconhecimento e confirmação de limite flexível. A linha de comando expõe o mesmo protocolo do
socket de controle, para a Fase 7.
**Critério**: a HMI liga só `medframework` e Qt (regra 6, checada por F5 do `make check-layers`).

### Fase 6 — imagem, KAS e `make`

`med-image-infusion.bb` (`require` da base e do perfil de desenvolvimento, com uma lista de
instalação), `kas/project-infusion-qemu.yml` e os alvos de D11. As receitas novas em
`recipes-apps` são classificadas para o F5.
**Critério**: a imagem constrói, `make check-layers-graph` sem violação, e o `.wic` é inspecionado
(GPT, slots, `/data`), não só o build verde.

### Fase 7 — suíte de aceitação, perfil `infusion`

Todas as asserções de plataforma (13 desde o selo do journal; eram 11), **sem mudar uma linha**, mais
as da bomba. Cada asserção da bomba tem de ser vista falhar com o defeito injetado.

| Asserção | O que verifica | Injeção que tem de fazê-la falhar |
|---|---|---|
| `infusion-active` | serviço ativo, `NRestarts=0`, `READY` | simulador ausente |
| `drug-library-seal` | biblioteca selada e conferida | arquivo de selo alterado |
| `ders-hard-limit` | taxa acima do limite rígido é recusada e auditada | limite removido da tabela |
| `ders-soft-limit` | acima do flexível exige confirmação; a confirmação é auditada | confirmação ignorada |
| `delivery-volume` | volume entregue (pelo *encoder* simulado) = taxa × tempo, dentro de *x*% | escala de passo errada |
| `occlusion-alarm` | oclusão injetada: alarme em até *T* s e motor parado | limiar de pressão desligado |
| `air-alarm` | ar injetado: alarme e motor parado | detector ignorado |
| `delivery-fault` | motor travado: comandado ≠ medido gera alarme | comparação desligada |
| `vtbi-complete` | ao fim do VTBI a bomba para e alarma | contagem de volume desligada |
| `deadman` | serviço de controle morto: o simulador para o motor em até `T_hb × k` | homem-morto desligado no produtor |
| `no-auto-resume` | serviço reiniciado durante infusão volta parado e em alarme | estado de terapia restaurado |
| `producer-lost` | simulador para de responder durante a infusão: alarme no controlador em até `T_prod` | supervisão do produtor desligada |
| `alarm-silence` | alarme silenciado volta depois de `T_sil` se a causa persistir | silenciamento sem prazo |
| `audit-trail` | cada transição e alarme está na trilha, e a cadeia verifica | um evento não emitido |
| `update-blocked` (se D7) | `rauc install` recusado durante a infusão | declaração não feita |

**Critério**: o perfil passa inteiro, e a tabela de injeções está registrada com a falha que cada
uma produziu.

### Fase 8 — medição (§8)

### Fase 9 — a bomba na placa · depois do QEMU

A imagem da bomba construída e executada no STM32MP257, com o simulador rodando no Linux.
**Critério**: zero linhas no `meta-med-bsp` e nas aplicações entre os dois alvos. É a portabilidade
da **segunda** classe, o que hoje só o EEG tem.

### Fase 10 — o laço do atuador no Cortex-M33 · opcional

O simulador (ou a parte de segurança do controle: homem-morto, oclusão, parada do motor) como
firmware do M33. É o argumento de particionamento da IEC 62304 `[§5.3.5?]` para a classe C, o
mesmo que o `amp` sustenta para o EEG. Custa:
- uma camada adjunta própria, porque o firmware de um mecanismo é hardware de um dispositivo;
- uma versão do formato de fio para comandos de atuador, já que o `med-amp-abi` é de quadros de
  amostra, e a mensagem de controle está cheia (8 de 8, CLAUDE.md);
- o RIF dos periféricos.

Fica declarado como trabalho futuro se não for feito.

---

## 8. As métricas

| ID | Métrica | Comando / método | Esperado | O que **não** significa |
|---|---|---|---|---|
| M1 | Linhas e arquivos alterados **por camada** | `git diff --stat base-infusao..HEAD -- meta-custom/<camada>` e `kas/` | bsp, distro, adjuntas = 0; framework pequeno; app = o resto | não mede esforço, e um framework bem desenhado pode exigir mudanças pequenas e importantes |
| M2 | Mudanças no framework, classificadas | uma linha por mudança: o que, por quê, genérica?, o EEG usa? | toda mudança genérica; vocabulário de bomba = 0 | "genérica" é argumento, não medida; a contagem de vocabulário é |
| M2b | Vocabulário de classe no framework, antes e depois | busca por termos de EEG (L3) e de bomba | EEG: *n* (registrado); bomba: 0 | não é violação da regra 9, que trata de componente |
| M3 | Pacotes dos três perfis | `diff` de `installed-package-names.txt`, **mesmo commit** | base ⊂ EEG e base ⊂ bomba; delta de cada um | não é similaridade em percentual (§2 do `RESULTS.md`) |
| M4 | Linhas de código próprias de cada classe vs. herdadas | contagem por diretório (código, sem comentário) | aplicação ≪ framework + plataforma | linhas não medem complexidade nem risco |
| M5 | Reuso de build | sumário de sstate do bitbake ao construir a bomba depois do EEG | a maioria das tarefas vem do sstate | depende do estado do cache; o método tem de fixar esse estado |
| M6 | Uso das APIs | quais `Medical*` cada aplicação chama | as duas classes usam o núcleo; cada uma ignora partes | "não usar" pode ser lacuna da aplicação, não do framework |
| M7 | Asserções de plataforma no perfil novo | `med-check.py infusion` | as 13 de plataforma, com o mesmo resultado do tomógrafo na base, sem mudar uma linha | — |
| M8 | Não regressão do EEG | `make check` e `make test` depois da Fase 2 | o resultado da base (§13) | — |
| M9 | Requisitos técnicos herdados | §9 | maioria herdada com 0 linhas | — |
| M10 | Superfície de adaptação do perfil | variáveis que o projeto KAS da bomba define | nenhuma `MED_EEG_*` | se precisar de uma, é achado (L5) |

---

## 9. Ligação com o plano de conformidade

A tabela de requisitos técnicos de `implementation_plan_conformity.md` §4 ganha uma coluna
**Bomba**, com quatro valores possíveis:

- **herdado**: atendido sem nenhuma linha específica (exemplos esperados: DAD-01, ATU-01, ATU-03,
  AUD-01);
- **herdado com configuração**: só uma declaração do perfil (exemplo: a unit da bomba no
  `boot-complete.target`);
- **exigiu trabalho**: com a linha de M1 que o pagou;
- **não se aplica**: com o motivo (exemplo: INT-04, perda de amostra, não tem o mesmo sentido para
  a bomba).

Mais os RTs `INF-xx` da Fase 1. O número que sai disso é o mais forte do trabalho sobre reuso:
"dos *N* requisitos técnicos da plataforma, a segunda classe herdou *k* sem escrever uma linha".

---

## 10. Riscos

- **R1. O framework exigir mais do que o previsto** (L1–L4). Não invalida o perfil: cada mudança
  é dado de M2. Invalida a expectativa "framework pequeno", e o texto mostra o número que saiu, não
  o esperado.
- **R2. O EEG regredir por mudança no framework.** Mitigado pelo critério da Fase 2: `make check`
  com o resultado da base (§13) antes de seguir.
- **R3. A simulação ser criticada como rasa.** Mitigado por declarar o que ela é (§1, §12) e por
  medir o software, não a física: as asserções verificam reação a falhas, não exatidão de entrega.
- **R4. A HMI sair cara.** Ela é a parte de menor valor para as métricas. A `infusion-ctl` sustenta
  a Fase 7 sem ela.
- **R5. D7 tocar o distro.** É o único caminho previsto para uma mudança abaixo do framework. Fica
  como decisão explícita e contada à parte.

---

## 11. Ordem de execução, e onde dá para parar

**0 → 1 → 2 → 3 → 4 → 6 → 7 → 8 → 5 → 9 → 10**

A HMI (5) vem depois da medição porque a suíte não depende dela.

### 11.1 A bomba vem antes do plano de conformidade (decidido em 2026-11-02)

O autor considera o problema mais grave da tese o framework não servir a outra classe de
dispositivo. Medido no mesmo dia, por linhas de código dos `.cpp` do framework (sem comentários,
divisão por arquivo, aproximada):

| Parte | Linhas | % | Serve a outra classe? |
|---|---|---|---|
| Serviços de plataforma (configuração, armazenamento, auditoria, atualização, IPC, tipos) | ~1990 | 52% | sim |
| Aquisição (`MedicalDevice.cpp`: drivers `simulated`, `rpmsg`, `iio`) | ~1390 | 36% | a sensores; não a atuadores (L1) |
| Arquivo de biossinal (`MedicalStorageBiosignal.cpp`) | ~430 | 11% | só sinal biológico (L3) |

O problema é uma **ausência** (a metade atuador, L1), não uma contaminação do framework inteiro.
Por isso a bomba vem primeiro: ela é o instrumento que mede essa ausência e conduz o desenho da
interface de atuador. Desenhar `MedicalActuator` antes, sem um atuador que o use, seria pressupor
o resultado que a Fase 2 deve medir.

**Sequência acordada**:

1. Conferir o selo do journal no QEMU (`implementation_plan_conformity.md` §5, A6). Não muda
   código e não afeta nenhuma contagem.
2. **Fase 0 deste plano, imediatamente**: o commit de base é marcado antes de qualquer mudança.
3. Fases 1 e 2: requisitos e a interface de atuador, com testes de host e o EEG no resultado da base.
4. O plano de conformidade continua **em commits separados**, cada um com a linha
   `Profile: conformity` no fim da mensagem. M1 deste plano soma só os commits **sem** essa linha:

   ```bash
   git log --numstat --format='%H %(trailers:key=Profile,valueonly)' base-infusao..HEAD -- meta-custom/<camada>
   ```

   É menos limpo que um intervalo de commits simples, e o método tem de ser declarado junto com o
   número. Um commit que misture as duas coisas invalida a separação, e por isso é proibido.

Consequência de prazo: com a bomba na frente, a defesa entre 20 e 27/11 fica improvável. A Fase 2
diz quanto custa o atuador, e é com esse número que a data se decide.

### 11.2 Onde dá para parar

Pontos de parada que deixam um resultado defensável:

| Parar depois de | O que fica |
|---|---|
| 4 | serviço e simulador com testes de host, M1/M2 parciais. Fraco: não há imagem |
| 7 | **o mínimo para a tese**: perfil construído, suíte com injeções, M1–M10 |
| 5 | o perfil apresentável na defesa, com tela |
| 9 | portabilidade da segunda classe |
| 10 | o argumento de particionamento para classe C |

---

## 12. O que o TCC pode afirmar em cada estado

| Estado | Afirmação defensável |
|---|---|
| Fase 7 + 8 | "Uma segunda classe de dispositivo, um atuador de classe C (hipótese), foi construída sobre a mesma plataforma. Ela alterou *a* linhas no framework, todas genéricas e classificadas, e zero nas camadas de SO e de placa. Herdou *k* dos *N* requisitos técnicos sem trabalho específico, e passou as 13 asserções de plataforma sem mudança." |
| Fase 9 | "...e executa nos dois alvos sem diferença de código de aplicação." |
| Fase 10 | "...com o laço de segurança do atuador no coprocessador de tempo real." |

**Em nenhum estado**: "a bomba atende à IEC 60601-2-24", "a exatidão de entrega é *x*%" sem dizer
"do modelo simulado", ou "o framework é independente de classe de dispositivo" (L3 mostra que
não é, e a medida honesta é quanto).

---

## 13. Registro de execução

### Fase 0 — linha de base (2026-11-05)

**Commit de base**: `git tag -a base-infusao` em `b1344a7` (o selo do journal, último commit antes
da bomba). Todas as medições abaixo são desse commit; as duas imagens foram construídas a partir
dele no mesmo dia (`make qemu`: 6580 tarefas, nenhuma refeita; `make tomograph`: 5639, 61 refeitas).

**Testes de host** (`make test`, 0 falhas em todas):

| Suíte | Verificações |
|---|---|
| `tests/framework` | 1727 |
| `tests/mcp2210` | 452 |
| `tests/ads1299` (driver da placa / branch do PR) | 1875 / 590 |
| `tests/m33-producer` | 322 |
| `tests/eeg-export` | 117 |
| `tests/hmi` | 60 |
| `tests/amp-bench` | 13 |
| `tests/derive` | 1466 |
| `tests/layer-check` | 26 injeções; `make check-layers` F1–F9 ok |

`tests/ads1299` relata "1 defeito confirmado, 0 resolvido", anterior a este plano.

**Suíte de execução** (`make check`, QEMU), 13 asserções de plataforma + 26 do EEG:

| Perfil | Resultado | Observação |
|---|---|---|
| EEG (`make check`) | **38/39** | só `journal-sealed`: 6 de 10 arquivos selados não verificam. Falso positivo do systemd 255 (`implementation_plan_conformity.md` §5 A8), e o número cresce cerca de 2 por boot |
| tomógrafo (`med-check.py tomograph`) | **13/13** | imagem nova, então era o boot de provisionamento do selo, o único em que o A8 ainda não aparece |

Critério de não regressão daqui em diante (M8): o EEG com 38/39 e `journal-sealed` como única
falha; o tomógrafo e a bomba com as 13 de plataforma, e o selo julgado como no EEG, até D11.

**Pacotes** (`installed-package-names.txt` do `buildhistory`, método de `RESULTS.md` §2):

| | Pacotes | `IMAGESIZE` |
|---|---|---|
| `med-image-eeg` | 239 | 311.492 KB |
| `med-image-tomograph` | 207 | 275.772 KB |
| em comum | 204 | — |
| só no EEG | 35 | — |
| só no tomógrafo | 3 (`weston`, `weston-init`, `libweston-13-0`) | — |

O tomógrafo **deixou de ser subconjunto** do EEG. Desde a estação de bancada, o EEG em QEMU usa
`MED_GUI_COMPOSITOR = "med-desktop-session"` (`kas/project-eeg-qemu.yml`), e os 35 pacotes só do
EEG são, em maioria, o desktop (`labwc`, `wlroots`, `sfwbar`, `pcmanfm-qt`, GTK) e o revisor
(`edfbrowser`, `med-review`). As aplicações do EEG são `eeg-acquisition-service` e `eeg-hmi-gui`,
mais `packagegroup-med-amp`. A bomba tem de ser medida contra um perfil com o **mesmo**
compositor, ou o delta de pacotes mede a escolha de tela e não a classe: decisão para a Fase 5.

**Vocabulário de classe no framework** (M2b, L3), em `meta-custom/meta-med-framework` (17 arquivos
`.h`/`.cpp`), sem distinção de maiúsculas:

```bash
P='lead.?off|electrode|biosignal|\bBDF|\bEEG|montage|biopotential'
grep -rhEi "$P" meta-custom/meta-med-framework --include=*.h --include=*.cpp | wc -l
```

| | Linhas | em código | em comentário | Arquivos |
|---|---|---|---|---|
| EEG / biopotencial | 136 | 96 | 40 | 7 |
| bomba (`infus`, `syringe`, `plunger`, `occlu`, `bolus`, `pump`) | 0 | 0 | 0 | 0 |

Por arquivo, para o EEG: `MedicalDevice.cpp` 49, `MedicalStorageBiosignal.cpp` 40,
`MedicalStorage.h` 20, `med_amp_abi.h` 15, `MedicalDevice.h` 9, `MedicalLogger.h` 2,
`MedicalLogger.cpp` 1. Os identificadores mais frequentes são `BiosignalWriter` (23), `leadOff`
(17), `BDF` (17) e `med_amp_lead_off` (10). Duas menções em comentário que não são vocabulário de
bomba, mas importam para ela: o DERS ("dose error reduction", `MedicalConfiguration.h:14`), que é
um conceito nascido nas bombas de infusão, e o cabeçalho de `MedicalDevice.h`, que se declara
"sensor and actuator abstraction" e não tem atuador (L1).

**D6**: o tomógrafo fica como perfil de base, como proposto. A troca de nome continua aberta com
o autor.

**Achado lateral**: o `RESULTS.md` §2 dizia que o tomógrafo passa "11/11 [...] journal persistente
e selado" e que é subconjunto estrito do EEG. As duas coisas estavam desatualizadas e foram
corrigidas lá, com estes números.

### Fase 1 — requisitos e perigos (2026-11-05)

`docs/REQUISITOS_INFUSAO.md`, commit `fdd7081`. Registro no próprio documento e na Fase 1 da §7.

### Fase 2 — framework (2026-11-05)

Quatro commits, um por decisão, na ordem D4 → D2 → D3 → D5, cada um com os testes de host e as
injeções antes de entrar. Todos sem a linha `Profile: conformity`.

**M2, as mudanças classificadas**:

| Commit | Decisão | O que | Genérica? | O EEG usa? | Linhas (framework) |
|---|---|---|---|---|---|
| `7ea7f2a` | D4 (L2) | unidade por canal (`DeviceConfig`/`DeviceInfo::channelUnits`, `unitOf`); `encodeFrame` com escala; o decodificador recusa escala não positiva | sim | não (recusa de escala: sim, no caminho de decodificação) | +93 −7 |
| `df8813a` | D2 | `RpmsgDevice` vira `LinkDevice` com o transporte como parâmetro; driver `seqpacket` | sim, é um transporte | mantém `rpmsg`, e ganha teste de host dele | +50 −12 |
| `d5a88e5` | D3 (L1) | `MedicalActuator.h`/`.cpp`: comando, sinal de vida, estado seguro, janela do homem-morto exigida, troca sem resposta encerra a sessão; driver `seqpacket` | sim | não | +476 −0 |
| `7528cd4` | D5 (L4) | seis `AuditEvent` ao fim do enum: `AlarmRaised`, `AlarmAcknowledged`, `TherapyStarted`, `TherapyStopped`, `SafetyLimitOverridden`, `ActuatorFault` | sim | não | +32 −0 |

**M1 parcial** (§11.1, `git log --numstat` sem os commits `Profile: conformity`):

| Camada | Linhas | Arquivos |
|---|---|---|
| `meta-med-bsp` | 0 | 0 |
| `meta-med-distro` | 0 | 0 |
| `meta-med-framework` | +651 −19 | 7 |
| `meta-med-app` | 0 | 0 |
| adjuntas (`afe-ads1299`, `desktop`, `host`) | 0 | 0 |
| `kas/` | 0 | 0 |

Das 651 linhas acrescentadas ao framework, **391 são código**, 189 são comentário e 71 são
linhas em branco. A §11.1 contou ~3800 linhas de código nos `.cpp` do framework, sem comentário.
As 391 linhas novas incluem `.h`, então a comparação é aproximada: algo como **10%** a mais. O `med-amp-abi` não mudou: a escala por quadro já existia no formato, e
só o codificador a fixava em 1 (achado da D4, que tornou desnecessária a versão nova do ABI que a
opção (b) previa).

**M2b depois**: vocabulário de bomba em `meta-med-framework` = 0. A primeira busca pegou o nome
do arquivo deste plano em dois comentários, que foram reescritos. Vocabulário de EEG: inalterado.

**Não regressão do EEG (M8)**: `make qemu` (6580 tarefas, 70 refeitas) e `make check`
**39/39**. A imagem era nova, então esse foi o boot de provisionamento do selo, em que
`journal-sealed` passa. Isso cumpre o critério da base: as outras 38 passam, e `journal-sealed` é a
única que pode falhar, pelo falso positivo A8. O EEG usa o `rpmsg`, e no QEMU o `simulated`, sobre
um `MedicalDevice.cpp` reescrito, e a aquisição (`acq-sample-rate` e as demais) passou inalterada.
`make check-layers` sem violação.

**Testes de host**: `tests/framework` passou de 1727 para **1890** verificações, 0 falhas. As
outras suítes ficaram iguais à base. Injeções feitas e vistas:

| Mudança | Injeção | Falhas |
|---|---|---|
| D4 | decodificador aceita escala ≤ 0 | 2, só as do teste de escala |
| D4 | fábrica aceita lista de unidades de outro tamanho | 3, só as do teste de unidades |
| D2 | acúmulo antes da resposta não é descartado | 3, só o teste de acúmulo |
| D2 | taxa do produtor não conferida | 2, só o teste de taxa |
| D3 | timeout não encerra a sessão | 6, só o teste de troca sem resposta |
| D3 | `start()` sem a janela do homem-morto | 5, nos testes que conferem a janela |

**Desvios do plano, decididos na execução**:
- **D3, `readback()`**: não está na interface. O que o mecanismo faz de fato é uma medida, e chega
  por um `MedicalDevice` no mesmo produtor. Um atuador que relatasse o próprio resultado estaria
  dando nota a si mesmo.
- **D3, driver simulado no framework**: não foi feito. Os testes usam um produtor falso num soquete
  real (`tests/framework/fake_producer.h`), e o homem-morto, que mora do lado do produtor, é
  testado na Fase 3.
- **D3, `rpmsg` para o atuador**: não foi registrado. Fica para a Fase 10, porque um driver que
  nada exercita não é registrado antes.

**Achados**:
- O caminho de execução do driver `rpmsg` (janela de conexão, espera da resposta de controle,
  descarte do acúmulo, conferência de taxa) **nunca tinha rodado fora da placa**. Ele agora tem 62
  verificações de host pelo `seqpacket`, que é o mesmo código. Só o transporte rpmsg continua
  restrito à placa.
- **Acrescentar um campo a `DeviceConfig` é mudança de ABI**, e ela foi vista: objetos de teste
  compilados com o cabeçalho velho davam falha de segmentação. O Makefile dos testes passou a
  rastrear dependência de cabeçalho. O `SOVERSION` não foi alterado, como em toda mudança de
  layout desde a 1.0.0, porque biblioteca e aplicações saem na mesma imagem. Isso é dívida
  declarada, não decisão nova.
- **A suíte de host não testa o `MedicalLogger` neste host**: falta `libsystemd-dev`. Isso já valia
  na base, e a própria suíte avisa. A D5 foi compilada pelo bitbake (`make framework`, zero
  avisos), mas o teste dela não rodou.
- A regra F8 do `make check-layers` recusou o driver novo até ele ser declarado como transporte.
  É a regra funcionando: um commit saiu antes da correção e foi emendado.

---

## 14. Backlog: o framework agnóstico ao formato (decidido em 2026-11-05, não executado)

Levantado pelo autor ao discutir uma classe de imagem. **Princípio**: o framework transporta,
guarda, confere e audita dados sem saber o que eles são, e quem produz e quem consome declara o
formato quando o dado passa. É o padrão que o framework já usa para drivers (fábrica por nome),
opções de front-end e setpoints de atuador, e que ainda não usa em dois lugares:

1. **Quadro de forma fixa.** `SampleFrame` é sempre canais × amostras em `double`, e o fio é `int32`
   mais um bloco de lead-off. O destino é um quadro com **descritor** (tipo do elemento,
   dimensões, unidade por eixo ou por canal) mais os bytes. O framework confere só o que pode
   conferir sem entender: tamanho = dimensões × tipo, CRC, sequência e tempo. Um descritor que não
   fecha com os bytes é recusado.
2. **Formato de arquivo dentro do framework.** O `BiosignalWriter` (BDF+) é código do framework. O
   destino são **gravadores de formato registrados por nome** no `MedicalStorage`, com o BDF+ no
   perfil do EEG e um DICOM, se houver, no de imagem.

O que isso resolveria: o vocabulário de classe medido na Fase 0 (L3, M2b: 96 linhas de código),
porque o `BiosignalWriter` sai e o `LeadOff` vira uma máscara genérica de **estado por canal**, que
serve ao eletrodo do EEG e ao detector de ar da bomba. Resolveria também a classe de imagem
(câmera por V4L2, `vivid` no QEMU) sem interface nova além de um transporte e um gravador.

O custo: uma versão nova do `med-amp-abi`, que o firmware do M33 tem de aprender, e o EEG,
validado na placa, muda de caminho. Por isso fica **depois** desta bomba: ela é medida sobre o
framework de hoje, e a mudança, se feita, entra como fase própria com o seu número (quantas linhas
de EEG saíram do framework).

**Por que o tomógrafo não virou um equipamento** (registrado na mesma conversa): a aquisição em
blocos e o formato de arquivo seriam resolvidos por este item. A reconstrução precisaria de
computação acelerada, que o framework não abstrai e que a regra 6 impede a aplicação de trazer.
E boa parte da dificuldade (física, reconstrução, intertravamentos de radiação) está fora da
plataforma. Ele continua como o perfil de base, sem aplicação (D6).
