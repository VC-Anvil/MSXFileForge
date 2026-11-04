// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Alonso (GS·RUN). MSXFileForge.
// Free software under the GNU GPL v3 (or later) - see LICENSE. Modified
// versions must be distributed under the same license.
#include "partition.h"
#include <cstring>
#include <set>

namespace msx {

namespace {
uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
uint32_t rd32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
void wr16(uint8_t* p, uint32_t v) { p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; }
void wr32(uint8_t* p, uint32_t v) { wr16(p, v); wr16(p + 2, v >> 16); }
bool isExtended(uint8_t t) { return t == 0x05 || t == 0x0F || t == 0x85; }
bool isPow2(int v) { return v > 0 && (v & (v - 1)) == 0; }
} // namespace

const char* partitionTypeName(uint8_t t) {
    switch (t) {
        case 0x00: return "empty";
        case 0x01: return "FAT12";
        case 0x04: return "FAT16 (<32MB)";
        case 0x05: return "extended";
        case 0x06: return "FAT16";
        case 0x0B: case 0x0C: return "FAT32";
        case 0x0E: return "FAT16 (LBA)";
        case 0x0F: return "extended (LBA)";
        default:   return "other";
    }
}

bool looksLikeFatBoot(const uint8_t* b) {
    int spc = b[0x0D], nfat = b[0x10], ndir = rd16(b + 0x11), spf = rd16(b + 0x16);
    long total = rd16(b + 0x13);
    if (total == 0) total = (long)rd32(b + 0x20);
    return (b[0] == 0xEB || b[0] == 0xE9) && rd16(b + 0x0B) == 512 && isPow2(spc) &&
           nfat >= 1 && nfat <= 4 && ndir > 0 && ndir % 16 == 0 && spf > 0 &&
           rd16(b + 0x0E) > 0 && total > 0;
}

std::vector<Partition> findPartitions(const DiskImage& disk) {
    std::vector<Partition> out;
    uint8_t mbr[512];
    if (!disk.readSector(0, mbr) || mbr[510] != 0x55 || mbr[511] != 0xAA) return out;
    if (looksLikeFatBoot(mbr)) return out;              // superfloppy
    long total = disk.logicalSectors();

    auto valid = [&](const uint8_t* e, long base) {
        long start = base + (long)rd32(e + 8), count = (long)rd32(e + 12);
        return (e[0] == 0x00 || e[0] == 0x80) && e[4] != 0 && rd32(e + 8) > 0 && count > 0 &&
               start + count <= total;
    };
    // Every non-empty entry must be well formed, or this is not an MBR.
    std::vector<const uint8_t*> prim;
    for (int i = 0; i < 4; ++i) {
        const uint8_t* e = mbr + 0x1BE + i * 16;
        if (e[4] == 0) continue;
        if (!valid(e, 0)) return {};
        prim.push_back(e);
    }
    if (prim.empty()) return out;

    int number = 0;
    long extStart = -1;
    for (const uint8_t* e : prim) {
        if (isExtended(e[4])) { if (extStart < 0) extStart = (long)rd32(e + 8); continue; }
        Partition p;
        p.number = ++number; p.type = e[4];
        p.firstSector = (long)rd32(e + 8); p.sectorCount = (long)rd32(e + 12);
        out.push_back(p);
    }
    // Logical drives: a chain of EBRs. Entry 0 = the drive (relative to this
    // EBR), entry 1 = the next EBR (relative to the extended partition).
    std::set<long> seen;
    long ebr = extStart;
    while (ebr > 0 && ebr < total && seen.insert(ebr).second && seen.size() < 256) {
        uint8_t s[512];
        if (!disk.readSector(ebr, s) || s[510] != 0x55 || s[511] != 0xAA) break;
        const uint8_t* e0 = s + 0x1BE;
        const uint8_t* e1 = s + 0x1CE;
        if (e0[4] != 0 && valid(e0, ebr)) {
            Partition p;
            p.number = ++number; p.type = e0[4]; p.logical = true;
            p.firstSector = ebr + (long)rd32(e0 + 8); p.sectorCount = (long)rd32(e0 + 12);
            out.push_back(p);
        }
        ebr = (e1[4] != 0 && isExtended(e1[4])) ? extStart + (long)rd32(e1 + 8) : -1;
    }
    return out;
}

std::vector<uint8_t> formatFatVolume(long totalSectors, long hiddenSectors, const std::string& label) {
    std::vector<uint8_t> img;
    if (totalSectors < 64) return img;
    const int rsvd = 1, nfat = 2, ndir = 512;
    const long rootSecs = ndir * 32 / 512;
    // FAT16 if the volume can hold at least 4085 clusters, using the smallest
    // cluster size that keeps the count <= 65524; otherwise FAT12 (<= 4084).
    auto layout = [&](bool f16, int c, long& fatSecs) {
        fatSecs = 1;
        long n = 0;
        for (int it = 0; it < 16; ++it) {   // FAT size and cluster count depend on each other
            n = (totalSectors - rsvd - nfat * fatSecs - rootSecs) / c;
            if (n < 0) n = 0;
            long bytes = f16 ? (n + 2) * 2 : ((n + 2) * 3 + 1) / 2;
            fatSecs = (bytes + 511) / 512;
        }
        return (totalSectors - rsvd - nfat * fatSecs - rootSecs) / c;
    };
    bool fat16 = false;
    int spc = 0;
    long spf = 0, clusters = 0;
    for (int c = 1; c <= 128; c *= 2) {
        long f; long n = layout(true, c, f);
        if (n <= 65524) { if (n >= 4085) { fat16 = true; spc = c; spf = f; clusters = n; } break; }
    }
    if (!fat16) {
        for (int c = 1; c <= 128; c *= 2) {
            long f; long n = layout(false, c, f);
            if (n <= 4084) { spc = c; spf = f; clusters = n; break; }
        }
    }
    if (spc == 0 || clusters < 1) return img;

    img.assign((size_t)totalSectors * 512, 0);
    uint8_t* b = img.data();
    b[0] = 0xEB; b[1] = 0x3C; b[2] = 0x90;
    memcpy(b + 3, "MSXFForg", 8);
    wr16(b + 0x0B, 512);
    b[0x0D] = (uint8_t)spc;
    wr16(b + 0x0E, rsvd);
    b[0x10] = nfat;
    wr16(b + 0x11, ndir);
    if (totalSectors < 65536) wr16(b + 0x13, (uint32_t)totalSectors);
    else wr32(b + 0x20, (uint32_t)totalSectors);
    b[0x15] = 0xF8;
    wr16(b + 0x16, (uint32_t)spf);
    wr16(b + 0x18, 63); wr16(b + 0x1A, 255);   // nominal CHS geometry
    wr32(b + 0x1C, (uint32_t)hiddenSectors);
    b[0x24] = 0x80;                               // drive number
    b[0x26] = 0x29;                               // extended boot signature
    wr32(b + 0x27, 0x4D535846u ^ (uint32_t)totalSectors); // volume serial
    char lab[12]; memset(lab, ' ', 11); lab[11] = 0;
    if (label.empty()) memcpy(lab, "NO NAME", 7);
    for (size_t i = 0; i < label.size() && i < 11; ++i) lab[i] = (char)toupper((unsigned char)label[i]);
    memcpy(b + 0x2B, lab, 11);
    memcpy(b + 0x36, fat16 ? "FAT16   " : "FAT12   ", 8);
    b[0x3E] = 0x18; b[0x3F] = 0xFE;               // boot code: JR $ (not bootable)
    b[0x1FE] = 0x55; b[0x1FF] = 0xAA;
    for (int f = 0; f < nfat; ++f) {
        uint8_t* fat = b + (size_t)(rsvd + f * spf) * 512;
        fat[0] = 0xF8; fat[1] = 0xFF; fat[2] = 0xFF;
        if (fat16) fat[3] = 0xFF;
    }
    if (!label.empty()) {                         // root VOL entry too
        uint8_t* root = b + (size_t)(rsvd + nfat * spf) * 512;
        memcpy(root, lab, 11);
        root[0x0B] = 0x08;
    }
    return img;
}

std::vector<uint8_t> buildPartitionedImage(const std::vector<long>& sizes, std::string* err) {
    std::vector<uint8_t> img;
    if (sizes.empty()) { if (err) *err = "no partitions requested"; return img; }
    for (long s : sizes)
        if (s < 64) { if (err) *err = "a partition needs at least 64 sectors"; return img; }
    const long kAlign = 63;                       // first volume at sector 63, classic layout
    bool useExt = sizes.size() > 4;
    long total = kAlign;
    for (size_t i = 0; i < sizes.size(); ++i) total += sizes[i] + ((useExt && i > 0) ? 1 : 0);
    if (total > 0x7FFFFFFFL / 512) { if (err) *err = "image too large"; return img; }
    img.assign((size_t)total * 512, 0);

    auto typeFor = [](const std::vector<uint8_t>& vol, long secs) -> uint8_t {
        bool f16 = memcmp(&vol[0x36], "FAT16", 5) == 0;   // as formatFatVolume chose
        return !f16 ? 0x01 : (secs < 65536 ? 0x04 : 0x06);
    };
    auto putEntry = [](uint8_t* e, uint8_t type, uint32_t start, uint32_t count) {
        e[0] = 0x00;
        e[1] = 0xFE; e[2] = 0xFF; e[3] = 0xFF;    // CHS "use LBA"
        e[4] = type;
        e[5] = 0xFE; e[6] = 0xFF; e[7] = 0xFF;
        wr32(e + 8, start); wr32(e + 12, count);
    };
    uint8_t* mbr = img.data();
    long pos = kAlign;
    size_t primaries = useExt ? 1 : sizes.size();
    for (size_t i = 0; i < primaries; ++i) {
        auto vol = formatFatVolume(sizes[i], pos);
        if (vol.empty()) { if (err) *err = "cannot format partition " + std::to_string(i + 1); return {}; }
        memcpy(&img[(size_t)pos * 512], vol.data(), vol.size());
        putEntry(mbr + 0x1BE + i * 16, typeFor(vol, sizes[i]), (uint32_t)pos, (uint32_t)sizes[i]);
        pos += sizes[i];
    }
    if (useExt) {
        long extStart = pos, extSize = total - pos;
        putEntry(mbr + 0x1BE + 16, 0x05, (uint32_t)extStart, (uint32_t)extSize);
        for (size_t i = 1; i < sizes.size(); ++i) {
            long ebr = pos, volStart = pos + 1;
            auto vol = formatFatVolume(sizes[i], volStart);
            if (vol.empty()) { if (err) *err = "cannot format partition " + std::to_string(i + 1); return {}; }
            memcpy(&img[(size_t)volStart * 512], vol.data(), vol.size());
            uint8_t* s = &img[(size_t)ebr * 512];
            putEntry(s + 0x1BE, typeFor(vol, sizes[i]), 1, (uint32_t)sizes[i]);
            long next = volStart + sizes[i];
            if (i + 1 < sizes.size())
                putEntry(s + 0x1CE, 0x05, (uint32_t)(next - extStart), (uint32_t)(sizes[i + 1] + 1));
            s[510] = 0x55; s[511] = 0xAA;
            pos = next;
        }
    }
    mbr[510] = 0x55; mbr[511] = 0xAA;
    return img;
}

} // namespace msx
