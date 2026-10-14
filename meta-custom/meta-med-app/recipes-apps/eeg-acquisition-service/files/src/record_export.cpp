// SPDX-License-Identifier: MIT
//
// The clinical file of an EEG session - see record_export.h.

#include "record_export.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <utility>

#include <medplatform/MedicalDevice.h>

namespace eeg {

namespace {

// ===========================================================================
// metadata.json - a reader for the subset of JSON the service writes. Strict:
// anything it does not understand is an error, never a guess.
// ===========================================================================

struct Json {
    enum class Kind { Null, Bool, Number, String, Array, Object };
    Kind kind = Kind::Null;
    bool boolean = false;
    double number = 0.0;
    std::string text;
    std::vector<Json> items;
    std::vector<std::pair<std::string, Json>> members;

    const Json* get(const std::string& key) const {
        for (const auto& member : members) {
            if (member.first == key) {
                return &member.second;
            }
        }
        return nullptr;
    }
};

class JsonParser {
public:
    explicit JsonParser(const std::string& input) : in_(input) {}

    bool parse(Json& out) {
        if (!value(out, 0)) {
            return false;
        }
        space();
        return at_ == in_.size();
    }

private:
    void space() {
        while (at_ < in_.size() &&
               (in_[at_] == ' ' || in_[at_] == '\n' || in_[at_] == '\r' || in_[at_] == '\t')) {
            ++at_;
        }
    }

    bool literal(const char* word) {
        const std::size_t length = std::strlen(word);
        if (in_.compare(at_, length, word) != 0) {
            return false;
        }
        at_ += length;
        return true;
    }

    bool string(std::string& out) {
        if (at_ >= in_.size() || in_[at_] != '"') {
            return false;
        }
        ++at_;
        while (at_ < in_.size()) {
            const char c = in_[at_++];
            if (c == '"') {
                return true;
            }
            if (static_cast<unsigned char>(c) < 0x20) {
                return false;
            }
            if (c != '\\') {
                out += c;
                continue;
            }
            if (at_ >= in_.size()) {
                return false;
            }
            const char e = in_[at_++];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case 'u': {
                    // Only what the service can emit: ASCII control escapes.
                    if (at_ + 4 > in_.size()) {
                        return false;
                    }
                    const unsigned long code = std::strtoul(in_.substr(at_, 4).c_str(), nullptr, 16);
                    if (code > 0x7F) {
                        return false;
                    }
                    out += static_cast<char>(code);
                    at_ += 4;
                    break;
                }
                default: return false;
            }
        }
        return false;
    }

    bool value(Json& out, int depth) {
        if (depth > 8) {
            return false;
        }
        space();
        if (at_ >= in_.size()) {
            return false;
        }
        const char c = in_[at_];
        if (c == '{') {
            out.kind = Json::Kind::Object;
            ++at_;
            space();
            if (at_ < in_.size() && in_[at_] == '}') {
                ++at_;
                return true;
            }
            for (;;) {
                space();
                std::string key;
                if (!string(key)) {
                    return false;
                }
                space();
                if (at_ >= in_.size() || in_[at_] != ':') {
                    return false;
                }
                ++at_;
                Json member;
                if (!value(member, depth + 1)) {
                    return false;
                }
                out.members.emplace_back(std::move(key), std::move(member));
                space();
                if (at_ < in_.size() && in_[at_] == ',') {
                    ++at_;
                    continue;
                }
                if (at_ < in_.size() && in_[at_] == '}') {
                    ++at_;
                    return true;
                }
                return false;
            }
        }
        if (c == '[') {
            out.kind = Json::Kind::Array;
            ++at_;
            space();
            if (at_ < in_.size() && in_[at_] == ']') {
                ++at_;
                return true;
            }
            for (;;) {
                Json item;
                if (!value(item, depth + 1)) {
                    return false;
                }
                out.items.push_back(std::move(item));
                space();
                if (at_ < in_.size() && in_[at_] == ',') {
                    ++at_;
                    continue;
                }
                if (at_ < in_.size() && in_[at_] == ']') {
                    ++at_;
                    return true;
                }
                return false;
            }
        }
        if (c == '"') {
            out.kind = Json::Kind::String;
            return string(out.text);
        }
        if (literal("true")) {
            out.kind = Json::Kind::Bool;
            out.boolean = true;
            return true;
        }
        if (literal("false")) {
            out.kind = Json::Kind::Bool;
            return true;
        }
        if (literal("null")) {
            return true;
        }
        const char* begin = in_.c_str() + at_;
        char* end = nullptr;
        out.number = std::strtod(begin, &end);
        if (end == begin || !std::isfinite(out.number)) {
            return false;
        }
        out.kind = Json::Kind::Number;
        at_ += static_cast<std::size_t>(end - begin);
        return true;
    }

    const std::string& in_;
    std::size_t at_ = 0;
};

// ===========================================================================
// Time. The session id is the UTC instant the session opened; the format
// wants local time (EDF+ item 8), and the offset in force is in the metadata.
// ===========================================================================

/// "YYYY-MM-DDTHH:MM:SS.mmmZ" to microseconds since the epoch.
bool parseSessionTime(const std::string& id, std::int64_t& micros) {
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0, millis = 0;
    char zone = 0;
    if (id.size() != 24 ||
        std::sscanf(id.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d.%3d%c", &year, &month, &day, &hour,
                    &minute, &second, &millis, &zone) != 8 ||
        zone != 'Z') {
        return false;
    }
    std::tm broken{};
    broken.tm_year = year - 1900;
    broken.tm_mon = month - 1;
    broken.tm_mday = day;
    broken.tm_hour = hour;
    broken.tm_min = minute;
    broken.tm_sec = second;
    const std::time_t seconds = timegm(&broken);
    micros = static_cast<std::int64_t>(seconds) * 1000000LL + millis * 1000LL;
    return true;
}

/// "+HH:MM" / "-HH:MM" to seconds.
bool parseOffset(const std::string& text, std::int64_t& seconds) {
    int hours = 0, minutes = 0;
    if (text.size() != 6 || (text[0] != '+' && text[0] != '-') ||
        std::sscanf(text.c_str() + 1, "%2d:%2d", &hours, &minutes) != 2) {
        return false;
    }
    seconds = (hours * 3600LL + minutes * 60LL) * (text[0] == '-' ? -1 : 1);
    return true;
}

std::int64_t floorDiv(std::int64_t a, std::int64_t b) {
    std::int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) {
        --q;
    }
    return q;
}

// ===========================================================================
// raw.bin, one frame at a time, in windows - a session can be hours long.
// ===========================================================================

constexpr std::size_t kWindow = 1 << 20;
constexpr std::uint32_t kMaxSamplesPerChannel = 4096;

class FrameScanner {
public:
    enum class Kind { Frame, BadFrame, Unreadable, Partial, End, Error };

    struct Item {
        Kind kind = Kind::End;
        med::SampleFrame frame;
        std::uint64_t bytes = 0;  ///< BadFrame/Unreadable/Partial: bytes consumed
        med::Status status = med::Status::Ok;
        std::string message;
    };

    FrameScanner(med::MedicalStorage& storage, std::string path, std::uint64_t size,
                 std::uint16_t channels, std::uint64_t start)
        : storage_(storage), path_(std::move(path)), size_(size), channels_(channels),
          offset_(start) {}

    std::uint64_t offset() const { return offset_; }

    Item next() {
        Item item;
        if (offset_ >= size_) {
            item.kind = Kind::End;
            return item;
        }
        const std::uint64_t remaining = size_ - offset_;
        if (!ensure(static_cast<std::size_t>(std::min<std::uint64_t>(remaining, 40)), item)) {
            return item;
        }
        if (remaining < 4 || magicAt(offset_) != med::amp::kFrameMagic) {
            return resync(item);
        }
        if (remaining < sizeof(med::amp::FrameHeader)) {
            item.kind = Kind::Partial;
            item.bytes = remaining;
            offset_ = size_;
            return item;
        }

        med::amp::FrameHeader header;
        std::memcpy(&header, at(offset_), sizeof(header));
        if (header.version != med::amp::kFrameVersion || header.channelCount != channels_ ||
            header.samplesPerChannel == 0 || header.samplesPerChannel > kMaxSamplesPerChannel) {
            // A magic number in the middle of noise, or a frame from a
            // geometry this session never had: not a frame.
            return resync(item);
        }
        const std::uint64_t frameBytes =
            sizeof(med::amp::FrameHeader) +
            static_cast<std::uint64_t>(header.channelCount) * header.samplesPerChannel * 4 +
            sizeof(med::amp::LeadOffBlock);
        if (remaining < frameBytes) {
            item.kind = Kind::Partial;
            item.bytes = remaining;
            offset_ = size_;
            return item;
        }
        if (!ensure(static_cast<std::size_t>(frameBytes), item)) {
            return item;
        }
        med::Result<med::SampleFrame> decoded =
            med::amp::decodeFrame(at(offset_), static_cast<std::size_t>(frameBytes));
        offset_ += frameBytes;
        if (!decoded) {
            // The header was plausible and the payload's CRC did not close: a
            // frame was here, and nothing in it can be trusted.
            item.kind = Kind::BadFrame;
            item.bytes = frameBytes;
            return item;
        }
        item.kind = Kind::Frame;
        item.frame = decoded.take();
        return item;
    }

private:
    const unsigned char* at(std::uint64_t offset) const {
        return window_.data() + (offset - windowStart_);
    }

    std::uint32_t magicAt(std::uint64_t offset) const {
        std::uint32_t magic = 0;
        std::memcpy(&magic, at(offset), sizeof(magic));
        return magic;
    }

    /// Makes [offset_, offset_ + n) available in the window.
    bool ensure(std::size_t n, Item& item) {
        if (offset_ >= windowStart_ && offset_ + n <= windowStart_ + window_.size()) {
            return true;
        }
        med::Result<std::vector<std::uint8_t>> chunk =
            storage_.read(path_, offset_, std::max(n, kWindow));
        if (!chunk) {
            item.kind = Kind::Error;
            item.status = chunk.status();
            item.message = chunk.message();
            return false;
        }
        window_ = chunk.take();
        windowStart_ = offset_;
        if (window_.size() < n) {
            item.kind = Kind::Error;
            item.status = med::Status::IoError;
            item.message = path_ + " shrank while it was being read";
            return false;
        }
        return true;
    }

    /// Skips forward to the next frame magic, or to the end.
    Item resync(Item& item) {
        const std::uint64_t from = offset_;
        ++offset_;
        while (offset_ + 4 <= size_) {
            if (!ensure(4, item)) {
                return item;
            }
            if (magicAt(offset_) == med::amp::kFrameMagic) {
                break;
            }
            ++offset_;
        }
        if (offset_ + 4 > size_) {
            offset_ = size_;
        }
        item.kind = Kind::Unreadable;
        item.bytes = offset_ - from;
        return item;
    }

    med::MedicalStorage& storage_;
    std::string path_;
    std::uint64_t size_;
    std::uint16_t channels_;
    std::uint64_t offset_;
    std::vector<unsigned char> window_;
    std::uint64_t windowStart_ = 0;
};

// ===========================================================================
// The plan: what each continuous stretch is, and what is said about it.
// ===========================================================================

struct Note {
    std::uint64_t record = 0;
    std::string text;
};

struct Segment {
    std::uint64_t firstOffset = 0;    ///< byte offset of its first frame
    std::uint64_t records = 0;
    std::uint64_t position = 0;       ///< frames from the session's first sample
    std::int64_t gapBefore = 0;       ///< frames missing before it; -1: unknown
    std::uint64_t failedCrcBefore = 0;
    std::uint64_t unreadableBefore = 0;
    std::vector<Note> starts;
    std::vector<Note> body;
    std::vector<Note> ends;
};

std::string seconds(double value) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.3f s", value);
    return buffer;
}

/// Names of the electrodes behind the bits of a contact mask, as the montage
/// declared them: "Fp1 F3", or channel numbers when nothing was declared.
std::string contactNames(std::uint16_t positive, std::uint16_t negative,
                         const SessionMetadata& meta) {
    const bool referential = meta.montage == "referential";
    std::vector<std::string> names;
    auto add = [&names](const std::string& name) {
        if (std::find(names.begin(), names.end(), name) == names.end()) {
            names.push_back(name);
        }
    };
    for (unsigned c = 0; c < meta.channels && c < 16; ++c) {
        const unsigned bit = 1U << c;
        const bool declared = c < meta.electrodes.size();
        if ((positive & bit) != 0) {
            if (!declared) {
                add(std::to_string(c + 1) + "+");
            } else if (referential) {
                add(meta.electrodes[c]);
            } else {
                add(meta.electrodes[c].substr(0, meta.electrodes[c].find('-')));
            }
        }
        if ((negative & bit) != 0) {
            if (referential) {
                // Every inverting input is the one reference electrode.
                add(meta.reference.empty() ? "REF" : meta.reference);
            } else if (!declared) {
                add(std::to_string(c + 1) + "-");
            } else {
                add(meta.electrodes[c].substr(meta.electrodes[c].find('-') + 1));
            }
        }
    }
    std::string out;
    for (const std::string& name : names) {
        out += (out.empty() ? "" : " ") + name;
    }
    return out;
}

std::string signalLabel(unsigned channel, const SessionMetadata& meta) {
    const bool declared = channel < meta.electrodes.size();
    const std::string number = std::to_string(channel + 1);
    // The one channel that is not an electrode says what it is, and never
    // "EEG": a reader sorts signals by that word. Checked before anything
    // that would give it an electrode's name.
    if (meta.auxiliaryChannel != 0 && channel + 1 == meta.auxiliaryChannel) {
        return meta.auxiliaryLabel + " " + number;
    }
    // With every input shorted the record is the converter's own noise.
    if (meta.input == "shorted") {
        return "Short " + number;
    }
    if (meta.montage == "referential") {
        return "EEG " + (declared ? meta.electrodes[channel] : number) + "-" +
               (meta.reference.empty() ? "REF" : meta.reference);
    }
    return "EEG " + (declared ? meta.electrodes[channel] : number + "+-" + number + "-");
}

/// The smallest annotation space, a whole number of 3-byte samples, with
/// which the writer's carry-forward empties every segment's notes by its last
/// record - found by simulating the writer, using the writer's own sizes.
std::uint32_t annotationSpace(const Segment& segment, double startFraction, double duration) {
    auto onset = [&](std::uint64_t record) {
        return startFraction + static_cast<double>(record) * duration;
    };
    std::vector<std::vector<med::BiosignalAnnotation>> perRecord(segment.records);
    std::size_t largest = 0;
    for (const std::vector<Note>* list : {&segment.starts, &segment.body, &segment.ends}) {
        for (const Note& note : *list) {
            med::BiosignalAnnotation annotation{onset(note.record), -1.0, note.text};
            largest = std::max(largest, med::BiosignalWriter::annotationBytes(annotation));
            perRecord[note.record].push_back(annotation);
        }
    }
    const std::size_t worstTimekeeping =
        med::BiosignalWriter::timekeepingBytes(onset(segment.records));
    std::size_t space = ((worstTimekeeping + largest + 2) / 3) * 3;
    for (;;) {
        std::vector<std::size_t> queue;
        std::size_t head = 0;
        for (std::uint64_t r = 0; r < segment.records; ++r) {
            for (const med::BiosignalAnnotation& a : perRecord[r]) {
                queue.push_back(med::BiosignalWriter::annotationBytes(a));
            }
            std::size_t used = med::BiosignalWriter::timekeepingBytes(onset(r));
            while (head < queue.size() && used + queue[head] <= space) {
                used += queue[head++];
            }
        }
        if (head == queue.size()) {
            return static_cast<std::uint32_t>(space);
        }
        space += 3;
    }
}

}  // namespace

// ===========================================================================

med::Result<SessionMetadata> parseMetadata(const std::string& text) {
    using MetaResult = med::Result<SessionMetadata>;
    Json root;
    if (!JsonParser(text).parse(root) || root.kind != Json::Kind::Object) {
        return MetaResult::fail(med::Status::InvalidArgument, "metadata.json is not JSON");
    }

    SessionMetadata meta;
    std::string missing;
    auto str = [&](const char* key, std::string& out, bool required) {
        const Json* v = root.get(key);
        if (v != nullptr && v->kind == Json::Kind::String) {
            out = v->text;
        } else if (required) {
            missing += std::string(missing.empty() ? "" : ", ") + key;
        }
    };
    auto num = [&](const char* key, double& out) {
        const Json* v = root.get(key);
        if (v != nullptr && v->kind == Json::Kind::Number) {
            out = v->number;
        } else {
            missing += std::string(missing.empty() ? "" : ", ") + key;
        }
    };

    str("session", meta.session, true);
    str("device_id", meta.deviceId, true);
    str("link", meta.link, true);
    str("software_version", meta.softwareVersion, true);
    str("unit", meta.unit, true);
    str("montage", meta.montage, true);
    str("utc_offset", meta.utcOffset, true);
    str("reference", meta.reference, false);
    str("prefiltering", meta.prefiltering, false);
    // Absent in a record written before these existed: electrodes on every
    // channel, in microvolts, which is what every such record was.
    str("input", meta.input, false);
    str("auxiliary_label", meta.auxiliaryLabel, false);
    str("auxiliary_unit", meta.auxiliaryUnit, false);
    double auxiliaryChannel = 0.0;
    for (const auto& field : {std::make_pair("auxiliary_channel", &auxiliaryChannel),
                              std::make_pair("auxiliary_uv_per_unit", &meta.uvPerUnit),
                              std::make_pair("auxiliary_uv_at_reference", &meta.uvAtReference),
                              std::make_pair("auxiliary_reference", &meta.auxiliaryReference)}) {
        const Json* v = root.get(field.first);
        if (v != nullptr && v->kind == Json::Kind::Number) {
            *field.second = v->number;
        } else if (v != nullptr && v->kind != Json::Kind::Null) {
            return MetaResult::fail(med::Status::InvalidArgument,
                                    std::string("metadata.json: ") + field.first +
                                        " is not a number");
        }
    }
    double channels = 0.0, bits = 0.0;
    num("channels", channels);
    num("sample_rate_hz", meta.sampleRateHz);
    num("full_scale_uv", meta.fullScaleUv);
    num("resolution_bits", bits);
    {
        const Json* v = root.get("clock_synchronized");
        if (v != nullptr && v->kind == Json::Kind::Bool) {
            meta.clockSynchronized = v->boolean;
        } else {
            missing += std::string(missing.empty() ? "" : ", ") + "clock_synchronized";
        }
    }
    if (!missing.empty()) {
        // A session recorded before the record declared its scale (D7) lands
        // here, and is refused rather than given a scale from elsewhere.
        return MetaResult::fail(med::Status::InvalidArgument,
                                "metadata.json lacks " + missing);
    }

    if (const Json* list = root.get("electrodes")) {
        if (list->kind == Json::Kind::Array) {
            for (const Json& item : list->items) {
                if (item.kind != Json::Kind::String) {
                    return MetaResult::fail(med::Status::InvalidArgument,
                                            "metadata.json: an electrode name is not a string");
                }
                meta.electrodes.push_back(item.text);
            }
        }
    }
    if (const Json* prescription = root.get("prescription")) {
        if (const Json* test = prescription->get("afe.test_signal")) {
            meta.testSignal = test->text;
        }
    }

    if (channels < 1 || channels > 16 || channels != std::floor(channels)) {
        return MetaResult::fail(med::Status::InvalidArgument,
                                "metadata.json: channels must be 1..16");
    }
    meta.channels = static_cast<std::uint16_t>(channels);
    meta.resolutionBits = static_cast<int>(bits);
    if (bits != meta.resolutionBits || meta.resolutionBits < 2 || meta.resolutionBits > 24) {
        // BDF+ samples are 24 bits; a deeper converter needs another format.
        return MetaResult::fail(med::Status::InvalidArgument,
                                "metadata.json: resolution_bits must be 2..24");
    }
    if (!(meta.fullScaleUv > 0.0) || !(meta.sampleRateHz > 0.0)) {
        return MetaResult::fail(med::Status::InvalidArgument,
                                "metadata.json: full scale and sample rate must be positive");
    }
    if (meta.montage != "bipolar" && meta.montage != "referential") {
        return MetaResult::fail(med::Status::InvalidArgument,
                                "metadata.json: unknown montage '" + meta.montage + "'");
    }
    if (!meta.electrodes.empty() && meta.electrodes.size() != meta.channels) {
        return MetaResult::fail(med::Status::InvalidArgument,
                                "metadata.json: electrodes do not match the channels");
    }
    if (meta.input != "normal" && meta.input != "shorted") {
        return MetaResult::fail(med::Status::InvalidArgument,
                                "metadata.json: unknown input '" + meta.input + "'");
    }
    // A record cannot be both: electrode names are the labels of an EEG, and
    // shorted inputs did not record one. Whichever of the two statements is
    // wrong, the file that would come out of them carries a scalp position
    // over a converter's noise.
    if (meta.input != "normal" && (!meta.electrodes.empty() || !meta.reference.empty())) {
        return MetaResult::fail(med::Status::InvalidArgument,
                                "metadata.json names electrodes, and the input was '" +
                                    meta.input + "': the record is not of electrodes");
    }

    if (auxiliaryChannel != std::floor(auxiliaryChannel) || auxiliaryChannel < 0 ||
        auxiliaryChannel > meta.channels) {
        return MetaResult::fail(med::Status::InvalidArgument,
                                "metadata.json: the auxiliary channel is not one of the channels");
    }
    meta.auxiliaryChannel = static_cast<unsigned>(auxiliaryChannel);
    if (meta.auxiliaryChannel != 0) {
        if (!(meta.uvPerUnit > 0.0) || !std::isfinite(meta.uvAtReference) ||
            !std::isfinite(meta.auxiliaryReference) || meta.auxiliaryUnit.empty()) {
            return MetaResult::fail(med::Status::InvalidArgument,
                                    "metadata.json: the auxiliary channel's conversion is not "
                                    "usable");
        }
        // Its label is the one thing that keeps a reader from taking it for
        // an electrode. Empty, or the very word it must not be, is refused.
        if (meta.auxiliaryLabel.empty() || meta.auxiliaryLabel.rfind("EEG", 0) == 0) {
            return MetaResult::fail(med::Status::InvalidArgument,
                                    "metadata.json: the auxiliary channel is labelled '" +
                                        meta.auxiliaryLabel +
                                        "', and it is not an EEG channel");
        }
        if (meta.input != "normal") {
            return MetaResult::fail(med::Status::InvalidArgument,
                                    "metadata.json: an auxiliary channel and shorted inputs: "
                                    "that channel cannot be both");
        }
    }
    return MetaResult::ok(std::move(meta));
}

med::Result<ExportSummary> exportSession(med::MedicalStorage& storage,
                                         const std::string& sessionDir, bool interrupted) {
    using ExportResult = med::Result<ExportSummary>;

    // ------------------------------------------------------------ metadata
    med::Result<std::string> text = storage.readString(sessionDir + "/metadata.json");
    if (!text) {
        return ExportResult::fail(text.status(), "metadata.json: " + text.message());
    }
    med::Result<SessionMetadata> parsed = parseMetadata(text.value());
    if (!parsed) {
        return ExportResult::fail(parsed.status(), parsed.message());
    }
    const SessionMetadata meta = parsed.take();

    std::int64_t sessionMicros = 0;
    std::int64_t offsetSeconds = 0;
    if (!parseSessionTime(meta.session, sessionMicros) ||
        !parseOffset(meta.utcOffset, offsetSeconds)) {
        return ExportResult::fail(med::Status::InvalidArgument,
                                  "metadata.json: session time or UTC offset unreadable");
    }

    // D7: the step the samples must be whole multiples of.
    const std::int64_t codeLimit = 1LL << (meta.resolutionBits - 1);
    const double lsbNv = meta.fullScaleUv * 1000.0 / static_cast<double>(codeLimit);

    const std::string raw = sessionDir + "/raw.bin";
    med::Result<std::uint64_t> rawSize = storage.size(raw);
    if (!rawSize) {
        return ExportResult::fail(rawSize.status(), "raw.bin: " + rawSize.message());
    }

    // -------------------------------------------------------------- plan
    ExportSummary summary;
    summary.interrupted = interrupted;
    std::vector<Segment> segments;
    std::uint32_t samplesPerChannel = 0;
    std::uint64_t lastSequence = 0;
    std::uint64_t crcSinceFrame = 0;
    std::uint64_t unreadableSinceFrame = 0;
    med::LeadOff contact;
    bool first = true;

    FrameScanner scanner(storage, raw, rawSize.value(), meta.channels, 0);
    for (;;) {
        const std::uint64_t at = scanner.offset();
        FrameScanner::Item item = scanner.next();
        if (item.kind == FrameScanner::Kind::End) {
            break;
        }
        if (item.kind == FrameScanner::Kind::Error) {
            return ExportResult::fail(item.status, "raw.bin: " + item.message);
        }
        if (item.kind == FrameScanner::Kind::BadFrame) {
            ++summary.failedCrc;
            ++crcSinceFrame;
            continue;
        }
        if (item.kind == FrameScanner::Kind::Unreadable) {
            unreadableSinceFrame += item.bytes;
            continue;
        }
        if (item.kind == FrameScanner::Kind::Partial) {
            summary.partialBytes = item.bytes;
            continue;
        }

        const med::SampleFrame& frame = item.frame;
        if (first) {
            samplesPerChannel = frame.samplesPerChannel;
        } else if (frame.samplesPerChannel != samplesPerChannel) {
            return ExportResult::fail(med::Status::InvalidArgument,
                                      "raw.bin changes frame geometry at byte " +
                                          std::to_string(at) +
                                          ": one clinical file has one record duration");
        }

        // D7, every sample: a whole number of converter steps, within range.
        for (const double microvolts : frame.samples) {
            const double nanovolts = std::round(microvolts * 1000.0);
            const double code = std::round(nanovolts / lsbNv);
            if (std::fabs(nanovolts - code * lsbNv) > 0.5 + 1e-9 ||
                code < -static_cast<double>(codeLimit) || code > static_cast<double>(codeLimit - 1)) {
                return ExportResult::fail(
                    med::Status::IntegrityError,
                    "sample of frame " + std::to_string(frame.sequence) + " (" +
                        std::to_string(nanovolts) + " nV) is not a whole number of steps of " +
                        std::to_string(lsbNv) +
                        " nV: the record does not match the scale its metadata declares");
            }
        }

        // Where it falls: the same stretch, or a new one after a gap.
        if (first || frame.sequence != lastSequence + 1) {
            Segment segment;
            segment.firstOffset = at;
            if (first) {
                segment.gapBefore = 0;
            } else {
                const Segment& previous = segments.back();
                if (frame.sequence > lastSequence) {
                    segment.gapBefore = static_cast<std::int64_t>(frame.sequence - lastSequence - 1);
                    summary.missing += static_cast<std::uint64_t>(segment.gapBefore);
                } else {
                    // The producer's sequence went backwards: it restarted,
                    // and how long it was away is not in the record.
                    segment.gapBefore = -1;
                    ++summary.restarts;
                }
                segment.position = previous.position + previous.records +
                                   static_cast<std::uint64_t>(std::max<std::int64_t>(segment.gapBefore, 0));
            }
            segment.failedCrcBefore = crcSinceFrame;
            segment.unreadableBefore = unreadableSinceFrame;
            segments.push_back(segment);
        }
        crcSinceFrame = 0;
        unreadableSinceFrame = 0;
        Segment& segment = segments.back();

        // Electrode contact, when it changes - not per record, like the audit.
        const med::LeadOff& now = frame.leadOff;
        if (first ? now.monitored() : now != contact) {
            std::string what;
            if (!now.monitored()) {
                what = "electrode contact: not monitored";
            } else if (now.detachedPositive() == 0 && now.detachedNegative() == 0) {
                what = "electrode contact: all attached";
            } else {
                what = "electrode contact: detached " +
                       contactNames(now.detachedPositive(), now.detachedNegative(), meta);
            }
            segment.body.push_back({segment.records, what});
        }
        contact = now;

        ++segment.records;
        ++summary.frames;
        lastSequence = frame.sequence;
        first = false;
    }

    if (segments.empty()) {
        return ExportResult::fail(med::Status::InvalidArgument,
                                  "raw.bin holds no valid frame: there is nothing to convert");
    }

    const double duration = static_cast<double>(samplesPerChannel) / meta.sampleRateHz;

    // What is said at the start and the end of each stretch.
    for (std::size_t k = 0; k < segments.size(); ++k) {
        Segment& s = segments[k];
        s.starts.push_back({0, "link " + meta.link});
        s.starts.push_back({0, meta.montage == "referential"
                                   ? "montage referential, reference " +
                                         (meta.reference.empty() ? std::string("REF")
                                                                 : meta.reference)
                                   : std::string("montage bipolar")});
        if (meta.input != "normal") {
            // Nor a measurement of the converter itself. The labels already
            // say so; this says it to a reader that shows annotations first.
            s.starts.push_back({0, "input " + meta.input + ", not EEG"});
        }
        if (meta.auxiliaryChannel != 0) {
            s.starts.push_back({0, "channel " + std::to_string(meta.auxiliaryChannel) + " is " +
                                       meta.auxiliaryLabel + " in " + meta.auxiliaryUnit +
                                       ", not EEG"});
        }
        if (meta.testSignal == "internal") {
            // A generator trace must never be read as EEG.
            s.starts.push_back({0, "test signal internal"});
        }
        if (!meta.clockSynchronized) {
            s.starts.push_back({0, "clock not synchronized"});
        }
        s.starts.push_back({0, "UTC offset " + meta.utcOffset});
        if (k > 0) {
            std::string after =
                s.gapBefore < 0
                    ? std::string("after sequence restart, gap unknown")
                    : "after gap: " + std::to_string(s.gapBefore) + " frames (" +
                          seconds(static_cast<double>(s.gapBefore) * duration) + ")";
            if (s.failedCrcBefore > 0) {
                after += ", " + std::to_string(s.failedCrcBefore) + " failed CRC";
            }
            if (s.unreadableBefore > 0) {
                after += ", " + std::to_string(s.unreadableBefore) + " bytes unreadable";
            }
            s.starts.push_back({0, after});
        }
        const std::uint64_t last = s.records - 1;
        if (k + 1 < segments.size()) {
            const Segment& next = segments[k + 1];
            s.ends.push_back({last, next.gapBefore < 0
                                        ? std::string("sequence restart follows")
                                        : "gap follows: " + std::to_string(next.gapBefore) +
                                              " frames (" +
                                              seconds(static_cast<double>(next.gapBefore) *
                                                      duration) +
                                              ")"});
        } else {
            // What followed the last valid frame and is not in any record.
            if (crcSinceFrame > 0) {
                s.ends.push_back({last, std::to_string(crcSinceFrame) +
                                            " frames failed CRC after the last valid one"});
            }
            if (unreadableSinceFrame > 0) {
                s.ends.push_back({last, std::to_string(unreadableSinceFrame) +
                                            " bytes unreadable after the last valid frame"});
            }
            if (summary.partialBytes > 0) {
                s.ends.push_back({last, "partial frame discarded (" +
                                            std::to_string(summary.partialBytes) + " bytes)"});
            }
            if (interrupted) {
                s.ends.push_back({last, "record interrupted"});
            }
        }
    }

    // ------------------------------------------------------------- write
    // Nothing from an earlier attempt survives: a file is complete only with
    // its sidecar, and an earlier attempt may have planned other names.
    {
        med::Result<std::vector<std::string>> entries = storage.list(sessionDir);
        if (!entries) {
            return ExportResult::fail(entries.status(), entries.message());
        }
        for (const std::string& name : entries.value()) {
            if (name.rfind("record", 0) == 0 && name.find(".bdf") != std::string::npos) {
                const med::Status removed = storage.remove(sessionDir + "/" + name);
                if (removed != med::Status::Ok) {
                    return ExportResult::fail(removed, "cannot remove stale " + name);
                }
            }
        }
    }

    const double lsbUv = lsbNv / 1000.0;
    const double frameMicros = duration * 1e6;
    const std::int64_t localMicros = sessionMicros + offsetSeconds * 1000000LL;

    for (std::size_t k = 0; k < segments.size(); ++k) {
        const Segment& s = segments[k];

        const std::int64_t start =
            localMicros + std::llround(static_cast<double>(s.position) * frameMicros);
        const std::int64_t wholeSeconds = floorDiv(start, 1000000LL);
        const std::time_t asTime = static_cast<std::time_t>(wholeSeconds);
        std::tm broken{};
        gmtime_r(&asTime, &broken);  // already local: the offset was added above

        med::BiosignalHeader header;
        header.patient = "X X X X";
        header.recording = meta.session + " X MedPlatform_" + meta.deviceId + "_" +
                           meta.softwareVersion;
        header.year = broken.tm_year + 1900;
        header.month = broken.tm_mon + 1;
        header.day = broken.tm_mday;
        header.hour = broken.tm_hour;
        header.minute = broken.tm_min;
        header.second = broken.tm_sec;
        header.startFraction = static_cast<double>(start - wholeSeconds * 1000000LL) / 1e6;
        header.recordDurationSeconds = duration;
        header.samplesPerRecord = samplesPerChannel;
        header.continuous = true;
        header.recordCount = s.records;
        for (unsigned c = 0; c < meta.channels; ++c) {
            med::BiosignalSignal signal;
            signal.label = signalLabel(c, meta);
            // The samples stay the converter's codes; what the header says
            // they mean is the record's own declaration. For every channel
            // but the auxiliary one that is microvolts, as it always was.
            const double lowUv = -static_cast<double>(codeLimit) * lsbUv;
            const double highUv = static_cast<double>(codeLimit - 1) * lsbUv;
            if (meta.auxiliaryChannel != 0 && c + 1 == meta.auxiliaryChannel) {
                signal.physicalDimension = meta.auxiliaryUnit;
                signal.physicalMinimum =
                    (lowUv - meta.uvAtReference) / meta.uvPerUnit + meta.auxiliaryReference;
                signal.physicalMaximum =
                    (highUv - meta.uvAtReference) / meta.uvPerUnit + meta.auxiliaryReference;
            } else {
                signal.physicalDimension = meta.unit;
                signal.physicalMinimum = lowUv;
                signal.physicalMaximum = highUv;
            }
            signal.digitalMinimum = static_cast<std::int32_t>(-codeLimit);
            signal.digitalMaximum = static_cast<std::int32_t>(codeLimit - 1);
            signal.prefiltering = meta.prefiltering;
            header.signals.push_back(signal);
        }
        header.annotationBytesPerRecord =
            annotationSpace(s, header.startFraction, header.recordDurationSeconds);

        char name[32];
        if (segments.size() == 1) {
            std::snprintf(name, sizeof(name), "record.bdf");
        } else {
            std::snprintf(name, sizeof(name), "record-%03u.bdf", static_cast<unsigned>(k + 1));
        }

        med::Result<std::unique_ptr<med::BiosignalWriter>> made =
            med::BiosignalWriter::create(storage, sessionDir + "/" + name, header);
        if (!made) {
            return ExportResult::fail(made.status(), std::string(name) + ": " + made.message());
        }
        med::BiosignalWriter& writer = *made.value();

        auto onset = [&](std::uint64_t record) {
            return header.startFraction + static_cast<double>(record) * duration;
        };
        std::vector<std::vector<med::BiosignalAnnotation>> notes(s.records);
        for (const std::vector<Note>* list : {&s.starts, &s.body, &s.ends}) {
            for (const Note& note : *list) {
                notes[note.record].push_back({onset(note.record), -1.0, note.text});
            }
        }

        FrameScanner reader(storage, raw, rawSize.value(), meta.channels, s.firstOffset);
        std::vector<std::int32_t> codes(static_cast<std::size_t>(samplesPerChannel) *
                                        meta.channels);
        std::uint64_t record = 0;
        while (record < s.records) {
            FrameScanner::Item item = reader.next();
            if (item.kind == FrameScanner::Kind::Error) {
                return ExportResult::fail(item.status, "raw.bin: " + item.message);
            }
            if (item.kind == FrameScanner::Kind::End) {
                return ExportResult::fail(med::Status::IoError,
                                          "raw.bin ended before the planned records");
            }
            if (item.kind != FrameScanner::Kind::Frame) {
                continue;
            }
            for (std::size_t i = 0; i < codes.size(); ++i) {
                const double nanovolts = std::round(item.frame.samples[i] * 1000.0);
                codes[i] = static_cast<std::int32_t>(std::llround(nanovolts / lsbNv));
            }
            const med::Status written =
                writer.appendRecord(onset(record), codes.data(), notes[record]);
            if (written != med::Status::Ok) {
                return ExportResult::fail(written, std::string(name) + ": record " +
                                                       std::to_string(record) + " refused");
            }
            ++record;
        }

        const med::Status finished = writer.finish();
        if (finished != med::Status::Ok) {
            return ExportResult::fail(finished, std::string(name) + ": could not be completed");
        }
        summary.files.push_back({name, writer.digest(), s.records});
    }

    return ExportResult::ok(std::move(summary));
}

std::string exportMarker(const ExportSummary& summary) {
    std::string out = "{\n  \"status\": \"converted\",\n  \"files\": [";
    for (std::size_t i = 0; i < summary.files.size(); ++i) {
        const ExportedFile& file = summary.files[i];
        out += std::string(i == 0 ? "\n" : ",\n") + "    {\"name\": \"" + file.name +
               "\", \"sha256\": \"" + file.sha256 +
               "\", \"records\": " + std::to_string(file.records) + "}";
    }
    out += "\n  ],\n";
    out += "  \"frames\": " + std::to_string(summary.frames) + ",\n";
    out += "  \"failed_crc\": " + std::to_string(summary.failedCrc) + ",\n";
    out += "  \"missing\": " + std::to_string(summary.missing) + ",\n";
    out += "  \"restarts\": " + std::to_string(summary.restarts) + ",\n";
    out += "  \"partial_bytes\": " + std::to_string(summary.partialBytes) + ",\n";
    out += std::string("  \"interrupted\": ") + (summary.interrupted ? "true" : "false") + "\n";
    out += "}\n";
    return out;
}

std::string refusalMarker(const std::string& reason) {
    std::string escaped;
    for (const char c : reason) {
        if (c == '"' || c == '\\') {
            escaped += '\\';
        }
        escaped += (static_cast<unsigned char>(c) < 0x20) ? ' ' : c;
    }
    return "{\n  \"status\": \"refused\",\n  \"reason\": \"" + escaped + "\"\n}\n";
}

}  // namespace eeg
