// SPDX-License-Identifier: MIT
//
// infusion-control-service - its sealed configuration: the controller's
// parameters, the drug library and the syringes, read through
// MedicalConfiguration and judged by SafetyLimit before anything starts.
//
// A configuration that fails here never reaches the controller (INF-03,
// INF-16): the service refuses to start, the same as the EEG with a
// calibration it cannot vouch for. Every violation is reported, not the first.

#pragma once

#include <string>
#include <vector>

#include <medplatform/MedicalConfiguration.h>
#include <medplatform/MedicalTypes.h>

#include "controller.h"

namespace infusion {

struct ServiceConfig {
    Parameters parameters;
    Library library;
    std::uint32_t missedHeartbeats = 3;
    double frameRateHz = 10.0;
    std::string commandSocket;
    std::string dataSocket;
    std::string controlSocket;
    std::string statusPath;
    std::string therapyMarker;
};

/// The limits every infusion.conf is held to. Exposed so a test can see them.
std::vector<med::SafetyLimit> serviceLimits();

/// InvalidArgument with every problem in `*problems` when the store is not
/// usable; a store that is usable gives no problems.
med::Result<ServiceConfig> loadServiceConfig(const med::MedicalConfiguration& config,
                                             std::vector<std::string>* problems);

}  // namespace infusion
