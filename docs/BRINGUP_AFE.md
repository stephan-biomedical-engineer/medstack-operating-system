# Registro de engenharia — o front-end analógico

> **O que este documento é**: o registro do que foi *feito* com o front-end analógico — o que
> quebrou, qual era a causa real e o que provou cada conserto. É o companheiro do
> `BRINGUP_STM32MP2.md` para a metade do conversor, e é onde os planos
> (`implementation_plan_afe_bench.md`, `implementation_plan_iio_afe.md`,
> `implementation_plan_ads1299_upstream.md`) depositam número medido.
>
> **O que este documento não é**: plano. Nada aqui é intenção; o que está escrito aconteceu, com data.
> Onde uma seção diz "não executada", é porque a fase correspondente não aconteceu e a seção existe
> para que a ausência tenha lugar em vez de ser silêncio.
>
> **Numeração**: as §1 a §10 correspondem, uma a uma, às fases do
> `implementation_plan_afe_bench.md` — que já aponta para elas por número. A §11 é o catálogo de
> defeitos, no formato do `BRINGUP_STM32MP2.md` §11. A §12 registra a migração dos drivers para a
> árvore do kernel, que não é fase de bancada. A §13 são as regras que este trabalho acrescenta.

---

## 0. A linha, dita primeiro

| | Estado em 2026-09-26 |
|---|---|
| Hardware do front-end | **não chegou.** Nada nas §2 a §10 pôde ser executado |
| Mapa de registradores | **transcrito** do SBAS499 (`Register_Map_ADS1299.md`) |
| Passada de datasheet (Fase 0) | **parcial**: 9 marcas viraram citação de tabela, 1 continua aberta |
| Defeitos achados por ela | **quatro**, mais uma ambiguidade e três lacunas (§11) |
| Drivers na árvore do kernel | **migrados**, compilando para arm64 (§12) |
| Amostra real adquirida | **nenhuma**, por nenhuma das quatro ligações |

Nenhuma constante deste front-end foi conferida contra **silício**. Conferido contra o datasheet e
conferido contra a peça são afirmações diferentes, e só a segunda encerra a dívida no sentido que o
`BRINGUP_STM32MP2.md` §11 regra 1 dá à palavra.

---

## 1. Fase 0 — a passada de datasheet · **parcial, 2026-09-26**

### 1.1 O insumo

`docs/Register_Map_ADS1299.md`: a Tabela 11 (atribuição de registradores) e as tabelas 12 a 28
(descrição de campo por registrador) do SBAS499, transcritas. Existe para que conferir uma constante
do driver seja uma consulta neste repositório e não num PDF que mais ninguém tem.

### 1.2 O que foi conferido

| Registrador | Tabela | Veredito |
|---|---|---|
| `ID` (0x00) | 12 / Fig. 50 | **confere** — `GENMASK(4,2) == 0x7` consolida o bit 4 "sempre lê 1" com `DEV_ID=11`; canais por `4 + 2·NU_CH` |
| `CONFIG1` (0x01) | 13 / Fig. 51 | **defeito D2** — faltava o segundo campo reservado (4:3 = 2h) |
| `CONFIG2` (0x02) | 14 / Fig. 52 | **defeito D1** — todos os campos uma posição acima |
| `CONFIG3` (0x03) | 15 / Fig. 53 | **confere**; e a falta de `BIAS_STAT` virou lacuna L1 |
| `LOFF` (0x04) | 16 / Fig. 54 | **confere**; nunca escrito, então a peça fica no reset — escolha de configuração feita por deixar o registrador como veio |
| `CHnSET` (0x05–0x0C) | 17 / Fig. 55 | **confere** — `PD` 7, `PGA` 6:4, `SRB2` 3, `MUX` 2:0 |
| Tabela de ganhos do PGA | 17 | **confere** — `{1,2,4,6,8,12,24}`, sete entradas porque 111 é "do not use" |
| Valores de `MUX` | 17 | **conferem**, os oito |
| `CONFIG4` (0x17) | 28 / Fig. 66 | **confere** — `SINGLE_SHOT` 3, `PD_LOFF_COMP` 1 |
| `LOFF_SENSP/N` (0x0F–0x10) | 20, 21 | **conferem**; nunca escritos → **defeito D3** |
| Amplitude do gerador de teste | 14 | **o datasheet não decide** — ambiguidade A1 |

Contagem de dívida: de 8 marcas `[SBAS499?]` originais, **9 viraram citação de tabela** e **1
permanece** — e essa uma deixou de ser leitura pendente para ser medição de bancada explícita.

### 1.3 O que a passada custou, e o que ela rendeu

Duas horas de leitura, dois registradores lidos com atenção antes do primeiro defeito aparecer. É o
primeiro argumento empírico de que a Fase 0 não é burocracia: **nenhuma das verificações posteriores
enxergaria o D1** (§11).

### 1.4 O que falta

- A seção de **temporização** do SBAS499 (§6/§7) não foi transcrita. Consequência imediata: o
  binding não pode declarar um máximo para `spi-max-frequency`, e os 4 MHz do exemplo vêm do
  parâmetro que a ponte usava, não do datasheet.
- O datasheet do **MCP2210** (DS20005176) não foi aberto. As 8 marcas `[DS20005176?]` continuam
  intactas.

---

## 2. Fase 1 — a ponte sozinha, sem conversor · **não executada**
## 3. Fase 2 — o barramento SPI transfere bytes · **não executada**
## 4. Fase 3 — o conversor responde, identidade do silício · **não executada**
## 5. Fase 4 — o caminho analógico, estático · **não executada**
## 6. Fase 5 — aquisição contínua, e as quatro medidas · **não executada**
## 7. Fase 6 — framework, aplicação e suíte · **não executada**
## 8. Fase 7 — as duas ligações lado a lado · **não executada**
## 9. Fase 8-A — varredura do datasheet do conversor · **não executada**
## 10. Fase 8-B — varredura do datasheet da ponte · **não executada**

Todas dependem do módulo, exceto a §10 (que roda com placa de avaliação e jumper) e a §13.1 do plano
de bancada (testes de host da máquina de transferência, que não precisam de hardware nenhum).

---

## 11. Defeitos, ambiguidades e lacunas

Formato do `BRINGUP_STM32MP2.md` §11: o que era, o que teria causado, e o que o pegou.

### D1 — `CONFIG2` com todos os campos uma posição acima

**O que era.** Reservado declarado em 7:6 em vez de 7:5; `INT_CAL` no bit 5 em vez do 4; `CAL_AMP`
no bit 3 em vez do 2 — sendo o bit 3 o *outro* campo reservado, que o datasheet manda escrever 0.

**O que teria causado.** Calculado antes e depois:

| escrita | antes | depois | o que o antes fazia |
|---|---|---|---|
| `off` | `0xc0` | `0xc0` | correto por acidente |
| `1x_slow` | `0xe0` | `0xd0` | reservado 7h (proibido) e **`INT_CAL` = 0** |
| `2x_slow` | `0xe8` | `0xd4` | idem, mais o bit 3 reservado escrito como 1 |

O gerador de teste interno **nunca era ligado**. Em silício: o autoteste do *probe* pede o sinal, lê
entrada em curto, o `swing` reprova, `dev_err_probe` recusa registrar — sintoma ("não aparece
`/dev/med-afe-eeg0`") a três passos da causa (um bit), e uma sessão de bancada gasta olhando
alimentação e fiação.

**O que o pegou.** Leitura do datasheet. **Nada mais poderia**: o valor de reset que a transcrição
errada produz é `0xc0`, que é o correto — uma varredura de registradores contra valores de reset é
**cega** a erro de posição de campo que preserve o reset, e uma releitura de escrita também, porque o
registrador é R/W e devolve o que se escreveu. Só a leitura do datasheet ou um osciloscópio
discordam. O critério 5 da Fase 3 do plano de bancada ganhou essa ressalva por causa deste defeito.

### D2 — `CONFIG1` sem o segundo campo reservado

**O que era.** O datasheet manda escrever 1 no bit 7 **e 2h nos bits 4:3**. O driver declarava só
`BIT(7)`.

**O que teria causado.** Nada, hoje — e a razão importa mais que o fato: `CONFIG1` só é tocado por
`regmap_update_bits()` no campo `DR`, que preserva os bits do reset da peça. O defeito era **latente
e armado**: com `CONFIG2` e `CONFIG3` reescritos no idioma `_RESERVED_VALUE`, quem alcançasse
`MASK_CONFIG1_RESERVED` seguindo o mesmo idioma escreveria `0x80 | DR` e zeraria o bit 4 **em toda
troca de taxa**. Conferido: o valor correto é `0x90`, e `0x90 | 6 = 0x96`, que é o reset do datasheet.

**O que o pegou.** A mesma leitura. E vale a nota: a armadilha foi *criada* ao harmonizar os dois
registradores vizinhos — consertar um registrador armou o seguinte.

### D3 — `lead_off_status` reportava bytes que o datasheet manda ignorar

**O que era.** O driver liga os comparadores de *lead-off* (`CONFIG4.PD_LOFF_COMP`) com o comentário
*"so lead_off_status means something"*, e **nunca** escreve `LOFF_SENSP`/`LOFF_SENSN`. O datasheet é
explícito: os bits de `LOFF_STATP` só são válidos onde o bit correspondente de `LOFF_SENSP` está em 1.

**O que teria causado.** O atributo lia dois bytes inválidos, e eles leem `00 00` — indistinguível de
**"todos os eletrodos conectados"**. Um status que não consegue relatar a falha que existe para
relatar, reportando justamente o valor tranquilizador.

**O que o pegou.** A leitura da Tabela 20/21 junto com o `grep` de quem escreve o quê. **Conserto
aplicado**: o atributo lê `LOFF_SENS*` primeiro, responde `disabled` quando nenhum canal tem detecção
habilitada, e mascara o status aos bits válidos quando tem. **Conserto não aplicado**: habilitar a
detecção por canal, porque isso injeta a corrente de *lead-off* nos eletrodos do paciente e a
magnitude vem do `LOFF`, que ninguém configura — é decisão clínica, não transcrição.

Efeito colateral: a recusa que o `do_derive_device_options` faz a `afe.lead_off_detection = false`
alega que *"o driver habilita os comparadores no probe e não oferece controle para desligar"* —
**meia verdade**. Comparadores ligados, detecção desligada.

### D4 — o driver não fixa o modo SPI

**O que era.** Nenhum `spi->mode` em lugar nenhum do driver. O ADS129x exige CPOL=0, CPHA=1.

**O que teria causado.** No caminho da ponte USB o modo vinha do parâmetro de módulo do MCP2210, e
funcionava por acidente de quem instanciava o dispositivo. Num **controlador SPI real** — que é a
ligação `spi`, o modo hat — o barramento subiria em modo 0 e o driver leria lixo com convicção.

**O que o pegou.** Escrever o binding do devicetree. A pergunta "quem declara o modo?" não tinha
resposta no código.

**Como foi fechado.** No devicetree, não no driver: `spi-cpha: true` declarada **e exigida** no
binding. Um DT sem ela é recusado pelo esquema, o que torna impossível a placa que configura o
barramento errado. Forçar `spi->mode` no driver continua sendo alternativa em aberto, e a decisão foi
descrever primeiro.

### A1 — a amplitude do gerador de teste: o datasheet não decide, e as camadas discordam

A Tabela 14 dá a amplitude como `1 × −(VREFP − VREFN)/2400` e **não diz** se isso é pico, pico a pico
ou passo diferencial. Fator de dois. E o repositório carrega **as duas leituras, em camadas que nunca
se encontram**:

| Onde | Afirma |
|---|---|
| `eeg.conf:57` e a tabela `simulated` da receita | gerador oscila VREF/2400 = 1,875 mV |
| `ti-ads1299.c`, `ADS1299_TEST_SIGNAL_PP_DIVISOR` | pico a pico é VREF/1200 = 3,75 mV |

Se a leitura certa for VREF/2400, o autoteste espera o dobro da oscilação real e **reprova silício
bom** — mesma forma do D1, alcançada pelo lado oposto. Um número alimenta um simulador e o outro uma
tolerância, então nenhum dos dois jamais pegaria o outro.

**Não resolvido de propósito.** Virou `[SBAS499? — o datasheet não decide. Medir na Fase 4]`, com as
duas leituras e a consequência escritas no fonte. Um palpite que faz um autoteste passar é pior que
um que o faz falhar.

### L1 a L3 — lacunas, não defeitos

- **L1, `BIAS_STAT` (CONFIG3 bit 0) não é reportado.** É o *lead-off* do eletrodo de BIAS: 0
  conectado, 1 não conectado. Expor exige pôr `CONFIG3` na tabela de voláteis do `regmap` na mesma
  mudança — o registrador é cacheado, e um atributo que o lesse reportaria "conectado" para sempre.
  (A redação anterior desta linha afirmava que um eletrodo de referência solto corrompe todos os
  canais ao mesmo tempo. É mais forte do que a folha de dados do registrador sustenta, e foi
  suavizada aqui e no comentário do driver — o efeito elétrico é hipótese, o estado é o que o bit
  reporta.)
- **L2, `SRB1` (MISC1 bit 5) não tem controle.** É a montagem de referência comum, que é como um
  gorro de EEG normalmente se liga. Decisão de hardware pendente: a chave da PCB alterna GND e o
  eletrodo do canal, e **o software não consegue ler a posição dela** — então o registro não pode
  afirmar a montagem. Sugestão registrada: ligar a chave a um dos 4 GPIOs do próprio conversor, que
  a diretriz de completude vai implementar de qualquer forma, e a montagem passa a ser medida em vez
  de afirmada.
- **L3, `BIAS_SENSP/N` nunca escritos**, coerente com o amplificador de bias estar inerte.

---

## 12. A árvore do kernel como fonte de verdade · **2026-09-26**

Decisão de arquitetura e sua execução, no mesmo dia. O detalhamento está no
`implementation_plan_ads1299_upstream.md` §2.2 e §2.3; aqui fica o que foi medido.

### 12.1 A base validada

| Item | Valor |
|---|---|
| Versão | 6.6.129, release ST `r3.1` → `PV = 6.6.129-stm32mp-r3.1` |
| Aquisição padrão | tarball `linux-6.6.129.tar.xz` + **um** patch, `0001-v6.6-stm32mp-r3.1.patch` |
| sha256 do tarball | confere byte a byte com `SRC_URI[kernel.sha256sum]` |
| Delta da ST | 4,9 MB, **162.253 linhas, 749 arquivos** |
| Ponto Git equivalente | `STMicroelectronics/linux`, `v6.6-stm32mp`, `548f960c059bc4b9165cb69895fd67551f6061ca` |
| Tag conferida na placa | `git tag --points-at 548f960c` → **`v6.6-stm32mp-r3.1`** |

### 12.2 A equivalência, provada antes de migrar

`scripts/med-kernel-fingerprint.sh` compara **conteúdo** e nunca metadados — modo, mtime e
diretórios vazios divergem legitimamente entre tarball desempacotado e checkout git, e nenhum deles
muda o kernel compilado.

| Verificação | Resultado |
|---|---|
| Arquivos, árvore do Yocto | 81889 |
| Arquivos, checkout Git | 81889 |
| Comuns **efetivamente comparados** | 81889 |
| Com hash divergente | **0** |
| Digest das duas | `35e19311ab288cf56345ea6396d47d0cc071dab2dfe7ed028c575f2202b3bf50` |
| `cmp` dos manifestos | idênticos, arquivo por arquivo |

**A ferramenta precisou de duas exclusões, e as duas apareceram ao executá-la, não ao raciocinar
sobre ela**: `.pc/` (768 backups pré-patch do quilt, um por arquivo que o patch da ST toca) e
`patches/` (o `series` e o symlink do quilt na raiz). Sem elas a comparação acusava 770 divergências
onde não havia nenhuma — uma verificação **incapaz de passar**, que é pior que nenhuma porque produz
alarme falso e consome a atenção que o alarme verdadeiro precisaria.

Segunda disciplina, que vale registrar: a contagem de arquivos comuns foi conferida (81889, e não
zero) **antes** de aceitar "nenhum hash divergente". Uma comparação que não compara nada também
devolve "nenhuma diferença".

### 12.3 O que foi para a árvore do kernel

Clone `linux-med`, branch `feature/ads1299-mcp2210-driver` a partir do commit validado — digest
inalterado, o que prova que a branch parte da árvore provada.

| # | Commit | O quê |
|---|---|---|
| 1 | `8ad9d41cda64` | `dt-bindings: iio: adc: add TI ADS1299 biopotential AFE` |
| 2 | `2b605504d251` | `iio: adc: ti-ads1299: add TI ADS1299 biopotential AFE driver` + `Kconfig` + `Makefile` + `MAINTAINERS` |
| 3 | `0c09dda8f323` | **A** — `fix the CONFIG2 field positions` (defeito D1) |
| 4 | `1aece8814b26` | **B** — `write both reserved fields of CONFIG1` (defeito D2) |
| 5 | `bf2119c02d99` | **C** — `spell out the CONFIG3 reserved value` |
| 6 | `55c319262d75` | **D** — `do not report lead-off status that is not valid` (defeito D3) |
| 7 | `da723f985714` | **E** — `cite the datasheet tables that were checked` |

A ordem não é arbitrária. **A antes de B** porque é A que introduz o idioma `_RESERVED_VALUE`, e é
esse idioma que torna o `CONFIG1` perigoso — a justificativa de B não existiria antes de A. E **E por
último** porque anotar o que foi conferido só faz sentido depois de os consertos terem entrado.

Dois commits anteriores, `f38399ec6832` e `efc4220f6c3a`, foram reescritos para os hashes 1 e 2 acima:
autoria e `Signed-off-by` passaram a trazer nome legal, que é o que a DCO do kernel certifica, e o
`MODULE_AUTHOR` e a entrada de `MAINTAINERS` foram para dentro do commit que adiciona o driver, onde
pertencem. Nada foi fundido.

**O congelamento vai dobrar A a E no commit 2**, e isso é esperado: uma série enviada ao kernel
apresenta o driver já correto, e não a sequência "introduz / acha defeito / corrige". O valor
histórico dessas cinco mensagens fica aqui, na §11, que é fora do git.

Medido na série:

- **compila para arm64 com `W=1`, zero avisos** (`CC [M] drivers/iio/adc/ti-ads1299.o`, ELF aarch64);
- `allmodconfig` resolve `CONFIG_TI_ADS1299=m` sozinho, o que prova que o `Kconfig` expressa
  dependência de verdade e não decoração;
- `make dt_binding_check` limpo, com o esquema **observado recusando** uma propriedade inesperada
  (`spi-cpha`, antes de ser declarada) — que é o que faz "o exemplo valida" significar algo;
- `checkpatch --strict --git` sobre os sete commits: **0 erros em todos**. Os cinco de correção
  saem `0/0/0` — *"no obvious style problems and is ready for submission"*. O ruído está só nos dois
  de introdução: o advisório de `MAINTAINERS`, que dispara em todo arquivo novo, um `CHECK` sobre um
  argumento de macro, e duas vezes o mesmo falso positivo sobre um membro declarado `__aligned(8)`;
- e o `spdxcheck` só passou a rodar depois de `ply` e `GitPython` existirem no ambiente. Antes disso
  ele abortava com `ModuleNotFoundError` **e o `checkpatch` seguia adiante** — ou seja, uma
  verificação que não acontecia parecia uma verificação que passava.

### 12.3.1 O arquivo in-tree **não** é idêntico ao do repositório do projeto

A verificação que eu tinha proposto era comparar os dois arquivos e exigir igualdade. Ela está errada,
e a correta é outra: comparar o **código**, com os comentários removidos. Assim ele é **byte a byte
idêntico** — zero diferença.

O que difere são comentários, e as diferenças são deliberadas, para o arquivo servir a uma submissão:

- saíram os selos `- CHECKED 2026-09-26`: a data de uma leitura mora no git, não no fonte;
- saíram as referências a `BRINGUP_STM32MP2.md` e ao plano de bancada — documento de repositório
  privado não pertence a um arquivo do kernel;
- e saíram as duas afirmações suavizadas, que estavam **também no fonte** e não só nas mensagens
  (ver L1 na §11).

Mais uma alteração mecânica: o `*/` de um comentário de banner passou para a própria linha, que era o
único achado real do `checkpatch` no arquivo original.

### 12.4 O que ficou pendente

- As **quatro correções de datasheet** (D1, D2, D3 e a documentação de `CONFIG3`) ainda não foram
  committadas na árvore do kernel — vão uma por commit, cada uma citando sua tabela.
- O `mcp2210-spi.c` ainda não migrou. Ao migrar, sai com ele o
  `static char *spi_device = "ads1299"`: uma ponte que instancia um dispositivo **por nome do
  conversor** é dependência real, em código, e enquanto ela existir os dois drivers não são
  independentes, apenas parecem.
- O `S:class-devupstream` da receita da ST **aparentemente** não é ajustado para `${WORKDIR}/git`.
  É leitura da receita, **não observação de build**, e fica pendência até a primeira integração dizer.
- Cinco comentários no `ti-ads1299.c` ainda justificam decisões citando MCP2210 e USB. O critério
  `grep -ci 'mcp2210\|usb\|bridge'` só vai a zero quando eles forem reescritos em termos genéricos.

### 12.5 A diferença de base 6.6 → mainline

A submissão é contra a mainline; a placa roda 6.6.129. Um arquivo, duas bases. A expectativa — e é
expectativa, não medida — é que o custo seja pequeno: o *backport* 6.9→6.6 do arquivo de origem já
custou **zero linhas de API**, medido. O que se sabe que precisará de retrabalho no congelamento:

- o binding, que aqui é arquivo novo porque a base 6.6 não tem o irmão, e contra a mainline vira
  **extensão do `ti,ads1298.yaml`**. Pré-criar esse nome nesta árvore seria pior: produziria conflito
  *add/add* no rebase em vez de uma conversão mecânica;
- os *hunks* de `drivers/iio/adc/Kconfig` e `Makefile`, porque o patch da ST já toca esses dois
  arquivos (20 arquivos sob `drivers/iio` e `drivers/spi` no total). Os `.c`, sendo arquivos novos,
  atravessam sem atrito.

---

## 13. As regras que este trabalho acrescenta

1. **Um defeito de posição de campo que preserva o valor de reset é invisível a toda verificação de
   registrador.** Nem varredura de reset, nem releitura de escrita. Só o datasheet ou o
   osciloscópio. (D1)
2. **Consertar um registrador pode armar o vizinho.** Harmonizar `CONFIG2` e `CONFIG3` criou a
   armadilha do `CONFIG1`, que não existia antes. (D2)
3. **Um status que não pode relatar a falha reporta o valor tranquilizador.** `00 00` lido de
   registradores inválidos é idêntico a "todos os eletrodos conectados". (D3)
4. **Escrever o binding é uma revisão do driver.** A pergunta "quem declara isto?" encontra o que o
   código esqueceu de declarar. (D4)
5. **Uma ambiguidade de datasheet é medição, não escolha.** Quando a folha não decide, registrar as
   duas leituras e a consequência vale mais que escolher uma — e um palpite que faz um autoteste
   passar é pior que um que o faz falhar. (A1)
6. **Uma ferramenta de comparação nova é suspeita até provar que pode reprovar.** As duas exclusões
   que a impressão digital precisou só apareceram ao executá-la, e sem elas ela era incapaz de
   passar. (§12.2)
7. **Antes de aceitar "nenhuma diferença", conferir que algo foi comparado.** Uma comparação vazia
   devolve o mesmo resultado que uma comparação bem-sucedida. (§12.2)
