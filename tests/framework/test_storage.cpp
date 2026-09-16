// SPDX-License-Identifier: MIT
//
// MedicalStorage: the three properties a record store for a medical device has
// to have - confidentiality by construction, atomicity, and containment.
//
// Two of the three are testable on a development host. Atomicity is not: it
// needs a power cut in the middle of a rename, and a test that claimed to
// exercise it by calling write() twice would be theatre. It is named in the
// "not covered" list of this directory's README rather than faked here.

#include <sys/stat.h>

#include <memory>
#include <string>
#include <vector>

#include "MedicalStorage.h"
#include "check.h"
#include "temp_dir.h"

using namespace med;
using medtest::TempDir;

namespace {

/// A store on a scratch directory. requireEncryptedBacking is false because a
/// tmpfs is not dm-crypt - and the test immediately above proves that the
/// requirement, when left on, actually refuses.
Result<std::unique_ptr<MedicalStorage>> openScratch(const std::string& root,
                                                    const std::string& nameSpace = "records") {
    StorageConfig config;
    config.root = root;
    config.nameSpace = nameSpace;
    config.requireEncryptedBacking = false;
    return MedicalStorage::open(config);
}

}  // namespace

void testStorage() {
    medtest::section("MedicalStorage - a recusa por criptografia");

    {
        TempDir dir;
        CHECK(dir.valid());

        // The default is to require an encrypted backing device, and a scratch
        // directory is not one. This is the check that makes the EEG service's
        // storage.require_encryption=true meaningful: on the board, the service
        // running at all IS the assertion that /data is a real LUKS volume.
        StorageConfig config;
        config.root = dir.path();
        config.nameSpace = "records";
        CHECK(config.requireEncryptedBacking);

        Result<std::unique_ptr<MedicalStorage>> refused = MedicalStorage::open(config);
        CHECK(!refused.isOk());
        CHECK_STATUS(refused.status(), Status::PermissionDenied);
        // The message has to name the opt-out, because an operator reading a
        // start-up failure needs to know the difference between "broken" and
        // "not configured for development".
        CHECK(refused.message().find("requireEncryptedBacking") != std::string::npos);
    }

    {
        // Absolute path required: a relative storage root would resolve against
        // whatever directory the service happened to start in.
        StorageConfig config;
        config.root = "data";
        config.requireEncryptedBacking = false;
        Result<std::unique_ptr<MedicalStorage>> refused = MedicalStorage::open(config);
        CHECK(!refused.isOk());
        CHECK_STATUS(refused.status(), Status::InvalidArgument);
    }

    {
        StorageConfig config;
        config.root = "/nonexistent-med-root-for-tests";
        config.requireEncryptedBacking = false;
        Result<std::unique_ptr<MedicalStorage>> refused = MedicalStorage::open(config);
        CHECK(!refused.isOk());
        CHECK_STATUS(refused.status(), Status::Unavailable);
    }

    medtest::section("MedicalStorage - escrita, leitura e listagem");

    {
        TempDir dir;
        CHECK(dir.valid());
        Result<std::unique_ptr<MedicalStorage>> opened = openScratch(dir.path());
        CHECK(opened.isOk());
        if (!opened.isOk()) {
            return;
        }
        const std::unique_ptr<MedicalStorage>& store = opened.value();

        // The namespace directory is created, and created 0700: records are
        // not world readable even before anything is written into them.
        struct stat info = {};
        CHECK_EQ(::stat(store->basePath().c_str(), &info), 0);
        CHECK_EQ(static_cast<long long>(info.st_mode & 0777), 0700LL);
        CHECK_EQ(store->basePath(), dir.path() + "/records");

        CHECK_STATUS(store->writeString("session.json", "{\"link\":\"usb\"}"), Status::Ok);
        CHECK(store->exists("session.json"));

        Result<std::string> read = store->readString("session.json");
        CHECK(read.isOk());
        CHECK_EQ(read.value(), std::string("{\"link\":\"usb\"}"));

        // Binary round trip, because sample records are not text.
        const unsigned char raw[] = {0x4D, 0x45, 0x45, 0x47, 0x00, 0xFF, 0x7F, 0x80};
        CHECK_STATUS(store->write("raw.bin", raw, sizeof(raw)), Status::Ok);
        Result<std::vector<std::uint8_t>> bytes = store->read("raw.bin");
        CHECK(bytes.isOk());
        CHECK_EQ(static_cast<long long>(bytes.value().size()),
                 static_cast<long long>(sizeof(raw)));
        if (bytes.isOk() && bytes.value().size() == sizeof(raw)) {
            bool identical = true;
            for (std::size_t i = 0; i < sizeof(raw); ++i) {
                if (bytes.value()[i] != raw[i]) {
                    identical = false;
                }
            }
            CHECK(identical);
        }

        // append() is what a long acquisition uses; rewriting a growing session
        // file on every frame is not viable.
        CHECK_STATUS(store->append("raw.bin", raw, sizeof(raw)), Status::Ok);
        Result<std::vector<std::uint8_t>> appended = store->read("raw.bin");
        CHECK(appended.isOk());
        CHECK_EQ(static_cast<long long>(appended.value().size()),
                 static_cast<long long>(2 * sizeof(raw)));

        // append() to a path that does not exist yet creates it - the first
        // frame of a session must not need a separate write().
        CHECK_STATUS(store->append("fresh.bin", raw, sizeof(raw)), Status::Ok);
        CHECK(store->exists("fresh.bin"));

        // Subdirectories are created on demand, which is how one session gets
        // its own directory.
        CHECK_STATUS(store->writeString("session-1/metadata.json", "{}"), Status::Ok);
        CHECK(store->exists("session-1/metadata.json"));

        Result<std::vector<std::string>> listing = store->list();
        CHECK(listing.isOk());
        if (listing.isOk()) {
            const std::vector<std::string>& names = listing.value();
            CHECK_EQ(static_cast<long long>(names.size()), 4LL);
            // Sorted, so that a caller taking the last session gets the last
            // session rather than whatever the filesystem enumerated last.
            bool sorted = true;
            for (std::size_t i = 1; i < names.size(); ++i) {
                if (names[i - 1] > names[i]) {
                    sorted = false;
                }
            }
            CHECK(sorted);
        }

        Result<std::string> absent = store->readString("never-written.json");
        CHECK(!absent.isOk());
        CHECK_STATUS(absent.status(), Status::NotFound);

        CHECK_STATUS(store->remove("fresh.bin"), Status::Ok);
        CHECK(!store->exists("fresh.bin"));

        Result<StorageInfo> info2 = store->info();
        CHECK(info2.isOk());
        if (info2.isOk()) {
            CHECK_EQ(info2.value().root, dir.path());
            CHECK(!info2.value().encrypted);
            CHECK(info2.value().totalBytes > 0);
        }
    }

    medtest::section("MedicalStorage - contenção de caminho");

    {
        TempDir dir;
        CHECK(dir.valid());
        Result<std::unique_ptr<MedicalStorage>> opened = openScratch(dir.path());
        CHECK(opened.isOk());
        if (!opened.isOk()) {
            return;
        }
        const std::unique_ptr<MedicalStorage>& store = opened.value();

        // Every one of these must be refused lexically, before any syscall.
        // The interesting attack is a symlink planted under /data, which is a
        // race against the filesystem - so a check that ran after stat() would
        // be checking a state that has already changed.
        const char* const escapes[] = {
            "../escape",          // straight out of the namespace
            "..",                 // the parent itself
            "a/../../escape",     // out via a legal-looking prefix
            "/etc/passwd",        // absolute
            "/",                  //
            "",                   // empty
            "a//b",               // empty component
            "a/",                 // trailing slash: names a directory
            "./a",                // single dot component
            "a/./b",              //
        };

        for (const char* path : escapes) {
            CHECK_MSG(store->write(path, "x", 1) == Status::InvalidArgument,
                      std::string("write aceitou '") + path + "'");
            CHECK_MSG(!store->read(path).isOk(),
                      std::string("read aceitou '") + path + "'");
            CHECK_MSG(store->append(path, "x", 1) == Status::InvalidArgument,
                      std::string("append aceitou '") + path + "'");
            CHECK_MSG(!store->exists(path),
                      std::string("exists aceitou '") + path + "'");
            CHECK_MSG(store->remove(path) == Status::InvalidArgument,
                      std::string("remove aceitou '") + path + "'");
        }

        // A NUL inside the path: the lexical check has to see the whole
        // std::string, not stop at the terminator the way a C API would.
        std::string embeddedNul = "ok";
        embeddedNul.push_back('\0');
        embeddedNul += "/../escape";
        CHECK_STATUS(store->write(embeddedNul, "x", 1), Status::InvalidArgument);

        // And the namespace itself is subject to the same rule, otherwise the
        // containment could be escaped at open() time.
        Result<std::unique_ptr<MedicalStorage>> badNamespace =
            openScratch(dir.path(), "../outside");
        CHECK(!badNamespace.isOk());
        CHECK_STATUS(badNamespace.status(), Status::InvalidArgument);

        // Legitimate nested paths still work - the containment must not be a
        // ban on directories.
        CHECK_STATUS(store->writeString("a/b/c/record.json", "{}"), Status::Ok);
        CHECK(store->exists("a/b/c/record.json"));
    }
}
