// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Alonso (GS·RUN). MSXFileForge.
// Free software under the GNU GPL v3 (or later) - see LICENSE. Modified
// versions must be distributed under the same license.
// MSXFileForge CLI - exercises the core library against real images.
//
// Run without arguments for the full command list. Every <FILE> argument may
// be a path into subdirectories ("GAMES/SAVE/SLOT1.DAT"), case-insensitive.
#include "../core/disk_image.h"
#include "../core/fat12.h"
#include "../core/msxfile.h"
#include "../core/rom.h"
#include "../core/cas.h"
#include "../core/util.h"
#include "../core/tape.h"
#include "../core/partition.h"
#include "hostfs.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#if defined(_WIN32)
#include <io.h>
#include <fcntl.h>
#endif

static void setStdoutBinary() {
#if defined(_WIN32)
    _setmode(_fileno(stdout), _O_BINARY); // stop \n -> \r\n mangling on raw output
#endif
}

using namespace msx;

static std::vector<uint8_t> slurp(const char* path) {
    std::vector<uint8_t> v;
    FILE* f = fopen(path, "rb");
    if (!f) return v;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n > 0) { v.resize((size_t)n); if (fread(v.data(),1,(size_t)n,f)!=(size_t)n) v.clear(); }
    fclose(f);
    return v;
}

// --part N (global option): which partition of a hard disk image to use.
static int g_part = 0;

// Mount the filesystem of an image: a floppy / superfloppy directly, a
// partitioned disk through partition g_part (default: the first FAT one).
static bool mountVolume(DiskImage& dk, Fat12& fs, std::string& err) {
    auto parts = findPartitions(dk);
    if (parts.empty()) {
        if (g_part > 1) { err = "image has no partition table"; return false; }
        return fs.mount(dk, &err);
    }
    const Partition* p = nullptr;
    if (g_part > 0) {
        for (auto& x : parts) if (x.number == g_part) p = &x;
        if (!p) { err = "no partition " + std::to_string(g_part) + " (the image has " +
                        std::to_string(parts.size()) + ")"; return false; }
    } else {
        for (auto& x : parts) if (x.fat()) { p = &x; break; }
        if (!p) { err = "no FAT partition on this disk"; return false; }
    }
    if (fs.mount(dk, p->firstSector, p->sectorCount, &err)) return true;
    // A table that is really junk in a floppy's boot sector: try the image as a
    // plain volume before giving up (only when no partition was asked for).
    std::string plain;
    if (g_part == 0 && fs.mount(dk, &plain)) return true;
    return false;
}

static int mountDisk(const char* path, DiskImage& dk, Fat12& fs) {
    std::string err;
    if (!dk.load(path, &err)) { fprintf(stderr, "load error: %s\n", err.c_str()); return 2; }
    if (!mountVolume(dk, fs, err)) { fprintf(stderr, "mount error: %s\n", err.c_str()); return 3; }
    return 0;
}

// Resolve a FILE argument ("NAME.EXT", "/DIR/NAME.EXT" or "DIR\\NAME.EXT").
static bool findEntry(const Fat12& fs, const std::string& name, DirEntry& out) {
    std::string p = name;
    if (p.empty() || (p[0] != '/' && p[0] != '\\')) p = "/" + p;
    return fs.lookup(p, out);
}

static int cmd_info(const char* img) {
    DiskImage dk; std::string err;
    if (!dk.load(img, &err)) { fprintf(stderr, "load error: %s\n", err.c_str()); return 2; }
    printf("Image      : %s\n", img);
    printf("Format     : %s\n", to_string(dk.format()));
    printf("Geometry   : %d cyl x %d head x %d spt (%ld logical sectors)\n",
           dk.cylinders(), dk.heads(), dk.sectorsPerTrack(), dk.logicalSectors());
    printf("Protected  : %s\n", dk.isProtected() ? "YES" : "no");
    auto parts = findPartitions(dk);
    if (!parts.empty()) {
        printf("Partitions : %zu (use --part N; default = first FAT partition)\n", parts.size());
        for (auto& p : parts)
            printf("  %d  %-8s %-15s start=%-9ld %8ld sectors (%ld MB)\n", p.number,
                   p.logical ? "logical" : "primary", partitionTypeName(p.type),
                   p.firstSector, p.sectorCount, p.sectorCount / 2048);
    }
    Fat12 fs;
    if (mountVolume(dk, fs, err)) {
        if (!parts.empty()) printf("Volume     : starts at sector %ld\n", fs.volumeStart());
        printf("Filesystem : FAT%d  OEM='%s'  label='%s'\n", fs.fatBits(),
               fs.oemName().c_str(), fs.volumeLabel().c_str());
        printf("Clusters   : %d x %d bytes  free=%ld bytes\n",
               fs.totalClusters(), fs.sectorsPerCluster()*512, fs.freeBytes());
    } else {
        printf("Filesystem : (none: %s)\n", err.c_str());
    }
    return 0;
}

static void printListing(const std::vector<DirEntry>& es) {
    printf("%-13s %-6s %10s  %s %s\n", "NAME", "ATTR", "SIZE", "DATE", "TIME");
    for (auto& e : es) {
        if (e.isVolume()) continue;
        printf("%-13s %-6s %10u  %s %s%s\n",
               e.name.c_str(), e.attrString().c_str(), e.size,
               e.dateString().c_str(), e.timeString().c_str(),
               e.deleted ? "  [deleted]" : "");
    }
}

static int cmd_ls(int argc, char** argv) {
    const char* img = argv[2];
    std::string path = "/";
    bool deleted = false;
    for (int i = 3; i < argc; ++i) {
        if (!strcmp(argv[i], "--deleted")) deleted = true;
        else path = argv[i];
    }
    DiskImage dk; Fat12 fs;
    int rc = mountDisk(img, dk, fs); if (rc) return rc;
    auto es = fs.list(path, deleted);
    printListing(es);
    return 0;
}

static int cmd_cat(int argc, char** argv, bool detok) {
    if (argc < 4) { fprintf(stderr, "need <image> <file>\n"); return 1; }
    DiskImage dk; Fat12 fs;
    int rc = mountDisk(argv[2], dk, fs); if (rc) return rc;
    DirEntry e;
    if (!findEntry(fs, argv[3], e)) { fprintf(stderr, "file not found: %s\n", argv[3]); return 4; }
    auto data = fs.readFile(e);
    if (detok) {
        std::string txt = detokenizeBasic(data);
        fwrite(txt.data(), 1, txt.size(), stdout);
    } else {
        setStdoutBinary();
        fwrite(data.data(), 1, data.size(), stdout);
    }
    return 0;
}

static int cmd_extract(int argc, char** argv) {
    if (argc < 5) { fprintf(stderr, "need <image> <file> <out>\n"); return 1; }
    DiskImage dk; Fat12 fs;
    int rc = mountDisk(argv[2], dk, fs); if (rc) return rc;
    DirEntry e;
    if (!findEntry(fs, argv[3], e)) { fprintf(stderr, "file not found: %s\n", argv[3]); return 4; }
    auto data = fs.readFile(e);
    FILE* out = fopen(argv[4], "wb");
    if (!out) { fprintf(stderr, "cannot write %s\n", argv[4]); return 5; }
    fwrite(data.data(), 1, data.size(), out);
    fclose(out);
    printf("wrote %zu bytes to %s\n", data.size(), argv[4]);
    return 0;
}

static int cmd_protect(const char* img) {
    DiskImage dk; std::string err;
    if (!dk.load(img, &err)) { fprintf(stderr, "load error: %s\n", err.c_str()); return 2; }
    printf("Format: %s   Protected: %s\n", to_string(dk.format()),
           dk.isProtected() ? "YES" : "no");
    if (dk.isProtected()) printf("%s\n", dk.protectionReport().c_str());
    int shown = 0, anomalies = 0;
    for (auto& s : dk.sectors()) {
        bool odd = s.nonStandard || s.deleted || s.idCrcError || s.dataCrcError || s.duplicateId;
        if (odd) anomalies++;
        if (odd && shown < 40) {
            printf("  C%02d H%d S%02d size=%zu%s%s%s%s%s lba=%ld\n",
                   s.cyl, s.head, s.sec, s.size,
                   s.nonStandard ? " NONSTD" : "",
                   s.deleted ? " DELETED" : "",
                   s.idCrcError ? " IDCRC" : "",
                   s.dataCrcError ? " DATACRC" : "",
                   s.duplicateId ? " DUPID" : "", s.lba);
            shown++;
        }
    }
    printf("Physical sectors: %zu   anomalies: %d\n", dk.sectors().size(), anomalies);
    return 0;
}

static int cmd_put(int argc, char** argv) {
    // put <image> <FILE> <hostfile> [outimage]
    if (argc < 5) { fprintf(stderr, "need <image> <FILE> <hostfile> [outimage]\n"); return 1; }
    DiskImage dk; Fat12 fs;
    int rc = mountDisk(argv[2], dk, fs); if (rc) return rc;
    DirEntry ent;
    if (!findEntry(fs, argv[3], ent)) { fprintf(stderr, "file not found: %s\n", argv[3]); return 4; }
    DirEntry* e = &ent;
    auto data = slurp(argv[4]);
    std::string err;
    if (!fs.writeFile(*e, data, &err)) { fprintf(stderr, "write: %s\n", err.c_str()); return 6; }
    const char* out = (argc >= 6) ? argv[5] : argv[2];
    if (!dk.save(out, &err)) { fprintf(stderr, "save: %s\n", err.c_str()); return 7; }
    printf("wrote %zu bytes into %s (start cl=%u) -> %s\n",
           data.size(), e->name.c_str(), e->startCluster, out);
    return 0;
}

static int cmd_del(int argc, char** argv) {
    if (argc < 4) { fprintf(stderr, "need <image> <FILE> [outimage]\n"); return 1; }
    DiskImage dk; Fat12 fs;
    int rc = mountDisk(argv[2], dk, fs); if (rc) return rc;
    DirEntry e;
    if (!findEntry(fs, argv[3], e)) { fprintf(stderr, "file not found: %s\n", argv[3]); return 4; }
    std::string err;
    bool ok = e.isDir() ? fs.removeDir(e, &err) : fs.deleteFile(e, &err);
    if (!ok) { fprintf(stderr, "delete: %s\n", err.c_str()); return 6; }
    const char* out = (argc >= 5) ? argv[4] : argv[2];
    if (!dk.save(out, &err)) { fprintf(stderr, "save: %s\n", err.c_str()); return 7; }
    printf("deleted %s -> %s\n", argv[3], out);
    return 0;
}

static int cmd_tok(int argc, char** argv) {
    // tok <textfile>  -> tokenize then detokenize (round-trip check)
    if (argc < 3) { fprintf(stderr, "need <textfile>\n"); return 1; }
    auto d = slurp(argv[2]);
    std::string text((const char*)d.data(), d.size());
    auto toks = tokenizeBasic(text);
    if (argc >= 4) { // tok <text> <out.bas> : write tokenized file
        FILE* o = fopen(argv[3], "wb");
        if (o) { fwrite(toks.data(),1,toks.size(),o); fclose(o); }
        fprintf(stderr, "tokenized %zu bytes -> %s\n", toks.size(), argv[3]);
    }
    std::string back = detokenizeBasic(toks);
    fwrite(back.data(), 1, back.size(), stdout);
    return 0;
}

static int cmd_raw(int argc, char** argv) {
    // raw <image> [out.bin]  -> report cluster map + optionally dump RAW data
    if (argc < 3) { fprintf(stderr, "need <image> [out.bin]\n"); return 1; }
    DiskImage dk; Fat12 fs;
    int rc = mountDisk(argv[2], dk, fs); if (rc) return rc;
    auto map = fs.clusterMap();
    long cb = (long)fs.sectorsPerCluster() * 512;
    long freeN = 0, named = 0, raw = 0;
    for (auto s : map) { if (s == 0) freeN++; else if (s == 1) named++; else raw++; }
    printf("Data clusters : %zu  (%ld bytes each)\n", map.size(), cb);
    printf("  named files : %ld  (%ld KB)\n", named, named*cb/1024);
    printf("  RAW unnamed : %ld  (%ld KB)  <-- uncatalogued game data\n", raw, raw*cb/1024);
    printf("  free        : %ld  (%ld KB)\n", freeN, freeN*cb/1024);
    if (argc >= 4) {
        auto data = fs.extractRawUnnamed();
        FILE* o = fopen(argv[3], "wb");
        if (o) { fwrite(data.data(),1,data.size(),o); fclose(o);
            printf("Extracted RAW data: %zu bytes -> %s\n", data.size(), argv[3]); }
    }
    return 0;
}

static int cmd_carve(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "need <image>\n"); return 1; }
    DiskImage dk; Fat12 fs;
    int rc = mountDisk(argv[2], dk, fs); if (rc) return rc;
    auto raw = fs.extractRawUnnamed();
    auto blocks = carveBlocks(raw);
    printf("RAW region: %zu bytes, %zu carved block(s):\n", raw.size(), blocks.size());
    int shown = 0;
    for (auto& b : blocks) {
        if (b.type == CarveType::Unknown && b.length < 512) continue;
        printf("  @0x%06lX  %-16s len=%-7ld", b.offset, carveTypeName(b.type), b.length);
        if (b.type == CarveType::Bload)
            printf("  start=%04X end=%04X exec=%04X", b.start, b.end, b.exec);
        printf("\n");
        if (++shown >= 60) { printf("  ...\n"); break; }
    }
    int nb=0,nB=0,nu=0; for (auto&b:blocks){ if(b.type==CarveType::Bload)nb++; else if(b.type==CarveType::BasicTokenized)nB++; else nu++; }
    printf("Totals: %d BLOAD, %d BASIC, %d unknown-gap\n", nb, nB, nu);
    return 0;
}

static int cmd_rawput(int argc, char** argv) {
    // rawput <image> <hostfile> [outimage]
    if (argc < 4) { fprintf(stderr, "need <image> <hostfile> [outimage]\n"); return 1; }
    DiskImage dk; Fat12 fs;
    int rc = mountDisk(argv[2], dk, fs); if (rc) return rc;
    auto data = slurp(argv[3]);
    long written = 0; std::string err;
    bool ok = fs.writeRawUnnamed(data, &written, &err);
    printf("wrote %ld of %zu bytes into RAW region%s\n", written, data.size(),
           ok ? "" : (" (" + err + ")").c_str());
    const char* out = (argc >= 5) ? argv[4] : argv[2];
    if (!dk.save(out, &err)) { fprintf(stderr, "save: %s\n", err.c_str()); return 7; }
    printf("saved -> %s\n", out);
    return ok ? 0 : 8;
}

static std::string baseName(const std::string& p) {
    size_t s = p.find_last_of("/\\");
    return s == std::string::npos ? p : p.substr(s + 1);
}

// "720" / "360" (KB floppies) or "<N>M" (MB) -> sectors; 0 if invalid.
static long parseSize(const std::string& t) {
    if (t == "720") return 1440;
    if (t == "360") return 720;
    if (t.size() > 1 && (t.back() == 'M' || t.back() == 'm')) {
        long mb = atol(t.substr(0, t.size() - 1).c_str());
        return (mb >= 1 && mb <= 2047) ? mb * 2048 : 0;   // < 2 GB: offsets are 32-bit on Windows
    }
    return 0;
}

static bool writeImage(const char* path, const std::vector<uint8_t>& img) {
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    bool ok = fwrite(img.data(), 1, img.size(), f) == img.size();
    return fclose(f) == 0 && ok;
}

static int cmd_format(int argc, char** argv) {
    // format <out> [360|720|<N>M] [LABEL]
    if (argc < 3) { fprintf(stderr, "need <out.dsk> [360|720|<N>M] [LABEL]\n"); return 1; }
    std::string sz = argc >= 4 ? argv[3] : "720";
    long secs = parseSize(sz);
    if (!secs) { fprintf(stderr, "size must be 360, 720 or <N>M (1-2047)\n"); return 1; }
    std::vector<uint8_t> img = (secs == 1440 || secs == 720) ? formatBlankImage((int)secs)
                                                              : formatFatVolume(secs, 0, argc >= 5 ? argv[4] : "");
    if (img.empty()) { fprintf(stderr, "cannot format that size\n"); return 1; }
    if (argc >= 5 && (secs == 1440 || secs == 720)) {   // floppy label via the filesystem
        DiskImage dk; Fat12 fs; std::string err;
        dk.loadBytes(img, argv[2], &err);
        if (!fs.mount(dk, &err) || !fs.setVolumeLabel(argv[4], &err)) { fprintf(stderr, "label: %s\n", err.c_str()); return 6; }
        img = dk.raw();
    }
    if (!writeImage(argv[2], img)) { fprintf(stderr, "cannot write %s\n", argv[2]); return 2; }
    printf("formatted %s image -> %s (%zu bytes)\n", sz.c_str(), argv[2], img.size());
    return 0;
}

static int cmd_mkhdd(int argc, char** argv) {
    // mkhdd <out.img> <N>M[,<N>M...]
    if (argc < 4) { fprintf(stderr, "need <out.img> <N>M[,<N>M...]\n"); return 1; }
    std::vector<long> sizes;
    std::string list = argv[3];
    size_t p = 0;
    while (p <= list.size()) {
        size_t c = list.find(',', p);
        std::string item = list.substr(p, c == std::string::npos ? std::string::npos : c - p);
        long secs = item.empty() ? 0 : parseSize(item.back() == 'M' || item.back() == 'm' ? item : item + "M");
        if (!secs || secs == 1440 || secs == 720) { fprintf(stderr, "bad partition size '%s' (use e.g. 32M)\n", item.c_str()); return 1; }
        sizes.push_back(secs);
        if (sizes.size() > 255) { fprintf(stderr, "at most 255 partitions\n"); return 1; }
        if (c == std::string::npos) break;
        p = c + 1;
    }
    std::string err;
    auto img = buildPartitionedImage(sizes, &err);
    if (img.empty()) { fprintf(stderr, "mkhdd: %s\n", err.c_str()); return 6; }
    if (!writeImage(argv[2], img)) { fprintf(stderr, "cannot write %s\n", argv[2]); return 2; }
    printf("created %s: %zu partition(s), %zu bytes\n", argv[2], sizes.size(), img.size());
    return 0;
}

static int cmd_add(int argc, char** argv) {
    // add <image> <hostfile> [/DIR] [NAME]
    if (argc < 4) { fprintf(stderr, "need <image> <hostfile> [/DIR] [NAME]\n"); return 1; }
    DiskImage dk; Fat12 fs; int rc = mountDisk(argv[2], dk, fs); if (rc) return rc;
    std::string dir = (argc >= 5) ? argv[4] : "/";
    std::string name;
    if (argc >= 6) name = argv[5];
    else {   // derive a valid 8.3 name from the host file name
        std::vector<std::string> taken;
        for (auto& x : fs.list(dir, false)) taken.push_back(x.name);
        name = Fat12::suggestName83(baseName(argv[3]), taken);
    }
    auto data = slurp(argv[3]);
    std::string err;
    if (!fs.createFile(dir, name, data, &err)) { fprintf(stderr, "add: %s\n", err.c_str()); return 6; }
    if (!dk.save(argv[2], &err)) { fprintf(stderr, "save: %s\n", err.c_str()); return 7; }
    std::string where = dir;
    if (where.empty() || (where.back() != '/' && where.back() != '\\')) where += "/";
    printf("added %s (%zu bytes) as %s%s\n", argv[3], data.size(), where.c_str(), name.c_str());
    return 0;
}

static int cmd_rename(int argc, char** argv) {
    if (argc < 5) { fprintf(stderr, "need <image> <OLD> <NEW>\n"); return 1; }
    DiskImage dk; Fat12 fs; int rc = mountDisk(argv[2], dk, fs); if (rc) return rc;
    DirEntry e;
    if (!findEntry(fs, argv[3], e)) { fprintf(stderr, "not found: %s\n", argv[3]); return 4; }
    std::string err;
    if (!fs.renameEntry(e, argv[4], &err)) { fprintf(stderr, "rename: %s\n", err.c_str()); return 6; }
    if (!dk.save(argv[2], &err)) { fprintf(stderr, "save: %s\n", err.c_str()); return 7; }
    printf("renamed %s -> %s\n", argv[3], argv[4]); return 0;
}

static int cmd_mkdir(int argc, char** argv) {
    if (argc < 4) { fprintf(stderr, "need <image> <NAME> [/PARENT]\n"); return 1; }
    DiskImage dk; Fat12 fs; int rc = mountDisk(argv[2], dk, fs); if (rc) return rc;
    std::string parent = (argc >= 5) ? argv[4] : "/";
    std::string err;
    if (!fs.makeDir(parent, argv[3], &err)) { fprintf(stderr, "mkdir: %s\n", err.c_str()); return 6; }
    if (!dk.save(argv[2], &err)) { fprintf(stderr, "save: %s\n", err.c_str()); return 7; }
    printf("created dir %s in %s\n", argv[3], parent.c_str()); return 0;
}

static std::string upper(std::string s) {
    for (auto& c : s) c = (char)toupper((unsigned char)c);
    return s;
}

static int cmd_undelete(int argc, char** argv) {
    // undelete <image> <[DIR/]NAME> [LETTER] [outimage]
    if (argc < 4) { fprintf(stderr, "need <image> <[DIR/]NAME> [LETTER] [outimage]\n"); return 1; }
    DiskImage dk; Fat12 fs; int rc = mountDisk(argv[2], dk, fs); if (rc) return rc;
    std::string path = argv[3];
    size_t cut = path.find_last_of("/\\");
    std::string dir = cut == std::string::npos ? "/" : path.substr(0, cut);
    if (dir.empty()) dir = "/";
    std::string leaf = upper(cut == std::string::npos ? path : path.substr(cut + 1));
    if (leaf.size() < 2) { fprintf(stderr, "name too short\n"); return 1; }
    int argi = 4;
    char letter = leaf[0];
    if (letter == '?') {
        if (argc <= argi || strlen(argv[argi]) != 1) {
            fprintf(stderr, "the first letter is lost: give it, e.g. `undelete img ?AME.BIN G`\n"); return 1;
        }
        letter = argv[argi++][0];
    } else if (argc > argi && strlen(argv[argi]) == 1) {
        letter = argv[argi++][0];
    }
    const char* out = argc > argi ? argv[argi] : argv[2];
    std::vector<DirEntry> hits;
    for (auto& e : fs.list(dir, true))
        if (e.deleted && upper(e.name).substr(1) == leaf.substr(1)) hits.push_back(e);
    if (hits.empty()) { fprintf(stderr, "no deleted entry matches %s in %s\n", leaf.c_str(), dir.c_str()); return 4; }
    std::string err;
    for (auto& e : hits) {   // several deletions of the same name: newest-recoverable first
        if (fs.undelete(e, letter, &err)) {
            if (!dk.save(out, &err)) { fprintf(stderr, "save: %s\n", err.c_str()); return 7; }
            printf("recovered %s (%u bytes) -> %s\n", e.name.c_str(), e.size, out);
            return 0;
        }
    }
    fprintf(stderr, "undelete: %s\n", err.c_str());
    return 6;
}

static int cmd_attrib(int argc, char** argv) {
    // attrib <image> <FILE> [+R -R +H -H +S -S +A -A ...]
    if (argc < 4) { fprintf(stderr, "need <image> <FILE> [+R|-R|+H|-H|+S|-S|+A|-A ...]\n"); return 1; }
    DiskImage dk; Fat12 fs; int rc = mountDisk(argv[2], dk, fs); if (rc) return rc;
    DirEntry e;
    if (!findEntry(fs, argv[3], e)) { fprintf(stderr, "not found: %s\n", argv[3]); return 4; }
    if (argc == 4) { printf("%s  %s\n", e.attrString().c_str(), e.name.c_str()); return 0; }
    uint8_t a = e.attr;
    for (int i = 4; i < argc; ++i) {
        std::string f = upper(argv[i]);
        uint8_t bit = 0;
        if (f.size() == 2) bit = f[1] == 'R' ? 0x01 : f[1] == 'H' ? 0x02 : f[1] == 'S' ? 0x04 : f[1] == 'A' ? 0x20 : 0;
        if (!bit || (f[0] != '+' && f[0] != '-')) { fprintf(stderr, "bad flag %s\n", argv[i]); return 1; }
        a = f[0] == '+' ? (uint8_t)(a | bit) : (uint8_t)(a & ~bit);
    }
    std::string err;
    if (!fs.setAttributes(e, a, &err)) { fprintf(stderr, "attrib: %s\n", err.c_str()); return 6; }
    if (!dk.save(argv[2], &err)) { fprintf(stderr, "save: %s\n", err.c_str()); return 7; }
    printf("%s  %s\n", e.attrString().c_str(), e.name.c_str());
    return 0;
}

static int cmd_label(int argc, char** argv) {
    // label <image> [LABEL | --clear]
    DiskImage dk; Fat12 fs; int rc = mountDisk(argv[2], dk, fs); if (rc) return rc;
    if (argc < 4) { printf("%s\n", fs.volumeLabel().empty() ? "(no label)" : fs.volumeLabel().c_str()); return 0; }
    std::string want = strcmp(argv[3], "--clear") == 0 ? "" : argv[3];
    std::string err;
    if (!fs.setVolumeLabel(want, &err)) { fprintf(stderr, "label: %s\n", err.c_str()); return 6; }
    if (!dk.save(argv[2], &err)) { fprintf(stderr, "save: %s\n", err.c_str()); return 7; }
    printf("label: %s\n", fs.volumeLabel().empty() ? "(removed)" : fs.volumeLabel().c_str());
    return 0;
}

static bool writeHost(const std::string& path, const std::vector<uint8_t>& d) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    bool ok = fwrite(d.data(), 1, d.size(), f) == d.size();
    return fclose(f) == 0 && ok;
}

static int unpackDir(const Fat12& fs, const std::string& dir, const std::string& host,
                     std::vector<int>& seen, int& files) {
    if (!hostfs::makeDir(host)) { fprintf(stderr, "cannot create %s\n", host.c_str()); return 5; }
    for (auto& e : fs.list(dir, false)) {
        if (e.isVolume() || e.name == "." || e.name == "..") continue;
        std::string sub = (dir == "/" ? "" : dir) + "/" + e.name;
        std::string target = hostfs::join(host, e.name);
        if (e.isDir()) {
            if (std::find(seen.begin(), seen.end(), (int)e.startCluster) != seen.end()) continue;
            seen.push_back((int)e.startCluster);
            if (int rc = unpackDir(fs, sub, target, seen, files)) return rc;
        } else {
            if (!writeHost(target, fs.readFile(e))) { fprintf(stderr, "cannot write %s\n", target.c_str()); return 5; }
            files++;
        }
    }
    return 0;
}

static int cmd_unpack(int argc, char** argv) {
    // unpack <image> <hostdir>
    if (argc < 4) { fprintf(stderr, "need <image> <hostdir>\n"); return 1; }
    DiskImage dk; Fat12 fs; int rc = mountDisk(argv[2], dk, fs); if (rc) return rc;
    std::vector<int> seen; int files = 0;
    if ((rc = unpackDir(fs, "/", argv[3], seen, files))) return rc;
    printf("extracted %d file(s) to %s\n", files, argv[3]);
    return 0;
}

static int packDir(Fat12& fs, const std::string& host, const std::string& dir, int& files) {
    std::vector<std::string> taken;
    for (auto& e : fs.list(dir, false)) taken.push_back(e.name);
    for (auto& h : hostfs::list(host)) {
        std::string name = Fat12::suggestName83(h.name, taken);
        taken.push_back(name);
        if (upper(h.name) != name) printf("  %s -> %s\n", hostfs::join(host, h.name).c_str(), name.c_str());
        std::string err, sub = (dir == "/" ? "" : dir) + "/" + name;
        if (h.isDir) {
            if (!fs.makeDir(dir, name, &err)) { fprintf(stderr, "mkdir %s: %s\n", sub.c_str(), err.c_str()); return 6; }
            if (int rc = packDir(fs, hostfs::join(host, h.name), sub, files)) return rc;
        } else {
            auto data = slurp(hostfs::join(host, h.name).c_str());
            if (!fs.createFile(dir, name, data, &err)) { fprintf(stderr, "add %s: %s\n", sub.c_str(), err.c_str()); return 6; }
            files++;
        }
    }
    return 0;
}

static int cmd_pack(int argc, char** argv) {
    // pack <hostdir> <out.dsk> [360|720] [--label NAME]
    if (argc < 4) { fprintf(stderr, "need <hostdir> <out.dsk> [360|720] [--label NAME]\n"); return 1; }
    if (!hostfs::isDir(argv[2])) { fprintf(stderr, "not a directory: %s\n", argv[2]); return 2; }
    int kb = 720; std::string label;
    for (int i = 4; i < argc; ++i) {
        if (!strcmp(argv[i], "--label") && i + 1 < argc) label = argv[++i];
        else if (!strcmp(argv[i], "360")) kb = 360;
        else if (!strcmp(argv[i], "720")) kb = 720;
        else { fprintf(stderr, "unknown option %s\n", argv[i]); return 1; }
    }
    DiskImage dk; Fat12 fs; std::string err;
    dk.loadBytes(formatBlankImage(kb == 360 ? 720 : 1440), argv[3], &err);
    if (!fs.mount(dk, &err)) { fprintf(stderr, "format: %s\n", err.c_str()); return 3; }
    if (!label.empty() && !fs.setVolumeLabel(label, &err)) { fprintf(stderr, "label: %s\n", err.c_str()); return 6; }
    int files = 0;
    if (int rc = packDir(fs, argv[2], "/", files)) return rc;
    if (!dk.save(argv[3], &err)) { fprintf(stderr, "save: %s\n", err.c_str()); return 7; }
    printf("packed %d file(s) into %dKB image %s (%ld bytes free)\n", files, kb, argv[3], fs.freeBytes());
    return 0;
}

static int cmd_crc(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "need <image> [FILE]\n"); return 1; }
    if (argc >= 4) {
        DiskImage dk; Fat12 fs; int rc = mountDisk(argv[2], dk, fs); if (rc) return rc;
        DirEntry e;
        if (!findEntry(fs, argv[3], e)) { fprintf(stderr, "not found: %s\n", argv[3]); return 4; }
        auto d = fs.readFile(e);
        printf("%08X  %s  (%zu bytes)\n", crc32(d), argv[3], d.size());
    } else {
        auto d = slurp(argv[2]);
        printf("%08X  %s  (%zu bytes)\n", crc32(d), argv[2], d.size());
    }
    return 0;
}

static int cmd_fsck(const char* img) {
    DiskImage dk; Fat12 fs; std::string err;
    if (!dk.load(img, &err)) { fprintf(stderr, "%s\n", err.c_str()); return 2; }
    if (!mountVolume(dk, fs, err)) { fprintf(stderr, "mount: %s\n", err.c_str()); return 3; }
    Fat12::FsCheck c = fs.verify();
    printf("used-by-files: %ld clusters   unreferenced(RAW/lost): %ld   free: %ld\n",
           c.usedByFiles, c.unreferenced, c.freeClusters);
    if (c.issues.empty()) printf("FAT integrity: OK (no cross-links or bad chains)\n");
    else { printf("issues (%zu):\n", c.issues.size());
        for (auto& i : c.issues) printf("  - %s\n", i.c_str()); }
    return c.issues.empty() ? 0 : 1;
}

// Every file on the disk as "/DIR/NAME" -> entry (directories walked once
// each, so a corrupt tree that links back to an ancestor cannot loop).
static void allFiles(const Fat12& fs, const std::string& dir,
                     std::vector<std::pair<std::string, DirEntry>>& out,
                     std::vector<int>& seenDirs) {
    for (auto& e : fs.list(dir, false)) {
        if (e.isVolume() || e.name == "." || e.name == "..") continue;
        std::string p = (dir == "/" ? "" : dir) + "/" + e.name;
        if (e.isDir()) {
            if (std::find(seenDirs.begin(), seenDirs.end(), (int)e.startCluster) != seenDirs.end()) continue;
            seenDirs.push_back((int)e.startCluster);
            allFiles(fs, p, out, seenDirs);
        } else out.push_back({p, e});
    }
}

static int cmd_diff(int argc, char** argv) {
    if (argc < 4) { fprintf(stderr, "need <imageA> <imageB>\n"); return 1; }
    DiskImage a, b; Fat12 fa, fb; int rc;
    if ((rc = mountDisk(argv[2], a, fa))) return rc;
    if ((rc = mountDisk(argv[3], b, fb))) return rc;
    std::vector<std::pair<std::string, DirEntry>> la, lb;
    std::vector<int> sa, sb;
    allFiles(fa, "/", la, sa); allFiles(fb, "/", lb, sb);
    auto findIn = [](std::vector<std::pair<std::string, DirEntry>>& l, const std::string& n) -> DirEntry* {
        for (auto& e : l) if (e.first == n) return &e.second;
        return nullptr;
    };
    printf("--- %s  vs  +++ %s\n", baseName(argv[2]).c_str(), baseName(argv[3]).c_str());
    int same = 0, diff = 0, onlyA = 0, onlyB = 0;
    for (auto& e : la) {
        DirEntry* o = findIn(lb, e.first);
        if (!o) { printf("  - only in A: %s\n", e.first.c_str()); onlyA++; continue; }
        uint32_t ca = crc32(fa.readFile(e.second)), cb = crc32(fb.readFile(*o));
        if (ca != cb) { printf("  * differs : %s (%08X vs %08X)\n", e.first.c_str(), ca, cb); diff++; }
        else same++;
    }
    for (auto& e : lb)
        if (!findIn(la, e.first)) { printf("  + only in B: %s\n", e.first.c_str()); onlyB++; }
    printf("summary: %d same, %d differ, %d only-A, %d only-B\n", same, diff, onlyA, onlyB);
    return (diff || onlyA || onlyB) ? 1 : 0;
}

static int cmd_tape(int argc, char** argv, const char* mode) {
    // <mode> <in> <out> [-2]
    if (argc < 4) { fprintf(stderr, "need <in> <out> [-2]\n"); return 1; }
    int baud = (argc >= 5 && !strcmp(argv[4], "-2")) ? 2400 : 1200;
    auto in = slurp(argv[2]);
    if (in.empty()) { fprintf(stderr, "cannot read %s\n", argv[2]); return 2; }
    std::vector<uint8_t> out; std::string err;
    if      (!strcmp(mode, "cas2wav")) out = casToWav(in, baud);
    else if (!strcmp(mode, "cas2tsx")) out = casToTsx(in, baud);
    else if (!strcmp(mode, "tsx2cas")) out = tsxToCas(in, &err);
    else if (!strcmp(mode, "wav2cas")) out = wavToCas(in, &err);
    if (!err.empty()) fprintf(stderr, "warning: %s\n", err.c_str());
    FILE* f = fopen(argv[3], "wb"); if (!f) { fprintf(stderr, "cannot write\n"); return 3; }
    fwrite(out.data(), 1, out.size(), f); fclose(f);
    printf("%s: %zu -> %zu bytes -> %s\n", mode, in.size(), out.size(), argv[3]);
    return 0;
}

static int cmd_rominfo(const char* path) {
    auto d = slurp(path);
    if (d.empty()) { fprintf(stderr, "cannot read %s\n", path); return 2; }
    RomInfo r = analyzeRom(d);
    printf("ROM        : %s\n", path);
    printf("Size       : %ld bytes (%d x 16KB pages)\n", r.size, r.pages16k);
    printf("Header 'AB' : @0x0000=%s  @0x4000=%s\n",
           r.hasHeaderAt0 ? "yes":"no", r.hasHeaderAt4000 ? "yes":"no");
    if (r.hasHeaderAt0 || r.hasHeaderAt4000)
        printf("Entries    : INIT=%04X STATEMENT=%04X DEVICE=%04X TEXT=%04X\n",
               r.init, r.statement, r.device, r.text);
    printf("Mapper     : %s\n", mapperName(r.mapper));
    return 0;
}

static int cmd_cas(const char* path) {
    auto d = slurp(path);
    if (d.empty()) { fprintf(stderr, "cannot read %s\n", path); return 2; }
    auto blocks = parseCas(d);
    printf("CAS blocks : %zu\n", blocks.size());
    for (auto& b : blocks)
        printf("  @0x%06lX  %-16s %-8s len=%ld\n", b.offset, casKindName(b.kind),
               b.filename.c_str(), b.dataLength);
    return 0;
}

int main(int argc, char** argv) {
    // Strip the global --part N option wherever it appears.
    std::vector<char*> args;
    for (int i = 0; i < argc; ++i) {
        if (!strcmp(argv[i], "--part") && i + 1 < argc) { g_part = atoi(argv[++i]); continue; }
        args.push_back(argv[i]);
    }
    argc = (int)args.size();
    args.push_back(nullptr);
    argv = args.data();
    if (argc < 3) {
        fprintf(stderr,
            "MSXFileForge CLI\n"
            "usage: msxforge <cmd> <file> [args] [--part N]   (FILE may be a path: DIR/NAME.EXT)\n"
            "  --part N selects partition N of a hard disk image (default: first FAT one)\n"
            "\n"
            "disk images (.dsk / .dmk):\n"
            "  info    <img>                       geometry, filesystem, free space\n"
            "  ls      <img> [/PATH] [--deleted]   list a directory\n"
            "  cat     <img> <FILE>                raw file bytes to stdout\n"
            "  detok   <img> <FILE>                tokenized BASIC as text\n"
            "  extract <img> <FILE> <out>          copy a file to the host\n"
            "  add     <img> <hostfile> [/DIR] [NAME]  add a new file\n"
            "  put     <img> <FILE> <hostfile> [outimg] overwrite a file\n"
            "  del     <img> <FILE|DIR> [outimg]   delete a file or an empty directory\n"
            "  rename  <img> <FILE> <NEWNAME>      rename in place\n"
            "  mkdir   <img> <NAME> [/PARENT]      create a directory\n"
            "  format  <out> [360|720|<N>M] [LABEL] blank floppy, or a FAT16 superfloppy\n"
            "  mkhdd   <out.img> <N>M[,<N>M...]    partitioned hard disk / SD image\n"
            "  pack    <hostdir> <out.dsk> [360|720] [--label L]  build a disk from a folder\n"
            "  unpack  <img> <hostdir>             extract the whole tree to a folder\n"
            "  undelete <img> <[DIR/]?NAME> [LETTER] [outimg]  recover a deleted file\n"
            "  attrib  <img> <FILE> [+R -R +H -H +S -S +A -A]  show / set attributes\n"
            "  label   <img> [LABEL|--clear]       show / set the volume label\n"
            "  fsck    <img>                       FAT integrity check\n"
            "  diff    <imgA> <imgB>               compare all files by CRC\n"
            "  crc     <img|file> [FILE]           CRC32 of an image or a file in it\n"
            "  protect <img>                       copy-protection / sector inspector\n"
            "  raw     <img> [out.bin]             cluster map, dump RAW unnamed data\n"
            "  rawput  <img> <hostfile> [outimg]   write RAW unnamed data back\n"
            "  carve   <img>                       split RAW data into BLOAD/BASIC blocks\n"
            "\n"
            "other files:\n"
            "  tok     <text.bas> [out.bas]        tokenize an ASCII listing\n"
            "  rominfo <rom>                       header + mapper guess\n"
            "  cas     <cas>                       cassette block list\n"
            "  cas2wav|cas2tsx <in> <out> [-2]     encode tape (-2 = 2400 baud)\n"
            "  wav2cas|tsx2cas <in> <out>          decode tape\n");
        return 1;
    }
    std::string cmd = argv[1];
    if (cmd == "info")    return cmd_info(argv[2]);
    if (cmd == "ls")      return cmd_ls(argc, argv);
    if (cmd == "cat")     return cmd_cat(argc, argv, false);
    if (cmd == "detok")   return cmd_cat(argc, argv, true);
    if (cmd == "extract") return cmd_extract(argc, argv);
    if (cmd == "protect") return cmd_protect(argv[2]);
    if (cmd == "put")     return cmd_put(argc, argv);
    if (cmd == "del")     return cmd_del(argc, argv);
    if (cmd == "tok")     return cmd_tok(argc, argv);
    if (cmd == "raw")     return cmd_raw(argc, argv);
    if (cmd == "carve")   return cmd_carve(argc, argv);
    if (cmd == "rawput")  return cmd_rawput(argc, argv);
    if (cmd == "format")  return cmd_format(argc, argv);
    if (cmd == "mkhdd")   return cmd_mkhdd(argc, argv);
    if (cmd == "add")     return cmd_add(argc, argv);
    if (cmd == "rename")  return cmd_rename(argc, argv);
    if (cmd == "mkdir")   return cmd_mkdir(argc, argv);
    if (cmd == "crc")     return cmd_crc(argc, argv);
    if (cmd == "undelete") return cmd_undelete(argc, argv);
    if (cmd == "attrib")  return cmd_attrib(argc, argv);
    if (cmd == "label")   return cmd_label(argc, argv);
    if (cmd == "pack")    return cmd_pack(argc, argv);
    if (cmd == "unpack")  return cmd_unpack(argc, argv);
    if (cmd == "fsck")    return cmd_fsck(argv[2]);
    if (cmd == "diff")    return cmd_diff(argc, argv);
    if (cmd == "cas2wav" || cmd == "cas2tsx" || cmd == "tsx2cas" || cmd == "wav2cas")
        return cmd_tape(argc, argv, cmd.c_str());
    if (cmd == "rominfo") return cmd_rominfo(argv[2]);
    if (cmd == "cas")     return cmd_cas(argv[2]);
    fprintf(stderr, "unknown command: %s\n", cmd.c_str());
    return 1;
}
