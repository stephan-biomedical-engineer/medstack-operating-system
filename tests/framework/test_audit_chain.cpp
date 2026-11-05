// SPDX-License-Identifier: MIT
//
// The audit hash chain's verifier (src/AuditChain.h): the canonical form, the
// journal export reader, and the four ways a trail is tampered with - a field
// edited, a record removed, two reordered, a field added - each of which must
// be found at the record where it happened, while a legitimate restart of the
// chain must not be.
//
// The reference digest is NOT computed by the code under test: the canonical
// string is written out by hand below and its SHA-256 came from Python's
// hashlib (implementation_plan_conformity.md, phase 2.1). A test whose expected
// value is produced by the implementation proves the implementation agrees
// with itself.
//
// This suite runs without libsystemd, and so does the verifier. MedicalLogger,
// which WRITES the chain, does not run on a host without libsystemd-dev; its
// half is held by test_logger.cpp where it can run, and by make check.

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "AuditChain.h"
#include "check.h"

using namespace med;
using audit::Fields;

namespace {

const std::string kZero(64, '0');

/// What MedicalLogger writes for one record, built the way it builds it.
Fields record(const std::string& component, std::uint64_t seq, const std::string& prev,
              const std::string& event, const Fields& extra = {}) {
    Fields f = extra;
    f["MESSAGE"] = "event " + std::to_string(seq);
    f["MED_COMPONENT"] = component;
    f["MED_AUDIT"] = "1";
    f["MED_AUDIT_FORMAT"] = "2";
    f["MED_AUDIT_EVENT"] = event;
    f["MED_AUDIT_SEQ"] = std::to_string(seq);
    f["MED_AUDIT_TIME"] = "2026-11-05T18:00:0" + std::to_string(seq % 10) + ".000Z";
    f["MED_AUDIT_PREV"] = prev;
    f["MED_DEVICE_ID"] = "TEST-0000";
    // Journal fields outside the digest, as the journal adds them.
    f["PRIORITY"] = "5";
    f["_PID"] = "627";
    f["SYSLOG_IDENTIFIER"] = component;
    f["MED_AUDIT_HASH"] = audit::digest(prev, f);
    return f;
}

std::vector<Fields> chain(const std::string& component, std::uint64_t first, int count,
                          std::string prev = kZero) {
    std::vector<Fields> out;
    for (int i = 0; i < count; ++i) {
        out.push_back(record(component, first + static_cast<std::uint64_t>(i), prev, "SELF_TEST_PASSED",
                             {{"MED_SESSION", "s" + std::to_string(i)}}));
        prev = out.back()["MED_AUDIT_HASH"];
    }
    return out;
}

bool brokeAt(const audit::Report& r, std::uint64_t seq, const std::string& what) {
    return std::any_of(r.breaks.begin(), r.breaks.end(), [&](const audit::Break& b) {
        return b.sequence == seq && b.what.find(what) != std::string::npos;
    });
}

std::string exportText(const std::vector<Fields>& entries) {
    std::string out;
    for (const Fields& e : entries) {
        for (const auto& f : e) {
            out += f.first + "=" + f.second + "\n";
        }
        out += "\n";
    }
    return out;
}

}  // namespace

void testAuditChain() {
    medtest::section("Cadeia de auditoria - a forma canônica, contra um vetor independente");
    {
        Fields f = {
            {"MESSAGE", "EEG acquisition service starting"},
            {"MED_AUDIT", "1"},
            {"MED_AUDIT_EVENT", "SYSTEM_START"},
            {"MED_AUDIT_FORMAT", "2"},
            {"MED_AUDIT_PREV", kZero},
            {"MED_AUDIT_SEQ", "1"},
            {"MED_AUDIT_TIME", "2026-11-05T18:25:59.124Z"},
            {"MED_COMPONENT", "eeg-acquisition-service"},
            {"MED_CONFIG", "/etc/medplatform/eeg.conf"},
            {"MED_NOTE", "a|b=c\nd"},
            // Outside the digest:
            {"MED_AUDIT_HASH", "whatever"},
            {"PRIORITY", "5"},
            {"_PID", "627"},
            {"SYSLOG_IDENTIFIER", "eeg-acquisition-service"},
        };
        const std::string expected =
            "MED_AUDIT=1:1\n"
            "MED_AUDIT_EVENT=12:SYSTEM_START\n"
            "MED_AUDIT_FORMAT=1:2\n"
            "MED_AUDIT_PREV=64:" + kZero + "\n"
            "MED_AUDIT_SEQ=1:1\n"
            "MED_AUDIT_TIME=24:2026-11-05T18:25:59.124Z\n"
            "MED_COMPONENT=23:eeg-acquisition-service\n"
            "MED_CONFIG=25:/etc/medplatform/eeg.conf\n"
            "MED_NOTE=7:a|b=c\nd\n"
            "MESSAGE=32:EEG acquisition service starting\n";
        CHECK(audit::canonical(f) == expected);
        // sha256(PREV "\n" canonical), by Python's hashlib.
        CHECK(audit::digest(kZero, f) ==
              "11d99d1ae90374acf4fc4acecedc9a5ec062dd40fb6bb1c4d670ea27fa9fbd88");

        CHECK(audit::covered("MESSAGE") && audit::covered("MED_DEVICE_ID"));
        CHECK(!audit::covered("MED_AUDIT_HASH") && !audit::covered("PRIORITY") &&
              !audit::covered("_PID") && !audit::covered("MESSAGE_ID"));
    }

    medtest::section("Cadeia de auditoria - íntegra, e as quebras legítimas que não são quebras");
    {
        std::vector<Fields> trail = chain("svc", 1, 5);
        audit::Report r = audit::verify(trail);
        CHECK_EQ(static_cast<long long>(r.breaks.size()), 0LL);
        CHECK_EQ(static_cast<long long>(r.components.size()), 1LL);
        CHECK_EQ(static_cast<long long>(r.components[0].verified), 5LL);
        CHECK_EQ(static_cast<long long>(r.components[0].firstSequence), 1LL);

        // A restart: the chain's persisted head was lost and it began again
        // from genesis. Reported, not a break.
        std::vector<Fields> again = chain("svc", 1, 3);
        trail.insert(trail.end(), again.begin(), again.end());
        r = audit::verify(trail);
        CHECK_EQ(static_cast<long long>(r.breaks.size()), 0LL);
        CHECK_EQ(static_cast<long long>(r.components[0].restarts), 1LL);
        CHECK_EQ(static_cast<long long>(r.components[0].verified), 8LL);

        // A journal that no longer holds the beginning (rotated, vacuumed).
        std::vector<Fields> tail = chain("svc", 7, 3, std::string(64, 'a'));
        r = audit::verify(tail);
        CHECK_EQ(static_cast<long long>(r.breaks.size()), 0LL);
        CHECK_EQ(static_cast<long long>(r.components[0].firstSequence), 7LL);

        // Two components interleaved are two chains.
        std::vector<Fields> a = chain("a", 1, 3), b = chain("b", 1, 3), both;
        for (int i = 0; i < 3; ++i) {
            both.push_back(a[static_cast<std::size_t>(i)]);
            both.push_back(b[static_cast<std::size_t>(i)]);
        }
        r = audit::verify(both);
        CHECK_EQ(static_cast<long long>(r.breaks.size()), 0LL);
        CHECK_EQ(static_cast<long long>(r.components.size()), 2LL);

        // Records that are not audit records are not counted.
        std::vector<Fields> mixed = chain("svc", 1, 2);
        mixed.insert(mixed.begin() + 1, Fields{{"MESSAGE", "diagnostic"}, {"MED_COMPONENT", "svc"}});
        CHECK_EQ(static_cast<long long>(audit::verify(mixed).entries), 2LL);
        CHECK_EQ(static_cast<long long>(audit::verify(mixed).breaks.size()), 0LL);
    }

    medtest::section("Cadeia de auditoria - cada adulteração, no registro em que ocorreu");
    {
        // A field edited.
        std::vector<Fields> edited = chain("svc", 1, 5);
        edited[2]["MED_SESSION"] = "forged";
        audit::Report r = audit::verify(edited);
        CHECK_EQ(static_cast<long long>(r.breaks.size()), 1LL);
        CHECK(brokeAt(r, 3, "content"));

        // The message edited, and the attribution edited: both are covered.
        std::vector<Fields> message = chain("svc", 1, 5);
        message[1]["MESSAGE"] = "nothing happened";
        CHECK(brokeAt(audit::verify(message), 2, "content"));
        std::vector<Fields> device = chain("svc", 1, 5);
        device[3]["MED_DEVICE_ID"] = "OTHER";
        CHECK(brokeAt(audit::verify(device), 4, "content"));

        // A record removed.
        std::vector<Fields> removed = chain("svc", 1, 5);
        removed.erase(removed.begin() + 2);
        r = audit::verify(removed);
        CHECK_EQ(static_cast<long long>(r.breaks.size()), 1LL);
        CHECK(brokeAt(r, 4, "sequence"));

        // Two records swapped.
        std::vector<Fields> swapped = chain("svc", 1, 5);
        std::swap(swapped[1], swapped[2]);
        r = audit::verify(swapped);
        CHECK(!r.breaks.empty());
        CHECK(brokeAt(r, 3, "sequence"));

        // A field added after the fact.
        std::vector<Fields> added = chain("svc", 1, 5);
        added[1]["MED_OPERATOR"] = "someone";
        CHECK(brokeAt(audit::verify(added), 2, "content"));

        // A hash rewritten to match an edit: caught by the next record's PREV.
        std::vector<Fields> rehashed = chain("svc", 1, 5);
        rehashed[1]["MED_SESSION"] = "forged";
        rehashed[1]["MED_AUDIT_HASH"] = audit::digest(rehashed[1]["MED_AUDIT_PREV"], rehashed[1]);
        r = audit::verify(rehashed);
        CHECK(brokeAt(r, 3, "linkage"));
        CHECK(!brokeAt(r, 2, "content"));

        // A fake restart in the middle is reported as a restart and the
        // records after it verify: the chain is a witness, not a signature,
        // and the verifier's output says so by counting it.
        std::vector<Fields> fake = chain("svc", 1, 5);
        std::vector<Fields> forged = chain("svc", 1, 2);
        fake.insert(fake.end(), forged.begin(), forged.end());
        CHECK_EQ(static_cast<long long>(audit::verify(fake).components[0].restarts), 1LL);

        // Malformed and unknown formats.
        std::vector<Fields> malformed = chain("svc", 1, 3);
        malformed[1].erase("MED_AUDIT_PREV");
        CHECK(brokeAt(audit::verify(malformed), 0, "malformed"));
        std::vector<Fields> future = chain("svc", 1, 3);
        future[1]["MED_AUDIT_FORMAT"] = "3";
        CHECK(brokeAt(audit::verify(future), 2, "format 3"));
    }

    medtest::section("Cadeia de auditoria - formato 1: só o encadeamento é verificável");
    {
        // Format 1 records carry no MED_AUDIT_FORMAT, and their content cannot
        // be recomputed from the journal. Their linkage can.
        std::vector<Fields> legacy = chain("svc", 1, 4);
        for (Fields& f : legacy) {
            f.erase("MED_AUDIT_FORMAT");
        }
        audit::Report r = audit::verify(legacy);
        CHECK_EQ(static_cast<long long>(r.breaks.size()), 0LL);
        CHECK_EQ(static_cast<long long>(r.components[0].legacy), 4LL);
        CHECK_EQ(static_cast<long long>(r.components[0].verified), 0LL);
        legacy[2]["MED_AUDIT_PREV"] = std::string(64, 'f');
        CHECK(brokeAt(audit::verify(legacy), 3, "linkage"));
    }

    medtest::section("Cadeia de auditoria - o formato de exportação do journal");
    {
        std::vector<Fields> trail = chain("svc", 1, 3);
        Result<std::vector<Fields>> parsed = audit::parseExport(exportText(trail));
        CHECK(parsed.isOk());
        CHECK(parsed.value() == trail);
        CHECK_EQ(static_cast<long long>(audit::verify(parsed.value()).components[0].verified), 3LL);

        // A value with a newline travels in the binary form: name, le64
        // length, bytes, newline.
        std::string binary = "MESSAGE=x\nMED_NOTE\n";
        const std::string value = "line one\nline two";
        for (int i = 0; i < 8; ++i) {
            binary.push_back(static_cast<char>((value.size() >> (8 * i)) & 0xff));
        }
        binary += value + "\n\n";
        Result<std::vector<Fields>> withBinary = audit::parseExport(binary);
        CHECK(withBinary.isOk());
        CHECK(withBinary.value().size() == 1 && withBinary.value()[0]["MED_NOTE"] == value);

        CHECK_STATUS(audit::parseExport("MESSAGE=x").status(), Status::IntegrityError);
        CHECK_STATUS(audit::parseExport("MED_NOTE\n\x05\x00\x00").status(), Status::IntegrityError);
        CHECK_STATUS(audit::parseExport(std::string("MED_NOTE\n\xff\x00\x00\x00\x00\x00\x00\x00ab\n", 20)).status(),
                     Status::IntegrityError);

        // A field repeated in one record is counted and is a break: the logger
        // never writes one, and only one of the values was hashed.
        std::string twice = exportText(chain("svc", 1, 2));
        const std::size_t at = twice.find("MED_SESSION=s1\n");
        twice.insert(at, "MED_SESSION=other\n");
        std::uint64_t duplicates = 0;
        Result<std::vector<Fields>> dup = audit::parseExport(twice, &duplicates);
        CHECK(dup.isOk());
        CHECK_EQ(static_cast<long long>(duplicates), 1LL);
        CHECK(brokeAt(audit::verify(dup.value()), 2, "twice"));
    }
}
