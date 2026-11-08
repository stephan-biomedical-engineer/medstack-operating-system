// SPDX-License-Identifier: MIT
//
// med-service-access - the root half of MedOS service access (ACE-03).
//
//   med-service-access grant FILE          verify a grant; open the window
//   med-service-access session-start ID    one SSH connection begins
//   med-service-access session-end ID [RESULT]
//   med-service-access revoke WHY          close the window
//
// Every step is an audit record (SecurityEvent) in the MedicalLogger chain of
// component "med-service-access", and a step whose record cannot be written
// does not happen: a grant that cannot be audited opens nothing, and a
// connection that cannot be audited is refused (the unit's ExecStartPre fails).
// "No access without a record" is the property; the exit status is how the
// shell around this enforces it.
//
// Exit status: 0 done, 1 refused, 2 usage or I/O error.

#include "ServiceGrant.h"

#include <medplatform/MedicalLogger.h>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#ifndef MED_SERVICE_ACCESS_VERSION
#define MED_SERVICE_ACCESS_VERSION "0"
#endif

namespace {

const char* kRunDir = "/run/med-service";
const char* kAuthorizedKeys = "/run/med-service/authorized_keys";
const char* kHostKeyPub = "/run/med-service/ssh_host_ed25519_key.pub";
const char* kLock = "/run/med-service/.lock";
const char* kNonces = "/var/lib/medplatform/service-access.nonces";
const char* kRaucConf = "/etc/rauc/system.conf";

bool readFile(const std::string& path, std::string& out, std::size_t max) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), {});
    return out.size() <= max;
}

// compatible= under [system] and path= under [keyring]: the two facts of the
// update configuration this tool shares with RAUC, read from RAUC's own file
// so the two can never disagree.
bool raucConfig(std::string& compatible, std::string& keyring) {
    std::ifstream f(kRaucConf);
    std::string line, section;
    while (std::getline(f, line)) {
        if (!line.empty() && line[0] == '[') {
            section = line;
            continue;
        }
        if (section == "[system]" && line.rfind("compatible=", 0) == 0) compatible = line.substr(11);
        if (section == "[keyring]" && line.rfind("path=", 0) == 0) keyring = line.substr(5);
    }
    if (!keyring.empty() && keyring[0] != '/') keyring = "/etc/rauc/" + keyring;
    return !compatible.empty() && !keyring.empty();
}

std::set<std::string> usedNonces() {
    std::set<std::string> s;
    std::ifstream f(kNonces);
    std::string line;
    while (std::getline(f, line))
        if (!line.empty()) s.insert(line);
    return s;
}

bool appendDurably(const char* path, const std::string& line) {
    int fd = ::open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    bool ok = ::write(fd, line.data(), line.size()) == static_cast<ssize_t>(line.size()) && ::fsync(fd) == 0;
    return ::close(fd) == 0 && ok;
}

bool writeAtomically(const std::string& path, const std::string& content, mode_t mode) {
    std::string tmp = path + ".new";
    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
    if (fd < 0) return false;
    bool ok = ::write(fd, content.data(), content.size()) == static_cast<ssize_t>(content.size()) &&
              ::fchmod(fd, mode) == 0 && ::fsync(fd) == 0;
    ok = (::close(fd) == 0) && ok;
    if (!ok || ::rename(tmp.c_str(), path.c_str()) != 0) {
        ::unlink(tmp.c_str());
        return false;
    }
    return true;
}

bool audit(const std::string& message, std::vector<med::LogField> fields) {
    return med::MedicalLogger::instance().audit(med::AuditEvent::SecurityEvent, message, fields) == med::Status::Ok;
}

int refused(const std::string& why) {
    std::fprintf(stderr, "med-service-access: refused: %s\n", why.c_str());
    if (!audit("service access refused", {{"access_action", "refused"}, {"access_reason", why}}))
        std::fprintf(stderr, "med-service-access: and the refusal could not be audited\n");
    return 1;
}

std::string hostKeyFingerprint() {
    std::string pub;
    if (!readFile(kHostKeyPub, pub, 4096)) return "unknown";
    std::istringstream in(pub);
    std::string type, blob;
    in >> type >> blob;
    std::string fp = med::service::keyFingerprint(blob);
    return fp.empty() ? "unknown" : fp;
}

int grant(const std::string& file) {
    struct stat st{};
    if (::stat(kAuthorizedKeys, &st) == 0) return refused("a service window is already open");

    std::string der;
    if (!readFile(file, der, med::service::kMaxGrantBytes)) return refused("no readable grant at " + file);
    std::string compatible, keyringPath, keyring;
    if (!raucConfig(compatible, keyringPath) || !readFile(keyringPath, keyring, 1 << 20))
        return refused("the update configuration names no compatible or no keyring");

    med::service::Verdict v = med::service::checkGrant(med::service::verifyGrant(der, keyring), compatible,
                                                       usedNonces());
    if (!v.ok) return refused(v.reason);
    const med::service::Grant& g = v.grant;

    // The nonce is spent before anything opens: a crash between the two can
    // waste a grant, never reuse one.
    if (!appendDurably(kNonces, g.nonce + "\n")) return refused("cannot record the grant's nonce");
    if (!writeAtomically(kAuthorizedKeys, med::service::authorizedKeysLine(g), 0644))
        return refused("cannot write the authorized key");

    if (!audit("service access granted",
               {{"access_action", "granted"},
                {"access_signer", g.signer},
                {"access_key", g.fingerprint},
                {"access_key_comment", g.keyComment},
                {"access_duration_s", std::to_string(g.durationS)},
                {"access_nonce", g.nonce},
                {"access_grant_reason", g.reason},
                {"access_host_key", hostKeyFingerprint()}})) {
        ::unlink(kAuthorizedKeys);
        std::fprintf(stderr, "med-service-access: the grant could not be audited; nothing opened\n");
        return 2;
    }
    std::printf("%u\n", g.durationS);
    return 0;
}

int session(const char* action, const std::string& id, const std::string& result) {
    std::vector<med::LogField> f = {{"access_action", action}, {"access_connection", id}};
    if (!result.empty()) f.push_back({"access_result", result});
    bool open = ::access(kAuthorizedKeys, F_OK) == 0;
    f.push_back({"access_window", open ? "open" : "closed"});
    if (!audit(std::string("service session ") + (std::strcmp(action, "session_start") == 0 ? "start" : "end"), f))
        return 2;
    // A connection that reaches the listener after the window closed is
    // recorded and refused here, before sshd runs.
    return (std::strcmp(action, "session_start") == 0 && !open) ? 1 : 0;
}

int revoke(const std::string& why) {
    bool wasOpen = ::unlink(kAuthorizedKeys) == 0;
    if (!audit("service access closed", {{"access_action", "closed"},
                                         {"access_reason", why},
                                         {"access_window", wasOpen ? "was open" : "was not open"}}))
        return 2;
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr,
                     "usage: med-service-access grant FILE | session-start ID | session-end ID [RESULT] |"
                     " revoke WHY\n");
        return 2;
    }
    const std::string cmd = argv[1], arg = argv[2];

    ::mkdir(kRunDir, 0755);
    // One writer of this component's chain at a time: two connections ending
    // together are two processes, and the chain head is a file.
    int lock = ::open(kLock, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (lock < 0 || ::flock(lock, LOCK_EX) != 0) {
        std::fprintf(stderr, "med-service-access: cannot lock %s: %s\n", kLock, std::strerror(errno));
        return 2;
    }

    med::LoggerConfig cfg;
    cfg.component = "med-service-access";
    cfg.softwareVersion = MED_SERVICE_ACCESS_VERSION;
    cfg.chainStatePath = "/var/lib/medplatform/med-service-access-audit-chain.state";
    if (med::MedicalLogger::instance().configure(cfg) != med::Status::Ok) {
        std::fprintf(stderr, "med-service-access: the audit trail is unavailable; nothing done\n");
        return 2;
    }

    if (cmd == "grant") return grant(arg);
    if (cmd == "session-start") return session("session_start", arg, "");
    if (cmd == "session-end") return session("session_end", arg, argc > 3 ? argv[3] : "");
    if (cmd == "revoke") return revoke(arg);
    std::fprintf(stderr, "med-service-access: unknown command '%s'\n", cmd.c_str());
    return 2;
}
