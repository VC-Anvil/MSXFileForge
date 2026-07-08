// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Alonso (GS·RUN). MSXFileForge.
// Free software under the GNU GPL v3 (or later) - see LICENSE. Modified
// versions must be distributed under the same license.
#include "rom.h"

namespace msx {

const char* mapperName(RomMapper m) {
    switch (m) {
        case RomMapper::None:      return "None (plain ROM)";
        case RomMapper::Konami:    return "Konami";
        case RomMapper::KonamiSCC: return "Konami SCC";
        case RomMapper::ASCII8:    return "ASCII8";
        case RomMapper::ASCII16:   return "ASCII16";
        default:                   return "Unknown";
    }
}

static uint16_t rd16(const std::vector<uint8_t>& d, size_t o) {
    return (o + 1 < d.size()) ? (uint16_t)(d[o] | (d[o+1] << 8)) : 0;
}

// openMSX-style write-target tally: scan for `LD (nnnn),A` (0x32 lo hi) aimed
// at each mapper's bank registers and pick the strongest match.
static RomMapper guessMapper(const std::vector<uint8_t>& d) {
    if (d.size() <= 0x8000 &&
        ((d.size() >= 2 && d[0] == 'A' && d[1] == 'B') ||
         (d.size() > 0x4000 && d[0x4000] == 'A' && d[0x4001] == 'B'))) {
        // <=32K with a ROM header and no need to bank -> plain.
        if (d.size() <= 0x8000) return RomMapper::None;
    }
    int konami = 0, scc = 0, ascii8 = 0, ascii16 = 0;
    for (size_t i = 0; i + 2 < d.size(); ++i) {
        if (d[i] != 0x32) continue;               // LD (nnnn),A
        uint16_t a = d[i+1] | (d[i+2] << 8);
        // Addresses are shared across mapper families, so tally independently.
        if (a == 0x6000) { konami++; ascii8++; ascii16++; }
        if (a == 0x8000 || a == 0xA000) konami++;
        if (a == 0x5000 || a == 0x9000 || a == 0xB000) scc++;
        if (a == 0x7000) { scc++; ascii8++; ascii16++; }
        if (a == 0x6800 || a == 0x7800) ascii8++;
    }
    int best = scc; RomMapper m = RomMapper::KonamiSCC;
    if (konami > best) { best = konami; m = RomMapper::Konami; }
    if (ascii8 > best) { best = ascii8; m = RomMapper::ASCII8; }
    if (ascii16 > best) { best = ascii16; m = RomMapper::ASCII16; }
    if (best == 0) {
        if (d.size() <= 0x8000) return RomMapper::None;
        return RomMapper::Unknown;
    }
    return m;
}

RomInfo analyzeRom(const std::vector<uint8_t>& d) {
    RomInfo r;
    r.size = (long)d.size();
    r.pages16k = (int)((d.size() + 0x3FFF) / 0x4000);
    r.hasHeaderAt0    = d.size() >= 2 && d[0] == 'A' && d[1] == 'B';
    r.hasHeaderAt4000 = d.size() > 0x4001 && d[0x4000] == 'A' && d[0x4001] == 'B';
    size_t hdr = r.hasHeaderAt0 ? 0 : (r.hasHeaderAt4000 ? 0x4000 : 0);
    if (r.hasHeaderAt0 || r.hasHeaderAt4000) {
        r.init      = rd16(d, hdr + 2);
        r.statement = rd16(d, hdr + 4);
        r.device    = rd16(d, hdr + 6);
        r.text      = rd16(d, hdr + 8);
    }
    r.mapper = guessMapper(d);
    return r;
}

} // namespace msx
