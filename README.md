# MSXFileForge

A native (C++/Dear ImGui) editor and inspector for **MSX** files, with first-class
support for reading **protected** and **RAW** disk images that a plain `.dsk`
cannot represent.

It handles the whole MSX file stack:

| Domain | What it does |
|--------|--------------|
| **Disk images** | `.dsk` / raw sector dumps, and `.dmk` track images (protected disks). Auto-detects geometry (SS/DS, 360/720 KB). |
| **Protected / RAW** | A `.dmk` preserves the physical IDAM/sector layout, so copy-protected disks survive: non-standard sector sizes, deleted-data address marks, duplicate IDs and CRC anomalies are surfaced in a **sector inspector**. The logical FAT12 space is reconstructed from the standard sectors, so a normal filesystem is still readable straight out of a protected image. |
| **FAT12 filesystem** | MSX-DOS disks: boot/BPB, FAT chains, root + subdirectories, volume label, free space, file extraction, and **deleted-entry recovery**. |
| **MSX files** | Type detection (tokenized BASIC `0xFF`, BLOAD binary `0xFE`, ASCII, data) and a full **BASIC detokenizer** (standard + `0xFF`-extended tokens, hex/oct/int/BCD-float constants, string & REM literal handling). |
| **Cartridge ROMs** | Size / page count, `AB` header + entry points (INIT/STATEMENT/DEVICE/TEXT), and **mapper detection** (None / Konami / Konami SCC / ASCII8 / ASCII16) via the openMSX-style write-target heuristic. |
| **Cassette** | `.cas` block layout: header sync, ASCII / Binary / Tokenized-BASIC file blocks with filenames. |

## Layout

```
core/     pure C++17 library, no GUI deps  (disk_image, fat12, msxfile, rom, cas)
cli/      msxforge  — command-line driver (also the verification oracle)
gui/      msxforge-gui — Dear ImGui + SDL2 + OpenGL3 desktop app
tools/    dsk2dmk — re-encodes a .dsk into a DMK track image (used for testing)
libs/     vendored Dear ImGui 1.92.7 + SDL2 (self-contained)
```

## Build (Windows, MinGW-w64)

```sh
./build.sh          # or: cmake -B build -G "MinGW Makefiles" && cmake --build build
```

Produces `build/msxforge.exe`, `build/msxforge-gui.exe`, `build/dsk2dmk.exe`.
`SDL2.dll` is copied next to the GUI exe.

## CLI usage

```
msxforge info    <image>              disk/format/geometry/FAT summary
msxforge ls      <image> [/PATH] [--deleted]
msxforge cat     <image> <FILE>       raw bytes to stdout
msxforge detok   <image> <FILE.BAS>   detokenize BASIC to stdout
msxforge extract <image> <FILE> <out>
msxforge protect <image>              physical sector / protection inspector
msxforge rominfo <rom>
msxforge cas     <cas>
```

## GUI

Drag a `.dsk` / `.dmk` / `.rom` / `.cas` onto the window (or type a path). Browse
the file tree, view any file as hex, read the detokenized BASIC / BLOAD header,
identify the ROM mapper, and inspect the physical sector table of protected
images.

## Verified against real media

- FAT12 read on commercial disks (Master of Monsters = 81 files, Perry Mason =
  61/66) — file listing, subdir walk, byte-exact extraction (`AVENTURA.COM`,
  29 726 bytes).
- BASIC detokenizer on real loaders (`VDP(10)=16 : CLS : PRINT"…" : _SYSTEM`),
  with MSX graphic bytes preserved inside strings.
- ROM mapper detection on a real Konami SCC cartridge (Vampire Killer).
- **DMK end-to-end**: a `.dsk` re-encoded to `.dmk` yields the *identical*
  filesystem (same 61 files, same detokenized BASIC) read through the track
  container — proving the protected/RAW disk path.

## Honest limitations (v1)

- DMK **CRC verification** is not yet performed (CRC fields are parsed but not
  recomputed); anomaly flags (non-standard size, deleted marks, geometry
  mismatch) are exercised by construction, not yet against a commercial
  copy-protected dump.
- BASIC **BCD floats** are decoded to a value and formatted with `%g` — readable
  and numerically correct, but not guaranteed byte-identical to the ROM lister.
- Writing/editing back to images is **read-only** so far (extraction works;
  in-place file insert/delete is the next milestone).

## License / provenance

Bundles Dear ImGui (MIT) and SDL2 (zlib). FAT12 handling is a clean C++ rewrite
informed by the sibling `dsk2rom` FAT tooling.
