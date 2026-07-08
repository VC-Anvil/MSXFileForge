// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Alonso (GS·RUN). MSXFileForge.
// Free software under the GNU GPL v3 (or later) - see LICENSE. Modified
// versions must be distributed under the same license.
#include "disk_image.h"
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <sstream>

namespace msx {

const char* to_string(DiskFormat f) {
    switch (f) {
        case DiskFormat::DSK_Plain: return "DSK (plain sectors)";
        case DiskFormat::RAW:       return "RAW sector dump";
        case DiskFormat::DMK:       return "DMK (track image)";
        case DiskFormat::HDD:       return "Hard disk image";
        default:                    return "Unknown";
    }
}

static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static bool readWhole(const std::string& path, std::vector<uint8_t>& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return false; }
    out.resize((size_t)n);
    size_t got = fread(out.data(), 1, (size_t)n, f);
    fclose(f);
    return got == (size_t)n;
}

static bool hasExt(const std::string& p, const char* ext) {
    if (p.size() < strlen(ext)) return false;
    std::string tail = p.substr(p.size() - strlen(ext));
    for (auto& c : tail) c = (char)tolower((unsigned char)c);
    return tail == ext;
}

bool DiskImage::load(const std::string& path, std::string* err) {
    path_ = path;
    format_ = DiskFormat::Unknown;
    raw_.clear(); sectors_.clear(); lba_.clear();
    cylinders_ = heads_ = spt_ = 0; protectedFlags_ = false;

    if (!readWhole(path, raw_)) { if (err) *err = "cannot read file"; return false; }

    // DMK is identified by extension + a plausible 16-byte header. Everything
    // else is treated as a flat sector dump.
    bool looksDmk = hasExt(path, ".dmk");
    if (!looksDmk && raw_.size() >= 16) {
        // A DMK track length at [2..3] that divides the body cleanly is a hint,
        // but we only auto-detect on extension to avoid mis-reading .dsk files.
    }

    if (looksDmk) {
        if (parseDmk(err)) { format_ = DiskFormat::DMK; return true; }
        // Fall through: some ".dmk" files are actually plain dumps.
    }
    if (parsePlain(err)) return true;
    if (err && err->empty()) *err = "unrecognized disk image";
    return false;
}

// ---- Plain / RAW ----------------------------------------------------------

void DiskImage::deriveGeometryFromBpb() {
    const uint8_t* b = raw_.data();
    int bps  = rd16(b + 0x0B);
    int spc  = b[0x0D];
    int tot  = rd16(b + 0x13);
    uint8_t media = b[0x15];

    // Trust the BPB only when it is internally consistent with a 512-byte MSX
    // floppy; otherwise fall back to size-based geometry.
    bool bpbOk = (bps == 512 && spc > 0 && tot > 0 &&
                  (long)tot * 512 <= (long)raw_.size() + 512);

    long totalSectors = (long)raw_.size() / 512;
    if (bpbOk && tot <= totalSectors) totalSectors = tot;

    // Standard MSX geometries keyed by media byte / total sectors.
    // 0xF9 = 720K DS (80x2x9), 0xF8 = 360K SS (80x1x9).
    if (totalSectors == 1440 || media == 0xF9) { cylinders_ = 80; heads_ = 2; spt_ = 9; }
    else if (totalSectors == 720 || media == 0xF8) { cylinders_ = 80; heads_ = 1; spt_ = 9; }
    else if (totalSectors == 1280) { cylinders_ = 80; heads_ = 2; spt_ = 8; }
    else if (totalSectors == 640)  { cylinders_ = 80; heads_ = 1; spt_ = 8; }
    else {
        // Best-effort: assume 9 spt, 2 heads.
        spt_ = 9; heads_ = 2;
        cylinders_ = (int)std::max<long>(1, totalSectors / (spt_ * heads_));
    }
}

void DiskImage::buildLbaMapForPlain() {
    long totalSectors = (long)raw_.size() / 512;
    lba_.resize((size_t)totalSectors);
    sectors_.clear();
    sectors_.reserve((size_t)totalSectors);
    int spt = spt_ > 0 ? spt_ : 9;
    int heads = heads_ > 0 ? heads_ : 2;
    for (long n = 0; n < totalSectors; ++n) {
        lba_[(size_t)n] = n * 512;
        SectorInfo si;
        long trackNo = n / spt;
        si.cyl = (int)(trackNo / heads);
        si.head = (int)(trackNo % heads);
        si.sec = (int)(n % spt) + 1;
        si.sizeCode = 2; si.size = 512;
        si.offset = n * 512;
        si.lba = n;
        sectors_.push_back(si);
    }
}

bool DiskImage::parsePlain(std::string* err) {
    if (raw_.size() < 512) { if (err) *err = "too small"; return false; }
    if (raw_.size() % 512 != 0) {
        // Not a clean sector dump. Still accept, padding the tail conceptually.
    }
    deriveGeometryFromBpb();
    buildLbaMapForPlain();
    // Classify plain vs raw by whether it carries a FAT-ish boot signature.
    const uint8_t* b = raw_.data();
    bool bootish = (b[0] == 0xEB || b[0] == 0xE9) && rd16(b + 0x0B) == 512;
    format_ = bootish ? DiskFormat::DSK_Plain : DiskFormat::RAW;
    return true;
}

// ---- DMK (David M. Keil track image) --------------------------------------
//
// Header (16 bytes):
//   [0]      write-protect (0xFF = protected)
//   [1]      number of tracks
//   [2..3]   track length (little-endian), includes the 0x80-byte IDAM table
//   [4]      option flags: bit4 = single-sided, bit6 = single-density(FM)
//   [5..11]  reserved, [12..15] 0 (real) / 0x12345678 (virtual/ old)
// Each track: 0x80-byte IDAM pointer table (64 x uint16 LE, bit15=MFM,
// bit14=density), each pointing (relative to track start) at the IDAM (0xFE)
// inside the track body. We walk those to recover every sector.
bool DiskImage::parseDmk(std::string* err) {
    if (raw_.size() < 16 + 0x80) { if (err) *err = "dmk too small"; return false; }
    const uint8_t* h = raw_.data();
    int ntracks = h[1];
    int trackLen = rd16(h + 2);
    bool singleSided = (h[4] & 0x10) != 0;
    if (ntracks <= 0 || ntracks > 100 || trackLen < 0x80 + 100) {
        if (err) *err = "dmk header implausible";
        return false;
    }
    int sides = singleSided ? 1 : 2;
    long need = 16 + (long)ntracks * sides * trackLen;
    if (need > (long)raw_.size() + trackLen) { // tolerate slight truncation
        if (err) *err = "dmk body truncated";
        return false;
    }

    heads_ = sides;
    cylinders_ = ntracks;
    // First pass: collect every physical sector; second pass assigns LBAs.
    int maxSecPerTrack = 0;
    struct Tmp { SectorInfo si; };
    std::vector<SectorInfo> phys;

    for (int t = 0; t < ntracks; ++t) {
        for (int s = 0; s < sides; ++s) {
            long trackOff = 16 + ((long)t * sides + s) * trackLen;
            if (trackOff + trackLen > (long)raw_.size()) break;
            const uint8_t* tk = raw_.data() + trackOff;
            int secOnTrack = 0;
            for (int i = 0; i < 64; ++i) {
                uint16_t ent = rd16(tk + i * 2);
                if (ent == 0) continue;
                int idamPtr = ent & 0x3FFF;    // offset within track of the IDAM
                bool mfm = (ent & 0x8000) != 0;
                if (idamPtr + 7 >= trackLen) { protectedFlags_ = true; continue; }
                const uint8_t* idam = tk + idamPtr;
                if (idam[0] != 0xFE) { protectedFlags_ = true; continue; }
                SectorInfo si;
                si.cyl = idam[1];
                si.head = idam[2];
                si.sec = idam[3];
                si.sizeCode = idam[4] & 3;
                si.size = (size_t)128 << si.sizeCode;
                if (si.size != 512) { si.nonStandard = true; protectedFlags_ = true; }
                if (si.cyl != t || si.head != s) { si.nonStandard = true; protectedFlags_ = true; }

                // Locate the data address mark (0xFB normal, 0xF8 deleted) that
                // follows the IDAM's CRC. Scan a short window forward.
                long dataOff = -1;
                int scanStart = idamPtr + 7;
                int scanEnd = std::min(trackLen, scanStart + (mfm ? 60 : 45));
                for (int p = scanStart; p < scanEnd; ++p) {
                    uint8_t v = tk[p];
                    if (v == 0xFB || v == 0xF8) {
                        if (v == 0xF8) { si.deleted = true; protectedFlags_ = true; }
                        dataOff = trackOff + p + 1;
                        break;
                    }
                }
                if (dataOff < 0) { si.nonStandard = true; protectedFlags_ = true; }
                else if (dataOff + (long)si.size <= (long)raw_.size()) si.offset = dataOff;
                else { si.nonStandard = true; protectedFlags_ = true; }

                phys.push_back(si);
                secOnTrack++;
            }
            maxSecPerTrack = std::max(maxSecPerTrack, secOnTrack);
        }
    }

    if (phys.empty()) { if (err) *err = "dmk: no sectors found"; return false; }
    spt_ = maxSecPerTrack;

    // Build a logical map for the standard-looking sectors so FAT12 can read a
    // regular filesystem straight out of a protected image. We map by CHS:
    // lba = (cyl*heads + head)*spt + (sec-1), only for 512-byte, in-range sectors.
    long maxLba = (long)cylinders_ * heads_ * (spt_ > 0 ? spt_ : 9);
    lba_.assign((size_t)maxLba, -1);
    for (auto& si : phys) {
        if (si.size == 512 && si.offset >= 0 && si.sec >= 1 &&
            !si.dataCrcError && !si.idCrcError) {
            long lba = ((long)si.cyl * heads_ + si.head) * (spt_ > 0 ? spt_ : 9) + (si.sec - 1);
            if (lba >= 0 && lba < maxLba && lba_[(size_t)lba] < 0) {
                lba_[(size_t)lba] = si.offset;
                si.lba = lba;
            }
        }
    }
    // Trim trailing unmapped holes so logicalSectors() reflects real content.
    while (!lba_.empty() && lba_.back() < 0) lba_.pop_back();

    sectors_ = std::move(phys);
    return true;
}

// ---- Logical access -------------------------------------------------------

bool DiskImage::readSector(long lba, uint8_t out[512]) const {
    if (lba < 0 || lba >= (long)lba_.size()) return false;
    long off = lba_[(size_t)lba];
    if (off < 0 || off + 512 > (long)raw_.size()) return false;
    memcpy(out, raw_.data() + off, 512);
    return true;
}

bool DiskImage::writeLogicalSector(long lba, const uint8_t in[512]) {
    if (lba < 0 || lba >= (long)lba_.size()) return false;
    long off = lba_[(size_t)lba];
    if (off < 0 || off + 512 > (long)raw_.size()) return false;
    memcpy(raw_.data() + off, in, 512);
    dirty_ = true;
    return true;
}

bool DiskImage::save(const std::string& path, std::string* err) const {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) { if (err) *err = "cannot open for writing"; return false; }
    size_t n = fwrite(raw_.data(), 1, raw_.size(), f);
    fclose(f);
    if (n != raw_.size()) { if (err) *err = "short write"; return false; }
    return true;
}

std::string DiskImage::protectionReport() const {
    if (!protectedFlags_) return {};
    int nonStd = 0, deleted = 0, badId = 0, badData = 0, dup = 0;
    for (auto& s : sectors_) {
        if (s.nonStandard) nonStd++;
        if (s.deleted) deleted++;
        if (s.idCrcError) badId++;
        if (s.dataCrcError) badData++;
    }
    std::ostringstream os;
    os << "Protection markers detected:";
    if (nonStd)  os << "\n  - " << nonStd  << " non-standard sector(s)";
    if (deleted) os << "\n  - " << deleted << " deleted-data address mark(s)";
    if (badId)   os << "\n  - " << badId   << " ID CRC error(s)";
    if (badData) os << "\n  - " << badData << " data CRC error(s)";
    (void)dup;
    return os.str();
}

} // namespace msx
