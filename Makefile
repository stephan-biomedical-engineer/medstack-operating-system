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

# runqemu is only worth running with hardware acceleration, which needs the KVM
# character device and membership of the group that owns it on the *host* - the
# numeric gid is what the kernel checks, so it is read from the device itself.
KVM_GID := $(shell stat -c %g /dev/kvm 2>/dev/null)

ifeq ($(NATIVE),1)
  KAS          := kas
  RUNTIME_ARGS :=
  TOOL         :=
else
  KAS  := $(KAS_CONTAINER)
  TOOL := $(KAS_CONTAINER)
  ifeq ($(KVM_GID),)
    RUNTIME_ARGS := --runtime-args "-p 2222:2222"
  else
    RUNTIME_ARGS := --runtime-args "--device /dev/kvm --group-add $(KVM_GID) -p 2222:2222"
  endif
endif

.PHONY: help tool pki checkout layers risks parse framework service qemu stm32 \
        tomograph shell runqemu image-info clean purge

help:
	@echo "MedPlatform build targets (append NATIVE=1 to bypass the container)"
	@echo
	@echo "  tool        fetch + checksum the pinned kas-container $(KAS_VERSION)"
	@echo "  pki         generate the update-signing CA (keys stay out of git)"
	@echo "  checkout    clone the external layers, write build/conf, do not build"
	@echo "  layers      bitbake-layers show-layers (parse only)"
	@echo "  risks       the three known metadata risks, all parse only"
	@echo "  parse       full recipe parse + dry-run task graph for $(IMAGE)"
	@echo "  framework   build med-framework-api only (first cross compile)"
	@echo "  service     build eeg-acquisition-service only (fast inner loop)"
	@echo "  qemu        build $(IMAGE) for qemux86-64"
	@echo "  stm32       build $(IMAGE) for the STM32MP257F-DK"
	@echo "  tomograph   build the tomograph profile (reuse validation)"
	@echo "  shell       interactive build environment"
	@echo "  runqemu     boot the QEMU image, KVM accelerated, serial console"
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
	$(KAS) shell $(QEMU_CFG) -c "bitbake -p"
	$(KAS) shell $(QEMU_CFG) -c "bitbake -n $(IMAGE)"

framework: $(TOOL)
	$(KAS) shell $(QEMU_CFG) -c "bitbake med-framework-api"

service: $(TOOL)
	$(KAS) shell $(QEMU_CFG) -c "bitbake eeg-acquisition-service"

qemu: $(TOOL)
	$(KAS) build $(QEMU_CFG)

stm32: $(TOOL)
	$(KAS) build $(STM32_CFG)

tomograph: $(TOOL)
	$(KAS) build $(TOMO_CFG)

shell: $(TOOL)
	$(KAS) shell $(QEMU_CFG)

# nographic + slirp: no X11 reaches the container (see docs/BUILD_CONTAINER.md)
# and slirp keeps qemu's networking in userspace, which avoids needing a tap
# device and CAP_NET_ADMIN. Port 2222 is forwarded out for ssh.
runqemu: $(TOOL)
	$(KAS) $(RUNTIME_ARGS) shell $(QEMU_CFG) \
	  -c "runqemu qemux86-64 nographic slirp"

image-info:
	@cat $(KAS_BUILD_DIR)/buildhistory/images/qemux86_64/glibc/$(IMAGE)/image-info.txt 2>/dev/null \
	  || echo "no buildhistory yet - run 'make qemu' first"
	@ls -lh $(KAS_BUILD_DIR)/tmp/deploy/images/qemux86-64/$(IMAGE)-qemux86-64.rootfs.ext4 2>/dev/null || true

clean: $(TOOL)
	$(KAS) clean $(QEMU_CFG)

purge: $(TOOL)
	$(KAS) purge $(QEMU_CFG)
