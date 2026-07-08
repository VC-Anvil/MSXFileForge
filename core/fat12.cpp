#include "fat12.h"
#include <cstring>
#include <cstdio>
#include <sstream>
#include <iomanip>

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

bool Fat12::mount(const DiskImage& disk, std::string* err) {
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

} // namespace msx
