# Containerised entry points for the MedPlatform Yocto build.
#
# Why: a Yocto build depends on a long, version-sensitive list of host packages,
# and "it built on my machine" is not a defensible claim in a thesis whose
# subject is a reproducible platform. Everything below runs bitbake inside the
# upstream kas image, pinned by version, so the build host stops being an
# assumption and becomes part of the recorded configuration - the same
# configuration-management argument the repository already makes with SBOM and
# buildhistory (IEC 62304 §5.1).
#
# Quick start:
#   make tool        fetch and verify the pinned kas-container script
#   make risks       cheap parse-only checks, run these before any long build
#   make qemu        build the EEG image for QEMU (the first-build target)
#   make runqemu     boot the result with KVM, serial console
#
# Set NATIVE=1 on any target to use the host's own kas instead of the container.
# See docs/BUILD_CONTAINER.md for what the container can and cannot do here.

KAS_VERSION           ?= 5.2
KAS_CONTAINER_URL     := https://raw.githubusercontent.com/siemens/kas/$(KAS_VERSION)/kas-container
KAS_CONTAINER_SHA256  := a9f6d91862478b49f1e8db49b75e1a09247a8393a7d6653f0d333f46f4d599cf
KAS_CONTAINER         := .kas-container/kas-container-$(KAS_VERSION)

# Machine-local overrides, optional and gitignored. This is where a build host
# says that DL_DIR, SSTATE_DIR or KAS_BUILD_DIR live somewhere other than the
# repository - an external disk, a scratch volume - without that path ever
# entering version control, and without anyone having to remember an export
# before every make. Included *before* the defaults so a value set here wins
# the "?=" below; "-include" so its absence is not an error.
#
# The split worth knowing when writing one: downloads/ and sstate-cache/ are
# read sequentially and tolerate a slow disk, while the build directory is
# millions of small files and does not. Measured on a USB 2.0 external HDD
# against this machine's NVMe: sequential 25 MB/s against 971 MB/s, and 2216
# small files/s against 29052 - an order of magnitude on exactly the operation
# do_unpack and do_install spend their time in.
-include local.mk

# These must be exported, not just set: kas forwards exactly this set of
# variables into bitbake, and kas-container bind-mounts the directories they
# name. kas-base.yml declares DL_DIR/SSTATE_DIR with "?=" so the values below
# win inside the container without breaking a native build.
export DL_DIR            ?= $(CURDIR)/downloads
export SSTATE_DIR        ?= $(CURDIR)/sstate-cache
export KAS_BUILD_DIR     ?= $(CURDIR)/build
export KAS_IMAGE_VERSION ?= $(KAS_VERSION)

QEMU_CFG  := kas/project-eeg-qemu.yml
STM32_CFG := kas/project-eeg-stm32mp2.yml
TOMO_CFG  := kas/project-tomograph.yml
IMAGE     := med-image-eeg

# Where the LUKS key for /data comes from, as a parameter on the build targets
# rather than as a second set of targets:
#
#   make stm32                   the profile's own answer ("tpm2")
#   make stm32 KEY=development   a key on a partition no update writes to
#
# Checked here rather than left to the build, because a typo would otherwise
# reach the recipe, be substituted into the shipped provisioning script, and
# only fail on the board at first boot.
KEY ?=
ifneq ($(KEY),)
  ifeq ($(filter $(KEY),tpm2 development),)
    $(error KEY must be "tpm2" or "development", not "$(KEY)")
  endif
endif

# Which profile the update-path helpers act on. A parameter on the existing
# targets rather than a second set of them, for the same reason KEY is one: the
# only thing that differs is the machine, and the machine is already declared -
# read it back out of the kas file instead of writing it down twice.
#
#   make bundle                  the QEMU profile
#   make bundle BOARD=stm32      the STM32MP257F-DK profile
BOARD ?= qemu
ifeq ($(filter $(BOARD),qemu stm32),)
  $(error BOARD must be "qemu" or "stm32", not "$(BOARD)")
endif

# Which kernel the board build uses. A parameter on the existing targets, in the
# shape of KEY and BOARD, and not a second set of targets: the only difference
# is where the kernel comes from.
#
#   make stm32                   ST's release: tarball 6.6.129 + the r3.1 patch
#   make stm32 KERNEL=med        the tree in ./linux-med, at its current HEAD
#
# It only fills in the pair MED_KERNEL_GIT/MED_KERNEL_SRCREV, which are still
# honoured when given by hand (and for a tree elsewhere). The revision is read
# from the tree at the moment of the call, so it is always a commit and never a
# branch tip, which is what the recipe demands. What the build fetches is that
# commit: uncommitted work in ./linux-med does not reach the image, hence the
# warning.
KERNEL ?=
ifneq ($(KERNEL),)
  ifneq ($(KERNEL),med)
    $(error KERNEL must be "med" or unset, not "$(KERNEL)")
  endif
  ifeq ($(wildcard linux-med/.git),)
    $(error KERNEL=med needs a clone of the kernel fork at ./linux-med)
  endif
  MED_KERNEL_GIT    := git://$(CURDIR)/linux-med;protocol=file;branch=$(shell git -C linux-med branch --show-current)
  MED_KERNEL_SRCREV := $(shell git -C linux-med rev-parse HEAD)
  ifneq ($(shell git -C linux-med status --porcelain --untracked-files=no),)
    $(warning ./linux-med has uncommitted changes; the build uses commit $(MED_KERNEL_SRCREV) without them)
  endif
endif
BOARD_CFG     := $(if $(filter stm32,$(BOARD)),$(STM32_CFG),$(QEMU_CFG))
BOARD_MACHINE := $(shell awk '/^machine:/{print $$2}' $(BOARD_CFG))

# runqemu is only worth running with hardware acceleration, which needs the KVM
# character device and membership of the group that owns it on the *host* - the
# numeric gid is what the kernel checks, so it is read from the device itself.
KVM_GID := $(shell stat -c %g /dev/kvm 2>/dev/null)

# kas forwards MED_DATA_KEY_SOURCE into bitbake because kas-base.yml lists it
# under "env:", but kas-container forwards only a fixed whitelist of variables
# into the container (kas-container:731), and that list is upstream's, not ours.
# So the native build exports it and the containerised one hands it to docker
# run explicitly. Getting this wrong is silent: the build succeeds with the
# profile's default.
ifeq ($(NATIVE),1)
  KAS          := kas
  RUNTIME_ARGS :=
  TOOL         :=
  KEY_ENV      := $(if $(KEY),MED_DATA_KEY_SOURCE=$(KEY),)
  KEY_ARGS     :=
  KERNEL_ARGS  :=
  export MED_KERNEL_GIT MED_KERNEL_SRCREV
  # native: kas reads MED_BENCH from the environment directly
else
  KAS  := $(KAS_CONTAINER)
  TOOL := $(KAS_CONTAINER)
  ifeq ($(KVM_GID),)
    RUNTIME_ARGS := --runtime-args "-p 2222:2222"
  else
    RUNTIME_ARGS := --runtime-args "--device /dev/kvm --group-add $(KVM_GID) -p 2222:2222"
  endif
  KEY_ENV  :=
  KEY_ARGS := $(if $(KEY),--runtime-args "-e MED_DATA_KEY_SOURCE=$(KEY)",)
  # Same gap as KEY_ARGS closes, for the pair that says where the kernel comes
  # from, and it had been left open. kas-base.yml lists MED_KERNEL_GIT and
  # MED_KERNEL_SRCREV under "env:", which makes kas forward them into bitbake -
  # but only once they are inside the container, and the whitelist above does
  # not carry them there. So the documented invocation,
  #
  #     MED_KERNEL_GIT=... MED_KERNEL_SRCREV=... make stm32
  #
  # dropped both, the bbappend's anonymous function returned early on an empty
  # MED_KERNEL_GIT, and the build produced ST's stock kernel with neither
  # front-end driver in it. Green, and wrong, which is the failure the comment
  # above this block warns about - written for the key and not applied here.
  #
  # The path is translated as well, because the tree is bind-mounted at /work:
  # a host path in that URL points at nothing inside the container, and the
  # fetch would fail at do_fetch rather than silently, but there is no reason
  # to make a person discover that.
  # --runtime-args accumulates (kas-container:395), so this composes with the
  # key above instead of replacing it.
  KERNEL_GIT_IN_CTR := $(subst $(CURDIR),/work,$(MED_KERNEL_GIT))
  KERNEL_ARGS := $(if $(MED_KERNEL_GIT),--runtime-args "-e MED_KERNEL_GIT=$(KERNEL_GIT_IN_CTR) -e MED_KERNEL_SRCREV=$(MED_KERNEL_SRCREV)",)
  # Same gap, same fix, for the bench-only kernel symbols.
  KERNEL_ARGS += $(if $(MED_BENCH),--runtime-args "-e MED_BENCH=$(MED_BENCH)",)
endif

.PHONY: help tool pki eject checkout layers risks parse framework service qemu stm32 \
        tomograph bundle verify-bundle bundle-disk shell runqemu \
        runqemu-tomograph check test image-info clean purge

help:
	@echo "MedPlatform build targets (append NATIVE=1 to bypass the container)"
	@echo
	@echo "  tool        fetch + checksum the pinned kas-container $(KAS_VERSION)"
	@echo "  pki         generate the update-signing CA (keys stay out of git)"
	@echo "  eject       safely unmount + power off the cache disk (before flashing)"
	@echo "  checkout    clone the external layers, write build/conf, do not build"
	@echo "  layers      bitbake-layers show-layers (parse only)"
	@echo "  risks       the three known metadata risks, all parse only"
	@echo "  parse       full recipe parse + dry-run task graph for $(IMAGE) (BOARD=stm32 too)"
	@echo "  framework   build med-framework-api only (first cross compile)"
	@echo "  service     build eeg-acquisition-service only (fast inner loop)"
	@echo "  qemu        build $(IMAGE) for qemux86-64"
	@echo "  stm32       build $(IMAGE) for the STM32MP257F-DK"
	@echo "              add KERNEL=med to build the kernel fork in ./linux-med at its HEAD"
	@echo "  tomograph   build the tomograph profile (reuse validation)"
	@echo "  bundle      build the signed RAUC update bundle (needs 'make pki')"
	@echo "              add BOARD=stm32 for the STM32MP257F-DK, and the same KEY= as the image"
	@echo "  verify-bundle  verify it exactly as the device would (keyring+purpose+CRL)"
	@echo "  bundle-disk    wrap the bundle in a disk the QEMU guest can mount"
	@echo "  shell       interactive build environment"
	@echo "  runqemu     boot the GPT disk image, KVM accelerated, serial console"
	@echo "  check       boot and assert on the running system (exits non-zero on failure)"
	@echo "  test        MedFramework host tests: no kas, no image, seconds"
	@echo "  image-info  size and package count of the last build"
	@echo "  clean       drop build artefacts, keep sstate and downloads"
	@echo "  purge       drop everything kas manages"

# Pinned by version *and* by content hash: the point of containerising is that
# the toolchain is reproducible, which a moving script would undo.
$(KAS_CONTAINER):
	@mkdir -p $(dir $@)
	curl -fsSL -o $@.tmp $(KAS_CONTAINER_URL)
	@echo "$(KAS_CONTAINER_SHA256)  $@.tmp" | sha256sum -c - \
	  || { rm -f $@.tmp; echo "checksum mismatch - refusing to run"; exit 1; }
	@chmod +x $@.tmp && mv $@.tmp $@
	@echo "kas-container $(KAS_VERSION) ready at $@"

# The echo is not decoration: with only a prerequisite and no recipe, a phony
# target whose file already exists prints "Nothing to be done for 'tool'",
# which reads like a failure when it means the opposite.
tool: $(KAS_CONTAINER)
	@echo "kas-container $(KAS_VERSION) present and checksummed at $(KAS_CONTAINER)"

# Runs on the host, not in the container: the private keys must never end up
# in a layer directory that gets bind-mounted into a build, and openssl is the
# one host tool this repository does depend on.
pki:
	./scripts/med-pki.sh

# Runs on the host and touches no build state. Meant to be run immediately
# before writing an image to an SD card: the cache disk and the card appear as
# the same kind of device with neighbouring letters, and removing one of them
# does not reduce the risk of writing to the wrong one - it removes it.
eject:
	./scripts/med-eject.sh

checkout: $(TOOL)
	$(KAS) checkout $(QEMU_CFG)

layers: $(TOOL)
	$(KAS) shell $(QEMU_CFG) -c "bitbake-layers show-layers"

# Everything here is parse-only and finishes in seconds. It exists so that a
# dangling bbappend or a layer incompatibility fails now instead of three hours
# into the first build. See the risk list in CLAUDE.md.
risks: $(TOOL)
	$(KAS) shell $(QEMU_CFG) -c "bitbake-layers show-appends | grep -i -A2 rauc || true"
	$(KAS) shell $(QEMU_CFG) -c "bitbake-layers show-recipes rauc-conf"
	@grep -H LAYERSERIES_COMPAT layers/meta-qt6/conf/layer.conf

parse: $(TOOL)
	$(KAS) $(KERNEL_ARGS) shell $(BOARD_CFG) -c "bitbake -p"
	$(KAS) $(KERNEL_ARGS) shell $(BOARD_CFG) -c "bitbake -n $(IMAGE)"
	$(KAS) $(KERNEL_ARGS) shell $(BOARD_CFG) -c \
	  "bitbake -e virtual/kernel | grep -E '^(PV|MED_AFE_KCONFIG|KERNEL_MODULE_AUTOLOAD)=' || true"

framework: $(TOOL)
	$(KAS) shell $(QEMU_CFG) -c "bitbake med-framework-api"

service: $(TOOL)
	$(KAS) shell $(QEMU_CFG) -c "bitbake eeg-acquisition-service"

qemu: $(TOOL)
	$(KEY_ENV) $(KAS) $(KEY_ARGS) $(KERNEL_ARGS) build $(QEMU_CFG)

stm32: $(TOOL)
	$(KEY_ENV) $(KAS) $(KEY_ARGS) $(KERNEL_ARGS) build $(STM32_CFG)

tomograph: $(TOOL)
	$(KEY_ENV) $(KAS) $(KEY_ARGS) $(KERNEL_ARGS) build $(TOMO_CFG)

# Cheapest test of the update path: no image boot, no bootloader, no slots -
# just the signature, the keyring, the codeSigning purpose and the CRL.
#
# KEY is forwarded here for the same reason it is on the image targets, and
# getting it wrong is just as silent: the bundle payload *is* a rootfs, so a
# bundle built with the profile's default key source installs a slot whose
# med-data-provision.sh looks for a key that is not on this board. It would
# build, sign, verify and install, and fail on the next boot.
#
# KERNEL too, for the same reason: the rootfs carries /lib/modules, and the
# kernel that loads them lives on med-boot, which no bundle replaces. A bundle
# built without KERNEL=med for a board flashed with it ships ST's modules for a
# kernel that is not running - every front-end module refused at load, after a
# signed, verified, successful install.
bundle: $(TOOL)
	$(KEY_ENV) $(KAS) $(KEY_ARGS) $(KERNEL_ARGS) shell $(BOARD_CFG) -c "bitbake med-bundle-eeg"

# Verifies with the *device's* keyring settings, which is the whole point: with
# rauc's defaults this same bundle fails with "unsuitable certificate purpose",
# because OpenSSL's CMS code falls back to the smime_sign purpose and rejects a
# codeSigning certificate. Verifying without these two flags would be a test of
# something the device does not do.
# Not the copy under the bundle recipe's WORKDIR, which is where this used to
# point: local.conf inherits rm_work, so ${WORKDIR} - recipe-sysroot-native
# included - is deleted the moment the recipe finishes. That made the path
# valid only in the window before rm_work ran, and "make verify-bundle" has
# been unable to find it since. The sysroots-components copy is what sstate
# installs and what rm_work does not touch; it is also build-arch rather than
# machine specific, so it needs no BUNDLE_MACHINE.
RAUC_NATIVE := $(firstword $(wildcard $(KAS_BUILD_DIR)/tmp-glibc/sysroots-components/*/rauc-native/usr/bin/rauc))
KEYRING     := meta-custom/meta-med-distro/recipes-core/rauc/rauc-conf/med-keyring.pem
BUNDLE      := $(KAS_BUILD_DIR)/tmp-glibc/deploy/images/$(BOARD_MACHINE)/med-bundle-eeg-$(BOARD_MACHINE).raucb

# RAUC refuses a block device ("Bundle is not a regular file"), so the bundle
# cannot simply be attached raw - it has to arrive as a file in a filesystem.
# mkfs.ext4 -d populates the image without needing root or a loop mount.
# Attach the result and install from it inside the guest:
#   runqemu ... qemuparams="-drive file=$(BUNDLE_DISK),if=virtio,format=raw,readonly=on"
#   mount -o ro /dev/vdb /mnt && rauc install /mnt/update.raucb
BUNDLE_DISK := $(KAS_BUILD_DIR)/bundle-disk.ext4

bundle-disk:
	@test -f $(BUNDLE) || { echo "no bundle - run 'make bundle' first"; exit 1; }
	@rm -rf $(KAS_BUILD_DIR)/bundle-stage && mkdir -p $(KAS_BUILD_DIR)/bundle-stage
	cp $(BUNDLE) $(KAS_BUILD_DIR)/bundle-stage/update.raucb
	rm -f $(BUNDLE_DISK) && truncate -s 200M $(BUNDLE_DISK)
	mkfs.ext4 -F -q -d $(KAS_BUILD_DIR)/bundle-stage $(BUNDLE_DISK)
	@rm -rf $(KAS_BUILD_DIR)/bundle-stage
	@echo "bundle disk ready at $(BUNDLE_DISK)"

verify-bundle:
	@test -x $(RAUC_NATIVE) || { echo "no rauc-native - run 'make bundle' first"; exit 1; }
	$(RAUC_NATIVE) info --keyring=$(KEYRING) \
	  -C keyring:check-purpose=codesign \
	  -C keyring:check-crl=true \
	  $(BUNDLE)

shell: $(TOOL)
	$(KAS) shell $(QEMU_CFG)

# nographic + slirp: no X11 reaches the container (see docs/BUILD_CONTAINER.md)
# and slirp keeps qemu's networking in userspace, which avoids needing a tap
# device and CAP_NET_ADMIN. Port 2222 is forwarded out for ssh.
# Boots the GPT disk, not the bare ext4. That is the difference between a
# rootfs on /dev/vda (no partition table, so RAUC finds no booted slot and its
# service dies) and one on /dev/vda2 labelled med-root-a. There is deliberately
# only one boot path: QB_KERNEL_ROOT is a single value per image, so serving
# both would need contradictory kernel roots - the same class of silent
# divergence that put "uboot" in system.conf next to an EFI/GRUB wks file.
# The .wic is named explicitly rather than passing "wic" as an fstype. runqemu
# resolves an fstype by globbing IMAGE_NAME first and the IMAGE_LINK_NAME
# symlink only as a fallback (scripts/runqemu:699), and IMAGE_NAME comes from
# the .qemuboot.conf - which do_write_qemuboot_conf does not rewrite when a
# build changes only WKS_FILE or the .wks, because neither is one of its
# vardeps. The result is that runqemu silently boots a stale disk image. Naming
# the symlink skips the glob entirely.
runqemu: $(TOOL)
	$(KAS) $(RUNTIME_ARGS) shell $(QEMU_CFG) \
	  -c 'runqemu qemux86-64 $$(echo $$BUILDDIR/tmp*/deploy/images/qemux86-64/$(IMAGE)-qemux86-64.rootfs.wic) nographic slirp'

# The runtime acceptance suite. Everything this repository claims about its
# behaviour at runtime was verified by hand until this existed - and five of the
# six defects found while implementing the A/B update path failed no build at
# all, so building is not the place those regressions will be caught.
check: $(TOOL)
	python3 scripts/med-check.py eeg

# The host-side functional tests. No kas, no container, no image: they compile
# the sources under test for the host and run in seconds, which is what makes
# them the inner loop `check` cannot be.
#
# Two suites, and the second one is not more of the first. `tests/framework`
# compiles the MedFramework, which is our C++ and runs in userspace.
# `tests/mcp2210` compiles a KERNEL driver for the host, against a shim that
# supplies the kernel APIs it calls and a fake MCP2210 that answers 64-byte
# reports - so the bridge's transfer state machine, its stall bound and its
# reply handling are exercised with no board, no USB and no cross toolchain.
# It is the only thing in this repository that can see a defect in kernel code
# before the hardware exists.
#
# All three suites are complementary and none substitutes for another. `test`
# sees a wrong LSB, a path that escapes its namespace, an option silently
# dropped and a driver that trusts a byte count it should not; `check` sees a
# service that is active while losing 62% of its samples. Nothing a host test
# can do would have caught that one.
test:
	@$(MAKE) --no-print-directory -C tests/framework check
	@$(MAKE) --no-print-directory -C tests/mcp2210 check
	@$(MAKE) --no-print-directory -C tests/m33-producer check

# Boots the tomograph and runs only the platform assertions - the ones that
# profile inherits without writing a line. It is what turns "the platform
# stands on its own with zero applications installed" from an inference about
# package lists into an observation about a running system.
runqemu-tomograph: $(TOOL)
	$(KAS) $(RUNTIME_ARGS) shell $(TOMO_CFG) \
	  -c 'runqemu qemux86-64 $$(echo $$BUILDDIR/tmp*/deploy/images/qemux86-64/med-image-tomograph-qemux86-64.rootfs.wic) nographic slirp'

image-info:
	@cat $(KAS_BUILD_DIR)/buildhistory/images/qemux86_64/glibc/$(IMAGE)/image-info.txt 2>/dev/null \
	  || echo "no buildhistory yet - run 'make qemu' first"
	@ls -lh $(KAS_BUILD_DIR)/tmp/deploy/images/qemux86-64/$(IMAGE)-qemux86-64.rootfs.ext4 2>/dev/null || true

clean: $(TOOL)
	$(KAS) clean $(QEMU_CFG)

purge: $(TOOL)
	$(KAS) purge $(QEMU_CFG)
