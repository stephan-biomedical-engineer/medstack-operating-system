// SPDX-License-Identifier: MIT
//
// The shared vocabulary: Status, Result<T> and the timestamp conversions.
//
// Small surface, and the reason it is tested first is that every other test in
// this directory reports through it. A Result that took the success branch on
// a failure would make the rest of the suite pass silently.

#include <memory>
#include <string>
#include <vector>

#include "MedicalTypes.h"
#include "check.h"

using namespace med;

void testTypes() {
    medtest::section("MedicalTypes - Status e Result");

    // Status has a stable spelling; it becomes a journal field, so it is ABI in
    // the same sense the frame header is. SCREAMING_SNAKE_CASE, because that is
    // what a journal field value looks like and what a verifier greps for.
    CHECK_EQ(std::string(toString(Status::Ok)), std::string("OK"));
    CHECK_EQ(std::string(toString(Status::WouldBlock)), std::string("WOULD_BLOCK"));
    CHECK(isOk(Status::Ok));
    CHECK(!isOk(Status::Timeout));

    {
        // Every status is spelled, spelled distinctly, and spelled in the one
        // case convention. WouldBlock exists precisely because it must not be
        // confused with Timeout - two very different clinical situations - so a
        // shared spelling would undo the distinction the enum was extended for.
        const Status all[] = {
            Status::Ok,         Status::InvalidArgument, Status::NotFound,
            Status::PermissionDenied, Status::Unavailable, Status::Timeout,
            Status::IoError,    Status::IntegrityError,  Status::OutOfRange,
            Status::NotSupported, Status::Internal,      Status::WouldBlock,
        };

        std::vector<std::string> spellings;
        for (const Status status : all) {
            const std::string name = toString(status);
            CHECK_MSG(!name.empty(), "um Status não tem grafia");
            for (const char c : name) {
                CHECK_MSG((c >= 'A' && c <= 'Z') || c == '_',
                          "grafia fora de SCREAMING_SNAKE_CASE: '" + name + "'");
            }
            spellings.push_back(name);
        }
        for (std::size_t i = 0; i < spellings.size(); ++i) {
            for (std::size_t j = i + 1; j < spellings.size(); ++j) {
                CHECK_MSG(spellings[i] != spellings[j],
                          "dois Status compartilham a grafia '" + spellings[i] + "'");
            }
        }
    }

    // WouldBlock was appended after Internal so that no existing enumerator
    // changed value. That is a compatibility promise and it is cheap to check.
    CHECK_EQ(static_cast<int>(Status::Ok), 0);
    CHECK(static_cast<int>(Status::WouldBlock) > static_cast<int>(Status::Internal));

    {
        Result<int> ok = Result<int>::ok(42);
        CHECK(ok.isOk());
        CHECK(static_cast<bool>(ok));
        CHECK_EQ(ok.value(), 42);
        CHECK(ok.message().empty());
    }

    {
        Result<int> bad = Result<int>::fail(Status::NotFound, "sem registro");
        CHECK(!bad.isOk());
        CHECK(!static_cast<bool>(bad));
        CHECK_STATUS(bad.status(), Status::NotFound);
        CHECK_EQ(bad.message(), std::string("sem registro"));
        // A failed Result still reads as defined data, so a caller that ignores
        // the status does not read uninitialised memory.
        CHECK_EQ(bad.value(), 0);
    }

    {
        // The guard that matters: constructing a failure with Status::Ok must
        // not produce a Result that tests as success. A caller writing
        // `if (r)` would take the success branch with an unset value, which is
        // the shape of a defect that never reports itself.
        Result<int> contradiction = Result<int>::fail(Status::Ok, "impossível");
        CHECK(!contradiction.isOk());
        CHECK_STATUS(contradiction.status(), Status::Internal);
    }

    {
        // Move-only payloads: the framework returns Result<unique_ptr<...>>
        // from every factory, so this instantiation has to work.
        Result<std::unique_ptr<int>> owned =
            Result<std::unique_ptr<int>>::ok(std::unique_ptr<int>(new int(7)));
        CHECK(owned.isOk());
        std::unique_ptr<int> taken = owned.take();
        CHECK(taken != nullptr);
        CHECK_EQ(*taken, 7);
    }

    medtest::section("MedicalTypes - tempo");

    {
        // Round trip through the wire format of the AMP link. Microseconds
        // since the epoch is what the frame header carries, and a conversion
        // that loses a microsecond here loses it in every clinical record.
        const std::uint64_t micros = 1788000000123456ULL;
        const Timestamp stamp = fromUnixMicros(micros);
        CHECK_EQ(static_cast<long long>(toUnixMicros(stamp)),
                 static_cast<long long>(micros));

        // ISO-8601 UTC with milliseconds: fixed width, 'T' separator, 'Z'
        // suffix. The format is parsed by whoever reads the audit trail, so
        // its shape is an interface.
        const std::string text = formatTimestamp(stamp);
        CHECK_EQ(static_cast<long long>(text.size()), 24LL);
        CHECK_EQ(text[10], 'T');
        CHECK_EQ(text[23], 'Z');
        CHECK_EQ(text[19], '.');
    }

    {
        // The epoch itself, which is the value a zeroed timestamp produces -
        // and a record stamped 1970 is the symptom the board's missing RTC
        // gives, so the formatter must render it rather than hide it.
        CHECK_EQ(formatTimestamp(fromUnixMicros(0)),
                 std::string("1970-01-01T00:00:00.000Z"));
    }

    {
        CHECK(std::string(frameworkVersion()).size() > 0);
    }
}
