# Plano de Implementação — Controle de Acesso Obrigatório (MAC): AppArmor vs. SELinux

> **Status**: plano corrente (não histórico). Complementa `PROJECT_CONTEXT.md`, que continua sendo
> a referência arquitetural autoritativa. Os `implementation_plan.md`,
> `implementation_plan_EEG.md` e `implementation_plan_improvements.md` são iterações anteriores e
> **não** descrevem o estado atual do repositório.
>
> **Escopo**: decide entre **AppArmor** e **SELinux** para o `MedOS`, justifica a escolha a partir
> das restrições já cravadas na arquitetura, e enumera as mudanças concretas — arquivo a arquivo —
> que a integração exige.
>
> **Nada aqui foi implementado nem validado por bitbake.** Nenhum `kas build` rodou neste
> repositório até o momento. Este documento é pré-implementação por decisão explícita: registrar a
> decisão antes de escrever a metadata.
>
> **Verificado nesta análise** (via `git ls-remote`, em 2026-08-15):
> `git.yoctoproject.org/meta-security` tem branch `scarthgap` (`b13f170`) e
> `git.yoctoproject.org/meta-selinux` tem branch `scarthgap` (`48f7451`). Ambos os caminhos são
> materialmente viáveis no release usado pelo projeto. Todo o resto abaixo é análise, não medição.

---

## 1. Qual é a lacuna que o MAC preenche

O `eeg-acquisition-service` já roda numa caixa apertada: `ProtectSystem=strict`,
`PrivateNetwork=yes`, `RestrictAddressFamilies=AF_UNIX`, `SystemCallFilter=@system-service`,
`ReadWritePaths=/data`, `MemoryDenyWriteExecute=yes`. O rootfs é imutável, não há gerenciador de
pacotes no campo e a mutação suportada é um *bundle* RAUC assinado. A pergunta legítima é: o que
sobra para um MAC fazer?

Três coisas, em ordem de peso para o TCC:

1. **Origem da restrição.** A caixa do systemd é configuração de *userspace* aplicada pelo PID 1.
   O perfil MAC é aplicado pelo kernel, independentemente de quem iniciou o processo e de o PID 1
   estar íntegro. É defesa em profundidade real, ainda que o ganho marginal sobre a *unit* atual
   seja moderado — e é honesto dizer isso no texto.
2. **Segregação verificável de itens de software (IEC 62304 §5.3).** A norma pede segregação entre
   itens de classes de segurança diferentes. Hoje o repositório argumenta segregação por *cgroups*,
   *namespaces* e escalonamento. Um perfil MAC transforma o argumento numa **declaração legível por
   máquina da fronteira de recursos de cada item** — o serviço de aquisição pode abrir exatamente
   `/dev/rpmsg0`, `/etc/medplatform/eeg.conf`, `/run/medplatform/*`, `/var/lib/medplatform/*` e
   `/data/*`, e nada mais. Esse arquivo é evidência auditável, não prosa.
3. **Trilha de auditoria de tentativa de violação.** Negações do LSM entram no subsistema `audit`,
   que este projeto já habilita (`CONFIG_AUDIT`/`CONFIG_AUDITSYSCALL` no fragmento de kernel e
   `10-journald-audit.conf`), e portanto chegam ao journald — o mesmo canal do `MedicalLogger`.
   Isso vale igualmente para AppArmor e SELinux.

O que o MAC **não** faz aqui: não substitui a caixa do systemd, não substitui o rootfs read-only e
não é exigido por nenhuma norma citada no trabalho. É reforço, e deve ser apresentado como tal.

---

## 2. Decisão: **AppArmor**

O critério não é "qual é mais forte" — é qual dos dois compõe com as três decisões já cravadas na
arquitetura: **rootfs imutável**, **atualização A/B por imagem de bloco** e **portabilidade
BSP-agnóstica via `linux-%.bbappend`**.

| Critério | AppArmor | SELinux |
|---|---|---|
| Modelo | caminho (*path-based*) | rótulo em `xattr` (*label-based*) |
| Camada Yocto | `meta-security` (`recipes-mac/AppArmor`) | `meta-selinux` + `refpolicy` |
| Rootfs read-only | transparente — perfis são arquivos em `/etc/apparmor.d` | exige rotulagem *offline* no `do_rootfs`; `autorelabel` no primeiro boot é impossível num rootfs imutável |
| `/data` LUKS formatado em runtime | irrelevante: a regra fala do caminho | ext4 criado por `mke2fs` nasce sem rótulo → exige `context=` na montagem ou relabel, com estado fora da imagem |
| Slot A/B do RAUC | perfis viajam dentro da imagem do slot; troca de slot é neutra | rótulos viajam na imagem de bloco (ok), mas qualquer estado de política fora dela quebra a simetria dos slots |
| As duas aplicações do PoC | ~40 linhas de perfil cada | módulos `.te`/`.fc`/`.if` na refpolicy — ou as apps rodam como `unconfined_t`, o que anula o propósito |
| Integração com o que já existe | `AppArmorProfile=` direto nas *units* já escritas | exige *stack* de transição de domínio |
| Impacto no footprint (métrica do TCC) | *parser* + `libapparmor`, poucos MB | política binária + `setools` + `audit`, ordem de dezenas de MB |
| Custo de depuração sem bancada | modo *complain* + `aa-logprof` | depuração de AVC, historicamente cara |

**O que se perde ao não escolher SELinux** — e que deve estar no texto, porque a banca pode
perguntar:

- rótulo sobrevive a *rename*, *hardlink* e *bind mount*; regra por caminho, não;
- *type enforcement* cobre objetos que o AppArmor nomeia mal (IPC System V, netlink, portas, `key`);
- MLS/MCS permite múltiplos níveis de sigilo sobre o mesmo binário;
- é o que a indústria regulada de fato usa em volume (Android, RHEL), o que dá um argumento de
  precedente que o AppArmor não dá com a mesma força.

A conclusão continua sendo AppArmor: o custo do SELinux é concentrado exatamente onde este projeto
tem menos folga (política própria para aplicações próprias, rotulagem sob rootfs imutável, footprint)
e o benefício adicional é concentrado onde este projeto tem menos necessidade (multinível, renomeação
de binários, superfície de rede — que já é `PrivateNetwork=yes`).

**Não empilhar os dois.** Na prática, escolha exclusiva.

---

## 3. Onde cada peça mora (fronteiras de camada)

| Camada | O que recebe | Por quê |
|---|---|---|
| **BSP** | nada | é o ponto: a habilitação do LSM não custa uma linha por BSP (§4.3) |
| **`meta-med-distro`** | `DISTRO_FEATURES`, config de kernel, *runtime* do AppArmor na imagem base | MAC é política de sistema operacional, agnóstica a classe de dispositivo — vale igual para EEG, tomógrafo e bomba de infusão |
| **`meta-med-framework`** | nada | MAC é política de kernel, não API de aplicação. **A regra 6 do `CLAUDE.md` permanece intacta**: nenhuma app ganha dependência nova |
| **`meta-med-app`** | o perfil de cada aplicação, instalado pela própria receita | política *sobre* a aplicação é conhecimento da aplicação, exatamente como a *unit* systemd que cada receita já instala |

A propriedade importante: **nenhuma linha de código C++ muda**, e a métrica de portabilidade
(`diff` do código de aplicação entre QEMU e STM32MP257 permanece vazio) não é afetada.

---

## 4. Mudanças concretas

### 4.1. `kas/kas-base.yml` — nova camada

```yaml
  meta-security:
    url: "https://git.yoctoproject.org/meta-security"
    branch: "scarthgap"
    path: "layers/meta-security"
    layers:
      meta-security:
```

Expor **apenas** `meta-security` — o repositório também carrega `meta-tpm`, `meta-integrity`,
`meta-hardening` e `meta-parsec`, que estão fora de escopo. Confirmar o `LAYERDEPENDS` de
`meta-security/conf/layer.conf` antes de assumir que resolve: a expectativa é `core`,
`openembedded-layer`, `meta-python` e `networking-layer`, todos já presentes no `kas-base.yml`, mas
isso **não foi verificado**.

### 4.2. `meta-med-distro/conf/distro/med-os.conf`

```
MED_DISTRO_FEATURES = "wayland opengl pam seccomp usrmerge polkit rauc apparmor"

# Sem esta opção, systemd ignora silenciosamente AppArmorProfile= nas units.
PACKAGECONFIG:append:pn-systemd = " apparmor"
```

O segundo item é o detalhe que costuma passar batido: `AppArmorProfile=` só tem efeito se o
`systemd` tiver sido compilado com suporte a AppArmor. Junta-se à linha `gcrypt cryptsetup` que já
existe no arquivo.

### 4.3. `meta-med-distro/recipes-kernel/linux/files/med-kernel-features.cfg`

```
# --- MAC: confinamento obrigatório por perfil (AppArmor).
CONFIG_SECURITY=y
CONFIG_SECURITYFS=y
CONFIG_SECURITY_APPARMOR=y
CONFIG_SECURITY_APPARMOR_BOOTPARAM_VALUE=1
CONFIG_DEFAULT_SECURITY_APPARMOR=y
CONFIG_LSM="landlock,lockdown,yama,integrity,apparmor"
```

`CONFIG_LSM` é a linha que importa e é o motivo pelo qual isto **não custa nada por BSP**: fixando a
lista de LSMs no fragmento, não é preciso injetar `security=apparmor apparmor=1` na linha de comando
do kernel — que seria `APPEND` no QEMU e *bootargs* de U-Boot/extlinux no STM32MP257, ou seja, dois
pontos de toque específicos de BSP, exatamente o que a regra 3 do `CLAUDE.md` existe para evitar.

`CONFIG_AUDIT`/`CONFIG_AUDITSYSCALL` já estão no fragmento e são o que leva as negações ao journald.

### 4.4. Imagem: *runtime* na base, modo por perfil de imagem

Em `med-image-base.bb`, acrescentar ao `MED_OS_INSTALL`:

```
    apparmor \
```

**Não instalar `apparmor-profiles`.** O pacote de perfis genéricos do *upstream* confina binários de
serviços que este sistema não tem e introduz falhas de acesso difíceis de atribuir. O sistema deve
carregar apenas os perfis que este projeto escreveu — o que também é o argumento mais forte para a
tese: a política é derivada do design, não herdada de uma distro genérica.

Modo de operação por imagem, seguindo o idioma de substituição em tempo de build que o repositório já
usa para `MED_EEG_DRIVER`:

| Imagem | `MED_APPARMOR_MODE` | Efeito |
|---|---|---|
| `med-image-base` (padrão) | `enforce` | negação bloqueia e é registrada |
| `med-image-test` | `complain` | negação apenas registrada — é o modo de coleta para `aa-logprof` |
| `med-image-prod` | `enforce` (reafirmado) | mesma lógica de `MED_ROOTFS_FEATURES`: propriedade definidora, reafirmada para não se perder num override |

O valor entra no cabeçalho do perfil como `flags=(${MED_APPARMOR_MODE})`. Isso encaixa
exatamente na divisão teste/produção que já existe e rende uma figura direta para o capítulo de
resultados.

### 4.5. Perfil do `eeg-acquisition-service` (`meta-med-app`)

Instalado pela receita em `/etc/apparmor.d/usr.bin.eeg-acquisition-service`, e referenciado na
*unit* que já existe:

```ini
AppArmorProfile=eeg-acquisition-service
```

Esboço do perfil — **rascunho a refinar em modo *complain***, não versão final:

```
abi <abi/3.0>,
include <tunables/global>

profile eeg-acquisition-service /usr/bin/eeg-acquisition-service flags=(enforce) {
  include <abstractions/base>

  # binário e a única biblioteca própria
  /usr/bin/eeg-acquisition-service  mr,
  /usr/lib/libmedframework.so*      mr,

  # configuração selada + o sidecar de integridade (somente leitura)
  /etc/medplatform/eeg.conf         r,
  /etc/medplatform/eeg.conf.sha256  r,

  # MedicalLogger -> journald
  /run/systemd/journal/socket       w,
  /run/systemd/journal/stdout       rw,

  # socket de amostras publicado para a HMI (RuntimeDirectory=)
  /run/medplatform/                 rw,
  /run/medplatform/**               rw,

  # cabeça da cadeia de hash de auditoria (StateDirectory=)
  /var/lib/medplatform/             rw,
  /var/lib/medplatform/**           rwk,

  # MedicalStorage -> volume cifrado de registros
  /data/                            r,
  /data/**                          rwk,

  # ponte AMP para o Cortex-M4 (ausente no alvo QEMU, inofensivo)
  /dev/rpmsg0                       rw,
  /dev/rpmsg_ctrl0                  rw,

  # negações explícitas: sem rede, sem exec, sem leitura de memória alheia
  deny network,
  deny /** x,
  deny @{PROC}/@{pid}/mem           rw,
}
```

Ler esse arquivo lado a lado com a seção de *sandboxing* de `eeg-acquisition.service` é a
demonstração central: **a mesma fronteira, declarada duas vezes, por dois mecanismos independentes.**

### 4.6. Perfil do `eeg-hmi-gui` — deixar para um segundo incremento

A HMI precisa de Qt6, plugins QML, socket Wayland, nós de DRM/GPU, fontes e cache de shaders. É um
perfil grande, frágil e caro de estabilizar, e o benefício de confinar o item de menor classe de
segurança é o menor da lista.

Recomendação: **confinar primeiro o item crítico** (aquisição, que toca dado de paciente e o
co-processador) e deixar a HMI em modo *complain* — ou sem perfil — no primeiro incremento. Isso não
é preguiça: é a própria priorização por classe de segurança que a IEC 62304 pede, e vale ser
registrada como decisão consciente no texto.

### 4.7. Cache de perfis sob rootfs read-only

O `apparmor_parser` compila os perfis a cada boot e tenta gravar cache em `/etc/apparmor.d/cache/`,
que num rootfs imutável falha — o sistema continua funcionando, apenas paga o custo de *parsing* em
todo boot. Duas saídas: aceitar o custo (medir, §6) ou pré-compilar o cache em tempo de build, o que
esbarra na arquitetura do *parser* (binário de host vs. alvo) e provavelmente não vale o esforço para
dois perfis. **Decisão inicial: aceitar o custo e medir.**

---

## 5. Ordem de execução

O ponto mais importante deste plano é o **sequenciamento**:

1. **Primeiro `kas build` verde (perfil QEMU), antes de qualquer coisa aqui.** Adicionar uma camada,
   uma `DISTRO_FEATURE` e um LSM sobre metadata que o bitbake nunca parseou multiplica a superfície
   de depuração. Os três riscos já registrados no `CLAUDE.md` — bbappend de `rauc-conf`, pino do
   `meta-qt6` em 6.8, nomes de pacotes Qt — vêm antes disto.
2. Camada + `DISTRO_FEATURES` + `PACKAGECONFIG` do systemd (§4.1, §4.2); validar com
   `bitbake-layers show-layers` antes de construir.
3. Fragmento de kernel (§4.3); conferir que o fragmento chegou ao `SRC_URI` e que não colide com o
   `CONFIG_LSM` do BSP.
4. `apparmor` na imagem base + `MED_APPARMOR_MODE` (§4.4). Build de `med-image-test`, boot em QEMU,
   `aa-status` — **ainda sem perfil próprio**, só confirmando que o LSM está ativo.
5. Perfil do serviço de aquisição em modo *complain*; rodar uma sessão completa de aquisição;
   coletar negações do journal e refinar.
6. Virar para *enforce*; repetir a sessão; executar o teste negativo (§6).
7. Só então, e como incremento separado: perfil da HMI (§4.6).

## 6. Plano de verificação

| Nível | Verificação |
|---|---|
| Parse | `kas shell kas/kas-base.yml -c "bitbake-layers show-layers"` resolve `meta-security` sem dependência faltante |
| Build | `bitbake -e virtual/kernel \| grep ^SRC_URI=` contém `med-kernel-features.cfg`; `do_kernel_configcheck` não reporta `CONFIG_LSM` sobrescrito pelo BSP |
| Build | `systemctl show eeg-acquisition -p AppArmorProfile` no alvo retorna o perfil (prova que o `PACKAGECONFIG` pegou) |
| QEMU | `aa-status` lista o perfil; `cat /proc/$(pidof eeg-acquisition-service)/attr/current` mostra `eeg-acquisition-service (enforce)` |
| QEMU | Sessão de aquisição completa em *enforce*: quadros com CRC válido, registro de sessão gravado em `/data`, **zero** linhas `apparmor="DENIED"` no journal |
| QEMU | **Teste negativo** (evidência citável): forçar o serviço a abrir um caminho fora do perfil e mostrar o par `apparmor="DENIED"` + registro do `MedicalLogger` na mesma trilha do journald |
| Métrica | `buildhistory` (já habilitado) dá o delta de tamanho da imagem com e sem MAC — entra direto na métrica de footprint |
| Métrica | Delta de tempo de boot com carregamento de perfis sem cache (§4.7) |
| Tese | Portabilidade: `diff` do código de aplicação entre alvos permanece **vazio**; a habilitação do LSM custa **zero linhas por BSP** |

## 7. Riscos e questões em aberto

1. **`LAYERDEPENDS` de `meta-security` não confirmado** (§4.1). Se pedir alguma camada ausente, o
   `kas-base.yml` cresce — e a hipótese de que a integração é barata precisa ser revista.
2. **Colisão de `CONFIG_LSM` com o BSP.** O `meta-st-stm32mp` pode trazer sua própria lista; dois
   fragmentos definindo a mesma variável produzem aviso no `do_kernel_configcheck` e um vencedor não
   óbvio. Verificar no alvo real, não só no QEMU.
3. **`PACKAGECONFIG` de systemd ausente = falha silenciosa**: `AppArmorProfile=` é ignorado sem
   erro. Por isso a verificação com `systemctl show` está no plano, e não só o `aa-status`.
4. **Perfil da HMI** (§4.6) é o item caro e frágil; adiado por decisão, não esquecido.
5. **`security_flags.inc` × build do AppArmor userspace**: o `med-os.conf` já aplica *hardening* de
   toolchain a todas as receitas e essa é, por comentário do próprio arquivo, a configuração mais
   invasiva do projeto. O *parser* do AppArmor é um candidato plausível a quebrar sob flags
   incomuns. Não verificado.
6. **Cache de perfis em rootfs imutável** (§4.7) — custo aceito, ainda não medido.
7. **Ganho marginal de segurança é moderado** e o texto do TCC deve dizer isso. O ganho principal é
   de auditabilidade e de evidência de segregação, não de redução de superfície: a *unit* do serviço
   já fecha quase tudo que o perfil fecha.
8. **A escolha é exclusiva.** Migrar para SELinux depois não é um incremento — é refazer §4 inteira,
   mais rotulagem, mais política própria. Se houver qualquer intenção de defender SELinux na banca,
   a decisão precisa ser revista **antes** do passo 2 de §5.
