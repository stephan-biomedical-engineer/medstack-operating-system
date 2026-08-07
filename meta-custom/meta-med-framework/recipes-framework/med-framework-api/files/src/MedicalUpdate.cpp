// SPDX-License-Identifier: MIT

#include "MedicalUpdate.h"

#include <systemd/sd-bus.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>

namespace med {
namespace {

// systemd spells the empty error as SD_BUS_ERROR_NULL, a C compound literal
// that is only available in C++ as a compiler extension. Value initialisation
// produces the identical {NULL, NULL, 0} without leaving standard C++.
const char* const kService = "de.pengutronix.rauc";
const char* const kObject = "/";
const char* const kInterface = "de.pengutronix.rauc.Installer";

Status statusFromBus(int rc) {
    switch (-rc) {
        case ENOENT:
        case ENOSYS:
        case EHOSTUNREACH:
        case ECONNREFUSED:
            return Status::Unavailable;
        case EACCES:
        case EPERM:
            return Status::PermissionDenied;
        case EINVAL:
            return Status::InvalidArgument;
        case ETIMEDOUT:
            return Status::Timeout;
        default:
            return Status::IoError;
    }
}

std::string describe(const sd_bus_error& error, int rc) {
    if (error.message != nullptr) {
        return error.message;
    }
    return std::strerror(rc < 0 ? -rc : rc);
}

/// Read a variant whose contents we do not know ahead of time and render it as
/// text. RAUC slot dictionaries mix strings, booleans and integers, and the
/// application only ever displays or logs them.
std::string readVariantAsString(sd_bus_message* message) {
    char type = 0;
    const char* contents = nullptr;
    if (sd_bus_message_peek_type(message, &type, &contents) <= 0) {
        return {};
    }

    switch (type) {
        case SD_BUS_TYPE_STRING:
        case SD_BUS_TYPE_OBJECT_PATH:
        case SD_BUS_TYPE_SIGNATURE: {
            const char* value = nullptr;
            if (sd_bus_message_read_basic(message, type, &value) < 0 || value == nullptr) {
                return {};
            }
            return value;
        }
        case SD_BUS_TYPE_BOOLEAN: {
            int value = 0;
            if (sd_bus_message_read_basic(message, type, &value) < 0) {
                return {};
            }
            return value != 0 ? "true" : "false";
        }
        case SD_BUS_TYPE_UINT32: {
            std::uint32_t value = 0;
            if (sd_bus_message_read_basic(message, type, &value) < 0) {
                return {};
            }
            return std::to_string(value);
        }
        case SD_BUS_TYPE_INT32: {
            std::int32_t value = 0;
            if (sd_bus_message_read_basic(message, type, &value) < 0) {
                return {};
            }
            return std::to_string(value);
        }
        case SD_BUS_TYPE_UINT64: {
            std::uint64_t value = 0;
            if (sd_bus_message_read_basic(message, type, &value) < 0) {
                return {};
            }
            return std::to_string(value);
        }
        case SD_BUS_TYPE_INT64: {
            std::int64_t value = 0;
            if (sd_bus_message_read_basic(message, type, &value) < 0) {
                return {};
            }
            return std::to_string(value);
        }
        default:
            sd_bus_message_skip(message, nullptr);
            return {};
    }
}

}  // namespace

const char* toString(UpdateOperation operation) noexcept {
    switch (operation) {
        case UpdateOperation::Idle:       return "idle";
        case UpdateOperation::Installing: return "installing";
        case UpdateOperation::Unknown:    return "unknown";
    }
    return "unknown";
}

struct MedicalUpdate::Impl {
    sd_bus* bus = nullptr;

    ~Impl() {
        if (bus != nullptr) {
            sd_bus_unref(bus);
        }
    }

    Result<std::string> stringProperty(const char* name) const {
        sd_bus_error error = {};
        char* value = nullptr;

        const int rc = sd_bus_get_property_string(bus, kService, kObject, kInterface, name,
                                                  &error, &value);
        if (rc < 0) {
            const std::string message = describe(error, rc);
            sd_bus_error_free(&error);
            return Result<std::string>::fail(statusFromBus(rc),
                                             std::string(name) + ": " + message);
        }

        std::string out = (value != nullptr) ? value : "";
        std::free(value);
        sd_bus_error_free(&error);
        return Result<std::string>::ok(std::move(out));
    }
};

MedicalUpdate::MedicalUpdate(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

MedicalUpdate::~MedicalUpdate() = default;

Result<std::unique_ptr<MedicalUpdate>> MedicalUpdate::connect() {
    using UpdateResult = Result<std::unique_ptr<MedicalUpdate>>;

    std::unique_ptr<Impl> impl(new Impl());

    const int rc = sd_bus_open_system(&impl->bus);
    if (rc < 0) {
        return UpdateResult::fail(statusFromBus(rc),
                                  std::string("cannot reach the system bus: ") +
                                      std::strerror(-rc));
    }

    // Probe one property so that "connected" means "RAUC is actually there",
    // not merely "a bus exists". Callers use this to decide whether the update
    // feature is available at all.
    std::unique_ptr<MedicalUpdate> update(new MedicalUpdate(std::move(impl)));
    Result<std::string> probe = update->impl_->stringProperty("Compatible");
    if (!probe) {
        return UpdateResult::fail(Status::Unavailable,
                                  "the RAUC service is not available: " + probe.message());
    }

    return UpdateResult::ok(std::move(update));
}

Result<std::string> MedicalUpdate::compatible() const {
    return impl_->stringProperty("Compatible");
}

Result<std::string> MedicalUpdate::bootSlot() const {
    return impl_->stringProperty("BootSlot");
}

Result<std::string> MedicalUpdate::lastError() const {
    return impl_->stringProperty("LastError");
}

Result<UpdateOperation> MedicalUpdate::operation() const {
    Result<std::string> raw = impl_->stringProperty("Operation");
    if (!raw) {
        return Result<UpdateOperation>::fail(raw.status(), raw.message());
    }
    if (raw.value() == "idle") {
        return Result<UpdateOperation>::ok(UpdateOperation::Idle);
    }
    if (raw.value() == "installing") {
        return Result<UpdateOperation>::ok(UpdateOperation::Installing);
    }
    return Result<UpdateOperation>::ok(UpdateOperation::Unknown);
}

Result<UpdateProgress> MedicalUpdate::progress() const {
    sd_bus_error error = {};
    sd_bus_message* reply = nullptr;

    int rc = sd_bus_get_property(impl_->bus, kService, kObject, kInterface, "Progress",
                                 &error, &reply, "(isi)");
    if (rc < 0) {
        const std::string message = describe(error, rc);
        sd_bus_error_free(&error);
        return Result<UpdateProgress>::fail(statusFromBus(rc), "Progress: " + message);
    }

    std::int32_t percentage = 0;
    const char* text = nullptr;
    std::int32_t nesting = 0;
    rc = sd_bus_message_read(reply, "(isi)", &percentage, &text, &nesting);
    if (rc < 0) {
        sd_bus_message_unref(reply);
        sd_bus_error_free(&error);
        return Result<UpdateProgress>::fail(Status::IoError, "malformed Progress property");
    }

    UpdateProgress progress;
    progress.percentage = percentage;
    progress.message = (text != nullptr) ? text : "";
    progress.nestingDepth = nesting;

    sd_bus_message_unref(reply);
    sd_bus_error_free(&error);
    return Result<UpdateProgress>::ok(std::move(progress));
}

Result<std::vector<SlotStatus>> MedicalUpdate::slots() const {
    using SlotsResult = Result<std::vector<SlotStatus>>;

    sd_bus_error error = {};
    sd_bus_message* reply = nullptr;

    int rc = sd_bus_call_method(impl_->bus, kService, kObject, kInterface, "GetSlotStatus",
                                &error, &reply, "");
    if (rc < 0) {
        const std::string message = describe(error, rc);
        sd_bus_error_free(&error);
        return SlotsResult::fail(statusFromBus(rc), "GetSlotStatus: " + message);
    }

    std::vector<SlotStatus> slots;

    rc = sd_bus_message_enter_container(reply, SD_BUS_TYPE_ARRAY, "(sa{sv})");
    if (rc < 0) {
        sd_bus_message_unref(reply);
        sd_bus_error_free(&error);
        return SlotsResult::fail(Status::IoError, "malformed GetSlotStatus reply");
    }

    while (sd_bus_message_enter_container(reply, SD_BUS_TYPE_STRUCT, "sa{sv}") > 0) {
        const char* name = nullptr;
        if (sd_bus_message_read(reply, "s", &name) < 0) {
            break;
        }

        SlotStatus slot;
        slot.name = (name != nullptr) ? name : "";

        if (sd_bus_message_enter_container(reply, SD_BUS_TYPE_ARRAY, "{sv}") > 0) {
            while (sd_bus_message_enter_container(reply, SD_BUS_TYPE_DICT_ENTRY, "sv") > 0) {
                const char* key = nullptr;
                if (sd_bus_message_read(reply, "s", &key) < 0 || key == nullptr) {
                    sd_bus_message_exit_container(reply);
                    break;
                }

                char variantType = 0;
                const char* variantContents = nullptr;
                (void)sd_bus_message_peek_type(reply, &variantType, &variantContents);
                (void)variantType;
                if (sd_bus_message_enter_container(reply, SD_BUS_TYPE_VARIANT,
                                                   variantContents) > 0) {
                    const std::string value = readVariantAsString(reply);
                    const std::string field = key;

                    if (field == "class") {
                        slot.slotClass = value;
                    } else if (field == "device") {
                        slot.device = value;
                    } else if (field == "bootname") {
                        slot.bootName = value;
                    } else if (field == "state") {
                        slot.state = value;
                        slot.booted = (value == "booted");
                    } else if (field == "boot-status") {
                        slot.bootStatus = value;
                    } else if (field == "sha256") {
                        slot.sha256 = value;
                    } else if (field == "bundle.version") {
                        slot.bundleVersion = value;
                    }

                    sd_bus_message_exit_container(reply);  // variant
                }
                sd_bus_message_exit_container(reply);  // dict entry
            }
            sd_bus_message_exit_container(reply);  // a{sv}
        }

        slots.push_back(slot);
        sd_bus_message_exit_container(reply);  // struct
    }

    sd_bus_message_exit_container(reply);  // array
    sd_bus_message_unref(reply);
    sd_bus_error_free(&error);

    return SlotsResult::ok(std::move(slots));
}

Status MedicalUpdate::install(const std::string& bundlePath) {
    if (bundlePath.empty() || bundlePath.front() != '/') {
        return Status::InvalidArgument;
    }

    sd_bus_error error = {};
    sd_bus_message* reply = nullptr;

    // InstallBundle(s, a{sv}) is the current entry point; the trailing 0 is the
    // number of option entries. Older RAUC only has Install(s), so fall back
    // rather than making the framework require a specific daemon version.
    int rc = sd_bus_call_method(impl_->bus, kService, kObject, kInterface, "InstallBundle",
                                &error, &reply, "sa{sv}", bundlePath.c_str(), 0);

    if (rc < 0 && sd_bus_error_has_name(&error, SD_BUS_ERROR_UNKNOWN_METHOD)) {
        // sd_bus_error_free() resets the struct, so it is reusable as-is.
        sd_bus_error_free(&error);
        rc = sd_bus_call_method(impl_->bus, kService, kObject, kInterface, "Install", &error,
                                &reply, "s", bundlePath.c_str());
    }

    const Status status = (rc < 0) ? statusFromBus(rc) : Status::Ok;

    if (reply != nullptr) {
        sd_bus_message_unref(reply);
    }
    sd_bus_error_free(&error);
    return status;
}

Result<std::string> MedicalUpdate::markBootedGood() {
    sd_bus_error error = {};
    sd_bus_message* reply = nullptr;

    int rc = sd_bus_call_method(impl_->bus, kService, kObject, kInterface, "Mark", &error,
                                &reply, "ss", "good", "booted");
    if (rc < 0) {
        const std::string message = describe(error, rc);
        sd_bus_error_free(&error);
        return Result<std::string>::fail(statusFromBus(rc), "Mark good: " + message);
    }

    const char* slotName = nullptr;
    const char* text = nullptr;
    rc = sd_bus_message_read(reply, "ss", &slotName, &text);
    const std::string marked = (rc >= 0 && slotName != nullptr) ? slotName : "";

    sd_bus_message_unref(reply);
    sd_bus_error_free(&error);
    return Result<std::string>::ok(marked);
}

Result<std::string> MedicalUpdate::markBootedBad() {
    sd_bus_error error = {};
    sd_bus_message* reply = nullptr;

    int rc = sd_bus_call_method(impl_->bus, kService, kObject, kInterface, "Mark", &error,
                                &reply, "ss", "bad", "booted");
    if (rc < 0) {
        const std::string message = describe(error, rc);
        sd_bus_error_free(&error);
        return Result<std::string>::fail(statusFromBus(rc), "Mark bad: " + message);
    }

    const char* slotName = nullptr;
    const char* text = nullptr;
    rc = sd_bus_message_read(reply, "ss", &slotName, &text);
    const std::string marked = (rc >= 0 && slotName != nullptr) ? slotName : "";

    sd_bus_message_unref(reply);
    sd_bus_error_free(&error);
    return Result<std::string>::ok(marked);
}

}  // namespace med
