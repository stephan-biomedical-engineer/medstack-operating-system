// SPDX-License-Identifier: MIT

#include "library.h"

#include <set>
#include <sstream>

namespace infusion {

namespace {

std::vector<std::string> splitList(const std::string& text) {
    std::vector<std::string> out;
    std::string item;
    std::istringstream in(text);
    while (std::getline(in, item, ',')) {
        const std::size_t a = item.find_first_not_of(" \t");
        const std::size_t b = item.find_last_not_of(" \t");
        out.push_back(a == std::string::npos ? std::string() : item.substr(a, b - a + 1));
    }
    return out;
}

bool validId(const std::string& id) {
    if (id.empty() || id.size() > 24) {
        return false;
    }
    for (const char c : id) {
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) {
            return false;
        }
    }
    return true;
}

}  // namespace

std::vector<med::SafetyLimit> serviceLimits() {
    return {
        {"service.heartbeat_ms", 50, 1000, true, "controller heartbeat period", {}, 0},
        {"service.missed_heartbeats", 1, 10, true, "heartbeats missed before the mechanism rests", {}, 1},
        {"service.producer_timeout_ms", 100, 5000, true, "readback silence before the mechanism is lost", {}, 0},
        {"alarm.occlusion_mmhg", 100, 900, true, "occlusion pressure limit", {}, 0},
        {"alarm.delivery_tolerance", 0.01, 0.20, true, "commanded vs measured flow, fraction", {}, 0},
        {"alarm.delivery_window_ms", 500, 10000, true, "window over which flow is judged", {}, 0},
        {"alarm.silence_max_s", 10, 120, true, "longest silence of an alarm", {}, 0},
        {"mechanism.max_rate_ml_h", 1, 1200, true, "the mechanism's highest rate", {}, 0},
        {"mechanism.frame_rate_hz", 1, 100, true, "readback frames per second", {}, 0},
    };
}

med::Result<ServiceConfig> loadServiceConfig(const med::MedicalConfiguration& config,
                                             std::vector<std::string>* problems) {
    std::vector<std::string> found;
    for (const med::SafetyViolation& v : config.validate(serviceLimits())) {
        found.push_back(v.key + ": " + v.reason);
    }

    ServiceConfig out;
    Parameters& p = out.parameters;
    const long long heartbeat = config.getInt("service.heartbeat_ms", 200);
    const long long missed = config.getInt("service.missed_heartbeats", 3);
    p.heartbeatPeriod = std::chrono::milliseconds(heartbeat);
    out.missedHeartbeats = static_cast<std::uint32_t>(missed > 0 ? missed : 0);
    p.producerTimeout = std::chrono::milliseconds(config.getInt("service.producer_timeout_ms", 600));
    p.occlusionMmHg = config.getDouble("alarm.occlusion_mmhg", 500.0);
    p.deliveryTolerance = config.getDouble("alarm.delivery_tolerance", 0.05);
    p.deliveryWindow = std::chrono::milliseconds(config.getInt("alarm.delivery_window_ms", 2000));
    p.silenceMax = std::chrono::milliseconds(config.getInt("alarm.silence_max_s", 120) * 1000);
    p.maxRateMlH = config.getDouble("mechanism.max_rate_ml_h", 1200.0);
    out.frameRateHz = config.getDouble("mechanism.frame_rate_hz", 10.0);

    // What one parameter demands of another - the application's knowledge,
    // which SafetyLimit deliberately does not carry.
    if (heartbeat * missed > 2000) {
        found.push_back("service.heartbeat_ms x service.missed_heartbeats: the dead-man window "
                        "would exceed 2000 ms");
    }
    if (out.frameRateHz > 0.0 &&
        static_cast<double>(p.producerTimeout.count()) < 2000.0 / out.frameRateHz) {
        found.push_back("service.producer_timeout_ms: shorter than two readback periods");
    }

    for (const char* key : {"mechanism.command", "mechanism.data", "control.socket",
                            "control.status", "control.therapy_marker"}) {
        if (config.getString(key, "").empty()) {
            found.push_back(std::string(key) + ": required");
        }
    }
    out.commandSocket = config.getString("mechanism.command", "");
    out.dataSocket = config.getString("mechanism.data", "");
    out.controlSocket = config.getString("control.socket", "");
    out.statusPath = config.getString("control.status", "");
    out.therapyMarker = config.getString("control.therapy_marker", "");

    // Syringes.
    std::set<std::string> seen;
    const std::vector<std::string> syringes = splitList(config.getString("syringes", ""));
    if (syringes.empty()) {
        found.push_back("syringes: the library names no syringe");
    }
    for (const std::string& id : syringes) {
        if (!validId(id) || !seen.insert("syringe:" + id).second) {
            found.push_back("syringes: '" + id + "' is not a valid, unique id");
            continue;
        }
        SyringeSpec s;
        s.id = id;
        const std::string base = "syringe." + id + ".";
        std::vector<med::SafetyLimit> limits = {
            {base + "volume_ml", 1, 100, true, "syringe volume", {}, 0},
            {base + "area_mm2", 10, 2000, true, "barrel cross-section", {}, 0},
        };
        for (const med::SafetyViolation& v : config.validate(limits)) {
            found.push_back(v.key + ": " + v.reason);
        }
        s.volumeMl = config.getDouble(base + "volume_ml", 0.0);
        s.areaMm2 = config.getDouble(base + "area_mm2", 0.0);
        out.library.syringes.push_back(s);
    }

    // Drugs.
    const std::vector<std::string> drugs = splitList(config.getString("drugs", ""));
    if (drugs.empty()) {
        found.push_back("drugs: the library names no drug");
    }
    for (const std::string& id : drugs) {
        if (!validId(id) || !seen.insert("drug:" + id).second) {
            found.push_back("drugs: '" + id + "' is not a valid, unique id");
            continue;
        }
        Drug d;
        d.id = id;
        const std::string base = "drug." + id + ".";
        d.name = config.getString(base + "name", "");
        if (d.name.empty()) {
            found.push_back(base + "name: required");
        }
        std::vector<med::SafetyLimit> limits = {
            {base + "soft_max_ml_h", 0.1, p.maxRateMlH, true, "soft rate limit", {}, 0},
            {base + "hard_max_ml_h", 0.1, p.maxRateMlH, true, "hard rate limit", {}, 0},
        };
        for (const med::SafetyViolation& v : config.validate(limits)) {
            found.push_back(v.key + ": " + v.reason);
        }
        d.softMaxMlH = config.getDouble(base + "soft_max_ml_h", 0.0);
        d.hardMaxMlH = config.getDouble(base + "hard_max_ml_h", 0.0);
        if (d.softMaxMlH > d.hardMaxMlH) {
            found.push_back(base + "soft_max_ml_h: above the hard limit");
        }
        out.library.drugs.push_back(d);
    }

    if (problems != nullptr) {
        *problems = found;
    }
    if (!found.empty()) {
        return med::Result<ServiceConfig>::fail(med::Status::InvalidArgument, found.front());
    }
    return med::Result<ServiceConfig>::ok(out);
}

}  // namespace infusion
