// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Alonso (GS·RUN). MSXFileForge.
// Free software under the GNU GPL v3 (or later) - see LICENSE. Modified
// versions must be distributed under the same license.
// dsk2dmk - re-encode a flat .dsk into a DMK track image.
//
// Used to exercise MSXFileForge's DMK reader end-to-end: a disk read through a
// DMK container must yield the exact same filesystem as the original .dsk. The
// encoding itself lives in the core library (msx::dskToDmk): standard IBM MFM
// tracks with gaps, 0xA1 syncs and valid ID/data CRCs.
#include "../core/disk_image.h"
#include <cstdio>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: dsk2dmk <in.dsk> <out.dmk>\n"); return 1; }
    FILE* f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", argv[1]); return 2; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); fprintf(stderr, "empty input\n"); return 2; }
    std::vector<uint8_t> dsk((size_t)n);
    if (fread(dsk.data(), 1, (size_t)n, f) != (size_t)n) { fclose(f); return 2; }
    fclose(f);

    std::string err;
    std::vector<uint8_t> out = msx::dskToDmk(dsk, &err);
    if (out.empty()) { fprintf(stderr, "dsk2dmk: %s\n", err.c_str()); return 4; }

    FILE* o = fopen(argv[2], "wb");
    if (!o) { fprintf(stderr, "cannot write %s\n", argv[2]); return 3; }
    size_t w = fwrite(out.data(), 1, out.size(), o);
    fclose(o);
    if (w != out.size()) { fprintf(stderr, "short write\n"); return 3; }
    fprintf(stderr, "wrote %s (%zu bytes, %d tracks)\n", argv[2], out.size(), out[1]);
    return 0;
}
