# Plano de implementação — controle do EEG em userspace

> **Status**: **nada aqui foi implementado.** Não existe socket de controle, não existe verbo, não
> existe máquina de estados, e a HMI não tem um único botão — `EegClient` expõe propriedades e
> `waveform()`, e nada mais. O que existe é o diagnóstico da §2, e ele não é uma lacuna de recurso:
> é uma confusão de conceitos que já produziu defeito medido.
>
> **Escopo**: quem manda no front-end, por que caminho os outros pedem, e o que o registro passa a
> saber sobre quem pediu o quê. Da prontidão declarada ao systemd até a suíte de aceitação abrindo e
> fechando uma sessão sozinha.
>
> **Não é** um plano de interface de usuário. A HMI ganha o mínimo para *pedir*; como uma tela
> clínica deve se parecer é outra discussão, e ela não bloqueia nada aqui.
>
> **Por que agora**: três coisas já dependem disto. A Fase 4 do `implementation_plan_m33_firmware.md`
> declara explicitamente que a sua injeção de falha **não pode ser executada** enquanto a unidade não
> souber falhar. A `RESULTS.md` §10 registra que o alvo físico está no regime *"verificado uma vez"* e
> não *"verificável"*, e a razão é que a suíte não tem como comandar nada por serial. E o `BIAS_STAT`
> do `ti-ads1299.c` não tem para onde ser reportado.
>
> **Leitura obrigatória antes**: `BRINGUP_STM32MP2.md` §9.9 (os 163 reinícios e por que nada os
> parou), `RESULTS.md` §3 (a contrapressão de publicação, que é o precedente de "um consumidor não
> comanda o produtor"), e o `eeg-acquisition.service` inteiro, que é onde a partição está escrita.

---

## 1. Objetivo

Que **o serviço de aquisição seja o único dono do front-end**, e que todo o resto — a HMI, a linha de
comando, a suíte de aceitação — obtenha o que precisa pedindo a ele, por um caminho autenticado e
auditado. Concretamente, ao fim deste plano:

- uma sessão clínica tem começo, fim, operador, exame e anotações, e **nenhum deles é o PID**;
- o serviço pode estar de pé e ocioso, e `systemctl` diz qual dos dois;
- uma unidade que não consegue subir **chega a `failed`** e aparece em `--state=failed`;
- uma prescrição que o front-end recusa derruba a *sessão*, não o serviço;
- toda requisição de controle, aceita ou recusada, tem registro com o uid de quem pediu;
- e a suíte de aceitação abre e fecha uma sessão sem um ser humano no console.

### O que este plano **não** faz

- **Não** deixa a HMI programar o conversor. Nunca. Ela pede; o serviço decide. Se essa linha cair, a
  HMI passa a ser item de software crítico e a partição que o trabalho inteiro defende deixa de
  existir.
- **Não** permite trocar ganho ou taxa de amostragem no meio de uma sessão — ver §6, os não-verbos.
- **Não** resolve o relógio. Um `sessionId` derivado de um relógio sem RTC continua errado; este
  plano dá à sessão um **identificador de exame** que não depende de tempo, o que reduz o dano e não
  o conserta (`RESULTS.md` §9).
- **Não** autentica pessoas. Ver §7.3, que é sobre a diferença entre identidade verificada e
  identidade afirmada.

---

## 2. O defeito conceitual: uma sessão não é a vida de um processo

Hoje as duas coisas são literalmente a mesma. O `main.cpp` abre a sessão no `exec`
(`session-<timestamp>`, um diretório por partida) e a fecha no `SIGTERM`. A consequência não é
estética; ela tem sintomas, e alguns já foram medidos:

| Sintoma | Estado |
|---|---|
| Um reinício do serviço cria uma sessão nova, sem ninguém pedir | por construção |
| O `metadata.json` não tem paciente, exame nem operador — não há campo | por construção |
| Nada encerra um registro a não ser parar a unidade | por construção |
| Não existe marcador de evento; um artefato observado não fica no registro | por construção |
| Um front-end que falha mata o **serviço**, porque `device.start()` está no caminho de partida | **medido**: 163 reinícios, 27/08 |
| `systemctl is-active` responde `active` para um serviço em *crash loop* | **medido**, e é a origem da regra do `acq-active` |
| A HMI não tem como pedir nada | **verificado**: `EegClient.h` só tem propriedades e `waveform()` |

O que uma sessão clínica tem e um processo não: um operador, um paciente, um começo e um fim
decididos por alguém, e a possibilidade de ser anotada. O que um processo tem e uma sessão não: um
PID, um `NRestarts` e uma política de reinício. Separar os dois é o plano inteiro.

---

## 3. Quem controla, e por que só ele

| Componente | Pode | Não pode |
|---|---|---|
| `eeg-acquisition-service` | abrir o front-end, programá-lo, gravar, decidir, auditar | — |
| `eeg-hmi-gui` | pedir sessão, anotar, consultar estado, exibir | tocar no conversor, escrever registro, ver o `/data` |
| `medctl` (novo) | os mesmos pedidos, sem GUI | idem |
| `med-check.py` | os mesmos pedidos, por console serial | idem |

O argumento é a IEC 62304 §5.3 e é o mesmo que já está escrito no sandbox da HMI: *"the HMI has no
business writing records, reaching the network or touching the front-end, and IEC 62304 software
partitioning is only credible if the partition is actually enforced"*. Um caminho em que a HMI
programasse o conversor promoveria a HMI a item crítico — e a HMI é 172 linhas de QML sobre Qt, que é
precisamente o software que não se quer na classe de segurança mais alta.

Há precedente de projeto para isso neste repositório, e é o da `RESULTS.md` §3: **um consumidor não
comanda o produtor.** Lá a lição custou 62% das amostras, porque um visualizador lento estava, de
fato, ditando a taxa de aquisição. A regra saiu dali: *um visualizador pode perder dados; o registro
não.* O controle é a mesma regra na direção inversa — o visualizador pode **pedir**, e o produtor
decide se atende.

---

## 4. A máquina de estados

```
                   ┌──────────────────────────────────────┐
                   │                                      │
   starting ──► ready ──► acquiring ──► stopping ──────────┘
      │           ▲                         
      │           └──── recusa de sessão ◄── (prescrição não honrada)
      ▼
   failed  (alto, visível, e com limite de reinício)
```

| Estado | O que já foi provado ao entrar nele | `sd_notify` |
|---|---|---|
| `starting` | nada | — |
| `ready` | config verificada e selada, envelope de segurança aprovado, front-end **existe e passou no autoteste**, `/data` montada e cifrada, os dois sockets publicados, slot A/B confirmado | `READY=1`, `STATUS="ready, no session"` |
| `acquiring` | front-end programado e **confirmou** a prescrição; `metadata.json` escrito | `STATUS="acquiring <exame>"` |
| `stopping` | — | `STATUS="closing <exame>"` |
| `failed` | — | `STATUS=<causa>`, e a unidade **chega** a `failed` |

Três decisões de projeto estão embutidas aí, e cada uma corrige um comportamento de hoje:

**1. `ready` prova o front-end sem adquirir.** `selfTest()` na ligação `amp` é literalmente abrir o
canal e fechá-lo se não estava aberto — então `ready` continua significando "o produtor existe". O
que sai do caminho de partida é `device.start()` e a sessão, não a verificação.

**2. Uma prescrição recusada derrota a sessão, não o serviço.** Hoje `sendControl()` falhando dentro
de `start()` faz o serviço sair com 1. Depois disto, `session.start` responde a recusa a quem pediu,
com o índice e o motivo que o `ControlAck` traz, e o serviço volta para `ready`. É a diferença entre
um aparelho que diz "esse ganho não pode" e um aparelho que reinicia.

**3. Prontidão é declarada, não presumida.** `Type=notify` com `sd_notify` — e **não é dependência
nova**: `sd_notify` mora na `libsystemd`, que o framework já linka. A regra 6 continua intacta
porque a chamada fica atrás de uma API do framework e a aplicação não inclui
`<systemd/sd-daemon.h>`.

### 4.1 O laço muda de âncora conforme o estado

Detalhe de implementação que precisa estar escrito, porque a versão ingênua queima uma CPU: hoje o
laço bloqueia em `device.read(500ms)`, que é o que o mantém em ritmo. Em `ready` **não há leitura de
dispositivo**, então o elemento que espera passa a ser o socket de controle, com timeout. Um laço que
só faça `accept(0ms)` em `ready` gira a 100%.

| Estado | O que o laço espera |
|---|---|
| `ready` | `receive` no canal de controle, timeout de ~500 ms |
| `acquiring` | `device.read(500ms)`, com `accept(0ms)` e `receive(0ms)` de controle antes |

E permanece **single-threaded**. Nada de thread de controle: o laço é `SCHED_RR` prioridade 50, e
uma segunda thread traria lock, inversão de prioridade e uma classe de defeito que o código hoje não
tem.

---

## 5. O transporte

### 5.1 Um segundo socket, separado do de amostras

`/run/medplatform/eeg-control.sock`, seqpacket, servido pelo `MedicalIpcServer` que já existe.

Separado do `/run/medplatform/eeg.sock` por três razões: as permissões são diferentes (assistir não é
comandar, e isso tem de ser expressável no filesystem); as semânticas são diferentes (o de amostras é
mangueira, não-bloqueante, e perder mensagem nele é aceitável — perder um `session.stop` não é); e o
de amostras tem N clientes enquanto o de controle tem um comando por vez.

### 5.2 A mensagem já existe

`amp::ControlMessage` / `ControlAck`: layout fixo, empacotado, `static_assert`, CRC, **resposta
obrigatória**, e a regra de que *uma opção que o outro lado não entende é uma recusa, nunca um
descarte silencioso*. É exatamente a disciplina que um comando clínico precisa, e ela já está escrita
e já tem teste de host.

Usar o mesmo tipo para HMI→serviço e para serviço→firmware não é economia de código: é a mesma
garantia nos dois saltos. O verbo vai na chave, os parâmetros nas chaves seguintes, e as 8 opções de
`kControlMaxOptions` cobrem folgadamente o vocabulário da §6.

### 5.3 As alternativas, e por que não

| Alternativa | Por que não |
|---|---|
| **D-Bus (sd-bus)** | não é questão de dependência — o `MedicalUpdate` já é cliente de sd-bus, então sairia de graça. É que o ABI já existe para isto, e um *handler* de método dentro de um laço de tempo real é superfície que não precisa existir. Se algum dia um terceiro precisar de introspecção, a decisão se reabre |
| **Só `systemctl start/stop`** | obrigaria alguém a escrever a identidade do exame num arquivo *antes* de subir a unidade, num rootfs somente-leitura, e esse alguém precisaria de autoridade. E transformaria "anotar um evento" em reinício de serviço |
| **Sinais (`SIGUSR1`)** | não carregam parâmetro, não têm resposta, e não têm como ser recusados com motivo |
| **Um arquivo em `/run` + `inotify`** | sem autenticação de par, sem confirmação, e com a corrida de escrita parcial de graça |

---

## 6. Os verbos

| Verbo | Parâmetros | Resposta | Auditoria |
|---|---|---|---|
| `session.start` | `operator`, `exam`, opcional `subject` | id da sessão, ou recusa com motivo | `AcquisitionStarted` + uid do par |
| `session.stop` | — | contadores da sessão | `AcquisitionStopped` com `frames`, `dropped`, `viewer missed` (que o laço já mantém) |
| `session.annotate` | `label`, opcional `detail` | aceito/recusado | `PatientDataWritten` |
| `device.status` | — | estado, lead-off, taxa entregue vs. configurada, `NRestarts` do produtor | nada |

Nenhum `AuditEvent` novo é necessário: os 17 do enum cobrem tudo, e uma requisição **recusada** é
`SecurityEvent`, que é o que ela é.

### Os não-verbos, e são decisões

- **Trocar ganho, taxa ou canais no meio de uma sessão.** O `metadata.json` declara *uma* taxa e *um*
  ganho. Mudá-los durante o registro produz um arquivo cujo próprio metadado é falso na segunda
  metade — o mesmo defeito de rastreabilidade que o envelope de segurança já evita ao tratar a taxa
  como conjunto discreto. Quem quer outro ganho abre outra sessão, e o registro diz que são duas.
- **Apagar ou editar um registro.** O `MedicalStorage` tem `remove()`, e o controle não o expõe.
- **Ligar o *bias drive* em runtime.** É prescrição de build (`afe.bias_drive`), pelas razões do
  `do_derive_device_options` — e é o único parâmetro do bloco que injeta corrente no paciente.

---

## 7. Autorização

### 7.1 O estado de hoje, que precisa ser dito primeiro

**Nenhuma das duas units declara `User=`.** Verificado: as duas rodam como root. Logo, credencial de
par como fronteira de autorização seria **decorativa** — e o comentário da unit da HMI que afirma que
a partição *"is actually enforced"* é verdadeiro para filesystem e syscalls e **falso para
identidade**. Corrigir isso é parte deste plano e não um refinamento posterior.

### 7.2 O desenho

| Peça | Escolha |
|---|---|
| Grupo | `medop`, **gid fixo** (`useradd-staticids`) |
| Socket de controle | `root:medop`, `0660` |
| Socket de amostras | continua como está — assistir é menos que comandar |
| HMI | `User=med-hmi`, `SupplementaryGroups=medop` |
| Verificação | `SO_PEERCRED` no `accept`, que o `MedicalIPC` **não tem** hoje |
| Recusa | resposta com motivo **e** registro `SecurityEvent` com uid, gid e pid do par |

O gid fixo não é preciosismo de reprodutibilidade: um gid que mude entre builds muda entre **slots
A/B**, e então a HMI do slot novo não conversa com o serviço do slot novo depois de uma atualização
bem-sucedida. É a mesma família de armadilha que a chave do `/data` no rootfs
(`implementation_plan_luks.md`), aplicada a permissões.

### 7.3 Identidade verificada e identidade afirmada

**Este aparelho não tem login.** O `operator` que a HMI enviar é uma *afirmação*; o uid que pediu é
*verificável*. Os dois vão para o registro, com nomes diferentes e rotulados como o que são.

Isso é uma limitação e vai declarada como tal, na `RESULTS.md` §9 e no texto. O que **não** se pode
fazer é escrever o operador afirmado no `metadata.json` sem qualificação, porque aí o registro passa
a afirmar uma autoria que ninguém verificou — que é pior que não ter o campo.

---

## 8. Mudanças, arquivo a arquivo

| Onde | O quê |
|---|---|
| `MedicalIPC.h/.cpp` | `SO_PEERCRED` no `accept`, expostas como `PeerCredentials`; grupo do socket |
| framework, novo | `MedicalService` (ou equivalente): `notifyReady()`, `notifyStatus()`, atrás de API — a aplicação não inclui `sd-daemon.h` |
| `MedicalDevice.h` | os verbos como chaves do vocabulário de controle; o ABI já é extensível por chave |
| `main.cpp` | a máquina de estados; sessão e `device.start()` saem do caminho de partida; o laço muda de âncora (§4.1) |
| `eeg-acquisition.service` | `Type=notify`, `StartLimitBurst`, `StartLimitIntervalSec` |
| `eeg-hmi.service` | `User=med-hmi`, `Group=`, `SupplementaryGroups=medop` |
| `eeg-hmi-gui` | `Q_INVOKABLE` que **pedem**; nada que programe |
| `medctl` (novo, `meta-med-app`) | os verbos por linha de comando |
| `med-check.py` | asserções que abrem, anotam e fecham uma sessão |
| recipes | o grupo `medop` com gid estático |

---

## 9. Fases

### Fase 1 — a unidade aprende a falhar

**Objetivo**: `failed` significa `failed`.
**Pré-requisito**: nenhum.
**Procedimento**: `Type=notify` na unit, `notifyReady()` chamado no ponto em que hoje começa o laço,
`StartLimitBurst`/`StartLimitIntervalSec` coerentes com a janela de reconexão de 5 s do `RpmsgDevice`.

**Critérios**:
1. `systemctl is-active` **não** responde `active` durante uma sequência de falhas;
2. após `StartLimitBurst` tentativas a unidade está em `failed` e aparece em
   `systemctl list-units --state=failed`;
3. `systemctl status` mostra a causa em `STATUS=`.

**Injeção de falha**: apontar `MED_EEG_DRIVER` para um driver inexistente. Hoje isso produz um laço
invisível de 2,5 s (medido: 163 reinícios, `--state=failed` vazio). Depois desta fase tem de produzir
uma unidade falha em menos de um minuto.
**O que uma reprovação significa**: a janela do limitador está mal dimensionada contra o tempo de
vida de cada tentativa — que é exatamente o erro de aritmética que deixou os 163 reinícios passarem.
**Registro**: `RESULTS.md` §3; e a Fase 4 do `implementation_plan_m33_firmware.md` fica desbloqueada.

### Fase 2 — a máquina de estados, ainda sem socket de controle

**Objetivo**: o serviço fica de pé e ocioso, e a sessão deixa de nascer do `exec`.
**Pré-requisito**: Fase 1.
**Procedimento**: implementar os estados; `session.start` ainda não existe, então a sessão é iniciada
por uma variável de ambiente ou pelo `medctl` mínimo — o que permite validar os estados antes do
transporte.

**Critérios**:
1. em `ready` por 60 s, o consumo de CPU do serviço é **≤ 1%** (é o critério que pega o laço girando,
   §4.1);
2. `systemctl status` distingue `ready` de `acquiring`;
3. um `ControlAck` de recusa deixa o serviço em `ready` e **não** o mata;
4. um autoteste que falha continua sendo fatal — `starting` é o único estado em que o front-end
   ausente derruba o processo.

**Injeção de falha**: fazer o produtor recusar uma opção (na ligação `simulated`, uma chave inválida
já basta). O critério 3 tem de ser observável; se o serviço morrer, a separação não foi feita.
**Registro**: `RESULTS.md` §3.

### Fase 3 — credenciais de par

**Objetivo**: quem pede é identificável, e quem não pode é recusado.
**Pré-requisito**: Fase 2.
**Procedimento**: `SO_PEERCRED` no `MedicalIPC`; grupo `medop` com gid estático; `User=` na HMI.

**Critérios**:
1. um par fora do grupo recebe recusa e **não** é atendido;
2. a recusa gera `SecurityEvent` com uid, gid e pid;
3. a HMI, como `med-hmi` em `medop`, é atendida;
4. o gid é o mesmo em dois builds independentes da mesma imagem.

**Injeção de falha**: conectar como um usuário sem o grupo (`su -s /bin/sh nobody`). Tem de ser
recusado **e** auditado — as duas coisas, porque uma recusa sem registro é uma tentativa que ninguém
saberá que houve.
**Registro**: `RESULTS.md` §4 (particionamento).

### Fase 4 — o socket de controle e os verbos

**Objetivo**: o vocabulário da §6, funcionando.
**Pré-requisito**: Fase 3.

**Critérios**:
1. `session.start` cria o diretório da sessão com `exam` e `operator` no `metadata.json`, este último
   rotulado como afirmado, ao lado do uid verificado;
2. `session.stop` fecha o registro e devolve `frames`/`dropped`/`viewer missed`;
3. `session.annotate` deixa a anotação **dentro** do registro, com carimbo;
4. um segundo `session.start` com sessão aberta é **recusado**, não enfileirado;
5. um verbo desconhecido é recusado nomeando-o, nunca ignorado.

**Injeção de falha**: cortar o cliente no meio de uma sessão (matar o `medctl`). A sessão tem de
continuar — o controle é episódico, não uma correia — e um `session.stop` posterior, de outro cliente
autorizado, tem de fechá-la.
**Registro**: `RESULTS.md` §3 e §5.

### Fase 5 — `medctl`

**Objetivo**: o caminho de controle exercitável sem GUI.
**Pré-requisito**: Fase 4.
**Critério**: os quatro verbos por linha de comando, com código de saída não-zero em recusa.
**Injeção de falha**: `medctl session.stop` sem sessão aberta → saída não-zero e mensagem.

### Fase 6 — a HMI pede

**Objetivo**: botões, e a partição de identidade fechada.
**Pré-requisito**: Fases 4 e 5.
**Critérios**:
1. iniciar e encerrar sessão pela tela;
2. `ps` mostra a HMI como `med-hmi`, não root;
3. a HMI **não** consegue abrir `/data` nem o nó do front-end — verificado tentando.

**Injeção de falha**: tirar a HMI do grupo `medop` e confirmar que ela exibe amostras e **não**
consegue comandar. É a prova de que assistir e comandar são privilégios distintos e não a mesma coisa
com dois nomes.

### Fase 7 — a suíte alcança a sessão

**Objetivo**: transformar "verificado uma vez" em "verificável".
**Pré-requisito**: Fase 5.
**Critério**: `make check` ganha asserções que abrem uma sessão, anotam, fecham, e conferem o
`metadata.json` e os contadores — e falham se qualquer um discordar.
**Injeção de falha**: cada asserção nova reverte-se contra um defeito plantado, pela regra da casa.
**Registro**: `RESULTS.md` §10, que hoje diz que a suíte não alcança a placa.

---

## 10. Ordem, e o que bloqueia o quê

```
Fase 1 ──► Fase 2 ──► Fase 3 ──► Fase 4 ──► Fase 5 ──► Fase 6
   │                                            └──────► Fase 7
   └──► desbloqueia a Fase 4 do plano do M33
```

A **Fase 1 é a mais barata e a que mais desbloqueia**: é uma diretiva de unit, uma chamada de
`sd_notify` e dois limites. Ela não depende de nada, vale por si, e é pré-requisito de uma injeção de
falha que outro plano já declarou como pendente.

---

## 11. Riscos e decisões em aberto

| Item | Por que é decisão e não implementação |
|---|---|
| **Uma atualização pode interromper uma sessão?** | O `MedicalUpdate` é cliente do RAUC e hoje ninguém pergunta. Com estado explícito o serviço pode recusar ou postergar — é a diferença entre perder um exame e adiar um update. **Decisão de projeto**, e ela tem lado regulatório |
| **Uma sessão sem `session.stop` é registro válido?** | Queda de energia deixa um `raw.bin` sem fechamento. Append-only significa que os dados estão lá; o que falta é a declaração de fim. Um leitor tem de saber distinguir, e alguém tem de dizer se isso é registro parcial ou registro inválido |
| **Vocabulário de anotação**: texto livre ou lista controlada | Texto livre é mais útil na bancada e pior para análise posterior |
| **`ready` mantém o dispositivo aberto?** | Na ligação `usb` manter aberto impede outro processo; fechar reintroduz a corrida de enumeração a cada sessão |
| Identidade de operador sem login | §7.3. Limitação declarada, não resolvida |
| Um par autorizado que trava com sessão aberta | O controle é episódico por projeto (Fase 4, injeção) — mas um exame aberto e esquecido é problema operacional real |

---

## 12. Consequências regulatórias

- **IEC 62304 §5.3**: a partição deixa de ser só filesystem e syscalls e passa a incluir
  **identidade** — a HMI não é root, não está no grupo que comanda o que não deve, e isso é
  verificável na placa.
- **Rastreabilidade**: toda requisição, aceita ou recusada, tem registro com o par que a fez. Hoje o
  registro não sabe que alguém pediu algo, porque não há como pedir.
- **Proveniência do registro**: o `metadata.json` passa a carregar exame, operador afirmado e uid
  verificado, com os três rotulados. Um campo qualificado é evidência; um campo ambíguo é passivo.
- **Verificação (§5.7)**: a Fase 7 é o que torna o comportamento do alvo físico reexecutável por
  outra pessoa, que é o que uma atividade de verificação significa.

---

## 13. Validação

*(Vazio de propósito. Uma fase sem número medido e data é código escrito, não resultado.)*

| Fase | Critério | Resultado | Data |
|---|---|---|---|
| 1 | unidade chega a `failed` e aparece em `--state=failed` | — | — |
| 2 | CPU ≤ 1% em `ready` por 60 s; recusa não mata o serviço | — | — |
| 3 | par fora do grupo recusado **e** auditado | — | — |
| 4 | sessão com exame no metadado; segundo `start` recusado | — | — |
| 5 | quatro verbos por CLI, saída não-zero em recusa | — | — |
| 6 | HMI como `med-hmi`, sem `/data`, sem front-end | — | — |
| 7 | `make check` abre, anota e fecha uma sessão | — | — |
