// SPDX-License-Identifier: MIT
//
// infusion-sim - the producer process: the model and the mechanism behind
// three local sockets.
//
//   command  the actuator protocol (Mechanism): one controller at a time, the
//            framework's MedicalActuator driver "seqpacket" on the other end
//   data     the readback, as sample frames: the framework's MedicalDevice
//            driver "seqpacket" on the other end. Five channels, one sample
//            each, at `frameRateHz`:
//               0 encoder position   mm
//               1 line pressure      mmHg
//               2 air in line        1 (0 or 1)
//               3 motor driven       1 (0 or 1)
//               4 dead-man tripped   1 (0 or 1)
//            at one micro-unit per LSB, so 500 mmHg fits the frame.
//   fault    fault injection, control messages of one key each: occlusion,
//            air, stall (0/1), drift (a fraction), silent (0/1: the producer
//            hangs - no answers, no frames, no dead-man timer). It exists in
//            this simulator only, so it exists in the simulated profile only:
//            nothing on the clinical path has a fault port.
//
// The process is the "producer" of implementation_plan_infusion_pump.md D2:
// it plays the part the real-time core's firmware plays for the EEG, and the
// control service cannot tell it from hardware.

#pragma once

#include <atomic>
#include <string>

namespace infusion {

struct SimulatorConfig {
    std::string commandPath;
    std::string dataPath;
    std::string faultPath;
    std::string syringe = "50ml";
    double frameRateHz = 10.0;
    /// Permission bits of the three sockets. The fault socket is always
    /// 0600 whatever this says: only its owner may break the mechanism.
    unsigned int socketMode = 0660;
};

/// Number of channels in a readback frame, and their units in order.
constexpr unsigned kReadbackChannels = 5;
extern const char* const kReadbackUnits[kReadbackChannels];

/// Run until `stop` is set. Returns 0, or 1 with `*error` set when a socket
/// could not be created or the syringe is unknown.
int runSimulator(const SimulatorConfig& config, const std::atomic<bool>& stop,
                 std::string* error = nullptr);

}  // namespace infusion
