// SPDX-License-Identifier: MIT
//
// MedFramework - inter-process and inter-core communication.
//
// Two transports behind one interface:
//
//   * RpmsgChar     - OpenAMP rpmsg character device (/dev/rpmsg*), the link
//                     to a real-time co-processor (Cortex-A35 <-> Cortex-M4 on
//                     the STM32MP257). Message oriented: one write is one
//                     message, and a read returns exactly one message.
//   * UnixSeqpacket - AF_UNIX SOCK_SEQPACKET, the link between medical
//                     services on the Linux side (acquisition daemon -> HMI).
//                     Also message oriented, which is why it is the local
//                     analogue of rpmsg: application code written against one
//                     works against the other unchanged.
//
// That symmetry is the point. On QEMU there is no second core, so the same
// application talks to a simulated source over a socket; on hardware it talks
// to firmware over rpmsg. Neither case requires application changes, which is
// what the portability claim of the thesis rests on.

#pragma once

#include <chrono>
#include <cstddef>
#include <memory>
#include <string>

#include "MedicalTypes.h"

namespace med {

enum class IpcTransport {
    RpmsgChar,
    UnixSeqpacket,
};

struct IpcEndpoint {
    IpcTransport transport = IpcTransport::UnixSeqpacket;

    /// Device node for RpmsgChar ("/dev/rpmsg0"), filesystem socket path for
    /// UnixSeqpacket ("/run/medplatform/<service>.sock").
    std::string address;

    /// Permission bits applied to a listening UNIX socket. Default 0660: the
    /// owning service and its group, nobody else.
    unsigned int socketMode = 0660;
};

/// A bidirectional, message oriented channel. Not thread safe: give each
/// thread its own channel, or serialise access externally.
class MedicalIpcChannel {
public:
    ~MedicalIpcChannel();

    /// Open an rpmsg endpoint, or connect to a listening UNIX socket.
    static Result<std::unique_ptr<MedicalIpcChannel>> connect(const IpcEndpoint& endpoint);

    /// Send exactly one message. Partial sends are reported as IoError rather
    /// than silently truncating - a truncated frame from an acquisition
    /// front-end is corrupt data, and corrupt data must never look like data.
    ///
    /// Returns **WouldBlock** when the peer is not draining fast enough. A
    /// channel obtained from MedicalIpcServer::accept() is non-blocking
    /// precisely so this can happen: a consumer must never be able to slow its
    /// producer down, because on this platform the producer is a real-time
    /// acquisition loop and the consumer is a display. Callers are expected to
    /// treat WouldBlock as "this peer missed this message" - counted and
    /// reported - and to keep the peer, which is present and merely slow.
    /// Every other non-Ok status means the peer is unusable and should be
    /// dropped.
    Status send(const void* data, std::size_t length);

    /// Receive one message, waiting up to `timeout`. A zero timeout polls.
    /// Returns Timeout (not an error state) when nothing arrived, and
    /// Unavailable when the peer closed the channel.
    Result<std::size_t> receive(void* buffer, std::size_t capacity,
                                std::chrono::milliseconds timeout);

    /// Raw descriptor, for callers that run their own event loop
    /// (QSocketNotifier, sd_event, poll). Ownership stays with the channel.
    int descriptor() const noexcept;

    IpcTransport transport() const noexcept;

    void close() noexcept;

    MedicalIpcChannel(const MedicalIpcChannel&) = delete;
    MedicalIpcChannel& operator=(const MedicalIpcChannel&) = delete;

private:
    MedicalIpcChannel(int fd, IpcTransport transport);

    /// accept() hands an already connected descriptor to a new channel; the
    /// constructor stays private so a descriptor can only enter a channel
    /// through connect() or accept().
    friend class MedicalIpcServer;

    int fd_;
    IpcTransport transport_;
};

/// Listening side of a local (UnixSeqpacket) channel. rpmsg has no listener
/// concept - the co-processor endpoint is created by the remoteproc firmware -
/// so constructing a server for RpmsgChar returns NotSupported.
class MedicalIpcServer {
public:
    ~MedicalIpcServer();

    static Result<std::unique_ptr<MedicalIpcServer>> listen(const IpcEndpoint& endpoint,
                                                            int backlog = 4);

    /// Accept one client, waiting up to `timeout`. Returns Timeout when no
    /// client arrived.
    ///
    /// The returned channel is **non-blocking**: see send()'s WouldBlock
    /// contract. This differs deliberately from a channel obtained through
    /// MedicalIpcChannel::connect(), which stays blocking - a client talking to
    /// a co-processor wants its write to complete, while a server publishing to
    /// viewers must never wait on one.
    Result<std::unique_ptr<MedicalIpcChannel>> accept(std::chrono::milliseconds timeout);

    int descriptor() const noexcept;
    const std::string& address() const noexcept;

    MedicalIpcServer(const MedicalIpcServer&) = delete;
    MedicalIpcServer& operator=(const MedicalIpcServer&) = delete;

private:
    MedicalIpcServer(int fd, std::string address);

    int fd_;
    std::string address_;
};

}  // namespace med
