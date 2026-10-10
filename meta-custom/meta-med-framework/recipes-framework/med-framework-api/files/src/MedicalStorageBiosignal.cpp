// SPDX-License-Identifier: MIT
//
// MedFramework - BDF+ writer (see MedicalStorage.h).
//
// Every constant below was checked against the EDF specification
// (edfplus.info/specs/edf.html), EDF+ (edfplus.info/specs/edfplus.html) and the
// BDF+ description (teuniz.net, "BDF+ format description") - recorded in
// implementation_plan_bdf.md, phase 0. They are the format, not choices.

#include "MedicalStorage.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <string>
#include <vector>

#include "MedDigest.h"

namespace med {

namespace {

/// BDF: 0xFF then "BIOSEMI", in the 8-byte version field.
const unsigned char kBdfVersion[8] = {0xFF, 'B', 'I', 'O', 'S', 'E', 'M', 'I'};

/// The annotation signal BDF+ requires, and its fixed header values.
const char* const kAnnotationLabel = "BDF Annotations";

/// BDF+ samples are 3 bytes, two's complement, little endian.
constexpr std::int32_t kDigitalLowest = -8388608;
constexpr std::int32_t kDigitalHighest = 8388607;
constexpr std::size_t kBytesPerSample = 3;

/// TAL delimiters (EDF+ 2.2.2).
constexpr char kTalDuration = 0x15;
constexpr char kTalSeparator = 0x14;
constexpr char kTalEnd = 0x00;

/// BDF+ allows a data record of at most 15 MB.
constexpr std::uint64_t kMaxRecordBytes = 15ULL * 1024ULL * 1024ULL;

/// Appends are fsync'd (MedicalStorage::append), so they are batched.
constexpr std::size_t kFlushBytes = 256 * 1024;

/// Onsets and durations are written to the microsecond; checks allow for the
/// rounding that implies.
constexpr double kTimeTolerance = 1e-6;

const char* const kMonths[12] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                                  "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};

bool printableAscii(const std::string& text) {
    for (const char c : text) {
        if (c < 32 || c > 126) {
            return false;
        }
    }
    return true;
}

/// A header text field: printable ASCII, left-justified, space-filled.
bool appendText(std::string& out, const std::string& value, std::size_t width) {
    if (value.size() > width || !printableAscii(value)) {
        return false;
    }
    out += value;
    out.append(width - value.size(), ' ');
    return true;
}

/// The shortest decimal spelling of `value` with at most `width` characters,
/// rounded to the nearest representable value, with a dot as the separator and
/// no exponent (EDF+ 2.1.3, item 6). Empty if it cannot fit.
std::string fitNumber(double value, std::size_t width) {
    if (!std::isfinite(value)) {
        return {};
    }
    for (int decimals = 9; decimals >= 0; --decimals) {
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "%.*f", decimals, value);
        std::string text(buffer);
        if (text.find('.') != std::string::npos) {
            while (!text.empty() && text.back() == '0') {
                text.pop_back();
            }
            if (!text.empty() && text.back() == '.') {
                text.pop_back();
            }
        }
        if (text == "-0") {
            text = "0";
        }
        if (text.size() <= width) {
            return text;
        }
    }
    return {};
}

/// A time in a TAL: sign, then seconds to the microsecond, no exponent.
std::string talTime(double seconds, bool withSign) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.6f", std::fabs(seconds));
    std::string text(buffer);
    while (!text.empty() && text.back() == '0') {
        text.pop_back();
    }
    if (!text.empty() && text.back() == '.') {
        text.pop_back();
    }
    if (withSign) {
        text.insert(text.begin(), seconds < 0.0 ? '-' : '+');
    }
    return text;
}

/// The time-keeping TAL every record starts with: its onset and nothing else.
std::string timekeepingTal(double onset) {
    std::string tal = talTime(onset, true);
    tal += kTalSeparator;
    tal += kTalSeparator;
    tal += kTalEnd;
    return tal;
}

std::string annotationTal(const BiosignalAnnotation& annotation) {
    std::string tal = talTime(annotation.onsetSeconds, true);
    if (annotation.durationSeconds >= 0.0) {
        tal += kTalDuration;
        tal += talTime(annotation.durationSeconds, false);
    }
    tal += kTalSeparator;
    tal += annotation.text;
    tal += kTalSeparator;
    tal += kTalEnd;
    return tal;
}

bool validAnnotationText(const std::string& text) {
    for (const char c : text) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 32 || u == 127) {
            return false;
        }
    }
    return true;
}

}  // namespace

struct BiosignalWriter::Impl {
    MedicalStorage& storage;
    std::string path;
    BiosignalHeader header;
    std::size_t annotationBytes = 0;  // a whole number of samples
    std::uint64_t written = 0;
    double previousOnset = 0.0;
    std::deque<BiosignalAnnotation> pending;
    std::vector<unsigned char> buffer;
    internal::Sha256 sha;
    Status failure = Status::Ok;
    bool finished = false;
    std::string digest;

    Impl(MedicalStorage& s, std::string p, BiosignalHeader h)
        : storage(s), path(std::move(p)), header(std::move(h)) {}

    Status flush() {
        if (buffer.empty()) {
            return Status::Ok;
        }
        sha.update(buffer.data(), buffer.size());
        const Status status = storage.append(path, buffer.data(), buffer.size());
        buffer.clear();
        return status;
    }

    /// The longest time-keeping TAL the file can need, so that an annotation
    /// accepted now still fits beside it in any later record.
    std::size_t worstTimekeeping(double onset) const {
        const double last = header.startFraction +
                            static_cast<double>(header.recordCount) * header.recordDurationSeconds;
        return timekeepingTal(std::max(onset, last)).size();
    }
};

BiosignalWriter::BiosignalWriter(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

BiosignalWriter::~BiosignalWriter() = default;

Result<std::unique_ptr<BiosignalWriter>> BiosignalWriter::create(MedicalStorage& storage,
                                                                 const std::string& relativePath,
                                                                 const BiosignalHeader& header) {
    using WriterResult = Result<std::unique_ptr<BiosignalWriter>>;
    auto invalid = [](const std::string& why) {
        return WriterResult::fail(Status::InvalidArgument, "BDF+ header: " + why);
    };

    // ------------------------------------------------------------- the shape
    if (header.signals.empty()) {
        return invalid("no signal");
    }
    if (header.signals.size() + 1 > 9999) {
        return invalid("more signals than the 4-character count can state");
    }
    if (header.samplesPerRecord == 0) {
        return invalid("no sample per record");
    }
    if (!(header.recordDurationSeconds > 0.0) || !std::isfinite(header.recordDurationSeconds)) {
        return invalid("a record's duration must be positive");
    }
    {
        // The duration is the clock of the whole file: a reader places record
        // k at k times this field, so whatever the field cannot hold is added
        // once per record. What is bounded is that sum over the file, not the
        // error of one record - 1/3 s fits as "0.333333", 0.3 us short, which
        // is nothing once and a third of a second over a long recording.
        const std::string spelled = fitNumber(header.recordDurationSeconds, 8);
        const double drift =
            spelled.empty()
                ? INFINITY
                : std::fabs(std::strtod(spelled.c_str(), nullptr) - header.recordDurationSeconds) *
                      static_cast<double>(std::max<std::uint64_t>(header.recordCount, 1));
        if (!(drift <= kTimeTolerance)) {
            return invalid("the record duration " + std::to_string(header.recordDurationSeconds) +
                           " s cannot be written in 8 characters without the file drifting by "
                           "more than a microsecond");
        }
    }
    if (header.recordCount > 99999999ULL) {
        return invalid("more records than the 8-character count can state");
    }
    if (!(header.startFraction >= 0.0 && header.startFraction < 1.0)) {
        return invalid("the start fraction must be in [0, 1)");
    }

    // ---------------------------------------------------------- the start date
    if (header.year < 1985 || header.year > 2084) {
        return invalid("the start year must be 1985..2084, the range a two-digit date "
                       "can express");
    }
    if (header.month < 1 || header.month > 12 || header.day < 1 || header.day > 31 ||
        header.hour < 0 || header.hour > 23 || header.minute < 0 || header.minute > 59 ||
        header.second < 0 || header.second > 59) {
        return invalid("the start date or time is not a date or a time");
    }

    // -------------------------------------------------- the annotation signal
    const std::size_t annotationSamples =
        (header.annotationBytesPerRecord + kBytesPerSample - 1) / kBytesPerSample;
    const std::size_t annotationBytes = annotationSamples * kBytesPerSample;
    {
        const double last = header.startFraction + static_cast<double>(header.recordCount) *
                                                       header.recordDurationSeconds;
        if (annotationBytes < timekeepingTal(last).size()) {
            return invalid("the annotation space cannot hold even a record's own onset");
        }
    }
    const std::uint64_t recordBytes =
        static_cast<std::uint64_t>(header.signals.size()) * header.samplesPerRecord *
            kBytesPerSample +
        annotationBytes;
    if (recordBytes > kMaxRecordBytes) {
        return invalid("a data record would exceed BDF+'s 15 MB");
    }

    // ------------------------------------------------------------ the header
    const std::size_t ns = header.signals.size() + 1;
    std::string text;
    text.reserve(256 * (ns + 1));

    text.append(reinterpret_cast<const char*>(kBdfVersion), sizeof(kBdfVersion));
    if (!appendText(text, header.patient, 80)) {
        return invalid("the patient field is longer than 80 characters or not printable ASCII");
    }
    {
        char start[32];
        std::snprintf(start, sizeof(start), "Startdate %02d-%s-%04d", header.day,
                      kMonths[header.month - 1], header.year);
        const std::string recording =
            std::string(start) + (header.recording.empty() ? "" : " " + header.recording);
        if (!appendText(text, recording, 80)) {
            return invalid("the recording field is longer than 80 characters or not "
                           "printable ASCII");
        }
    }
    {
        char field[16];
        std::snprintf(field, sizeof(field), "%02d.%02d.%02d", header.day, header.month,
                      header.year % 100);
        appendText(text, field, 8);
        std::snprintf(field, sizeof(field), "%02d.%02d.%02d", header.hour, header.minute,
                      header.second);
        appendText(text, field, 8);
    }
    appendText(text, std::to_string(256 * (ns + 1)), 8);
    appendText(text, header.continuous ? "BDF+C" : "BDF+D", 44);
    appendText(text, std::to_string(header.recordCount), 8);
    appendText(text, fitNumber(header.recordDurationSeconds, 8), 8);
    appendText(text, std::to_string(ns), 4);

    // Per signal, field by field across all signals - the format's layout.
    for (const BiosignalSignal& signal : header.signals) {
        if (signal.label.empty() || !appendText(text, signal.label, 16)) {
            return invalid("label '" + signal.label + "' is empty, longer than 16 characters "
                           "or not printable ASCII");
        }
        if (signal.label == kAnnotationLabel) {
            return invalid("'" + signal.label + "' is the annotation signal's label");
        }
    }
    appendText(text, kAnnotationLabel, 16);

    for (const BiosignalSignal& signal : header.signals) {
        if (!appendText(text, signal.transducer, 80)) {
            return invalid("transducer of '" + signal.label + "' does not fit 80 characters");
        }
    }
    appendText(text, "", 80);

    for (const BiosignalSignal& signal : header.signals) {
        if (!appendText(text, signal.physicalDimension, 8)) {
            return invalid("dimension of '" + signal.label + "' does not fit 8 characters");
        }
    }
    appendText(text, "", 8);

    for (const BiosignalSignal& signal : header.signals) {
        if (!(signal.physicalMinimum < signal.physicalMaximum)) {
            return invalid("'" + signal.label + "': physical minimum must be below maximum");
        }
        const std::string field = fitNumber(signal.physicalMinimum, 8);
        if (field.empty()) {
            return invalid("'" + signal.label + "': physical minimum does not fit 8 characters");
        }
        appendText(text, field, 8);
    }
    appendText(text, "-1", 8);

    for (const BiosignalSignal& signal : header.signals) {
        const std::string field = fitNumber(signal.physicalMaximum, 8);
        if (field.empty()) {
            return invalid("'" + signal.label + "': physical maximum does not fit 8 characters");
        }
        appendText(text, field, 8);
    }
    appendText(text, "1", 8);

    for (const BiosignalSignal& signal : header.signals) {
        if (signal.digitalMinimum < kDigitalLowest || signal.digitalMaximum > kDigitalHighest ||
            !(signal.digitalMinimum < signal.digitalMaximum)) {
            return invalid("'" + signal.label + "': digital range must lie within 24 bits, "
                           "minimum below maximum");
        }
        appendText(text, std::to_string(signal.digitalMinimum), 8);
    }
    appendText(text, std::to_string(kDigitalLowest), 8);

    for (const BiosignalSignal& signal : header.signals) {
        appendText(text, std::to_string(signal.digitalMaximum), 8);
    }
    appendText(text, std::to_string(kDigitalHighest), 8);

    for (const BiosignalSignal& signal : header.signals) {
        if (!appendText(text, signal.prefiltering, 80)) {
            return invalid("prefiltering of '" + signal.label + "' does not fit 80 characters");
        }
    }
    appendText(text, "", 80);

    for (std::size_t i = 0; i < header.signals.size(); ++i) {
        appendText(text, std::to_string(header.samplesPerRecord), 8);
    }
    appendText(text, std::to_string(annotationSamples), 8);

    for (std::size_t i = 0; i < ns; ++i) {
        appendText(text, "", 32);
    }

    if (text.size() != 256 * (ns + 1)) {
        return WriterResult::fail(Status::Internal, "BDF+ header has the wrong size");
    }

    // ------------------------------------------------- start from nothing
    for (const std::string& stale : {relativePath + ".sha256", relativePath}) {
        if (storage.exists(stale)) {
            const Status removed = storage.remove(stale);
            if (removed != Status::Ok) {
                return WriterResult::fail(removed, "cannot remove the previous " + stale);
            }
        }
    }

    std::unique_ptr<Impl> impl(new Impl(storage, relativePath, header));
    impl->annotationBytes = annotationBytes;
    impl->buffer.assign(text.begin(), text.end());
    return WriterResult::ok(std::unique_ptr<BiosignalWriter>(new BiosignalWriter(std::move(impl))));
}

Status BiosignalWriter::appendRecord(double onsetSeconds, const std::int32_t* samples,
                                     const std::vector<BiosignalAnnotation>& annotations) {
    Impl& w = *impl_;
    if (w.failure != Status::Ok) {
        return w.failure;
    }
    if (w.finished) {
        return Status::InvalidArgument;
    }
    if (samples == nullptr || !std::isfinite(onsetSeconds)) {
        return Status::InvalidArgument;
    }
    if (w.written >= w.header.recordCount) {
        return Status::OutOfRange;
    }

    // ---------------------------------------------------------- the onset
    const BiosignalHeader& h = w.header;
    if (w.written == 0 || h.continuous) {
        // The first record starts at the header's start (EDF+: the first TAL
        // is "+0.X"); in a continuous file, so does every record after it,
        // shifted by whole durations.
        const double expected =
            h.startFraction + static_cast<double>(w.written) * h.recordDurationSeconds;
        if (std::fabs(onsetSeconds - expected) > kTimeTolerance) {
            return Status::InvalidArgument;
        }
    } else if (onsetSeconds < w.previousOnset + h.recordDurationSeconds - kTimeTolerance) {
        // A later record that starts before the previous one ended overlaps
        // it; the format requires records that follow in time to follow in
        // the file.
        return Status::InvalidArgument;
    }

    // ------------------------------------------- everything checked first
    const std::size_t ns = h.signals.size();
    const std::size_t count = static_cast<std::size_t>(h.samplesPerRecord) * ns;
    for (std::size_t i = 0; i < count; ++i) {
        const BiosignalSignal& signal = h.signals[i % ns];
        if (samples[i] < signal.digitalMinimum || samples[i] > signal.digitalMaximum) {
            return Status::OutOfRange;
        }
    }
    for (const BiosignalAnnotation& annotation : annotations) {
        if (!std::isfinite(annotation.onsetSeconds) ||
            !std::isfinite(annotation.durationSeconds) ||
            !validAnnotationText(annotation.text)) {
            return Status::InvalidArgument;
        }
        // Carried forward whole and never split (EDF+: a TAL may not overflow
        // into another record), so one that cannot fit beside the longest
        // onset this file can have would wait for ever.
        if (annotationTal(annotation).size() + w.worstTimekeeping(onsetSeconds) >
            w.annotationBytes) {
            return Status::InvalidArgument;
        }
    }

    // ---------------------------------------------------- the signals
    for (std::size_t s = 0; s < ns; ++s) {
        for (std::size_t i = 0; i < h.samplesPerRecord; ++i) {
            const std::uint32_t v = static_cast<std::uint32_t>(samples[i * ns + s]);
            w.buffer.push_back(static_cast<unsigned char>(v & 0xFFu));
            w.buffer.push_back(static_cast<unsigned char>((v >> 8) & 0xFFu));
            w.buffer.push_back(static_cast<unsigned char>((v >> 16) & 0xFFu));
        }
    }

    // ------------------------------------------------ the annotations
    for (const BiosignalAnnotation& annotation : annotations) {
        w.pending.push_back(annotation);
    }
    std::string tals = timekeepingTal(onsetSeconds);
    while (!w.pending.empty()) {
        const std::string tal = annotationTal(w.pending.front());
        if (tals.size() + tal.size() > w.annotationBytes) {
            break;  // the rest waits, in order, for the next record
        }
        tals += tal;
        w.pending.pop_front();
    }
    w.buffer.insert(w.buffer.end(), tals.begin(), tals.end());
    w.buffer.insert(w.buffer.end(), w.annotationBytes - tals.size(), 0);

    w.previousOnset = onsetSeconds;
    ++w.written;

    if (w.buffer.size() >= kFlushBytes) {
        w.failure = w.flush();
    }
    return w.failure;
}

Status BiosignalWriter::finish() {
    Impl& w = *impl_;
    if (w.failure != Status::Ok) {
        return w.failure;
    }
    if (w.finished) {
        return Status::Ok;
    }
    if (w.written != w.header.recordCount) {
        // The header already says how many records there are; a file with
        // fewer is one whose header is false.
        return Status::IntegrityError;
    }
    if (!w.pending.empty()) {
        // Annotations nobody will ever read. The caller sized the space too
        // small for what it had to say.
        return Status::OutOfRange;
    }

    w.failure = w.flush();
    if (w.failure != Status::Ok) {
        return w.failure;
    }
    w.digest = w.sha.finishHex();
    if (w.digest.empty()) {
        w.failure = Status::IntegrityError;
        return w.failure;
    }

    // The sidecar is what makes the file complete, so it comes last, and in
    // the format sha256sum -c reads.
    const std::string::size_type slash = w.path.find_last_of('/');
    const std::string name = slash == std::string::npos ? w.path : w.path.substr(slash + 1);
    w.failure = w.storage.writeString(w.path + ".sha256", w.digest + "  " + name + "\n");
    if (w.failure == Status::Ok) {
        w.finished = true;
    }
    return w.failure;
}

const std::string& BiosignalWriter::digest() const noexcept {
    return impl_->digest;
}

std::size_t BiosignalWriter::annotationBytes(const BiosignalAnnotation& annotation) {
    return annotationTal(annotation).size();
}

std::size_t BiosignalWriter::timekeepingBytes(double onsetSeconds) {
    return timekeepingTal(onsetSeconds).size();
}

}  // namespace med
