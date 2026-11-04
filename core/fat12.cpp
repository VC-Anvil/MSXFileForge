// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Alonso (GS·RUN). MSXFileForge.
// Free software under the GNU GPL v3 (or later) - see LICENSE. Modified
// versions must be distributed under the same license.
#include "fat12.h"
#include <cstring>
#include <cstdio>
#include <sstream>
#include <iomanip>
#include <functional>
#include <set>
#include <ctime>

namespace msx {

static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t* p) { return p[0] | (p[1]<<8) | (p[2]<<16) | ((uint32_t)p[3]<<24); }

static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(' ');
    if (a == std::string::npos) return {};
    size_t b = s.find_last_not_of(' ');
    return s.substr(a, b - a + 1);
}

std::string DirEntry::dateString() const {
    if (date == 0) return "----------";
    int y = 1980 + ((date >> 9) & 0x7F);
    int m = (date >> 5) & 0x0F;
    int d = date & 0x1F;
    std::ostringstream os;
    os << std::setfill('0') << std::setw(4) << y << '-'
       << std::setw(2) << m << '-' << std::setw(2) << d;
    return os.str();
}
std::string DirEntry::timeString() const {
    int h = (time >> 11) & 0x1F;
    int mi = (time >> 5) & 0x3F;
    int s = (time & 0x1F) * 2;
    std::ostringstream os;
    os << std::setfill('0') << std::setw(2) << h << ':'
       << std::setw(2) << mi << ':' << std::setw(2) << s;
    return os.str();
}
std::string DirEntry::attrString() const {
    std::string a = "------";
    if (attr & 0x01) a[0] = 'R';
    if (attr & 0x02) a[1] = 'H';
    if (attr & 0x04) a[2] = 'S';
    if (attr & 0x08) a[3] = 'V';
    if (attr & 0x10) a[4] = 'D';
    if (attr & 0x20) a[5] = 'A';
    return a;
}

bool Fat12::sectorBytes(long lba, uint8_t out[512]) const {
    return disk_ && disk_->readSector(lba, out);
}

// Standard MSX/PC floppy geometries, keyed by the media descriptor byte that
// also opens the FAT. Used when the boot sector carries no usable BPB (some
// MSX-DOS 1 era disks and many game disks with custom boot code).
namespace {
struct MediaGeo { uint8_t media; int spc, ndir, spf, total; };
const MediaGeo kMediaGeo[] = {
    {0xF8, 2, 112, 2,  720},   // 360K SS, 80 tracks, 9 spt
    {0xF9, 2, 112, 3, 1440},   // 720K DS, 80 tracks, 9 spt
    {0xFA, 2, 112, 1,  640},   // 320K SS, 80 tracks, 8 spt
    {0xFB, 2, 112, 2, 1280},   // 640K DS, 80 tracks, 8 spt
    {0xFC, 1,  64, 2,  360},   // 180K SS, 40 tracks, 9 spt
    {0xFD, 2, 112, 2,  720},   // 360K DS, 40 tracks, 9 spt
    {0xFE, 1,  64, 1,  320},   // 160K SS, 40 tracks, 8 spt
    {0xFF, 2, 112, 1,  640},   // 320K DS, 40 tracks, 8 spt
};
bool isPow2(int v) { return v > 0 && (v & (v - 1)) == 0; }
} // namespace

bool Fat12::mountFromMediaByte(std::string* err) {
    uint8_t f[512];
    if (!sectorBytes(1, f) || f[1] != 0xFF || f[2] != 0xFF) {
        if (err) *err = "not a FAT12 boot sector";
        return false;
    }
    for (const auto& g : kMediaGeo) {
        if (g.media != f[0]) continue;
        bps_ = 512; spc_ = g.spc; rsvd_ = 1; nfat_ = 2; ndir_ = g.ndir;
        spf_ = g.spf; totalSectors_ = g.total; media_ = g.media;
        return true;
    }
    if (err) *err = "not a FAT12 boot sector (unknown media byte)";
    return false;
}

bool Fat12::mount(DiskImage& disk, std::string* err) {
    disk_ = &disk;
    mounted_ = false;
    uint8_t b[512];
    if (!sectorBytes(0, b)) { if (err) *err = "cannot read boot sector"; return false; }

    bps_  = rd16(b + 0x0B);
    spc_  = b[0x0D];
    rsvd_ = rd16(b + 0x0E);
    nfat_ = b[0x10];
    ndir_ = rd16(b + 0x11);
    totalSectors_ = rd16(b + 0x13);
    media_ = b[0x15];
    spf_  = rd16(b + 0x16);
    oem_  = trim(std::string((const char*)b + 3, 8));

    bool bpbOk = bps_ == 512 && isPow2(spc_) && spc_ <= 64 && nfat_ >= 1 && nfat_ <= 4 &&
                 ndir_ > 0 && ndir_ % 16 == 0 && spf_ > 0 && rsvd_ > 0;
    if (bpbOk && totalSectors_ == 0) totalSectors_ = (int)disk.logicalSectors();
    if (bpbOk) {
        long data = (long)totalSectors_ - rsvd_ - (long)nfat_ * spf_ - (ndir_ * 32 + 511) / 512;
        bpbOk = data >= spc_;
    }
    if (!bpbOk) {
        oem_.clear();
        if (!mountFromMediaByte(err)) return false;
    }

    // Never address sectors the container does not have (a truncated image, or
    // a media-byte guess larger than the dump). DMK images may legitimately
    // end in unreadable holes, so their logical size is not a cap.
    if (disk.format() != DiskFormat::DMK && totalSectors_ > disk.logicalSectors())
        totalSectors_ = (int)disk.logicalSectors();

    fatStartSec_  = rsvd_;
    rootStartSec_ = rsvd_ + (long)nfat_ * spf_;
    rootSectors_  = (ndir_ * 32 + bps_ - 1) / bps_;
    dataStartSec_ = rootStartSec_ + rootSectors_;
    long dataSectors = totalSectors_ - dataStartSec_;
    totalClusters_ = spc_ > 0 ? (int)(dataSectors / spc_) : 0;
    // A FAT12 entry is 1.5 bytes: never address clusters the FAT cannot hold.
    int fatCapacity = (int)((long)spf_ * 512 * 2 / 3) - 2;
    if (totalClusters_ > fatCapacity) totalClusters_ = fatCapacity;
    if (totalClusters_ > 4084) totalClusters_ = 4084;
    if (totalClusters_ < 1) { if (err) *err = "FAT12: no data area"; return false; }

    // Volume label: prefer a root VOL entry, else BPB ext label if present.
    volLabel_.clear();
    for (auto& e : readRootDir(false)) {
        if (e.isVolume()) { volLabel_ = e.name; break; }
    }
    if (volLabel_.empty() && bpbOk && b[0x26] == 0x29) // extended boot sig
        volLabel_ = trim(std::string((const char*)b + 0x2B, 11));

    mounted_ = true;
    return true;
}

int Fat12::fatEntry(int cluster) const {
    long fatByte = fatStartSec_ * 512 + (cluster + cluster / 2);
    long sec = fatByte / 512;
    int off = (int)(fatByte % 512);
    uint8_t s0[512], s1[512];
    if (!sectorBytes(sec, s0)) return 0xFFF;
    uint8_t lo = s0[off];
    uint8_t hi;
    if (off == 511) { if (!sectorBytes(sec + 1, s1)) return 0xFFF; hi = s1[0]; }
    else hi = s0[off + 1];
    uint16_t v = (uint16_t)(lo | (hi << 8));
    return (cluster & 1) ? (v >> 4) & 0xFFF : v & 0xFFF;
}

std::vector<DirEntry> Fat12::parseEntries(const std::vector<uint8_t>& buf,
                                          long baseOffset, bool includeDeleted, bool* ended) {
    std::vector<DirEntry> out;
    if (ended) *ended = false;
    for (size_t i = 0; i + 32 <= buf.size(); i += 32) {
        const uint8_t* e = buf.data() + i;
        if (e[0] == 0x00) { if (ended) *ended = true; break; } // end of directory
        bool del = (e[0] == 0xE5);
        if (del && !includeDeleted) continue;
        uint8_t attr = e[0x0B];
        if (attr == 0x0F) continue;          // LFN (not used by MSX-DOS, skip)
        DirEntry d;
        d.deleted = del;
        d.raw83.assign((const char*)e, 11);
        std::string base = trim(std::string((const char*)e, 8));
        std::string ext  = trim(std::string((const char*)e + 8, 3));
        if (del && !base.empty()) base[0] = '?'; // first char was overwritten
        d.name = ext.empty() ? base : base + "." + ext;
        if ((attr & 0x08) && !(attr & 0x10))     // volume label: 11 chars, no dot
            d.name = trim(std::string((const char*)e, 11));
        d.attr = attr;
        d.time = rd16(e + 0x16);
        d.date = rd16(e + 0x18);
        d.startCluster = rd16(e + 0x1A);
        d.size = rd32(e + 0x1C);
        d.entryOffset = baseOffset >= 0 ? baseOffset + (long)i : -1;
        out.push_back(std::move(d));
    }
    return out;
}

std::vector<DirEntry> Fat12::readRootDir(bool includeDeleted) const {
    std::vector<uint8_t> buf;
    buf.reserve((size_t)rootSectors_ * 512);
    long base = -1;
    for (int s = 0; s < rootSectors_; ++s) {
        uint8_t sec[512];
        if (!sectorBytes(rootStartSec_ + s, sec)) break;
        if (base < 0) base = (rootStartSec_ + s) * 512;
        buf.insert(buf.end(), sec, sec + 512);
    }
    return parseEntries(buf, base, includeDeleted);
}

std::vector<DirEntry> Fat12::readDirAtCluster(int cluster, bool includeDeleted) const {
    // Parse cluster by cluster: a grown directory's clusters are usually not
    // adjacent, so each entry's offset must come from its own cluster.
    std::vector<DirEntry> out;
    for (int c : chainOf(cluster)) {
        std::vector<uint8_t> buf;
        long firstSec = dataStartSec_ + (long)(c - 2) * spc_;
        for (int s = 0; s < spc_; ++s) {
            uint8_t sec[512];
            if (!sectorBytes(firstSec + s, sec)) break;
            buf.insert(buf.end(), sec, sec + 512);
        }
        bool ended = false;
        auto part = parseEntries(buf, firstSec * 512, includeDeleted, &ended);
        out.insert(out.end(), part.begin(), part.end());
        if (ended || buf.size() < (size_t)spc_ * 512) break;
    }
    return out;
}

std::vector<DirEntry> Fat12::readDir(int dirCluster, bool includeDeleted) const {
    return dirCluster == 0 ? readRootDir(includeDeleted) : readDirAtCluster(dirCluster, includeDeleted);
}

bool Fat12::lookup(const std::string& path, DirEntry& out) const {
    if (!mounted_) return false;
    size_t cut = path.find_last_of("/\\");
    std::string dir = (cut == std::string::npos) ? "/" : path.substr(0, cut);
    std::string leaf = (cut == std::string::npos) ? path : path.substr(cut + 1);
    if (leaf.empty() || leaf == "." || leaf == "..") return false;
    int dc = resolveDirCluster(dir);
    if (dc < 0) return false;
    for (char& c : leaf) c = (char)toupper((unsigned char)c);
    for (auto& e : readDir(dc, false)) {
        if (e.isVolume()) continue;
        std::string nm = e.name;
        for (char& c : nm) c = (char)toupper((unsigned char)c);
        if (nm == leaf) { out = e; return true; }
    }
    return false;
}

std::vector<DirEntry> Fat12::list(const std::string& path, bool includeDeleted) const {
    if (!mounted_) return {};
    // Normalize path into components.
    std::vector<std::string> parts;
    std::string cur;
    for (char c : path) {
        if (c == '/' || c == '\\') { if (!cur.empty()) { parts.push_back(cur); cur.clear(); } }
        else cur.push_back(c);
    }
    if (!cur.empty()) parts.push_back(cur);

    if (parts.empty()) return readRootDir(includeDeleted);

    // Descend from root, matching each component (case-insensitive) to a DIR.
    std::vector<DirEntry> dir = readRootDir(false);
    int targetCluster = -1;
    for (size_t p = 0; p < parts.size(); ++p) {
        std::string want = parts[p];
        for (auto& c : want) c = (char)toupper((unsigned char)c);
        const DirEntry* found = nullptr;
        for (auto& e : dir) {
            if (!e.isDir()) continue;
            std::string nm = e.name;
            for (auto& c : nm) c = (char)toupper((unsigned char)c);
            if (nm == want) { found = &e; break; }
        }
        if (!found) return {};
        targetCluster = found->startCluster;
        if (p + 1 < parts.size())
            dir = readDirAtCluster(targetCluster, false);
    }
    return readDirAtCluster(targetCluster, includeDeleted);
}

std::vector<uint8_t> Fat12::readFile(const DirEntry& e) const {
    std::vector<uint8_t> out;
    if (!mounted_ || e.isDir()) return out;
    out.reserve(e.size);
    int cluster = e.startCluster;
    long remaining = e.size;
    long clusterBytes = (long)spc_ * 512;
    int guard = 0;
    while (remaining > 0 && cluster >= 2 && cluster < 0xFF8 &&
           guard++ < totalClusters_ + 8) {
        long firstSec = dataStartSec_ + (long)(cluster - 2) * spc_;
        for (int s = 0; s < spc_ && remaining > 0; ++s) {
            uint8_t sec[512];
            if (!sectorBytes(firstSec + s, sec)) return out;
            long chunk = remaining < 512 ? remaining : 512;
            out.insert(out.end(), sec, sec + chunk);
            remaining -= chunk;
        }
        cluster = fatEntry(cluster);
        (void)clusterBytes;
    }
    return out;
}

long Fat12::freeBytes() const {
    if (!mounted_) return 0;
    long freeCl = 0;
    for (int c = 2; c < totalClusters_ + 2; ++c)
        if (fatEntry(c) == 0) freeCl++;
    return freeCl * spc_ * 512;
}

// ---- Write support --------------------------------------------------------

bool Fat12::readByteAt(long off, uint8_t& out) const {
    long lba = off / 512; int within = (int)(off % 512);
    uint8_t sec[512];
    if (!disk_ || !disk_->readSector(lba, sec)) return false;
    out = sec[within];
    return true;
}

bool Fat12::writeByteAt(long off, uint8_t value) {
    long lba = off / 512; int within = (int)(off % 512);
    uint8_t sec[512];
    if (!disk_ || !disk_->readSector(lba, sec)) return false;
    sec[within] = value;
    return disk_->writeLogicalSector(lba, sec);
}

void Fat12::setFatEntry(int cluster, int value) {
    value &= 0xFFF;
    for (int f = 0; f < nfat_; ++f) {
        long base = (fatStartSec_ + (long)f * spf_) * 512;
        long o = base + cluster + cluster / 2;
        uint8_t lo, hi;
        if (!readByteAt(o, lo)) continue;
        if (!readByteAt(o + 1, hi)) continue;
        uint16_t v = (uint16_t)(lo | (hi << 8));
        if (cluster & 1) v = (uint16_t)((v & 0x000F) | (value << 4));
        else             v = (uint16_t)((v & 0xF000) | value);
        writeByteAt(o, (uint8_t)(v & 0xFF));
        writeByteAt(o + 1, (uint8_t)(v >> 8));
    }
}

int Fat12::allocCluster() {
    for (int c = 2; c < totalClusters_ + 2; ++c)
        if (fatEntry(c) == 0) return c;
    return -1;
}

std::vector<int> Fat12::chainOf(int startCluster) const {
    // Stops at the end marker, at an out-of-range value, or when a corrupt FAT
    // loops back to a cluster already in the chain.
    std::vector<int> chain;
    std::vector<bool> seen((size_t)totalClusters_ + 2, false);
    int c = startCluster;
    while (c >= 2 && c < totalClusters_ + 2 && !seen[(size_t)c]) {
        seen[(size_t)c] = true;
        chain.push_back(c);
        c = fatEntry(c);
    }
    return chain;
}

bool Fat12::writeClusterData(int cluster, const uint8_t* src, long len) {
    long firstSec = dataStartSec_ + (long)(cluster - 2) * spc_;
    for (int s = 0; s < spc_; ++s) {
        uint8_t sec[512];
        long chunk = len - (long)s * 512;
        if (chunk >= 512) {
            memcpy(sec, src + (long)s * 512, 512);
        } else if (chunk > 0) {
            memcpy(sec, src + (long)s * 512, chunk);
            memset(sec + chunk, 0, 512 - chunk);
        } else {
            memset(sec, 0, 512);
        }
        if (!disk_->writeLogicalSector(firstSec + s, sec)) return false;
    }
    return true;
}

int Fat12::countFreeClusters() const {
    int n = 0;
    for (int c = 2; c < totalClusters_ + 2; ++c)
        if (fatEntry(c) == 0) n++;
    return n;
}

void Fat12::stamp(uint16_t& date, uint16_t& time) const {
    if (fixedStamp_) { date = fixedDate_; time = fixedTime_; return; }
    std::time_t now = std::time(nullptr);
    std::tm tmv{};
#if defined(_WIN32)
    localtime_s(&tmv, &now);
#else
    localtime_r(&now, &tmv);
#endif
    int y = tmv.tm_year + 1900;
    if (y < 1980) y = 1980;
    if (y > 2107) y = 2107;
    date = (uint16_t)(((y - 1980) << 9) | ((tmv.tm_mon + 1) << 5) | tmv.tm_mday);
    time = (uint16_t)((tmv.tm_hour << 11) | (tmv.tm_min << 5) | (tmv.tm_sec / 2));
}

bool Fat12::writeFile(DirEntry& e, const std::vector<uint8_t>& data, std::string* err) {
    if (!mounted_) { if (err) *err = "not mounted"; return false; }
    if (e.isDir()) { if (err) *err = "cannot overwrite a directory"; return false; }

    long clusterBytes = (long)spc_ * 512;
    long needL = data.empty() ? 0 : (long)((data.size() + clusterBytes - 1) / clusterBytes);
    std::vector<int> chain = (e.startCluster >= 2) ? chainOf(e.startCluster) : std::vector<int>{};

    // Check the space up front so a failed write leaves the disk untouched
    // (no half-allocated clusters, no truncated file).
    if (needL > (long)chain.size() &&
        needL - (long)chain.size() > (long)countFreeClusters()) {
        if (err) *err = "disk full (no free clusters)";
        return false;
    }
    int needClusters = (int)needL;
    const size_t oldLen = chain.size();
    auto releaseNew = [&]() {                  // undo this call's allocations
        for (size_t i = oldLen; i < chain.size(); ++i) setFatEntry(chain[i], 0);
    };
    while ((int)chain.size() < needClusters) {
        int nc = allocCluster();
        if (nc < 0) { releaseNew(); if (err) *err = "disk full (no free clusters)"; return false; }
        setFatEntry(nc, 0xFFF); // reserve immediately so the next alloc skips it
        chain.push_back(nc);
    }

    // Write the payload across the (first needClusters) clusters.
    for (int i = 0; i < needClusters; ++i) {
        long off = (long)i * clusterBytes;
        long len = (long)data.size() - off; if (len > clusterBytes) len = clusterBytes;
        if (!writeClusterData(chain[i], data.data() + off, len)) {
            releaseNew();
            if (err) *err = "write error (sector not writable)";
            return false;
        }
    }

    // Relink the FAT: chain[0..need-1] linked, last -> EOF; free the rest.
    for (int i = 0; i < needClusters; ++i) {
        int next = (i + 1 < needClusters) ? chain[i + 1] : 0xFFF;
        setFatEntry(chain[i], next);
    }
    for (size_t i = needClusters; i < chain.size(); ++i)
        setFatEntry(chain[i], 0); // free the surplus tail of the old chain

    // Update the directory entry (size, start cluster, modification time).
    uint16_t newStart = (needClusters > 0) ? (uint16_t)chain[0] : 0;
    uint32_t newSize = (uint32_t)data.size();
    uint16_t date, time; stamp(date, time);
    if (e.entryOffset >= 0) {
        writeByteAt(e.entryOffset + 0x16, (uint8_t)(time & 0xFF));
        writeByteAt(e.entryOffset + 0x17, (uint8_t)(time >> 8));
        writeByteAt(e.entryOffset + 0x18, (uint8_t)(date & 0xFF));
        writeByteAt(e.entryOffset + 0x19, (uint8_t)(date >> 8));
        writeByteAt(e.entryOffset + 0x1A, (uint8_t)(newStart & 0xFF));
        writeByteAt(e.entryOffset + 0x1B, (uint8_t)(newStart >> 8));
        writeByteAt(e.entryOffset + 0x1C, (uint8_t)(newSize & 0xFF));
        writeByteAt(e.entryOffset + 0x1D, (uint8_t)((newSize >> 8) & 0xFF));
        writeByteAt(e.entryOffset + 0x1E, (uint8_t)((newSize >> 16) & 0xFF));
        writeByteAt(e.entryOffset + 0x1F, (uint8_t)((newSize >> 24) & 0xFF));
    }
    e.startCluster = newStart;
    e.size = newSize;
    e.date = date; e.time = time;
    return true;
}

bool Fat12::deleteFile(const DirEntry& e, std::string* err) {
    if (!mounted_) { if (err) *err = "not mounted"; return false; }
    if (e.isDir()) { if (err) *err = "refusing to delete a directory (use removeDir)"; return false; }
    if (e.deleted || e.entryOffset < 0 || (!e.raw83.empty() && e.raw83[0] == '.')) {
        if (err) *err = "entry is not a live file";
        return false;
    }
    for (int c : chainOf(e.startCluster)) setFatEntry(c, 0);
    writeByteAt(e.entryOffset, 0xE5); // mark deleted
    return true;
}

bool Fat12::removeDir(const DirEntry& e, std::string* err) {
    if (!mounted_) { if (err) *err = "not mounted"; return false; }
    if (!e.isDir() || e.deleted || e.entryOffset < 0 || e.startCluster < 2 ||
        (!e.raw83.empty() && e.raw83[0] == '.')) {
        if (err) *err = "not a removable subdirectory";
        return false;
    }
    for (auto& c : readDirAtCluster(e.startCluster, false)) {
        if (c.name == "." || c.name == "..") continue;
        if (err) *err = "directory is not empty";
        return false;
    }
    for (int c : chainOf(e.startCluster)) setFatEntry(c, 0);
    writeByteAt(e.entryOffset, 0xE5);
    return true;
}

bool Fat12::undelete(DirEntry& e, char firstChar, std::string* err) {
    if (!mounted_) { if (err) *err = "not mounted"; return false; }
    if (!e.deleted || e.entryOffset < 0 || e.raw83.size() != 11) {
        if (err) *err = "entry is not a deleted file";
        return false;
    }
    if (e.isDir() || e.isVolume()) {
        if (err) *err = "only files can be recovered (directory chains are not recorded)";
        return false;
    }
    std::string candidate = e.name;
    if (candidate.empty()) { if (err) *err = "entry has no name"; return false; }
    candidate[0] = (char)toupper((unsigned char)firstChar);
    if (!validName83(candidate, err)) return false;
    uint8_t nm[11];
    memcpy(nm, e.raw83.data(), 11);
    nm[0] = (uint8_t)toupper((unsigned char)firstChar);

    // Which directory holds the entry? (Needed for the name collision check.)
    int dirOf = -1;
    std::set<int> seen;
    std::vector<int> todo{0};
    while (!todo.empty() && dirOf < 0) {
        int dc = todo.back(); todo.pop_back();
        if (!seen.insert(dc).second) continue;
        for (auto& x : readDir(dc, true)) {
            if (x.entryOffset == e.entryOffset) { dirOf = dc; break; }
            if (!x.deleted && x.isDir() && x.startCluster >= 2 && x.name != "." && x.name != "..")
                todo.push_back(x.startCluster);
        }
    }
    if (dirOf < 0) { if (err) *err = "entry not found in the directory tree"; return false; }
    if (nameTaken(dirOf, nm)) { if (err) *err = "a live file already has that name"; return false; }

    long clusterBytes = (long)spc_ * 512;
    long need = ((long)e.size + clusterBytes - 1) / clusterBytes;
    if (need > 0) {
        if (e.startCluster < 2 || e.startCluster - 2 + need > totalClusters_) {
            if (err) *err = "start cluster / size out of range";
            return false;
        }
        for (long i = 0; i < need; ++i) {
            if (fatEntry(e.startCluster + (int)i) != 0) {
                if (err) *err = "cluster " + std::to_string(e.startCluster + i) +
                                " has been reused; the data cannot be recovered intact";
                return false;
            }
        }
        for (long i = 0; i < need; ++i) {
            int c = e.startCluster + (int)i;
            setFatEntry(c, i + 1 < need ? c + 1 : 0xFFF);
        }
    }
    writeByteAt(e.entryOffset, nm[0]);
    e.raw83.assign((const char*)nm, 11);
    e.name = candidate;
    e.deleted = false;
    return true;
}

bool Fat12::setAttributes(DirEntry& e, uint8_t attr, std::string* err) {
    if (!mounted_ || e.entryOffset < 0 || e.deleted) { if (err) *err = "not a live entry"; return false; }
    if (!e.raw83.empty() && e.raw83[0] == '.') { if (err) *err = "cannot change . or .."; return false; }
    uint8_t v = (uint8_t)((e.attr & 0x18) | (attr & 0x27));
    if (!writeByteAt(e.entryOffset + 0x0B, v)) { if (err) *err = "write error"; return false; }
    e.attr = v;
    return true;
}

bool Fat12::setVolumeLabel(const std::string& label, std::string* err) {
    if (!mounted_) { if (err) *err = "not mounted"; return false; }
    if (label.size() > 11) { if (err) *err = "a volume label has at most 11 characters"; return false; }
    for (char ch : label) {
        unsigned char c = (unsigned char)ch;
        if (c < 0x20 || c == 0x7F || strchr("\"*+,./:;<=>?[\\]|", c)) {
            if (err) *err = "reserved character in the label";
            return false;
        }
    }
    long existing = -1;
    for (auto& e : readRootDir(false)) if (e.isVolume()) { existing = e.entryOffset; break; }
    if (label.empty()) {
        if (existing >= 0) writeByteAt(existing, 0xE5);
        volLabel_.clear();
        return true;
    }
    uint8_t nm[11]; memset(nm, ' ', 11);
    for (size_t i = 0; i < label.size(); ++i) nm[i] = (uint8_t)toupper((unsigned char)label[i]);
    long slot = existing >= 0 ? existing : freeDirSlot(0, false);
    if (slot < 0) { if (err) *err = "root directory is full"; return false; }
    writeDirEntry(slot, nm, 0x08, 0, 0);
    volLabel_ = trim(std::string((const char*)nm, 11));
    return true;
}

// ---- RAW / uncatalogued data ----------------------------------------------

long Fat12::clusterByteOffset(int cluster) const {
    return (dataStartSec_ + (long)(cluster - 2) * spc_) * 512;
}

void Fat12::collectNamedClusters(int cluster, std::vector<uint8_t>& status, bool isRoot) const {
    // Walk the tree breadth-first; `seen` stops corrupt directories that link
    // back to an ancestor (or themselves) from looping forever.
    std::set<int> seen;
    std::vector<int> todo{isRoot ? 0 : cluster};
    while (!todo.empty()) {
        int dc = todo.back(); todo.pop_back();
        if (!seen.insert(dc).second) continue;
        for (auto& e : readDir(dc, false)) {
            if (e.name == "." || e.name == ".." || e.deleted) continue; // deleted stay "raw"
            if (e.isVolume()) continue;
            for (int c : chainOf(e.startCluster)) {
                int idx = c - 2;
                if (idx >= 0 && idx < (int)status.size()) status[idx] = CL_NAMED;
            }
            if (e.isDir() && e.startCluster >= 2) todo.push_back(e.startCluster);
        }
    }
}

std::vector<uint8_t> Fat12::clusterMap() const {
    std::vector<uint8_t> st(totalClusters_ > 0 ? totalClusters_ : 0, CL_FREE);
    if (!mounted_) return st;
    for (int c = 2; c < totalClusters_ + 2; ++c) {
        int idx = c - 2;
        if (idx < 0 || idx >= (int)st.size()) continue;
        if (fatEntry(c) != 0) st[idx] = CL_RAW; // allocated; refined below
    }
    collectNamedClusters(0, st, true);
    return st;
}

std::vector<uint8_t> Fat12::extractRawUnnamed() const {
    std::vector<uint8_t> out;
    if (!mounted_) return out;
    auto st = clusterMap();
    for (int i = 0; i < (int)st.size(); ++i) {
        if (st[i] != CL_RAW) continue;
        int cluster = i + 2;
        long firstSec = dataStartSec_ + (long)(cluster - 2) * spc_;
        for (int s = 0; s < spc_; ++s) {
            uint8_t sec[512];
            if (disk_->readSector(firstSec + s, sec)) out.insert(out.end(), sec, sec + 512);
        }
    }
    return out;
}

long Fat12::rawUnnamedCapacity() const {
    if (!mounted_) return 0;
    auto st = clusterMap();
    long raw = 0;
    for (auto s : st) if (s == CL_RAW) raw++;
    return raw * (long)spc_ * 512;
}

bool Fat12::writeRawUnnamed(const std::vector<uint8_t>& data, long* written, std::string* err) {
    if (written) *written = 0;
    if (!mounted_) { if (err) *err = "not mounted"; return false; }
    auto st = clusterMap();
    long pos = 0, total = (long)data.size();
    for (int i = 0; i < (int)st.size() && pos < total; ++i) {
        if (st[i] != CL_RAW) continue;
        int cluster = i + 2;
        long firstSec = dataStartSec_ + (long)(cluster - 2) * spc_;
        for (int s = 0; s < spc_ && pos < total; ++s) {
            uint8_t sec[512];
            if (!disk_->readSector(firstSec + s, sec)) { if (err) *err = "read error"; return false; }
            long chunk = total - pos; if (chunk > 512) chunk = 512;
            memcpy(sec, data.data() + pos, (size_t)chunk); // partial tail keeps old bytes
            if (!disk_->writeLogicalSector(firstSec + s, sec)) { if (err) *err = "write error"; return false; }
            pos += chunk;
        }
    }
    if (written) *written = pos;
    if (pos < total) { if (err) *err = "data larger than RAW capacity; truncated"; return false; }
    return true;
}

// ---- Directory writes: create / rename / mkdir ----------------------------

bool Fat12::validName83(const std::string& name, std::string* err) {
    auto bad = [&](const char* why) { if (err) *err = std::string("invalid 8.3 name '") + name + "': " + why; return false; };
    if (name.empty()) return bad("empty");
    size_t dot = name.find('.');
    if (dot != std::string::npos && name.find('.', dot + 1) != std::string::npos) return bad("more than one dot");
    std::string base = (dot == std::string::npos) ? name : name.substr(0, dot);
    std::string ext  = (dot == std::string::npos) ? ""   : name.substr(dot + 1);
    if (base.empty()) return bad("empty base name");
    if (base.size() > 8) return bad("base name longer than 8 characters");
    if (ext.size() > 3) return bad("extension longer than 3 characters");
    if (dot != std::string::npos && ext.empty()) return bad("empty extension after the dot");
    static const char* kReserved = "\"*+,./:;<=>?[\\]| ";
    for (char ch : base + ext) {
        unsigned char c = (unsigned char)ch;
        if (c < 0x20 || c == 0x7F || strchr(kReserved, c)) return bad("reserved character");
    }
    if ((unsigned char)base[0] == 0xE5) return bad("first character 0xE5 marks deleted entries");
    return true;
}

std::string Fat12::suggestName83(const std::string& hostName,
                                 const std::vector<std::string>& taken) {
    auto clean = [](const std::string& in, size_t maxLen) {
        std::string out;
        for (char ch : in) {
            unsigned char c = (unsigned char)ch;
            if (c == ' ' || c == '.') continue;
            if (c < 0x20 || c == 0x7F || strchr("\"*+,/:;<=>?[\\]|", c)) c = '_';
            out += (char)toupper(c);
        }
        if (!out.empty() && (unsigned char)out[0] == 0xE5) out[0] = '_';
        if (out.size() > maxLen) out.resize(maxLen);
        return out;
    };
    size_t dot = hostName.find_last_of('.');
    bool hasExt = dot != std::string::npos && dot > 0 && dot + 1 < hostName.size();
    std::string base = clean(hasExt ? hostName.substr(0, dot) : hostName, 64);
    std::string ext  = clean(hasExt ? hostName.substr(dot + 1) : "", 3);
    if (base.empty()) base = "FILE";
    auto isTaken = [&](const std::string& n) {
        for (auto t : taken) {
            for (auto& c : t) c = (char)toupper((unsigned char)c);
            if (t == n) return true;
        }
        return false;
    };
    auto join = [&](const std::string& b) { return ext.empty() ? b : b + "." + ext; };
    std::string first = join(base.substr(0, 8));
    if (base.size() <= 8 && !isTaken(first)) return first;
    for (int n = 1; n < 100000; ++n) {
        std::string tail = "~" + std::to_string(n);
        std::string cand = join(base.substr(0, 8 - tail.size()) + tail);
        if (!isTaken(cand)) return cand;
    }
    return first;
}

void Fat12::pack83(const std::string& name, uint8_t out[11]) {
    memset(out, ' ', 11);
    if (name == ".") { out[0] = '.'; return; }
    if (name == "..") { out[0] = '.'; out[1] = '.'; return; }
    size_t dot = name.find_last_of('.');
    std::string base = (dot == std::string::npos) ? name : name.substr(0, dot);
    std::string ext  = (dot == std::string::npos) ? ""   : name.substr(dot + 1);
    for (size_t i = 0; i < base.size() && i < 8; ++i) out[i] = (uint8_t)toupper((unsigned char)base[i]);
    for (size_t i = 0; i < ext.size()  && i < 3; ++i) out[8 + i] = (uint8_t)toupper((unsigned char)ext[i]);
}

bool Fat12::nameTaken(int dirCluster, const uint8_t name11[11], long ignoreOffset) const {
    for (auto& e : readDir(dirCluster, false)) {
        if (e.entryOffset == ignoreOffset || e.raw83.size() != 11) continue;
        if (memcmp(e.raw83.data(), name11, 11) == 0) return true;
    }
    return false;
}

int Fat12::resolveDirCluster(const std::string& path) const {
    std::vector<std::string> parts; std::string cur;
    for (char c : path) { if (c == '/' || c == '\\') { if (!cur.empty()) { parts.push_back(cur); cur.clear(); } } else cur.push_back(c); }
    if (!cur.empty()) parts.push_back(cur);
    if (parts.empty()) return 0; // root
    std::vector<DirEntry> dir = readRootDir(false);
    int cluster = 0;
    for (size_t p = 0; p < parts.size(); ++p) {
        std::string want = parts[p]; for (auto& c : want) c = (char)toupper((unsigned char)c);
        const DirEntry* found = nullptr;
        for (auto& e : dir) {
            if (!e.isDir()) continue;
            std::string nm = e.name; for (auto& c : nm) c = (char)toupper((unsigned char)c);
            if (nm == want) { found = &e; break; }
        }
        if (!found) return -1;
        cluster = found->startCluster;
        if (p + 1 < parts.size()) dir = readDirAtCluster(cluster, false);
    }
    return cluster;
}

long Fat12::freeDirSlot(int dirCluster, bool allowGrow) {
    if (dirCluster == 0) {
        for (int i = 0; i < ndir_; ++i) {
            long off = rootStartSec_ * 512 + (long)i * 32;
            uint8_t b; if (!readByteAt(off, b)) return -1;
            if (b == 0x00 || b == 0xE5) return off;
        }
        return -1; // the root directory has a fixed size
    }
    int slotsPerCluster = spc_ * 512 / 32;
    std::vector<int> chain = chainOf(dirCluster);
    for (int c : chain) {
        long firstSec = dataStartSec_ + (long)(c - 2) * spc_;
        for (int s = 0; s < slotsPerCluster; ++s) {
            long off = firstSec * 512 + (long)s * 32;
            uint8_t b; if (!readByteAt(off, b)) return -1;
            if (b == 0x00 || b == 0xE5) return off;
        }
    }
    // Subdirectories grow: append a zeroed cluster to the directory's chain.
    if (chain.empty() || !allowGrow) return -1;
    int nc = allocCluster();
    if (nc < 0) return -1;
    std::vector<uint8_t> zero((size_t)spc_ * 512, 0);
    if (!writeClusterData(nc, zero.data(), (long)zero.size())) return -1;
    setFatEntry(nc, 0xFFF);
    setFatEntry(chain.back(), nc);
    return clusterByteOffset(nc);
}

void Fat12::writeDirEntry(long off, const uint8_t name11[11], uint8_t attr,
                          uint16_t cluster, uint32_t size) {
    uint8_t e[32]; memset(e, 0, sizeof e);
    memcpy(e, name11, 11);
    e[0x0B] = attr;
    uint16_t date, time; stamp(date, time);
    e[0x16] = time & 0xFF; e[0x17] = (time >> 8) & 0xFF;
    e[0x18] = date & 0xFF; e[0x19] = (date >> 8) & 0xFF;
    e[0x1A] = cluster & 0xFF; e[0x1B] = (cluster >> 8) & 0xFF;
    e[0x1C] = size & 0xFF; e[0x1D] = (size >> 8) & 0xFF;
    e[0x1E] = (size >> 16) & 0xFF; e[0x1F] = (size >> 24) & 0xFF;
    for (int i = 0; i < 32; ++i) writeByteAt(off + i, e[i]);
}

bool Fat12::createFile(const std::string& dirPath, const std::string& name,
                       const std::vector<uint8_t>& data, std::string* err) {
    if (!mounted_) { if (err) *err = "not mounted"; return false; }
    if (!validName83(name, err)) return false;
    int dc = resolveDirCluster(dirPath);
    if (dc < 0) { if (err) *err = "directory not found"; return false; }
    uint8_t nm[11]; pack83(name, nm);
    if (nameTaken(dc, nm)) { if (err) *err = "a file with that name already exists"; return false; }

    long clusterBytes = (long)spc_ * 512;
    long need = (long)((data.size() + clusterBytes - 1) / clusterBytes);
    // Growing a full subdirectory costs one more cluster: count it up front so
    // a failed create leaves the directory as it was.
    bool grow = dc != 0 && freeDirSlot(dc, false) < 0;
    if (need + (grow ? 1 : 0) > countFreeClusters()) { if (err) *err = "disk full (no free clusters)"; return false; }
    long slot = freeDirSlot(dc);
    if (slot < 0) { if (err) *err = "directory is full"; return false; }
    uint8_t prevFirst = 0; readByteAt(slot, prevFirst);
    writeDirEntry(slot, nm, 0x20 /*archive*/, 0, 0);
    DirEntry tmp; tmp.entryOffset = slot; tmp.startCluster = 0; tmp.size = 0; tmp.attr = 0x20;
    if (!writeFile(tmp, data, err)) {
        // Roll back the directory entry so no phantom 0-byte file is left.
        uint8_t blank[32]; memset(blank, 0, sizeof blank);
        blank[0] = prevFirst == 0xE5 ? 0xE5 : 0x00;
        for (int i = 0; i < 32; ++i) writeByteAt(slot + i, blank[i]);
        return false;
    }
    return true;
}

bool Fat12::renameEntry(DirEntry& e, const std::string& newName, std::string* err) {
    if (!mounted_ || e.entryOffset < 0 || e.deleted || (!e.raw83.empty() && e.raw83[0] == '.')) {
        if (err) *err = "cannot rename";
        return false;
    }
    if (!validName83(newName, err)) return false;
    uint8_t nm[11]; pack83(newName, nm);
    // The entry's own directory: find it by scanning for the record offset.
    long dirOf = -1;
    std::set<int> seen;
    std::vector<int> todo{0};
    while (!todo.empty() && dirOf < 0) {
        int dc = todo.back(); todo.pop_back();
        if (!seen.insert(dc).second) continue;
        for (auto& x : readDir(dc, false)) {
            if (x.entryOffset == e.entryOffset) { dirOf = dc; break; }
            if (x.isDir() && x.startCluster >= 2 && x.name != "." && x.name != "..")
                todo.push_back(x.startCluster);
        }
    }
    if (dirOf >= 0 && nameTaken((int)dirOf, nm, e.entryOffset)) {
        if (err) *err = "a file with that name already exists";
        return false;
    }
    for (int i = 0; i < 11; ++i) writeByteAt(e.entryOffset + i, nm[i]);
    e.raw83.assign((const char*)nm, 11);
    std::string base((const char*)nm, 8), ext((const char*)nm + 8, 3);
    base = trim(base); ext = trim(ext);
    e.name = ext.empty() ? base : base + "." + ext;
    return true;
}

bool Fat12::makeDir(const std::string& parentPath, const std::string& name, std::string* err) {
    if (!mounted_) { if (err) *err = "not mounted"; return false; }
    if (!validName83(name, err)) return false;
    int parent = resolveDirCluster(parentPath);
    if (parent < 0) { if (err) *err = "parent directory not found"; return false; }
    uint8_t nm[11]; pack83(name, nm);
    if (nameTaken(parent, nm)) { if (err) *err = "name already exists"; return false; }
    bool grow = parent != 0 && freeDirSlot(parent, false) < 0;
    if (countFreeClusters() < (grow ? 2 : 1)) { if (err) *err = "disk full"; return false; }
    long slot = freeDirSlot(parent);
    if (slot < 0) { if (err) *err = "parent directory is full"; return false; }
    int nc = allocCluster();
    if (nc < 0) { if (err) *err = "disk full"; return false; }
    setFatEntry(nc, 0xFFF);
    // Zero the new directory cluster, then write . and .. entries.
    long firstSec = dataStartSec_ + (long)(nc - 2) * spc_;
    uint8_t zero[512]; memset(zero, 0, sizeof zero);
    for (int s = 0; s < spc_; ++s) disk_->writeLogicalSector(firstSec + s, zero);
    uint8_t dot[11], dotdot[11]; pack83(".", dot); pack83("..", dotdot);
    writeDirEntry(firstSec * 512 + 0,  dot,    0x10, (uint16_t)nc, 0);
    writeDirEntry(firstSec * 512 + 32, dotdot, 0x10, (uint16_t)parent, 0);
    // Add the entry to the parent.
    writeDirEntry(slot, nm, 0x10, (uint16_t)nc, 0);
    return true;
}

// ---- Integrity check ------------------------------------------------------

Fat12::FsCheck Fat12::verify() const {
    FsCheck r;
    if (!mounted_) { r.issues.push_back("filesystem not mounted"); return r; }
    std::vector<int> owner(totalClusters_ > 0 ? totalClusters_ : 0, -1);
    long clusterBytes = (long)spc_ * 512;
    std::set<int> seenDirs;
    // Walk every named file/dir chain, flagging bad pointers, cross-links and
    // sizes that do not match the chain length.
    std::function<void(int, const std::string&)> walk = [&](int dirCluster, const std::string& base) {
        if (!seenDirs.insert(dirCluster).second) {
            r.issues.push_back((base.empty() ? "/" : base) + ": directory loop");
            return;
        }
        for (auto& e : readDir(dirCluster, false)) {
            if (e.name == "." || e.name == ".." || e.deleted || e.isVolume()) continue;
            std::string path = base + "/" + e.name;
            int c = e.startCluster, guard = 0;
            long chainLen = 0;
            bool broken = false;
            if (c == 1 || (c >= 2 && c - 2 >= totalClusters_ && c < 0xFF8)) {
                r.issues.push_back(path + ": start cluster out of range"); broken = true;
            }
            while (!broken && c >= 2 && c < 0xFF8 && guard++ < totalClusters_ + 4) {
                int idx = c - 2;
                if (idx >= (int)owner.size()) { r.issues.push_back(path + ": cluster out of range"); broken = true; break; }
                if (owner[idx] >= 0) { r.issues.push_back(path + ": cross-linked at cluster " + std::to_string(c)); broken = true; break; }
                owner[idx] = 1; r.usedByFiles++; chainLen++;
                int nxt = fatEntry(c);
                if (nxt == 0) { r.issues.push_back(path + ": chain hits a free cluster"); broken = true; break; }
                if (nxt == 1 || (nxt >= 0xFF0 && nxt <= 0xFF7)) {
                    r.issues.push_back(path + ": chain hits a reserved/bad cluster marker"); broken = true; break;
                }
                c = nxt;
            }
            if (!broken && !e.isDir()) {
                long want = ((long)e.size + clusterBytes - 1) / clusterBytes;
                if (want != chainLen)
                    r.issues.push_back(path + ": size " + std::to_string(e.size) + " needs " +
                                       std::to_string(want) + " cluster(s) but the chain has " +
                                       std::to_string(chainLen));
            }
            if (e.isDir() && e.startCluster >= 2) walk(e.startCluster, path);
        }
    };
    walk(0, "");
    for (int c = 2; c < totalClusters_ + 2; ++c) {
        int idx = c - 2;
        int v = fatEntry(c);
        if (v == 0) r.freeClusters++;
        else if (v == 0xFF7) continue; // marked bad: neither free nor lost
        else if (idx >= 0 && idx < (int)owner.size() && owner[idx] < 0) r.unreferenced++;
    }
    return r;
}

} // namespace msx
