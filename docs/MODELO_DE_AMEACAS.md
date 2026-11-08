# Contexto de segurança, interfaces e componentes de terceiros

> **Status**: 2026-11-07. Atende a três requisitos técnicos de `docs/CONFORMIDADE.md` que a IEC
> 81001-5-1 exige e o projeto não tinha: **THR-02** (contexto de segurança, §7.1.2, p. 30), **THR-03**
> (interfaces e fronteiras de confiança, §5.4.3, p. 23–24), **INV-05** (classificação dos
> componentes de terceiros, §4.3, p. 20, e A.3, p. 37) e, desde a mesma data, **THR-01** (o modelo de
> ameaças, §4.2, p. 20, e §7.2, p. 30–31: STRIDE sobre o fluxo de dados, na §4).
>
> **O que este documento não é**: uma análise de risco de segurança de um fabricante. É o
> levantamento técnico que esse processo usaria, feito sobre a plataforma e os três perfis
> (EEG, bomba de infusão simulada e tomógrafo como base).

---

## 1. Contexto de segurança (THR-02)

A IEC 81001-5-1 §7.1.2 pede os requisitos mínimos e as suposições sobre o ambiente: a) posição na rede;
b) segurança física ou cibernética provida pelo ambiente; c) isolamento de rede; d) impacto da
degradação da segurança na segurança do paciente; e) controles em hardware dedicado. Para cada perfil:

### 1.1 EEG na placa (STM32MP257, o produto-alvo)

| Item | Suposição ou fato | Consequência |
|---|---|---|
| a) Rede | A função clínica **não precisa de rede**. A placa tem Ethernet, e a imagem de desenvolvimento a usa (DHCP, `sshd`). A de produção (`med-image-eeg-prod`, 2026-11-08) não escuta em porta nenhuma até que uma concessão assinada abra o acesso de serviço, por tempo limitado (ACE-03) | Toda porta exposta é superfície sem função clínica. Medido na imagem de desenvolvimento: `22/tcp` (sshd) e `68/udp` (DHCP) (`attack-surface`, TST-01) |
| b) Segurança física | **Suposição declarada**: o equipamento fica numa sala clínica, e quem tem acesso físico a ele é pessoal da instituição. O operador alcança a tela, as portas USB, o cartão de memória e o conector do ST-LINK (serial e SWD) | Um atacante com acesso físico **não é mitigado**: o cartão sai da placa com o rootfs, a trilha e o `/data` cifrado com a chave de desenvolvimento **no mesmo cartão** (DAD-02), e o SWD dá depuração total dos dois núcleos (§2, linha 2) |
| c) Isolamento | Estação isolada, sem rede clínica e sem integração com prontuário | A troca de dados com outros sistemas (exportar o BDF+) é por mídia, fora do escopo |
| d) Impacto na segurança do paciente | Registro perdido, alterado ou atribuído a outro paciente → diagnóstico errado ou exame repetido; indisponibilidade → exame não feito. Classe B como hipótese (62304 §4.3) | A integridade e a autenticidade do registro são o ativo central (AUD-*, INT-*) |
| e) Hardware dedicado | O conversor pertence ao Cortex-M33, e o RIF tira do Linux o acesso ao SPI6, aos GPIOs e à interrupção do conversor (link `amp`, SEG-01). O OP-TEE está presente, mas não há fTPM (`implementation_plan_luks.md` §9) | O controle de hardware protege a **aquisição** de um Linux comprometido; não protege a chave de `/data` nem a trilha |

### 1.2 Bomba de infusão (simulada, QEMU)

Mesma plataforma e mesmas suposições de a), b) e c). Muda o impacto (d): um comando não autorizado ou
adulterado **muda uma taxa de infusão** (perigo H8 de `REQUISITOS_INFUSAO.md`), e a classe é C como
hipótese. O mecanismo simulado roda no Linux, então não há o controle de hardware de e). Esse é o
argumento da Fase 10 do plano da bomba.

### 1.3 Estação no PC (`meta-med-host`)

O contêiner do EEG num Ubuntu do usuário. **Não é a configuração de dispositivo médico**: `/data` não é
cifrado, e não há caminho de atualização além de um `.deb` novo (`host/README.md`). Fica fora deste
contexto.

---

## 2. Interfaces e fronteiras de confiança (THR-03)

A IEC 81001-5-1 §5.4.3 pede, para **cada interface física e lógica**: se é externa ou interna, se
cruza fronteira de confiança, quem e quais ativos ela alcança, papéis e permissões, ameaças,
controles (inclusive validação de entrada) e itens de terceiros envolvidos. Ameaças por letra do
STRIDE (Anexo C): **S**poofing, **T**ampering, **R**epudiation, **I**nformation disclosure, **D**enial of
service, **E**levation of privilege.

| # | Interface | Tipo | Fronteira? | Quem alcança | Ameaças | Controles existentes | Lacuna |
|---|---|---|---|---|---|---|---|
| 1 | Console serial (`ttyS0` no QEMU; VCP do ST-LINK na placa) | física, externa | sim | quem tem o cabo | S, E | na produção: nenhuma conta tem senha utilizável (`root:*`), e o login de root é recusado (`prod_image.py`); na de desenvolvimento, nenhum: root sem senha (`debug-tweaks`) | a imagem de desenvolvimento; a de produção não foi gravada na placa (ACE-03) |
| 2 | SWD/JTAG do ST-LINK | física, externa | sim | acesso físico | T, I, E | nenhum | a porta de depuração não é fechada (81001-5-1 §5.3.2 g)–h), §7.2 h)) |
| 3 | Ethernet + `sshd` em `22/tcp` | rede, externa | sim | a rede local | S, I, E | na produção: o `sshd` de fábrica mascarado; só escuta numa janela aberta por concessão (linha 20), para `med-service` com a chave concedida, sem senha, sem root, sem encaminhamento, cada conexão auditada (`service_access.py`); na de desenvolvimento, nenhum: root sem senha | a imagem de desenvolvimento; a chave de host muda a cada janela, e o técnico a confere pelo registro de auditoria, não por um `known_hosts` (ACE-03) |
| 4 | Cliente DHCP em `68/udp` | rede, externa | sim | a rede local | T, D | o `systemd-networkd` roda como `systemd-network` | — |
| 5 | Cartão de memória (rootfs, `med-boot`, `/data`) | física, armazenamento | sim | quem retira o cartão | I, T | LUKS2 em `/data` (DAD-01) | a chave de desenvolvimento está no mesmo cartão (DAD-02); este projeto não configura verificação do rootfs nem do kernel no boot (se a cadeia do ST autentica o FIP, não foi conferido) |
| 6 | USB: ponte do conversor (link `usb`) | física, externa | sim | quem pluga um dispositivo | T, D, E | o driver aceita só o VID/PID da ponte; autoteste do conversor no *probe*; o udev nomeia só o que passou nele | um dispositivo que imite o VID/PID é aceito pelo kernel |
| 7 | USB: mídia com pacote de atualização | física, externa | sim | o operador | T, S | assinatura e verity do pacote, chaveiro do aparelho, `check-purpose=codesign` (ATU-01) | a identidade de compatibilidade é por máquina, não por classe (ATU-02) |
| 8 | Tela e entrada do operador (HMI) | física, externa | sim | o operador | S, R | o operador é **afirmado**, e o registro diz isso (`affirmed_operator`, AUD-05) | nada verifica quem opera; a HMI roda como root (ACE-02) |
| 9 | `/run/medplatform/eeg-control.sock` | lógica, interna | sim (requisitante → serviço) | processos root ou do grupo do socket (`0660`) | S, T, R, E | codec com CRC e recusa de mensagem malformada; toda recusa é auditada | não lê a credencial do requisitante (ACE-01) |
| 10 | `/run/medplatform/eeg.sock` (amostras) | lógica, interna | sim | idem (`0660`) | I, D | canal não bloqueante: um leitor lento não freia a aquisição (`acq-sample-rate`); CRC por quadro | qualquer processo com acesso ao socket lê o sinal |
| 11 | `/run/medplatform/infusion-control.sock` | lógica, interna | sim | idem (`0660`, dono root) | S, T, R, E | DERS, codec, auditoria de cada comando | ACE-01; para a bomba é o perigo H8 |
| 12 | Sockets do mecanismo simulado (`command`, `data` `0660`; `fault` `0600`) | lógica, interna | sim | o controlador; a porta de falhas só root | T, D | homem-morto do lado do mecanismo; uma troca sem resposta encerra a sessão; a porta de falhas existe só no perfil simulado | — |
| 13 | `rpmsg` A35 ↔ M33 (`/dev/med-amp0`) | lógica, entre núcleos | sim | o serviço de aquisição (`DeviceAllow=char-rpmsg_char`) | T, D | CRC por quadro, sequência, resposta de controle obrigatória, ABI versionada (`med-amp-abi`) | os quadros não são autenticados: um root no A35 pode escrever na memória compartilhada |
| 14 | IIO do conversor (`/run/med-afe/eeg0`, links `spi`/`usb`) | lógica, kernel ↔ serviço | sim | o serviço de aquisição | T | cada configuração escrita é lida de volta; o passo de conversão é o declarado, e o texto do kernel o confere | — |
| 15 | D-Bus do mecanismo de atualização | lógica, interna | sim | processos com acesso ao barramento de sistema | T, E | política do D-Bus do RAUC (só root instala) | — |
| 16 | Trilha de auditoria (`/var/log/journal`) | armazenamento | sim | root | T, R | selo do journal (AUD-02, com o falso positivo do systemd 255) e cadeia verificável (AUD-03); desde 2026-11-07 em `/data`, fora dos slots A/B (AUD-06) | o selo do systemd 255 (AUD-02) |
| 17 | Registros clínicos em `/data` | armazenamento | sim | o serviço (escrita); `med-review` lê só o BDF+ publicado | I, T | LUKS2; `.sha256` ao lado de cada arquivo (INT-01); `EACCES` medido e injetado (DAD-05) | a chave no mesmo cartão (DAD-02) |
| 18 | Configuração selada (`/etc/medplatform/*.conf`) | armazenamento | sim | root | T | selo SHA-256 conferido ao carregar (INT-02); fora do envelope, recusa subir (INT-03) | o rootfs é gravável na imagem de desenvolvimento; na de produção é somente leitura desde o kernel (SIS-01) |
| 19 | Entrega da chave de verificação do selo (`/run/med-journal-seal/verify-key`) | lógica, interna | sim | root, só no boot de provisionamento | I | `tmpfs`, `0400`, apagada no reboot, nunca no journal | quem é root no primeiro boot a lê |
| 20 | USB: mídia com concessão de acesso de serviço (`med-service`) | física, externa | sim | quem pluga a mídia | S, T, E | CMS verificada contra o **chaveiro de atualização** (CA, CRL, `codeSigning`); `compatible` do aparelho; *nonce* gasto antes de abrir; duração limitada, contada a partir da aceitação e não do relógio; montada somente leitura, `nosuid,nodev,noexec`; recusa e concessão auditadas (ACE-03) | o kernel analisa o sistema de arquivos de uma mídia não confiável antes de qualquer verificação; a concessão vale para todos os aparelhos do mesmo `compatible`, não para um número de série |

**O que a tabela mostra, contado**: 20 interfaces, 9 físicas ou de rede e 11 lógicas ou de
armazenamento. Todas cruzam uma fronteira de confiança. **Uma não tem controle nenhum** (2, o SWD),
**quinze têm controle e lacuna declarada**, e **quatro não têm lacuna** (4, 12, 14 e 15). Até
2026-11-08 eram três sem controle: o console (1) e o `sshd` (3) ganharam controle com a imagem de
produção, e a mídia de concessão (20) entrou com ele. A fronteira mais fraca do produto é a física: o SWD e o cartão (linhas 2 e 5).

---

## 3. Componentes de terceiros: *maintained*, *supported*, *required* (INV-05)

A IEC 81001-5-1 §4.3 classifica cada item pela transferência de risco:
- ***maintained***: o fabricante assume o risco e **fornece atualizações de segurança** (§6.3.2, §6.3.3);
- ***supported***: o fabricante **notifica** o cliente sobre riscos conhecidos (§6.3.1);
- ***required***: o fabricante considera os riscos **conhecidos antes da liberação**, sem atualização
  (§5.2.1, §5.2.3).

O critério aqui é técnico: **é *maintained* o que o caminho de atualização de campo entrega**. Esse
caminho é o pacote A/B, que regrava um slot do rootfs e nada mais.

| Classe | O quê | Quantos | Por quê |
|---|---|---|---|
| ***maintained*** | todo pacote do rootfs: glibc, systemd, OpenSSL, Qt, BusyBox, o MedFramework, as aplicações, o EDFbrowser, os **módulos** do kernel e o firmware do M33 (`/lib/firmware`, carregado pelo remoteproc) | 240 pacotes no EEG, 210 na bomba, 208 no tomógrafo (`buildhistory`, 2026-11-05) | o pacote A/B os regrava inteiros |
| ***required*** | **a imagem do kernel e o *device tree*** (na `med-boot` da placa; na partição EFI do QEMU), o TF-A (`fsbla1/2`), o FIP com OP-TEE e U-Boot (`fip-a/b`), o ambiente do U-Boot, o GRUB do QEMU | os dois layouts em `meta-med-bsp/wic/` | **ficam fora dos slots A/B**, e nenhuma atualização de campo os escreve (`BOOT_SLOT_AB_STM32MP2.md`) |
| ***supported*** | o sistema do PC da estação (`meta-med-host`) e o DKMS dos drivers | — | não são entregues como parte do dispositivo |

**O achado**: o **kernel**, que é o componente de terceiros com mais vulnerabilidades publicadas e o
que mais precisaria de correção em campo, é, de fato, ***required***, e não ***maintained***. A IEC
81001-5-1 §6.3.2 pede atualização para todo item *maintained*, e a plataforma não consegue entregá-la
ao kernel. Há um segundo efeito, que é risco da própria medida (ISO 14971 §7.5): os **módulos** vão no
rootfs e são atualizados, e a **imagem** do kernel não. Um pacote construído para outra versão do kernel
traria módulos que o kernel em campo recusa carregar (`vermagic`), entre eles o driver do conversor. A
correção é um slot A/B também para `med-boot`/ESP, ou o kernel dentro do slot. Fica como dívida
declarada, ligada ao backlog do mecanismo de atualização (`implementation_plan_infusion_pump.md` §14.2).

---

## 4. Modelo de ameaças (THR-01)

A IEC 81001-5-1 §7.2 pede, quando aplicável: fluxo da informação, fronteiras de confiança, processos,
armazenamentos, entidades externas, protocolos, portas físicas e de depuração, conexões de placa
(JTAG), vetores de ataque contra o hardware, ameaças, questões identificadas e dependências de
terceiros. A §2 cobre as interfaces e as fronteiras, e a §3 as dependências. Esta seção junta o
resto. O método é o STRIDE (Anexo C), aplicado a cada interface da §2, e não substitui o processo de
risco de segurança de um fabricante: a revisão anual (§7.2), a estimativa de risco (§7.3) e a
aceitabilidade (§7.4) são dele.

### 4.1 Fluxo de dados

```
 eletrodos ─▶ conversor ─SPI─▶ Cortex-M33 ─rpmsg (13)─▶ serviço de aquisição ─▶ /data (17): sessão, BDF+
                                    ▲                         │  │                    ▲
                     (link usb: ponte USB (6) ─▶ IIO (14))    │  └─ eeg.sock (10) ─▶ HMI (8) ◀── operador
                                                              │                      │
                                         eeg-control.sock (9) ◀────── HMI, medctl ───┘
                                                              │
                                          journald ─▶ /var/log/journal em /data (16) ◀── cadeia (AUD-03)

 pacote de atualização (7, USB) ─▶ RAUC (15) ─▶ slot inativo      configuração selada (18) ─▶ serviços
 console (1), SWD (2), sshd (3), cartão (5): acesso direto ao Linux, ao M33 ou à mídia
```

A bomba troca o conversor e o M33 pelo mecanismo simulado (12) e o `eeg-control.sock` pelo
`infusion-control.sock` (11). O resto do fluxo é o mesmo.

### 4.2 Ativos e atores

| Ativo | O que se perde |
|---|---|
| **A1** registro clínico (sessão bruta, BDF+, metadados) | confidencialidade (dado de saúde), integridade (diagnóstico) |
| **A2** trilha de auditoria | rastreabilidade, não repúdio |
| **A3** configuração e calibração seladas, biblioteca de fármacos | exatidão da medição ou da dose |
| **A4** o software (rootfs, kernel, firmware do M33) | tudo o que depende dele |
| **A5** chaves: de `/data`, de verificação do selo; a de assinatura das atualizações fica fora do aparelho | A1, A2, A4 |
| **A6** a função: aquisição contínua, terapia | disponibilidade, e na bomba a segurança do paciente |

Atores: o **operador** (confiável, pode errar); alguém com **acesso físico** não autorizado; um
**processo comprometido** no Linux, por vulnerabilidade; um atacante na **rede local**; um **dispositivo
USB** malicioso; e a **cadeia de suprimento** (um pacote forjado).

### 4.3 Ameaças

Situação: **mitigada** (há controle, verificado), **parcial** (há controle, que não cobre a ameaça
inteira) ou **não mitigada**. RT é o requisito de `docs/CONFORMIDADE.md` que verifica o controle.

| ID | Interface | STRIDE | Ameaça | Ativo | Controle | RT | Situação |
|---|---|---|---|---|---|---|---|
| T01 | 7 | S, T | pacote de atualização forjado ou alterado | A4 | assinatura, verity, chaveiro do aparelho, `check-purpose=codesign` | ATU-01 | mitigada |
| T02 | 7 | T | pacote de outro aparelho ou de outra classe | A4, A6 | identidade de compatibilidade, por máquina | ATU-02 | parcial: não distingue classe |
| T03 | 7 | D | atualização interrompida deixa o aparelho inoperante | A6 | A/B atômico, retorno automático | ATU-03 | mitigada |
| T04 | 7 | D | versão nova quebrada é confirmada | A6 | slot confirmado só depois de `READY=1` | ATU-04 | mitigada |
| T05 | 7 | T, E | **atualização para uma versão anterior, com vulnerabilidade conhecida** | A4 | **nenhum**: o `system.conf` não tem *handler* de instalação, e nada recusa uma versão mais antiga | ATU-06 | não mitigada |
| T06 | 16 | T, R | registros da trilha alterados | A2 | cadeia verificável; selo do SO | AUD-03, AUD-02 | parcial: quem tem root recalcula a cadeia, e o selo tem falso positivo |
| T07 | 16 | R | **o fim da trilha apagado** | A2 | nenhum: uma cauda cortada parece uma cadeia que parou | AUD-03 | não mitigada |
| T08 | 16 | R | trilha perdida numa atualização | A2 | estado em `/data`, fora dos slots | AUD-06 | mitigada |
| T09 | 5, 17 | I | registros lidos com o cartão retirado | A1 | LUKS2 em `/data` | DAD-01, DAD-02 | não mitigada: a chave está no mesmo cartão |
| T10 | 17 | T | registro clínico alterado | A1 | `.sha256` ao lado do arquivo | INT-01 | parcial: o hash não tem chave, quem altera recalcula |
| T11 | 18 | T | configuração, calibração ou biblioteca de fármacos alterada | A3 | selo conferido ao carregar, `SafetyLimit` | INT-02, INT-03 | parcial: o selo é um hash sem chave, e o rootfs é gravável nas imagens medidas |
| T12 | 9, 11 | S, E | comando por um processo não autorizado | A6, A1 | permissões do socket (`0660`, root) | ACE-01 | não mitigada: na bomba, muda uma taxa (H8) |
| T13 | 8 | S, R | operador se passa por outro, ou nega o que fez | A2 | o operador é afirmado e rotulado assim | AUD-05 | não mitigada |
| T14 | 10 | I | sinal ao vivo lido por outro processo | A1 | permissões do socket | — | parcial |
| T15 | 13 | T | quadros forjados no `rpmsg` por um root no Linux | A1 | CRC e sequência protegem contra erro, não contra adversário; o conversor fica fora do Linux | SEG-01 | parcial |
| T16 | 6 | S, T | dispositivo USB que imita a ponte do conversor | A1 | VID/PID e autoteste no *probe* | — | parcial |
| T17 | 1, 3 | S, E | login pelo console ou pelo `sshd` | todos | na produção: nenhuma senha utilizável, `sshd` de fábrica mascarado, acesso só por concessão | ACE-03 | parcial: mitigada na imagem de produção do QEMU; a de desenvolvimento não tem o controle, e a de produção não foi gravada na placa |
| T18 | 2 | T, I, E | depuração pelo SWD, nos dois núcleos | todos | nenhum | ACE-03 | não mitigada |
| T19 | todas | E | vulnerabilidade conhecida explorada | A4 | o rootfs é atualizável pelo A/B; o kernel não (INV-05); `cve-check` desligado | INV-04, INV-05 | não mitigada |
| T20 | 9–15 | E | serviço comprometido escala privilégio | todos | sandbox do systemd | SEG-02, ACE-02, SIS-02 | parcial: 14 de 17 serviços rodam como root, sem MAC |
| T21 | 17 | D | disco cheio impede a sessão ou a atualização | A6 | reserva do volume | DAD-04 | mitigada |
| T22 | 10 | D | um leitor lento freia a aquisição | A6 | canal não bloqueante | INT-04 | mitigada |
| T23 | 12, 13 | D | mecanismo ou produtor mudo ou travado | A6 | homem-morto no mecanismo, alarme no controlador (bomba) | INF-09, INF-10 | mitigada no perfil da bomba |
| T24 | 19 | I | chave de verificação do selo lida no boot de provisionamento | A5 | `tmpfs`, `0400`, apagada no reboot | THR-04 | parcial |
| T26 | 20, 3 | S, E | **concessão de serviço forjada, alterada ou reaproveitada** | todos | CMS contra o chaveiro de atualização, `codeSigning`, CRL; *nonce* de uso único; duração limitada; cada passo auditado | ACE-03 | mitigada (QEMU: concessão de CA estranha e reuso recusados, `service_access.py`) |
| T25 | 5 | T | **rootfs ou kernel alterados no cartão** | A4 | **nenhum**: a verity protege o pacote de atualização, não o slot em uso, e nada verifica o kernel no boot | SIS-03 | não mitigada |

**Contagem**: 26 ameaças. **8 mitigadas** (T01, T03, T04, T08, T21, T22, T23, T26; a T08 só desde a
Fase 5.1, a T26 desde 2026-11-08), **10 parciais** e **8 não mitigadas** (T05, T07, T09, T12, T13, T18,
T19, T25). A T17 passou de não mitigada a parcial com a imagem de produção, e a T26 entrou com o
acesso de serviço. As não mitigadas se concentram em três vetores: o **acesso físico** (T09, T18, T25), a
**identidade de quem comanda** (T12, T13) e a **manutenção do software** (T05, T19). O modelo
revelou duas ameaças que nenhum requisito cobria, e que viraram requisitos novos: a atualização para
versão anterior (T05 → **ATU-06**) e a integridade do software em uso no boot (T25 → **SIS-03**).

