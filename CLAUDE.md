# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

`MedPlatform` — an undergraduate engineering thesis (TCC) proposing a reusable, layered Yocto Project reference architecture for building multiple classes of embedded **medical devices** (not a single specific device). The proof of concept is an EEG acquisition system with Asymmetric Multiprocessing (AMP: Cortex-A35 Linux + Cortex-M4 real-time core) and an HMI display, targeting both QEMU x86-64 (simulation) and STM32MP257 (physical hardware).

Yocto release: **Scarthgap (5.0 LTS)**. Build orchestration: Siemens **KAS**.

The PoC's analogue front-end is the **TI ADS1299** (8-channel, 24-bit biopotential AFE, SPI + `DRDY`), wired to the Cortex-M4 — Linux never touches the converter, it receives sample frames over `rpmsg`. `docs/implementation_plan_ads1299.md` is the current plan for that integration and lists the changes it still requires.

`docs/implementation_plan_mac.md` is a second current plan: it records the AppArmor-vs-SELinux decision for `MedOS` (outcome: **AppArmor**, via `meta-security`), the file-by-file changes it needs, and — importantly — that it is sequenced *after* the first green `kas build`, not before. Nothing in it is implemented yet; the build it waited on is now green, so it is unblocked.

`docs/implementation_plan_rauc.md` is the third current plan, and the only one written against a booted system rather than ahead of one. `rauc.service` fails on QEMU, and the plan separates that into three independent causes (no GPT slots, no partition-backed rootfs, wrong bootloader backend) rather than the single "U-Boot" one it looks like. Two decisions in it bind future work: `bootloader=` becomes `MED_BOOTLOADER`, a per-machine variable substituted like `MED_EEG_DRIVER` — the distro layer must not name a bootloader; and validation is explicitly capped at "RAUC writes the inactive slot" on QEMU, because `boot-attempts` is `uboot`/`barebox`-only, so a GRUB A/B switch on QEMU would exercise a different fallback mechanism than the STM32MP257 ships. Nothing in it is implemented yet.

`docs/PROJECT_CONTEXT.md` is the authoritative, up-to-date architectural reference for this repo — read it before making non-trivial changes. The remaining `docs/implementation_plan*.md` (`implementation_plan.md`, `implementation_plan_EEG.md`, `implementation_plan_improvements.md`) are earlier design iterations (Portuguese; some use an older `produto`/`meta-produto-*` naming scheme that was superseded by the current `med`/`meta-med-*` naming with a 3-layer, not 2-layer, custom stack) — treat them as historical design rationale, not current spec.

**Current state**: the three `meta-med-*` layers are implemented (distro policy, kernel fragment, RAUC/LUKS config, the MedFramework C++ library, both EEG applications, packagegroups and images), and **the QEMU profile builds green** — `make qemu` completed all 4954 tasks on 2026-08-16, producing `med-image-eeg-qemux86-64.rootfs.ext4` (199 packages) in `build/tmp-glibc/deploy/images/qemux86-64/`. The bitbake metadata is therefore no longer unverified: recipe syntax, layer resolution and the package split all hold for `qemux86-64`. The image manifest carries `eeg-acquisition-service`, `eeg-hmi-gui`, `libmedframework1` and all four `packagegroup-med-*`.

Validated on the host toolchain (independently of bitbake):

- all 7 MedFramework translation units compile clean under `-Wall -Wextra -Wpedantic -Wshadow -Wconversion`;
- a functional test of `MedicalConfiguration`, `MedicalStorage`, `MedicalDevice` and `MedicalLogger` (55 checks) passes;
- `eeg-acquisition-service` builds, runs, publishes CRC-valid frames to an IPC client, writes session records, and refuses to start on a tampered config or an out-of-range safety parameter.

The three risks this file used to flag are all resolved, and the resolutions are worth keeping:

1. meta-rauc ships `rauc-conf.bb` **unversioned**, so `rauc-conf_%.bbappend` was dangling — a hard parse error. The bbappend is now `rauc-conf.bbappend` (no `_%`).
2. The `meta-qt6` branch `6.8` pin in `kas-base.yml` is good; it builds Qt 6.8.4.
3. `qtbase-plugins` and `qtdeclarative-qmlplugins` do exist — `qt6.inc` declares them via `PACKAGE_BEFORE_PN` with `ALLOW_EMPTY`. Both are installed in the image.

Also learned from that first build: **a QML application recipe needs `qtdeclarative-native` in `DEPENDS`**. `qt6-cmake.bbclass` only prepends `qtbase-native` and aims `QT_HOST_PATH` at it, so without that dep the target `Qt6QmlConfig.cmake` finds no `Qt6QmlTools`/`Qt6QuickTools` (`qmlcachegen`, `qmltyperegistrar`, `qmlimportscanner`) and `find_package(Qt6 ... Qml)` fails at `do_configure`. It is a build-time host-tool dep, so it does not count against rule 6 below. Having target `qtdeclarative` in `DEPENDS` does not substitute for it.

What is still unbuilt: the STM32MP257 profile (`make stm32mp2`), the Tomograph profile (`make tomograph`), `med-image-prod`, and booting the QEMU image under `runqemu`.

## Build Commands

There is no local toolchain beyond `kas` — all builds run through it, which clones `poky`, `meta-openembedded`, `meta-rauc`, etc. into `layers/` on first run (network access required; outputs are gitignored).

**Prefer the `Makefile`**: it runs the same builds inside the pinned upstream kas container (`ghcr.io/siemens/kas/kas:5.2`), which is how the build host itself stays reproducible. `make help` lists the targets; `make risks` (parse-only, seconds) is the right first command on a fresh clone, and `make framework` / `make service` are the cheap inner loops. Append `NATIVE=1` to any target to use the host's own kas instead — both modes share `downloads/` and `sstate-cache/`. `docs/BUILD_CONTAINER.md` covers the setup and its measured limits (notably: no X11 reaches the container under snap-packaged Docker, so the Qt HMI must be exercised with `NATIVE=1`).

The raw kas commands below still work and are what the Makefile targets wrap:

```bash
# Build the EEG PoC image for QEMU (fast inner-loop target, no hardware needed)
kas build kas/project-eeg-qemu.yml

# Build the EEG PoC image for the STM32MP257 physical target
kas build kas/project-eeg-stm32mp2.yml

# Build the Tomograph profile (architecture reuse/portability validation)
kas build kas/project-tomograph.yml

# Boot the QEMU build
kas shell kas/project-eeg-qemu.yml -c "runqemu qemux86-64 nographic"

# Inspect layer setup / priorities (parse-only sanity check, doesn't build)
kas shell kas/kas-base.yml -c "bitbake-layers show-layers"

# Verify a kernel config fragment made it into the build for the active target
kas shell kas/project-eeg-qemu.yml -c "bitbake -e virtual/kernel | grep ^SRC_URI="

# Fast inner loop: build the acquisition path alone, never compiling Qt.
# (`kas build` has no --extra-conf option — building the recipes directly is
# the way to skip the image, and `make framework` / `make service` wrap these.)
kas shell kas/project-eeg-qemu.yml -c "bitbake med-framework-api"
kas shell kas/project-eeg-qemu.yml -c "bitbake eeg-acquisition-service"
```

Each `kas/project-*.yml` is a standalone entrypoint that includes `kas/kas-base.yml` and sets its own `machine:`, `target:` and the `MED_EEG_*` variables that select the acquisition backend. `kas-project.yml` at the repo root is a thin alias for `kas/project-eeg-qemu.yml`.

The hardware adaptation surface is deliberately one variable: `MED_EEG_DRIVER` (`simulated` on QEMU, `rpmsg` on the STM32MP257) is substituted into `/etc/medplatform/eeg.conf` at build time. No application source differs between the two targets — that is what the portability metric measures.

## Architecture: the 4-layer `MedStack`

Strict, unidirectional dependency chain — upper layers consume lower layers only through abstract APIs; lower layers never depend on upper layers:

```
meta-med-app        (priority 10) — business logic, HMI GUI, device profiles/images
        │  consumes MedFramework C++ APIs only
        ▼
meta-med-framework   (priority 9)  — MedFramework: C++ abstraction over OS/HW
        │  POSIX, D-Bus, systemd, cgroups
        ▼
meta-med-distro      (priority 8)  — MedOS: distro policy, security hardening
        │  drivers, DTS, TF-A, U-Boot/GRUB
        ▼
BSP layer            (priority 6-7) — vendor layers (meta-st-stm32mp, meta-yocto-bsp)
```

- **`meta-med-distro`** (`meta-custom/meta-med-distro/`): OS policy layer. `conf/distro/med-os.conf` sets `INIT_MANAGER = "systemd"`, `usrmerge`, `rauc`. Security/compliance surface: read-only rootfs, RAUC A/B OTA, LUKS, PAM. Kernel policy is injected via the **wildcard** `recipes-kernel/linux/linux-%.bbappend` so distro-level kernel requirements (cgroups, OverlayFS, OpenAMP/rpmsg) apply automatically regardless of which vendor kernel (`linux-yocto`, `linux-st`, etc.) is in use — never target a specific `linux-<vendor>_%.bbappend` here. Disk layout lives in `wic/med-partitions.wks` (GPT dual-boot A/B with encrypted `/data`).

- **`meta-med-framework`** (`meta-custom/meta-med-framework/`): `med-framework-api` provides 6 standardized C++ abstraction headers apps must use instead of touching the OS/kernel directly:
  - `MedicalIPC` — OpenAMP `rpmsg` inter-core comms (Cortex-A35 ↔ Cortex-M4) + local IPC sockets
  - `MedicalLogger` — structured, tamper-evident audit logging to `systemd-journald` (IEC 62304 / FDA traceability)
  - `MedicalStorage` — secure persistence to encrypted `/data`
  - `MedicalUpdate` — D-Bus client wrapper around the RAUC OTA daemon
  - `MedicalConfiguration` — calibration tables, operational thresholds, DERS safety-parameter checks
  - `MedicalDevice` — universal sensor/actuator abstraction (AFEs, ADCs)

  Implementation notes: the library is one shared object (`libmedframework.so`, CMake, C++17) whose only external dependencies are `libsystemd` (sd-journal, sd-bus) and `libcrypto` (SHA-256) — everything else is POSIX, which is why it builds unchanged for both targets. `MedicalTypes.h` carries the shared `Status`/`Result<T>` vocabulary; `MedicalDevice.h` also defines the `med::amp::FrameHeader` wire format used both by the rpmsg link and by the service→HMI socket. Built-in device drivers: `simulated` and `rpmsg`.

- **`meta-med-app`** (`meta-custom/meta-med-app/`): pure userspace. Apps: `eeg-acquisition-service` (data daemon, uses `MedicalIPC`/`MedicalLogger`), `eeg-hmi-gui` (Qt6 Quick/Wayland UI). Packagegroups hold **platform capability only, never applications** — `packagegroup-med-core` (framework runtime), `packagegroup-med-amp` (AMP enablement + BSP firmware hook via `MED_AMP_FIRMWARE`), `packagegroup-med-gui` (weston + Qt, no app). Images install the apps: `med-image-eeg.bb`, `med-image-tomograph.bb`, both `require recipes-core/images/med-image-base.bb` across the layer boundary.

- **`kas/`**: `kas-base.yml` is the single source of truth for external repo deps (`poky`, `meta-openembedded`, `meta-rauc`, `meta-qt6`) and the three `meta-med-*` layer paths, plus `local_conf_header` (package format, sstate/downloads dirs, buildstats). Per-target files (`project-eeg-qemu.yml`, `project-eeg-stm32mp2.yml`, `project-tomograph.yml`) set `machine:`, `target:`, the `MED_EEG_*` variables, and any hardware-specific repo (e.g. `meta-st-stm32mp` for the STM32 target). `debug-tweaks` is deliberately *not* set globally — dev images request it themselves so `med-image-prod` stays hardened.

## Rules when editing this codebase

1. **No layer-boundary bleeding**: never put application logic in `meta-med-distro`, and never put BSP/hardware drivers in `meta-med-app` or `meta-med-framework`. Apps must call `Medical*.h` APIs from the framework layer, never raw sysfs/devfs or BSP drivers directly.
2. **Scarthgap compatibility**: every `conf/layer.conf` must declare `LAYERSERIES_COMPAT_<collection> = "scarthgap"`. Use modern override syntax (`:`, e.g. `IMAGE_INSTALL:append`), not the legacy `_` syntax.
3. **Kernel changes** belong in `meta-med-distro/recipes-kernel/linux/linux-%.bbappend` (wildcard, not a vendor-specific bbappend) so they stay portable across BSPs.
4. Do not commit build outputs (`build/`, `layers/`, `downloads/`, `sstate-cache/`, `.kas/`) — already covered by `.gitignore`.
5. Security features (read-only rootfs, RAUC A/B, audit logging, LUKS) exist to satisfy IEC 62304 software-partitioning/traceability requirements — treat them as architectural constraints, not optional hardening.
6. **Applications never gain a second dependency.** `eeg-acquisition-service` and `eeg-hmi-gui` link `medframework` and (for the HMI) Qt, and nothing else. If a new feature seems to need a system library in an app recipe's `DEPENDS`, the feature belongs behind a framework API instead — that constraint is the thesis, not a style rule.
7. **`linux-%` also matches non-kernels** (`linux-libc-headers`, `linux-firmware`). The bbappend guards on `bb.data.inherits_class('kernel', d)`; keep that guard when extending it.
8. **Config stores are sealed at build time.** `MedicalConfiguration` refuses to load a store without a matching `.sha256` sidecar, so any recipe shipping one must generate the sidecar after substituting variables (see `do_seal_configuration` in `eeg-acquisition-service_1.0.0.bb`).
