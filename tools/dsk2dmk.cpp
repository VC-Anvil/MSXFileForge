// dsk2dmk - re-encode a flat .dsk into a DMK track image.
//
// Used to exercise MSXFileForge's DMK reader end-to-end: a disk read through a
// DMK container must yield the exact same filesystem as the original .dsk. Lays
// out standard double-density (MFM) 512-byte sectors, side-major, sector 1..spt.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>

static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: dsk2dmk <in.dsk> <out.dmk>\n"); return 1; }
    FILE* f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", argv[1]); return 2; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> dsk((size_t)n);
    if (fread(dsk.data(), 1, (size_t)n, f) != (size_t)n) { fclose(f); return 2; }
    fclose(f);

    // Geometry from the BPB (fall back to 720K DS).
    int spc = dsk[0x0D], tot = rd16(&dsk[0x13]);
    int spt = 9, heads = 2;
    if (tot == 720) { heads = 1; spt = 9; }
    else if (tot == 1440) { heads = 2; spt = 9; }
    else if (tot == 640) { heads = 1; spt = 8; }
    else if (tot == 1280) { heads = 2; spt = 8; }
    int cyls = tot / (spt * heads);
    (void)spc;
    fprintf(stderr, "geometry: %dc x %dh x %ds (tot=%d)\n", cyls, heads, spt, tot);

    const int SECSZ = 512;
    const int perSec = 7 + 2 + 1 + SECSZ + 2; // IDAM(7) + gap(2) + DAM(1) + data + crc(2)
    const int trackLen = 0x80 + spt * perSec + 128; // + tail slack

    std::vector<uint8_t> out;
    uint8_t hdr[16] = {0};
    hdr[0] = 0x00;                 // not write protected
    hdr[1] = (uint8_t)cyls;        // tracks
    hdr[2] = trackLen & 0xFF; hdr[3] = (trackLen >> 8) & 0xFF;
    hdr[4] = (heads == 1) ? 0x10 : 0x00; // bit4 = single sided
    out.insert(out.end(), hdr, hdr + 16);

    for (int c = 0; c < cyls; ++c) {
        for (int h = 0; h < heads; ++h) {
            std::vector<uint8_t> track(trackLen, 0x00);
            int idamTable = 0;   // write pointers into first 0x80 bytes
            int body = 0x80;
            for (int s = 0; s < spt; ++s) {
                long lba = ((long)c * heads + h) * spt + s;
                long srcOff = lba * SECSZ;
                int idamPos = body;
                track[body++] = 0xFE;          // ID address mark
                track[body++] = (uint8_t)c;    // cyl
                track[body++] = (uint8_t)h;    // head
                track[body++] = (uint8_t)(s + 1); // sector (1-based)
                track[body++] = 0x02;          // size code (512)
                track[body++] = 0x00;          // ID CRC hi (dummy)
                track[body++] = 0x00;          // ID CRC lo
                track[body++] = 0x4E;          // gap
                track[body++] = 0x4E;
                track[body++] = 0xFB;          // data address mark
                if (srcOff + SECSZ <= (long)dsk.size())
                    memcpy(&track[body], &dsk[srcOff], SECSZ);
                body += SECSZ;
                track[body++] = 0x00;          // data CRC hi
                track[body++] = 0x00;          // data CRC lo
                // IDAM pointer: offset of 0xFE within track, bit15=MFM.
                uint16_t ptr = (uint16_t)(idamPos | 0x8000);
                track[idamTable++] = ptr & 0xFF;
                track[idamTable++] = (ptr >> 8) & 0xFF;
            }
            out.insert(out.end(), track.begin(), track.end());
        }
    }

    FILE* o = fopen(argv[2], "wb");
    if (!o) { fprintf(stderr, "cannot write %s\n", argv[2]); return 3; }
    fwrite(out.data(), 1, out.size(), o);
    fclose(o);
    fprintf(stderr, "wrote %s (%zu bytes)\n", argv[2], out.size());
    return 0;
}
