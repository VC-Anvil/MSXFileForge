// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Alonso (GS·RUN). MSXFileForge.
// Free software under the GNU GPL v3 (or later) - see LICENSE. Modified
// versions must be distributed under the same license.
// MSXFileForge - disk image abstraction.
//
// A floppy is presented as a flat, logical 512-byte sector space no matter what
// container it came from:
//   * .dsk / raw  -> already a flat sector dump (SS/DS, 360/720KB, ...).
//   * .dmk        -> David M. Keil track image; preserves the real IDAM/sector
//                    layout, so copy-protected disks (non-standard sizes, weak
//                    bits, deleted address marks, CRC errors, duplicate IDs)
//                    survive here where a plain .dsk would not.
//
// The class both reconstructs the logical sector space (so FAT12 can read a
// normal filesystem even out of a .dmk) AND exposes the per-sector physical
// truth via sectors(), which is what makes the "protected" view meaningful.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace msx {

enum class DiskFormat { Unknown, DSK_Plain, RAW, DMK, HDD };

const char* to_string(DiskFormat f);

// Physical description of one sector as it was found on the medium.
struct SectorInfo {
    int  cyl = 0, head = 0, sec = 0;   // CHS id as recorded in the IDAM
    int  sizeCode = 2;                 // 0=128 1=256 2=512 3=1024
    size_t size = 512;                 // declared payload size
    long offset = -1;                  // byte offset of payload in raw image, or -1
    bool deleted = false;              // deleted-data address mark (protection)
    bool idCrcError = false;
    bool dataCrcError = false;
    bool nonStandard = false;          // size!=512, dup id, or otherwise irregular
    long lba = -1;                     // logical block assigned, or -1 if unmapped
};

class DiskImage {
public:
    bool load(const std::string& path, std::string* err = nullptr);

    bool loaded() const { return !raw_.empty(); }
    DiskFormat format() const { return format_; }
    const std::string& path() const { return path_; }

    // Logical geometry (derived from the BPB when present, else from size).
    int cylinders() const { return cylinders_; }
    int heads() const { return heads_; }
    int sectorsPerTrack() const { return spt_; }
    int sectorSize() const { return 512; }
    long logicalSectors() const { return (long)lba_.size(); }

    // Read one logical 512-byte sector. Returns false if out of range or the
    // sector is unmapped (a hole a protected image can legitimately have).
    bool readSector(long lba, uint8_t out[512]) const;

    // Write one logical 512-byte sector back into the container. Returns false
    // if the sector is out of range or unmapped. Sets the dirty flag.
    bool writeLogicalSector(long lba, const uint8_t in[512]);

    // Persist the (possibly edited) container bytes to disk.
    bool save(const std::string& path, std::string* err = nullptr) const;
    bool dirty() const { return dirty_; }
    void clearDirty() { dirty_ = false; }
    void markDirty() { dirty_ = true; }

    // Mutable container bytes (for the raw hex editor). Does NOT mark dirty by
    // itself - callers that actually change bytes should call markDirty().
    std::vector<uint8_t>& rawMut() { return raw_; }

    // Physical sector table (for the protected/RAW inspector).
    const std::vector<SectorInfo>& sectors() const { return sectors_; }

    // True if any physical sector looks like copy protection.
    bool isProtected() const { return protectedFlags_; }
    // Human summary of why it is considered protected (empty if not).
    std::string protectionReport() const;

    // Raw container bytes (for the whole-image hex view / RAW mode).
    const std::vector<uint8_t>& raw() const { return raw_; }

private:
    bool parsePlain(std::string* err);
    bool parseDmk(std::string* err);
    void deriveGeometryFromBpb();
    void buildLbaMapForPlain();

    std::string path_;
    DiskFormat format_ = DiskFormat::Unknown;
    std::vector<uint8_t> raw_;
    std::vector<SectorInfo> sectors_;
    std::vector<long> lba_;   // lba_[n] = offset of logical sector n, or -1
    int cylinders_ = 0, heads_ = 0, spt_ = 0;
    bool protectedFlags_ = false;
    bool dirty_ = false;
};

} // namespace msx
