// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Alonso (GS·RUN). MSXFileForge.
// Free software under the GNU GPL v3 (or later) - see LICENSE. Modified
// versions must be distributed under the same license.
// Cartridge ROM header parsing and mapper detection on synthetic images.
#include "check.h"
#include "rom.h"
#include <initializer_list>

using namespace msx;

namespace {

// A ROM of `size` bytes with an "AB" header at 0x0000 whose code writes A to
// each address in `regs` (LD (nnnn),A), `times` times each.
std::vector<uint8_t> rom(size_t size, std::initializer_list<uint16_t> regs, int times = 3) {
    std::vector<uint8_t> d(size, 0xFF);
    d[0] = 'A'; d[1] = 'B'; d[2] = 0x10; d[3] = 0x40; // INIT = 0x4010
    size_t p = 0x10;
    for (int t = 0; t < times; ++t)
        for (uint16_t a : regs) { d[p++] = 0x3E; d[p++] = (uint8_t)t; d[p++] = 0x32; d[p++] = a & 0xFF; d[p++] = a >> 8; }
    d[p++] = 0xC9;
    return d;
}

} // namespace

TEST(rom_header_fields) {
    auto d = rom(0x4000, {});
    d[4] = 0x00; d[5] = 0x50; d[8] = 0x34; d[9] = 0x12;
    RomInfo r = analyzeRom(d);
    CHECK(r.hasHeaderAt0);
    CHECK(!r.hasHeaderAt4000);
    CHECK_EQ(r.init, 0x4010);
    CHECK_EQ(r.statement, 0x5000);
    CHECK_EQ(r.text, 0x1234);
    CHECK_EQ(r.pages16k, 1);
    CHECK_EQ(r.mapper, RomMapper::None);
}

TEST(rom_header_at_4000) {
    std::vector<uint8_t> d(0x8000, 0xFF);
    d[0x4000] = 'A'; d[0x4001] = 'B'; d[0x4002] = 0x00; d[0x4003] = 0x80;
    RomInfo r = analyzeRom(d);
    CHECK(!r.hasHeaderAt0);
    CHECK(r.hasHeaderAt4000);
    CHECK_EQ(r.init, 0x8000);
    CHECK_EQ(r.mapper, RomMapper::None);
}

TEST(rom_mapper_detection) {
    CHECK_EQ(analyzeRom(rom(0x20000, {0x6000, 0x8000, 0xA000})).mapper, RomMapper::Konami);
    CHECK_EQ(analyzeRom(rom(0x20000, {0x5000, 0x7000, 0x9000, 0xB000})).mapper, RomMapper::KonamiSCC);
    CHECK_EQ(analyzeRom(rom(0x20000, {0x6000, 0x6800, 0x7000, 0x7800})).mapper, RomMapper::ASCII8);
    // Regression: a plain 0x6000/0x7000 game tied ASCII8 and ASCII16 and was
    // reported as ASCII8.
    CHECK_EQ(analyzeRom(rom(0x20000, {0x6000, 0x7000})).mapper, RomMapper::ASCII16);
    CHECK_EQ(analyzeRom(rom(0x20000, {0x6000, 0x77FF})).mapper, RomMapper::ASCII16);
}

TEST(rom_plain_large_and_unknown) {
    // 48K/64K without bank writes: plain ROM, not "Unknown".
    CHECK_EQ(analyzeRom(rom(0xC000, {})).mapper, RomMapper::None);
    CHECK_EQ(analyzeRom(rom(0x10000, {})).mapper, RomMapper::None);
    CHECK_EQ(analyzeRom(rom(0x40000, {})).mapper, RomMapper::Unknown);
    CHECK_EQ(analyzeRom({}).mapper, RomMapper::None);
    CHECK_EQ(analyzeRom({}).size, 0L);
}
