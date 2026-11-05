// SPDX-License-Identifier: MIT
//
// infusion-sim-fault - inject a fault into the simulated mechanism.
//
//   infusion-sim-fault [--socket PATH] KEY VALUE
//     occlusion|air|stall|silent 0|1, drift FRACTION
//
// Part of the simulator's package, so it exists wherever the simulator does and
// nowhere else: the clinical path has no fault port. Exit status 0 when the
// simulator took the fault, 1 when it refused it, 2 when it did not answer.

#include <chrono>
#include <cstdio>
#include <string>

#include <medplatform/MedicalDevice.h>
#include <medplatform/MedicalIPC.h>

int main(int argc, char** argv) {
    std::string socketPath = "/run/infusion-sim/fault.sock";
    int first = 1;
    if (argc > 2 && std::string(argv[1]) == "--socket") {
        socketPath = argv[2];
        first = 3;
    }
    if (argc - first != 2) {
        std::fprintf(stderr, "usage: %s [--socket PATH] KEY VALUE\n", argv[0]);
        return 2;
    }
    auto message = med::amp::encodeControl({{argv[first], argv[first + 1]}});
    med::IpcEndpoint endpoint;
    endpoint.transport = med::IpcTransport::UnixSeqpacket;
    endpoint.address = socketPath;
    auto channel = med::MedicalIpcChannel::connect(endpoint);
    if (!message || !channel ||
        channel.value()->send(message.value().data(), message.value().size()) != med::Status::Ok) {
        std::fprintf(stderr, "infusion-sim-fault: %s unreachable\n", socketPath.c_str());
        return 2;
    }
    unsigned char buffer[256];
    auto received = channel.value()->receive(buffer, sizeof(buffer), std::chrono::milliseconds(2000));
    auto ack = received ? med::amp::decodeControlAck(buffer, received.value())
                        : med::Result<med::amp::ControlAck>::fail(received.status());
    if (!ack) {
        std::fprintf(stderr, "infusion-sim-fault: no answer\n");
        return 2;
    }
    if (ack.value().rejectedIndex != 0) {
        std::fprintf(stderr, "infusion-sim-fault: refused: %s\n", ack.value().detail);
        return 1;
    }
    return 0;
}
