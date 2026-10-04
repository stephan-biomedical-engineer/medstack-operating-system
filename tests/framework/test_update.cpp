// SPDX-License-Identifier: MIT
//
// MedicalUpdate: the half that needs no update daemon.
//
// Compiled only when libsystemd's headers are available, like the logger.
//
// What is testable here is reportReady(), and it is the half that carries the
// A/B argument: an application never confirms its slot, it tells the service
// manager it is doing its job, and the OS confirms the slot only once
// boot-complete.target - which the unit is required by - has been reached. So
// the contract worth pinning down is the message, where it goes, and that the
// three outcomes stay three: Ok (told), Unavailable (nobody to tell - run by
// hand, or a unit that is not Type=notify) and IoError (somebody to tell, and
// telling failed). Collapsing the last two would make a unit whose notification
// socket is broken indistinguishable from a developer running the binary in a
// shell, and only one of those holds back a boot.
//
// The service manager is played by a datagram socket bound in a temporary
// directory, which is exactly what systemd hands a Type=notify unit in
// NOTIFY_SOCKET. What is NOT testable here is the D-Bus half - connect(),
// install(), markBootedGood() - which needs rauc.service on a bus; main() lists
// it as uncovered on every run.

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>

#include "MedicalUpdate.h"
#include "check.h"
#include "temp_dir.h"

using namespace med;
using medtest::TempDir;

namespace {

/// NOTIFY_SOCKET is process state, and the suite runs other tests after this
/// one; whatever was there before is put back.
class NotifySocketEnv {
public:
    NotifySocketEnv() {
        const char* previous = std::getenv("NOTIFY_SOCKET");
        hadPrevious_ = (previous != nullptr);
        if (hadPrevious_) {
            previous_ = previous;
        }
    }
    ~NotifySocketEnv() {
        if (hadPrevious_) {
            ::setenv("NOTIFY_SOCKET", previous_.c_str(), 1);
        } else {
            ::unsetenv("NOTIFY_SOCKET");
        }
    }
    NotifySocketEnv(const NotifySocketEnv&) = delete;
    NotifySocketEnv& operator=(const NotifySocketEnv&) = delete;

private:
    bool hadPrevious_ = false;
    std::string previous_;
};

/// A bound AF_UNIX datagram socket, standing in for the service manager.
int bindListener(const std::string& path) {
    const int fd = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return -1;
    }
    sockaddr_un address = {};
    address.sun_family = AF_UNIX;
    if (path.size() >= sizeof(address.sun_path)) {
        ::close(fd);
        return -1;
    }
    std::memcpy(address.sun_path, path.c_str(), path.size());
    if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

/// Everything waiting on the listener, without blocking: sd_notify() has
/// already returned when this runs, so a message that is not there was never
/// sent.
std::string drain(int fd) {
    char buffer[512];
    const ssize_t got = ::recv(fd, buffer, sizeof(buffer) - 1, MSG_DONTWAIT);
    if (got <= 0) {
        return std::string();
    }
    return std::string(buffer, static_cast<std::size_t>(got));
}

}  // namespace

void testUpdate() {
    medtest::section("MedicalUpdate - reportReady(), sem gerenciador de serviço");

    {
        NotifySocketEnv restore;
        ::unsetenv("NOTIFY_SOCKET");
        // Run by hand, or from a unit that is not Type=notify: nobody to tell.
        // Unavailable, and not an error - a developer running the binary in a
        // shell must not see a failure the device would not.
        CHECK_STATUS(MedicalUpdate::reportReady(), Status::Unavailable);
    }

    medtest::section("MedicalUpdate - reportReady(), com um socket de notificação");

    {
        TempDir dir;
        CHECK(dir.valid());
        const std::string path = dir.file("notify");
        const int listener = bindListener(path);
        CHECK_MSG(listener >= 0, std::strerror(errno));

        NotifySocketEnv restore;
        ::setenv("NOTIFY_SOCKET", path.c_str(), 1);

        CHECK_STATUS(MedicalUpdate::reportReady(), Status::Ok);
        // The exact message is the interface: systemd completes a Type=notify
        // start job on READY=1 and on nothing else, so a typo here is a unit
        // that never starts and a slot that is never confirmed.
        CHECK_EQ(drain(listener), std::string("READY=1"));

        // The variable is left in place: a long-running service may report
        // more than once (a later STATUS= or WATCHDOG=1), and unsetting it
        // here would silently end that.
        const char* after = std::getenv("NOTIFY_SOCKET");
        CHECK(after != nullptr && path == after);

        // Idempotent from the caller's side: a second report is sent, not
        // refused, and systemd ignores it once the unit is active.
        CHECK_STATUS(MedicalUpdate::reportReady(), Status::Ok);
        CHECK_EQ(drain(listener), std::string("READY=1"));

        ::close(listener);
    }

    medtest::section("MedicalUpdate - reportReady(), socket que não responde");

    {
        TempDir dir;
        CHECK(dir.valid());

        NotifySocketEnv restore;
        // Somebody to tell, and telling fails. This is the unit whose start
        // job will time out and whose slot will not be confirmed, so it must
        // not be reported the same way as "nobody to tell".
        ::setenv("NOTIFY_SOCKET", dir.file("absent").c_str(), 1);
        CHECK_STATUS(MedicalUpdate::reportReady(), Status::IoError);

        // A path that is not a socket at all.
        ::setenv("NOTIFY_SOCKET", dir.path().c_str(), 1);
        CHECK_STATUS(MedicalUpdate::reportReady(), Status::IoError);
    }
}
