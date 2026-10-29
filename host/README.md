# A estação EEG num PC Ubuntu

O perfil EEG roda como um **container** no Ubuntu, em vez de como sistema operacional numa placa. O
userspace é o da imagem Yocto, sem recompilar nada para o Ubuntu: o MedFramework, o serviço de
aquisição, a configuração selada, a HMI e as units com seus sandboxes são os mesmos pacotes que
`med-image-eeg` instala. O Ubuntu entra com o que numa placa vem do BSP: o kernel, os drivers do
front-end, a regra udev que dá nome ao conversor e a tela.

```
Ubuntu                                   container med-eeg (med-image-eeg-host)
──────                                   ─────────────────────────────────────
kernel + drivers (DKMS)             ──▶  /dev/iio:deviceN e o sysfs dele
udev (70-med-afe-ads1299.rules)     ──▶  /run/med-afe/{node,eeg0}
Wayland do GNOME (wayland-0)        ──▶  /run/host-display/wayland-0  → eeg-hmi-gui
/var/lib/medplatform/data           ──▶  /data
ícone no menu                       ──▶  /run/eeg-hmi-request/open    → eeg-hmi.service
```

## Em outro PC: dois pacotes

O outro PC não compila nada além dos drivers, e isso o DKMS faz sozinho:

```sh
sudo apt install ./medplatform-afe-dkms_*.deb ./medplatform-station_*.deb
```

- **`medplatform-afe-dkms`** compila os dois drivers para o kernel do PC (e de novo a cada
  atualização de kernel) e instala a regra udev e as opções do módulo da camada adjunta. Com Secure
  Boot e sem chave MOK na máquina, o DKMS cria uma e pede uma **senha temporária** durante o
  `apt install`. No próximo boot aparece uma tela azul (MokManager): escolha *Enroll MOK* →
  *Continue* → *Yes* e digite a senha. Isso é feito uma vez por PC.
- **`medplatform-station`** instala o container, a unit, o ícone e o serviço que atende o ícone, e
  coloca quem rodou o `sudo apt install` no grupo `medplatform`. Para outros usuários:
  `sudo usermod -aG medplatform <usuário>` (vale na hora, sem sair da sessão).

Depois, **MedPlatform EEG** no menu de aplicativos, **sem senha**. A primeira abertura sobe o
container e abre a HMI; o **Close** da HMI fecha a tela e desliga a estação (o conversor para de
adquirir), e fica desabilitado enquanto há uma gravação. Botão direito no ícone → *Stop the station*
também desliga. Pelo terminal: `medplatform-station`, `medplatform-station stop`,
`journalctl -u medplatform-station@$USER`, `journalctl -u medplatform-requests` e
`sudo machinectl shell med-eeg`.

O ícone não pede nada ao systemd diretamente: ele deixa um arquivo `open.<usuário>` em
`/run/medplatform-requests`, e um serviço do próprio Ubuntu (`medplatform-requests.path`) atende
como root, depois de conferir que o dono do arquivo é o usuário nomeado e está no grupo
`medplatform`. Uma regra polkit foi a primeira tentativa e não funcionou: no Ubuntu 24.04 ela
autorizava o `pkcheck` e nunca o `systemctl start` vindo do menu, que pedia senha duas vezes.

O PC precisa ser x86-64 e estar numa **sessão Wayland** (o padrão do Ubuntu 22.04+ com Intel/AMD; com
NVIDIA o login às vezes cai em Xorg, e aí a HMI não tem onde desenhar).

## Gerar os pacotes (na máquina com o build Yocto)

```sh
make host                    # o container, conversor pela ponte USB (padrão)
make host LINK=simulated     # ou sem front-end: o gerador sintético do framework
make host-deb                # os dois .deb, em build/debs/
```

A versão de cada pacote diz de onde ele veio: o commit da plataforma para o container (com `.dirty`
se havia mudança não commitada) e o commit do `linux-med` para os drivers.

## O que muda em relação a uma placa

- **`/data` não é cifrado.** É um diretório do Ubuntu, e o perfil declara
  `storage.require_encryption = false`. Cada sessão registra isso em seus metadados.
- **Não há A/B nem RAUC.** O pacote `rauc` está na imagem porque o framework depende dele, mas as
  units estão mascaradas. Atualizar é instalar um `.deb` novo; os registros em
  `/var/lib/medplatform/data` ficam, mesmo com `apt purge`.
- **O conversor pode ser conectado com a estação aberta.** Uma regra udev do pacote
  (`71-medplatform-station.rules`) percebe a ponte e empresta o dispositivo ao container em execução
  (`machinectl bind`); o serviço de aquisição, que tenta de novo a cada poucos segundos, o encontra
  sozinho. Desconectar e conectar de novo também funciona.
- **Os drivers são da branch `board/medplatform-afe`** do `linux-med`, cuja ABI é a que a receita do
  serviço escreve (`lead_off`, `temperature_channel`, `input_mux`...). A série para o kernel oficial
  (o DKMS `ads1299-linux` e o HEAD de `feature/ads1299-mcp2210-driver`) usa outra ABI, e com ela o
  serviço recusa iniciar (`the front-end has no attribute ...`). O `medplatform-afe-dkms` se recusa
  a instalar se outra cópia de `ti-ads1299` ou `hid-mcp2210` estiver no sistema, e diz qual remover.
- **O `/sys` do container é gravável quando há conversor.** O nspawn monta o próprio sysfs somente
  leitura por cima de qualquer bind do diretório do conversor, então o container recebe o `/sys`
  inteiro gravável (`SYSTEMD_NSPAWN_API_VFS_WRITABLE`). Quem restringe é o sandbox do próprio
  serviço, como na placa: `ProtectKernelTunables` deixa o `/sys` somente leitura e `ReadWritePaths`
  reabre só `/run/med-afe/eeg0`. Os outros processos root do container não têm essa restrição.
- **A HMI desenha por software** (o container não recebe a GPU) e ocupa cerca de um núcleo.
- **Só a HMI usa a tela.** O revisor (`med-review`) roda como outro usuário e não alcança o socket
  Wayland do usuário do desktop. No Ubuntu, abra o BDF+ de `/var/lib/medplatform/data` com o
  EDFbrowser do `apt`.
- **Uma estação por PC.** O container se chama `med-eeg`, e dois usuários não abrem cada um o seu ao
  mesmo tempo.
