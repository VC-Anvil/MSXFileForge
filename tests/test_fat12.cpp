// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Alonso (GS·RUN). MSXFileForge.
// Free software under the GNU GPL v3 (or later) - see LICENSE. Modified
// versions must be distributed under the same license.
// FAT12: format, create, read, overwrite, delete, rename, mkdir, fsck.
#include "check.h"
#include "disk_image.h"
#include "fat12.h"
#include "util.h"
#include <cstring>

using namespace msx;

namespace {

std::vector<uint8_t> pattern(size_t n, uint8_t seed) {
    std::vector<uint8_t> v(n);
    uint32_t x = 0x9E3779B9u ^ seed;
    for (auto& b : v) { x = x * 1664525u + 1013904223u; b = (uint8_t)(x >> 24); }
    return v;
}

struct Disk {
    DiskImage dk;
    Fat12 fs;
    explicit Disk(int sectors = 1440) {
        std::string err;
        bool ok = dk.loadBytes(formatBlankImage(sectors), "blank.dsk", &err);
        CHECK(ok);
        CHECK(fs.mount(dk, &err));
        fs.setTimestamp(0x5A21, 0x6000); // 2025-01-01 12:00:00, deterministic
    }
    // Re-mount from the current bytes, as a fresh reader would see them.
    void remount() {
        std::vector<uint8_t> bytes = dk.raw();
        std::string err;
        CHECK(dk.loadBytes(bytes, "again.dsk", &err));
        CHECK(fs.mount(dk, &err));
    }
    bool find(const std::string& path, DirEntry& out) const { return fs.lookup(path, out); }
};

bool clean(const Fat12& fs) {
    auto r = fs.verify();
    for (auto& i : r.issues) fprintf(stderr, "    fsck: %s\n", i.c_str());
    return r.issues.empty();
}

} // namespace

TEST(fat12_blank_720k_geometry) {
    Disk d;
    CHECK_EQ(d.fs.bytesPerSector(), 512);
    CHECK_EQ(d.fs.sectorsPerCluster(), 2);
    // 1440 - 1 boot - 2*3 FAT - 7 root = 1426 data sectors = 713 clusters.
    CHECK_EQ(d.fs.totalClusters(), 713);
    CHECK_EQ(d.fs.freeBytes(), 713L * 1024);
    CHECK(d.fs.list("/").empty());
    CHECK(clean(d.fs));
    CHECK_EQ(d.dk.format(), DiskFormat::DSK_Plain);
    CHECK_EQ(d.dk.heads(), 2);
}

TEST(fat12_blank_360k_geometry) {
    Disk d(720);
    CHECK_EQ(d.fs.totalClusters(), 354); // (720 - 1 - 4 - 7) / 2
    CHECK_EQ(d.dk.heads(), 1);
}

TEST(fat12_create_and_read_back) {
    Disk d;
    std::string err;
    auto a = pattern(1, 1), b = pattern(1024, 2), c = pattern(5000, 3);
    CHECK(d.fs.createFile("/", "A.BIN", a, &err));
    CHECK(d.fs.createFile("/", "B.BIN", b, &err));
    CHECK(d.fs.createFile("/", "c.dat", c, &err));
    CHECK(d.fs.createFile("/", "EMPTY", {}, &err));
    d.remount();
    auto ls = d.fs.list("/");
    CHECK_EQ(ls.size(), (size_t)4);
    DirEntry e;
    CHECK(d.find("/A.BIN", e)); CHECK(d.fs.readFile(e) == a);
    CHECK(d.find("/b.bin", e)); CHECK(d.fs.readFile(e) == b);
    CHECK(d.find("/C.DAT", e)); CHECK(d.fs.readFile(e) == c); CHECK_EQ(e.size, 5000u);
    CHECK(d.find("/EMPTY", e)); CHECK_EQ(e.size, 0u); CHECK_EQ(e.startCluster, 0);
    CHECK_EQ(e.date, 0x5A21); CHECK_EQ(e.time, 0x6000);
    // 1 + 1 + 5 clusters used.
    CHECK_EQ(d.fs.freeBytes(), (713L - 7) * 1024);
    CHECK(clean(d.fs));
}

TEST(fat12_overwrite_grow_and_shrink) {
    Disk d;
    std::string err;
    CHECK(d.fs.createFile("/", "F.BIN", pattern(100, 1), &err));
    CHECK(d.fs.createFile("/", "G.BIN", pattern(3000, 2), &err)); // fences F's chain
    DirEntry e;
    CHECK(d.find("/F.BIN", e));
    auto big = pattern(10000, 9);
    CHECK(d.fs.writeFile(e, big, &err));
    d.remount();
    CHECK(d.find("/F.BIN", e)); CHECK(d.fs.readFile(e) == big);
    CHECK(d.find("/G.BIN", e)); CHECK(d.fs.readFile(e) == pattern(3000, 2));
    CHECK_EQ(d.fs.freeBytes(), (713L - 10 - 3) * 1024);
    CHECK(d.find("/F.BIN", e));
    CHECK(d.fs.writeFile(e, pattern(10, 4), &err));
    CHECK_EQ(d.fs.freeBytes(), (713L - 1 - 3) * 1024);
    CHECK(d.fs.writeFile(e, {}, &err));
    CHECK_EQ(d.fs.freeBytes(), (713L - 3) * 1024);
    CHECK(clean(d.fs));
}

TEST(fat12_delete_frees_and_lists_deleted) {
    Disk d;
    std::string err;
    CHECK(d.fs.createFile("/", "GONE.TXT", pattern(2048, 5), &err));
    DirEntry e;
    CHECK(d.find("/GONE.TXT", e));
    CHECK(d.fs.deleteFile(e, &err));
    CHECK_EQ(d.fs.freeBytes(), 713L * 1024);
    CHECK(d.fs.list("/").empty());
    auto all = d.fs.list("/", true);
    CHECK_EQ(all.size(), (size_t)1);
    CHECK(all[0].deleted);
    CHECK_EQ(all[0].name, std::string("?ONE.TXT"));
    CHECK(!d.find("/GONE.TXT", e));
    CHECK(clean(d.fs));
}

TEST(fat12_rename) {
    Disk d;
    std::string err;
    CHECK(d.fs.createFile("/", "OLD.BAS", pattern(10, 1), &err));
    CHECK(d.fs.createFile("/", "TAKEN.BAS", pattern(10, 2), &err));
    DirEntry e;
    CHECK(d.find("/OLD.BAS", e));
    CHECK(!d.fs.renameEntry(e, "TAKEN.BAS", &err));      // duplicate
    CHECK(!d.fs.renameEntry(e, "TOOLONGNAME.BAS", &err)); // not 8.3
    CHECK(!d.fs.renameEntry(e, "BAD*.BAS", &err));        // illegal char
    CHECK(d.find("/OLD.BAS", e));
    CHECK(d.fs.renameEntry(e, "new.bas", &err));
    CHECK_EQ(e.name, std::string("NEW.BAS"));
    d.remount();
    CHECK(d.find("/NEW.BAS", e));
    CHECK(d.fs.readFile(e) == pattern(10, 1));
    CHECK(!d.find("/OLD.BAS", e));
}

TEST(fat12_name_validation) {
    std::string err;
    CHECK(Fat12::validName83("AUTOEXEC.BAS", &err));
    CHECK(Fat12::validName83("A", &err));
    CHECK(Fat12::validName83("12345678.123", &err));
    CHECK(Fat12::validName83("GAME-1.$$$", &err));
    CHECK(!Fat12::validName83("", &err));
    CHECK(!Fat12::validName83("123456789", &err));
    CHECK(!Fat12::validName83("A.1234", &err));
    CHECK(!Fat12::validName83(".BAS", &err));
    CHECK(!Fat12::validName83("A.B.C", &err));
    CHECK(!Fat12::validName83("A B", &err));
    CHECK(!Fat12::validName83("A/B", &err));
    CHECK(!Fat12::validName83(".", &err));
}

TEST(fat12_create_rejects_bad_and_truncated_duplicates) {
    Disk d;
    std::string err;
    CHECK(d.fs.createFile("/", "LONGFILE.BAS", pattern(10, 1), &err));
    // Used to be silently truncated to LONGFILE.BAS, creating a duplicate entry.
    CHECK(!d.fs.createFile("/", "LONGFILENAME.BAS", pattern(10, 2), &err));
    CHECK(!d.fs.createFile("/", "longfile.bas", pattern(10, 2), &err));
    CHECK(!d.fs.createFile("/NOPE", "X.BIN", pattern(10, 2), &err));
    CHECK_EQ(d.fs.list("/").size(), (size_t)1);
}

TEST(fat12_disk_full_rolls_back) {
    Disk d(720);
    std::string err;
    long freeBefore = d.fs.freeBytes();
    // One byte more than the whole disk.
    CHECK(!d.fs.createFile("/", "HUGE.BIN", pattern((size_t)freeBefore + 1, 1), &err));
    CHECK(!err.empty());
    CHECK_EQ(d.fs.freeBytes(), freeBefore);   // no leaked clusters
    CHECK(d.fs.list("/").empty());            // no phantom 0-byte entry
    CHECK(clean(d.fs));
    // Growing an existing file past the end must leave it untouched.
    auto small = pattern(1500, 2);
    CHECK(d.fs.createFile("/", "SMALL.BIN", small, &err));
    DirEntry e; CHECK(d.find("/SMALL.BIN", e));
    long freeMid = d.fs.freeBytes();
    CHECK(!d.fs.writeFile(e, pattern((size_t)freeBefore + 10, 3), &err));
    CHECK_EQ(d.fs.freeBytes(), freeMid);
    CHECK(d.find("/SMALL.BIN", e));
    CHECK(d.fs.readFile(e) == small);
    // Exactly filling the disk works.
    CHECK(d.fs.createFile("/", "FILL.BIN", pattern((size_t)freeMid, 4), &err));
    CHECK_EQ(d.fs.freeBytes(), 0L);
    CHECK(clean(d.fs));
}

TEST(fat12_root_directory_full) {
    Disk d;
    std::string err;
    for (int i = 0; i < 112; ++i) {
        char nm[16]; snprintf(nm, sizeof nm, "F%03d.TXT", i);
        CHECK(d.fs.createFile("/", nm, {}, &err));
    }
    CHECK(!d.fs.createFile("/", "ONEMORE.TXT", {}, &err));
    CHECK_EQ(d.fs.list("/").size(), (size_t)112);
}

TEST(fat12_subdirectories) {
    Disk d;
    std::string err;
    CHECK(d.fs.makeDir("/", "GAMES", &err));
    CHECK(d.fs.makeDir("/GAMES", "SAVE", &err));
    CHECK(!d.fs.makeDir("/", "games", &err)); // exists
    auto data = pattern(3333, 7);
    CHECK(d.fs.createFile("/GAMES/SAVE", "SLOT1.DAT", data, &err));
    d.remount();
    DirEntry e;
    CHECK(d.find("/GAMES", e)); CHECK(e.isDir());
    CHECK(d.find("/games/save/slot1.dat", e));
    CHECK(d.fs.readFile(e) == data);
    auto sub = d.fs.list("/GAMES/SAVE");
    // ".", "..", SLOT1.DAT
    CHECK_EQ(sub.size(), (size_t)3);
    CHECK_EQ(sub[0].name, std::string("."));
    CHECK_EQ(sub[1].name, std::string(".."));
    CHECK(clean(d.fs));
}

TEST(fat12_subdirectory_grows_past_one_cluster) {
    Disk d;
    std::string err;
    CHECK(d.fs.makeDir("/", "MANY", &err));
    // One 1KB cluster holds 32 entries; "." and ".." take two.
    for (int i = 0; i < 100; ++i) {
        char nm[16]; snprintf(nm, sizeof nm, "N%03d", i);
        CHECK(d.fs.createFile("/MANY", nm, pattern(1, (uint8_t)i), &err));
    }
    d.remount();
    CHECK_EQ(d.fs.list("/MANY").size(), (size_t)102);
    DirEntry e;
    CHECK(d.find("/MANY/N099", e));
    CHECK(d.fs.readFile(e) == pattern(1, 99));
    CHECK(clean(d.fs));
}

TEST(fat12_remove_empty_directory) {
    Disk d;
    std::string err;
    CHECK(d.fs.makeDir("/", "TMP", &err));
    CHECK(d.fs.createFile("/TMP", "X", pattern(5, 1), &err));
    DirEntry dir;
    CHECK(d.find("/TMP", dir));
    CHECK(!d.fs.removeDir(dir, &err));   // not empty
    DirEntry x; CHECK(d.find("/TMP/X", x));
    CHECK(d.fs.deleteFile(x, &err));
    CHECK(d.find("/TMP", dir));
    CHECK(d.fs.removeDir(dir, &err));
    CHECK(!d.find("/TMP", dir));
    CHECK_EQ(d.fs.freeBytes(), 713L * 1024);
    CHECK(clean(d.fs));
}

TEST(fat12_fsck_detects_corruption) {
    Disk d;
    std::string err;
    CHECK(d.fs.createFile("/", "A.BIN", pattern(3000, 1), &err));
    CHECK(d.fs.createFile("/", "B.BIN", pattern(3000, 2), &err));
    DirEntry a, b;
    CHECK(d.find("/A.BIN", a)); CHECK(d.find("/B.BIN", b));
    // Point B's directory entry at A's chain: a cross-link.
    auto& raw = d.dk.rawMut();
    raw[(size_t)b.entryOffset + 0x1A] = (uint8_t)(a.startCluster & 0xFF);
    raw[(size_t)b.entryOffset + 0x1B] = (uint8_t)(a.startCluster >> 8);
    auto r = d.fs.verify();
    bool cross = false, lost = false;
    for (auto& i : r.issues) {
        if (i.find("cross-linked") != std::string::npos) cross = true;
    }
    CHECK(cross);
    CHECK_EQ(r.unreferenced, 3L); // B's original 3 clusters are now orphaned
    (void)lost;
}

TEST(fat12_fsck_detects_size_mismatch) {
    Disk d;
    std::string err;
    CHECK(d.fs.createFile("/", "A.BIN", pattern(3000, 1), &err)); // 3 clusters
    DirEntry a; CHECK(d.find("/A.BIN", a));
    auto& raw = d.dk.rawMut();
    raw[(size_t)a.entryOffset + 0x1C] = 0x00; // size 3000 -> 0x0B00 & 0xFF00 = 2816
    raw[(size_t)a.entryOffset + 0x1D] = 0x30; // 0x3000 = 12288 bytes > 3 clusters
    auto r = d.fs.verify();
    bool found = false;
    for (auto& i : r.issues) if (i.find("size") != std::string::npos) found = true;
    CHECK(found);
}

TEST(fat12_self_referencing_directory_terminates) {
    Disk d;
    std::string err;
    CHECK(d.fs.makeDir("/", "LOOP", &err));
    DirEntry loop; CHECK(d.find("/LOOP", loop));
    // Plant an entry inside LOOP that points back at LOOP itself.
    CHECK(d.fs.makeDir("/LOOP", "BACK", &err));
    DirEntry back; CHECK(d.find("/LOOP/BACK", back));
    auto& raw = d.dk.rawMut();
    raw[(size_t)back.entryOffset + 0x1A] = (uint8_t)(loop.startCluster & 0xFF);
    raw[(size_t)back.entryOffset + 0x1B] = (uint8_t)(loop.startCluster >> 8);
    auto r = d.fs.verify();       // must return, not recurse forever
    CHECK(!r.issues.empty());
    auto map = d.fs.clusterMap(); // same
    CHECK(!map.empty());
}

TEST(fat12_raw_unnamed_roundtrip) {
    Disk d;
    std::string err;
    CHECK(d.fs.createFile("/", "LOADER.BIN", pattern(700, 1), &err));
    CHECK(d.fs.createFile("/", "PAYLOAD", pattern(4096, 2), &err));
    DirEntry p; CHECK(d.find("/PAYLOAD", p));
    // Turn PAYLOAD into "RAW" data: wipe its directory entry but keep the FAT chain,
    // like loader disks do.
    auto& raw = d.dk.rawMut();
    memset(&raw[(size_t)p.entryOffset], 0, 32);
    // Put LOADER back at the start of the table so the directory stays contiguous.
    CHECK_EQ(d.fs.rawUnnamedCapacity(), 4096L);
    auto blob = d.fs.extractRawUnnamed();
    CHECK(blob == pattern(4096, 2));
    auto repl = pattern(4096, 3);
    long written = 0;
    CHECK(d.fs.writeRawUnnamed(repl, &written, &err));
    CHECK_EQ(written, 4096L);
    CHECK(d.fs.extractRawUnnamed() == repl);
    CHECK(!d.fs.writeRawUnnamed(pattern(5000, 4), &written, &err));
    CHECK_EQ(written, 4096L);
}

TEST(fat12_mount_rejects_garbage) {
    DiskImage dk; Fat12 fs; std::string err;
    std::vector<uint8_t> junk(737280, 0x00);
    CHECK(dk.loadBytes(junk, "zero.dsk", &err));
    CHECK(!fs.mount(dk, &err));
}

TEST(fat12_mount_falls_back_to_media_byte) {
    // MSX-DOS 1 era disks may carry a boot sector without a usable BPB; the
    // geometry then comes from the media descriptor in the first FAT byte.
    std::string err;
    auto img = formatBlankImage(1440);
    {
        DiskImage dk; Fat12 fs;
        CHECK(dk.loadBytes(img, "a.dsk", &err)); CHECK(fs.mount(dk, &err));
        CHECK(fs.createFile("/", "HELLO.TXT", pattern(1234, 1), &err));
        img = dk.raw();
    }
    memset(img.data() + 3, 0, 0x1E - 3); // wipe the BPB (keep the FAT/root)
    DiskImage dk; Fat12 fs;
    CHECK(dk.loadBytes(img, "b.dsk", &err));
    CHECK(fs.mount(dk, &err));
    CHECK_EQ(fs.totalClusters(), 713);
    DirEntry e; CHECK(fs.lookup("/HELLO.TXT", e));
    CHECK(fs.readFile(e) == pattern(1234, 1));
}

TEST(fat12_dir_entry_strings) {
    DirEntry e;
    e.date = (uint16_t)(((2024 - 1980) << 9) | (3 << 5) | 15);
    e.time = (uint16_t)((13 << 11) | (45 << 5) | (30 / 2));
    e.attr = 0x21;
    CHECK_EQ(e.dateString(), std::string("2024-03-15"));
    CHECK_EQ(e.timeString(), std::string("13:45:30"));
    CHECK_EQ(e.attrString(), std::string("R----A"));
}

// ---- Regressions found in review ------------------------------------------

TEST(fat12_entry_offsets_in_split_subdirectory) {
    // A directory whose second cluster is not adjacent to the first: entries
    // there used to get the offset of the wrong place on disk, so put/delete/
    // rename wrote into another file's data.
    Disk d;
    std::string err;
    CHECK(d.fs.makeDir("/", "SUB", &err));
    auto other = pattern(1024, 77);
    CHECK(d.fs.createFile("/", "OTHER.DAT", other, &err));   // the cluster right after SUB
    for (int i = 0; i < 31; ++i) {                            // spills into a new cluster
        char nm[16]; snprintf(nm, sizeof nm, "F%02d.TXT", i);
        CHECK(d.fs.createFile("/SUB", nm, pattern(10, (uint8_t)i), &err));
    }
    DirEntry f;
    CHECK(d.find("/SUB/F30.TXT", f));
    CHECK(d.fs.writeFile(f, pattern(3000, 5), &err));
    DirEntry o; CHECK(d.find("/OTHER.DAT", o));
    CHECK(d.fs.readFile(o) == other);
    CHECK(d.find("/SUB/F30.TXT", f));
    CHECK(d.fs.readFile(f) == pattern(3000, 5));
    CHECK(clean(d.fs));
    // Delete everything in SUB, then SUB itself.
    for (int i = 0; i < 31; ++i) {
        char nm[24]; snprintf(nm, sizeof nm, "/SUB/F%02d.TXT", i);
        DirEntry e; CHECK(d.find(nm, e)); CHECK(d.fs.deleteFile(e, &err));
    }
    CHECK(d.find("/OTHER.DAT", o)); CHECK(d.fs.readFile(o) == other);
    DirEntry sub; CHECK(d.find("/SUB", sub));
    CHECK(d.fs.removeDir(sub, &err));
    CHECK_EQ(d.fs.freeBytes(), (713L - 1) * 1024);
    CHECK(clean(d.fs));
}

TEST(fat12_dot_entries_cannot_be_removed_or_renamed) {
    Disk d;
    std::string err;
    CHECK(d.fs.makeDir("/", "SUB", &err));
    DirEntry e;
    CHECK(!d.find("/SUB/.", e));
    CHECK(!d.find("/SUB/..", e));
    for (auto& x : d.fs.list("/SUB")) {
        DirEntry y = x;
        CHECK(!d.fs.removeDir(x, &err));
        CHECK(!d.fs.renameEntry(y, "LOOP", &err));
        CHECK(!d.fs.deleteFile(x, &err));
    }
    CHECK(clean(d.fs));
}

TEST(fat12_media_fallback_clamped_to_image_and_failed_write_frees) {
    // A 360K dump whose (BPB-less) FAT claims the 720K media byte: mount must
    // not address sectors past the end, and a write that cannot fit leaves
    // the free space unchanged.
    std::string err;
    auto img = formatBlankImage(1440);
    img.resize(720 * 512);
    memset(img.data() + 3, 0, 0x1E - 3);
    DiskImage dk; Fat12 fs;
    CHECK(dk.loadBytes(img, "half.dsk", &err));
    CHECK(fs.mount(dk, &err));
    CHECK_EQ(fs.totalClusters(), (720 - 1 - 6 - 7) / 2);
    long before = fs.freeBytes();
    CHECK(!fs.createFile("/", "BIG.BIN", pattern(500 * 1024, 1), &err));
    CHECK_EQ(fs.freeBytes(), before);
    CHECK(fs.verify().issues.empty());
    CHECK_EQ(fs.verify().unreferenced, 0L);
}

TEST(fat12_write_repairs_looping_chain_instead_of_freeing_it) {
    Disk d;
    std::string err;
    CHECK(d.fs.createFile("/", "A.BIN", pattern(3000, 1), &err)); // clusters 2,3,4
    DirEntry a; CHECK(d.find("/A.BIN", a));
    // Corrupt: last cluster points back to the first.
    long fat = 512;
    auto& raw = d.dk.rawMut();
    for (int f = 0; f < 2; ++f) {               // FAT[4] = 2  (even entry: low 12 bits)
        long o = fat + f * 3 * 512 + 4 + 4 / 2;
        raw[(size_t)o] = 0x02; raw[(size_t)o + 1] = (uint8_t)((raw[(size_t)o + 1] & 0xF0));
    }
    CHECK(!d.fs.verify().issues.empty());
    CHECK(d.fs.writeFile(a, pattern(100, 2), &err));
    CHECK(d.find("/A.BIN", a));
    CHECK(d.fs.readFile(a) == pattern(100, 2));
    CHECK(clean(d.fs));
    CHECK_EQ(d.fs.freeBytes(), (713L - 1) * 1024);
}

TEST(fat12_full_directory_growth_is_accounted) {
    // Fill the disk so exactly one cluster is free.
    Disk d(720);
    std::string err;
    CHECK(d.fs.makeDir("/", "SUB", &err));
    for (int i = 0; i < 30; ++i) {              // "." + ".." + 30 = one full cluster
        char nm[16]; snprintf(nm, sizeof nm, "F%02d", i);
        CHECK(d.fs.createFile("/SUB", nm, {}, &err));
    }
    CHECK(d.fs.createFile("/", "FILL.BIN", pattern((size_t)d.fs.freeBytes() - 1024, 1), &err));
    CHECK_EQ(d.fs.freeBytes(), 1024L);
    // Creating a 1-cluster file in the full SUB needs 2 clusters: refuse cleanly.
    CHECK(!d.fs.createFile("/SUB", "X", pattern(100, 2), &err));
    CHECK_EQ(d.fs.freeBytes(), 1024L);
    // A subdirectory with a free slot only needs 1 cluster for a new folder.
    DirEntry f; CHECK(d.find("/SUB/F00", f)); CHECK(d.fs.deleteFile(f, &err));
    CHECK(d.fs.makeDir("/SUB", "INNER", &err));
    CHECK_EQ(d.fs.freeBytes(), 0L);
    CHECK(clean(d.fs));
}

// ---- Undelete, attributes, volume label ------------------------------------

TEST(fat12_undelete_restores_file) {
    Disk d;
    std::string err;
    auto data = pattern(5000, 3);
    CHECK(d.fs.createFile("/", "GAME.BIN", data, &err));
    CHECK(d.fs.createFile("/", "KEEP.BIN", pattern(100, 4), &err));
    DirEntry e; CHECK(d.find("/GAME.BIN", e));
    CHECK(d.fs.deleteFile(e, &err));
    DirEntry gone;
    for (auto& x : d.fs.list("/", true)) if (x.deleted) gone = x;
    CHECK_EQ(gone.name, std::string("?AME.BIN"));
    CHECK(!d.fs.undelete(gone, '*', &err));        // illegal first character
    CHECK(d.fs.undelete(gone, 'g', &err));
    CHECK_EQ(gone.name, std::string("GAME.BIN"));
    d.remount();
    CHECK(d.find("/GAME.BIN", e));
    CHECK(d.fs.readFile(e) == data);
    CHECK(clean(d.fs));
    CHECK_EQ(d.fs.freeBytes(), (713L - 5 - 1) * 1024);
}

TEST(fat12_undelete_refuses_reused_clusters_and_collisions) {
    Disk d;
    std::string err;
    CHECK(d.fs.createFile("/", "OLD.BIN", pattern(3000, 1), &err));  // clusters 2-4
    CHECK(d.fs.createFile("/", "KEEP.BIN", pattern(100, 2), &err));  // cluster 5
    DirEntry e; CHECK(d.find("/OLD.BIN", e));
    CHECK(d.fs.deleteFile(e, &err));
    DirEntry k; CHECK(d.find("/KEEP.BIN", k));
    CHECK(d.fs.writeFile(k, pattern(2000, 3), &err));                 // grows into cluster 2
    DirEntry gone;
    for (auto& x : d.fs.list("/", true)) if (x.deleted) gone = x;
    CHECK(gone.deleted);
    CHECK(!d.fs.undelete(gone, 'O', &err));
    CHECK(err.find("reused") != std::string::npos);
    // Name collision: a live file took the deleted file's name meanwhile.
    CHECK(d.fs.createFile("/", "TWIN.TXT", {}, &err));
    CHECK(d.fs.createFile("/", "OTHER.TXT", {}, &err));
    DirEntry t; CHECK(d.find("/TWIN.TXT", t)); CHECK(d.fs.deleteFile(t, &err));
    DirEntry o; CHECK(d.find("/OTHER.TXT", o)); CHECK(d.fs.renameEntry(o, "TWIN.TXT", &err));
    DirEntry gt;
    for (auto& x : d.fs.list("/", true)) if (x.deleted && x.name == "?WIN.TXT") gt = x;
    CHECK(gt.deleted);
    CHECK(!d.fs.undelete(gt, 'T', &err));
    CHECK(err.find("already") != std::string::npos);
    CHECK(clean(d.fs));
}

TEST(fat12_undelete_in_subdirectory) {
    Disk d;
    std::string err;
    CHECK(d.fs.makeDir("/", "SAVE", &err));
    CHECK(d.fs.createFile("/SAVE", "SLOT.DAT", pattern(1500, 9), &err));
    DirEntry e; CHECK(d.find("/SAVE/SLOT.DAT", e));
    CHECK(d.fs.deleteFile(e, &err));
    DirEntry gone;
    for (auto& x : d.fs.list("/SAVE", true)) if (x.deleted) gone = x;
    CHECK(d.fs.undelete(gone, 'S', &err));
    CHECK(d.find("/SAVE/SLOT.DAT", e));
    CHECK(d.fs.readFile(e) == pattern(1500, 9));
    CHECK(clean(d.fs));
}

TEST(fat12_set_attributes) {
    Disk d;
    std::string err;
    CHECK(d.fs.createFile("/", "A.BIN", pattern(10, 1), &err));
    CHECK(d.fs.makeDir("/", "DIR", &err));
    DirEntry e; CHECK(d.find("/A.BIN", e));
    CHECK(d.fs.setAttributes(e, 0x01 | 0x02 | 0x10 /* dir bit ignored */, &err));
    d.remount();
    CHECK(d.find("/A.BIN", e));
    CHECK_EQ(e.attr, (uint8_t)0x03);
    DirEntry dir; CHECK(d.find("/DIR", dir));
    CHECK(d.fs.setAttributes(dir, 0x02, &err));
    CHECK(d.find("/DIR", dir));
    CHECK_EQ(dir.attr, (uint8_t)0x12);       // still a directory
    CHECK(clean(d.fs));
}

TEST(fat12_volume_label) {
    Disk d;
    std::string err;
    CHECK(d.fs.volumeLabel().empty());
    CHECK(d.fs.setVolumeLabel("my disk 123", &err));
    CHECK_EQ(d.fs.volumeLabel(), std::string("MY DISK 123"));
    d.remount();
    CHECK_EQ(d.fs.volumeLabel(), std::string("MY DISK 123"));
    CHECK(d.fs.setVolumeLabel("GAMES", &err));   // replaces, no second entry
    int vols = 0;
    for (auto& e : d.fs.list("/")) if (e.isVolume()) vols++;
    CHECK_EQ(vols, 1);
    CHECK(!d.fs.setVolumeLabel("TWELVE CHARS", &err));
    CHECK(!d.fs.setVolumeLabel("A*B", &err));
    CHECK(d.fs.setVolumeLabel("", &err));
    d.remount();
    CHECK(d.fs.volumeLabel().empty());
    CHECK(clean(d.fs));
}
