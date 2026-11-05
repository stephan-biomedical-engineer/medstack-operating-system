// SPDX-License-Identifier: MIT
//
// A producer on the far side of a "seqpacket" link, for the host suite.
//
// It speaks the producer protocol the "rpmsg" and "seqpacket" drivers share -
// sample frames out, control messages in, an acknowledgement owed to each -
// over a real AF_UNIX SOCK_SEQPACKET socket, from a thread of its own. Nothing
// in it is mocked below the socket: the driver under test opens, sends,
// receives and decodes exactly as it does against a producer process on a
// target, which is the point of a transport that exists on the host.
//
// What it does is scripted (Script), and what it saw is recorded and read
// after stop(), from the test's thread, never checked from its own.

#pragma once

#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "MedicalDevice.h"
#include "MedicalIPC.h"

namespace medtest {

class FakeProducer {
public:
    struct Script {
        /// Frames sent as soon as the client connects, before any control
        /// message arrives: the backlog a driver has to discard.
        unsigned framesBeforeAck = 0;
        /// Frames sent after each acknowledgement (or after connecting, when
        /// the client sends nothing within `quietBeforeStreaming`).
        unsigned framesAfterAck = 0;
        double rateHz = 250.0;
        std::uint16_t channels = 8;
        std::uint32_t samplesPerChannel = 14;
        /// Value of every sample, in micro-units; and the wire scale to send
        /// them at (nano-units per LSB).
        double sampleValue = 0.0;
        std::int32_t scaleNanoUnitsPerLsb = 1;
        /// Answer to control messages: 0 accepts, otherwise 1 + the index of
        /// the option refused. `refuseKey`, when set, refuses the message that
        /// carries that key and accepts every other one.
        std::uint16_t rejectIndex = 0;
        std::string refuseKey;
        std::string detail;
        /// Never answer a control message at all.
        bool silent = false;
        /// Stop answering after this many acknowledgements (0: never stop).
        unsigned answerAtMost = 0;
        std::chrono::milliseconds quietBeforeStreaming{200};
    };

    FakeProducer(const std::string& path, Script script) : path_(path), script_(script) {
        med::IpcEndpoint endpoint;
        endpoint.transport = med::IpcTransport::UnixSeqpacket;
        endpoint.address = path_;
        med::Result<std::unique_ptr<med::MedicalIpcServer>> server =
            med::MedicalIpcServer::listen(endpoint);
        if (server) {
            server_ = server.take();
            thread_ = std::thread([this] { run(); });
        }
    }

    ~FakeProducer() { stop(); }

    bool listening() const { return static_cast<bool>(server_); }

    void stop() {
        stopping_ = true;
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    /// Every control message received, decoded, in order of arrival.
    std::vector<std::map<std::string, std::string>> controls() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return controls_;
    }

    /// Messages that were neither a decodable control message nor empty.
    unsigned malformed() const { return malformed_; }

    /// When each control message arrived, on the steady clock.
    std::vector<std::chrono::steady_clock::time_point> arrivals() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return arrivals_;
    }

    bool clientClosed() const { return clientClosed_; }

private:
    void sendFrames(med::MedicalIpcChannel& channel, unsigned count) {
        for (unsigned i = 0; i < count; ++i) {
            med::SampleFrame frame;
            frame.sequence = ++sequence_;
            frame.timestamp = med::Clock::now();
            frame.channelCount = script_.channels;
            frame.samplesPerChannel = script_.samplesPerChannel;
            frame.samples.assign(
                static_cast<std::size_t>(script_.channels) * script_.samplesPerChannel,
                script_.sampleValue);
            std::vector<unsigned char> wire;
            if (med::amp::encodeFrame(frame, script_.rateHz, script_.scaleNanoUnitsPerLsb,
                                      wire) != med::Status::Ok) {
                ++malformed_;
                return;
            }
            channel.send(wire.data(), wire.size());
        }
    }

    void run() {
        std::unique_ptr<med::MedicalIpcChannel> channel;
        while (!stopping_ && !channel) {
            med::Result<std::unique_ptr<med::MedicalIpcChannel>> accepted =
                server_->accept(std::chrono::milliseconds(50));
            if (accepted) {
                channel = accepted.take();
            }
        }
        if (!channel) {
            return;
        }

        sendFrames(*channel, script_.framesBeforeAck);

        bool streamed = false;
        unsigned answered = 0;
        const auto connected = std::chrono::steady_clock::now();
        while (!stopping_) {
            unsigned char buffer[2048];
            med::Result<std::size_t> received =
                channel->receive(buffer, sizeof(buffer), std::chrono::milliseconds(20));
            if (!received) {
                if (received.status() == med::Status::Unavailable) {
                    clientClosed_ = true;
                    break;
                }
                if (!streamed &&
                    std::chrono::steady_clock::now() - connected > script_.quietBeforeStreaming) {
                    // A client with nothing to configure: stream anyway.
                    sendFrames(*channel, script_.framesAfterAck);
                    streamed = true;
                }
                continue;
            }
            if (received.value() == 0) {
                clientClosed_ = true;
                break;
            }

            med::Result<std::map<std::string, std::string>> control =
                med::amp::decodeControl(buffer, received.value());
            if (!control) {
                ++malformed_;
                continue;
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                controls_.push_back(control.value());
                arrivals_.push_back(std::chrono::steady_clock::now());
            }

            if (script_.silent ||
                (script_.answerAtMost != 0 && answered >= script_.answerAtMost)) {
                continue;
            }
            std::uint16_t reject = script_.rejectIndex;
            if (!script_.refuseKey.empty()) {
                reject = 0;
                std::uint16_t index = 0;
                for (const auto& option : control.value()) {
                    ++index;
                    if (option.first == script_.refuseKey) {
                        reject = index;
                        break;
                    }
                }
            }
            const std::vector<unsigned char> ack =
                med::amp::encodeControlAck(reject, script_.detail);
            channel->send(ack.data(), ack.size());
            ++answered;
            if (reject == 0) {
                sendFrames(*channel, script_.framesAfterAck);
                streamed = true;
            }
        }
    }

    std::string path_;
    Script script_;
    std::unique_ptr<med::MedicalIpcServer> server_;
    std::thread thread_;
    std::atomic<bool> stopping_{false};
    std::atomic<bool> clientClosed_{false};
    std::atomic<unsigned> malformed_{0};
    std::uint64_t sequence_ = 0;
    mutable std::mutex mutex_;
    std::vector<std::map<std::string, std::string>> controls_;
    std::vector<std::chrono::steady_clock::time_point> arrivals_;
};

}  // namespace medtest
