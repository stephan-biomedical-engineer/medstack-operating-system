// SPDX-License-Identifier: MIT
//
// MedFramework - field update (OTA) control.
//
// A thin, typed client of the RAUC D-Bus interface (de.pengutronix.rauc,
// object "/", interface de.pengutronix.rauc.Installer) reached over sd-bus on
// the system bus.
//
// Why applications get an API rather than a shell-out to `rauc`: the update of
// a medical device is a regulated activity. The application has to be able to
// tell the operator which software version is running, refuse to start an
// update while an acquisition is in progress, and confirm the new slot as good
// only after its own post-update self test passes. All three need structured
// state, not parsed CLI output.
//
// The A/B mechanics themselves stay in the OS layer: this class never touches
// a block device or a bootloader environment.

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "MedicalTypes.h"

namespace med {

enum class UpdateOperation {
    Idle,
    Installing,
    Unknown,
};

const char* toString(UpdateOperation operation) noexcept;

struct SlotStatus {
    std::string name;        ///< "rootfs.0"
    std::string slotClass;   ///< "rootfs"
    std::string device;      ///< "/dev/disk/by-partlabel/med-root-a"
    std::string bootName;    ///< "A" / "B"
    std::string state;       ///< "booted" / "inactive"
    std::string bootStatus;  ///< "good" / "bad"
    std::string sha256;      ///< digest of the installed slot content
    std::string bundleVersion;
    bool booted = false;
};

struct UpdateProgress {
    int percentage = 0;
    std::string message;
    int nestingDepth = 0;
};

class MedicalUpdate {
public:
    ~MedicalUpdate();

    /// Connect to the system bus and resolve the RAUC service.
    /// Unavailable when the daemon is not running - a valid state on a
    /// development image, and the caller is expected to degrade rather than
    /// fail.
    static Result<std::unique_ptr<MedicalUpdate>> connect();

    /// Tell the service manager that this program is now doing its job.
    ///
    /// This - not markBootedGood() - is how an application takes part in
    /// confirming an update. A unit that declares Type=notify and is required
    /// by boot-complete.target holds that target until it reports ready, and
    /// the OS marks the booted slot good only once the target is reached. A
    /// program that never gets this far leaves the slot unconfirmed, and the
    /// bootloader falls back after its attempts run out. Keeping the call this
    /// narrow keeps one writer of the bootloader's counters: the OS.
    ///
    /// Static, and needs no D-Bus or RAUC: readiness is owed to the service
    /// manager whether an update daemon exists or not. Unavailable when the
    /// process was not started with a notification socket (run by hand, or
    /// from a unit that is not Type=notify).
    static Status reportReady();

    /// Compatible string the running image was built with; a bundle whose
    /// compatible differs is rejected by RAUC before anything is written.
    Result<std::string> compatible() const;

    /// Boot slot in use, e.g. "A".
    Result<std::string> bootSlot() const;

    Result<UpdateOperation> operation() const;
    Result<UpdateProgress> progress() const;
    Result<std::string> lastError() const;
    Result<std::vector<SlotStatus>> slots() const;

    /// Start installing a bundle. Returns as soon as RAUC accepted the
    /// request; the install itself is asynchronous - poll progress() or
    /// operation(). The bundle signature is verified by the daemon against
    /// /etc/rauc/keyring.pem; an unsigned or untrusted bundle never reaches
    /// the inactive slot.
    Status install(const std::string& bundlePath);

    /// Confirm the currently booted slot after a successful post-update self
    /// test. Until this is called, the bootloader still counts boot attempts
    /// and will fall back to the previous slot.
    Result<std::string> markBootedGood();

    /// Mark the booted slot bad and request a fallback on the next boot.
    Result<std::string> markBootedBad();

    MedicalUpdate(const MedicalUpdate&) = delete;
    MedicalUpdate& operator=(const MedicalUpdate&) = delete;

private:
    struct Impl;
    explicit MedicalUpdate(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl_;
};

}  // namespace med
