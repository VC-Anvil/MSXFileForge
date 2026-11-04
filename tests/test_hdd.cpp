// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Alonso (GS·RUN). MSXFileForge.
// Free software under the GNU GPL v3 (or later) - see LICENSE. Modified
// versions must be distributed under the same license.
// Hard disk / SD images: FAT16 volumes, MBR + extended partitions.
#include "check.h"
#include "disk_image.h"
#include "fat12.h"
#include "partition.h"
#include "util.h"
#include <cstring>

using namespace msx;

namespace {

std::vector<uint8_t> pattern(size_t n, uint8_t seed) {
    std::vector<uint8_t> v(n);
    uint32_t x = 0xC0FFEEu ^ seed;
    for (auto& b : v) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; b = (uint8_t)x; }
    return v;
}
const long MB = 2048; // sectors

bool clean(const Fat12& fs) {
    auto r = fs.verify();
    for (auto& i : r.issues) fprintf(stderr, "    fsck: %s\n", i.c_str());
    return r.issues.empty();
}

} // namespace

TEST(hdd_fat16_superfloppy) {
    std::string err;
    DiskImage dk;
    CHECK(dk.loadBytes(formatFatVolume(32 * MB, 0, "nextor"), "sd.dsk", &err));
    CHECK_EQ(dk.format(), DiskFormat::HDD);
    CHECK(findPartitions(dk).empty());
    Fat12 fs;
    CHECK(fs.mount(dk, &err));
    CHECK_EQ(fs.fatBits(), 16);
    CHECK_EQ(fs.sectorsPerCluster(), 1);   // 65536 sectors still fit 1-sector clusters
    CHECK(fs.totalClusters() >= 4085 && fs.totalClusters() <= 65524);
    CHECK_EQ(fs.volumeLabel(), std::string("NEXTOR"));
    CHECK(clean(fs));
}

TEST(hdd_fat16_chains_past_cluster_4095) {
    // Cluster numbers above 0xFFF exist only on FAT16: a chain through 4095
    // must not be mistaken for an end-of-chain marker.
    std::string err;
    DiskImage dk;
    CHECK(dk.loadBytes(formatFatVolume(32 * MB), "sd.dsk", &err));
    Fat12 fs; CHECK(fs.mount(dk, &err));
    fs.setTimestamp(0x5A21, 0);
    auto big = pattern(6 * 1024 * 1024, 1);         // ~12300 clusters of 512 bytes
    CHECK(fs.createFile("/", "BIG.BIN", big, &err));
    CHECK(fs.makeDir("/", "SUB", &err));
    CHECK(fs.createFile("/SUB", "TAIL.BIN", pattern(5000, 2), &err));
    long freeBefore = fs.freeBytes();
    DiskImage again; CHECK(again.loadBytes(dk.raw(), "sd.dsk", &err));
    Fat12 fs2; CHECK(fs2.mount(again, &err));
    DirEntry e;
    CHECK(fs2.lookup("/BIG.BIN", e)); CHECK(fs2.readFile(e) == big);
    CHECK(fs2.lookup("/SUB/TAIL.BIN", e)); CHECK(fs2.readFile(e) == pattern(5000, 2));
    CHECK(e.startCluster > 4095);
    CHECK_EQ(fs2.freeBytes(), freeBefore);
    CHECK(clean(fs2));
    // Shrink and delete free everything again.
    CHECK(fs2.lookup("/BIG.BIN", e));
    CHECK(fs2.writeFile(e, pattern(10, 3), &err));
    CHECK(fs2.deleteFile(e, &err));
    CHECK(clean(fs2));
}

TEST(hdd_small_volume_is_fat12) {
    std::string err;
    DiskImage dk;
    CHECK(dk.loadBytes(formatFatVolume(2 * MB), "small.dsk", &err));
    Fat12 fs; CHECK(fs.mount(dk, &err));
    CHECK_EQ(fs.fatBits(), 12);
    CHECK(fs.createFile("/", "A.BIN", pattern(300000, 4), &err));
    DirEntry e; CHECK(fs.lookup("/A.BIN", e));
    CHECK(fs.readFile(e) == pattern(300000, 4));
    CHECK(clean(fs));
}

TEST(hdd_cluster_size_grows_with_volume) {
    std::string err;
    struct { long sectors; int spc; } cases[] = {{64 * MB, 2}, {128 * MB, 4}, {512 * MB, 16}};
    for (auto& c : cases) {
        DiskImage dk; CHECK(dk.loadBytes(formatFatVolume(c.sectors), "v.img", &err));
        Fat12 fs; CHECK(fs.mount(dk, &err));
        CHECK_EQ(fs.fatBits(), 16);
        CHECK_EQ(fs.sectorsPerCluster(), c.spc);
        CHECK(fs.totalClusters() >= 4085 && fs.totalClusters() <= 65524);
    }
}

TEST(hdd_large_volume_uses_32bit_sector_count) {
    std::string err;
    auto vol = formatFatVolume(100 * MB);
    CHECK_EQ(vol[0x13] | (vol[0x14] << 8), 0);      // > 65535 sectors: 0x20 field
    DiskImage dk; CHECK(dk.loadBytes(vol, "big.dsk", &err));
    Fat12 fs; CHECK(fs.mount(dk, &err));
    CHECK_EQ(fs.fatBits(), 16);
    CHECK_EQ(fs.totalBytes(), 100L * 1024 * 1024);
    CHECK(fs.totalClusters() <= 65524);
}

TEST(hdd_primary_partitions) {
    std::string err;
    auto img = buildPartitionedImage({16 * MB, 8 * MB, 2 * MB}, &err);
    CHECK(!img.empty());
    DiskImage dk; CHECK(dk.loadBytes(img, "hdd.img", &err));
    auto parts = findPartitions(dk);
    CHECK_EQ(parts.size(), (size_t)3);
    if (parts.size() != 3) return;
    CHECK_EQ(parts[0].firstSector, 63L);
    CHECK_EQ(parts[0].type, (uint8_t)0x04);
    CHECK_EQ(parts[2].type, (uint8_t)0x01);           // 2MB -> FAT12
    CHECK(!parts[1].logical);
    // Each partition is its own filesystem; writes stay inside it.
    for (auto& p : parts) {
        Fat12 fs;
        CHECK(fs.mount(dk, p.firstSector, p.sectorCount, &err));
        CHECK_EQ(fs.volumeStart(), p.firstSector);
        char nm[16]; snprintf(nm, sizeof nm, "P%d.DAT", p.number);
        CHECK(fs.createFile("/", nm, pattern(50000, (uint8_t)p.number), &err));
    }
    for (auto& p : parts) {
        Fat12 fs; CHECK(fs.mount(dk, p.firstSector, p.sectorCount, &err));
        auto ls = fs.list("/");
        CHECK_EQ(ls.size(), (size_t)1);
        DirEntry e;
        char nm[16]; snprintf(nm, sizeof nm, "/P%d.DAT", p.number);
        CHECK(fs.lookup(nm, e));
        CHECK(fs.readFile(e) == pattern(50000, (uint8_t)p.number));
        CHECK(clean(fs));
    }
    // The whole image does not mount as a single FAT volume.
    Fat12 whole; CHECK(!whole.mount(dk, &err));
}

TEST(hdd_extended_partition_chain) {
    std::string err;
    auto img = buildPartitionedImage({4 * MB, 4 * MB, 3 * MB, 5 * MB, 2 * MB, 6 * MB}, &err);
    DiskImage dk; CHECK(dk.loadBytes(img, "hdd.img", &err));
    auto parts = findPartitions(dk);
    CHECK_EQ(parts.size(), (size_t)6);
    if (parts.size() != 6) return;
    CHECK(!parts[0].logical);
    for (size_t i = 1; i < parts.size(); ++i) CHECK(parts[i].logical);
    CHECK_EQ(parts[3].sectorCount, 5 * MB);
    for (auto& p : parts) {
        Fat12 fs;
        CHECK(fs.mount(dk, p.firstSector, p.sectorCount, &err));
        CHECK_EQ(fs.totalBytes(), p.sectorCount * 512);
        CHECK(clean(fs));
    }
}

TEST(hdd_mbr_rejections) {
    std::string err;
    // A FAT boot sector is never read as an MBR, even with 0x55AA.
    DiskImage dk; CHECK(dk.loadBytes(formatFatVolume(8 * MB), "a.img", &err));
    CHECK(findPartitions(dk).empty());
    // An entry that runs past the end of the image invalidates the table.
    auto img = buildPartitionedImage({4 * MB}, &err);
    img[0x1BE + 12] = 0xFF; img[0x1BE + 13] = 0xFF; img[0x1BE + 14] = 0xFF;
    CHECK(dk.loadBytes(img, "b.img", &err));
    CHECK(findPartitions(dk).empty());
    // A self-referencing EBR chain terminates.
    auto ext = buildPartitionedImage({4 * MB, 4 * MB, 4 * MB, 4 * MB, 4 * MB}, &err);
    DiskImage d2; CHECK(d2.loadBytes(ext, "c.img", &err));
    auto parts = findPartitions(d2);
    CHECK_EQ(parts.size(), (size_t)5);
    long extStart = ext[0x1CE + 8] | (ext[0x1CE + 9] << 8) | (ext[0x1CE + 10] << 16);
    uint8_t* ebr2 = &ext[(size_t)(extStart + 1 + 4 * MB) * 512];   // second EBR
    ebr2[0x1CE + 8] = 0; ebr2[0x1CE + 9] = 0; ebr2[0x1CE + 10] = 0; ebr2[0x1CE + 11] = 0; // -> back to the first
    CHECK(d2.loadBytes(ext, "c.img", &err));
    CHECK(findPartitions(d2).size() <= 5);
}

TEST(hdd_fat32_is_refused) {
    std::string err;
    // A realistic FAT32 boot sector: no root entry count, no 16-bit sizes,
    // sectors-per-FAT and root cluster in the FAT32 extension.
    std::vector<uint8_t> vol((size_t)MB * 512, 0);   // 1MB is enough to hold the BPB
    uint8_t* b = vol.data();
    b[0] = 0xEB; b[1] = 0x58; b[2] = 0x90;
    b[0x0B] = 0x00; b[0x0C] = 0x02;      // 512 bytes/sector
    b[0x0D] = 8; b[0x0E] = 32;           // spc, reserved
    b[0x10] = 2;                         // FATs
    b[0x15] = 0xF8;
    b[0x20] = 0x00; b[0x21] = 0x00; b[0x22] = 0x02;   // total32
    b[0x24] = 0x00; b[0x25] = 0x04;      // spf32
    b[0x2C] = 2;                         // root cluster
    b[0x1FE] = 0x55; b[0x1FF] = 0xAA;
    DiskImage dk; CHECK(dk.loadBytes(vol, "f32.img", &err));
    Fat12 fs;
    CHECK(!fs.mount(dk, &err));
    CHECK(err.find("FAT32") != std::string::npos);
}

// ---- Regressions found in review ------------------------------------------

namespace {
// Clusters the BPB of a formatted volume implies, and whether its FAT holds them.
bool fatHoldsClusters(const std::vector<uint8_t>& v, long* clustersOut = nullptr) {
    auto r16 = [&](int o) { return (long)(v[(size_t)o] | (v[(size_t)o + 1] << 8)); };
    long total = r16(0x13);
    if (!total) total = r16(0x20) | (r16(0x22) << 16);
    long spc = v[0x0D], rsvd = r16(0x0E), nfat = v[0x10], ndir = r16(0x11), spf = r16(0x16);
    long n = (total - rsvd - nfat * spf - (ndir * 32 + 511) / 512) / spc;
    bool f16 = memcmp(&v[0x36], "FAT16", 5) == 0;
    if (clustersOut) *clustersOut = n;
    if (f16 != (n >= 4085)) return false;            // label must match Microsoft's rule
    long bytes = f16 ? (n + 2) * 2 : ((n + 2) * 3 + 1) / 2;
    return bytes <= spf * 512;
}
} // namespace

TEST(hdd_truncated_fat16_stays_fat16) {
    std::string err;
    auto vol = formatFatVolume(8192);                 // FAT16, 8095 clusters
    {
        DiskImage dk; CHECK(dk.loadBytes(vol, "v.img", &err));
        Fat12 fs; CHECK(fs.mount(dk, &err));
        CHECK(fs.createFile("/", "A.BIN", pattern(100 * 1024, 1), &err));
        vol = dk.raw();
    }
    vol.resize(4000 * 512);                           // truncated dump
    DiskImage dk; CHECK(dk.loadBytes(vol, "v.img", &err));
    Fat12 fs; CHECK(fs.mount(dk, &err));
    CHECK_EQ(fs.fatBits(), 16);
    DirEntry e; CHECK(fs.lookup("/A.BIN", e));
    CHECK(fs.readFile(e) == pattern(100 * 1024, 1));
    CHECK(clean(fs));
    // A partition entry smaller than its volume's BPB: same rule.
    DiskImage d2; CHECK(d2.loadBytes(formatFatVolume(8192), "w.img", &err));
    Fat12 f2; CHECK(f2.mount(d2, 0, 4000, &err));
    CHECK_EQ(f2.fatBits(), 16);
}

TEST(hdd_floppy_with_table_like_boot_code_is_a_floppy) {
    std::string err;
    for (int variant = 0; variant < 3; ++variant) {
        DiskImage base; CHECK(base.loadBytes(formatBlankImage(1440), "f.dsk", &err));
        Fat12 f0; CHECK(f0.mount(base, &err));
        CHECK(f0.createFile("/", "GAME.BAS", pattern(1000, 7), &err));
        auto img = base.raw();
        uint8_t* e = &img[0x1BE];
        e[0] = 0; e[4] = variant == 2 ? 0x01 : 0x21; e[8] = 10; e[12] = 100;
        img[510] = 0x55; img[511] = 0xAA;
        if (variant == 0) img[0] = 0x00;              // valid BPB, odd jump byte
        if (variant >= 1) memset(&img[3], 0, 0x1E - 3);  // no BPB: media-byte floppy
        DiskImage dk; CHECK(dk.loadBytes(img, "f.dsk", &err));
        CHECK(findPartitions(dk).empty());
        Fat12 fs; CHECK(fs.mount(dk, &err));
        DirEntry x; CHECK(fs.lookup("/GAME.BAS", x));
        CHECK(fs.readFile(x) == pattern(1000, 7));
    }
}

TEST(hdd_fsck_accepts_clusters_near_fff0) {
    std::string err;
    DiskImage dk; CHECK(dk.loadBytes(formatFatVolume(66067), "v.img", &err));
    Fat12 fs; CHECK(fs.mount(dk, &err));
    CHECK_EQ(fs.fatBits(), 16);
    CHECK_EQ(fs.sectorsPerCluster(), 1);
    CHECK_EQ(fs.totalClusters(), 65522);              // highest cluster 0xFFF3
    CHECK(fs.createFile("/", "FILL.BIN", pattern((size_t)65518 * 512, 2), &err));
    CHECK(fs.createFile("/", "TAIL.BIN", pattern(4 * 512, 3), &err));
    DirEntry t; CHECK(fs.lookup("/TAIL.BIN", t));
    CHECK_EQ(t.startCluster, 0xFFF0);
    CHECK(fs.readFile(t) == pattern(4 * 512, 3));
    CHECK(clean(fs));
    CHECK_EQ(fs.freeBytes(), 0L);
}

TEST(hdd_format_fat_always_big_enough) {
    // Every size up to 9000 sectors plus sizes the fixed-point used to miss.
    int bad = 0;
    std::vector<long> sizes;
    for (long n = 64; n <= 9000; ++n) sizes.push_back(n);
    for (long n : {375L, 4136L, 4141L, 4161L, 65536L, 131071L, 1048577L, 4178399L}) sizes.push_back(n);
    for (long n : sizes) {
        auto v = formatFatVolume(n);
        if (v.empty() || !fatHoldsClusters(v)) { if (bad++ < 5) fprintf(stderr, "    size %ld\n", n); }
    }
    CHECK_EQ(bad, 0);
}

TEST(hdd_old_boot_sectors_keep_floppy_geometry) {
    std::string err;
    // total16 == 0 and boot code where a DOS 3.31 BPB keeps total32: the
    // code bytes must not be taken as a sector count.
    auto img = formatBlankImage(1440);
    img[0x13] = 0; img[0x14] = 0;
    img[0x20] = 0x00; img[0x21] = 0x00; img[0x22] = 0x01; img[0x23] = 0x00;  // "65536"
    DiskImage dk; CHECK(dk.loadBytes(img, "o.dsk", &err));
    Fat12 fs; CHECK(fs.mount(dk, &err));
    CHECK_EQ(fs.fatBits(), 12);
    CHECK_EQ(fs.totalClusters(), 713);
    // Same through a DMK container.
    DiskImage dm; CHECK(dm.loadBytes(dskToDmk(img, &err), "o.dmk", &err));
    Fat12 fd; CHECK(fd.mount(dm, &err));
    CHECK_EQ(fd.fatBits(), 12);
    CHECK(clean(fd));
}

TEST(hdd_floppy_without_bpb_and_code_at_0x24_mounts) {
    std::string err;
    auto img = formatBlankImage(1440);
    memset(&img[3], 0, 0x3E - 3);
    img[0x0B] = 0x00; img[0x0C] = 0x02;          // only bytes/sector survives
    img[0x24] = 0xCD; img[0x25] = 0x12;          // boot code, not a FAT32 field
    DiskImage dk; CHECK(dk.loadBytes(img, "c.dsk", &err));
    Fat12 fs;
    CHECK(fs.mount(dk, &err));
    CHECK_EQ(fs.totalClusters(), 713);
}

TEST(hdd_overlapping_partitions_are_not_both_exposed) {
    std::string err;
    auto img = buildPartitionedImage({8 * MB, 8 * MB}, &err);
    // Make entry 2 start inside entry 1.
    uint8_t* e2 = &img[0x1CE];
    long s1 = img[0x1BE + 8] | (img[0x1BE + 9] << 8) | (img[0x1BE + 10] << 16);
    long s2 = s1 + 100;
    e2[8] = s2 & 0xFF; e2[9] = (s2 >> 8) & 0xFF; e2[10] = (s2 >> 16) & 0xFF; e2[11] = 0;
    DiskImage dk; CHECK(dk.loadBytes(img, "o.img", &err));
    auto parts = findPartitions(dk);
    CHECK_EQ(parts.size(), (size_t)1);
}
