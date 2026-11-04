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
    std::vector<uint8_t> bytes;
    if (!readWhole(path, bytes)) {
        path_ = path; format_ = DiskFormat::Unknown;
        raw_.clear(); sectors_.clear(); lba_.clear();
        if (err) *err = "cannot read file";
        return false;
    }
    return loadBytes(std::move(bytes), path, err);
}

bool DiskImage::loadBytes(std::vector<uint8_t> bytes, const std::string& nameHint,
                          std::string* err) {
    path_ = nameHint;
    format_ = DiskFormat::Unknown;
    raw_ = std::move(bytes);
    sectors_.clear(); lba_.clear(); lbaSector_.clear();
    cylinders_ = heads_ = spt_ = 0; protectedFlags_ = false; dirty_ = false;
    if (err) err->clear();

    // DMK is identified by extension only, to avoid mis-reading .dsk files
    // whose first bytes happen to look like a DMK header.
    if (hasExt(nameHint, ".dmk")) {
        if (parseDmk(err)) { format_ = DiskFormat::DMK; return true; }
        // Fall through: some ".dmk" files are actually plain dumps.
        sectors_.clear(); lba_.clear(); lbaSector_.clear();
        cylinders_ = heads_ = spt_ = 0; protectedFlags_ = false;
    }
    if (parsePlain(err)) { if (err) err->clear(); return true; }
    if (err && err->empty()) *err = "unrecognized disk image";
    raw_.clear();
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
//
// CRCs are CRC-16/CCITT (poly 0x1021, init 0xFFFF). For MFM the three 0xA1
// sync bytes before each mark are included, which is the same as starting
// from 0xCDB4 at the mark byte itself.

uint16_t crc16Ccitt(const uint8_t* p, size_t n, uint16_t crc) {
    for (size_t i = 0; i < n; ++i) {
        crc ^= (uint16_t)(p[i] << 8);
        for (int k = 0; k < 8; ++k) crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}

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
    std::vector<SectorInfo> phys;
    std::vector<int> perTrack;   // standard (512-byte, in-place) sectors per track

    for (int t = 0; t < ntracks; ++t) {
        for (int s = 0; s < sides; ++s) {
            long trackOff = 16 + ((long)t * sides + s) * trackLen;
            if (trackOff + trackLen > (long)raw_.size()) break;
            const uint8_t* tk = raw_.data() + trackOff;
            int standard = 0;
            size_t firstOnTrack = phys.size();
            for (int i = 0; i < 64; ++i) {
                uint16_t ent = rd16(tk + i * 2);
                if (ent == 0) continue;
                int idamPtr = ent & 0x3FFF;    // offset within track of the IDAM
                bool mfm = (ent & 0x8000) != 0;
                if (idamPtr < 0x80 || idamPtr + 7 > trackLen) { protectedFlags_ = true; continue; }
                const uint8_t* idam = tk + idamPtr;
                if (idam[0] != 0xFE) { protectedFlags_ = true; continue; }
                SectorInfo si;
                si.cyl = idam[1];
                si.head = idam[2];
                si.sec = idam[3];
                si.sizeCode = idam[4] & 3;
                si.size = (size_t)128 << si.sizeCode;
                uint16_t idCrc = crc16Ccitt(idam, 5, mfm ? 0xCDB4 : 0xFFFF);
                si.idCrcError = idCrc != (uint16_t)((idam[5] << 8) | idam[6]);
                if (si.size != 512) si.nonStandard = true;
                if (si.cyl != t || si.head != s) si.nonStandard = true;

                // Locate the data address mark (0xFB normal, 0xF8 deleted) that
                // follows the IDAM's CRC. Scan a short window forward.
                long dataOff = -1;
                int scanStart = idamPtr + 7;
                int scanEnd = std::min(trackLen, scanStart + (mfm ? 60 : 45));
                int damPos = -1;
                for (int p = scanStart; p < scanEnd; ++p) {
                    uint8_t v = tk[p];
                    if (v == 0xFB || v == 0xF8) {
                        if (v == 0xF8) si.deleted = true;
                        dataOff = trackOff + p + 1;
                        damPos = p;
                        break;
                    }
                }
                si.mfm = mfm;
                if (dataOff < 0) si.nonStandard = true;
                else if (damPos + 1 + (long)si.size + 2 <= (long)trackLen) {
                    si.offset = dataOff;
                    si.markOffset = trackOff + damPos;
                    uint16_t dc = crc16Ccitt(tk + damPos, 1 + si.size, mfm ? 0xCDB4 : 0xFFFF);
                    const uint8_t* stored = tk + damPos + 1 + si.size;
                    si.dataCrcError = dc != (uint16_t)((stored[0] << 8) | stored[1]);
                } else if (dataOff + (long)si.size <= (long)raw_.size()) {
                    si.offset = dataOff;           // runs off the track: no CRC to check
                    si.nonStandard = true;
                } else si.nonStandard = true;

                // The same sector ID twice on one track is a classic protection.
                for (size_t k = firstOnTrack; k < phys.size(); ++k) {
                    if (phys[k].cyl == si.cyl && phys[k].head == si.head && phys[k].sec == si.sec) {
                        si.duplicateId = true; si.nonStandard = true;
                        phys[k].duplicateId = true; phys[k].nonStandard = true;
                    }
                }
                if (si.size == 512 && si.offset >= 0 && !si.idCrcError && !si.duplicateId &&
                    si.cyl == t && si.head == s)
                    standard++;
                phys.push_back(si);
            }
            perTrack.push_back(standard);
        }
    }

    if (phys.empty()) { if (err) *err = "dmk: no sectors found"; return false; }
    for (auto& si : phys)
        if (si.nonStandard || si.deleted || si.idCrcError || si.dataCrcError) protectedFlags_ = true;

    // Logical sectors per track: the BPB of a readable boot sector wins; else
    // the most common count of standard sectors (a protected track with extra
    // or missing sectors must not shift every other track's LBAs).
    int spt = 0;
    for (auto& si : phys) {
        if (si.cyl == 0 && si.head == 0 && si.sec == 1 && si.size == 512 && si.offset >= 0 &&
            !si.idCrcError) {
            const uint8_t* b = raw_.data() + si.offset;
            int bpbSpt = rd16(b + 0x18);
            if (rd16(b + 0x0B) == 512 && bpbSpt >= 8 && bpbSpt <= 10) spt = bpbSpt;
            break;
        }
    }
    if (spt == 0) {
        int counts[65] = {0};
        for (int c : perTrack) if (c > 0 && c <= 64) counts[c]++;
        for (int c = 1; c <= 64; ++c) if (counts[c] > counts[spt]) spt = c;
        if (spt == 0) spt = 9;
    }
    spt_ = spt;

    // Build a logical map for the standard-looking sectors so FAT12 can read a
    // regular filesystem straight out of a protected image. We map by CHS:
    // lba = (cyl*heads + head)*spt + (sec-1), only for 512-byte sectors whose
    // ID is readable and unique. A data CRC error still maps (a real FDC
    // returns the bytes along with the error status).
    long maxLba = (long)cylinders_ * heads_ * spt_;
    lba_.assign((size_t)maxLba, -1);
    lbaSector_.assign((size_t)maxLba, -1);
    for (size_t k = 0; k < phys.size(); ++k) {
        SectorInfo& si = phys[k];
        if (si.size == 512 && si.offset >= 0 && si.sec >= 1 && si.sec <= spt_ &&
            si.head < heads_ && !si.idCrcError && !si.duplicateId) {
            long lba = ((long)si.cyl * heads_ + si.head) * spt_ + (si.sec - 1);
            if (lba >= 0 && lba < maxLba && lba_[(size_t)lba] < 0) {
                lba_[(size_t)lba] = si.offset;
                lbaSector_[(size_t)lba] = (int)k;
                si.lba = lba;
            }
        }
    }
    // Trim trailing unmapped holes so logicalSectors() reflects real content.
    while (!lba_.empty() && lba_.back() < 0) { lba_.pop_back(); lbaSector_.pop_back(); }

    sectors_ = std::move(phys);
    return true;
}

std::vector<uint8_t> dskToDmk(const std::vector<uint8_t>& dsk, std::string* err) {
    std::vector<uint8_t> out;
    if (dsk.size() < 512) { if (err) *err = "image too small"; return out; }
    long tot = (rd16(&dsk[0x0B]) == 512) ? rd16(&dsk[0x13]) : 0;
    if (tot <= 0 || (long)dsk.size() < tot * 512) tot = (long)dsk.size() / 512;
    int spt, heads;
    switch (tot) {
        case 1440: spt = 9; heads = 2; break;
        case 720:  spt = 9; heads = (dsk[0x15] == 0xFD) ? 2 : 1; break;
        case 1280: spt = 8; heads = 2; break;
        case 640:  spt = 8; heads = (dsk[0x15] == 0xFF) ? 2 : 1; break;
        case 360:  spt = 9; heads = 1; break;
        case 320:  spt = 8; heads = 1; break;
        default:
            if (err) *err = "unsupported geometry (" + std::to_string(tot) + " sectors)";
            return out;
    }
    int cyls = (int)(tot / (spt * heads));

    const int kTrackLen = 0x1900;                     // usual DD DMK track size
    const int kFixed = 12 + 4 + 4 + 2 + 22 + 12 + 4 + 512 + 2; // per sector, sans gap3
    const int kLead = 80 + 12 + 4 + 50;               // gap4a, sync, IAM, gap1
    int gap3 = (kTrackLen - 0x80 - kLead) / spt - kFixed;
    if (gap3 > 84) gap3 = 84;
    if (gap3 < 10) gap3 = 10;

    uint8_t hdr[16] = {0};
    hdr[1] = (uint8_t)cyls;
    hdr[2] = kTrackLen & 0xFF; hdr[3] = (kTrackLen >> 8) & 0xFF;
    hdr[4] = (heads == 1) ? 0x10 : 0x00;
    out.insert(out.end(), hdr, hdr + 16);

    for (int c = 0; c < cyls; ++c) {
        for (int h = 0; h < heads; ++h) {
            std::vector<uint8_t> tk((size_t)kTrackLen, 0x4E);
            memset(tk.data(), 0, 0x80);
            size_t p = 0x80;
            auto fill = [&](int n, uint8_t v) { for (int i = 0; i < n; ++i) tk[p++] = v; };
            fill(80, 0x4E); fill(12, 0x00);
            tk[p++] = 0xC2; tk[p++] = 0xC2; tk[p++] = 0xC2; tk[p++] = 0xFC; // index mark
            fill(50, 0x4E);
            for (int s = 0; s < spt; ++s) {
                fill(12, 0x00); fill(3, 0xA1);
                size_t idam = p;
                tk[p++] = 0xFE; tk[p++] = (uint8_t)c; tk[p++] = (uint8_t)h;
                tk[p++] = (uint8_t)(s + 1); tk[p++] = 0x02;
                uint16_t crc = crc16Ccitt(&tk[idam], 5, 0xCDB4);
                tk[p++] = (uint8_t)(crc >> 8); tk[p++] = (uint8_t)crc;
                fill(22, 0x4E); fill(12, 0x00); fill(3, 0xA1);
                size_t dam = p;
                tk[p++] = 0xFB;
                long src = (((long)c * heads + h) * spt + s) * 512;
                if (src + 512 <= (long)dsk.size()) memcpy(&tk[p], &dsk[(size_t)src], 512);
                else memset(&tk[p], 0xE5, 512);
                p += 512;
                crc = crc16Ccitt(&tk[dam], 513, 0xCDB4);
                tk[p++] = (uint8_t)(crc >> 8); tk[p++] = (uint8_t)crc;
                fill(gap3, 0x4E);
                uint16_t ptr = (uint16_t)(idam | 0x8000);   // bit15 = MFM
                tk[(size_t)s * 2] = ptr & 0xFF; tk[(size_t)s * 2 + 1] = ptr >> 8;
            }
            out.insert(out.end(), tk.begin(), tk.end());
        }
    }
    return out;
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
    // DMK: a sector whose data field runs past its track end stays readable,
    // but writing it would clobber the next track's header, so refuse.
    if (format_ == DiskFormat::DMK) {
        if ((size_t)lba >= lbaSector_.size() || lbaSector_[(size_t)lba] < 0) return false;
        if (sectors_[(size_t)lbaSector_[(size_t)lba]].markOffset < 0) return false;
    }
    memcpy(raw_.data() + off, in, 512);
    // DMK: keep the data CRC valid, unless the sector carried a deliberate CRC
    // error (copy protection), which is preserved as found.
    if (format_ == DiskFormat::DMK && (size_t)lba < lbaSector_.size() && lbaSector_[(size_t)lba] >= 0) {
        const SectorInfo& si = sectors_[(size_t)lbaSector_[(size_t)lba]];
        if (si.markOffset >= 0 && !si.dataCrcError &&
            si.markOffset + 1 + 512 + 2 <= (long)raw_.size()) {
            uint16_t crc = crc16Ccitt(raw_.data() + si.markOffset, 513, si.mfm ? 0xCDB4 : 0xFFFF);
            raw_[(size_t)si.markOffset + 513] = (uint8_t)(crc >> 8);
            raw_[(size_t)si.markOffset + 514] = (uint8_t)crc;
        }
    }
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
        if (s.duplicateId) dup++;
    }
    std::ostringstream os;
    os << "Protection markers detected:";
    if (nonStd)  os << "\n  - " << nonStd  << " non-standard sector(s)";
    if (deleted) os << "\n  - " << deleted << " deleted-data address mark(s)";
    if (badId)   os << "\n  - " << badId   << " ID CRC error(s)";
    if (badData) os << "\n  - " << badData << " data CRC error(s)";
    if (dup)     os << "\n  - " << dup     << " sector(s) sharing a duplicate ID";
    return os.str();
}

} // namespace msx
