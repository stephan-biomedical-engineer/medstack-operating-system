# Build containerizado com kas

> **Status**: documento operacional, não plano. Descreve como o repositório é construído hoje.
> O `Makefile` na raiz é a interface; este documento explica o que ele faz e por quê.
>
> **Verificado nesta máquina** em 2026-08-15 (Ubuntu, kernel 6.8): Docker 29.6.1 instalado via
> snap, usuário no grupo `docker`, `/dev/kvm` presente com gid 109 e acessível.
> Os limites descritos na §5 foram **medidos**, não presumidos.

---

## 1. Por que containerizar

Um build Yocto depende de uma lista longa e sensível a versão de pacotes do host (`gawk`,
`chrpath`, `diffstat`, `texinfo`, `zstd`, `python3-jinja2`, locale UTF-8, …). Enquanto essa lista
for um pressuposto tácito da máquina do autor, "plataforma reprodutível" é uma afirmação sem
lastro — e reprodutibilidade é justamente parte do que este trabalho reivindica.

Containerizar move o host de build para dentro da configuração registrada, ao lado do SBOM
(`create-spdx`) e do `buildhistory` que o `med-os.conf` já habilita. É o mesmo argumento de gestão
de configuração da IEC 62304 §5.1, aplicado ao ambiente de construção e não só ao produto.

O que **não** muda: as camadas, as receitas e os arquivos `kas/*.yml` são idênticos dentro e fora
do container. `NATIVE=1` em qualquer alvo do `Makefile` usa o kas do host — os dois caminhos
compartilham `downloads/` e `sstate-cache/`.

## 2. Uso

```bash
make tool        # baixa e confere o kas-container fixado (uma vez)
make risks       # checagens parse-only: segundos, rodar antes de qualquer build longo
make qemu        # constrói med-image-eeg para qemux86-64
make runqemu     # boota o resultado com KVM, console serial
make help        # lista completa
```

Sequência recomendada para o primeiro build, do mais barato ao mais caro:

| Passo | Comando | Custo | O que falha aqui |
|---|---|---|---|
| 1 | `make tool` | segundos | rede, hash do script |
| 2 | `make checkout` | minutos, ~10 GB | URLs e branches das camadas |
| 3 | `make layers` | segundos | resolução de camadas, `LAYERSERIES_COMPAT` |
| 4 | `make risks` | segundos | `.bbappend` órfão do `rauc-conf`, pino do meta-qt6 |
| 5 | `make parse` | ~1 min | sintaxe de receita, `DEPENDS` |
| 6 | `make framework` | ~20 min | primeira compilação cruzada do MedFramework |
| 7 | `make service` | ~10 min | `do_seal_configuration`, empacotamento da unit |
| 8 | `make qemu` | horas | `RDEPENDS` dos packagegroups, `do_rootfs`, Qt |

Os passos 6 e 7 existem porque `RDEPENDS` de packagegroup só é resolvido no `do_rootfs`: compilar
o caminho de aquisição isolado é a forma mais rápida de chegar a um build verde antes de encarar o Qt.

## 3. O que o `Makefile` fixa

| Item | Valor | Por quê |
|---|---|---|
| Script `kas-container` | 5.2, **verificado por sha256** | mesma versão do kas do host; um script móvel desfaria a reprodutibilidade que motiva o container |
| Imagem | `ghcr.io/siemens/kas/kas:5.2` | derivada da mesma versão (`KAS_IMAGE_VERSION`) |
| `DL_DIR` | `<repo>/downloads` | montado em `/downloads` no container |
| `SSTATE_DIR` | `<repo>/sstate-cache` | montado em `/sstate` |
| `KAS_BUILD_DIR` | `<repo>/build` | montado em `/build` |

O script não é *vendorizado* no repositório: são 780 linhas de código de terceiros que poluiriam o
diff. A pinagem por versão **e** hash no `Makefile` dá a mesma garantia com um artefato de duas
linhas. Para trocar de versão, atualizar `KAS_VERSION` e `KAS_CONTAINER_SHA256` juntos.

## 4. A armadilha do sstate (mudança em `kas-base.yml`)

Este é o motivo pelo qual containerizar não é só "rodar o mesmo comando dentro do Docker".

O `kas-base.yml` declarava:

```
DL_DIR = "${TOPDIR}/../downloads"
SSTATE_DIR = "${TOPDIR}/../sstate-cache"
```

Dentro do container, `TOPDIR` é `/build`. Logo `DL_DIR` resolveria para `/downloads` — que por
coincidência **é** o ponto de montagem — e `SSTATE_DIR` para `/sstate-cache`, que **não é**: o
kas-container monta o cache em `/sstate`. Com atribuição forte (`=`), o `local.conf` vence a
variável de ambiente, e o resultado seria um sstate gravado dentro do sistema de arquivos efêmero
do container, descartado a cada execução (`--rm`) — ou seja, todo build partindo do zero, que é
exatamente o oposto do que se quer de um cache.

A correção é usar atribuição fraca:

```
DL_DIR ?= "${TOPDIR}/../downloads"
SSTATE_DIR ?= "${TOPDIR}/../sstate-cache"
```

O kas repassa `DL_DIR` e `SSTATE_DIR` para o bitbake via `BB_ENV_PASSTHROUGH_ADDITIONS`
(`kas/libkas.py`), e variáveis vindas do ambiente já estão no *datastore* quando o `local.conf` é
lido. Com `?=`, o ambiente vence dentro do container e o valor local continua valendo no build
nativo. Um único par de arquivos serve aos dois modos.

## 5. Limites medidos nesta máquina

**Sem X11 dentro do container.** O Docker aqui é o pacote *snap*, cujo confinamento restringe bind
mounts. Montar `/tmp/.X11-unix` "funciona" sem erro, mas o diretório chega **vazio** ao container
(verificado: o host tem `X0` e `X1`, o container vê `total 0`). Consequência prática: o teste
gráfico da HMI Qt/Wayland **não** roda pelo container. Três saídas:

1. `make runqemu NATIVE=1` — kas do host, janela SDL normal. É a mais simples;
2. `make runqemu` e testar apenas o caminho de aquisição pelo console serial;
3. QEMU com VNC (`runqemu ... qemuparams="-vnc :0"`) e porta publicada, se o teste gráfico
   precisar mesmo sair do container.

Mantida como está: o container cobre o *build*, que é o que precisa ser reprodutível; o teste
gráfico é interativo e local por natureza.

**Bind mount do repositório funciona** porque o projeto está sob `$HOME`. Movê-lo para fora
(`/opt`, `/srv`, disco externo montado em `/mnt`) quebraria o Docker snap.

**KVM exige o gid do host.** O `Makefile` lê `stat -c %g /dev/kvm` e passa `--device /dev/kvm
--group-add <gid>`; sem o `--group-add`, o dispositivo aparece mas o usuário do build não tem
permissão e o `runqemu` cai para TCG (ordens de magnitude mais lento) sem avisar de forma óbvia.

**O repositório é montado somente-leitura** no alvo `build` (padrão `--repo-ro` do kas-container),
e leitura-escrita no `shell`. Nada do build escreve na árvore de fontes.

**Espaço em disco**: o `tmp/` do build cresce dezenas de GB. Ele fica em `build/`, no repositório,
não dentro do Docker — mas a imagem do kas (~2 GB) vive em `/var/snap/docker/common/var-lib-docker`.

## 6. Fora de escopo (por ora)

- **CI**: os mesmos alvos rodam em GitHub Actions/GitLab CI sem alteração, já que o container é o
  mesmo. Não configurado porque o repositório ainda não tem um build verde para proteger.
- **Pinagem por digest**: `KAS_IMAGE_VERSION` fixa a *tag* `5.2`, não o digest. Fixar
  `ghcr.io/siemens/kas/kas@sha256:...` é o passo final de reprodutibilidade e faz sentido depois do
  primeiro build verde, quando existir um digest sabidamente bom para registrar.
- **`buildtools-tarball`**: alternativa do próprio Yocto ao container para normalizar o host.
  Resolve menos (só a toolchain de build, não o sistema base) e não foi adotada.
