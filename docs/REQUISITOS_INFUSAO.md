# Requisitos e análise de perigos — bomba de infusão simulada

> **Status**: Fase 1 de `implementation_plan_infusion_pump.md`, 2026-11-05. Documento de projeto;
> nada aqui está implementado. As cláusulas marcadas `[norma §x?]` não foram conferidas no texto da
> norma (mesma convenção de `implementation_plan_conformity.md` §2, D2) e não entram em contagem
> nenhuma até a Fase 0 daquele plano.
>
> **O que este documento não é**: uma análise de risco de uma bomba real. O mecanismo é simulado
> (`implementation_plan_infusion_pump.md` §1, §4.2). Os perigos são os de uma bomba de seringa
> real, porque é contra eles que o software tem de ser desenhado; os controles e as asserções
> verificam o **software**, e todo número de tempo, pressão ou volume abaixo é **parâmetro do
> modelo**, escolhido para ser testável, não um valor clínico.

---

## 1. Classe de segurança: C, como hipótese declarada (D8)

A IEC 62304 (§4.3 a), p. 16) classifica o sistema de software pelo pior dano a que uma falha dele
pode contribuir, depois dos controles de risco **externos** ao software.

- **Dano possível**: a sobreinfusão de um fármaco de alta vigilância (insulina, heparina,
  opioides, vasopressores) pode matar. A subinfusão e o atraso de terapia também podem.
- **Controles externos ao software**: nenhum, neste perfil. O mecanismo simulado não tem sensor de
  oclusão independente, pinça antifluxo livre, nem supervisor de hardware que pare o motor. Cada um
  desses, numa bomba real, poderia baixar a classe de uma parte do software. Aqui não há o que
  creditar.
- **Logo**: classe **C**, contra a classe B (hipótese) do EEG, cuja falha leva a um diagnóstico
  errado mediado por um clínico.

Isso é hipótese pelo mesmo motivo que a classe B do EEG é: a classificação de um produto real
depende do uso pretendido e da análise de risco do fabricante, e este trabalho não tem nenhum dos
dois. O que a hipótese faz é **escolher o caso mais exigente** para o teste da plataforma. A
diferença de classe obriga mais atividades da IEC 62304 (projeto detalhado, §5.4.2–5.4.4, p. 23–24;
critérios adicionais de verificação de unidade, §5.5.4, p. 24, ambos só na classe C), e é o argumento para a Fase 10
(o laço de segurança no Cortex-M33: a segregação da §5.3.5, p. 23, que a 62304 exige na classe C).
As cláusulas desta seção e da §3 foram conferidas em 2026-11-07 (`SystematicReview/IEC62304_extracao.md`,
`ISO14971_extracao.md`, `IEC81001-5-1_extracao.md`); as das normas 60601 e da guia da FDA, que não
estão em mãos, continuam com `?`.

---

## 2. Parâmetros do modelo

Valores escolhidos para que cada asserção da Fase 7 termine em segundos no QEMU. Ficam no arquivo
de configuração selado da bomba, com limites em `SafetyLimit`, e não no código.

| Símbolo | Significado | Valor | Observação |
|---|---|---|---|
| `T_hb` | período do sinal de vida controlador → atuador | 200 ms | |
| `k` | sinais perdidos até o estado seguro | 3 | estado seguro em ≤ `T_hb × k` = 600 ms, mais um período de controle do simulador |
| `T_prod` | silêncio do produtor até o alarme no controlador | 600 ms | o simétrico do homem-morto |
| `P_occ` | limiar de pressão de oclusão | 500 mmHg | bombas reais oferecem faixas de ~100 a ~900 mmHg |
| `T_occ` | prazo para o alarme de oclusão na taxa de teste | 30 s | depende da taxa e da complacência `C` do modelo; fixado na Fase 3 |
| `Q_test` | taxa de teste das asserções | 100 mL/h | |
| `ε_vol` | tolerância de volume entregue (modelo) | 1 % ou 1 passo, o maior | testa o modelo e a contagem, não a exatidão de uma bomba |
| `ε_del` | diferença comandado × medido que é falha de entrega | 5 % sustentado por 2 s | |
| `T_sil` | duração máxima do silenciamento de um alarme | 60 s | IEC 60601-1-8 `[60601-1-8 §6.3?]` limita o silenciamento; o valor é do modelo |

---

## 3. Requisitos técnicos da bomba (`INF-xx`)

No formato de `implementation_plan_conformity.md` §4. **Situação**: todos ✗ (nada implementado).
**Asserção** é o nome previsto na Fase 7 de `implementation_plan_infusion_pump.md`; as que este
documento acrescenta estão marcadas *(nova)*.

| RT | Requisito | Origem | Mecanismo previsto | Asserção prevista | Perigo |
|---|---|---|---|---|---|
| INF-01 | Uma taxa acima do limite **rígido** do fármaco é recusada e a recusa é auditada | `[60601-2-24?]`, orientação FDA para bombas `[?]` | DERS: `SafetyLimit` sobre a biblioteca selada | `ders-hard-limit` | H1 |
| INF-02 | Uma taxa acima do limite **flexível** só é aceita com confirmação explícita do operador, e a confirmação é auditada com quem confirmou | idem | DERS; evento `SafetyLimitOverridden` (D5) | `ders-soft-limit` | H1 |
| INF-03 | A biblioteca de fármacos é selada na construção e o serviço não sobe com ela alterada | 14971 §7.1 b) p. 12–13; 81001-5-1 §5.8.3 p. 27 (integridade dos arquivos relevantes) | `do_seal_configuration`, `MedicalConfiguration` | `drug-library-seal` | H1, H7 |
| INF-04 | O volume entregue (medido pelo *encoder*) é a integral da taxa comandada, dentro de `ε_vol` | `[60601-2-24?]` | contagem do serviço sobre a leitura de volta do atuador | `delivery-volume` | H1, H2 |
| INF-05 | Comandado ≠ medido por mais de `ε_del` gera alarme de alta prioridade e para o motor, **nos dois sentidos** (motor travado e motor disparado) | `[60601-2-24?]` | `MedicalActuator::readback()` comparado ao comando | `delivery-fault` | H1, H2 |
| INF-06 | Pressão acima de `P_occ` gera alarme de oclusão de alta prioridade e para o motor em até `T_occ` na taxa de teste | `[60601-2-24?]` | canal de pressão do simulador; limite em `SafetyLimit` | `occlusion-alarm` | H2 |
| INF-07 | Ar na linha gera alarme de alta prioridade e para o motor | `[60601-2-24?]` | canal binário do detector | `air-alarm` | H3 |
| INF-08 | Ao fim do VTBI a bomba para e alarma (média prioridade); nunca entrega além do VTBI | `[60601-2-24?]` | contagem de volume do serviço | `vtbi-complete` | H1 |
| INF-09 | Sem sinal de vida do controlador por `T_hb × k`, o **atuador** para o motor sozinho | IEC 62304 classe C, `[60601-1 §14?]` (PEMS) | homem-morto do lado do produtor, obrigatório no contrato de `MedicalActuator` (D3) | `deadman` | H1 |
| INF-10 | Sem resposta do atuador por `T_prod`, o controlador entra em alarme de alta prioridade e não comanda mais nada até reconhecimento | idem | o simétrico de INF-09, no serviço | `producer-lost` *(nova)* | H2 |
| INF-11 | Um reinício do serviço de controle (falha, reboot, atualização) durante a infusão volta **parado e em alarme**, nunca infundindo | 62304 §5.2.3 p. 21 (medida de controle de risco em software entra nos requisitos) | estado de terapia não persistido como "em curso"; marcador de terapia interrompida | `no-auto-resume` | H4 |
| INF-12 | Alarmes têm prioridade (alta, média, baixa); o silenciamento dura no máximo `T_sil` e o alarme volta se a causa persistir | `[60601-1-8?]` | gerenciador de alarmes do serviço | `alarm-silence` *(nova)* | H5 |
| INF-13 | Toda transição de estado, alarme, reconhecimento e confirmação está na trilha, e a cadeia verifica | 62304 §5.2.2 e) p. 21 (trilha de auditoria) | `MedicalLogger`, eventos de D5; verificador da cadeia (conformidade, Fase 2.1) | `audit-trail` | H6 |
| INF-14 | Uma atualização não é instalada durante a terapia | requisito de produto | D7 (*handler* do RAUC + declaração pelo `MedicalUpdate`) | `update-blocked` | H4 |
| INF-15 | Um slot cuja bomba não passa o autoteste do mecanismo não é confirmado | 14971 §7.1 b) p. 12–13; 62304 §5.5.4 f) p. 24 (autodiagnóstico, classe C) | `Type=notify`, `READY=1` depois do autoteste, `boot-complete.target` | `infusion-active` | H2 |
| INF-16 | Uma configuração com parâmetro fora do envelope (por exemplo, `T_hb × k` maior que o permitido) recusa subir | DERS | `SafetyLimit` sobre a configuração do serviço | teste de host (Fase 4) | H1, H2 |

---

## 4. Análise de perigos

Formato reduzido da ISO 14971 (cl. 5, p. 10–12, e cl. 7, p. 12–14): perigo → situação perigosa → causa no software ou no
mecanismo → controle de risco → RT que o verifica. **Severidade** é qualitativa (catastrófica,
crítica, séria); **probabilidade** não é estimada, porque não há dado de campo nem modelo de falha
de um mecanismo real, e inventar uma seria pior que declarar a ausência.

### H1. Sobreinfusão — catastrófica

| Causa | Controle | RT |
|---|---|---|
| Operador programa taxa errada (erro de digitação, unidade) | DERS rígido e flexível | INF-01, INF-02 |
| Biblioteca de fármacos corrompida ou adulterada | selo na construção | INF-03 |
| Motor entrega mais que o comandado | comparação comandado × medido | INF-05 |
| Contagem de volume errada, VTBI ultrapassado | contagem sobre o *encoder*; parada no VTBI | INF-04, INF-08 |
| Controlador trava com o motor ligado | homem-morto no atuador | INF-09 |
| Configuração do serviço com parâmetro de segurança absurdo | envelope de configuração | INF-16 |
| *Bolus* pós-oclusão (pressão acumulada liberada ao desobstruir) | **nenhum**: fora do escopo (D9) | — |
| Fluxo livre (seringa fora do suporte) | **nenhum**: controle mecânico, fora do escopo (D9) | — |

### H2. Subinfusão e atraso de terapia — crítica

| Causa | Controle | RT |
|---|---|---|
| Oclusão a jusante | alarme de pressão | INF-06 |
| Motor travado | comparação comandado × medido | INF-05 |
| Atuador deixa de responder | alarme do controlador | INF-10 |
| Imagem atualizada com o mecanismo quebrado | slot não confirmado, volta ao anterior | INF-15 |
| Parada não sinalizada | toda parada fora de "concluída" é alarme | INF-05, INF-06, INF-07, INF-10 |

### H3. Embolia gasosa — catastrófica

| Causa | Controle | RT |
|---|---|---|
| Ar na linha | detector, alarme, parada | INF-07 |
| Detector com falha (sempre "sem ar") | **nenhum no software**: o modelo não tem detector redundante; declarado | — |

### H4. Retomada indevida — crítica

| Causa | Controle | RT |
|---|---|---|
| `Restart=on-failure` religa o serviço e ele restaura a terapia | sem retomada automática | INF-11 |
| Reboot ou atualização no meio da infusão | sem retomada; atualização bloqueada | INF-11, INF-14 |

O conflito com o EEG é de propósito: lá, `Restart=on-failure` é o comportamento correto, porque
voltar a adquirir é seguro. Num atuador, voltar a agir sem um humano decidir não é.

### H5. Alarme não percebido — crítica

| Causa | Controle | RT |
|---|---|---|
| Alarme silenciado e esquecido | silenciamento limitado; o alarme volta | INF-12 |
| Alarme de baixa prioridade mascarando um de alta | prioridade; o de maior prioridade é o apresentado | INF-12 |
| HMI travada ou ausente | **parcial**: o estado de alarme está no serviço e na trilha, não depende da HMI; a apresentação ao operador depende, e a HMI não tem supervisão | INF-13 (só o registro) |

### H6. Perda de rastreabilidade — séria

| Causa | Controle | RT |
|---|---|---|
| Evento não registrado | auditoria de cada transição | INF-13 |
| Trilha alterada | selo do SO e cadeia da aplicação | AUD-02, AUD-03 (herdados; AUD-02 com o falso positivo de `implementation_plan_conformity.md` §5 A8) |
| Trilha perdida numa atualização | estado persistente fora do slot | AUD-06 (herdado; aberto) |

### H7. Fármaco ou seringa errados — catastrófica

| Causa | Controle | RT |
|---|---|---|
| Fármaco errado escolhido na biblioteca | confirmação do programa pelo operador, auditada | INF-02 (só a confirmação), INF-13 |
| Tamanho de seringa declarado diferente do instalado | **nenhum**: o modelo não detecta a seringa; o tamanho é campo do programa, confirmado e auditado | INF-13 (só o registro) |

### H8. Comando por quem não deveria — crítica

| Causa | Controle | RT |
|---|---|---|
| Qualquer processo que alcança o socket de controle comanda a bomba | **nenhum hoje**: é a lacuna ACE-01 da plataforma (o socket não lê a credencial do requisitante), herdada pela bomba | ACE-01 (herdado; aberto) |

H8 é o perigo em que a bomba torna urgente uma lacuna que o EEG tolera: um comando não
autorizado ao EEG inicia ou para uma gravação; à bomba, muda uma taxa.

---

## 5. Cobertura

| | Quantidade |
|---|---|
| Perigos | 8 |
| Causas | 26 |
| Causas com controle e asserção prevista | 18 |
| Causas com controle só de registro (sem asserção que veja o dano) | 3 (H5 HMI, H7 fármaco, H7 seringa) |
| Causas cujo controle é um RT da plataforma ainda aberto | 2 (H6 trilha perdida numa atualização, AUD-06; H8, ACE-01) |
| Causas **sem controle**, declaradas | 3 (*bolus* pós-oclusão, fluxo livre, detector de ar com falha) |
| RTs `INF-xx` | 16, dos quais 15 com asserção prevista e 1 com teste de host |
| Asserções novas para a Fase 7 | 2 (`producer-lost`, `alarm-silence`) |

Critério da Fase 1 (`implementation_plan_infusion_pump.md` §7): todo perigo tem pelo menos um
controle e uma asserção prevista. **Cumprido**, com uma ressalva: H8 só tem controle herdado e
aberto (ACE-01). Ele não é resolvido pela bomba, e entra na lista do que a bomba **revela** sobre a
plataforma.

**Ligação com o plano de conformidade**: H6 e H8 dependem de RTs da plataforma (AUD-02, AUD-03,
AUD-06, ACE-01). Para a bomba, eles são "herdados" no sentido da §9 do plano da bomba, e herdam
também o estado: três deles estão abertos ou parciais hoje.
