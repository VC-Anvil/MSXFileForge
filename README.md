# MSXFileForge

A native (C++/Dear ImGui) editor and inspector for **MSX** files, with first-class
support for reading **protected** and **RAW** disk images that a plain `.dsk`
cannot represent.

It handles the whole MSX file stack:

| Domain | What it does |
|--------|--------------|
| **Disk images** | `.dsk` / raw sector dumps, and `.dmk` track images (protected disks). Auto-detects geometry (SS/DS, 360/720 KB). |
| **Protected / RAW** | A `.dmk` preserves the physical IDAM/sector layout, so copy-protected disks survive: non-standard sector sizes, deleted-data address marks, duplicate IDs and CRC anomalies are surfaced in a **sector inspector**. Many games (Fray, Xak, ...) show only 1-2 catalogued files while the rest of the disk is **RAW uncatalogued sector data** read directly by a custom loader (not encryption); a **cluster map + RAW extractor** makes that data visible and exportable. |
| **FAT12 filesystem** | MSX-DOS disks: boot/BPB, FAT chains, root + subdirectories, volume label, free space, file extraction, **deleted-entry recovery**, and **read/write** (overwrite / grow / shrink with automatic cluster (re)allocation across both FAT copies, delete). |
| **MSX files** | Type detection (tokenized BASIC `0xFF`, BLOAD binary `0xFE`, ASCII, data), a full **BASIC detokenizer** and an inverse **tokenizer** (edit a listing as text and save it back tokenized). Handles standard + `0xFF`-extended tokens, hex/oct/int/BCD-float constants, line references, string & REM literals. |
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
msxforge tok     <text> [out.bas]     tokenize text BASIC (round-trip check)
msxforge extract <image> <FILE> <out>
msxforge put     <image> <FILE> <hostfile> [outimage]   overwrite a file
msxforge del     <image> <FILE> [outimage]              delete a file
msxforge protect <image>              physical sector / protection inspector
msxforge raw     <image> [out.bin]    cluster map (named/RAW/free) + extract RAW data
msxforge rominfo <rom>
msxforge cas     <cas>
```

## GUI

Drag a `.dsk` / `.dmk` / `.rom` / `.cas` onto the window, use **File > Open**
(Ctrl+O), or type a path. Then:

- Browse the FAT12 tree (directories in yellow, deleted files in red).
- **Edit** a file three ways: as a **BASIC listing** (re-tokenized on save), as
  **text**, or as **hex** (click a byte or use the offset/value poke box;
  changed bytes highlight yellow).
- **Import & replace**, **Export to file**, or **Delete** a file.
- **Save image** / **Save image as** (Ctrl+S / Ctrl+Shift+S) to persist.
- Inspect the physical sector table of protected images.
- Built-in **Manual** (Help > Manual / F1) and `(?)` tooltips throughout.

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

## Editing & write verification

The edit → save → reopen pipeline (identical in CLI and GUI) is verified on real
media: overwriting a file grows/shrinks its cluster chain byte-exactly, both FAT
copies stay consistent, and unrelated files keep the same checksum. A BASIC edit
(change a line + add a line) round-trips through tokenize → write → reopen →
detokenize, and delete frees clusters and drops the entry.

## Honest limitations

- DMK **CRC verification** is not yet performed (CRC fields are parsed but not
  recomputed); anomaly flags (non-standard size, deleted marks, geometry
  mismatch) are exercised by construction, not yet against a commercial
  copy-protected dump.
- BASIC **BCD floats** are decoded/encoded numerically — correct in value but not
  guaranteed byte-identical to the ROM lister's exact rounding.
- Creating brand-new files / renaming (as opposed to overwrite/delete) and
  writing back into a `.dmk` container are the next milestones (disk writes
  target flat `.dsk`/raw today).

## License / provenance

Bundles Dear ImGui (MIT) and SDL2 (zlib). FAT12 handling is a clean C++ rewrite
informed by the sibling `dsk2rom` FAT tooling.
