# AI Agent Context: MedPlatform Project

> **Purpose**: This document provides strict architectural context, design principles, and guidelines for AI Agents (LLMs, coding assistants, autonomous agents) working within this repository.

---

## 1. Project Identity & Scope

* **Project Name**: `MedPlatform` (Medical Reference Architecture)
* **OS Layer Name**: `MedOS` (`meta-med-distro`)
* **Framework Layer Name**: `MedFramework` (`meta-med-framework`)
* **Application Layer Name**: `MedApp` (`meta-med-app`)
* **Yocto Release Target**: `Scarthgap` (5.0 LTS)
* **Build Orchestration**: Siemens **KAS** (`kas/*.yml`)
* **Academic Context**: Undergraduate Engineering Thesis (TCC - Trabalho de Conclusão de Curso)
* **Proof of Concept (PoC)**: Electroencephalogram (EEG) with Asymmetric Multiprocessing (AMP: Cortex-A35 + Cortex-M4) and HMI Display on **STM32MP257** and **QEMU x86-64**.

---

## 2. Core Architectural Thesis Statement

> *"Unlike projects proposing a single specific medical device, this project proposes a reusable, layered reference architecture for building multiple classes of embedded medical devices."*

---

## 3. The 4-Layer Architectural Hierarchy (`MedStack`)

All code written in this repository must strictly adhere to the 4-layer unidirectionally decoupled hierarchy. Upper layers consume lower layers via abstract APIs; lower layers **never** depend on upper layers.

```text
+-------------------------------------------------------------------------+
|                        meta-med-app (Priority 10)                       |
|           [Business Logic, HMI GUI & Device Profiles]                   |
|  - Applications: eeg-acquisition-service, eeg-hmi-gui                   |
|  - Targets: med-image-eeg, med-image-tomograph                         |
|  - Packagegroups: packagegroup-med-core, med-amp, med-gui               |
+-------------------------------------------------------------------------+
                                     │
                                     ▼ (Consumes MedFramework C++ APIs)
+-------------------------------------------------------------------------+
|                      meta-med-framework (Priority 9)                    |
|                [Medical Abstraction Framework - MedFramework]           |
|  - C++ Abstraction APIs: MedicalIPC, MedicalLogger, MedicalStorage,     |
|    MedicalUpdate, MedicalConfiguration, MedicalDevice                   |
|  - Strict Isolation: Apps NEVER touch Linux OS / HW directly            |
+-------------------------------------------------------------------------+
                                     │
                                     ▼ (POSIX, D-Bus, Systemd, Cgroups)
+-------------------------------------------------------------------------+
|                      meta-med-distro (Priority 8)                       |
|                [OS Policies & Security Hardening - MedOS]               |
|  - Distro Policy: med-os.conf (Systemd exclusive)                       |
|  - Security & Compliance: Read-Only RootFS, RAUC A/B OTA, LUKS, PAM     |
|  - OS Kernel Fragments: linux-%.bbappend (Cgroups v2, OverlayFS, OpenAMP)|
|  - Disk Layout: WIC partition scheme (med-partitions.wks)               |
+-------------------------------------------------------------------------+
                                     │
                                     ▼ (Drivers, DTS, TF-A, U-Boot / GRUB)
+-------------------------------------------------------------------------+
|                         BSP Layer (Priority 6-7)                        |
|  - External vendor layers: meta-st-stm32mp, meta-yocto-bsp              |
|  - Hardware enablement: Linux Kernel tree, Device Trees, Bootloader     |
+-------------------------------------------------------------------------+
```

---

## 4. Key Components Breakdown

### 4.1. `kas/` (Build Configuration Directory)
* `kas/kas-base.yml`: Common repository definitions (`poky`, `meta-openembedded`, `meta-rauc`, `meta-qt6`), layer paths, and BitBake environment settings.
* `kas/project-eeg-qemu.yml`: KAS entrypoint for simulation (`machine: qemux86-64`, `target: med-image-eeg`, `MED_EEG_DRIVER = "simulated"`).
* `kas/project-eeg-stm32mp2.yml`: KAS entrypoint for physical target (`machine: stm32mp257f-ev1`, `target: med-image-eeg`, `MED_EEG_DRIVER = "rpmsg"`).
* `kas/project-tomograph.yml`: KAS entrypoint for Tomograph profile validation (`target: med-image-tomograph`).
* `kas-project.yml` (repo root): alias for `kas/project-eeg-qemu.yml`, so the default entrypoint is the PoC.

### 4.2. `meta-med-distro` (OS Infrastructure & Regulatory Foundation)
* **Distro Config**: `conf/distro/med-os.conf` enforces `INIT_MANAGER = "systemd"`, `usrmerge`, `rauc`.
* **Kernel Policy**: `recipes-kernel/linux/linux-%.bbappend` injects `med-kernel-features.cfg` using wildcard `linux-%` so it automatically applies to any vendor kernel (`linux-yocto`, `linux-raspberrypi`, `linux-st`).
* **OTA & Partitioning**: `recipes-core/rauc/` configures RAUC A/B slot system, and `wic/med-partitions.wks` defines GPT Dual-Boot layout with encrypted `/data`.

### 4.3. `meta-med-framework` (C++ Middleware Domain Abstraction)
Contains 6 standardized C++ abstraction classes:
1. `MedicalIPC`: OpenAMP `rpmsg` inter-core communication (Cortex-A35 <-> Cortex-M4) & local IPC sockets.
2. `MedicalLogger`: Structured, tamper-evident audit logging targeting `systemd-journald` (IEC 62304 / FDA compliance).
3. `MedicalStorage`: Secure data persistence interface to encrypted storage (`/data`).
4. `MedicalUpdate`: D-Bus client wrapper for RAUC A/B OTA update daemon.
5. `MedicalConfiguration`: Calibration tables, operational thresholds, and DERS safety parameter verification.
6. `MedicalDevice`: Universal abstraction interface for biomedical sensors (AFEs, ADCs) and actuators.

### 4.4. `meta-med-app` (Pure Userspace Applications & Profiles)
* **Applications**: `eeg-acquisition-service` (data daemon using `MedicalIPC`, `MedicalDevice`, `MedicalStorage`, `MedicalConfiguration`, `MedicalUpdate` & `MedicalLogger`), `eeg-hmi-gui` (Qt6 Quick / Wayland user interface).
* **Packagegroups** carry reusable platform capability and **never an application**: `packagegroup-med-core.bb` (MedFramework runtime), `packagegroup-med-amp.bb` (AMP enablement plus the `MED_AMP_FIRMWARE` hook a BSP fills in), `packagegroup-med-gui.bb` (weston + Qt runtime). Applications are installed by the image that owns them, which is what lets the tomograph profile reuse `med-core` and `med-gui` byte for byte.
* **Images**: `med-image-eeg.bb` (PoC) and `med-image-tomograph.bb`, both `require recipes-core/images/med-image-base.bb` from `meta-med-distro`.

### 4.5. Implementation status

The three layers are implemented; **no `kas build` has run yet**, so the BitBake metadata itself is unverified. Validated on the host toolchain: the seven MedFramework translation units compile warning-free under `-Wall -Wextra -Wpedantic -Wshadow -Wconversion`; a 55-check functional test of `MedicalConfiguration`, `MedicalStorage`, `MedicalDevice` and `MedicalLogger` passes; and `eeg-acquisition-service` runs end to end — publishing CRC-valid AMP frames to an IPC client, writing session records with `0600`, and refusing to start on either a tampered configuration or an out-of-range safety parameter.

Two assumptions to confirm on the first real build: `rauc-conf_%.bbappend` expects meta-rauc to provide a `rauc-conf` recipe, and `meta-qt6` is pinned to branch `6.8`.

---

## 5. Strict Guidelines for AI Agents

When editing or extending this codebase, AI Agents **MUST** strictly comply with the following rules:

1. **No Layer Boundary Bleeding**:
   - Do **NOT** place application logic inside `meta-med-distro`.
   - Do **NOT** place BSP or hardware-specific drivers inside `meta-med-app` or `meta-med-framework`.
   - Applications in `meta-med-app` MUST consume `MedFramework` C++ APIs (`Medical*.h`), never calling raw kernel sysfs/devfs or BSP drivers directly.

2. **Scarthgap Compatibility**:
   - All `conf/layer.conf` files MUST declare `LAYERSERIES_COMPAT_<collection> = "scarthgap"`.
   - Use modern BitBake override syntax (`:` instead of `_`, e.g., `IMAGE_INSTALL:append = " ..."`).
   - Use canonical systemd configuration (`INIT_MANAGER = "systemd"`).

3. **Kernel Abstraction**:
   - Always append kernel fragments using `linux-%.bbappend` in `meta-med-distro` to ensure compatibility across vendor BSP kernels.

4. **KAS Multi-Target Paradigm**:
   - Maintain `kas/kas-base.yml` as the single source of truth for repository dependencies.
   - Do NOT commit generated build artifacts (`build/`, `layers/`, `sstate-cache/`, `downloads/`).

5. **Regulatory Alignment (IEC 62304 / FDA)**:
   - Frame security features (Read-Only RootFS, RAUC A/B, Audit Logging, LUKS) as architectural enablers for IEC 62304 software partitioning and traceability.

---

## 6. Common Build Commands

* **Build PoC EEG for QEMU**:
  ```bash
  kas build kas/project-eeg-qemu.yml
  ```
* **Build PoC EEG for STM32MP257**:
  ```bash
  kas build kas/project-eeg-stm32mp2.yml
  ```
* **Run QEMU Simulation**:
  ```bash
  kas shell kas/project-eeg-qemu.yml -c "runqemu qemux86-64 nographic"
  ```
* **Inspect BitBake Layers**:
  ```bash
  kas shell kas/kas-base.yml -c "bitbake-layers show-layers"
  ```
