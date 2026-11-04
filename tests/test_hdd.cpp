// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Alonso (GS·RUN). MSXFileForge.
// Free software under the GNU GPL v3 (or later) - see LICENSE. Modified
// versions must be distributed under the same license.
// Hard disk / SD images: FAT16 volumes, MBR + extended partitions.
#include "check.h"
#include "disk_image.h"
#include "fat12.h"
#include "partition.h"
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
    auto vol = formatFatVolume(8 * MB);
    vol[0x16] = 0; vol[0x17] = 0;          // FAT32: sectors-per-FAT moves to 0x24
    vol[0x24] = 0x10;
    DiskImage dk; CHECK(dk.loadBytes(vol, "f32.img", &err));
    Fat12 fs;
    CHECK(!fs.mount(dk, &err));
    CHECK(err.find("FAT32") != std::string::npos);
}
