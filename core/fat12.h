// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Alonso (GS·RUN). MSXFileForge.
// Free software under the GNU GPL v3 (or later) - see LICENSE. Modified
// versions must be distributed under the same license.
// MSXFileForge - FAT12 filesystem reader over a DiskImage.
//
// MSX-DOS disks are plain FAT12. This sits on the logical sector space provided
// by DiskImage, so it reads a normal filesystem regardless of whether the
// underlying container was a .dsk or a protected .dmk (as long as the FS sectors
// themselves are standard). Directories (including subdirectories) are walked,
// files are read by cluster chain, and deleted entries can be surfaced for
// recovery.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "disk_image.h"

namespace msx {

struct DirEntry {
    std::string name;      // 8.3, trimmed, e.g. "AUTOEXEC.BAS"
    std::string raw83;     // packed 11-char field, for round-trips
    uint8_t attr = 0;      // 0x01 RO 0x02 HID 0x04 SYS 0x08 VOL 0x10 DIR 0x20 ARC
    uint16_t startCluster = 0;
    uint32_t size = 0;
    uint16_t date = 0, time = 0;
    bool deleted = false;  // first byte 0xE5 (recoverable)
    long entryOffset = -1; // byte offset of the 32-byte record, relative to the volume

    bool isDir() const { return (attr & 0x10) != 0; }
    bool isVolume() const { return (attr & 0x08) != 0; }
    std::string dateString() const;
    std::string timeString() const;
    std::string attrString() const;
};

class Fat12 {
public:
    // Mount the filesystem. Returns false if the boot sector / BPB is not FAT12.
    // Takes a non-const image so files can be written back in place.
    bool mount(DiskImage& disk, std::string* err = nullptr);
    // Mount a volume that starts at `firstSector` of the image (a partition of
    // a hard disk / SD card image). `sectorCount` caps the volume (0 = to the
    // end of the image).
    bool mount(DiskImage& disk, long firstSector, long sectorCount, std::string* err = nullptr);
    bool mounted() const { return mounted_; }

    // 12 or 16. FAT12 below 4085 clusters, FAT16 from there (as in MS-DOS).
    int fatBits() const { return fat16_ ? 16 : 12; }
    // First sector of the mounted volume inside the image (0 for floppies).
    long volumeStart() const { return partBase_; }

    // BPB-derived facts.
    int bytesPerSector() const { return bps_; }
    int sectorsPerCluster() const { return spc_; }
    int totalClusters() const { return totalClusters_; }
    long totalBytes() const { return (long)totalSectors_ * bps_; }
    long freeBytes() const;
    std::string volumeLabel() const { return volLabel_; }
    std::string oemName() const { return oem_; }

    // List a directory. path is "/" for root, or "/GAMES/SAVE" etc.
    // If includeDeleted, 0xE5 entries are returned too (deleted==true).
    std::vector<DirEntry> list(const std::string& path, bool includeDeleted = false) const;

    // Read a file's bytes by following its cluster chain (clamped to size).
    std::vector<uint8_t> readFile(const DirEntry& e) const;

    // ---- Write support ----
    // Overwrite a file's contents, growing/shrinking its cluster chain as
    // needed. Updates both FAT copies and the directory entry size. `e` is
    // updated in place (size, startCluster). Returns false (with err) if there
    // is not enough free space. The change lives in the in-memory image; call
    // DiskImage::save() to persist. Re-list the directory afterwards.
    bool writeFile(DirEntry& e, const std::vector<uint8_t>& data, std::string* err = nullptr);

    // Mark a file deleted (0xE5) and free its clusters. Re-list afterwards.
    bool deleteFile(const DirEntry& e, std::string* err = nullptr);

    // Create a NEW file `name` (8.3) in directory `dirPath` ("/" or "/SUB")
    // with the given bytes. Fails if the name exists or the directory is full.
    bool createFile(const std::string& dirPath, const std::string& name,
                    const std::vector<uint8_t>& data, std::string* err = nullptr);

    // Remove an EMPTY subdirectory (only "." and ".." left) and free its chain.
    bool removeDir(const DirEntry& e, std::string* err = nullptr);

    // Recover a deleted FILE (first name byte 0xE5). Like MSX-DOS UNDEL, this
    // assumes the file's clusters were contiguous from its start cluster and
    // requires every one of them to still be free; `firstChar` replaces the
    // lost first letter. Updates `e`. Directories are not recoverable.
    bool undelete(DirEntry& e, char firstChar, std::string* err = nullptr);

    // Set the read-only / hidden / system / archive bits (0x01 0x02 0x04 0x20)
    // of an entry; the directory and volume bits are kept. Updates `e`.
    bool setAttributes(DirEntry& e, uint8_t attr, std::string* err = nullptr);

    // Set (or, with an empty string, remove) the volume label: the root
    // directory VOL entry, up to 11 characters, stored in upper case.
    bool setVolumeLabel(const std::string& label, std::string* err = nullptr);

    // Find a file or directory by path ("/GAMES/SAVE/SLOT1.DAT", case-insensitive).
    // Returns false if any component is missing.
    bool lookup(const std::string& path, DirEntry& out) const;

    // True if `name` is a legal MSX-DOS 8.3 name (1-8 char base, optional 1-3
    // char extension, no spaces or reserved characters). Case-insensitive.
    static bool validName83(const std::string& name, std::string* err = nullptr);

    // Turn an arbitrary host file name into a valid 8.3 name that is not in
    // `taken` (case-insensitive): upper case, spaces dropped, reserved
    // characters -> '_', base cut to 8 and extension to 3, "~N" on clashes.
    static std::string suggestName83(const std::string& hostName,
                                     const std::vector<std::string>& taken);

    // Timestamp written into new/modified directory entries (FAT date/time
    // words). By default the current local time is used; a fixed stamp makes
    // output reproducible (tests, deterministic builds).
    void setTimestamp(uint16_t date, uint16_t time) { fixedDate_ = date; fixedTime_ = time; fixedStamp_ = true; }
    void useCurrentTime() { fixedStamp_ = false; }

    // Rename an entry in place (8.3). Updates `e`.
    bool renameEntry(DirEntry& e, const std::string& newName, std::string* err = nullptr);

    // Create a subdirectory `name` inside `parentPath` (with . and .. entries).
    bool makeDir(const std::string& parentPath, const std::string& name, std::string* err = nullptr);

    // Filesystem integrity report.
    struct FsCheck {
        long freeClusters = 0, usedByFiles = 0, unreferenced = 0;
        std::vector<std::string> issues;   // empty == clean
    };
    FsCheck verify() const;

    // ---- RAW / uncatalogued data ----
    // Many MSX games (Fray, Xak, ...) store their bulk data in raw sectors that
    // are marked allocated in the FAT but have NO directory entry - a custom
    // loader reads them by sector, not by name. This is NOT encryption.
    // clusterMap() returns one status per data cluster (cluster n -> index n-2):
    //   0 = free, 1 = belongs to a named file/dir, 2 = allocated but unnamed (RAW).
    enum { CL_FREE = 0, CL_NAMED = 1, CL_RAW = 2 };
    std::vector<uint8_t> clusterMap() const;

    // Concatenate every allocated-but-unnamed cluster's bytes, in cluster order.
    // This is the raw game payload the loader consumes.
    std::vector<uint8_t> extractRawUnnamed() const;

    // Inverse of extractRawUnnamed: write `data` back across the RAW (allocated-
    // but-unnamed) clusters, in the same order. Exact round-trip for same-size
    // data. If data exceeds the RAW capacity it is truncated (err set, returns
    // false); *written receives the bytes actually stored.
    bool writeRawUnnamed(const std::vector<uint8_t>& data, long* written = nullptr,
                         std::string* err = nullptr);

    // Total bytes available in the RAW region (allocated-but-unnamed clusters).
    long rawUnnamedCapacity() const;

    // Byte offset where a data cluster begins, relative to the volume (add
    // volumeStart() * 512 for the position in the image).
    long clusterByteOffset(int cluster) const;

    int firstDataCluster() const { return 2; }
    int dataClusterCount() const { return totalClusters_; }

private:
    int  resolveDirCluster(const std::string& path) const; // 0=root, -1=not found
    long freeDirSlot(int dirCluster, bool allowGrow = true); // logical offset or -1
    void writeDirEntry(long off, const uint8_t name11[11], uint8_t attr,
                       uint16_t cluster, uint32_t size);
    void stamp(uint16_t& date, uint16_t& time) const;
    int  countFreeClusters() const;
    bool nameTaken(int dirCluster, const uint8_t name11[11], long ignoreOffset = -1) const;
    std::vector<DirEntry> readDir(int dirCluster, bool includeDeleted) const;
    bool mountFromMediaByte(std::string* err);
    static void pack83(const std::string& name, uint8_t out[11]);
    void collectNamedClusters(int cluster, std::vector<uint8_t>& status, bool isRoot) const;
    bool writeByteAt(long imageOffset, uint8_t value);
    bool readByteAt(long imageOffset, uint8_t& out) const;
    void setFatEntry(int cluster, int value);   // writes all FAT copies
    int  allocCluster();                         // first free cluster, or -1
    std::vector<int> chainOf(int startCluster) const;
    bool writeClusterData(int cluster, const uint8_t* src, long len);
    std::vector<DirEntry> readDirAtCluster(int cluster, bool includeDeleted) const;
    std::vector<DirEntry> readRootDir(bool includeDeleted) const;
    int fatEntry(int cluster) const;
    bool sectorBytes(long lba, uint8_t out[512]) const;   // volume-relative read
    bool putSector(long lba, const uint8_t in[512]);       // volume-relative write
    static std::vector<DirEntry> parseEntries(const std::vector<uint8_t>& buf,
                                              long baseOffset, bool includeDeleted,
                                              bool* ended = nullptr);

    DiskImage* disk_ = nullptr;
    bool mounted_ = false;
    int bps_ = 512, spc_ = 2, rsvd_ = 1, nfat_ = 2, ndir_ = 112;
    int totalSectors_ = 0, spf_ = 3, totalClusters_ = 0;
    bool fat16_ = false;
    int eoc_ = 0xFFF, eocMin_ = 0xFF8, bad_ = 0xFF7, rsvMin_ = 0xFF0; // per FAT width
    long partBase_ = 0, partLimit_ = 0;
    int nextFree_ = 2;                          // no free cluster below this one
    mutable long cacheLba_ = -1;                // one-sector read cache
    mutable const DiskImage* cacheDisk_ = nullptr;
    mutable uint64_t cacheGen_ = 0;
    mutable uint8_t cache_[512] = {0};
    uint8_t media_ = 0;
    long fatStartSec_ = 0, rootStartSec_ = 0, dataStartSec_ = 0;
    int rootSectors_ = 0;
    std::string volLabel_, oem_;
    bool fixedStamp_ = false;
    uint16_t fixedDate_ = 0, fixedTime_ = 0;
};

} // namespace msx
