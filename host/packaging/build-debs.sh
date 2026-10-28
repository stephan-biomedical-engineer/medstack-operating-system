#!/bin/bash
# SPDX-License-Identifier: MIT
#
# Builds the two packages another PC installs instead of a Yocto build:
#
#   medplatform-station   the container (med-image-eeg-host), its unit, the
#                         menu icon and the polkit rule - amd64
#   medplatform-afe-dkms  the front-end's two drivers as DKMS source, plus the
#                         udev rule and module options of the adjunct layer - all
#
#   host/packaging/build-debs.sh [ROOTFS] [AFE_REF]
#
# ROOTFS defaults to the last "make host"; AFE_REF to board/medplatform-afe,
# the driver whose sysfs ABI the service's per-link translation writes (the
# upstream series speaks another one - host/README.md). Output: build/debs/.
# Nothing here needs root, the network or kas.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
host=$(cd "$here/.." && pwd)
repo=$(cd "$host/.." && pwd)
linux=$repo/linux-med
afe_layer=$repo/meta-custom/meta-med-afe-ads1299/recipes-core
out=$repo/build/debs

rootfs=${1:-$(ls -1 "$repo"/build/tmp*/deploy/images/qemux86-64/med-image-eeg-host-qemux86-64.rootfs.tar.zst 2>/dev/null | head -n 1 || true)}
afe_ref=${2:-board/medplatform-afe}

[ -n "$rootfs" ] && [ -f "$rootfs" ] || { echo "no container archive - build it with: make host" >&2; exit 1; }
git -C "$linux" rev-parse --verify -q "$afe_ref^{commit}" >/dev/null \
    || { echo "linux-med has no ref '$afe_ref'" >&2; exit 1; }

stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
mkdir -p "$out"

# Versions say where each package came from: the platform commit (and whether
# the tree had uncommitted changes) for the container, the driver commit for
# the modules. A package nobody can trace back is not one to hand out.
date=$(date +%Y%m%d%H%M)
platform_rev=$(git -C "$repo" rev-parse --short HEAD)
git -C "$repo" diff --quiet HEAD -- meta-custom kas host || platform_rev="$platform_rev.dirty"
afe_rev=$(git -C "$linux" rev-parse --short=12 "$afe_ref^{commit}")
station_version="1.0.0~$date.$platform_rev"
afe_version="1.0.0+$afe_rev"

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
install -D -m 0644 "$host/medplatform-station-open@.service" "$pkg/usr/lib/systemd/system/medplatform-station-open@.service"
install -D -m 0644 "$host/medplatform-station.desktop" "$pkg/usr/share/applications/medplatform-station.desktop"
install -D -m 0644 "$host/50-medplatform-station.rules" "$pkg/usr/share/polkit-1/rules.d/50-medplatform-station.rules"
install -D -m 0644 "$host/README.md" "$pkg/usr/share/doc/medplatform-station/README.md"
control_scripts "$here/station" "$pkg/DEBIAN" "$station_version"
cat > "$pkg/DEBIAN/control" <<EOF
Package: medplatform-station
Version: $station_version
Architecture: amd64
Maintainer: MedPlatform <stephanbrrs8@gmail.com>
Depends: systemd-container, zstd, tar, polkitd | policykit-1
Recommends: medplatform-afe-dkms, libnotify-bin
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
install -d "$pkg/DEBIAN" "$src"
git -C "$linux" show "$afe_ref:drivers/iio/adc/ti-ads1299.c" > "$src/ti-ads1299.c"
git -C "$linux" show "$afe_ref:drivers/hid/hid-mcp2210.c" > "$src/hid-mcp2210.c"
git -C "$linux" show "$afe_ref:drivers/hid/hid-ids.h" > "$src/hid-ids.h"
chmod 0644 "$src"/*.c "$src"/*.h
install -m 0644 "$here/afe-dkms/Kbuild" "$src/Kbuild"
sed -e "s/@VERSION@/$afe_version/g" -e "s|@REF@|$afe_ref ($afe_rev)|g" \
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
 kernel: an IIO driver for the 8-channel biopotential converter and a HID
 driver that registers the USB-to-SPI bridge carrying it as an SPI
 controller. From linux-med $afe_ref ($afe_rev), the sysfs ABI the
 medplatform-station acquisition service writes. Includes the udev rule that
 names the converter and the bridge's module options.
 .
 With Secure Boot, DKMS signs the modules with this machine's MOK, creating
 and enrolling one at the next boot if there is none.
EOF
afe_deb=$out/medplatform-afe-dkms_${afe_version}_all.deb
dpkg-deb --root-owner-group -Zxz --build "$pkg" "$afe_deb" >/dev/null

echo "$station_deb  (link: $link)"
echo "$afe_deb  ($afe_ref)"
