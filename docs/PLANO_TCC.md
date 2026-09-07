# Plano de Execução do TCC — MedPlatform

> **Status**: plano de execução submetido à aprovação do orientador. Não é registro de medição
> (`RESULTS.md`) nem registro de engenharia (`BRINGUP_*.md`) — é o documento que organiza o que
> falta fazer, em que ordem, e o que se corta se o calendário apertar.
>
> **Quando**: 25/08/2026. **Horizonte**: ~16 semanas, entrega e defesa em dezembro de 2026
> (datas a confirmar com o orientador).
>
> **Como ler**: a §2 diz onde o trabalho está, com o nível de evidência de cada item; a §4 é o
> cronograma; a §6 traz os riscos e a **ordem de corte**, que é a decisão principal a aprovar.

---

## 1. O que o trabalho propõe

A tese é que uma arquitetura em camadas com dependência estritamente unidirecional, somada a um
framework de abstração de uso obrigatório, permite construir *classes distintas* de dispositivo
médico reaproveitando uma plataforma comum — e que esse reaproveitamento é **mensurável**, não
apenas afirmado.

O objeto construído é o `MedStack`: quatro camadas Yocto (`meta-med-bsp`, `meta-med-distro`,
`meta-med-framework`, `meta-med-app`) sobre Scarthgap 5.0 LTS, com um sistema de aquisição de EEG
como prova de conceito e um perfil de tomógrafo como caso de controle. As restrições que a
IEC 62304 impõe — segregação de itens de software, rastreabilidade, atualização controlada — entram
como restrições arquiteturais, não como endurecimento opcional.

A afirmação que o trabalho já sustenta, e que dá a medida do resto do plano: **a plataforma que as
duas classes de dispositivo herdam tem 206 pacotes; o delta específico do EEG são 3**, e o conjunto
do tomógrafo é subconjunto estrito do conjunto do EEG. O mesmo código de aplicação e de framework
compila e executa para `qemux86-64` e para `aarch64/cortexa35` sem uma linha de diferença.

---

## 2. Onde o trabalho está hoje

A distinção que organiza esta seção é a mesma que organiza o repositório: **construído**,
**executado em simulação** e **executado em hardware** são três níveis de evidência diferentes.

| Resultado | Evidência | Nível |
|---|---|---|
| Reuso entre classes de dispositivo | 206 pacotes comuns, delta de 3; `diff` só com remoções | QEMU |
| Caminho de aquisição íntegro | Sessão de 5 min 22 s; 3.226 quadros; resto zero na divisão por 840 B | QEMU |
| Particionamento de software | `exposure 3.7`; `SCHED_RR` prio. 50 concedido pelo kernel | QEMU |
| Trilha de auditoria selada | `journalctl --verify` → `PASS`, nos dois alvos | **Hardware** |
| Política de atualização A/B | Bundle assinado verificado contra o keyring do dispositivo e escrito no slot inativo | QEMU |
| Volume `/data` LUKS2 | Provisionado em 13 s no primeiro boot; `dm/uuid` começa com `CRYPT-` | **Hardware** |
| Portabilidade para o alvo físico | Boot completo da STM32MP257F-DK; slot A pelo PARTUUID que o layout fixa | **Hardware** |
| Interface de operador | Renderiza; morre sob o próprio filtro de syscalls (`mincore`) | Parcial |
| Ativação de slot pelo bootloader | Ambiente do U-Boot legível; seleção de slot não implementada | Parcial |
| Caminho AMP / `rpmsg` | Cortex-M33 presente e disponível; sem firmware | Não exercitado |
| Controle de acesso obrigatório (MAC) | Decisão registrada (AppArmor via `meta-security`); nenhuma linha escrita | Não implementado |

**Uma fraqueza que já caiu.** A `CONTRIBUTION.md` §4 listava como fraqueza número um: *"nenhum
`kas build` rodou — sem isso, arquitetura de referência é uma proposta de arquitetura"*. Isso deixou
de valer em 16/08/2026 para o QEMU, em 17/08 para o STM32MP257, e em 18/08 a placa física bootou. As
demais fraquezas daquela lista continuam de pé e estão endereçadas, uma a uma, no cronograma da §4.

---

## 3. Como este trabalho produz evidência

Vale declarar isto como método, porque é o que separa o trabalho de um relato do tipo "usamos Yocto
no nosso produto" e porque foi ele que produziu os achados mais úteis.

São quatro níveis de evidência, e um não substitui o outro:

1. **Build verde.** Prova sintaxe, resolução de camadas e grafo de dependências. Não prova
   comportamento.
2. **Inspeção do artefato produzido.** Ler a tabela de partições do `.wic`, extrair o pacote e ler o
   arquivo instalado. Dos treze defeitos catalogados no porte para hardware, **seis não tinham
   sintoma nenhum** — não quebraram build, não emitiram aviso, e só apareceram assim.
3. **Suíte de aceitação em execução.** `make check` boota a imagem e executa 22 asserções sobre o
   console serial. É a única coisa do repositório que pega regressão de comportamento.
4. **Execução no alvo físico.** Quatro defeitos do primeiro boot dependiam de hardware que a imagem
   não contém — pânico da GPU no mundo seguro, colisão de rótulos de partição com o eMMC de fábrica,
   ausência de RTC.

Duas regras derivadas orientam o restante do plano:

- **Uma asserção que nunca viu a falha que procura é uma afirmação, não uma verificação** — três
  defeitos passaram por asserções que os declaravam saudáveis.
- **Um valor que decide o comportamento do produto precisa estar escrito em alguma camada deste
  repositório**, mesmo quando coincide com o default do upstream, porque um default vence por
  ausência de adversário — e isso já aconteceu três vezes (`WKS_FILE` duas, `QB_MEM` uma).

---

## 4. Cronograma — seis fases até a defesa

Cada fase termina em um entregável verificável **e** em um capítulo escrito. As datas são propostas,
ancoradas numa defesa em meados de dezembro; o critério de pronto é o que não deve mudar.

### Fase 1 — Consolidação e dívida técnica conhecida
**25/08 – 07/09 (2 semanas)**

Fechar o que já está diagnosticado e custa pouco, para que nenhuma medição posterior seja feita
sobre um sistema com defeito conhecido.

- **Escopo**: `SystemCallFilter=mincore` na HMI; `QB_MEM` declarado em `meta-med-bsp`; asserção de
  estabilidade com janela maior que 30 s; tratamento do relógio sem RTC; captura das figuras já
  possíveis; **compra do módulo ADS1299** (prazo de entrega é o risco crítico do plano).
- **Critério de pronto**: suíte 22/22 e interface de operador de pé por ≥ 10 min sem reinício, com
  captura de tela arquivada.
- **Já feito nesta fase** (bancada de 27/08): `SystemCallFilter=mincore` aplicado e **validado em
  hardware**, e a interface de operador renderizando em monitor HDMI. Continuam pendentes, e nenhuma
  sobrevive a um reboot: a regra de udev do `/dev/galcore`, o `weston.ini` com `idle-time=0`, o modo
  CEA fixado, o `QB_MEM` do QEMU e os limites de reinício da unidade de aquisição. Ver
  `BRINGUP_HMI_STM32MP2.md` §9.
- **Texto**: capítulo de metodologia (§3 acima).

### Fase 2 — Fechar a atualização A/B no alvo físico
**08/09 – 28/09 (3 semanas)**

Hoje o trabalho afirma "política de atualização validada em QEMU, integração com bootloader não
validada". Esta fase remove a segunda metade da frase.

- **Escopo**: script de U-Boot lendo `BOOT_ORDER` e `BOOT_<slot>_LEFT`; `extlinux.conf` por slot;
  instalação real de bundle na placa; **rollback provocado** por injeção de falha, que é o resultado
  que interessa.
- **Critério de pronto**: o dispositivo instala bundle no slot inativo, boota nele, e retorna
  sozinho ao slot anterior quando o novo é marcado ruim — com tempos medidos.
- **Texto**: capítulo de atualização e integridade.

### Fase 3 — Caminho AMP: firmware no Cortex-M33
**29/09 – 19/10 (3 semanas)**

O driver `rpmsg` do framework nunca foi exercitado em alvo nenhum. Esta fase o exercita com sinal
sintético gerado no coprocessador, antes de qualquer conversor físico entrar na conta.

- **Escopo**: firmware mínimo no M33 emitindo o cabeçalho de quadro de 40 bytes que o framework já
  declara; enfrentar a restrição `Support of signed firmware only` observada no primeiro boot;
  primeira medição de **latência, jitter e perda de amostras** — hoje listados como não medidos.
- **Também nesta fase**: a camada adjunta vazia, com receitas construindo módulos triviais. É o
  passo que verifica `module.bbclass`, gancho e autoload enquanto errar ainda custa zero, e é
  pré-requisito das duas frentes de driver da Fase 4.
- **Critério de pronto**: a mesma aplicação, sem recompilar, consome quadros do coprocessador
  trocando um valor de configuração.
- **Texto**: capítulo de multiprocessamento assimétrico.

### Fase 4 — Front-end analógico ADS1299
**20/10 – 09/11 (3 semanas)**

O teste mais duro da tese: introduzir um *part-number* concreto e verificar que ele não vaza para
cima.

- **Escopo, modo hat**: ligação SPI + `DRDY` ao M33 e descrição no device tree; firmware com mapa de
  registradores e conversão para nanovolts; parâmetros clínicos (ganho, taxa de dados, detecção de
  eletrodo solto) em `eeg.conf`; aquisição do sinal de teste interno do conversor.
- **Escopo, modo USB**: a **camada adjunta** (`meta-med-afe-ads1299`, fora das quatro camadas do
  `MedStack`), o *backport* do `ti-ads1298.c` de 6.9 para 6.6 e sua adaptação para o ADS1299 com
  canal de carimbo de tempo, e o driver `mcp2210` (`spi_controller` sobre HID) — precedido da
  avaliação das implementações de terceiros já existentes. Ver `implementation_plan_iio_afe.md`, que
  é a autoridade sobre esta metade, incluindo a ordem: **o driver do AFE vem antes da ponte**, por
  ser o de menor risco e maior retorno.
- **Critério de pronto**: **zero ocorrências das strings `ADS1299` e `MCP2210` fora da camada
  adjunta e do firmware** — verificável por `grep` —, e a mesma aplicação consumindo dos três
  drivers (`simulated`, `rpmsg`, `iio`) por troca de configuração.
- **Texto**: capítulo da prova de conceito EEG.

### Fase 5 — Segurança, perfil de produção e avaliação comparativa
**10/11 – 30/11 (3 semanas)**

Pacote deliberadamente **sem dependência de hardware**: é ele que sobe de posição se a Fase 3 ou a 4
escorregar.

- **Escopo**: AppArmor via `meta-security`, com perfis para os dois serviços e medição do
  antes/depois; `med-image-prod` com rootfs imutável efetivamente exercitado; comparação com
  Buildroot e com base Debian por critérios declarados (SBOM, reprodutibilidade, suporte LTS,
  auditoria de licenças); tabela de rastreabilidade IEC 62304.
- **Critério de pronto**: perfil de produção bootando com rootfs somente-leitura e a suíte passando
  sobre ele.
- **Texto**: capítulos de segurança e de avaliação.

### Fase 6 — Fechamento
**01/12 – 14/12 (2 semanas)**

- **Escopo**: consolidação das medições, produção final das figuras, revisão integral, preparação da
  defesa. Nenhuma implementação nova entra aqui.
- **Critério de pronto**: todo número do texto reproduzível pelo comando que o produziu.

---

## 5. O que entra, e o que fica para depois

**No escopo do TCC**

- Correções pendentes e lacunas de medição
- Seleção de slot A/B pelo bootloader, com rollback
- Firmware no Cortex-M33 e caminho `rpmsg` fim a fim
- Front-end ADS1299 físico, nos dois modos (hat/AMP e USB/IIO)
- Dois drivers de kernel na camada adjunta, um deles derivado de driver mainline
- AppArmor e perfil de produção
- Avaliação comparativa com alternativas

**Trabalho futuro declarado**

- TPM real e *secure boot* — hoje inalcançável no conjunto de camadas (só emuladores em software,
  sem fTPM para o OP-TEE)
- Terceira classe de dispositivo ou terceiro alvo, para fortalecer a métrica de portabilidade com
  esforço medido
- Suíte de aceitação automatizada contra alvo serial: hoje a verificação em hardware é manual
- Modelagem explícita de variabilidade (*feature model*)
- Perfil de tomógrafo no alvo físico
- Submissão do suporte a ADS1299 para o kernel *upstream* — fora do caminho crítico, e um bônus
  possível justamente por o ponto de partida ser um driver mainline
- Submissão a processo de certificação real

---

## 6. Riscos e plano de corte

| Risco | Impacto | Mitigação |
|---|---|---|
| Firmware assinado obrigatório no Cortex-M33 | Bloqueia a Fase 3 inteira | Investigar na Fase 1, não na 3. Se for bloqueio duro, a restrição vira resultado documentado e a fase encurta. |
| Prazo de entrega do módulo ADS1299 | Adia ou elimina a Fase 4 | Compra na semana 1. Se não chegar até 20/10, a Fase 5 assume o lugar e o conversor vira trabalho futuro. |
| Seleção de slot exige mais U-Boot que o previsto | Estoura a Fase 2 | Caixa de tempo de 3 semanas; ambiente semeado e script escrito já são resultado parcial publicável. |
| Bring-up analógico (ruído, eletrodos, alimentação) | Consome a Fase 4 sem produzir sinal utilizável | Aceitar o sinal de teste interno do conversor como critério mínimo; eletrodos são bônus. |
| Verificação em hardware é manual | Regressão silenciosa entre fases | Reexecutar o roteiro de aceitação da placa ao fim de cada fase, com resultado datado. |
| Espaço em disco e tempo de build | Já parou o trabalho uma vez (152 GB em diretórios de trabalho) | Já mitigado (`rm_work`, caches em disco externo); monitorar. |

**Ordem de corte, se o calendário apertar.** Esta ordem é a decisão que peço para aprovar junto com
o plano — descartar nesta sequência:

1. O **modo USB** (camada adjunta e os dois drivers de kernel) — o modo hat sustenta sozinho o
   argumento de particionamento, que é o mais forte dos dois
2. Firmware no Cortex-M33 (o driver `rpmsg` permanece não exercitado, e isso já está declarado como
   limitação)
3. Avaliação comparativa com Buildroot e Debian
4. Perfil de produção com rootfs imutável

**Nunca cortar**: a Fase 1, a Fase 2 e o tempo de escrita. Sem a Fase 2, a afirmação sobre
atualização de campo permanece pela metade — e é a metade que um avaliador de dispositivo médico vai
perguntar primeiro.

---

## 7. O que precisa ser medido antes da defesa

Esta lista não é nova: sai da `CONTRIBUTION.md` §4 e da `RESULTS.md` §9. O que o plano faz é
atribuir cada item a uma fase.

| Lacuna | Fase |
|---|---|
| Latência, jitter e perda de amostras no caminho de aquisição | 3 |
| Fallback A/B em boot falho, com bootloader real | 2 |
| Métrica de portabilidade fortalecida com esforço medido, não só `diff` vazio | 4 e 5 |
| Comparação com alternativas de construção de imagem | 5 |
| Tabela de rastreabilidade IEC 62304 | 5 |
| Rootfs imutável exercitado | 5 |
| Carimbo de tempo confiável no alvo físico | 1 |
| Tempo de build e tempo de boot como números publicados | 5 |

---

## 8. Estrutura proposta da monografia

O material bruto de cada capítulo já existe como registro de engenharia no repositório; a escrita é
de consolidação, não de partida.

| Capítulo | Material de origem |
|---|---|
| 1. Introdução e contexto regulatório | — |
| 2. Trabalhos relacionados e posicionamento | `CONTRIBUTION.md` (7 correntes, lacuna reivindicada) |
| 3. Metodologia de evidência | §3 deste plano; §11 do `BRINGUP_STM32MP2.md` |
| 4. Arquitetura MedStack | `PROJECT_CONTEXT.md` |
| 5. O framework de abstração médica | `meta-med-framework`: 6 APIs, 2 dependências externas |
| 6. Prova de conceito EEG | `meta-med-app`; `implementation_plan_ads1299.md` |
| 7. Porte para o alvo físico | `BRINGUP_STM32MP2.md` (13 defeitos, 10 regras) |
| 8. Resultados medidos | `RESULTS.md`; `BRINGUP_HMI_QEMU.md` |
| 9. Discussão, limitações e trabalho futuro | §5 e §6 deste plano |

As figuras estão planejadas em conjunto (pilha de camadas, layouts de disco dos dois alvos, cadeia
de boot, formato de quadro, sinal adquirido e seu espectro, tela do operador, cadeia de hash da
trilha de auditoria). **Nenhum documento tem imagem hoje**; a produção delas está distribuída entre
as fases 1, 4 e 6.

---

## 9. O que peço ao orientador

1. **Confirmar a data** de entrega e de defesa, e o formato exigido do texto — o cronograma inteiro
   está ancorado em meados de dezembro.
2. **Aprovar o escopo e, sobretudo, a ordem de corte** da §6. É a decisão que evita improviso se
   algo escorregar em novembro.
3. **Disponibilidade de hardware e laboratório**: módulo de avaliação do ADS1299, eletrodos, e
   acesso a osciloscópio para o bring-up analógico da Fase 4.
4. **Definir o peso da comparação** com Buildroot e Debian: capítulo próprio ou seção da avaliação.
5. **Marcar três revisões intermediárias** — proponho 28/09, 09/11 e 30/11, ao fim das fases 2, 4
   e 5.

---

*Todo número citado aqui vem de `RESULTS.md`, que traz, em cada caso, o comando que o produziu.*
