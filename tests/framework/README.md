# Verificações de host do MedFramework

```sh
make -C tests/framework check      # ou, da raiz:  make test
```

Compila os fontes do MedFramework para o **host** e exercita o comportamento da
biblioteca. Sem bitbake, sem imagem, sem alvo, sem QEMU. Leva segundos.

**1246 verificações, 0 falhas** na última execução (2026-09-16, `g++ 13`,
sem `libsystemd-dev`).

---

## Por que este diretório existe

O `CLAUDE.md` afirmava que "a functional test of `MedicalConfiguration`,
`MedicalStorage`, `MedicalDevice` e `MedicalLogger` (55 checks) passes" e que
"80 host checks of the new framework behaviour pass". **Essas verificações nunca
estiveram no repositório.** Foram rodadas fora da árvore e não foram
commitadas, o que as tornava exatamente aquilo que este projeto chama de
alegação: não reproduzíveis a partir do clone, e sem como saber se ainda
passavam.

O único teste versionado era o `scripts/med-check.py`, que é de *runtime* — 23
asserções sobre uma imagem que já bootou. Ele continua sendo o único que pega
uma regressão que um build não pega, e nada aqui o substitui. O que este
diretório cobre é a camada abaixo: o comportamento da biblioteca, com um ciclo
de segundos em vez de um build de imagem.

## Como está organizado

| Arquivo | O que cobre |
|---|---|
| `check.h` | o *runner*: contagem, `file:line`, status de saída |
| `temp_dir.h` | diretório temporário que se remove |
| `test_types.cpp` | `Status`, `Result<T>`, conversões de tempo |
| `test_configuration.cpp` | tipos, forma do arquivo, selo de integridade, limites de segurança |
| `test_storage.cpp` | recusa por criptografia, escrita/leitura, contenção de caminho |
| `test_device.cpp` | fábrica, vocabulário de opções por driver, aritmética do simulador, ABI do enlace AMP |
| `test_logger.cpp` | vocabulário de auditoria e cadeia de hash — **só com `libsystemd-dev`** |

Sem gtest e sem Catch2, pela mesma regra que governa a biblioteca: o
MedFramework tem exatamente duas dependências externas e isso é uma alegação da
tese. Um `apt install` antes de rodar a evidência é uma barreira a mais entre o
leitor e a reprodução.

## Três coisas que estes testes travam no lugar

**O comentário inline continua sendo valor.** `afe.gain = 24 # PGA` produz a
string `"24   # PGA"`, não o número 24. Isso custou uma imagem inteira e um boot
de QEMU para ser descoberto (seis asserções falharam de uma vez), e a correção
foi deliberadamente **não** mexer no parser — foi fazer o `do_seal_configuration`
recusar o arquivo no build. Se alguém "consertar" o parser, este teste falha e
aponta a razão.

**O framework não tem vocabulário de classe de dispositivo.** Cada driver tem só
o do seu transporte: o `simulated`, os parâmetros do gerador; o `rpmsg`, nenhum
(repassa ao produtor); o `iio`, nomes de atributo sysfs. A primeira seção de
`test_device.cpp` é a regressão da época em que o framework conhecia `afe.gain`
e companhia nos três drivers e recusava o resto — e prova o contrário
configurando um transdutor de pressão pelos três, sem mudar uma linha.

**O CRC-32 tem um vetor de fora deste repositório.** `0xCBF43926` é o valor de
conferência publicado do CRC-32 para `"123456789"`. É o único vetor da suíte
cuja origem é externa, e a distinção importa: um teste escrito a partir da mesma
leitura que escreveu a implementação prova **consistência, não correção**. Este
prova que o firmware do outro núcleo, escrito contra a mesma norma, calculará o
mesmo número.

## Injeção de falha

Pela regra que o `acq-active` ensinou a este repositório — *uma asserção que
nunca viu a falha que procura é uma alegação, não uma verificação* — três
defeitos foram introduzidos de propósito e a suíte os pegou:

| Defeito introduzido | O que a suíte reportou |
|---|---|
| simulador volta a ignorar chave desconhecida | `create(simulated).status() == NotSupported`, para cada chave `afe.*` |
| `rpmsg` volta a julgar chaves em vez de repassar | `forwarded.isOk()` |
| `iio` aceita `/` e `.` no nome do atributo | `create(iio).status() == InvalidArgument` |
| quantização passa a usar meio LSB | `amostra fora da grade do LSB: 0.078231` |
| contenção de caminho aceita `..` | `write aceitou '../escape'` (e `read`, `append`, `exists`, `remove`) |

Os fontes foram restaurados e conferidos byte a byte contra a cópia
pré-injeção.

Vale registrar o que a **primeira** execução pegou, antes de qualquer injeção:
`toString(Status::Ok)` devolve `"OK"` e não `"Ok"`. O teste estava errado e a
biblioteca certa — a grafia é `SCREAMING_SNAKE_CASE` porque vira valor de campo
do journal. A asserção foi corrigida e ampliada: hoje ela cobra que **todo**
`Status` tenha grafia, que nenhuma se repita e que todas sigam a convenção.

## O que esta suíte **não** cobre

O binário imprime esta lista no fim de toda execução, inclusive quando passa —
uma suíte verde que silenciosamente testou menos é o modo de falha que este
repositório já pagou uma vez (22/22 numa imagem entregando 94 das 250
amostras/s).

- **`MedicalLogger`**, quando falta `libsystemd-dev`. Instale e recompile; a
  suíte passa a cobrir o vocabulário de auditoria e a cadeia de hash.
- **`MedicalUpdate`** — é cliente D-Bus do daemon RAUC. Num host sem
  `rauc.service` no barramento, a única resposta possível é "o daemon não está
  lá", o que testa o host e não o *wrapper*. É coberto no alvo, pela asserção
  `rauc-active` do `make check`.
- **`MedicalIPC`** — o caminho de socket é exercitado no alvo, e foi lá que o
  defeito do `accept4` sem `SOCK_NONBLOCK` apareceu. Um teste de host do
  *loopback* é possível e não substituiria a asserção `acq-sample-rate`.
- **Atomicidade do `MedicalStorage`** — precisa de um corte de energia no meio
  de um `rename`. Um teste que alegasse exercitá-la chamando `write()` duas
  vezes seria teatro.
- **Tudo que depende de hardware** — nenhum módulo de kernel, nenhuma amostra
  real, nenhuma constante de datasheet. Ver
  `docs/implementation_plan_afe_bench.md`.
- **Concorrência** — a cadeia de hash do logger e a fábrica de drivers têm
  mutex, e nada aqui os exercita sob contenção.

## O passo seguinte

`tests/afe/`, a §13.1 do `docs/implementation_plan_afe_bench.md`: a máquina de
estados de transferência do MCP2210, testável no host com `mcp2210_command()`
substituído por um duplo. Pega fragmentação na fronteira de 60 bytes,
`-EMSGSIZE` acima de 512, o limite de *stalls* e o corte em
`received + got > len` — sem nenhum hardware. Custa uma costura no driver, e é
justamente a refatoração que não se quer fazer depois da bancada montada.
