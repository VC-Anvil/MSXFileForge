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

    if (bps_ != 512 || spc_ == 0 || nfat_ == 0 || ndir_ == 0 || spf_ == 0 || rsvd_ == 0) {
        if (err) *err = "not a FAT12 boot sector";
        return false;
    }
    if (totalSectors_ == 0) totalSectors_ = (int)disk.logicalSectors();

    fatStartSec_  = rsvd_;
    rootStartSec_ = rsvd_ + (long)nfat_ * spf_;
    rootSectors_  = (ndir_ * 32 + bps_ - 1) / bps_;
    dataStartSec_ = rootStartSec_ + rootSectors_;
    long dataSectors = totalSectors_ - dataStartSec_;
    totalClusters_ = spc_ > 0 ? (int)(dataSectors / spc_) : 0;

    // Volume label: prefer a root VOL entry, else BPB ext label if present.
    volLabel_.clear();
    for (auto& e : readRootDir(false)) {
        if (e.isVolume()) { volLabel_ = e.name; break; }
    }
    if (volLabel_.empty() && b[0x26] == 0x29) // extended boot sig
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
                                          long baseOffset, bool includeDeleted) {
    std::vector<DirEntry> out;
    for (size_t i = 0; i + 32 <= buf.size(); i += 32) {
        const uint8_t* e = buf.data() + i;
        if (e[0] == 0x00) break;             // end of directory
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
    std::vector<uint8_t> buf;
    long firstBase = -1;
    int guard = 0;
    while (cluster >= 2 && cluster < 0xFF8 && guard++ < totalClusters_ + 4) {
        long firstSec = dataStartSec_ + (long)(cluster - 2) * spc_;
        for (int s = 0; s < spc_; ++s) {
            uint8_t sec[512];
            if (!sectorBytes(firstSec + s, sec)) break;
            if (firstBase < 0) firstBase = (firstSec + s) * 512;
            buf.insert(buf.end(), sec, sec + 512);
        }
        cluster = fatEntry(cluster);
    }
    return parseEntries(buf, firstBase, includeDeleted);
}

const DirEntry* Fat12::resolve(const std::string& path) const {
    // Walk the path components; returns a heap-lived copy is awkward, so we keep
    // a static thread-local last-resolved entry. Simpler: return by storing.
    // Here we resolve into a cached member via a small trick: not needed because
    // list() re-resolves. This helper is unused for root.
    (void)path;
    return nullptr;
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
    std::vector<int> chain;
    int c = startCluster, guard = 0;
    while (c >= 2 && c < 0xFF8 && guard++ < totalClusters_ + 8) {
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

bool Fat12::writeFile(DirEntry& e, const std::vector<uint8_t>& data, std::string* err) {
    if (!mounted_) { if (err) *err = "not mounted"; return false; }
    if (e.isDir()) { if (err) *err = "cannot overwrite a directory"; return false; }

    long clusterBytes = (long)spc_ * 512;
    int needClusters = data.empty() ? 0 : (int)((data.size() + clusterBytes - 1) / clusterBytes);

    std::vector<int> have = (e.startCluster >= 2) ? chainOf(e.startCluster) : std::vector<int>{};

    // Grow the chain if needed by allocating free clusters.
    std::vector<int> chain = have;
    if ((int)chain.size() < needClusters) {
        // Temporarily reserve as we go so allocCluster doesn't return dupes.
        int extra = needClusters - (int)chain.size();
        for (int i = 0; i < extra; ++i) {
            int nc = allocCluster();
            if (nc < 0) { if (err) *err = "disk full (no free clusters)"; return false; }
            setFatEntry(nc, 0xFFF); // reserve immediately
            chain.push_back(nc);
        }
    }

    // Write the payload across the (first needClusters) clusters.
    for (int i = 0; i < needClusters; ++i) {
        long off = (long)i * clusterBytes;
        long len = (long)data.size() - off; if (len > clusterBytes) len = clusterBytes;
        if (!writeClusterData(chain[i], data.data() + off, len)) {
            if (err) *err = "write error"; return false;
        }
    }

    // Relink the FAT: chain[0..need-1] linked, last -> EOF; free the rest.
    for (int i = 0; i < needClusters; ++i) {
        int next = (i + 1 < needClusters) ? chain[i + 1] : 0xFFF;
        setFatEntry(chain[i], next);
    }
    for (size_t i = needClusters; i < chain.size(); ++i)
        setFatEntry(chain[i], 0); // free surplus (both old chain tail + over-alloc)

    // Update the directory entry (size + start cluster).
    uint16_t newStart = (needClusters > 0) ? (uint16_t)chain[0] : 0;
    uint32_t newSize = (uint32_t)data.size();
    if (e.entryOffset >= 0) {
        writeByteAt(e.entryOffset + 0x1A, (uint8_t)(newStart & 0xFF));
        writeByteAt(e.entryOffset + 0x1B, (uint8_t)(newStart >> 8));
        writeByteAt(e.entryOffset + 0x1C, (uint8_t)(newSize & 0xFF));
        writeByteAt(e.entryOffset + 0x1D, (uint8_t)((newSize >> 8) & 0xFF));
        writeByteAt(e.entryOffset + 0x1E, (uint8_t)((newSize >> 16) & 0xFF));
        writeByteAt(e.entryOffset + 0x1F, (uint8_t)((newSize >> 24) & 0xFF));
    }
    e.startCluster = newStart;
    e.size = newSize;
    return true;
}

bool Fat12::deleteFile(const DirEntry& e, std::string* err) {
    if (!mounted_) { if (err) *err = "not mounted"; return false; }
    if (e.isDir()) { if (err) *err = "refusing to delete a directory"; return false; }
    for (int c : chainOf(e.startCluster)) setFatEntry(c, 0);
    if (e.entryOffset >= 0) writeByteAt(e.entryOffset, 0xE5); // mark deleted
    return true;
}

// ---- RAW / uncatalogued data ----------------------------------------------

long Fat12::clusterByteOffset(int cluster) const {
    return (dataStartSec_ + (long)(cluster - 2) * spc_) * 512;
}

void Fat12::collectNamedClusters(int cluster, std::vector<uint8_t>& status, bool isRoot) const {
    std::vector<DirEntry> entries = isRoot ? readRootDir(false) : readDirAtCluster(cluster, false);
    for (auto& e : entries) {
        if (e.name == "." || e.name == ".." || e.deleted) continue; // deleted stay "raw"
        if (e.isVolume()) continue;
        for (int c : chainOf(e.startCluster)) {
            int idx = c - 2;
            if (idx >= 0 && idx < (int)status.size()) status[idx] = CL_NAMED;
        }
        if (e.isDir() && e.startCluster >= 2) collectNamedClusters(e.startCluster, status, false);
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

long Fat12::freeDirSlot(int dirCluster) {
    if (dirCluster == 0) {
        for (int i = 0; i < ndir_; ++i) {
            long off = rootStartSec_ * 512 + (long)i * 32;
            uint8_t b; if (!readByteAt(off, b)) return -1;
            if (b == 0x00 || b == 0xE5) return off;
        }
        return -1;
    }
    int slotsPerCluster = spc_ * 512 / 32;
    for (int c : chainOf(dirCluster)) {
        long firstSec = dataStartSec_ + (long)(c - 2) * spc_;
        for (int s = 0; s < slotsPerCluster; ++s) {
            long off = firstSec * 512 + (long)s * 32;
            uint8_t b; if (!readByteAt(off, b)) return -1;
            if (b == 0x00 || b == 0xE5) return off;
        }
    }
    return -1;
}

void Fat12::writeDirEntry(long off, const uint8_t name11[11], uint8_t attr,
                          uint16_t cluster, uint32_t size) {
    uint8_t e[32]; memset(e, 0, sizeof e);
    memcpy(e, name11, 11);
    e[0x0B] = attr;
    e[0x1A] = cluster & 0xFF; e[0x1B] = (cluster >> 8) & 0xFF;
    e[0x1C] = size & 0xFF; e[0x1D] = (size >> 8) & 0xFF;
    e[0x1E] = (size >> 16) & 0xFF; e[0x1F] = (size >> 24) & 0xFF;
    for (int i = 0; i < 32; ++i) writeByteAt(off + i, e[i]);
}

bool Fat12::createFile(const std::string& dirPath, const std::string& name,
                       const std::vector<uint8_t>& data, std::string* err) {
    if (!mounted_) { if (err) *err = "not mounted"; return false; }
    int dc = resolveDirCluster(dirPath);
    if (dc < 0) { if (err) *err = "directory not found"; return false; }
    // Reject a duplicate name.
    std::string up = name; for (auto& c : up) c = (char)toupper((unsigned char)c);
    for (auto& e : list(dirPath, false)) {
        std::string nm = e.name; for (auto& c : nm) c = (char)toupper((unsigned char)c);
        if (nm == up) { if (err) *err = "a file with that name already exists"; return false; }
    }
    long slot = freeDirSlot(dc);
    if (slot < 0) { if (err) *err = "directory is full"; return false; }
    uint8_t nm[11]; pack83(name, nm);
    writeDirEntry(slot, nm, 0x20 /*archive*/, 0, 0);
    DirEntry tmp; tmp.entryOffset = slot; tmp.startCluster = 0; tmp.size = 0; tmp.attr = 0x20;
    return writeFile(tmp, data, err);   // allocates clusters + updates size/start
}

bool Fat12::renameEntry(DirEntry& e, const std::string& newName, std::string* err) {
    if (!mounted_ || e.entryOffset < 0) { if (err) *err = "cannot rename"; return false; }
    uint8_t nm[11]; pack83(newName, nm);
    for (int i = 0; i < 11; ++i) writeByteAt(e.entryOffset + i, nm[i]);
    e.raw83.assign((const char*)nm, 11);
    return true;
}

bool Fat12::makeDir(const std::string& parentPath, const std::string& name, std::string* err) {
    if (!mounted_) { if (err) *err = "not mounted"; return false; }
    int parent = resolveDirCluster(parentPath);
    if (parent < 0) { if (err) *err = "parent directory not found"; return false; }
    std::string up = name; for (auto& c : up) c = (char)toupper((unsigned char)c);
    for (auto& e : list(parentPath, false)) {
        std::string nm = e.name; for (auto& c : nm) c = (char)toupper((unsigned char)c);
        if (nm == up) { if (err) *err = "name already exists"; return false; }
    }
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
    uint8_t nm[11]; pack83(name, nm);
    writeDirEntry(slot, nm, 0x10, (uint16_t)nc, 0);
    return true;
}

// ---- Integrity check ------------------------------------------------------

Fat12::FsCheck Fat12::verify() const {
    FsCheck r;
    if (!mounted_) { r.issues.push_back("filesystem not mounted"); return r; }
    std::vector<int> owner(totalClusters_ > 0 ? totalClusters_ : 0, -1);
    // Walk every named file/dir chain, flagging bad pointers and cross-links.
    std::function<void(int, bool, const std::string&)> walk =
        [&](int cluster, bool isRoot, const std::string& base) {
        for (auto& e : (isRoot ? readRootDir(false) : readDirAtCluster(cluster, false))) {
            if (e.name == "." || e.name == ".." || e.deleted || e.isVolume()) continue;
            std::string path = base + "/" + e.name;
            int c = e.startCluster, guard = 0;
            while (c >= 2 && c < 0xFF8 && guard++ < totalClusters_ + 4) {
                int idx = c - 2;
                if (idx < 0 || idx >= (int)owner.size()) { r.issues.push_back(path + ": cluster out of range"); break; }
                if (owner[idx] >= 0) { r.issues.push_back(path + ": cross-linked at cluster " + std::to_string(c)); break; }
                owner[idx] = 1; r.usedByFiles++;
                int nxt = fatEntry(c);
                if (nxt == 0) { r.issues.push_back(path + ": chain hits a free cluster"); break; }
                c = nxt;
            }
            if (e.isDir() && e.startCluster >= 2) walk(e.startCluster, false, path);
        }
    };
    walk(0, true, "");
    for (int c = 2; c < totalClusters_ + 2; ++c) {
        int idx = c - 2;
        if (fatEntry(c) == 0) r.freeClusters++;
        else if (idx >= 0 && idx < (int)owner.size() && owner[idx] < 0) r.unreferenced++;
    }
    return r;
}

} // namespace msx
