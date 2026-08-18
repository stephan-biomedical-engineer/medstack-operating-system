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
* **PoC Analogue Front-End**: Texas Instruments **ADS1299** (8-channel, 24-bit biopotential AFE, SPI + `DRDY`), attached to the Cortex-M4 and reached from Linux over OpenAMP `rpmsg`. Linux never talks to the converter directly. See `docs/implementation_plan_ads1299.md` for the integration plan and the changes it requires — that document is current, unlike the other `implementation_plan*.md` files.

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
|  - Disk POLICY: A/B slot count, names, symmetry; /data is not a slot     |
|  - Names no machine, no bootloader, no device node                       |
+-------------------------------------------------------------------------+
                                     │
                                     ▼ (Selects a layout by MED_WKS_FILE)
+-------------------------------------------------------------------------+
|                       meta-med-bsp (Priority 7)                         |
|                    [Board Adaptation - PLACEMENT]                       |
|  - Disk PLACEMENT: wic/med-partitions-efi.wks (GRUB/EFI machines),      |
|    wic/med-partitions-stm32mp2.wks.in (TF-A/FIP at ROM offsets)         |
|  - Boot path: QB_FSINFO / QB_KERNEL_ROOT for the emulator               |
|  - The ONLY custom layer permitted to name a machine or a bootloader    |
+-------------------------------------------------------------------------+
                                     │
                                     ▼ (Drivers, DTS, TF-A, U-Boot / GRUB)
+-------------------------------------------------------------------------+
|                      Vendor BSP Layer (Priority 6)                      |
|  - External vendor layers: meta-st-stm32mp, meta-yocto-bsp              |
|  - Hardware enablement: Linux Kernel tree, Device Trees, Bootloader     |
+-------------------------------------------------------------------------+
```

---

## 4. Key Components Breakdown

### 4.1. `kas/` (Build Configuration Directory)
* `kas/kas-base.yml`: Common repository definitions (`poky`, `meta-openembedded`, `meta-rauc`, `meta-qt6`), layer paths, and BitBake environment settings.
* `kas/project-eeg-qemu.yml`: KAS entrypoint for simulation (`machine: qemux86-64`, `target: med-image-eeg`, `MED_EEG_DRIVER = "simulated"`).
* `kas/project-eeg-stm32mp2.yml`: KAS entrypoint for physical target (`machine: stm32mp25-disco`, `target: med-image-eeg`, `MED_EEG_DRIVER = "rpmsg"`).
* `kas/project-tomograph.yml`: KAS entrypoint for Tomograph profile validation (`target: med-image-tomograph`).
* `kas-project.yml` (repo root): alias for `kas/project-eeg-qemu.yml`, so the default entrypoint is the PoC.

**The hardware adaptation surface is a small, enumerable set of variables**, and none of them is
answered by a layer above `meta-med-bsp`. This is a sharper and more honest form of the thesis
claim than "one variable", and it is what a reviewer can check. Four are valued in the KAS project
file of the target; `MED_WKS_FILE` is answered by `meta-med-bsp` keyed on `MACHINE`, because a disk
layout is a file and not a scalar:

| Variable | QEMU | STM32MP257 | Why it cannot live in a layer |
|---|---|---|---|
| `MED_EEG_DRIVER` | `simulated` | `rpmsg` | The front-end is the machine's, not the distro's |
| `MED_BOOTLOADER` | `noop` | `uboot` | RAUC has to talk to the bootloader the board actually has |
| `MED_WKS_FILE` | `med-partitions-efi.wks` | `med-partitions-stm32mp2.wks.in` | TF-A/U-Boot live at fixed offsets on the SoC. Both files are in `meta-med-bsp`; the vendor BSP supplies neither |
| `MED_DATA_KEY_SOURCE` | `development` | `tpm2` | Key custody depends on the hardware present |
| `MED_EEG_REQUIRE_ENCRYPTION` | `true` | `true` | Policy, defaulted safe, overridable only deliberately |
| `MED_AMP_FIRMWARE` | unset | BSP recipe | Co-processor firmware is a BSP artefact |

No application source, no framework source and no OS policy differs between the two targets.

### 4.2. `meta-med-distro` (OS Infrastructure & Regulatory Foundation)
* **Distro Config**: `conf/distro/med-os.conf` enforces `INIT_MANAGER = "systemd"`, `usrmerge`, `rauc`.
* **Kernel Policy**: `recipes-kernel/linux/linux-%.bbappend` injects `med-kernel-features.cfg` using wildcard `linux-%` so it automatically applies to any vendor kernel (`linux-yocto`, `linux-raspberrypi`, `linux-st`).
* **OTA & Partitioning**: `recipes-core/rauc/` configures the RAUC A/B slot system. The *policy* it depends on — two identically sized slots named `med-root-a`/`med-root-b`, `/data` outside them — is stated here and honoured by every layout; the layouts themselves are in `meta-med-bsp` (§4.3), because saying where partitions start means naming a bootloader. `med-os.conf` selects one with `WKS_FILE = "${MED_WKS_FILE}"` and names no file; `med-image-base.bb` fails the build with an actionable message if no machine adaptation answers.
* **Encrypted `/data`**: `recipes-core/med-data-volume/` owns the volume. wic cannot write a LUKS header, so `med-data-provision.service` converts the partition on first boot and opens it on every boot. Its safeguard distinguishes "never provisioned" from "LUKS header damaged" — identical symptoms — by keying on the pristine filesystem label wic writes, and refuses anything else rather than reformatting over patient records.
* **Signing PKI**: `scripts/med-pki.sh` generates the update-signing CA. Only the public keyring (CA certificate + CRL) is committed, in `recipes-core/rauc/rauc-conf/`; private keys stay in the gitignored `pki/`.

### 4.3. `meta-med-bsp` (Board Adaptation — the Portability Surface)
* **Charter**: everything true of a *board* rather than of the platform. It is the only custom layer permitted to name a machine, a bootloader, a partition offset or a device node, and that restriction is what makes the reuse claim countable — a new board adds files here and changes nothing above.
* **Disk layouts**: `wic/med-partitions-efi.wks` (GRUB/EFI machines: ESP, `med-root-a`, `med-root-b`, `med-data`) and `wic/med-partitions-stm32mp2.wks.in` (`fsbla1/2`, `metadata1/2`, `fip-a/b`, `u-boot-env` at the offsets the STM32MP2 ROM code searches, then `bootfs` and the same three A/B partitions). Same policy, different placement.
* **Selection**: `MED_WKS_FILE_DEFAULT:<machine>` in `conf/layer.conf`, then `MED_WKS_FILE ?= "${MED_WKS_FILE_DEFAULT}"`. The two-step is load-bearing: a bare `MED_WKS_FILE:<machine> =` would beat the unsuffixed assignment a KAS project file makes, so an explicit per-target override would silently lose. Verified by fault injection — setting `MED_WKS_FILE = ""` from a KAS fragment does override the layer, and trips the guard below.
* **Boot path**: `QB_FSINFO` and `QB_KERNEL_ROOT` for `qemux86-64`. `/dev/vda2` is coupled to the partition *order* of `med-partitions-efi.wks`; both now live in this layer, so inserting a partition ahead of the rootfs can no longer invalidate a device path recorded two layers up. Conf scope rather than recipe scope is safe here because nothing in poky's `qemux86-64.conf` or `qemuboot.bbclass` assigns either variable — checked, not assumed.
* **Dependency direction**: `LAYERDEPENDS_meta-med-bsp = "core"`. This layer provides and never consumes; a bbappend here onto a recipe from a higher layer would invert the chain and is forbidden.
* **Why it exists**: the boundary was crossed twice without failing a build. `med-partitions.wks` hardcoded `loader=grub-efi` and `console=ttyS0,115200` behind a generic filename inside the OS policy layer — the same layer where `rauc-conf.bbappend` spends twelve lines explaining that naming a bootloader there is forbidden — and `med-image-base.bb` carried `QB_KERNEL_ROOT = "/dev/vda2"` two lines under a comment asserting it was device-class agnostic. Neither surfaced until `stm32mp25-disco` needed a different answer, at which point the distro default won by forfeit and wic tried to build an EFI/GRUB ESP for a board with no ESP.
* **Not here yet, and belongs here**: per-machine key custody (`med-data-provision.sh` hardcodes `/dev/disk/by-partlabel/esp` and `mount -t vfat`, which exist only in the EFI layout; gated on `make check` because that script is validated end-to-end on QEMU), and `/etc/fw_env.config` for `stm32mp25-disco` (RAUC's `uboot` backend calls `fw_setenv`, which reads that path; `u-boot-fw-config-stm32mp` installs `fw_env.config.mmc/.nand/.nor` and never that name).

### 4.4. `meta-med-framework` (C++ Middleware Domain Abstraction)
Contains 6 standardized C++ abstraction classes:
1. `MedicalIPC`: OpenAMP `rpmsg` inter-core communication (Cortex-A35 <-> Cortex-M4) & local IPC sockets.
2. `MedicalLogger`: Structured, tamper-evident audit logging targeting `systemd-journald` (IEC 62304 / FDA compliance).
3. `MedicalStorage`: Secure data persistence interface to encrypted storage (`/data`).
4. `MedicalUpdate`: D-Bus client wrapper for RAUC A/B OTA update daemon.
5. `MedicalConfiguration`: Calibration tables, operational thresholds, and DERS safety parameter verification.
6. `MedicalDevice`: Universal abstraction interface for biomedical sensors (AFEs, ADCs) and actuators.

### 4.5. `meta-med-app` (Pure Userspace Applications & Profiles)
* **Applications**: `eeg-acquisition-service` (data daemon using `MedicalIPC`, `MedicalDevice`, `MedicalStorage`, `MedicalConfiguration`, `MedicalUpdate` & `MedicalLogger`), `eeg-hmi-gui` (Qt6 Quick / Wayland user interface).
* **Packagegroups** carry reusable platform capability and **never an application**: `packagegroup-med-core.bb` (MedFramework runtime), `packagegroup-med-amp.bb` (AMP enablement plus the `MED_AMP_FIRMWARE` hook a BSP fills in), `packagegroup-med-gui.bb` (weston + Qt runtime). Applications are installed by the image that owns them, which is what lets the tomograph profile reuse `med-core` and `med-gui` byte for byte.
* **Images**: `med-image-eeg.bb` (PoC) and `med-image-tomograph.bb`, both `require recipes-core/images/med-image-base.bb` **and** `med-image-dev.inc` from `meta-med-distro`. The `.inc` holds the development profile (writable rootfs, ssh, debug-tweaks, verification tooling) in one place so the two device profiles cannot drift apart — which matters because the reuse metric is only meaningful if they differ exclusively by device class. `med-image-prod.bb` requires only the base, so production differs by not opting in.
* **Update bundle**: `recipes-core/bundles/med-bundle-eeg.bb`. `RAUC_BUNDLE_COMPATIBLE` must equal what `rauc-conf.bbappend` writes into `system.conf`, and `RAUC_BUNDLE_FORMAT` must be `verity`; neither has a safe default, and a mismatch is only caught at install time on the device.

### 4.6. Implementation status

The QEMU profile builds, boots and is verified. `docs/RESULTS.md` is the authoritative record of
what has been **measured**; this section only says where things stand.

Built and validated on `qemux86-64`: both device profiles, the signed A/B update path (bundle
verified against the device keyring and written to the inactive slot), and the encrypted `/data`
volume. `make check` boots the image and runs 21 runtime assertions, exiting non-zero on failure.

The **STM32MP257** target now builds too (`make stm32`, `MACHINE=stm32mp25-disco`), producing a
2.57 GiB `.wic` whose GPT was verified by inspection: 11 partitions, both slots exactly 1 GiB,
`med-root-a`'s PARTUUID equal to the `root=PARTUUID=` in the BSP's generated `extlinux.conf`, and
TF-A/FIP magic at the offsets the ROM code searches. Nothing has been *booted* on that hardware,
and boot-slot selection is not implemented — `bootfs` is shared by both slots and `extlinux.conf`
hardcodes slot A, so a device RAUC has marked "boot B" still boots A.

Never built: `med-image-prod`. Read-only rootfs, real-time latency, AMP/`rpmsg`, TPM and bootloader
integration are therefore unmeasured — see `RESULTS.md` §9, which lists absences explicitly so they
are not read as results.

**The lesson that governs this repository**: in a system with an update path, *"it builds and
boots" is not evidence*. Of the six defects found while implementing the A/B path, five failed no
build at all — they produced artefacts that compiled, booted and ran, and would have surfaced on a
device in the field attempting an update. Inspect the produced artefact (`sfdisk -l` the `.wic`,
read the installed config) and run `make check`; do not trust a green build.

---

## 5. Strict Guidelines for AI Agents

When editing or extending this codebase, AI Agents **MUST** strictly comply with the following rules:

1. **No Layer Boundary Bleeding**:
   - Do **NOT** place application logic inside `meta-med-distro`.
   - Do **NOT** place BSP or hardware-specific drivers inside `meta-med-app` or `meta-med-framework`.
   - Applications in `meta-med-app` MUST consume `MedFramework` C++ APIs (`Medical*.h`), never calling raw kernel sysfs/devfs or BSP drivers directly.
   - A machine name, a bootloader name, a partition offset or a device node (`/dev/vda2`, `mmcblk`, `ttyS0`, `by-partlabel/esp`) belongs in `meta-med-bsp` and **nowhere above it**. No build reports a violation of this rule; both known violations hid behind a generic-looking name over machine-specific content. When a value differs per board, add it to `meta-med-bsp` keyed by machine override rather than defaulting it upstack.

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

6. **Verify the artefact, not the build**:
   - A successful `bitbake` run proves almost nothing about the update path, the disk layout or the
     encrypted volume. Run `make check` after any change that could affect runtime behaviour.
   - When adding an assertion, make it fail on purpose first. `acq-active` used
     `systemctl is-active` and passed a crash-looping service, because a `Type=simple` unit with
     `Restart=on-failure` is briefly active on every retry. **An assertion that has never seen the
     failure it looks for is a claim, not a check.**

7. **Machine properties belong in the project file**:
   - Anything that differs between targets — bootloader, disk layout, key custody, front-end driver
     — is a variable valued in `kas/project-*.yml`, never a constant in a layer. `system.conf` once
     hardcoded `bootloader=uboot` while `med-partitions.wks`, in the same layer, documented itself
     as targeting EFI/GRUB machines; no build could detect the contradiction.

---

## 6. Common Build Commands

Prefer the `Makefile`: it runs the same builds inside the pinned upstream kas container, which is
what keeps the build host itself reproducible. `make help` lists every target. Append `NATIVE=1` to
use the host's own kas instead.

```bash
make pki          # once: generate the development signing CA (keys stay out of git)
make qemu         # build med-image-eeg for qemux86-64
make tomograph    # build the tomograph profile (the reuse control case)
make bundle       # build the signed RAUC update bundle
make verify-bundle  # verify it exactly as the device would (keyring + purpose + CRL)
make runqemu      # boot the GPT disk, serial console
make check        # boot and run 21 runtime assertions
```

Raw kas still works and is what those targets wrap:

```bash
kas build kas/project-eeg-qemu.yml
kas shell kas/kas-base.yml -c "bitbake-layers show-layers"
```

Note that `make runqemu` boots the **`.wic`**, not the bare `ext4`. There is deliberately one QEMU
boot path: `QB_KERNEL_ROOT` takes a single value, so serving both would need contradictory kernel
roots, and without the partition table RAUC resolves no slots at all.

---

## 7. Documents

* `docs/RESULTS.md` — measured evidence. Every number carries the command that produced it and what
  it does **not** mean. Read before claiming anything about runtime behaviour.
* `docs/BRINGUP_STM32MP2.md` — engineering record of the port to the physical target: what broke,
  what the real cause was (rarely the symptom), and what proved each fix. Also the host-side work the
  port forced — disk exhaustion, `rm_work`, the external cache disk — and the `tpm2` finding. Read it
  before repeating any of it.
* `docs/implementation_plan_rauc.md`, `docs/implementation_plan_luks.md` — current, **implemented**;
  their §8 sections record measured results. The LUKS plan's §9 records a second execution: key
  custody became a per-machine fact, and `tpm2` turned out to be unreachable in the current layer
  set, so custody stays development-grade on **both** targets.
* `docs/implementation_plan_mac.md`, `docs/implementation_plan_ads1299.md` — current, **not yet
  implemented**.
* `docs/implementation_plan.md`, `_EEG.md`, `_improvements.md` — earlier design iterations, kept as
  rationale. Some use the superseded `produto`/`meta-produto-*` naming. Not current spec.
* `docs/BUILD_CONTAINER.md` — the container build host and its measured limits.
