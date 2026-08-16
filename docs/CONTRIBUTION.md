# Posicionamento da contribuição: MedPlatform frente à literatura

> Nota de trabalho para o texto do TCC. Não é especificação do repositório —
> para isso, veja `docs/PROJECT_CONTEXT.md`. As referências marcadas com
> *(verificar)* precisam ter a citação exata conferida antes de irem para o texto final.

---

## 1. Isolando o que é, de fato, a contribuição

O enunciado atual ("não propomos um dispositivo, propomos uma arquitetura de referência
reutilizável") é a *forma* certa de claim, mas ainda é genérico — quase toda arquitetura de
referência diz isso. O que o repositório realmente sustenta, e que é mais raro:

1. **Uma arquitetura de referência em 4 camadas com dependência unidirecional imposta pelo
   próprio mecanismo de build** (prioridade de layer + a regra "aplicação nunca ganha uma
   segunda dependência"). A restrição arquitetural é *verificável mecanicamente* — o `DEPENDS`
   de uma receita de aplicação é um invariante checável, não só um desenho em UML.
2. **Uma superfície de adaptação de hardware reduzida a uma variável** (`MED_EEG_DRIVER`),
   com métrica operacionalizada: `diff` do código de aplicação entre QEMU e STM32MP257 = vazio.
3. **Segregação de criticidade por processador** (ADS1299 → Cortex-M4 → `rpmsg` → Linux;
   Linux nunca toca no conversor), usada como argumento de particionamento IEC 62304 —
   não apenas como decisão de desempenho.
4. **Uma segunda instanciação (tomógrafo) como evidência de reuso** — que é exatamente o
   protocolo de avaliação da literatura de linhas de produto de software.

É nesses quatro pontos, e não no "EEG com Yocto", que a diferença precisa ser defendida.

---

## 2. As correntes teóricas em que o trabalho cai — e o vizinho de cada uma

### (a) Arquiteturas de referência de software

A corrente mais direta. O framework canônico é **Angelov, Grefen & Greefhorst (2012)**,
*"A framework for analysis and design of software reference architectures"*, Information and
Software Technology 54:417–431; e o anterior **Angelov et al. (2009)**, que classifica
arquiteturas de referência em cinco tipos por contexto, objetivo e design. Há também a linha
da **Elisa Yumi Nakagawa (ICMC/USP)** sobre modelos de referência para AR (RAModel) —
conveniente por ser produção brasileira. *(verificar a citação exata do RAModel.)*

**O que muda aqui:** essa literatura trata a AR como artefato de projeto (documento, modelo,
padrão). O artefato deste trabalho é *executável* — a arquitetura é o conjunto de metadados que
produz a imagem. Cabe posicionar-se formalmente na taxonomia de Angelov: uma AR
*preliminary, facilitation, single-organization*, implementada como código. Poucos trabalhos
dessa corrente entregam a AR como build system.

### (b) Linhas de produto de software / gestão de variabilidade

Clements & Northrop (2001); modelos de features desde FODA (Kang et al., 1990); variabilidade
em software de sistemas (Berger et al., IEEE TSE 2013, sobre Kconfig/Linux). Yocto é,
literalmente, um mecanismo de variabilidade — `DISTRO_FEATURES`, `PACKAGECONFIG`, overrides,
layers.

**O que muda aqui:** os perfis (`med-image-eeg`, `med-image-tomograph`) são *produtos de uma
linha*, e `MED_EEG_DRIVER` é um ponto de variação. **Esta é a maior oportunidade de
fortalecimento teórico do TCC**: nomear explicitamente os pontos de variação, o tipo de binding
(build-time vs. run-time) e apresentar um feature model, ainda que pequeno. Sem isso, um
avaliador dessa área dirá que se fez SPL sem saber que se fez.

### (c) Linux em sistemas críticos de segurança

**ELISA (Enabling Linux In Safety Applications, Linux Foundation)** é o vizinho mais próximo
institucionalmente — e tem um **Medical Devices Working Group** dedicado, cuja missão é
justamente identificar quais componentes do Linux entram na análise de segurança sob as normas
de dispositivos médicos. **Automotive Grade Linux** é o análogo estrutural mais forte: uma
distro de referência em camadas, baseada em Yocto, para um domínio regulado.

**O que muda aqui:** ELISA ataca *evidência de certificação de componentes* (o kernel é
analisável?); AGL ataca *interoperabilidade e reuso no domínio automotivo*. Este trabalho ataca
uma terceira coisa: **como estruturar o repositório de build para que a aplicação médica fique
isolada do SO e do BSP por contrato**. O enquadramento honesto é "AGL para dispositivos
médicos, em escala de TCC" — cita-se ELISA/AGL para demonstrar conhecimento do estado da
prática, não para competir com eles.

### (d) Processo e conformidade regulatória de software médico

**MDevSPICE** (Lepmets, McCaffery, Clarke, Finnegan, Dorling — Regulated Software Research
Centre, Dundalk), modelo de avaliação de processo cujo PRM deriva de IEC 62304, ISO/IEC 12207,
ISO 14971 e ISO 13485. Somam-se IEC 62304 (§5.3, segregação de itens por classe; §8.1.2, SOUP),
ISO 14971, FDA (Seção 524B do FD&C Act, com SBOM obrigatório) e, no Brasil, a RDC ANVISA
751/2022.

**O que muda aqui:** essa corrente é quase toda **de processo** — como se organiza o ciclo de
vida. A contribuição deste trabalho é **de arquitetura e de tooling**: rootfs read-only, RAUC
A/B, LUKS e log auditável não como hardening opcional, mas como habilitadores de requisitos
normativos; e o Yocto gerando SPDX/SBOM nativamente resolve, na prática, parte do problema de
SOUP que essa literatura discute no papel. **Uma tabela de rastreabilidade "requisito normativo
→ decisão arquitetural → artefato no repositório" é o item de maior retorno por esforço no
texto do TCC.**

### (e) Multiprocessamento assimétrico e criticidade mista

Burns & Davis, *"Mixed Criticality Systems — A Review"* (survey clássico, continuamente
atualizado), mais a literatura aplicada de AMP com OpenAMP/rpmsg em Zynq, i.MX e STM32MP1 para
aquisição determinística. Alternativas concorrentes na literatura: PREEMPT_RT, Xenomai,
hipervisores (Jailhouse, Xen).

**O que muda aqui:** o AMP em si não é novidade — é engenharia consolidada. A diferença é
usá-lo como **argumento de classificação de segurança**: colocando o AFE atrás do M4, o item de
software Linux pode ser argumentado como de classe inferior sob IEC 62304 §5.3. Não vender o
AMP como contribuição técnica; vender a *justificativa normativa da partição*, e comparar
explicitamente com PREEMPT_RT e hipervisor (por que AMP e não os outros dois).

### (f) Cadeia de suprimentos, reprodutibilidade e atualização

Lamb & Zacchiroli, *"Reproducible Builds: Increasing the Integrity of Software Supply Chains"*,
IEEE Software (2022); TUF/Uptane para atualização segura; comparações RAUC/SWUpdate/Mender.
Para o log à prova de adulteração: Schneier & Kelsey, *"Secure audit logs to support computer
forensics"* (ACM TISSEC, 1999) e MACs forward-secure (Bellare & Yee) — é a base teórica do que
o systemd chama de FSS, e dá lastro ao `MedicalLogger`.

### (g) A corrente que o trabalho *contrasta*, não continua

A literatura extensa de "sistema de aquisição de EEG de baixo custo com ADS1299" (o ecossistema
OpenBCI e derivados). Aqui a diferenciação é limpa e já está dada: **para eles o EEG é a
contribuição; aqui ele é a instanciação de prova**. Usar dois ou três desses trabalhos como
contraponto no capítulo de trabalhos relacionados.

---

## 3. A lacuna que se pode reivindicar

A busca realizada não retornou trabalho acadêmico publicado que proponha **uma arquitetura de
referência em camadas, baseada em Yocto, específica para dispositivos médicos, com framework de
abstração obrigatório e métrica de portabilidade medida**. O material existente se divide em:
AR sem implementação (correntes a/b), implementação sem AR (relatos industriais do tipo "usamos
Yocto no nosso produto"), processo sem arquitetura (corrente d) e esforços de comunidade sem
publicação acadêmica (ELISA, AGL).

Enunciar a lacuna assim — **"não localizei"**, com a estratégia de busca declarada — e não como
"não existe". É defensável e honesto.

---

## 4. O que hoje enfraquece a diferenciação (corrigir antes da defesa)

- **Nenhum `kas build` rodou.** Sem isso, "arquitetura de referência" é uma *proposta* de
  arquitetura. Prioridade máxima: um build limpo no QEMU vale mais que qualquer capítulo
  adicional.
- **Métrica de portabilidade fraca por construção.** `diff` vazio entre dois alvos é
  consequência do desenho, não descoberta. Fortalecer com esforço medido: linhas de metadado e
  tempo para adicionar um *terceiro* alvo ou uma terceira classe de dispositivo.
- **Sem comparação com alternativas.** Buildroot, base Debian sem Yocto, imagem baseada em
  container. Um avaliador perguntará "por que Yocto?" — responder com critérios (SBOM,
  reprodutibilidade, suporte LTS, licenciamento auditável), não com preferência.
- **Sem avaliação quantitativa.** Tempo de build, tamanho de imagem, tempo de boot,
  latência/jitter do caminho `rpmsg`, perda de amostras, tempo de rollback do RAUC.
- **Alinhamento IEC 62304 afirmado, não demonstrado.** Falta a tabela de rastreabilidade da
  seção (d).
- **Variabilidade não modelada** — ver seção (b).

---

## Fontes consultadas

- [Working Groups – ELISA](https://elisa.tech/community/working-groups/)
- [medical-devices — lists.elisa.tech](https://lists.elisa.tech/g/medical-devices)
- [The Linux Foundation Launches ELISA Project](https://www.linuxfoundation.org/press/press-release/the-linux-foundation-launches-elisa-project-enabling-linux-in-safety-critical-systems)
- [A framework for analysis and design of software reference architectures (ScienceDirect)](https://www.sciencedirect.com/science/article/abs/pii/S0950584911002333)
- [A classification of software reference architectures (TU/e)](https://research.tue.nl/en/publications/a-classification-of-software-reference-architectures-analyzing-th/)
- [Development and benefits of MDevSPICE®](https://dl.acm.org/doi/abs/10.1002/smr.1781)
- [Piloting MDevSPICE® (ICSSP 2015, PDF)](https://doras.dcu.ie/21087/1/Lepmets_et_al_Piloting_MDevSPICE_the_Medical_Device_Software_Process_Assessment_Framework_ICSSP2015.pdf)
