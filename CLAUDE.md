# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

`MedPlatform` — an undergraduate engineering thesis (TCC) proposing a reusable, layered Yocto Project reference architecture for building multiple classes of embedded **medical devices** (not a single specific device). The proof of concept is an EEG acquisition system with Asymmetric Multiprocessing (AMP: Cortex-A35 Linux + Cortex-M4 real-time core) and an HMI display, targeting both QEMU x86-64 (simulation) and STM32MP257 (physical hardware).

Yocto release: **Scarthgap (5.0 LTS)**. Build orchestration: Siemens **KAS**.

The PoC's analogue front-end is the **TI ADS1299** (8-channel, 24-bit biopotential AFE, SPI + `DRDY`), wired to the Cortex-M4 — Linux never touches the converter, it receives sample frames over `rpmsg`. `docs/implementation_plan_ads1299.md` is the current plan for that integration and lists the changes it still requires.

`docs/implementation_plan_mac.md` is a second current plan: it records the AppArmor-vs-SELinux decision for `MedOS` (outcome: **AppArmor**, via `meta-security`), the file-by-file changes it needs, and — importantly — that it is sequenced *after* the first green `kas build`, not before. Nothing in it is implemented yet; the build it waited on is now green, so it is unblocked.

`docs/implementation_plan_rauc.md` is the third current plan, and — unlike the other two — it is **implemented and validated**; its §8 records the measured results. The A/B update path now works on QEMU: `rauc.service` starts, enumerates both slots, identifies the booted one, and `rauc install` verifies a signed verity bundle against the device keyring and writes the inactive slot. Two decisions in it bind future work: `bootloader=` is now `MED_BOOTLOADER`, a per-machine variable substituted like `MED_EEG_DRIVER` (the distro layer must not name a bootloader), and validation is deliberately capped below a real GRUB A/B switch, because `boot-attempts` is `uboot`/`barebox`-only — so GRUB fallback on QEMU would exercise a different mechanism than the STM32MP257 ships. Say "the A/B policy is validated on QEMU, the bootloader integration on hardware"; anything stronger overstates it.

`docs/implementation_plan_luks.md` is the fourth plan and, like the third, is **implemented and validated** — its §8 records the measured results. `/data` is a real LUKS2 volume, provisioned on first boot by `med-data-provision.service`, and `MED_EEG_REQUIRE_ENCRYPTION` is now `"true"` on QEMU: `MedicalStorage` requires `/data`'s `dm/uuid` to start with `CRYPT-`, so the acquisition service running at all *is* the assertion. Key custody is `MED_DATA_KEY_SOURCE`, defaulting to `tpm2` so a target that forgets fails loudly rather than shipping something that looks encrypted and is not; `tpm2` itself refuses to provision rather than half-existing. **Where** a development key may live is now a second, separate variable pair (`MED_KEY_STORE_DEV`/`MED_KEY_STORE_FSTYPE`) answered per machine by `meta-med-bsp`, because "a partition no update writes to" is a partition table and therefore a board fact — the ESP on EFI machines, `bootfs` on the STM32MP2. Its §9 also records that `tpm2` is **not reachable** in the current layer set (meta-tpm ships only software emulators; there is no OP-TEE fTPM recipe), so custody is development-grade on *both* targets, not just on the simulation one.

Two things there are worth knowing before touching that code. The safeguard in `med-data-provision.sh` distinguishes "never provisioned" from "LUKS header damaged" — which produce identical symptoms — by keying on the pristine ext4 label wic writes, and refuses anything else rather than reformatting over patient records; it has been exercised by zeroing the header, and it holds. And a key has to survive an A/B slot switch: a key on the rootfs would be destroyed by the first *successful* update, leaving the device unable to decrypt its own records, which is why the development key lives on the ESP. That is the same hazard §7.3 describes for TPM PCR sealing, and §7.3 is still unevaluated.

That work also turned up a rule worth carrying: **in this repository, "it builds and boots" is not evidence about the update path.** Six defects were found, and five of them failed nothing — a `system.conf` missing `check-purpose` (every field update rejected, with the correct certificate as the cause), a bundle `compatible` string that silently disagreed with the device's, `WKS_FILE ?=` in the distro conf that never won against `qemux86-64.conf` because `bitbake.conf` includes the machine conf before the distro conf (so the A/B GPT layout was never in the image at all), A/B slots that came out different sizes because wic treats `--size` as a minimum and applies a 1.3 overhead factor only to a partition with a `--source`, and an `/etc/fstab` naming `/dev/sda1` for `/boot` because wic's `--ondisk` defaults to `sda` while QEMU presents virtio. Inspect produced artefacts (`sfdisk -l` the `.wic`, read the installed config) rather than trusting a green build.

`make check` is the runtime acceptance suite: it boots the QEMU image, runs 21 assertions over the serial console, and exits non-zero on failure. Run it after any change that could affect runtime behaviour — it is the only thing in this repository that catches a regression a build cannot. When adding an assertion, remember what fault injection taught this suite: `acq-active` used `systemctl is-active` and passed a crash-looping service, because a `Type=simple` unit with `Restart=on-failure` is briefly active on every retry. An assertion that has never seen the failure it looks for is a claim, not a check.

`docs/RESULTS.md` is the measured evidence — the experimental half of the thesis. It is the place to look before claiming anything about this platform's behaviour, and the place to record a new measurement. Every number carries the command that produced it and an explicit statement of what it does **not** mean; its §9 lists what has not been measured, so an absence is not read as a result. Two things there are easy to overstate and are deliberately framed against that: the reuse figure is reported as "the platform both device classes inherit is 206 packages, the EEG's device-specific delta is 3" rather than as a similarity percentage (the tomograph installs no device application, so its delta is zero by construction), and the A/B update result covers the update *policy*, not bootloader integration.

`docs/BRINGUP_STM32MP2.md` is the engineering record of the port to the physical target — every defect, its real cause, and what proved the fix, plus the host-side disk work the port forced. Its §10 is the transferable part: seven rules earned the expensive way, including "a documented assumption about third-party behaviour is debt", "a generic name over machine-specific content is the standard disguise for layer bleeding", and "a defect hidden behind a refusal is still a defect".

`docs/PROJECT_CONTEXT.md` is the authoritative, up-to-date architectural reference for this repo — read it before making non-trivial changes. The remaining `docs/implementation_plan*.md` (`implementation_plan.md`, `implementation_plan_EEG.md`, `implementation_plan_improvements.md`) are earlier design iterations (Portuguese; some use an older `produto`/`meta-produto-*` naming scheme that was superseded by the current `med`/`meta-med-*` naming with a 4-layer, not 2-layer, custom stack — `meta-med-bsp` is newer than all of them) — treat them as historical design rationale, not current spec.

**Current state**: the four `meta-med-*` layers are implemented (distro policy, kernel fragment, RAUC/LUKS config, the MedFramework C++ library, both EEG applications, packagegroups and images), and **the QEMU profile builds green** — `make qemu` completed all 4954 tasks on 2026-08-16, producing `med-image-eeg-qemux86-64.rootfs.ext4` (209 packages) in `build/tmp-glibc/deploy/images/qemux86-64/`. The bitbake metadata is therefore no longer unverified: recipe syntax, layer resolution and the package split all hold for `qemux86-64`. The image manifest carries `eeg-acquisition-service`, `eeg-hmi-gui`, `libmedframework1` and all four `packagegroup-med-*`.

Validated on the host toolchain (independently of bitbake):

- all 7 MedFramework translation units compile clean under `-Wall -Wextra -Wpedantic -Wshadow -Wconversion`;
- a functional test of `MedicalConfiguration`, `MedicalStorage`, `MedicalDevice` and `MedicalLogger` (55 checks) passes;
- `eeg-acquisition-service` builds, runs, publishes CRC-valid frames to an IPC client, writes session records, and refuses to start on a tampered config or an out-of-range safety parameter.

The three risks this file used to flag are all resolved, and the resolutions are worth keeping:

1. meta-rauc ships `rauc-conf.bb` **unversioned**, so `rauc-conf_%.bbappend` was dangling — a hard parse error. The bbappend is now `rauc-conf.bbappend` (no `_%`).
2. The `meta-qt6` branch `6.8` pin in `kas-base.yml` is good; it builds Qt 6.8.4.
3. `qtbase-plugins` and `qtdeclarative-qmlplugins` do exist — `qt6.inc` declares them via `PACKAGE_BEFORE_PN` with `ALLOW_EMPTY`. Both are installed in the image.

Also learned from that first build: **a QML application recipe needs `qtdeclarative-native` in `DEPENDS`**. `qt6-cmake.bbclass` only prepends `qtbase-native` and aims `QT_HOST_PATH` at it, so without that dep the target `Qt6QmlConfig.cmake` finds no `Qt6QmlTools`/`Qt6QuickTools` (`qmlcachegen`, `qmltyperegistrar`, `qmlimportscanner`) and `find_package(Qt6 ... Qml)` fails at `do_configure`. It is a build-time host-tool dep, so it does not count against rule 6 below. Having target `qtdeclarative` in `DEPENDS` does not substitute for it.

The **STM32MP257 profile now builds green too** — `make stm32` completed all 5364 tasks on 2026-08-17, producing `med-image-eeg-stm32mp25-disco.rootfs.wic` (2.57 GiB) for `MACHINE=stm32mp25-disco`. The same recipes, the same applications and the same framework cross-compile for aarch64/cortexa35, which is the portability claim the thesis makes, now with a second machine behind it.

Getting there cost one defect worth remembering, because it is the same shape as the `WKS_FILE ?=` one below: **meta-st-stm32mp does not set `WKS_FILE`**. Its machine conf sets `WKS_FILE_DEPENDS` and leaves `#WKS_FILE += "${OPTEE_WIC_FILE}"` commented out, shipping `wic/sdcard-stm32mp257f-dk-optee-example.wks.in` as an example to copy — so `med-partitions.wks`'s claim that "vendor BSPs that need TF-A/U-Boot at fixed offsets ship their own `WKS_FILE`" was false, the distro default won by forfeit, and `do_image_wic` tried to build an EFI/GRUB ESP for a board with no ESP. It failed on `install: cannot stat '.../Image.gz'` (meta-st deploys the kernel under `${DEPLOY_DIR_IMAGE}/kernel/`), which named the kernel and not the layout — the symptom was three steps from the cause. The fix is `meta-med-bsp/wic/med-partitions-stm32mp2.wks.in`, and it was verified by dumping the GPT of the produced `.wic`, not by the green build: 11 partitions, both slots exactly 1 GiB, `med-root-a`'s PARTUUID equal to the `root=PARTUUID=` the BSP's generated `extlinux.conf` names, `med-data` carrying the pristine `ext4`/`med-data` label the provisioning safeguard keys on, and the TF-A/FIP magic bytes at the offsets the ROM code searches.

That fix first landed in `meta-med-distro/wic/`, which was wrong and is worth recording as such: it put a board name in the OS policy layer. The audit that followed found the boundary had already been crossed twice (`med-partitions.wks` naming `grub-efi`, `med-image-base.bb` naming `/dev/vda2`), which is why `meta-med-bsp` now exists and why both of those moved into it. Revalidated after the move, not assumed: `make qemu` green, `make check` 21/21, `make stm32` green with `do_image_wic` passing again, the QEMU GPT byte-identical to before the rename, `bitbake -p` clean with zero warnings, and `MED_WKS_FILE` resolving per machine from the new layer (`bitbake -e | grep ^WKS_FILE=`). The precedence claim in that layer's comments was fault-injected rather than reasoned about: `MED_WKS_FILE = ""` from a KAS fragment does override the layer default and does trip the build-time guard, which is what proves the two-step `_DEFAULT` indirection was necessary.

What is still unbuilt or unvalidated: the Tomograph profile (`make tomograph`), `med-image-prod`, booting the QEMU image under `runqemu`, and — on the STM32MP257 — **boot-slot selection**. `bootfs` is shared by both slots and the BSP's `extlinux.conf` hardcodes slot A's PARTUUID, so a device RAUC has marked "boot B" still boots A; closing that needs a U-Boot script reading `BOOT_ORDER`, which is what the `u-boot-env` partition exists for. Nothing has been booted on hardware.

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

The **application's** hardware adaptation surface is one variable: `MED_EEG_DRIVER` (`simulated` on QEMU, `rpmsg` on the STM32MP257) is substituted into `/etc/medplatform/eeg.conf` at build time. No application source differs between the two targets — that is what the portability metric measures, and it is the claim to make.

Do not extend that sentence to the platform: **the platform's adaptation surface is five variables plus one layer.** `MED_EEG_DRIVER`, `MED_EEG_REQUIRE_ENCRYPTION`, `MED_BOOTLOADER` and `MED_DATA_KEY_SOURCE` are set per target in the KAS project files, `MED_WKS_FILE` is answered per machine by `meta-med-bsp`, and that layer is where any further board fact goes. This file used to claim "the hardware adaptation surface is deliberately one variable" flatly, which was false by four variables — a thesis claim, so worth stating precisely. A consequence worth stating too: because those values live in KAS YAML rather than in a layer, the layer stack alone does not build a working STM32 image, and `kas` is load-bearing metadata rather than only an orchestrator.

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
        │  selects a disk layout and a bootloader by variable, names neither
        ▼
meta-med-bsp         (priority 7)  — board adaptation: disk layouts, boot path
        │  drivers, DTS, TF-A, U-Boot/GRUB
        ▼
vendor BSP layer     (priority 6)  — meta-st-stm32mp, meta-yocto-bsp
```

**`meta-med-bsp` is the only custom layer allowed to name a machine, a bootloader or a device node.** That restriction is what makes the reuse claim countable: a new board adds files there and changes nothing above it. The split it enforces is *policy vs. placement* — "two identically-sized slots called med-root-a/med-root-b, and /data is not one of them" is policy and lives in `meta-med-distro`; "TF-A starts at 17 KiB, GRUB reads the ESP, the kernel gets `console=ttyS0`" is placement and lives here. The layer exists because that line was crossed twice without failing a build: `med-partitions.wks` hardcoded `loader=grub-efi` behind a generic filename in the OS policy layer, and `med-image-base.bb` carried `QB_KERNEL_ROOT = "/dev/vda2"` — a virtio path plus a partition index from one particular disk — in a recipe whose own description calls it device-class agnostic. Neither surfaced until a second machine needed a different answer.

- **`meta-med-distro`** (`meta-custom/meta-med-distro/`): OS policy layer. `conf/distro/med-os.conf` sets `INIT_MANAGER = "systemd"`, `usrmerge`, `rauc`. Security/compliance surface: read-only rootfs, RAUC A/B OTA, LUKS, PAM. Kernel policy is injected via the **wildcard** `recipes-kernel/linux/linux-%.bbappend` so distro-level kernel requirements (cgroups, OverlayFS, OpenAMP/rpmsg) apply automatically regardless of which vendor kernel (`linux-yocto`, `linux-st`, etc.) is in use — never target a specific `linux-<vendor>_%.bbappend` here. It has **no `wic/` directory**: `med-os.conf` does `WKS_FILE = "${MED_WKS_FILE}"` (hard `=`, so no machine conf can take it back — poky's `qemux86-64.conf:41` sets `WKS_FILE ?=`) and never names a layout.

- **`meta-med-bsp`** (`meta-custom/meta-med-bsp/`): board adaptation, and today it is `conf/layer.conf` plus two disk layouts — `wic/med-partitions-efi.wks` (GPT dual-boot A/B with encrypted `/data`, for GRUB/EFI machines; this is the file formerly known as `meta-med-distro/wic/med-partitions.wks`) and `wic/med-partitions-stm32mp2.wks.in` (the same A/B policy over the TF-A/FIP/`u-boot-env` chain the STM32MP2 ROM code requires). Selection is a machine override on `MED_WKS_FILE_DEFAULT`, then `MED_WKS_FILE ?= "${MED_WKS_FILE_DEFAULT}"` — the two-step matters, because a bare `MED_WKS_FILE:qemux86-64 =` would beat the unsuffixed assignment a KAS project file makes and silently win over it. `LAYERDEPENDS` is `core` only, and there must never be a bbappend here onto a recipe from a higher layer: this layer provides and does not consume.

- **`meta-med-framework`** (`meta-custom/meta-med-framework/`): `med-framework-api` provides 6 standardized C++ abstraction headers apps must use instead of touching the OS/kernel directly:
  - `MedicalIPC` — OpenAMP `rpmsg` inter-core comms (Cortex-A35 ↔ Cortex-M4) + local IPC sockets
  - `MedicalLogger` — structured, tamper-evident audit logging to `systemd-journald` (IEC 62304 / FDA traceability)
  - `MedicalStorage` — secure persistence to encrypted `/data`
  - `MedicalUpdate` — D-Bus client wrapper around the RAUC OTA daemon
  - `MedicalConfiguration` — calibration tables, operational thresholds, DERS safety-parameter checks
  - `MedicalDevice` — universal sensor/actuator abstraction (AFEs, ADCs)

  Implementation notes: the library is one shared object (`libmedframework.so`, CMake, C++17) whose only external dependencies are `libsystemd` (sd-journal, sd-bus) and `libcrypto` (SHA-256) — everything else is POSIX, which is why it builds unchanged for both targets. `MedicalTypes.h` carries the shared `Status`/`Result<T>` vocabulary; `MedicalDevice.h` also defines the `med::amp::FrameHeader` wire format used both by the rpmsg link and by the service→HMI socket. Built-in device drivers: `simulated` and `rpmsg`.

- **`meta-med-app`** (`meta-custom/meta-med-app/`): pure userspace. Apps: `eeg-acquisition-service` (data daemon, uses `MedicalIPC`/`MedicalLogger`), `eeg-hmi-gui` (Qt6 Quick/Wayland UI). Packagegroups hold **platform capability only, never applications** — `packagegroup-med-core` (framework runtime), `packagegroup-med-amp` (AMP enablement + BSP firmware hook via `MED_AMP_FIRMWARE`), `packagegroup-med-gui` (weston + Qt, no app). Images install the apps: `med-image-eeg.bb`, `med-image-tomograph.bb`, both `require recipes-core/images/med-image-base.bb` across the layer boundary.

- **`kas/`**: `kas-base.yml` is the single source of truth for external repo deps (`poky`, `meta-openembedded`, `meta-rauc`, `meta-qt6`) and the four `meta-med-*` layer paths, plus `local_conf_header` (package format, sstate/downloads dirs, buildstats). Per-target files (`project-eeg-qemu.yml`, `project-eeg-stm32mp2.yml`, `project-tomograph.yml`) set `machine:`, `target:`, the `MED_EEG_*` variables, and any hardware-specific repo (e.g. `meta-st-stm32mp` for the STM32 target). `debug-tweaks` is deliberately *not* set globally — dev images request it themselves so `med-image-prod` stays hardened.

## Rules when editing this codebase

1. **No layer-boundary bleeding**: never put application logic in `meta-med-distro`, and never put BSP/hardware drivers in `meta-med-app` or `meta-med-framework`. Apps must call `Medical*.h` APIs from the framework layer, never raw sysfs/devfs or BSP drivers directly. **A machine name, a bootloader name, a partition offset or a device node (`/dev/vda2`, `mmcblk`, `ttyS0`, `by-partlabel/esp`) appears only in `meta-med-bsp`** — anywhere above it, that is the bug, and note that no build will report it. Both times this rule was broken, the mechanism was a generic-looking name over machine-specific content; when a value differs per board, add it to `meta-med-bsp` keyed by machine override rather than defaulting it upstack.
2. **Scarthgap compatibility**: every `conf/layer.conf` must declare `LAYERSERIES_COMPAT_<collection> = "scarthgap"`. Use modern override syntax (`:`, e.g. `IMAGE_INSTALL:append`), not the legacy `_` syntax.
3. **Kernel changes** belong in `meta-med-distro/recipes-kernel/linux/linux-%.bbappend` (wildcard, not a vendor-specific bbappend) so they stay portable across BSPs.
4. Do not commit build outputs (`build/`, `layers/`, `downloads/`, `sstate-cache/`, `.kas/`) — already covered by `.gitignore`.
5. Security features (read-only rootfs, RAUC A/B, audit logging, LUKS) exist to satisfy IEC 62304 software-partitioning/traceability requirements — treat them as architectural constraints, not optional hardening.
6. **Applications never gain a second dependency.** `eeg-acquisition-service` and `eeg-hmi-gui` link `medframework` and (for the HMI) Qt, and nothing else. If a new feature seems to need a system library in an app recipe's `DEPENDS`, the feature belongs behind a framework API instead — that constraint is the thesis, not a style rule.
7. **`linux-%` also matches non-kernels** (`linux-libc-headers`, `linux-firmware`). The bbappend guards on `bb.data.inherits_class('kernel', d)`; keep that guard when extending it.
8. **Config stores are sealed at build time.** `MedicalConfiguration` refuses to load a store without a matching `.sha256` sidecar, so any recipe shipping one must generate the sidecar after substituting variables (see `do_seal_configuration` in `eeg-acquisition-service_1.0.0.bb`).
