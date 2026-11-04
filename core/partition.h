// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Alonso (GS·RUN). MSXFileForge.
// Free software under the GNU GPL v3 (or later) - see LICENSE. Modified
// versions must be distributed under the same license.
// MSXFileForge - hard disk / SD card images: MBR partitions and FAT volumes.
//
// MSX-DOS 2 and Nextor devices use a PC-style MBR. Nextor's FDISK typically
// creates one primary partition plus an extended partition holding further
// logical drives, all FAT12 or FAT16. A "superfloppy" (no partition table,
// FAT boot sector at sector 0) is reported as having no partitions.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "disk_image.h"

namespace msx {

struct Partition {
    int number = 0;          // 1-based, in table order (primaries, then logicals)
    uint8_t type = 0;        // MBR type byte (0x01 FAT12, 0x04/0x06/0x0E FAT16, ...)
    long firstSector = 0;    // absolute LBA in the image
    long sectorCount = 0;
    bool logical = false;    // inside an extended partition
    bool fat() const { return type == 0x01 || type == 0x04 || type == 0x06 || type == 0x0E; }
};

const char* partitionTypeName(uint8_t type);

// True if the 512 bytes look like a FAT boot sector with a usable BPB.
bool looksLikeFatBoot(const uint8_t sector[512]);

// Partitions listed by the MBR (and the chain of extended boot records).
// Empty when sector 0 is not an MBR (floppy / superfloppy images).
std::vector<Partition> findPartitions(const DiskImage& disk);

// A freshly formatted FAT volume of `totalSectors` 512-byte sectors: FAT16
// when it can hold 4085+ clusters, else FAT12. `hiddenSectors` is the
// volume's LBA on its disk (stored in the BPB). `label` up to 11 chars.
std::vector<uint8_t> formatFatVolume(long totalSectors, long hiddenSectors = 0,
                                     const std::string& label = "");

// A whole disk image: an MBR followed by one FAT volume per entry of
// `sizesSectors`. Up to 4 become primary partitions; with more, the first is
// primary and the rest are logical drives in an extended partition.
std::vector<uint8_t> buildPartitionedImage(const std::vector<long>& sizesSectors,
                                           std::string* err = nullptr);

} // namespace msx
