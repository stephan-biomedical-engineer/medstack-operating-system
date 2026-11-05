// SPDX-License-Identifier: MIT
//
// infusion-ctl - ask the infusion control service for something, from a
// command line: the same requests the display will make, over the same socket,
// in the same message. It exists so that a therapy can be programmed, started,
// paused and stopped, and an alarm acknowledged, with no display - by a person
// at a console, by a bench script, by the acceptance suite.
//
// It decides nothing. Its exit status is the service's answer: 0 accepted,
// 1 refused, 2 the request never got an answer. The answer's text goes to
// stdout; the full state is in the service's status file.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>

#include <medplatform/MedicalDevice.h>
#include <medplatform/MedicalIPC.h>

namespace {

constexpr const char* kDefaultSocket = "/run/medplatform/infusion-control.sock";

void usage(const char* program) {
    std::fprintf(stderr,
                 "usage: %s [--socket PATH] VERB [KEY=VALUE ...]\n"
                 "\n"
                 "  program drug=ID syringe=ID rate_ml_h=N vtbi_ml=N [confirm=1 operator=NAME]\n"
                 "  start | pause | resume | stop | status\n"
                 "  acknowledge operator=NAME | silence operator=NAME\n"
                 "\n"
                 "exit status: 0 accepted, 1 refused by the service, 2 no answer\n",
                 program);
}

}  // namespace

int main(int argc, char** argv) {
    std::string socketPath = kDefaultSocket;
    std::map<std::string, std::string> request;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--help" || argument == "-h") {
            usage(argv[0]);
            return 0;
        }
        if (argument == "--socket" && i + 1 < argc) {
            socketPath = argv[++i];
            continue;
        }
        const std::size_t equals = argument.find('=');
        if (equals == std::string::npos) {
            if (request.count("verb") != 0) {
                usage(argv[0]);
                return 2;
            }
            request["verb"] = argument;
        } else {
            request[argument.substr(0, equals)] = argument.substr(equals + 1);
        }
    }
    if (request.count("verb") == 0) {
        usage(argv[0]);
        return 2;
    }

    auto message = med::amp::encodeControl(request);
    if (!message) {
        std::fprintf(stderr, "infusion-ctl: %s\n", message.message().c_str());
        return 2;
    }
    med::IpcEndpoint endpoint;
    endpoint.transport = med::IpcTransport::UnixSeqpacket;
    endpoint.address = socketPath;
    auto channel = med::MedicalIpcChannel::connect(endpoint);
    if (!channel) {
        std::fprintf(stderr, "infusion-ctl: %s: %s\n", socketPath.c_str(), channel.message().c_str());
        return 2;
    }
    if (channel.value()->send(message.value().data(), message.value().size()) != med::Status::Ok) {
        std::fprintf(stderr, "infusion-ctl: the request could not be sent\n");
        return 2;
    }
    // Long enough for an acknowledgement that reconnects a lost mechanism: the
    // framework's connection window is five seconds, its self test two more.
    unsigned char buffer[256];
    auto received = channel.value()->receive(buffer, sizeof(buffer), std::chrono::milliseconds(10000));
    if (!received) {
        std::fprintf(stderr, "infusion-ctl: no answer: %s\n", received.message().c_str());
        return 2;
    }
    auto ack = med::amp::decodeControlAck(buffer, received.value());
    if (!ack) {
        std::fprintf(stderr, "infusion-ctl: %s\n", ack.message().c_str());
        return 2;
    }
    std::printf("%s\n", ack.value().detail);
    return ack.value().rejectedIndex == 0 ? 0 : 1;
}
