# Plano de implementação — atendimento aos requisitos técnicos das normas

> **Status**: plano, 2026-11-02. **Nada nele foi executado** além do levantamento da §5.
>
> **Horizonte**: defesa entre 20/11 e 27/11/2026. A janela de implementação é de cerca de **uma
> semana e meia** (até ~13/11), porque o texto precisa estar pronto antes da banca. Esse prazo
> manda na §8, a ordem de corte.
>
> **Onde o resultado mora**: a matriz e a tabela de requisitos vão para `docs/CONFORMIDADE.md`,
> criado na Fase 1. Os números medidos vão para `RESULTS.md`, numa seção nova, com o mesmo padrão
> das outras (comando, resultado e o que o número **não** significa).

---

## 1. O que este plano produz, e o que não produz

**Produz**: a demonstração, requisito por requisito, de que o software de plataforma implementa os
**controles técnicos** que as normas adotadas como base exigem (trilha de auditoria, proteção e
integridade dos dados, atualização segura, segregação, controle de acesso, inventário de software),
com a evidência de cada um e o nível dessa evidência.

**Não produz**: conformidade com a norma no sentido regulatório. A IEC 62304 e a ISO 14971 são
normas de **processo** do fabricante (plano de desenvolvimento, arquivo de gerenciamento de risco,
registros de verificação e liberação). Uma plataforma de software não é, sozinha, conforme ou não
conforme a elas.

**Vocabulário, decidido** (D1): o texto do TCC diz "atendimento aos requisitos técnicos" ou "o
requisito é habilitado pela arquitetura". Não diz "o sistema está conforme a IEC 62304", nem
"certificável".

---

## 2. Método: duas tabelas ligadas pelo identificador do requisito

```
cláusula da norma ──deriva──▶ requisito técnico (RT-xx) ──é atendido por──▶ mecanismo
                                                         ──é comprovado por──▶ evidência (nível)
```

1. **Matriz normativa** (de cima para baixo). Cada cláusula aplicável recebe uma categoria (D3),
   os RTs que derivam dela e uma justificativa de uma ou duas linhas. Ela existe para que a
   seleção das cláusulas seja **auditável**: as cláusulas de processo aparecem, classificadas como
   fora do escopo, em vez de omitidas.
2. **Tabela de requisitos técnicos** (de baixo para cima). Cada RT tem mecanismo, evidência, nível
   e situação.

A rastreabilidade nos dois sentidos é, ela mesma, a forma que a IEC 62304 pede: entre requisitos,
testes e medidas de controle de risco (62304 §5.1.1 c), p. 18) e do perigo até a verificação da
medida (62304 §7.3.3, p. 31). A estrutura do capítulo já é parte do argumento.

### Categorias de cláusula (D3)

| Cat. | Significado |
|---|---|
| **A** | Habilitada pela arquitetura, com evidência que falha sozinha se a propriedade quebrar (nível E2 ou E3) |
| **B** | Habilitada, com evidência por inspeção ou medição registrada (nível E1) |
| **C** | Parcial: o mecanismo existe e não cobre tudo o que a cláusula pede |
| **D** | Não atendida, declarada como limitação |
| **E** | Fora do escopo: processo do fabricante, não propriedade do software |

### Níveis de evidência de um RT (D4)

Os mesmos quatro níveis da proposta (§6.5), com o nível de injeção de falha separado:

| Nível | Significado |
|---|---|
| **E0** | Declarado: o código ou o documento existe, e nada o executou |
| **E1** | Construção e inspeção do artefato produzido (imagem, `.wic`, configuração instalada) |
| **E2** | Asserção automática sobre o sistema em execução (`make check`, testes de host) |
| **E3** | E2, e a asserção **foi vista falhar** com o defeito injetado |

A coluna **Placa** marca se o RT foi observado no STM32MP257. O motivo do E3 é a regra do
repositório: "uma asserção que nunca viu a falha que procura é uma afirmação, não uma verificação".

### Marcadores de cláusula (D2)

Todo número de cláusula neste plano está marcado como `[norma §x?]` até ser conferido no texto da
norma. É a mesma dívida que `[SBAS499?]` representa nos drivers: uma referência normativa não
conferida não vai para o texto final. A Fase 0 paga essa dívida, e depende do autor ter as
normas em mãos.

**Fase 0, 2026-11-07**: as três normas essenciais foram conferidas pelo autor, por leitura
integral, nas extrações `SystematicReview/IEC62304_extracao.md`, `IEC81001-5-1_extracao.md` e
`ISO14971_extracao.md`. Toda cláusula delas neste plano traz agora a **página impressa** da
edição lida. ◐ marca uma âncora que a extração classifica como indireta. Continuam com `?` só as
fontes que não estão em `SystematicReview/normas/`: LGPD, FD&C §524B e RDC 546/2021.
---

## 3. Normas e escopo

| Norma | Edição a confirmar | Papel aqui |
|---|---|---|
| IEC 62304 (ABNT NBR IEC 62304) | **2006 + AMD1:2015, ed. 1.1** (consolidada) ✅ | requisitos de software, inclusive os de segurança da informação e a trilha de auditoria (§5.2.2 e), p. 21); arquitetura e segregação (§5.3.1–5.3.6, p. 22–23); verificação (§5.5 unidade, §5.6 integração, §5.7 sistema); SOUP (§3.29, p. 14; §8.1.2, p. 32) |
| ISO 14971 | **2019, 3ª ed.** ✅ | ordem obrigatória das medidas de controle de risco, com o projeto primeiro (§7.1, p. 12–13); verificar a **implementação** e a **eficácia** de cada medida (§7.2, p. 13); riscos introduzidos pelas próprias medidas (§7.5, p. 14); perigos de dados e de segurança, como exemplos informativos (Tabelas C.1 e C.2) |
| IEC 81001-5-1 | **2021, ed. 1.0** ✅ | **norma de processo**: modelagem de ameaças (§4.2, §7.2), princípios de projeto seguro (§5.3.2), segregação sem restrição de classe (§5.3.3 a)), testes de mitigação e de vulnerabilidades (§5.7), integridade e autenticidade do que é entregue (§5.8.3, §6.3.3), lista reproduzível de componentes (cl. 8) |
| FD&C Act §524B | 2023 `[?]` | (b)(1) monitorar vulnerabilidades, (b)(2) atualizações e correções, (b)(3) SBOM. Texto não está em `normas/` |
| LGPD (Lei 13.709/2018) | — `[?]` | dado de saúde é dado sensível `[art. 5º, II?]`; medidas de segurança `[art. 46?]`. Texto não está em `normas/` |
| RDC ANVISA 751/2022 | ✅ em `normas/` (tradução oficial), não extraída | enquadramento e classificação; quase toda categoria E |
| RDC ANVISA 848/2024 | ✅ em `normas/`, art. 8º §2º conferido (`ISO14971_extracao.md` §12) | requisitos essenciais de segurança e desempenho; torna obrigatória a mesma ordem de prioridade das medidas da ISO 14971 §7.1, com alarmes como exemplo de proteção |

**Correções que a leitura das normas impôs** (Fase 0):
- **A IEC 81001-5-1 não lista controles técnicos.** Ela diz que seus requisitos "*do not specify
  PRODUCT capabilities*" (A.1, p. 35): exige que o fabricante modele ameaças, derive requisitos e
  os teste. Os controles de produto (cifragem, trilha, rootfs somente leitura) estão na IEC TR
  60601-4-5 e na IEC 62443-4-2, que não estão em mãos. Por isso várias âncoras antigas
  `[81001-5-1?]` caíram ou ficaram ◐, e por isso entram os requisitos novos da §4.7, que são de
  **processo**.
- **A trilha de auditoria se ancora na IEC 62304 §5.2.2 e)**, onde está escrita literalmente, e
  não na 81001-5-1, que só a menciona como fonte de relatos (§9.2 e)).
- **A IEC 81001-5-1 não usa as classes A/B/C** (A.2, p. 36). A classe B assumida para o EEG não
  reduz nenhuma obrigação de segurança da informação.
- **A ISO 14971 §7.1 é exigência, não orientação**: "*shall use [...] in the priority order
  listed*".
- **Independência dos testes** (81001-5-1 §5.7.5 e Tabela A.1): um trabalho feito por uma pessoa
  não a atende. Entra como limitação declarada.
- **"Defesa em profundidade" não é "arquitetura em camadas"** (81001-5-1 §3.11): as camadas do
  MedStack são de dependência. Os mecanismos de defesa atravessam as camadas.

Os requisitos essenciais da ANVISA ficam, agora, na RDC 848/2024 (que revogou a 546/2021 `[?]`,
a confirmar) e no Guia ANVISA 38/2020 de cibersegurança, ambos em `normas/` e ainda não extraídos.

**Classe de segurança do software (D5)**: a aplicabilidade de várias cláusulas da IEC 62304
depende da classe (A, B ou C, 62304 §4.3 a), p. 16). O plano assume **classe B** para o EEG de
diagnóstico: uma falha pode levar a diagnóstico errado, com lesão não grave. Essa é uma
**hipótese declarada**, não o resultado de uma análise de risco, e o texto tem de dizer isso.

---

## 4. Os requisitos técnicos

**Desde a Fase 1 (2026-11-07), a tabela viva está em `docs/CONFORMIDADE.md` §2**, e a matriz
normativa em §3 do mesmo documento. As tabelas que estavam aqui foram para lá, com as correções da
auditoria de evidência da Fase 1 (§10), para que não existam duas cópias que divirjam. A história delas
está no git.

`python3 scripts/med-conformity-check.py` (também em `make test`) confere as regras da Fase 1 e
imprime a contagem. Ela não é repetida aqui, para não envelhecer: o número vale o que o verificador
imprime, e a §4 do `CONFORMIDADE.md` é reescrita e conferida por ele (`--fix`).

---

## 5. Achados do levantamento (2026-11-02)

São achados de leitura do repositório e do artefato, não de execução, e são tratados como tal.

**A1. Toda a evidência de execução é de imagem de desenvolvimento.** O `med-image-dev.inc` faz
`MED_ROOTFS_FEATURES = ""` e acrescenta `debug-tweaks` e `ssh-server-openssh`. O `med-image-eeg` e o
`med-image-tomograph` o incluem. A imagem de produção (`med-image-prod.bb`) é a única com rootfs
somente leitura e sem login de root, e:
- nunca foi construída (CLAUDE.md, "Still unbuilt");
- não instala aplicação nenhuma: é a base sem o perfil de desenvolvimento;
- não existe variante de produção do EEG, embora a receita diga que "device profiles derive their
  production variants from this recipe".

Consequência: SIS-01 e ACE-03 estão em E0 e não podem ser afirmados. E há uma dificuldade de
método: sem login, o `make check` não consegue rodar numa imagem de produção (D6).

**A2. A cadeia de hash da auditoria não tem verificador.** O `MedicalLogger.h` diz que "um
verificador refaz a conta", e nenhum verificador existe no repositório (busca por `MED_AUDIT_HASH`
e `verify` em scripts, testes e receitas). O teste de host (`tests/framework/test_logger.cpp`)
confere que a cadeia avança, não que uma alteração é detectada. É "medir estado não é medir
função" aplicado à trilha de auditoria. O selo do journal (AUD-02) está um degrau acima: é
conferido (`journal-integro`), mas nunca foi visto falhar.
**Medido em 2026-11-05, e pior que "sem verificador"**: o formato 1 **não admitia** verificador. O
hash cobria as chaves do chamador (`session_id`) na ordem do chamador. O journal guarda a chave
normalizada (`MED_SESSION_ID`), sem inversa, e não preserva a ordem dos campos. Sobre 42 registros
reais do QEMU:

| Como refazer o hash | Verificam |
|---|---|
| campos na ordem do journal | 5 de 42 |
| campos em ordem alfabética | 3 de 42 |
| qualquer permutação (até 6 campos) | 21 de 42 |
| só o encadeamento (`PREV` = `HASH` anterior) | 40 de 40 possíveis |

O encadeamento era verificável e o conteúdo não, porque um campo alterado era indistinguível de
um campo reordenado. Além disso, a atribuição (`MED_DEVICE_ID`, `MED_SW_VERSION`) ficava fora do
hash. Corrigido pelo formato 2 (Fase 2.1, §10).

**A3. 12 dos 18 eventos de auditoria declarados são emitidos.** Busca por `AuditEvent::<nome>` nos
`.cpp`, excluindo o próprio logger:

| Emitidos (12) | Nunca emitidos (6) |
|---|---|
| SystemStart, SystemStop, SelfTestPassed, SelfTestFailed, ConfigurationChanged, AcquisitionStarted, AcquisitionStopped, PatientDataWritten, SafetyLimitViolation, DeviceFault, SecurityEvent, ElectrodeContactChanged | UpdateStarted, UpdateSucceeded, UpdateFailed, PatientDataExported, PatientDataRead, CalibrationChanged |

Leitura de cada ausência, para não contar errado:
- **Update\*** (3): uma atualização não entra na trilha estruturada. O RAUC registra no journal, mas
  fora dos campos e da cadeia do `MedicalLogger`. É lacuna real (ATU-05).
- **PatientDataExported**: a exportação **é** auditada, mas com o tipo errado. `export_main.cpp:172`
  emite `PatientDataWritten` ("clinical file written"). A correção é de uma linha. **Corrigido em
  2026-11-07** (Fase 2.3): o exportador emite `PatientDataExported`, e a asserção `export-audited` o
  confere; ela foi vista falhar na imagem anterior (`exported=0 misfiled=1`). O EEG passa a 13 de 18.
- **PatientDataRead**: o revisor (EDFbrowser) é de terceiros e não é modificado. A leitura não é
  auditada, e o máximo alcançável sem modificá-lo é registrar a abertura da sessão de revisão.
- **CalibrationChanged**: não existe caminho de mudança de calibração em execução, porque o
  armazém é selado na construção. A ausência é correta; o evento é declarado sem uso, e isso se
  diz em vez de se contar como lacuna.

**A4. O SBOM é gerado e nunca foi inspecionado** (INV-01). Último gerado: 2026-10-13.

**A5. Já conhecidos e confirmados**: a trilha fica dentro do slot A/B (AUD-06); `CapabilityBoundingSet=`
ausente em `eeg-acquisition.service` (SEG-02); socket de controle sem credencial (ACE-01).

**A6. O selo do journal nunca foi demonstrado.** `Seal=yes` (`10-journald-audit.conf`) só sela
quando existe uma chave gerada por `journalctl --setup-keys`, e nada no repositório executa esse
comando. A busca por `setup-keys`, `fss` e `verify-key` só encontra um comentário do
`med-check.py`. Ele mesmo registra que a asserção `journal-integro` mede integridade estrutural,
não o selo ("verifying seals needs `--verify-key=`, which the command never passed"), e que o
selo "is asserted by RESULTS.md §5 on other evidence". O `RESULTS.md` §5 diz que "o *Forward
Secure Sealing* valida". As duas afirmações não se sustentam juntas.
**Medido em 2026-11-02 no QEMU**: não existe `fss`, e nenhum dos 6 arquivos do journal traz `SEALED`
em `Compatible flags`. O `RESULTS.md` §5 foi corrigido no mesmo dia. Na placa não foi medido; o
esperado é o mesmo, pela mesma configuração.
**Corrigido em 2026-11-03/05** (antecipa D10 e parte da Fase 5.1, por decisão do autor): a receita
`med-journal-seal` (`meta-med-distro`) cria a chave no primeiro boot de cada `machine-id`, gira o
journal para que o arquivo novo nasça selado e entrega a chave de verificação uma única vez, num
arquivo em `/run` (tmpfs, `0400`). O `make check` a recolhe para `build/med-journal-keys/<machine-id>.key`
(asserção `journal-seal-key`) e verifica cada arquivo selado com ela e com uma chave errada
(`journal-sealed`). No boot de provisionamento as duas passam. Nos seguintes, ver A8.
**Feito em 2026-11-06** (`tests/infusion-target/inject_runtime.py`, num disco novo): as duas
asserções passam e reprovam com a falha injetada. `journal-sealed` reprova com uma chave errada no
host (3 de 3 selados recusados). `journal-seal-key` reprova num provisionamento sem testemunha,
com a chave fora do host e do `/run` do aparelho. E o byte alterado da Fase 2.2 foi detectado
(ver §10).

**A8. O selo do systemd 255 reprova arquivos que ninguém alterou.** Medido em 2026-11-03 no QEMU
(systemd 255.21, poky `70500d37`), no segundo boot depois do provisionamento: **2 de 5** arquivos
selados não verificam com a chave do dispositivo, sem que nada os tenha tocado e com desligamentos
limpos. Os arquivos que falham são sempre dos mesmos dois tipos, e os que foram girados no meio de
uma execução sempre passam:
- **o arquivo ativo no desligamento**, reaberto no boot seguinte: `Epoch sequence not continuous
  (0 vs 0)`. Lido no código: `journal-verify.c:1136` só aceita a mesma época em duas etiquetas
  seguidas quando o arquivo tem uma etiqueta só, e `journal_file_write_final_tag`
  (`src/shared/journal-file-util.c:387`) grava uma etiqueta a cada fechamento. Um arquivo fechado
  e reaberto dentro da mesma época de 15 minutos fica com duas etiquetas da mesma época, e o
  verificador o recusa. O `main` atual do systemd tem a mesma condição.
- **o arquivo que recebe o descarregamento de `/run` no boot**: `Tag failed verification` na
  etiqueta 1. **Hipótese**, coerente com o código e não provada: `journal_file_append_entry_internal`
  (`journal-file.c`, perto da linha 2348) troca o `seqnum_id` do cabeçalho quando o arquivo ainda
  não tem entradas, e esse campo entra no HMAC do cabeçalho (`journal-authenticate.c:323`) que a
  primeira etiqueta já cobriu.

Consequência: com o systemd desta versão, o selo **não distingue** um arquivo adulterado de um
desligamento comum. Um verificador que reprova a operação normal não serve de evidência, e
tolerar as duas mensagens no `make check` seria tolerar exatamente a mensagem que uma adulteração
produziria. O caminho está em D11.

**A7. Com o rootfs somente leitura, a imagem de produção perde a trilha de auditoria.** Previsto
por leitura, não observado, porque a imagem nunca foi construída. A política existe
(`med-image-prod.bb`), mas três coisas que o EEG precisa gravar moram no rootfs:
- `/var/log/journal`: o comentário de `10-journald-audit.conf` diz que o diretório é criado na
  construção "because the rootfs is read-only in production". Criar o diretório não o torna
  gravável. Num rootfs somente leitura o journald não consegue gravar ali e cai para `/run`, e a
  trilha some a cada boot.
- `/var/lib/medplatform` (`StateDirectory=` das duas units, `chainStatePath` do `MedicalLogger`):
  num rootfs somente leitura o systemd não cria o diretório e o serviço não sobe. Se `/var/lib`
  virar tmpfs, a cadeia recomeça a cada boot.
- `/etc/machine-id` vazio: num rootfs somente leitura o systemd usa um identificador transitório,
  diferente a cada boot, e ele nomeia o diretório do journal.

Mais dois riscos de construção, não verificados: scripts de pós-instalação que o
`read-only-rootfs` exige que rodem na construção, e o fato de não existir variante de produção do
EEG (só a base). E uma restrição do repositório: o `EDFbrowser` fica fora de `med-image-prod` por
decisão regulatória (CLAUDE.md), então `review-reach` não se aplica ao produto.

Isso transforma a antiga Fase 6 (trilha fora do slot) em **pré-requisito** da imagem de produção.
Os dois problemas têm a mesma solução: estado persistente num volume que nenhuma atualização
escreve.

---

## 6. Decisões

- **D1. Vocabulário** (§1).
- **D2. Marcadores `[norma §x?]`** até a Fase 0 (§2). Fase 0 feita em 2026-11-07 para as três normas essenciais.
- **D3. Categorias de cláusula A–E** (§2).
- **D4. Níveis E0–E3 + Placa** (§2). Um RT só conta como ✅ em E2 ou acima; em E1, só quando a
  propriedade é estática do artefato (exemplo: ATU-02).
- **D5. Classe B como hipótese declarada** (§3).
- **D6. Como verificar a imagem de produção sem login.** Em três camadas, nenhuma das quais
  precisa de shell no dispositivo:
  1. **Inspeção do artefato** (E1): montar o rootfs do `.wic` e conferir `ro` no `/etc/fstab`, a
     senha de root bloqueada em `/etc/shadow`, a ausência de `sshd`/`dropbear` e de pacotes de
     depuração no `buildhistory`.
  2. **Console serial durante o boot** (E2): o systemd imprime o estado de cada unit no console.
     Critério: nenhum `[FAILED]`, e o boot chega ao alvo final.
  3. **Disco lido depois do desligamento** (E2): desligar o QEMU de forma limpa (`system_powerdown`
     pelo monitor), abrir o volume `/data` no host com a chave de desenvolvimento da ESP, ler a
     trilha com `journalctl -D` e rodar o verificador da cadeia (Fase 2). Isso prova, sem login,
     que o produto gravou uma trilha íntegra e persistente.

  Rejeitada: uma variante "de produção com login para teste". Ela seria exatamente a contaminação
  que o `med-image-dev.inc` existe para evitar.
- **D7. Onde mora a auditoria de atualização.** Não na aplicação: atualização é plataforma, e a
  aplicação auditar a atualização seria vazamento de camada. Também não no `meta-med-distro`, que
  fica abaixo do framework e não pode usar o `MedicalLogger`. **Proposta**: um serviço do próprio
  `meta-med-framework` que, a cada boot, lê o estado do RAUC pelo `MedicalUpdate` (slot ativo,
  versão do bundle instalado, último erro) e emite `UpdateSucceeded` ou `UpdateFailed` quando ele
  mudou desde o último registro. `UpdateStarted` fica fora: o boot não vê o início de uma
  instalação, e para isso seria preciso um *hook* do RAUC. **A confirmar com o autor antes da Fase 2.**
- **D8. O estado persistente vai para `/data`.** `/var/log/journal` e `/var/lib/medplatform` passam
  a ser montagens de bind de diretórios em `/data`. Motivos: é cifrado (a trilha contém dado
  clínico indireto), nenhuma atualização o escreve (resolve AUD-06), e já existe. A ordenação é
  declarada, não presumida: o `systemd-journal-flush` usa `RequiresMountsFor=/var/log/journal`, e
  `/data` só monta depois do `med-data-provision`. Antes do flush, a trilha fica em `/run`, como já
  acontece hoje. Isso entra no `meta-med-distro` (política do SO, sem nome de máquina) e vale para
  desenvolvimento e produção. Assim os dois não divergem no ponto que se está medindo.
- **D9. `machine-id`.** A decidir na Fase 5 por experimento, não por leitura. Opções:
  (a) aceitar o transitório e conferir que `journalctl -m` lê todos os diretórios da trilha;
  (b) gravar um identificador por dispositivo no provisionamento, na mesma partição da chave, e
  entregá-lo ao systemd antes do boot (`systemd.machine_id=` na linha de comando). A (b) é a
  correta para o produto e custa mais, porque toca o bootloader de cada máquina (`meta-med-bsp`).
- **D10. Chave de selo do journal** (implementada, §5 A6). Gerada no primeiro boot de cada
  `machine-id` (`journalctl --setup-keys`). A chave de **selo** (`fss`) fica no dispositivo e evolui
  a cada época. A chave de **verificação** sai do dispositivo e nunca é gravada nele: ela regenera
  a chave de selo de qualquer época, então uma cópia onde o root alcança deixaria regravar e
  selar de novo a trilha. *Corrige a versão anterior desta decisão*, que a guardava na partição
  da chave de desenvolvimento e com isso anulava o selo. Ela vai para `/run` (tmpfs) só no boot de
  provisionamento e não entra no journal; quem não a recolher a perde no próximo boot, e as
  etiquetas daquela identidade ficam inverificáveis (provisionamento sem testemunha, falha
  correta). No QEMU quem recolhe é o `make check`; na placa, o operador pelo console serial. Uma
  identidade por slot A/B, enquanto D9 não decidir o contrário.
- **D11. O que fazer com o falso positivo do selo (A8).** *A decidir.* Opções:
  (a) *patch* no systemd, em `meta-med-distro`, que corrija o lado que grava ou o que verifica,
  para que a trilha verifique num `journalctl` sem modificação, e relato ao upstream;
  (b) declarar o selo do SO como limitação e apoiar AUD-02 na cadeia do `MedicalLogger`
  (AUD-03), com verificador;
  (c) as duas, nessa ordem: primeiro o verificador da cadeia (Fase 2.1, que é preciso de qualquer
  forma), depois o *patch*. **Recomendada: (c).** Enquanto não se decide, `journal-sealed` continua
  reprovando nos boots seguintes ao provisionamento, de propósito: é o estado verdadeiro.
- **Construção por parâmetro, não por alvo novo.** A imagem `med-image-eeg-prod` se constrói por
  `make qemu IMAGE=med-image-eeg-prod` (a conferir: o `Makefile` faz `IMAGE := med-image-eeg`, e uma
  variável da linha de comando vence `:=` no make). **Não usar `make parse`** numa configuração
  nova: ele roda `bitbake -n`, que apaga o `.wic` já gerado.

---

## 7. Fases

Cada fase tem critério numérico, injeção de falha e onde o número é registrado.

### Fase 0 — conferir as cláusulas `[precisa das normas]` · ~0,5 dia

Transcrever, de cada norma, as cláusulas que a §4 cita e as que ela omitiu. Classificar cada uma
de A a E. Remover os marcadores `?` conferidos.
**Critério**: zero marcadores `[…?]` em `docs/CONFORMIDADE.md`. Cláusula não conferida não entra na
contagem final.

### Fase 1 — `docs/CONFORMIDADE.md` · ~1 dia

Matriz normativa + tabela de RTs final, com a contagem por categoria e por nível.
**Critério**: todo RT cita pelo menos uma cláusula; toda cláusula da matriz cita pelo menos um RT
ou é categoria E; toda evidência aponta um arquivo, uma asserção ou uma seção do `RESULTS.md` que
existe.

### Fase 2 — auditoria · ~1,5 dia

1. **Verificador da cadeia** (AUD-03): `scripts/med-audit-verify.py`, que lê `journalctl -o json`
   e refaz `MED_AUDIT_HASH = SHA-256(MED_AUDIT_PREV ‖ registro)`, na ordem de `MED_AUDIT_SEQ`.
   A forma exata do registro sai do `MedicalLogger.cpp`, não desta linha. Ele entra no
   `make check` como asserção `audit-chain`.
   **Injeção**: alterar um campo de um registro exportado, apagar um registro, trocar dois de ordem;
   cada um tem de falhar na posição certa. Uma quebra legítima (reinício da cadeia depois de
   reboot, `MedicalLogger.cpp:180`) não pode ser contada como adulteração: verificar como o
   código a distingue antes de escrever o verificador.
2. **Selo visto falhar** (AUD-02): numa cópia de um arquivo `.journal` **selado**, alterar um byte e
   rodar `journalctl --verify --verify-key= --file=`. **Critério**: `FAIL`. Registrar no `RESULTS.md`
   §5. Mais as injeções das asserções do selo (chave ausente do host; chave errada já é testada a
   cada execução). Pelo A8, este item só prova algo depois de D11: hoje um `FAIL` aparece também
   sem adulteração.
3. **`PatientDataExported`** no exportador, no lugar de `PatientDataWritten`.
4. **Auditoria de atualização** (ATU-05), conforme D7, se D7 for aprovada.
   **Injeção**: um `rauc install` com bundle recusado tem de produzir `UpdateFailed`.

**Critério da fase**: eventos emitidos sobem de 12/18 para 16/18 (Update\* sem `Started` = +2,
`Exported` = +1, `Started` = +1 se houver hook), com a leitura de cada ausência restante escrita.
`make check` continua verde, com a asserção nova.

### Fase 3 — confinamento · ~0,5 dia

`CapabilityBoundingSet=` (vazio ou o mínimo para `SCHED_RR`, que precisa de `CAP_SYS_NICE`
se o serviço não for root: conferir na unit), `UMask=0077`, `ProtectProc=invisible`,
`ProcSubset=pid`.
**Critério**: exposure medido antes e depois, no `RESULTS.md` §4; `acq-realtime` e
`acq-sample-rate` continuam verdes (o confinamento não pode tirar o tempo real).
**Injeção**: o limiar de `sandbox-exposure` baixa para o valor novo, e reverter uma diretiva tem de
fazê-lo falhar.

### Fase 4 — inventário · ~0,5 dia

Abrir o SPDX do `med-image-eeg`. Contar os pacotes, quantos têm licença declarada (não
`NOASSERTION`) e quantos têm versão e origem.
**Critério**: os números no `RESULTS.md`, numa seção nova, com o comando. Opcional, se sobrar
tempo: uma rodada única de `cve-check` como medição (INV-04), com o número de CVEs abertos
declarado e **não** tratado.

### Fase 5 — imagem de produção do EEG · ~4 dias, só QEMU

A política já existe (`med-image-prod.bb`). O que falta é o perfil do EEG sobre ela e o estado
persistente que um rootfs somente leitura exige (§5, A7). Esta fase absorve a antiga Fase 6
(trilha fora do slot), porque as duas têm a mesma solução (D8).

**5.1 Estado persistente em `/data`** (D8, D10) · ~1,5–2 dias. Antes da imagem de produção, e
validado na de **desenvolvimento**, onde há shell para ver o que acontece:
- `/var/log/journal` e `/var/lib/medplatform` passam a ser montagens de bind de `/data`;
- ~~a chave de selo é gerada no provisionamento (D10)~~ feito em 2026-11-03 (§5 A6); falta só
  que o diretório da chave, `/var/log/journal/<machine-id>/`, passe a morar em `/data` com o resto.

**Critério**: `make check` verde; a trilha de antes de um `rauc install` aparece depois de
bootar no outro slot (AUD-06); `journalctl --verify --verify-key=` passa com o selo (AUD-02).
**Injeção**: sem o bind, `journal-persistent` tem de falhar (a asserção muda para conferir que
`/var/log/journal` está em `/data`, não só que é diretório); um byte alterado num arquivo selado
tem de dar `FAIL` com a chave.

**5.2 `machine-id`** (D9) · ~0,5 dia. Experimento com a opção (a). A (b) só se a (a) falhar.

**5.3 Receita** · ~0,5 dia. Separar a lista de instalação do EEG num `.inc` comum, para que
`med-image-eeg` (desenvolvimento) e `med-image-eeg-prod` (produção) difiram **só pela base**, a
mesma disciplina do caso de controle (`med-image-dev.inc`). A receita nova fica no
`meta-med-app`, sem `edfbrowser`/`med-review`.
**Critério**: o `diff` de pacotes entre as duas contém apenas ferramentas de desenvolvimento e o
revisor. Esse `diff` é um número para o `RESULTS.md`: o que o produto deixa de ter.

**5.4 Construir e corrigir** · ~0,5–1 dia, incerto. Os defeitos que só a construção mostra
(pós-instalações, unidades que escrevem no rootfs). Cada um entra no registro da §10.

**5.5 Verificar** (D6) · ~1 dia. As três camadas: artefato, console e disco depois do
desligamento, com o verificador da cadeia.
**Critério**: SIS-01 e ACE-03 em E1 (artefato) e o boot sem unidade falha em E2. A trilha do produto
foi lida e verificada fora dele, e o serviço de aquisição subiu: o registro `SystemStart` está na
trilha, e `/data` cifrado é a pré-condição para ele existir.

**Fora desta fase**: a imagem de produção na placa. No STM32MP257 o arquivo de projeto declara
`MED_DATA_KEY_SOURCE = "tpm2"`, que recusa provisionar; ela fica para depois do TCC.

### Fase 7 — contagem e redação · ~0,5 dia

Recontar a tabela final, escrever a seção do `RESULTS.md` e o capítulo.

**Total**: Fases 0–4 e 7 ≈ 4,5 dias; Fase 5 ≈ 4 dias. **Cerca de 8,5 dias úteis**, contra uns 9
disponíveis entre 03/11 e 13/11. Não sobra margem, e a §8 é o que se corta.

Ordem de execução: **2 → 5.1 → 3 → 5.2–5.5 → 4 → 0 → 1 → 7**. O verificador da cadeia (Fase 2) vem
antes porque a Fase 5 o usa. A 5.1 vem logo depois porque é a de maior risco, e descobrir cedo que
ela falha deixa tempo para cortar. As Fases 0 e 1 (redação) podem andar em paralelo, porque
dependem das normas e não do build.

---

## 8. Ordem de corte

Se o prazo apertar, corta-se de baixo para cima:

1. `cve-check` da Fase 4.
2. Auditoria de atualização (Fase 2, item 4). O resto da Fase 2 fica.
3. Opção (b) do `machine-id` (5.2): fica a (a), com a limitação declarada.
4. Fase 3 (confinamento): SEG-02 fica parcial, com o motivo.
5. Camada 3 da verificação de produção (5.5, disco depois do desligamento): ficam artefato e
   console, E1 e E2 sem a trilha lida.
6. Fases 5.2–5.5 inteiras: a 5.1 fica (ela resolve AUD-06 e AUD-02 também no desenvolvimento), e a
   imagem de produção vira limitação declarada em E0.

**Não se corta**: as Fases 0, 1, 2.1, 2.2, 5.1 e 7. Sem elas não há contagem defensável, e a trilha
de auditoria, o requisito mais citado, continuaria sem verificação e sem sobreviver a uma
atualização.

**Atualizado em 2026-11-02**: o segundo perfil (bomba de infusão,
`implementation_plan_infusion_pump.md`) passou **à frente** deste plano, por decisão do autor
(§11.1 daquele plano). Daqui em diante, todo commit deste plano termina com a linha
`Profile: conformity`, para que a contagem por camada da bomba possa excluí-lo. A exceção é a
conferência do selo (§5, A6), que vem antes de tudo e não muda código.
**Atualizado em 2026-11-05**: a conferência virou correção ("Quero corrigir isso"): a geração da
chave (D10) foi implementada antes da bomba e vai num commit próprio com `Profile: conformity`. O
falso positivo que ela revelou (A8) fica para D11; a bomba segue antes dele.

---

## 9. O que o TCC pode afirmar em cada estado

| Estado | Afirmação defensável |
|---|---|
| Hoje | "*N* requisitos técnicos derivados de seis normas foram identificados; *k* têm evidência automática." Sem números de cláusula. |
| Fase 0 + 1 | A contagem por categoria e por nível, com as cláusulas conferidas. |
| Fase 2 | "A integridade da trilha de auditoria é verificada, e a verificação foi vista detectar alteração, remoção e reordenação." |
| Fase 5.1 | "A trilha de auditoria sobrevive a uma atualização A/B e é selada", com o comando que mostra cada parte. |
| Fase 5 inteira | "A imagem de produção do EEG foi construída e executada no alvo emulado; suas propriedades foram verificadas no artefato, no boot e na trilha que ela gravou." **Não** "no alvo físico". |

**Em nenhum estado**: "o sistema está conforme a IEC 62304", nem "pronto para certificação".

---

## 10. Registro de execução

- **2026-11-08**, `cve-check` (INV-04, o opcional da Fase 4): uma rodada sobre `med-image-eeg`, com a
  configuração fora do repositório (`bitbake -R build/cve.conf`). 120 receitas; **892 CVEs sem patch**,
  778 deles do kernel (um teto: o `cve-check` não olha a configuração do kernel) e 114 em 18 receitas;
  17.867 corrigidos e 35 ignorados (`RESULTS.md` §12). Nada foi tratado. INV-04 passa a ◐ E1, e as duas
  cláusulas que estavam em D passam a C. **Contagem**: 44 RTs (28 ✅, 9 ◐, 7 ✗); cláusulas técnicas
  A 18, B 6, C 16, **D 0**. Custo medido: ligar a classe muda a assinatura das tarefas, e a construção
  recompilou boa parte da imagem (Qt inclusive); com o download do NVD sem chave, foram mais de 2 h.
- **2026-11-08**, **Fase 7** (contagem e redação): `RESULTS.md` §14 registra o estado final e a
  evolução (D 10 → 4 → 2). Contagem final: 44 RTs, 28 ✅, 8 ◐, 8 ✗; 40 cláusulas técnicas, A 18,
  B 6, C 14, D 2. **As duas em D dependem só do INV-04**, então a rodada opcional de `cve-check`
  (primeiro item da ordem de corte) é o que ainda as tiraria de D. No capítulo da dissertação
  (`Dissertacao_PTBR/02-elementos-textuais/`), entraram só seções e comentários de esboço, no
  estilo dos que já existem: o texto corrido é do autor. **O plano está concluído**, exceto pelo
  que depende de decisão do autor: D7 (e com ela o ATU-05), D9 (b) e D11.
- **2026-11-08**, **Fases 5.2–5.5** (imagem de produção do EEG, QEMU). **5.3**: a lista do EEG foi
  para `med-image-eeg.inc`; `med-image-eeg` (desenvolvimento) e `med-image-eeg-prod` (sobre
  `med-image-prod.bb`) diferem pela base e pelo revisor. `make qemu IMAGE=med-image-eeg-prod`
  constrói (o alvo passou a respeitar `IMAGE`). **Diferença de pacotes**: 240 → 215; saem os 25 do
  desenvolvimento: `openssh*` (6 pacotes e o *packagegroup*), o revisor (`med-review`,
  `edfbrowser`, `desktop-file-utils`, `shared-mime-info-data`), as ferramentas de verificação
  (`systemd-analyze`, `procps*`, `util-linux-chrt`, `-taskset`, `libproc2`, `ncurses`), e o que o
  `read-only-rootfs` dispensa (`base-passwd`, `shadow`, `update-rc.d`, `update-alternatives-opkg`).
  Root fica `root:*` no `/etc/shadow` do artefato. **5.4, dois defeitos, da mesma causa**: o
  `runqemu` acrescenta ` rw` ao `QB_KERNEL_ROOT`, então o kernel montava gravável a raiz de uma
  imagem somente leitura. (1) O `systemd-remount-fs` falhou num boot de quatro, e o motivo só
  apareceu lendo o journal do produto depois de desligado: `mount: /: mount point is busy`. (2) No
  primeiro boot, com a raiz ainda gravável, o systemd gravou um `machine-id` **permanente** no
  rootfs: aquele disco tinha 1 diretório de journal para 4 boots, e o disco corrigido tem um por
  boot, com `Initializing machine ID from random generator` e `Installed transient
  /etc/machine-id` no console. Correção: `QB_KERNEL_ROOT:append = " ro"` quando a imagem é
  `read-only-rootfs` (`med-image-base.bb`), o mesmo que o poky faz com o `APPEND` do bootloader;
  o dispositivo continua sendo resposta do `meta-med-bsp`. **5.5**: `tests/conformity/prod_image.py`
  verifica sem shell no produto. Console: boota, nenhuma unidade falha, o serviço de aquisição
  sobe, root é recusado. Desliga pelo botão de energia (ACPI, `system_powerdown`). Depois, um
  sistema de desenvolvimento recebe o disco do produto como pendrive USB somente leitura, abre o
  `/data` com a chave do próprio disco, lê a trilha com `journalctl -D` e a confere com
  `med-audit-verify`. **8/8 em 4 execuções seguidas** sobre um disco novo: `SystemStart` em cada
  boot, cadeia sem quebra através dos boots. **5.2 (D9), resultado do experimento (a)**: com a raiz
  realmente somente leitura, o `machine-id` muda a cada boot. O `journalctl -D` lê todos os
  diretórios, e a cadeia do `MedicalLogger` não depende dele (o estado dela está em `/data`).
  **Mas o selo do SO fica inverificável por construção**: o `med-journal-seal` gera chave por
  `machine-id`, então gera uma por boot, e a de verificação de cada uma se perde em `/run`. Na
  produção, o AUD-02 só se sustenta com a opção (b).
  **Acesso de serviço** (política decidida pelo autor em 2026-11-08, ACE-03 reescrito): nenhuma conta
  com senha utilizável; o `sshd` de fábrica mascarado; uma concessão CMS assinada com o **chaveiro de
  atualização** (as mesmas regras do RAUC: `codeSigning`, CRL), numa mídia USB rotulada
  `med-service`, abre por uma duração contada desde a aceitação (não pelo relógio) um `sshd` próprio,
  para `med-service` e a chave concedida, com *nonce* de uso único. Concessão, recusa, cada conexão
  e o fechamento são registros `SecurityEvent` da cadeia do `MedicalLogger`
  (`meta-med-framework/recipes-framework/med-service-access`). Host: 39 verificações, 4 injeções.
  QEMU: `tests/conformity/service_access.py` 15/15. **Injeção**: sem a máscara do `sshd` de fábrica,
  8 asserções reprovaram, todas previstas; uma nona prevista passou, e o defeito era do predicado
  ("não grava no rootfs" aceitava `denied`, que um login recusado também imprime), corrigido e
  revalidado em 15/15. Um defeito de construção: o `useradd` falhou porque o grupo
  `systemd-journal` ainda não existia (sem `RDEPENDS` em `systemd`, o opkg roda a pré-instalação
  antes). **Contagem**: 44 RTs (28 ✅, 8 ◐, 8 ✗); cláusulas técnicas A 18, B 6, C 14, D 2. Fora:
  a placa, o SWD, e a identidade por aparelho na concessão (vale por `compatible`).
- **2026-11-02**, A6: selo medido ausente no QEMU; `RESULTS.md` §5 corrigido.
- **2026-11-07**, **Fase 4** (INV-01, INV-02): o SBOM aberto e contado pela primeira vez
  (`scripts/med-sbom-report.py`, `RESULTS.md` §12). São 240 de 240 pacotes com documento, licença, versão e
  fornecedor, e **0 com a origem da fonte**; 119 de 126 receitas têm CPE. INV-01 ✅. O INV-02 fica ◐,
  porque o fornecedor é `OpenEmbedded` em todos, não o fabricante. O `cve-check`, opcional, não foi
  rodado (primeiro item da ordem de corte, §8).
- **2026-11-07**, **Fase 3** (SEG-02): o serviço de aquisição sem capacidade nenhuma, mais `UMask`,
  `ProtectProc`, `ProcSubset` e `IPAddressDeny`. *Exposure* de 3.7 para 1.3, com `SCHED_RR` 50 e a taxa
  mantidos; `make check` 43/43. O limite de `sandbox-exposure` baixou para 1.3, e um *drop-in* que
  devolve as capacidades o faz reprovar (3.3). O serviço ainda roda como root (ACE-02).
- **2026-11-07**, **THR-01 e Fase 2.3**: o modelo de ameaças (`MODELO_DE_AMEACAS.md` §4, 25 ameaças) e
  os dois requisitos que ele revelou (ATU-06, SIS-03). O exportador passou a emitir
  `PatientDataExported`, com a asserção `export-audited`, vista falhar antes da correção. `make
  check` do EEG: 43/43.
- **2026-11-07**, **Fase 5.1, primeira metade** (D8): `/var/log/journal` e `/var/lib/medplatform` são
  montagens de bind de `/data/state/{journal,medplatform}`. Elas são criadas, e na primeira vez
  copiadas do rootfs, por `med-state.service`, depois do `data.mount`; tudo no `med-data-volume`, na
  camada de SO. A ordem com o journal não precisou ser forçada: o `systemd-journal-flush` já exige
  `/var/log/journal` montado, e a trilha fica em `/run` até lá. **AUD-06 demonstrado no QEMU** pelo
  que uma atualização faz ao disco (`tests/conformity/slot_rewrite.py`): o slot A foi regravado com
  um rootfs limpo entre dois boots, o `machine-id` mudou, os 42 registros de antes continuaram, e a
  cadeia seguiu de 42 para 45 com 0 quebras e 0 reinícios. `journal-persistent` (reescrita) e
  `state-persistent` (nova) foram vistas falhar numa imagem sem o bind. `make check` do EEG:
  **42/42**. Ficam para a outra metade: a chave de selo (D10, já feita em 2026-11-03), o
  `machine-id` (D9) e a imagem de produção. **Erro registrado**: a primeira versão das duas
  asserções usava `findmnt`, que não existe nesta imagem. Elas reprovaram tanto na imagem sem o
  bind quanto **na imagem com ele**, e a primeira reprovação foi lida como a injeção de falha. Ela
  não provava nada, porque o comando ausente reprovaria qualquer imagem. Agora as asserções leem o
  `/proc/self/mountinfo`, e a injeção foi refeita. **Achado para a Fase 5**: num rootfs somente
  leitura, o `var-volatile-lib.service` do OE monta um tmpfs sobre `/var/lib`; a montagem do estado
  foi ordenada depois dele, para não ser coberta. Na placa, não medido.
- **2026-11-07**, os quatro RTs baratos da §4.7: **THR-02, THR-03 e INV-05** em
  `docs/MODELO_DE_AMEACAS.md` (contexto de segurança por perfil; 19 interfaces, 3 sem controle
  nenhum, 12 com lacuna, 4 sem lacuna; *maintained*/*required*/*supported*), todos ✅ em E1. **TST-01**
  é a asserção nova de plataforma `attack-surface` (toda porta fora do loopback declarada, com motivo),
  vista falhar no alvo com uma porta não declarada aberta: ✅ em E3. As asserções de plataforma
  passam de 13 a 14. O ACE-02 ganhou a medida: 14 de 17 serviços rodam como root na imagem da bomba.
  **Achado do INV-05**: o kernel, o *device tree* e a cadeia de boot ficam fora dos slots A/B nos dois
  alvos, então o kernel é *required*, não *maintained*. Os módulos vão no rootfs e a imagem do kernel
  não, e um pacote para outra versão do kernel traria módulos que o kernel em campo recusaria. O
  verificador ganhou a quinta regra (a contagem escrita bate com as tabelas) e `--fix`, vistos
  falhar. **Contagem**: 42 RTs, 22 ✅, 10 ◐, 10 ✗; das 40 cláusulas técnicas, A 14, B 3, C 16, D 7.
- **2026-11-07**, **Fase 1**: `docs/CONFORMIDADE.md`, gerado da tabela do §4 e conferido por
  `scripts/med-conformity-check.py`. O verificador confere quatro regras: todo RT cita uma cláusula
  conferida; toda cláusula nomeia um RT ou é E, nos dois sentidos; a categoria é **calculada** dos
  RTs e não escolhida; e toda evidência acima de E0 aponta uma asserção, seção do `RESULTS.md` ou
  arquivo que existe. Quatro injeções no documento, uma por regra, foram vistas, e cada uma pegou
  só a sua. **A auditoria de evidência corrigiu a tabela**: AUD-05 caiu de E2 para E1 e TMP-01 de E2
  para E0, porque nenhuma asserção os confere; ACE-01 caiu de E2 para "—", porque não há mecanismo;
  AUD-04 subiu para E3, pela suíte da bomba; o TMP-01 ganhou uma âncora interpretativa (◐), porque
  não citava cláusula nenhuma; o AUD-03 citava "idem". Também ganharam ponteiro de evidência
  INT-03, ATU-04, ACE-02, ACE-03, SIS-01, SIS-02, INV-01, INV-02, INV-03, THR-04 e INV-06.
  **Contagem**: 42 RTs (18 ✅, 11 ◐, 13 ✗; E3 11, E2 9, E1 7, E0 5, sem evidência 10); 67 cláusulas,
  das quais 40 técnicas: **14 A, 1 B, 15 C, 10 D**; 27 são processo do fabricante (E).
- **2026-11-07**, **Fase 0**: as origens dos 34 RTs foram trocadas pelas cláusulas conferidas nas
  extrações do autor, com página. As âncoras fracas foram substituídas ou marcadas ◐: AUD-01,
  AUD-04 e ATU-05 passaram da 81001-5-1 e da 14971 para a 62304 §5.2.2 e); SEG-03 de 62304 §5.3
  para §5.2.2 a) e §5.5.4 c); SEG-04 de §5.3.1 para §5.3.6; INV-02 perdeu "licença", que a 62304
  não exige. Entraram 8 RTs de processo que a 81001-5-1 exige e o plano não tinha (§4.7), e o §3
  ganhou as correções que a leitura impôs. Ficam com `?` só a LGPD, o §524B e a RDC 546, que não
  estão em `normas/`. A matriz normativa com as categorias A–E é a Fase 1.
- **2026-11-06**, Fase 2.2 (AUD-02) e as injeções do selo: numa cópia de um arquivo selado que
  verifica, **um** byte alterado dentro de um `MESSAGE=`. Isso foi provado antes do veredito:
  tamanhos iguais, primeira diferença na posição 947.137 e o resto com o mesmo md5.
  `journalctl --verify --verify-key=` dá `FAIL: ... File corruption detected` (rc=1), e o original
  continua `PASS`. **Ressalva**: a cópia também falha **sem** a chave (`nokey_rc=1`), pelo hash do
  objeto de dados. O teste prova que a adulteração é detectada, não que foi o selo que a detectou.
  O selo é o que resta contra quem altera o conteúdo e recalcula os hashes, e esse caso não foi
  testado. Três tentativas anteriores deram veredito sobre uma cópia que **não** tinha um byte
  só alterado: o `dd` do BusyBox não aceita `conv=notrunc`, o `head` dele não aceita `-c`, e uma
  linha de mais de 1024 caracteres travou o `ash`. A prova de "exatamente um byte" passou a
  condicionar o veredito.
- **2026-11-05**, Fase 2.1 (AUD-03): formato 2 da cadeia (`src/AuditChain.h`). O hash agora cobre
  `MESSAGE` e todo `MED_*` exceto o próprio `MED_AUDIT_HASH`, em ordem alfabética e com o comprimento
  de cada valor como prefixo. O `MedicalLogger` recusa dois campos com o mesmo nome no journal.
  Verificador `med-audit-verify` (lê `journalctl -o export`, sem libsystemd, no pacote
  `med-framework-api-audit-verify`, instalado pelo `packagegroup-med-framework`). Host: 46
  verificações, com o vetor de referência calculado pelo `hashlib` do Python. Injeções: aceitar
  qualquer conteúdo (5 falhas), não conferir a sequência (2), tirar o prefixo de comprimento (2,
  pegas **só** pelo vetor independente). Alvo: `audit-chain`, nova no `make check`, 40/40 no
  primeiro boot; no segundo, 43 registros em 2 cadeias, todos verificados, e a cópia com um
  `MESSAGE` alterado recusada (39/40, só `journal-sealed`, pelo A8).
- **2026-11-03**, D10: `med-journal-seal` implementado; `make check` 38/39 no boot de provisionamento
  com `journal-seal-key` e `journal-sealed` verdes (a falha foi `journal-integro`, que verificava sem
  chave um arquivo selado; o predicado foi corrigido). No boot seguinte, 38/39 com `journal-sealed`
  reprovando 2 de 5 arquivos (A8). Injeções de falha não feitas.
