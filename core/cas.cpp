// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Alonso (GS·RUN). MSXFileForge.
// Free software under the GNU GPL v3 (or later) - see LICENSE. Modified
// versions must be distributed under the same license.
#include "cas.h"
#include <cstring>

namespace msx {

const char* casKindName(CasBlockKind k) {
    switch (k) {
        case CasBlockKind::Header:         return "Header";
        case CasBlockKind::Ascii:          return "ASCII file";
        case CasBlockKind::Binary:         return "Binary file";
        case CasBlockKind::BasicTokenized: return "Tokenized BASIC";
        default:                           return "Custom block";
    }
}

// The 8-byte block sync marker that begins every .cas block.
static const uint8_t kSync[8] = {0x1F,0xA6,0xDE,0xBA,0xCC,0x13,0x7D,0x74};

static bool matchSync(const std::vector<uint8_t>& d, size_t p) {
    return p + 8 <= d.size() && memcmp(&d[p], kSync, 8) == 0;
}

std::vector<CasBlock> parseCas(const std::vector<uint8_t>& d) {
    std::vector<CasBlock> blocks;
    // Collect all sync positions first (blocks are 8-byte aligned in real .cas,
    // but we scan byte-wise to tolerate hand-made images).
    std::vector<size_t> syncs;
    for (size_t p = 0; p + 8 <= d.size(); ++p)
        if (matchSync(d, p)) { syncs.push_back(p); p += 7; }

    for (size_t s = 0; s < syncs.size(); ++s) {
        size_t p = syncs[s];
        CasBlock b;
        b.offset = (long)p;
        size_t payload = p + 8;
        // A standard file header carries 10 identical marker bytes then a
        // 6-char filename.
        if (payload + 16 <= d.size()) {
            uint8_t m = d[payload];
            bool ten = true;
            for (int i = 0; i < 10; ++i) if (d[payload + i] != m) { ten = false; break; }
            if (ten && (m == 0xEA || m == 0xD0 || m == 0xD3)) {
                b.kind = (m == 0xEA) ? CasBlockKind::Ascii
                       : (m == 0xD0) ? CasBlockKind::Binary
                                     : CasBlockKind::BasicTokenized;
                b.filename.assign((const char*)&d[payload + 10], 6);
                // trim trailing spaces/nuls
                while (!b.filename.empty() &&
                       (b.filename.back() == ' ' || b.filename.back() == 0))
                    b.filename.pop_back();
                b.dataOffset = (long)(payload + 16);
            } else {
                b.kind = CasBlockKind::Custom;
                b.dataOffset = (long)payload;
            }
        } else {
            b.dataOffset = (long)payload;
        }
        long end = (s + 1 < syncs.size()) ? (long)syncs[s + 1] : (long)d.size();
        b.dataLength = end - b.dataOffset;
        if (b.dataLength < 0) b.dataLength = 0;
        blocks.push_back(b);
    }
    return blocks;
}

} // namespace msx
