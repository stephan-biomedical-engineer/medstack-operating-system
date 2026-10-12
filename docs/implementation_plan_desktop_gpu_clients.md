# Plano — clientes acelerados no ambiente de trabalho, na placa

> **Estado**: Fases 0, 1, 2 e 4 cumpridas; Fase 3 parcial (§7). A HMI desenha pela GPU dentro do
> ambiente, na placa. O perfil da placa em `kas/` **não** foi mudado no repositório: a linha do
> ensaio fica fora dos commits até a Fase 3 fechar.
> **Origem**: `implementation_plan_desktop.md` §6.4 — na STM32MP257F-DK o compositor do ambiente
> (labwc, sobre wlroots 0.17.4) sobe na GPU e mostra imagem, e a HMI cai com SIGSEGV em laço.
> **Decisão do operador (2026-10-12)**: seguir pelo porte no wlroots, em vez de deixar o ambiente
> só no QEMU.
> **Onde mora**: tudo na camada adjunta `meta-med-desktop`, como *patches* da receita `wlroots`.
> Zero linhas nas quatro camadas da plataforma; a HMI não muda.

---

## 1. O problema, como foi medido

| Fato | Onde |
|---|---|
| A HMI cai na `QSGRenderThread`, dentro de `libwayland-client`, logo após `wl_compositor.create_surface` | §6.4, `exception-trace` e `WAYLAND_DEBUG` |
| O driver da GPU conhece `wl_viv`, `wl_drm` e `EGL_WL_bind_wayland_display` | §6.4, `strings` |
| O labwc anuncia 42 globais, com `wl_drm` e `zwp_linux_dmabuf_v1`, e **sem `wl_viv`** | §6.4 |
| O weston, na mesma placa e com o mesmo driver, roda a mesma HMI sem reinícios | `BRINGUP_HMI_STM32MP2.md` |

A leitura: o EGL do **cliente** deste driver entrega quadros por um protocolo do fabricante. Esse
protocolo só passa a existir no compositor quando ele chama `eglBindWaylandDisplayWL` - é o driver,
dentro do processo do compositor, que registra o global. Sem o global o cliente usa um objeto nulo.

**O que ainda é inferência**: que o weston anuncia `wl_viv` nesta placa. Ninguém olhou. É a Fase 0.

## 2. O que o wlroots tinha e o que tem

Lido no repositório do wlroots (espelho em `downloads/git2/`), nas tags indicadas.

| | 0.14.1 (tinha) | 0.17.4 (o que usamos) |
|---|---|---|
| Chamar o *bind* | `wlr_egl_bind_display()`, `render/egl.c:401` | removido; só resta o ponteiro `eglQueryWaylandBufferWL` no cabeçalho da 0.15 e nada depois |
| Reconhecer o buffer do driver | `eglQueryWaylandBufferWL(EGL_TEXTURE_FORMAT)` | não existe |
| Importar como imagem | `eglCreateImageKHR(EGL_WAYLAND_BUFFER_WL)`, `render/egl.c:470-500` | não existe |
| Virar textura | `gles2_texture_from_wl_drm()`, `render/gles2/texture.c:235`, alvo `GL_TEXTURE_EXTERNAL_OES`, com `inverted_y` | `gles2_texture_from_buffer()` aceita **dmabuf** ou **ponteiro de dados**, e mais nada. A textura não tem mais campo `inverted_y` |

E os pontos de encaixe que a 0.17.4 oferece, que são o motivo de isto ser um porte e não uma
reescrita:

- `wlr_buffer_register_resource_interface()` (`include/wlr/interfaces/wlr_buffer.h`): qualquer
  tipo de `wl_buffer` pode ser apresentado ao resto da biblioteca, com `is_instance` e
  `from_resource`. É como `wl_shm`, `wl_drm` e o dmabuf entram hoje.
- `wlr_renderer_init_wl_display()` (`render/wlr_renderer.c:225`): o único lugar onde o renderizador
  vê o `wl_display`. O labwc o chama uma vez (`src/server.c:393`).
- `wlr_client_buffer_create()` (`types/buffer/client.c:57`): a textura é criada **no commit**,
  enquanto o recurso do cliente ainda existe.
- O *shader* para textura externa (`tex_ext`) e `glEGLImageTargetTexture2DOES` continuam lá.

## 3. O desenho

Três *patches*, cada um aplicável e testável sem o seguinte.

**P1 — o *bind*.** Em `wlr_renderer_init_wl_display()`, se o renderizador é o GLES2 e o EGL tem
`EGL_WL_bind_wayland_display`, chamar `eglBindWaylandDisplayWL`. Nada mais. Com isto sozinho o
global aparece e o cliente deixa de usar um objeto nulo; os quadros dele ainda não são entendidos.

**P2 — o tipo de buffer.** Um `wlr_buffer` novo que embrulha o recurso do cliente, registrado por
`wlr_buffer_register_resource_interface()`. `is_instance` pergunta ao EGL
(`eglQueryWaylandBufferWL`); largura e altura vêm da mesma consulta. Não oferece dmabuf nem
ponteiro de dados - só o renderizador GLES2 sabe o que fazer com ele. Escuta a destruição do
recurso.

**P3 — a textura.** Um terceiro ramo em `gles2_texture_from_buffer()`: se o buffer é do tipo de
P2, `eglCreateImageKHR(EGL_WAYLAND_BUFFER_WL)` e uma textura. O alvo segue o que o EGL responder
em `EGL_TEXTURE_FORMAT` - `GL_TEXTURE_2D` para RGB/RGBA, externo para `EGL_TEXTURE_EXTERNAL_WL` -
que é o que o weston faz, e não o "sempre externo" da 0.14.

Como ficou escrito (2026-10-12), no que difere do desenho acima:

- P2 e P3 são um *patch* só (`0003`), porque um sem o outro não muda nada que se veja.
- O tipo de buffer entra por um ponto novo, `wlr_buffer_register_fallback_resource_interface()`:
  é perguntado **por último**, depois de `wl_shm`, dmabuf e os demais. A função que o reconhece
  entrega o recurso ao driver, e um driver só deve ver o que ninguém mais reconheceu - a ordem de
  registro não garantia isso.
- O recurso é do driver, com os dados de usuário dele; o `wlr_buffer` correspondente é
  reencontrado pelo *listener* de destruição do recurso.
- Duas variáveis para injeção: `WLR_EGL_NO_BIND_WL_DISPLAY=1` (P1) e
  `WLR_GLES2_NO_EGL_CLIENT_BUFFERS=1` (a textura).
- Origem no canto inferior: **não tratada**. O *patch* registra um erro por buffer e desenha de
  cabeça para baixo.

Decisões já tomadas:

- **O `wl_drm` do próprio wlroots.** Se o *bind* fizer o driver registrar um `wl_drm` dele, haverá
  dois globais com o mesmo nome e o cliente pode escolher o errado. Quando o *bind* tem sucesso,
  o wlroots **não** cria o seu. A Fase 0 adiantou a resposta: o weston desta série não implementa
  `wl_drm`, e a HMI o recebe sob o weston - logo quem o registra é o driver. A Fase 1 confere
  pela contagem.
- **Sem *direct scanout* para estes buffers.** O grafo de cena tenta, pede dmabuf, não recebe e
  compõe. É o comportamento certo e não custa código.
- **O `is_instance` precisa do EGL e só recebe o recurso.** Um ponteiro estático, preenchido no
  *bind*. Feio e suficiente: há um renderizador por compositor.

## 4. Fases

Cada rodada na placa é um `make bundle` e um `rauc install` do operador, ~15 min. O slot com o
weston fica intacto do começo ao fim e é a volta: `rauc status mark-active other && reboot`.

### Fase 0 — o controle positivo, antes de qualquer código

**Objetivo**: ver o mecanismo funcionando onde ele funciona.
**Como**: no slot do weston, a lista de globais que a HMI recebe (`WAYLAND_DEBUG=1`), e a
contagem de `wl_drm`.
**Critério**: `wl_viv` está na lista.
**O que uma reprovação significa**: a leitura da §1 está errada, e o plano para aqui - a causa da
queda é outra e tem de ser procurada com um *backtrace*, não com um porte.

### Fase 1 — P1

**Critérios**, na placa: `wl_viv` na lista de globais do labwc; **um** `wl_drm`; a HMI **não cai
mais em `create_surface`**. Ela ainda não desenha - o esperado é uma janela vazia, uma recusa do
commit, ou uma queda em outro lugar, e qualquer das três é progresso mensurável.
**Injeção**: o mesmo binário com o *bind* desligado por variável de ambiente tem de voltar ao
SIGSEGV em `create_surface`. Sem isso, "parou de cair" não distingue o *patch* de um acaso.
**O que uma reprovação significa**: se o *bind* falha ou não registra o global, este driver exige
algo mais do compositor do que a chamada; para-se e lê-se como o weston inicializa o EGL.

### Fase 2 — P2 e P3

**Critérios**: a HMI aparece no monitor, dentro de uma janela do labwc; `NRestarts=0` aos 5 min;
os traços na orientação certa; **CPU da HMI ≤ 25% de um núcleo** (no weston: ≈ 17%).
**Injeção**: P3 desligado → a janela existe e fica vazia, com "Failed to create texture" no
diário; nada cai.
**Riscos que só a placa responde**:
- **imagem de cabeça para baixo.** A 0.17 não tem mais onde guardar `inverted_y`. Se o driver
  responder invertido, é mais um campo na textura e uma troca de coordenadas no *shader* - um
  quarto *patch*, pequeno;
- **formato que o driver devolve** e se a textura externa amostra certo;
- **vida do buffer**: o cliente destrói o `wl_buffer` com a imagem ainda em uso. A 0.14 resolvia
  mantendo a `EGLImage`; é o que se repete, e é o que o teste de fechar a janela mede.

### Fase 3 — o ambiente inteiro, e os critérios da D2

**Critérios** (os de `implementation_plan_desktop.md` §4, Fase D2): imagem em um boot, com e sem
monitor no boot; modo CEA programado; `NRestarts=0` aos 5 min; a barra visível; `Files` abre;
duplo clique abre o BDF+; minimizar e restaurar a HMI pela barra; fechar o revisor com um menu
aberto não derruba o compositor.
**Injeção**: matar o compositor → as unidades clientes voltam com ele.

### Fase 4 — regressão e registro

`make qemu && make check` com os três *patches*: **35/35**. No QEMU não há GPU e o caminho novo
não é exercitado; o que isto prova é só que os *patches* não quebram o caminho por software.
Registro em `implementation_plan_desktop.md` §6 e mensagens de commit.

## 5. O que este plano não cobre

- **O QEMU não testa o porte.** Não há como: o caminho só existe com este driver. Toda evidência
  de P2 e P3 vem da placa, uma rodada por vez.
- **Outras placas e outros drivers.** Os *patches* são genéricos na forma (`EGL_WL_bind_wayland_display`
  é extensão pública) e validados em um driver só.
- **Envio ao wlroots.** O projeto removeu este caminho de propósito; os *patches* ficam nossos, a
  cada versão, como os do gerenciador de arquivos. É dívida declarada.
- **A confirmação do slot.** Uma imagem com a tela morta é confirmada como boa porque só a
  aquisição pertence a `boot-complete.target` (§6.3). Fica fora deste plano e continua aberto; é
  o que faria uma rodada ruim voltar sozinha.

## 6. Quando parar

O plano termina sem sucesso, e a estação fica no weston com o ambiente no QEMU, se:

- a Fase 0 reprovar;
- o *bind* não registrar o global (Fase 1);
- depois de **quatro rodadas** na Fase 2 a HMI não tiver desenhado um quadro correto.

Nos três casos o que foi medido entra na §6 do plano do ambiente como o obstáculo, com o mesmo
peso de um resultado.

## 7. Validação

*(Vazio de propósito.)*

| Fase | Critério | Resultado | Data |
|---|---|---|---|
| 0 | `wl_viv` anunciado pelo weston | **cumprido** — no slot do weston a HMI recebe `wl_viv`, `wl_drm` e `zwp_linux_dmabuf_v1`, duas vezes cada (dois registros no mesmo processo; se é um global ou dois de cada, isto não distingue) | 2026-10-12 |
| 1 | global presente, um `wl_drm`, sem queda em `create_surface` | **cumprido, sem a injeção** — `wl_viv` anunciado; `wl_drm` na mesma contagem de `wl_viv` (o do wlroots não foi criado); a HMI deixou de morrer por sinal e passou a ser desconectada no primeiro commit: `wl_buffer@40: error 0: unknown buffer type`, saída 255. A linha "bound" não aparece no diário porque é de nível INFO. **A injeção (bind desligado → SIGSEGV de volta) não foi executada** | 2026-10-12 |
| 2 | HMI desenhada, `NRestarts=0`, CPU ≤ 25% | **cumprido na primeira rodada, sem as injeções** — a HMI desenha dentro do labwc pela GPU: `NRestarts=0` no compositor e na HMI, **17% de um núcleo** em 20 s, orientação e cores certas, 249,7 amostras/s (captura `grim`, `build/med-bench/20261012-desktop-files/05-placa-labwc.png`). Aberto: o cliente registra `__buffer_handle_release: ERROR: invalid state=1` a cada devolução de buffer (45 em 45), com uma devolução por ciclo e na ordem do protocolo; não se sabe se acontece sob o weston. As duas injeções não foram executadas | 2026-10-12 |
| 3 | critérios da D2 | **parcial** — visto pelo operador no monitor, com o *patch* da barra: a barra aparece; `Files` abre e lista os BDF+; clicar em `MedPlatform EEG` minimiza a HMI (fundo preto) e clicar de novo a traz de volta desenhando. Medido depois do *patch* da barra: **`sfwbar` 0 ticks em 5 s** (antes: um núcleo inteiro), **HMI 18% de um núcleo** em 20 s, `NRestarts=0` no compositor e na HMI; captura `07-placa-com-barra.png` com a barra (`Files`, `MedPlatform EEG`), a HMI terminando acima dela e o sinal de teste do conversor em oito canais. **Não medido ainda**: duplo clique abrindo o revisor, moldura e botões do `Files`, boot sem monitor, `NRestarts` aos 5 min, compositor morto, as duas injeções do porte | 2026-10-12 |
| 4 | `make check` 35/35 | **cumprido** — `make qemu && make check` com os três *patches* do wlroots, o da barra e o `wlr-randr`: **35/35**. O QEMU não tem GPU e não exercita o caminho novo; isto mostra que o caminho por software não quebrou | 2026-10-12 |

### 7.1 O que a Fase 3 encontrou antes de começar: a barra em laço (2026-10-12)

Com a HMI desenhando, a barra não aparecia: a HMI ocupava as 1080 linhas (`grim`) e o processo
`sfwbar` estava vivo e calado. Duas hipóteses minhas caíram antes da causa aparecer - a ordem
entre a barra e a troca de modo (reiniciada com o modo já programado, nada mudou; a mudança na
receita foi desfeita) e uma parada à espera de algo (o processo estava a 98% de um núcleo).

Como foi achado: o rastro do protocolo, comparado com o do QEMU, parava no evento de estado da
janela da HMI; a thread principal, parada por 20 ms e lida em `/proc/<pid>/task/<pid>/syscall`
quarenta vezes, estava sempre em `trigger.c:86-92` ou em `gmain.c:6533`.

A causa: `trigger_emit_in_main_context()` era `void` e passada como `GSourceFunc` a
`g_main_context_invoke()`, que executa `while (function (data));`. O laço termina conforme o que
sobrar no registrador de retorno - zero num build x86-64, o próprio argumento num aarch64. É um
defeito do `sfwbar` v1.0_beta16, já reescrito no ramo principal, e **o QEMU não podia mostrá-lo**:
a mesma configuração, a mesma janela e o mesmo evento passam lá.

Corrigido por um *patch* de três linhas na receita (`0001-trigger-return-from-the-main-context-callback.patch`),
compilado no perfil QEMU e **ainda não rodado na placa**.

Consequência para o que já foi medido: desde a primeira imagem do ambiente na placa, a barra
consumia um núcleo inteiro. Os 17% da HMI foram medidos com esse núcleo ocupado.
