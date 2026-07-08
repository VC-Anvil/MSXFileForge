// MSXFileForge CLI - exercises the core library against real images.
//
//   msxforge info    <image>
//   msxforge ls      <image> [/PATH] [--deleted]
//   msxforge cat     <image> <FILE>        (raw bytes to stdout)
//   msxforge detok   <image> <FILE.BAS>    (detokenize BASIC to stdout)
//   msxforge extract <image> <FILE> <out>
//   msxforge protect <image>               (protected/RAW sector inspector)
//   msxforge rominfo <rom>
//   msxforge cas     <cas>
#include "../core/disk_image.h"
#include "../core/fat12.h"
#include "../core/msxfile.h"
#include "../core/rom.h"
#include "../core/cas.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

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

static int mountDisk(const char* path, DiskImage& dk, Fat12& fs) {
    std::string err;
    if (!dk.load(path, &err)) { fprintf(stderr, "load error: %s\n", err.c_str()); return 2; }
    if (!fs.mount(dk, &err)) { fprintf(stderr, "mount error: %s\n", err.c_str()); return 3; }
    return 0;
}

static const DirEntry* findEntry(const Fat12& fs, const std::string& name,
                                 std::vector<DirEntry>& listing) {
    std::string want = name; for (auto& c : want) c = (char)toupper((unsigned char)c);
    for (auto& e : listing) {
        std::string nm = e.name; for (auto& c : nm) c = (char)toupper((unsigned char)c);
        if (nm == want) return &e;
    }
    return nullptr;
}

static int cmd_info(const char* img) {
    DiskImage dk; std::string err;
    if (!dk.load(img, &err)) { fprintf(stderr, "load error: %s\n", err.c_str()); return 2; }
    printf("Image      : %s\n", img);
    printf("Format     : %s\n", to_string(dk.format()));
    printf("Geometry   : %d cyl x %d head x %d spt (%ld logical sectors)\n",
           dk.cylinders(), dk.heads(), dk.sectorsPerTrack(), dk.logicalSectors());
    printf("Protected  : %s\n", dk.isProtected() ? "YES" : "no");
    Fat12 fs;
    if (fs.mount(dk, &err)) {
        printf("Filesystem : FAT12  OEM='%s'  label='%s'\n",
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
    auto root = fs.list("/", false);
    const DirEntry* e = findEntry(fs, argv[3], root);
    if (!e) { fprintf(stderr, "file not found: %s\n", argv[3]); return 4; }
    auto data = fs.readFile(*e);
    if (detok) {
        std::string txt = detokenizeBasic(data);
        fwrite(txt.data(), 1, txt.size(), stdout);
    } else {
        fwrite(data.data(), 1, data.size(), stdout);
    }
    return 0;
}

static int cmd_extract(int argc, char** argv) {
    if (argc < 5) { fprintf(stderr, "need <image> <file> <out>\n"); return 1; }
    DiskImage dk; Fat12 fs;
    int rc = mountDisk(argv[2], dk, fs); if (rc) return rc;
    auto root = fs.list("/", false);
    const DirEntry* e = findEntry(fs, argv[3], root);
    if (!e) { fprintf(stderr, "file not found: %s\n", argv[3]); return 4; }
    auto data = fs.readFile(*e);
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
        bool odd = s.nonStandard || s.deleted || s.idCrcError || s.dataCrcError;
        if (odd) anomalies++;
        if (odd && shown < 40) {
            printf("  C%02d H%d S%02d size=%zu%s%s%s%s lba=%ld\n",
                   s.cyl, s.head, s.sec, s.size,
                   s.nonStandard ? " NONSTD" : "",
                   s.deleted ? " DELETED" : "",
                   s.idCrcError ? " IDCRC" : "",
                   s.dataCrcError ? " DATACRC" : "", s.lba);
            shown++;
        }
    }
    printf("Physical sectors: %zu   anomalies: %d\n", dk.sectors().size(), anomalies);
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
    if (argc < 3) {
        fprintf(stderr,
            "MSXFileForge CLI\n"
            "usage: msxforge <cmd> <file> [args]\n"
            "  info|ls|cat|detok|extract|protect  <disk-image>\n"
            "  rominfo <rom>   cas <cas>\n");
        return 1;
    }
    std::string cmd = argv[1];
    if (cmd == "info")    return cmd_info(argv[2]);
    if (cmd == "ls")      return cmd_ls(argc, argv);
    if (cmd == "cat")     return cmd_cat(argc, argv, false);
    if (cmd == "detok")   return cmd_cat(argc, argv, true);
    if (cmd == "extract") return cmd_extract(argc, argv);
    if (cmd == "protect") return cmd_protect(argv[2]);
    if (cmd == "rominfo") return cmd_rominfo(argv[2]);
    if (cmd == "cas")     return cmd_cas(argv[2]);
    fprintf(stderr, "unknown command: %s\n", cmd.c_str());
    return 1;
}
