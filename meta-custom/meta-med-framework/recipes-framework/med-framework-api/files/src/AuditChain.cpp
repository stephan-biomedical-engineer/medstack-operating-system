// SPDX-License-Identifier: MIT

#include "AuditChain.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>

#include "MedDigest.h"

namespace med {
namespace audit {

const char* const kDuplicateMarker = "__MED_VERIFY_DUPLICATE";

const char* const kGenesis =
    "0000000000000000000000000000000000000000000000000000000000000000";

bool covered(const std::string& name) {
    if (name == "MESSAGE") {
        return true;
    }
    return name.compare(0, 4, "MED_") == 0 && name != "MED_AUDIT_HASH";
}

std::string canonical(const Fields& fields) {
    std::string out;
    for (const auto& field : fields) {  // std::map: sorted by name
        if (!covered(field.first)) {
            continue;
        }
        out.append(field.first);
        out.push_back('=');
        out.append(std::to_string(field.second.size()));
        out.push_back(':');
        out.append(field.second);
        out.push_back('\n');
    }
    return out;
}

std::string digest(const std::string& prev, const Fields& fields) {
    return internal::sha256Hex(prev + "\n" + canonical(fields));
}

Result<std::vector<Fields>> parseExport(const std::string& data, std::uint64_t* duplicates) {
    using R = Result<std::vector<Fields>>;
    std::vector<Fields> entries;
    Fields current;
    std::uint64_t repeated = 0;
    std::size_t at = 0;

    auto keep = [&](std::string name, std::string value) {
        if (!current.emplace(std::move(name), std::move(value)).second) {
            ++repeated;
            current[kDuplicateMarker] = "1";
        }
    };

    while (at < data.size()) {
        const std::size_t eol = data.find('\n', at);
        if (eol == std::string::npos) {
            return R::fail(Status::IntegrityError, "export stream ends inside a line");
        }
        if (eol == at) {
            // Empty line: the entry is complete.
            if (!current.empty()) {
                entries.push_back(std::move(current));
                current.clear();
            }
            at = eol + 1;
            continue;
        }
        const std::string line = data.substr(at, eol - at);
        const std::size_t equals = line.find('=');
        if (equals != std::string::npos) {
            keep(line.substr(0, equals), line.substr(equals + 1));
            at = eol + 1;
            continue;
        }
        // Binary form: NAME '\n' le64 length, bytes, '\n'.
        at = eol + 1;
        if (data.size() - at < 8) {
            return R::fail(Status::IntegrityError, "export stream truncated in a length");
        }
        std::uint64_t length = 0;
        for (int i = 7; i >= 0; --i) {
            length = (length << 8) | static_cast<unsigned char>(data[at + static_cast<std::size_t>(i)]);
        }
        at += 8;
        if (length > data.size() - at || data.size() - at - length < 1 ||
            data[at + length] != '\n') {
            return R::fail(Status::IntegrityError, "export stream truncated in a value");
        }
        keep(line, data.substr(at, length));
        at += length + 1;
    }
    if (!current.empty()) {
        entries.push_back(std::move(current));
    }
    if (duplicates != nullptr) {
        *duplicates = repeated;
    }
    return R::ok(std::move(entries));
}

namespace {

bool parseSequence(const std::string& text, std::uint64_t& out) {
    if (text.empty() || text.size() > 20) {
        return false;
    }
    for (const char c : text) {
        if (c < '0' || c > '9') {
            return false;
        }
    }
    errno = 0;
    const unsigned long long value = std::strtoull(text.c_str(), nullptr, 10);
    if (errno != 0) {
        return false;
    }
    out = static_cast<std::uint64_t>(value);
    return true;
}

const std::string* get(const Fields& fields, const char* name) {
    const auto it = fields.find(name);
    return it == fields.end() ? nullptr : &it->second;
}

struct Chain {
    ComponentReport report;
    bool started = false;
    std::uint64_t lastSequence = 0;
    std::string lastHash;
};

}  // namespace

Report verify(const std::vector<Fields>& entries) {
    Report report;
    std::map<std::string, Chain> chains;
    std::vector<std::string> order;  // components, in order of first appearance

    for (const Fields& entry : entries) {
        const std::string* isAudit = get(entry, "MED_AUDIT");
        if (isAudit == nullptr || *isAudit != "1") {
            continue;
        }
        ++report.entries;

        const std::string* component = get(entry, "MED_COMPONENT");
        const std::string name = component ? *component : std::string();
        if (chains.find(name) == chains.end()) {
            order.push_back(name);
            chains[name].report.component = name;
        }
        Chain& chain = chains[name];
        ++chain.report.records;

        const std::string* seqText = get(entry, "MED_AUDIT_SEQ");
        const std::string* prev = get(entry, "MED_AUDIT_PREV");
        const std::string* hash = get(entry, "MED_AUDIT_HASH");
        std::uint64_t seq = 0;
        if (component == nullptr || seqText == nullptr || prev == nullptr || hash == nullptr ||
            !parseSequence(*seqText, seq)) {
            report.breaks.push_back({name, seq, "malformed: a chain field is missing or unreadable"});
            continue;
        }

        // The logger never writes a field twice, so a repeated one was added
        // after the fact - and only one of its values could have been hashed.
        if (entry.count(kDuplicateMarker) != 0) {
            report.breaks.push_back({name, seq, "a field appears twice in the record"});
        }

        // Linkage.
        const bool genesis = seq == 1 && *prev == kGenesis;
        if (!chain.started) {
            chain.started = true;
            chain.report.firstSequence = seq;
        } else if (genesis) {
            ++chain.report.restarts;
        } else {
            if (seq != chain.lastSequence + 1) {
                report.breaks.push_back(
                    {name, seq,
                     "sequence: expected " + std::to_string(chain.lastSequence + 1) + ", got " +
                         std::to_string(seq) + " (a record removed, or records reordered)"});
            } else if (*prev != chain.lastHash) {
                report.breaks.push_back(
                    {name, seq, "linkage: PREV is not the previous record's HASH"});
            }
        }

        // Content.
        const std::string* format = get(entry, "MED_AUDIT_FORMAT");
        if (format == nullptr) {
            ++chain.report.legacy;
        } else if (*format == kFormat) {
            if (digest(*prev, entry) == *hash) {
                ++chain.report.verified;
            } else {
                report.breaks.push_back(
                    {name, seq, "content: the digest does not match the record (a field altered)"});
            }
        } else {
            report.breaks.push_back({name, seq, "format " + *format + " is not known to this verifier"});
        }

        chain.lastSequence = seq;
        chain.lastHash = *hash;
    }

    for (const std::string& name : order) {
        report.components.push_back(chains[name].report);
    }
    return report;
}

}  // namespace audit
}  // namespace med
