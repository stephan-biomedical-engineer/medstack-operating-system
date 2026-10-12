// SPDX-License-Identifier: MIT
//
// medctl - ask the acquisition service for something, from a command line.
//
// The same requests the operator display makes, over the same socket, in the
// same message. It exists so that a session can be opened and closed with no
// display at all - by a person at a console, by a bench script, by the
// acceptance suite - and so that everything the display can ask for can be
// asked for, and refused, where the refusal can be read.
//
// It decides nothing. A request is a verb and at most a few words; the
// service answers every one, and this program's exit status is that answer:
// 0 accepted, 1 refused, 2 the request never got an answer.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <medplatform/MedicalDevice.h>
#include <medplatform/MedicalIPC.h>
#include <medplatform/MedicalTypes.h>

namespace {

constexpr const char* kDefaultSocket = "/run/medplatform/eeg-control.sock";

void usage(const char* program) {
    std::fprintf(stderr,
                 "usage: %s [--socket PATH] VERB [KEY=VALUE ...]\n"
                 "\n"
                 "  session.start [operator=NAME] [exam=ID] [subject=ID]\n"
                 "                 begin recording; refused while a session is open\n"
                 "  session.stop   end the session being recorded\n"
                 "  session.status say whether one is open\n"
                 "\n"
                 "exit status: 0 accepted, 1 refused by the service, 2 no answer\n",
                 program);
}

}  // namespace

int main(int argc, char** argv) {
    std::string socketPath = kDefaultSocket;
    if (const char* fromEnvironment = std::getenv("MED_EEG_CONTROL_SOCKET")) {
        socketPath = fromEnvironment;
    }

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
        if (request.count("verb") == 0) {
            request["verb"] = argument == "status" ? "session.status" : argument;
            continue;
        }
        const std::string::size_type equals = argument.find('=');
        if (equals == std::string::npos || equals == 0) {
            std::fprintf(stderr, "medctl: '%s' is not KEY=VALUE\n", argument.c_str());
            return 2;
        }
        const std::string key = argument.substr(0, equals);
        if (key == "verb" || request.count(key) != 0) {
            std::fprintf(stderr, "medctl: '%s' given twice\n", key.c_str());
            return 2;
        }
        request[key] = argument.substr(equals + 1);
    }
    if (request.count("verb") == 0) {
        usage(argv[0]);
        return 2;
    }

    // Refused here, with the limit named, rather than sent short: the wire
    // format has fixed fields and the encoder does not trim.
    const med::Result<std::vector<unsigned char>> encoded = med::amp::encodeControl(request);
    if (!encoded) {
        std::fprintf(stderr, "medctl: %s\n", encoded.message().c_str());
        return 2;
    }

    med::IpcEndpoint endpoint;
    endpoint.transport = med::IpcTransport::UnixSeqpacket;
    endpoint.address = socketPath;
    med::Result<std::unique_ptr<med::MedicalIpcChannel>> channel =
        med::MedicalIpcChannel::connect(endpoint);
    if (!channel) {
        std::fprintf(stderr, "medctl: cannot reach the acquisition service at %s (%s)\n",
                     socketPath.c_str(), med::toString(channel.status()));
        return 2;
    }

    const med::Status sent =
        channel.value()->send(encoded.value().data(), encoded.value().size());
    if (sent != med::Status::Ok) {
        std::fprintf(stderr, "medctl: the request could not be sent (%s)\n", med::toString(sent));
        return 2;
    }

    // The service answers between two sample frames. Three seconds is several
    // of its read timeouts: silence past that is a service that is not looping.
    unsigned char buffer[512];
    const med::Result<std::size_t> received =
        channel.value()->receive(buffer, sizeof(buffer), std::chrono::milliseconds(3000));
    if (!received) {
        std::fprintf(stderr, "medctl: no answer from the acquisition service (%s)\n",
                     med::toString(received.status()));
        return 2;
    }
    const med::Result<med::amp::ControlAck> ack =
        med::amp::decodeControlAck(buffer, received.value());
    if (!ack) {
        std::fprintf(stderr, "medctl: the answer was not an answer: %s\n", ack.message().c_str());
        return 2;
    }

    if (ack.value().rejectedIndex != 0) {
        std::fprintf(stderr, "refused: %s\n", ack.value().detail);
        return 1;
    }
    std::printf("%s\n", ack.value().detail);
    return 0;
}
