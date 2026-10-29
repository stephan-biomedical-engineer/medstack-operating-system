#!/bin/bash
# SPDX-License-Identifier: MIT
#
# Builds the two packages another PC installs instead of a Yocto build:
#
#   medplatform-station   the container (med-image-eeg-host), its unit, the
#                         menu icon and the request service behind it - amd64
#   medplatform-afe-dkms  the front-end's two drivers as DKMS source, plus the
#                         udev rule and module options of the adjunct layer - all
#
#   host/packaging/build-debs.sh [ROOTFS]
#
# ROOTFS defaults to the last "make host". The drivers come from two places,
# each a git ref (committed state only, never a working tree):
#
#   ADS1299_REPO  the converter driver's out-of-tree repository, the one its
#                 upstream submission is made from (default ../ads1299-linux)
#   ADS1299_REF   default main - its sysfs ABI is the one the service's
#                 per-link translation writes
#   MCP2210_REF   the bridge driver in linux-med, default board/medplatform-afe
#
# Output: build/debs/. Nothing here needs root, the network or kas.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
host=$(cd "$here/.." && pwd)
repo=$(cd "$host/.." && pwd)
linux=$repo/linux-med
ads1299_repo=${ADS1299_REPO:-$(cd "$repo/.." && pwd)/ads1299-linux}
afe_layer=$repo/meta-custom/meta-med-afe-ads1299/recipes-core
out=$repo/build/debs

rootfs=${1:-$(ls -1 "$repo"/build/tmp*/deploy/images/qemux86-64/med-image-eeg-host-qemux86-64.rootfs.tar.zst 2>/dev/null | head -n 1 || true)}
ads1299_ref=${ADS1299_REF:-main}
mcp2210_ref=${MCP2210_REF:-board/medplatform-afe}

[ -n "$rootfs" ] && [ -f "$rootfs" ] || { echo "no container archive - build it with: make host" >&2; exit 1; }
git -C "$ads1299_repo" rev-parse --verify -q "$ads1299_ref^{commit}" >/dev/null \
    || { echo "$ads1299_repo has no ref '$ads1299_ref' (ADS1299_REPO, ADS1299_REF)" >&2; exit 1; }
git -C "$linux" rev-parse --verify -q "$mcp2210_ref^{commit}" >/dev/null \
    || { echo "linux-med has no ref '$mcp2210_ref' (MCP2210_REF)" >&2; exit 1; }

stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
mkdir -p "$out"

# Versions say where each package came from: the platform commit (and whether
# the tree had uncommitted changes) for the container, the driver commit for
# the modules. A package nobody can trace back is not one to hand out.
date=$(date +%Y%m%d%H%M)
platform_rev=$(git -C "$repo" rev-parse --short HEAD)
git -C "$repo" diff --quiet HEAD -- meta-custom kas host || platform_rev="$platform_rev.dirty"
ads1299_rev=$(git -C "$ads1299_repo" rev-parse --short=12 "$ads1299_ref^{commit}")
mcp2210_rev=$(git -C "$linux" rev-parse --short=12 "$mcp2210_ref^{commit}")
station_version="1.0.0~$date.$platform_rev"
afe_version="1.0.0+$ads1299_rev.$mcp2210_rev"

link=$(tar --zstd -xOf "$rootfs" ./etc/medplatform/eeg.conf | sed -n 's/^acquisition.link *= *//p')
[ -n "$link" ] || { echo "$rootfs carries no acquisition.link - is it med-image-eeg-host?" >&2; exit 1; }

control_scripts() {  # $1 source dir, $2 DEBIAN dir, $3 version
    for script in preinst postinst prerm postrm; do
        [ -f "$1/$script" ] || continue
        sed "s/@VERSION@/$3/g" "$1/$script" > "$2/$script"
        chmod 0755 "$2/$script"
    done
}

# --- medplatform-station ---------------------------------------------------
pkg=$stage/station
install -d "$pkg/DEBIAN"
install -D -m 0644 "$rootfs" "$pkg/usr/lib/medplatform-station/rootfs.tar.zst"
install -D -m 0755 "$host/medplatform-station-run" "$pkg/usr/lib/medplatform-station/medplatform-station-run"
install -D -m 0755 "$host/medplatform-station" "$pkg/usr/bin/medplatform-station"
install -D -m 0644 "$host/medplatform-station@.service" "$pkg/usr/lib/systemd/system/medplatform-station@.service"
install -D -m 0755 "$host/medplatform-station-request" "$pkg/usr/lib/medplatform-station/medplatform-station-request"
install -D -m 0755 "$host/medplatform-station-afe" "$pkg/usr/lib/medplatform-station/medplatform-station-afe"
install -D -m 0644 "$host/medplatform-station-afe@.service" "$pkg/usr/lib/systemd/system/medplatform-station-afe@.service"
install -D -m 0644 "$host/71-medplatform-station.rules" "$pkg/usr/lib/udev/rules.d/71-medplatform-station.rules"
install -D -m 0644 "$host/medplatform-requests.path" "$pkg/usr/lib/systemd/system/medplatform-requests.path"
install -D -m 0644 "$host/medplatform-requests.service" "$pkg/usr/lib/systemd/system/medplatform-requests.service"
install -D -m 0644 "$host/medplatform-station.desktop" "$pkg/usr/share/applications/medplatform-station.desktop"
install -D -m 0644 "$host/README.md" "$pkg/usr/share/doc/medplatform-station/README.md"
control_scripts "$here/station" "$pkg/DEBIAN" "$station_version"
cat > "$pkg/DEBIAN/control" <<EOF
Package: medplatform-station
Version: $station_version
Architecture: amd64
Maintainer: MedPlatform <stephanbrrs8@gmail.com>
Depends: systemd-container, zstd, tar
Recommends: medplatform-afe-dkms, libnotify-bin, pulseaudio-utils, sound-theme-freedesktop
Installed-Size: $(du -sk "$pkg" | cut -f1)
Section: science
Priority: optional
Description: MedPlatform EEG station (front-end link: $link)
 The EEG profile of the MedPlatform Yocto reference architecture, run as a
 systemd-nspawn container on this PC: the MedFramework, the acquisition
 service, the sealed configuration and the operator display, built by Yocto
 (platform commit $platform_rev). The PC supplies the kernel, the front-end
 drivers (medplatform-afe-dkms) and a Wayland display.
 .
 For bench and demonstration use. Records are not encrypted and there is no
 A/B update path: this is not the medical device configuration.
EOF
station_deb=$out/medplatform-station_${station_version}_amd64.deb
dpkg-deb --root-owner-group -Zxz --build "$pkg" "$station_deb" >/dev/null

# --- medplatform-afe-dkms --------------------------------------------------
pkg=$stage/afe
src=$pkg/usr/src/medplatform-afe-$afe_version
install -d "$pkg/DEBIAN" "$src/ads1299" "$src/mcp2210"
# The converter's repository as committed, whole: its Makefile, its compat
# layer and the driver - what its own README builds.
git -C "$ads1299_repo" archive "$ads1299_ref" | tar -x -C "$src/ads1299"
git -C "$linux" show "$mcp2210_ref:drivers/hid/hid-mcp2210.c" > "$src/mcp2210/hid-mcp2210.c"
git -C "$linux" show "$mcp2210_ref:drivers/hid/hid-ids.h" > "$src/mcp2210/hid-ids.h"
chmod -R u=rwX,go=rX "$src"
install -m 0644 "$here/afe-dkms/Kbuild" "$src/mcp2210/Kbuild"
sed -e "s/@VERSION@/$afe_version/g" \
    -e "s|@ADS1299@|$(basename "$ads1299_repo") $ads1299_ref, $ads1299_rev|g" \
    -e "s|@MCP2210@|$mcp2210_ref, $mcp2210_rev|g" \
    "$here/afe-dkms/dkms.conf.in" > "$src/dkms.conf"
chmod 0644 "$src/dkms.conf"
install -D -m 0644 "$afe_layer/udev/files/70-med-afe-ads1299.rules" "$pkg/usr/lib/udev/rules.d/70-med-afe-ads1299.rules"
install -D -m 0644 "$afe_layer/modprobe/files/med-afe-bridge.conf" "$pkg/usr/lib/modprobe.d/med-afe-bridge.conf"
control_scripts "$here/afe-dkms" "$pkg/DEBIAN" "$afe_version"
cat > "$pkg/DEBIAN/control" <<EOF
Package: medplatform-afe-dkms
Version: $afe_version
Architecture: all
Maintainer: MedPlatform <stephanbrrs8@gmail.com>
Depends: dkms (>= 3), linux-headers-generic | linux-headers
Installed-Size: $(du -sk "$pkg" | cut -f1)
Section: kernel
Priority: optional
Description: MedPlatform analogue front-end drivers (DKMS)
 The EEG front-end's two kernel drivers, built by DKMS for every installed
 kernel: an IIO driver for the 8-channel biopotential converter, from
 $(basename "$ads1299_repo") $ads1299_ref ($ads1299_rev), the out-of-tree
 repository of its upstream submission; and a HID driver that registers the
 USB-to-SPI bridge carrying it as an SPI controller, from linux-med
 $mcp2210_ref ($mcp2210_rev). Includes the udev rule that names the converter
 and the bridge's module options.
 .
 With Secure Boot, DKMS signs the modules with this machine's MOK, creating
 and enrolling one at the next boot if there is none.
EOF
afe_deb=$out/medplatform-afe-dkms_${afe_version}_all.deb
dpkg-deb --root-owner-group -Zxz --build "$pkg" "$afe_deb" >/dev/null

echo "$station_deb  (link: $link)"
echo "$afe_deb  (ti-ads1299: $ads1299_ref $ads1299_rev, hid-mcp2210: $mcp2210_ref $mcp2210_rev)"
