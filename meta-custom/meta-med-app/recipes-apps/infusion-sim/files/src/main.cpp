// SPDX-License-Identifier: MIT
//
// infusion-sim - the simulated mechanism of a syringe pump, as a producer
// process. See simulator.h for what it serves and syringe_model.h for what it
// models; neither is a statement about a real pump.
//
// Usage: infusion-sim [--syringe 10ml|20ml|50ml] [--rate-hz N] DIR
//   listens on DIR/command.sock, DIR/data.sock and DIR/fault.sock.

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "simulator.h"

namespace {

std::atomic<bool> gStop{false};

void onSignal(int) { gStop = true; }

int usage() {
    std::fprintf(stderr, "usage: infusion-sim [--syringe 10ml|20ml|50ml] [--rate-hz N] DIR\n");
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    infusion::SimulatorConfig config;
    std::string dir;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--syringe" && i + 1 < argc) {
            config.syringe = argv[++i];
        } else if (arg == "--rate-hz" && i + 1 < argc) {
            config.frameRateHz = std::atof(argv[++i]);
        } else if (!arg.empty() && arg[0] != '-' && dir.empty()) {
            dir = arg;
        } else {
            return usage();
        }
    }
    if (dir.empty()) {
        return usage();
    }
    config.commandPath = dir + "/command.sock";
    config.dataPath = dir + "/data.sock";
    config.faultPath = dir + "/fault.sock";

    struct sigaction action;
    std::memset(&action, 0, sizeof(action));
    action.sa_handler = onSignal;
    sigaction(SIGTERM, &action, nullptr);
    sigaction(SIGINT, &action, nullptr);

    std::string error;
    const int status = infusion::runSimulator(config, gStop, &error);
    if (status != 0) {
        std::fprintf(stderr, "infusion-sim: %s\n", error.c_str());
    }
    return status;
}
