// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Alonso (GS·RUN). MSXFileForge.
// Free software under the GNU GPL v3 (or later) - see LICENSE. Modified
// versions must be distributed under the same license.
#include "util.h"
#include <cstring>

namespace msx {

uint32_t crc32(const uint8_t* data, size_t n) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        init = true;
    }
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

static void wr16(uint8_t* p, uint16_t v) { p[0] = v & 0xFF; p[1] = v >> 8; }

std::vector<uint8_t> formatBlankImage(int totalSectors) {
    // Standard MSX geometries.
    int spc, ndir, spf, spt, heads; uint8_t media;
    if (totalSectors == 720) {           // 360KB SS
        spc = 2; ndir = 112; spf = 2; spt = 9; heads = 1; media = 0xF8;
    } else {                             // default 720KB DS
        totalSectors = 1440;
        spc = 2; ndir = 112; spf = 3; spt = 9; heads = 2; media = 0xF9;
    }
    std::vector<uint8_t> img((size_t)totalSectors * 512, 0);
    uint8_t* b = img.data();

    // Boot sector: JMP + OEM + BPB.
    b[0] = 0xEB; b[1] = 0xFE; b[2] = 0x90;
    memcpy(b + 3, "MSXFForg", 8);
    wr16(b + 0x0B, 512);      // bytes/sector
    b[0x0D] = (uint8_t)spc;   // sectors/cluster
    wr16(b + 0x0E, 1);        // reserved sectors
    b[0x10] = 2;              // number of FATs
    wr16(b + 0x11, (uint16_t)ndir);
    wr16(b + 0x13, (uint16_t)totalSectors);
    b[0x15] = media;
    wr16(b + 0x16, (uint16_t)spf);
    wr16(b + 0x18, (uint16_t)spt);
    wr16(b + 0x1A, (uint16_t)heads);
    b[0x1FE] = 0x55; b[0x1FF] = 0xAA;   // boot signature

    // Initialize both FATs: reserved cluster 0/1 = media,FF,FF.
    for (int f = 0; f < 2; ++f) {
        long fatOff = (1 + (long)f * spf) * 512;
        img[fatOff + 0] = media;
        img[fatOff + 1] = 0xFF;
        img[fatOff + 2] = 0xFF;
    }
    // Root directory + data area stay zeroed (empty).
    return img;
}

} // namespace msx
