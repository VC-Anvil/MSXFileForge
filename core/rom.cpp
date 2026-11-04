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
//   Konami      : 0x6000 0x8000 0xA000 (0x4000-0x5FFF is fixed)
//   Konami SCC  : 0x5000 0x7000 0x9000 0xB000
//   ASCII8      : 0x6000 0x6800 0x7000 0x7800
//   ASCII16     : 0x6000 0x7000 (often written as 0x77FF)
static RomMapper guessMapper(const std::vector<uint8_t>& d) {
    bool header = (d.size() >= 2 && d[0] == 'A' && d[1] == 'B') ||
                  (d.size() > 0x4001 && d[0x4000] == 'A' && d[0x4001] == 'B');
    // <=32K with a ROM header fits in the slot unbanked -> plain.
    if (header && d.size() <= 0x8000) return RomMapper::None;
    int konami = 0, scc = 0, ascii8 = 0, ascii16 = 0;
    for (size_t i = 0; i + 2 < d.size(); ++i) {
        if (d[i] != 0x32) continue;               // LD (nnnn),A
        uint16_t a = d[i+1] | (d[i+2] << 8);
        // Addresses are shared across mapper families, so tally independently.
        switch (a) {
            case 0x6000: konami++; ascii8++; ascii16++; break;
            case 0x8000: case 0xA000: konami++; break;
            case 0x5000: case 0x9000: case 0xB000: scc++; break;
            case 0x7000: scc++; ascii8++; ascii16++; break;
            case 0x6800: case 0x7800: ascii8++; break;
            case 0x77FF: ascii16++; break;
            default: break;
        }
    }
    // Ties go to the mapper whose registers are a subset of the others'
    // (ASCII16 before ASCII8: a pure 0x6000/0x7000 game is ASCII16).
    int best = ascii16; RomMapper m = RomMapper::ASCII16;
    if (ascii8 > best) { best = ascii8; m = RomMapper::ASCII8; }
    if (konami > best) { best = konami; m = RomMapper::Konami; }
    if (scc > best)    { best = scc;    m = RomMapper::KonamiSCC; }
    if (best == 0) {
        // No bank switching found: a 48/64K ROM can still be plain (mirrored
        // over pages 0-3); anything bigger must bank somehow.
        if (d.size() <= 0x10000) return RomMapper::None;
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
