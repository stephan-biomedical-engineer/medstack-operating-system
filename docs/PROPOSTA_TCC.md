# Proposta de TCC — MedPlatform

**Uma arquitetura de referência em camadas, baseada em Yocto, para o software de plataforma de
dispositivos médicos embarcados**

> **Status**: proposta submetida à apreciação do orientador — 25/08/2026.
> **Complemento**: o detalhamento de execução (fases, entregáveis, riscos e ordem de corte) está em
> `PLANO_TCC.md`. Esta proposta trata do *quê* e do *porquê*; aquele documento trata do *como* e do
> *quando*.

---

## 1. Contextualização

Um dispositivo médico eletromédico moderno — de uma bomba de infusão a um tomógrafo — é, hoje, um
computador. A parte que o distingue clinicamente (a aquisição do sinal, o algoritmo, a interface com
o operador) costuma ser uma fração pequena do software embarcado; o restante é *plataforma*: kernel,
inicialização, gerência de energia, armazenamento, rede, atualização de campo, registro de eventos,
criptografia.

Essa plataforma carrega requisitos que não vêm do domínio clínico, mas da regulação. A **IEC 62304**
exige segregação verificável entre itens de software de classes de segurança diferentes (§5.3) e
tratamento explícito de software de terceiros (§8.1.2, *SOUP*). A **ISO 14971** exige que o risco
seja gerenciado ao longo do ciclo de vida. A legislação americana passou a exigir **SBOM** para
submissão (Seção 524B do FD&C Act) e, no Brasil, a **RDC ANVISA 751/2022** reorganizou o
enquadramento de dispositivos. Somam-se as expectativas contemporâneas de segurança cibernética:
atualização de campo autenticada, dado de paciente cifrado em repouso, trilha de auditoria que
resista à adulteração.

O que se observa na prática industrial é que essa plataforma é **reconstruída a cada produto**, por
equipes diferentes, e que a conformidade é tratada como uma camada de endurecimento aplicada ao
final — quando as decisões que a tornariam viável já foram tomadas de outra forma. O resultado é
duplicação de esforço, evidência de conformidade produzida a posteriori e decisões arquiteturais que
não podem ser verificadas mecanicamente.

---

## 2. Problema

> Como estruturar o software de plataforma de dispositivos médicos embarcados de modo que
> **(i)** múltiplas classes de dispositivo reaproveitem a mesma base,
> **(ii)** a aplicação clínica permaneça isolada do sistema operacional e do hardware por contrato
> verificável, e
> **(iii)** os requisitos regulatórios sejam habilitados por construção, e não acrescentados depois?

Três dificuldades sustentam o problema. Primeiro, "isolamento entre camadas" costuma ser um desenho
em documento, não uma restrição que algum mecanismo imponha — nada falha quando ela é violada.
Segundo, "reuso" costuma ser afirmado sem métrica, ou medido de um jeito que a própria construção
garante trivialmente. Terceiro, evidência de comportamento em sistemas embarcados é frequentemente
substituída por evidência de *construção*: um build verde é apresentado como se dissesse algo sobre
execução.

---

## 3. Lacuna na literatura

A busca realizada **não localizou** trabalho acadêmico publicado que proponha uma arquitetura de
referência em camadas, baseada em Yocto, específica para dispositivos médicos, com framework de
abstração de uso obrigatório e métrica de portabilidade medida. O material existente se distribui
em quatro grupos, nenhum deles ocupando essa posição:

| Grupo | Exemplo representativo | O que falta para ocupar a lacuna |
|---|---|---|
| Arquitetura de referência **sem** implementação | Angelov, Grefen & Greefhorst (2012); RAModel (Nakagawa) | A AR é artefato de projeto — documento e modelo, não código executável |
| Implementação **sem** arquitetura de referência | Relatos industriais do tipo "usamos Yocto no nosso produto" | Não generalizam nem medem reuso entre classes de dispositivo |
| Processo **sem** arquitetura | MDevSPICE (Lepmets, McCaffery et al.) | Trata do ciclo de vida, não de como o repositório de build é estruturado |
| Comunidade **sem** publicação acadêmica | ELISA (Medical Devices WG), Automotive Grade Linux | ELISA ataca certificabilidade de componentes; AGL, um domínio regulado que não é o médico |

A lacuna é enunciada como **"não localizei"**, com a estratégia de busca declarada — e não como
"não existe". É a formulação defensável.

---

## 4. Objetivo geral

Propor, implementar e avaliar experimentalmente uma arquitetura de referência em camadas para o
software de plataforma de dispositivos médicos embarcados, na qual a fronteira entre aplicação
clínica, sistema operacional e hardware seja imposta por mecanismos verificáveis, e na qual
requisitos regulatórios sejam habilitados por decisões de arquitetura — demonstrando-a por meio de
duas classes de dispositivo construídas sobre a mesma base e de dois alvos de hardware distintos.

---

## 5. Objetivos específicos

1. **Especificar** uma hierarquia de quatro camadas com dependência estritamente unidirecional, e
   definir regras de fronteira cuja violação seja detectável mecanicamente.
2. **Implementar** um framework de abstração em C++ que constitua a única superfície pela qual uma
   aplicação médica alcança o sistema operacional, cobrindo comunicação entre núcleos, persistência,
   configuração com limites de segurança, registro auditável, atualização e acesso a dispositivos.
3. **Construir** duas classes de dispositivo sobre a mesma plataforma — um sistema de aquisição de
   EEG como prova de conceito e um perfil de tomógrafo como caso de controle.
4. **Demonstrar portabilidade** construindo e executando o mesmo perfil de dispositivo em dois
   alvos: um emulado (QEMU x86-64) e um físico (STM32MP257, Cortex-A35 com coprocessador de tempo
   real Cortex-M33).
5. **Incorporar como restrições arquiteturais** os requisitos regulatórios relevantes: segregação de
   itens de software, trilha de auditoria à prova de adulteração, atualização A/B assinada e
   verificável, e dado de paciente cifrado em repouso.
6. **Definir e aplicar um protocolo de evidência** que distinga construção de execução, incluindo
   uma suíte de aceitação executável que converta "verificado uma vez" em "verificável".
7. **Avaliar quantitativamente** reuso entre classes, portabilidade entre alvos, particionamento de
   software e o caminho de atualização, declarando explicitamente o que cada medida **não**
   significa.

---

## 6. O que o trabalho traz de novo

Cinco pontos, e é neles — não em "um EEG feito com Yocto" — que a contribuição precisa ser
defendida.

### 6.1 Fronteira de camada verificável por máquina

A separação entre aplicação, sistema operacional e adaptação de hardware não é um desenho: é um
invariante que se checa. Nenhum nome de máquina, de bootloader ou de nó de dispositivo pode aparecer
acima da camada de adaptação, e uma aplicação não pode adquirir uma segunda dependência além do
framework — o que torna a lista de dependências de uma receita de aplicação um invariante
inspecionável. A relevância prática ficou demonstrada no próprio desenvolvimento: **a regra foi
violada duas vezes sem que nenhum build falhasse**, e ambas as violações só apareceram quando um
segundo alvo precisou de uma resposta diferente. Uma restrição arquitetural que nada verifica é uma
recomendação.

### 6.2 O framework como mecanismo de conformidade, não como biblioteca de conveniência

O que distingue o framework proposto de uma camada de utilidades é que **suas recusas são o
requisito**. A persistência se recusa a abrir um repositório de registros cujo dispositivo de
suporte não seja um volume cifrado; a configuração se recusa a carregar uma tabela de calibração sem
selo de integridade conferido; um parâmetro fora do envelope de segurança declarado impede o serviço
de iniciar, em vez de ser ajustado silenciosamente. A política regulatória, nesse desenho, é
*executada* — o serviço estar no ar é, ele próprio, a afirmação de que a condição vale.

### 6.3 Reuso medido sobre o artefato produzido, com caso de controle

O reuso é medido contando o que efetivamente entrou na imagem: **a plataforma comum às duas classes
de dispositivo tem 206 pacotes, e o delta específico do EEG são 3**, com o conjunto do tomógrafo
sendo subconjunto estrito do conjunto do EEG. O caso de controle foi construído para que a medida
valesse: enquanto os dois perfis diferiam também em configuração de desenvolvimento, a medição
contaria deriva acidental como se fosse diferença de classe de dispositivo. A formulação é
deliberadamente conservadora — não se reporta uma porcentagem de similaridade, porque o tomógrafo
não instala aplicação alguma e seu delta é zero por construção.

### 6.4 Superfície de adaptação declarada com precisão

A afirmação usual de portabilidade ("basta trocar o BSP") não é verificável. Aqui ela é decomposta e
medida em dois níveis: a superfície de adaptação **da aplicação** é uma variável de configuração — o
código-fonte das aplicações não difere entre os dois alvos —, enquanto a superfície de adaptação
**da plataforma** são quatro variáveis de projeto, mais uma respondida por máquina, mais a camada de
adaptação. Essa distinção importa: a versão anterior deste enunciado dizia "uma variável" para os
dois casos, e era falsa por quatro variáveis.

### 6.5 Um protocolo de evidência, e o registro dos defeitos que ele encontrou

O trabalho distingue quatro níveis de evidência — build verde, inspeção do artefato produzido, suíte
de aceitação em execução e execução no alvo físico — e sustenta que um não substitui o outro. O
resultado metodológico é mensurável: **dos treze defeitos catalogados no porte para o alvo físico,
seis não apresentaram sintoma algum** (não quebraram build, não emitiram aviso) e só apareceram por
inspeção do artefato; outros quatro só apareceram ao energizar a placa. Uma segunda regra derivada
tem consequência prática direta — **uma asserção que nunca viu a falha que procura é uma afirmação,
não uma verificação** —, e três defeitos reais haviam passado por asserções que os declaravam
saudáveis.

O registro sistemático desses defeitos, com causa real e evidência da correção, é em si um resultado
pouco comum: a literatura industrial raramente publica o percurso negativo, e é ele que torna o
trabalho reprodutível por outra equipe.

---

## 7. Fundamentação e posicionamento

O trabalho se apoia em seis correntes e contrasta com uma sétima:

| Corrente | Referência de apoio | Posição deste trabalho |
|---|---|---|
| (a) Arquiteturas de referência | Angelov, Grefen & Greefhorst (2012) | A AR é entregue como artefato **executável** — o conjunto de metadados que produz a imagem |
| (b) Linhas de produto de software | Clements & Northrop (2001); FODA; Berger et al. (2013) | Os perfis de dispositivo são produtos de uma linha; os pontos de variação são explicitados |
| (c) Linux em sistemas críticos | ELISA (Medical Devices WG); Automotive Grade Linux | Enquadramento honesto: "AGL para dispositivos médicos, em escala de TCC" |
| (d) Processo e conformidade | MDevSPICE; IEC 62304; ISO 14971; RDC 751/2022 | Contribuição de **arquitetura e ferramental**, não de processo; SBOM gerado nativamente |
| (e) AMP e criticidade mista | Burns & Davis (survey); OpenAMP/rpmsg | O AMP não é a novidade: a novidade é usá-lo como **justificativa normativa da partição** |
| (f) Cadeia de suprimentos e atualização | Lamb & Zacchiroli (2022); Schneier & Kelsey (1999) | Atualização A/B assinada e log encadeado como habilitadores de requisito |
| (g) EEG de baixo custo com ADS1299 | Ecossistema OpenBCI e derivados | **Contraste**: para eles o EEG é a contribuição; aqui é a instanciação de prova |

---

## 8. Metodologia

Pesquisa aplicada de natureza construtiva: o conhecimento é produzido pela construção de um artefato
e pela sua avaliação experimental. A avaliação se dá sobre **o que o processo de construção
efetivamente produziu** — a imagem, a tabela de partições, o pacote instalado, o registro escrito em
disco — e não sobre a descrição do que deveria ter sido produzido.

Instrumentos:

- **Dois alvos**, um emulado e um físico, para que a portabilidade seja observada e não deduzida.
- **Um caso de controle** (segunda classe de dispositivo), que é o protocolo de avaliação usual da
  literatura de linhas de produto.
- **Uma suíte de aceitação executável**, que boota a imagem e verifica asserções sobre o sistema em
  execução, retornando falha quando alguma não vale.
- **Injeção de falha** como técnica de validação das próprias asserções: uma verificação só é aceita
  depois de ter sido vista falhando na condição que existe para detectar.

---

## 9. Resultados preliminares já obtidos

A proposta não parte do zero; o que segue já está medido e registrado, com o comando que produziu
cada número.

| Resultado | Situação |
|---|---|
| Plataforma comum de 206 pacotes; delta de 3 para o EEG | Medido em simulação |
| Integridade do caminho de aquisição (3.226 quadros, sem escrita parcial) | Medido em simulação |
| Particionamento: escalonamento de tempo real concedido pelo kernel e sandbox medido | Medido em simulação |
| Atualização A/B: bundle assinado verificado no dispositivo e escrito no slot inativo | Medido em simulação |
| Trilha de auditoria selada, verificada íntegra | Medido **nos dois alvos** |
| Volume de dados de paciente cifrado, provisionado no primeiro boot | Medido **no alvo físico** |
| Boot completo do alvo físico a partir do layout de disco projetado | Medido **no alvo físico** |
| Mesmo código de aplicação e framework compilando para as duas arquiteturas | Verificado nos dois |

---

## 10. Resultados esperados

1. Fechamento do caminho de atualização no alvo físico, incluindo retorno automático ao slot
   anterior em boot falho.
2. Primeira caracterização quantitativa do caminho de aquisição entre núcleos — latência, variação e
   perda de amostras.
3. Integração de um conversor analógico-digital real de biopotenciais, com o critério de aceitação de
   que nenhuma referência ao componente apareça acima da camada de adaptação.
4. Controle de acesso obrigatório e perfil de produção com sistema de arquivos imutável exercitado.
5. Tabela de rastreabilidade ligando requisito normativo → decisão arquitetural → artefato no
   repositório.
6. Comparação fundamentada com alternativas de construção de imagem, por critérios declarados.

---

## 11. Cronograma macro

| Período | Etapa |
|---|---|
| Ago – Set | Consolidação da base medida e fechamento da atualização de campo no alvo físico |
| Set – Out | Caminho de aquisição entre núcleos, com caracterização quantitativa |
| Out – Nov | Integração do front-end analógico real |
| Nov | Segurança, perfil de produção e avaliação comparativa |
| Dez | Consolidação do texto, figuras e defesa |

O detalhamento por fase, com entregáveis, critérios de pronto, riscos e ordem de corte, está em
`PLANO_TCC.md`.

---

## 12. Referências principais

- ANGELOV, S.; GREFEN, P.; GREEFHORST, D. *A framework for analysis and design of software reference
  architectures*. Information and Software Technology, v. 54, n. 4, p. 417–431, 2012.
- CLEMENTS, P.; NORTHROP, L. *Software Product Lines: Practices and Patterns*. Addison-Wesley, 2001.
- KANG, K. et al. *Feature-Oriented Domain Analysis (FODA) Feasibility Study*. SEI, 1990.
- BERGER, T. et al. *A Study of Variability Models and Languages in the Systems Software Domain*.
  IEEE Transactions on Software Engineering, 2013.
- LEPMETS, M.; McCAFFERY, F.; CLARKE, P. *Development and benefits of MDevSPICE*. Journal of
  Software: Evolution and Process, 2015.
- BURNS, A.; DAVIS, R. *Mixed Criticality Systems — A Review*. Technical report, University of York.
- LAMB, C.; ZACCHIROLI, S. *Reproducible Builds: Increasing the Integrity of Software Supply
  Chains*. IEEE Software, 2022.
- SCHNEIER, B.; KELSEY, J. *Secure audit logs to support computer forensics*. ACM TISSEC, 1999.
- IEC 62304 — *Medical device software — Software life cycle processes*.
- ISO 14971 — *Medical devices — Application of risk management to medical devices*.
- ANVISA. *RDC nº 751/2022*.
- The Linux Foundation. *ELISA — Enabling Linux In Safety Applications*, Medical Devices Working
  Group.
