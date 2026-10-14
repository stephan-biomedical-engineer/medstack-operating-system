// SPDX-License-Identifier: MIT
//
// MedicalStorage: the three properties a record store for a medical device has
// to have - confidentiality by construction, atomicity, and containment.
//
// Two of the three are testable on a development host. Atomicity is not: it
// needs a power cut in the middle of a rename, and a test that claimed to
// exercise it by calling write() twice would be theatre. It is named in the
// "not covered" list of this directory's README rather than faked here.

#include <grp.h>
#include <sys/stat.h>
#include <unistd.h>

#include <memory>
#include <string>
#include <vector>

#include "MedicalStorage.h"
#include <cstring>

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

    medtest::section("MedicalStorage - um registro que cresce sem que o produtor espere o volume");
    {
        TempDir dir;
        std::unique_ptr<MedicalStorage> store = openScratch(dir.path(), "s").take();
        CHECK(store != nullptr);
        if (store) {
            // Every record handed in is in the file, whole and in order, once
            // the stream is closed - and not necessarily before.
            const std::size_t recordBytes = 496;
            const std::uint32_t count = 20000;
            Result<std::unique_ptr<RecordStream>> opened = store->openStream("a/raw.bin");
            CHECK(opened.isOk());
            if (opened.isOk()) {
                std::unique_ptr<RecordStream> stream = opened.take();
                std::vector<std::uint8_t> record(recordBytes);
                std::uint64_t accepted = 0;
                for (std::uint32_t i = 0; i < count; ++i) {
                    for (std::size_t b = 0; b < recordBytes; ++b) {
                        record[b] = static_cast<std::uint8_t>(i + b);
                    }
                    std::memcpy(record.data(), &i, sizeof(i));
                    if (stream->append(record.data(), record.size()) == Status::Ok) {
                        ++accepted;
                    }
                }
                const RecordStreamStats stats = stream->close();
                // Nothing vanishes: accepted is written or failed, the rest
                // was refused, and the three add up to what was handed in.
                CHECK_EQ(static_cast<long long>(stats.written + stats.failed),
                         static_cast<long long>(accepted));
                CHECK_EQ(static_cast<long long>(stats.written + stats.failed + stats.refused),
                         static_cast<long long>(count));
                CHECK_EQ(static_cast<long long>(stats.failed), 0LL);
                // The point of the thing: a producer faster than the volume
                // gets many records per synchronisation, with nobody having
                // said how many.
                CHECK(stats.syncs >= 1);
                CHECK(stats.syncs < stats.written);

                Result<std::uint64_t> size = store->size("a/raw.bin");
                CHECK(size.isOk());
                CHECK_EQ(static_cast<long long>(size.isOk() ? size.value() : 0),
                         static_cast<long long>(stats.written * recordBytes));
                // In order and unbroken. Refused records leave gaps in the
                // numbering and nothing else, so each record is checked
                // against its own number.
                bool whole = true;
                std::uint32_t previous = 0;
                for (std::uint64_t r = 0; r < stats.written && whole; ++r) {
                    Result<std::vector<std::uint8_t>> piece =
                        store->read("a/raw.bin", r * recordBytes, recordBytes);
                    if (!piece.isOk() || piece.value().size() != recordBytes) {
                        whole = false;
                        break;
                    }
                    std::uint32_t number = 0;
                    std::memcpy(&number, piece.value().data(), sizeof(number));
                    if (r > 0 && number <= previous) {
                        whole = false;
                    }
                    previous = number;
                    for (std::size_t b = sizeof(number); b < recordBytes; ++b) {
                        if (piece.value()[b] != static_cast<std::uint8_t>(number + b)) {
                            whole = false;
                        }
                    }
                }
                CHECK(whole);

                // Closed is closed, and closing twice says the same thing.
                CHECK_STATUS(stream->append(record.data(), record.size()), Status::Unavailable);
                CHECK_EQ(static_cast<long long>(stream->close().written),
                         static_cast<long long>(stats.written));
            }

            // A queue that holds two records refuses the third while the
            // first two wait, says so, and counts it. The record that does
            // not fit at all is refused for ever.
            Result<std::unique_ptr<RecordStream>> small = store->openStream("b/raw.bin", 8);
            CHECK(small.isOk());
            if (small.isOk()) {
                std::unique_ptr<RecordStream> stream = small.take();
                CHECK_STATUS(stream->append("123456789", 9), Status::WouldBlock);
                std::uint64_t refused = 1;
                std::uint64_t taken = 0;
                for (int i = 0; i < 2000; ++i) {
                    const Status status = stream->append("abcd", 4);
                    if (status == Status::Ok) {
                        ++taken;
                    } else if (status == Status::WouldBlock) {
                        ++refused;
                    }
                }
                CHECK_EQ(static_cast<long long>(taken + refused), 2001LL);
                const RecordStreamStats stats = stream->close();
                CHECK_EQ(static_cast<long long>(stats.refused), static_cast<long long>(refused));
                CHECK_EQ(static_cast<long long>(stats.written), static_cast<long long>(taken));
                Result<std::uint64_t> size = store->size("b/raw.bin");
                CHECK_EQ(static_cast<long long>(size.isOk() ? size.value() : 0),
                         static_cast<long long>(taken * 4));
            }

            // A stream continues a record that is already there.
            CHECK_STATUS(store->append("c/raw.bin", "head", 4), Status::Ok);
            Result<std::unique_ptr<RecordStream>> more = store->openStream("c/raw.bin");
            CHECK(more.isOk());
            if (more.isOk()) {
                std::unique_ptr<RecordStream> stream = more.take();
                CHECK_STATUS(stream->append("tail", 4), Status::Ok);
                CHECK_STATUS(stream->append("", 0), Status::Ok);
                CHECK_STATUS(stream->append(nullptr, 4), Status::InvalidArgument);
                stream->close();
                Result<std::string> content = store->readString("c/raw.bin");
                CHECK(content.isOk() && content.value() == "headtail");
            }

            // The same containment as every other path into the store.
            CHECK(!store->openStream("../escape.bin").isOk());
            CHECK(!store->openStream("/etc/passwd").isOk());
            CHECK(!store->openStream("ok.bin", 0).isOk());
        }
    }

    medtest::section("MedicalStorage - leitura por faixa");
    {
        // The derived clinical file reads a session record in pieces, because
        // hours of acquisition do not fit in memory. Each piece has to be the
        // bytes at that place, and the end of the file has to read as such,
        // not as an error.
        TempDir dir;
        StorageConfig config;
        config.root = dir.path();
        config.nameSpace = "r";
        config.requireEncryptedBacking = false;
        std::unique_ptr<MedicalStorage> store = MedicalStorage::open(config).take();
        CHECK(store != nullptr);
        if (store) {
            std::vector<std::uint8_t> content(100000);
            for (std::size_t i = 0; i < content.size(); ++i) {
                content[i] = static_cast<std::uint8_t>((i * 7 + i / 256) & 0xFF);
            }
            CHECK_STATUS(store->write("raw.bin", content.data(), content.size()), Status::Ok);

            Result<std::uint64_t> size = store->size("raw.bin");
            CHECK(size.isOk());
            CHECK_EQ(static_cast<long long>(size.value()), 100000LL);

            Result<std::vector<std::uint8_t>> middle = store->read("raw.bin", 65530, 20);
            CHECK(middle.isOk());
            CHECK(middle.value() ==
                  std::vector<std::uint8_t>(content.begin() + 65530, content.begin() + 65550));

            Result<std::vector<std::uint8_t>> tail = store->read("raw.bin", 99990, 64);
            CHECK(tail.isOk());
            CHECK_EQ(static_cast<long long>(tail.value().size()), 10LL);
            CHECK(tail.value() == std::vector<std::uint8_t>(content.end() - 10, content.end()));

            Result<std::vector<std::uint8_t>> past = store->read("raw.bin", 100000, 64);
            CHECK(past.isOk());
            CHECK(past.value().empty());

            CHECK_STATUS(store->read("../escape", 0, 4).status(), Status::InvalidArgument);
            CHECK(!store->size("nada.bin").isOk());
            CHECK_STATUS(store->size("").status(), Status::InvalidArgument);
        }
    }

    medtest::section("MedicalStorage - publicar um arquivo pronto a um grupo de leitura");

    {
        TempDir dir;
        Result<std::unique_ptr<MedicalStorage>> opened = openScratch(dir.path());
        CHECK(opened.isOk());
        if (opened.isOk()) {
            const std::unique_ptr<MedicalStorage>& store = opened.value();

            // A group this process may give a file to without being root: its
            // own. On the device the caller is root and the group is the
            // reviewer's; the mechanism is the same.
            std::string ownGroup;
            if (const struct group* own = ::getgrgid(::getgid())) {
                ownGroup = own->gr_name;
            }
            CHECK(!ownGroup.empty());

            CHECK_STATUS(store->writeString("session-a/record.bdf", "clinical"), Status::Ok);
            CHECK_STATUS(store->writeString("session-a/raw.bin", "raw"), Status::Ok);

            CHECK_STATUS(store->publish("session-a/record.bdf", "a.bdf", ownGroup), Status::Ok);

            const std::string review = dir.path() + "/review";
            const std::string tree = review + "/" +
                store->basePath().substr(dir.path().size() + 1);
            const std::string published = tree + "/a.bdf";

            struct stat first = {};
            struct stat second = {};
            CHECK_EQ(::stat((store->basePath() + "/session-a/record.bdf").c_str(), &first), 0);
            CHECK_EQ(::stat(published.c_str(), &second), 0);
            // One file with two names, not a copy.
            CHECK(first.st_ino == second.st_ino);
            CHECK_EQ(static_cast<int>(second.st_nlink), 2);
            CHECK_EQ(static_cast<int>(second.st_mode & 0777), 0640);
            CHECK(second.st_gid == ::getgid());

            struct stat treeInfo = {};
            CHECK_EQ(::stat(tree.c_str(), &treeInfo), 0);
            CHECK_EQ(static_cast<int>(treeInfo.st_mode & 0777), 0750);
            CHECK_EQ(::stat(review.c_str(), &treeInfo), 0);
            CHECK_EQ(static_cast<int>(treeInfo.st_mode & 0777), 0750);

            // What was NOT published is exactly as closed as before: still
            // 0600, still behind 0700 directories, and absent from the tree.
            struct stat raw = {};
            CHECK_EQ(::stat((store->basePath() + "/session-a/raw.bin").c_str(), &raw), 0);
            CHECK_EQ(static_cast<int>(raw.st_mode & 0777), 0600);
            CHECK_EQ(static_cast<int>(raw.st_nlink), 1);
            struct stat sessionDir = {};
            CHECK_EQ(::stat((store->basePath() + "/session-a").c_str(), &sessionDir), 0);
            CHECK_EQ(static_cast<int>(sessionDir.st_mode & 0777), 0700);
            CHECK_EQ(::stat(store->basePath().c_str(), &sessionDir), 0);
            CHECK_EQ(static_cast<int>(sessionDir.st_mode & 0777), 0700);
            CHECK(::stat((tree + "/raw.bin").c_str(), &raw) != 0);

            // Again is Ok, and still one extra name.
            CHECK_STATUS(store->publish("session-a/record.bdf", "a.bdf", ownGroup), Status::Ok);
            CHECK_EQ(::stat(published.c_str(), &second), 0);
            CHECK_EQ(static_cast<int>(second.st_nlink), 2);

            // A file derived again is a new inode; the name follows it.
            CHECK_STATUS(store->remove("session-a/record.bdf"), Status::Ok);
            CHECK_STATUS(store->writeString("session-a/record.bdf", "clinical, again"), Status::Ok);
            CHECK_STATUS(store->publish("session-a/record.bdf", "a.bdf", ownGroup), Status::Ok);
            CHECK_EQ(::stat((store->basePath() + "/session-a/record.bdf").c_str(), &first), 0);
            CHECK_EQ(::stat(published.c_str(), &second), 0);
            CHECK(first.st_ino == second.st_ino);

            // Refusals.
            CHECK_STATUS(store->publish("session-a/absent.bdf", "b.bdf", ownGroup), Status::NotFound);
            CHECK_STATUS(store->publish("session-a/record.bdf", "../b.bdf", ownGroup),
                         Status::InvalidArgument);
            CHECK_STATUS(store->publish("session-a/record.bdf", "x/b.bdf", ownGroup),
                         Status::InvalidArgument);
            CHECK_STATUS(store->publish("session-a/record.bdf", "", ownGroup),
                         Status::InvalidArgument);
            CHECK_STATUS(store->publish("../escape", "b.bdf", ownGroup), Status::InvalidArgument);
            // A directory is not a file to hand out.
            CHECK_STATUS(store->publish("session-a", "b.bdf", ownGroup), Status::InvalidArgument);
            // A symbolic link would publish whatever it points at.
            CHECK_EQ(::symlink("/etc/passwd", (store->basePath() + "/session-a/link").c_str()), 0);
            CHECK_STATUS(store->publish("session-a/link", "b.bdf", ownGroup),
                         Status::InvalidArgument);
            // No such identity on this system: nothing is published, and the
            // file is not loosened on the way to finding that out.
            CHECK_STATUS(store->writeString("session-a/other.bdf", "x"), Status::Ok);
            CHECK_STATUS(store->publish("session-a/other.bdf", "c.bdf", "no-such-group-medtest"),
                         Status::Unavailable);
            struct stat other = {};
            CHECK_EQ(::stat((store->basePath() + "/session-a/other.bdf").c_str(), &other), 0);
            CHECK_EQ(static_cast<int>(other.st_mode & 0777), 0600);
            CHECK(::stat((tree + "/c.bdf").c_str(), &other) != 0);

            // Unpublishing removes the name and leaves the record.
            CHECK_STATUS(store->unpublish("a.bdf"), Status::Ok);
            CHECK(::stat(published.c_str(), &second) != 0);
            CHECK(store->exists("session-a/record.bdf"));
            CHECK_STATUS(store->unpublish("a.bdf"), Status::NotFound);
            CHECK_STATUS(store->unpublish("../a.bdf"), Status::InvalidArgument);
        }
    }
}
