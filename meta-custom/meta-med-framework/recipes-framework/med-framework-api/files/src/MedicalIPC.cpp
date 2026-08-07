// SPDX-License-Identifier: MIT

#include "MedicalIPC.h"

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace med {
namespace {

Status statusFromErrno(int error) {
    switch (error) {
        case ENOENT:
        case ENODEV:
        case ECONNREFUSED:
        case ENXIO:
            return Status::Unavailable;
        case EACCES:
        case EPERM:
            return Status::PermissionDenied;
        case EINVAL:
        case ENAMETOOLONG:
            return Status::InvalidArgument;
        case ETIMEDOUT:
        case EAGAIN:
            return Status::Timeout;
        default:
            return Status::IoError;
    }
}

/// Fill sun_path, refusing anything that would be silently truncated - a
/// truncated socket path connects to the wrong endpoint, which is worse than
/// failing.
bool fillSocketAddress(const std::string& path, struct sockaddr_un& address) {
    if (path.empty() || path.size() >= sizeof(address.sun_path)) {
        return false;
    }
    std::memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.c_str(), path.size());
    return true;
}

/// Wait for `events` on fd. Returns 1 ready, 0 timeout, -1 error (errno set).
int waitFor(int fd, short events, std::chrono::milliseconds timeout) {
    struct pollfd descriptor = {};
    descriptor.fd = fd;
    descriptor.events = events;

    const auto milliseconds = timeout.count();
    const int limit = milliseconds < 0 ? -1
                     : milliseconds > 0x7FFFFFFF ? 0x7FFFFFFF
                     : static_cast<int>(milliseconds);

    for (;;) {
        const int ready = ::poll(&descriptor, 1, limit);
        if (ready < 0 && errno == EINTR) {
            continue;  // a signal is not a transport error
        }
        return ready;
    }
}

}  // namespace

MedicalIpcChannel::MedicalIpcChannel(int fd, IpcTransport transport)
    : fd_(fd), transport_(transport) {}

MedicalIpcChannel::~MedicalIpcChannel() {
    close();
}

Result<std::unique_ptr<MedicalIpcChannel>> MedicalIpcChannel::connect(
    const IpcEndpoint& endpoint) {
    using ChannelResult = Result<std::unique_ptr<MedicalIpcChannel>>;

    if (endpoint.address.empty()) {
        return ChannelResult::fail(Status::InvalidArgument, "empty endpoint address");
    }

    if (endpoint.transport == IpcTransport::RpmsgChar) {
        const int fd = ::open(endpoint.address.c_str(), O_RDWR | O_CLOEXEC);
        if (fd < 0) {
            return ChannelResult::fail(statusFromErrno(errno),
                                       "open " + endpoint.address + ": " +
                                           std::strerror(errno));
        }
        return ChannelResult::ok(
            std::unique_ptr<MedicalIpcChannel>(new MedicalIpcChannel(fd, endpoint.transport)));
    }

    struct sockaddr_un address = {};
    if (!fillSocketAddress(endpoint.address, address)) {
        return ChannelResult::fail(Status::InvalidArgument,
                                   "socket path too long: " + endpoint.address);
    }

    const int fd = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return ChannelResult::fail(statusFromErrno(errno),
                                   std::string("socket: ") + std::strerror(errno));
    }

    if (::connect(fd, reinterpret_cast<struct sockaddr*>(&address), sizeof(address)) != 0) {
        const int saved = errno;
        ::close(fd);
        return ChannelResult::fail(statusFromErrno(saved),
                                   "connect " + endpoint.address + ": " +
                                       std::strerror(saved));
    }

    return ChannelResult::ok(
        std::unique_ptr<MedicalIpcChannel>(new MedicalIpcChannel(fd, endpoint.transport)));
}

Status MedicalIpcChannel::send(const void* data, std::size_t length) {
    if (fd_ < 0) {
        return Status::Unavailable;
    }
    if (data == nullptr || length == 0) {
        return Status::InvalidArgument;
    }

    for (;;) {
        const ssize_t written = ::write(fd_, data, length);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return statusFromErrno(errno);
        }
        // Both transports are message oriented, so a short write means the
        // message was truncated on the wire. Report it instead of letting the
        // peer parse a partial sample frame.
        return static_cast<std::size_t>(written) == length ? Status::Ok : Status::IoError;
    }
}

Result<std::size_t> MedicalIpcChannel::receive(void* buffer, std::size_t capacity,
                                               std::chrono::milliseconds timeout) {
    if (fd_ < 0) {
        return Result<std::size_t>::fail(Status::Unavailable, "channel is closed");
    }
    if (buffer == nullptr || capacity == 0) {
        return Result<std::size_t>::fail(Status::InvalidArgument, "no receive buffer");
    }

    const int ready = waitFor(fd_, POLLIN, timeout);
    if (ready < 0) {
        return Result<std::size_t>::fail(statusFromErrno(errno),
                                         std::string("poll: ") + std::strerror(errno));
    }
    if (ready == 0) {
        return Result<std::size_t>::fail(Status::Timeout, "no message within timeout");
    }

    for (;;) {
        const ssize_t received = ::read(fd_, buffer, capacity);
        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }
            return Result<std::size_t>::fail(statusFromErrno(errno),
                                             std::string("read: ") + std::strerror(errno));
        }
        if (received == 0) {
            return Result<std::size_t>::fail(Status::Unavailable, "peer closed the channel");
        }
        return Result<std::size_t>::ok(static_cast<std::size_t>(received));
    }
}

int MedicalIpcChannel::descriptor() const noexcept {
    return fd_;
}

IpcTransport MedicalIpcChannel::transport() const noexcept {
    return transport_;
}

void MedicalIpcChannel::close() noexcept {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

MedicalIpcServer::MedicalIpcServer(int fd, std::string address)
    : fd_(fd), address_(std::move(address)) {}

MedicalIpcServer::~MedicalIpcServer() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    if (!address_.empty()) {
        ::unlink(address_.c_str());
    }
}

Result<std::unique_ptr<MedicalIpcServer>> MedicalIpcServer::listen(const IpcEndpoint& endpoint,
                                                                   int backlog) {
    using ServerResult = Result<std::unique_ptr<MedicalIpcServer>>;

    if (endpoint.transport != IpcTransport::UnixSeqpacket) {
        return ServerResult::fail(Status::NotSupported,
                                  "rpmsg endpoints are created by the remote firmware");
    }

    struct sockaddr_un address = {};
    if (!fillSocketAddress(endpoint.address, address)) {
        return ServerResult::fail(Status::InvalidArgument,
                                  "socket path too long: " + endpoint.address);
    }

    const int fd = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return ServerResult::fail(statusFromErrno(errno),
                                  std::string("socket: ") + std::strerror(errno));
    }

    // A stale socket from a killed instance would make bind() fail with
    // EADDRINUSE forever; systemd restarts the service, so clean up first.
    ::unlink(endpoint.address.c_str());

    if (::bind(fd, reinterpret_cast<struct sockaddr*>(&address), sizeof(address)) != 0) {
        const int saved = errno;
        ::close(fd);
        return ServerResult::fail(statusFromErrno(saved),
                                  "bind " + endpoint.address + ": " + std::strerror(saved));
    }

    // Restrict before anyone can connect: between bind() and chmod() the
    // socket exists with the process umask.
    if (::chmod(endpoint.address.c_str(), static_cast<mode_t>(endpoint.socketMode)) != 0) {
        const int saved = errno;
        ::close(fd);
        ::unlink(endpoint.address.c_str());
        return ServerResult::fail(statusFromErrno(saved),
                                  std::string("chmod: ") + std::strerror(saved));
    }

    if (::listen(fd, backlog) != 0) {
        const int saved = errno;
        ::close(fd);
        ::unlink(endpoint.address.c_str());
        return ServerResult::fail(statusFromErrno(saved),
                                  std::string("listen: ") + std::strerror(saved));
    }

    return ServerResult::ok(
        std::unique_ptr<MedicalIpcServer>(new MedicalIpcServer(fd, endpoint.address)));
}

Result<std::unique_ptr<MedicalIpcChannel>> MedicalIpcServer::accept(
    std::chrono::milliseconds timeout) {
    using ChannelResult = Result<std::unique_ptr<MedicalIpcChannel>>;

    if (fd_ < 0) {
        return ChannelResult::fail(Status::Unavailable, "server is closed");
    }

    const int ready = waitFor(fd_, POLLIN, timeout);
    if (ready < 0) {
        return ChannelResult::fail(statusFromErrno(errno),
                                   std::string("poll: ") + std::strerror(errno));
    }
    if (ready == 0) {
        return ChannelResult::fail(Status::Timeout, "no client within timeout");
    }

    const int client = ::accept4(fd_, nullptr, nullptr, SOCK_CLOEXEC);
    if (client < 0) {
        return ChannelResult::fail(statusFromErrno(errno),
                                   std::string("accept: ") + std::strerror(errno));
    }

    return ChannelResult::ok(std::unique_ptr<MedicalIpcChannel>(
        new MedicalIpcChannel(client, IpcTransport::UnixSeqpacket)));
}

int MedicalIpcServer::descriptor() const noexcept {
    return fd_;
}

const std::string& MedicalIpcServer::address() const noexcept {
    return address_;
}

}  // namespace med
