// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Alonso (GS·RUN). MSXFileForge.
// Free software under the GNU GPL v3 (or later) - see LICENSE. Modified
// versions must be distributed under the same license.
// Disk containers: plain .dsk geometry, DMK parsing, CRCs and protection.
#include "check.h"
#include "disk_image.h"
#include "fat12.h"
#include "util.h"
#include <cstring>

using namespace msx;

namespace {

typedef std::vector<uint8_t> Bytes;

Bytes pattern(size_t n, uint8_t seed) {
    Bytes v(n);
    uint32_t x = 0x12345u * (seed + 1);
    for (auto& b : v) { x = x * 1103515245u + 12345u; b = (uint8_t)(x >> 16); }
    return v;
}

// A 720K disk with a few files, as raw .dsk bytes.
Bytes sampleDsk(int sectors = 1440) {
    DiskImage dk; Fat12 fs; std::string err;
    dk.loadBytes(formatBlankImage(sectors), "s.dsk", &err);
    fs.mount(dk, &err);
    fs.setTimestamp(0x5A21, 0);
    fs.createFile("/", "BOOT.BAS", pattern(700, 1), &err);
    fs.createFile("/", "DATA.BIN", pattern(20000, 2), &err);
    fs.makeDir("/", "SUB", &err);
    fs.createFile("/SUB", "DEEP.TXT", pattern(1500, 3), &err);
    return dk.raw();
}

// Offset (in the DMK file) of track t/side s and its IDAM pointer table.
long trackOffset(const Bytes& dmk, int t, int s) {
    int sides = (dmk[4] & 0x10) ? 1 : 2;
    int len = dmk[2] | (dmk[3] << 8);
    return 16 + ((long)t * sides + s) * len;
}
long idamOffset(const Bytes& dmk, int t, int s, int idx) {
    long to = trackOffset(dmk, t, s);
    int ptr = (dmk[(size_t)(to + idx * 2)] | (dmk[(size_t)(to + idx * 2 + 1)] << 8)) & 0x3FFF;
    return to + ptr;
}

bool sameFiles(DiskImage& dk) {
    Fat12 fs; std::string err;
    if (!fs.mount(dk, &err)) { fprintf(stderr, "    mount: %s\n", err.c_str()); return false; }
    DirEntry e;
    bool ok = fs.lookup("/BOOT.BAS", e) && fs.readFile(e) == pattern(700, 1) &&
              fs.lookup("/DATA.BIN", e) && fs.readFile(e) == pattern(20000, 2) &&
              fs.lookup("/SUB/DEEP.TXT", e) && fs.readFile(e) == pattern(1500, 3);
    return ok && fs.verify().issues.empty();
}

} // namespace

TEST(crc16_ccitt_known_values) {
    const uint8_t check[] = {'1','2','3','4','5','6','7','8','9'};
    CHECK_EQ(crc16Ccitt(check, 9), (uint16_t)0x29B1);   // CRC-16/CCITT-FALSE check value
    const uint8_t sync[] = {0xA1, 0xA1, 0xA1};
    CHECK_EQ(crc16Ccitt(sync, 3), (uint16_t)0xCDB4);    // MFM preset
}

TEST(crc32_known_value) {
    const uint8_t check[] = {'1','2','3','4','5','6','7','8','9'};
    CHECK_EQ(crc32(check, 9), 0xCBF43926u);
}

TEST(plain_image_geometry) {
    DiskImage dk; std::string err;
    CHECK(dk.loadBytes(formatBlankImage(1440), "a.dsk", &err));
    CHECK_EQ(dk.format(), DiskFormat::DSK_Plain);
    CHECK_EQ(dk.cylinders(), 80); CHECK_EQ(dk.heads(), 2); CHECK_EQ(dk.sectorsPerTrack(), 9);
    CHECK_EQ(dk.logicalSectors(), 1440L);
    CHECK(!dk.isProtected());
    CHECK(dk.loadBytes(formatBlankImage(720), "b.dsk", &err));
    CHECK_EQ(dk.heads(), 1);
    // No boot signature: still readable, classified as a RAW dump.
    CHECK(dk.loadBytes(Bytes(737280, 0xE5), "c.dsk", &err));
    CHECK_EQ(dk.format(), DiskFormat::RAW);
    CHECK(!dk.loadBytes(Bytes(100, 0), "d.dsk", &err));
    CHECK(!err.empty());
}

TEST(plain_sector_read_write) {
    DiskImage dk; std::string err;
    CHECK(dk.loadBytes(formatBlankImage(1440), "a.dsk", &err));
    uint8_t buf[512];
    memset(buf, 0x5A, sizeof buf);
    CHECK(!dk.dirty());
    CHECK(dk.writeLogicalSector(100, buf));
    CHECK(dk.dirty());
    uint8_t back[512];
    CHECK(dk.readSector(100, back));
    CHECK(memcmp(buf, back, 512) == 0);
    CHECK(!dk.readSector(1440, back));
    CHECK(!dk.readSector(-1, back));
    CHECK(!dk.writeLogicalSector(1440, buf));
}

TEST(dmk_roundtrip_reads_same_filesystem) {
    for (int sectors : {1440, 720}) {
        Bytes dsk = sampleDsk(sectors);
        std::string err;
        Bytes dmk = dskToDmk(dsk, &err);
        CHECK(!dmk.empty());
        DiskImage dk;
        CHECK(dk.loadBytes(dmk, "x.dmk", &err));
        CHECK_EQ(dk.format(), DiskFormat::DMK);
        CHECK(!dk.isProtected());
        CHECK_EQ(dk.sectorsPerTrack(), 9);
        CHECK_EQ(dk.logicalSectors(), (long)sectors);
        for (auto& si : dk.sectors()) {
            CHECK(!si.idCrcError);
            CHECK(!si.dataCrcError);
        }
        // Every logical sector equals the source .dsk.
        bool same = true;
        uint8_t buf[512];
        for (long l = 0; l < sectors; ++l)
            if (!dk.readSector(l, buf) || memcmp(buf, &dsk[(size_t)l * 512], 512) != 0) same = false;
        CHECK(same);
        CHECK(sameFiles(dk));
    }
}

TEST(dmk_writes_keep_crcs_valid) {
    std::string err;
    DiskImage dk;
    CHECK(dk.loadBytes(dskToDmk(sampleDsk(), &err), "x.dmk", &err));
    {
        Fat12 fs; CHECK(fs.mount(dk, &err));
        CHECK(fs.createFile("/", "NEW.BIN", pattern(5000, 9), &err));
    }
    // Reload the edited container: no CRC errors may appear.
    DiskImage again;
    CHECK(again.loadBytes(dk.raw(), "y.dmk", &err));
    CHECK(!again.isProtected());
    Fat12 fs; CHECK(fs.mount(again, &err));
    DirEntry e; CHECK(fs.lookup("/NEW.BIN", e));
    CHECK(fs.readFile(e) == pattern(5000, 9));
    CHECK(sameFiles(again));
}

TEST(dmk_detects_id_crc_error) {
    std::string err;
    Bytes dmk = dskToDmk(sampleDsk(), &err);
    long idam = idamOffset(dmk, 10, 1, 4);      // track 10 side 1, sector 5
    dmk[(size_t)idam + 5] ^= 0xFF;              // break the ID CRC
    DiskImage dk;
    CHECK(dk.loadBytes(dmk, "x.dmk", &err));
    CHECK(dk.isProtected());
    int bad = 0;
    for (auto& si : dk.sectors()) if (si.idCrcError) { bad++; CHECK_EQ(si.lba, -1L); }
    CHECK_EQ(bad, 1);
    // The unreadable sector is a hole; everything else still maps.
    uint8_t buf[512];
    CHECK(!dk.readSector((10 * 2 + 1) * 9 + 4, buf));
    CHECK(dk.readSector((10 * 2 + 1) * 9 + 5, buf));
    CHECK(dk.protectionReport().find("ID CRC") != std::string::npos);
}

TEST(dmk_detects_data_crc_error_but_keeps_data) {
    std::string err;
    Bytes dsk = sampleDsk();
    Bytes dmk = dskToDmk(dsk, &err);
    long idam = idamOffset(dmk, 2, 0, 0);
    // Corrupt the stored data CRC of that sector: DAM is 37 bytes past the ID CRC.
    long dam = idam + 7 + 22 + 12 + 3;
    CHECK_EQ(dmk[(size_t)dam], (uint8_t)0xFB);
    dmk[(size_t)dam + 513] ^= 0x01;
    DiskImage dk;
    CHECK(dk.loadBytes(dmk, "x.dmk", &err));
    CHECK(dk.isProtected());
    int bad = 0;
    for (auto& si : dk.sectors()) if (si.dataCrcError) bad++;
    CHECK_EQ(bad, 1);
    uint8_t buf[512];
    CHECK(dk.readSector(2 * 2 * 9, buf));
    CHECK(memcmp(buf, &dsk[(size_t)(2 * 2 * 9) * 512], 512) == 0);
    // Writing a protected sector must not "repair" its deliberate CRC error.
    memset(buf, 0x11, sizeof buf);
    CHECK(dk.writeLogicalSector(2 * 2 * 9, buf));
    DiskImage again; CHECK(again.loadBytes(dk.raw(), "y.dmk", &err));
    bad = 0;
    for (auto& si : again.sectors()) if (si.dataCrcError) bad++;
    CHECK_EQ(bad, 1);
}

TEST(dmk_detects_deleted_data_mark) {
    std::string err;
    Bytes dmk = dskToDmk(sampleDsk(), &err);
    long idam = idamOffset(dmk, 40, 0, 3);
    long dam = idam + 7 + 22 + 12 + 3;
    dmk[(size_t)dam] = 0xF8;                     // deleted data address mark
    // Fix up the data CRC so only the mark differs.
    uint16_t crc = crc16Ccitt(&dmk[(size_t)dam], 513, 0xCDB4);
    dmk[(size_t)dam + 513] = (uint8_t)(crc >> 8); dmk[(size_t)dam + 514] = (uint8_t)crc;
    DiskImage dk;
    CHECK(dk.loadBytes(dmk, "x.dmk", &err));
    CHECK(dk.isProtected());
    int del = 0;
    for (auto& si : dk.sectors()) if (si.deleted) { del++; CHECK(!si.dataCrcError); }
    CHECK_EQ(del, 1);
    CHECK(dk.protectionReport().find("deleted") != std::string::npos);
}

TEST(dmk_duplicate_ids_and_extra_sectors_do_not_shift_lbas) {
    // Regression: one protected track with more sector headers than the rest
    // used to raise sectors-per-track for the whole disk, shifting every LBA
    // and making the filesystem unreadable.
    std::string err;
    Bytes dmk = dskToDmk(sampleDsk(), &err);
    long to = trackOffset(dmk, 5, 0);
    // Add a 10th IDAM pointer on track 5 that repeats sector 1's header.
    dmk[(size_t)to + 18] = dmk[(size_t)to + 0];
    dmk[(size_t)to + 19] = dmk[(size_t)to + 1];
    for (bool wipeBpbSpt : {false, true}) {
        Bytes img = dmk;
        if (wipeBpbSpt) {
            // Make the BPB useless for the geometry: fall back to the most
            // common per-track count.
            long boot = idamOffset(img, 0, 0, 0) + 7 + 22 + 12 + 3 + 1;
            img[(size_t)boot + 0x18] = 0; img[(size_t)boot + 0x19] = 0;
        }
        DiskImage dk;
        CHECK(dk.loadBytes(img, "x.dmk", &err));
        CHECK(dk.isProtected());
        CHECK_EQ(dk.sectorsPerTrack(), 9);
        int dups = 0;
        for (auto& si : dk.sectors()) if (si.duplicateId) dups++;
        CHECK_EQ(dups, 2);
        CHECK(dk.protectionReport().find("duplicate") != std::string::npos);
        uint8_t buf[512];
        CHECK(!dk.readSector(5 * 2 * 9, buf)); // the ambiguous sector is a hole
        CHECK(dk.readSector(5 * 2 * 9 + 1, buf));
        CHECK(dk.readSector(1439, buf));
    }
}

TEST(dmk_bad_extension_falls_back_to_plain) {
    std::string err;
    DiskImage dk;
    CHECK(dk.loadBytes(sampleDsk(), "mislabeled.dmk", &err));
    CHECK_EQ(dk.format(), DiskFormat::DSK_Plain);
    CHECK(sameFiles(dk));
}

TEST(dmk_rejects_unknown_geometry) {
    std::string err;
    CHECK(dskToDmk(Bytes(1000 * 512, 0), &err).empty());
    CHECK(!err.empty());
}

TEST(dmk_sector_running_off_track_is_read_only) {
    std::string err;
    Bytes dmk = dskToDmk(sampleDsk(), &err);
    long to = trackOffset(dmk, 0, 0);
    int len = dmk[2] | (dmk[3] << 8);
    // Move sector 9's header near the end of the track so its data runs past it.
    long idam = idamOffset(dmk, 0, 0, 8);
    long dst = to + len - 20;
    memmove(&dmk[(size_t)dst], &dmk[(size_t)idam], 7);
    dmk[(size_t)dst + 7] = 0xFB;
    uint16_t ptr = (uint16_t)((dst - to) | 0x8000);
    dmk[(size_t)to + 16] = ptr & 0xFF; dmk[(size_t)to + 17] = ptr >> 8;
    DiskImage dk;
    CHECK(dk.loadBytes(dmk, "x.dmk", &err));
    CHECK(dk.isProtected());
    uint8_t buf[512];
    memset(buf, 0x33, sizeof buf);
    CHECK(!dk.writeLogicalSector(8, buf));     // would overwrite the next track
    CHECK(dk.writeLogicalSector(7, buf));
    DiskImage again;
    CHECK(again.loadBytes(dk.raw(), "y.dmk", &err));
    CHECK(again.readSector(9, buf));          // C0 H1 S1 still intact
}
