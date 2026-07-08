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
    long entryOffset = -1; // byte offset of the 32-byte record in the image

    bool isDir() const { return (attr & 0x10) != 0; }
    bool isVolume() const { return (attr & 0x08) != 0; }
    std::string dateString() const;
    std::string timeString() const;
    std::string attrString() const;
};

class Fat12 {
public:
    // Mount the filesystem. Returns false if the boot sector / BPB is not FAT12.
    bool mount(const DiskImage& disk, std::string* err = nullptr);
    bool mounted() const { return mounted_; }

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

private:
    std::vector<DirEntry> readDirAtCluster(int cluster, bool includeDeleted) const;
    std::vector<DirEntry> readRootDir(bool includeDeleted) const;
    int fatEntry(int cluster) const;
    bool sectorBytes(long lba, uint8_t out[512]) const;
    const DirEntry* resolve(const std::string& path) const;
    static std::vector<DirEntry> parseEntries(const std::vector<uint8_t>& buf,
                                              long baseOffset, bool includeDeleted);

    const DiskImage* disk_ = nullptr;
    bool mounted_ = false;
    int bps_ = 512, spc_ = 2, rsvd_ = 1, nfat_ = 2, ndir_ = 112;
    int totalSectors_ = 0, spf_ = 3, totalClusters_ = 0;
    uint8_t media_ = 0;
    long fatStartSec_ = 0, rootStartSec_ = 0, dataStartSec_ = 0;
    int rootSectors_ = 0;
    std::string volLabel_, oem_;
};

} // namespace msx
