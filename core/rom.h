// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Alonso (GS·RUN). MSXFileForge.
// Free software under the GNU GPL v3 (or later) - see LICENSE. Modified
// versions must be distributed under the same license.
// MSXFileForge - MSX cartridge ROM inspection.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace msx {

enum class RomMapper {
    None,        // plain 8/16/32K
    Konami,      // Konami without SCC (bank regs 0x6000/0x8000/0xA000)
    KonamiSCC,   // Konami with SCC (0x5000/0x7000/0x9000/0xB000)
    ASCII8,      // ASCII 8KB
    ASCII16,     // ASCII 16KB
    Unknown
};

const char* mapperName(RomMapper m);

struct RomInfo {
    long size = 0;
    bool hasHeaderAt0 = false;     // "AB" at 0x0000
    bool hasHeaderAt4000 = false;  // "AB" at 0x4000
    uint16_t init = 0, statement = 0, device = 0, text = 0; // from the active header
    RomMapper mapper = RomMapper::Unknown;
    int pages16k = 0;              // number of 16KB pages
};

RomInfo analyzeRom(const std::vector<uint8_t>& d);

} // namespace msx
