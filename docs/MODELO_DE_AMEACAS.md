# Contexto de segurança, interfaces e componentes de terceiros

> **Status**: 2026-11-07. Atende a três requisitos técnicos de `docs/CONFORMIDADE.md` que a IEC
> 81001-5-1 exige e o projeto não tinha: **THR-02** (contexto de segurança, §7.1.2, p. 30), **THR-03**
> (interfaces e fronteiras de confiança, §5.4.3, p. 23–24) e **INV-05** (classificação dos
> componentes de terceiros, §4.3, p. 20, e A.3, p. 37). O **modelo de ameaças** em si (THR-01, §4.2 e
> §7.2: STRIDE sobre o fluxo de dados) parte das tabelas abaixo e ainda não está feito.
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
| a) Rede | A função clínica **não precisa de rede**. A placa tem Ethernet, e a imagem de desenvolvimento a usa (DHCP, `sshd`). A de produção não deveria expor serviço nenhum, e **nunca foi construída** (ACE-03) | Toda porta exposta é superfície sem função clínica. Medido na imagem de desenvolvimento: `22/tcp` (sshd) e `68/udp` (DHCP) (`attack-surface`, TST-01) |
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
| 1 | Console serial (`ttyS0` no QEMU; VCP do ST-LINK na placa) | física, externa | sim | quem tem o cabo | S, E | nenhum na imagem de desenvolvimento: login de root sem senha (`debug-tweaks`) | a imagem de produção o remove, e nunca foi construída (ACE-03) |
| 2 | SWD/JTAG do ST-LINK | física, externa | sim | acesso físico | T, I, E | nenhum | a porta de depuração não é fechada (81001-5-1 §5.3.2 g)–h), §7.2 h)) |
| 3 | Ethernet + `sshd` em `22/tcp` | rede, externa | sim | a rede local | S, I, E | nenhum: login de root sem senha; só a imagem de desenvolvimento tem o `sshd` | ACE-03 |
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
| 16 | Trilha de auditoria (`/var/log/journal`) | armazenamento | sim | root | T, R | selo do journal (AUD-02, com o falso positivo do systemd 255) e cadeia verificável (AUD-03) | a trilha fica dentro do slot A/B (AUD-06) |
| 17 | Registros clínicos em `/data` | armazenamento | sim | o serviço (escrita); `med-review` lê só o BDF+ publicado | I, T | LUKS2; `.sha256` ao lado de cada arquivo (INT-01); `EACCES` medido e injetado (DAD-05) | a chave no mesmo cartão (DAD-02) |
| 18 | Configuração selada (`/etc/medplatform/*.conf`) | armazenamento | sim | root | T | selo SHA-256 conferido ao carregar (INT-02); fora do envelope, recusa subir (INT-03) | o rootfs é gravável nas imagens medidas (SIS-01) |
| 19 | Entrega da chave de verificação do selo (`/run/med-journal-seal/verify-key`) | lógica, interna | sim | root, só no boot de provisionamento | I | `tmpfs`, `0400`, apagada no reboot, nunca no journal | quem é root no primeiro boot a lê |

**O que a tabela mostra, contado**: 19 interfaces, 8 físicas ou de rede e 11 lógicas ou de
armazenamento. Todas cruzam uma fronteira de confiança. **Três não têm controle nenhum** (1, 2 e 3,
de depuração ou de acesso remoto), **doze têm controle e lacuna declarada**, e **quatro não têm lacuna**
(4, 12, 14 e 15). A fronteira mais fraca do produto é a física: o SWD e o cartão (linhas 2 e 5).

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
