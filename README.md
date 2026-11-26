# 7zipl

![Static Badge](https://img.shields.io/badge/CLAUDE-VIBECODED?style=for-the-badge&logo=claude&label=VIBECODED&labelColor=%23333333&color=%23de7356)

<p align="center"><img src="data/icons/org.sevenzipl.SevenZipL.svg" alt="7zipl icon: a blue box with a zipper between a 7 and a z" width="128"></p>

The 7-Zip File Manager for Linux, rewritten with plain GTK 4 (no libadwaita). It runs 7-Zip's own engine
in-process: the same formats, compression settings and results as `7zz`, with the File Manager's window,
dialogs and keyboard shortcuts.

The 7-Zip engine in `C/` and `CPP/` is upstream [7-Zip] by Igor Pavlov, unchanged. Only the front-end is new
(`gtk/`). The Windows File Manager (`7zFM.exe`) and `7zG.exe` don't build on Linux; 7zipl replaces both.

## List of Contents

- **[Parts](#parts)**
- **[Features](#features)**
- **[Requirements](#requirements)**
- **[Installation](#installation)**
  - [Building](#building)
- **[Quick start](#quick-start)**
  - [Command line](#command-line)
  - [Keyboard shortcuts](#keyboard-shortcuts)
- **[Coming from 7-Zip on Windows](#coming-from-7-zip-on-windows)**
- **[Settings](#settings)**
- **[Testing](#testing)**
- **[Known limitations](#known-limitations)**
- **[License](#license)**

## Parts

| Part | Binary / file | Use it for |
|---|---|---|
| File manager | `7zipl` (window class `7zipl`) | browsing folders and archives, all archive operations |
| File-manager actions | `7zipl --extract`, `--extract-here`, `--compress`, … | "Extract here" / "Add to archive" entries in other file managers (what `7zG` does on Windows) |
| Engine | `sevenzip` (static library from `C/`, `CPP/`) | 7-Zip's codecs and archive handlers, the same sources as `7zz` |
| Desktop entry | `org.sevenzipl.SevenZipL.desktop` | launchers and "Open With" for archive types |

## Features

- **Same engine as `7zz`**: the GUI links the code of `CPP/7zip/Bundles/Alone2` (the `7zz` build) and calls
  7-Zip's `Extract()`, `UpdateArchive()` and `HashCalc()` with its own callbacks. No `7z` child process and no
  output parsing.
- **Archives as folders**: open an archive and browse it like a folder; archives inside archives (`.tar.gz` →
  `.tar` → files) open on <kbd>Enter</kbd>, one level per archive. The address bar takes paths inside archives
  (`~/a.7z/docs/img`).
- **Create**: 7z, zip, tar, wim, gzip, bzip2 and xz, with 7-Zip's Add to Archive dialog: level, method (LZMA2,
  LZMA, PPMd, BZip2, Deflate, Deflate64), dictionary, word size, solid block size, CPU threads, volumes,
  update mode (add/update/freshen/synchronize), path mode, `-snl`/`-snh`/`-ssw`/`-sdel`, and free-form
  parameters (`qs=on mtf=off`).
- **Encryption**: AES-256 for 7z (with encrypted file names) and zip (AES-256 or ZipCrypto). Encrypted archives
  ask for the password once and keep it while they are open; updating them keeps header encryption on.
- **Extract and test** every format 7-Zip reads: 7z, zip, rar/rar5, tar, gz, bz2, xz, zstd, lzma, iso, udf, cab,
  chm, dmg, hfs, apfs, wim, vhd(x), vmdk, qcow, squashfs, cramfs, ext, fat, ntfs, rpm, deb, cpio, arj, lzh, msi,
  nsis, and more.
- **Change archives in place**: add files (into the open folder), create folders, delete and rename items.
- **Checksums**: **File → CRC** with every hasher in the engine (CRC32, CRC64, XXH64, MD5, SHA1, SHA256,
  SHA384, SHA512, SHA3-224 to SHA3-512, BLAKE2sp, CKSUM; `*` for all), for files on disk and items inside archives.
- **Progress like 7-Zip**: elapsed and remaining time, speed, sizes, compression ratio, Pause and Cancel; errors
  and warnings end in a message list instead of disappearing.
- **Plain GTK 4**: system theme colours (pywal themes work), square corners, no client-side title bar.

## Requirements

- Linux with GTK 4.14 or newer and GLib 2.76 or newer
- To build: a C++17 compiler (GCC or Clang), CMake 3.16+, Ninja or Make, pkg-config

The engine has no other dependencies.

## Installation

AUR packages are prepared in [`aur/`](aur) (`7zipl-git` builds from this repository, `7zipl-bin` installs the
release tarball).

### Building

```sh
git clone https://github.com/TheElevatedOne/7zipl.git
cd 7zipl
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build
sudo cmake --install build
```

The first build compiles about 310 7-Zip source files and takes a minute or two.

## Quick start

```sh
7zipl                      # opens the last folder
7zipl ~/Downloads/a.7z     # opens an archive
7zipl ~/a.7z/docs          # opens a folder inside an archive
```

Select files and press **Add** to create an archive; select archives and press **Extract**. Inside an archive,
**Extract** extracts the selection (or the open folder) without the path of the folder you are in, like 7-Zip.

### Command line

These run without the main window, for file-manager context menus:

| Command | Does |
|---|---|
| `7zipl --extract FILE...` | Extract dialog for the archives |
| `7zipl --extract-here FILE...` | extracts into the archive's folder |
| `7zipl --extract-to-folder FILE...` | extracts each archive into `<name>/` next to it |
| `7zipl --compress FILE...` | Add to Archive dialog |
| `7zipl --compress-to 7z FILE...` | adds to `<name>.7z` with the saved settings (also `zip`, `tar`, `xz`, …) |
| `7zipl --test FILE...` | tests the archives |
| `7zipl --hash SHA256 FILE...` | checksums (`*` for every method) |
| `7zipl --profile DIR` | uses `DIR` for settings instead of `~/.config/7zipl` |

### Keyboard shortcuts

| Key | Action |
|---|---|
| <kbd>Enter</kbd> | open: folders and archives inside, other files with their application |
| <kbd>Ctrl</kbd>+<kbd>PgDn</kbd> / <kbd>Shift</kbd>+<kbd>Enter</kbd> | Open Inside / Open Outside |
| <kbd>Backspace</kbd> | up one level (out of an archive at its root) |
| <kbd>F2</kbd>, <kbd>F5</kbd>, <kbd>F6</kbd>, <kbd>F7</kbd> | Rename, Copy To, Move To, Create Folder |
| <kbd>Delete</kbd> / <kbd>Shift</kbd>+<kbd>Delete</kbd> | to the Trash (on disk) or from the archive / delete permanently |
| <kbd>Alt</kbd>+<kbd>Enter</kbd> | Properties (item and archive information) |
| <kbd>Ctrl</kbd>+<kbd>A</kbd>, numpad <kbd>+</kbd> <kbd>-</kbd> <kbd>*</kbd> | select all, select / deselect by mask, invert |
| <kbd>Ctrl</kbd>+<kbd>R</kbd>, <kbd>Ctrl</kbd>+<kbd>H</kbd>, <kbd>Ctrl</kbd>+<kbd>L</kbd> | refresh, hidden files, address bar |
| <kbd>Alt</kbd>+<kbd>Shift</kbd>+<kbd>0</kbd>–<kbd>9</kbd> / <kbd>Alt</kbd>+<kbd>0</kbd>–<kbd>9</kbd> | set / go to a favorite folder |
| <kbd>Ctrl</kbd>+<kbd>F3</kbd>, <kbd>F5</kbd>, <kbd>F6</kbd> | sort by name, date, size |

Single-key shortcuts work in the file list only, so typing in the address bar never deletes or renames files.

## Coming from 7-Zip on Windows

The same: toolbar (Add, Extract, Test, Copy, Move, Delete, Info), menus, the Add to Archive and Extract dialogs
and their option names, the progress window, "Confirm File Replace", favorites, column set per archive format,
folders first.

Different:

- **Delete** on disk moves to the Trash (freedesktop.org); <kbd>Shift</kbd>+<kbd>Delete</kbd> deletes.
- Attributes are Unix permissions (`-rw-r--r--`) on disk.
- Not there (yet): two panels (<kbd>F9</kbd>), split/combine files, the benchmark, SFX archives, editing a file
  inside an archive and saving it back, language files, shell integration on Windows-only APIs.

## Settings

`~/.config/7zipl/7zipl.conf` (a key file), written on exit and when a dialog closes with OK; **Tools → Options**
shows the view settings. It holds the view options, window size, last folder, favorites, the Add to Archive and
Extract defaults, and the folder history. `--profile DIR` or `SZL_PROFILE=DIR` uses another folder.

Temporary copies (opening a file inside an archive, nested archives) go to `$XDG_RUNTIME_DIR/7zipl/` and are
removed when 7zipl exits.

## Testing

```sh
ctest --test-dir build --output-on-failure
```

runs `7zipl --selftest`: creating, listing, extracting (with a folder prefix removed), testing, renaming,
deleting, hashing, encrypted headers, zip/tar/gz, and the main window browsing into archives and adding a file
into an archive folder. It needs a display and exits with 77 (skipped) without one. To run it without a screen:

```sh
gtk4-broadwayd :5 &
GDK_BACKEND=broadway BROADWAY_DISPLAY=:5 GDK_DEBUG=no-portals ./build/7zipl --selftest
```

## Known limitations

- Archives inside archives are read-only (changes would have to be packed back into the outer archive).
- Formats 7-Zip can only read (rar, iso, dmg, …) can't be changed; delete, rename and add are disabled for them.
- zstd archives can be extracted but not created (the 7-Zip engine has a zstd decoder only).

## License

The 7-Zip engine is GNU LGPL 2.1 or later, with parts under the BSD 3-clause license and the unRAR license
restriction; see [DOC/License.txt](DOC/License.txt) and [DOC/unRarLicense.txt](DOC/unRarLicense.txt). The GTK
front-end in `gtk/` is LGPL 2.1 or later. 7zipl isn't made or endorsed by the 7-Zip author.

[7-Zip]: https://7-zip.org
