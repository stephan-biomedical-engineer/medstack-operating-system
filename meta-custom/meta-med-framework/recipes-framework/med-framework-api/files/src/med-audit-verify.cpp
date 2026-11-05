// SPDX-License-Identifier: MIT
//
// med-audit-verify - replay the MedFramework audit hash chain.
//
//   journalctl -o export MED_AUDIT=1 | med-audit-verify
//   journalctl -D /mnt/data/journal -o export | med-audit-verify   (offline)
//   med-audit-verify FILE
//
// Reads journalctl's export format, so it needs no journal library and runs
// the same on the device, on a host reading a device's disk, and in the host
// test suite. Prints one line per component and one per break:
//
//   component=NAME records=N verified=N legacy=N restarts=N first_seq=N
//   break component=NAME seq=N what=...
//   entries=N components=N breaks=N
//
// Exit status: 0 no break, 1 at least one break, 2 the input could not be read.
//
// What it proves, and what it does not: that the records it read form unbroken
// chains whose content matches their digests. Not that nothing was removed
// from the END of a chain (a truncated tail looks like a chain that stopped),
// and not that the chain was not recomputed by someone with root - the chain
// is an integrity witness, not a signature. The journal's own seal is the
// mechanism meant for the second.

#include <cstdio>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>

#include "AuditChain.h"

int main(int argc, char** argv) {
    std::string data;
    if (argc > 2 || (argc == 2 && std::string(argv[1]) == "--help")) {
        std::fprintf(stderr, "usage: med-audit-verify [FILE|-]   (journalctl -o export)\n");
        return 2;
    }
    if (argc == 2 && std::string(argv[1]) != "-") {
        std::ifstream in(argv[1], std::ios::binary);
        if (!in) {
            std::fprintf(stderr, "med-audit-verify: cannot read %s\n", argv[1]);
            return 2;
        }
        data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    } else {
        std::ostringstream all;
        all << std::cin.rdbuf();
        data = all.str();
    }

    std::uint64_t duplicates = 0;
    med::Result<std::vector<med::audit::Fields>> entries =
        med::audit::parseExport(data, &duplicates);
    if (!entries) {
        std::fprintf(stderr, "med-audit-verify: %s\n", entries.message().c_str());
        return 2;
    }

    const med::audit::Report report = med::audit::verify(entries.value());
    for (const auto& c : report.components) {
        std::printf("component=%s records=%llu verified=%llu legacy=%llu restarts=%llu first_seq=%llu\n",
                    c.component.c_str(), static_cast<unsigned long long>(c.records),
                    static_cast<unsigned long long>(c.verified),
                    static_cast<unsigned long long>(c.legacy),
                    static_cast<unsigned long long>(c.restarts),
                    static_cast<unsigned long long>(c.firstSequence));
    }
    for (const auto& b : report.breaks) {
        std::printf("break component=%s seq=%llu what=%s\n", b.component.c_str(),
                    static_cast<unsigned long long>(b.sequence), b.what.c_str());
    }
    std::printf("entries=%llu components=%zu breaks=%zu duplicate_fields=%llu\n",
                static_cast<unsigned long long>(report.entries), report.components.size(),
                report.breaks.size(), static_cast<unsigned long long>(duplicates));
    return report.breaks.empty() ? 0 : 1;
}
