// SPDX-License-Identifier: MIT

#include "MedicalConfiguration.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <sstream>
#include <string>

#include "MedDigest.h"

namespace med {
namespace {

const char* const kSidecarSuffix = ".sha256";

/// Print a limit value the way it was written in the configuration file, so a
/// violation message reads "not one of {250, 500, 1000}" and not
/// "{250.000000, 500.000000}". A message an operator has to decode is a message
/// that gets skipped.
std::string formatNumber(double value) {
    std::ostringstream out;
    out << value;
    return out.str();
}

std::string trim(const std::string& text) {
    const std::string::size_type first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const std::string::size_type last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

Result<std::string> readWholeFile(const std::string& path) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return Result<std::string>::fail(
            errno == ENOENT ? Status::NotFound : Status::IoError,
            "open " + path + ": " + std::strerror(errno));
    }

    std::string content;
    char buffer[8192];
    for (;;) {
        const ssize_t n = ::read(fd, buffer, sizeof(buffer));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            const int saved = errno;
            ::close(fd);
            return Result<std::string>::fail(Status::IoError, std::strerror(saved));
        }
        if (n == 0) {
            break;
        }
        content.append(buffer, static_cast<std::size_t>(n));
    }
    ::close(fd);
    return Result<std::string>::ok(std::move(content));
}

Status writeFileAtomically(const std::string& path, const std::string& content, mode_t mode) {
    std::string temporary = path + ".tmpXXXXXX";
    const int fd = ::mkstemp(&temporary[0]);
    if (fd < 0) {
        return Status::IoError;
    }

    Status status = Status::Ok;
    std::size_t written = 0;
    while (written < content.size()) {
        const ssize_t n = ::write(fd, content.data() + written, content.size() - written);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            status = Status::IoError;
            break;
        }
        written += static_cast<std::size_t>(n);
    }

    if (status == Status::Ok && ::fchmod(fd, mode) != 0) {
        status = Status::IoError;
    }
    if (status == Status::Ok && ::fsync(fd) != 0) {
        status = Status::IoError;
    }
    ::close(fd);

    if (status != Status::Ok || ::rename(temporary.c_str(), path.c_str()) != 0) {
        ::unlink(temporary.c_str());
        return Status::IoError;
    }
    return Status::Ok;
}

bool parseDouble(const std::string& text, double& out) {
    if (text.empty()) {
        return false;
    }
    errno = 0;
    char* end = nullptr;
    const double value = std::strtod(text.c_str(), &end);
    if (errno != 0 || end == text.c_str() || *end != '\0') {
        return false;
    }
    out = value;
    return true;
}

bool parseInt(const std::string& text, long long& out) {
    if (text.empty()) {
        return false;
    }
    errno = 0;
    char* end = nullptr;
    const long long value = std::strtoll(text.c_str(), &end, 10);
    if (errno != 0 || end == text.c_str() || *end != '\0') {
        return false;
    }
    out = value;
    return true;
}

bool parseBool(const std::string& text, bool& out) {
    if (text == "1" || text == "true" || text == "yes" || text == "on") {
        out = true;
        return true;
    }
    if (text == "0" || text == "false" || text == "no" || text == "off") {
        out = false;
        return true;
    }
    return false;
}

}  // namespace

struct MedicalConfiguration::Impl {
    std::string path;
    // std::map, not unordered_map: serialisation has to be byte-for-byte
    // reproducible or the integrity digest would depend on hash iteration
    // order.
    std::map<std::string, std::string> values;
    bool dirty = false;

    std::string serialise() const {
        std::ostringstream out;
        out << "# MedPlatform configuration store\n"
            << "# Managed by MedicalConfiguration - edit through the API so the\n"
            << "# integrity sidecar stays valid.\n";
        for (const std::pair<const std::string, std::string>& entry : values) {
            out << entry.first << " = " << entry.second << "\n";
        }
        return out.str();
    }

    void parse(const std::string& content) {
        values.clear();
        std::istringstream in(content);
        std::string line;
        while (std::getline(in, line)) {
            const std::string trimmed = trim(line);
            if (trimmed.empty() || trimmed[0] == '#' || trimmed[0] == ';') {
                continue;
            }
            const std::string::size_type separator = trimmed.find('=');
            if (separator == std::string::npos) {
                continue;  // malformed lines are ignored, never guessed at
            }
            const std::string key = trim(trimmed.substr(0, separator));
            const std::string value = trim(trimmed.substr(separator + 1));
            if (!key.empty()) {
                values[key] = value;
            }
        }
    }
};

MedicalConfiguration::MedicalConfiguration(std::string path) : impl_(new Impl()) {
    impl_->path = std::move(path);
}

MedicalConfiguration::~MedicalConfiguration() = default;

Result<std::unique_ptr<MedicalConfiguration>> MedicalConfiguration::load(
    const std::string& path, bool verifyIntegrity) {
    using ConfigResult = Result<std::unique_ptr<MedicalConfiguration>>;

    if (path.empty()) {
        return ConfigResult::fail(Status::InvalidArgument, "empty configuration path");
    }

    Result<std::string> content = readWholeFile(path);
    if (!content) {
        return ConfigResult::fail(content.status(), content.message());
    }

    if (verifyIntegrity) {
        const std::string sidecarPath = path + kSidecarSuffix;
        Result<std::string> sidecar = readWholeFile(sidecarPath);
        if (!sidecar) {
            return ConfigResult::fail(
                Status::IntegrityError,
                "no integrity sidecar at " + sidecarPath +
                    ": the configuration cannot be verified and must not be used "
                    "clinically");
        }

        const std::string expected = trim(sidecar.value()).substr(0, 64);
        const std::string actual = internal::sha256Hex(content.value());
        if (actual.empty() || actual != expected) {
            return ConfigResult::fail(Status::IntegrityError,
                                      "configuration digest mismatch for " + path +
                                          " (expected " + expected + ", got " + actual + ")");
        }
    }

    std::unique_ptr<MedicalConfiguration> configuration(new MedicalConfiguration(path));
    configuration->impl_->parse(content.value());
    return ConfigResult::ok(std::move(configuration));
}

std::unique_ptr<MedicalConfiguration> MedicalConfiguration::empty(const std::string& path) {
    return std::unique_ptr<MedicalConfiguration>(new MedicalConfiguration(path));
}

Result<std::string> MedicalConfiguration::getString(const std::string& key) const {
    const std::map<std::string, std::string>::const_iterator it = impl_->values.find(key);
    if (it == impl_->values.end()) {
        return Result<std::string>::fail(Status::NotFound, "no such key: " + key);
    }
    return Result<std::string>::ok(it->second);
}

Result<double> MedicalConfiguration::getDouble(const std::string& key) const {
    Result<std::string> raw = getString(key);
    if (!raw) {
        return Result<double>::fail(raw.status(), raw.message());
    }
    double value = 0.0;
    if (!parseDouble(raw.value(), value)) {
        return Result<double>::fail(Status::InvalidArgument,
                                    key + " is not a number: " + raw.value());
    }
    return Result<double>::ok(value);
}

Result<long long> MedicalConfiguration::getInt(const std::string& key) const {
    Result<std::string> raw = getString(key);
    if (!raw) {
        return Result<long long>::fail(raw.status(), raw.message());
    }
    long long value = 0;
    if (!parseInt(raw.value(), value)) {
        return Result<long long>::fail(Status::InvalidArgument,
                                       key + " is not an integer: " + raw.value());
    }
    return Result<long long>::ok(value);
}

Result<bool> MedicalConfiguration::getBool(const std::string& key) const {
    Result<std::string> raw = getString(key);
    if (!raw) {
        return Result<bool>::fail(raw.status(), raw.message());
    }
    bool value = false;
    if (!parseBool(raw.value(), value)) {
        return Result<bool>::fail(Status::InvalidArgument,
                                  key + " is not a boolean: " + raw.value());
    }
    return Result<bool>::ok(value);
}

std::string MedicalConfiguration::getString(const std::string& key,
                                            const std::string& fallback) const {
    Result<std::string> value = getString(key);
    return value ? value.value() : fallback;
}

double MedicalConfiguration::getDouble(const std::string& key, double fallback) const {
    Result<double> value = getDouble(key);
    return value ? value.value() : fallback;
}

long long MedicalConfiguration::getInt(const std::string& key, long long fallback) const {
    Result<long long> value = getInt(key);
    return value ? value.value() : fallback;
}

bool MedicalConfiguration::getBool(const std::string& key, bool fallback) const {
    Result<bool> value = getBool(key);
    return value ? value.value() : fallback;
}

Status MedicalConfiguration::set(const std::string& key, const std::string& value) {
    if (key.empty() || key.find_first_of("=\n\r") != std::string::npos) {
        return Status::InvalidArgument;
    }
    if (value.find_first_of("\n\r") != std::string::npos) {
        return Status::InvalidArgument;
    }
    impl_->values[key] = value;
    impl_->dirty = true;
    return Status::Ok;
}

Status MedicalConfiguration::setDouble(const std::string& key, double value) {
    std::ostringstream out;
    out.precision(17);  // round-trips an IEEE-754 double exactly
    out << value;
    return set(key, out.str());
}

Status MedicalConfiguration::setInt(const std::string& key, long long value) {
    return set(key, std::to_string(value));
}

bool MedicalConfiguration::has(const std::string& key) const {
    return impl_->values.find(key) != impl_->values.end();
}

std::vector<std::string> MedicalConfiguration::keys() const {
    std::vector<std::string> out;
    out.reserve(impl_->values.size());
    for (const std::pair<const std::string, std::string>& entry : impl_->values) {
        out.push_back(entry.first);
    }
    return out;
}

bool MedicalConfiguration::dirty() const noexcept {
    return impl_->dirty;
}

Status MedicalConfiguration::commit() {
    if (impl_->path.empty()) {
        return Status::InvalidArgument;
    }

    const std::string content = impl_->serialise();
    const std::string digest = internal::sha256Hex(content);
    if (digest.empty()) {
        return Status::Internal;
    }

    // Order matters: the store first, the sidecar second. An interruption
    // between the two is detected on the next load as an integrity error,
    // which is the safe outcome. The reverse order would leave a sidecar that
    // vouches for content that was never written.
    const Status stored = writeFileAtomically(impl_->path, content, 0640);
    if (stored != Status::Ok) {
        return stored;
    }

    const Status sealed =
        writeFileAtomically(impl_->path + kSidecarSuffix, digest + "\n", 0640);
    if (sealed != Status::Ok) {
        return sealed;
    }

    impl_->dirty = false;
    return Status::Ok;
}

std::string MedicalConfiguration::digest() const {
    return internal::sha256Hex(impl_->serialise());
}

std::vector<SafetyViolation> MedicalConfiguration::validate(
    const std::vector<SafetyLimit>& limits) const {
    std::vector<SafetyViolation> violations;

    for (const SafetyLimit& limit : limits) {
        const std::map<std::string, std::string>::const_iterator it =
            impl_->values.find(limit.key);

        if (it == impl_->values.end()) {
            if (limit.required) {
                SafetyViolation violation;
                violation.key = limit.key;
                violation.reason = "missing required safety parameter" +
                                   (limit.description.empty() ? std::string()
                                                              : " (" + limit.description + ")");
                violation.present = false;
                violations.push_back(violation);
            }
            continue;
        }

        double value = 0.0;
        if (!parseDouble(it->second, value)) {
            SafetyViolation violation;
            violation.key = limit.key;
            violation.reason = "not a number: " + it->second;
            violation.present = true;
            violations.push_back(violation);
            continue;
        }

        SafetyViolation violation;
        violation.key = limit.key;
        violation.value = value;
        violation.present = true;

        const std::string suffix =
            limit.description.empty() ? std::string() : " - " + limit.description;

        if (value < limit.minimum || value > limit.maximum) {
            violation.reason = "outside the permitted range [" +
                               std::to_string(limit.minimum) + ", " +
                               std::to_string(limit.maximum) + "]" + suffix;
            violations.push_back(violation);
            continue;
        }

        if (!limit.allowed.empty()) {
            bool found = false;
            std::string permitted;
            for (const double candidate : limit.allowed) {
                // Exact equality is intended. These are settings a device
                // either offers or does not, written as literals in both the
                // configuration and the limit table, so a tolerance here would
                // only serve to accept a value the hardware cannot take.
                if (candidate == value) {
                    found = true;
                }
                if (!permitted.empty()) {
                    permitted += ", ";
                }
                permitted += formatNumber(candidate);
            }
            if (!found) {
                violation.reason = "not one of the values the front-end offers {" +
                                   permitted + "}" + suffix;
                violations.push_back(violation);
                continue;
            }
        }

        if (limit.multipleOf > 0.0) {
            const double quotient = value / limit.multipleOf;
            const double rounded = (quotient < 0.0) ? -std::floor(-quotient + 0.5)
                                                    : std::floor(quotient + 0.5);
            if (std::fabs(quotient - rounded) > 1e-9) {
                violation.reason = "not a multiple of " +
                                   formatNumber(limit.multipleOf) + suffix;
                violations.push_back(violation);
                continue;
            }
        }
    }

    return violations;
}

const std::string& MedicalConfiguration::path() const noexcept {
    return impl_->path;
}

}  // namespace med
