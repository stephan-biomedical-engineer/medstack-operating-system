# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

`MedPlatform` — an undergraduate engineering thesis (TCC) proposing a reusable, layered Yocto Project reference architecture for building multiple classes of embedded **medical devices** (not a single specific device). The proof of concept is an EEG acquisition system with Asymmetric Multiprocessing (AMP: Cortex-A35 Linux + Cortex-M4 real-time core) and an HMI display, targeting both QEMU x86-64 (simulation) and STM32MP257 (physical hardware).

Yocto release: **Scarthgap (5.0 LTS)**. Build orchestration: Siemens **KAS**.

`docs/PROJECT_CONTEXT.md` is the authoritative, up-to-date architectural reference for this repo — read it before making non-trivial changes. `docs/implementation_plan*.md` are earlier design iterations (Portuguese; some use an older `produto`/`meta-produto-*` naming scheme that was superseded by the current `med`/`meta-med-*` naming with a 3-layer, not 2-layer, custom stack) — treat them as historical design rationale, not current spec.

**Current state**: the entire `meta-custom/` tree (all `.bb`, `.bbappend`, `.conf`, `layer.conf`, and header/source files) exists only as an empty scaffold — every file is 0 bytes. This is directory/file structure staged ahead of implementation, not working recipes. Don't assume any recipe, class, or config actually contains the logic its filename implies; check before relying on it.

## Build Commands

There is no local toolchain beyond `kas` — all builds run through it, which clones `poky`, `meta-openembedded`, `meta-rauc`, etc. into `layers/` on first run (network access required; outputs are gitignored).

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
```

Each `kas/project-*.yml` is a standalone entrypoint that includes `kas/kas-base.yml` and sets its own `machine:` and `target:` — there is no single default project file at the repo root (`kas-project.yml` exists but is currently empty).

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

- **`meta-med-app`** (`meta-custom/meta-med-app/`): pure userspace. Apps: `eeg-acquisition-service` (data daemon, uses `MedicalIPC`/`MedicalLogger`), `eeg-hmi-gui` (Qt/Wayland UI). Packagegroups: `packagegroup-med-core` (common), `packagegroup-med-amp` (real-time acquisition), `packagegroup-med-gui` (HMI graphics). Images: `med-image-eeg.bb`, `med-image-tomograph.bb`.

- **`kas/`**: `kas-base.yml` is the single source of truth for external repo deps (`poky`, `meta-openembedded`, `meta-rauc`) and the three `meta-med-*` layer paths, plus `local_conf_header` (package format, sstate/downloads dirs, etc). Per-target files (`project-eeg-qemu.yml`, `project-eeg-stm32mp2.yml`, `project-tomograph.yml`) only set `machine:`, `target:`, and any hardware-specific repo (e.g. `meta-st-stm32mp` for the STM32 target).

## Rules when editing this codebase

1. **No layer-boundary bleeding**: never put application logic in `meta-med-distro`, and never put BSP/hardware drivers in `meta-med-app` or `meta-med-framework`. Apps must call `Medical*.h` APIs from the framework layer, never raw sysfs/devfs or BSP drivers directly.
2. **Scarthgap compatibility**: every `conf/layer.conf` must declare `LAYERSERIES_COMPAT_<collection> = "scarthgap"`. Use modern override syntax (`:`, e.g. `IMAGE_INSTALL:append`), not the legacy `_` syntax.
3. **Kernel changes** belong in `meta-med-distro/recipes-kernel/linux/linux-%.bbappend` (wildcard, not a vendor-specific bbappend) so they stay portable across BSPs.
4. Do not commit build outputs (`build/`, `layers/`, `downloads/`, `sstate-cache/`, `.kas/`) — already covered by `.gitignore`.
5. Security features (read-only rootfs, RAUC A/B, audit logging, LUKS) exist to satisfy IEC 62304 software-partitioning/traceability requirements — treat them as architectural constraints, not optional hardening.
