#!/bin/sh
# Impressão digital do conteúdo de uma árvore de kernel, para comparar a árvore
# que o Yocto consome (tarball + patch da ST) com um checkout git do fork.
#
# Compara CONTEÚDO e não metadados: modo de arquivo, mtime e diretórios vazios
# diferem legitimamente entre um tarball desempacotado e um checkout git, e
# nenhum deles muda o kernel compilado. Um manifesto ordenado de
# (sha256, caminho relativo) e um hash sobre ele é o que pode bater.
#
# Uso:
#   scripts/med-kernel-fingerprint.sh <dir> [arquivo-de-manifesto]
#
# O manifesto é o artefato útil: quando o hash final não bate, o `diff` entre
# dois manifestos nomeia exatamente quais arquivos divergem - que é a diferença
# entre "as árvores diferem" e "as árvores diferem nestes 3 arquivos".
set -eu

tree=${1:?uso: $0 <dir> [manifesto]}
manifest=${2:-}

[ -d "$tree" ] || { echo "não é diretório: $tree" >&2; exit 1; }

tmp=$(mktemp)
trap 'rm -f "$tmp"' EXIT

cd "$tree"
# Exclusões, e cada uma tem motivo. Sem elas esta ferramenta NÃO PODE passar:
# a árvore que o Yocto desempacota carrega 768 arquivos que um checkout git
# jamais terá, e a comparação acusaria divergência onde não há nenhuma.
#
#   .git/                  o repositório em si não é conteúdo do kernel
#   .pc/                   backups pré-patch do quilt, que é como o Yocto aplica
#                          o patch da ST - uma cópia de cada arquivo tocado
#   patches/               o diretório de trabalho do quilt na raiz: o symlink
#                          para o patch da ST e o arquivo 'series'. Só a raiz,
#                          nunca um 'patches/' aninhado, que poderia ser
#                          conteúdo de verdade
#   .scmversion            escrito pela classe kernel do Yocto
#   .checkpatch-camelcase* cache que o checkpatch escreve ao rodar na árvore
#   .config, Module.symvers, include/generated, include/config
#                          artefatos de build; ausentes numa árvore work-shared,
#                          presentes se alguém apontar isto para uma árvore
#                          construída, e aí a exclusão salva a comparação
find . -type f \
	-not -path './.git/*' \
	-not -path './.pc/*' \
	-not -path './patches/*' \
	-not -path './include/generated/*' \
	-not -path './include/config/*' \
	-not -name '.scmversion' \
	-not -name '.checkpatch-camelcase*' \
	-not -name '.config' \
	-not -name '.config.old' \
	-not -name 'Module.symvers' \
	-print0 \
  | LC_ALL=C sort -z \
  | xargs -0 -P "$(nproc)" -n 256 sha256sum \
  | LC_ALL=C sort -k2 > "$tmp"

files=$(wc -l < "$tmp")
digest=$(LC_ALL=C sha256sum < "$tmp" | cut -d' ' -f1)

if [ -n "$manifest" ]; then
	cp "$tmp" "$manifest"
	echo "manifesto: $manifest"
fi

echo "arquivos:  $files"
echo "digest:    $digest"
