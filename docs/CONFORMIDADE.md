# Atendimento aos requisitos técnicos das normas — matriz e requisitos

> **O que este documento é**: a verificação, com números e evidência, dos **controles técnicos** que
> três normas exigem ou motivam, sobre a plataforma MedPlatform. **O que não é**: uma declaração de
> conformidade. As três normas são de **processo**: a conformidade se verifica pela documentação do
> fabricante (IEC 62304 §1.4; IEC 81001-5-1 A.1), e este trabalho não é fabricante. Vocabulário
> (D1 do plano): "atende ao requisito técnico", nunca "está conforme".
>
> Fase 1 de `implementation_plan_conformity.md`, 2026-11-07. Normas conferidas por leitura integral do
> autor, com página impressa: **IEC 62304:2006+AMD1:2015**, **ISO 14971:2019** e **IEC 81001-5-1:2021**
> (`SystematicReview/*_extracao.md`). Verificado por `python3 scripts/med-conformity-check.py`, que
> recalcula as categorias e confere cada ponteiro de evidência; os números abaixo são a saída dele.

## 1. Como ler

Duas tabelas, ligadas pelo identificador do requisito técnico (RT):

- **§2, os requisitos técnicos**: o que cada um exige, de que cláusula vem, que mecanismo o atende,
  que evidência o sustenta, em que nível e em que situação.
- **§3, a matriz normativa**: cada cláusula aplicável, a categoria que os seus RTs lhe dão e os RTs que
  derivam dela. As cláusulas de processo aparecem como categoria E, em vez de omitidas, para que a
  seleção seja auditável.

| Nível | Significado |
|---|---|
| **E0** | declarado: o código ou o documento existe, e nada o executou |
| **E1** | construção e inspeção do artefato (imagem, `.wic`, configuração instalada) |
| **E2** | asserção automática sobre o sistema em execução (`make check`, testes de host) |
| **E3** | E2, e a asserção **foi vista falhar** com o defeito injetado |

| Categoria da cláusula | Regra (calculada, não escolhida) |
|---|---|
| **A** | todos os seus RTs atendidos, com evidência E2 ou E3 |
| **B** | todos atendidos, com evidência só de inspeção (E1) |
| **C** | parcial: algum RT atendido ou parcial, algum não |
| **D** | nenhum RT atendido: limitação declarada |
| **E** | fora do software: processo ou documentação do fabricante |

Situação do RT: ✅ atendido, ◐ parcial, ✗ aberto. ◐ na origem marca uma âncora que a extração da norma
classifica como indireta. A coluna **Placa** marca o que foi observado no STM32MP257.

## 2. Os requisitos técnicos

### Auditoria e rastreabilidade

| RT | Requisito | Origem | Mecanismo | Evidência | Nível | Placa | Sit. |
|---|---|---|---|---|---|---|---|
| AUD-01 | A trilha persiste entre reinicializações | 62304 §5.2.2 e) p. 21 ("*audit trail*"; a persistência é interpretação, não texto) | journald `Storage=persistent`; correção do `VOLATILE_LOG_DIR` | `journal-persistent`: `/var/log/journal` vem de `/data` e `/var/log` não é link; `--list-boots` após reboot (`RESULTS.md` §5) | E2 | sim | ✅ |
| AUD-02 | A alteração de um registro é detectável (selo do SO) | 62304 §5.2.2 e) p. 21; 81001-5-1 §7.2 e §7.4 p. 30–31 ◐ (ameaça de adulteração no modelo de ameaças) | Forward Secure Sealing: `Seal=yes` + chave criada no 1º boot (`med-journal-seal.service`), chave de verificação fora do dispositivo | `journal-seal-key`, `journal-sealed`: no boot de provisionamento todo arquivo selado valida com a chave e é recusado com uma chave errada; nos boots seguintes 2 de 5 falham por falso positivo do systemd 255 (§5, A6 e A8; `RESULTS.md` §5) | E2 | — | ◐ falso positivo do systemd |
| AUD-03 | A alteração de um registro é detectável (cadeia da aplicação) | 62304 §5.2.2 e) p. 21; 81001-5-1 §7.2 e §7.4 p. 30–31 ◐ (ameaça de adulteração no modelo de ameaças) | cadeia SHA-256 do `MedicalLogger`, formato 2 (`src/AuditChain.h`), verificada por `med-audit-verify` | `audit-chain`: 43 registros em 2 cadeias, todos verificados através de um reboot, e uma cópia com um campo alterado recusada no alvo; 46 verificações de host com vetor independente e 3 injeções (§5, A2; `RESULTS.md` §5) | E2 | — | ✅ |
| AUD-04 | Os eventos relevantes são registrados | 62304 §5.2.2 e) p. 21 | 24 tipos de `AuditEvent` (18, mais 6 da bomba) | a bomba emite e a suíte confere 9 tipos (`audit-trail`, vista falhar no lote 1 de `tests/infusion-target/inject.py`); o EEG emite 13 dos seus 18, e a exportação é conferida como `PATIENT_DATA_EXPORTED` (`export-audited`, vista falhar na imagem anterior à correção); faltam os 3 de atualização, a leitura e a calibração (§5 A3 de `implementation_plan_conformity.md`) | E3 | — | ◐ |
| AUD-05 | O registro identifica quem agiu | 62304 §5.2.2 e) p. 21 (autenticação, trilha); 81001-5-1 §3.31 e §5.4.3 f) ◐ | `affirmed_operator` no metadado | `affirmed_operator` gravado e rotulado como afirmado (`implementation_plan_eeg_control.md` §13); o socket de controle não lê a credencial do requisitante; nenhuma asserção confere o campo | E1 | sim | ◐ |
| AUD-06 | A trilha sobrevive a uma atualização | 62304 §5.2.2 e) p. 21 e §8.3 (histórico recuperável) ◐ | `/var/log/journal` e `/var/lib/medplatform` montados (bind) de `/data/state`, que nenhuma atualização escreve (`med-state.sh`, `med-data-volume`) | `journal-persistent` e `state-persistent`, vistas falhar numa imagem sem o bind; no QEMU, o slot A regravado com um rootfs limpo entre dois boots: os 42 registros de antes continuam, a cadeia segue de 42 para 45 sem quebra nem reinício (`tests/conformity/slot_rewrite.py`); na placa, não medido | E3 | — | ✅ |
| TMP-01 | O registro carrega uma hora confiável, ou declara que não carrega | 62304 §5.2.2 e) p. 21 ◐ (interpretação: uma trilha sem hora confiável é incompleta) | `clock_synchronized` no `metadata.json` | `clock_synchronized` no `metadata.json` (`MedicalTypes.h`); nenhuma asserção o confere; a bomba não o declara (`implementation_plan_infusion_pump.md` §13, Fase 8) | E0 | sim | ◐ |

### Proteção e integridade dos dados

| RT | Requisito | Origem | Mecanismo | Evidência | Nível | Placa | Sit. |
|---|---|---|---|---|---|---|---|
| DAD-01 | Dado de paciente cifrado em repouso | 81001-5-1 §7.2 e §7.4 ◐; 14971 Tabela C.2 p. 34 ("*Data without encryption*", informativa) | LUKS2 em `/data`; o `MedicalStorage` exige `dm/uuid` começando em `CRYPT-` | `data-encrypted`, `storage-requires-encryption`, `no-encryption-waiver` | E3 | sim | ✅ mecanismo |
| DAD-02 | A chave fica sob custódia fora da mídia | 81001-5-1 §7.4 ◐ (§5.8.4 trata de chave de assinatura, não de chave de dados) | `MED_DATA_KEY_SOURCE` | chave de desenvolvimento no mesmo disco; `tpm2` inalcançável (`implementation_plan_luks.md` §9) | E1 | sim | ✗ |
| DAD-03 | Uma falha de armazenamento não destrói dado | 14971 §7.1 b) p. 12–13; Tabela C.1 p. 32 (dados: disponibilidade, integridade) | salvaguarda do `med-data-provision.sh` | cabeçalho LUKS zerado: recusa reformatar (`RESULTS.md` §7) | E3 | — | ✅ |
| DAD-04 | Disco cheio não corrompe a sessão nem impede atualização | 14971 §7.1 a) ou b) p. 12–13; Tabela C.1 (dados: disponibilidade) | `storage.reserve_mib` | `storage-reserve` | E2 | sim | ✅ |
| DAD-05 | O acesso a dado de paciente é restrito por papel | 81001-5-1 §5.3.2 b) p. 22 (menor privilégio), §5.4.3 c) e f) | usuário `med-review`, permissões | `review-reach`, `desktop-reach` (EACCES medido e injetado) | E3 | sim | ✅ |
| INT-01 | O registro clínico tem integridade verificável | 62304 §3.22 e §5.2.2 e) e g) p. 21 ◐ | arquivo `.sha256` ao lado de cada arquivo (`MedicalStorage`) | `session-closed`, `record-bdf` | E2 | sim | ✅ (injeção a registrar) |
| INT-02 | Configuração e calibração não podem ser adulteradas | 14971 §7.1 b) p. 12–13; Tabela C.1 (dados: integridade; medição) | armazém selado na construção | `config-seal`, `config-seal-owner`; recusa de configuração adulterada (teste de host) | E3 | sim | ✅ |
| INT-03 | Parâmetro fora do envelope de segurança impede a operação | 14971 §7.1 b) p. 12–13 e §5.3 p. 11 (limites das características ligadas à segurança) | `SafetyLimit` do `MedicalConfiguration` | recusa ao iniciar (`tests/framework`, `tests/infusion-control`); no alvo, com um limite alterado na biblioteca da bomba, o serviço recusa subir (`tests/infusion-target/inject_runtime.py`) | E3 | — | ✅ |
| INT-04 | Perda de amostra deixa marca no registro | 62304 §5.2.2 b) e d) p. 21, §5.5.4 d) p. 24, §7.1.2 d) p. 30 ◐ | CRC por quadro, salto de sequência (`med_producer_on_lost`) | `acq-sample-rate` (falha injetada: `SOCK_NONBLOCK`) | E3 | sim | ✅ |

### Atualização

| RT | Requisito | Origem | Mecanismo | Evidência | Nível | Placa | Sit. |
|---|---|---|---|---|---|---|---|
| ATU-01 | Só se instala atualização autenticada | 81001-5-1 §5.8.3 p. 27, §6.3.3 p. 29, §5.8.4; 62304 §5.8.8 p. 27–28; 14971 Tabela C.2 ("*Software updates without authenticity confirmation*") | RAUC, bundle verity assinado, `check-purpose=codesign` | bundle recusado com certificado inadequado; instalado com o certo (`RESULTS.md` §6) | E3 | sim | ✅ |
| ATU-02 | Uma atualização de outro aparelho é recusada | nas três normas, só 62304 §5.8.8 p. 27–28 ◐ (entrega sem alteração não autorizada) | a identidade de compatibilidade do mecanismo (no RAUC, `compatible`; no Mender, `device_type`; no SWUpdate, `hardware-compatibility`) | defeito encontrado e corrigido (`implementation_plan_rauc.md` §8) | E1 | sim | ✅ (injeção a registrar) |
| ATU-03 | A atualização é atômica e recuperável | 14971 §7.1 a) p. 12–13 e §7.5 p. 14 (o retorno pode reinstalar versão vulnerável) | A/B + contadores do U-Boot | fallback por falha injetada: 3→0, sobe em B (`implementation_plan_uboot_ab.md` §8) | E3 | sim | ✅ (pânico de kernel não testado) |
| ATU-04 | Um slot só é confirmado se o dispositivo funciona | 14971 §7.1 b) p. 12–13 (medida) e §7.2 p. 13 (eficácia verificada) | `boot-complete.target` + `READY=1` após `device.start()` | slot sem aquisição nunca é confirmado, observado sem ajuda em 2026-10-08 (`BRINGUP_STM32MP2.md` §9.16); `infusion-active` com o simulador ausente: o serviço não fica pronto (`tests/infusion-target/inject_runtime.py`) | E3 | sim | ✅ |
| ATU-05 | A atualização é registrada na trilha | 62304 §5.2.2 e) p. 21 e §8.3; 81001-5-1 E.2.5 d) p. 50 ◐ (informativo) | — | `UpdateStarted/Succeeded/Failed` nunca são emitidos (§5, A3); para a segunda classe, ver o achado do ATU-02 em `implementation_plan_infusion_pump.md` §13, Fase 8 | — | — | ✗ |
| ATU-06 | Uma atualização para uma versão anterior é recusada | 14971 §7.5 p. 14 (o retorno pode reinstalar versão vulnerável); 81001-5-1 §7.2 p. 30–31 ◐ (ameaça T05 do modelo) | — | nada recusa uma versão mais antiga: o `system.conf` não tem *handler* de instalação (`MODELO_DE_AMEACAS.md` §4, T05) | — | — | ✗ |

### Segregação e confinamento

| RT | Requisito | Origem | Mecanismo | Evidência | Nível | Placa | Sit. |
|---|---|---|---|---|---|---|---|
| SEG-01 | O item de aquisição fica segregado do sistema de uso geral | 62304 §5.3.5 p. 23 (exigida na classe C) e §4.3 d) p. 17; 81001-5-1 §5.3.3 a) p. 23 (sem classe) | AMP: o conversor só no M33, periféricos atribuídos por RIF | aquisição pelo link `amp` com o Linux sem acesso ao conversor (`BRINGUP_STM32MP2.md` §9.16) | E2 | sim | ✅ (só link `amp`) |
| SEG-02 | O processo de aquisição roda confinado | 62304 B.4.3 p. 43; 81001-5-1 §5.3.3 a) p. 23, §5.3.2 b) e f) p. 22 | sandbox do systemd: sem capacidade nenhuma, `UMask=0077`, `ProtectProc=invisible`, `ProcSubset=pid`, sem rede | `sandbox-exposure`: de 3.7 para **1.3**, com o tempo real mantido (`acq-realtime`, `acq-sample-rate`); vista falhar com as capacidades devolvidas em execução, 3.3 (`tests/infusion-target/inject_runtime.py`; `RESULTS.md` §4) | E3 | — | ✅ |
| SEG-03 | A aquisição tem prioridade de tempo real garantida | 62304 §5.2.2 a) p. 21 (requisitos de tempo), §5.5.4 c) p. 24 ◐ | `SCHED_RR` 50 | `acq-realtime` | E2 | — | ✅ |
| SEG-04 | A fronteira entre camadas é verificada | 62304 §5.3.6 p. 23 (verificação da arquitetura) e §5.3.1 | `make check-layers` (F1–F9), `med-layer-guard.bbclass` | falha o build de imagem na violação (`RESULTS.md` §11) | E3 | — | ✅ |

### Controle de acesso e endurecimento

| RT | Requisito | Origem | Mecanismo | Evidência | Nível | Placa | Sit. |
|---|---|---|---|---|---|---|---|
| ACE-01 | Comandos ao serviço vêm de requisitante identificado | 81001-5-1 §5.4.3 d) e f) p. 23–24, §3.43 (autenticação na fronteira de confiança) | socket `0660 root` | quem alcança o socket comanda; sem credencial do requisitante | — | sim | ✗ |
| ACE-02 | Cada processo roda com o menor privilégio | 81001-5-1 §5.3.2 b) p. 22 (literal) | usuários de serviço | a HMI do EEG roda como root, não como `med-hmi` (`implementation_plan_hmi_desktop.md`); **14 de 17 serviços em execução rodam como root** na imagem da bomba (`attack-surface`), os da bomba entre eles, com `CapabilityBoundingSet=` vazio | E2 | — | ◐ |
| ACE-03 | O produto não oferece login remoto nem depuração | 81001-5-1 §5.3.2 g) e h) p. 22, §7.2 g) e h) p. 31; 14971 Tabela C.2 ("*Unsecured data ports*") | `med-image-prod` remove `debug-tweaks`, ssh, login de root | **a imagem de produção nunca foi construída** (`med-image-prod.bb`); tudo o que foi medido é imagem de desenvolvimento (§5 A1 do plano; Fase 5) | E0 | — | ✗ |
| SIS-01 | O sistema de arquivos raiz é imutável | 81001-5-1 §5.3.1 e §5.3.2 f) p. 22 ◐ (não exigido nominalmente) | `read-only-rootfs` | só em `med-image-prod.bb`; as imagens medidas têm rootfs gravável, e com ele somente leitura a trilha e o estado da cadeia não teriam onde ser gravados (§5 A1 e A7 do plano; Fase 5) | E0 | — | ✗ |
| SIS-02 | Controle de acesso obrigatório | 81001-5-1 §5.3.1 e §5.3.2 b) p. 22 ◐ (não exigido nominalmente) | AppArmor (`implementation_plan_mac.md`) | não implementado (`implementation_plan_mac.md`) | — | — | ✗ |
| SIS-03 | O software em uso (rootfs e kernel) é verificado no boot | 81001-5-1 §5.8.3 p. 27 (verificação de integridade de executáveis e arquivos relevantes) | — | a verity protege só o pacote de atualização; nada verifica o slot em uso nem o kernel no boot (`MODELO_DE_AMEACAS.md` §4, T25) | — | — | ✗ |

### Inventário de software e terceiros

| RT | Requisito | Origem | Mecanismo | Evidência | Nível | Placa | Sit. |
|---|---|---|---|---|---|---|---|
| INV-01 | Cada imagem tem SBOM | 81001-5-1 cl. 8 p. 32 (lista reproduzível de componentes), §5.7.3 d) | `INHERIT += "create-spdx"` (`med-os.conf`); contado por `med-sbom-report.py` | `med-sbom-report.py`: os **240 de 240** pacotes do EEG têm documento SPDX, com licença, versão e fornecedor; **0 de 240 dizem de onde veio a fonte** (`downloadLocation`); 119 de 126 receitas têm CPE (as 7 sem são *packagegroups* e `glibc-locale`) (`RESULTS.md` §12) | E1 | — | ✅ |
| INV-02 | Cada componente de terceiros (SOUP) tem nome, versão e licença | 62304 §8.1.2 p. 32 (título, fabricante, versão; **licença não é exigida**); 81001-5-1 cl. 8, E.2.1 b) | SPDX + `buildhistory` | nome e versão em 240 de 240; o fornecedor é `Organization: OpenEmbedded` em todos, genérico, e não o fabricante de cada componente (62304 §8.1.2 b)); a origem da fonte falta em todos (`med-sbom-report.py`, `RESULTS.md` §12) | E1 | — | ◐ |
| INV-03 | O código de kernel próprio não é SOUP não controlado | 62304 §3.29 p. 14 (deixa de ser SOUP o que foi feito para o dispositivo **e** tem registros do processo) | driver derivado de `ti-ads1298.c` (mainline) e submetido à revisão pública | submetido à `linux-iio` (`implementation_plan_ads1299_upstream.md`; data e Message-ID a registrar) | E1 | — | ✅ |
| INV-04 | Vulnerabilidades conhecidas são identificadas | 81001-5-1 §5.7.3 p. 25, §6.2.1, §4.1.7; 62304 §7.1.3 p. 30 | `cve-check` (não habilitado) | — | — | — | ✗ |
| INV-05 | Cada componente de terceiros está classificado como *maintained*, *supported* ou *required* | 81001-5-1 §4.3 p. 20, A.3 p. 37 | critério: *maintained* é o que o pacote A/B entrega | `MODELO_DE_AMEACAS.md` §3: todo o rootfs é *maintained* (240 pacotes no EEG, 210 na bomba); **o kernel, o device tree, o TF-A, o FIP e o GRUB são *required***, porque ficam fora dos slots A/B | E1 | — | ✅ |
| INV-06 | O código é compilado com opções de compilação seguras | 81001-5-1 B.5.4 p. 41–42, §5.7.3 d) | `security_flags.inc` (`med-os.conf`) | herdado em `med-os.conf`, não inspecionado no artefato | E0 | — | ◐ |

### Ameaças e testes de segurança (Fase 0)

| RT | Requisito | Origem | Mecanismo | Evidência | Nível | Placa | Sit. |
|---|---|---|---|---|---|---|---|
| THR-01 | Todo produto tem modelo de ameaças, com fluxo de dados, fronteiras de confiança, portas físicas e de depuração e dependências de terceiros | 81001-5-1 §4.2 p. 20, §7.2 p. 30–31, §5.4.2 | DFD + STRIDE sobre as 19 interfaces (Anexo C) | `MODELO_DE_AMEACAS.md` §4: fluxo de dados, ativos, atores e 25 ameaças, 7 mitigadas, 9 parciais e 9 não mitigadas, cada uma ligada ao RT que verifica o controle; a revisão anual e a aceitabilidade são do fabricante | E1 | — | ✅ |
| THR-02 | O contexto de segurança está declarado (posição na rede, segurança física, isolamento, impacto na segurança do paciente) | 81001-5-1 §7.1.2 p. 30 | um parágrafo por perfil | `MODELO_DE_AMEACAS.md` §1: rede, segurança física, isolamento, impacto, hardware dedicado, para o EEG na placa, a bomba e a estação no PC; a segurança física é **suposição declarada**, e o atacante com acesso físico não é mitigado | E1 | — | ✅ |
| THR-03 | Cada interface, física e lógica, está caracterizada: fronteira de confiança, papéis, ameaças, validação de entrada | 81001-5-1 §5.4.3 a)–j) p. 23–24 | tabela de interfaces | `MODELO_DE_AMEACAS.md` §2: 19 interfaces, todas cruzando fronteira de confiança; 3 sem controle nenhum (console, SWD, sshd), 12 com lacuna declarada, 4 sem lacuna | E1 | — | ✅ |
| THR-04 | A chave privada de assinatura do código tem controles de proteção declarados | 81001-5-1 §5.8.4 p. 27 | `pki/` fora do git; `MOK.priv` não rastreado | a chave de desenvolvimento é gerada por `med-pki.sh` e fica na árvore de trabalho, fora do git; nada a protege além disso | E1 | — | ◐ |
| TST-01 | A superfície de ataque da imagem é testada (portas abertas, serviços com privilégio elevado) | 81001-5-1 §5.7.3 b) p. 25 | `attack-surface` no `make check` (plataforma): toda porta fora do loopback tem de estar declarada, com motivo | `attack-surface`, vista falhar no alvo com uma porta não declarada aberta (`tests/infusion-target/inject_runtime.py`); medido na bomba: expostas `22/tcp` (sshd, desenvolvimento) e `68/udp` (DHCP), loopback `53` | E3 | — | ✅ |
| TST-02 | As interfaces externas passam por casos de abuso e *fuzzing* | 81001-5-1 §5.7.3 a) e e) p. 25 | — | nada; trabalho futuro | — | — | ✗ |

## 3. A matriz normativa

### IEC 62304:2006+AMD1:2015

| Norma | Cláusula | Assunto | RTs | Cat. | Observação |
|---|---|---|---|---|---|
| 62304 | 1.4 (p. 9–10) | conformidade | — | E | conformidade é execução de processos verificada por documentação; este trabalho não declara conformidade (D1) |
| 62304 | 3.29 (p. 14) | SOUP | INV-03 | B |  |
| 62304 | 4.1 (p. 15) | sistema de gestão da qualidade | — | E | do fabricante |
| 62304 | 4.2 | gerenciamento de risco pela ISO 14971 | — | E | ver as linhas da ISO 14971 |
| 62304 | 4.3 a) (p. 16) | classe de segurança | — | E | a classe é decisão do fabricante; aqui é hipótese declarada (B para o EEG, C para a bomba, D5) |
| 62304 | 4.3 d) (p. 17) | itens com classes distintas exigem segregação justificada | SEG-01 | A |  |
| 62304 | 5.1.1 c) (p. 18) | rastreabilidade requisito ↔ teste ↔ controle de risco | — | E | atendida em parte por este documento, que é ele mesmo a rastreabilidade |
| 62304 | 5.1.10 (p. 20) | ferramentas de desenvolvimento sob controle | — | E | processo; o projeto fixa as revisões das camadas (`kas`) e constrói num contêiner fixado (`BUILD_CONTAINER.md`) |
| 62304 | 5.2.2 a) (p. 21) | requisitos de tempo | SEG-03 | A |  |
| 62304 | 5.2.2 b) (p. 21) | faixas, limites e valores padrão | INT-03, INT-04 | A |  |
| 62304 | 5.2.2 d) (p. 21) | alarmes e mensagens ao operador | INT-04 | A |  |
| 62304 | 5.2.2 e) (p. 21) | segurança da informação: autenticação, autorização, **trilha de auditoria**, integridade | AUD-01, AUD-02, AUD-03, AUD-04, AUD-05, AUD-06, TMP-01, INT-01, ATU-05 | C |  |
| 62304 | 5.2.2 g) (p. 21) | definição de dados | INT-01 | A |  |
| 62304 | 5.2.3 (p. 21) | medidas de controle de risco nos requisitos | — | E | processo; para a bomba, feito em `REQUISITOS_INFUSAO.md` |
| 62304 | 5.3.1 (p. 22) | arquitetura documentada | SEG-04 | A |  |
| 62304 | 5.3.3–5.3.4 (p. 22) | requisitos funcionais e de plataforma do SOUP | INV-02 | C |  |
| 62304 | 5.3.5 (p. 23) | segregação necessária ao controle de risco (classe C) | SEG-01 | A |  |
| 62304 | 5.3.6 (p. 23) | verificação da arquitetura | SEG-04 | A |  |
| 62304 | 5.4 (p. 23–24) | projeto detalhado (classe C) | — | E | não exigido na classe B; exigido para a bomba (classe C, hipótese) e não feito como processo |
| 62304 | 5.5–5.7 (p. 24–27) | verificação de unidade, integração e sistema | — | E | processo; o projeto tem `make check` e testes de host com injeção de falha, que são evidência e não conformidade |
| 62304 | 5.8.5 (p. 27) | procedimento e ambiente de criação documentados | — | E | processo; `BUILD_CONTAINER.md` e os arquivos `kas` |
| 62304 | 5.8.8 (p. 27–28) | entrega sem corrupção nem alteração não autorizada | ATU-01, ATU-02 | A |  |
| 62304 | 6 (p. 28–30) | manutenção | — | E | processo do fabricante; a IEC 60601-1 não o exige (Tabela C.3) |
| 62304 | 7.1.2 d) (p. 30) | falha de hardware que torna o software imprevisível | INT-04 | A |  |
| 62304 | 7.1.3 (p. 30) | anomalias publicadas do SOUP | INV-04 | D |  |
| 62304 | 7.3.3 (p. 31) | rastreabilidade perigo → medida → verificação | — | E | processo; feita para a bomba em `REQUISITOS_INFUSAO.md`, ilustrativa |
| 62304 | 8.1.2 (p. 32) | identificação do SOUP | INV-02 | C |  |
| 62304 | 8.1.3 (p. 32) | configuração do sistema documentada | INV-01 | B |  |
| 62304 | 8.3 | histórico recuperável da configuração | AUD-06, ATU-05 | C |  |
| 62304 | 9 (p. 33–34) | resolução de problemas | — | E | processo do fabricante |
| 62304 | B.4.3 (p. 43) | segregação por qualquer mecanismo que impeça um item de afetar outro | SEG-02 | A |  |

### ISO 14971:2019

| Norma | Cláusula | Assunto | RTs | Cat. | Observação |
|---|---|---|---|---|---|
| 14971 | 4.1–4.5 (p. 7–10) | processo, política, plano e arquivo de risco | — | E | do fabricante; não há arquivo de risco, só a análise ilustrativa da bomba |
| 14971 | 5.1–5.5 (p. 10–12) | análise de risco | — | E | do fabricante; ilustrativa para a bomba (`REQUISITOS_INFUSAO.md`) |
| 14971 | 5.3 (p. 11) | características ligadas à segurança e seus limites | INT-03 | A |  |
| 14971 | 7.1 a) (p. 12–13) | projeto inerentemente seguro (primeira prioridade) | SEG-01, SEG-04, ATU-03, DAD-04, ACE-03 | C |  |
| 14971 | 7.1 b) (p. 12–13) | medidas de proteção no dispositivo | DAD-01, DAD-03, INT-02, INT-03, ATU-01, ATU-04, AUD-02, AUD-03, SEG-02 | C |  |
| 14971 | 7.1 c) (p. 12–13) | informação de segurança e treinamento | — | E | documentação ao usuário, do fabricante |
| 14971 | 7.2 (p. 13) | verificar a implementação **e** a eficácia de cada medida | ATU-04 | A | o método inteiro: os níveis E2 e E3 deste documento |
| 14971 | 7.5 (p. 14) | riscos introduzidos pelas próprias medidas | ATU-03, DAD-02, ATU-06 | C |  |
| 14971 | 8–10 (p. 14–16) | risco residual global, revisão, pós-produção | — | E | do fabricante; não há aparelho em campo |

### IEC 81001-5-1:2021

| Norma | Cláusula | Assunto | RTs | Cat. | Observação |
|---|---|---|---|---|---|
| 81001-5-1 | 4.1 (p. 18–20) | gestão da qualidade, divulgação de vulnerabilidades | — | E | do fabricante |
| 81001-5-1 | 4.2 (p. 20) | risco de segurança com modelagem de ameaças | THR-01 | B |  |
| 81001-5-1 | 4.3 (p. 20) | itens *maintained*, *supported*, *required* | INV-05 | B |  |
| 81001-5-1 | 5.1 (p. 21) | planejamento, ambiente de desenvolvimento, codificação segura | — | E | processo; o build em contêiner é a parte técnica do §5.1.2 |
| 81001-5-1 | 5.2 (p. 21–22) | requisitos de segurança e sua revisão | — | E | processo |
| 81001-5-1 | 5.3.1 (p. 22) | arquitetura de defesa em profundidade | SIS-01, SIS-02 | D |  |
| 81001-5-1 | 5.3.2 b) (p. 22) | menor privilégio | ACE-02, DAD-05, SEG-02 | C |  |
| 81001-5-1 | 5.3.2 f) (p. 22) | redução da superfície de ataque | SIS-01, TST-01 | C |  |
| 81001-5-1 | 5.3.2 g)–h) (p. 22) | remover acesso e informação de depuração | ACE-03 | D |  |
| 81001-5-1 | 5.3.3 a) (p. 23) | segregação eficaz dos itens | SEG-01, SEG-02 | A |  |
| 81001-5-1 | 5.4.2 (p. 23) | projeto cobre as ameaças do modelo | THR-01 | B |  |
| 81001-5-1 | 5.4.3 (p. 23–24) | interfaces seguras, fronteiras de confiança | THR-03, ACE-01 | C |  |
| 81001-5-1 | 5.5 (p. 24–25) | implementação e revisão | — | E | processo |
| 81001-5-1 | 5.7.2 (p. 25) | teste de mitigação: tentar derrotá-la | — | E | processo; a injeção de falha deste trabalho é a mesma ideia (nível E3) |
| 81001-5-1 | 5.7.3 (p. 25) | teste de vulnerabilidades, superfície de ataque, *fuzzing*, composição | INV-04, TST-01, TST-02, INV-06, INV-01 | C |  |
| 81001-5-1 | 5.7.4–5.7.5 (p. 25–26) | teste de intrusão; objetividade e independência | — | E | processo; **não atendido** e declarado: o trabalho é de uma pessoa (Tabela A.1) |
| 81001-5-1 | 5.8.2 e 5.8.7 (p. 26–27) | guias de operação segura e de descomissionamento | — | E | documentação do fabricante; não existem |
| 81001-5-1 | 5.8.3 (p. 27) | verificação de integridade de executáveis e arquivos | ATU-01, INT-02, SIS-03 | C |  |
| 81001-5-1 | 5.8.4 (p. 27) | proteção da chave privada de assinatura | THR-04 | C |  |
| 81001-5-1 | 6.1.1, 6.3.2 (p. 28–29) | prazos e disponibilização de atualizações de segurança | — | E | processo; o mecanismo que as entrega é o ATU-01 |
| 81001-5-1 | 6.2.1 (p. 28) | monitorar fontes de vulnerabilidades | INV-04 | D |  |
| 81001-5-1 | 6.3.3 (p. 29) | atualização com integridade verificável pelo usuário | ATU-01 | A |  |
| 81001-5-1 | 7.1.2 (p. 30) | contexto de segurança | THR-02 | B |  |
| 81001-5-1 | 7.2 (p. 30–31) | identificação de ameaças e impactos | THR-01, AUD-02, DAD-01 | C |  |
| 81001-5-1 | 7.3–7.5 (p. 31) | estimar, controlar e monitorar o risco de segurança | — | E | processo |
| 81001-5-1 | 8 (p. 32) | lista reproduzível de componentes externos | INV-01, INV-02 | C |  |
| 81001-5-1 | 9 (p. 32–34) | tratamento de vulnerabilidades e problemas | — | E | processo; o envio de correções ao kernel é o caminho do §9.5 para software aberto |

## 4. Contagem

Saída de `scripts/med-conformity-check.py`, 2026-11-07:

| | |
|---|---|
| Requisitos técnicos | **44**: 26 ✅, 8 ◐, 10 ✗ |
| Por nível | E3 14 · E2 8 · E1 11 · E0 4 · sem evidência 7 |
| Cláusulas na matriz | **67**: A 15 · B 6 · C 15 · D 4 · E 27 |
| Cláusulas técnicas (sem as E) | **40**: atendidas com evidência que falha sozinha (A) 15 · só por inspeção (B) 6 · parcialmente (C) 15 · não (D) 4 |

**A afirmação que este documento sustenta** é a última linha da tabela: quantas das cláusulas técnicas
das três normas (as que não são processo do fabricante) são atendidas, e com que evidência, cada número
apontando o comando que o produziu. Os números da tabela são reescritos pelo verificador
(`--fix`) e conferidos por ele, então não ficam para trás de uma mudança numa linha.
**O que não sustenta**: conformidade a qualquer das três, nem prontidão para certificação.

## 5. Fontes citadas e não conferidas

Saem da contagem pela regra da Fase 0 (cláusula não conferida não conta), e o texto do TCC não as cita
com número de cláusula até que sejam conferidas:

- **AUD-02**: 524B(b)(2)
- **DAD-01**: LGPD
- **DAD-05**: LGPD
- **ATU-01**: 524B(b)(2)
- **ATU-02**: 524B(b)(2)
- **ATU-03**: 524B(b)(2)
- **INV-01**: 524B(b)(3)
- **INV-04**: 524B(b)(1)

Em `SystematicReview/normas/` e ainda não extraídos: RDC ANVISA 751/2022, RDC 848/2024 (só o art. 8º, §2º
foi conferido), RDC 657/2022, Guia ANVISA 38/2020 e Portaria Inmetro 384. Fora da pasta: IEC TR
60601-4-5 e IEC 62443-4-2, que são onde estão os controles de **produto** que a 81001-5-1 não lista.

## 6. Limitações declaradas

- **Independência dos testes**: o trabalho é de uma pessoa; a IEC 81001-5-1 §5.7.5 e a Tabela A.1 pedem
  departamento ou organização independente para os testes de mitigação e de intrusão.
- **Classe de segurança**: hipótese declarada (B para o EEG, C para a bomba), não resultado de uma
  análise de risco do fabricante (62304 §4.3; 14971 cl. 4).
- **Imagem de produção**: nunca construída; tudo o que foi medido é imagem de desenvolvimento (ACE-03,
  SIS-01).
- **Selo do SO**: o systemd 255 reprova arquivos íntegros depois do boot de provisionamento (AUD-02;
  `implementation_plan_conformity.md` §5 A8, decisão D11 em aberto).
- **Defesa em profundidade** (81001-5-1 §3.11) não é a arquitetura em camadas: as camadas do MedStack
  são de dependência, e os mecanismos de defesa as atravessam.
